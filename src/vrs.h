#pragma once
// Foveated rendering with D3D12 Variable Rate Shading tier 2 (shading-rate image). Game-independent core:
// the host tells it the render size and calls VrsOnSetRenderTargets from its OMSetRenderTargets hook; scene passes
// (render-size colour target + depth) get a per-tile rate image: 1x1 in the centre, 2x2 ring, 4x4 outside.
#include <d3d12.h>
namespace vrs {

struct Config {
    bool enabled = false;
    float inner = 0.6f;   // full-rate region (fraction of the image, width and height)
    float mid = 2.0f;     // 2x2 region (outside it: 4x4, or 2x2 if 4x4 is unsupported)
    bool coarseOuter = false;   // 4x4 outside the mid region
};
void SetConfig(const Config& c);
void SetRenderSize(unsigned w, unsigned h);          // the game's scene (DLSS input) size
void SetCenter(float u, float v);                     // fovea centre (uv), shared by both eyes
void OnSetRenderTargets(ID3D12GraphicsCommandList* l, unsigned rt0W, unsigned rt0H, bool hasDepth);
void OnReset(ID3D12GraphicsCommandList* l);
void Report();

}
