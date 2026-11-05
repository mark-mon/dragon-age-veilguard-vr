#pragma once
#include <d3d12.h>
namespace davg {
typedef void(STDMETHODCALLTYPE* PFN_ECL)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
// Command-list tracking + post-stage replay for native stereo (see replay.cpp).
bool ReplayInstall(ID3D12Device* dummyDevice);
// ExecuteCommandLists hook: executes the batch (and the replay, if any) itself through realEcl.
bool ReplayOnExecute(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists, PFN_ECL realEcl);
void ReplayOnPresent(ID3D12Resource* const* backBuffers, int count);
void ReplaySetStereoFrame(bool on);   // fbview: the frame being built renders two views
bool ReplayDidStereo();
void ReplaySetMode(int mode);         // 0 off, 1 stereo replay
void ReplayTraceArm(int frames);
void ReplayHudTraceArm();
void ReplaySetHudLayer(int on);
void ReplaySetSwapEyes(bool on);   // config swap_eyes (diagnostic)
void ReplayHudProbe();
// Present hook: the queue the swapchain presents on; only its batches are tracked / replayed.
void ReplaySetMainQueue(ID3D12CommandQueue* q);
// menu status (approximate, read from another thread)
uint64_t ReplayFrameCount();
bool ReplayLastFrameStereo();
bool ReplayHudActive();   // hudprobe.trigger: read back the output-size RGBA8 targets before the composite (rt8_cN_*.bmp)
// slhook: a DLSS evaluate for `eye` (0 left, 1 right) writing `output` was recorded into `cmdList` (ID3D12GraphicsCommandList*).
void ReplayNoteDlss(void* cmdList, int eye, void* output);
void ReplayNoteDlssDepth(void* cmdList, void* depth);
void ReplayNoteDlssCam(void* cmdList, const float posFwd[6]);   // left view's camera position of the frame this evaluate belongs to
// slhook: the DLSS output tagged on `cmdList` (the evaluate recorded into the same list writes it).
void ReplayNoteDlssOutput(void* cmdList, void* output);   // 1: UI on its own quad layer (stereo), 0: UI drawn into both eyes
void ReplaySetDebug(int debug, int startShift);
void ReplaySetSnapshot(int on);
void ReplayInputsDump();
void ReplayDlssDump();
void ReplayDlssDumpLite();
void ReplaySetVrs(bool on);
void ReplaySetPairStamp(bool on);   // no Streamline: pair frames with poses by the build serial at the upscaler list Reset
void ReplaySetLeftInputs(int on);
void ReplaySetSceneHistory(int on);   // per-view copies of the scene's temporal history (stereo_scene_history, default 1)
void ReplaySetHistory(int on);        // per-eye copies of temporal post history textures (stereo_replay_history, default 1)      // diagnostic: log full-size texture transitions per batch
}
