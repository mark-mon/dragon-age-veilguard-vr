// Multi-name proxy: the same DLL works as dinput8.dll or version.dll (rename the file).
// At load it reads its own file name and forwards every export to the system DLL of that name.
// Exports for all supported names are present in every copy; unused ones are never called.
#include <windows.h>
#include <cstdint>
#include <cwchar>
#include "proxy.h"
#include "log.h"

namespace davg {

static HMODULE g_real = nullptr;
static wchar_t g_selfName[64] = L"";
static ProxyKind g_kind = ProxyKind::Unknown;

const wchar_t* ProxySelfName() { return g_selfName; }
ProxyKind ProxyGetKind() { return g_kind; }

void ProxyDetect(HMODULE self) {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(self, path, MAX_PATH);
    const wchar_t* b = wcsrchr(path, L'\\');
    b = b ? b + 1 : path;
    wcsncpy_s(g_selfName, b, _TRUNCATE);
    _wcslwr_s(g_selfName);
    if (!wcscmp(g_selfName, L"dinput8.dll")) g_kind = ProxyKind::Dinput8;
    else if (!wcscmp(g_selfName, L"version.dll")) g_kind = ProxyKind::Version;
    else g_kind = ProxyKind::Unknown;   // loaded by some other loader (e.g. as an addon): no forwarding needed
}

// Loads the real system DLL with the same name as this file. Lazy: called from the first forwarded export,
// never from DllMain (avoids loading DLLs under the loader lock).
static HMODULE Real() {
    HMODULE m = (HMODULE)InterlockedCompareExchangePointer((void**)&g_real, nullptr, nullptr);
    if (m) return m;
    if (g_kind == ProxyKind::Unknown) return nullptr;
    wchar_t path[MAX_PATH];
    GetSystemDirectoryW(path, MAX_PATH);
    wcscat_s(path, L"\\");
    wcscat_s(path, g_selfName);
    m = LoadLibraryW(path);
    if (!m) LOGE("proxy: could not load %S (%lu)", path, GetLastError());
    InterlockedExchangePointer((void**)&g_real, m);
    return m;
}

FARPROC ProxyReal(const char* name) {
    HMODULE m = Real();
    FARPROC p = m ? GetProcAddress(m, name) : nullptr;
    if (!p) LOGE("proxy: %s not found in system %S", name, g_selfName);
    return p;
}

} // namespace davg
using namespace davg;

// Generic forwarder for functions with only integer/pointer arguments (x64: up to 8 are passed through).
// All version.dll exports qualify.
typedef uintptr_t(WINAPI* Fwd8)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
#define FORWARD8(name)                                                                                  \
    extern "C" uintptr_t WINAPI Proxy_##name(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d,      \
                                             uintptr_t e, uintptr_t f, uintptr_t g, uintptr_t h) {     \
        static Fwd8 p = nullptr;                                                                        \
        if (!p) p = (Fwd8)ProxyReal(#name);                                                             \
        return p ? p(a, b, c, d, e, f, g, h) : 0;                                                       \
    }

// ---- dinput8.dll
FORWARD8(DirectInput8Create)
FORWARD8(DllCanUnloadNow)
FORWARD8(DllGetClassObject)
FORWARD8(DllRegisterServer)
FORWARD8(DllUnregisterServer)
FORWARD8(GetdfDIJoystick)

// ---- version.dll
FORWARD8(GetFileVersionInfoA)
FORWARD8(GetFileVersionInfoByHandle)
FORWARD8(GetFileVersionInfoExA)
FORWARD8(GetFileVersionInfoExW)
FORWARD8(GetFileVersionInfoSizeA)
FORWARD8(GetFileVersionInfoSizeExA)
FORWARD8(GetFileVersionInfoSizeExW)
FORWARD8(GetFileVersionInfoSizeW)
FORWARD8(GetFileVersionInfoW)
FORWARD8(VerFindFileA)
FORWARD8(VerFindFileW)
FORWARD8(VerInstallFileA)
FORWARD8(VerInstallFileW)
FORWARD8(VerLanguageNameA)
FORWARD8(VerLanguageNameW)
FORWARD8(VerQueryValueA)
FORWARD8(VerQueryValueW)
