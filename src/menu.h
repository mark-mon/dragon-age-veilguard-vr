#pragma once
#include <d3d11.h>
#include "vrkit_menu_placement.h"
namespace davg {
// XR thread (owner of the D3D11 device / immediate context).
bool MenuInit(ID3D11Device* dev, ID3D11DeviceContext* ctx);   // once; registers the options (config key menu_enabled = 0 skips it)
bool MenuFrame();                                              // every headset frame; true while open (texture redrawn)
ID3D11Texture2D* MenuTexture();                                // R8G8B8A8_TYPELESS, premultiplied alpha
const vrkit::menu::Placement& MenuPlacement();
}
