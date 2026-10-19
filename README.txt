BlurVR 1.3 - seated VR for Blur (SteamVR / OpenVR)
==================================================
Play Blur in a headset, seated. Built and tested with a Valve Index; any SteamVR headset should work.
Unofficial fan mod; not affiliated with the game's publisher or developer.

https://www.youtube.com/watch?v=-Gwqe6Wcjd8

What it does
  MENUS : the game is shown on a floating screen in front of you (SteamVR overlay).
  RACES : true stereo 3D, seated. Your head moves the in-game camera (chase view and in-car view).
          The camera position is fixed to the car; you look around from your seat.
          Shadows, lights, motion blur and the HUD are all handled per eye.
          The rear-view mirror is a real 3D panel: the road behind you is rendered once per eye, with depth.
  Game controls are unchanged.

Requirements
  Windows, SteamVR, Blur (32-bit; any version of the game should do, Steam or not, see "What is new in 1.3"), a graphics card that
  runs Direct3D 11 (any recent one).

Install
  1. Unzip so that the BlurVR folder sits inside your Blur folder (next to Blur.exe),
     e.g.  ...\steamapps\common\Blur\BlurVR\
  2. Run BlurVR\install.bat (copies d3d9.dll next to Blur.exe). uninstall.bat removes it again.
  3. Start SteamVR, then launch Blur from Steam.
  4. In the game's video options (Options > Graphics > Resolution) choose the extra resolution BlurVR adds.
     It is 2160x1200 until you change CustomWidth / CustomHeight in BlurVR.ini (see Resolution below).
     The game starts at the size you chose from then on. V-Sync is turned off by BlurVR itself, the headset sets the frame rate.

Resolution
  In races the picture is rendered side by side (left eye | right eye), so each eye gets HALF the width.
    1920x1080 = 960x1080 per eye       (light, soft)
    2160x1200 = 1080x1200 per eye      (BlurVR's default; the Vive's panel size)
    3840x2160 = 1920x2160 per eye      (sharp, heavy)
    4936x2740 = 2468x2740 per eye      (what SteamVR itself renders for a Valve Index; needs a strong graphics card)
  BlurVR adds one entry to the game's resolution list: CustomWidth x CustomHeight in BlurVR.ini.
  Recommended sizes (the headset's panel size per eye x 1.4 on each side, SteamVR's default supersampling), both eyes side by side:
    HTC Vive, Oculus Rift CV1 (1080x1200 per eye) ........ 3024x1680
    Oculus Rift S (1280x1440) ............................ 3584x2016
    Valve Index, Vive Pro, Meta Quest 1 (1440x1600) ...... 4032x2240   (4936x2740 = SteamVR's own size for the Index)
    Meta Quest Pro (1800x1920) ........................... 5040x2688
    Meta Quest 2, Quest 3S (1832x1920) ................... 5128x2688
    Meta Quest 3 (2064x2208) ............................. 5780x3092
    Valve Steam Frame, HP Reverb G2, Pico 4 (2160x2160) .. 6048x3024
    HTC Vive Pro 2, Vive Focus 3 (2448x2448) ............. 6856x3428
  Your monitor has no such mode, so the game runs in a window (a window larger than your screens is normal);
  only the headset sees the picture. Bigger is sharper but heavier: start with the Vive-sized entry and go up while it stays smooth.
  BlurVR keeps the game's start-up size even when Windows cannot make a window that big (StartWidth / StartHeight in BlurVR.ini
  force a size if you ever need to; 0 = the size you chose in the game's options).

Keys
  F9  recenter (sit comfortably, look straight ahead, press F9). This is SteamVR's own recentre, so recentering in SteamVR does the same.
      BlurVR also recenters at the start of every race (set AutoRecenter=0 in BlurVR.ini to turn that off).
  F10 head tracking on/off (stereo image stays, camera stops following your head)
  F8  mode override: AUTO -> FORCE MENU -> FORCE RACE. Use it if the mod picks the wrong mode.
  Num6 3D rear-view mirror on/off (same as Mirror3D in the ini)
  Num7 timing probe on/off (see Troubleshooting)
  Num8 save what the headset sees as picture files (see Troubleshooting)
  Num9 vertical field of view: fixed / as in 1.1 (for comparing; see OldVerticalTangents)

Settings: BlurVR.ini holds only the options most people need (resolution, start size, shared-texture transport).
  Every option is listed below; add a line under its [section] in BlurVR.ini to use it (keys left out use the default shown).
  Edit with the game closed. Delete BlurVR.ini to go back to all defaults.

[General]
  Enabled=1          0 = the mod does nothing (the game runs flat)
  Log=0              1 = write BlurVR\BlurVR.log (for bug reports; the log of the previous run is kept as BlurVR.previous.log)
  RecenterKey=0x78   virtual-key codes (hex): F9 recenter
  ToggleKey=0x79     F10 head tracking on/off
  ModeKey=0x77       F8 cycle mode (auto / force menu / force race)
  StartMode=0        0 = auto (floating screen in menus, seated 3D in races), 1 = always floating screen, 2 = always race/3D
  AutoRecenter=1     1 = also recentre every time a race starts
  ThreadedInit=1     start OpenVR on a worker thread (keeps SteamVR from flagging the game as not responding)
  DeferVR=1          keep the game flat (SteamVR theatre) until the first level starts loading, then start VR

[Tracking]
  Rotation=1  Position=1   head rotation / position (lean in your seat) move the camera
  PositionScale=1.0  game units per metre
  IpdScale=1.0       scale of the eye separation (1.0 = your real IPD)
  EyeTransform=1     1 = each eye's position (and rotation, on headsets with canted displays) from SteamVR; 0 = half the IPD to each side
  OldVerticalTangents=0  1 = read the headset's upward and downward field of view swapped, as 1.1 did (squashed periphery on headsets
                     that see further down than up, such as the Quest 2)

