// Command-list tracking + post-stage replay for native stereo (and the back-buffer trace diagnostic).
//
// With two engine views (fbview.cpp: left eye = view 0, right eye = view 1) the game renders and post-processes both
// views, each with its own DLSS output (output-size RGBA16F), but runs its final stage (tonemap / UI composite into
// the composite target, then CopyResource to the back buffer) only once, on the last view (right eye).
//
// Per frame, in the ExecuteCommandLists call that renders into the composite target:
//   1. execute the game's batch as is                  -> composite target = right eye (with HUD)
//   2. copy composite -> our right-eye texture
//   3. copy the left view's DLSS output over the right view's DLSS output
//   4. put every resource touched by the replayed lists back into the state it had at the start of the range
//   5. re-execute the game's lists from the first consumer of the right view's DLSS output through the list that
//      finishes the composite target                  -> composite target = left eye
//   6. hand left (composite target) + right (our texture) to the XR host. The game's own back-buffer copy
//      afterwards then shows the left eye on the monitor.
// All resources are identified from the barriers recorded into the lists (hooks on the real D3D12 command list).
#include <windows.h>
#include <d3d12.h>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <array>
#include <vector>
#include <string>
#include <cstdio>
#include <cmath>
#include <algorithm>
#include "replay.h"
#include "xr_host.h"
#include "fbview.h"
#include "fovdlss.h"
#include "vrs.h"
#include "slhook.h"
#include "globals.h"
#include "log.h"
#include "../third_party/minhook/include/MinHook.h"

extern "C" const int VT12_CL_Reset, VT12_CL_ResourceBarrier, VT12_CL_CopyResource, VT12_CL_CopyTextureRegion,
    VT12_CL_OMSetRenderTargets, VT12_CL_DrawInstanced, VT12_CL_DrawIndexedInstanced, VT12_CL_ClearRenderTargetView, VT12_DEV_CreateRenderTargetView, VT12_CL_Dispatch;

namespace davg {

typedef HRESULT(STDMETHODCALLTYPE* PFN_Reset)(ID3D12GraphicsCommandList*, ID3D12CommandAllocator*, ID3D12PipelineState*);
typedef void(STDMETHODCALLTYPE* PFN_Barrier)(ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*);
typedef void(STDMETHODCALLTYPE* PFN_CopyRes)(ID3D12GraphicsCommandList*, ID3D12Resource*, ID3D12Resource*);
typedef void(STDMETHODCALLTYPE* PFN_CopyTex)(ID3D12GraphicsCommandList*, const D3D12_TEXTURE_COPY_LOCATION*, UINT, UINT, UINT,
                                             const D3D12_TEXTURE_COPY_LOCATION*, const D3D12_BOX*);
static PFN_Reset o_Reset; static PFN_Barrier o_Barrier; static PFN_CopyRes o_CopyRes; static PFN_CopyTex o_CopyTex;

// ---------------------------------------------------------------- per-list event recording
struct Ev { ID3D12Resource* res; uint32_t before, after; uint8_t kind; };   // kind 0 transition, 1 copy to back buffer
// Render-target trace (HUD analysis): per list, each OMSetRenderTargets with the draws that followed.
struct RtRec { ID3D12Resource* rt0; uint32_t nRt; uint32_t draws; uint32_t clears; uint32_t evIdx; uint32_t w, h, fmt; ID3D12Resource* src = nullptr; };   // nRt 0xD15 = dispatches, 0xC0F = copy src -> rt0
struct RtvInfo { ID3D12Resource* res; uint32_t w, h, fmt; };
struct ListRec { std::vector<Ev> ev; std::vector<RtRec> rt; int dlssEye = -1; ID3D12Resource* dlssOut = nullptr; ID3D12Resource* tagOut = nullptr; uint32_t dlssEv = 0; bool hasCam = false; float cam[6] = {}; void* depth = nullptr; uint64_t stamp = 0; };   // dlss*: Streamline evaluate recorded here
static std::atomic<int> g_rtTrace{0};
static std::atomic<bool> g_vrsOn{false};   // VRS foveation active (OMSetRenderTargets hook does the work)      // >0: record render targets / draws (frames left)
static std::mutex g_rtvMu;
static std::unordered_map<SIZE_T, RtvInfo> g_rtv;   // RTV descriptor -> resource
static UINT g_rtvInc = 32;
static bool g_rtvAlways = true;   // RTVs may be created long before a trace
static std::mutex g_mu;
static std::unordered_map<ID3D12GraphicsCommandList*, ListRec> g_lists;
static std::atomic<bool> g_track{false};       // record transitions (stereo replay or trace armed)
static std::atomic<ID3D12Resource*> g_bb[4];
static std::atomic<ID3D12Resource*> g_compositeSrc{nullptr};   // source of the last copy into the back buffer
static std::mutex g_compMu;
static std::vector<ID3D12Resource*> g_compSet;                    // every resource ever copied into the back buffer
static DXGI_FORMAT g_bbFormat = DXGI_FORMAT_UNKNOWN;
static UINT g_outW = 1920, g_outH = 1080;

// descriptor cache
static std::mutex g_descMu;
static std::unordered_map<ID3D12Resource*, D3D12_RESOURCE_DESC> g_desc;
static D3D12_RESOURCE_DESC Desc(ID3D12Resource* r) {
    std::lock_guard<std::mutex> lk(g_descMu);
    auto it = g_desc.find(r);
    if (it != g_desc.end()) return it->second;
    D3D12_RESOURCE_DESC d = r->GetDesc();
    if (g_desc.size() > 20000) g_desc.clear();
    g_desc[r] = d;
    return d;
}
static bool IsBB(ID3D12Resource* r) { if (!r) return false; for (auto& b : g_bb) if (b.load() == r) return true; return false; }
static bool Typeless1010102(DXGI_FORMAT f) { return f == DXGI_FORMAT_R10G10B10A2_TYPELESS || f == DXGI_FORMAT_R10G10B10A2_UNORM || f == DXGI_FORMAT_R10G10B10A2_UINT; }
static bool IsCompositeLike(ID3D12Resource* r) {
    { std::lock_guard<std::mutex> lk(g_compMu); for (auto* c : g_compSet) if (c == r) return true; }
    D3D12_RESOURCE_DESC d = Desc(r);
    if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.Width != g_outW || d.Height != g_outH) return false;
    if (Typeless1010102(g_bbFormat)) return Typeless1010102(d.Format);
    return d.Format == g_bbFormat;
}
static bool IsOutputRgba16f(ID3D12Resource* r) {
    D3D12_RESOURCE_DESC d = Desc(r);
    return d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.Width == g_outW && d.Height == g_outH &&
           (d.Format == DXGI_FORMAT_R16G16B16A16_FLOAT || d.Format == DXGI_FORMAT_R16G16B16A16_TYPELESS) && d.DepthOrArraySize == 1;
}

static HRESULT STDMETHODCALLTYPE Hk_Reset(ID3D12GraphicsCommandList* l, ID3D12CommandAllocator* a, ID3D12PipelineState* p) {
    if (g_vrsOn.load()) vrs::OnReset(l);
    if (g_track.load()) { std::lock_guard<std::mutex> lk(g_mu); auto& r = g_lists[l]; r.ev.clear(); r.rt.clear(); r.dlssEye = -1; r.dlssOut = nullptr; r.tagOut = nullptr; r.hasCam = false; r.depth = nullptr; r.stamp = FbViewNewestSerial(); }
    return o_Reset(l, a, p);
}
static void STDMETHODCALLTYPE Hk_Barrier(ID3D12GraphicsCommandList* l, UINT n, const D3D12_RESOURCE_BARRIER* b) {
    if (g_track.load()) {
        std::lock_guard<std::mutex> lk(g_mu);
        auto& rec = g_lists[l];
        for (UINT i = 0; i < n; i++)
            if (b[i].Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION && b[i].Transition.pResource &&
                !(b[i].Flags & D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY) && rec.ev.size() < 4096)
                rec.ev.push_back({b[i].Transition.pResource, (uint32_t)b[i].Transition.StateBefore, (uint32_t)b[i].Transition.StateAfter, 0});
    }
    o_Barrier(l, n, b);
}
static void TraceCopy(ID3D12GraphicsCommandList* l, ID3D12Resource* dst, ID3D12Resource* src);
static void STDMETHODCALLTYPE Hk_CopyRes(ID3D12GraphicsCommandList* l, ID3D12Resource* dst, ID3D12Resource* src) {
    TraceCopy(l, dst, src);
    if (IsBB(dst) && src) {
        if (g_compositeSrc.load() != src) {   // the composite target ring rotates every frame
            g_compositeSrc = src;
            std::lock_guard<std::mutex> lk(g_compMu);
            bool known = false; for (auto* r : g_compSet) if (r == src) known = true;
            if (!known) { if (g_compSet.size() >= 8) g_compSet.erase(g_compSet.begin()); g_compSet.push_back(src); }
        }
        if (g_track.load()) { std::lock_guard<std::mutex> lk(g_mu); g_lists[l].ev.push_back({src, 0, 0, 1}); }
    }
    o_CopyRes(l, dst, src);
}
static void STDMETHODCALLTYPE Hk_CopyTex(ID3D12GraphicsCommandList* l, const D3D12_TEXTURE_COPY_LOCATION* d, UINT x, UINT y, UINT z,
                                         const D3D12_TEXTURE_COPY_LOCATION* s, const D3D12_BOX* box) {
    if (d && s) TraceCopy(l, d->pResource, s->pResource);
    o_CopyTex(l, d, x, y, z, s, box);
}


