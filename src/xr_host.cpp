// xr_host: OpenXR output for D3D12 games (see xr_host.h for the design).
// Structure ported from the Star Ocean Divine Force mod (so6vr/src/xr_runtime.cpp, D3D11 + keyed mutexes),
// with the hand-off rebuilt for a D3D12 producer (shared NT-handle textures + shared fences).
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <vector>
#include <atomic>
#include <mutex>
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include "xr_host.h"
#include "fovdlss.h"
#include "vrs.h"
#include "menu.h"
#include "vrkit_menu.h"
#include "globals.h"
#include "log.h"

namespace davg {
namespace {

// ---- OpenXR function table -------------------------------------------------------------------------
#define DAVG_XR_GLOBAL_FUNCS(X) X(xrEnumerateInstanceExtensionProperties) X(xrCreateInstance)
#define DAVG_XR_INSTANCE_FUNCS(X) \
    X(xrDestroyInstance) X(xrGetInstanceProperties) X(xrResultToString) \
    X(xrGetSystem) X(xrGetSystemProperties) X(xrPollEvent) \
    X(xrCreateSession) X(xrDestroySession) X(xrBeginSession) X(xrEndSession) \
    X(xrCreateReferenceSpace) X(xrDestroySpace) \
    X(xrEnumerateSwapchainFormats) X(xrCreateSwapchain) X(xrDestroySwapchain) \
    X(xrEnumerateSwapchainImages) X(xrAcquireSwapchainImage) X(xrWaitSwapchainImage) X(xrReleaseSwapchainImage) \
    X(xrWaitFrame) X(xrBeginFrame) X(xrEndFrame) X(xrLocateViews) X(xrEnumerateViewConfigurationViews) \
    X(xrGetD3D11GraphicsRequirementsKHR)

PFN_xrGetInstanceProcAddr p_xrGetInstanceProcAddr = nullptr;
#define DAVG_DECL(fn) PFN_##fn p_##fn = nullptr;
DAVG_XR_GLOBAL_FUNCS(DAVG_DECL)
DAVG_XR_INSTANCE_FUNCS(DAVG_DECL)
#undef DAVG_DECL

// ---- config (atomics: written by XrHostApplyConfig, read by the XR thread) --------------------------
std::atomic<bool>  c_enabled{true};
std::atomic<float> c_screenDistance{2.5f}, c_screenWidth{3.2f}, c_screenHeight{0.0f};
std::atomic<int>   c_dumpFrame{0};
std::atomic<uint32_t> g_recW{0}, g_recH{0};
std::atomic<uint32_t> g_maxSwW{0}, g_maxSwH{0};   // runtime's maximum swapchain image size
// xr_pacing: 0 headset (submit every headset frame, held frames re-sent with their pose) | 1 game (submit only when the
// game delivered a new frame, so the runtime sees the real frame rate and its reprojection / motion smoothing engages)
std::atomic<int> c_pacing{1};
HANDLE g_newFrameEvent = nullptr;
std::atomic<int64_t> g_lastWaitQpc{0};   // when the last xrWaitFrame returned (pose prediction extrapolates from it)   // headset's recommended per-eye size
// HUD layer (stereo): the game's UI image on its own quad
std::atomic<float> c_hudDistance{1.5f}, c_hudWidth{1.4f}, c_hudHeight{-0.05f};
std::atomic<int>   c_hudAnchor{0};          // 0 world (recentred reference), 1 head
std::atomic<bool>  c_hudPremul{true}, c_hudVisible{true};
static bool g_poseSbs = false;
constexpr int kMonoHoldFrames = 10;   // frames without a pose before falling back to the flat screen

// ---- XR state (XR thread) ----------------------------------------------------------------------------
HMODULE     g_loader = nullptr;
XrInstance  g_instance = XR_NULL_HANDLE;
XrSystemId  g_system = XR_NULL_SYSTEM_ID;
XrSession   g_session = XR_NULL_HANDLE;
XrSpace     g_space = XR_NULL_HANDLE;
XrSpace     g_viewSpace = XR_NULL_HANDLE;   // head-locked HUD
std::atomic<bool> g_running{false};
std::atomic<XrTime> g_lastDisplayTime{0};
std::atomic<XrDuration> g_displayPeriod{11111111};
std::mutex  g_sessionMutex;   // session / space teardown vs. locate calls from other threads

ID3D11Device*         g_dev = nullptr;   // our own device on the game's GPU
ID3D11Device5*        g_dev5 = nullptr;
ID3D11DeviceContext*  g_ctx = nullptr;
ID3D11DeviceContext4* g_ctx4 = nullptr;
LUID                  g_gameLuid{};

// One XR swapchain per shared set (flat/mono: back buffer size; stereo: double-width eye pair).
struct XrSc {
    XrSwapchain sc = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageD3D11KHR> images;
    std::vector<ID3D11RenderTargetView*> rtvs;
    UINT w = 0, h = 0;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    bool hasImage = false;
};
XrSc g_sc[3];   // [0] screen, [1] stereo (left eye), [2] HUD
XrSc g_scR;     // stereo right eye: one swapchain per eye (2026-11-04: 2 x 4776 > 8192 runtime max broke the second eye)
XrSc g_menuSc;   // settings menu (vrkit), copied straight from the menu texture

// blit pipeline (XR device)
ID3D11VertexShader* g_vs = nullptr;
ID3D11PixelShader*  g_psDecode = nullptr;   // gamma-encoded source -> linear (the sRGB target re-encodes)
ID3D11PixelShader*  g_psPass = nullptr;     // copy values as they are
ID3D11PixelShader*  g_psHud = nullptr;      // HUD: gamma-encoded rgb -> linear, alpha kept
ID3D11SamplerState* g_samp = nullptr;
ID3D11SamplerState* g_sampLin = nullptr;   // LumaSharpen taps
ID3D11Buffer* g_blitCb = nullptr;
std::atomic<float> c_sharpen{0.0f};
ID3D11RasterizerState* g_rs = nullptr;

// recentre reference: yaw-only rotation + position, in LOCAL space (read by the game thread too)
std::mutex g_refMutex;
float g_refYaw = 0.0f; XrVector3f g_refPos{0, 0, 0};
std::atomic<bool> g_recenterReq{false};
std::mutex g_fovMutex;
XrFovf g_fov[2] = {}; bool g_fovValid = false;

std::atomic<bool> g_dumpReq{false};
int g_dumpIndex = 0;

// ---- shared ring -------------------------------------------------------------------------------------
constexpr int kSlots = 3;
enum Kind : int { kFlat = 0, kMono = 1, kStereo = 2, kSbs = 3 };   // kSbs: back buffer holds both eyes side by side
inline int SetOf(int kind) { return kind == kStereo ? 1 : 0; }
struct SlotMeta {
    uint64_t fenceValue = 0, seq = 0; uint32_t generation = 0;
    int kind = kFlat;
    XrHostEye eyes[2] = {};    // LOCAL space (mono / stereo)
    bool hud = false; uint32_t hudGeneration = 0;   // stereo: slot of the HUD set holds this frame's UI image
};
struct SharedSet {
    const char* name = "";
    UINT w = 0, h = 0; DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    ID3D11Texture2D* tex11[kSlots] = {};          // owned by the XR device
    ID3D12Resource*  tex12[kSlots] = {};          // the same textures opened on the game device
    uint32_t generation = 0;
    uint64_t lastRealloc = 0;
    bool vertical = false;                        // stereo: eyes stacked top / bottom instead of side by side
};
SharedSet g_set[3];                               // [0] flat / mono (back buffer), [1] stereo (2 eyes side by side), [2] HUD
std::mutex g_setMutex;
SlotMeta g_meta[kSlots];
std::mutex g_metaMutex;
std::atomic<int> g_latestSlot{-1}, g_readingSlot{-1};
std::atomic<uint64_t> g_slotRelease[kSlots];      // release-fence value after the XR thread's last read of a slot

// fences (created on the XR device, opened on the game device)
ID3D11Fence* g_frameFence11 = nullptr; ID3D11Fence* g_releaseFence11 = nullptr;
HANDLE g_frameFenceHandle = nullptr, g_releaseFenceHandle = nullptr;
uint64_t g_releaseValue = 0;                      // XR thread
std::atomic<bool> g_xrReady{false};               // device + fences + blit ready

// producer (render thread)
ID3D12Device* g_gameDev = nullptr;
ID3D12Fence* g_frameFence12 = nullptr; ID3D12Fence* g_releaseFence12 = nullptr;
ID3D12Fence* g_copyFence = nullptr;               // our own: tells when a slot's command allocator is free
ID3D12CommandQueue* g_lastQueue = nullptr;        // queue of the previous hand-off (orders work across queues)
uint64_t g_frameValue = 0, g_copyValue = 0;
uint64_t g_slotCopyDone[kSlots] = {};
ID3D12CommandAllocator* g_alloc[kSlots] = {};
ID3D12GraphicsCommandList* g_list[kSlots] = {};
std::atomic<bool> g_threadStarted{false};
bool g_producerFailed = false;
uint64_t g_seq = 0;
// mono / stereo state of the frame being prepared (render thread)
std::mutex g_poseMutex;
bool g_poseSet = false; XrHostEye g_framePose[2] = {};   // LOCAL space
bool g_stereoThisFrame = false;
int g_framesWithoutPose = 1000;

// stats
std::atomic<uint64_t> s_published{0}, s_skipBusy{0}, s_skipAlloc{0}, s_held{0}, s_stereo{0};
std::atomic<uint64_t> s_xrFrames{0}, s_xrLayers{0}, s_newFrames{0};
std::atomic<uint64_t> s_waitUs{0};

template <typename T> void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

const char* XrStr(XrResult r) {
    static thread_local char buf[XR_MAX_RESULT_STRING_SIZE];
    if (p_xrResultToString && g_instance && XR_SUCCEEDED(p_xrResultToString(g_instance, r, buf))) return buf;
    snprintf(buf, sizeof(buf), "XrResult(%d)", (int)r);
    return buf;
}
#define XR_CHECK(call) do { XrResult _r = (call); if (XR_FAILED(_r)) { LOGE("%s failed: %s", #call, XrStr(_r)); return false; } } while (0)

DXGI_FORMAT TypedFormat(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default: return f;
    }
}
bool IsLinearFloat(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_R16G16B16A16_FLOAT || f == DXGI_FORMAT_R11G11B10_FLOAT || f == DXGI_FORMAT_R32G32B32A32_FLOAT;
}
bool Supported(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_R10G10B10A2_UNORM || f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM ||
           f == DXGI_FORMAT_R16G16B16A16_FLOAT || f == DXGI_FORMAT_R11G11B10_FLOAT;
}

// ---- pose math (yaw about +Y; x' = c x + s z, z' = -s x + c z; quaternion (0, sin(y/2), 0, cos(y/2))) ----
XrQuaternionf QMul(const XrQuaternionf& a, const XrQuaternionf& b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
XrQuaternionf QYaw(float yaw) { return {0, std::sin(yaw * 0.5f), 0, std::cos(yaw * 0.5f)}; }
XrVector3f RotY(float yaw, const XrVector3f& v) {
    float c = std::cos(yaw), s = std::sin(yaw);
    return {c * v.x + s * v.z, v.y, -s * v.x + c * v.z};
}
void GetRef(float& yaw, XrVector3f& pos) { std::lock_guard<std::mutex> l(g_refMutex); yaw = g_refYaw; pos = g_refPos; }
// LOCAL -> recentred
XrPosef ToRef(const XrPosef& p, float yaw, const XrVector3f& ref) {
    XrPosef o;
    o.position = RotY(-yaw, {p.position.x - ref.x, p.position.y - ref.y, p.position.z - ref.z});
    o.orientation = QMul(QYaw(-yaw), p.orientation);
    return o;
}
// recentred -> LOCAL
XrPosef FromRef(const XrPosef& p, float yaw, const XrVector3f& ref) {
    XrPosef o;
    XrVector3f r = RotY(yaw, p.position);
    o.position = {r.x + ref.x, r.y + ref.y, r.z + ref.z};
    o.orientation = QMul(QYaw(yaw), p.orientation);
    return o;
}

// =====================================================================================
// OpenXR setup (XR thread)
// =====================================================================================
bool LoadLoader() {
    if (g_loader) return true;
    std::wstring path = G().gameDir + L"openxr_loader.dll";
    g_loader = LoadLibraryW(path.c_str());
    if (!g_loader) { LOGE("xr: %S not found (err %lu); VR output disabled", path.c_str(), GetLastError()); return false; }
    p_xrGetInstanceProcAddr = (PFN_xrGetInstanceProcAddr)GetProcAddress(g_loader, "xrGetInstanceProcAddr");
    if (!p_xrGetInstanceProcAddr) { LOGE("xr: openxr_loader.dll has no xrGetInstanceProcAddr"); return false; }
#define DAVG_LOAD_GLOBAL(fn) p_xrGetInstanceProcAddr(XR_NULL_HANDLE, #fn, (PFN_xrVoidFunction*)&p_##fn);
    DAVG_XR_GLOBAL_FUNCS(DAVG_LOAD_GLOBAL)
#undef DAVG_LOAD_GLOBAL
    return p_xrCreateInstance && p_xrEnumerateInstanceExtensionProperties;
}

// ---- eye tracking (XR_EXT_eye_gaze_interaction) -> foveated DLSS centre per eye ----------------------------------
bool g_gazeExt = false, g_gazeSupported = false;
XrActionSet g_gazeSet = XR_NULL_HANDLE; XrAction g_gazeAction = XR_NULL_HANDLE; XrSpace g_gazeSpace = XR_NULL_HANDLE;
std::atomic<int> c_gazeMode{0};   // 0 off (image centre), 1 eye tracking, 2 simulated (debug circle)
std::atomic<uint64_t> s_gazeOk{0}, s_gazeLost{0};
template <class F> bool LoadFn(const char* n, F& f) { return XR_SUCCEEDED(p_xrGetInstanceProcAddr(g_instance, n, (PFN_xrVoidFunction*)&f)) && f; }
PFN_xrSyncActions p_xrSyncActions; PFN_xrLocateSpace p_xrLocateSpace;
void SetupGaze() {
    PFN_xrStringToPath toPath; PFN_xrCreateActionSet cas; PFN_xrCreateAction ca; PFN_xrSuggestInteractionProfileBindings sib;
    PFN_xrAttachSessionActionSets att; PFN_xrCreateActionSpace cspace;
    if (!LoadFn("xrStringToPath", toPath) || !LoadFn("xrCreateActionSet", cas) || !LoadFn("xrCreateAction", ca) ||
        !LoadFn("xrSuggestInteractionProfileBindings", sib) || !LoadFn("xrAttachSessionActionSets", att) ||
        !LoadFn("xrCreateActionSpace", cspace) || !LoadFn("xrSyncActions", p_xrSyncActions) || !LoadFn("xrLocateSpace", p_xrLocateSpace)) {
        LOGW("xr: eye gaze: action functions missing"); return; }
    XrActionSetCreateInfo si{XR_TYPE_ACTION_SET_CREATE_INFO}; strcpy_s(si.actionSetName, "davgvr_gaze"); strcpy_s(si.localizedActionSetName, "Eye gaze");
    if (XR_FAILED(cas(g_instance, &si, &g_gazeSet))) { LOGW("xr: eye gaze: action set failed"); return; }
    XrActionCreateInfo ai{XR_TYPE_ACTION_CREATE_INFO}; ai.actionType = XR_ACTION_TYPE_POSE_INPUT;
    strcpy_s(ai.actionName, "gaze"); strcpy_s(ai.localizedActionName, "Eye gaze");
    if (XR_FAILED(ca(g_gazeSet, &ai, &g_gazeAction))) { LOGW("xr: eye gaze: action failed"); return; }
    XrPath prof, bind;
    toPath(g_instance, "/interaction_profiles/ext/eye_gaze_interaction", &prof);
    toPath(g_instance, "/user/eyes_ext/input/gaze_ext/pose", &bind);
    XrActionSuggestedBinding b{g_gazeAction, bind};
    XrInteractionProfileSuggestedBinding sb{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING}; sb.interactionProfile = prof; sb.suggestedBindings = &b; sb.countSuggestedBindings = 1;
    XrResult r = sib(g_instance, &sb);
    XrSessionActionSetsAttachInfo at{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO}; at.countActionSets = 1; at.actionSets = &g_gazeSet;
    XrResult r2 = att(g_session, &at);
    XrActionSpaceCreateInfo asi{XR_TYPE_ACTION_SPACE_CREATE_INFO}; asi.action = g_gazeAction; asi.poseInActionSpace.orientation.w = 1;
    XrResult r3 = cspace(g_session, &asi, &g_gazeSpace);
    LOGI("xr: eye gaze action set up (bindings %s, attach %s, space %s)", XrStr(r), XrStr(r2), XrStr(r3));
    if (XR_FAILED(r3)) g_gazeSpace = XR_NULL_HANDLE;
}
// Gaze direction (head space) -> uv of each eye's image. The game renders each eye with a symmetric frustum of the
// eye's larger half-tangents (fbview), centred on the eye's forward axis.
void GazeToEyes(const XrVector3f& dHead, const XrFovf fov[2], const XrQuaternionf eyeRot[2], float uv[2][2]) {
    for (int e = 0; e < 2; e++) {
        XrQuaternionf q = eyeRot[e]; q.x = -q.x; q.y = -q.y; q.z = -q.z;   // inverse: head -> eye
        // rotate dHead by q
        float x = dHead.x, y = dHead.y, z = dHead.z;
        float tx = 2 * (q.y * z - q.z * y), ty = 2 * (q.z * x - q.x * z), tz = 2 * (q.x * y - q.y * x);
        float rx = x + q.w * tx + (q.y * tz - q.z * ty), ry = y + q.w * ty + (q.z * tx - q.x * tz), rz = z + q.w * tz + (q.x * ty - q.y * tx);
        float hx = std::max(std::tan(-fov[e].angleLeft), std::tan(fov[e].angleRight));
        float hy = std::max(std::tan(fov[e].angleUp), std::tan(-fov[e].angleDown));
        float u = 0.5f, v = 0.5f;
        if (rz < -1e-3f) { u = 0.5f + 0.5f * (rx / -rz) / hx; v = 0.5f - 0.5f * (ry / -rz) / hy; }
        uv[e][0] = std::clamp(u, 0.0f, 1.0f); uv[e][1] = std::clamp(v, 0.0f, 1.0f);
    }
}
void UpdateGaze(XrTime t) {
    static float sm[2][2] = {{0.5f, 0.5f}, {0.5f, 0.5f}};
    int mode = c_gazeMode.load();
    float uv[2][2] = {{0.5f, 0.5f}, {0.5f, 0.5f}};
    XrFovf fov[2];
    bool haveFov = XrHostHeadsetFov(fov);
    XrQuaternionf eyeRot[2] = {{0, 0, 0, 1}, {0, 0, 0, 1}};
    if (mode == 1 && g_gazeSpace && p_xrSyncActions && haveFov) {
        XrActiveActionSet as{g_gazeSet, XR_NULL_PATH};
        XrActionsSyncInfo sy{XR_TYPE_ACTIONS_SYNC_INFO}; sy.countActiveActionSets = 1; sy.activeActionSets = &as;
        p_xrSyncActions(g_session, &sy);
        XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
        if (XR_SUCCEEDED(p_xrLocateSpace(g_gazeSpace, g_viewSpace, t, &loc)) && (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
            const XrQuaternionf& q = loc.pose.orientation;   // gaze forward = -Z rotated by q
            XrVector3f d{-2 * (q.x * q.z + q.w * q.y), -2 * (q.y * q.z - q.w * q.x), -(1 - 2 * (q.x * q.x + q.y * q.y))};
            XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO}; li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO; li.displayTime = t; li.space = g_viewSpace;
            XrViewState vs{XR_TYPE_VIEW_STATE}; XrView v[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}}; uint32_t n = 0;
            if (XR_SUCCEEDED(p_xrLocateViews(g_session, &li, &vs, 2, &n, v)) && n == 2) { eyeRot[0] = v[0].pose.orientation; eyeRot[1] = v[1].pose.orientation; }
            GazeToEyes(d, fov, eyeRot, uv);
            s_gazeOk++;
        } else { s_gazeLost++; uv[0][0] = sm[0][0]; uv[0][1] = sm[0][1]; uv[1][0] = sm[1][0]; uv[1][1] = sm[1][1]; }   // keep the last
    } else if (mode == 2) {   // debug: the centre circles at 0.25 Hz
        static LARGE_INTEGER f0{}; LARGE_INTEGER q, fr; QueryPerformanceCounter(&q); QueryPerformanceFrequency(&fr); if (!f0.QuadPart) f0 = q;
        double s = double(q.QuadPart - f0.QuadPart) / fr.QuadPart * 2 * 3.14159265 * 0.25;
        for (int e = 0; e < 2; e++) { uv[e][0] = 0.5f + 0.2f * (float)std::cos(s); uv[e][1] = 0.5f + 0.15f * (float)std::sin(s); }
    }
    for (int e = 0; e < 2; e++) for (int k = 0; k < 2; k++) {   // light smoothing (eye tracker jitter); saccades settle in ~3 frames
        float a = mode == 1 ? 0.5f : 1.0f;
        sm[e][k] += (uv[e][k] - sm[e][k]) * a;
    }
    fovdlss::SetEyeCenter(0, sm[0][0], sm[0][1]); fovdlss::SetEyeCenter(1, sm[1][0], sm[1][1]);
    vrs::SetCenter((sm[0][0] + sm[1][0]) * 0.5f, (sm[0][1] + sm[1][1]) * 0.5f);
    static DWORD tl = 0;
    if (mode == 1 && GetTickCount() - tl > 10000) { tl = GetTickCount();
        LOGI("xr: eye gaze %llu ok, %llu not tracked (10 s)", (unsigned long long)s_gazeOk.exchange(0), (unsigned long long)s_gazeLost.exchange(0)); }
}

bool CreateInstance() {
    uint32_t n = 0;
    XR_CHECK(p_xrEnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr));
    std::vector<XrExtensionProperties> exts(n, {XR_TYPE_EXTENSION_PROPERTIES});
    XR_CHECK(p_xrEnumerateInstanceExtensionProperties(nullptr, n, &n, exts.data()));
    bool hasD3D11 = false;
    for (auto& e : exts) if (strcmp(e.extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME) == 0) hasD3D11 = true;
    if (!hasD3D11) { LOGE("xr: runtime lacks %s", XR_KHR_D3D11_ENABLE_EXTENSION_NAME); return false; }
    bool hasGaze = false;
    for (auto& e : exts) if (strcmp(e.extensionName, XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME) == 0) hasGaze = true;
    const char* enabled[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME, XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME};
    g_gazeExt = hasGaze;
    LOGI("xr: eye gaze extension %s", hasGaze ? "available (enabled)" : "not offered by the runtime");
    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy_s(ci.applicationInfo.applicationName, "Dragon Age The Veilguard VR");
    ci.applicationInfo.applicationVersion = 1;
    strcpy_s(ci.applicationInfo.engineName, "davgvr");
    ci.applicationInfo.engineVersion = 1;
    ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ci.enabledExtensionCount = hasGaze ? 2 : 1;
    ci.enabledExtensionNames = enabled;
    XR_CHECK(p_xrCreateInstance(&ci, &g_instance));
#define DAVG_LOAD_INST(fn) \
    if (XR_FAILED(p_xrGetInstanceProcAddr(g_instance, #fn, (PFN_xrVoidFunction*)&p_##fn)) || !p_##fn) { LOGE("xr: missing function " #fn); return false; }
    DAVG_XR_INSTANCE_FUNCS(DAVG_LOAD_INST)
#undef DAVG_LOAD_INST
    XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
    if (XR_SUCCEEDED(p_xrGetInstanceProperties(g_instance, &ip)))
        LOGI("xr: runtime %s %u.%u.%u", ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion),
             XR_VERSION_MINOR(ip.runtimeVersion), XR_VERSION_PATCH(ip.runtimeVersion));
    return true;
}

// 1 = got a system, 0 = headset not available yet, -1 = hard failure
int TryGetSystem() {
    XrSystemGetInfo gi{XR_TYPE_SYSTEM_GET_INFO};
    gi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrResult r = p_xrGetSystem(g_instance, &gi, &g_system);
    if (r == XR_ERROR_FORM_FACTOR_UNAVAILABLE) return 0;
    if (XR_FAILED(r)) { LOGE("xr: xrGetSystem failed: %s", XrStr(r)); return -1; }
    XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
    XrSystemEyeGazeInteractionPropertiesEXT gp{XR_TYPE_SYSTEM_EYE_GAZE_INTERACTION_PROPERTIES_EXT};
    if (g_gazeExt) sp.next = &gp;
    if (XR_SUCCEEDED(p_xrGetSystemProperties(g_instance, g_system, &sp))) {
        g_gazeSupported = g_gazeExt && gp.supportsEyeGazeInteraction;
        if (g_gazeExt) LOGI("xr: eye gaze interaction %s", g_gazeSupported ? "SUPPORTED by this headset" : "not supported by this headset");
        g_maxSwW = sp.graphicsProperties.maxSwapchainImageWidth; g_maxSwH = sp.graphicsProperties.maxSwapchainImageHeight;
        LOGI("xr: headset '%s' (vendor %u, max swapchain %ux%u, max layers %u)", sp.systemName, sp.vendorId,
             sp.graphicsProperties.maxSwapchainImageWidth, sp.graphicsProperties.maxSwapchainImageHeight,
             sp.graphicsProperties.maxLayerCount);
    }
    uint32_t nv = 0;
    if (XR_SUCCEEDED(p_xrEnumerateViewConfigurationViews(g_instance, g_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &nv, nullptr)) && nv >= 1) {
        std::vector<XrViewConfigurationView> v(nv, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
        if (XR_SUCCEEDED(p_xrEnumerateViewConfigurationViews(g_instance, g_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, nv, &nv, v.data())))
        {
            LOGI("xr: recommended eye size %ux%u (max %ux%u), %u views", v[0].recommendedImageRectWidth, v[0].recommendedImageRectHeight,
                 v[0].maxImageRectWidth, v[0].maxImageRectHeight, nv);
            g_recW = v[0].recommendedImageRectWidth; g_recH = v[0].recommendedImageRectHeight;
        }
    }
    return 1;
}

typedef HRESULT(WINAPI* PFN_Compile)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
const char* kHlsl = R"(
struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; };
V vs(uint id : SV_VertexID) {
    V o; float2 t = float2((id << 1) & 2, id & 2);
    o.uv = t; o.p = float4(t * float2(2, -2) + float2(-1, 1), 0, 1); return o;
}
Texture2D tex : register(t0); SamplerState smp : register(s0); SamplerState lin : register(s1);
cbuffer Params : register(b0) { float sharpen; float3 pad; float2 texel; float2 pad2; float4 uvr; };   // texel = 1 / source size; uvr = uv scale.xy, offset.zw (one eye of a stereo texture)
float3 decode(float3 c) { return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }
// LumaSharpen (CeeJay.dk, ReShade / SweetFX, "normal" pattern) - from sharedtools\vrkit\lumasharpen.md (FF16 VR)
float3 LumaSharpen(float3 ori, float2 uv) {
    float2 d = texel * 0.5;
    float3 blur = tex.SampleLevel(lin, uv + float2(d.x, -d.y), 0).rgb + tex.SampleLevel(lin, uv - float2(d.x, -d.y), 0).rgb +
                  tex.SampleLevel(lin, uv + d, 0).rgb + tex.SampleLevel(lin, uv - d, 0).rgb;
    blur *= 0.25;
    const float sharpClamp = 0.035;
    float3 sharp = ori - blur;
    float3 strengthLuma = float3(0.2126, 0.7152, 0.0722) * sharpen * (0.5 / sharpClamp);
    float sharpLuma = saturate(dot(sharp, strengthLuma) + 0.5);
    sharpLuma = (sharpClamp * 2.0) * sharpLuma - sharpClamp;
    return ori + sharpLuma;
}
float3 src(V i) { float2 uv = i.uv * uvr.xy + uvr.zw; float3 c = saturate(tex.Sample(smp, uv).rgb); if (sharpen > 0) c = saturate(LumaSharpen(c, uv)); return c; }
float4 psDecode(V i) : SV_Target { return float4(decode(src(i)), 1); }
float4 psPass(V i) : SV_Target { return float4(src(i), 1); }
// Cinematic letterbox (user 2026-11-05: in-engine cutscenes put opaque black bars at the top / bottom of the UI
// target): a row that is opaque black across its whole width, in a band touching the top or bottom edge, is a
// letterbox bar -> transparent in VR. A fully black UI (fade to black) is kept: the middle row must not be black.
bool LetterboxRow(float y) {
    [unroll] for (int k = 0; k < 9; k++) {
        float4 s = tex.SampleLevel(smp, float2((k + 0.5) / 9.0, y) * uvr.xy + uvr.zw, 0);
        if (s.a < 0.97 || max(s.r, max(s.g, s.b)) > 0.02) return false;
    }
    return true;
}
float4 psHud(V i) : SV_Target {
    float4 c = saturate(tex.Sample(smp, i.uv * uvr.xy + uvr.zw));
    if (c.a > 0.97 && max(c.r, max(c.g, c.b)) <= 0.02 && LetterboxRow(i.uv.y) && !LetterboxRow(0.5) &&
        LetterboxRow(i.uv.y < 0.5 ? 0.002 : 0.998)) return float4(0, 0, 0, 0);
    return float4(decode(c.rgb), c.a);
}
)";
ID3DBlob* Compile(PFN_Compile c, const char* entry, const char* target) {
    ID3DBlob *b = nullptr, *err = nullptr;
    HRESULT hr = c(kHlsl, strlen(kHlsl), "davgvr_blit", nullptr, nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &b, &err);
    if (FAILED(hr)) LOGE("xr: blit compile %s failed: %s", entry, err ? (const char*)err->GetBufferPointer() : "?");
    if (err) err->Release();
    return b;
}
bool CreateBlit() {
    HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = dc ? (PFN_Compile)GetProcAddress(dc, "D3DCompile") : nullptr;
    if (!compile) { LOGE("xr: d3dcompiler_47.dll not available"); return false; }
    ID3DBlob* vs = Compile(compile, "vs", "vs_5_0");
    ID3DBlob* p1 = Compile(compile, "psDecode", "ps_5_0");
    ID3DBlob* p2 = Compile(compile, "psPass", "ps_5_0");
    ID3DBlob* p3 = Compile(compile, "psHud", "ps_5_0");
    if (!vs || !p1 || !p2 || !p3) return false;
    g_dev->CreatePixelShader(p3->GetBufferPointer(), p3->GetBufferSize(), nullptr, &g_psHud); p3->Release();
    g_dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g_vs);
    g_dev->CreatePixelShader(p1->GetBufferPointer(), p1->GetBufferSize(), nullptr, &g_psDecode);
    g_dev->CreatePixelShader(p2->GetBufferPointer(), p2->GetBufferSize(), nullptr, &g_psPass);
    vs->Release(); p1->Release(); p2->Release();
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;   // same size: 1:1
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    g_dev->CreateSamplerState(&sd, &g_samp);
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    g_dev->CreateSamplerState(&sd, &g_sampLin);
    D3D11_BUFFER_DESC bd{}; bd.ByteWidth = 48; bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    g_dev->CreateBuffer(&bd, nullptr, &g_blitCb);
    D3D11_RASTERIZER_DESC rd{}; rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE;
    g_dev->CreateRasterizerState(&rd, &g_rs);
    return g_vs && g_psDecode && g_psPass && g_samp && g_rs;
}

