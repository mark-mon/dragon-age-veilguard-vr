# Dragon Age: The Veilguard VR (DAVGVR) - findings

Project: D:\vrconversions\dragonageveilgard (agent\, davgvr\ source, builds\). Game: D:\SteamLibrary\steamapps\common\Dragon Age The Veilguard.
Next games in the series plan: Inquisition (Frostbite 2014, D3D11, 64-bit - closest to Mass Effect Andromeda),
Dragon Age 2 and Origins (Eclipse engine, 32-bit, D3D9 - xrbridge route). Keep game-independent code separate.

## Machines
- Dev/test PC GAMEPC5070: RTX 5070, SteamVR null driver (set by Enable-NullHmd, backup steamvr.vrsettings.davgbak), no headset.
- Final testing by the user on a separate PC with an RTX 5090. Steam account shared; user keeps Steam in offline mode.

## Executable (2026-10-29)
- `Dragon Age The Veilguard.exe`, 110 MB, Steam app 1845910, Frostbite **2021-2.3**, built 2025-01-15.
- Fixed base 0x140000000 (no DYNAMIC_BASE). SteamStub wrapper (`.bind` section, entry in .bind) but `.text` is not
  encrypted on disk (normal code statistics). Extra code section `ctr` (0xC25000 bytes) - unknown yet.
- Reflection sections `typeinfo` / `fieldinf` intact: `tools/fb21_sdk.py` dumps ~7000 types (from the disk image too;
  field type names need the runtime image).
  TypeInfoData: +0 name, +8 hash, +0xC u16 flags, +0xE u16 size, +0x10 GUID, +0x28 ?, +0x32 u16 field count,
  +0x38 runtime TypeInfo* (of the super class?), +0x40 fields. FieldInfoData 0x18: name, hash, u16 flags, u16 offset, TypeInfo*.
- D3D12 renderer lives in `Engine.Render.Core2.PlatformPcDx12.dll` (unprotected, exports DeviceInit / DeviceShutdown ...).
- Imports by name: VERSION.dll (exe and render DLL) -> **proxy = version.dll**; also DINPUT8, WINMM, dxgi, d3d11 (one call).
- Streamline (sl.interposer, DLSS, DLSS-G frame generation, Reflex, PCL), XeSS, FSR2 exports, NRD, Aftermath, DirectStorage.
- D3D12Core from System32 (no Agility SDK override in use).

## Agent route
- Launch via steam://rungameid/1845910; window up after ~2 s; BioWare logos; title "PRESS ANY BUTTON" at ~75-95 s.
- Space -> main menu with Continue highlighted -> Enter -> gameplay ~15 s later (Minrathous Streets, "Find Neve Gallus").
- Windowed 1920x1080 on this PC.

## XR bridge (D3D12) - src/xr_host.cpp (2026-10-29, written, not yet tested in game)
Game-independent; reusable for other D3D12 games (Inquisition is D3D11 -> use the SO6/MEA path there).
- XR thread ("davgvr XR") owns its own D3D11 device (d3d11.dll / dxgi.dll loaded from System32 by full path),
  created on the LUID from xrGetD3D11GraphicsRequirementsKHR, or the game's D3D12 adapter LUID when the runtime
  reports all zeros (SteamVR null driver). openxr_loader.dll is loaded from the game folder; missing loader / no
  headset never stops the game (retries every 2 s / 5 s). Frame loop at the headset rate, decoupled from the game.
- Hand-off: 3 shared textures (back buffer size + typed format), created on the XR D3D11 device with
  MISC_SHARED | SHARED_NTHANDLE and opened on the game's D3D12 device (OpenSharedHandle). Recreated at most once per
  0.5 s (SO6 lesson); old ones released only after our last copy on the game queue completed.
- Producer (render thread, Present hook, before the original Present): per-slot allocator + command list on the
  game device; back buffer PRESENT->COPY_SOURCE->PRESENT, shared texture COMMON->COPY_DEST->COMMON; recorded on the
  queue the swapchain presents on, so it runs after the game's rendering.
  Sync: queue->Wait(releaseFence, slotRelease[slot]) ; ECL(copy) ; Signal(frameFence, ++n) ; Signal(copyFence, ++c).
  XR thread: ctx4->Wait(frameFence, n) ; blit ; ctx4->Signal(releaseFence, ++r) ; slotRelease[slot] = r ; Flush.
  CPU side: the producer never picks the newest slot or the one being read; a slot whose previous copy (own
  copyFence) has not finished is skipped - the render thread never blocks.
- Swapchain -> queue: hooks on the real dxgi IDXGIFactory::CreateSwapChain (vtable slot 10),
  IDXGIFactory2::CreateSwapChainForHwnd (15) and CreateSwapChainForComposition (24) record the queue passed as the
  device. Fallback: the last DIRECT queue that called ExecuteCommandLists on the present thread (logged once).
- Colour: XR swapchain prefers R8G8B8A8_UNORM_SRGB (then BGRA sRGB; else 8-bit UNORM / R10G10B10A2 with a warning).
  Shader blit on the XR device: gamma-encoded UNORM source (R10G10B10A2 / 8-bit) is decoded to linear so the sRGB
  target re-encodes it (display matches the monitor); float sources are passed through.
- Theater: quad layer in LOCAL space, `screen_distance` (2.5), `screen_width` (3.2), `screen_height` (0.0).
  F9 recentres (screen in front of the current head yaw / position). `vr_enabled = 0` skips everything.
- Debug: `dump_frame = N` writes the swapchain image of the Nth new game frame; `DAVGVR\dumpscreen.trigger` the next
  one -> `DAVGVR\screen_NNN.bmp` (as submitted, display-encoded) + "mean level" in the log. `reload.trigger`
  re-applies the screen settings.
- Log lines: "xr: runtime ...", "xr: headset '...'", "xr: recommended eye size", "xr: session created; swapchain
  formats offered: ...", "xr: screen swapchain WxH format F", "xr: session state -> N", "xr: shared textures ...",
  "xr: producer ready ...", "xr: first frame handed over ...", "CreateSwapChainForHwnd: swapchain .. presents on
  queue ..", and every 10 s "xr stats: headset .. fps (.. with screen layer), new game frames ../s, xrWaitFrame .. ms
  | game frames published .., skipped .. busy / .. allocator".
- Test 005 (PC, null HMD): theater works first try - session runs, 0 skipped, colours match the monitor (dump mean ~40).

### Head-tracked output (xr_host, 2026-10-29, built, untested)
- **Recentred reference frame**: yaw-only rotation + position of the head, captured when the session starts, on F9
  and XrHostRecenter. The quad and every pose exchanged with the game side use it (recentred head looking ahead =
  origin facing -Z). `XrHostLocateEyesAt(t, views)`, `XrHostLocateEyes(views, frames)`, `XrHostPredictTime(frames)`
  return recentred poses; `XrHostHeadsetFov(fov[2])` = per-eye FOVs cached every headset frame.
