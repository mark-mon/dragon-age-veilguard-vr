#pragma once
#include <windows.h>
#include <dxgi.h>
namespace davg {
void DredEnable();                                  // before any D3D12 device exists
void DredReport(IDXGISwapChain* sc, HRESULT hr);
void DredPoll(IDXGISwapChain* sc);                  // every Present: reports once if the device was removed    // once, after Present reported a removed device
}
