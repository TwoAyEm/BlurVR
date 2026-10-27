# BlurVR 1.7.1c

**Seated VR for Blur (SteamVR / OpenVR, or OpenXR).**

Play Blur in a headset, seated. Built and tested with a Valve Index; any SteamVR headset should work.

> [!NOTE]
> Unofficial fan mod; not affiliated with the game's publisher or developer.

[Watch the video](https://www.youtube.com/watch?v=-Gwqe6Wcjd8) · [Download the latest release](https://github.com/TwoAyEm/BlurVR/releases)

**Contents:** [What it does](#what-it-does) · [Requirements](#requirements) · [Install](#install) · [Resolution](#resolution) · [Graphics quality](#graphics-quality) · [Keys](#keys) · [Settings](#settings) · [OpenXR](#openxr-optional) · [How it works](#how-it-works) · [Known limitations](#known-limitations) · [Troubleshooting](#troubleshooting) · [What is new](#what-is-new) · [License](#license)

## What it does

- **Menus:** the game is shown on a floating screen in front of you (SteamVR overlay).
- **Races:** true stereo 3D, seated. Your head moves the in-game camera (chase view and in-car view).
  - The camera position is fixed to the car; you look around from your seat.
  - Shadows, lights, motion blur and the HUD are all handled per eye.
  - The rear-view mirror is a real 3D panel: the road behind you is rendered once per eye, with depth.
  - The markers over the AI cars (name, weapon, health) are shown over their cars in the world, at their depth, whichever way you look, and go to the cars nearest to your head. The HUD no longer sways with the car's acceleration.
- Game controls are unchanged.

## Requirements

- Windows
- SteamVR (or, instead, an OpenXR runtime that has a 32-bit build: see [OpenXR](#openxr-optional))
- Blur (32-bit; any version of the game should do, Steam or not, see Version 1.3 under [What is new](#what-is-new))
- A graphics card that runs Direct3D 11 (any recent one)

## Install

1. Download the zip of the latest version from the [releases page](https://github.com/TwoAyEm/BlurVR/releases) and unzip it so that the `BlurVR` folder sits inside your Blur folder (next to `Blur.exe`), e.g. `...\steamapps\common\Blur\BlurVR\`
2. Run `BlurVR\install.bat` (copies `d3d9.dll` next to `Blur.exe`). `uninstall.bat` removes it again.
3. Start SteamVR, then launch Blur from Steam. (To use OpenXR instead of SteamVR's OpenVR, see [OpenXR](#openxr-optional).)
4. In the game's video options (**Options > Graphics > Resolution**) choose the size for your headset (see [Resolution](#resolution)).
   It is 2160x1200 until you change `Resolution` under `[VR]` in `BlurVR.ini`, or pick another size in the options.
   The game starts at that size from then on. V-Sync is turned off by BlurVR itself; the headset sets the frame rate.

## Resolution

In races the picture is rendered side by side (left eye | right eye), so each eye gets **half** the width.

| Size | Per eye | Notes |
|---|---|---|
| 1920x1080 | 960x1080 | light, soft; not in the list, but you can write it as `Resolution=1920x1080` |
| 2160x1200 | 1080x1200 | BlurVR's default; the Vive's panel size |
| 3840x2160 | 1920x2160 | sharp, heavy; not in the list either, `Resolution=3840x2160` works |
| 4936x2740 | 2468x2740 | what SteamVR itself renders for a Valve Index; needs a strong graphics card |

In VR, BlurVR replaces the game's resolution list (the monitor's 16:9 and 4:3 modes mean nothing in a headset) with the sizes below: the headset's panel size per eye x 1.4 on each side (SteamVR's default supersampling), both eyes side by side. The `Resolution` in `BlurVR.ini` (`[VR]` section) is always in the list too, whatever it is. Flat (`Stereo=0`) the game's own list is untouched.

| Headset (panel per eye) | Size in the list |
|---|---|
| HTC Vive, Oculus Rift CV1 (1080x1200) | 3024x1680 (2160x1200 = the panel's own size: the default, light) |
| Oculus Rift S (1280x1440) | 3584x2016 |
| Valve Index, Vive Pro, Meta Quest 1 (1440x1600) | 4032x2240 (4936x2740 = SteamVR's own size for the Index) |
| Meta Quest Pro (1800x1920) | 5040x2688 |
| Meta Quest 2, Quest 3S (1832x1920) | 5128x2688 |
| Meta Quest 3 (2064x2208) | 5780x3092 |
| Valve Steam Frame, HP Reverb G2, Pico 4 (2160x2160) | 6048x3024 |
| HTC Vive Pro 2, Vive Focus 3 (2448x2448) | 6856x3428 |
| Bigscreen Beyond (2560x2560) | 7168x3584 |

Your monitor has no such mode, so the game runs in a window (a window larger than your screens is normal); only the headset sees the picture. Bigger is sharper but heavier: start with the Vive-sized entry and go up while it stays smooth.

In VR the size is kept in `BlurVR.ini` (`[VR] Resolution=WxH`): the game starts at it, even when Windows cannot make a window that big, and a size you pick in the game's video options is written there. The game's own settings (`settings.xml`) keep the size of the flat game, so with `Stereo=0` or `Enabled=0` it starts at your normal resolution again. BlurVR puts the flat size back when the game exits (and at the next start if the game was killed); the untouched file is copied to `settings.xml.blurvr-backup` beside it, and `BlurVR\display_backup.ini` holds the flat size.

## Graphics quality

**Use the game's own quality setting** (the video options in the game's menus) to trade picture quality for frame rate. It adjusts the reflections and the shadows, among other things, for the whole picture and without any help from BlurVR; on the lowest level a Valve Index held 90 frames a second at 3584x2016 in a race. BlurVR's own switches for single effects (`Reflections` under `[VR]`, and `NoLensFlare`) do part of the same job: they are kept for now, but the game's own setting is the preferred way and they may be removed in a later version.

## Keys

| Key | What it does |
|---|---|
| **F9** | Recenter (sit comfortably, look straight ahead, press F9). This is SteamVR's own recentre, so recentering in SteamVR does the same. BlurVR also recenters at the start of every race (set `AutoRecenter=0` in `BlurVR.ini` to turn that off). |
| **F10** | Head tracking on/off (stereo image stays, camera stops following your head) |
| **F8** | Mode override: AUTO → FORCE MENU → FORCE RACE. Use it if the mod picks the wrong mode. Flat (`Stereo=0`) it does the same job for the menu key repeat limit: AUTO → forced on → forced off. |
| **Num6** | 3D rear-view mirror on/off (same as `Mirror3D` in the ini) |
| **Num7** | Timing probe on/off (see [Troubleshooting](#troubleshooting)) |
| **Num8** | Save what the headset sees as picture files (see [Troubleshooting](#troubleshooting)) |
| **Num9** | FPS counter on/off (same as `FpsCounter` in the ini) |

## Settings

`BlurVR.ini` holds only the options most people need (the mode, resolution, shared transport, desktop window). Every option is listed below; add a line under its `[section]` in `BlurVR.ini` to use it (keys left out use the default shown). Edit with the game closed. Delete `BlurVR.ini` to go back to all defaults.

The mod has three states:

| `Enabled` | `Stereo` | State |
|---|---|---|
| `0` | any | **Off.** The mod does nothing at all. |
| `1` | `1` | **VR.** |
| `1` | `0` | **Flat.** The game stays on the monitor, SteamVR is never started and only the `[Display]` and `[Input]` settings apply. The `[VR]` and `[Render]` settings are ignored when flat (the extra resolution is not offered either), so the game starts at the size it saved. |

> [!NOTE]
> **Settings that moved** are read from their new place only. An older `BlurVR.ini` keeps working for `Stereo`, `ForceNoVSync`, `SharedTransport`, `DesktopView`, `DesktopPresentEvery` and `FitDesktopWindow` (still read from `[Render]`) and for `CustomWidth` / `CustomHeight` (read as `Resolution`).
> `UiGlow`, `NoBloom`, `LoadingGlow`, `MaxFps`, `StartWidth`, `StartHeight` and `StartAtSavedSize` are gone, and `NoLensFlare` is only read from `[VR]`.
> `Bloom` replaces `UiGlow`, `LoadingGlow` and `NoBloom` (`Bloom=0` is "no glow anywhere"). `ForceNoVSync=0` follows the display's refresh rate.

### `[General]`

| Key | Default | What it does |
|---|---|---|
| `Enabled` | `1` | Master switch. 0 = the mod does nothing at all (the game runs as if it were not installed) |
| `Stereo` | `1` | 1 = VR. 0 = flat: no SteamVR and no VR changes to the game; only `[Display]` and `[Input]` apply |
| `Log` | `0` | 1 = write `BlurVR\BlurVR.log` (for bug reports; the log of the previous run is kept as `BlurVR.previous.log`) |
| `RecenterKey` | `0x78` | Virtual-key code (hex): F9 recenter |
| `ToggleKey` | `0x79` | F10 head tracking on/off |
| `ModeKey` | `0x77` | F8 cycle mode (auto / force menu / force race) |
| `StartMode` | `0` | 0 = auto (floating screen in menus, seated 3D in races), 1 = always floating screen, 2 = always race/3D |
| `AutoRecenter` | `1` | 1 = also recentre every time a race starts |
| `ThreadedInit` | `1` | Start the VR runtime (OpenVR or OpenXR) on a worker thread (keeps SteamVR from flagging the game as not responding) |
| `DeferVR` | `1` | Keep the game flat (SteamVR theatre) until the first level starts loading, then start VR |

### `[Tracking]`

| Key | Default | What it does |
|---|---|---|
| `Rotation` | `1` | Head rotation moves the camera |
| `Position` | `1` | Head position (lean in your seat) moves the camera |
| `PositionScale` | `1.0` | Game units per metre |
| `IpdScale` | `1.0` | Scale of the eye separation (1.0 = your real IPD) |
| `EyeTransform` | `1` | 1 = each eye's position (and rotation, on headsets with canted displays) from SteamVR; 0 = half the IPD to each side |

### `[Display]`  (VR and flat)

| Key | Default | What it does |
|---|---|---|
| `ForceNoVSync` | `0` | 1 = V-Sync off: the game runs as fast as it can. 0 = the game's own V-Sync setting (with V-Sync on the frame rate follows the monitor, e.g. 60). **In VR this key is ignored:** V-Sync is always off there and the headset's own refresh rate (90, 120, 144 ...) sets the frame rate |

### `[Input]`  (VR and flat)

| Key | Default | What it does |
|---|---|---|
| `KeyRepeatMs` | `60` | In the game's menus a held key or pad button repeats once every this many milliseconds, whatever the frame rate (0 = as the game does it) |

Blur counts how long a key is held in frames and, after a second, repeats it every frame, so at 300 fps a held arrow key scrolls a menu 300 times a second. BlurVR instead gives the game a tap, a wait of half a second, then a tap every `KeyRepeatMs`; the game never sees the key held.

- Races are not touched (the arrow keys steer there) except while a pause or results menu is up over one. BlurVR recognises it by the full-screen dim the game draws behind it (`Log=1` writes `race menu: up` / `gone` to `BlurVR.log`).
- If it is not recognised, **F8** forces the limit on: press F8 once while the menu is up, twice = off, three times = automatic again.
- The pad (XInput) gets the same rule: its buttons, the D-pad and the left stick's four directions. The triggers and the right stick do not.

### `[VR]`  (ignored when `Stereo=0`)

| Key | Default | What it does |
|---|---|---|
| `Api` | `openvr` | How the headset is reached: `openvr` = SteamVR, `openxr` = the system's active OpenXR runtime (see [OpenXR](#openxr-optional): it needs a runtime with a 32-bit build). If OpenXR cannot start the game stays flat and `BlurVR.log` says why. |
| `OpenXRRuntime` | (empty) | Only with `Api=openxr`: the full path of the `.json` of the OpenXR runtime to use instead of the system's active one (empty = the system's). Used for this game only. |
| `Resolution` | `2160x1200` | The size of the VR picture, both eyes side by side (see [Resolution](#resolution)). It is the extra entry in the game's resolution list and the size the game starts at. Pick another size in the game's video options and BlurVR writes it here for you. The game's own settings keep the flat game's size. |
| `Bloom` | `100` | Strength of the game's glow around its menus, UI and loading screens, in percent (0 = none, 200 = double) |
| `NoLensFlare` | `0` | 1 = drop the flare sprites and the glow around lights |
| `MotionBlur` | `1` | 0 = no motion blur: a sharper picture, but **no real gain in frame rate** (the blur is about 0.1 ms of a 13 ms frame, measured with a Valve Index), so turn it off for the look, not for speed. The game's blur pass then writes the picture without blur (one texture read a pixel instead of the blur's many). 1 = the game's own motion blur, drawn for each eye. |
| `Reflections` | `1` | **Prefer the game's own quality setting (see [Graphics quality](#graphics-quality)); this switch may be removed later.** 0 = leave the game's reflection pass out in races: the water then shows no reflection (it looks fine without it). BlurVR turns off the game's own switch for it (a tweakable the game tests before it culls or draws anything for the pass), so the game does none of that work for it: in a test on the LA River at 3584x2016 that took 0.7 ms off a frame of 13 ms (about 76 to 81 frames a second). The menu showroom keeps its floor's reflection. If the game's switch is not found in your build, the pass's draw calls are left out instead, which saves less. 1 = the game's own reflections. |
| `SharedTransport` | `1` | Run the game on a Direct3D 9Ex device and hand each finished race frame to SteamVR on the GPU instead of copying it through the CPU (saves several ms a frame, most at large sizes). On by default; it falls back to the CPU copy by itself when it cannot work. Set 0 if textures look wrong or the picture is black |
| `DesktopView` | `off` | What the window on the desktop shows: `off` = no picture of the game at all: the window says "Playing in VR" with the game's logo and a reminder to click the window for the keyboard and gamepad (also in the menus and while loading), and is painted twice a second, which saves the GPU the copy of the game's picture. `left` = the left eye's view in a race (cropped to the window's shape and drawn at the window's own size, which is cheap), `right` = the right eye's, `both` = the whole side-by-side picture. The headset's picture is not affected. `off` needs a windowed game without multisampling (otherwise the window shows the left eye's view). Before 1.5 the default was `left`. |
| `DesktopPresentEvery` | `1` | In a race, the window on the desktop (a mirror of the headset) really updates only every n-th frame (2-4 saves a little GPU time; 1 = every frame) |
| `FitDesktopWindow` | `1` | 1 = the game's window is never made bigger than the work area of its monitor (at a 3840x2160 setting it would cover the whole desktop, or two monitors). The game still renders at the size it chose; the window shows one eye's view of it scaled to fit (`DesktopView`). Windowed mode only. 0 = as the game wants. |
| `FpsCounter` | `0` | 1 = a small panel in the headset shows how many frames a second the game presents (in races and in the showroom; **Num9** switches it in game), with a graph of the last 18 seconds under it. It stays in front of your face, a little to the lower left, and is green at the headset's refresh rate, yellow below it and red far below (a game that SteamVR holds to half the headset's rate shows red; with `Api=openxr` the rate is the one the OpenXR runtime reports for its display). The line of the graph has the same colours and is scaled from the lowest to the highest rate of those seconds. Off by default |

### `[Render]`  (VR only)

| Key | Default | What it does |
|---|---|---|
| `FovScale` | `1.0` | Below 1 zooms in, above 1 zooms out |
| `SeamGutter` | `0` | Black pixels at the inner edge of each eye (stops glow bleeding across the seam) |

Advanced (leave alone unless told to): `MainPassMinWidthFraction=0.9`, `RequireDepthBuffer=1`, `HalfResPasses=1`, `ReflectionLookupFix=1`, `ReflectionFlip=-1`.

### `[Hud]`

| Key | Default | What it does |
|---|---|---|
| `WorldLocked` | `1` | 1 = the HUD hangs in front of the car, 0 = glued to your view |
| `Scale` | `0.6` | Panel width as a fraction of one eye's width |
| `Distance` | `2.0` | Apparent distance in metres |
| `OffsetY` | `0.0` | Vertical offset (fraction of height, + = down) |
| `XOffset` | `0.0` | Extra convergence per eye, as a fraction of one eye's width |
| `VeilRace`, `VeilResults`, `VeilMenu` | `120`, `36`, `100` | How dark the veil behind popups is (0-255) |
| `DimStrongPct` | `60` | How dark the scene gets behind pause/results menus (0-100) |
| `DropBackdrop` | `1` | Drop the game's full-screen dim/blur copies (they would blank the HUD layer) |
| `HideFullscreenOverlay` | `1` | Hide the HUD layer while a full-screen video plays |
| `CountInDrop` | `1` | Hide the race-start letterbox pieces (0 = draw them into the HUD) |
| `CoverageEvery` | `8` | How often (in frames) each UI draw is checked for covering the screen (1 = every frame; slower) |
| `Mirror3D` | `1` | Rear-view mirror as its own 3D panel (real depth) |
| `MirrorConvergence`, `MirrorEyeScale`, `MirrorHeadParallax` | `1.5`, `1`, `1` | Depth that sits at the panel (m), eye separation scale, head parallax |

The markers over the AI cars are not part of this panel and have no settings (see [What it does](#what-it-does)).

### `[Menu]`

The floating screen in menus.

| Key | Default | What it does |
|---|---|---|
| `Distance` | `2.2` | Distance (m) |
| `Width` | `2.6` | Width (m) |
| `HeightOffset` | `0.0` | Height relative to your eyes (m) |

### `[Debug]`

| Key | Default | What it does |
|---|---|---|
| `KeyMirror3D` | `0x66` (Num6) | Key for the 3D mirror switch |
| `KeyPerf` | `0x67` (Num7) | Key for the timing probe switch |
| `KeyDump` | `0x68` (Num8) | Key for saving the headset picture as files |
| `KeyFps` | `0x69` (Num9) | Key for the FPS counter switch |
| `AdjustViewRotation` | `1` | |

### `[Culling]`  (VR only)

The game decides what to draw from a view frustum around your head (both eyes' field of view, the way you look) instead of from its own, narrower camera. BlurVR finds the game's camera code in the running `Blur.exe` and checks that its data is laid out as expected; if not, the game is left alone and `BlurVR.log` says why (`game camera: ...` and `head frustum: ...` lines). Tried so far: the Steam build and a non-Steam build. The markers over the AI cars and the water's reflection use the same hooks, so the markers stay on the HUD panel and the reflection is culled by the game's own camera when the code is not found.

| Key | Default | What it does |
|---|---|---|
| `MarginDeg` | `6.0` | Degrees added to every side of that frustum (covers canted displays and quick head turns) |
| `PullBack` | `0.3` | Metres the frustum's apex is moved back (covers the eyes' offset and leaning) |

### `[Perf]`

| Key | Default | What it does |
|---|---|---|
| `Enabled` | `0` | 1 = write timing lines to the log once a second while racing (needs `Log=1`) |

### `[Foveation]`

| Key | Default | What it does |
|---|---|---|
| `Report` | `1` | 1 = write the headset model, refresh rate, render size and lens hidden area to the log at VR start (needs `Log=1`) |

## OpenXR (optional)

BlurVR reaches the headset through SteamVR's OpenVR by default. Set `Api=openxr` under `[VR]` in `BlurVR.ini` to use OpenXR instead: the game then runs on the system's active OpenXR runtime (SteamVR's or another one). Without the line, or with `Api=openvr`, nothing changes.

- **The runtime needs a 32-bit build.** Blur is a 32-bit program and sees only the OpenXR runtime that is registered for 32-bit programs: the `ActiveRuntime` value in `HKEY_LOCAL_MACHINE\SOFTWARE\WOW6432Node\Khronos\OpenXR\1` has to point to a runtime `.json` whose library is a 32-bit DLL. SteamVR registers one from version 2.17 (answer yes when it asks whether to make SteamVR the default OpenXR runtime). Whether another runtime has a 32-bit build is for its maker to say. A 32-bit `.json` that is not the registered one can be given in `OpenXRRuntime` under `[VR]`. OpenXR support is new: it has been tried with SteamVR's OpenXR runtime and a Valve Index, where it looked the same as OpenVR, and with Virtual Desktop's OpenXR runtime and a Pico 4 (see Runtimes below); other runtimes and headsets have not been tried. Please send `BlurVR.log` with any problem.
- **If OpenXR cannot start** (no 32-bit runtime, no headset, a failed session) the game stays on the monitor and `BlurVR.log` (with `Log=1`) says why, in lines that start with `OpenXR:`. BlurVR does not switch to SteamVR's OpenVR by itself; set `Api=openvr` for that.
- **What is the same:** races, the HUD, the mirror, the markers, the settings and the keys. **What differs:** the recentre key (F9, and the recentre at the start of every race) is BlurVR's own, because OpenXR has no way to ask the runtime to recentre (a recentre done in the runtime itself is followed too), and the floating menu screen is an OpenXR quad layer instead of a SteamVR overlay.
- **Runtimes:** tried with SteamVR's OpenXR runtime on a Valve Index, and with Virtual Desktop's OpenXR runtime (VDXR) on a Pico 4 streamed from a PC with a Radeon RX 7600. Virtual Desktop's streamer folder has a 32-bit runtime file, `OpenXR\virtualdesktop-openxr-32.json`, and that run gave its path in `OpenXRRuntime` under `[VR]`. On the Pico 4 the menus and races ran, and with `MotionBlur=0` and `Reflections=0` a race held the headset's 72 frames a second at 3024x1680 (`SharedTransport` on). The game crashed there when a race ended and the showroom was about to load, until `Blur.exe` was patched to use more than 2 GB of memory (a "[4 GB patch](https://ntcore.com/4gb-patch/)", large address aware); after that two races in a row, the way back to the showroom and the exit went through cleanly.
- `openxr_loader.dll` in the `BlurVR` folder is the 32-bit OpenXR loader of the Khronos Group (see `LICENSE_openxr.txt`).

## How it works

### Menu or race?

When the game opens files under `levels\showroom` it is a menu; any other `levels\<track>` is a race.

### In short

Blur is 32-bit Direct3D 9, so BlurVR patches it at runtime. Its vertex shaders get `g_mViewProj`; BlurVR decomposes that matrix to recover the camera and projection, then in the main 3D pass draws every object twice (left half / right half of the render target) with a per-eye offset, the headset's per-eye projection and your head pose. Screen-space passes (motion blur, shadow decals, deferred lights) are re-expressed in each eye's own camera. The HUD is drawn once per eye. The finished image is split and sent to SteamVR.

Source code: [github.com/TwoAyEm/BlurVR](https://github.com/TwoAyEm/BlurVR). Start with [`source/NOTES.md`](https://github.com/TwoAyEm/BlurVR/blob/main/source/NOTES.md); build with `source/build.bat` on Windows or `build.sh` with Zig.

## Known limitations

- The rear-view mirror still decides what to draw with the game's own camera.
- HUD detection is heuristic (worked out from how the game draws), so an unusual screen can show UI pieces in the wrong place.
- Menus are the game image on a flat screen; use mouse/keyboard/controller as normal.
- The markers over the AI cars were tried on the Steam build and on a non-Steam build; with another build of the game they may stay on the HUD panel. A popup in the middle of the screen can take a piece of itself along with a marker, and a lock-on reticle or rival arrow further from the marker than its picture reaches is cut off.
- By default (`DesktopView=off`) the window on the desktop only says "Playing in VR", but it is still the game's window: closing it quits the game, and **the keyboard and the gamepad go to the game only while that window has the focus**, so click it once after the game has started (it says so under "Playing in VR"). With `left` or `right` it is a mirror of one eye, cropped to the window's shape; it is not the headset's full view.
- The menu key repeat limit is also on while a race loads and during its countdown (BlurVR sees the same full-screen fill there), so a key held down through the start reaches the game as taps until the countdown ends.

## Troubleshooting

- **The view is still culled at the edges / looking away from the direction of travel:** set `Log=1` under `[General]`, start a race for a minute, and look in `BlurVR.log` for the lines that start with `game camera:` and `head frustum:` (the water's reflection has `reflection frustum:` lines of its own). They say whether the game's camera code was found in your `Blur.exe`, whether its data is laid out as expected, and how many frustums were replaced. If they say it was not found, send `BlurVR.log` and the size and date of your `Blur.exe`.
- **The markers over the AI cars stay on the HUD panel:** they need `Stereo=1` and the game's camera code to be found. With `Log=1` the `game camera:` and `ai hud:` lines in `BlurVR.log` say whether the game's code was found and why a check failed; send the log and the size and date of your `Blur.exe`.
- **Nothing happens / the game is flat:** start SteamVR first; run `uninstall.bat` to go back to the plain game. With `Api=openxr` start the OpenXR runtime instead and read the `OpenXR:` lines in `BlurVR.log` (set `Log=1`): they say whether a 32-bit runtime was found, whether it sees a headset and how far the start got (see [OpenXR](#openxr-optional)).
- **Something looks wrong in the headset:** press **Num8** while it is on screen. BlurVR saves `BlurVR_dump_<time>_picture_*.bmp` (the whole side-by-side picture, large files) and one file per HUD layer in the BlurVR folder: attach them to the report.
- **Bug reports** should also include `BlurVR.log`: set `Log=1` under `[General]`, reproduce the problem, close the game, and send `BlurVR\BlurVR.log`.
- **Slow:** lower the game's own quality setting first (see [Graphics quality](#graphics-quality)), then `Resolution` under `[VR]`. **Num7** (or `Enabled=1` under `[Perf]`, with `Log=1`) writes timing lines to the log.
- **Odd textures or a black picture:** if `SharedTransport` (on by default in VR) gives these on your graphics card, add `SharedTransport=0` under `[VR]`.
- **To run the game flat,** set `Stereo=0` (SteamVR is not used; the `[Display]` and `[Input]` settings still apply), or `Enabled=0` (the mod does nothing), or run `uninstall.bat`.

## What is new

### Version 1.7.1c

**Other builds of the game**

- **The head frustum now works on the 1.0 build of the game with rewritten code.** 1.7.1b found the game's `SetCamera` on that build through the calls to `SetCameraData`, but then refused it: the code had moved the load of the size of the camera description out of line (a jump to a block that loads it and jumps back), and the check did not follow the jump. It does now. Checked on that build's `Blur.exe` with `tools/camera_sig_check.py`; not yet seen in the game.

### Version 1.7.1b

**Other builds of the game**

- **A build whose code was rewritten gets the head frustum too.** Some non-Steam builds of the game have a few percent of their functions moved and rewritten (the function's address holds a jump to a copy with extra instructions in it). On one such build, from a user's log, BlurVR found the game's `SetCameraData` by its bytes but not `SetCamera`, so the game kept culling with its own frustum (`the camera code was not found` in `BlurVR.log`). BlurVR now also looks for `SetCamera` through the calls to `SetCameraData` and, when it finds it behind a jump, hooks the jump instead of the function; the function is checked against its instructions before anything is changed. This was tried on test builds made for it and has not been seen yet on that user's build. Builds that worked before are found as before.
- **When the camera code is still not found, the `game camera:` line says what was seen:** which calls were looked at and the first bytes of the function there. That line and the whole `BlurVR.log` are what to send.

### Version 1.7.1

**Rear-view mirror**

- **The 3D rear-view mirror is in stereo at every quality level of the game.** At the lowest quality the game draws its UI, the mirror's picture included, before the scene of the frame, and BlurVR did not recognise the mirror's picture then: the mirror scene was still drawn once for each eye, but each eye's mirror showed both halves side by side. It is now picked up whichever order the game draws in.

**Graphics quality**

- **The game's own quality setting is the preferred way to trade picture quality for frame rate** (see [Graphics quality](#graphics-quality)): it adjusts the reflections and the shadows, among other things, and a Valve Index held 90 frames a second at 3584x2016 on its lowest level. `Reflections` and `NoLensFlare` under `[VR]` are kept for now but may be removed in a later version.

### Version 1.7

**Motion blur**

- **The blur follows the direction of travel, wherever you look.** The game works its blur out for one flat picture and it was put into each eye as if you always looked straight ahead along the car, so the blur radiated from the middle of each eye's view. It is now worked out for what each eye sees: it runs along the car's direction of travel whichever way your head is turned, and a turn of your head is not blurred. (The game's own blur shaders come in two kinds, which take the depth differently; the one the game uses at its default settings has been seen working in the headset, the other kind has only been checked against a calculation.)
- **New: `MotionBlur=0` (under `[VR]`) turns the motion blur off.** The game's blur pass then writes the picture without blur, so the picture is sharper, but that gives no real gain in frame rate: the blur was about 0.1 ms of a 13 ms frame in a test, so turn it off for the look, not for speed (the game's depth of field, if a scene uses it, is part of the same pass and goes too). `1` (the default) is the game's blur as before. With `Log=1`, `BlurVR.log` has a `motion blur:` line saying which of the game's blur shaders a run uses and whether it was drawn as a copy.

**Showroom**

- **The floor's reflection in the menu showroom follows your head, and the car is whole in it.** The showroom draws the reflection into a picture as big as the main one, so it was treated as the main picture and followed your head up and down: looking up lost the car's reflection and turned the shadow black, looking down pinched the bottom of the floor. It is now recognised by its camera (the game's mirror camera, under the main one) and drawn like the water of a race: once for each eye, from that eye's own mirror image, held level as the flat game holds it, so it does not change as you nod and it shifts as you move your head. The car's lower half was missing from that picture for a different reason: the game cuts its reflection off at the floor with a clip plane that is fixed to its own camera, and with the picture drawn from your head that plane stayed where the game's camera had it, cutting the car away higher or lower as your head was. The plane is now carried to each eye's view. This applies to the water of a race too, where the same plane belongs to the reflection.

**Frame rate**

- **New: `Reflections=0` (under `[VR]`) leaves the water's reflection out of races** (the game's own quality setting does this and more, and is the preferred way; this switch may be removed later). The game has a switch of its own for its reflection pass; BlurVR turns it off in races, so the game neither culls nor draws the pass, and the mod does not have to draw it twice. In a test on the LA River at 3584x2016 that saved 0.7 ms of a frame of 13 ms (about 5 frames a second). The menu showroom keeps its floor's reflection. `1` (the default) is the game's reflections as before. With `Log=1`, `BlurVR.log` has a `reflections:` line saying whether the game's switch was found.

**FPS counter and the window on the desktop**

- **The FPS counter's colours are right under OpenXR, and follow a change of the headset's refresh rate.** They were measured against a rate that jumped about: SteamVR's predicted display period follows the pace of the frames just submitted, it is not the refresh period. The counter now asks the runtime for the headset's refresh rate once a second (`XR_FB_display_refresh_rate`, which SteamVR offers; with a runtime that does not, the first display period it reports is used), so a change of the rate in SteamVR's settings shows within a second. With `Log=1`, `BlurVR.log` lists the extensions the runtime offers and, every five seconds, the display periods it reported.
- **The window on the desktop says "Click this window to use the keyboard and gamepad"** (with `DesktopView=off`): the game only reads them while its window has the focus.

**Other**

- New host check in `source\tools`: `motion_blur_check.py` checks the motion blur (the matrix of each eye against a calculation, and the switch on a real Direct3D 9 device).
- `hotpath_check.py` and `reflection_cull_check.py` in `source\tools` now also check the showroom's reflection: that its picture is found from its camera and drawn once for each eye, and that the game's clip plane for it follows each eye.

### Version 1.6

**OpenXR (optional)**

- **`Api=openxr` runs the game on OpenXR instead of SteamVR's OpenVR.** There is a new `Api` key under `[VR]`: `openvr` (the default) or `openxr`. With OpenXR the game uses the system's active OpenXR runtime: the race picture goes to the headset as an OpenXR projection layer, the menus and loading screens on a floating OpenXR screen, and head tracking, the HUD, the mirror, the markers and every other setting work as before. It has been tried with SteamVR's OpenXR runtime and a Valve Index, where it looked the same as OpenVR; other runtimes and headsets have not been tried. Without the line, or with `Api=openvr`, nothing changes.
- **The runtime needs a 32-bit build,** because Blur is a 32-bit game. SteamVR registers one from version 2.17. [OpenXR](#openxr-optional) says how to check, and what to do for another runtime.
- **`OpenXRRuntime`** (under `[VR]`, empty by default) is the path of the `.json` of the OpenXR runtime to use instead of the system's active one, for this game only.
- **If OpenXR cannot start** (no 32-bit runtime, no headset, a failed session) the game stays on the monitor and `BlurVR.log` says why, in lines that start with `OpenXR:`. It does not switch to OpenVR by itself.
- **Recentring is BlurVR's own with OpenXR:** F9 and the recentre at the start of every race set a reference pose in BlurVR, because OpenXR cannot ask the runtime to recentre. A recentre done in the runtime is followed too.

**FPS counter**

- **New, off by default: an FPS counter in the headset.** `[VR] FpsCounter=1` (or **Num9** in game) puts a small panel, `62 FPS`, in front of your face in races and in the showroom: the number of frames the game presents a second, refreshed twice a second. It is green when the game keeps up with the headset's refresh rate, yellow below it and red far below. Under it is a graph of the last 18 seconds (a point every quarter of a second), scaled from the lowest to the highest rate in them, with the line in the same colours. It is part of the picture sent to the headset (also in the window on the desktop with `DesktopView` left, right or both) and is not shown on loading screens or in videos, nor with `Stereo=0`.
- **With `Api=openxr` the colours are measured against the refresh rate the OpenXR runtime reports** (the frame period it predicts for its display; with `Log=1` it is in `BlurVR.log` as `OpenXR: display period ...`). A runtime that reports none leaves the panel white. The figure and the graph are the same with both APIs.
- The figure has been seen working in the headset; the graph under it, and the panel with OpenXR, have so far only been checked without a headset.

**Package**

- The zip has two new files next to `openvr_api.dll`: `openxr_loader.dll` (the 32-bit OpenXR loader of the Khronos Group) and its license, `LICENSE_openxr.txt`. The loader is only used with `Api=openxr`; copy it too if you update by replacing single files.
- `install.bat` says that an OpenXR runtime can be started instead of SteamVR.

**Other**

- The OpenXR headers are in `source\third_party\openxr`, the code is in `source\src\openxr_runtime.cpp` (`source\NOTES.md`, section 27), and `build.bat` and `build.sh` need nothing new. New host checks in `source\tools`: `openxr_check.py` tests the loader and the runtime without the game (with `--system` it also starts the runtime and runs frames of a test picture in the headset), and `fps_counter_check.py` checks the FPS counter (counting, colours, the panel on a real Direct3D 9 device).

### Version 1.5

**Races**

- **The markers over the AI cars sit over their cars.** The game puts a marker (name, race position, weapon, health, the lock-on reticle) over every AI car, but BlurVR draws all of the game's 2D UI on one flat panel, so with your head turned the markers stayed on the panel instead of over the cars. Now each marker is cut out of the UI and drawn over its car in both eyes, at the car's distance (so it has the car's depth), whichever way you look. Close up it has its real size; far away it is never smaller than a few degrees, so it stays readable. Like in the flat game, a marker is never hidden behind anything.
- **The markers go to the cars nearest to where you look.** The game gives its five markers to the cars nearest the player's car, wherever you are looking. Now they go to the five nearest your head within 80 degrees of where you look, so looking out of the side window marks the cars beside you.
- **Water reflections follow your head.** The game draws the reflection in water (LA River and other levels) from a camera of its own with a narrow view cone that did not turn with your head, so with your head turned the reflection lost whole objects (the sky showed where a wall should be, with a straight edge between them) and seemed to slide as you looked around. It is now drawn for what both eyes see, and each eye's mirror image sits where it should (the two eyes were swapped in the reflection before).
- **The reflection is drawn level, like the game draws it.** The game lays a band of dark ground haze under the horizon of its own (level) camera over the reflection, so when you nodded down the far water looked up into that band and lost its reflection (the far bank and the signs on it). The reflection is now drawn as if you looked straight ahead, turned only to the way you face, so nodding or tilting your head no longer changes it.
- **The water at the docks follows your head too.** It reflects the scene picture on the screen instead, and the point it reflects was never moved to your eye. As in the flat game it shows the funnel of the ship and not its hull.
- A level with a lot of water costs a little more frame time, because the reflection is now drawn for everything both eyes see.
- **The HUD no longer sways with the car's acceleration** in the headset (the game moves, scales and turns the whole HUD with it, which would shift the markers). It stays at rest.
- The markers have no settings and run only in VR (`Stereo=1`). BlurVR finds the game's code for them by what it looks like, as for the camera in 1.3 (tried so far: the Steam build and a non-Steam build), and checks it before it changes anything; if something is not as expected the markers stay on the HUD panel as in 1.4, and `BlurVR.log` says why (`ai hud: ...` lines).

**Desktop window**

- **`DesktopView=off`, the new default.** The window on the desktop says "Playing in VR" with the game's logo (taken from `Blur.exe`) instead of showing a mirror of the headset, in the menus and loading screens too. Nothing of the game's picture is copied to it: the window is painted twice a second, which saves the graphics card the work of the mirror. It is still the game's window (the keyboard and the gamepad only work while it has the focus, which the window says, and closing it quits the game). Needs a windowed game without multisampling; otherwise the window shows the left eye's view and `BlurVR.log` says why. `left`, `right` and `both` work as before, and an older `BlurVR.ini` that has a `DesktopView` line keeps it; set `DesktopView=left` to get the mirror back.

**Settings**

- **`DesktopView` is `off` by default** and is in the `BlurVR.ini` that comes with the mod; `ForceNoVSync` and `KeyRepeatMs` are no longer there (their defaults are unchanged, add them under `[Display]` and `[Input]` to change them).
- **Removed:** `[Culling] HeadFrustum`. The view frustum around your head (1.2) is now always used in VR (it was already VR only); `MarginDeg` and `PullBack` stay. A `HeadFrustum=0` in an older `BlurVR.ini` is ignored.

**Other**

- **The flat display mode is put back when the game exits.** In 1.4 this only happened at the next start: Blur ends with `TerminateProcess`, which skipped BlurVR's exit code, so after every VR run `settings.xml` still held the VR size and `BlurVR.log` began with "the last VR run did not end properly". BlurVR now hooks that call and restores the flat size first (a game that is killed from outside is still repaired at the next start).
- Building from source needs `-lgdi32` (the message window is drawn with GDI); `build.bat` and `build.sh` have it. New host checks in `source\tools`: `ai_hud_check.py`, `desktop_message_check.py`, `exit_restore_check.py` and `reflection_cull_check.py`.

### Version 1.4

**Menus**

- **Menu key repeat limit.** Blur makes its own auto-repeat in the menus: it counts how long a key is held in frames, so at a high frame rate (with V-Sync off, say) a held arrow key scrolled a menu far too fast. New `[Input] KeyRepeatMs` (default 60): in the menus a held key or pad button now reaches the game as a tap, a short wait, then a tap every 60 ms, whatever the frame rate. It covers the keyboard and XInput pads (buttons, D-pad, left stick), in VR and flat, in the front-end menus and in the pause and results menus of a race (BlurVR recognises those by the full-screen dim the game draws behind them). **F8** forces it on or off if a menu is not recognised; `KeyRepeatMs=0` turns it off.

**Flat mode**

- **`Enabled` is the master switch, and `Stereo=0` runs the game flat:** SteamVR is never started and only the `[Display]` and `[Input]` settings apply, so the key repeat limit and the V-Sync setting work without a headset. `Enabled=0` still does nothing at all.

**Resolution**

- **One setting for the VR size:** `[VR] Resolution=WxH` replaces `CustomWidth`, `CustomHeight`, `StartWidth`, `StartHeight` and `StartAtSavedSize`.
- **Headset sizes in the game's list.** In VR the game's resolution list shows sizes for headsets (HTC Vive, Rift, Valve Index, Quest, Steam Frame, Vive Pro 2, Bigscreen Beyond and others, see [Resolution](#resolution)) instead of the monitor's modes.
- **The VR size no longer overwrites your normal one.** A size picked in the game's video options is saved in `BlurVR.ini`, and the game's own `settings.xml` keeps the flat game's size (put back when the game exits, with a backup beside it), so `Stereo=0` or `Enabled=0` starts at your normal resolution again.

**Settings**

- **Tidied into sections:** new `[Display]`, `[Input]` and `[VR]` sections. `ForceNoVSync` is now `0` by default (the game's own V-Sync setting; in VR V-Sync is always off and the headset sets the frame rate). `SharedTransport` is on by default. `Bloom` is one setting for the glow around menus, UI and loading screens. `NoLensFlare`, `DesktopView` (now `left`, `right` or `both`), `DesktopPresentEvery` and `FitDesktopWindow` moved under `[VR]`.
- **Removed:** `MaxFps` (use V-Sync), `NoBloom`, `UiGlow` and `LoadingGlow` (replaced by `Bloom`), and `OldVerticalTangents` with its Num9 key (the corrected vertical field of view is confirmed on the Quest and with SteamVR).
- An older `BlurVR.ini` keeps working where [Settings](#settings) says so, and the `BlurVR.ini` that comes with the mod is now a short file with the six settings most people need.

**Other**

- The BSD 3-Clause licence is included ([License](#license)), and this readme is now `README.md`.

<details>
<summary><b>Version 1.3</b></summary>

- BlurVR should now work with any version of Blur, not only the Steam one. The head-following view frustum of 1.2 was tied to the addresses of the Steam build and did nothing on any other copy of the game (a non-Steam build of the same game, for example, is the same code and was left alone because its file is a different size). Now BlurVR finds the game's camera code in the running `Blur.exe` by what the code looks like, not by where it is, recognises the camera you are looking through from its own data, and checks that the game's data is laid out the way it expects before it changes anything. If some version of the game differs too much for that, the game is left alone (it draws what it always drew) and `BlurVR.log` says why. Tried so far: the Steam build and a non-Steam build.

</details>

<details>
<summary><b>Version 1.2</b></summary>

- Objects no longer pop in and out at the edges of your view or when you look away from the direction of travel. The game decides what to draw from its own camera, which is narrower than the headset's view and does not turn with your head (it is 60 degrees tall; the Index shows 109). BlurVR now gives it a view frustum around your head, wide enough for both eyes (`MarginDeg` and `PullBack` under `[Culling]`). Drawing more of the world can cost frame time.
- Quest 2 and other headsets that see further down than up: the upward and downward field of view were read swapped, which squashed the periphery and bent lines when the head turned. Fixed.
- Each eye's position and rotation come from SteamVR (`EyeTransform`), which covers canted displays and IPD changes while playing.
- The camera position and the pixel shaders' camera now move with each eye, so highlights, reflections and the sky no longer slide over surfaces when the head moves.

(The last three: [pull request #2](https://github.com/TwoAyEm/BlurVR/pull/2) by uKER.)

</details>

<details>
<summary><b>Version 1.1</b></summary>

- Much higher resolutions: the game runs on a Direct3D 9Ex device and gives SteamVR each frame as a shared texture, so the picture no longer goes through the CPU (`SharedTransport`). 4936x2740, the Index's own size, is practical on a strong card.
- The game starts at the size you chose even when a window that big does not fit on your screens (and the start-up videos and menus keep their normal shape); the extra resolution can be picked from the options as before.
- Lower CPU cost per frame: fewer Direct3D calls around every duplicated draw, and the HUD no longer waits for the graphics card.
- The window on the desktop shows one eye's view instead of the squashed side-by-side picture (`DesktopView`), and is kept within the monitor's work area however high the resolution is (`FitDesktopWindow`).
- Photo viewer in the menus shows the photos in 3D; V-Sync is switched off by the mod in VR.
- Diagnostics are opt-in: logging is off unless `Log=1`; **Num7** (timing) and **Num8** (picture files) are there for bug reports.

</details>

## License

BlurVR is released under the [BSD 3-Clause License](https://github.com/TwoAyEm/BlurVR/blob/main/LICENSE.txt). `openvr_api.dll` is Valve's and comes with its own license (`LICENSE_openvr.txt`). `openxr_loader.dll` is the Khronos Group's OpenXR loader (Apache-2.0 or MIT, `LICENSE_openxr.txt`).