// Our own D3D11 device on the GPU the runtime asks for (normally the game's), plus the shared fences.
bool CreateXrDevice(const LUID& luidIn, D3D_FEATURE_LEVEL minLevel) {
    if (g_dev) return true;
    LUID luid = luidIn;
    if (luid.LowPart == 0 && luid.HighPart == 0) {
        luid = g_gameLuid;
        LOGI("xr: runtime reported no specific GPU; using the game's GPU (luid %08lx:%08lx)", luid.HighPart, luid.LowPart);
    }
    wchar_t sys[MAX_PATH]; GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring d3d11Path = std::wstring(sys) + L"\\d3d11.dll", dxgiPath = std::wstring(sys) + L"\\dxgi.dll";
    using PFN_CreateFactory1 = HRESULT(WINAPI*)(REFIID, void**);
    auto pCreateFactory1 = (PFN_CreateFactory1)GetProcAddress(LoadLibraryW(dxgiPath.c_str()), "CreateDXGIFactory1");
    auto pCreateDevice = (PFN_D3D11_CREATE_DEVICE)GetProcAddress(LoadLibraryW(d3d11Path.c_str()), "D3D11CreateDevice");
    if (!pCreateFactory1 || !pCreateDevice) { LOGE("xr: could not resolve DXGI/D3D11 entry points"); return false; }
    IDXGIFactory1* factory = nullptr;
    if (FAILED(pCreateFactory1(__uuidof(IDXGIFactory1), (void**)&factory))) { LOGE("xr: CreateDXGIFactory1 failed"); return false; }
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 d{}; adapter->GetDesc1(&d);
        if (memcmp(&d.AdapterLuid, &luid, sizeof(LUID)) == 0) { LOGI("xr: GPU %S", d.Description); break; }
        adapter->Release(); adapter = nullptr;
    }
    factory->Release();
    if (!adapter) { LOGE("xr: headset GPU (luid %08lx:%08lx) not found", luid.HighPart, luid.LowPart); return false; }
    if (memcmp(&luid, &g_gameLuid, sizeof(LUID)) != 0) LOGW("xr: the headset uses a different GPU than the game; frame sharing will fail");
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL got{};
    HRESULT hr = pCreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &g_dev, &got, &g_ctx);
    adapter->Release();
    if (FAILED(hr)) { LOGE("xr: could not create the XR D3D11 device (0x%08lx)", hr); return false; }
    if (got < minLevel) LOGW("xr: device feature level %x below runtime minimum %x", got, minLevel);
    if (FAILED(g_dev->QueryInterface(__uuidof(ID3D11Device5), (void**)&g_dev5)) ||
        FAILED(g_ctx->QueryInterface(__uuidof(ID3D11DeviceContext4), (void**)&g_ctx4))) {
        LOGE("xr: ID3D11Device5 / ID3D11DeviceContext4 not available (needs Windows 10 1703+)"); return false;
    }
    if (FAILED(g_dev5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, __uuidof(ID3D11Fence), (void**)&g_frameFence11)) ||
        FAILED(g_dev5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, __uuidof(ID3D11Fence), (void**)&g_releaseFence11)) ||
        FAILED(g_frameFence11->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &g_frameFenceHandle)) ||
        FAILED(g_releaseFence11->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &g_releaseFenceHandle))) {
        LOGE("xr: shared fence creation failed"); return false;
    }
    if (!CreateBlit()) { LOGE("xr: blit pipeline failed"); return false; }
    LOGI("xr: D3D11 device created (feature level %x), shared fences ready", got);
    g_xrReady = true;
    return true;
}

