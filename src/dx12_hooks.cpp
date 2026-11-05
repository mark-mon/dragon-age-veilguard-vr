// D3D12 / DXGI hooks (phase 1: observe only).
// A private dummy D3D12 device + command queue + flip swapchain gives the vtables of the real
// implementations (D3D12Core / dxgi.dll); MinHook patches those functions, so every caller is seen,
// including Streamline's interposer which forwards to them.
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include "dx12_hooks.h"
#include "fbview.h"
#include "replay.h"
#include "window_size.h"
#include "dred.h"
#include "limiter.h"
#include "log.h"
#include "globals.h"
#include "xr_host.h"
#include "../third_party/minhook/include/MinHook.h"

namespace davg {

typedef HRESULT(STDMETHODCALLTYPE* PFN_Present)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* PFN_Present1)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_ResizeBuffers)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* PFN_ResizeBuffers1)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT, const UINT*, IUnknown* const*);
typedef void(STDMETHODCALLTYPE* PFN_ExecuteCommandLists)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);

static PFN_Present        o_Present;
static PFN_Present1       o_Present1;
static PFN_ResizeBuffers  o_ResizeBuffers;
static PFN_ResizeBuffers1 o_ResizeBuffers1;
static PFN_ExecuteCommandLists o_ECL;
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateSwapChain)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateSwapChainForHwnd)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
                                                               const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateSwapChainForComposition)(IDXGIFactory2*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*, IDXGISwapChain1**);
static PFN_CreateSwapChain o_CreateSwapChain;
static PFN_CreateSwapChainForHwnd o_CreateSwapChainForHwnd;
static PFN_CreateSwapChainForComposition o_CreateSwapChainForComposition;

// ---- swapchain -> the D3D12 command queue it presents on
static std::mutex g_scmu;
static std::unordered_map<IDXGISwapChain*, ID3D12CommandQueue*> g_scQueue;
static thread_local ID3D12CommandQueue* t_lastDirectQueue = nullptr;   // fallback: last DIRECT queue used on this thread

static void HookSwapchainVtable(IUnknown* sc);
static void NoteSwapchain(const char* how, IUnknown* dev, IUnknown* sc) {
    if (!sc) return;
    HookSwapchainVtable(sc);
    ID3D12CommandQueue* q = nullptr;
    if (dev && SUCCEEDED(dev->QueryInterface(__uuidof(ID3D12CommandQueue), (void**)&q)) && q) {
        q->Release();   // the swapchain keeps the queue alive
        IDXGISwapChain* s = nullptr;
        if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain), (void**)&s)) && s) {
            { std::lock_guard<std::mutex> l(g_scmu); g_scQueue[s] = q; }
            LOGI("%s: swapchain %p presents on queue %p", how, s, q);
            s->Release();
        }
    } else LOGW("%s: device argument %p is not an ID3D12CommandQueue (swapchain %p)", how, dev, sc);
}
static ID3D12CommandQueue* QueueFor(IDXGISwapChain* sc) {
    {
        std::lock_guard<std::mutex> l(g_scmu);
        auto it = g_scQueue.find(sc);
        if (it != g_scQueue.end()) return it->second;
    }
    static bool warned = false;
    if (!warned && t_lastDirectQueue) { warned = true; LOGW("swapchain %p: queue unknown, using the last DIRECT queue %p of the present thread", sc, t_lastDirectQueue); }
    return t_lastDirectQueue;
}
static HRESULT STDMETHODCALLTYPE Hk_CreateSwapChain(IDXGIFactory* f, IUnknown* dev, DXGI_SWAP_CHAIN_DESC* d, IDXGISwapChain** out) {
    HRESULT hr = o_CreateSwapChain(f, dev, d, out);
    if (SUCCEEDED(hr) && out) NoteSwapchain("CreateSwapChain", dev, *out);
    return hr;
}
static HRESULT STDMETHODCALLTYPE Hk_CreateSwapChainForHwnd(IDXGIFactory2* f, IUnknown* dev, HWND h, const DXGI_SWAP_CHAIN_DESC1* d,
                                                           const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* o, IDXGISwapChain1** out) {
    HRESULT hr = o_CreateSwapChainForHwnd(f, dev, h, d, fs, o, out);
    if (SUCCEEDED(hr) && out) NoteSwapchain("CreateSwapChainForHwnd", dev, *out);
    return hr;
}
static HRESULT STDMETHODCALLTYPE Hk_CreateSwapChainForComposition(IDXGIFactory2* f, IUnknown* dev, const DXGI_SWAP_CHAIN_DESC1* d, IDXGIOutput* o, IDXGISwapChain1** out) {
    HRESULT hr = o_CreateSwapChainForComposition(f, dev, d, o, out);
    if (SUCCEEDED(hr) && out) NoteSwapchain("CreateSwapChainForComposition", dev, *out);
    return hr;
}