- **Mono**: `XrHostSetFramePose(eyes[2])` before the frame's Present -> that back buffer is handed over with the poses
  (converted to LOCAL) and submitted as a projection layer (both views = the full image, own pose / FOV each).
  Frames without a pose are held (last head-tracked frame stays, "held" in the stats); after 10 in a row -> quad.
- **Stereo**: `XrHostPublishStereo(queue, left, right, state, eyes[2])` copies both eyes (CopyTextureRegion,
  source state -> COPY_SOURCE -> state) into a separate 2W x H shared ring ("stereo" set; the "screen" set stays at
  back buffer size, so switching modes never reallocates) -> projection layer with the two halves. The frame's
  Present then hands over nothing. Queue must be DIRECT. Work on a different queue than the previous hand-off first
  waits on our copy fence, so all fence signals stay ordered.
- One XR swapchain per set (screen / stereo). Log "xr: submitting quad (flat screen) | projection (mono) |
  projection (stereo)" on each change; stats line adds "(N stereo)" and "held N".
- Dumps: mono / flat one image; stereo the side-by-side image with "mean level left .. right ..".

## View pipeline (Frostbite 2021) - src/fbview.cpp (2026-10-29)
- `exe+29CCBC0` (per frame, job thread) -> `exe+29ABFE0 buildViews(out, matrixTable, frameCtx)`:
  out +0 entries (0x2080 each, capacity 15), +8 pointer array (entry+0x150), +0x10 count.
  Walks the registered view sources (max 15), for each: entry = zeroed, 6 RenderViews from the default
  constructor `exe+2A08AB0` + assignment `exe+2A083B0`, field defaults, then
  `exe+298C400 setupView(entry, viewIndex, r8, camParams, prevXf, curXf)` (args 5/6 on the stack), then the
  render-settings object at `*(exe+6207E90)` vtable +0x98 (applies GameRenderSettings: ForceFov, ...) and +0xA0,
  then the per-view matrix table row viewIndex*0x110 (4 rows = split-screen slots), then the pointer array.
- camParams: +0 xf, +0x48 physical lens block (+0x50 sensor 42.7x24 mm, +0x68 fovY rad used when +0xD2 != 0),
  +0x84 float2 -> view+0x90, +0x8C fovY (rad), +0xA8/+0xAC principal point (0.5, 0.5) -> view+0x104 (only in the
  fov path, +0xD2 == 0), +0xB4 near, +0xB8 aspect, +0xBC render target id -> entry+0x1F90, +0xC4 far,
  **+0xE8 viewport u16 x, y, w, h in output pixels** -> entry+0x1F88 (rescaled to render size by exe+29A3C43).
- Transforms (0x90): rows right, up, BACK, position (metres, +Y up); camera looks along -row2 (same as MEA).
- RenderView 0x540: +0 transform, +0x90 float2, +0xC0 lens, +0x100 fovY rad, +0x104 principal point, +0x110 near,
  +0x114 far, +0x118 aspect, +0x140 dirty, +0x2B0 fovX, matrices +0x2C0 / +0x340 / +0x480 / +0x4C0.
  Setters: 2A26900 fovY, 2A267C0 principal point, 2A269F0 near, 2A268E0 far, 2A26750 aspect, 2A274B0 transform,
  2A1AFA0 lens. Entry = 6 RenderViews (0x1F80) + params (+0x1F88 viewport, +0x1F90 target id, +0x1F94 index, ...).
- `exe+360CA2B` WorldRenderModule::setupViews consumes the entries (per-view render target via id +0x1F90).
- Settings: getSettings(*(exe+5F11A80), &TypeInfo) = exe+11D33A0; GameRenderSettings TypeInfo exe+6200380.
  SplitScreenTestViewMode / SplitScreenTestVrEnable / Stereo* fields are not read by any code (dead on PC).

## Head tracking (works, 2026-10-29 test 006)
- setupView hook, first view with index 0 per build = main camera. Camera = level(game camera) * head pose
  (OpenXR axes = Frostbite camera axes). fovY/aspect from the headset (symmetric, covering both eyes), written to
  cam+0x8C and cam+0x68. Previous transform = what we rendered last frame (motion vectors include head motion).
- Pose ring keyed by build serial; Present uses serial - pose_latency_frames (1). Mono projection layer.

## Native stereo (2026-10-29 tests 007-034) - two engine views + command-list replay
### Views (src/fbview.cpp)
- Appending a second view entry with the SAME view index 0: dark frame, 12-24 fps, then hang (per-view resources of
  slot 0 shared). **View index 1 (split-screen slot 1) renders fine**, ~105 builds/s vs 135 with one view (5070).
- The entry is built like the engine does: 6 RenderViews via 2A08AB0 + 2A083B0, `setupView(e1, 1, ...)`, registry
  vt+0x98 / +0xA0 (`*(exe+6207E90)`), forced update `exe+2A2A8E0(e1, exe+4E22A98, 0xF)`, matrix table row 1,
  `ptrs[1] = e1+0x150`, count = 2. Left eye = view 0, right eye = view 1.
- **The camera transform block has two matrices (+0x00 and +0x40); view matrices are built from +0x40.** Writing
  only +0x00 gave identical eyes. WriteXf writes both, ReadXf reads +0x40.
- Split-screen viewports in one target (cam+0xE8 halves) -> garbled green patch; abandoned.
### Getting the left eye out (src/replay.cpp)
- Both views render into the same full-size targets; the final stage (tonemap, UI composite, copy to the back
  buffer) runs ONCE, for the last view (right). The left eye is produced by replaying the game's own command lists:
  - In the ECL hook, DLSS outputs are found as output-size RGBA16F textures entering UAV: first in the frame = left,
    second = right. The batch is split where the left DLSS output leaves UAV and it is copied to `R.leftDlss` right
    away (later passes alias that memory - transient heaps).
  - Composite target = output-size R10G10B10A2 going COMMON->RENDER_TARGET (or a member of the known set).
  - Replay: execute [0..s), snapshot the range inputs (mode 2 = inputs only), execute [s..n); own list copies
    composite -> right eye, left DLSS -> the DLSS output, restores inputs (left copies of shared inputs preferred)
    and resource states; re-execute lists [s..compEnd]; publish (left = composite, right = saved copy).
  - Own command lists: ring of 12, fence-checked, never blocking. Clones released through a 10-frame graveyard
    (releasing in-flight clones hung the GPU).
