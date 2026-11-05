// Foveated rendering: D3D12 VRS tier 2 shading-rate image on the game's scene passes. See vrs.h.
#include <windows.h>
#include <d3d12.h>
#include <atomic>
#include <mutex>
#include <map>
#include <unordered_set>
#include <vector>
#include <algorithm>
#include <cmath>
#include "vrs.h"
#include "log.h"

namespace vrs {

static std::mutex g_mu;
static Config g_cfg;
static std::atomic<bool> g_on{false};
static std::atomic<unsigned> g_rw{0}, g_rh{0};
static float g_cu = 0.5f, g_cv = 0.5f;
static int g_tier = -1; static UINT g_tile = 16; static bool g_extra = false;
static ID3D12Device* g_dev = nullptr;
static ID3D12CommandQueue* g_q = nullptr; static ID3D12CommandAllocator* g_alloc = nullptr;
static ID3D12GraphicsCommandList* g_list = nullptr; static ID3D12Fence* g_fence = nullptr; static UINT64 g_fv = 0;
static std::map<uint64_t, ID3D12Resource*> g_images;   // key: size + centre (1/32 steps) + config -> rate image
static std::unordered_set<ID3D12GraphicsCommandList*> g_applied;
static std::atomic<uint64_t> s_set{0}, s_cleared{0}, s_built{0};
static DWORD g_lastBuild = 0;

void SetConfig(const Config& c) {
    std::lock_guard<std::mutex> l(g_mu);
    bool ch = c.enabled != g_cfg.enabled || c.inner != g_cfg.inner || c.mid != g_cfg.mid || c.coarseOuter != g_cfg.coarseOuter;
    g_cfg = c; g_on = c.enabled;
    if (ch) LOGI("vrs: %s, full rate %.2f, 2x2 to %.2f, outside %s", c.enabled ? "ON" : "off", c.inner, c.mid, c.coarseOuter ? "4x4" : "2x2");
}
void SetRenderSize(unsigned w, unsigned h) { g_rw = w; g_rh = h; }
void SetCenter(float u, float v) { std::lock_guard<std::mutex> l(g_mu); g_cu = u; g_cv = v; }

static bool Init(ID3D12GraphicsCommandList* cl) {
    if (g_tier >= 0) return g_tier >= 2;
    g_tier = 0;
    if (FAILED(cl->GetDevice(__uuidof(ID3D12Device), (void**)&g_dev)) || !g_dev) return false;
    D3D12_FEATURE_DATA_D3D12_OPTIONS6 o6{};
    if (SUCCEEDED(g_dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS6, &o6, sizeof(o6)))) {
        g_tier = o6.VariableShadingRateTier == D3D12_VARIABLE_SHADING_RATE_TIER_2 ? 2 : o6.VariableShadingRateTier == D3D12_VARIABLE_SHADING_RATE_TIER_1 ? 1 : 0;
        g_tile = o6.ShadingRateImageTileSize ? o6.ShadingRateImageTileSize : 16; g_extra = o6.AdditionalShadingRatesSupported;
    }
    LOGI("vrs: GPU VRS tier %d, tile %u px, 4x4 rates %s", g_tier, g_tile, g_extra ? "yes" : "no");
    if (g_tier < 2) return false;
    return true;
}