// ---- stats
static std::atomic<uint64_t> g_presents{0}, g_ecl{0}, g_eclLists{0};
static std::mutex g_qmu;
struct QueueInfo { D3D12_COMMAND_LIST_TYPE type; uint64_t calls = 0, lists = 0; };
static std::unordered_map<ID3D12CommandQueue*, QueueInfo> g_queues;
static IDXGISwapChain* g_lastSc = nullptr;
static DWORD g_presentThread = 0;

static void DescribeSwapchain(IDXGISwapChain* sc, const char* why) {
    DXGI_SWAP_CHAIN_DESC d{};
    if (FAILED(sc->GetDesc(&d))) return;
    IDXGISwapChain1* sc1 = nullptr; DXGI_SWAP_CHAIN_DESC1 d1{};
    if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain1), (void**)&sc1))) { sc1->GetDesc1(&d1); sc1->Release(); }
    ID3D12Device* dev = nullptr; sc->GetDevice(__uuidof(ID3D12Device), (void**)&dev);
    RECT rc{}; GetClientRect(d.OutputWindow, &rc);
    LOGI("swapchain %p (%s): %ux%u fmt %d buffers %u swapEffect %d flags %X windowed %d hwnd %p client %ldx%ld scaling %d alpha %d d3d12dev %p (thread %lu)",
         sc, why, d.BufferDesc.Width, d.BufferDesc.Height, (int)d.BufferDesc.Format, d.BufferCount, (int)d.SwapEffect,
         d.Flags, d.Windowed, d.OutputWindow, rc.right, rc.bottom, (int)d1.Scaling, (int)d1.AlphaMode, dev, GetCurrentThreadId());
    if (dev) dev->Release();
    IDXGISwapChain3* sc3 = nullptr;
    if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain3), (void**)&sc3))) {
        ID3D12Resource* bb = nullptr;
        if (SUCCEEDED(sc3->GetBuffer(sc3->GetCurrentBackBufferIndex(), __uuidof(ID3D12Resource), (void**)&bb))) {
            D3D12_RESOURCE_DESC rd = bb->GetDesc();
            LOGI("  back buffer %p: %llux%u fmt %d flags %X", bb, (unsigned long long)rd.Width, rd.Height, (int)rd.Format, (unsigned)rd.Flags);
            bb->Release();
        }
        sc3->Release();
    }
}

