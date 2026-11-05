// Build without the settings menu (NOMENU=1 ./build.sh): stubs for xr_host.
#include <d3d11.h>
#include "menu.h"
namespace davg {
bool MenuInit(ID3D11Device*, ID3D11DeviceContext*) { return false; }
bool MenuFrame() { return false; }
ID3D11Texture2D* MenuTexture() { return nullptr; }
const vrkit::menu::Placement& MenuPlacement() { static vrkit::menu::Placement p; return p; }
}
namespace vrkit { namespace menu { bool IsOpen() { return false; } } }
