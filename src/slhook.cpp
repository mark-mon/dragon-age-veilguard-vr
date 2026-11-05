// Streamline (NVIDIA SL 2.x) hooks: one DLSS context (viewport) per eye.
//
// The game evaluates DLSS once per engine view, always on viewport 0, so with two views (stereo) both eyes share one
// DLSS history: each eye is blended with the other eye's image (ghosts). Here the right eye's calls are moved to
// viewport 1 (tags, constants, evaluate), and the game's DLSS options are applied to viewport 1 as well.
// Which eye a call belongs to: the DLSS output texture (ScalingOutputColor tag); replay.cpp reports each frame which
// output texture belongs to the left / right view. Constants / evaluate follow the last tag call on the same thread.
#include <windows.h>
#include <d3d12.h>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <cstring>
#include "slhook.h"
#include "fovdlss.h"
#include "vrs.h"
#include "replay.h"
#include "log.h"
#include "../third_party/minhook/include/MinHook.h"

namespace davg {

// ---- minimal SL 2.x declarations (layouts from sl_struct.h / sl_core_types.h)
struct SlGuid { uint32_t a; uint16_t b, c; uint8_t d[8]; };
struct SlBase { SlBase* next; SlGuid type; size_t version; };            // 32 bytes
struct SlViewport { SlBase base; uint32_t value; };
struct SlResource { SlBase base; uint32_t type; void* native; void* memory; void* view; uint32_t state; };
struct SlResourceTag { SlBase base; SlResource* resource; uint32_t bufferType; uint32_t lifecycle; const void* extent; };
enum { kBufferTypeScalingOutputColor = 4 };
enum { kFeatureDLSS = 0 };

typedef int(__cdecl* PFN_slSetTag)(const SlViewport& vp, const SlResourceTag* tags, uint32_t n, void* cmd);
typedef int(__cdecl* PFN_slSetConstants)(const void* consts, const void* frame, const SlViewport& vp);
typedef int(__cdecl* PFN_slEvaluateFeature)(uint32_t feature, const void* frame, const SlBase** inputs, uint32_t n, void* cmd);
typedef int(__cdecl* PFN_slGetFeatureFunction)(uint32_t feature, const char* name, void*& fn);
typedef int(__cdecl* PFN_slDLSSSetOptions)(const SlViewport& vp, const void* options);
static PFN_slSetTag o_SetTag; static PFN_slSetConstants o_SetConstants; static PFN_slEvaluateFeature o_Evaluate;
static PFN_slGetFeatureFunction o_GetFeatureFunction; static PFN_slDLSSSetOptions o_DLSSSetOptions;

static std::atomic<int> g_enabled{0};
static std::mutex g_mu;
static std::unordered_map<void*, int> g_outEye;          // DLSS output texture -> eye (0 left, 1 right)
static thread_local int t_eye = 0;
static thread_local int t_nextEye = 0;     // eye of the next tag group on this thread
static SlGuid g_vpType{}; static bool g_vpTypeKnown = false;
static std::vector<uint8_t> g_lastOptions;               // last DLSS options the game set (copied for viewport 1)
static std::atomic<bool> g_vp1Options{false};
static std::atomic<int> g_logLeft{40};
static std::atomic<int> g_constLog{0};
static std::atomic<uint64_t> s_tokPairs{0}, s_tokOdd{0}, s_evOdd{0}, s_evPairs{0};   // frame tokens with exactly 2 SetConstants / evaluates vs other counts
static std::atomic<int> g_seqLog{0};
static std::atomic<int> g_errLog{20};
static std::atomic<uint64_t> s_remapped{0}, s_constRemapped{0}, s_evalRemapped{0};
static uintptr_t g_tokKey = 0; static int g_tokCount = 0;
static struct TokCam { uintptr_t tok; uint64_t seq; float pos[6]; } g_tokCam[16];   // frame tokens are a small reused ring: take the NEWEST entry
static uint64_t g_tokCamSeq = 0;   // pos xyz + forward xyz   // frame token -> left view camera position (SetConstants)
static int g_tokCamNext = 0;
static uintptr_t g_evTokKey = 0; static int g_evTokCount = 0;   // DLSS evaluates per frame token: 1st = left, 2nd = right
static std::atomic<int> g_nextTagEye{0};
// Eye of a tag group, decided ONCE at its first tag (the depth tag) and kept for the whole group + its evaluate on this
// thread. The views are tagged / evaluated on different worker threads, concurrently on a many-core CPU: deciding each
// SetTag call from a global "next eye" split groups between the viewports and gave the right eye's DLSS mixed inputs
// (aliased / noisy right eye on the user's PC, not on the dev PC - 2026-11-03). The group's depth texture identifies the
// view (learned from GPU execution order by replay.cpp: the first DLSS evaluate executed in a frame is the left view).
static thread_local int t_grpEye = -1;
static thread_local void* t_grpDepth = nullptr;
static thread_local bool t_grpSplitCounted = false;
static std::atomic<uint64_t> s_grpSplit{0};
// Tag routing (2026-11-04): "order" = every tag goes to the eye after the last evaluate (stage 14). "per_type" = each
// buffer type alternates left / right on its own (k-th depth tag -> eye k&1), so a view whose tags arrive before the
// other view's evaluate (worker threads overlapping) still lands in its own viewport. Per-type eye counts are logged in
// both modes: in "order" mode a type that is not ~50/50 is being routed to the wrong eye.
static std::atomic<int> g_tagRouting{0};
static constexpr int kTypes = 64;
static std::atomic<uint32_t> g_typeCnt[kTypes];
static std::atomic<uint32_t> s_typeEye[kTypes][2];
static std::atomic<uint64_t> s_typeResync{0};
static std::atomic<int> g_rightJitter{0};
static std::atomic<uint64_t> s_jitOverride{0};
static std::mutex g_depthMu;
static std::unordered_map<void*, int> g_depthEye;   // depth texture -> eye
static std::atomic<uint64_t> s_grpByDepth{0}, s_grpByOrder{0}, s_evalFlip{0}, s_depthRelearn{0};
void SlHookLearnDepthEye(void* depth, int eye) {
    if (!depth || eye < 0) return;
    std::lock_guard<std::mutex> l(g_depthMu);
    auto it = g_depthEye.find(depth);
    if (it == g_depthEye.end()) { if (g_depthEye.size() > 32) g_depthEye.clear(); g_depthEye[depth] = eye; }
    else if (it->second != eye) { it->second = eye; s_depthRelearn++; }
}
static int DepthEye(void* depth) {
    std::lock_guard<std::mutex> l(g_depthMu);
    auto it = g_depthEye.find(depth); return it == g_depthEye.end() ? -1 : it->second;
}
static std::atomic<void*> g_lastOutEye[2] = {};               // last ScalingOutputColor tagged for each eye (routing decision)                        // eye of the next tag group (after the left evaluate: right)

static void Note(const SlViewport& vp) { if (!g_vpTypeKnown) { g_vpType = vp.base.type; g_vpTypeKnown = true; } }
static SlViewport Vp(const SlViewport& like, uint32_t value) { SlViewport v = like; v.base.next = nullptr; v.value = value; return v; }

static void LogDlssDll();
static int __cdecl Hk_SetTag(const SlViewport& vp, const SlResourceTag* tags, uint32_t n, void* cmd) {
    Note(vp);
    int eye = -1;
    if (g_seqLog.load() > 0) {
        g_seqLog--;
        for (uint32_t i = 0; i < n && tags; i++)
            LOGI("slseq: SetTag vp %u type %u res %p lifecycle %u thread %lu nextEye %d", vp.value, tags[i].bufferType,
                 tags[i].resource ? tags[i].resource->native : nullptr, tags[i].lifecycle, GetCurrentThreadId(), t_nextEye);
    }
    for (uint32_t i = 0; i < n && tags; i++)
        if (tags[i].bufferType == kBufferTypeScalingOutputColor && tags[i].resource) {
            std::lock_guard<std::mutex> l(g_mu);
            // (no lookup by texture: transient outputs are re-aliased between the views across frames, so a texture seen as
            // the right output earlier can be the left one now -> left tags routed to viewport 1, stale left output, test 070)
            if (g_logLeft.load() > 0) { g_logLeft--; LOGI("sl: slSetTag vp %u n %u output %p -> eye %d (thread %lu)", vp.value, n, tags[i].resource->native, eye, GetCurrentThreadId()); }
        }
    // Tag groups (inputs ..., output) then evaluate come per view on one thread, left then right: the group after a
    // left evaluate belongs to the right eye. The output tag confirms / corrects it.
    for (uint32_t i = 0; i < n && tags; i++)
        if (tags[i].bufferType == 0 && tags[i].resource) {   // depth = first tag of a group
            t_grpDepth = tags[i].resource->native;
            // stage 16 routed groups by depth texture: on the user's PC the views' depth textures swap every frame (map
            // relearned ~800x / 10 s) -> both evaluates got the same eye, no stereo. Now: routing as before (per call),
            // the group eye only measures how often a group would be SPLIT between the viewports (diagnostic).
            t_grpEye = g_nextTagEye.load(); s_grpByOrder++; t_grpSplitCounted = false;
        }
    t_eye = g_nextTagEye.load();
    if (t_grpEye >= 0 && t_eye != t_grpEye && !t_grpSplitCounted) { s_grpSplit++; t_grpSplitCounted = true; }   // this group's tags went to both viewports
    for (uint32_t i = 0; i < n && tags; i++)
        if (tags[i].bufferType == kBufferTypeScalingOutputColor && tags[i].resource) {
            if (!(g_tagRouting.load() == 1 && g_enabled.load() && vp.value == 0)) {   // hold a reference: the evaluate may name it after the engine dropped it (scene change)
                IUnknown* nu = (IUnknown*)tags[i].resource->native;
                nu->AddRef();
                IUnknown* old = (IUnknown*)g_lastOutEye[t_eye & 1].exchange(nu);
                if (old) old->Release();
            }
            ReplayNoteDlssOutput(cmd, tags[i].resource->native);   // the evaluate on this command list writes it
        }
    // per-tag eye
    const bool perType = g_tagRouting.load() == 1 && g_enabled.load() && vp.value == 0;
        std::vector<SlResourceTag> to[2];
    int eyes[64]; uint32_t m = n < 64 ? n : 64;
    for (uint32_t i = 0; i < m && tags; i++) {
        uint32_t bt = tags[i].bufferType % kTypes;
        int e = perType ? (int)(g_typeCnt[bt].fetch_add(1) & 1) : t_eye;
        if (!perType) g_typeCnt[bt].fetch_add(1);
        eyes[i] = e; s_typeEye[bt][e & 1]++;
    }
    if (perType && tags && n <= 64) {
        for (uint32_t i = 0; i < n; i++)
            if (tags[i].bufferType == kBufferTypeScalingOutputColor && tags[i].resource) {   // the per-eye output follows its tag's eye
                IUnknown* nu = (IUnknown*)tags[i].resource->native; nu->AddRef();
                IUnknown* old = (IUnknown*)g_lastOutEye[eyes[i] & 1].exchange(nu);
                if (old) old->Release();
                // undo the order-based store above if it went to the other eye: harmless, the evaluate reads its own eye's slot
            }
        bool all0 = true, all1 = true;
        for (uint32_t i = 0; i < n; i++) { if (eyes[i]) all0 = false; else all1 = false; }
        if (all0) return o_SetTag(vp, tags, n, cmd);
        SlViewport v1 = Vp(vp, 1);
        if (all1) { s_remapped++; return o_SetTag(v1, tags, n, cmd); }
        for (uint32_t i = 0; i < n; i++) to[eyes[i] & 1].push_back(tags[i]);
        int r0 = o_SetTag(vp, to[0].data(), (uint32_t)to[0].size(), cmd);
        s_remapped++;
        int r1 = o_SetTag(v1, to[1].data(), (uint32_t)to[1].size(), cmd);
        return r0 != 0 ? r0 : r1;
    }
    if (g_enabled.load() && vp.value == 0 && t_eye == 1) {
        SlViewport v1 = Vp(vp, 1); s_remapped++;
        int r = o_SetTag(v1, tags, n, cmd);
        if (r != 0 && g_errLog.load() > 0) { g_errLog--; LOGW("sl: slSetTag(vp 1) failed: %d", r); }
        return r;
    }
    return o_SetTag(vp, tags, n, cmd);
}
void SlHookSetRightJitter(int mode) {
    static const char* n[] = {"as the game reports", "zero", "negated", "previous frame's"};
    if (g_rightJitter.exchange(mode) != mode) LOGI("sl: right view DLSS jitter: %s", n[mode & 3]);
}
void SlHookSetTagRouting(int mode) {
    if (g_tagRouting.exchange(mode) != mode) LOGI("sl: DLSS tag routing %s", mode == 1 ? "per buffer type" : "call order");
}
static int __cdecl Hk_SetConstants(const void* c, const void* frame, const SlViewport& vp) {
    Note(vp);
    if (g_seqLog.load() > 0) { g_seqLog--; LOGI("slseq: SetConstants vp %u token %p thread %lu", vp.value, frame, GetCurrentThreadId()); }
    if (g_constLog.load() > 0) {
        g_constLog--;
        const float* f = (const float*)((const char*)c + 32);   // after BaseStructure
        uint32_t tok = (uint32_t)(uintptr_t)frame;   // token object address (one per frame in flight)
        // 5 float4x4 (80 floats) then jitter(2) mvecScale(2) pinhole(2) cameraPos(3) up(3) right(3) fwd(3) near far fov aspect
        LOGI("sl: slSetConstants vp %u token %08X thread %lu jitter (%.3f %.3f) pos (%.3f %.3f %.3f) right (%.3f %.3f %.3f) fwd (%.3f %.3f %.3f) near %.3f far %.1f fov %.3f aspect %.3f",
             vp.value, tok, GetCurrentThreadId(), f[80], f[81], f[86], f[87], f[88], f[92], f[93], f[94], f[95], f[96], f[97], f[98], f[99], f[100], f[101]);
        LOGI("sl:   cameraViewToClip [%.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f]",
             f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11], f[12], f[13], f[14], f[15]);
        LOGI("sl:   clipToPrevClip [%.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f] mvScale (%.4f %.4f) mvInvalid %.1f bools %02X %02X %02X %02X %02X %02X",
             f[48], f[49], f[50], f[51], f[52], f[53], f[54], f[55], f[56], f[57], f[58], f[59], f[60], f[61], f[62], f[63], f[82], f[83], f[102],
             ((const uint8_t*)(f + 103))[0], ((const uint8_t*)(f + 103))[1], ((const uint8_t*)(f + 103))[2], ((const uint8_t*)(f + 103))[3],
             ((const uint8_t*)(f + 103))[4], ((const uint8_t*)(f + 103))[5]);
    }
    // Both views' constants come on one thread, left first, with the same frame token: the 2nd call per token = right.
    int nth = 0;
    {
        std::lock_guard<std::mutex> l(g_mu);
        uintptr_t key = (uintptr_t)frame;
        if (key != g_tokKey) {
            if (g_tokKey) { if (g_tokCount == 2) s_tokPairs++; else s_tokOdd++; }
            g_tokKey = key; g_tokCount = 0;
        }
        nth = g_tokCount++;
        if (nth == 0) {   // the frame's left view: remember where its camera is (pairs the frame with the head pose used)
            const float* f = (const float*)((const char*)c + 32);
            TokCam& t = g_tokCam[g_tokCamNext]; g_tokCamNext = (g_tokCamNext + 1) % 16;
            t.tok = key; t.seq = ++g_tokCamSeq; t.pos[0] = f[86]; t.pos[1] = f[87]; t.pos[2] = f[88]; t.pos[3] = f[95]; t.pos[4] = f[96]; t.pos[5] = f[97];
        }
    }
    // Right view jitter test (2026-11-04: right eye aliased with DLSS only - FSR, fed by the engine's own per-view
    // jitter, is clean): the game reports the same jitter for both views; if view 1 is really rendered with another
    // (or no) offset, DLSS mis-registers every frame. stereo_dlss_right_jitter = game | zero | negate | previous.
    alignas(16) uint8_t cbuf[640];
    static float s_prevJit[2] = {0, 0}; static float s_lastJit[2] = {0, 0};
    if (nth == 0) { const float* f = (const float*)((const char*)c + 32); s_prevJit[0] = s_lastJit[0]; s_prevJit[1] = s_lastJit[1]; s_lastJit[0] = f[80]; s_lastJit[1] = f[81]; }
    const int jm = g_rightJitter.load();
    if (nth == 1 && jm != 0) {
        memcpy(cbuf, c, sizeof(cbuf));
        float* f = (float*)(cbuf + 32);
        if (jm == 1) { f[80] = 0; f[81] = 0; }
        else if (jm == 2) { f[80] = -f[80]; f[81] = -f[81]; }
        else if (jm == 3) { f[80] = s_prevJit[0]; f[81] = s_prevJit[1]; }
        c = cbuf; s_jitOverride++;
    }
    if (g_enabled.load() && vp.value == 0 && nth == 1) {
        SlViewport v1 = Vp(vp, 1); s_constRemapped++;
        int r = o_SetConstants(c, frame, v1);
        if (r != 0 && g_errLog.load() > 0) { g_errLog--; LOGW("sl: slSetConstants(vp 1) failed: %d", r); }
        return r;
    }
    return o_SetConstants(c, frame, vp);
}
static int __cdecl Hk_Evaluate(uint32_t feature, const void* frame, const SlBase** inputs, uint32_t n, void* cmd) {
    int eye = t_eye;
    if (feature == kFeatureDLSS) LogDlssDll();
    if (feature == kFeatureDLSS) {   // the views evaluate in order (left, right) with the same frame token, possibly on different threads
        std::lock_guard<std::mutex> l(g_mu);
        uintptr_t key = (uintptr_t)frame;
        if (key != g_evTokKey) {
            if (g_evTokKey) { if (g_evTokCount == 2) s_evPairs++; else s_evOdd++; }
            g_evTokKey = key; g_evTokCount = 0;
        }
        int tokEye = g_evTokCount++ == 1 ? 1 : 0;
        eye = tokEye;
        if (t_grpEye >= 0 && t_grpEye != tokEye) s_evalFlip++;   // diagnostic only: the evaluate follows the call order
        g_nextTagEye = (g_enabled.load() && eye == 0) ? 1 : 0;
        if (eye == 1 && g_tagRouting.load() == 1)   // after the right view: every type should have an even count (next tag = left)
            for (auto& c : g_typeCnt) { uint32_t v = c.load(); if (v & 1) { c.fetch_add(1); s_typeResync++; } }
        // the output this eye's tag group named (per eye, not per thread: views may be tagged and evaluated on
        // different threads, and after a resize a thread's last tag can name a destroyed texture -> black left eye, test 062)
        ReplayNoteDlss(cmd, eye, g_lastOutEye[eye & 1].load());
        ReplayNoteDlssDepth(cmd, t_grpDepth);
        if (eye == 0) {   // newest entry for this token (test 117: the first found was often an older use of the token -> wrong pose)
            const TokCam* best = nullptr;
            for (auto& t : g_tokCam) if (t.tok == key && (!best || t.seq > best->seq)) best = &t;
            if (best) ReplayNoteDlssCam(cmd, best->pos);
        }
    }
    if (feature == kFeatureDLSS) fovdlss::SetThreadEye(eye);   // the NGX evaluate inside runs on this thread
    if (g_seqLog.load() > 0) {
        g_seqLog--;
        LOGI("slseq: Evaluate feature %u n %u eye %d thread %lu", feature, n, eye, GetCurrentThreadId());
        for (uint32_t i = 0; i < n; i++) {
            const SlBase* b = inputs[i];
            if (!b) continue;
            LOGI("slseq:   input %u type %08X ver %llu%s", i, b->type.a, (unsigned long long)b->version,
                 g_vpTypeKnown && memcmp(&b->type, &g_vpType, sizeof(SlGuid)) == 0 ? " (viewport)" : "");
        }
    }
    if (feature == kFeatureDLSS) { t_nextEye = (g_enabled.load() && eye == 0) ? 1 : 0; t_grpEye = -1; }
    // tags passed directly as inputs (SL 2.x allows it): use the output texture to decide the eye
    for (uint32_t i = 0; i < n; i++) {
        const SlBase* b = inputs[i];
        if (!b || (g_vpTypeKnown && memcmp(&b->type, &g_vpType, sizeof(SlGuid)) == 0)) continue;
    }
    if (g_logLeft.load() > 0) { g_logLeft--; LOGI("sl: slEvaluateFeature feature %u n %u eye %d (thread %lu)", feature, n, eye, GetCurrentThreadId()); }
    if (!g_enabled.load() || feature != kFeatureDLSS || eye != 1 || !g_vpTypeKnown) return o_Evaluate(feature, frame, inputs, n, cmd);
    // Make sure viewport 1 has DLSS options before its first evaluate.
    if (!g_vp1Options.load() && o_DLSSSetOptions && !g_lastOptions.empty()) {
        SlViewport v1{}; v1.base.type = g_vpType; v1.base.version = 1; v1.value = 1;
        int r = o_DLSSSetOptions(v1, g_lastOptions.data());
        g_vp1Options = true;
        LOGI("sl: DLSS options applied to viewport 1 (result %d)", r);
    }
    std::vector<const SlBase*> in(inputs, inputs + n);
    SlViewport v1{};
    bool replaced = false;
    for (auto& p : in)
        if (p && memcmp(&p->type, &g_vpType, sizeof(SlGuid)) == 0) {
            const SlViewport* v = (const SlViewport*)p;
            if (v->value == 0) { v1 = *v; v1.base.next = nullptr; v1.value = 1; p = &v1.base; replaced = true; }
        }
    if (!replaced) return o_Evaluate(feature, frame, inputs, n, cmd);
    s_evalRemapped++;
    int r = o_Evaluate(feature, frame, in.data(), n, cmd);
    if (g_errLog.load() > 0 && (r != 0 || s_evalRemapped.load() < 3)) { g_errLog--; LOGW("sl: slEvaluateFeature(vp 1) -> %d", r); }
    return r;
}
// Which DLSS the game really runs (2026-11-04: the user's jaggy right eye was a 3.7 nvngx_dlss.dll left in the game
// folder by a swapper; 310.6 fixed it). Version from the loaded DLL's VS_FIXEDFILEINFO, mode / size from DLSSOptions.
static void LogDlssDll() {
    static std::atomic<bool> done{false};
    if (done.exchange(true)) return;
    HMODULE m = GetModuleHandleW(L"nvngx_dlss.dll");
    if (!m) { LOGI("sl: nvngx_dlss.dll not loaded yet"); done = false; return; }
    wchar_t path[MAX_PATH] = L"?"; GetModuleFileNameW(m, path, MAX_PATH);
    unsigned v[4] = {0, 0, 0, 0};
    if (HRSRC r = FindResourceW(m, MAKEINTRESOURCEW(1), MAKEINTRESOURCEW(16)))   // VS_VERSION_INFO, RT_VERSION
        if (HGLOBAL g = LoadResource(m, r)) {
            const uint8_t* b = (const uint8_t*)LockResource(g); DWORD n = SizeofResource(m, r);
            for (DWORD i = 0; b && i + 52 <= n; i += 4)
                if (*(const uint32_t*)(b + i) == 0xFEEF04BD) {   // VS_FIXEDFILEINFO signature
                    uint32_t ms = *(const uint32_t*)(b + i + 8), ls = *(const uint32_t*)(b + i + 12);
                    v[0] = ms >> 16; v[1] = ms & 0xFFFF; v[2] = ls >> 16; v[3] = ls & 0xFFFF; break;
                }
        }
    LOGI("sl: DLSS DLL %u.%u.%u.%u (%S)%s", v[0], v[1], v[2], v[3], path,
         v[0] && v[0] < 310 ? " - OLD (DLSS 3.x): the right eye looked jaggy with this, use 310.x" : "");
}
static int __cdecl Hk_DLSSSetOptions(const SlViewport& vp, const void* options) {
    {   // DLSSOptions after BaseStructure: mode, outputWidth, outputHeight
        static uint32_t lastMode = 0xFFFFFFFF, lastW = 0;
        const uint32_t* o = (const uint32_t*)((const char*)options + 32);
        if (o[0] != lastMode || o[1] != lastW) {
            static const char* names[] = {"off", "max performance", "balanced", "max quality", "ultra performance", "ultra quality", "DLAA"};
            LOGI("sl: DLSS mode %s (%u), output %ux%u", o[0] < 7 ? names[o[0]] : "?", o[0], o[1], o[2]);
            lastMode = o[0]; lastW = o[1];
        }
    }
    Note(vp);
    // DLSSOptions: BaseStructure + fields; copy a generous block (the struct is < 256 bytes in SL 2.x)
    { std::lock_guard<std::mutex> l(g_mu); g_lastOptions.assign((const uint8_t*)options, (const uint8_t*)options + 256); }
    int r = o_DLSSSetOptions(vp, options);
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1) < 3) LOGI("sl: slDLSSSetOptions vp %u -> %d (called every view and frame; logged 3x)", vp.value, r);
    if (vp.value == 0 && g_enabled.load()) {
        SlViewport v1 = Vp(vp, 1);
        int r1 = o_DLSSSetOptions(v1, options);
        if (r1 != 0 && g_errLog.load() > 0) { g_errLog--; LOGW("sl: slDLSSSetOptions(vp 1) failed: %d", r1); }
        if (!g_vp1Options.exchange(true)) LOGI("sl: slDLSSSetOptions copied to viewport 1 -> %d", r1);
    }
    return r;
}
static int __cdecl Hk_GetFeatureFunction(uint32_t feature, const char* name, void*& fn) {
    int r = o_GetFeatureFunction(feature, name, fn);
    LOGI("sl: slGetFeatureFunction(%u, %s) -> %d %p", feature, name ? name : "?", r, fn);
    if (r == 0 && fn && name && !strcmp(name, "slDLSSSetOptions") && !o_DLSSSetOptions) {
        o_DLSSSetOptions = (PFN_slDLSSSetOptions)fn;
        fn = (void*)Hk_DLSSSetOptions;
    }
    return r;
}