- Replay start: s = list after the last DLSS dispatch. Starting one list early re-runs the right DLSS and both eyes
  match; with per-eye DLSS the right-eye DLSS sits in that list, so **`stereo_replay_start_shift` defaults to 1 when
  per-eye DLSS is on** (test 033 matrix: per-eye + shift 1 = clean left eye, real parallax, sharp).
### Per-eye DLSS (src/slhook.cpp)
- Shared DLSS history between the eyes ghosts the left eye (test 031). Streamline is hooked (slSetTag,
  slSetConstants, slEvaluateFeature, slGetFeatureFunction -> wrapped slDLSSSetOptions); the right eye goes to
  Streamline viewport 1 (ViewportHandle GUID begins 171B6435): tags route by a per-thread eye toggle corrected by the
  output-tag map from replay, constants by call order per frame token, evaluate swaps the ViewportHandle, options
  are copied to viewport 1.
- Test 033 (world_scale 20, Minrathous): per-eye off + shift 0 = ghosted left; per-eye off + shift 1 = clean but
  soft (history from the other eye); per-eye on + shift 0 = identical eyes; **per-eye on + shift 1 = correct**.
### Diagnostics
- Triggers in <game>\DAVGVR\: dumpscreen, reload, trace, inputs, slconst, dumpdlss (RGBA16F readback -> BMP).
- Log every 5 s: "stereo views" eye positions; replay/sl errors.

## Final stage / HUD (2026-10-29 test 036, `hudtrace.trigger`)
Last batch of the frame (ecl#33, ~79 lists), after the right view's DLSS (list 37):
- lists 39-61: post on output-size R11G11B10 (f26) targets (bloom chain 1x1 .. 1920x1080).
- lists 65-75: **UI** into an output-size **R8G8B8A8_UNORM_SRGB** (f29) target (cleared in 65, ~10 draws), plus a second
  f29 target used as intermediate (menus / blur).
- list 76-77: composite target (R10G10B10A2, f24) COMMON->RT, **one draw** = tonemapped scene + UI, then -> COMMON and
  CopyResource to the back buffer.
HUD layer (replay.cpp): UI target (test 045 readback, rt8_c0 = minimap / quest text, premultiplied alpha) = the
output-size RGBA8 texture that enters RENDER_TARGET in [replay start, composite) and is STILL a render target when the
composite list starts (that list itself makes it a shader resource; the intermediate f29 target ends as SRV and is
empty). The first rule "last RT->SRV" picked the empty intermediate (044). Game
pass: execute up to the composite list, own list copies the UI image out and overwrites it with a zeroed texture,
then the composite; the left replay does the same (blank only). XR host: third shared set "hud" (RGBA8) copied in the
same list as the eyes, own XR swapchain (sRGB, rgb decoded, alpha kept), quad layer with source alpha, premultiplied.
Config hud_layer / hud_width / hud_distance / hud_height / hud_anchor world|head / hud_premultiplied; F10 toggles.

## Keys
- F8 cycles vr_mode stereo -> head -> flat (render thread, foreground check; test 036 OK). F9 recentre. F10 HUD panel.

## Pitfall: both views can share one DLSS output texture (test 038)
The frame graph's transient allocator does not always give the two views different DLSS output textures. In test 038
(same build, same save) both views wrote the SAME RGBA16F texture: the old "second distinct texture = right" rule
found no right output (no replay range, every frame) and the texture->eye map sent both evaluates to Streamline
viewport 1 (slEvaluateFeature(vp 1) -> 20, green garbage). Fixes: right output = next output-size RGBA16F UAV write
after the left one was saved (same texture allowed); DLSS evaluate eye = call order per frame token (1st left, 2nd
right, like the constants), tag groups follow the last evaluate globally (views may run on different threads); the
texture map is only used when the two outputs differ.

## DLSS pass identification (tests 040-042)
The texture heuristic (first output-size RGBA16F entering UAV = left DLSS output) failed on some launches: another
pass matched first (041 run 1), or the right output had no barriers at all (041 run 3) -> no replay range, garbage.
Now slhook passes each DLSS evaluate's command list (SL CommandBuffer = ID3D12GraphicsCommandList*), eye (call order
per frame token) and output (ScalingOutputColor tag) to replay.cpp (ReplayNoteDlss); the list record carries it, so
the ECL hook knows exactly which list holds each view's DLSS. Left save after the left evaluate's list (or the output's
UAV exit), replay starts after the right evaluate's list (or the right output's UAV exit). Heuristic kept as fallback.

## HUD layer stability (tests 037-050)
- Switching the HUD layer on during startup (logos / title / loading: frames alternate with and without stereo) made the
  game quit a few seconds later (037, 043, 046) - no exception, no TDR event, no Application Error: Frostbite closes
  itself after a device-removed error. Now the HUD layer only engages after 200 consecutive stereo frames.
- Replay own command lists: the ring of 12 ran out with the HUD's extra lists ("own lists busy" -> frames without
  replay, HUD flicker): ring is 32 now.
- DRED (src/dred.cpp, config dred = 1): auto breadcrumbs + page fault + context enabled before any device exists;
  on a removed device (Present result or polled every 30 Presents) the unfinished command lists with the ops around
  the failing one and the page-fault allocations are logged ("dred:" lines). Our own lists / textures are named
  ("davgvr replay list N", "davgvr xr copy N", "davgvr hud copy", ...).

## Render resolution (src/window_size.cpp, test 051)
- The game renders at its window's client size (borderless windowed; swapchain follows WM_SIZE via ResizeBuffers).
  The window procedure is subclassed (WM_GETMINMAXINFO allows > monitor size, WM_WINDOWPOSCHANGING keeps the size) and
  the client area is set to the target once, on the window thread (posted message), after 600 presents in stereo.