bool CreateSession() {
    XrGraphicsRequirementsD3D11KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    XR_CHECK(p_xrGetD3D11GraphicsRequirementsKHR(g_instance, g_system, &req));
    if (!CreateXrDevice(req.adapterLuid, req.minFeatureLevel)) return false;
    XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    binding.device = g_dev;
    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &binding;
    sci.systemId = g_system;
    XR_CHECK(p_xrCreateSession(g_instance, &sci, &g_session));
    XrReferenceSpaceCreateInfo rs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rs.poseInReferenceSpace.orientation.w = 1.0f;
    XR_CHECK(p_xrCreateReferenceSpace(g_session, &rs, &g_space));
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    if (XR_FAILED(p_xrCreateReferenceSpace(g_session, &rs, &g_viewSpace))) { LOGW("xr: no VIEW space; head-anchored HUD unavailable"); g_viewSpace = XR_NULL_HANDLE; }
    if (g_gazeSupported && g_viewSpace) SetupGaze();
    uint32_t n = 0;
    p_xrEnumerateSwapchainFormats(g_session, 0, &n, nullptr);
    std::vector<int64_t> f(n);
    p_xrEnumerateSwapchainFormats(g_session, n, &n, f.data());
    char buf[512]; int o = 0;
    for (uint32_t i = 0; i < n && o < (int)sizeof(buf) - 8; ++i) o += snprintf(buf + o, sizeof(buf) - o, "%lld ", (long long)f[i]);
    LOGI("xr: session created; swapchain formats offered: %s", n ? buf : "(none)");
    return true;
}

