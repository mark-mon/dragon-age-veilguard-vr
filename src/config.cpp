#include "config.h"
#include "log.h"
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <windows.h>

namespace davg {

static std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)tolower(c); });
    return s;
}

bool Config::Load(const wchar_t* path) {
    FILE* f = _wfopen(path, L"r");
    if (!f) return false;
    std::lock_guard<std::mutex> lk(mu_);
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        std::string l = line;
        size_t c = l.find_first_of("#;");
        if (c != std::string::npos) l = l.substr(0, c);
        size_t eq = l.find('=');
        if (eq == std::string::npos) continue;
        std::string k = Lower(Trim(l.substr(0, eq)));
        std::string v = Trim(l.substr(eq + 1));
        if (!k.empty()) m_[k] = v;
    }
    fclose(f);
    return true;
}

std::string Config::GetStr(const char* key, const char* def) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = m_.find(Lower(key));
    return it == m_.end() ? std::string(def) : it->second;
}
int Config::GetInt(const char* key, int def) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = m_.find(Lower(key));
    return it == m_.end() ? def : (int)strtol(it->second.c_str(), nullptr, 0);
}
float Config::GetFloat(const char* key, float def) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = m_.find(Lower(key));
    return it == m_.end() ? def : (float)atof(it->second.c_str());
}
bool Config::GetBool(const char* key, bool def) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = m_.find(Lower(key));
    if (it == m_.end()) return def;
    std::string v = Lower(it->second);
    return v == "1" || v == "true" || v == "yes" || v == "on";
}
void Config::Set(const char* key, const std::string& value) {
    std::lock_guard<std::mutex> lk(mu_);
    m_[Lower(key)] = value;
}
void Config::Dump() const {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& kv : m_) LOGI("config: %s = %s", kv.first.c_str(), kv.second.c_str());
}

static const char* kDefaultConfig =
"# Dragon Age: The Veilguard VR - config.txt (next to the game exe)\n"
"# log_level: 0 error, 1 warn, 2 info, 3 debug, 4 trace\n"
"log_level = 2\n"
"# hook_dx12: 0 = load and log only, no D3D12 / DXGI hooks\n"
"hook_dx12 = 1\n"
"# vr_enabled: 0 = no OpenXR at all (the game runs on the monitor only)\n"
"vr_enabled = 1\n"
"# Flat virtual screen (theater) in the headset: distance and width in metres, height of its centre\n"
"# relative to the eye height at the start / last recentre (F9 recentres)\n"
"screen_distance = 2.5\n"
"screen_width = 3.2\n"
"screen_height = 0.0\n"
"# Write the headset image of the Nth new game frame to DAVGVR\\screen_000.bmp (0 = off).\n"
"# DAVGVR\\dumpscreen.trigger dumps the next one on demand.\n"
"dump_frame = 0\n"
"# vr_mode: flat (virtual screen) | head (head-tracked, mono) | stereo (native per-eye, default). F8 cycles.\n"
"vr_mode = stereo\n"
"# level_camera: 1 = the game camera's pitch/roll are removed; the headset supplies them\n"
"level_camera = 1\n"
"# world_scale: game metres per real metre (1.0 = life size)\n"
"world_scale = 1.0\n"
"# pose_latency_frames: game frames between the camera update and the Present that shows it\n"
"pose_latency_frames = 1\n"
"# debug_fake_head_yaw_deg: oscillating fake head yaw for tests without a moving headset (0 = off)\n"
"debug_fake_head_yaw_deg = 0\n"
"# HUD (stereo): hud_layer 1 = the game's UI on its own panel, 0 = drawn into both eyes. F10 shows / hides the panel.\n"
"hud_layer = 1\n"
"# panel width and distance in metres, height of its centre relative to eye height; anchor world | head\n"
"hud_width = 1.4\n"
"hud_distance = 1.5\n"
"hud_height = -0.05\n"
"hud_anchor = world\n"
"hud_premultiplied = 1\n"
"# Render resolution (each eye): game = the game's own window size | auto = 16:9 at the headset's recommended eye\n"
"# height x render_scale | WxH (e.g. 2560x1440). The game window is resized (larger than the monitor if needed),\n"
"# once per launch at the title screen; a change takes effect after restarting the game.\n"
"render_resolution = auto\n"
"render_scale = 1.0\n"
"render_resolution_hold = 1\n"
"# xr_pacing: game = submit only when the game has a new frame (the runtime sees the real frame rate, so SteamVR\n"
"# reprojection / motion smoothing can work) | headset = submit every headset frame (re-sends the last frame)\n"
"xr_pacing = game\n"
"# game_fps_divider: 0 = off | N = hold the game to exactly headset refresh / N (90 Hz: 2 -> 45 fps, 3 -> 30 fps),\n"
"# a steady cadence for frame generation (OFXR-Bridge 2x / 3x, SteamVR motion smoothing). Default 2 (+ OFXR-Bridge 2x).\n"
"game_fps_divider = 2\n"
"# fov_dlss: foveated DLSS - DLSS only reconstructs the centre of each eye (big GPU saving). Default on.\n"
"fov_dlss = 1\n"
"# dred = 1: D3D12 device-removed diagnostics (breadcrumbs, page faults) for debugging GPU crashes. Off by default.\n"
"dred = 0\n"
;

void WriteDefaultConfig(const wchar_t* path) {
    if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) return;
    FILE* f = _wfopen(path, L"w");
    if (!f) return;
    fputs(kDefaultConfig, f);
    fclose(f);
}

} // namespace davg
