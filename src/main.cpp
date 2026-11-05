// Dragon Age: The Veilguard VR - entry point.
// Loaded by "Dragon Age The Veilguard.exe" as version.dll (see proxy.cpp).
#include <windows.h>
#include <string>
#include "globals.h"
#include "proxy.h"
#include "log.h"
#include "crash.h"
#include "fovdlss.h"
#include "modwatch.h"
#include "dx12_hooks.h"
#include "triggers.h"
#include "xr_host.h"
#include "window_size.h"
#include "limiter.h"
#include "fbview.h"
#include "slhook.h"
#include "../third_party/minhook/include/MinHook.h"

namespace davg {
Globals& G() { static Globals g; return g; }
}
using namespace davg;

static DWORD WINAPI InitThread(void*) {
    // Wait for the game to load d3d12.dll itself (delay-loaded by the exe / the PlatformPcDx12 DLL).
    DWORD t0 = GetTickCount();
    while (!GetModuleHandleW(L"d3d12.dll")) {
        Sleep(10);
        if (GetTickCount() - t0 > 180000) { LOGE("d3d12.dll was not loaded within 180 s - giving up"); return 0; }
    }
    LOGI("d3d12.dll loaded after %lu ms", GetTickCount() - t0);
    MH_STATUS s = MH_Initialize();
    if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) { LOGE("MH_Initialize failed: %d", (int)s); return 0; }
    CrashInstallExitHooks();
    fovdlss::Install();
    const bool waitDev = G().cfg.GetBool("hook_dx12", true) && Dx12HookCreateDevice();
    if (G().cfg.GetBool("hook_streamline", true)) {
        for (int i = 0; i < 200 && !GetModuleHandleW(L"sl.interposer.dll"); i++) Sleep(10);
        SlHookInstall();
    }
    // dx12 hooks install themselves from the game's own device (Hk_CreateDev); the old dummy-device path only if the
    // game's device never shows up (mod loaded late)
    if (G().cfg.GetBool("hook_dx12", true) && !(waitDev && Dx12WaitGameDevice(120000))) { LOGW("dx12 hooks: no game device seen - dummy-device install"); InstallDx12Hooks(); }
    if (G().cfg.GetBool("fb_view_hooks", true)) FbViewInstall();
    TriggersStart();
    return 0;
}

static void Init(HMODULE self) {
    G().self = self;
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir = exe;
    dir = dir.substr(0, dir.find_last_of(L"\\/") + 1);
    G().gameDir = dir;
    G().outDir = dir + L"DAVGVR\\";
    CreateDirectoryW(G().outDir.c_str(), nullptr);

    std::wstring cfgPath = dir + L"config.txt";
    WriteDefaultConfig(cfgPath.c_str());
    G().cfgPath = cfgPath;
    G().cfg.Load(cfgPath.c_str());
    LogInit((dir + L"davgvr.log").c_str(), G().cfg.GetInt("log_level", LOG_INFO));
    LOGI("DAVG VR " __DATE__ " " __TIME__ " loaded as %S into %S (pid %lu)", ProxySelfName(), exe, GetCurrentProcessId());
    if (ProxyGetKind() == ProxyKind::Unknown) LOGW("loaded under an unknown file name: exports are not forwarded");
    HMODULE m = GetModuleHandleW(nullptr);
    LOGI("exe base %p", m);
    G().cfg.Dump();
    XrHostApplyConfig();
    WindowSizeApplyConfig();
    LimiterApplyConfig();
    CrashInstall();
    ModWatchStart();
    CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        ProxyDetect(h);
        wchar_t name[64];
        swprintf_s(name, L"Local\\DAVGVR_init_%lu", GetCurrentProcessId());
        HANDLE e = CreateEventW(nullptr, TRUE, FALSE, name);
        bool first = e && GetLastError() != ERROR_ALREADY_EXISTS;
        if (first) { G().initEvent = e; Init(h); }
        else if (e) CloseHandle(e);
    } else if (reason == DLL_PROCESS_DETACH) {
        LOGI("unloading");
        LogFlush();
    }
    return TRUE;
}
