Dragon Age: The Veilguard VR (DAVGVR) - version 1.0 (2026-11-05)
================================================================
Native stereo VR for Dragon Age: The Veilguard (Steam, D3D12) through OpenXR: two real engine views per frame
(true 3D, not a flat screen), head tracking, the HUD on its own panel, an in-headset settings menu, and foveated
DLSS for extra performance.

REQUIREMENTS
- An OpenXR runtime (SteamVR, Virtual Desktop / VDXR, Oculus, WMR ...) and a strong NVIDIA RTX GPU (DLSS).
- In the game's graphics settings:
    Upscaling: DLSS (recommended). FSR / XeSS also give stereo; "no upscaler" (native) does NOT give stereo.
    Frame generation: OFF.   Display mode: windowed or borderless (the mod resizes the window to the VR size).
- DLSS DLL 310 or newer (e.g. 310.6) in the game folder. The game's own 3.7 DLL makes the right eye jaggy -
  swap it with DLSS Swapper (or copy nvngx_dlss.dll 310.x into the game folder). The log shows the version.

INSTALL
1. Copy version.dll and openxr_loader.dll into the game folder:
   ...\steamapps\common\Dragon Age The Veilguard  (next to "Dragon Age The Veilguard.exe")
   (version.pdb is optional - it only makes crash reports more useful.)
2. Start the game, then start your VR runtime / put the headset on. config.txt is created on the first run.
Uninstall: delete version.dll, openxr_loader.dll (and version.pdb, config.txt, davgvr.log, the DAVGVR folder).

CONTROLS
- Settings menu: hold L3 + R3 (both stick clicks) on the gamepad, or press Insert. D-pad / arrows to move,
  A / Enter to change, B / Backspace to go back. Changes are saved to config.txt.
- F9 recentre, F10 show / hide the HUD panel, F8 cycle mode (stereo / head-tracked mono / flat screen).

MENU HIGHLIGHTS
- Display > Render resolution "auto" + Render scale: the headset's own per-eye resolution x scale.
  Applied at the next game start (the window size is set once per launch).
- Display > Per-eye DLSS: keep ON (otherwise both eyes share a DLSS history = ghosting).
- Display > Sharpen (LumaSharpen) 0 - 5.
- Foveated DLSS > Foveated DLSS (upscaling) - OFF by default, recommended ON: DLSS reconstructs only the centre of
  each eye (centre width / height, default 0.6), the periphery gets a cheap upscale + temporal smoothing. Big GPU saving, hard to notice in the headset.
  "Centre follows: Eye tracking" uses OpenXR eye tracking (XR_EXT_eye_gaze_interaction) on headsets that offer it.
  "Show centre border" draws the centre region in red.
- Foveated DLSS > VRS foveated rendering: experimental, small gain (coarser shading toward the edges).
- HUD: panel size / distance / height, world- or head-anchored. Cutscene letterbox bars are removed from the panel.
- Frame pacing "Game" (default) lets SteamVR reprojection / motion smoothing work at the game's real frame rate;
  Frame limit (refresh / N) gives a steady cadence for frame generation tools.

KNOWN ISSUES / CAVEATS
- TOO HIGH A RENDER RESOLUTION CAN BREAK THE STEREO EFFECT. Above roughly 4300 pixels of height per eye (e.g. render
  scale 1.3 on a ~3400 px-tall headset) the left eye's picture comes out shifted and the 3D effect falls apart.
  If the depth looks wrong or one eye looks offset / smeared at the edge, LOWER the render scale (1.25 was fine
  on an RTX 5090 at 5000 px per eye) and restart the game.
- A render resolution / scale change needs a game restart.
- Native (no upscaler) mode has no stereo - use DLSS (best), FSR or XeSS.
- Eye-tracked foveation was only tested with a simulated gaze (no eye-tracking headset here).
- It is a heavy game: two full views per frame. Use foveated DLSS, a DLSS performance mode and the render scale to
  find your balance.

LOGS / BUG REPORTS
davgvr.log in the game folder (plus anything in the DAVGVR folder after a "Debug > Dump ..." menu action).
