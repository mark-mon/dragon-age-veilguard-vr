// Frostbite 2021 (Dragon Age: The Veilguard) view hooks: probe + head tracking (+ stereo later).
//
//   exe+29ABFE0 buildViews(out, matrixTable, frameCtx)   out: +0 entries (0x2080 each), +8 ptr array, +0x10 count
//                runs once per game frame on a job thread; calls setupView for every view source.
//   exe+298C400 setupView(entry, idx, r8, camParams, prevXf, curXf)   (args 5/6 on the stack)
//   camParams: +0 xf, +0x48 lens block (+0x68 fovY rad used when +0xD2 != 0), +0x8C fovY (rad), +0xA8/+0xAC
//              principal point (0.5, 0.5), +0xB4 near, +0xB8 aspect, +0xC4 far, +0xD2 "explicit lens" flag
//   transforms (0x90): rows right, up, BACK, position (metres, +Y up); the camera looks along -row2.
//   RenderView (0x540): +0 transform (0x90), +0x100 fovY, +0x104 offset xy, +0x110 near, +0x114 far,
//              +0x118 aspect, +0x140 dirty, +0x2B0 fovX
#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include "fbview.h"
#include "xr_host.h"
#include "replay.h"
#include "fovdlss.h"
#include "vrs.h"
#include "slhook.h"
#include "globals.h"
#include "log.h"
#include "../third_party/minhook/include/MinHook.h"