void DestroySwapchain(XrSc& s) {
    for (auto* r : s.rtvs) if (r) r->Release();
    s.rtvs.clear();
    if (s.sc) p_xrDestroySwapchain(s.sc);
    s.sc = XR_NULL_HANDLE;
    s.images.clear();
    s.w = s.h = 0;
    s.hasImage = false;
}

DXGI_FORMAT PickSwapchainFormat() {
    uint32_t n = 0;
    p_xrEnumerateSwapchainFormats(g_session, 0, &n, nullptr);
    std::vector<int64_t> fmts(n);
    p_xrEnumerateSwapchainFormats(g_session, n, &n, fmts.data());
    auto has = [&](DXGI_FORMAT f) { for (auto x : fmts) if (x == f) return true; return false; };
    for (DXGI_FORMAT f : {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB}) if (has(f)) return f;
    for (DXGI_FORMAT f : {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM})
        if (has(f)) { LOGW("xr: runtime offers no 8-bit sRGB format; using %d (colours may look washed out)", (int)f); return f; }
    LOGE("xr: no usable swapchain format");
    return DXGI_FORMAT_UNKNOWN;
}
bool IsSrgb(DXGI_FORMAT f) { return f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB; }

bool CreateSwapchain(XrSc& s, UINT w, UINT h, const char* what) {
    DestroySwapchain(s);
    DXGI_FORMAT fmt = PickSwapchainFormat();
    if (fmt == DXGI_FORMAT_UNKNOWN) return false;
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT;
    ci.format = fmt; ci.sampleCount = 1; ci.width = w; ci.height = h; ci.faceCount = 1; ci.arraySize = 1; ci.mipCount = 1;
    XR_CHECK(p_xrCreateSwapchain(g_session, &ci, &s.sc));
    uint32_t n = 0;
    XR_CHECK(p_xrEnumerateSwapchainImages(s.sc, 0, &n, nullptr));
    s.images.assign(n, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    XR_CHECK(p_xrEnumerateSwapchainImages(s.sc, n, &n, (XrSwapchainImageBaseHeader*)s.images.data()));
    s.rtvs.assign(n, nullptr);
    for (uint32_t i = 0; i < n; ++i) {
        D3D11_RENDER_TARGET_VIEW_DESC rd{}; rd.Format = fmt; rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        if (FAILED(g_dev->CreateRenderTargetView(s.images[i].texture, &rd, &s.rtvs[i]))) { LOGE("xr: swapchain RTV failed"); return false; }
    }
    s.w = w; s.h = h; s.fmt = fmt;
    LOGI("xr: %s swapchain %ux%u format %d (%u images)", what, w, h, (int)fmt, n);
    return true;
}

// Menu swapchain: R8G8B8A8 (sRGB preferred) so the menu's R8G8B8A8_TYPELESS texture can be copied with CopyResource.
bool CreateMenuSwapchain(UINT w, UINT h) {
    DestroySwapchain(g_menuSc);
    uint32_t n = 0;
    p_xrEnumerateSwapchainFormats(g_session, 0, &n, nullptr);
    std::vector<int64_t> fmts(n);
    p_xrEnumerateSwapchainFormats(g_session, n, &n, fmts.data());
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    for (DXGI_FORMAT f : {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM})
        for (auto x : fmts) if (fmt == DXGI_FORMAT_UNKNOWN && x == f) fmt = f;
    if (fmt == DXGI_FORMAT_UNKNOWN) { LOGE("xr: no R8G8B8A8 swapchain format for the menu"); return false; }
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    ci.format = fmt; ci.sampleCount = 1; ci.width = w; ci.height = h; ci.faceCount = 1; ci.arraySize = 1; ci.mipCount = 1;
    XR_CHECK(p_xrCreateSwapchain(g_session, &ci, &g_menuSc.sc));
    p_xrEnumerateSwapchainImages(g_menuSc.sc, 0, &n, nullptr);
    g_menuSc.images.assign(n, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    XR_CHECK(p_xrEnumerateSwapchainImages(g_menuSc.sc, n, &n, (XrSwapchainImageBaseHeader*)g_menuSc.images.data()));
    g_menuSc.w = w; g_menuSc.h = h; g_menuSc.fmt = fmt;
    LOGI("xr: menu swapchain %ux%u format %d (%u images)", w, h, (int)fmt, n);
    return true;
}

// Settings menu: init once, then every headset frame; copies the menu texture into its swapchain while open.
bool UpdateMenu(bool shouldRender) {
    static bool inited = false;
    if (!inited) { inited = true; MenuInit(g_dev, g_ctx); }
    if (!MenuFrame() || !shouldRender) return false;
    ID3D11Texture2D* tex = MenuTexture();
    if (!tex) return false;
    D3D11_TEXTURE2D_DESC td{}; tex->GetDesc(&td);
    if ((!g_menuSc.sc || g_menuSc.w != td.Width || g_menuSc.h != td.Height) && !CreateMenuSwapchain(td.Width, td.Height)) return false;
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO}; wi.timeout = 100000000;
    if (XR_FAILED(p_xrAcquireSwapchainImage(g_menuSc.sc, &ai, &idx))) return false;
    bool ok = XR_SUCCEEDED(p_xrWaitSwapchainImage(g_menuSc.sc, &wi));
    if (ok) g_ctx->CopyResource(g_menuSc.images[idx].texture, tex);
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    p_xrReleaseSwapchainImage(g_menuSc.sc, &ri);
    if (ok) g_menuSc.hasImage = true;
    return ok && g_menuSc.hasImage;
}

void TearDownSession() {
    DestroySwapchain(g_sc[0]);
    DestroySwapchain(g_sc[1]);
    DestroySwapchain(g_sc[2]);
    DestroySwapchain(g_scR);
    DestroySwapchain(g_menuSc);
    std::lock_guard<std::mutex> lk(g_sessionMutex);
    g_running = false;
    if (g_space) p_xrDestroySpace(g_space);
    if (g_viewSpace) p_xrDestroySpace(g_viewSpace);
    g_viewSpace = XR_NULL_HANDLE;
    if (g_session) p_xrDestroySession(g_session);
    g_space = XR_NULL_HANDLE;
    g_session = XR_NULL_HANDLE;
}

bool HandleEvents() {
    XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
    while (p_xrPollEvent(g_instance, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            auto* e = reinterpret_cast<XrEventDataSessionStateChanged*>(&ev);
            LOGI("xr: session state -> %d", (int)e->state);
            if (e->state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                XrResult r = p_xrBeginSession(g_session, &bi);
                g_running = XR_SUCCEEDED(r);
                g_recenterReq = true;   // reference = where the head is when the session starts
                LOGI("xr: xrBeginSession: %s", XrStr(r));
            } else if (e->state == XR_SESSION_STATE_STOPPING) {
                g_running = false;
                p_xrEndSession(g_session);
                LOGI("xr: session ended (runtime stopped it)");
            } else if (e->state == XR_SESSION_STATE_EXITING || e->state == XR_SESSION_STATE_LOSS_PENDING) {
                LOGW("xr: session lost; will retry");
                return false;
            }
        } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            LOGE("xr: instance loss pending");
            return false;
        }
        ev = {XR_TYPE_EVENT_DATA_BUFFER};
    }
    return true;
}

// =====================================================================================
// XR thread: frames
// =====================================================================================
// Raw eye views in LOCAL space (caller holds g_sessionMutex or is the XR thread). Caches the FOVs.
bool LocateLocal(XrTime t, XrView v[2], XrViewStateFlags* flags) {
    if (!g_session || !g_space || !p_xrLocateViews) return false;
    XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
    li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    li.displayTime = t; li.space = g_space;
    XrViewState vs{XR_TYPE_VIEW_STATE};
    v[0] = {XR_TYPE_VIEW}; v[1] = {XR_TYPE_VIEW};
    uint32_t n = 0;
    if (XR_FAILED(p_xrLocateViews(g_session, &li, &vs, 2, &n, v)) || n != 2) return false;
    if (flags) *flags = vs.viewStateFlags;
    if (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) {
        std::lock_guard<std::mutex> l(g_fovMutex);
        g_fov[0] = v[0].fov; g_fov[1] = v[1].fov; g_fovValid = true;
    }
    return true;
}

void DoRecenter(XrTime t) {
    XrView v[2]; XrViewStateFlags fl = 0;
    if (!LocateLocal(t, v, &fl) || !(fl & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) { LOGW("xr: recentre: no head pose"); return; }
    const XrQuaternionf& q = v[0].pose.orientation;
    float fx = -2.0f * (q.x * q.z + q.w * q.y);
    float fz = -(1.0f - 2.0f * (q.x * q.x + q.y * q.y));
    float yaw = std::atan2(-fx, -fz);
    XrVector3f pos{(v[0].pose.position.x + v[1].pose.position.x) * 0.5f, (v[0].pose.position.y + v[1].pose.position.y) * 0.5f,
                   (v[0].pose.position.z + v[1].pose.position.z) * 0.5f};
    { std::lock_guard<std::mutex> l(g_refMutex); g_refYaw = yaw; g_refPos = pos; }
    LOGI("xr: recentred (yaw %.1f deg, head %.2f %.2f %.2f)", yaw * 57.29578f, pos.x, pos.y, pos.z);
}

void ScreenPose(XrPosef& pose) {
    float yaw; XrVector3f ref; GetRef(yaw, ref);
    XrPosef p; p.orientation = {0, 0, 0, 1}; p.position = {0.0f, c_screenHeight.load(), -c_screenDistance.load()};
    pose = FromRef(p, yaw, ref);
}

void WriteBmp(const std::wstring& path, UINT w, UINT h, const std::vector<uint8_t>& bgra) {
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return;
    BITMAPFILEHEADER fh{}; BITMAPINFOHEADER ih{};
    fh.bfType = 0x4D42; fh.bfOffBits = sizeof(fh) + sizeof(ih); fh.bfSize = fh.bfOffBits + w * h * 4;
    ih.biSize = sizeof(ih); ih.biWidth = (LONG)w; ih.biHeight = -(LONG)h; ih.biPlanes = 1; ih.biBitCount = 32; ih.biCompression = BI_RGB;
    fwrite(&fh, sizeof(fh), 1, f); fwrite(&ih, sizeof(ih), 1, f); fwrite(bgra.data(), 1, bgra.size(), f);
    fclose(f);
}

// Dumps a swapchain image (as submitted: display-encoded values for sRGB formats). Stereo images hold the two
// eyes side by side; the log then gives the mean level of each half.
bool g_dumpSbs = false;   // the dump texture already holds the eyes side by side (per-eye swapchains)
void DumpImage(ID3D11Texture2D* src, DXGI_FORMAT scFmt, int kind) {
    D3D11_TEXTURE2D_DESC d{}; src->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
    d.MipLevels = 1; d.ArraySize = 1;
    ID3D11Texture2D* st = nullptr;
    if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &st))) { LOGE("xr: dump: staging texture failed (fmt %d)", (int)d.Format); return; }
    g_ctx->CopySubresourceRegion(st, 0, 0, 0, 0, src, 0, nullptr);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(g_ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) { st->Release(); LOGE("xr: dump: map failed"); return; }
    std::vector<uint8_t> out((size_t)d.Width * d.Height * 4);
    double sum[2] = {0, 0};
    bool bgr = scFmt == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || scFmt == DXGI_FORMAT_B8G8R8A8_UNORM;
    bool ten = scFmt == DXGI_FORMAT_R10G10B10A2_UNORM;
    UINT half = d.Width / 2;
    const bool vdump = kind == kStereo && g_set[1].vertical && !g_dumpSbs;   // stacked eyes: written side by side
    for (UINT y = 0; y < d.Height; ++y) {
        const uint8_t* row = (const uint8_t*)m.pData + (size_t)y * m.RowPitch;
        for (UINT x = 0; x < d.Width; ++x) {
            uint8_t r, g, b;
            if (ten) { uint32_t v = ((const uint32_t*)row)[x]; r = uint8_t((v & 1023) >> 2); g = uint8_t(((v >> 10) & 1023) >> 2); b = uint8_t(((v >> 20) & 1023) >> 2); }
            else if (bgr) { b = row[x * 4]; g = row[x * 4 + 1]; r = row[x * 4 + 2]; }
            else { r = row[x * 4]; g = row[x * 4 + 1]; b = row[x * 4 + 2]; }
            uint8_t* o = &out[((size_t)y * d.Width + x) * 4];
            if (kind < 0) {   // HUD: premultiplied colour over mid grey; sum = coverage
                uint8_t al = row[x * 4 + 3];
                int bg = (255 - al) / 2;
                r = (uint8_t)std::min(255, r + bg); g = (uint8_t)std::min(255, g + bg); b = (uint8_t)std::min(255, b + bg);
                sum[1] += al;
            }
            o[0] = b; o[1] = g; o[2] = r; o[3] = 255;
            if (kind >= 0) sum[(vdump ? y >= d.Height / 2 : x >= half) ? 1 : 0] += (r + g + b) / 3.0;
        }
    }
    g_ctx->Unmap(st, 0); st->Release();
    wchar_t name[64];
    if (kind == -2) swprintf_s(name, L"menu_%03d.bmp", g_dumpIndex - 1);
    else if (kind < 0) swprintf_s(name, L"hud_%03d.bmp", g_dumpIndex - 1);
    else swprintf_s(name, L"screen_%03d.bmp", g_dumpIndex++);
    std::wstring path = G().outDir + name;
    if (vdump) {
        UINT ew = d.Width, eh = d.Height / 2;
        std::vector<uint8_t> sbs((size_t)ew * 2 * eh * 4);
        for (UINT y = 0; y < eh; ++y) {
            memcpy(&sbs[((size_t)y * ew * 2) * 4], &out[((size_t)y * ew) * 4], (size_t)ew * 4);
            memcpy(&sbs[((size_t)y * ew * 2 + ew) * 4], &out[((size_t)(y + eh) * ew) * 4], (size_t)ew * 4);
        }
        WriteBmp(path, ew * 2, eh, sbs);
    } else WriteBmp(path, d.Width, d.Height, out);
    double px = double(d.Width) * d.Height;
    const char* kn = kind == kStereo ? "stereo" : kind == kMono ? "mono" : kind == -2 ? "menu" : kind < 0 ? "hud" : "flat";
    if (kind < 0)
        LOGI("xr: dumped %S (%s %ux%u, swapchain fmt %d, mean coverage %.1f%%)", path.c_str(), kn, d.Width, d.Height, (int)scFmt, sum[1] / px / 2.55);
    else if (kind == kStereo)
        LOGI("xr: dumped %S (%s, %ux%u, swapchain fmt %d, mean level left %.1f right %.1f)", path.c_str(), kn, d.Width, d.Height,
             (int)scFmt, sum[0] / (px / 2), sum[1] / (px / 2));
    else
        LOGI("xr: dumped %S (%s, %ux%u, swapchain fmt %d, mean level %.1f)", path.c_str(), kn, d.Width, d.Height, (int)scFmt, (sum[0] + sum[1]) / px);
}

