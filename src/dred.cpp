// D3D12 Device Removed Extended Data (DRED): breadcrumbs + page fault info, logged when the device is removed.
// Enabled before any D3D12 device is created. config dred = 1 (default OFF since 2026-10-30: the user's 5090 bluescreened
// with it forced on; it changes how the driver tracks every allocation - turn it on only to diagnose a device removal).
#include <windows.h>
#include <d3d12.h>
#include <dxgi.h>
#include <string>
#include <atomic>
#include "dred.h"
#include "log.h"
#include "globals.h"

namespace davg {

static std::atomic<bool> g_reported{false};

void DredEnable() {
    if (!G().cfg.GetBool("dred", false)) return;
    HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
    auto get = d3d12 ? (HRESULT(WINAPI*)(REFIID, void**))GetProcAddress(d3d12, "D3D12GetDebugInterface") : nullptr;
    if (!get) { LOGW("dred: D3D12GetDebugInterface not found"); return; }
    ID3D12DeviceRemovedExtendedDataSettings1* s1 = nullptr;
    ID3D12DeviceRemovedExtendedDataSettings* s = nullptr;
    if (SUCCEEDED(get(__uuidof(ID3D12DeviceRemovedExtendedDataSettings1), (void**)&s1)) && s1) {
        s1->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        s1->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        s1->SetBreadcrumbContextEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        s1->Release();
        LOGI("dred: auto breadcrumbs + page faults + context on");
    } else if (SUCCEEDED(get(__uuidof(ID3D12DeviceRemovedExtendedDataSettings), (void**)&s)) && s) {
        s->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        s->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        s->Release();
        LOGI("dred: auto breadcrumbs + page faults on");
    } else LOGW("dred: settings interface not available");
}

static const char* OpName(D3D12_AUTO_BREADCRUMB_OP op) {
    static const char* n[] = {"SetMarker", "BeginEvent", "EndEvent", "DrawInstanced", "DrawIndexedInstanced", "ExecuteIndirect",
        "Dispatch", "CopyBufferRegion", "CopyTextureRegion", "CopyResource", "CopyTiles", "ResolveSubresource", "ClearRTV",
        "ClearUAV", "ClearDSV", "ResourceBarrier", "ExecuteBundle", "Present", "ResolveQueryData", "BeginSubmission",
        "EndSubmission", "DecodeFrame", "ProcessFrames", "AtomicCopyBufferUINT", "AtomicCopyBufferUINT64",
        "ResolveSubresourceRegion", "WriteBufferImmediate", "DecodeFrame1", "SetProtectedResourceSession", "DecodeFrame2",
        "ProcessFrames1", "BuildRaytracingAS", "EmitRaytracingASPostbuild", "CopyRaytracingAS", "DispatchRays",
        "InitializeMetaCommand", "ExecuteMetaCommand", "EstimateMotion", "ResolveMotionVectorHeap", "SetPipelineState1",
        "InitializeExtensionCommand", "ExecuteExtensionCommand", "DispatchMesh", "EncodeFrame", "ResolveEncoderOutputMetadata"};
    return (unsigned)op < sizeof(n) / sizeof(n[0]) ? n[op] : "?";
}
static std::string Name(const char* a, const wchar_t* w) {
    if (a && *a) return a;
    if (w && *w) { std::string s; for (; *w && s.size() < 80; ++w) s += (char)(*w < 128 ? *w : '?'); return s; }
    return "";
}

void DredReport(IDXGISwapChain* sc, HRESULT presentHr) {
    if (g_reported.exchange(true)) return;
    ID3D12Device* dev = nullptr;
    if (!sc || FAILED(sc->GetDevice(__uuidof(ID3D12Device), (void**)&dev)) || !dev) { LOGE("dred: present failed %08lX, no device", presentHr); return; }
    HRESULT reason = dev->GetDeviceRemovedReason();
    LOGE("dred: DEVICE REMOVED (present %08lX, reason %08lX)", presentHr, reason);
    ID3D12DeviceRemovedExtendedData1* d1 = nullptr;
    if (FAILED(dev->QueryInterface(__uuidof(ID3D12DeviceRemovedExtendedData1), (void**)&d1)) || !d1) { LOGE("dred: no DRED data"); dev->Release(); LogFlush(); return; }
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 bo{};
    if (SUCCEEDED(d1->GetAutoBreadcrumbsOutput1(&bo))) {
        int nodes = 0;
        for (const D3D12_AUTO_BREADCRUMB_NODE1* nd = bo.pHeadAutoBreadcrumbNode; nd && nodes < 64; nd = nd->pNext, ++nodes) {
            UINT done = nd->pLastBreadcrumbValue ? *nd->pLastBreadcrumbValue : 0;
            if (done >= nd->BreadcrumbCount) continue;   // finished lists are not interesting
            LOGE("dred: list %p '%s' queue %p '%s': %u of %u ops done", nd->pCommandList,
                 Name(nd->pCommandListDebugNameA, nd->pCommandListDebugNameW).c_str(), nd->pCommandQueue,
                 Name(nd->pCommandQueueDebugNameA, nd->pCommandQueueDebugNameW).c_str(), done, nd->BreadcrumbCount);
            UINT from = done > 6 ? done - 6 : 0, to = done + 6 < nd->BreadcrumbCount ? done + 6 : nd->BreadcrumbCount;
            std::string ops;
            for (UINT i = from; i < to; i++) { ops += (i == done ? "[>" : ""); ops += OpName(nd->pCommandHistory[i]); ops += (i == done ? "<]" : ""); ops += " "; }
            LOGE("dred:   ops %u..%u: %s", from, to, ops.c_str());
            for (UINT c = 0; c < nd->BreadcrumbContextsCount && c < 8; c++)
                LOGE("dred:   context at op %u: %S", nd->pBreadcrumbContexts[c].BreadcrumbIndex, nd->pBreadcrumbContexts[c].pContextString);
        }
        LOGE("dred: %d breadcrumb nodes", nodes);
    } else LOGE("dred: no breadcrumbs");
    D3D12_DRED_PAGE_FAULT_OUTPUT1 pf{};
    if (SUCCEEDED(d1->GetPageFaultAllocationOutput1(&pf))) {
        LOGE("dred: page fault VA %016llX", (unsigned long long)pf.PageFaultVA);
        int k = 0;
        for (auto* a = pf.pHeadExistingAllocationNode; a && k < 12; a = a->pNext, ++k)
            LOGE("dred:   existing allocation '%s' type %d object %p", Name(a->ObjectNameA, a->ObjectNameW).c_str(), (int)a->AllocationType, a->pObject);
        k = 0;
        for (auto* a = pf.pHeadRecentFreedAllocationNode; a && k < 12; a = a->pNext, ++k)
            LOGE("dred:   recently freed '%s' type %d object %p", Name(a->ObjectNameA, a->ObjectNameW).c_str(), (int)a->AllocationType, a->pObject);
    }
    d1->Release(); dev->Release();
    LogFlush();
}

// Render thread, every Present (cheap: every 30th call asks the device).
void DredPoll(IDXGISwapChain* sc) {
    static unsigned n = 0;
    if (g_reported.load() || (++n % 30) != 0 || !sc) return;
    ID3D12Device* dev = nullptr;
    if (FAILED(sc->GetDevice(__uuidof(ID3D12Device), (void**)&dev)) || !dev) return;
    HRESULT r = dev->GetDeviceRemovedReason();
    dev->Release();
    if (r != S_OK) DredReport(sc, r);
}

} // namespace davg
