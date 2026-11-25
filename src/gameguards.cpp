// Game crash guards. See gameguards.h.
//
// Emitter resource guard (2026-11-24, user crashes 2026-11-05 and 2026-11-24, same instruction): the effects system
// (code that reads EmitterSystemSettings, exe+1C3F370 in the Steam build of 2026-10) walks an emitter's resource slots
// and calls exe+1C46C20(self, resource, out) for each; it compares a slot with the settings' default object, not with
// null, so an empty slot crashes at the first instruction (mov rax,[rdx]). The callee only fills a small descriptor at
// `out` from the resource, so with a null resource we skip it (out keeps its previous contents) and count it.
#include <windows.h>
#include <atomic>
#include "gameguards.h"
#include "resolver.h"
#include "globals.h"
#include "log.h"
#include "../third_party/minhook/include/MinHook.h"

namespace davg {

using PFN_EmitterRes = void(__fastcall*)(void* self, void* res, void* out);
static PFN_EmitterRes o_EmitterRes = nullptr;
static std::atomic<bool> g_emitterOn{true};
static std::atomic<uint64_t> g_emitterSkips{0}, g_emitterSkipsTotal{0};

static void __fastcall Hk_EmitterRes(void* self, void* res, void* out) {
    if (!res && g_emitterOn.load(std::memory_order_relaxed)) {
        if (g_emitterSkipsTotal++ == 0) LOGW("guard: effects system passed an empty resource slot - skipped (would have crashed the game)");
        g_emitterSkips++;
        return;
    }
    o_EmitterRes(self, res, out);
}

void GameGuardsApplyConfig() {
    bool on = G().cfg.GetBool("crash_guard_emitter", true);
    if (on != g_emitterOn.exchange(on)) LOGI("guard: emitter resource guard %s", on ? "on" : "off");
}

void GameGuardsInstall() {
    GameGuardsApplyConfig();
    void* p = ResolveSig("EmitterResourceDesc",
        "48 89 5C 24 08 48 89 74 24 18 57 48 83 EC 20 48 8B 02 48 8B F1 48 8B CA 49 8B D8 48 8B FA FF 50 10 83 C0 F9 83 F8 2A 77");
    ResolverSaveCache();
    if (!p) { LOGW("guard: emitter resource function not found in this game build - guard not installed"); return; }
    if (MH_CreateHook(p, (void*)Hk_EmitterRes, (void**)&o_EmitterRes) != MH_OK || MH_EnableHook(p) != MH_OK) {
        LOGE("guard: emitter resource hook failed"); return;
    }
    LOGI("guard: emitter resource guard installed (exe+%llX), %s", (unsigned long long)((char*)p - (char*)GetModuleHandleW(nullptr)),
         g_emitterOn.load() ? "on" : "off");
}

void GameGuardsReport() {
    uint64_t n = g_emitterSkips.exchange(0);
    if (n) LOGW("guard: %llu empty effect resource slots skipped (10 s), %llu since start", (unsigned long long)n,
                (unsigned long long)g_emitterSkipsTotal.load());
}

}  // namespace davg