struct XrSide { uint32_t generation = 0; ID3D11ShaderResourceView* srv[kSlots] = {}; UINT w = 0, h = 0; DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN; bool vertical = false; };
XrSide g_xs[3];

bool SyncXrSide(int si) {
    std::lock_guard<std::mutex> lk(g_setMutex);
    SharedSet& set = g_set[si]; XrSide& xs = g_xs[si];
    if (!set.generation || !set.tex11[0]) return false;
    if (xs.generation == set.generation) return true;
    for (auto& s : xs.srv) SafeRelease(s);
    for (int i = 0; i < kSlots; ++i) {
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{}; sd.Format = set.fmt; sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels = 1;
        if (FAILED(g_dev->CreateShaderResourceView(set.tex11[i], &sd, &xs.srv[i]))) { LOGE("xr: SRV on shared texture failed"); xs.generation = 0; return false; }
    }
    xs.generation = set.generation; xs.w = set.w; xs.h = set.h; xs.fmt = set.fmt; xs.vertical = set.vertical;
    LOGI("xr: using %s shared textures %ux%u fmt %d (generation %u)", set.name, xs.w, xs.h, (int)xs.fmt, xs.generation);
    return true;
}

SlotMeta g_shown;            // meta of the image currently in the swapchain to submit (XR thread)
bool g_shownValid = false;