static void OnPresent(IDXGISwapChain* sc, UINT sync, UINT flags) {
    {   // back buffers for the trace (cheap: GetBuffer just AddRefs)
        IDXGISwapChain3* sc3 = nullptr;
        if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain3), (void**)&sc3))) {
            DXGI_SWAP_CHAIN_DESC d{}; sc3->GetDesc(&d);
            ID3D12Resource* bbs[4] = {}; int cnt = (int)(d.BufferCount < 4 ? d.BufferCount : 4);
            for (int i = 0; i < cnt; i++) { sc3->GetBuffer(i, __uuidof(ID3D12Resource), (void**)&bbs[i]); if (bbs[i]) bbs[i]->Release(); }
            ReplaySetMainQueue(QueueFor(sc));
            ReplayOnPresent(bbs, cnt);
            sc3->Release();
        }
    }
    uint64_t n = g_presents.fetch_add(1) + 1;
    if (sc != g_lastSc) { g_lastSc = sc; DescribeSwapchain(sc, "first present"); }
    if (g_presentThread != GetCurrentThreadId()) {
        LOGI("present thread is %lu (was %lu), sync %u flags %X", GetCurrentThreadId(), g_presentThread, sync, flags);
        g_presentThread = GetCurrentThreadId();
    }
    static DWORD tLast = GetTickCount(); static uint64_t nLast = 0, eLast = 0;
    DWORD now = GetTickCount();
    if (now - tLast >= 10000) {
        double s = (now - tLast) / 1000.0;
        LOGI("stats: %.1f fps, %.0f ExecuteCommandLists/s (%llu presents total)", (n - nLast) / s, (g_ecl.load() - eLast) / s, (unsigned long long)n);
        {
            std::lock_guard<std::mutex> l(g_qmu);
            for (auto& q : g_queues) LOGI("  queue %p type %d: %llu calls, %llu lists", q.first, (int)q.second.type,
                                          (unsigned long long)q.second.calls, (unsigned long long)q.second.lists);
        }
        tLast = now; nLast = n; eLast = g_ecl.load();
    }
}

static HRESULT STDMETHODCALLTYPE Hk_Present(IDXGISwapChain* sc, UINT sync, UINT flags) {
    DredPoll(sc);
    if (!(flags & DXGI_PRESENT_TEST)) { OnPresent(sc, sync, flags); FbViewOnPresent(); XrHostOnPresent(sc, QueueFor(sc)); WindowSizeOnPresent(sc, FbViewMode() == 2 && XrHostRunning()); }
    if (!(flags & DXGI_PRESENT_TEST)) LimiterOnPresent();
    HRESULT hr = o_Present(sc, sync, flags);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DEVICE_HUNG) DredReport(sc, hr);
    return hr;
}
static HRESULT STDMETHODCALLTYPE Hk_Present1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* p) {
    DredPoll(sc);
    if (!(flags & DXGI_PRESENT_TEST)) { OnPresent(sc, sync, flags); FbViewOnPresent(); XrHostOnPresent(sc, QueueFor(sc)); WindowSizeOnPresent(sc, FbViewMode() == 2 && XrHostRunning()); }
    if (!(flags & DXGI_PRESENT_TEST)) LimiterOnPresent();
    HRESULT hr = o_Present1(sc, sync, flags, p);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DEVICE_HUNG) DredReport(sc, hr);
    return hr;
}
static HRESULT STDMETHODCALLTYPE Hk_ResizeBuffers(IDXGISwapChain* sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT fl) {
    LOGI("ResizeBuffers(%p, %u, %ux%u, fmt %d, flags %X)", sc, n, w, h, (int)f, fl);
    HRESULT hr = o_ResizeBuffers(sc, n, w, h, f, fl);
    if (SUCCEEDED(hr)) DescribeSwapchain(sc, "after resize"); else LOGW("ResizeBuffers failed %08lX", hr);
    return hr;
}
static HRESULT STDMETHODCALLTYPE Hk_ResizeBuffers1(IDXGISwapChain3* sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT fl, const UINT* m, IUnknown* const* q) {
    LOGI("ResizeBuffers1(%p, %u, %ux%u, fmt %d, flags %X)", sc, n, w, h, (int)f, fl);
    HRESULT hr = o_ResizeBuffers1(sc, n, w, h, f, fl, m, q);
    if (SUCCEEDED(hr)) DescribeSwapchain(sc, "after resize1"); else LOGW("ResizeBuffers1 failed %08lX", hr);
    return hr;
}
static void STDMETHODCALLTYPE Hk_ECL(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists) {
    g_ecl.fetch_add(1); g_eclLists.fetch_add(n);
    {
        std::lock_guard<std::mutex> l(g_qmu);
        auto it = g_queues.find(q);
        if (it == g_queues.end()) {
            D3D12_COMMAND_QUEUE_DESC d = q->GetDesc();
            it = g_queues.emplace(q, QueueInfo{d.Type}).first;
            LOGI("command queue %p: type %d priority %d flags %X node %X (thread %lu)", q, (int)d.Type, d.Priority, (unsigned)d.Flags, d.NodeMask, GetCurrentThreadId());
        }
        it->second.calls++; it->second.lists += n;
        if (it->second.type == D3D12_COMMAND_LIST_TYPE_DIRECT) t_lastDirectQueue = q;
    }
    if (!ReplayOnExecute(q, n, lists, o_ECL)) o_ECL(q, n, lists);
}

