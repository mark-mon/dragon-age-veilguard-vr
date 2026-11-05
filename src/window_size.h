#pragma once
#include <dxgi.h>
namespace davg {
void WindowSizeApplyConfig();
// Render thread, every Present. vrActive: a VR mode is on and the headset is running.
void WindowSizeOnPresent(IDXGISwapChain* sc, bool vrActive);
}
