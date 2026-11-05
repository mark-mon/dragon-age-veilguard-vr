#pragma once
namespace davg {
void LimiterApplyConfig();   // game_fps_divider
void LimiterOnPresent();     // render thread, in the Present hook before the real Present
}
