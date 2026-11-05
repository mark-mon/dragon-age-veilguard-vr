// Trigger files in <game>\DAVGVR\ for automated tests:
//   dumpexe.trigger -> writes the exe's in-memory image to DAVGVR\exe_image.bin (decrypted / unpacked code)
//   reload.trigger  -> re-reads config.txt (log_level, xr screen settings)
//   dumpscreen.trigger -> writes the next headset screen image to DAVGVR\screen_NNN.bmp
#include <windows.h>
#include <string>
#include <cstdio>
#include "triggers.h"
#include "globals.h"
#include "log.h"
#include "xr_host.h"
#include "fbview.h"
#include "replay.h"
#include "window_size.h"
#include "limiter.h"
#include "slhook.h"

namespace davg {

static bool Take(const wchar_t* name) {
    std::wstring p = G().outDir + name;
    if (GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    DeleteFileW(p.c_str());
    LOGI("trigger %S", name);
    return true;
}

static void DumpExe() {
    HMODULE m = GetModuleHandleW(nullptr);
    auto dos = (IMAGE_DOS_HEADER*)m;
    auto nt = (IMAGE_NT_HEADERS64*)((char*)m + dos->e_lfanew);
    SIZE_T size = nt->OptionalHeader.SizeOfImage;
    std::wstring path = G().outDir + L"exe_image.bin";
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) { LOGE("dumpexe: cannot open %S", path.c_str()); return; }
    static char zero[0x10000];
    SIZE_T off = 0, readable = 0;
    while (off < size) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery((char*)m + off, &mbi, sizeof(mbi))) break;
        SIZE_T end = (SIZE_T)((char*)mbi.BaseAddress + mbi.RegionSize - (char*)m);
        if (end > size) end = size;
        bool ok = mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) && mbi.Protect != 0;
        while (off < end) {
            SIZE_T n = end - off; if (n > sizeof(zero)) n = sizeof(zero);
            fwrite(ok ? (char*)m + off : zero, 1, n, f);
            if (ok) readable += n;
            off += n;
        }
    }
    fclose(f);
    LOGI("dumpexe: %llu bytes (%llu readable) -> %S", (unsigned long long)size, (unsigned long long)readable, path.c_str());
}

static DWORD WINAPI Loop(void*) {
    for (;;) {
        Sleep(250);
        if (Take(L"dumpexe.trigger")) DumpExe();
        if (Take(L"reload.trigger")) { G().cfg.Load(G().cfgPath.c_str()); LogSetLevel(G().cfg.GetInt("log_level", LOG_INFO)); XrHostApplyConfig(); FbViewApplyConfig(); WindowSizeApplyConfig(); LimiterApplyConfig(); }
        if (Take(L"dumpscreen.trigger")) XrHostRequestDump();
        if (Take(L"viewdump.trigger")) FbViewDumpTrigger();
        if (Take(L"trace.trigger")) ReplayTraceArm(1);
        if (Take(L"hudtrace.trigger")) ReplayHudTraceArm();
        if (Take(L"hudprobe.trigger")) ReplayHudProbe();
        if (Take(L"inputs.trigger")) ReplayInputsDump();
        if (Take(L"dumpdlss.trigger")) ReplayDlssDump();
        if (Take(L"dumplite.trigger")) ReplayDlssDumpLite();
        if (Take(L"slconst.trigger")) SlHookLogConstants(12);
    }
}

void TriggersStart() { CreateThread(nullptr, 0, Loop, nullptr, 0, nullptr); }

} // namespace davg