namespace davg {
static float g_lastReqAspect = 0;   // aspect requested for the left eye (diagnostic)
// fov_from_viewport (default OFF): report the horizontal FOV as tanY * viewport aspect. This was WRONG for this engine:
// the projection really uses the aspect written to cam+0xB8 (user log 2026-10-30 19:58: view-projection x scale 0.905 =
// 1/tan(47.8 deg), the headset's per-eye width), so turning this on stretched both eyes ~1.22x. Kept as an option.
static std::atomic<bool> c_fovFromViewport{false};
static float ViewportAspect(const uint8_t* cam) {
    uint16_t w = *(const uint16_t*)(cam + 0xec), h = *(const uint16_t*)(cam + 0xee);
    return (w >= 16 && h >= 16) ? float(w) / float(h) : 0.0f;
}


static const uintptr_t RVA_BUILDVIEWS = 0x29ABFE0;
static const uintptr_t RVA_SETUPVIEW  = 0x298C400;

typedef void* (__fastcall* PFN_BuildViews)(void* out, void* table, void* r8);
typedef void  (__fastcall* PFN_SetupView)(void* entry, uint16_t idx, void* r8, void* cam, void* prevXf, void* curXf);
static PFN_BuildViews o_Build;
static PFN_SetupView  o_Setup;

// ---------------------------------------------------------------- config
enum class Mode { Flat = 0, Head = 1, Stereo = 2 };
static std::atomic<int> c_mode{(int)Mode::Flat};
static std::atomic<bool> c_level{true};
static float c_worldScale = 1.0f;        // game metres per real metre
static int   c_latency = 1;              // builds between setupView and the Present that shows it
static float c_fakeYawDeg = 0.0f;        // debug: oscillating head yaw (null HMD)
static bool  c_probeLog = false;

// ---------------------------------------------------------------- small math
struct V3 { float x, y, z; };
static V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
static V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
static V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
static V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
static float Len(V3 a) { return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z); }
static V3 Norm(V3 a) { float l = Len(a); return l > 1e-8f ? a * (1.0f / l) : V3{0, 0, 1}; }
static V3 QRot(const XrQuaternionf& q, V3 v) {
    V3 u{q.x, q.y, q.z}; float s = q.w;
    V3 t = Cross(u, v) * 2.0f;
    return v + t * s + Cross(u, t);
}
static XrQuaternionf QMul(const XrQuaternionf& a, const XrQuaternionf& b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
static XrQuaternionf QYaw(float rad) { return {0, std::sin(rad * 0.5f), 0, std::cos(rad * 0.5f)}; }

struct Xf { V3 r, u, b, p; };    // Frostbite rows (right, up, back, position)
static Xf ReadXf(const float* m) { m += 16; return {{m[0], m[1], m[2]}, {m[4], m[5], m[6]}, {m[8], m[9], m[10]}, {m[12], m[13], m[14]}}; }   // +0x40 matrix
// The 0x90-byte view transform holds two 4x4 matrices: +0x00 and +0x40 (the one the view / projection matrices are
// built from, RenderView +0x2C0..), then a float4. Both get the same transform.
static void WriteXf(float* m, const Xf& x) {
    for (int k = 0; k < 2; k++) {
        float* t = m + k * 16;
        t[0] = x.r.x; t[1] = x.r.y; t[2] = x.r.z; t[3] = 0;
        t[4] = x.u.x; t[5] = x.u.y; t[6] = x.u.z; t[7] = 0;
        t[8] = x.b.x; t[9] = x.b.y; t[10] = x.b.z; t[11] = 0;
        t[12] = x.p.x; t[13] = x.p.y; t[14] = x.p.z; t[15] = 1;
    }
}
// Yaw-only basis from the game camera (the headset supplies pitch and roll).
static Xf Level(const Xf& c) {
    V3 back{c.b.x, 0, c.b.z};
    if (std::fabs(back.x) + std::fabs(back.z) < 1e-4f) back = {-c.u.x * (c.b.y > 0 ? 1.f : -1.f), 0, -c.u.z * (c.b.y > 0 ? 1.f : -1.f)};
    back = Norm(back);
    V3 up{0, 1, 0};
    return {Norm(Cross(up, back)), up, back, c.p};
}
// Camera = base * head: rows of base rotated by the head pose (OpenXR axes = Frostbite camera axes:
// x right, y up, z back, right-handed), position offset in the base frame.
static Xf ApplyPose(const Xf& base, const XrPosef& pose, float scale) {
    const XrQuaternionf& q = pose.orientation;
    V3 hr = QRot(q, {1, 0, 0}), hu = QRot(q, {0, 1, 0}), hb = QRot(q, {0, 0, 1});
    auto toWorld = [&](V3 v) { return base.r * v.x + base.u * v.y + base.b * v.z; };
    V3 p{pose.position.x, pose.position.y, pose.position.z};
    return {toWorld(hr), toWorld(hu), toWorld(hb), base.p + toWorld(p) * scale};
}

// ---------------------------------------------------------------- per-frame pose ring
struct FramePose { uint64_t serial = 0; XrHostEye eyes[2]{}; bool valid = false; bool sbs = false; bool hasCam = false; float cam[3] = {}; float rot[9] = {}; };
static const int kRing = 64;   // several buildViews calls per rendered frame (shadow / reflection views)
static FramePose g_ring[kRing];
static std::mutex g_ringMu;
static std::atomic<uint64_t> g_buildSerial{0};      // incremented per buildViews call
static std::atomic<uint64_t> g_newestMain{0};       // serial of the newest main-camera build (ring entry)
static thread_local bool t_mainDone = false;          // first main-view setupView of this build handled
static thread_local uint64_t t_serial = 0;

struct RightEyeArgs { alignas(16) uint8_t cam[0x100]; alignas(16) float cur[36]; alignas(16) float prev[36];
                      uint16_t idx; void* r8; bool valid; XrFovf fov; XrPosef pose; };
static thread_local RightEyeArgs t_right;
static thread_local bool t_sbsOk = false;
static bool c_sbs = true;            // stereo: both eyes side by side in the game's output (split screen)
static Xf g_prevXfR{}; static uint64_t g_prevSerialR = 0; static bool g_prevValidR = false;
static std::atomic<uint64_t> g_stereoBuilds{0};
static int c_stereoRegister = 0;
static int c_stereoBuild = 1;
static int c_stereoIdx = 1;
static int c_stereoForceUpdate = 1;   // explicitly run RenderView::update (exe+2A2A8E0) on the appended view          // view index (split-screen slot) for the appended right eye; -1 = same as main        // 0 = copy entry 0, 1 = construct like the engine     // experiment: also call the view registry hooks for the appended view
static std::mutex g_prevMu;
static Xf g_prevXf{}; static uint64_t g_prevSerial = 0; static bool g_prevValid = false;
static std::atomic<uint64_t> g_builds{0}, g_setups{0}, g_applied{0}, g_noPose{0};
static std::atomic<int> g_dumpArmed{0};
static std::atomic<int> g_jitLog{0};   // jitter check: builds left to log (viewdump trigger)
static int g_dumpIndex = 0;
static uint32_t g_lastCount = 0xFFFFFFFF;
static XrTime g_t0 = 0;

static bool Readable(const void* p, size_t n) {
    MEMORY_BASIC_INFORMATION m{};
    if (!p || !VirtualQuery(p, &m, sizeof(m))) return false;
    if (m.State != MEM_COMMIT || (m.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    return (const char*)p + n <= (const char*)m.BaseAddress + m.RegionSize;
}
static void DumpBlob(const char* tag, const void* p, size_t n) {
    if (!Readable(p, n)) { LOGW("viewdump: %s %p not readable", tag, p); return; }
    char name[64]; snprintf(name, sizeof(name), "vd%02d_%s.bin", g_dumpIndex, tag);
    std::wstring path = G().outDir + std::wstring(name, name + strlen(name));
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (f) { fwrite(p, 1, n, f); fclose(f); }
    LOGI("viewdump: %s %p (%zu bytes) -> %s", tag, p, n, name);
}
static void LogXf(const char* tag, const float* m) {
    if (!Readable(m, 0x40)) return;
    LOGI("  %s r(%.3f %.3f %.3f) u(%.3f %.3f %.3f) b(%.3f %.3f %.3f) p(%.2f %.2f %.2f)", tag,
         m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10], m[12], m[13], m[14]);
}

// Head pose (relative to the recentre reference) and both eyes, for a frame built now.
static bool GetHead(XrHostEye eyes[2], XrPosef& head) {
    XrView v[2];
    if (!XrHostLocateEyes(v, 1.0f + (float)c_latency)) return false;
    for (int i = 0; i < 2; i++) { eyes[i].pose = v[i].pose; eyes[i].fov = v[i].fov; }
    head.orientation = v[0].pose.orientation;
    head.position = {(v[0].pose.position.x + v[1].pose.position.x) * 0.5f, (v[0].pose.position.y + v[1].pose.position.y) * 0.5f,
                     (v[0].pose.position.z + v[1].pose.position.z) * 0.5f};
    if (c_fakeYawDeg != 0.0f) {   // debug: oscillate the head yaw (0.25 Hz) on top of the tracked pose
        double t = GetTickCount64() / 1000.0;
        XrQuaternionf y = QYaw((float)(c_fakeYawDeg * 3.14159265 / 180.0 * std::sin(t * 2 * 3.14159265 * 0.25)));
        head.orientation = QMul(y, head.orientation);
        for (int i = 0; i < 2; i++) {
            eyes[i].pose.orientation = QMul(y, eyes[i].pose.orientation);
            V3 p{eyes[i].pose.position.x, eyes[i].pose.position.y, eyes[i].pose.position.z};
            p = QRot(y, p); eyes[i].pose.position = {p.x, p.y, p.z};
        }
    }
    return true;
}

static void __fastcall Hk_Setup(void* entry, uint16_t idx, void* r8, void* cam, void* prevXf, void* curXf) {
    uint64_t n = g_setups.fetch_add(1);
    bool dump = g_dumpArmed.load() == 2;
    if (dump || (c_probeLog && n < 4)) {
        const float* c = (const float*)cam;
        LOGI("setupView #%llu entry %p idx %u cam %p prev %p cur %p fov %.3f/%.3f aspect %.3f d2 %u", (unsigned long long)n, entry, idx, cam,
             prevXf, curXf, c[0x8c / 4], c[0x68 / 4], c[0xb8 / 4], ((uint8_t*)c)[0xd2]);
        LogXf("curXf", (const float*)curXf);
    }
    if (dump) {
        char t[32];
        snprintf(t, sizeof(t), "cam%u", idx); DumpBlob(t, cam, 0x100);
        snprintf(t, sizeof(t), "cur%u", idx); DumpBlob(t, curXf, 0x90);
    }

    Mode mode = (Mode)c_mode.load();
    // Only the first view of a build is the main camera (other views: render-to-texture, menu scenes).
    if (mode != Mode::Flat && !t_mainDone && idx == 0) {
        t_mainDone = true;
        XrHostEye eyes[2]; XrPosef head;
        if (GetHead(eyes, head)) {
            alignas(16) uint8_t camCopy[0x100]; memcpy(camCopy, cam, sizeof(camCopy));
            alignas(16) float xf[36]; memcpy(xf, curXf, sizeof(xf));
            alignas(16) float pxf[36]; memcpy(pxf, prevXf, sizeof(pxf));
            Xf game = ReadXf(xf);
            Xf base = c_level ? Level(game) : game;
            bool stereo = mode == Mode::Stereo;
            Xf now = ApplyPose(base, stereo ? eyes[0].pose : head, c_worldScale);
            WriteXf(xf, now);
            if (stereo) {
                // Right eye: same camera parameters, right eye pose; appended as a second view after the build.
                memcpy(t_right.cam, cam, sizeof(t_right.cam));
                Xf pr = ApplyPose(base, eyes[1].pose, c_worldScale);
                memcpy(t_right.cur, curXf, sizeof(t_right.cur)); WriteXf(t_right.cur, pr);
                memcpy(t_right.prev, curXf, sizeof(t_right.prev));
                {
                    std::lock_guard<std::mutex> l(g_prevMu);
                    if (g_prevValidR && t_serial == g_prevSerialR + 1) WriteXf(t_right.prev, g_prevXfR); else WriteXf(t_right.prev, pr);
                    g_prevXfR = pr; g_prevSerialR = t_serial; g_prevValidR = true;
                }
                float tyR = (std::max)(std::tan(eyes[1].fov.angleUp), std::tan(-eyes[1].fov.angleDown));
                float txR = (std::max)(std::tan(-eyes[1].fov.angleLeft), std::tan(eyes[1].fov.angleRight));
                *(float*)(t_right.cam + 0x8c) = 2.0f * std::atan(tyR);
                *(float*)(t_right.cam + 0x68) = 2.0f * std::atan(tyR);
                *(float*)(t_right.cam + 0xb8) = txR / tyR;
                // Split screen: left eye renders into the left half of the output, right eye into the right half.
                if (c_sbs) {
                    uint16_t W = *(uint16_t*)(t_right.cam + 0xec), H = *(uint16_t*)(t_right.cam + 0xee);
                    uint16_t x0 = *(uint16_t*)(t_right.cam + 0xe8), y0 = *(uint16_t*)(t_right.cam + 0xea);
                    if (W >= 64 && H >= 64) {
                        uint16_t hw = W / 2;
                        *(uint16_t*)(t_right.cam + 0xe8) = (uint16_t)(x0 + hw); *(uint16_t*)(t_right.cam + 0xec) = (uint16_t)(W - hw);
                        *(uint16_t*)(camCopy + 0xe8) = x0; *(uint16_t*)(camCopy + 0xea) = y0;
                        *(uint16_t*)(camCopy + 0xec) = hw; *(uint16_t*)(camCopy + 0xee) = H;
                        t_sbsOk = true;
                    }
                }
                t_right.idx = idx; t_right.r8 = r8; t_right.valid = true;
                { float va = c_fovFromViewport.load() ? ViewportAspect(t_right.cam) : 0.0f; if (va > 0) txR = tyR * va; }
                t_right.fov = {-std::atan(txR), std::atan(txR), std::atan(tyR), -std::atan(tyR)};
                t_right.pose = eyes[1].pose;
            }
            // Previous transform = what we really rendered last frame, so motion vectors (DLSS, TAA, motion blur)
            // include the head motion. Falls back to the game's previous camera with this head pose.
            {
                std::lock_guard<std::mutex> l(g_prevMu);
                if (g_prevValid && t_serial == g_prevSerial + 1) WriteXf(pxf, g_prevXf);
                else { Xf pg = ReadXf(pxf); WriteXf(pxf, ApplyPose(c_level ? Level(pg) : pg, head, c_worldScale)); }
                g_prevXf = now; g_prevSerial = t_serial; g_prevValid = true;
            }
            // Symmetric frustum covering both eyes (mono): fovY and aspect from the max tangents.
            float tu = 0, td = 0, tl = 0, tr = 0;
            for (int i = 0; i < 2; i++) {
                tu = (std::max)(tu, std::tan(eyes[i].fov.angleUp)); td = (std::max)(td, std::tan(-eyes[i].fov.angleDown));
                tl = (std::max)(tl, std::tan(-eyes[i].fov.angleLeft)); tr = (std::max)(tr, std::tan(eyes[i].fov.angleRight));
            }
            float ty = (std::max)(tu, td), tx = (std::max)(tl, tr);
            if (stereo) {   // left eye: its own symmetric frustum
                ty = (std::max)(std::tan(eyes[0].fov.angleUp), std::tan(-eyes[0].fov.angleDown));
                tx = (std::max)(std::tan(-eyes[0].fov.angleLeft), std::tan(eyes[0].fov.angleRight));
            }
            float fovY = 2.0f * std::atan(ty), aspect = tx / ty;
            g_lastReqAspect = aspect;
            *(float*)(camCopy + 0x8c) = fovY;
            *(float*)(camCopy + 0x68) = fovY;
            *(float*)(camCopy + 0xb8) = aspect;
            o_Setup(entry, idx, r8, camCopy, pxf, xf);
            // The frame shows the head pose (both eyes see the same image from the head centre).
            FramePose fp; fp.serial = t_serial; fp.valid = true;
            { float va = c_fovFromViewport.load() ? ViewportAspect(camCopy) : 0.0f; if (va > 0) tx = ty * va; }
            XrFovf f{-std::atan(tx), std::atan(tx), std::atan(ty), -std::atan(ty)};
            for (int i = 0; i < 2; i++) { fp.eyes[i].pose = head; fp.eyes[i].fov = f; }
            if (stereo) { fp.eyes[0].pose = eyes[0].pose; fp.eyes[1].pose = t_right.pose; fp.eyes[1].fov = t_right.fov; fp.sbs = t_sbsOk; }
            { std::lock_guard<std::mutex> l(g_ringMu); g_ring[t_serial % kRing] = fp; }
            g_newestMain = t_serial;
            uint64_t a = g_applied.fetch_add(1);
            if (a == 0 || (a % 3000) == 0) LOGI("fbview: head tracking active (fovY %.1f deg, requested aspect %.3f, viewport aspect %.3f -> submitted fovX %.1f deg, level %d, scale %.2f, latency %d)",
                             fovY * 57.2958f, aspect, ViewportAspect(camCopy), 2.0f * std::atan(tx) * 57.2958f, (int)c_level.load(), c_worldScale, c_latency);
            return;
        }
        g_noPose.fetch_add(1);
    }
    o_Setup(entry, idx, r8, cam, prevXf, curXf);
}

// Builds a view entry exactly like buildViews does before calling setupView (exe+29AC200..29AC3C1):
// zeroed, six RenderViews from the engine's default constructor, then the same field defaults.
typedef void* (__fastcall* PFN_RvDefault)(void* tmp);            // exe+2A08AB0 RenderView::RenderView(tmp)
typedef void  (__fastcall* PFN_RvAssign)(void* dst, const void* src); // exe+2A083B0 RenderView::operator=
static void ConstructEntry(char* e, const char* e0) {
    char* base = (char*)GetModuleHandleW(nullptr);
    auto rvDefault = (PFN_RvDefault)(base + 0x2A08AB0);
    auto rvAssign = (PFN_RvAssign)(base + 0x2A083B0);
    alignas(16) static thread_local uint8_t tmp[0x600];
    uint32_t keep = *(uint32_t*)(e + 0x2078);
    memset(e, 0, 0x2080);
    rvAssign(e, rvDefault(tmp));
    for (int i = 0; i < 4; i++) rvAssign(e + 0x540 + i * 0x540, rvDefault(tmp));
    rvAssign(e + 0x1a40, rvDefault(tmp));
    *(uint64_t*)(e + 0x1f88) = 0; e[0x1f80] = 0;
    *(uint16_t*)(e + 0x1f94) = 0xffff; *(int32_t*)(e + 0x1f90) = -1;
    memcpy(e + 0x1fa0, e0 + 0x1fa0, 0x90);           // constant matrices (overwritten by setupView anyway)
    *(void**)(e + 0x2060) = *(void* const*)(e0 + 0x2060);
    *(uint32_t*)(e + 0x2040) = 0; *(int32_t*)(e + 0x2044) = -1; *(uint16_t*)(e + 0x2048) = 0x101;
    *(uint32_t*)(e + 0x204c) = 0; *(float*)(e + 0x2050) = 1.0f; *(float*)(e + 0x2054) = -1.0f; *(float*)(e + 0x2058) = 1.0f;
    *(uint16_t*)(e + 0x205c) = 0; *(uint64_t*)(e + 0x2070) = 0; e[0x207c] = 0;
    *(uint32_t*)(e + 0x2078) = keep;
}

static void* __fastcall Hk_Build(void* out, void* table, void* r8) {
    uint64_t n = g_builds.fetch_add(1);
    t_mainDone = false;
    t_right.valid = false; t_sbsOk = false;
    t_serial = g_buildSerial.fetch_add(1) + 1;
    int armed = g_dumpArmed.load();
    if (armed == 1) g_dumpArmed = 2;
    void* r = o_Build(out, table, r8);
    uint32_t count = *(uint32_t*)((char*)out + 0x10);
    if (count >= 1 && *(char**)out) {   // the left view's camera position as built: pairs the rendered frame with its head pose
        const float* a0 = (const float*)*(char**)out;
        std::lock_guard<std::mutex> l(g_ringMu);
        FramePose& fp = g_ring[t_serial % kRing];
        if (fp.serial == t_serial) { fp.hasCam = true; fp.cam[0] = a0[12]; fp.cam[1] = a0[13]; fp.cam[2] = a0[14];
            for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) fp.rot[i * 3 + j] = a0[i * 4 + j]; }
    }
    bool appended = false;
    if (t_right.valid && count == 1) {
        appended = true;
        // Native stereo experiment: append the right eye as a second view (copy of the main entry, set up again).
        char* entries = *(char**)out;
        void** ptrs = *(void***)((char*)out + 8);
        char* e0 = entries; char* e1 = entries + 0x2080;
        if (c_stereoBuild == 0) memcpy(e1, e0, 0x2080);
        else ConstructEntry(e1, e0);
        uint16_t idx1 = (uint16_t)(c_stereoIdx >= 0 ? c_stereoIdx : t_right.idx);
        o_Setup(e1, idx1, t_right.r8, t_right.cam, t_right.prev, t_right.cur);
        if (c_stereoRegister) {
            char* base = (char*)GetModuleHandleW(nullptr);
            void* obj = *(void**)(base + 0x6207E90);
            void** vt = *(void***)obj;
            ((void(__fastcall*)(void*, void*))vt[0x98 / 8])(obj, e1);
            ((void(__fastcall*)(void*, void*))vt[0xa0 / 8])(obj, e1);
        }
        if (c_stereoForceUpdate) {   // recompute the right view's matrices from its transform/lens (dirty bits)
            char* base = (char*)GetModuleHandleW(nullptr);
            auto upd = (void(__fastcall*)(void*, const void*, uint32_t))(base + 0x2A2A8E0);
            *(uint32_t*)(e1 + 0x140) |= 0xF;
            upd(e1, base + 0x4E22A98, 0xF);
        }
        if (table && c_stereoIdx > 0 && c_stereoIdx < 4) {   // per-view matrix table row, as the builder fills it
            memcpy((char*)table + idx1 * 0x110 + 0x00, e1 + 0x480, 0x40);
            memcpy((char*)table + idx1 * 0x110 + 0x40, e1 + 0x4c0, 0x40);
            memcpy((char*)table + idx1 * 0x110 + 0x80, e1 + 0x2c0, 0x40);
            memcpy((char*)table + idx1 * 0x110 + 0xc0, e1 + 0x340, 0x40);
            memcpy((char*)table + idx1 * 0x110 + 0x100, (char*)table + t_right.idx * 0x110 + 0x100, 8);
        }
        ptrs[1] = e1 + 0x150;
        if (g_jitLog.load() > 0 && table) {   // jitter check: does the builder put a per-frame offset into the table rows?
            g_jitLog--;
            auto row = [&](const char* tag, const float* m) {
                LOGI("jit %s 480: %.5f %.5f %.5f %.5f | %.5f %.5f %.5f %.5f | %.5f %.5f %.5f %.5f | %.5f %.5f %.5f %.5f", tag,
                     m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
            };
            const float* t0 = (const float*)((char*)table + t_right.idx * 0x110);
            const float* t1 = (const float*)((char*)table + idx1 * 0x110);
            LOGI("jit build %u: rows L %u R %u | L +100 %.5f %.5f %.5f %.5f | R +100 %.5f %.5f %.5f %.5f", t_serial, t_right.idx, idx1,
                 t0[64], t0[65], t0[66], t0[67], t1[64], t1[65], t1[66], t1[67]);
            row("Lentry", (const float*)(e0 + 0x480)); row("Ltable", t0);
            row("Lentry4c0", (const float*)(e0 + 0x4c0)); row("Ltable40", t0 + 16);
            row("Rentry", (const float*)(e1 + 0x480)); row("Rtable", t1);
        }
        static DWORD tl = 0;
        if (GetTickCount() - tl > 5000) {
            tl = GetTickCount();
            const float* a = (const float*)e0; const float* b = (const float*)e1;
            LOGI("fbview: dirty v0 %X v1 %X | 2c0r3 v0 (%.3f %.3f %.3f) v1 (%.3f %.3f %.3f) | 340r3 v0 (%.3f %.3f %.3f) v1 (%.3f %.3f %.3f) | 4c0r3 v0 (%.3f %.3f %.3f) v1 (%.3f %.3f %.3f)",
                 *(uint32_t*)(e0 + 0x140), *(uint32_t*)(e1 + 0x140),
                 a[0x2c0/4+12], a[0x2c0/4+13], a[0x2c0/4+14], b[0x2c0/4+12], b[0x2c0/4+13], b[0x2c0/4+14],
                 a[0x340/4+12], a[0x340/4+13], a[0x340/4+14], b[0x340/4+12], b[0x340/4+13], b[0x340/4+14],
                 a[0x4c0/4+12], a[0x4c0/4+13], a[0x4c0/4+14], b[0x4c0/4+12], b[0x4c0/4+13], b[0x4c0/4+14]);
            LOGI("fbview: stereo views: v0 pos (%.3f %.3f %.3f) fov %.3f idx %u | v1 pos (%.3f %.3f %.3f) fov %.3f idx %u | v0 m480[12..14] (%.3f %.3f %.3f) v1 (%.3f %.3f %.3f)",
                 a[12], a[13], a[14], a[0x100 / 4], *(uint16_t*)(e0 + 0x1f94), b[12], b[13], b[14], b[0x100 / 4], *(uint16_t*)(e1 + 0x1f94),
                 a[0x480 / 4 + 12], a[0x480 / 4 + 13], a[0x480 / 4 + 14], b[0x480 / 4 + 12], b[0x480 / 4 + 13], b[0x480 / 4 + 14]);
            // projection actually built by the engine: diagonals of the four matrices, aspect, fovX (RenderView +0x118 / +0x2B0)
            LOGI("fbview: v0 proj check: aspect %.3f fovY %.3f fovX %.3f | diag 2c0 (%.3f %.3f %.3f %.3f) 340 (%.3f %.3f %.3f %.3f) 480 (%.3f %.3f %.3f %.3f) 4c0 (%.3f %.3f %.3f %.3f) | req aspect %.3f",
                 a[0x118 / 4], a[0x100 / 4], a[0x2b0 / 4],
                 a[0x2c0/4], a[0x2c0/4+5], a[0x2c0/4+10], a[0x2c0/4+15], a[0x340/4], a[0x340/4+5], a[0x340/4+10], a[0x340/4+15],
                 a[0x480/4], a[0x480/4+5], a[0x480/4+10], a[0x480/4+15], a[0x4c0/4], a[0x4c0/4+5], a[0x4c0/4+10], a[0x4c0/4+15],
                 g_lastReqAspect);
        }
        *(uint32_t*)((char*)out + 0x10) = count = 2;
        if (g_stereoBuilds.fetch_add(1) == 0) LOGI("fbview: stereo - appended right-eye view (entries %p, register %d)", entries, c_stereoRegister);
    }
    if (count > 0) ReplaySetStereoFrame(appended);
    static bool vtLogged = false;
    if (!vtLogged && count > 0) {   // RE aid: the view registry object used by the builder (exe+6207E90)
        vtLogged = true;
        char* base = (char*)GetModuleHandleW(nullptr);
        void* obj = *(void**)(base + 0x6207E90);
        if (Readable(obj, 8)) {
            void** vt = *(void***)obj;
            if (Readable(vt, 0xC0))
                LOGI("fbview: view registry %p vtable exe+%llX: +98 exe+%llX +a0 exe+%llX +a8 exe+%llX", obj, (unsigned long long)((char*)vt - base),
                     (unsigned long long)((char*)vt[0x98 / 8] - base), (unsigned long long)((char*)vt[0xa0 / 8] - base), (unsigned long long)((char*)vt[0xa8 / 8] - base));
        }
        char* e = *(char**)out;
        LOGI("fbview: entry0 +1f88 %llX +1f8c %u x %u +1f90 %d +2040 %u +2044 %X +2048 %u +2060 %p +2070 %p +2078 %u ptr0 %p",
             *(unsigned long long*)(e + 0x1f88), *(uint16_t*)(e + 0x1f8c), *(uint16_t*)(e + 0x1f8e), *(int*)(e + 0x1f90), *(uint32_t*)(e + 0x2040),
             *(uint32_t*)(e + 0x2044), *(uint8_t*)(e + 0x2048), *(void**)(e + 0x2060), *(void**)(e + 0x2070), *(uint32_t*)(e + 0x2078), **(void***)((char*)out + 8));
    }
    if (count != g_lastCount) { LOGI("fbview: game renders %u view(s) (build %llu)", count, (unsigned long long)n); g_lastCount = count; }
    if (armed == 1) {
        char* entries = *(char**)out;
        DumpBlob("out", out, 0x40);
        for (uint32_t i = 0; i < count && i < 4; i++) { char t[16]; snprintf(t, sizeof(t), "entry%u", i); DumpBlob(t, entries + i * 0x2080, 0x2080); }
        g_dumpIndex++;
        g_dumpArmed = 0;
    }
    static std::atomic<DWORD> tLast{GetTickCount()}; static uint64_t bLast = 0, aLast = 0;
    DWORD now = GetTickCount(), tl = tLast.load();
    if (now - tl >= 10000 && tLast.compare_exchange_strong(tl, now)) {
        LOGI("fbview: %.1f builds/s, %u views, mode %d, head applied %llu (no pose %llu)", (g_builds - bLast) / ((now - tl) / 1000.0), count,
             c_mode.load(), (unsigned long long)(g_applied - aLast), (unsigned long long)g_noPose.load());
        bLast = g_builds; aLast = g_applied;
    }
    return r;
}

// Render thread, right before the game's Present: tell the XR side which pose this frame was rendered with.
static void PollModeKey() {
    static bool down = false;
    bool k = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    if (k && !down) {
        DWORD pid = 0; GetWindowThreadProcessId(GetForegroundWindow(), &pid);
        if (pid == GetCurrentProcessId()) {
            // stereo -> head -> flat -> stereo
            int m = c_mode.load();
            int n = m == (int)Mode::Stereo ? (int)Mode::Head : m == (int)Mode::Head ? (int)Mode::Flat : (int)Mode::Stereo;
            c_mode = n;
            SlHookEnable(n == (int)Mode::Stereo && G().cfg.GetBool("stereo_dlss_per_eye", true));
            ReplaySetMode(n == (int)Mode::Stereo && !c_sbs && G().cfg.GetBool("stereo_replay", true) ? 1 : 0);
            LOGI("F8: vr_mode -> %s", n == (int)Mode::Stereo ? "stereo" : n == (int)Mode::Head ? "head" : "flat");
        }
    }
    down = k;
}

void FbViewOnPresent() {
    PollModeKey();
    if ((Mode)c_mode.load() == Mode::Flat) return;
    uint64_t s = g_buildSerial.load();
    if (s <= (uint64_t)c_latency) return;
    uint64_t want = s - c_latency;
    FramePose fp;
    { std::lock_guard<std::mutex> l(g_ringMu); fp = g_ring[want % kRing]; }
    if ((Mode)c_mode.load() == Mode::Stereo && !fp.sbs) return;   // stereo frames are published by the replay
    if (fp.valid && fp.serial == want) XrHostSetFramePose(fp.eyes, fp.sbs);
}

int FbViewMode() { int m = c_mode.load(); return m == (int)Mode::Stereo ? 2 : m == (int)Mode::Head ? 1 : 0; }

bool FbViewStereoPoses(XrHostEye eyes[2]) {
    uint64_t s = g_buildSerial.load();
    if (s <= (uint64_t)c_latency) return false;
    uint64_t want = s - c_latency;
    FramePose fp;
    { std::lock_guard<std::mutex> l(g_ringMu); fp = g_ring[want % kRing]; }
    if (!fp.valid || fp.serial != want) return false;
    eyes[0] = fp.eyes[0]; eyes[1] = fp.eyes[1];
    return true;
}

// The ring entry whose left camera is where this frame's left camera was (Streamline constants) - exact pairing of image
// and head pose. The fixed "latency" frame count is wrong whenever the game's simulation runs ahead by a varying amount
// (limiter, uneven frame times): the image then gets the pose of a neighbouring frame -> objects jump sideways when the
// head moves (user, 2026-11-01).
static std::atomic<uint64_t> s_camMatch{0}, s_camMiss{0}, s_camDelta[5] = {};
static double s_camDistSum = 0, s_camAngSum = 0; static std::atomic<uint64_t> s_camAngBad{0};
static std::atomic<bool> c_pairCam{true};
unsigned long long FbViewNewestSerial() { return g_newestMain.load(); }
// Pairing without Streamline (FSR / other upscalers, 2026-11-04): the serial of the newest main build when the frame's
// upscaler command list was RESET (= started recording). Checked against the camera pairing in DLSS mode (stamp stats).
static std::atomic<uint64_t> s_stampHit{0}, s_stampOld{0}, s_stampNew{0}, s_serialHit{0}, s_serialMiss{0};
bool FbViewStereoPosesForSerial(unsigned long long serial, XrHostEye eyes[2]) {
    if (!serial) return FbViewStereoPoses(eyes);
    FramePose fp;
    { std::lock_guard<std::mutex> l(g_ringMu); fp = g_ring[serial % kRing]; }
    if (!fp.valid || fp.serial != serial) { s_serialMiss++; return FbViewStereoPoses(eyes); }
    s_serialHit++;
    eyes[0] = fp.eyes[0]; eyes[1] = fp.eyes[1];
    static DWORD t0 = GetTickCount();
    if (GetTickCount() - t0 >= 10000) { t0 = GetTickCount();
        LOGI("fbview: frame/pose pairing by command-list stamp: %llu paired, %llu not in the ring (10 s)", (unsigned long long)s_serialHit.exchange(0), (unsigned long long)s_serialMiss.exchange(0)); }
    return true;
}
static uint64_t g_stampForCam = 0;
void FbViewNoteStamp(unsigned long long stamp) { g_stampForCam = stamp; }
bool FbViewStereoPosesForCam(const float cam[6], XrHostEye eyes[2]) {
    if (!c_pairCam.load()) return FbViewStereoPoses(eyes);
    // cam = left camera position + forward (Streamline). Match position AND orientation: with the head turning in place the
    // left eye barely moves, so position alone tied between builds with different head rotations (user, 2026-11-02:
    // "everything jumps"). Orientation error = 1 - |dot(forward, best row of the build's rotation)|.
    uint64_t s = g_buildSerial.load();
    FramePose best; float bestD = 1e30f, bestPos = 0, bestAng = 0;
    {
        std::lock_guard<std::mutex> l(g_ringMu);
        for (auto& fp : g_ring) {
            if (!fp.valid || !fp.hasCam) continue;
            float dx = fp.cam[0] - cam[0], dy = fp.cam[1] - cam[1], dz = fp.cam[2] - cam[2];
            float dp = dx * dx + dy * dy + dz * dz;
            float bestDot = 0;
            for (int r = 0; r < 3; r++) { float d = std::fabs(fp.rot[r * 3] * cam[3] + fp.rot[r * 3 + 1] * cam[4] + fp.rot[r * 3 + 2] * cam[5]); if (d > bestDot) bestDot = d; }
            float da = 1.0f - bestDot;
            float d = dp + da * 1000.0f;   // 1e-6 in dot (~0.08 deg) weighs like 1 mm
            if (d < bestD) { bestD = d; best = fp; bestPos = std::sqrt(dp); bestAng = std::acos(std::min(1.0f, bestDot)) * 57.2958f; }
        }
    }
    bestD = bestPos * bestPos;
    float tol = 0.01f + 2e-5f * (std::fabs(cam[0]) + std::fabs(cam[1]) + std::fabs(cam[2]));
    if (bestD > tol * tol) { s_camMiss++; return FbViewStereoPoses(eyes); }
    s_camDistSum += std::sqrt(bestD) * 1000.0; s_camAngSum += bestAng;
    if (bestAng > 0.05f) s_camAngBad++;
    s_camMatch++;
    uint64_t dlt = s - best.serial; s_camDelta[dlt < 4 ? dlt : 4]++;
    if (g_stampForCam) { if (g_stampForCam == best.serial) s_stampHit++; else if (g_stampForCam < best.serial) s_stampOld++; else s_stampNew++; g_stampForCam = 0; }
    eyes[0] = best.eyes[0]; eyes[1] = best.eyes[1];
    static DWORD t0 = GetTickCount();
    if (GetTickCount() - t0 >= 10000) {
        t0 = GetTickCount();
        uint64_t m = s_camMatch.load();
        LOGI("fbview: frame/pose pairing by camera: %llu matched (avg distance %.3f mm, avg angle %.4f deg, %llu > 0.05 deg), %llu fell back to latency %d | builds behind the newest: 0:%llu 1:%llu 2:%llu 3:%llu 4+:%llu (10 s)",
             (unsigned long long)s_camMatch.exchange(0), m ? s_camDistSum / m : 0.0, m ? s_camAngSum / m : 0.0, (unsigned long long)s_camAngBad.exchange(0), (unsigned long long)s_camMiss.exchange(0), c_latency,
             (unsigned long long)s_camDelta[0].exchange(0), (unsigned long long)s_camDelta[1].exchange(0), (unsigned long long)s_camDelta[2].exchange(0),
             (unsigned long long)s_camDelta[3].exchange(0), (unsigned long long)s_camDelta[4].exchange(0));
        s_camDistSum = 0; s_camAngSum = 0;
        LOGI("fbview: command-list stamp vs camera pairing: same build %llu, stamp older %llu, stamp newer %llu (10 s)",
             (unsigned long long)s_stampHit.exchange(0), (unsigned long long)s_stampOld.exchange(0), (unsigned long long)s_stampNew.exchange(0));
    }
    return true;
}

void FbViewApplyConfig() {
    std::string m = G().cfg.GetStr("vr_mode", "stereo");
    c_mode = m == "flat" ? (int)Mode::Flat : m == "stereo" ? (int)Mode::Stereo : (int)Mode::Head;
    c_level = G().cfg.GetBool("level_camera", true);
    c_worldScale = G().cfg.GetFloat("world_scale", 1.0f);
    c_latency = G().cfg.GetInt("pose_latency_frames", 1);
    c_pairCam = G().cfg.GetStr("pose_pairing", "camera") != "latency";   // camera pairing verified by the world-lock test (122)
    c_fakeYawDeg = G().cfg.GetFloat("debug_fake_head_yaw_deg", 0.0f);
    c_fovFromViewport = G().cfg.GetBool("fov_from_viewport", false);
    ReplaySetSwapEyes(G().cfg.GetBool("swap_eyes", false));
    c_probeLog = G().cfg.GetBool("view_probe", false);
    c_stereoRegister = G().cfg.GetInt("stereo_register_view", 1);
    c_stereoBuild = G().cfg.GetInt("stereo_build", 1);
    c_stereoIdx = G().cfg.GetInt("stereo_view_index", 1);
    c_stereoForceUpdate = G().cfg.GetInt("stereo_force_update", 1);
    LOGI("fbview stereo config: register %d build %d index %d force_update %d", G().cfg.GetInt("stereo_register_view", 1), G().cfg.GetInt("stereo_build", 1), c_stereoIdx, c_stereoForceUpdate);
    c_sbs = G().cfg.GetBool("stereo_side_by_side", false);
    ReplaySetSnapshot(G().cfg.GetInt("stereo_replay_snapshot", 2));
    const bool perEye = G().cfg.GetBool("stereo_dlss_per_eye", true);
    SlHookEnable(c_mode.load() == (int)Mode::Stereo && perEye);
    { fovdlss::Config fc; fc.enabled = G().cfg.GetBool("fov_dlss", true);
      fc.width = G().cfg.GetFloat("fov_dlss_width", 0.6f); fc.height = G().cfg.GetFloat("fov_dlss_height", 0.6f);
      fc.periphDlss = G().cfg.GetBool("fov_dlss_periphery", false);
      auto preset = [](const std::string& v) -> unsigned { if (v.size() == 1 && v[0] >= 'A' && v[0] <= 'Z') return (unsigned)(v[0] - 'A' + 1); if (v.size() == 1 && v[0] >= 'a' && v[0] <= 'z') return (unsigned)(v[0] - 'a' + 1); return 0u; };
      fc.periphPreset = preset(G().cfg.GetStr("fov_dlss_periphery_preset", "E")); if (!fc.periphPreset) fc.periphPreset = 5;
      fc.periphTaa = G().cfg.GetBool("fov_dlss_periphery_taa", true); fc.taaBlend = G().cfg.GetFloat("fov_dlss_taa_blend", 0.1f);
      fc.centrePreset = preset(G().cfg.GetStr("fov_dlss_centre_preset", "game")); fc.debugBorder = G().cfg.GetBool("fov_dlss_border", false);
      fovdlss::SetConfig(fc); }
    { vrs::Config vc; vc.enabled = G().cfg.GetBool("fov_vrs", false); vc.inner = G().cfg.GetFloat("fov_vrs_inner", 0.6f);
      vc.mid = G().cfg.GetFloat("fov_vrs_mid", 2.0f); vc.coarseOuter = G().cfg.GetBool("fov_vrs_outer_4x4", false);
      vrs::SetConfig(vc); ReplaySetVrs(vc.enabled); }
    SlHookSetRightJitter(0);   // test options retired (2026-11-04): jitter as the game reports, routing by call order
    ReplaySetPairStamp(G().cfg.GetBool("pose_pairing_stamp", true));
    SlHookSetTagRouting(0);
    ReplaySetLeftInputs(G().cfg.GetInt("stereo_replay_left_inputs", 1));
    ReplaySetHistory(G().cfg.GetBool("stereo_replay_history", true) ? 1 : 0);
    ReplaySetSceneHistory(0);   // off: no effect on the right eye, broke HUD / menus (user, stage 15)
    { std::string h = G().cfg.GetStr("hud_layer", "1");   // 0 / 1 (menu writes false / true), 2 / 3 debug modes
      ReplaySetHudLayer(h == "true" ? 1 : h == "false" ? 0 : atoi(h.c_str())); }
    ReplaySetDebug(G().cfg.GetInt("stereo_replay_debug", 0), G().cfg.GetInt("stereo_replay_start_shift", perEye ? 1 : 0));
    ReplaySetMode(c_mode.load() == (int)Mode::Stereo && !c_sbs && G().cfg.GetBool("stereo_replay", true) ? 1 : 0);
    LOGI("fbview config: vr_mode %s, level_camera %d, world_scale %.2f, pose_latency_frames %d, fake yaw %.1f",
         m.c_str(), (int)c_level.load(), c_worldScale, c_latency, c_fakeYawDeg);
}

void FbViewDumpTrigger() { g_dumpArmed = 1; g_jitLog = 6; }

bool FbViewInstall() {
    FbViewApplyConfig();
    char* base = (char*)GetModuleHandleW(nullptr);
    static const uint8_t pb[] = { 0x40, 0x55, 0x48, 0x8D, 0xAC, 0x24 };       // push rbp; lea rbp,[rsp-..]
    static const uint8_t ps[] = { 0x48, 0x8B, 0xC4, 0x56, 0x57, 0x41, 0x57 }; // mov rax,rsp; push rsi; push rdi; push r15
    if (memcmp(base + RVA_BUILDVIEWS, pb, sizeof(pb)) != 0 || memcmp(base + RVA_SETUPVIEW, ps, sizeof(ps)) != 0) {
        LOGE("fbview: function prologues do not match this game build - view hooks disabled (game updated?)");
        return false;
    }
    bool ok = true;
    if (MH_CreateHook(base + RVA_BUILDVIEWS, (void*)Hk_Build, (void**)&o_Build) != MH_OK || MH_EnableHook(base + RVA_BUILDVIEWS) != MH_OK) ok = false;
    if (MH_CreateHook(base + RVA_SETUPVIEW, (void*)Hk_Setup, (void**)&o_Setup) != MH_OK || MH_EnableHook(base + RVA_SETUPVIEW) != MH_OK) ok = false;
    LOGI("fbview: hooks %s (buildViews exe+%llX, setupView exe+%llX)", ok ? "on" : "FAILED",
         (unsigned long long)RVA_BUILDVIEWS, (unsigned long long)RVA_SETUPVIEW);
    return ok;
}

} // namespace davg