[Render]
  CustomWidth=2160  CustomHeight=1200   the extra resolution offered in the game's options (0 = off)
  StartWidth=0  StartHeight=0           the size the game starts at (0 = the size saved by the game)
  StartAtSavedSize=1    1 = a window that Windows made smaller than the chosen size does not shrink the game's picture
  SharedTransport=0     1 = run the game on a Direct3D 9Ex device and hand each finished race frame to SteamVR on the GPU
                        instead of copying it through the CPU (saves several ms a frame, most at large sizes). Set 0 if anything
                        looks wrong. The ini shipped with the mod sets it to 1.
  DesktopView=1         what the window on the desktop shows in a race: 1 = the left eye's view (cropped to the window's shape and
                        drawn at the window's own size, which is cheap), 2 = the right eye's, 0 = the whole side-by-side picture.
                        The headset's picture is not affected.
  DesktopPresentEvery=1 in a race, the window on the desktop (a mirror of the headset) really updates only every n-th frame
                        (2-4 saves a little GPU time; 1 = every frame)
  FitDesktopWindow=1    1 = the game's window is never made bigger than the work area of its monitor (at a 3840x2160 setting it
                        would cover the whole desktop, or two monitors). The game still renders at the size it chose; the window
                        shows one eye's view of it scaled to fit (DesktopView). Windowed mode only. 0 = as the game wants.
  ForceNoVSync=1        1 = turn the game's V-Sync off, the headset sets the frame rate (if the game runs at the wrong speed,
                        set 0 and turn V-Sync off in the game's video options)
  MaxFps=0              frame rate cap (0 = none). 60 avoids audio stutter some get at high frame rates
  Stereo=1              0 = one centre picture for both eyes
  FovScale=1.0          <1 zooms in, >1 zooms out
  SeamGutter=0          black pixels at the inner edge of each eye (stops glow bleeding across the seam)
  NoLensFlare=0         1 = drop the flare sprites and the glow around lights
  NoBloom=0             1 = no glow around menus, loading screens and the HUD
  Advanced (leave alone unless told to): MainPassMinWidthFraction=0.9, RequireDepthBuffer=1, HalfResPasses=1,
  ReflectionLookupFix=1, ReflectionFlip=-1