// Draws shared slot `slot` of side `si` into the next image of swapchain `sc` (full-screen triangle).
bool BlitToSwapchain(XrSc& sc, int si, int slot, ID3D11PixelShader* ps, ID3D11Texture2D** outTex, float sharpen = 0.0f, const float* uvr = nullptr) {
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO}; wi.timeout = 100000000;
    if (XR_FAILED(p_xrAcquireSwapchainImage(sc.sc, &ai, &idx))) return false;
    bool ok = false;
    if (XR_SUCCEEDED(p_xrWaitSwapchainImage(sc.sc, &wi))) {
        D3D11_VIEWPORT vp{0, 0, (float)sc.w, (float)sc.h, 0, 1};
        g_ctx->OMSetRenderTargets(1, &sc.rtvs[idx], nullptr);
        g_ctx->RSSetViewports(1, &vp);
        g_ctx->RSSetState(g_rs);
        g_ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        g_ctx->OMSetDepthStencilState(nullptr, 0);
        g_ctx->IASetInputLayout(nullptr);
        g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g_ctx->VSSetShader(g_vs, nullptr, 0);
        g_ctx->PSSetShader(ps, nullptr, 0);
        g_ctx->PSSetShaderResources(0, 1, &g_xs[si].srv[slot]);
        ID3D11SamplerState* samps[2] = {g_samp, g_sampLin};
        g_ctx->PSSetSamplers(0, 2, samps);
        if (g_blitCb) {
            D3D11_MAPPED_SUBRESOURCE m{};
            if (SUCCEEDED(g_ctx->Map(g_blitCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
                float* f = (float*)m.pData; memset(f, 0, 48);
                f[8] = uvr ? uvr[0] : 1.0f; f[9] = uvr ? uvr[1] : 1.0f; f[10] = uvr ? uvr[2] : 0.0f; f[11] = uvr ? uvr[3] : 0.0f;
                f[0] = ps == g_psHud ? 0.0f : sharpen;
                f[4] = g_xs[si].w ? 1.0f / g_xs[si].w : 0.0f; f[5] = g_xs[si].h ? 1.0f / g_xs[si].h : 0.0f;
                g_ctx->Unmap(g_blitCb, 0);
            }
            g_ctx->PSSetConstantBuffers(0, 1, &g_blitCb);
        }
        g_ctx->Draw(3, 0);
        ID3D11ShaderResourceView* nullSrv = nullptr; ID3D11RenderTargetView* nullRtv = nullptr;
        g_ctx->PSSetShaderResources(0, 1, &nullSrv);
        g_ctx->OMSetRenderTargets(1, &nullRtv, nullptr);
        if (outTex) *outTex = sc.images[idx].texture;
        ok = true;
    }
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    p_xrReleaseSwapchainImage(sc.sc, &ri);
    return ok;
}

// Copies the newest game frame (if any) into the swapchain of its kind (+ the HUD swapchain for stereo frames with a HUD).
void UpdateImage() {
    static uint64_t lastSeq = 0;
    int slot = g_latestSlot.load();
    if (slot < 0) return;
    g_readingSlot = slot;
    int again = g_latestSlot.load();
    if (again != slot) { slot = again; g_readingSlot = slot; if (slot < 0) return; }
    SlotMeta meta;
    { std::lock_guard<std::mutex> lk(g_metaMutex); meta = g_meta[slot]; }
    int si = SetOf(meta.kind);
    if (meta.seq == lastSeq || !SyncXrSide(si) || meta.generation != g_xs[si].generation) { g_readingSlot = -1; return; }
    XrSc& sc = g_sc[si];
    const bool perEye = si == 1;   // stereo: one swapchain per eye
    const bool vert = g_xs[si].vertical;
    const UINT sw = perEye ? (vert ? g_xs[si].w : g_xs[si].w / 2) : g_xs[si].w;
    const UINT sh = perEye ? (vert ? g_xs[si].h / 2 : g_xs[si].h) : g_xs[si].h;
    if (!sc.sc || sc.w != sw || sc.h != sh)
        if (!CreateSwapchain(sc, sw, sh, si ? "stereo left eye" : "screen")) { g_readingSlot = -1; return; }
    if (perEye && (!g_scR.sc || g_scR.w != sw || g_scR.h != sh))
        if (!CreateSwapchain(g_scR, sw, sh, "stereo right eye")) { g_readingSlot = -1; return; }
    bool hud = meta.hud && SyncXrSide(2) && meta.hudGeneration == g_xs[2].generation;
    if (hud && (!g_sc[2].sc || g_sc[2].w != g_xs[2].w || g_sc[2].h != g_xs[2].h))
        if (!CreateSwapchain(g_sc[2], g_xs[2].w, g_xs[2].h, "hud")) hud = false;
    g_ctx4->Wait(g_frameFence11, meta.fenceValue);   // GPU wait for the game's copy
    bool decode = IsSrgb(sc.fmt) && !IsLinearFloat(g_xs[si].fmt);
    ID3D11Texture2D* shownTex = nullptr; ID3D11Texture2D* hudTex = nullptr;
    ID3D11Texture2D* shownR = nullptr;
    bool ok;
    if (perEye) {
        const float uvL[4] = {vert ? 1.0f : 0.5f, vert ? 0.5f : 1.0f, 0.0f, 0.0f};
        const float uvR[4] = {vert ? 1.0f : 0.5f, vert ? 0.5f : 1.0f, vert ? 0.0f : 0.5f, vert ? 0.5f : 0.0f};
        ok = BlitToSwapchain(sc, si, slot, decode ? g_psDecode : g_psPass, &shownTex, c_sharpen.load(), uvL) &&
             BlitToSwapchain(g_scR, si, slot, decode ? g_psDecode : g_psPass, &shownR, c_sharpen.load(), uvR);
    } else ok = BlitToSwapchain(sc, si, slot, decode ? g_psDecode : g_psPass, &shownTex, c_sharpen.load());
    bool hudOk = hud && BlitToSwapchain(g_sc[2], 2, slot, IsSrgb(g_sc[2].fmt) ? g_psHud : g_psPass, &hudTex);
    g_ctx4->Signal(g_releaseFence11, ++g_releaseValue);
    g_slotRelease[slot].store(g_releaseValue);
    g_ctx->Flush();
    if (ok) {
        bool dump = g_dumpReq.exchange(false);
        int df = c_dumpFrame.load();
        if (df > 0 && s_newFrames.load() + 1 == (uint64_t)df) dump = true;
        if (dump && perEye && shownR) {   // both eye images side by side into one texture for the dump
            D3D11_TEXTURE2D_DESC td{}; shownTex->GetDesc(&td);
            td.Width = sc.w * 2; td.Height = sc.h; td.MipLevels = 1; td.ArraySize = 1; td.BindFlags = 0; td.MiscFlags = 0; td.Usage = D3D11_USAGE_DEFAULT; td.CPUAccessFlags = 0;
            ID3D11Texture2D* both = nullptr;
            if (SUCCEEDED(g_dev->CreateTexture2D(&td, nullptr, &both))) {
                D3D11_BOX b{0, 0, 0, sc.w, sc.h, 1};
                g_ctx->CopySubresourceRegion(both, 0, 0, 0, 0, shownTex, 0, &b);
                g_ctx->CopySubresourceRegion(both, 0, sc.w, 0, 0, shownR, 0, &b);
                g_dumpSbs = true; DumpImage(both, sc.fmt, meta.kind); g_dumpSbs = false;
                both->Release();
            }
            dump = false;
            { const XrQuaternionf& q0 = meta.eyes[0].pose.orientation; const XrVector3f& p0 = meta.eyes[0].pose.position;
              float yaw = std::atan2(2.0f * (q0.w * q0.y + q0.x * q0.z), 1.0f - 2.0f * (q0.y * q0.y + q0.x * q0.x)) * 57.29578f;
              LOGI("xr: dump pose: left eye q (%.5f %.5f %.5f %.5f) yaw %.3f deg pos (%.4f %.4f %.4f) fov L %.4f R %.4f", q0.x, q0.y, q0.z, q0.w, yaw,
                   p0.x, p0.y, p0.z, meta.eyes[0].fov.angleLeft, meta.eyes[0].fov.angleRight); }
            if (hudOk) DumpImage(hudTex, g_sc[2].fmt, -1);
        }
        if (dump) { DumpImage(shownTex, sc.fmt, meta.kind);
                    { const XrQuaternionf& q0 = meta.eyes[0].pose.orientation; const XrVector3f& p0 = meta.eyes[0].pose.position;
                      float yaw = std::atan2(2.0f * (q0.w * q0.y + q0.x * q0.z), 1.0f - 2.0f * (q0.y * q0.y + q0.x * q0.x)) * 57.29578f;
                      LOGI("xr: dump pose: left eye q (%.5f %.5f %.5f %.5f) yaw %.3f deg pos (%.4f %.4f %.4f) fov L %.4f R %.4f", q0.x, q0.y, q0.z, q0.w, yaw,
                           p0.x, p0.y, p0.z, meta.eyes[0].fov.angleLeft, meta.eyes[0].fov.angleRight); } if (hudOk) DumpImage(hudTex, g_sc[2].fmt, -1);
                    if (vrkit::menu::IsOpen() && MenuTexture()) DumpImage(MenuTexture(), DXGI_FORMAT_R8G8B8A8_UNORM, -2); }
        sc.hasImage = true;
        if (perEye) g_scR.hasImage = true;
        if (hudOk) g_sc[2].hasImage = true;
        g_shown = meta; g_shown.hud = hudOk || (meta.hud && g_sc[2].hasImage);
        g_shownValid = true;
        lastSeq = meta.seq;
        s_newFrames++;
    }
    g_readingSlot = -1;
}

void SubmitOneFrame() {
    XrFrameWaitInfo wi{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState fs{XR_TYPE_FRAME_STATE};
    LARGE_INTEGER q0, q1, qf; QueryPerformanceCounter(&q0);
    if (XR_FAILED(p_xrWaitFrame(g_session, &wi, &fs))) { Sleep(5); return; }
    QueryPerformanceCounter(&q1); QueryPerformanceFrequency(&qf);
    s_waitUs += uint64_t((q1.QuadPart - q0.QuadPart) * 1000000 / qf.QuadPart);
    g_lastDisplayTime.store(fs.predictedDisplayTime);
    UpdateGaze(fs.predictedDisplayTime);
    { LARGE_INTEGER q; QueryPerformanceCounter(&q); g_lastWaitQpc.store(q.QuadPart); }
    if (fs.predictedDisplayPeriod > 0) {
        // The predicted period jumps to 2x / 3x while SteamVR throttles (reprojection): 30 / 45 / 90 Hz thousands of times
        // per session on the user's Index-class HMD. The refresh rate = the shortest period seen; the limiter and the
        // pose prediction use that, not the throttled value.
        XrDuration cur = g_displayPeriod.load();
        if (cur == 0 || fs.predictedDisplayPeriod < cur - 200000) {
            g_displayPeriod = fs.predictedDisplayPeriod;
            LOGI("xr: headset refresh %.1f Hz", 1e9 / double(fs.predictedDisplayPeriod));
        }
        static XrDuration lastP = 0; static uint64_t throttled = 0, total = 0; static DWORD t0 = GetTickCount();
        total++; if (fs.predictedDisplayPeriod > g_displayPeriod.load() + 200000) throttled++;
        (void)lastP;
        if (GetTickCount() - t0 >= 10000) {
            if (throttled) LOGI("xr: SteamVR throttled %.0f%% of headset frames (predicted period > 1 refresh, last 10 s)", 100.0 * throttled / (double)total);
            throttled = total = 0; t0 = GetTickCount();
        }
    }
    XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO};
    if (XR_FAILED(p_xrBeginFrame(g_session, &bi))) return;
    s_xrFrames++;

    if (g_recenterReq.exchange(false)) DoRecenter(fs.predictedDisplayTime);
    XrView headViews[2]; XrViewStateFlags headFl = 0;
    bool headOk = LocateLocal(fs.predictedDisplayTime, headViews, &headFl) && (headFl & XR_VIEW_STATE_ORIENTATION_VALID_BIT);   // also keeps the cached FOVs current
    if (fs.shouldRender) UpdateImage();
    bool menuShown = UpdateMenu(fs.shouldRender);
    static vrkit::menu::QuadPlacer menuPlacer;
    XrPosef head{};
    if (headOk) {
        head.orientation = headViews[0].pose.orientation;
        head.position = {(headViews[0].pose.position.x + headViews[1].pose.position.x) * 0.5f, (headViews[0].pose.position.y + headViews[1].pose.position.y) * 0.5f,
                         (headViews[0].pose.position.z + headViews[1].pose.position.z) * 0.5f};
    }
    menuPlacer.Update(menuShown, headOk, head);

    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    XrCompositionLayerProjection proj{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerProjectionView pv[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    XrCompositionLayerQuad hudQuad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    XrCompositionLayerQuad menuQuad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    const XrCompositionLayerBaseHeader* layers[3] = {};
    uint32_t layerCount = 0;
    static int lastKind = -1;
    if (fs.shouldRender && g_shownValid) {
        int kind = g_shown.kind;
        XrSc& sc = g_sc[SetOf(kind)];
        if (sc.sc && sc.hasImage && (kind != kStereo || (g_scR.sc && g_scR.hasImage))) {
            if (kind == kFlat) {
                quad.space = g_space;
                quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                quad.subImage.swapchain = sc.sc;
                quad.subImage.imageRect = {{0, 0}, {(int32_t)sc.w, (int32_t)sc.h}};
                ScreenPose(quad.pose);
                float w = c_screenWidth.load();
                quad.size = {w, w * float(sc.h) / float(sc.w)};
                layers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
            } else {
                bool halves = kind == kSbs;   // kStereo: one swapchain per eye
                int32_t ew = halves ? (int32_t)sc.w / 2 : (int32_t)sc.w;
                for (int e = 0; e < 2; ++e) {
                    pv[e].pose = g_shown.eyes[e].pose;
                    pv[e].fov = g_shown.eyes[e].fov;
                    if (kind == kStereo) {
                        XrSc& es = e ? g_scR : sc;
                        pv[e].subImage.swapchain = es.sc;
                        pv[e].subImage.imageRect = {{0, 0}, {(int32_t)es.w, (int32_t)es.h}};
                    } else {
                        pv[e].subImage.swapchain = sc.sc;
                        pv[e].subImage.imageRect = {{halves ? e * ew : 0, 0}, {ew, (int32_t)sc.h}};
                    }
                }
                proj.space = g_space;
                proj.viewCount = 2;
                proj.views = pv;
                layers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&proj);
            }
            layerCount = 1;
            s_xrLayers++;
            if (kind == kStereo && g_shown.hud && g_sc[2].sc && g_sc[2].hasImage && c_hudVisible.load()) {
                bool head = c_hudAnchor.load() == 1 && g_viewSpace;
                hudQuad.space = head ? g_viewSpace : g_space;
                hudQuad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
                                     (c_hudPremul.load() ? 0 : XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT);
                hudQuad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                hudQuad.subImage.swapchain = g_sc[2].sc;
                hudQuad.subImage.imageRect = {{0, 0}, {(int32_t)g_sc[2].w, (int32_t)g_sc[2].h}};
                XrPosef p; p.orientation = {0, 0, 0, 1}; p.position = {0.0f, c_hudHeight.load(), -c_hudDistance.load()};
                if (head) hudQuad.pose = p;
                else { float yaw; XrVector3f ref; GetRef(yaw, ref); hudQuad.pose = FromRef(p, yaw, ref); }
                float w = c_hudWidth.load();
                hudQuad.size = {w, w * float(g_sc[2].h) / float(g_sc[2].w)};
                layers[1] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&hudQuad);
                layerCount = 2;
                static bool loggedHud = false;
                if (!loggedHud) { loggedHud = true; LOGI("xr: HUD layer on (%ux%u, %s anchored)", g_sc[2].w, g_sc[2].h, head ? "head" : "world"); }
            }
            if (kind != lastKind) {
                lastKind = kind;
                LOGI("xr: submitting %s", kind == kFlat ? "quad (flat screen)" : kind == kMono ? "projection (mono)" : kind == kSbs ? "projection (stereo, side by side in the back buffer)" : "projection (stereo)");
            }
        }
    }
    if (menuShown) {   // settings menu, on top of everything
        menuPlacer.Apply(MenuPlacement(), g_space, g_viewSpace, g_menuSc.w, g_menuSc.h, menuQuad);
        menuQuad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;   // premultiplied
        menuQuad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        menuQuad.subImage.swapchain = g_menuSc.sc;
        menuQuad.subImage.imageRect = {{0, 0}, {(int32_t)g_menuSc.w, (int32_t)g_menuSc.h}};
        layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&menuQuad);
    }
    XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
    ei.displayTime = fs.predictedDisplayTime;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ei.layerCount = layerCount;
    ei.layers = layers;
    XrResult r = p_xrEndFrame(g_session, &ei);
    if (XR_FAILED(r)) { static int failures = 0; if (failures++ < 5) LOGE("xr: xrEndFrame failed: %s", XrStr(r)); }
}

void PeriodicStats() {
    static DWORD t0 = GetTickCount();
    DWORD now = GetTickCount();
    if (now - t0 < 10000) return;
    double s = (now - t0) / 1000.0; t0 = now;
    uint64_t f = s_xrFrames.exchange(0), l = s_xrLayers.exchange(0), n = s_newFrames.exchange(0), w = s_waitUs.exchange(0);
    uint64_t p = s_published.exchange(0), sb = s_skipBusy.exchange(0), sa = s_skipAlloc.exchange(0), h = s_held.exchange(0), st = s_stereo.exchange(0);
    LOGI("xr stats: headset %.1f fps (%llu with a layer), new game frames %.1f/s, xrWaitFrame %.2f ms avg | game frames published %llu (%llu stereo), skipped %llu busy / %llu allocator, held %llu",
         f / s, (unsigned long long)l, n / s, f ? w / 1000.0 / f : 0.0, (unsigned long long)p, (unsigned long long)st,
         (unsigned long long)sb, (unsigned long long)sa, (unsigned long long)h);
}

DWORD WINAPI XrThreadMain(LPVOID) {
    SetThreadDescription(GetCurrentThread(), L"davgvr XR");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    if (!LoadLoader() || !CreateInstance()) { LOGE("xr: VR output disabled for this run"); return 0; }
    bool loggedWaiting = false, f9 = false;
    for (;;) {
        bool k = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
        if (k && !f9) { LOGI("F9: recentre"); g_recenterReq = true; }
        f9 = k;
        static bool f10 = false;
        bool k10 = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
        if (k10 && !f10) { DWORD pid = 0; GetWindowThreadProcessId(GetForegroundWindow(), &pid);
            if (pid == GetCurrentProcessId()) { c_hudVisible = !c_hudVisible.load(); LOGI("F10: HUD %s", c_hudVisible.load() ? "shown" : "hidden"); } }
        f10 = k10;
        if (g_system == XR_NULL_SYSTEM_ID) {
            int r = TryGetSystem();
            if (r < 0) { LOGE("xr: VR output disabled for this run"); return 0; }
            if (r == 0) {
                if (!loggedWaiting) { LOGI("xr: no headset available yet; retrying every 2 s"); loggedWaiting = true; }
                Sleep(2000);
                continue;
            }
        }
        if (!g_session && !CreateSession()) { LOGE("xr: session creation failed; retrying in 5 s"); Sleep(5000); continue; }
        if (!HandleEvents()) { TearDownSession(); g_system = XR_NULL_SYSTEM_ID; Sleep(2000); continue; }
        PeriodicStats();
        if (!g_running) { Sleep(10); continue; }
        if (c_pacing.load() == 1 && g_newFrameEvent) {
            // game-paced: wait (up to 100 ms, so the session and the menu stay alive without game frames) for a new frame
            WaitForSingleObject(g_newFrameEvent, vrkit::menu::IsOpen() ? 0 : 100);
        }
        SubmitOneFrame();
    }
}