// Builds a rate image (R8_UINT, one texel per tile) with no GPU work: a CPU-writable custom-heap texture created
// directly in SHADING_RATE_SOURCE and filled with WriteToSubresource. (Test 160: building it with our own queue from
// inside the OMSetRenderTargets hook - ExecuteCommandLists through the mod's own ECL hook - killed the game.)
static ID3D12Resource* Build(unsigned rw, unsigned rh, float cu, float cv, const Config& c) {
    const UINT w = (rw + g_tile - 1) / g_tile, h = (rh + g_tile - 1) / g_tile;
    D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = w; d.Height = h; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.Format = DXGI_FORMAT_R8_UINT; d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    D3D12_HEAP_PROPERTIES hp = g_dev->GetCustomHeapProperties(0, D3D12_HEAP_TYPE_UPLOAD);
    hp.Type = D3D12_HEAP_TYPE_CUSTOM; hp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE; hp.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
    ID3D12Resource* img = nullptr;
    HRESULT hr = g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE, nullptr, __uuidof(ID3D12Resource), (void**)&img);
    if (FAILED(hr)) { LOGE("vrs: rate image %ux%u failed %08lX", w, h, hr); return nullptr; }
    std::vector<uint8_t> m((size_t)w * h);
    const uint8_t outer = c.coarseOuter && g_extra ? (uint8_t)D3D12_SHADING_RATE_4X4 : (uint8_t)D3D12_SHADING_RATE_2X2;
    for (UINT y = 0; y < h; y++) for (UINT x = 0; x < w; x++) {
        float u = ((x + 0.5f) * g_tile) / rw, v = ((y + 0.5f) * g_tile) / rh;
        float r = std::max(std::fabs(u - cu), std::fabs(v - cv)) * 2.0f;   // rectangles, as fovdlss
        m[(size_t)y * w + x] = r <= c.inner ? (uint8_t)D3D12_SHADING_RATE_1X1 : r <= c.mid ? (uint8_t)D3D12_SHADING_RATE_2X2 : outer;
    }
    hr = img->Map(0, nullptr, nullptr);
    if (SUCCEEDED(hr)) { hr = img->WriteToSubresource(0, nullptr, m.data(), w, w * h); img->Unmap(0, nullptr); }
    if (FAILED(hr)) { LOGE("vrs: rate image write failed %08lX", hr); img->Release(); return nullptr; }
    s_built++;
    return img;
}

void OnSetRenderTargets(ID3D12GraphicsCommandList* l, unsigned w, unsigned h, bool hasDepth) {
    const bool on = g_on.load();
    const unsigned rw = g_rw.load(), rh = g_rh.load();
    const bool scene = on && rw && w == rw && h == rh && hasDepth;
    bool applied;
    { std::lock_guard<std::mutex> lk(g_mu); applied = g_applied.count(l) != 0; }
    if (!scene && !applied) return;
    ID3D12GraphicsCommandList5* l5 = nullptr;
    if (FAILED(l->QueryInterface(__uuidof(ID3D12GraphicsCommandList5), (void**)&l5)) || !l5) return;
    if (scene) {
        ID3D12Resource* img = nullptr;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            if (Init(l)) {
                int qu = (int)std::lround(g_cu * 32), qv = (int)std::lround(g_cv * 32);
                uint64_t key = ((uint64_t)rw << 48) ^ ((uint64_t)rh << 32) ^ ((uint64_t)(qu & 0xFF) << 24) ^ ((uint64_t)(qv & 0xFF) << 16) ^
                               ((uint64_t)(g_cfg.inner * 100) << 8) ^ (uint64_t)(g_cfg.mid * 100) ^ (g_cfg.coarseOuter ? 1ull << 63 : 0);
                auto it = g_images.find(key);
                if (it != g_images.end()) img = it->second;
                else if (g_images.size() < 512 && GetTickCount() - g_lastBuild >= 20) {
                    g_lastBuild = GetTickCount();
                    img = Build(rw, rh, qu / 32.0f, qv / 32.0f, g_cfg);
                    if (img) g_images[key] = img;
                } else if (!g_images.empty()) img = g_images.rbegin()->second;   // (rare) reuse the newest while throttled
            }
        }
        if (img) {
            D3D12_SHADING_RATE_COMBINER comb[2] = {D3D12_SHADING_RATE_COMBINER_PASSTHROUGH, D3D12_SHADING_RATE_COMBINER_MAX};
            l5->RSSetShadingRate(D3D12_SHADING_RATE_1X1, comb);
            l5->RSSetShadingRateImage(img);
            std::lock_guard<std::mutex> lk(g_mu); g_applied.insert(l);
            s_set++;
        }
    } else {
        l5->RSSetShadingRateImage(nullptr);
        l5->RSSetShadingRate(D3D12_SHADING_RATE_1X1, nullptr);
        std::lock_guard<std::mutex> lk(g_mu); g_applied.erase(l);
        s_cleared++;
    }
    l5->Release();
}
void OnReset(ID3D12GraphicsCommandList* l) { std::lock_guard<std::mutex> lk(g_mu); g_applied.erase(l); }
void Report() {
    uint64_t a = s_set.exchange(0), b = s_cleared.exchange(0), c = s_built.exchange(0);
    if (g_on.load()) LOGI("vrs: rate image set on %llu scene passes, cleared %llu, %llu images built (render %ux%u) (10 s)",
                          (unsigned long long)a, (unsigned long long)b, (unsigned long long)c, g_rw.load(), g_rh.load());
}

}  // namespace vrs
