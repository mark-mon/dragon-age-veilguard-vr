#include "log.h"
#include <cstdio>
#include <share.h>
#include <io.h>

namespace davg {

static FILE* g_file = nullptr;
static int g_level = LOG_INFO;
static CRITICAL_SECTION g_cs;
static bool g_csInit = false;
static const char* kNames[] = {"ERROR", "WARN ", "INFO ", "DEBUG", "TRACE"};

void LogInit(const wchar_t* path, int level) {
    if (!g_csInit) { InitializeCriticalSection(&g_cs); g_csInit = true; }
    g_level = level;
    if (g_file) fclose(g_file);
    g_file = _wfsopen(path, L"w", _SH_DENYWR);
    // Commit the log to disk once a second: a bluescreen loses whatever is still in the OS cache (user's 5090 run
    // 2026-10-30 left a 0-byte log).
    static bool started = false;
    if (!started && g_file) {
        started = true;
        HANDLE t = CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
            for (;;) {
                Sleep(1000);
                if (!g_file) continue;
                EnterCriticalSection(&g_cs);
                fflush(g_file);
                HANDLE h = (HANDLE)_get_osfhandle(_fileno(g_file));
                LeaveCriticalSection(&g_cs);
                if (h != INVALID_HANDLE_VALUE) FlushFileBuffers(h);
            }
        }, nullptr, 0, nullptr);
        if (t) { SetThreadPriority(t, THREAD_PRIORITY_BELOW_NORMAL); CloseHandle(t); }
    }
}

void LogSetLevel(int level) { g_level = level; }
int LogGetLevel() { return g_level; }

void LogWrite(int level, const char* fmt, ...) {
    if (level > g_level || !g_file) return;
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    EnterCriticalSection(&g_cs);
    fprintf(g_file, "%02d:%02d:%02d.%03d [%s] [%5lu] %s\n", st.wHour, st.wMinute, st.wSecond,
            st.wMilliseconds, kNames[level < 0 ? 0 : (level > 4 ? 4 : level)], GetCurrentThreadId(), buf);
    if (level <= LOG_INFO) fflush(g_file);
    LeaveCriticalSection(&g_cs);
}

void LogFlush() {
    if (g_file) { EnterCriticalSection(&g_cs); fflush(g_file); LeaveCriticalSection(&g_cs); }
}

} // namespace davg
