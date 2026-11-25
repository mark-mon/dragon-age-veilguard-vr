#pragma once
// Guards against crashes inside the game's own code (found by signature, toggled by config / Debug menu).
namespace davg {
void GameGuardsInstall();      // after MinHook init, once the exe is unpacked
void GameGuardsApplyConfig();  // crash_guard_emitter (default on)
void GameGuardsReport();       // 10 s stats
}