typedef void(STDMETHODCALLTYPE* PFN_OMSet)(ID3D12GraphicsCommandList*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL, const D3D12_CPU_DESCRIPTOR_HANDLE*);
typedef void(STDMETHODCALLTYPE* PFN_Draw)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, UINT);
typedef void(STDMETHODCALLTYPE* PFN_DrawIdx)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, INT, UINT);
typedef void(STDMETHODCALLTYPE* PFN_ClearRtv)(ID3D12GraphicsCommandList*, D3D12_CPU_DESCRIPTOR_HANDLE, const FLOAT*, UINT, const D3D12_RECT*);
typedef void(STDMETHODCALLTYPE* PFN_CreateRtv)(ID3D12Device*, ID3D12Resource*, const D3D12_RENDER_TARGET_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
static PFN_OMSet o_OMSet; static PFN_Draw o_Draw; static PFN_DrawIdx o_DrawIdx; static PFN_ClearRtv o_ClearRtv; static PFN_CreateRtv o_CreateRtv;
static RtvInfo RtvRes(SIZE_T p) { std::lock_guard<std::mutex> lk(g_rtvMu); auto it = g_rtv.find(p); return it == g_rtv.end() ? RtvInfo{} : it->second; }
static void STDMETHODCALLTYPE Hk_CreateRtv(ID3D12Device* d, ID3D12Resource* r, const D3D12_RENDER_TARGET_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE h) {
    if (g_rtTrace.load() > 0 || g_rtvAlways) {
        RtvInfo inf{r, 0, 0, 0};
        if (r) { D3D12_RESOURCE_DESC d = r->GetDesc(); inf.w = (uint32_t)d.Width; inf.h = d.Height; inf.fmt = (uint32_t)(desc ? desc->Format : d.Format); }
        std::lock_guard<std::mutex> lk(g_rtvMu); g_rtv[h.ptr] = inf;
    }
    o_CreateRtv(d, r, desc, h);
}
static void STDMETHODCALLTYPE Hk_OMSet(ID3D12GraphicsCommandList* l, UINT n, const D3D12_CPU_DESCRIPTOR_HANDLE* h, BOOL single, const D3D12_CPU_DESCRIPTOR_HANDLE* dsv) {
    if (g_vrsOn.load()) {   // foveated rendering: VRS rate image on scene passes (render-size colour + depth)
        RtvInfo r0 = (n && h) ? RtvRes(h[0].ptr) : RtvInfo{};
        unsigned rw, rh; if (fovdlss::LastRenderSize(rw, rh)) vrs::SetRenderSize(rw, rh);
        o_OMSet(l, n, h, single, dsv);
        vrs::OnSetRenderTargets(l, r0.w, r0.h, dsv != nullptr);
        if (g_rtTrace.load() > 0 && g_track.load()) { std::lock_guard<std::mutex> lk(g_mu); auto& rec = g_lists[l];
            if (rec.rt.size() < 512) rec.rt.push_back({r0.res, n, 0, 0, (uint32_t)rec.ev.size(), r0.w, r0.h, r0.fmt}); }
        return;
    }
    if (g_rtTrace.load() > 0 && g_track.load()) {
        RtvInfo r0 = (n && h) ? RtvRes(h[0].ptr) : RtvInfo{};
        std::lock_guard<std::mutex> lk(g_mu);
        auto& rec = g_lists[l];
        if (rec.rt.size() < 512) rec.rt.push_back({r0.res, n, 0, 0, (uint32_t)rec.ev.size(), r0.w, r0.h, r0.fmt});
    }
    o_OMSet(l, n, h, single, dsv);
}
static void TraceCopy(ID3D12GraphicsCommandList* l, ID3D12Resource* dst, ID3D12Resource* src) {
    if (g_rtTrace.load() <= 0 || !g_track.load() || !dst) return;
    D3D12_RESOURCE_DESC d = dst->GetDesc();
    std::lock_guard<std::mutex> lk(g_mu);
    auto& rec = g_lists[l];
    if (rec.rt.size() < 512) { RtRec r{dst, 0xC0F, 0, 0, (uint32_t)rec.ev.size(), (uint32_t)d.Width, d.Height, (uint32_t)d.Format}; r.src = src; rec.rt.push_back(r); }
}
typedef void(STDMETHODCALLTYPE* PFN_Dispatch)(ID3D12GraphicsCommandList*, UINT, UINT, UINT);
static PFN_Dispatch o_Dispatch;
static void STDMETHODCALLTYPE Hk_Dispatch(ID3D12GraphicsCommandList* l, UINT x, UINT y, UINT z) {
    if (g_rtTrace.load() > 0 && g_track.load()) {
        std::lock_guard<std::mutex> lk(g_mu);
        auto& rec = g_lists[l];
        if (!rec.rt.empty() && rec.rt.back().nRt == 0xD15) rec.rt.back().draws++;
        else if (rec.rt.size() < 512) rec.rt.push_back({nullptr, 0xD15, 1, 0, (uint32_t)rec.ev.size(), x, y, z});
    }
    o_Dispatch(l, x, y, z);
}
static void CountDraw(ID3D12GraphicsCommandList* l) {
    if (g_rtTrace.load() > 0 && g_track.load()) {
        std::lock_guard<std::mutex> lk(g_mu);
        auto it = g_lists.find(l);
        if (it != g_lists.end() && !it->second.rt.empty()) it->second.rt.back().draws++;
    }
}
static void STDMETHODCALLTYPE Hk_Draw(ID3D12GraphicsCommandList* l, UINT a, UINT b, UINT c, UINT d) { CountDraw(l); o_Draw(l, a, b, c, d); }
static void STDMETHODCALLTYPE Hk_DrawIdx(ID3D12GraphicsCommandList* l, UINT a, UINT b, UINT c, INT d, UINT e) { CountDraw(l); o_DrawIdx(l, a, b, c, d, e); }
static void STDMETHODCALLTYPE Hk_ClearRtv(ID3D12GraphicsCommandList* l, D3D12_CPU_DESCRIPTOR_HANDLE h, const FLOAT* c, UINT n, const D3D12_RECT* r) {
    if (g_rtTrace.load() > 0 && g_track.load()) {
        RtvInfo res = RtvRes(h.ptr);
        std::lock_guard<std::mutex> lk(g_mu);
        auto& rec = g_lists[l];
        if (rec.rt.size() < 512) rec.rt.push_back({res.res, 0xC1EA, 0, 1, (uint32_t)rec.ev.size(), res.w, res.h, res.fmt});
    }
    o_ClearRtv(l, h, c, n, r);
}

// ---------------------------------------------------------------- frame state (render thread)
static int g_eclInFrame = 0, g_frame = 0;
static ID3D12Resource* g_dlssOut[2] = {};      // per-view DLSS outputs seen this frame (order of first UAV write)
static std::unordered_map<ID3D12Resource*, uint32_t> g_state;   // last known state this frame (from executed lists)
struct Snap { ID3D12Resource* clone = nullptr; D3D12_RESOURCE_DESC desc{}; };
static std::atomic<int> g_traceArmed{0};
static std::atomic<int> g_inputsDump{0};       // diagnostic: log the replay inputs and the view spans once
struct FrameEv { int batch; int list; Ev e; };
static std::vector<FrameEv> g_frameEvs;        // this frame's executed transitions, in order
static int g_lDlssBatch = -1, g_rDlssBatch = -1;
// Left-view copies of the replay inputs, taken at the end of the left view's work (before the right view overwrites
// shared buffers). The input set comes from the previous frame's replay.
static std::vector<ID3D12Resource*> g_prevInputs;
static std::unordered_map<ID3D12Resource*, D3D12_RESOURCE_DESC> g_prevDesc;   // their descs, read while they were alive (a freed one crashed GetDesc after a load, test 105)
static std::unordered_map<ID3D12Resource*, Snap> g_leftSnaps;
static std::unordered_map<ID3D12Resource*, ID3D12Resource*> g_leftTaken;   // this frame: input -> its left copy
static std::atomic<int> g_leftInputs{1};
// Per-eye history (stereo_replay_history): textures the replayed range reads from the previous frame and rewrites
// (temporal post effects). Without this each eye reads the history the OTHER eye wrote last (the left replay runs after
// the right pass) - a constant eye-offset mismatch: noise / ringing in the right eye even with the head still (user, 2026-11-01).
static std::atomic<int> g_history{1};
static bool g_frameCamValid = false; static float g_frameCam[6] = {};
static uint64_t g_frameStamp = 0;
static std::atomic<bool> g_pairStamp{true};   // this frame's left upscaler list: newest main build serial at its Reset   // this frame's left camera (from the left evaluate)
static std::unordered_map<ID3D12Resource*, Snap> g_histL, g_histR;   // left's / right's copy of each history texture
static std::unordered_map<ID3D12Resource*, bool> g_histLValid;
static int g_histLogs = 3;
static int g_idLogs = 4;   // DLSS identification detail for the first stereo batches (after start / each size change)
static std::atomic<int> g_mode{0};             // 0 off, 1 replay stereo
static std::atomic<bool> g_stereoFrame{false}; // fbview: this frame renders two views (set at present of the frame)
static bool g_didReplay = false;
static bool g_leftSaved = false;
static bool g_rightEntered = false;
static std::atomic<bool> g_slSeen{false};
static std::atomic<int> g_lastSlFrame{-1000}, g_lastUpFrame{-1000};   // the upscaler can be switched in-game: DLSS / barrier mode follows the last 30 frames
static bool SlRecent();
static bool UpRecent();   // Streamline evaluates are annotated on the command lists   // this frame: the right view's DLSS output entered UAV (after the left one)
static std::atomic<int> g_debug{0};       // 1: identity replay (no left image swap) - must reproduce the right eye
static std::atomic<int> g_startShift{0};
static std::atomic<int> g_snapshot{2};    // snapshot/restore everything the replayed range touches  // debug: start the replay range this many lists later          // this frame's left DLSS output was copied to R.leftDlss
static std::atomic<uint64_t> s_replays{0}, s_noRange{0}, s_published{0}, s_ownBusy{0}, s_leftIn{0}, s_leftRest{0};
static int g_mapLogs = 6;
static std::vector<ID3D12Resource*> g_rangeSet;   // diagnostic readbacks

// replay resources (game device)
struct Res {
    ID3D12Device* dev = nullptr;
    static const int kSlots = 32;
    ID3D12CommandAllocator* alloc[kSlots] = {};
    ID3D12GraphicsCommandList* list[kSlots] = {};
    ID3D12Fence* fence = nullptr; uint64_t fenceVal = 0; uint64_t slotDone[kSlots] = {};
    ID3D12Resource* right = nullptr; D3D12_RESOURCE_DESC rightDesc{};
    ID3D12Resource* leftDlss = nullptr; D3D12_RESOURCE_DESC leftDesc{};   // persistent copy of the left view's DLSS output
    ID3D12Resource* hud = nullptr; ID3D12Resource* hudZero = nullptr; D3D12_RESOURCE_DESC hudDesc{};   // UI image copy + transparent black
    int slot = 0;
} R;

static void Barrier(ID3D12GraphicsCommandList* l, ID3D12Resource* r, uint32_t a, uint32_t b) {
    if (a == b) return;
    D3D12_RESOURCE_BARRIER br{}; br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; br.Transition.pResource = r;
    br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    br.Transition.StateBefore = (D3D12_RESOURCE_STATES)a; br.Transition.StateAfter = (D3D12_RESOURCE_STATES)b;
    o_Barrier(l, 1, &br);   // through the original (not recorded)
}

static int g_frameNo = 0;
static std::vector<std::pair<int, ID3D12Resource*>> g_grave;   // (frame retired, clone)
static bool EnsureLeftCopy(ID3D12Resource* like) {
    D3D12_RESOURCE_DESC d = Desc(like);
    if (R.leftDlss && R.leftDesc.Width == d.Width && R.leftDesc.Height == d.Height && R.leftDesc.Format == d.Format) return true;
    if (R.leftDlss) { g_grave.push_back({g_frameNo, R.leftDlss}); R.leftDlss = nullptr; }   // GPU may still read it
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = d; rd.Flags = D3D12_RESOURCE_FLAG_NONE; rd.MipLevels = 1; rd.Alignment = 0; rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN; rd.DepthOrArraySize = 1;
    if (FAILED(R.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, __uuidof(ID3D12Resource), (void**)&R.leftDlss))) {
        LOGE("replay: left DLSS copy %llux%u fmt %d failed", (unsigned long long)d.Width, d.Height, (int)d.Format); return false;
    }
    R.leftDesc = d;
    R.leftDlss->SetName(L"davgvr left dlss");
    LOGI("replay: left-eye DLSS copy texture %llux%u fmt %d", (unsigned long long)d.Width, d.Height, (int)d.Format);
    return true;
}

static bool EnsureRes(ID3D12CommandQueue* q, ID3D12Resource* like) {
    if (!R.dev) {
        if (FAILED(q->GetDevice(__uuidof(ID3D12Device), (void**)&R.dev))) return false;
        for (int i = 0; i < Res::kSlots; i++) {
            if (FAILED(R.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void**)&R.alloc[i]))) return false;
            if (FAILED(R.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, R.alloc[i], nullptr, __uuidof(ID3D12GraphicsCommandList), (void**)&R.list[i]))) return false;
            R.list[i]->Close();
            wchar_t nm[48]; swprintf_s(nm, L"davgvr replay list %d", i); R.list[i]->SetName(nm);
        }
        if (FAILED(R.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void**)&R.fence))) return false;
        LOGI("replay: resources ready on device %p", R.dev);
    }
    if (!like) return true;
    D3D12_RESOURCE_DESC d = Desc(like);
    if (!R.right || R.rightDesc.Width != d.Width || R.rightDesc.Height != d.Height || R.rightDesc.Format != d.Format) {
        if (R.right) { R.right->Release(); R.right = nullptr; }   // previous uses completed long ago (size changes are rare)
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd = d; rd.Flags = D3D12_RESOURCE_FLAG_NONE; rd.MipLevels = 1;
        if (FAILED(R.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, __uuidof(ID3D12Resource), (void**)&R.right))) {
            LOGE("replay: right-eye texture %llux%u fmt %d failed", (unsigned long long)d.Width, d.Height, (int)d.Format); return false;
        }
        R.rightDesc = d;
        R.right->SetName(L"davgvr right eye");
        LOGI("replay: right-eye texture %llux%u fmt %d", (unsigned long long)d.Width, d.Height, (int)d.Format);
    }
    return true;
}

static PFN_ECL g_realEcl = nullptr;