// =====================================================================================
// Render thread: producer
// =====================================================================================
bool StartXr(IDXGISwapChain* sc) {
    ID3D12Device* dev = nullptr;
    if (FAILED(sc->GetDevice(__uuidof(ID3D12Device), (void**)&dev)) || !dev) { LOGE("xr: swapchain has no D3D12 device"); return false; }
    g_gameLuid = dev->GetAdapterLuid();
    dev->Release();
    HANDLE t = CreateThread(nullptr, 0, XrThreadMain, nullptr, 0, nullptr);
    if (!t) { LOGE("xr: could not start the XR thread"); return false; }
    CloseHandle(t);
    LOGI("xr: XR thread started (game GPU luid %08lx:%08lx)", g_gameLuid.HighPart, g_gameLuid.LowPart);
    return true;
}

// Opens the shared fences and creates the per-slot command lists on the game's device (once).
bool EnsureProducer(ID3D12CommandQueue* queue) {
    if (g_gameDev) return true;
    ID3D12Device* dev = nullptr;
    if (FAILED(queue->GetDevice(__uuidof(ID3D12Device), (void**)&dev)) || !dev) { LOGE("xr: queue has no device"); return false; }
    HRESULT hr = dev->OpenSharedHandle(g_frameFenceHandle, __uuidof(ID3D12Fence), (void**)&g_frameFence12);
    if (SUCCEEDED(hr)) hr = dev->OpenSharedHandle(g_releaseFenceHandle, __uuidof(ID3D12Fence), (void**)&g_releaseFence12);
    if (SUCCEEDED(hr)) hr = dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void**)&g_copyFence);
    for (int i = 0; i < kSlots && SUCCEEDED(hr); ++i) {
        hr = dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void**)&g_alloc[i]);
        if (SUCCEEDED(hr)) hr = dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc[i], nullptr, __uuidof(ID3D12GraphicsCommandList), (void**)&g_list[i]);
        if (SUCCEEDED(hr)) { g_list[i]->Close(); wchar_t nm[40]; swprintf_s(nm, L"davgvr xr copy %d", i); g_list[i]->SetName(nm); }
    }
    if (FAILED(hr)) { LOGE("xr: producer setup failed (0x%08lx)", hr); dev->Release(); return false; }
    g_gameDev = dev;   // keep the reference
    D3D12_COMMAND_QUEUE_DESC qd = queue->GetDesc();
    LOGI("xr: producer ready on game device %p, queue %p (type %d)", dev, queue, (int)qd.Type);
    return true;
}

// (Re)creates a shared ring for the given size/format. Never more than once per 0.5 s per set.
bool EnsureSharedSet(int si, UINT w, UINT h, DXGI_FORMAT fmt, bool vertical = false) {
    SharedSet& set = g_set[si];
    if (set.w == w && set.h == h && set.fmt == fmt && set.vertical == vertical && set.tex12[0]) return true;
    if (set.tex12[0] && GetTickCount64() - set.lastRealloc < 500) {
        static uint64_t refused = 0;
        ++refused;
        if ((refused & (refused - 1)) == 0) LOGW("xr: %s shared texture size change to %ux%u refused (too soon; %llu refusals)", set.name, w, h, (unsigned long long)refused);
        return false;
    }
    // The old textures may still be in use by our copies on the game's queue: wait (bounded) for them.
    if (g_copyFence && g_copyFence->GetCompletedValue() < g_copyValue) {
        HANDLE e = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        g_copyFence->SetEventOnCompletion(g_copyValue, e);
        if (WaitForSingleObject(e, 500) != WAIT_OBJECT_0) LOGW("xr: old shared textures still busy after 500 ms");
        CloseHandle(e);
    }
    for (int i = 0; i < 200 && g_readingSlot.load() >= 0; ++i) Sleep(1);
    std::lock_guard<std::mutex> lk(g_setMutex);
    set.lastRealloc = GetTickCount64();
    {   // slots of this set must not be shown any more
        std::lock_guard<std::mutex> ml(g_metaMutex);
        int l = g_latestSlot.load();
        if (l >= 0 && (si == 2 ? g_meta[l].hud : SetOf(g_meta[l].kind) == si)) g_latestSlot = -1;
    }
    for (int i = 0; i < kSlots; ++i) { SafeRelease(set.tex12[i]); SafeRelease(set.tex11[i]); }   // XR-side SRVs keep their own refs
    set.w = set.h = 0;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1; td.Format = fmt;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    for (int i = 0; i < kSlots; ++i) {
        HRESULT hr = g_dev->CreateTexture2D(&td, nullptr, &set.tex11[i]);   // ID3D11Device is free-threaded
        if (FAILED(hr)) { LOGE("xr: shared texture creation failed (0x%08lx, %ux%u fmt %d)", hr, w, h, (int)fmt); return false; }
        HANDLE hnd = nullptr;
        IDXGIResource1* res = nullptr;
        if (SUCCEEDED(set.tex11[i]->QueryInterface(__uuidof(IDXGIResource1), (void**)&res))) {
            hr = res->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &hnd);
            res->Release();
        }
        if (FAILED(hr) || !hnd) { LOGE("xr: shared handle creation failed (0x%08lx)", hr); return false; }
        hr = g_gameDev->OpenSharedHandle(hnd, __uuidof(ID3D12Resource), (void**)&set.tex12[i]);
        CloseHandle(hnd);
        if (FAILED(hr)) { LOGE("xr: OpenSharedHandle (texture) failed (0x%08lx)", hr); return false; }
    }
    set.w = w; set.h = h; set.fmt = fmt; set.vertical = vertical;
    set.generation++;
    LOGI("xr: %s shared textures %ux%u format %d (generation %u)", set.name, w, h, (int)fmt, set.generation);
    return true;
}

void Barrier(ID3D12GraphicsCommandList* l, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    if (a == b) return;
    D3D12_RESOURCE_BARRIER br{};
    br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    br.Transition.pResource = r; br.Transition.StateBefore = a; br.Transition.StateAfter = b;
    br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    l->ResourceBarrier(1, &br);
}

// Picks a free slot with a free command allocator; returns -1 (and counts the skip) if none.
int PickSlot() {
    int latest = g_latestSlot.load(), reading = g_readingSlot.load();
    int slot = -1;
    for (int i = 1; i <= kSlots; ++i) {
        int c = ((latest < 0 ? 0 : latest) + i) % kSlots;
        if (c != reading && c != latest) { slot = c; break; }
    }
    if (slot < 0) { s_skipBusy++; return -1; }
    if (g_copyFence->GetCompletedValue() < g_slotCopyDone[slot]) { s_skipAlloc++; return -1; }
    return slot;
}

