// In-headset settings menu (vrkit / Dear ImGui), drawn on the XR thread's own D3D11 device and shown by xr_host as
// a quad layer on top of everything. Open / close: Insert, or hold L3 + R3. World-anchored by default (it stays where
// it opened; menu_anchor = head makes it follow the headset).
//
// Every option is a config.txt key: a change is written into the in-memory config and the normal apply functions
// (XrHostApplyConfig / FbViewApplyConfig) re-read it, so the menu and the file can never disagree. The file is
// rewritten when the menu closes (vrkit Options::Save keeps comments and order).
#include <windows.h>
#include <d3d11.h>
#include <string>
#include <cstdio>
#include "menu.h"
#include "vrkit_options.h"
#include "vrkit_menu.h"
#include "globals.h"
#include "log.h"
#include "xr_host.h"
#include "fbview.h"
#include "replay.h"
#include "slhook.h"
#include "limiter.h"

namespace davg {

namespace {
struct MenuCfg {
    std::string vrMode = "stereo";
    float worldScale = 1.0f;
    int latency = 1;
    bool level = true;
    bool perEye = true;
    std::string tagRouting = "order";
    float sharpen = 0.0f;
    bool fov = false, fovPeriph = false, fovTaa = true, fovBorder = false; float fovW = 0.6f, fovH = 0.6f; std::string fovGaze = "off"; bool vrsOn = false, vrs4 = false; float vrsIn = 0.6f, vrsMid = 2.0f;
    std::string rightJitter = "game";
    bool history = true;
    bool sceneHistory = true;
    bool hudLayer = true;
    float hudWidth = 1.4f, hudDistance = 1.5f, hudHeight = -0.05f;
    std::string hudAnchor = "world";
    float screenWidth = 3.2f, screenDistance = 2.5f;
    std::string renderRes = "auto";
    float renderScale = 1.0f;
    std::string menuAnchor = "world";
    std::string pacing = "game";
    bool swapEyes = false;
    int divider = 0;
} M;
vrkit::menu::Placement g_place;
bool g_ok = false;

void ApplyAll() {
    // in-memory config <- every option's value, then the regular apply paths re-read it
    for (auto& o : vrkit::Options::Get().All())
        if (!o.key.empty() && o.key.rfind("menu_", 0) != 0) G().cfg.Set(o.key.c_str(), vrkit::Options::Get().ValueText(o));
    XrHostApplyConfig();
    FbViewApplyConfig();
    LimiterApplyConfig();
}
void ApplyPlacement() { g_place.anchor = M.menuAnchor == "head" ? 1 : 0; }

const char* ModeName() { int m = FbViewMode(); return m == 2 ? "stereo" : m == 1 ? "head" : "flat"; }

std::string Status() {
    static DWORD t0 = GetTickCount(); static uint64_t f0 = ReplayFrameCount(); static float fps = 0;
    DWORD now = GetTickCount();
    if (now - t0 >= 1000) { uint64_t f = ReplayFrameCount(); fps = (f - f0) * 1000.0f / float(now - t0); f0 = f; t0 = now; }
    char b[128];
    snprintf(b, sizeof(b), "%.0f fps | %s%s | HUD %s", fps, ModeName(), FbViewMode() == 2 ? (ReplayLastFrameStereo() ? " ok" : " (no replay)") : "",
             ReplayHudActive() ? "panel" : "in eyes");
    return b;
}
} // namespace

bool MenuInit(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    if (g_ok) return true;
    if (!G().cfg.GetBool("menu_enabled", true)) return false;
    vrkit::SetLog([](const char* m) { LOGI("%s", m); });
    Config& c = G().cfg;
    M.vrMode = c.GetStr("vr_mode", "stereo");
    M.worldScale = c.GetFloat("world_scale", 1.0f);
    M.latency = c.GetInt("pose_latency_frames", 1);
    M.level = c.GetBool("level_camera", true);
    M.perEye = c.GetBool("stereo_dlss_per_eye", true);
    M.tagRouting = c.GetStr("stereo_dlss_tag_routing", "order");
    M.sharpen = c.GetFloat("lumasharpen", 0.0f);
    M.fov = c.GetBool("fov_dlss", true); M.fovPeriph = c.GetBool("fov_dlss_periphery", false); M.fovTaa = c.GetBool("fov_dlss_periphery_taa", true); M.fovBorder = c.GetBool("fov_dlss_border", false);
    M.fovGaze = c.GetStr("fov_dlss_gaze", "off"); M.vrsOn = c.GetBool("fov_vrs", false); M.vrs4 = c.GetBool("fov_vrs_outer_4x4", false); M.vrsIn = c.GetFloat("fov_vrs_inner", 0.6f); M.vrsMid = c.GetFloat("fov_vrs_mid", 2.0f); M.fovW = c.GetFloat("fov_dlss_width", 0.6f); M.fovH = c.GetFloat("fov_dlss_height", 0.6f);
    M.rightJitter = c.GetStr("stereo_dlss_right_jitter", "game");
    M.history = c.GetBool("stereo_replay_history", true);
    M.sceneHistory = c.GetBool("stereo_scene_history", false);
    { std::string h = c.GetStr("hud_layer", "1"); M.hudLayer = !(h == "0" || h == "false"); }
    M.hudWidth = c.GetFloat("hud_width", 1.4f); M.hudDistance = c.GetFloat("hud_distance", 1.5f); M.hudHeight = c.GetFloat("hud_height", -0.05f);
    M.hudAnchor = c.GetStr("hud_anchor", "world");
    M.screenWidth = c.GetFloat("screen_width", 3.2f); M.screenDistance = c.GetFloat("screen_distance", 2.5f);
    M.renderRes = c.GetStr("render_resolution", "auto");
    M.renderScale = c.GetFloat("render_scale", 1.0f);
    g_place.distance = c.GetFloat("menu_distance", 1.0f);
    g_place.width = c.GetFloat("menu_width", 0.8f);
    g_place.offsetY = c.GetFloat("menu_height", -0.05f);
    M.menuAnchor = c.GetStr("menu_anchor", "world");
    M.pacing = c.GetStr("xr_pacing", "game");
    M.swapEyes = c.GetBool("swap_eyes", false);
    M.divider = c.GetInt("game_fps_divider", 2);
    ApplyPlacement();

    auto& o = vrkit::Options::Get();
    auto apply = [] { ApplyAll(); };
    // View
    auto& mode = o.AddChoice("View", "vr_mode", "Mode", &M.vrMode, {"stereo", "head", "flat"},
                             "stereo: native per-eye 3D. head: one image, head tracked. flat: virtual screen.\nF8 cycles too.", apply);
    mode.choiceLabels = {"Stereo (3D)", "Head tracked (mono)", "Flat screen"};
    o.AddFloat("View", "world_scale", "World scale", &M.worldScale, 0.25f, 4.0f, 0.05f, 2,
               "Game metres per real metre. Higher = the world looks smaller, depth stronger.", apply);
    o.AddInt("View", "pose_latency_frames", "Pose latency (frames)", &M.latency, 0, 4, 1,
             "Game frames between the camera update and the frame that shows it.", apply);
    o.AddBool("View", "level_camera", "Level camera", &M.level, "The headset supplies pitch / roll (the game camera's are removed).", apply);
    o.AddBool("View", "swap_eyes", "Swap eyes", &M.swapEyes, "Diagnostic: shows the left image to the right eye. If depth looks inside-out, this fixes it\n(and please report it).", apply);
    o.AddAction("View", "Recentre (F9)", [] { XrHostRecenter(); }, "Forward = where you are looking now.");
    o.AddFloat("View", "screen_width", "Flat screen width (m)", &M.screenWidth, 1.0f, 8.0f, 0.1f, 1, "Flat mode only.", apply);
    o.AddFloat("View", "screen_distance", "Flat screen distance (m)", &M.screenDistance, 1.0f, 8.0f, 0.1f, 1, "Flat mode only.", apply);
    // HUD
    o.AddBool("HUD", "hud_layer", "HUD on its own panel", &M.hudLayer,
              "On: minimap / quest text / menus on a panel. Off: painted into both eyes. F10 hides the panel.", apply);
    o.AddFloat("HUD", "hud_width", "Panel width (m)", &M.hudWidth, 0.4f, 4.0f, 0.05f, 2, "", apply);
    o.AddFloat("HUD", "hud_distance", "Panel distance (m)", &M.hudDistance, 0.5f, 5.0f, 0.05f, 2, "", apply);
    o.AddFloat("HUD", "hud_height", "Panel height (m)", &M.hudHeight, -1.5f, 1.5f, 0.02f, 2, "Up (+) or down (-) from eye level.", apply);
    auto& ha = o.AddChoice("HUD", "hud_anchor", "Panel anchor", &M.hudAnchor, {"world", "head"},
                           "world: fixed in front of the recentred view. head: follows the headset.", apply);
    ha.choiceLabels = {"World (fixed)", "Head (follows)"};
    // Display
    std::vector<std::string> rr = {"auto", "auto16x9", "game", "1920x1080", "2560x1440", "3840x2160"};
    bool known = false; for (auto& r : rr) if (r == M.renderRes) known = true;
    if (!known) rr.push_back(M.renderRes);
    auto& res = o.AddChoice("Display", "render_resolution", "Render resolution (per eye)", &M.renderRes, rr,
                            "auto: the headset's own eye resolution x render scale. auto16x9: 16:9 at the eye height.\nApplied at the next game start.");
    res.needsRestart = true;
    auto& scl = o.AddFloat("Display", "render_scale", "Render scale (auto)", &M.renderScale, 0.4f, 1.5f, 0.05f, 2,
                           "Multiplies the headset's recommended eye height. Applied at the next game start.");
    scl.needsRestart = true;
    auto& pc = o.AddChoice("Display", "xr_pacing", "Frame pacing", &M.pacing, {"game", "headset"},
                           "game: submit only new game frames (SteamVR sees the real fps; reprojection / motion smoothing).\nheadset: re-send the last frame every headset frame.", apply);
    pc.choiceLabels = {"Game (real fps)", "Headset (always 90)"};
    o.AddInt("Display", "game_fps_divider", "Frame limit (refresh / N)", &M.divider, 0, 4, 1,
             "0 = off. 2 = half the headset refresh (90 Hz -> 45), 3 = a third (-> 30). For frame generation\n(OFXR-Bridge 2x / 3x, SteamVR motion smoothing): a steady 1/N cadence.", apply);
    o.AddBool("Display", "stereo_dlss_per_eye", "Per-eye DLSS", &M.perEye, "Off: both eyes share one DLSS history (ghosting).", apply);
    o.AddFloat("Display", "lumasharpen", "Sharpen (LumaSharpen)", &M.sharpen, 0.0f, 5.0f, 0.05f, 2,
               "Sharpens the eye images (not the HUD / menu). 0 = off, 0.65 = classic default, up to 5. Live.", apply);
    o.AddBool("Foveated DLSS", "fov_dlss", "Foveated DLSS (upscaling)", &M.fov, "DLSS (game preset) only in the centre of each eye; the rest cheaper. Live.", apply);
    auto& fg = o.AddChoice("Foveated DLSS", "fov_dlss_gaze", "Centre follows", &M.fovGaze, {"off", "eyes", "sim"},
                           "Image centre, or your eyes (headsets with OpenXR eye tracking), or a test circle.", apply);
    fg.choiceLabels = {"Image centre", "Eye tracking", "Test circle"};
    o.AddFloat("Foveated DLSS", "fov_dlss_width", "Centre width", &M.fovW, 0.2f, 1.0f, 0.05f, 2, "Fraction of the eye image.", apply);
    o.AddFloat("Foveated DLSS", "fov_dlss_height", "Centre height", &M.fovH, 0.2f, 1.0f, 0.05f, 2, "Fraction of the eye image.", apply);
    o.AddBool("Foveated DLSS", "fov_dlss_periphery_taa", "Periphery smoothing (TAA)", &M.fovTaa, "Cheap temporal smoothing of the outer area (removes the sparkle). Almost free.", apply);
    o.AddBool("Foveated DLSS", "fov_dlss_periphery", "Periphery DLSS (2nd pass)", &M.fovPeriph, "A second DLSS pass for the outer area. Best quality, but with DLSS 310 it costs\nabout as much as the centre crop saves.", apply);
    o.AddBool("Foveated DLSS", "fov_vrs", "VRS foveated rendering", &M.vrsOn, "Coarser shading toward the edges while the scene is drawn (GPU VRS tier 2). Live.", apply);
    o.AddFloat("Foveated DLSS", "fov_vrs_inner", "VRS full-detail size", &M.vrsIn, 0.2f, 1.0f, 0.05f, 2, "Centre region shaded at full rate (fraction of the image).", apply);
    o.AddFloat("Foveated DLSS", "fov_vrs_mid", "VRS 2x2 size", &M.vrsMid, 0.2f, 2.0f, 0.05f, 2, "Up to here: 1 shade per 2x2 pixels. Beyond: 4x4 (or 2x2).", apply);
    o.AddBool("Foveated DLSS", "fov_vrs_outer_4x4", "VRS 4x4 outside", &M.vrs4, "Off: 2x2 everywhere outside the full-detail region.", apply);
    o.AddBool("Foveated DLSS", "fov_dlss_border", "Show centre border", &M.fovBorder, "Draws the edge of the centre region in red.", apply);
    // (2026-11-04) removed from the menu: right-eye DLSS jitter / input routing tests, per-eye post / scene history -
    // the jaggy right eye was an old DLSS DLL (3.7) in the game folder; none of these changed it (user).
    // Debug
    o.AddAction("Debug", "Dump headset image", [] { XrHostRequestDump(); }, "DAVGVR\\screen_NNN.bmp (+ hud_NNN.bmp).");
    o.AddAction("Debug", "Dump DLSS outputs", [] { ReplayDlssDump(); }, "Both eyes' DLSS images: DAVGVR\\dlss_*.bmp (send them with the log).");
    o.AddAction("Debug", "HUD probe", [] { ReplayHudProbe(); }, "Reads back the UI targets before the composite: DAVGVR\\rt8_*.bmp.");
    o.AddAction("Debug", "HUD trace", [] { ReplayHudTraceArm(); }, "Logs the render targets of the final stage (davgvr.log 'hudtrace').");
    o.AddAction("Debug", "Command list trace", [] { ReplayTraceArm(1); }, "Logs one frame's barriers (davgvr.log 'trace').");
    o.AddAction("Debug", "Reload config.txt", [] {
        G().cfg.Load(G().cfgPath.c_str()); XrHostApplyConfig(); FbViewApplyConfig(); LOGI("menu: config reloaded"); },
        "Re-reads the file (values edited outside the menu).");
    // Menu panel
    o.AddFloat("Menu", "menu_distance", "Distance (m)", &g_place.distance, 0.4f, 3.0f, 0.05f, 2, "How far away this panel is.");
    o.AddFloat("Menu", "menu_width", "Width (m)", &g_place.width, 0.3f, 2.0f, 0.05f, 2, "");
    o.AddFloat("Menu", "menu_height", "Height (m)", &g_place.offsetY, -1.0f, 1.0f, 0.02f, 2, "Up (+) or down (-) from eye level.");
    auto& ma = o.AddChoice("Menu", "menu_anchor", "Anchor", &M.menuAnchor, {"world", "head"},
                           "world: stays where it opened (turn your head to read it). head: follows the headset.", [] { ApplyPlacement(); });
    ma.choiceLabels = {"World (fixed)", "Head (follows)"};

    vrkit::menu::Settings s;
    s.title = "Dragon Age: The Veilguard VR";
    s.configPath = G().cfgPath;
    s.statusLine = [] { return Status(); };
    s.input.chordHoldMs = c.GetInt("menu_hold_ms", 300);
    g_ok = vrkit::menu::Init(dev, ctx, s);
    LOGI("menu: %s (Insert or hold L3+R3; %s anchored)", g_ok ? "ready" : "init FAILED", g_place.anchor ? "head" : "world");
    return g_ok;
}

bool MenuFrame() {
    if (!g_ok) return false;
    static bool wasOpen = false;
    if (!wasOpen) M.vrMode = ModeName();   // F8 may have changed it while the menu was closed
    bool open = vrkit::menu::Update();
    if (open != wasOpen) LOGI("menu: %s", open ? "opened" : "closed");
    wasOpen = open;
    return open;
}
ID3D11Texture2D* MenuTexture() { return vrkit::menu::Texture(); }
const vrkit::menu::Placement& MenuPlacement() { return g_place; }

} // namespace davg