[Hud]
  WorldLocked=1     1 = the HUD hangs in front of the car, 0 = glued to your view
  Scale=0.6         panel width as a fraction of one eye's width
  Distance=2.0      apparent distance in metres
  OffsetY=0.0       vertical offset (fraction of height, + = down)
  XOffset=0.0       extra convergence per eye, as a fraction of one eye's width
  VeilRace=120 VeilResults=36 VeilMenu=100   how dark the veil behind popups is (0-255)
  DimStrongPct=60   how dark the scene gets behind pause/results menus (0-100)
  UiGlow=100        strength of the glow around UI elements (%)
  LoadingGlow=1     keep the game's glow on the loading screen
  DropBackdrop=1    drop the game's full-screen dim/blur copies (they would blank the HUD layer)
  HideFullscreenOverlay=1  hide the HUD layer while a full-screen video plays
  CountInDrop=1     hide the race-start letterbox pieces (0 = draw them into the HUD)
  CoverageEvery=8   how often (in frames) each UI draw is checked for covering the screen (1 = every frame; slower)
  Mirror3D=1        rear-view mirror as its own 3D panel (real depth)
  MirrorConvergence=1.5  MirrorEyeScale=1  MirrorHeadParallax=1   depth that sits at the panel (m), eye separation scale, head parallax

[Menu]
  Distance=2.2  Width=2.6  HeightOffset=0.0    the floating screen in menus: distance (m), width (m), height relative to your eyes (m)

[Debug]
  KeyMirror3D=0x66 (Num6)   KeyPerf=0x67 (Num7)   KeyDump=0x68 (Num8)   KeyVerticalTangents=0x69 (Num9)    keys for the switches above
  AdjustViewRotation=1

[Culling]
  HeadFrustum=1     1 = the game decides what to draw from a view frustum around your head (both eyes' field of view, the way you look)
                    instead of from its own, narrower camera.  0 = as in 1.1.  BlurVR finds the game's camera code in the running Blur.exe
                    and checks that its data is laid out as expected; if not, the game is left alone and BlurVR.log says why
                    ("game camera: ..." and "head frustum: ..." lines).  Tried so far: the Steam build and the same game without the Steam
                    wrapper (Blur.exe of 13,738,496 bytes).
  MarginDeg=6.0     degrees added to every side of that frustum (covers canted displays and quick head turns)
  PullBack=0.3      metres the frustum's apex is moved back (covers the eyes' offset and leaning)

[Perf]
  Enabled=0         1 = write timing lines to the log once a second while racing (needs Log=1)

[Foveation]
  Report=1          1 = write the headset model, refresh rate, render size and lens hidden area to the log at VR start (needs Log=1)

How menu/race is detected
  When the game opens files under levels\showroom it is a menu; any other levels\<track> is a race.

How it works (short)
  Blur is 32-bit Direct3D 9 under Steam DRM, so it is patched at runtime. Its vertex shaders get g_mViewProj;
  BlurVR decomposes that matrix to recover the camera and projection, then in the main 3D pass draws every
  object twice (left half / right half of the render target) with a per-eye offset, the headset's per-eye
  projection and your head pose. Screen-space passes (motion blur, shadow decals, deferred lights) are
  re-expressed in each eye's own camera. The HUD is drawn once per eye. The finished image is split and sent to SteamVR.
  Source code: https://github.com/TwoAyEm/BlurVR (start with source/NOTES.md; build with source/build.bat on Windows or build.sh with Zig).

Known limitations
  * The rear-view mirror and the planar reflections still decide what to draw with the game's own cameras.
  * HUD detection is heuristic (worked out from how the game draws), so an unusual screen can show UI pieces in the wrong place.
  * Menus are the game image on a flat screen; use mouse/keyboard/controller as normal.
  * The window on the desktop is a mirror of one eye (DesktopView), cropped to the window's shape; it is not the headset's full view.