- render_resolution = auto (16:9 at the headset's recommended eye height x render_scale) | WxH | game.
  Null HMD recommends 2748x2880 per eye -> auto x0.5 = 2560x1440: applied at the title screen, stereo + HUD layer fine
  (~100 fps on the 5070). 3200x1800 applied later IN GAME: right eye fine, LEFT EYE BLACK (not investigated) -> the
  size is applied only once per launch; later changes need a game restart.
- Every per-size resource re-creates itself (left DLSS copy, right eye, HUD copy, XR shared sets / swapchains).

## In-headset settings menu (stage 7, src/menu.cpp + vrkit/, test 053)
- vrkit (sharedtools) on the XR thread's own D3D11 device: MenuInit on the first headset frame, MenuFrame every
  frame (vrkit draws with its own device-context state), CopyResource of the R8G8B8A8_TYPELESS menu texture into an
  R8G8B8A8_UNORM_SRGB XR swapchain (usage TRANSFER_DST), quad layer last (on top), source alpha, premultiplied.
- Placement: vrkit QuadPlacer, world-anchored by default (captures head heading/position in LOCAL when it opens);
  head pose = midpoint of the two located eyes.
- Options are config keys; onChange writes every option into the in-memory Config (now mutex-protected, Set()) and
  calls XrHostApplyConfig + FbViewApplyConfig, so the menu reuses the normal apply paths. vr_mode is re-synced from
  the live mode (F8) whenever the menu is closed. Bools are saved as true / false (hud_layer accepts both).
- Build: vrkit + imgui sources, d3d11.lib d3dcompiler.lib (ImGui DX11 backend). DLL 2.2 MB.

## Crash fix 1 (2026-10-30, user's 5090 + Index-class HMD "lighthouse", recommended eye 3980x3424 -> 6086x3424)
- C0000374 heap corruption, detected in g_state.clear() (ReplayOnPresent) ~1 s after the HUD panel engaged. The game
  renders on several DIRECT queues ("composite rendered on queue A" then "... queue B" within 5 ms, test 056), from
  more than one thread; ReplayOnExecute / ReplayOnPresent shared their frame state without a lock.
- Fix: one recursive mutex around ReplayOnExecute and ReplayOnPresent (recursive: XrHostPublishStereo's own
  ExecuteCommandLists re-enters the hook). A filter "only the queue that renders the composite" broke stereo (test 055:
  it locked onto a start-up queue) and was removed.
- Both builds rebuilt: NOMENU=1 ./build.sh -> dist_nomenu (stage 6), ./build.sh -> dist (stage 7 with the menu).
- 2026-10-30 later: the user's 5090 BLUESCREENED with fix 1 (0-byte log). Suspects: DRED forced on (driver-side
  allocation tracking) at ~6086x3424 per eye. Fix 2: DRED default off (dred = 1 to enable), log committed to disk
  once a second by a below-normal-priority thread (FlushFileBuffers; log lines still only fflush to the CRT/OS cache).

## Fix 3 (2026-10-30): projection aspect, resize caches, game pacing, GPU timing (tests 057-059)
- The engine ignores cam+0xB8 aspect for the projection: it uses the viewport (16:9 render target). RenderView +0x118
  shows the requested aspect, but the images keep normal proportions in 16:9. So the submitted per-eye FOV is now
  fovY plus horizontal tan = tanY * viewport aspect (config fov_from_viewport, default 1). Before: Index-class HMD
  (L -47.8/+42.2, +-42.5) eye images squeezed 1.48x horizontally = weak depth. Null HMD (90x90): 121.3 deg submitted.
- Resource description cache (by address) now lives one frame; an output size change resets every per-resource replay
  cache. First in-game resize now keeps both eyes; a SECOND resize in a session still blacks the left eye (057), so
  the size is still applied once per launch (render_resolution_live = 1 to test).
- xr_pacing = game (default): XR thread waits (<= 100 ms) for a new published frame before xrWaitFrame; pose prediction
  extrapolates from the time of the last xrWaitFrame. xr stats then show headset fps = game fps (059: 67.5 = 67.5).
- GPU timing (059, 5070, 2560x1440/eye, per frame): left DLSS save 0.04 ms, left input copies 0.19 (77 MB), snapshots
  0.24 (77 MB), game post for the right eye 0.70, restore 0.32 (77 MB), left replay 0.67, XR hand-off 0.09; frame
  period 15.3 ms. The mod's own stereo work is ~1.6 ms (~10%): the cost is the engine's second full view.
- Fix 4 (test 060): frame limiter (src/limiter.cpp) in the Present hook: cadence = headset period (XrHostDisplayPeriodNs)
  x game_fps_divider, high-resolution waitable timer + 0.6 ms spin; restarts the cadence when the game falls behind.
  Dev PC: divider 2 -> 45.0 fps, 3 -> 30.0 fps, xr stats new game frames equal. For OFXR-Bridge 2x / 3x.
- Fix 5 (2026-10-30 evening): fix 3's fov_from_viewport was WRONG. User log 19:58 "proj check": view-projection x scale
  0.905 = 1/tan(47.8 deg) and RenderView fovX 1.668 rad = 2 atan(tan(42.5) * 1.203): the engine DOES use the aspect
  written to cam+0xB8. Reporting 116.9 deg (tanY * 16:9) stretched both eyes ~1.22x. Default back to off. The user's
  earlier "mono" (06:12) was most likely the stale resource-description cache after the auto resize (identical eyes).
  Dev dumps: disparity sign correct (near objects further right in the left eye, far ~0). Added swap_eyes (diagnostic).

## Fix 6 (2026-10-31): flat "z-3d" stereo at headset resolution (tests 061-100)
- Symptom (user, 5090, 6086x3424 per eye): depth weak / cardboard-like in game, world_scale 10 changed nothing. The user's
  log shows the engine eye cameras 0.72 m apart at world_scale 10 - camera offsets fine, the images were wrong.