static bool Hook(void* target, void* det, void** orig, const char* name) {
    MH_STATUS s = MH_CreateHook(target, det, orig);
    if (s == MH_OK) s = MH_EnableHook(target);
    if (s != MH_OK) { LOGE("hook %s at %p failed: %d", name, target, (int)s); return false; }
    HMODULE m = nullptr; char path[MAX_PATH] = "?";
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)target, &m)) GetModuleFileNameA(m, path, MAX_PATH);
    LOGI("hooked %s at %p (%s+%llX)", name, target, strrchr(path, '\\') ? strrchr(path, '\\') + 1 : path, (unsigned long long)((char*)target - (char*)m));
    return true;
}

// Start-up race (2026-11-04): the old install made its own dummy D3D12 device + a dummy swapchain through Streamline's
// DXGI proxy on a side thread while the game initialised -> the game vanished on many launches (tests 130, 146: 4/5;
// 148: 5/5 once the dummy came after the game's device). Now: hook D3D12CreateDevice early; when the game's first device
// is created, install everything from THAT device on the game's thread (factory hooks, ExecuteCommandLists from our own
// queue on its device, command list hooks), and hook the swapchain vtable from the game's own swapchain when it is
// created (CreateSwapChain* hooks). No dummy device, no dummy swapchain.
typedef HRESULT(WINAPI* PFN_CreateDev)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
static PFN_CreateDev o_CreateDev;
static std::atomic<bool> g_gameDevice{false}, g_installed{false};
static bool InstallFromDevice(ID3D12Device* dev);
static HRESULT WINAPI Hk_CreateDev(IUnknown* ad, D3D_FEATURE_LEVEL fl, REFIID riid, void** out) {
    HRESULT hr = o_CreateDev(ad, fl, riid, out);
    if (out && SUCCEEDED(hr) && *out && !g_gameDevice.exchange(true)) {
        LOGI("dx12 hooks: the game created its D3D12 device (thread %lu) - installing from it", GetCurrentThreadId());
        ID3D12Device* d = nullptr;
        if (SUCCEEDED(((IUnknown*)*out)->QueryInterface(__uuidof(ID3D12Device), (void**)&d)) && d) {
            if (!g_installed.exchange(true)) InstallFromDevice(d);
            d->Release();
        }
    }
    return hr;
}
bool Dx12HookCreateDevice() {
    HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
    void* t = d3d12 ? (void*)GetProcAddress(d3d12, "D3D12CreateDevice") : nullptr;
    if (!t) return false;
    MH_STATUS st = MH_CreateHook(t, (void*)Hk_CreateDev, (void**)&o_CreateDev);
    if (st == MH_OK) st = MH_EnableHook(t);
    LOGI("dx12 hooks: D3D12CreateDevice %s", st == MH_OK ? "hooked (hooks follow the game's own device)" : "NOT hooked");
    return st == MH_OK;
}
bool Dx12WaitGameDevice(DWORD ms) {   // true: installed from the game's device
    DWORD t0 = GetTickCount();
    while (!g_installed.load() && GetTickCount() - t0 < ms) Sleep(10);
    return g_installed.load();
}
static std::atomic<bool> g_scHooked{false};
static void HookSwapchainVtable(IUnknown* scu) {
    if (g_scHooked.exchange(true)) return;
    IDXGISwapChain* sc = nullptr;
    if (FAILED(scu->QueryInterface(__uuidof(IDXGISwapChain), (void**)&sc)) || !sc) { g_scHooked = false; return; }
    void** vs = *(void***)sc;
    Hook(vs[8], (void*)Hk_Present, (void**)&o_Present, "IDXGISwapChain::Present");
    Hook(vs[13], (void*)Hk_ResizeBuffers, (void**)&o_ResizeBuffers, "IDXGISwapChain::ResizeBuffers");
    IDXGISwapChain1* sc1 = nullptr;
    if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain1), (void**)&sc1)) && sc1) { Hook((*(void***)sc1)[22], (void*)Hk_Present1, (void**)&o_Present1, "IDXGISwapChain1::Present1"); sc1->Release(); }
    IDXGISwapChain3* sc3 = nullptr;
    if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain3), (void**)&sc3)) && sc3) { Hook((*(void***)sc3)[39], (void*)Hk_ResizeBuffers1, (void**)&o_ResizeBuffers1, "IDXGISwapChain3::ResizeBuffers1"); sc3->Release(); }
    sc->Release();
    LOGI("dx12 hooks: swapchain hooks from the game's swapchain");
}
static bool InstallFromDevice(ID3D12Device* dev) {
    HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
    auto pCreateFactory = (HRESULT(WINAPI*)(UINT, REFIID, void**))GetProcAddress(dxgi, "CreateDXGIFactory2");
    IDXGIFactory2* f2 = nullptr;
    if (pCreateFactory && SUCCEEDED(pCreateFactory(0, __uuidof(IDXGIFactory2), (void**)&f2)) && f2) {
        void** vf = *(void***)f2;
        Hook(vf[10], (void*)Hk_CreateSwapChain, (void**)&o_CreateSwapChain, "IDXGIFactory::CreateSwapChain");
        Hook(vf[15], (void*)Hk_CreateSwapChainForHwnd, (void**)&o_CreateSwapChainForHwnd, "IDXGIFactory2::CreateSwapChainForHwnd");
        Hook(vf[24], (void*)Hk_CreateSwapChainForComposition, (void**)&o_CreateSwapChainForComposition, "IDXGIFactory2::CreateSwapChainForComposition");
        f2->Release();
    } else LOGE("dx12 hooks: no DXGI factory - swapchain hooks missing");
    D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* q = nullptr;
    bool ok = false;
    if (SUCCEEDED(dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), (void**)&q)) && q) {
        ok = Hook((*(void***)q)[10], (void*)Hk_ECL, (void**)&o_ECL, "ID3D12CommandQueue::ExecuteCommandLists");
        q->Release();
    }
    ReplayInstall(dev);
    LOGI("dx12 hooks %s (from the game's device)", ok ? "installed" : "partly failed");
    return ok;
}