Troubleshooting
  The view is still culled at the edges / looking away from the direction of travel: set Log=1 under [General], start a race for a minute,
  and look in BlurVR.log for the lines that start with "game camera:" and "head frustum:". They say whether the game's camera code was
  found in your Blur.exe, whether its data is laid out as expected, and how many frustums were replaced. If they say it was not found,
  send BlurVR.log and the size and date of your Blur.exe.
  Nothing happens / the game is flat: start SteamVR first; run uninstall.bat to go back to the plain game.
  Something looks wrong in the headset: press Num8 while it is on screen. BlurVR saves BlurVR_dump_<time>_picture_*.bmp
  (the whole side-by-side picture, large files) and one file per HUD layer in the BlurVR folder: attach them to the report.
  A bug report should also include BlurVR.log: set Log=1 under [General], reproduce the problem, close the game, and send BlurVR\BlurVR.log.
  Slow: lower CustomWidth/CustomHeight, or make sure SharedTransport=1. Num7 (or Enabled=1 under [Perf], with Log=1) writes timing lines to the log.
  If SharedTransport=1 gives odd textures or a black picture on your graphics card, set SharedTransport=0.
  To run the game flat, run uninstall.bat (or set Enabled=0 in BlurVR.ini).

What is new in 1.3
  * BlurVR should now work with any version of Blur, not only the Steam one. The head-following view frustum of 1.2 was tied to the
    addresses of the Steam build and did nothing on any other copy of the game (the Steam build with its wrapper removed, for example,
    is the same code and was left alone because its file is a different size). Now BlurVR finds the game's camera code in the running
    Blur.exe by what the code looks like, not by where it is, recognises the camera you are looking through from its own data, and checks
    that the game's data is laid out the way it expects before it changes anything. If some version of the game differs too much for
    that, the game is left alone (it draws what it always drew) and BlurVR.log says why. Tried so far: the Steam build and the same game
    without the Steam wrapper (Blur.exe of 13,738,496 bytes).

What is new in 1.2
  * Objects no longer pop in and out at the edges of your view or when you look away from the direction of travel. The game decides
    what to draw from its own camera, which is narrower than the headset's view and does not turn with your head (it is 60 degrees
    tall; the Index shows 109). BlurVR now gives it a view frustum around your head, wide enough for both eyes (HeadFrustum, MarginDeg
    and PullBack under [Culling]). Drawing more of the world can cost frame time: HeadFrustum=0 gives back the old behaviour.
  * Quest 2 and other headsets that see further down than up: the upward and downward field of view were read swapped, which
    squashed the periphery and bent lines when the head turned. Fixed (OldVerticalTangents=1 restores the old reading, Num9 switches
    between the two for comparing).
  * Each eye's position and rotation come from SteamVR (EyeTransform), which covers canted displays and IPD changes while playing.
  * The camera position and the pixel shaders' camera now move with each eye, so highlights, reflections and the sky no longer slide
    over surfaces when the head moves.
    (The last three: pull request #2 by uKER.)

What is new in 1.1
  * Much higher resolutions: the game runs on a Direct3D 9Ex device and gives SteamVR each frame as a shared texture, so the picture
    no longer goes through the CPU (SharedTransport). 4936x2740, the Index's own size, is practical on a strong card.
  * The game starts at the size you chose even when a window that big does not fit on your screens (and the start-up videos and
    menus keep their normal shape); the extra resolution can be picked from the options as before.
  * Lower CPU cost per frame: fewer Direct3D calls around every duplicated draw, and the HUD no longer waits for the graphics card.
  * The window on the desktop shows one eye's view instead of the squashed side-by-side picture (DesktopView), and is kept within
    the monitor's work area however high the resolution is (FitDesktopWindow).
  * Photo viewer in the menus shows the photos in 3D; V-Sync is switched off by the mod (ForceNoVSync, MaxFps).
  * Diagnostics are opt-in: logging is off unless Log=1; Num7 (timing) and Num8 (picture files) are there for bug reports.
