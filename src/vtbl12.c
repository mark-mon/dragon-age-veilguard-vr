/* D3D12 vtable slot indices from the SDK's C declarations (never hand-maintained). */
#define CINTERFACE
#define COBJMACROS
#include <stddef.h>
#include <d3d12.h>
const int VT12_CL_Close = (int)(offsetof(ID3D12GraphicsCommandListVtbl, Close) / sizeof(void*));
const int VT12_CL_Reset = (int)(offsetof(ID3D12GraphicsCommandListVtbl, Reset) / sizeof(void*));
const int VT12_CL_ResourceBarrier = (int)(offsetof(ID3D12GraphicsCommandListVtbl, ResourceBarrier) / sizeof(void*));
const int VT12_CL_CopyResource = (int)(offsetof(ID3D12GraphicsCommandListVtbl, CopyResource) / sizeof(void*));
const int VT12_CL_CopyTextureRegion = (int)(offsetof(ID3D12GraphicsCommandListVtbl, CopyTextureRegion) / sizeof(void*));
const int VT12_CL_OMSetRenderTargets = (int)(offsetof(ID3D12GraphicsCommandListVtbl, OMSetRenderTargets) / sizeof(void*));
const int VT12_CL_DrawInstanced = (int)(offsetof(ID3D12GraphicsCommandListVtbl, DrawInstanced) / sizeof(void*));
const int VT12_CL_DrawIndexedInstanced = (int)(offsetof(ID3D12GraphicsCommandListVtbl, DrawIndexedInstanced) / sizeof(void*));
const int VT12_CL_ClearRenderTargetView = (int)(offsetof(ID3D12GraphicsCommandListVtbl, ClearRenderTargetView) / sizeof(void*));
const int VT12_DEV_CreateRenderTargetView = (int)(offsetof(ID3D12DeviceVtbl, CreateRenderTargetView) / sizeof(void*));
const int VT12_DEV_GetDescriptorHandleIncrementSize = (int)(offsetof(ID3D12DeviceVtbl, GetDescriptorHandleIncrementSize) / sizeof(void*));
const int VT12_CL_Dispatch = (int)(offsetof(ID3D12GraphicsCommandListVtbl, Dispatch) / sizeof(void*));