// ---------------------------------------------------------------- HUD layer
// The game draws its UI into an output-size RGBA8 (sRGB) target late in the final stage, and the composite pass then
// blends it over the tonemapped scene with one draw. With the HUD layer on, the UI image is copied out right before
// the composite and the target is overwritten with transparent black, so both eyes are composited without the HUD.
static std::atomic<int> g_hudLayer{1};
static std::atomic<int> g_hudProbe{0};
static std::atomic<bool> g_swapEyes{false};   // debug: left image to the right eye and vice versa
static int g_stereoStreak = 0;   // consecutive frames with a stereo replay
static char g_probeTags[8][24];
static std::atomic<uint64_t> s_hud{0};
static bool IsUiLike(ID3D12Resource* r) {
    D3D12_RESOURCE_DESC d = Desc(r);
    return d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.Width == g_outW && d.Height == g_outH && d.DepthOrArraySize == 1 &&
           d.MipLevels == 1 && (d.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || d.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS || d.Format == DXGI_FORMAT_R8G8B8A8_UNORM);
}
static bool EnsureHud(ID3D12Resource* like) {
    D3D12_RESOURCE_DESC d = Desc(like);
    if (R.hud && R.hudDesc.Width == d.Width && R.hudDesc.Height == d.Height && R.hudDesc.Format == d.Format) return true;
    if (R.hud) { R.hud->Release(); R.hud = nullptr; }
    if (R.hudZero) { R.hudZero->Release(); R.hudZero = nullptr; }
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = d; rd.Flags = D3D12_RESOURCE_FLAG_NONE;
    // committed resources in default heaps start zeroed: hudZero stays transparent black (never written)
    if (FAILED(R.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, __uuidof(ID3D12Resource), (void**)&R.hud)) ||
        FAILED(R.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, __uuidof(ID3D12Resource), (void**)&R.hudZero))) {
        LOGE("replay: HUD textures %llux%u fmt %d failed", (unsigned long long)d.Width, d.Height, (int)d.Format);
        if (R.hud) { R.hud->Release(); R.hud = nullptr; }
        return false;
    }
    R.hudDesc = d;
    R.hud->SetName(L"davgvr hud copy"); R.hudZero->SetName(L"davgvr hud zero");
    LOGI("replay: HUD textures %llux%u fmt %d", (unsigned long long)d.Width, d.Height, (int)d.Format);
    return true;
}
// Own list: optionally copy the UI image out, then blank it (state `st` before and after).
static void HudGrabAndBlank(ID3D12GraphicsCommandList* l, ID3D12Resource* ui, uint32_t st, bool grab) {
    const int mode = g_hudLayer.load();   // 1 grab + blank, 2 grab only (debug), 3 blank only (debug)
    if (mode == 3) grab = false;
    if (mode == 2) {
        if (grab) {
            Barrier(l, ui, st, D3D12_RESOURCE_STATE_COPY_SOURCE);
            Barrier(l, R.hud, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
            l->CopyResource(R.hud, ui);
            Barrier(l, R.hud, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
            Barrier(l, ui, D3D12_RESOURCE_STATE_COPY_SOURCE, st);
        }
        return;
    }
    if (grab) {
        Barrier(l, ui, st, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Barrier(l, R.hud, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        l->CopyResource(R.hud, ui);
        Barrier(l, R.hud, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
        Barrier(l, ui, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    } else Barrier(l, ui, st, D3D12_RESOURCE_STATE_COPY_DEST);
    Barrier(l, R.hudZero, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    l->CopyResource(ui, R.hudZero);
    Barrier(l, R.hudZero, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    Barrier(l, ui, D3D12_RESOURCE_STATE_COPY_DEST, st);
}

// Snapshot clones of the resources the replayed range touches (identical descriptors, committed).
static void KeepAliveTrim();
static void KeepAlive(ID3D12Resource* r);
static std::unordered_map<ID3D12Resource*, Snap> g_snaps;
static ID3D12Resource* SnapIn(std::unordered_map<ID3D12Resource*, Snap>& map, ID3D12Resource* r, const D3D12_RESOURCE_DESC* known = nullptr);
static ID3D12Resource* SnapFor(ID3D12Resource* r) { return SnapIn(g_snaps, r); }
static ID3D12Resource* SnapIn(std::unordered_map<ID3D12Resource*, Snap>& map, ID3D12Resource* r, const D3D12_RESOURCE_DESC* known) {
    D3D12_RESOURCE_DESC d = known ? *known : r->GetDesc();
    Snap& sn = map[r];
    if (sn.clone && memcmp(&sn.desc, &d, sizeof(d)) == 0) return sn.clone;
    if (sn.clone) { g_grave.push_back({g_frameNo, sn.clone}); sn.clone = nullptr; }   // GPU may still use it: release later
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC cd = d;
    if (cd.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) cd.Flags &= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    else cd.Flags &= (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    if (cd.Layout == D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE || cd.Layout == D3D12_TEXTURE_LAYOUT_64KB_STANDARD_SWIZZLE) cd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    HRESULT hr = R.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &cd, D3D12_RESOURCE_STATE_COMMON, nullptr, __uuidof(ID3D12Resource), (void**)&sn.clone);
    if (FAILED(hr)) { sn.clone = nullptr; LOGW("replay: snapshot clone failed %08lX (dim %d %llux%u fmt %d flags %X)", hr, (int)d.Dimension, (unsigned long long)d.Width, d.Height, (int)d.Format, (unsigned)d.Flags); return nullptr; }
    sn.desc = d;
    static int created = 0;
    if (++created <= 64) LOGI("replay: snapshot clone for %p (dim %d %llux%u fmt %d mips %u)", r, (int)d.Dimension, (unsigned long long)d.Width, d.Height, (int)d.Format, d.MipLevels);
    return sn.clone;
}

// ---- debug: DLSS output dumps (RGBA16F -> 8-bit BMP), dumpdlss.trigger
static std::atomic<int> g_dlssDumpReq{0};
struct Readback { int fmt = 0; bool rgba8 = false; ID3D12Resource* buf = nullptr; D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{}; UINT rows = 0; UINT64 rowSize = 0, total = 0; uint64_t fence = 0; const char* tag = ""; };
static std::vector<Readback> g_rb;
static void RecordReadback(ID3D12GraphicsCommandList* l, ID3D12Resource* tex, uint32_t state, const char* tag) {
    D3D12_RESOURCE_DESC d = tex->GetDesc();
    Readback rb; rb.tag = tag; rb.fmt = (int)d.Format;
    rb.rgba8 = d.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS || d.Format == DXGI_FORMAT_R8G8B8A8_UNORM || d.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
               d.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS || d.Format == DXGI_FORMAT_B8G8R8A8_UNORM || d.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    R.dev->GetCopyableFootprints(&d, 0, 1, 0, &rb.fp, &rb.rows, &rb.rowSize, &rb.total);
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd{}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = rb.total; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(R.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, __uuidof(ID3D12Resource), (void**)&rb.buf))) return;
    Barrier(l, tex, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource = rb.buf; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = rb.fp;
    D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource = tex; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex = 0;
    o_CopyTex(l, &dst, 0, 0, 0, &src, nullptr);
    Barrier(l, tex, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
    g_rb.push_back(rb);
}
static float Half(uint16_t h) {
    uint32_t s = (h >> 15) & 1, e = (h >> 10) & 0x1f, m = h & 0x3ff;
    if (e == 0) return (s ? -1.f : 1.f) * m / 16777216.f;
    if (e == 31) return 0;
    return (s ? -1.f : 1.f) * std::ldexp(1.0f + m / 1024.f, (int)e - 15);
}
static void FinishReadbacks() {
    for (auto it = g_rb.begin(); it != g_rb.end();) {
        if (R.fence->GetCompletedValue() < it->fence) { ++it; continue; }
        uint8_t* p = nullptr; D3D12_RANGE rr{0, (SIZE_T)it->total};
        if (SUCCEEDED(it->buf->Map(0, &rr, (void**)&p))) {
            UINT w = it->fp.Footprint.Width, h = it->fp.Footprint.Height;
            const bool r16 = it->fmt == DXGI_FORMAT_R16G16B16A16_FLOAT || it->fmt == DXGI_FORMAT_R16G16B16A16_TYPELESS;
            if (!it->rgba8 && !r16) {   // other formats: raw rows, decoded offline
                char name[128]; snprintf(name, sizeof(name), "raw_%s_%03d_%ux%u_f%d_p%u.bin", it->tag, g_frame % 1000, w, h, it->fmt, it->fp.Footprint.RowPitch);
                std::wstring path = G().outDir + std::wstring(name, name + strlen(name));
                if (FILE* f = _wfopen(path.c_str(), L"wb")) { fwrite(p + it->fp.Offset, 1, (size_t)it->fp.Footprint.RowPitch * h, f); fclose(f); }
                LOGI("replay: dumped %s", name);
                it->buf->Unmap(0, nullptr); it->buf->Release(); it = g_rb.erase(it); continue;
            }
            std::vector<uint8_t> out((size_t)w * h * 3);
            double sum = 0, asum = 0;
            if (it->rgba8) for (UINT y = 0; y < h; y++) {
                const uint8_t* row = p + it->fp.Offset + (size_t)y * it->fp.Footprint.RowPitch;
                for (UINT x = 0; x < w; x++) {
                    uint8_t a = row[x * 4 + 3]; asum += a;
                    int bg = (255 - a) / 2;   // premultiplied colour over mid grey
                    for (int c = 0; c < 3; c++) { int v = row[x * 4 + c] + bg; uint8_t b = (uint8_t)(v > 255 ? 255 : v); out[((size_t)(h - 1 - y) * w + x) * 3 + (2 - c)] = b; sum += b; }
                }
            }
            else for (UINT y = 0; y < h; y++) {
                const uint16_t* row = (const uint16_t*)(p + it->fp.Offset + (size_t)y * it->fp.Footprint.RowPitch);
                for (UINT x = 0; x < w; x++) for (int c = 0; c < 3; c++) {
                    float v = Half(row[x * 4 + c]); v = v / (1.0f + v);                // simple tonemap
                    v = v <= 0 ? 0 : std::pow(v, 1.0f / 2.2f);
                    uint8_t b = (uint8_t)(v >= 1 ? 255 : v * 255.0f);
                    out[((size_t)(h - 1 - y) * w + x) * 3 + (2 - c)] = b; sum += b;
                }
            }
            it->buf->Unmap(0, nullptr);
            char name[96]; snprintf(name, sizeof(name), "%s_%s_%03d.bmp", it->rgba8 ? "rt8" : "dlss", it->tag, g_frame % 1000);
            std::wstring path = G().outDir + std::wstring(name, name + strlen(name));
            FILE* f = _wfopen(path.c_str(), L"wb");
            if (f) {
                uint32_t rowB = (w * 3 + 3) & ~3u, img = rowB * h;
                uint8_t hdr[54] = {'B', 'M'}; *(uint32_t*)(hdr + 2) = 54 + img; *(uint32_t*)(hdr + 10) = 54; *(uint32_t*)(hdr + 14) = 40;
                *(int32_t*)(hdr + 18) = (int32_t)w; *(int32_t*)(hdr + 22) = (int32_t)h; *(uint16_t*)(hdr + 26) = 1; *(uint16_t*)(hdr + 28) = 24; *(uint32_t*)(hdr + 34) = img;
                fwrite(hdr, 1, 54, f);
                std::vector<uint8_t> pad(rowB - w * 3);
                for (UINT y = 0; y < h; y++) { fwrite(&out[(size_t)y * w * 3], 1, w * 3, f); if (!pad.empty()) fwrite(pad.data(), 1, pad.size(), f); }
                fclose(f);
            }
            LOGI("replay: dumped %s (%ux%u, mean %.1f, mean alpha %.1f)", name, w, h, sum / ((double)w * h * 3), asum / ((double)w * h));
        }
        it->buf->Release();
        it = g_rb.erase(it);
    }
}

// Records our own small command list (copies + state fixes) and executes it on q (never blocks).

template <typename F> static bool RunOwnList(ID3D12CommandQueue* q, F record) {
    int s = R.slot; R.slot = (R.slot + 1) % Res::kSlots;
    if (R.fence->GetCompletedValue() < R.slotDone[s]) { s_ownBusy++; return false; }   // never block the render thread
    R.alloc[s]->Reset();
    ID3D12GraphicsCommandList* l = R.list[s];
    l->Reset(R.alloc[s], nullptr);
    record(l);
    if (FAILED(l->Close())) { LOGE("replay: own list Close failed"); return false; }
    ID3D12CommandList* ls[] = {l};
    g_realEcl(q, 1, ls);
    q->Signal(R.fence, ++R.fenceVal);
    R.slotDone[s] = R.fenceVal;
    return true;
}

// ---------------------------------------------------------------- GPU timing of our own work (log every 10 s)
// Timestamps are written into lists we submit anyway (left save, snapshots, restore) plus one list after the replay
// and one after the hand-off. Per frame: left-save copy, left-input copies, snapshots, the game's post range for
// the right eye, restore, left replay, XR hand-off, and the GPU frame period on this queue.
namespace tm {
enum Mark { LSave0, LSave1, LIn0, LIn1, Snap0, Snap1, Post1, Restore1, Replay1, Publish1, kMarks };
const int kFrames = 8;
ID3D12QueryHeap* heap = nullptr; ID3D12Resource* rb = nullptr; double toMs = 0;
uint64_t slotFence[kFrames] = {}; uint32_t slotMask[kFrames] = {}; bool slotBusy[kFrames] = {};
int cur = 0; uint32_t curMask = 0; bool on = false;
uint64_t snapBytes = 0, restoreBytes = 0, linBytes = 0; int snapCount = 0, restoreCount = 0, linCount = 0;
double sum[16] = {}; int cnt[16] = {}; uint64_t lastPublish = 0; double sumBytes[3] = {}; int frames = 0;
bool Ensure(ID3D12CommandQueue* q) {
    if (heap) return true;
    if (!R.dev) return false;
    D3D12_QUERY_HEAP_DESC hd{}; hd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP; hd.Count = kFrames * kMarks;
    if (FAILED(R.dev->CreateQueryHeap(&hd, __uuidof(ID3D12QueryHeap), (void**)&heap))) { LOGW("timing: no query heap"); return false; }
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd{}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = kFrames * kMarks * 8; bd.Height = 1; bd.DepthOrArraySize = 1;
    bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(R.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, __uuidof(ID3D12Resource), (void**)&rb))) return false;
    UINT64 f = 0; q->GetTimestampFrequency(&f);
    toMs = f ? 1000.0 / double(f) : 0;
    LOGI("timing: GPU timestamps on (%llu Hz)", (unsigned long long)f);
    return true;
}
// start of a frame's marks (first marked list of the frame)
void Begin() { if (!on) { on = true; curMask = 0; snapBytes = restoreBytes = linBytes = 0; snapCount = restoreCount = linCount = 0; } }
void Write(ID3D12GraphicsCommandList* l, Mark m) {
    if (!heap || slotBusy[cur]) return;
    Begin();
    l->EndQuery(heap, D3D12_QUERY_TYPE_TIMESTAMP, cur * kMarks + m);
    curMask |= 1u << m;
}
uint64_t Bytes(ID3D12Resource* r) { D3D12_RESOURCE_DESC d = Desc(r); return R.dev->GetResourceAllocationInfo(0, 1, &d).SizeInBytes; }
// in the hand-off list: last mark + resolve; the slot is read back once R.fence passes
void End(ID3D12GraphicsCommandList* l) {
    if (!heap || !on || slotBusy[cur]) { on = false; return; }
    Write(l, Publish1);
    l->ResolveQueryData(heap, D3D12_QUERY_TYPE_TIMESTAMP, cur * kMarks, kMarks, rb, (UINT64)cur * kMarks * 8);
    slotMask[cur] = curMask; slotBusy[cur] = true;
    sumBytes[0] += snapBytes / 1048576.0; sumBytes[1] += restoreBytes / 1048576.0; sumBytes[2] += linBytes / 1048576.0;
    on = false;
}
void Committed(uint64_t fence) {   // after End's list was submitted
    if (slotBusy[cur] && !slotFence[cur]) { slotFence[cur] = fence; cur = (cur + 1) % kFrames; }
}
void Collect() {
    if (!heap || !R.fence) return;
    uint64_t done = R.fence->GetCompletedValue();
    for (int sl = 0; sl < kFrames; sl++) {
        if (!slotBusy[sl] || !slotFence[sl] || done < slotFence[sl]) continue;
        uint64_t* p = nullptr; D3D12_RANGE rr{(SIZE_T)sl * kMarks * 8, (SIZE_T)(sl + 1) * kMarks * 8};
        if (SUCCEEDED(rb->Map(0, &rr, (void**)&p))) {
            const uint64_t* t = p + sl * kMarks; uint32_t m = slotMask[sl];
            auto add = [&](int k, int a, int b) { if ((m >> a & 1) && (m >> b & 1) && t[b] >= t[a]) { sum[k] += (t[b] - t[a]) * toMs; cnt[k]++; } };
            add(0, LSave0, LSave1); add(1, LIn0, LIn1); add(2, Snap0, Snap1); add(3, Snap1, Post1); add(4, Post1, Restore1);
            add(5, Restore1, Replay1); add(6, Replay1, Publish1);
            if (m >> Publish1 & 1) { if (lastPublish && t[Publish1] > lastPublish) { sum[7] += (t[Publish1] - lastPublish) * toMs; cnt[7]++; } lastPublish = t[Publish1]; }
            frames++;
            D3D12_RANGE wr{0, 0}; rb->Unmap(0, &wr);
        }
        slotBusy[sl] = false; slotFence[sl] = 0;
    }
}
void Report() {
    if (!frames) return;
    auto avg = [&](int k) { return cnt[k] ? sum[k] / cnt[k] : 0.0; };
    LOGI("timing (GPU ms per frame, %d frames): left DLSS save %.2f | left input copies %.2f (%.0f MB) | snapshots %.2f (%.0f MB) | "
         "game post (right eye) %.2f | restore %.2f (%.0f MB) | left replay %.2f | XR hand-off %.2f | frame period %.2f",
         frames, avg(0), avg(1), sumBytes[2] / frames, avg(2), sumBytes[0] / frames, avg(3), avg(4), sumBytes[1] / frames, avg(5), avg(6), avg(7));
    for (auto& x : sum) x = 0;
    for (auto& x : cnt) x = 0;
    sumBytes[0] = sumBytes[1] = sumBytes[2] = 0; frames = 0;
}
} // namespace tm

// Called from the ExecuteCommandLists hook (render thread). Returns true if it executed the batch itself.
// The frame state below (g_state, g_frameEvs, g_dlssOut, ...) is shared by every ExecuteCommandLists caller and by
// Present. The game submits on several DIRECT queues (the swapchain's own queue only presents; the frame is rendered
// on another one), possibly from other threads: everything here runs under one lock (user crash 2026-10-30: heap
// corruption in g_state.clear() at Present). Filtering to "the composite queue" was tried and broke stereo (test 055:
// the queue roles change between logos / title / game), so all queues are still tracked, just serialized.
static std::atomic<ID3D12CommandQueue*> g_mainQueue{nullptr};
static std::recursive_mutex g_execMu;
void ReplaySetMainQueue(ID3D12CommandQueue*) {}

// Per-view scene history (stereo_scene_history): textures the scene rendering reads from "last frame" and rewrites
// (temporal denoisers / AO / reflections / volumetrics). The engine keeps one copy for both views, so each view read what the
// other view wrote last: noise baked into the right IMAGE (swap eyes moves it; per-eye DLSS / post history: no change; not on
// the title screen - user 2026-11-02).
// Learned per frame from the barrier stream: phase 0 = up to the left DLSS evaluate list, phase 1 = after it up to the right
// evaluate list. A texture whose first access in BOTH phases is a read and that is written in both = per-view history.
// Applied the next frame: right before the list where view 1 first touches it (any batch - the right view's work starts
// inside the left batch and spans several, test 115), save the left view's version and put the right view's own (from last
// frame) in; after the right evaluate batch save the right view's and put the left view's back.
static std::atomic<int> g_sceneHist{1};
static int g_evalOrder = 0;   // DLSS evaluates executed this frame
static int g_upOrder = 0;     // upscaler passes found from barriers this frame (no Streamline)
static std::atomic<bool> g_upSeen{false};
static std::atomic<bool> g_upscalerDetect{true};
static std::atomic<uint64_t> s_eyeOrderMismatch{0};
struct PhaseUse { int8_t first = -1; bool written = false; };   // first: 1 read, 0 write
static std::unordered_map<ID3D12Resource*, PhaseUse> g_ph0, g_ph1;  // this frame
static int g_phase = 0;                                             // this frame: 0 before / at the left evaluate, 1 after, 2 after the right one
static std::unordered_map<ID3D12Resource*, int> g_histSet;          // learned (value = frames seen, decays)
static std::unordered_map<ID3D12Resource*, Snap> g_scH0, g_scH1;
static std::unordered_map<ID3D12Resource*, bool> g_scH1Valid;
static std::unordered_map<ID3D12Resource*, uint32_t> g_scSwapped;   // this frame: swapped in -> state after the swap point's first event... (unused value)
static std::unordered_map<ID3D12CommandList*, std::vector<std::pair<ID3D12Resource*, uint32_t>>> g_swapAt;   // list -> (texture, state before)
static int g_scLogs = 3;
static std::atomic<uint64_t> s_scSwaps{0}, s_scRestores{0};
static const uint32_t kWriteStates = D3D12_RESOURCE_STATE_RENDER_TARGET | D3D12_RESOURCE_STATE_UNORDERED_ACCESS | D3D12_RESOURCE_STATE_COPY_DEST | D3D12_RESOURCE_STATE_DEPTH_WRITE;
static void NotePhaseUse(std::unordered_map<ID3D12Resource*, PhaseUse>& m, const Ev& e) {
    PhaseUse& u = m[e.res];
    if (u.first < 0) u.first = (e.before != D3D12_RESOURCE_STATE_COMMON && !(e.before & kWriteStates)) ? 1 : 0;
    if (e.after & kWriteStates) u.written = true;
}
// End of frame: learn the history set from this frame's phases.
static void LearnSceneHistory() {
    if (!g_sceneHist.load() || g_ph1.empty()) { g_ph0.clear(); g_ph1.clear(); g_phase = 0; g_scSwapped.clear(); g_swapAt.clear(); return; }
    for (auto it = g_histSet.begin(); it != g_histSet.end();) { if (--it->second <= 0) it = g_histSet.erase(it); else ++it; }
    int added = 0;
    for (auto& kv : g_ph1) {
        auto i0 = g_ph0.find(kv.first);
        if (i0 == g_ph0.end() || kv.second.first != 1 || !kv.second.written || i0->second.first != 1 || !i0->second.written) continue;
        D3D12_RESOURCE_DESC d = Desc(kv.first);
        if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.Width * 16 < g_outW) continue;
        if (!g_histSet.count(kv.first)) added++;
        g_histSet[kv.first] = 8;   // forget after 8 frames without the pattern
    }
    if (added && g_scLogs > 0) {
        g_scLogs--;
        std::string t; for (auto& kv : g_histSet) { D3D12_RESOURCE_DESC d = Desc(kv.first); char b[72]; snprintf(b, sizeof(b), "%p %llux%u f%d; ", (void*)kv.first, (unsigned long long)d.Width, d.Height, (int)d.Format); t += b; }
        LOGI("replay: per-view scene history textures (%zu): %s", g_histSet.size(), t.c_str());
    }
    g_ph0.clear(); g_ph1.clear(); g_phase = 0; g_scSwapped.clear(); g_swapAt.clear();
}
// ExecuteCommandLists that performs the planned swaps in front of the right lists.
static void STDMETHODCALLTYPE EclWithSwaps(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists) {
    UINT start = 0;
    for (UINT i = 0; i < n; i++) {
        auto it = g_swapAt.find(lists[i]);
        if (it == g_swapAt.end()) continue;
        if (i > start) g_realEcl(q, i - start, lists + start);
        start = i;
        auto sw = std::move(it->second); g_swapAt.erase(it);
        if (!EnsureRes(q, nullptr)) continue;
        RunOwnList(q, [&](ID3D12GraphicsCommandList* l) {
            for (auto& p : sw) {
                ID3D12Resource* r = p.first; uint32_t st = p.second;
                ID3D12Resource* c0 = SnapIn(g_scH0, r); ID3D12Resource* c1 = SnapIn(g_scH1, r);
                if (!c0 || !c1) continue;
                Barrier(l, r, st, D3D12_RESOURCE_STATE_COPY_SOURCE);
                Barrier(l, c0, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
                l->CopyResource(c0, r);
                Barrier(l, c0, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
                if (g_scH1Valid[r]) {
                    Barrier(l, r, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
                    Barrier(l, c1, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
                    l->CopyResource(r, c1);
                    Barrier(l, c1, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
                    Barrier(l, r, D3D12_RESOURCE_STATE_COPY_DEST, st);
                } else Barrier(l, r, D3D12_RESOURCE_STATE_COPY_SOURCE, st);
                g_scSwapped[r] = 1; s_scSwaps++;
            }
        });
    }
    if (start < n) g_realEcl(q, n - start, lists + start);
}
static bool ExecInner(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists, PFN_ECL realEcl, int k,
                      std::vector<std::vector<Ev>>& evs, std::vector<int>& dlEye, std::vector<ID3D12Resource*>& dlOut, std::vector<uint32_t>& dlEv,
                      std::vector<int>& dlCam, std::vector<std::array<float, 6>>& dlCamPos, std::vector<std::vector<RtRec>>& rts, bool rtLog);
bool ReplayOnExecute(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists, PFN_ECL realEcl) {
    g_realEcl = realEcl;
    if (!g_track.load()) return false;
    std::lock_guard<std::recursive_mutex> execLock(g_execMu);
    int k = g_eclInFrame++;
    // Snapshot this batch's events.
    std::vector<std::vector<Ev>> evs(n);
    std::vector<int> dlEye(n, -1); std::vector<ID3D12Resource*> dlOut(n, nullptr); std::vector<uint32_t> dlEv(n, 0);
    std::vector<int> dlCam(n, 0); std::vector<std::array<float, 6>> dlCamPos(n); std::vector<void*> dlDepth(n, nullptr); std::vector<uint64_t> dlStamp(n, 0);
    std::vector<std::vector<RtRec>> rts;
    const bool rtLog = g_rtTrace.load() == 1;
    if (rtLog) rts.resize(n);
    {
        std::lock_guard<std::mutex> lk(g_mu);
        for (UINT i = 0; i < n; i++) {
            auto it = g_lists.find((ID3D12GraphicsCommandList*)lists[i]);
            if (it != g_lists.end()) {
                evs[i] = std::move(it->second.ev); dlEye[i] = it->second.dlssEye; dlEv[i] = it->second.dlssEv; if (it->second.hasCam) { dlCam[i] = 1; for (int c = 0; c < 6; c++) dlCamPos[i][c] = it->second.cam[c]; } dlOut[i] = it->second.dlssOut; dlDepth[i] = it->second.depth; dlStamp[i] = it->second.stamp; if (rtLog) rts[i] = std::move(it->second.rt);
                // consumed: a list created later at the same address (CreateCommandList returns it open, no Reset) must not
                // inherit this record - a stale DLSS output / barrier resource crashed the driver in our copies (tests 065, 066)
                g_lists.erase(it);
            }
        }
    }
    // No Streamline (DLSS off: FSR / XeSS / TAA built into the game): find each view's upscaler pass from its barriers
    // (test 137). Signature: one command list where >= 2 output-size textures enter UAV (history ping-pong + work
    // buffers); its output = the last output-size RGBA16 texture that LEAVES UAV in that list without having entered UAV
    // there (FSR: f9 8>C0, histories are f10 40>8 / 8>40). The 1st such list in a frame is the left view.
    std::vector<char> synth(n, 0);
    if (!SlRecent() && g_upscalerDetect.load()) {
        for (UINT i = 0; i < n; i++) {
            const auto& ev = evs[i];
            int enter = 0; ID3D12Resource* out = nullptr; uint32_t outIdx = 0;
            for (size_t a = 0; a < ev.size(); a++) {
                const Ev& e = ev[a];
                if (e.kind != 0) continue;
                D3D12_RESOURCE_DESC d = Desc(e.res);
                if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.Width != g_outW || d.Height != g_outH) continue;
                if (e.after == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) enter++;
                else if (e.before == D3D12_RESOURCE_STATE_UNORDERED_ACCESS && IsOutputRgba16f(e.res)) {
                    bool entered = false;
                    for (size_t b = 0; b < a; b++) if (ev[b].kind == 0 && ev[b].res == e.res && ev[b].after == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) { entered = true; break; }
                    if (!entered) { out = e.res; outIdx = (uint32_t)a; }
                }
            }
            if (enter >= 2 && out && g_upOrder < 2) {
                g_lastUpFrame = g_frame;
                dlEye[i] = g_upOrder++; dlOut[i] = out; dlEv[i] = UINT32_MAX; synth[i] = 1; (void)outIdx;   // UINT32_MAX: output already exact
                KeepAlive(out);
                if (!g_upSeen.exchange(true)) LOGI("replay: no Streamline - upscaler passes identified from barriers (list %u, output %p)", i, out);
                static int lg = 0;
                if (lg++ < 6) LOGI("replay: upscaler view %d: list %u/%u output %p (%d output-size UAV writes)", dlEye[i], i, n, out, enter);
            }
        }
    }
    for (UINT i = 0; i < n; i++) if (dlEye[i] == 0 && dlStamp[i]) g_frameStamp = dlStamp[i];   // left view's list: build serial when it started recording
    // GPU execution order of the DLSS evaluates teaches slhook which depth texture belongs to which view
    for (UINT i = 0; i < n; i++) if (dlEye[i] >= 0 && g_evalOrder < 2) { SlHookLearnDepthEye(dlDepth[i], g_evalOrder); if (dlEye[i] != g_evalOrder) s_eyeOrderMismatch++; g_evalOrder++; }
    // scene history: phases (learning) + swap plan for this batch
    int lE = -1, rE = -1;
    for (UINT i = 0; i < n; i++) { if (dlEye[i] == 0) lE = (int)i; else if (dlEye[i] == 1) rE = (int)i; }
    const bool sceneOn = g_sceneHist.load() && g_mode.load() == 1 && g_stereoFrame.load();
    bool planned = false;
    if (sceneOn) {
        for (UINT i = 0; i < n; i++) {
            for (auto& e : evs[i]) {
                if (e.kind != 0) continue;
                if (g_phase == 0) NotePhaseUse(g_ph0, e);
                else if (g_phase == 1) {
                    bool firstForView1 = !g_ph1.count(e.res);
                    NotePhaseUse(g_ph1, e);
                    // view 1's first access of a learned history texture: swap right before this list
                    if (firstForView1 && g_histSet.count(e.res) && !g_scSwapped.count(e.res) &&
                        e.before != D3D12_RESOURCE_STATE_COMMON && !(e.before & kWriteStates)) {   // still the history pattern (read first)
                        g_swapAt[lists[i]].push_back({e.res, e.before});
                        g_scSwapped[e.res] = 1; planned = true;
                    }
                }
            }
            if ((int)i == lE && g_phase == 0) g_phase = 1;
            if ((int)i == rE && g_phase == 1) g_phase = 2;
        }
    }
    PFN_ECL ecl = planned ? (PFN_ECL)EclWithSwaps : realEcl;
    bool handled = ExecInner(q, n, lists, ecl, k, evs, dlEye, dlOut, dlEv, dlCam, dlCamPos, rts, rtLog);
    if (!handled && planned) { EclWithSwaps(q, n, lists); handled = true; for (UINT i = 0; i < n; i++) for (auto& e : evs[i]) if (e.kind == 0) g_state[e.res] = e.after; }
    if (planned) g_swapAt.clear();   // a planned list that was not executed in this batch must not trigger later
    if (sceneOn && rE >= 0 && !g_scSwapped.empty()) {
        // after the right view: keep its version for next frame, put the left view's back (next frame's left view reads it)
        if (!handled) { realEcl(q, n, lists); handled = true; for (UINT i = 0; i < n; i++) for (auto& e : evs[i]) if (e.kind == 0) g_state[e.res] = e.after; }
        std::vector<ID3D12Resource*> done;
        for (auto& kv : g_scSwapped) done.push_back(kv.first);
        bool ok = EnsureRes(q, nullptr) && RunOwnList(q, [&](ID3D12GraphicsCommandList* l) {
            for (auto* r : done) {
                auto i0 = g_scH0.find(r); auto i1 = g_scH1.find(r);
                if (i0 == g_scH0.end() || i1 == g_scH1.end() || !i0->second.clone || !i1->second.clone || !g_state.count(r)) continue;
                uint32_t st = g_state[r];
                Barrier(l, r, st, D3D12_RESOURCE_STATE_COPY_SOURCE);
                Barrier(l, i1->second.clone, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
                l->CopyResource(i1->second.clone, r);
                Barrier(l, i1->second.clone, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
                Barrier(l, r, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
                Barrier(l, i0->second.clone, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
                l->CopyResource(r, i0->second.clone);
                Barrier(l, i0->second.clone, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
                Barrier(l, r, D3D12_RESOURCE_STATE_COPY_DEST, st);
                s_scRestores++;
            }
        });
        for (auto* r : done) g_scH1Valid[r] = ok;
        g_scSwapped.clear();
        g_scSwapped[nullptr] = 1;   // mark: done for this frame (no more swaps until Present)
    }
    return handled;
}

static bool ExecInner(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists, PFN_ECL realEcl, int k,
                      std::vector<std::vector<Ev>>& evs, std::vector<int>& dlEye, std::vector<ID3D12Resource*>& dlOut, std::vector<uint32_t>& dlEv,
                      std::vector<int>& dlCam, std::vector<std::array<float, 6>>& dlCamPos, std::vector<std::vector<RtRec>>& rts, bool rtLog) {
    // The DLSS output of each evaluate = the output-size RGBA16F texture Streamline transitions to UAV for it, on the
    // evaluate's own command list (the engine tags it on another list; the per-eye last tag lags behind transient
    // aliasing -> corrupt / black left eye, test 069).
    for (UINT i = 0; i < n; i++) {
        if (dlEye[i] < 0 || dlEv[i] == UINT32_MAX) continue;
        ID3D12Resource* found = nullptr;
        const auto& ev = evs[i];
        for (size_t k = std::min<size_t>(dlEv[i], ev.size()); k < ev.size() && !found; k++)
            if (ev[k].kind == 0 && ev[k].after == D3D12_RESOURCE_STATE_UNORDERED_ACCESS && IsOutputRgba16f(ev[k].res)) found = ev[k].res;
        for (size_t k = std::min<size_t>(dlEv[i], ev.size()); k-- > 0 && !found;)
            if (ev[k].kind == 0 && ev[k].after == D3D12_RESOURCE_STATE_UNORDERED_ACCESS && IsOutputRgba16f(ev[k].res)) found = ev[k].res;
        static int lg = 0;
        if (found && found != dlOut[i] && lg++ < 20) LOGI("replay: DLSS eye %d output from the evaluate's barriers %p (tag said %p)", dlEye[i], found, dlOut[i]);
        if (!found) { static int nf = 0; if (nf++ < 10) LOGW("replay: DLSS eye %d: no output barrier on the evaluate list (%zu events from %u) - using the tag %p", dlEye[i], ev.size(), dlEv[i], dlOut[i]); }
        if (found) dlOut[i] = found;
    }
    if (g_inputsDump.load() || g_mode.load() == 1)
        for (UINT i = 0; i < n; i++) for (auto& e : evs[i]) if (e.kind == 0 && g_frameEvs.size() < 200000) g_frameEvs.push_back({k, (int)i, e});
    if (g_traceArmed.load() > 0) {
        for (UINT i = 0; i < n; i++) {
            if (evs[i].empty()) continue;
            std::string s;
            bool full = false;   // lists with an output-size RGBA16F texture (upscaler: DLSS / FSR) are logged in full
            for (auto& e : evs[i]) if (e.kind == 0 && IsOutputRgba16f(e.res)) { full = true; break; }
            for (auto& e : evs[i]) {
                if (s.size() > (full ? 16000u : 1500u)) { s += " ..."; break; }
                D3D12_RESOURCE_DESC d = Desc(e.res);
                char b[96];
                if (e.kind == 1) snprintf(b, sizeof(b), "COPY->BB from %p; ", e.res);
                else if (d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.Width >= g_outW / 2) snprintf(b, sizeof(b), "%p %llux%u f%d %X>%X; ", e.res, (unsigned long long)d.Width, d.Height, (int)d.Format, e.before, e.after);
                else continue;
                s += b;
            }
            for (size_t o = 0, part = 0; o < s.size(); o += 1800, part++)   // log lines are max 2 KB
                LOGI("trace f%d ecl#%d list %u/%u%s: %s", g_frame, k, i, n, part ? " (cont)" : "", s.substr(o, 1800).c_str());
        }
    }
    // Per-view DLSS outputs: output-size RGBA16F textures entering UAV state, in order of appearance.
    int lastDlssList = -1;
    int compStart = -1, compEnd = -1;
    ID3D12Resource* comp = nullptr;
    for (UINT i = 0; i < n && !comp; i++)
        for (auto& e : evs[i])
            if (e.kind == 0 && e.before == D3D12_RESOURCE_STATE_COMMON && e.after == D3D12_RESOURCE_STATE_RENDER_TARGET && IsCompositeLike(e.res)) { comp = e.res; break; }
    // Preferred: the command lists Streamline recorded the per-view DLSS evaluates into (slhook -> ReplayNoteDlss).
    int lEval = -1, rEval = -1;
    for (UINT i = 0; i < n; i++) { if (dlEye[i] == 0) lEval = (int)i; else if (dlEye[i] == 1) rEval = (int)i; }
    const bool slMode = SlRecent() || UpRecent();
    if (lEval >= 0 && dlCam[lEval]) { g_frameCamValid = true; for (int c = 0; c < 6; c++) g_frameCam[c] = dlCamPos[lEval][c]; }
    if (slMode) {
        if (lEval >= 0 && !g_leftSaved) g_dlssOut[0] = dlOut[lEval];
        if (rEval >= 0) {
            g_dlssOut[1] = dlOut[rEval]; g_rightEntered = true;
            lastDlssList = rEval;
            for (int i = rEval; i < (int)n; i++)
                for (auto& e : evs[i]) if (e.kind == 0 && e.res == g_dlssOut[1] && e.before == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) { if (i > lastDlssList) lastDlssList = i; goto exitFound; }
            exitFound:;
        }
    }
    // Fallback (no Streamline annotations): right view's output = the next output-size RGBA16F UAV write after the left one was saved - possibly the same
    // (aliased) texture as the left one.
    for (UINT i = 0; i < n && !slMode; i++) {
        for (auto& e : evs[i]) {
            if (e.kind != 0) continue;
            if (e.after == D3D12_RESOURCE_STATE_UNORDERED_ACCESS && IsOutputRgba16f(e.res)) {
                if (!g_dlssOut[0]) g_dlssOut[0] = e.res;
                else if (!g_dlssOut[1] && (e.res != g_dlssOut[0] || g_leftSaved)) { g_dlssOut[1] = e.res; g_rightEntered = true; }
            }
            if (e.before == D3D12_RESOURCE_STATE_UNORDERED_ACCESS && e.res == g_dlssOut[1] && g_rightEntered) { lastDlssList = (int)i; }
        }
    }
    for (UINT i = 0; i < n && comp; i++)
        for (auto& e : evs[i]) {
            if (e.kind != 0 || e.res != comp) continue;
            if (e.after == D3D12_RESOURCE_STATE_RENDER_TARGET && compStart < 0) compStart = (int)i;
            if (e.before == D3D12_RESOURCE_STATE_RENDER_TARGET) compEnd = (int)i;
        }
    if (compStart >= 0 && g_mainQueue.exchange(q) != q) LOGI("replay: composite rendered on queue %p", q);
    // Left view's DLSS output finished in this batch (and the right one not yet seen): save it right away, before the
    // frame graph reuses its memory for the right view (transient aliasing).
    if (g_mode.load() == 1 && g_stereoFrame.load() && g_dlssOut[0] && !g_leftSaved && compStart < 0) {
        int j = -1;
        for (UINT i = slMode ? (UINT)std::max(lEval, 0) : 0; i < n && j < 0; i++)
            for (auto& e : evs[i]) if (e.kind == 0 && e.res == g_dlssOut[0] && e.before == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) { j = (int)i; break; }
        if (slMode && (lEval < 0 || (j >= 0 && rEval >= 0 && j > rEval))) j = -1;   // only in the batch with the left evaluate
        if (slMode && j < 0 && lEval >= 0) j = lEval;   // no barrier after the evaluate in this batch: right after it
        // sanity: the left DLSS output must be an output-size texture (a stale / wrong pointer crashed the driver, test 065)
        if (j >= 0) {
            D3D12_RESOURCE_DESC dd = Desc(g_dlssOut[0]);
            if (dd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || dd.Width != g_outW || dd.Height != g_outH) {
                static int warned = 0;
                if (warned++ < 10) LOGW("replay: left DLSS output %p is %llux%u, output is %ux%u - skipped", g_dlssOut[0], (unsigned long long)dd.Width, dd.Height, g_outW, g_outH);
                j = -1;
            }
        }
        if (j >= 0) {
            static ID3D12Resource* lastL = nullptr; static int nlog = 0;
            if (g_dlssOut[0] != lastL && nlog++ < 40) {
                D3D12_RESOURCE_DESC dd = Desc(g_dlssOut[0]);
                LOGI("replay: left DLSS output now %p (%llux%u fmt %d mips %u array %u flags %X layout %d align %llu), list %d of %u", g_dlssOut[0],
                     (unsigned long long)dd.Width, dd.Height, (int)dd.Format, dd.MipLevels, dd.DepthOrArraySize, (unsigned)dd.Flags, (int)dd.Layout,
                     (unsigned long long)dd.Alignment, j, n);
            }
            lastL = g_dlssOut[0];
        }
        if (j >= 0 && EnsureRes(q, nullptr) && EnsureLeftCopy(g_dlssOut[0])) {
            uint32_t st = 0;
            for (int i = 0; i <= j; i++) for (auto& e : evs[i]) if (e.kind == 0) { g_state[e.res] = e.after; if (e.res == g_dlssOut[0]) st = e.after; }
            realEcl(q, (UINT)(j + 1), lists);
            tm::Ensure(q);
            bool ok = RunOwnList(q, [&](ID3D12GraphicsCommandList* l) {
                tm::Write(l, tm::LSave0);
                Barrier(l, g_dlssOut[0], st, D3D12_RESOURCE_STATE_COPY_SOURCE);
                Barrier(l, R.leftDlss, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
                D3D12_TEXTURE_COPY_LOCATION dst{R.leftDlss, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}}, src{g_dlssOut[0], D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
                l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);   // mip 0 only (CopyResource needs identical descs)
                Barrier(l, R.leftDlss, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
                Barrier(l, g_dlssOut[0], D3D12_RESOURCE_STATE_COPY_SOURCE, st);
                tm::Write(l, tm::LSave1);
            });
            g_leftSaved = ok;
            // Left copies of last frame's replay inputs (the post work after DLSS, e.g. the R11G11B10 colour + exposure).
            // The left view writes them after its evaluate, possibly several lists later in this batch, and later lists may
            // reuse the memory (transient aliasing): copy each right after the list where the left view last made it readable.
            // Copying at the end of the batch gave a dark left eye at headset size, copying right after the evaluate gave
            // the previous frame's image (identical eyes) - tests 071-084.
            int K = j;   // copy point (inclusive list index)
            std::vector<std::pair<ID3D12Resource*, ID3D12Resource*>> cp;   // (right-range input, its clone)
            std::vector<ID3D12Resource*> srcOf;
            std::string mapLog;
            if (g_leftInputs.load() && !g_prevInputs.empty()) {
                auto readable = [](uint32_t st) { return st != D3D12_RESOURCE_STATE_COMMON && !(st & (D3D12_RESOURCE_STATE_RENDER_TARGET | D3D12_RESOURCE_STATE_UNORDERED_ACCESS | D3D12_RESOURCE_STATE_COPY_DEST | D3D12_RESOURCE_STATE_DEPTH_WRITE)); };
                std::unordered_map<ID3D12Resource*, int> lastRead;   // resource -> last list (this batch) where it became readable
                std::unordered_map<ID3D12Resource*, int> touched;    // resource -> order of its last event
                int ord = 0;
                for (UINT i = 0; i < n; i++) for (auto& e : evs[i]) if (e.kind == 0) { touched[e.res] = ord++; if (readable(e.after)) lastRead[e.res] = (int)i; else lastRead.erase(e.res); }
                for (auto* r : g_prevInputs) {
                    if (r == g_dlssOut[0]) continue;
                    // only image inputs (>= 1/4 of the output width): small ones (exposure, histograms, buffers) stay shared -
                    // one exposure for both eyes, and a late-written one darkened the left eye (test 085)
                    {
                        D3D12_RESOURCE_DESC rd = g_prevDesc[r];
                        if (rd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || rd.Width * 4 < g_outW) continue;
                        // no depth buffers: the right view's depth is written in this batch after the left post work (test 087)
                        switch (rd.Format) { case DXGI_FORMAT_R32G8X24_TYPELESS: case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: case DXGI_FORMAT_D32_FLOAT:
                            case DXGI_FORMAT_R24G8_TYPELESS: case DXGI_FORMAT_D24_UNORM_S8_UINT: case DXGI_FORMAT_D16_UNORM: continue; default: break; }
                        if (rd.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) continue;
                    }
                    ID3D12Resource* src = lastRead.count(r) && lastRead[r] > j ? r : nullptr;
                    if (!src) {   // the left view's own texture for it: same size / format, made readable latest
                        D3D12_RESOURCE_DESC rd = g_prevDesc[r];
                        int best = -1;
                        for (auto& kv : lastRead) {
                            if (kv.first == g_dlssOut[0] || kv.second <= j || touched[kv.first] <= best) continue;   // made readable after the evaluate
                            D3D12_RESOURCE_DESC cd = Desc(kv.first);
                            if (cd.Dimension == rd.Dimension && cd.Width == rd.Width && cd.Height == rd.Height && cd.Format == rd.Format &&
                                cd.DepthOrArraySize == rd.DepthOrArraySize && cd.MipLevels == rd.MipLevels) { best = touched[kv.first]; src = kv.first; }
                        }
                    }
                    if (!src) continue;
                    D3D12_RESOURCE_DESC sdesc = Desc(src);   // src was used in this batch: alive
                    ID3D12Resource* c = SnapIn(g_leftSnaps, r, &sdesc);
                    if (!c) continue;
                    cp.push_back({r, c}); srcOf.push_back(src);
                    if (lastRead[src] > K) K = lastRead[src];
                    D3D12_RESOURCE_DESC sd = Desc(src);
                    char b[112]; snprintf(b, sizeof(b), "%p<-%p@l%d %llux%u f%d; ", (void*)r, (void*)src, lastRead[src], (unsigned long long)sd.Width, sd.Height, (int)sd.Format); mapLog += b;
                }
            }
            if (K > j) {
                for (int i = j + 1; i <= K; i++) for (auto& e : evs[i]) if (e.kind == 0) g_state[e.res] = e.after;
                realEcl(q, (UINT)(K - j), lists + j + 1);
            }
            if (g_mapLogs > 0) { g_mapLogs--; LOGI("replay: left inputs copied after list %d (evaluate %d) of %u: %s", K, j, n, mapLog.c_str()); }
            if (!cp.empty() && RunOwnList(q, [&](ID3D12GraphicsCommandList* l) {
                    tm::Write(l, tm::LIn0);
                    for (size_t k = 0; k < cp.size(); k++) {
                        auto& p = cp[k]; ID3D12Resource* src = srcOf[k];
                        tm::linBytes += tm::heap ? tm::Bytes(src) : 0; tm::linCount++;
                        uint32_t st = g_state[src];
                        Barrier(l, src, st, D3D12_RESOURCE_STATE_COPY_SOURCE);
                        Barrier(l, p.second, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
                        l->CopyResource(p.second, src);
                        Barrier(l, p.second, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
                        Barrier(l, src, D3D12_RESOURCE_STATE_COPY_SOURCE, st);
                    }
                    tm::Write(l, tm::LIn1);
                })) { for (auto& p : cp) g_leftTaken[p.first] = p.second; s_leftIn++; }
            for (UINT i = (UINT)K + 1; i < n; i++) for (auto& e : evs[i]) if (e.kind == 0) g_state[e.res] = e.after;
            if ((UINT)(K + 1) < n) realEcl(q, n - (UINT)(K + 1), lists + K + 1);
            return true;
        }
    }
    if (g_dlssOut[0] && g_dlssOut[1]) SlHookSetOutputs(g_dlssOut[0], g_dlssOut[1]);
    if (g_idLogs > 0 && compStart >= 0 && g_leftSaved && g_mode.load() == 1 && g_stereoFrame.load()) {
        g_idLogs--;
        std::string line;
        for (UINT i = 0; i < n; i++)
            for (auto& e : evs[i]) {
                if (e.kind != 0 || !IsOutputRgba16f(e.res) || line.size() > 1800) continue;
                char b[80]; snprintf(b, sizeof(b), "l%u %p %X>%X%s; ", i, (void*)e.res, e.before, e.after,
                                     e.res == g_dlssOut[1] ? "*R" : e.res == g_dlssOut[0] ? "*L" : ""); line += b;
            }
        LOGI("replay: id batch n %u lEval %d rEval %d compStart %d L %p R %p leftSaved %d | %s", n, lEval, rEval, compStart, g_dlssOut[0], g_dlssOut[1], (int)g_leftSaved, line.c_str());
        for (int li = rEval; li >= 0 && li <= rEval + 2 && li < (int)n; li++) {
            std::string ll;
            for (auto& e : evs[li]) {
                if (ll.size() > 1800) break;
                D3D12_RESOURCE_DESC dd = Desc(e.res);
                char b[96]; snprintf(b, sizeof(b), "%p %llux%u f%d %X>%X k%d; ", (void*)e.res, (unsigned long long)dd.Width, dd.Height, (int)dd.Format, e.before, e.after, e.kind); ll += b;
            }
            LOGI("replay: id list %d (%zu events): %s", li, evs[li].size(), ll.c_str());
        }
    }
    bool want = g_mode.load() == 1 && g_stereoFrame.load() && compStart >= 0 && compEnd >= compStart && !g_didReplay && g_leftSaved;
    if (want && g_dlssOut[1]) {   // the right output receives the saved left one: same size and format, or no replay
        D3D12_RESOURCE_DESC rd = Desc(g_dlssOut[1]);
        if (rd.Width != R.leftDesc.Width || rd.Height != R.leftDesc.Height || rd.Format != R.leftDesc.Format) {
            static int warned = 0;
            if (warned++ < 10) LOGW("replay: right DLSS output %p (%llux%u fmt %d) does not match the left copy (%llux%u fmt %d) - frame not replayed",
                                   g_dlssOut[1], (unsigned long long)rd.Width, rd.Height, (int)rd.Format, (unsigned long long)R.leftDesc.Width, R.leftDesc.Height, (int)R.leftDesc.Format);
            want = false;
        }
    }
    if (!want) {
        if (g_mode.load() == 1 && g_stereoFrame.load() && compStart >= 0) {
            s_noRange++;
            static DWORD tl = 0; DWORD now = GetTickCount();
            if (now - tl > 3000) { tl = now; LOGI("replay: no range (A): batch ecl#%d n %u, composite %d..%d, DLSS L %p R %p rightEntered %d, lastDlss %d, leftSaved %d, didReplay %d",
                k, n, compStart, compEnd, g_dlssOut[0], g_dlssOut[1], (int)g_rightEntered, lastDlssList, (int)g_leftSaved, (int)g_didReplay); }
        }
        // keep state tracking up to date
        for (UINT i = 0; i < n; i++) for (auto& e : evs[i]) if (e.kind == 0) g_state[e.res] = e.after;
        realEcl(q, n, lists);
        return true;
    }
    // The list that ends the right view's DLSS write (UAV -> SRV barrier) also holds its first consumers: start there.
    int s = lastDlssList + (slMode ? 1 : g_startShift.load());
    if (rtLog) {
        LOGI("hudtrace: batch ecl#%d %u lists, lastDlss %d, replay start %d, composite %p lists %d..%d", k, n, lastDlssList, s, comp, compStart, compEnd);
        for (int i = std::max(0, lastDlssList - 1); i < (int)n; i++) {
            std::string line;
            for (auto& r : rts[i]) {
                if (line.size() > 1400) { line += " ..."; break; }
                char b[160];
                if (r.nRt == 0xD15) snprintf(b, sizeof(b), "DISPATCH x%u (%ux%ux%u) @e%u; ", r.draws, r.w, r.h, r.fmt, r.evIdx);
                else if (r.nRt == 0xC0F) snprintf(b, sizeof(b), "COPY %p -> %s%p %ux%u f%u @e%u; ", (void*)r.src, r.rt0 == comp && comp ? "COMP " : "", r.rt0, r.w, r.h, r.fmt, r.evIdx);
                else snprintf(b, sizeof(b), "%s%s %p %ux%u f%u n%u d%u @e%u; ", r.clears ? "CLEAR " : "", r.rt0 == comp && comp ? "COMP" : "rt", r.rt0,
                         r.w, r.h, r.fmt, r.clears ? 0 : r.nRt, r.draws, r.evIdx);
                line += b;
            }
            std::string tr;
            for (size_t ei = 0; ei < evs[i].size(); ei++) {
                auto& e = evs[i][ei];
                if (e.kind != 0 || tr.size() > 1400) continue;
                D3D12_RESOURCE_DESC dd = Desc(e.res);
                if (e.res != comp && !(dd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && dd.Width * 4 >= g_outW)) continue;
                char b[96]; snprintf(b, sizeof(b), "e%zu %s%p %llux%u f%d %X>%X; ", ei, e.res == comp ? "COMP " : "", (void*)e.res, (unsigned long long)dd.Width, dd.Height, (int)dd.Format, e.before, e.after); tr += b;
            }
            LOGI("hudtrace: list %d: %s", i, line.empty() ? "-" : line.c_str());
            LOGI("hudtrace: list %d barriers: %s", i, tr.c_str());
        }
        LogFlush();
    }
    if (lastDlssList < 0 || !g_dlssOut[0] || !g_dlssOut[1] || s > compStart) {
        s_noRange++;
        { static DWORD tl = 0; DWORD now = GetTickCount();
          if (now - tl > 3000) { tl = now; LOGI("replay: no range (B): batch ecl#%d n %u, composite %d..%d, DLSS L %p R %p, lastDlss %d, start %d",
              k, n, compStart, compEnd, g_dlssOut[0], g_dlssOut[1], lastDlssList, s); } }
        for (UINT i = 0; i < n; i++) for (auto& e : evs[i]) if (e.kind == 0) g_state[e.res] = e.after;
        realEcl(q, n, lists);
        return true;
    }
    // States at the start of the replay range (before list s) and at the end of the batch.
    std::unordered_map<ID3D12Resource*, uint32_t> atStart;   // first StateBefore within [s..compEnd]
    for (int i = s; i <= compEnd; i++) for (auto& e : evs[i]) if (e.kind == 0 && !atStart.count(e.res)) atStart[e.res] = e.before;
    // Resource states just before list s (for the DLSS outputs, which may not be touched inside the range).
    for (int i = 0; i < s; i++) for (auto& e : evs[i]) if (e.kind == 0) g_state[e.res] = e.after;
    std::unordered_map<ID3D12Resource*, uint32_t> seenBefore = g_state;
    // history textures: content from the previous frame (not touched before s this frame), read and rewritten in the range
    std::vector<ID3D12Resource*> hist;
    if (g_history.load()) {
        std::unordered_map<ID3D12Resource*, bool> written;
        for (int i = s; i <= compEnd; i++) for (auto& e : evs[i])
            if (e.kind == 0 && (e.after & (D3D12_RESOURCE_STATE_RENDER_TARGET | D3D12_RESOURCE_STATE_UNORDERED_ACCESS | D3D12_RESOURCE_STATE_COPY_DEST))) written[e.res] = true;
        for (auto& kv : atStart) {
            ID3D12Resource* r = kv.first;
            if (r == comp || r == g_dlssOut[1] || seenBefore.count(r) || !written.count(r)) continue;
            if (kv.second == D3D12_RESOURCE_STATE_COMMON || (kv.second & (D3D12_RESOURCE_STATE_RENDER_TARGET | D3D12_RESOURCE_STATE_UNORDERED_ACCESS | D3D12_RESOURCE_STATE_COPY_DEST))) continue;   // first use = read
            D3D12_RESOURCE_DESC d = Desc(r);
            if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.Width * 8 < g_outW || (d.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) continue;
            hist.push_back(r);
        }
        if (g_histLogs > 0 && !hist.empty()) {
            g_histLogs--;
            std::string t; for (auto* r : hist) { D3D12_RESOURCE_DESC d = Desc(r); char b[80]; snprintf(b, sizeof(b), "%p %llux%u f%d st %X; ", (void*)r, (unsigned long long)d.Width, d.Height, (int)d.Format, atStart[r]); t += b; }
            LOGI("replay: per-eye history textures (%zu): %s", hist.size(), t.c_str());
        }
    }
    uint32_t dlssLState = g_state.count(g_dlssOut[0]) ? g_state[g_dlssOut[0]] : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    uint32_t dlssRStateAtS = g_state.count(g_dlssOut[1]) ? g_state[g_dlssOut[1]] : dlssLState;
    for (UINT i = s; i < n; i++) for (auto& e : evs[i]) if (e.kind == 0) g_state[e.res] = e.after;
    std::unordered_map<ID3D12Resource*, uint32_t> atEnd;
    for (auto& kv : atStart) atEnd[kv.first] = g_state.count(kv.first) ? g_state[kv.first] : kv.second;

    // HUD: the UI target = output-size RGBA8 render target whose last render-target use in [s, compStart) is latest.
    ID3D12Resource* ui = nullptr; uint32_t uiState = 0;
    // Only in steady state: switching it on during startup / loading (frames alternating with and without stereo) crashed
    // the game (tests 037, 043, 046); switched on in game it runs (044).
    if (g_hudLayer.load() && g_stereoStreak >= 200) {
        // UI target (test 045): output-size RGBA8 that enters RENDER_TARGET in [s, compStart) and is still a render target
        // when the composite list starts (the composite list itself makes it a shader resource). Intermediate UI targets
        // (menus / blur) end as shader resources and are skipped.
        int best = -1;
        std::unordered_map<ID3D12Resource*, uint32_t> last; std::unordered_map<ID3D12Resource*, int> enteredRt;
        for (int i = 0; i < compStart; i++)
            for (auto& e : evs[i]) {
                if (e.kind != 0 || !IsUiLike(e.res)) continue;
                last[e.res] = e.after;
                if (i >= s && e.after == D3D12_RESOURCE_STATE_RENDER_TARGET) enteredRt[e.res] = i;
            }
        for (auto& kv : enteredRt)
            if (last[kv.first] == D3D12_RESOURCE_STATE_RENDER_TARGET && kv.second > best) { best = kv.second; ui = kv.first; }
        if (ui) {
            uiState = D3D12_RESOURCE_STATE_RENDER_TARGET;
            if (!EnsureRes(q, nullptr) || !EnsureHud(ui)) ui = nullptr;
        }
        static int logs = 0; static ID3D12Resource* lastUi = (ID3D12Resource*)1;
        if (ui != lastUi && logs < 40) { logs++; LOGI("replay: HUD target %p (state %X before the composite, lists %d..%d, composite %d..%d)", ui, uiState, s, best, compStart, compEnd); }
        lastUi = ui;
    }
    g_prevInputs.clear(); g_prevDesc.clear();
    for (auto& kv : atStart)
        if (kv.first != comp && kv.first != g_dlssOut[1] && kv.second != D3D12_RESOURCE_STATE_COMMON && seenBefore.count(kv.first)) { g_prevInputs.push_back(kv.first); g_prevDesc[kv.first] = Desc(kv.first); }
    // 1. the game's batch, split before the range to snapshot everything the range touches
    if (!EnsureRes(q, comp)) { realEcl(q, n, lists); return true; }
    std::vector<std::pair<ID3D12Resource*, ID3D12Resource*>> snaps;
    if (g_snapshot.load()) {
        for (auto& kv : atStart) {
            if (kv.first == comp) continue;
            // mode 2 (default): only inputs - resources that already hold content produced earlier this frame
            // (first state in the range is not COMMON and the resource was transitioned before the range).
            if (g_snapshot.load() == 2 && (kv.second == D3D12_RESOURCE_STATE_COMMON || !seenBefore.count(kv.first))) continue;
            if (kv.first == g_dlssOut[1] && g_debug.load() != 1) continue;   // overwritten with the left image anyway
            ID3D12Resource* c = SnapFor(kv.first);
            if (c) snaps.push_back({kv.first, c});
        }
    }
    if (s > 0) realEcl(q, (UINT)s, lists);
    const int dumpReq = g_dlssDumpReq.exchange(0);
    bool dumpNow = dumpReq != 0;
    const bool dumpLite = dumpReq == 2;   // dumplite.trigger: only the left inputs + both composites (big sizes: less readback memory)
    if (dumpNow && !dumpLite) {   // right DLSS output as the game's post work is about to read it (later its memory is reused)
        uint32_t st = dlssRStateAtS;
        RunOwnList(q, [&](ID3D12GraphicsCommandList* l) { RecordReadback(l, g_dlssOut[1], st, "R"); RecordReadback(l, R.leftDlss, D3D12_RESOURCE_STATE_COMMON, "Lsaved"); });
        for (auto& rb : g_rb) if (!rb.fence) rb.fence = R.fenceVal;
    }
    if (g_inputsDump.load()) {
        g_inputsDump = 0;
        auto descs = [&](ID3D12Resource* r) { D3D12_RESOURCE_DESC d = Desc(r); char b[96];
            snprintf(b, sizeof(b), "%s %llux%u f%d", d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ? "buf" : "tex", (unsigned long long)d.Width, d.Height, (int)d.Format); return std::string(b); };
        LOGI("inputs: replay batch %d lists %d..%d, L-DLSS %p R-DLSS %p", k, s, compEnd, g_dlssOut[0], g_dlssOut[1]);
        for (auto& kv : atStart)
            LOGI("inputs:   %p %s first-state %X seen-before %d snap %d", kv.first, descs(kv.first).c_str(), kv.second, (int)seenBefore.count(kv.first),
                 (int)(std::find_if(snaps.begin(), snaps.end(), [&](auto& p) { return p.first == kv.first; }) != snaps.end()));
        // every resource transitioned this frame before the range: batch/list of its first and last event
        std::unordered_map<ID3D12Resource*, std::pair<int,int>> firstLast; std::vector<ID3D12Resource*> order;
        for (size_t i = 0; i < g_frameEvs.size(); i++) {
            auto& fe = g_frameEvs[i];
            if (fe.batch == k && fe.list >= s) break;
            auto it = firstLast.find(fe.e.res);
            if (it == firstLast.end()) { firstLast[fe.e.res] = {fe.batch * 1000 + fe.list, fe.batch * 1000 + fe.list}; order.push_back(fe.e.res); }
            else it->second.second = fe.batch * 1000 + fe.list;
        }
        for (auto* r : order) {
            D3D12_RESOURCE_DESC d = Desc(r);
            if (d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.Width < 64) continue;
            LOGI("span: %p %s first b%d.l%d last b%d.l%d%s", r, descs(r).c_str(), firstLast[r].first / 1000, firstLast[r].first % 1000,
                 firstLast[r].second / 1000, firstLast[r].second % 1000, atStart.count(r) ? "  <- replay input" : "");
        }
        LogFlush();
    }
    if (!snaps.empty()) {
        tm::Ensure(q);
        RunOwnList(q, [&](ID3D12GraphicsCommandList* l) {
            tm::Write(l, tm::Snap0);
            for (auto& p : snaps) {
                tm::snapBytes += tm::heap ? tm::Bytes(p.first) : 0; tm::snapCount++;
                Barrier(l, p.first, atStart[p.first], D3D12_RESOURCE_STATE_COPY_SOURCE);
                Barrier(l, p.second, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
                l->CopyResource(p.second, p.first);
                Barrier(l, p.second, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
                Barrier(l, p.first, D3D12_RESOURCE_STATE_COPY_SOURCE, atStart[p.first]);
            }
            tm::Write(l, tm::Snap1);
        });
    }
    if (g_hudProbe.exchange(0) && compStart > s && EnsureRes(q, nullptr)) {
        // diagnostic: read back every output-size RGBA8 texture used in [s, compStart) as it is right before the composite
        std::vector<std::pair<ID3D12Resource*, uint32_t>> cands;
        for (int i = s; i < compStart; i++)
            for (auto& e : evs[i]) if (e.kind == 0 && IsUiLike(e.res) && std::find_if(cands.begin(), cands.end(), [&](auto& c) { return c.first == e.res; }) == cands.end()) cands.push_back({e.res, 0});
        for (auto& c : cands) {
            std::string tr;
            for (int i = 0; i < compStart; i++) for (auto& e : evs[i]) if (e.kind == 0 && e.res == c.first) { c.second = e.after; char b[40]; snprintf(b, sizeof(b), "l%d %X>%X; ", i, e.before, e.after); tr += b; }
            LOGI("hudprobe: %p %s", c.first, tr.c_str());
        }
        realEcl(q, (UINT)(compStart - s), lists + s);
        int idx = 0;
        RunOwnList(q, [&](ID3D12GraphicsCommandList* l) {
            for (auto& c : cands) { if (idx >= 8) break; snprintf(g_probeTags[idx], sizeof(g_probeTags[idx]), "c%d", idx); RecordReadback(l, c.first, c.second, g_probeTags[idx]); idx++; }
        });
        for (auto& rb : g_rb) if (!rb.fence) rb.fence = R.fenceVal;
        realEcl(q, n - (UINT)compStart, lists + compStart);
        ui = nullptr;
    } else if (ui) {   // game pass (right eye): UI lists, grab + blank the UI image, then the composite and the rest
        if (compStart > s) realEcl(q, (UINT)(compStart - s), lists + s);
        if (!RunOwnList(q, [&](ID3D12GraphicsCommandList* l) { HudGrabAndBlank(l, ui, uiState, true); })) ui = nullptr;
        realEcl(q, n - (UINT)compStart, lists + compStart);
    } else realEcl(q, n - (UINT)s, lists + s);
    uint32_t compState = atEnd.count(comp) ? atEnd[comp] : D3D12_RESOURCE_STATE_COMMON;
    uint32_t dlssREnd = g_state.count(g_dlssOut[1]) ? g_state[g_dlssOut[1]] : dlssRStateAtS;
    // 2-4. copy composite -> right texture, left DLSS output -> right DLSS output, restore states for the range

    bool ok = RunOwnList(q, [&](ID3D12GraphicsCommandList* l) {
        tm::Write(l, tm::Post1);
        Barrier(l, comp, compState, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Barrier(l, R.right, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        l->CopyResource(R.right, comp);
        Barrier(l, R.right, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
        Barrier(l, comp, D3D12_RESOURCE_STATE_COPY_SOURCE, compState);
        for (auto* r : hist) {   // right pass done: keep its history, give the left replay the left one
            uint32_t cur = atEnd[r];
            ID3D12Resource* cr = SnapIn(g_histR, r); ID3D12Resource* cl = SnapIn(g_histL, r);
            if (!cr || !cl) continue;
            Barrier(l, r, cur, D3D12_RESOURCE_STATE_COPY_SOURCE);
            Barrier(l, cr, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
            l->CopyResource(cr, r);
            Barrier(l, cr, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
            if (g_histLValid[r]) {
                Barrier(l, r, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
                Barrier(l, cl, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
                l->CopyResource(r, cl);
                Barrier(l, cl, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
                Barrier(l, r, D3D12_RESOURCE_STATE_COPY_DEST, cur);
            } else Barrier(l, r, D3D12_RESOURCE_STATE_COPY_SOURCE, cur);
        }
        std::unordered_map<ID3D12Resource*, ID3D12Resource*> snapOf;
        for (auto& p : snaps) snapOf[p.first] = p.second;
        // Order: plain state fixes + snapshot restores first, then the left view's inputs, then the left DLSS image -
        // so a snapshot of a texture that shares memory with one of them (transient aliasing) cannot overwrite left data.
        for (int pass = 0; pass < 2; pass++)
        for (auto& kv : atStart) {
            if (kv.first == g_dlssOut[1] && g_debug.load() != 1) continue;
            if ((g_leftTaken.count(kv.first) != 0) != (pass == 1)) continue;
            uint32_t cur = atEnd[kv.first];
            auto it = snapOf.find(kv.first);
            auto lt = g_leftTaken.find(kv.first);
            if (lt != g_leftTaken.end()) {   // left view's content of this input
                s_leftRest++;
                Barrier(l, kv.first, cur, D3D12_RESOURCE_STATE_COPY_DEST);
                Barrier(l, lt->second, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
                l->CopyResource(kv.first, lt->second);
                Barrier(l, lt->second, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
                Barrier(l, kv.first, D3D12_RESOURCE_STATE_COPY_DEST, kv.second);
            } else if (it != snapOf.end()) {
                if (kv.first == g_dlssOut[1]) cur = dlssRStateAtS;   // identity mode: already put back to its start state above
                Barrier(l, kv.first, cur, D3D12_RESOURCE_STATE_COPY_DEST);
                Barrier(l, it->second, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
                l->CopyResource(kv.first, it->second);
                Barrier(l, it->second, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
                Barrier(l, kv.first, D3D12_RESOURCE_STATE_COPY_DEST, kv.second);
            } else if (cur != kv.second) Barrier(l, kv.first, cur, kv.second);
            if (tm::heap && (lt != g_leftTaken.end() || it != snapOf.end())) { tm::restoreBytes += tm::Bytes(kv.first); tm::restoreCount++; }
        }
        if (g_debug.load() != 1) {
            Barrier(l, R.leftDlss, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
            Barrier(l, g_dlssOut[1], dlssREnd, D3D12_RESOURCE_STATE_COPY_DEST);
            { D3D12_TEXTURE_COPY_LOCATION dst{g_dlssOut[1], D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}}, src{R.leftDlss, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
              l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr); }
            Barrier(l, R.leftDlss, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
            Barrier(l, g_dlssOut[1], D3D12_RESOURCE_STATE_COPY_DEST, dlssRStateAtS);
        } else Barrier(l, g_dlssOut[1], dlssREnd, dlssRStateAtS);
        tm::Write(l, tm::Restore1);
    });
    if (!ok) { s_noRange++; return true; }
    if (dumpNow) {   // what the left replay starts from: DLSS output (left copy in it) and the restored inputs
        static char tags[8][8];
        int t = 0;
        RunOwnList(q, [&](ID3D12GraphicsCommandList* l) {
            if (!dumpLite) RecordReadback(l, g_dlssOut[1], dlssRStateAtS, "RwithL");
            for (auto& kv : g_leftTaken) { if (t >= 8) break; snprintf(tags[t], sizeof(tags[t]), "in%d", t); RecordReadback(l, kv.first, atStart.count(kv.first) ? atStart[kv.first] : D3D12_RESOURCE_STATE_COMMON, tags[t]); t++; }
            RecordReadback(l, R.right, D3D12_RESOURCE_STATE_COMMON, "compR");
        });
        for (auto& rb : g_rb) if (!rb.fence) rb.fence = R.fenceVal;
        // every output-size texture the range touches, before the left replay (b) and after it (a)
        g_rangeSet.clear();
        for (auto& kv : atStart) {
            D3D12_RESOURCE_DESC dd = Desc(kv.first);
            if (!dumpLite && dd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && dd.Width == g_outW && dd.Height == g_outH && g_rangeSet.size() < 10) g_rangeSet.push_back(kv.first);
        }
        static char tb[10][8];
        RunOwnList(q, [&](ID3D12GraphicsCommandList* l) {
            for (size_t t = 0; t < g_rangeSet.size(); t++) { snprintf(tb[t], sizeof(tb[t]), "b%zu", t); RecordReadback(l, g_rangeSet[t], atStart[g_rangeSet[t]], tb[t]); }
        });
        { std::string ls; for (size_t t = 0; t < g_rangeSet.size(); t++) { char b[64]; D3D12_RESOURCE_DESC dd = Desc(g_rangeSet[t]); snprintf(b, sizeof(b), "%zu=%p f%d st %X>%X; ", t, (void*)g_rangeSet[t], (int)dd.Format, atStart[g_rangeSet[t]], atEnd[g_rangeSet[t]]); ls += b; }
          LOGI("replay: range textures: %s", ls.c_str()); }
        for (auto& rb : g_rb) if (!rb.fence) rb.fence = R.fenceVal;
    }
    // 5. replay [s .. compEnd]
    if (ui) {   // left eye: same split, blank only
        if (compStart > s) g_realEcl(q, (UINT)(compStart - s), lists + s);
        RunOwnList(q, [&](ID3D12GraphicsCommandList* l) { HudGrabAndBlank(l, ui, uiState, false); });
        g_realEcl(q, (UINT)(compEnd - compStart + 1), lists + compStart);
    } else g_realEcl(q, (UINT)(compEnd - s + 1), lists + s);
    // after the replay every resource of the range is back in its end state (same lists); DLSS-R holds left data.
    if (!hist.empty()) {
        bool okH = RunOwnList(q, [&](ID3D12GraphicsCommandList* l) {
            for (auto* r : hist) {
                auto ir = g_histR.find(r); auto il = g_histL.find(r);
                if (ir == g_histR.end() || il == g_histL.end() || !ir->second.clone || !il->second.clone) continue;
                uint32_t cur = atEnd[r];
                Barrier(l, r, cur, D3D12_RESOURCE_STATE_COPY_SOURCE);
                Barrier(l, il->second.clone, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
                l->CopyResource(il->second.clone, r);
                Barrier(l, il->second.clone, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
                Barrier(l, r, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
                Barrier(l, ir->second.clone, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
                l->CopyResource(r, ir->second.clone);
                Barrier(l, ir->second.clone, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
                Barrier(l, r, D3D12_RESOURCE_STATE_COPY_DEST, cur);
            }
        });
        for (auto* r : hist) g_histLValid[r] = okH;
    }
    if (tm::heap && tm::on) RunOwnList(q, [&](ID3D12GraphicsCommandList* l) { tm::Write(l, tm::Replay1); });
    if (dumpNow) {
        static char ta[10][8];
        RunOwnList(q, [&](ID3D12GraphicsCommandList* l) {
            RecordReadback(l, comp, compState, "compL");
            for (size_t t = 0; t < g_rangeSet.size(); t++) { snprintf(ta[t], sizeof(ta[t]), "a%zu", t); RecordReadback(l, g_rangeSet[t], atEnd[g_rangeSet[t]], ta[t]); }
        });
        for (auto& rb : g_rb) if (!rb.fence) rb.fence = R.fenceVal;
    }
    g_didReplay = true;
    s_replays++;
    // 6. hand both eyes to the XR side: left = composite target (state compState), right = our copy (COMMON)
    XrHostEye eyes[2];
    if (compState != D3D12_RESOURCE_STATE_COMMON) {
        static bool warned = false;
        if (!warned) { warned = true; LOGW("replay: composite target ends in state %X, not COMMON - stereo not published", compState); }
    } else if (g_frameCamValid ? (FbViewNoteStamp(g_frameStamp), FbViewStereoPosesForCam(g_frameCam, eyes))
                               : g_pairStamp.load() ? FbViewStereoPosesForSerial(g_frameStamp, eyes) : FbViewStereoPoses(eyes)) {
        const bool sw = g_swapEyes.load();
        if (XrHostPublishStereo(q, sw ? R.right : comp, sw ? comp : R.right, D3D12_RESOURCE_STATE_COMMON, eyes, ui && g_hudLayer.load() != 3 ? R.hud : nullptr, D3D12_RESOURCE_STATE_COMMON)) { s_published++; if (ui) s_hud++; }
    }
    if (tm::heap && tm::on) { if (RunOwnList(q, [&](ID3D12GraphicsCommandList* l) { tm::End(l); })) tm::Committed(R.fenceVal); else tm::on = false; }
    static bool first = true;
    if (first) {
        first = false;
        LOGI("replay: first stereo replay - batch ecl#%d (%u lists), replayed lists %d..%d, composite %p, DLSS outputs L %p R %p, %zu resources restored",
             k, n, s, compEnd, comp, g_dlssOut[0], g_dlssOut[1], atStart.size());
    }
    return true;
}

void ReplayOnPresent(ID3D12Resource* const* all, int count) {
    std::lock_guard<std::recursive_mutex> execLock(g_execMu);
    for (int i = 0; i < 4; i++) g_bb[i] = i < count ? all[i] : nullptr;
    // Resource descriptions are cached by address; freed resources' addresses are reused by new ones (always, and en
    // masse after a resize), so the cache only lives for one frame (user 2026-10-30: after the auto resize
    // 3840x2160 -> 6086x3424 stale sizes broke the eye copies -> both eyes the same image).
    { std::lock_guard<std::mutex> lk(g_descMu); g_desc.clear(); }
    if (count > 0 && all[0]) {
        D3D12_RESOURCE_DESC d = Desc(all[0]);
        if (g_outW != (UINT)d.Width || g_outH != d.Height) {
            // output size changed: every per-resource cache refers to the old frame graph's resources
            g_idLogs = 4;
            LOGI("replay: output size %ux%u -> %llux%u: replay caches reset", g_outW, g_outH, (unsigned long long)d.Width, d.Height);
            for (auto* m : {&g_snaps, &g_leftSnaps, &g_histL, &g_histR, &g_scH0, &g_scH1})
                for (auto& kv : *m) if (kv.second.clone) g_grave.push_back({g_frameNo, kv.second.clone});
            g_snaps.clear(); g_leftSnaps.clear(); g_histL.clear(); g_histR.clear(); g_histLValid.clear(); g_scH0.clear(); g_scH1.clear(); g_scH1Valid.clear(); g_histSet.clear(); g_scSwapped.clear(); g_swapAt.clear(); g_ph0.clear(); g_ph1.clear(); g_phase = 0; g_leftTaken.clear(); g_prevInputs.clear(); g_prevDesc.clear(); g_state.clear();
            { std::lock_guard<std::mutex> lk(g_compMu); g_compSet.clear(); }
            g_compositeSrc = nullptr;
            g_stereoStreak = 0;
            SlHookReset();
        }
        g_outW = (UINT)d.Width; g_outH = d.Height; g_bbFormat = d.Format;
    }
    if (R.fence && !g_rb.empty()) FinishReadbacks();
    tm::Collect();
    if (tm::on && !g_didReplay) tm::on = false;   // frame without a hand-off: drop its partial marks
    { int r = g_rtTrace.load(); if (r > 0) g_rtTrace = r - 1; }
    int a = g_traceArmed.load();
    if (a > 0) { LOGI("trace f%d: present after %d ExecuteCommandLists calls", g_frame, g_eclInFrame); g_traceArmed = a - 1; }
    static DWORD tLast = GetTickCount();
    DWORD now = GetTickCount();
    if (now - tLast >= 10000 && g_mode.load() == 1) {
        LOGI("replay: %llu stereo replays, %llu published (%llu with HUD layer), %llu frames without a replay range, %llu own lists busy, left inputs taken %llu / restored %llu (10 s), left list %d",
             (unsigned long long)s_replays.load(), (unsigned long long)s_published.load(), (unsigned long long)s_hud.load(), (unsigned long long)s_noRange.load(), (unsigned long long)s_ownBusy.load(),
             (unsigned long long)s_leftIn.load(), (unsigned long long)s_leftRest.load(), 0);
        LOGI("replay: evaluates whose recorded eye differs from the GPU order: %llu (10 s)", (unsigned long long)s_eyeOrderMismatch.exchange(0));
        if (g_sceneHist.load()) LOGI("replay: scene history: %zu textures, %llu swaps in / %llu restores (10 s)", g_histSet.size(),
                                     (unsigned long long)s_scSwaps.exchange(0), (unsigned long long)s_scRestores.exchange(0));
        s_leftIn = 0; s_leftRest = 0; g_mapLogs = 1; g_idLogs = 1;
        s_hud = 0;
        tm::Report();
        s_ownBusy = 0;
        s_replays = 0; s_published = 0; s_noRange = 0; tLast = now;
        SlHookReport();
    }
    g_stereoStreak = g_didReplay ? g_stereoStreak + 1 : 0;
    if (g_mode.load() == 1 && g_stereoFrame.load() && !g_didReplay) {
        static DWORD tl = 0; DWORD now = GetTickCount();
        if (now - tl > 3000) {
            tl = now;
            std::string line; int cnt = 0;
            for (auto& fe : g_frameEvs) {
                if (!((fe.e.after == D3D12_RESOURCE_STATE_UNORDERED_ACCESS || fe.e.before == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) && IsOutputRgba16f(fe.e.res))) continue;
                if (++cnt > 16) { line += "..."; break; }
                char b[96]; snprintf(b, sizeof(b), "b%d.l%d %p %X>%X; ", fe.batch, fe.list, fe.e.res, fe.e.before, fe.e.after); line += b;
            }
            LOGI("replay: frame %d without replay (%d batches, left saved %d): output-size RGBA16F UAV events: %s", g_frame, g_eclInFrame, (int)g_leftSaved, line.empty() ? "none" : line.c_str());
        }
    }
    g_frame++; g_eclInFrame = 0; g_didReplay = false; g_leftSaved = false; g_rightEntered = false; g_frameCamValid = false; g_frameStamp = 0;
    LearnSceneHistory();
    g_evalOrder = 0; g_upOrder = 0;
    g_frameNo = g_frame;
    KeepAliveTrim();
    while (!g_grave.empty() && g_frameNo - g_grave.front().first > 10) { g_grave.front().second->Release(); g_grave.erase(g_grave.begin()); }
    g_dlssOut[0] = g_dlssOut[1] = nullptr;
    g_frameEvs.clear();
    g_leftTaken.clear();
    g_state.clear();
    g_track = g_mode.load() == 1 || g_traceArmed.load() > 0;
}

void ReplaySetStereoFrame(bool on) { g_stereoFrame = on; }
bool ReplayDidStereo() { return g_didReplay; }
void ReplaySetSnapshot(int on) { g_snapshot = on; }
void ReplaySetLeftInputs(int on) { g_leftInputs = on; }
void ReplaySetSceneHistory(int on) { if (g_sceneHist.exchange(on) != on) LOGI("replay: per-view scene history %d", on); }
void ReplaySetHistory(int on) { if (g_history.exchange(on) != on) LOGI("replay: per-eye history %d", on); }
void ReplaySetDebug(int debug, int startShift) { if (debug == 1) { LOGW("replay: identity debug mode disabled (hung the GPU in test 016)"); debug = 0; } g_debug = debug; g_startShift = startShift; LOGI("replay: debug %d, start shift %d", debug, startShift); }
// DLSS outputs we may copy later in the frame: hold a reference (taken while the engine's tag names it, so it is alive)
// for ~10 frames - a texture freed at a scene change and still named by our bookkeeping crashed the driver (tests 065-067).
static std::mutex g_keepMu;
static std::vector<std::pair<int, ID3D12Resource*>> g_keep;
static bool SlRecent() { return g_frame - g_lastSlFrame.load() < 30; }
static bool UpRecent() { return g_frame - g_lastUpFrame.load() < 30; }
static void KeepAlive(ID3D12Resource* r) {
    if (!r) return;
    std::lock_guard<std::mutex> lk(g_keepMu);
    for (auto& k : g_keep) if (k.second == r) { k.first = g_frameNo; return; }
    r->AddRef(); g_keep.push_back({g_frameNo, r});
}
static void KeepAliveTrim() {
    std::lock_guard<std::mutex> lk(g_keepMu);
    for (size_t i = 0; i < g_keep.size();)
        if (g_frameNo - g_keep[i].first > 10) { g_keep[i].second->Release(); g_keep[i] = g_keep.back(); g_keep.pop_back(); } else ++i;
}
void ReplayNoteDlss(void* cmdList, int eye, void* output) {
    if (!cmdList || !g_track.load()) return;
    std::lock_guard<std::mutex> lk(g_mu);
    auto& rec = g_lists[(ID3D12GraphicsCommandList*)cmdList];
    rec.dlssEye = eye;
    rec.dlssEv = (uint32_t)rec.ev.size();   // barriers Streamline records for the evaluate come after this
    // prefer the output tagged on this same command list (exact); the per-eye last tag is only a fallback
    rec.dlssOut = rec.tagOut ? rec.tagOut : (ID3D12Resource*)output;
    KeepAlive(rec.dlssOut);   // fallback pointer: slhook holds a reference to it, so it is alive here
    static int fb = 0;
    if (!rec.tagOut && output && fb++ < 10) LOGI("replay: DLSS eye %d on list %p: output not tagged on this list, using the eye's last tag %p", eye, cmdList, output);
    g_lastSlFrame = g_frame;
    if (!g_slSeen.exchange(true)) LOGI("replay: DLSS passes identified from Streamline (eye %d list %p output %p)", eye, cmdList, output);
}
uint64_t ReplayFrameCount() { return (uint64_t)g_frame; }
bool ReplayLastFrameStereo() { return g_stereoStreak > 0; }
bool ReplayHudActive() { return g_hudLayer.load() && g_stereoStreak >= 200; }
void ReplayHudProbe() { g_hudProbe = 1; LOGI("hudprobe: armed"); }
void ReplayNoteDlssDepth(void* cmdList, void* depth) {
    if (!cmdList || !g_track.load()) return;
    std::lock_guard<std::mutex> lk(g_mu);
    g_lists[(ID3D12GraphicsCommandList*)cmdList].depth = depth;
}
void ReplayNoteDlssCam(void* cmdList, const float pos[6]) {
    if (!cmdList || !g_track.load()) return;
    std::lock_guard<std::mutex> lk(g_mu);
    auto& rec = g_lists[(ID3D12GraphicsCommandList*)cmdList];
    rec.hasCam = true; for (int c = 0; c < 6; c++) rec.cam[c] = pos[c];
}
void ReplayNoteDlssOutput(void* cmdList, void* output) {
    if (!cmdList || !output || !g_track.load()) return;
    std::lock_guard<std::mutex> lk(g_mu);
    g_lists[(ID3D12GraphicsCommandList*)cmdList].tagOut = (ID3D12Resource*)output;
    KeepAlive((ID3D12Resource*)output);
}
void ReplaySetSwapEyes(bool on) { if (g_swapEyes.exchange(on) != on) LOGI("replay: swap eyes %d", (int)on); }
void ReplaySetHudLayer(int on) { g_hudLayer = on; LOGI("replay: HUD layer %d", on); }
void ReplaySetMode(int mode) { g_mode = mode; g_track = mode == 1 || g_traceArmed.load() > 0; LOGI("replay: mode %d", mode); }
void ReplayDlssDump() { g_dlssDumpReq = 1; }
void ReplayDlssDumpLite() { g_dlssDumpReq = 2; }
void ReplaySetVrs(bool on) { g_vrsOn = on; }
void ReplaySetPairStamp(bool on) { if (g_pairStamp.exchange(on) != on) LOGI("replay: pairing without Streamline by command-list stamp %d", (int)on); }
void ReplayInputsDump() { g_inputsDump = 1; LOGI("inputs: dump armed"); }
void ReplayHudTraceArm() { LOGI("hudtrace: armed"); g_rtTrace = 4; }
void ReplayTraceArm(int frames) { LOGI("trace: %d frames", frames); g_traceArmed = frames; g_track = true; }

bool ReplayInstall(ID3D12Device* dev) {
    ID3D12CommandAllocator* a = nullptr; ID3D12GraphicsCommandList* l = nullptr;
    if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void**)&a))) return false;
    if (FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, a, nullptr, __uuidof(ID3D12GraphicsCommandList), (void**)&l))) { a->Release(); return false; }
    void** vt = *(void***)l;
    bool ok = true;
    auto hook = [&](int idx, void* det, void** orig, const char* name) {
        if (MH_CreateHook(vt[idx], det, orig) != MH_OK || MH_EnableHook(vt[idx]) != MH_OK) { LOGE("replay: hook %s failed", name); ok = false; }
    };
    hook(VT12_CL_Reset, (void*)Hk_Reset, (void**)&o_Reset, "Reset");
    hook(VT12_CL_ResourceBarrier, (void*)Hk_Barrier, (void**)&o_Barrier, "ResourceBarrier");
    hook(VT12_CL_CopyResource, (void*)Hk_CopyRes, (void**)&o_CopyRes, "CopyResource");
    hook(VT12_CL_CopyTextureRegion, (void*)Hk_CopyTex, (void**)&o_CopyTex, "CopyTextureRegion");
    hook(VT12_CL_OMSetRenderTargets, (void*)Hk_OMSet, (void**)&o_OMSet, "OMSetRenderTargets");
    hook(VT12_CL_DrawInstanced, (void*)Hk_Draw, (void**)&o_Draw, "DrawInstanced");
    hook(VT12_CL_Dispatch, (void*)Hk_Dispatch, (void**)&o_Dispatch, "Dispatch");
    hook(VT12_CL_DrawIndexedInstanced, (void*)Hk_DrawIdx, (void**)&o_DrawIdx, "DrawIndexedInstanced");
    hook(VT12_CL_ClearRenderTargetView, (void*)Hk_ClearRtv, (void**)&o_ClearRtv, "ClearRenderTargetView");
    {
        void** dvt = *(void***)dev;
        if (MH_CreateHook(dvt[VT12_DEV_CreateRenderTargetView], (void*)Hk_CreateRtv, (void**)&o_CreateRtv) != MH_OK ||
            MH_EnableHook(dvt[VT12_DEV_CreateRenderTargetView]) != MH_OK) { LOGE("replay: hook CreateRenderTargetView failed"); ok = false; }
        g_rtvInc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    }
    l->Close(); l->Release(); a->Release();
    LOGI("replay: command list hooks %s", ok ? "on" : "partly failed");
    return ok;
}

} // namespace davg