- **Root cause: the output size.** Resize tests on the dev PC (same build, disparity measured on the dumps at world_scale 1
  and 4): 1920x1080, 2560x1440, 3840x2160 = correct stereo; 2048x1152, 3072x1728 (and the user's 6086x3424) = both eyes
  identical. The engine renders both views correctly at every size (left vs right DLSS outputs differ by 25-52 px on the
  ground at 3072 wide, test 078) but at the non-standard sizes the final composite does not follow the replayed left
  image: test 094 read back every output-size texture of the replay range before / after the left replay - the post
  colour (R11G11B10) holds the LEFT image after the replay, yet the composite comes out identical to the right eye.
  Exact mechanism not found yet (candidate: a copy / alternate path at non-mode sizes).
- Fix (superseded by Fix 7): render_resolution = auto snapped to the nearest verified 16:9 size (1920x1080, 2560x1440, 3840x2160);
  render_resolution_snap = 0 disables it; an explicit WxH that is not one of them logs a warning.
  5120x2880 crashed the 12 GB 5070 (test 097) - not in the list.
- Also fixed on the way:
  - crash in our left-save CopyResource (tests 065-068): list records keyed by command-list address are erased once
    executed; DLSS outputs held by reference (KeepAlive, 10 frames) - the per-eye "last tag" could name a texture the engine
    had already freed at a scene change. Streamline tags are never on the evaluate's command list (always the fallback).
  - SL tag routing by sequence only (the output-texture -> eye map went stale: transient outputs swap between views).
  - Left-input copies (post colour R11G11B10 + half-size) now taken right after the list where the LEFT view made them
    readable (after its evaluate, before later lists reuse the memory); only image inputs >= 1/4 output width, no depth
    (a depth buffer written late in the batch blackened the left eye, test 087), exposure etc. shared between the eyes.
  - Copies use CopyTextureRegion (mip 0) for the DLSS images; left-DLSS copy texture retired through the graveyard.
- Diagnostics added: dumpdlss.trigger now also writes raw_*.bin (RwithL, in*, compL, compR, range textures b*/a*) for
  offline decoding (R11G11B10 / R10G10B10A2); periodic "left inputs taken / restored" counters; "id batch / id list" lines.
- Dev PC agent: device_commit_files caches by staged path - stage every new build under a new folder name.

## Fix 7 (2026-11-01): any render size works - restore order (tests 101-109)
- The real cause of the identical eyes at "non-standard" sizes (Fix 6): **transient aliasing between a snapshot-restored
  input and a left-view input**. The restore list wrote the left post colour / left DLSS image first and then, in the same
  list, restored other inputs from their snapshots; at sizes where the frame graph places one of those inputs over the same
  memory, the snapshot (right-eye bytes) overwrote the left data. 1080p / 1440p / 2160p happened not to alias.
  Fix: restore pass 1 = state fixes + snapshot restores, pass 2 = left inputs, then the left DLSS image last.
  Test 108: 3072x1728 = full stereo (disparity scales with world_scale).
- Final-stage traces (hudtrace with dispatches / copies, tests 106 / 107) are structurally identical at 2560x1440 and
  3072x1728: the post draw into the R11G11B10 colour sits in the DLSS evaluate list right after the evaluate; replay
  starts at the next list; composite reads the post colour + bloom + UI (RGBA8) targets.
- render_resolution = auto now = the headset's recommended eye size x render_scale (any aspect); auto16x9 = old 16:9 at
  eye height; render_resolution_snap = 1 rounds to 1080p/1440p/2160p. Test 109 (null HMD 2748x2880 x 0.6 = 1648x1728,
  taller than wide): stereo correct, game HUD laid out correctly (round minimap), 60 s soak OK.
- Crash fix: Desc() / SnapIn() called GetDesc on last frame's replay inputs, which can be freed by a level load
  (test 105, VERSION+1AF51) - their descs are now stored while they are alive.
- Headset refresh: SteamVR's predicted display period jumps to 2x / 3x while it throttles; the refresh rate is now the
  shortest period seen (the limiter followed the throttled values: 33.7 / 21.3 fps at n/3 on the user's PC).
  Log: "xr: SteamVR throttled N% of headset frames" every 10 s.
- hudtrace now also logs Dispatch (counts, groups) and CopyResource / CopyTextureRegion (src -> dst) per list.

## Fix 8 (2026-11-02): noisy right eye - per-eye post history (test 114)
- User: right eye noisy / "something constantly moving", even with the head still; not on the title screen; per-eye DLSS
  off, render_scale and layout made no difference. Ruled out on the dev PC: Streamline frame tokens always carry exactly
  2 SetConstants + 2 evaluates (test 111); clipToPrevClip correct per eye, static and with simulated head yaw
  (debug_fake_head_yaw_deg, test 113); depthInverted 1, cameraMotionIncluded 1; MV / input colour / exposure tags are the
  same textures for both views (rewritten per view), depth differs.
- Cause (likely): the replayed range reads textures from the previous frame and rewrites them (temporal post history).
  The left replay runs after the right pass, so each eye read the other eye's history - a constant eye-offset mismatch.
- Fix: stereo_replay_history = 1 (default, menu Display "Per-eye post history"): history = range texture not touched
  before s this frame, first use a read, rewritten in the range, 2D >= 1/8 output width, no depth. After the right pass:
  copy -> histR, histL -> texture (left's from last frame); after the left replay: texture -> histL, histR -> texture.
  Found 3 at 1648x1728: two output-size RGBA8 (f27) + one half-size R11G11B10. GPU cost ~0.1 ms.
- Dev PC graphics settings may not enable the effect that showed it on the user's PC (dev dumps were clean either way).

## Fix 9 (2026-11-02): head-motion judder - exact frame / head-pose pairing (tests 115, 116)
- User: at a steady 45 fps (n/2) characters jump a few inches sideways when the head turns; in headset pacing (90 fps
  re-submits) the whole view sways by the same amount instead. = image submitted with the head pose of another frame.
- The old pairing took the pose ring entry "newest buildViews serial - pose_latency_frames". buildViews runs several times
  per rendered frame (other views), so that entry was essentially never the rendered frame: test 115 at n/2 with simulated
  head yaw - the rendered frame was 0 builds behind the newest in ~37% of frames and 4+ in the rest, never 1.
- Fix: Streamline SetConstants (1st call per frame token = left view) gives the camera position actually rendered;
  Evaluate(eye 0) attaches it to the command list (ReplayNoteDlssCam); fbview stores each build's left-view position
  (entry +0x30, floats 12..14) in the 64-entry pose ring; the replay publishes the ring entry with that exact position
  (FbViewStereoPosesForCam). Test 116: 450/450 frames matched, distance 0.000 (bit-exact), no fallbacks.
  Log: "fbview: frame/pose pairing by camera" every 10 s. Falls back to the latency rule if no entry matches.
- Per-view SCENE history (stereo_scene_history) attempted for the noisy right eye (noise follows the right IMAGE: swap eyes
  moves it; per-eye DLSS / post history: no change; not on the title screen) - test 115: the right view's scene spans
  several batches (its depth prepass runs before the batch with its DLSS), the swap put last frame's depth in -> broken
  right eye. Default OFF. Needs per-list swap points (first view-1 read after view 0 finished). Candidates found at
  1648x1728 output: render-size depth (f19) x2, quarter-size depth x2, render-size R11G11B10, output-size R11G11B10, RGBA16F.
- Generic (sharedtools candidate): pair submitted frames to head poses by an engine-side camera key, never by frame counts.

### Fix 9 follow-up (2026-11-02, tests 117-120)
- User on stage 12: "everything jumps" - worse. World-lock test (117): simulated yaw, 6 dumps per method, image shift of
  distant content vs yaw change of the SUBMITTED pose. Camera pairing: errors -55 / -62 / +89 px (~4-6 deg) in 3 of 5
  pairs; old latency rule: within +-11 px. Causes: (1) position-only key ties when the head turns in place -> added the
  forward vector; (2) Streamline frame tokens are a small reused ring and the lookup took the FIRST entry for a token, often
  an older frame's camera -> now the newest entry. Not yet re-verified: tests 118/119 - the game process started without a
  window and without loading the mod (EA app?), 120 found no game process.
- stage13: pose_pairing = latency (old rule) by default, camera pairing opt-in. world-lock script: scratchpad worldlock.py
  (dump pose lines "xr: dump pose" + phase correlation of the upper-centre band of the left eye).

### Fix 9 verified (2026-11-03, test 122; Smart App Control had blocked the DLL in 118-121)
- Tests 118/119 failed because Windows Smart App Control switched on and blocked the unsigned version.dll
  (0xC0E90002, CodeIntegrity events 3033/3077, "did not meet the Enterprise signing level"). User turned it off.
- World-lock test with an exact rotation model (worldlock2.py: 120x120 template at tangent x = -0.15 / 0 / +0.15, 30%
  height, predicted position after a pure yaw of the SUBMITTED pose difference, normalised cross-correlation search):
  camera pairing (with forward vector + newest-token fix): ratio measured/predicted 0.99-1.04 for all good matches;
  old latency rule: 1.13-1.15 in one pair (a wrong frame's pose), others ~1.0. Camera pairing is now the default.
- The "4 % smaller shift" from the first analysis was the linear (tangent-at-centre) approximation, not a FOV error:
  Streamline's cameraViewToClip is exactly the requested 90x90 (x scale 1.000, y 1.000) at a 1648x1728 viewport.
- Pairing stats now: rendered frame = newest build in 440 of 450 frames (n/2), the rest 1-4+ behind.
- 2026-11-03: user confirms on the 5090 + Index-class HMD - judder completely gone with stage 14.

## Fix 10 (2026-11-03): per-view scene history v2 (test 126) - for the noisy right eye
- Learned per frame from the barrier stream: phase 0 = up to the left DLSS evaluate list, phase 1 = after it up to the
  right evaluate list (the right view's work starts inside the left batch and spans several batches). Texture whose first
  access in both phases is a read and that is written in both, 2D, >= 1/16 output width = per-view history (kept 8 frames).
- Next frame: right before the list where view 1 first touches it (ECL wrapper EclWithSwaps, any batch): copy the texture
  -> H0 (left view's new), H1 (right view's own, last frame) -> texture; after the right evaluate batch: texture -> H1,
  H0 -> texture. Planned only if the first access is still a read.
- Test 126 (1648x1728): 16-18 textures, all render-size or quarter (depth f19 x6, RGBA8 x4, RGBA16F x3, R11G11B10 x2,
  R10G10B10A2, R16G16, R16): ~15 swaps + 15 restores per frame, right eye normal (no fog), stereo unchanged, ~1 ms GPU.
  Noise itself not reproducible on the dev PC (lower settings); user test pending. stereo_scene_history = true (default).

## Fix 11 (2026-11-03): right-eye DLSS inputs - tag groups routed by view identity (test 127)
- User (stage 15): per-eye scene history made no difference and broke the HUD / menus (now default OFF). The right eye
  looks like it has NO anti-aliasing (jaggies), not noise.
- User log: the two views' DLSS tag groups are recorded on DIFFERENT worker threads at the same time (same millisecond).
  Every SetTag call (one tag per call) picked its eye from the global g_nextTagEye, which only flips at an evaluate -> on a
  many-core CPU a group could be split between viewports 0 and 1: the right eye's DLSS got mixed inputs -> no stable
  history -> aliased. The dev PC (fewer cores) never interleaves, which is why it never reproduced.
- Fix: a group's eye is decided once at its first tag (depth, type 0) from depth texture -> eye, learned in replay.cpp from
  GPU execution order (1st DLSS evaluate executed in a frame = left view); kept for the group and its evaluate on that
  thread (thread_local t_grpEye). Fallback before learning: old order rule. Log every 10 s: "sl: tag groups by depth
  texture N, by order N | evaluates where the group's eye differs from the call order N" and "replay: evaluates whose
  recorded eye differs from the GPU order N" - non-zero counts on the user's PC = the old routing was wrong that often.
- Test 127 (dev PC): all groups routed by depth, 0 differences, stereo unchanged.

### Fix 11 FAILED on the user's PC (stage 16) - reverted (2026-11-04, test 130)
- User: stage 16 lost stereo ("flat screen with head tracking"). Log: 0 stereo replays,
  "evaluates whose recorded eye differs from the GPU order 413", "depth->eye relearned 827".
- Cause: on the user's 5090 the two views' depth textures swap every frame (the engine
  ping-pongs them), so routing by depth texture identity flip-flops and both evaluates
  land on the same eye. Depth identity is NOT a stable view key.
- Stage 17 (test 132 on dev PC: 650 stereo replays / 10 s, SPLIT 0): routing back to stage 14 (per-call g_nextTagEye, evaluate eye = token order).
  The tag-group logic stays only as a diagnostic: the 10 s report line
  "sl: DLSS tag groups N, SPLIT between the eyes' viewports N, evaluated on the other eye ..."
  tells whether concurrent tag recording mixes the eyes' DLSS inputs (aliased right eye theory).
- Per-eye scene history: menu label now "(experimental)", default off.

### Stage 17 user log + stage 18 (2026-11-04, test 133)
- User log (stage 17): SPLIT 0 -> tag groups are NOT split across threads (concurrency theory ruled out). But
  "evaluated on the other eye than they were tagged for" = 450 of 900 groups every 10 s (dev PC: 0): one group per frame
  is tagged under the other eye's "next eye" state -> the right viewport may hold stale / wrong inputs.
- Stage 18: config stereo_dlss_tag_routing = order | per_type (menu, live). per_type: k-th tag of each buffer type ->
  eye k&1, resync at the right evaluate. Report "sl: tags per buffer type left/right" (types 0 depth, 1 mvec, 3 colour,
  4 output, 13 exposure). Dev PC: 50/50 for all types in both modes, stereo kept (test 133).
- LumaSharpen (sharedtools\vrkit\lumasharpen.md) in the D3D11 eye blit (psDecode / psPass, not HUD), linear sampler
  s1, cbuffer b0 {sharpen, texel}. lumasharpen = 0..5 (menu), live. Dev PC dump: edge gradient 8.3 -> 13.1 at 1.5.
- DLSS OFF = no stereo (user): replay range + left output come from the Streamline DLSS evaluate. Game folder has
  libxess.dll (hookable) and Streamline; FSR / TAA are in the exe -> need a non-DLSS anchor.

## Fix 12 (2026-11-04): stereo without DLSS (tests 134-138)
- Upscaler setting: Documents\BioWare\Dragon Age The Veilguard\settings\ProfileOptions_profile,
  GstRender.UI_TemporalUpsampleMethod (2 = DLSS; 0 = native TAA at full size; 1 and 3 = the game's built-in upscalers,
  render 824x864 -> 1648x1728 on the dev PC; libxess.dll never loaded in any run). Tests restore the file.
- With DLSS off: 0 stereo replays (replay range came only from the Streamline evaluate).
- Trace (137): each view has ONE command list with the upscaler: >= 2 output-size textures enter UAV (history f10
  40>8, work f34/f28/f61), output = output-size RGBA16 (f9 typeless) that LEAVES UAV in that list without entering it
  there (method 1: f9 8>C0; method 3: f9 8>40, 40>C0). Histories f10 enter and leave in the same list.
- replay.cpp: when no Streamline was seen, such lists get synthetic dlEye (1st per frame = left), dlOut = that output,
  dlEv = UINT32_MAX (skip the evaluate-barrier search); slMode = g_slSeen || g_upSeen.
- Test 138: methods 1 and 3: 650-760 stereo replays / 10 s, HUD layer on, dumps: correct stereo, L/R sharpness equal.
- Open: method 0 (native TAA, no output-size upscale) - no anchor yet; pose pairing falls back to latency without the
  Streamline camera.

### Stage 18/19 user log (2026-11-04)
- "Right-eye DLSS inputs" per-type routing: with per-eye DLSS ON every buffer type is 450/450 in BOTH modes (and the
  user saw no difference) -> DLSS tag routing is ruled out as the cause of the aliased right eye.
- No stereo without DLSS on the user PC: the upscaler was switched in-game; g_slSeen was sticky, so barrier detection
  never started. Stage 20: SlRecent() / UpRecent() = an evaluate / barrier-found upscaler in the last 30 frames.
- Test 139: DLSS (method 2) and method 1 still 670-750 stereo replays / 10 s.
- LumaSharpen confirmed by the user ("works great").

## Fix 13 (2026-11-04): user stage 20 results + FSR judder, per-eye swapchains (tests 140-144)
- User: right-eye noise GONE with FSR and XeSS (game's own upscalers) -> the aliased right eye is DLSS-specific, not the
  replay. Judder back in FSR / XeSS (no Streamline camera). No VR with no upscaler (UI_AntiAliasing 0: no temporal pass,
  trace shows no output-size UAV upscaler list at all).
- Test 140: the view matrices the mod copies for the right view (entry +0x480 / +0x4c0 and the per-view table rows) equal
  the builder's for the left - no jitter in these (they are view matrices). Jitter question open: stage 21 test option
  stereo_dlss_right_jitter = game | zero | negate | previous (overrides the 2nd SetConstants per token, floats 80/81).
- Per-eye swapchains: g_sc[1] = left eye, g_scR = right eye, both eye-sized; the shared texture stays 2-eye (side by side
  unless > 16384). Blit shader gets a uv rect (cbuffer uvr). Dump composes both eyes. User's 4776x4108 x2 had exceeded the
  SteamVR max 8192 either way (log error). Test 142: 2 x 4120 = 8240 wide works.
- Test 141: render_scale 1.5 -> 4122 wide, the game padded its DLSS output to 4124 -> replay refused ("left DLSS output is
  4124x4320, output is 4122x4320"). Auto sizes now rounded to multiples of 8.
- Pairing without Streamline: each command list is stamped at Reset with the newest main build serial
  (FbViewNewestSerial); the left upscaler list's stamp picks the ring entry (FbViewStereoPosesForSerial). Config
  pose_pairing_stamp (default on). Test 144 (FSR, fake yaw 8 deg, n/2): stamp ratios 0.97-1.05 on all good matches;
  latency rule 1.10-1.13 / 0.93-0.95 and one 2x pair. In DLSS mode the log compares stamp vs camera pairing
  ("command-list stamp vs camera pairing").

## Fix 14 (2026-11-04): intermittent start-up exit (tests 145-150)
- User: every other launch the game closes before VR; log ends right after the swapchain vtable hooks. Dev PC test 130
  identical. No exception, no Windows crash record (145), and exit tracing (ExitProcess / TerminateProcess(self) /
  RtlExitUserProcess hooks) logged nothing -> killed outside the normal exit path.
- 146: 4/5 launches died DURING our dummy D3D12 device / DXGI factory / swapchain creation (InitThread, parallel to the
  game's own device + NGX / Streamline init). 148 (dummy creation after the game's device): 5/5 died -> the dummy
  swapchain through Streamline's DXGI proxy is the trigger, not only the parallel device init.
- Fix: hook D3D12CreateDevice right after d3d12.dll loads; on the game's first device: factory hooks, ExecuteCommandLists
  from our own queue on that device, command list hooks (ReplayInstall) - all on the game's thread. Swapchain vtable
  hooked from the game's own swapchain in the CreateSwapChain* hooks. No dummy device / swapchain (old path only if no
  game device within 120 s). 150: 5/5 launches fine, stereo on the title screen in all.
- Also: DLSS right-eye dumps from the user (stage 21): right DLSS output has ~16% less high-frequency energy than the
  saved left (laplacian 4.26 vs 5.10) - softer, not jagged; none of the right-jitter options changed the image visibly.

## Fix 15 (2026-11-04 evening): the jaggy right eye was an old DLSS DLL (user)
- User: DLSS Swapper showed the game folder's nvngx_dlss.dll was back at DLSS 3.7; swapping 310.6.0.0 back in fixed the
  right eye completely. Per-eye DLSS must stay ON. The right-jitter / input-routing tests, per-eye post history and
  per-eye scene history made no visible difference -> removed from the menu (stage 23): jitter + routing are fixed to
  "game" / "order" (config keys ignored), scene history forced off, stereo_replay_history still read from config.
- Why only the right eye: it runs on the mod's second DLSS instance (Streamline viewport 1); the 3.7 DLL evidently handled
  that instance worse than the game's own (likely preset / model differences between instances). Not investigated further.
- Stage 23 logs "sl: DLSS DLL a.b.c.d (path)" at the first evaluate (VS_FIXEDFILEINFO of the loaded nvngx_dlss.dll,
  warns below 310) and "sl: DLSS mode ..." on every DLSSOptions mode / size change. The preset actually chosen with
  "default" is decided inside the DLL and is not visible through Streamline.
- User's PC also loads an OpenXR API layer "CheekyFoveatedDLSS" (C:\Program Files\CheekyFoveatedDLSS) - note for future
  oddities.

## Fix 16 (2026-11-04): foveated DLSS (src/fovdlss.*, tests 152-158) - own implementation of Cheeky Foveated DLSS's idea
- Hook point: _nvngx.dll exports NVSDK_NGX_D3D12_CreateFeature / EvaluateFeature(_C) (under Streamline; game-independent).
  Eye of each evaluate from slhook (fovdlss::SetThreadEye) - SL calls NGX synchronously inside slEvaluateFeature.
- 152: NGX output sub-rectangles on the game's feature -> BAD00005 (invalid parameter). 153: the game's feature with a
  smaller OutWidth/OutHeight -> BAD00005 too. Working method (154): our own DLSS feature per game handle, created from
  the game's parameter block (create flags 0xB = HDR | MV low-res | depth inverted, PerfQualityValue copied - without the
  flags the centre came out darker) at crop sizes; evaluate it with Width/Height + render subrect = crop, input bases
  (colour / depth / MV / translucency / bias) offset by the crop, Output = our crop-size texture; then composite.
- Composite (compute, recorded right after the NGX evaluate on the game's list; NGX itself rebinds heaps there): centre
  DLSS inside (feathered 8 %), periphery outside. 157: a 2nd DLSS feature for the periphery (DLAA at render size) costs
  the same for presets E / F / K / M with DLSS 310.6 (~ no net gain) -> periphery = game's input colour + own TAA
  (output-size history per eye, MV reprojection with MV.Scale, 3x3 tonemapped neighbourhood clamp, blend 0.1).
- Results (dev PC, render scale 0.8 -> 2200x2304 per eye, DLSS performance, centre 0.5 x 0.5): full DLSS 55.4 fps,
  raw periphery 65.2, TAA periphery 63.9 (+15 %). No visible ghosting with simulated head yaw (158).
- Config: fov_dlss, fov_dlss_width / height, fov_dlss_periphery_taa, fov_dlss_taa_blend, fov_dlss_periphery (2nd DLSS),
  fov_dlss_periphery_preset, fov_dlss_centre_preset (letter or "game"), fov_dlss_border.

### Fix 16 follow-up: eye-tracked centre (test 159)
- xr_host: enables XR_EXT_eye_gaze_interaction when offered, checks XrSystemEyeGazeInteractionPropertiesEXT, one action
  set (pose action bound to /user/eyes_ext/input/gaze_ext/pose, profile /interaction_profiles/ext/eye_gaze_interaction),
  action space located in VIEW space each xrWaitFrame -> gaze direction -> per-eye uv (eye orientation from xrLocateViews
  in VIEW space, symmetric frustum of the larger half-tangents as fbview renders) -> fovdlss::SetEyeCenter, smoothing 0.5.
  Config fov_dlss_gaze = off | eyes | sim (circle 0.25 Hz).
- fovdlss: when the crop base moves (B0 -> B1), the centre feature gets its own crop-size MV texture = game MVs at the crop
  + (B1 - B0) converted with MV.Scale (compute pass), MV base 0. Test 159 (sim): centre moves 0.3 <-> 0.7, no smearing in
  dumps, 64 fps (fixed centre 64, no foveation 55). Null HMD: extension not offered, mode "eyes" stays centred.

## Fix 17 (2026-11-04): VRS foveated rendering (src/vrs.*, tests 160-161)
- Tier 2 shading-rate image (R8_UINT, 16 px tiles on the 5070) applied in the OMSetRenderTargets hook to passes with a
  render-size RT0 + a DSV (scene passes; render size from the last DLSS-SR evaluate); cleared again on other targets and
  at Reset. Combiners {PASSTHROUGH, MAX} (keeps any coarser rate of the game). Centre = average of both eyes' centres
  (gaze), quantised to 1/32; one cached image per centre / size / config.
- 160: building the image with our own queue (ExecuteCommandLists + fence wait) from inside the OMSetRenderTargets hook
  killed the game silently. Fix: custom heap (WRITE_COMBINE, L0) texture created in SHADING_RATE_SOURCE and filled with
  WriteToSubresource - no GPU submission.
- 161 (render 1100x1152): off 55.3 / VRS (0.5 full, 0.8 2x2, 4x4 outside) 56.6 / foveated DLSS 63.8 / both 65.0 fps.
  ~450 scene passes per eye-frame get the image. 4x4 = visible 8x8 blocks after DLSS -> defaults now 0.6 full rate,
  2x2 everywhere else, 4x4 off. The game is compute-bound (lighting / RT / DLSS); VRS only cuts pixel shading.

## Fix 18 (2026-11-05): bluescreens on live centre resize, cinematic letterbox, 1.3x left-eye shift (tests 165-169)
- User: two bugchecks 0x139/3 (nvlddmkm), BOTH while changing the foveated DLSS centre 0.6 -> 0.5 in game. fovdlss
  released the old centre feature (NGX ReleaseFeature), its output / MV-crop / TAA history textures at once, while
  command lists already recorded (queued frames + the replay) still referenced them -> GPU use-after-free. Fix: deferred
  release (FlushDead at each evaluate frees objects older than 5 s and 30 evaluates). Also pushed to sharedtools.
  (The earlier minidumps - same nvlddmkm offset per driver - may share this cause if the size was changed then too.)
- sharedtools\foveated_dlss had FF16's fix (evaluate OutWidth/OutHeight = render size under SL 2.4 -> size from create);
  merged here (logs "fovdlss: evaluate sizes" twice).
- Cinematic letterbox: in-engine cutscenes draw opaque black bars into the UI target. psHud makes a pixel transparent
  when its row is opaque black at 9 samples across, the band touches the top/bottom edge (row 0.002 / 0.998 black too)
  and the middle row is not black (fades kept). Test 169: normal HUD still shows (coverage 1.2 %), shader compiles.
- 1.3x (5176x4448 per eye, user 5090): left eye shifted. User dumps: in0 (left f26 input, restored) aligned with the
  right eye; after the left replay range texture a2 (f26) and the final left image are translated by exactly
  (+667, +667) px (ORB + RANSAC, scale 1.000); RwithL (left DLSS copy in the right DLSS output) aligned. So one replayed
  post pass reading the f26 input writes it shifted; the game's own (right) pass is fine.
  Dev PC: 5176x2224 no shift (width is not it); 2588x4448 = no replay range (portrait); 5176x4448 / 4600x4448 /
  5176x4320 run at ~1 fps and the game dies on any dump (12 GB). Above ~4300 px height the left inputs restore
  2 full-size f26 targets from one source ("X<-S, Y<-S") and the 1/4-res f26 too. Open; user asked for a 1.3x dump with
  foveated DLSS off. Live resize (render_resolution reload) is refused once per launch - each size needs a launch.
- Debug: dumplite.trigger (left inputs + compL/compR only, no range set / bmp dumps).

## Release 1.0 (2026-11-05)
- User: "make a 1.0 release with what we got". builds\release_1.0 = stage 28 binaries (version.dll, openxr_loader.dll,
  version.pdb), user README (requirements: DLSS/FSR/XeSS, frame gen off, DLSS DLL >= 310; install; controls L3+R3 or
  Insert / F8 / F9 / F10; menu; caveat: too high a render resolution (~4300+ px eye height) breaks the left eye ->
  lower render scale) and DAVGVR_1.0.zip (dlls + README). Foveated DLSS stays off by default (README recommends ON).
