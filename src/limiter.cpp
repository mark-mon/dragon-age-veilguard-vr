// Frame limiter tied to the headset: holds the game to exactly (headset refresh / game_fps_divider) by waiting in the
// game's Present. A steady 1/2 or 1/3 cadence is what frame generators (OFXR-Bridge 2x/3x, SteamVR motion smoothing)
// expect; the game wandering between 50 and 60 fps on a 90 Hz headset fits none of them.
// Waits on a high-resolution waitable timer and spins the last ~0.6 ms. Config game_fps_divider = 0 (off) / 1 .. 4.
#include <windows.h>
#include <atomic>
#include "limiter.h"
#include "xr_host.h"
#include "globals.h"
#include "log.h"

namespace davg {

static std::atomic<int> c_divider{0};
static HANDLE g_timer = nullptr;
static int64_t g_next = 0;          // QPC of the next allowed Present
static int64_t g_freq = 0;

void LimiterApplyConfig() {
    int d = G().cfg.GetInt("game_fps_divider", 0);
    if (d < 0) d = 0; if (d > 6) d = 6;
    if (c_divider.exchange(d) != d) { g_next = 0; LOGI("limiter: game_fps_divider %d%s", d, d ? "" : " (off)"); }
}

void LimiterOnPresent() {
    int d = c_divider.load();
    if (d <= 0) return;
    int64_t periodNs = XrHostDisplayPeriodNs();
    if (periodNs <= 0) return;                       // no headset timing yet
    if (!g_freq) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); g_freq = f.QuadPart; }
    if (!g_timer) g_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    const int64_t step = int64_t(double(periodNs) * d * g_freq / 1e9);
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    if (g_next == 0 || now.QuadPart - g_next > step) g_next = now.QuadPart;   // first frame, or the game fell behind: restart the cadence
    const int64_t spin = g_freq * 6 / 10000;         // 0.6 ms
    int64_t wait = g_next - now.QuadPart - spin;
    if (wait > 0 && g_timer) {
        LARGE_INTEGER due; due.QuadPart = -(wait * 10000000 / g_freq);   // relative, 100 ns units
        if (SetWaitableTimer(g_timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(g_timer, 100);
    }
    do { QueryPerformanceCounter(&now); } while (now.QuadPart < g_next);
    g_next += step;
    static int64_t t0 = 0; static int frames = 0;
    if (!t0) t0 = now.QuadPart;
    if (++frames >= 300) {
        LOGI("limiter: %.1f fps (target %.1f = headset %.1f Hz / %d)", frames * double(g_freq) / double(now.QuadPart - t0),
             1e9 / double(periodNs) / d, 1e9 / double(periodNs), d);
        frames = 0; t0 = now.QuadPart;
    }
}

} // namespace davg
