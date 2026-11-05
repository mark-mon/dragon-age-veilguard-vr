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
- Use DLSS Swapper to swap in a DLSS 4.5 DLL, because the game's included 3.7 DLL makes the right eye jaggy.
  (davgvr.log shows the DLSS version in use.)

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
- Foveated DLSS > Foveated DLSS (upscaling) - ON by default: DLSS reconstructs only the centre of
  each eye (centre width / height, default 0.6), the periphery gets a cheap upscale + temporal smoothing. Big GPU saving, hard to notice in the headset.
  "Centre follows: Eye tracking" uses OpenXR eye tracking (XR_EXT_eye_gaze_interaction) on headsets that offer it.
  "Show centre border" draws the centre region in red.
- Foveated DLSS > VRS foveated rendering: experimental, small gain (coarser shading toward the edges).
- HUD: panel size / distance / height, world- or head-anchored. Cutscene letterbox bars are removed from the panel.
- Frame pacing "Game" (default) lets SteamVR reprojection / motion smoothing work at the game's real frame rate;
  Frame limit (refresh / N, default 2) gives a steady cadence for frame generation tools.

DEFAULT SETTINGS (what you get without changing anything; all are in the menu and in config.txt)
  View      Mode: Stereo (3D)                 World scale: 1.00 (life size)
            Level camera: on                  Pose latency: 1 frame          Swap eyes: off
  HUD       HUD on its own panel: on          Panel: 1.40 m wide, 1.50 m away, 0.05 m below eye level, world-anchored
  Display   Render resolution: auto (headset's per-eye size)                 Render scale: 1.00
            Frame pacing: Game (real fps)     Frame limit: 2 (= 45 fps @ 90 Hz)   Per-eye DLSS: on
            Sharpen (LumaSharpen): 0 (off)
  Foveated  Foveated DLSS: ON                                                Centre follows: image centre
            Centre width / height: 0.60 / 0.60                               Periphery smoothing (TAA): on
            Periphery DLSS (2nd pass): off    Show centre border: off
            VRS foveated rendering: off       VRS full-detail size 0.60, 2x2 size 2.00, 4x4 outside: off
  Menu      1.00 m away, 0.80 m wide, world-anchored; open with L3 + R3 held 0.3 s or Insert
  Flat mode Virtual screen 3.2 m wide, 2.5 m away
  (Upgrading from a test build? Delete config.txt once to get these defaults - an existing file is kept.)

RECOMMENDED SETUP: N/2 + OFXR-BRIDGE (the defaults are set up for this)
This game is far too heavy for a native 90 fps in VR (two full views per frame). What plays well:
- Frame limit 2: the game runs at exactly half the headset refresh (45 fps at 90 Hz) with a steady cadence.
  On a 5090 this gave 45 fps at ~70 % GPU, versus 36 fps at 98 % GPU unlimited (the unlimited game and the VR
  compositor fight over the GPU).
- OFXR-Bridge frame generation 2x on top -> 90 fps in the headset. (Without it, SteamVR Motion Smoothing / Oculus
  ASW can fill the gaps instead - Frame pacing must stay on "Game" for either.)
- Foveated DLSS on, render scale 1.0 to start; then raise the render scale while GPU use stays below ~85 % at
  45 fps (missed frames show more with frame generation). Stay at or below ~1.25 - see known issues.
- Changing the Frame limit while playing once crashed the game - set it before loading a save.

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
