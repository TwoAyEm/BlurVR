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
  4. In the game's video options (Options > Graphics > Resolution) choose the size for your headset (see Resolution below).
     It is 2160x1200 until you change Resolution under [VR] in BlurVR.ini, or pick another size in the options.
     The game starts at that size from then on. V-Sync is turned off by BlurVR itself, the headset sets the frame rate.

Resolution
  In races the picture is rendered side by side (left eye | right eye), so each eye gets HALF the width.
    1920x1080 = 960x1080 per eye       (light, soft; not in the list, but you can write it as Resolution=1920x1080)
    2160x1200 = 1080x1200 per eye      (BlurVR's default; the Vive's panel size)
    3840x2160 = 1920x2160 per eye      (sharp, heavy; not in the list either, Resolution=3840x2160 works)
    4936x2740 = 2468x2740 per eye      (what SteamVR itself renders for a Valve Index; needs a strong graphics card)
  In VR BlurVR replaces the game's resolution list (the monitor's 16:9 and 4:3 modes mean nothing in a headset) with these sizes: the
  headset's panel size per eye x 1.4 on each side (SteamVR's default supersampling), both eyes side by side. The Resolution in BlurVR.ini
  ([VR] section) is always in the list too, whatever it is. Flat (Stereo=0) the game's own list is untouched.
    HTC Vive, Oculus Rift CV1 (1080x1200 per eye) ........ 3024x1680   (2160x1200 = the panel's own size: the default, light)
    Oculus Rift S (1280x1440) ............................ 3584x2016
    Valve Index, Vive Pro, Meta Quest 1 (1440x1600) ...... 4032x2240   (4936x2740 = SteamVR's own size for the Index)
    Meta Quest Pro (1800x1920) ........................... 5040x2688
    Meta Quest 2, Quest 3S (1832x1920) ................... 5128x2688
    Meta Quest 3 (2064x2208) ............................. 5780x3092
    Valve Steam Frame, HP Reverb G2, Pico 4 (2160x2160) .. 6048x3024
    HTC Vive Pro 2, Vive Focus 3 (2448x2448) ............. 6856x3428
    Bigscreen Beyond (2560x2560) ......................... 7168x3584
  Your monitor has no such mode, so the game runs in a window (a window larger than your screens is normal);
  only the headset sees the picture. Bigger is sharper but heavier: start with the Vive-sized entry and go up while it stays smooth.
  In VR the size is kept in BlurVR.ini ([VR] Resolution=WxH): the game starts at it, even when Windows cannot make a window that big, and a
  size you pick in the game's video options is written there. The game's own settings (settings.xml) keep the size of the flat game, so with
  Stereo=0 or Enabled=0 it starts at your normal resolution again. BlurVR puts the flat size back when the game exits (and at the next start
  if the game was killed); the untouched file is copied to settings.xml.blurvr-backup beside it, and BlurVR\display_backup.ini holds the flat size.

Keys
  F9  recenter (sit comfortably, look straight ahead, press F9). This is SteamVR's own recentre, so recentering in SteamVR does the same.
      BlurVR also recenters at the start of every race (set AutoRecenter=0 in BlurVR.ini to turn that off).
  F10 head tracking on/off (stereo image stays, camera stops following your head)
  F8  mode override: AUTO -> FORCE MENU -> FORCE RACE. Use it if the mod picks the wrong mode.
  Num6 3D rear-view mirror on/off (same as Mirror3D in the ini)
  Num7 timing probe on/off (see Troubleshooting)
  Num8 save what the headset sees as picture files (see Troubleshooting)
  Num9 vertical field of view: fixed / as in 1.1 (for comparing; see OldVerticalTangents)

Settings: BlurVR.ini holds only the options most people need (the mode, resolution, start size).
  Every option is listed below; add a line under its [section] in BlurVR.ini to use it (keys left out use the default shown).
  Edit with the game closed. Delete BlurVR.ini to go back to all defaults.
  The mod has three states: Enabled=0 does nothing at all; Enabled=1 with Stereo=1 is VR; Enabled=1 with Stereo=0 is "flat": the
  game stays on the monitor, SteamVR is never started and only the [Display] settings apply. The [VR] and [Render] settings are
  ignored when flat (the extra resolution is not offered either), so the game starts at the size it saved.
  Settings that moved are read from their new place only: an older BlurVR.ini keeps working for Stereo, ForceNoVSync, SharedTransport,
  DesktopView, DesktopPresentEvery, FitDesktopWindow (still read from [Render]) and for CustomWidth / CustomHeight (read as Resolution),
  but UiGlow, NoBloom, LoadingGlow, MaxFps, StartWidth, StartHeight and StartAtSavedSize are gone, and NoLensFlare is only read from [VR].
  Bloom replaces UiGlow, LoadingGlow and NoBloom (Bloom=0 is "no glow anywhere"). ForceNoVSync=0 follows the display's refresh rate.

[General]
  Enabled=1          master switch. 0 = the mod does nothing at all (the game runs as if it were not installed)
  Stereo=1           1 = VR. 0 = flat: no SteamVR and no VR changes to the game; only the [Display] settings apply
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

[Display]       (VR and flat)
  ForceNoVSync=0        1 = V-Sync off: the game runs as fast as it can. 0 = the game's own V-Sync setting (with V-Sync on the frame
                        rate follows the monitor, e.g. 60). In VR this key is ignored: V-Sync is always off there and the headset's
                        own refresh rate (90, 120, 144 ...) sets the frame rate

[VR]            (ignored when Stereo=0)
  Resolution=2160x1200  the size of the VR picture, both eyes side by side (see Resolution above). It is the extra entry in the
                        game's resolution list and the size the game starts at. Pick another size in the game's video options and
                        BlurVR writes it here for you. The game's own settings keep the flat game's size.
  Bloom=100             strength of the game's glow around its menus, UI and loading screens, in percent (0 = none, 200 = double)
  NoLensFlare=0         1 = drop the flare sprites and the glow around lights
  SharedTransport=1     run the game on a Direct3D 9Ex device and hand each finished race frame to SteamVR on the GPU
                        instead of copying it through the CPU (saves several ms a frame, most at large sizes). On by default; it
                        falls back to the CPU copy by itself when it cannot work. Set 0 if textures look wrong or the picture is black
  DesktopView=left      what the window on the desktop shows in a race: left = the left eye's view (cropped to the window's shape and
                        drawn at the window's own size, which is cheap), right = the right eye's, both = the whole side-by-side picture.
                        The headset's picture is not affected.
  DesktopPresentEvery=1 in a race, the window on the desktop (a mirror of the headset) really updates only every n-th frame
                        (2-4 saves a little GPU time; 1 = every frame)
  FitDesktopWindow=1    1 = the game's window is never made bigger than the work area of its monitor (at a 3840x2160 setting it
                        would cover the whole desktop, or two monitors). The game still renders at the size it chose; the window
                        shows one eye's view of it scaled to fit (DesktopView). Windowed mode only. 0 = as the game wants.

[Render]        (VR only)
  FovScale=1.0          <1 zooms in, >1 zooms out
  SeamGutter=0          black pixels at the inner edge of each eye (stops glow bleeding across the seam)
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
  Slow: lower Resolution under [VR]. Num7 (or Enabled=1 under [Perf], with Log=1) writes timing lines to the log.
  If SharedTransport (on by default in VR) gives odd textures or a black picture on your graphics card, add SharedTransport=0 under [VR].
  To run the game flat, set Stereo=0 (SteamVR is not used; the [Display] settings still apply), or Enabled=0 (the mod does nothing),
  or run uninstall.bat.

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
  * Photo viewer in the menus shows the photos in 3D; V-Sync is switched off by the mod in VR.
  * Diagnostics are opt-in: logging is off unless Log=1; Num7 (timing) and Num8 (picture files) are there for bug reports.
