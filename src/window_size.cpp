// Render resolution at headset size (game-independent, for windowed / borderless D3D games).
//
// The game renders at its window's client size (the swapchain follows WM_SIZE). To render each eye at the headset's
// resolution the game window's client area is set to the target size - larger than the monitor if needed: the window
// procedure is subclassed so WM_GETMINMAXINFO allows it and (render_resolution_hold = 1) WM_WINDOWPOSCHANGING keeps it.
// The resize is done once, on the window's own thread (posted message), after the game has shown frames for a while
// (resizing during startup loading crashed other engines).
//
// Config: render_resolution = game | auto | auto16x9 | WxH ; render_scale (auto only, default 1.0) ; render_resolution_hold.
// auto = the headset's recommended eye size * render_scale (pixels where the headset needs them);
// auto16x9 = 16:9 at the headset's eye height * render_scale; render_resolution_snap = 1 rounds to 1080p / 1440p / 2160p.
#include <windows.h>
#include <dxgi.h>
#include <atomic>
#include <string>
#include <cstdio>
#include <cstdlib>
#include "window_size.h"
#include "xr_host.h"
#include "globals.h"
#include "log.h"

namespace davg {

static const UINT WM_DAVG_RESIZE = WM_APP + 0x3A7;
static HWND g_hwnd = nullptr;
static WNDPROC g_orig = nullptr;
static std::atomic<int> g_tw{0}, g_th{0};          // target client size (0 = leave the game alone)
static std::atomic<bool> g_hold{true};
static std::atomic<int> g_state{0};                // 0 waiting, 1 posted, 2 done
static uint64_t g_presents = 0;
static std::atomic<bool> g_dirty{true};

// render_resolution_snap = 1: the 16:9 sizes that worked before the restore-order fix (tests 092-108)
static const int kSnap[][2] = {{1920, 1080}, {2560, 1440}, {3840, 2160}};
static const int kSnapN = sizeof(kSnap) / sizeof(kSnap[0]);

static bool ClientSize(HWND h, int& w, int& ht) { RECT r{}; if (!GetClientRect(h, &r)) return false; w = r.right; ht = r.bottom; return true; }

static void OuterSize(HWND h, int cw, int ch, int& ow, int& oh) {
    RECT r{0, 0, cw, ch};
    DWORD st = (DWORD)GetWindowLongPtrW(h, GWL_STYLE), ex = (DWORD)GetWindowLongPtrW(h, GWL_EXSTYLE);
    AdjustWindowRectEx(&r, st, FALSE, ex);
    ow = r.right - r.left; oh = r.bottom - r.top;
}

static LRESULT CALLBACK SubProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    int tw = g_tw.load(), th = g_th.load();
    if (m == WM_DAVG_RESIZE && tw > 0) {
        int ow, oh; OuterSize(h, tw, th, ow, oh);
        RECT wr{}; GetWindowRect(h, &wr);
        BOOL ok = SetWindowPos(h, nullptr, wr.left, wr.top, ow, oh, SWP_NOZORDER | SWP_NOACTIVATE);
        int cw = 0, ch = 0; ClientSize(h, cw, ch);
        LOGI("render resolution: window resized to client %dx%d (wanted %dx%d, outer %dx%d) %s", cw, ch, tw, th, ow, oh, ok ? "" : "FAILED");
        g_state = 2;
        return 0;
    }
    if (tw > 0 && g_state.load() == 2) {
        if (m == WM_GETMINMAXINFO) {
            LRESULT r = CallWindowProcW(g_orig, h, m, wp, lp);
            MINMAXINFO* mm = (MINMAXINFO*)lp;
            mm->ptMaxTrackSize.x = mm->ptMaxSize.x = 16384; mm->ptMaxTrackSize.y = mm->ptMaxSize.y = 16384;
            return r;
        }
        if (m == WM_WINDOWPOSCHANGING && g_hold.load()) {
            WINDOWPOS* p = (WINDOWPOS*)lp;
            if (!(p->flags & SWP_NOSIZE)) {
                int ow, oh; OuterSize(h, tw, th, ow, oh);
                if (p->cx != ow || p->cy != oh) {
                    static int logged = 0;
                    if (logged++ < 5) LOGI("render resolution: game resized the window to %dx%d, kept at %dx%d", p->cx, p->cy, ow, oh);
                    p->cx = ow; p->cy = oh;
                }
            }
        }
    } else if (m == WM_GETMINMAXINFO && g_state.load() == 1 && tw > 0) {
        LRESULT r = CallWindowProcW(g_orig, h, m, wp, lp);
        MINMAXINFO* mm = (MINMAXINFO*)lp;
        mm->ptMaxTrackSize.x = mm->ptMaxSize.x = 16384; mm->ptMaxTrackSize.y = mm->ptMaxSize.y = 16384;
        return r;
    }
    return CallWindowProcW(g_orig, h, m, wp, lp);
}

