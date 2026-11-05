# Dragon Age: The Veilguard VR

Native stereo VR for **Dragon Age: The Veilguard** (Steam, D3D12) through OpenXR. It is a proxy `version.dll`
that loads next to the game.

- **True 3D:** the engine renders two views per frame. The left eye replays the game's post-processing over its own
  DLSS output. Head tracking is tied to the exact frame each pose was rendered for.
- **Upscalers:** per-eye DLSS through Streamline. FSR and XeSS also work.
- **HUD and menu:** the HUD sits on its own panel (cutscene letterbox bars removed), and there is a settings menu in
  the headset (hold L3 + R3, or press Insert).
- **Foveated DLSS:** DLSS reconstructs only the centre of each eye; the periphery gets cheap upscaling plus temporal
  smoothing. The centre can follow OpenXR eye tracking.
- **VRS foveated rendering:** experimental.
- **Extras:** LumaSharpen, frame pacing and a frame limiter for SteamVR motion smoothing or frame generation.

## Download / install

Get `DAVGVR_1.0.zip` from [Releases](../../releases). Copy `version.dll` and `openxr_loader.dll` into the game
folder (next to `Dragon Age The Veilguard.exe`), start the game, then your VR runtime.
Use DLSS Swapper to swap in a DLSS 4.5 DLL, because the game's included 3.7 DLL makes the right eye jaggy.
Full player notes are in [release/README.txt](release/README.txt). They cover:
- requirements: DLSS / FSR / XeSS upscaling, frame generation off
- controls
- the menu and default settings
- known issues

**Recommended setup (the defaults):** frame limit 2, i.e. a steady 45 fps at 90 Hz, plus OFXR-Bridge 2x frame
generation for 90 fps in the headset. Foveated DLSS is on, starting at render scale 1.0.

**Caveat:** too high a render resolution (roughly above 4300 px of eye height) breaks the left eye and the stereo
effect. If depth looks wrong, lower the render scale and restart the game.

## Building

The source was cross-built from Linux with clang-cl + lld-link against an MSVC / Windows SDK sysroot. `build.sh`
expects a toolchain wrapper at `$TC/build.sh`; the default is `/home/claude/tc`. Output goes to
`dist/version.dll`, `dist/version.pdb` and `dist/openxr_loader.dll`. The sources also build with MSVC: compile
everything listed in `build.sh` into a DLL with `src/exports.def`.

## Layout

| Path | Contents |
|---|---|
| `src/` | The mod. `fbview.cpp` (engine views and stereo poses), `replay.cpp` (left-eye post-process replay), `slhook.cpp` (Streamline / DLSS), `fovdlss.cpp` (foveated DLSS), `vrs.cpp`, `xr_host.cpp` (OpenXR), `menu.cpp` |
| `vrkit/` | Settings menu (Dear ImGui) |
| `third_party/` | MinHook, OpenXR headers, OpenXR loader (x64) |
| `tools/` | Reverse-engineering helpers |
| `FINDINGS.md` | Development notes: engine structures, every fix and test |

## Credits

- [MinHook](https://github.com/TsudaKageyu/minhook) (BSD-2)
- [OpenXR SDK](https://github.com/KhronosGroup/OpenXR-SDK) (Apache-2.0)
- [Dear ImGui](https://github.com/ocornut/imgui) (MIT)
- The foveated DLSS idea comes from [Cheeky Foveated DLSS](https://github.com/ClarkCheekyKent/CheekyFoveatedDLSS).
  This is an independent implementation on the public NGX API.
