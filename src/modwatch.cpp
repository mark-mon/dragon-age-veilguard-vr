// Logs every DLL the process loads (name, base, size, time since our load) through the loader's
// DLL notification callback. Useful to see what the game, Streamline and overlays load and when.
#include <windows.h>
#include <winternl.h>
#include "modwatch.h"
#include "log.h"

namespace davg {

typedef struct { ULONG Flags; const UNICODE_STRING* FullDllName; const UNICODE_STRING* BaseDllName; PVOID DllBase; ULONG SizeOfImage; } LDR_DLL_NOTIF_DATA;
typedef VOID(CALLBACK* LDR_DLL_NOTIFICATION_FUNCTION)(ULONG reason, const LDR_DLL_NOTIF_DATA* data, PVOID ctx);
typedef NTSTATUS(NTAPI* PFN_LdrRegisterDllNotification)(ULONG, LDR_DLL_NOTIFICATION_FUNCTION, PVOID, PVOID*);

static DWORD g_t0;
static void* g_cookie;

static VOID CALLBACK OnDll(ULONG reason, const LDR_DLL_NOTIF_DATA* d, PVOID) {
    if (reason != 1 || !d || !d->FullDllName) return;   // 1 = loaded
    LOGI("module load +%5lums %p %08lX %.*S", GetTickCount() - g_t0, d->DllBase, d->SizeOfImage,
         (int)(d->FullDllName->Length / 2), d->FullDllName->Buffer);
}

void ModWatchStart() {
    g_t0 = GetTickCount();
    auto reg = (PFN_LdrRegisterDllNotification)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "LdrRegisterDllNotification");
    if (reg && reg(0, OnDll, nullptr, &g_cookie) == 0) LOGI("module watch on");
    else LOGW("module watch: LdrRegisterDllNotification not available");
}

} // namespace davg