void WindowSizeApplyConfig() {
    std::string m = G().cfg.GetStr("render_resolution", "auto");
    g_hold = G().cfg.GetBool("render_resolution_hold", true);
    int w = 0, h = 0;
    if (m == "game" || m == "off" || m == "0") { w = h = 0; }
    else if (m != "auto" && m != "auto16x9" && sscanf(m.c_str(), "%dx%d", &w, &h) != 2) { LOGW("render_resolution '%s' not understood (game | auto | WxH)", m.c_str()); w = h = 0; }
    if (m == "auto") w = -1;   // resolved once the headset is known
    if (m == "auto16x9") w = -2;
    g_tw = w; g_th = h;
    g_dirty = true;
    LOGI("render resolution config: %s, scale %.2f, hold %d", m.c_str(), G().cfg.GetFloat("render_scale", 1.0f), (int)g_hold.load());
}

// Render thread, every Present.
void WindowSizeOnPresent(IDXGISwapChain* sc, bool vrActive) {
    ++g_presents;
    if (!vrActive) return;
    if (!g_dirty.load() && g_state.load() != 0) return;
    if (g_tw.load() == 0) { g_dirty = false; return; }
    if (g_presents < 600) return;   // let the game come up first
    if (g_tw.load() < 0) {   // auto (-1: the headset's own eye size, -2: 16:9 at the headset's eye height)
        uint32_t rw, rh;
        if (!XrHostRecommendedEyeSize(rw, rh)) return;
        float sc = G().cfg.GetFloat("render_scale", 1.0f);
        // multiples of 8: the game pads odd sizes (4122 -> 4124 DLSS output, test 141) and the replay then refuses the eyes
        int h = (int(rh * sc + 4.0f)) & ~7;
        int w = g_tw.load() == -2 ? (h * 16 / 9) & ~7 : int(rw * sc + 4.0f) & ~7;
        if (h < 360) h = 360;
        if (w < 360) w = 360;
        if (G().cfg.GetBool("render_resolution_snap", false)) {   // old behaviour: nearest standard 16:9 mode
            int best = 0;
            for (int i = 0; i < kSnapN; i++) if (std::abs(kSnap[i][1] - h) < std::abs(kSnap[best][1] - h)) best = i;
            LOGI("render resolution auto: headset eye %ux%u x %.2f -> %dx%d, snapped to %dx%d", rw, rh, sc, w, h, kSnap[best][0], kSnap[best][1]);
            w = kSnap[best][0]; h = kSnap[best][1];
        } else LOGI("render resolution auto: headset eye %ux%u x %.2f -> %dx%d per eye%s", rw, rh, sc, w, h, g_tw.load() == -2 ? " (16:9)" : "");
        g_tw = w; g_th = h;
    }
    DXGI_SWAP_CHAIN_DESC d{};
    if (FAILED(sc->GetDesc(&d)) || !d.OutputWindow) return;
    HWND h = d.OutputWindow;
    int cw = 0, ch = 0; ClientSize(h, cw, ch);
    g_dirty = false;
    if (cw == g_tw.load() && ch == g_th.load()) { g_state = 2; return; }
    static bool resizedOnce = false;
    if (resizedOnce && !G().cfg.GetBool("render_resolution_live", false)) {   // test 051: a second resize in game left the left eye black - only once per launch
        LOGW("render resolution: %dx%d takes effect after restarting the game (changing it in game breaks the left eye)", g_tw.load(), g_th.load());
        g_tw = cw; g_th = ch; g_state = 2;
        return;
    }
    resizedOnce = true;
    if (!d.Windowed) { LOGW("render resolution: exclusive fullscreen - set the game to windowed / borderless to change the render size"); g_state = 2; return; }
    if (!g_orig) {
        g_hwnd = h;
        g_orig = (WNDPROC)SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)SubProc);
        if (!g_orig) { LOGE("render resolution: could not subclass the game window (%lu)", GetLastError()); g_tw = 0; return; }
    }
    LOGI("render resolution: client %dx%d -> %dx%d", cw, ch, g_tw.load(), g_th.load());
    g_state = 1;
    PostMessageW(h, WM_DAVG_RESIZE, 0, 0);
}

} // namespace davg