// Records the copies into the slot's texture of set `si` and submits them on `queue`.
// srcs[i] is copied to x offset dstX[i]; states[i] is the source's state before and after.
bool SubmitCopies(ID3D12CommandQueue* queue, int slot, int si, int n, ID3D12Resource* const* srcs, const UINT* dstX,
                  const D3D12_RESOURCE_STATES* states, ID3D12Resource* hudSrc = nullptr, D3D12_RESOURCE_STATES hudState = D3D12_RESOURCE_STATE_COMMON,
                  const UINT* dstY = nullptr) {
    ID3D12GraphicsCommandList* l = g_list[slot];
    g_alloc[slot]->Reset();
    l->Reset(g_alloc[slot], nullptr);
    ID3D12Resource* dst = g_set[si].tex12[slot];
    for (int i = 0; i < n; ++i) Barrier(l, srcs[i], states[i], D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(l, dst, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    for (int i = 0; i < n; ++i) {
        if (n == 1 && dstX[0] == 0 && (!dstY || dstY[0] == 0)) { l->CopyResource(dst, srcs[0]); break; }
        D3D12_TEXTURE_COPY_LOCATION d{}, s{};
        d.pResource = dst; d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; d.SubresourceIndex = 0;
        s.pResource = srcs[i]; s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; s.SubresourceIndex = 0;
        l->CopyTextureRegion(&d, dstX[i], dstY ? dstY[i] : 0, 0, &s, nullptr);
    }
    Barrier(l, dst, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    for (int i = 0; i < n; ++i) Barrier(l, srcs[i], D3D12_RESOURCE_STATE_COPY_SOURCE, states[i]);
    if (hudSrc) {
        ID3D12Resource* hd = g_set[2].tex12[slot];
        Barrier(l, hudSrc, hudState, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Barrier(l, hd, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        l->CopyResource(hd, hudSrc);
        Barrier(l, hd, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
        Barrier(l, hudSrc, D3D12_RESOURCE_STATE_COPY_SOURCE, hudState);
    }
    if (FAILED(l->Close())) { LOGE("xr: copy list Close failed"); g_producerFailed = true; return false; }

    // Work submitted on different queues: order it after everything we submitted before.
    if (g_lastQueue && g_lastQueue != queue) queue->Wait(g_copyFence, g_copyValue);
    g_lastQueue = queue;
    queue->Wait(g_releaseFence12, g_slotRelease[slot].load());   // XR thread done reading this slot
    ID3D12CommandList* lists[] = {l};
    queue->ExecuteCommandLists(1, lists);
    queue->Signal(g_frameFence12, ++g_frameValue);
    queue->Signal(g_copyFence, ++g_copyValue);
    g_slotCopyDone[slot] = g_copyValue;
    return true;
}

void Publish(int slot, int kind, const XrHostEye* eyes, int si, bool hud = false) {
    {
        std::lock_guard<std::mutex> lk(g_metaMutex);
        SlotMeta& m = g_meta[slot];
        m.fenceValue = g_frameValue;
        m.seq = ++g_seq;
        m.generation = g_set[si].generation;
        m.kind = kind;
        if (eyes) { m.eyes[0] = eyes[0]; m.eyes[1] = eyes[1]; }
        m.hud = hud; m.hudGeneration = hud ? g_set[2].generation : 0;
    }
    g_latestSlot = slot;
    if (g_newFrameEvent) SetEvent(g_newFrameEvent);
    s_published++;
}

bool ProducerReady(ID3D12CommandQueue* queue) {
    if (!c_enabled.load() || g_producerFailed || !g_xrReady.load() || !g_running.load() || !queue) return false;
    if (!EnsureProducer(queue)) { g_producerFailed = true; return false; }
    D3D12_COMMAND_QUEUE_DESC qd = queue->GetDesc();
    if (qd.Type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
        static bool warned = false;
        if (!warned) { warned = true; LOGW("xr: queue %p is type %d, not DIRECT; frames not sent", queue, (int)qd.Type); }
        return false;
    }
    return true;
}

} // namespace

// =====================================================================================
// Public API
// =====================================================================================
void XrHostApplyConfig() {
    Config& c = G().cfg;
    c_enabled = c.GetBool("vr_enabled", true);
    c_screenDistance = c.GetFloat("screen_distance", 2.5f);
    c_screenWidth = c.GetFloat("screen_width", 3.2f);
    c_screenHeight = c.GetFloat("screen_height", 0.0f);
    c_dumpFrame = c.GetInt("dump_frame", 0);
    g_set[0].name = "screen"; g_set[1].name = "stereo"; g_set[2].name = "hud";
    c_hudDistance = c.GetFloat("hud_distance", 1.5f);
    c_hudWidth = c.GetFloat("hud_width", 1.4f);
    c_hudHeight = c.GetFloat("hud_height", -0.05f);
    c_hudAnchor = c.GetStr("hud_anchor", "world") == "head" ? 1 : 0;
    c_hudPremul = c.GetBool("hud_premultiplied", true);
    c_pacing = c.GetStr("xr_pacing", "game") == "headset" ? 0 : 1;
    c_sharpen = std::max(0.0f, std::min(c.GetFloat("lumasharpen", 0.0f), 10.0f));
    { std::string g = c.GetStr("fov_dlss_gaze", "off"); c_gazeMode = g == "eyes" ? 1 : g == "sim" ? 2 : 0;
      LOGI("xr config: foveated DLSS centre follows %s%s", g == "eyes" ? "eye tracking" : g == "sim" ? "a simulated gaze circle" : "the image centre",
           g == "eyes" && !g_gazeSupported ? " (no eye tracking on this headset / runtime yet - centre stays fixed)" : ""); }
    LOGI("xr config: lumasharpen %.2f", c_sharpen.load());
    if (!g_newFrameEvent) g_newFrameEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);   // auto-reset
    LOGI("xr config: pacing %s", c_pacing.load() ? "game (one submit per new game frame)" : "headset (every headset frame)");
    LOGI("xr config: hud %.2f m wide at %.2f m, height %.2f, anchor %s, premultiplied %d", c_hudWidth.load(), c_hudDistance.load(),
         c_hudHeight.load(), c_hudAnchor.load() ? "head" : "world", (int)c_hudPremul.load());
    LOGI("xr config: vr_enabled %d, screen %.2f m wide at %.2f m, height %.2f, dump_frame %d", (int)c_enabled.load(),
         c_screenWidth.load(), c_screenDistance.load(), c_screenHeight.load(), c_dumpFrame.load());
}

void XrHostSetFramePose(const XrHostEye eyes[2], bool sideBySide) {
    float yaw; XrVector3f ref; GetRef(yaw, ref);
    std::lock_guard<std::mutex> l(g_poseMutex);
    for (int e = 0; e < 2; ++e) { g_framePose[e].pose = FromRef(eyes[e].pose, yaw, ref); g_framePose[e].fov = eyes[e].fov; }
    g_poseSet = true; g_poseSbs = sideBySide;
}

void XrHostOnPresent(IDXGISwapChain* sc, ID3D12CommandQueue* queue) {
    if (!c_enabled.load() || g_producerFailed) return;
    if (!g_threadStarted.exchange(true)) {
        if (!StartXr(sc)) { g_producerFailed = true; return; }
    }
    // This frame's mode: stereo (already handed over), mono (pose set), or flat.
    bool stereo = g_stereoThisFrame; g_stereoThisFrame = false;
    bool mono = false, sbs = false; XrHostEye eyes[2];
    { std::lock_guard<std::mutex> l(g_poseMutex); if (g_poseSet) { mono = true; sbs = g_poseSbs; eyes[0] = g_framePose[0]; eyes[1] = g_framePose[1]; g_poseSet = false; } }
    if (stereo) { g_framesWithoutPose = 0; return; }
    if (mono) g_framesWithoutPose = 0;
    else if (++g_framesWithoutPose < kMonoHoldFrames) { s_held++; return; }   // keep the last head-tracked frame

    if (!g_xrReady.load() || !g_running.load()) return;
    if (!queue) {
        static bool warned = false;
        if (!warned) { warned = true; LOGW("xr: no command queue known for swapchain %p; frames not sent", sc); }
        return;
    }
    if (!ProducerReady(queue)) return;

    IDXGISwapChain3* sc3 = nullptr;
    if (FAILED(sc->QueryInterface(__uuidof(IDXGISwapChain3), (void**)&sc3))) return;
    ID3D12Resource* bb = nullptr;
    HRESULT hr = sc3->GetBuffer(sc3->GetCurrentBackBufferIndex(), __uuidof(ID3D12Resource), (void**)&bb);
    sc3->Release();
    if (FAILED(hr) || !bb) return;
    D3D12_RESOURCE_DESC bd = bb->GetDesc();
    DXGI_FORMAT fmt = TypedFormat(bd.Format);
    if (!Supported(fmt) || bd.SampleDesc.Count != 1) {
        static bool warned = false;
        if (!warned) { warned = true; LOGW("xr: back buffer format %d (samples %u) not supported", (int)bd.Format, bd.SampleDesc.Count); }
        bb->Release(); return;
    }
    if (!EnsureSharedSet(0, (UINT)bd.Width, bd.Height, fmt)) { bb->Release(); return; }
    int slot = PickSlot();
    if (slot < 0) { bb->Release(); return; }
    UINT x0 = 0; D3D12_RESOURCE_STATES st = D3D12_RESOURCE_STATE_PRESENT;
    bool ok = SubmitCopies(queue, slot, 0, 1, &bb, &x0, &st);
    bb->Release();
    if (!ok) return;
    Publish(slot, mono ? (sbs ? kSbs : kMono) : kFlat, mono ? eyes : nullptr, 0);
    static bool first = true;
    if (first) { first = false; LOGI("xr: first frame handed over (slot %d, %llux%u fmt %d)", slot, (unsigned long long)bd.Width, bd.Height, (int)fmt); }
}

bool XrHostPublishStereo(ID3D12CommandQueue* queue, ID3D12Resource* left, ID3D12Resource* right,
                         D3D12_RESOURCE_STATES state, const XrHostEye eyes[2], ID3D12Resource* hud, D3D12_RESOURCE_STATES hudState) {
    if (!left || !right || !eyes) return false;
    if (!ProducerReady(queue)) return false;
    D3D12_RESOURCE_DESC ld = left->GetDesc(), rd = right->GetDesc();
    DXGI_FORMAT fmt = TypedFormat(ld.Format);
    if (ld.Width != rd.Width || ld.Height != rd.Height || TypedFormat(rd.Format) != fmt || !Supported(fmt) ||
        ld.SampleDesc.Count != 1 || ld.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
        static bool warned = false;
        if (!warned) { warned = true; LOGW("xr: stereo eyes not usable (L %llux%u fmt %d, R %llux%u fmt %d)", (unsigned long long)ld.Width, ld.Height,
                                           (int)ld.Format, (unsigned long long)rd.Width, rd.Height, (int)rd.Format); }
        return false;
    }
    // Side by side unless that exceeds the runtime's maximum swapchain width (user 2026-10-31: 2 x 6086 = 12172 > 8192
    // on SteamVR - created without error, but the right eye was broken): then the eyes are stacked top / bottom.
    // The shared texture holds both eyes (side by side, or stacked if wider than the D3D12 texture limit); the eyes
    // then go to one swapchain each, so the runtime's swapchain limit only applies per eye.
    const uint32_t mw = 16384, mh = 16384;
    bool vertical = (UINT)ld.Width * 2 > mw;
    if (vertical && mh && ld.Height * 2 > mh) {
        static bool warned = false;
        if (!warned) { warned = true; LOGE("xr: eyes %llux%u do not fit the runtime's max swapchain %ux%u either way - lower render_scale", (unsigned long long)ld.Width, ld.Height, mw, mh); }
        return false;
    }
    { static int lastV = -1; if (lastV != (int)vertical) { lastV = vertical; LOGI("xr: stereo layout %s (eyes %llux%u, runtime max %ux%u)", vertical ? "top / bottom" : "side by side", (unsigned long long)ld.Width, ld.Height, mw, mh); } }
    if (!EnsureSharedSet(1, vertical ? (UINT)ld.Width : (UINT)ld.Width * 2, vertical ? ld.Height * 2 : ld.Height, fmt, vertical)) return false;
    if (hud) {
        D3D12_RESOURCE_DESC hd = hud->GetDesc();
        DXGI_FORMAT hf = TypedFormat(hd.Format);
        if (hd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || hd.SampleDesc.Count != 1 || hd.MipLevels != 1 ||
            (hf != DXGI_FORMAT_R8G8B8A8_UNORM && hf != DXGI_FORMAT_B8G8R8A8_UNORM) || !EnsureSharedSet(2, (UINT)hd.Width, hd.Height, hf)) {
            static bool warned = false;
            if (!warned) { warned = true; LOGW("xr: HUD image not usable (%llux%u fmt %d mips %u)", (unsigned long long)hd.Width, hd.Height, (int)hd.Format, hd.MipLevels); }
            hud = nullptr;
        }
    }
    int slot = PickSlot();
    if (slot < 0) return false;
    ID3D12Resource* srcs[2] = {left, right};
    UINT xs[2] = {0, vertical ? 0u : (UINT)ld.Width};
    UINT ys[2] = {0, vertical ? ld.Height : 0u};
    D3D12_RESOURCE_STATES sts[2] = {state, state};
    if (!SubmitCopies(queue, slot, 1, 2, srcs, xs, sts, hud, hudState, ys)) return false;
    float yaw; XrVector3f ref; GetRef(yaw, ref);
    XrHostEye local[2];
    for (int e = 0; e < 2; ++e) { local[e].pose = FromRef(eyes[e].pose, yaw, ref); local[e].fov = eyes[e].fov; }
    Publish(slot, kStereo, local, 1, hud != nullptr);
    s_stereo++;
    g_stereoThisFrame = true;
    static bool first = true;
    if (first) { first = false; LOGI("xr: first stereo frame handed over (eyes %llux%u fmt %d)", (unsigned long long)ld.Width, ld.Height, (int)fmt); }
    return true;
}

bool XrHostRunning() { return g_running.load(); }

XrTime XrHostPredictTime(float frames) {
    XrTime last = g_lastDisplayTime.load();
    if (last == 0) return 0;
    // the XR thread may have waited a while since its last xrWaitFrame (game pacing): move the base forward by the
    // time elapsed since then, so the prediction stays relative to "now"
    LARGE_INTEGER q, f; QueryPerformanceCounter(&q); QueryPerformanceFrequency(&f);
    int64_t then = g_lastWaitQpc.load();
    XrTime elapsed = then ? XrTime(double(q.QuadPart - then) * 1e9 / double(f.QuadPart)) : 0;
    if (elapsed < 0 || elapsed > 1000000000LL || c_pacing.load() == 0) elapsed = 0;   // headset pacing: unchanged
    return last + elapsed + XrTime(double(g_displayPeriod.load()) * frames);
}

bool XrHostLocateEyesAt(XrTime t, XrView out[2]) {
    if (!g_running.load() || t == 0) return false;
    XrViewStateFlags fl = 0;
    {
        std::lock_guard<std::mutex> lk(g_sessionMutex);
        if (!g_running.load() || !LocateLocal(t, out, &fl)) return false;
    }
    float yaw; XrVector3f ref; GetRef(yaw, ref);
    for (int e = 0; e < 2; ++e) out[e].pose = ToRef(out[e].pose, yaw, ref);
    static bool logged = false;
    if (!logged) {
        logged = true;
        float ipd = std::sqrt(std::pow(out[0].pose.position.x - out[1].pose.position.x, 2.0f) +
                              std::pow(out[0].pose.position.y - out[1].pose.position.y, 2.0f) +
                              std::pow(out[0].pose.position.z - out[1].pose.position.z, 2.0f));
        LOGI("xr: eye views: IPD %.1f mm, FOV L(%.1f %.1f %.1f %.1f) R(%.1f %.1f %.1f %.1f) deg", ipd * 1000.0f,
             out[0].fov.angleLeft * 57.29578f, out[0].fov.angleRight * 57.29578f, out[0].fov.angleUp * 57.29578f, out[0].fov.angleDown * 57.29578f,
             out[1].fov.angleLeft * 57.29578f, out[1].fov.angleRight * 57.29578f, out[1].fov.angleUp * 57.29578f, out[1].fov.angleDown * 57.29578f);
    }
    const XrViewStateFlags need = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
    return (fl & need) == need;
}

bool XrHostLocateEyes(XrView views[2], float predictFrames) { return XrHostLocateEyesAt(XrHostPredictTime(predictFrames), views); }

bool XrHostHeadsetFov(XrFovf out[2]) {
    std::lock_guard<std::mutex> l(g_fovMutex);
    if (!g_fovValid) return false;
    out[0] = g_fov[0]; out[1] = g_fov[1];
    return true;
}

void XrHostRecenter() { g_recenterReq = true; }
int64_t XrHostDisplayPeriodNs() { return g_running.load() ? (int64_t)g_displayPeriod.load() : 0; }
bool XrHostRecommendedEyeSize(uint32_t& w, uint32_t& h) { w = g_recW.load(); h = g_recH.load(); return w && h; }
void XrHostRequestDump() { g_dumpReq = true; }

void XrHostReport(char* buf, size_t size) {
    snprintf(buf, size, "xr: running %d, ready %d, latest slot %d, published %llu", (int)g_running.load(), (int)g_xrReady.load(),
             g_latestSlot.load(), (unsigned long long)s_published.load());
}

bool XrHostPublishMenu(ID3D12CommandQueue*, ID3D12Resource*, D3D12_RESOURCE_STATES) { return false; }

} // namespace davg
