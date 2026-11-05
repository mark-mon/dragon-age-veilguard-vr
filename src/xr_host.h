#pragma once
// xr_host: game-independent OpenXR output for D3D12 games.
//
// The XR thread owns its own D3D11 device (on the game's GPU) and the OpenXR session, and runs the
// OpenXR frame loop at the headset rate, decoupled from the game. The game's render thread hands
// over images through a ring of 3 shared textures (created on the XR D3D11 device, opened in the
// game's D3D12 device), synchronised with two shared fences:
//   frame fence   : signalled by the game's queue after a copy, waited on by the XR context before reading
//   release fence : signalled by the XR context after reading a slot, waited on by the game's queue
//                   before it overwrites that slot.
// No CPU waits on the render thread (a slot whose previous copy has not finished is skipped).
//
// Config keys (G().cfg, re-applied with XrHostApplyConfig): vr_enabled, screen_distance, screen_width,
// screen_height, dump_frame. Keys: F9 = recentre (polled on the XR thread).
#ifndef XR_NO_PROTOTYPES
#define XR_NO_PROTOTYPES
#endif
#ifndef XR_USE_PLATFORM_WIN32
#define XR_USE_PLATFORM_WIN32
#endif
#ifndef XR_USE_GRAPHICS_API_D3D11
#define XR_USE_GRAPHICS_API_D3D11
#endif
#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <cstddef>

namespace davg {

// Reads the config keys into the host (call at load and after config.txt was reloaded).
void XrHostApplyConfig();

// Render thread, from the Present hook, before the original Present. `queue` is the D3D12 command queue
// the swapchain presents on (the copy is recorded on it, in order after the game's rendering). Starts the
// XR thread on the first call; afterwards copies the back buffer into the shared ring (flat screen).
void XrHostOnPresent(IDXGISwapChain* sc, ID3D12CommandQueue* queue);

// True while an OpenXR session is running (frame loop active).
bool XrHostRunning();

// ---- head tracking ----
// All poses exchanged with the game side are in the RECENTRED reference frame: yaw-only rotation + position of
// the head at session start / F9 / XrHostRecenter (the same reference the flat screen is placed in), so a
// recentred head looking straight ahead is at the origin facing -Z. The host converts to LOCAL for submission.
struct XrHostEye { XrPosef pose; XrFovf fov; };

// Display time `frames` headset frames after the last one the XR thread waited for (0 = no session yet).
XrTime XrHostPredictTime(float frames);
// Any thread: both eyes' poses (recentred frame) and FOVs at display time t. False if no session / tracking invalid.
bool XrHostLocateEyesAt(XrTime t, XrView out[2]);
// Same, at XrHostPredictTime(predictFrames).
bool XrHostLocateEyes(XrView views[2], float predictFrames = 1.0f);
// Per-eye FOVs of the headset (cached from the last locate); false until the first one.
bool XrHostHeadsetFov(XrFovf out[2]);

// Headset's recommended per-eye image size (false until the system was found).
bool XrHostRecommendedEyeSize(uint32_t& w, uint32_t& h);
// Headset frame period in ns (0 while no session is running).
int64_t XrHostDisplayPeriodNs();

// F9 / programmatic recentre: captures the reference (also done automatically when the session starts).
void XrHostRecenter();

// Writes the next new headset image (XR side, as submitted) to <game>\DAVGVR\screen_NNN.bmp
// (stereo: both eyes side by side, mean level per eye in the log).
void XrHostRequestDump();

// One-line summary of the producer side.
void XrHostReport(char* buf, size_t size);

// ---- mono head-tracked mode ----
// Render thread, any time before the Present of the frame these poses were rendered with (recentred frame).
// That Present then hands over the back buffer with these poses and it is shown as a projection layer (both
// eyes sample the full image, each with its own pose / FOV). Frames presented without a pose keep showing the
// last head-tracked frame; after 10 of them in a row the flat screen (quad) takes over again.
void XrHostSetFramePose(const XrHostEye eyes[2], bool sideBySide = false);   // sideBySide: left eye = left half of the back buffer

// ---- native stereo ----
// Render thread: hands over both eye images (D3D12 resources currently in `state`, same size / format; the state is
// restored after the copy) with the poses (recentred frame) and FOVs they were rendered with; recorded on `queue`
// (must be a DIRECT queue). Shown as a projection layer (eyes side by side in one shared texture of 2W x H). The
// next Present then does not hand over the back buffer. Returns false if the frame could not be handed over.
// Optional `hud`: the game's UI image (RGBA8, premultiplied alpha, in `hudState`), shown as its own quad layer
// (config hud_distance / hud_width / hud_height / hud_anchor world|head / hud_premultiplied; F10 shows / hides it).
bool XrHostPublishStereo(ID3D12CommandQueue* queue, ID3D12Resource* left, ID3D12Resource* right,
                         D3D12_RESOURCE_STATES state, const XrHostEye eyes[2],
                         ID3D12Resource* hud = nullptr, D3D12_RESOURCE_STATES hudState = D3D12_RESOURCE_STATE_COMMON);

// ---- later ----
// Settings menu: premultiplied RGBA8 image (D3D12) shown as a quad on top; null = hide. Not implemented yet (false).
bool XrHostPublishMenu(ID3D12CommandQueue* queue, ID3D12Resource* rgba8, D3D12_RESOURCE_STATES state);

} // namespace davg