void SlHookSetOutputs(void* left, void* right) {
    std::lock_guard<std::mutex> l(g_mu);
    if (left && left == right) { g_outEye.erase(left); return; }   // both views write the same (aliased) output: ambiguous
    if (left) g_outEye[left] = 0;
    if (right) g_outEye[right] = 1;
    if (g_outEye.size() > 64) g_outEye.clear();
}
void SlHookReset() {
    std::lock_guard<std::mutex> l(g_mu);
    g_outEye.clear();
    for (auto& e : g_lastOutEye) if (IUnknown* old = (IUnknown*)e.exchange(nullptr)) old->Release();
    LOGI("sl: per-eye state reset (output size changed)");
}
void SlHookEnable(bool on) {
    if (g_enabled.exchange(on) != on) LOGI("sl: per-eye DLSS %s", on ? "on (right eye = viewport 1)" : "off");
}
uint64_t SlHookRemapped() { return s_remapped.load(); }
void SlHookReport() {
    fovdlss::Report();
    vrs::Report();
    LOGI("sl: right eye on viewport 1: %llu tags, %llu constants, %llu evaluates (since start)", (unsigned long long)s_remapped.load(),
         (unsigned long long)s_constRemapped.load(), (unsigned long long)s_evalRemapped.load());
    LOGI("sl: DLSS tag groups %llu, SPLIT between the eyes' viewports %llu, evaluated on the other eye than they were tagged for %llu | depth texture changed eye %llu (10 s)",
         (unsigned long long)s_grpByOrder.exchange(0), (unsigned long long)s_grpSplit.exchange(0), (unsigned long long)s_evalFlip.exchange(0),
         (unsigned long long)s_depthRelearn.exchange(0)); s_grpByDepth = 0;
    {
        char buf[512]; int k = snprintf(buf, sizeof buf, "sl: tags per buffer type left/right (10 s, routing %s):", g_tagRouting.load() == 1 ? "per type" : "order");
        for (int t = 0; t < kTypes && k < (int)sizeof buf - 24; t++) {
            uint32_t a = s_typeEye[t][0].exchange(0), b = s_typeEye[t][1].exchange(0);
            if (a || b) k += snprintf(buf + k, sizeof buf - k, " %d:%u/%u", t, a, b);
        }
        LOGI("%s | resyncs %llu | right jitter overridden %llu", buf, (unsigned long long)s_typeResync.exchange(0), (unsigned long long)s_jitOverride.exchange(0));
    }
    LOGI("sl: frame tokens with 2 constant sets %llu, other counts %llu | with 2 evaluates %llu, other %llu (10 s) - 'other' = eyes may get the wrong view's data",
         (unsigned long long)s_tokPairs.exchange(0), (unsigned long long)s_tokOdd.exchange(0), (unsigned long long)s_evPairs.exchange(0), (unsigned long long)s_evOdd.exchange(0));
}
void SlHookLogConstants(int n) { g_seqLog = n * 4; g_constLog = n * 2; }

bool SlHookInstall() {
    HMODULE m = GetModuleHandleW(L"sl.interposer.dll");
    if (!m) { LOGW("sl: sl.interposer.dll not loaded - per-eye DLSS unavailable"); return false; }
    struct H { const char* name; void* det; void** orig; } hs[] = {
        {"slSetTag", (void*)Hk_SetTag, (void**)&o_SetTag},
        {"slSetConstants", (void*)Hk_SetConstants, (void**)&o_SetConstants},
        {"slEvaluateFeature", (void*)Hk_Evaluate, (void**)&o_Evaluate},
        {"slGetFeatureFunction", (void*)Hk_GetFeatureFunction, (void**)&o_GetFeatureFunction},
    };
    bool ok = true;
    for (auto& h : hs) {
        void* t = (void*)GetProcAddress(m, h.name);
        if (!t || MH_CreateHook(t, h.det, h.orig) != MH_OK || MH_EnableHook(t) != MH_OK) { LOGE("sl: hook %s failed", h.name); ok = false; }
    }
    LOGI("sl: Streamline hooks %s", ok ? "on" : "partly failed");
    return ok;
}

} // namespace davg