bool InstallDx12Hooks() {
    if (g_installed.exchange(true)) return true;
    g_scHooked = true;   // this path hooks the swapchain vtable from its dummy swapchain
    HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
    HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
    auto pCreateDevice = (PFN_D3D12_CREATE_DEVICE)GetProcAddress(d3d12, "D3D12CreateDevice");
    auto pCreateFactory = (HRESULT(WINAPI*)(UINT, REFIID, void**))GetProcAddress(dxgi, "CreateDXGIFactory2");
    if (!pCreateDevice || !pCreateFactory) { LOGE("dx12 hooks: D3D12CreateDevice / CreateDXGIFactory2 not found"); return false; }

    DredEnable();
    LOGI("dx12 hooks: creating the dummy device");
    IDXGIFactory4* fac = nullptr;
    if (FAILED(pCreateFactory(0, __uuidof(IDXGIFactory4), (void**)&fac))) { LOGE("dx12 hooks: factory failed"); return false; }
    IDXGIAdapter1* ad = nullptr;
    fac->EnumAdapters1(0, &ad);
    ID3D12Device* dev = nullptr;
    HRESULT hr = pCreateDevice(ad, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), (void**)&dev);
    if (ad) ad->Release();
    if (FAILED(hr)) { LOGE("dx12 hooks: dummy device failed %08lX", hr); fac->Release(); return false; }
    D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* q = nullptr;
    dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), (void**)&q);

    WNDCLASSEXW wc{ sizeof(wc) }; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = G().self; wc.lpszClassName = L"DAVGVR_dummy";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    DXGI_SWAP_CHAIN_DESC1 sd{}; sd.Width = 64; sd.Height = 64; sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = 2; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain1* sc = nullptr;
    hr = q ? fac->CreateSwapChainForHwnd(q, hwnd, &sd, nullptr, nullptr, &sc) : E_FAIL;
    if (FAILED(hr) || !sc) { LOGE("dx12 hooks: dummy swapchain failed %08lX", hr); }
    else LOGI("dx12 hooks: dummy swapchain created");

    // Factory: record which queue each swapchain presents on (IDXGIFactory slot 10, IDXGIFactory2 slots 15 / 24).
    {
        IDXGIFactory2* f2 = nullptr;
        if (SUCCEEDED(fac->QueryInterface(__uuidof(IDXGIFactory2), (void**)&f2))) {
            void** vf = *(void***)f2;
            Hook(vf[10], (void*)Hk_CreateSwapChain, (void**)&o_CreateSwapChain, "IDXGIFactory::CreateSwapChain");
            Hook(vf[15], (void*)Hk_CreateSwapChainForHwnd, (void**)&o_CreateSwapChainForHwnd, "IDXGIFactory2::CreateSwapChainForHwnd");
            Hook(vf[24], (void*)Hk_CreateSwapChainForComposition, (void**)&o_CreateSwapChainForComposition, "IDXGIFactory2::CreateSwapChainForComposition");
            f2->Release();
        }
    }
    void** vq = *(void***)q;
    bool ok = Hook(vq[10], (void*)Hk_ECL, (void**)&o_ECL, "ID3D12CommandQueue::ExecuteCommandLists");
    if (sc) {
        void** vs = *(void***)sc;
        ok &= Hook(vs[8], (void*)Hk_Present, (void**)&o_Present, "IDXGISwapChain::Present");
        ok &= Hook(vs[13], (void*)Hk_ResizeBuffers, (void**)&o_ResizeBuffers, "IDXGISwapChain::ResizeBuffers");
        ok &= Hook(vs[22], (void*)Hk_Present1, (void**)&o_Present1, "IDXGISwapChain1::Present1");
        IDXGISwapChain3* sc3 = nullptr;
        if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain3), (void**)&sc3))) {
            ok &= Hook((*(void***)sc3)[39], (void*)Hk_ResizeBuffers1, (void**)&o_ResizeBuffers1, "IDXGISwapChain3::ResizeBuffers1");
            sc3->Release();
        }
        // The dummy swapchain was made through Streamline's DXGI proxy. Releasing it (and its window) while the game creates
        // its own device / swapchain on other threads is the last thing before the intermittent silent start-up exit
        // (user 2026-11-04 "every other launch"; dev PC test 130): keep both alive (64x64, never presented).
    }
    LOGI("dx12 hooks: swapchain hooks done, installing command list hooks");
    ReplayInstall(dev);
    LOGI("dx12 hooks: command list hooks done");
    dev->Release();
    fac->Release();
    LOGI("dx12 hooks %s", ok ? "installed" : "partly failed");
    return ok;
}

} // namespace davg
