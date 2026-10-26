# BlurVR – developer notes

How BlurVR 1.0 works, what the game does, and why each non-obvious decision was made. Read this before changing the renderer.

Contents

1. What it is, build, deploy
2. Frame flow in one page
3. Conventions and the core maths
4. What we know about Blur's renderer
5. Per-eye fixes for screen-space passes
6. The HUD / UI layers
7. VR session, menus, loading
8. Resolution and display modes
9. Settings reference
10. Known limits, features, ideas
11. How to investigate a new rendering problem
12. Code map

---------------------------------------------------------------------------------------------------------

## 1. What it is, build, deploy

BlurVR is a **32-bit Direct3D 9 proxy `d3d9.dll`** that sits next to `Blur.exe` (no
file patching and no injection of game code; everything happens by hooking at runtime). It talks to SteamVR
through OpenVR's C API (`openvr_api.dll` in the `BlurVR\` subfolder; `third_party/openvr_capi.h`). With `[VR] Api=openxr` it talks to the system's OpenXR runtime instead
(`openxr_loader.dll`, 32-bit, in the same folder; section 27).

* Menus and loading: the game image is shown on a floating SteamVR overlay in front of the player.
* Races: true stereo, seated. Every draw of the main 3D pass is issued twice (left half / right half of the game's
  render target), the finished side-by-side (SBS) back buffer is split and submitted to the compositor.
* Game controls are unchanged. Head tracking drives only the in-game camera (position is fixed to the car).

Build (Linux, cross-compile with Zig): `./build.sh` in the repository root. It needs `pip install ziglang` and
produces `d3d9.dll` (x86). The `.def` file maps the stdcall-decorated exports to the plain names the game links
to. Zig/libunwind leaks some extra exports into the DLL; they are harmless.

Deploy: copy `d3d9.dll` to the game folder (next to `Blur.exe`) and keep it, `BlurVR.ini`, `openvr_api.dll`
in `<game>\BlurVR\`. `install.bat` / `uninstall.bat` in the release package do the copy. The mod writes
`BlurVR\BlurVR.log` on every run – ask for it with any bug report.

There is no way to run the game from the build machine. **Every change that touches rendering has to be tested in
the headset.**

## 2. Frame flow in one page

```
Direct3DCreate9            -> load real d3d9, hook IDirect3D9 (CreateDevice, mode list), hook CreateFile* imports
CreateDevice               -> hook the IDirect3DDevice9 vtable (draws, state, shaders, textures, Present, Reset)
                              VR start is deferred until the first level begins loading (see 7)
every game draw call       -> RunDraw(): ClassifyDraw() then one of
                                0 normal   : untouched (plus the per-eye special cases below)
                                1 scene    : issued twice, once per eye (scissor + rewritten matrices)
                                2 HUD      : rendered into an off-screen HUD layer
Present                    -> ComposeHud (draw the HUD layer into each eye as a flat world-locked panel),
                              copy back buffer to the compositor texture, Submit(L), Submit(R) or show the
                              menu overlay, then WaitGetPoses for the next frame's head pose
```

Race versus menu is decided from the files the game opens (`levels\showroom` = menu, any other `levels\<track>` =
race) plus a "3D scene is really being drawn" probe (>= 20 tracked scene draws for 15 frames). `F8` overrides.

## 3. Conventions and the core maths

* **Row vectors, D3DX style**: `p' = p * M`. Matrices on the CPU are `Matrix4` (doubles). Constants arrive from the
  game as 4 float4 registers; the game's layout (row-major vs transposed) is auto-detected the first time a valid
  view-projection decomposes (`g_matrixLayout`, 1 = row, 2 = transposed; ini `[Matrix] Layout` can force it).
* Left-handed D3D space, +Z forward. OpenVR is right-handed, -Z forward: the pose is converted by flipping Z.
* **Camera recovery**: the game only uploads `g_mViewProj` (and sometimes `g_mView`) to vertex shaders. A matrix
  `X = V * P` with `V` rigid and `P = [[a,0,0,0],[0,b,0,0],[0,0,c,1],[0,0,d,0]]` can be decomposed
  (`DecomposeViewProjection`): `X[:,3] = V[:,2]`, `X[:,0] = a*V[:,0]`, `X[:,1] = b*V[:,1]`,
  `X[:,2] = c*V[:,2] + d*e3`. Blur uses a **reversed depth range** (`c ~ 0`, `d > 0`), so the sign tests are loose.
  This gives `a` (x scale), `b` (y scale), `c`, `d`; they are kept in `g_projXScale/YScale/DepthScale/DepthBias`.
* **Eye delta** `g_vr.eyeCameraDelta[e]` (`Ceye`): camera-space delta between the game's camera and the eye's
  head-tracked camera, row-vector form, `pos_eye = pos_old * C`. Built from the pose relative to the recentre pose
  plus half the IPD along X (`e` = 0 left, 1 right, 2 centre). The game keeps driving the car camera; only the
  head offset is added.
* **Per-eye projection** `BuildEyeProjection(box, eye)`: `box` is the headset's tangent box `{left,right,up,down}`
  from `GetProjectionRaw`. In the default "NDC eyes" layout the SBS image is one render target and each eye owns
  one half of clip space: left eye NDC x in [-1,0], right in [0,1]. Depth terms `c`, `d` are the game's own so the
  depth buffer stays consistent with the game's shaders.
* **Rewriting the matrices of a draw** (`ApplyEyeConstants`): for every tracked vertex-shader matrix `X`:
  * view-projection and world-view-projection: `X' = X * inv(P_old) * C * P_new`
  * view and world-view: `X' = X * C`;   projection: `X' = P_new`;   3x3 view (`K_V3`): rotated by `C`.
  * camera world position (`g_vViewPosition`, VS and PS) and the pixel shaders' own view / view-projection columns: see section 23.
  The game's original values stay in the shadow array `g_vsConstants`; the rewritten ones are uploaded through the
  **original** (unhooked) `SetVertexShaderConstantF` so the shadow copy is never polluted, and restored after.
* Each eye is drawn with scissor = its half of the image, minus the **seam gutter** (`SeamGutter` px of black at the
  inner edge of each eye so glow/blur cannot bleed across the seam; 0 by default, 24 was the old default).
* Planar reflections (half-size pass) get a mirrored-camera delta (conjugation of `C`; the axis is the one the game's reflection camera has turned round against the main camera, found by `game_camera.cpp` (section 25), else chosen by the sign of the
  view determinant) and their lookup matrix `g_ReflectionViewMatrix0/1/3` in the main pass is rewritten with the
  same transform (records kept in `g_reflectionRecords`, matched by the x/y/w columns of the reflection view-proj).

## 4. What we know about Blur's renderer

Blur (Bizarre Creations, 2010) is a Direct3D 9 deferred renderer using ps/vs 3.0 shaders compiled with D3DX, so
**every shader carries a constant table (CTAB) with the HLSL variable names**. That is how all identification is
done – by constant names, never by shader index (indices are the creation order in one run and not stable).

Pass sizes (back buffer SBS 1920x1080 for example):

| render target | what |
|---|---|
| back buffer size, with depth | main scene pass (G-buffer/forward) – the one that gets stereo-duplicated |
| back buffer size, no depth, `g_Ui...` shaders | menu/HUD UI |
| 0.45–0.55 of back buffer | half-size passes: planar reflection (when drawing 3D), glow/flare sprites, motion-blur (960x540) |
| 0.2–0.3 | quarter-size bloom chain |
| 256x256 | shadow-plane texture for car shadows, blurred by two small passes |

Constants and shaders we rely on (all identified from CTABs and disassembly, ps_3_0):

* Vertex matrices: `g_mWorldViewProj`, `g_mViewProj` / `g_ViewProjMatrix`, `g_mWorldView`, `g_mView` (4x4 or 3x3),
  `g_mProj`, and the split forms `g_vViewMatrix0..3`, `g_vViewProjMatrix0..3` (four consecutive float4s);
  `g_ReflectionViewMatrix0/1/3`.
* Motion blur PS: `g_mMatrix` (c0..c3) + `g_vStepScale`, `g_vSettings`, `g_vNoise`, `g_vDepthScale`.
* Shadow decal PS: `g_ShadowPlaneSpaceMatrix` (c0..c3), `g_ShadowMult`, `g_PixelOffset`, `g_vDepth`.
* Deferred lights: full-screen VS interpolates four far corners `g_vFar0..3` (c200..c203) with `g_vUVScale/Offset`
  (c204/205); PS uses `g_vPosition` (light position in mono view space, w = attenuation), `g_vColour`,
  `g_vDepth`, `g_vViewUpVector`. Light volumes (spot/projected): `g_vXY`, `g_vPosition`, `g_mLight`.
* Lens flares: `g_FlareSize` / `g_FlareColour`. UI: any shader with a `g_Ui...` constant.
* Final composite (to back buffer): `g_mColour`, `g_vFilmGrain`, `g_vSettings1/2`, `g_vSpecialFX`; it samples the
  quarter-size bloom texture (so `NoLensFlare=1` replaces the last quarter-size input with opaque black; that removes the glow around lights on the car).

The game **culls with its own narrow camera frustum**, so objects outside it never get submitted; with a wide head
turn away from the direction of travel they can pop out (see section 10). Since 1.2 the proxy replaces that frustum with one around the head (section 25).

## 5. Per-eye fixes for screen-space passes

All of these were wrong by default because they assume **one full-width camera image**; in the SBS image the
screen centre, UV range and frustum corners are those of the whole image, not of an eye. Each is handled by running
the pass once per eye (scissor = eye half) with constants rewritten for that eye. All use the original constant
setters and restore afterwards.

**Motion blur** (`MotionBlurPerEye`, `BlurMatrixForEyeRawDepth`, `BlurMatrixForEye`). The pass finds where each pixel was in the previous frame with the game's `g_mMatrix`
(c0..c3, one row per input, columns x, y, w used): `(x', y', w') = q * M`, the previous texture coordinate is `(x'/w', y'/w')`. It is made for one mono camera with the whole
picture, so its centre of expansion landed on the seam between the eyes (and with an eye's own picture it would radiate from the middle of that picture, wherever the head looks);
the pass is drawn once per eye, scissored to its half, with a matrix made for that eye. **The game has two kinds of blur shader, and what `q` is depends on the kind**
(`PixelShaderInfo::blurDepthRaw`, found by looking for an `add` with `c4` = `g_vDepthScale` in the bytecode; checked on all 26): 8 of the 26 (the ones that finish
with `mul_sat oC0, r0, c4.w`) hand the matrix `q = (u, v, depth, 1)` with the depth as the buffer holds it (`depth = c + d/Z`, `Z` the distance along the view axis, `c, d` = `g_vDepthScale.xy`
= the projection's depth terms, the same numbers the mod's own projection has); the other 18 (those with depth of field among them, and the two minimal ones) first turn the depth into a distance,
`z = c4.y / (|depth| - c4.x)`, and hand it `q = (z*u, z*v, z, 1)`. The shader the game uses at its default settings (pixel shader 656 in the log, ps_3_0, 8 constants, run on a target of half
the side-by-side picture) is of the first kind: a `Log=1` run showed `g_mMatrix` at rest as the 4x4 identity and, while driving, the car's motion in the depth row (c2 = (0.48, 0.39, ., 0.955)) with
the vanishing point at about (0.51, 0.41) of the picture: a perspective reprojection in (u, v, depth) space.
**`BlurMatrixForEyeRawDepth`** (the first kind). The game's matrix is a projective map of clip space: `(u, v, depth, 1)` is the clip position divided by `Z`, `(x', y', z', w')` is the previous
clip position over `Z` (`w' = Z'/Z`; the matrix has no usable z' column, the game leaves it as the identity's). So for an eye the whole thing is linear in homogeneous coordinates and
one matrix: `(u, v, depth, 1)` of the eye -> the eye camera's `(tan x, tan y, 1, 1/Z)` (`1/Z = (depth - c)/d` is linear in the depth) -> `inv(C)` (`C` = `eyeCameraDelta[eye]`: the head's pose
and the eye's place in it) -> the game camera's `(Z*u, Z*v, Z*depth, Z)` -> the game's matrix -> `(x', y', w')` -> the point in the previous game camera (`(2x' - w')/A, (w' - 2y')/B, w'`; `A, B` =
the game's projection scales) with the W of the point carried over as it is -> `C` (the head is held where it is between the frames, only the game camera's motion blurs) -> the eye's
`(x', y', w')`. At rest the game's matrix is the identity and so is the result. The blur runs along the car's direction of travel whichever way the head looks (the point straight ahead of the
car has no blur at any head turn: `motion_blur_check.py`). The head's offset from the game camera cancels out of a rigid camera motion, so it makes no difference to the vectors (a version that
left it out gave the same numbers to a tenth of a thousandth of the picture in the headset). **Seen in the headset** (2026-10-25, Valve Index): the blur runs along the direction of travel
with the head turned 70 degrees to the side (the road's streaks follow the wall beside it), a few thousandths of the picture long at its edge.
**`BlurMatrixForEye`** is the same chain for the second kind (`z`, `inv(C)` and `C` as above, the translation of `C` added to the constant row of the result). It has been checked
against a calculation only, no headset run has used a shader of that kind.
**What went wrong on the way.** The first version of the head-aware matrix was `BlurMatrixForEye` applied to the shader of the first kind (the second kind had been assumed from the
disassembly of one of the game's shaders): the whole picture smeared, with streaks that fanned out from the direction of travel but were far too long. It was found by a log of the game's matrix
(dumps next to the pictures with Num8 were no help without it). The matrix before that (`T * M * S`, the eye's texture coordinates mapped onto the game's picture as if the eye looked where the game
camera does: `T = [[a1,0,0,0],[0,a2,0,0],[0,0,1,0],[b1,b2,0,1]]`, `a1 = A*(r-l)`, `b1 = 0.5 + 0.5*A*(l - 2*off*(r-l))`, `a2 = 0.5*B*(up+dn)`, `b2 = 0.5 - 0.5*B*up`, `off` 0 / 0.5) is exact for the
first kind as long as the head looks where the game camera does, which is why it looked right, and it leaves the centre of expansion at the middle of each eye's picture, which was the complaint.
If a matrix cannot be made (degenerate tangents or delta, no depth terms) the eye is drawn with the game's own matrix and `Log=1` says so once (`motion blur: eye N: no matrix could be made`);
`Log=1` also writes one `motion blur: pixel shader N (...)` line for each blur shader a run draws, with its kind.

**Motion blur off** (`[VR] MotionBlur=0`, `DrawMotionBlurStill`). The pass cannot just be skipped: its pixel shader writes the texture that the final pass reads next (colour in
sampler 0, depth, stencil and noise in 1..3), so a skipped draw leaves the target undefined. The draw is made once (no per-eye split, nothing to remap) as the game's own blur draws a picture in
which nothing moves. **What the blur writes matters, not just its colour:** the shaders that take `(u, v, depth, 1)` (8 of the 26, the one the game uses at its default settings among
them; they run on a target of half the size of the picture) write `mul_sat oC0.xyz, colour, c4.w` and, as the alpha, `mul_sat oC0.w, |velocity|, c6.z`, the length of the blur, and the final pass
(`psToneMap...MotionBlur...`: `lrp(scene colour, blur colour, max(blur alpha, ...))`, sampler `motionBlurTexture`) takes the blurred picture by that alpha. The first version of `MotionBlur=0`
swapped in a plain copy of sampler 0 with an alpha of 1 (taken from the other 18 shaders, whose alpha is a constant): the whole picture then came from the half-size texture, and a headset run
showed it very blurry. Now, for the kind that takes `(u, v, depth, 1)` as ps_3_0 with `g_vDepthScale` in c4, the blur's pixel shader is swapped for `kMotionBlurStillShader30`, which writes what
the blur writes at zero velocity, the colour image times `c4.w` (saturated) and an alpha of 0, for one texture read a pixel instead of the blur's twelve. Hand assembled, made once on first use
through `g_deviceOriginals[VT_CreatePixelShader]` (so `H_CreatePixelShader` never sees it); a Direct3D 9 pixel shader has to be of the model of its vertex shader, and the real ones of this kind are
all ps_3_0. Every other blur shader is drawn once with its own pixel shader and a matrix that moves nothing (the identity for the kind that takes `(u, v, depth, 1)`, the one that leaves each
pixel where it is for the kind that takes `(z*u, z*v, z, 1)`): it costs what it did, but writes what the game writes for a picture that does not move, whatever its alpha is. The blur's shader and
`g_mMatrix` are put back with the original setters (`g_currentPsHandle`, new, kept by `H_SetPixelShader` like `g_currentVsHandle`); the mod's own tracking (`g_currentPsInfo`) never changes. If
the handle is not known the blur is drawn as the game draws it. **The `g_vFilter` variants carry the game's depth-of-field blur as well** (the game's own name for the pass is `MotionBlurDoF`):
with `MotionBlur=0` that goes too, in any scene that uses them. `Log=1` writes one `motion blur: pixel shader N (...)` line for each blur shader (8 at most) a run draws, saying which way it was
drawn. The perf probe still counts the pass in its `motion-blur` phase (the classifier runs before the swap), so `[Perf]` shows what is left of it. **No gain in the frame rate was seen**
(2026-10-25, Valve Index: about 68 fps with the blur on and with it off): something else limits the frame (in the first measurements, section 13, the CPU did; not measured at this resolution), and the pass is small next to the others (it was too small to be listed
among the GPU phases of those measurements). The switch is a matter of taste then, a sharper picture without the blur, not a speed-up.

**Checks.** `tools/motion_blur_check.py` first runs `BlurMatrixForEye` and `BlurMatrixForEyeRawDepth` against a reprojection worked out from first principles (random game cameras, heads
turned up to 57 degrees about each axis, points in front of the camera: 13000 and 27000 points, worst error 1e-6 of the picture; no blur at the point straight ahead of the car for head turns
of 0, 25 and 40 degrees either way; for the raw depth kind no vector at all for a car at rest); each fails for a translation left out, the delta not inverted or left out, the projection
scales swapped, the eye's offset or y sign wrong, the depth terms wrong, the point's W lost and a wrong column (ten mutants of `BlurMatrixForEyeRawDepth`, nine of the other, tried on copies
of the source). It checks that the parser tells the two kinds of shader apart. Then it links the whole mod and runs `MotionBlurPerEye` on a real Direct3D 9 device with a stand-in for the blur
(a ps_3_0 and a ps_2_0 shader with the blur's constant table that paint red, each with the vertex shader of its model, and one that adds `g_vDepthScale` like the second kind). With `MotionBlur=0`
the ps_3_0 stand-in of the first kind must give a target that is the source texture times c4.w (2 in the check) with an alpha of 0, from one draw; every other stand-in must be drawn once
with its own shader and the matrix that moves nothing, and `g_mMatrix`, the shader, the tracking and the game's scissor state must be as before; the shader for it must be made once, not every
frame. With `MotionBlur=1`, or when the bound shader's handle is unknown, the stand-in is drawn once per eye (both
halves red), for either kind. Each of: the blur's shader or matrix not put back, an alpha of 1, no scale by c4.w, a wrong matrix for either kind, the wrong kind or model chosen, and the shader made every frame makes
the check fail (tried on copies of the source; the saturation cannot be seen on the check's 8-bit target).

**Shadow decals** (`ApplyShadowDecalForEye`). The decal PS rebuilds the pixel's camera-space position from depth and
screen position: `q = (u*Z, v*Z, k, Z)`, `Z = 0.115/|depth|`, then multiplies by `g_ShadowPlaneSpaceMatrix` to get
decal-plane coordinates. Per eye: `M_new = inv(B_e) * inv(C) * B_old * M_orig`, where `B(P)` (`ScreenZMapping`) maps a
camera-space point to `(u*Z, v*Z, 1, Z)` for projection `P` (columns `0.5*P[:,3]+0.5*P[:,0]`,
`0.5*P[:,3]-0.5*P[:,1]`, `(0,0,0,1)`, `P[:,3]`). The decal box itself is an ordinary stereo-duplicated draw. Confirmed
in the headset: player car, CPU cars, moving shadows past street lights, jumps.

**Deferred lights, full-screen passes** (`DrawDeferredLightPerEye`). New far corners from the eye's tangent box
(x from `(-1-P20)/P00`, `(1-P20)/P00`, y analogous, scaled by the original corner depth so the interpolated ray keeps
its length), light positions `pos * C` (w untouched), `g_vViewUpVector` rotated by the 3x3 of `C`, scissor = eye half,
draw, restore. Identified by: bound PS has `g_vPosition` (1–8 float4) and the bound VS has `g_vFar0`.
Confirmed better than the game's own in the headset; always on.

**Deferred lights, light volumes** (`ApplyLightVolumeForEye`). `g_vXY = (2/P00, -2/P11, (-1-P20)/P00, (1-P21)/P11)`,
`g_mLight' = inv(C) * g_mLight`, `g_vPosition' = pos * C`; the volume itself is a normal stereo-duplicated draw.

**Light streaks / flare sprites** (`DrawStreakToLayer`/`FlushStreakLayer`). Untracked alpha-blended sprites in the half-size
glow pass are positioned in the game's mono screen space and would ghost in both eyes. They are captured into a
layer and re-projected into each eye as a quad 40 units in front, through the head transform, so they stay locked to
the world direction. `NoLensFlare=1` drops the lens-flare ghost sprites outright (a screen-space effect that cannot work
per eye). Always on; it does not depend on any ini option.

**Reflection lookup** – see section 3.

## 6. The HUD / UI layers

The HUD is drawn **once**, to an off-screen texture, and composed into each eye at the end of the frame as a flat
panel hanging in front of the car (`Hud WorldLocked=1`: the panel is a point `Distance` metres ahead of the game
camera seen through each eye's head pose; `0` glues it to the view). Three layers exist:

* **HUD layer** (`g_hudTexture`): normal alpha-blended UI. Drawn with separate alpha blending so the layer keeps a
  proper coverage alpha (over-blend for normal draws, alpha preserved for additive ones).
* **Multiply layer** (`g_multiplyLayer*`): multiplicative blends (dark glass behind menu panels); starts white and is
  multiplied over the scene.
* **Top layer** (`g_topLayer*`): things the game draws straight to the screen *before/over* the UI, e.g. the connecting
  spinner, popups' veils; composed after the HUD, optionally with a veil.

Because the game's menus and HUD rely on being drawn over the live scene, a set of **heuristics** decides what goes
where. They are the most fragile part of the mod; they were tuned in the headset, one screen at a time:

* a draw goes to the HUD when it targets the back buffer after the scene, is alpha blended (or reads only small
  textures), and is not the opaque full-screen scene composite (`composite` test: shader-less blit or opaque draw that
  samples a >= 0.9 back-buffer-wide RT texture);
* rear-view-mirror picture = a wide short RT texture (>= 300 wide, aspect between 3 and 12, **not of the back buffer's own shape**,
  `HasBackBufferShape`) composited into the main image (see section 16 for why the last part is needed);
* menu UI composed into its own off-screen target before the 3D scene is also redirected to the HUD layer;
* **backdrop dropping** (`DropBackdrop`): the game puts a dimmed/blurred full-size copy of the scene behind its own
  popups. That copy would blank our separate UI layer, so it is dropped (detected with an **occlusion query** on a
  scratch target – `HudDrawCoversScreen` – or by texture size) and replaced by our own dim (`g_dimLevel`,
  colour (12,6,28), smoothed) or by a veil in the top layer, with strengths `VeilRace`, `VeilResults`, `VeilMenu`,
  `DimStrongPct`;
* a loading/intro video is an opaque full-screen picture in the HUD layer (512x256 second texture); coverage is
  measured on a 96x54 readback every 3rd frame and the layer is hidden while it is up (`HideFullscreenOverlay`);
* HUD draws that sample a blurred scene RT work because the panel is composed late; only the final full-screen composite
  is excluded.

**UI glow / bloom** (`[VR] Bloom`, one value for all of it). The game draws its 2D elements a second time into a small
render target (540x267, or the full-size UI target in the showroom menus), blurs that into a ~135x66 glow texture and
adds it over the finished picture in its last pass. That pass is mono and would smear across both eyes, so it is
intercepted instead: the glow texture is noted (`NoteSceneBloom`, one frame only) and its draw skipped, and
`ComposeHudLayers` adds it (`addGameGlow`, ONE/ONE, strength `Bloom`) over the world-locked layer quads, which follow
the UI in each eye. Identification is structural (render-target sizes, additive blend onto the back buffer), never by
shader id (ids differ between runs). Showroom menus: the UI draws are also run into the game's own UI target
("shadow" draw) so its glow chain has something to blur, and its final pass is skipped. In the race the glow-buffer
UI draws are kept (`g_menuGlowFrame`), except those that sample a render target (mirror picture, scene), so only the
2D elements glow. `Bloom=0` disables all of it (the UI draws into the glow buffers are then skipped, as they would smear across both eyes).
The loading screen's glow is the same value: it is a small blurred copy of the picture added over the whole screen (ONE/ONE), recognised by
its blend state and a render target under a quarter of the back buffer, and `DrawGlowAtStrength` (`draw_dispatch.cpp`) draws it at `Bloom` percent:
the blend factor (`D3DBLEND_BLENDFACTOR`) scales what is added up to 100 %, a second draw adds the part above that (200 % = twice), 0 skips it.
Flat, nothing of this runs (`Bloom` is forced to 100 and the game's glow pass is left alone).

**UI clipping with the stencil buffer** (always on). The game clips its UI with stencil, not scissor or viewport (the
log shows scissor off and the full viewport on every UI draw): a *mask draw* (`STENCILFUNC=ALWAYS`, `STENCILPASS=REPLACE`,
reference 1..3, additive blend of black so it paints nothing) writes the region id, and the draws that must be clipped use
`STENCILFUNC=EQUAL` with that reference. This is what clips the scrolling car lists in the showroom. The layers used to have
no depth-stencil surface, so every one of those tests silently passed and the lists spilled out of their frame. Now all
layers share one D24S8 surface (`g_hudDepth`, layer-sized; the game also has one stencil buffer for its whole UI):
`BeginLayerDepth` swaps it in for the game's own surface (only when the game had one bound) and forces depth test and write
off, as before; it is cleared once per frame at the first layer draw (`ClearHudStencilOncePerFrame`) and whenever the game
clears the stencil of the surface it draws its UI with (`H_Clear` -> `OnGameStencilClear`, matched by surface pointer).
Colour-write-off draws (`cw=0`) used to be skipped; they now run into the stencil buffer only. A UI draw that is dropped
(backdrop copies, intro pieces) but writes the stencil is still run stencil-only (`DrawHudStencilOnly`), otherwise the draws
clipped by it would vanish. If the stencil buffer cannot be created the UI simply stays unclipped (logged). The log lines to
look for start with `UI stencil:`.

**Photo viewer** (`photo_stereo.cpp`). The game saves photos to `%APPDATA%\bizarre creations\blur\photo\*.jpg` from the frame it
rendered, so in VR they are the side-by-side picture (2160x1071 here: left eye | right eye). The menu's viewer loads the file
into a full-size render-target texture and draws it with the UI shader (`vs=256 ps=387`) onto the back buffer: blended in the
photo list, then opaque once a photo is opened. The opaque draw samples a >= 90% render-target texture, so the "composite" rule
leaves it untouched (plan 0) and the finished picture is head-locked on the lenses, huge, and each eye shows its own half
(scaled, shifted, but nothing like a screen in the world). `IsPhotoDraw` recognises the photo's texture (the first UI-shader
draw of a full-size render-target texture within 3 s of the file being opened picks it) and then every back-buffer draw that
samples it in slot 0 while the 3D menu scene is on; `DrawPhotoStereo` runs those once per eye into the per-eye layers (`g_mirrorSurface`, which therefore always exist) with a per-eye texture that holds that eye's half
of the photo, 1:1 and centred on a transparent background (`PreparePhotoEyeTextures`); `ComposeHudLayers` shows the layers on
the HUD panel below the HUD. The two halves are not a ready pair on a flat panel: each was rendered with its own off-axis
projection (tangent box), so the point at infinity is at `-left / (right - left)` of its image (about 60% in the left one and
40% in the right one). `PhotoShifts` moves each image so that point lands in the middle (infinity on the panel, nearer things in
front), and both eyes are trimmed to the window they have in common, so no strip is seen by one eye only. The capture-time
tangent boxes are not in the file, the current headset's are used. This was judged right in the headset as it is (a depth
and a crop setting with live keys were tried and removed: a larger separation was no better than this plain alignment). The
viewer's other draws (the blurred backdrop pair, drawn with the 2160x1071 render target and
the tone-mapping constants) are still dropped by the backdrop rule. A non-stereo photo (taken without the mod) would be split in
halves too. (How it was found: dumping the submitted picture and each layer to files showed the photo baked into the back buffer
but absent from every layer; the temporary dump code is gone, `GetRenderTargetData` of the back buffer / a layer in
`VrSubmitFrame` is the way to do it again.)

Known: popups still look slightly dim (`Veil*` settings); root cause never found.

## 7. VR session, menus, loading

* **Deferred start** (`DeferVR`): the game stays flat (SteamVR theatre) until the first level starts loading and a
  3D menu scene is really drawn, then OpenVR is initialised. **Threaded init** (`ThreadedInit`): OpenVR is started on
  a worker thread because the game stalls ~0.75 s loading the intro videos right after device creation, and SteamVR
  marks a scene app that misses that long as "not responding" and throws up the dashboard.
* Pose: `WaitGetPoses` runs right after `Present` (VR_NextPose), so the pose is ready when the next frame starts.
  **Tracking space is SteamVR's seated space** (`SetTrackingSpace(Seated)`; menu overlay in the same space), so the
  reference for "straight ahead" is SteamVR's own zero pose, not something the mod remembers. A recentre in SteamVR
  therefore just works. The mod's own recentre (`F9`; also at every race start if `AutoRecenter=1`, the default) calls `IVRChaperone::ResetZeroPose(Seated)`, i.e. the
  same thing as SteamVR's recentre, and logs the head pose it found. Only if the chaperone interface cannot be
  obtained does it fall back to remembering a reference pose itself (`refYaw` / `refPos`). The
  `SeatedZeroPoseReset` / `StandingZeroPoseReset` events only re-place the floating menu.
* Frame transport: back buffer → `StretchRect` to a render target → `GetRenderTargetData` to system memory →
  `UpdateSubresource` into a D3D11 texture created on the adapter SteamVR wants (`GetDXGIOutputInfo`) → `Submit`.
  This is a CPU round trip and a real cost; a shared-handle texture would avoid it (idea, section 10).
* Menu: dark scene layer submitted to both eyes + the game image on a `SetOverlayTransformAbsolute` overlay placed
  flat in front of the head (re-placed for a few frames when the dashboard is up or after a recentre because the
  pose is unreliable then).
* **Movies** (tutorial videos, cutscenes, credits: Bink files in `UI\movies\*.bik`, decoded by `binkw32.dll`). The game stops
  drawing its 3D scene and plays the video full screen, so those frames go to the floating screen like any flat frame, but it
  keeps drawing its menu UI under the video, and that UI (diverted into the HUD layers) was composed over the picture. A
  `.bik` file open (seen by the `CreateFile` hook, `OnGameFileOpen`) arms `g_moviePhase`; the first frame without scene draws
  makes it "playing" (`UpdateMovieState`, called from `VrSubmitFrame`), and the first frame with scene draws ends it (an armed
  state that never sees the scene stop times out after 8 s). While playing, `ComposeHudLayers` composes nothing. Log lines:
  `movie opened: ...`, `movie: the 3D scene stopped ...`, `movie: over ...`.
* Hotkeys: F8 mode override, F9 recentre, F10 head tracking on/off.
* With `[VR] Api=openxr` the runtime is reached through OpenXR (`openxr_runtime.cpp`); the pose, recentre, menu and submit steps above have OpenXR counterparts, section 27.

## 8. Resolution and display modes

**Why 4K looks better on a 1080-per-eye headset.** The lens warp needs about 1.4x supersampling (SteamVR's default for
a Vive is 1512x1680 per eye); the SBS target at 3840x2160 is 1920x2160 per eye, which also smooths shimmer. 1920x1080
gives only 960x1080 per eye.

**Resolution list** (`[VR] Resolution`, default 2160x1200; `kHeadsetSizes` in `display_modes.cpp`). The game lists the adapter's display modes; the mod
wraps `GetAdapterModeCount` / `EnumAdapterModes` (IDirect3D9 vtable slots 6 / 7). In VR it hides the monitor's modes (16:9, 4:3: they mean nothing for a
picture that is two eyes side by side) and offers the headset sizes of the README table instead (panel per eye x 1.4 each way, both eyes together: Vive
3024x1680 ... Bigscreen Beyond 7168x3584, plus the Vive panel size 2160x1200 and the Index's SteamVR size 4936x2740), once per refresh rate the monitor
has (the game lists a size per rate), ascending. The current `Resolution` is always added when it is not in the table, so a custom size never vanishes and
the size the game starts at is always in its list. Flat the wrappers pass the monitor's own list through. None of the offered sizes is a monitor mode, and
a full-screen device needs one, so `AdjustPresentParams` makes any full-screen request for a size the monitor has no mode of (`MonitorHasMode`, the original
enumeration) a windowed back buffer; only the headset sees the picture.

**Where the size lives** (`video_settings.cpp`).  In VR the game's picture size is the VR size and belongs in BlurVR.ini (`[VR] Resolution`); the game's own
settings.xml keeps the flat game's mode, so Stereo=0 / Enabled=0 starts at the old resolution again.  The game keeps its mode in five attributes of the
`<shared_graphical_settings>` element (`width`, `height`, `windowed`, `x`, `y`) and reads them right after the proxy is loaded, so `DllMain` runs
`VideoSettingsStart` first: it copies the five values to `BlurVR\display_backup.ini` (`[Flat]`, `[Session] Open=1`), copies the whole file to
`settings.xml.blurvr-backup` beside it, and seeds the element with the VR size and `windowed="1"` (the extra mode is not a monitor mode, so it can only be a
window; this is the state the old runs reached by themselves once the size had been picked).  `VideoSettingsEnd` puts the five values back whatever the game wrote meanwhile (other attributes such as `vsync` stay as the game wrote them) and sets `Open=0`.  A
start that finds `Open=1` (the game was killed from outside) restores the flat values first, in flat mode as well as in VR, and in VR keeps the flat values from
the sidecar instead of copying the seeded ones.  Files are read and written as bytes (BOM, CRLF and everything outside the five values are untouched), the
write goes to a temporary file that is moved over the original, and nothing is touched when the element or the file is missing.  A size the game switches to
on a running device (`AdjustPresentParams(pp, true)` from `H_Reset`) is `VideoSettingsNoteMode`d when it is one of the modes the mod offered the game (a size
asked for for another reason, a dragged window say, is not in that list): `WritePrivateProfileString` stores it as `[VR] Resolution` (the other lines of
the ini, comments included, are kept).  The first `CreateDevice` is never taken as a pick.

**When the flat mode is put back.**  Blur never leaves through `ExitProcess`: its shutdown function tears the subsystems down and ends with `TerminateProcess(GetCurrentProcess(), 0)` (Blur.exe 0x42F826, the same in the Steam and non-Steam
builds), and a terminated process never gets `DLL_PROCESS_DETACH`.  Until 1.5 the restore hung on that detach, so it never ran in a real game (every log of 1.4 starts with "the last VR run did not end properly")
and only the next start of the same BlurVR folder put the flat mode back.  Now `InstallExitHook` patches the executable's import of `TerminateProcess` (`PatchImport`; from DllMain, and again from Direct3DCreate9 in case the import table is bound later) and
the hook calls `VideoSettingsEnd` when the target is the process itself, the last moment the game gives (whatever the game wrote to settings.xml before that is overwritten in the five values only).  The `DLL_PROCESS_DETACH` path stays for a game
that does use `ExitProcess`; a process killed from outside is still repaired at the next start.  Two installs of the game share `%APPDATA%\bizarre creations\blur\settings.xml` but each BlurVR folder has its own sidecar: an install whose first start finds the VR mode
in settings.xml (the other one was killed) takes it for the flat mode.  `tools/exit_restore_check.py` runs a stand-in executable with the proxy in a scratch folder with its own `APPDATA` and ends it with `TerminateProcess` of itself, with a normal exit, and with
`TerminateProcess` of another process first (which must not restore): all three must leave the flat values in settings.xml and `Open=0`.

**V-Sync** (`[Display] ForceNoVSync`, default 0). The game saves `vsync="1"` in its settings and presents with a V-Sync wait, which held it at the monitor's 60 Hz (the watchdog lines in the log counted 60 presents per second) whatever the headset's refresh rate, so SteamVR had to reproject every frame. In VR, `AdjustPresentParams` (`CreateDevice` and `Reset`) therefore always sets `D3DPRESENT_INTERVAL_IMMEDIATE`, whatever the key says: `WaitGetPoses` right after `Present` is what paces the game to the headset (so 90 / 120 / 144 Hz follow the headset, never the monitor). Flat (`Stereo=0`), `ForceNoVSync=1` turns V-Sync off (unlimited frame rate) and 0 leaves the game's own setting (V-Sync on = the monitor's refresh rate). It does nothing when `Enabled=0`. There is no frame cap any more (`MaxFps` and `frame_limiter.cpp` were removed): V-Sync on is the way to hold a rate, and in VR the headset is.

## 9. Settings reference (BlurVR.ini)

The mod has three states. `Enabled` (1) is the master switch: 0 = the mod does nothing. `Stereo` (1, `[General]`) picks the half: 1 = VR, 0 = flat
(SteamVR, the size override, the D3D9Ex device, the extra video mode, the head-frustum hook and every `[VR]` / `[Render]` setting are off; only `[Display]`
and `[Input]` apply).  `LoadConfig` computes `g_cfg.vr = enabled && stereo`; VR-only code asks `g_cfg.vr`, not `g_cfg.enabled`, and the `[VR]` values that act without
looking at `g_cfg.vr` (Bloom, NoLensFlare, MotionBlur, the sizes) are forced to "as the game draws it" when flat.  A key that moved is still read from its old section
where `ReadIniInt(..., oldSection)` says so: Stereo, ForceNoVSync, SharedTransport, DesktopView, DesktopPresentEvery, FitDesktopWindow from `[Render]`.
`[General]` Enabled, Stereo, Log (0), AutoRecenter (1), RecenterKey (0x78 F9), ToggleKey (0x79 F10), ModeKey (0x77 F8), StartMode (0 auto /
1 floating screen / 2 race), ThreadedInit, DeferVR.
`[Display]` (VR and flat) ForceNoVSync (0; in VR V-Sync is always off, the headset paces the game).
`[Input]` (VR and flat) KeyRepeatMs (60 ms; 0 = off): menu key repeat limiter, `input_limiter.cpp` (section 19).
`[VR]` (ignored when flat) Api (`openvr`, the default, or `openxr`, section 27), OpenXRRuntime (the .json of an OpenXR runtime, empty = the system's active one), Resolution (WxH, 2160x1200; `CustomWidth`/`CustomHeight` of `[Render]` are read when it is missing), Bloom (100 %: the game's glow around
menus, UI and loading screens; 0 = none), NoLensFlare (0; 1 drops the flare sprites and the bloom/glow around lights), MotionBlur (1; 0 = the blur pass copies the picture, section 5), Reflections (1; 0 = the game's reflection pass is switched off in races, section 13), SharedTransport (1, section 15),
DesktopView (off; left / right / both; 1 / 2 / 0 / 3 still read, section 20), DesktopPresentEvery (1, section 18), FitDesktopWindow (1), FpsCounter (0, section 28).
`[Tracking]` Rotation, Position, PositionScale (game units per metre), IpdScale.
`[Render]` (VR only) FovScale, SeamGutter (0); undocumented tuning: MainPassMinWidthFraction (0.9), RequireDepthBuffer, HalfResPasses, ReflectionLookupFix,
ReflectionFlip; `[Hud]` CoverageEvery (8, section 18).
`[Hud]` WorldLocked, Scale (0.6, fraction of one eye's width), Distance, OffsetY, XOffset, Mirror3D (1),
MirrorConvergence (1.5), MirrorEyeScale (1), MirrorHeadParallax (1),
VeilRace, VeilResults, VeilMenu, DimStrongPct, CountInDrop (1), DropBackdrop, HideFullscreenOverlay.
`[Menu]` Distance, Width, HeightOffset.
`[Matrix]` Layout (`auto`|`row`|`transposed`).
`[Debug]` KeyMirror3D (Num6), KeyPerf (Num7), KeyDump (Num8; section 16), KeyFps (Num9; section 28), AdjustViewRotation.
`[Perf]` Enabled (0; the probe in section 13).
`[Foveation]` Report (1; the lens analysis in section 13).

## 10. Known limits, features, ideas

### Stereo rear-view mirror (finished feature)

Settings: `[Hud] Mirror3D`, `MirrorConvergence`, `MirrorEyeScale`, `MirrorHeadParallax` (defaults 1, 1.5, 1, 1);
Num6 toggles it live (`PollSwitchKeys`, `vr_runtime.cpp`; each press is logged as `switch: ...`). Code: `mirror_stereo.cpp`. Confirmed working in the headset. The game renders the mirror into a small wide render target (e.g. 864x160). While Mirror3D is
on, each 3D draw into it (`IsMirrorScenePass`) is run twice by `DrawMirrorSceneStereo`: once per half of the target
(left half = left eye), with the rear camera moved to that eye. The eye offset is taken from `eyeCameraDelta`, turned
into world space with the main camera's axes (`NoteMainCameraAxes`) and into the rear camera's space with its own
axes (`RecoverCamera`); the projection gets a horizontal shear so that things `MirrorConvergence` metres away have no
disparity (nearer pops out of the panel, farther recedes). When the UI draw of the mirror picture comes
(`g_drawIsMirror`), `PrepareMirrorEyeTextures` stretches each half into its own full-size texture and the draw is run
twice, into the per-eye layers `g_mirrorSurface[eye]`; `ComposeHudLayers` draws each eye's layer on the HUD panel
(same place and distance as the UI). If the rear camera cannot be recovered the picture stays mono (log line
"rear camera not recovered"). The game flips the picture in the UI draw, not in its projection.

Known limits:

* **Culling** by the game's narrow camera frustum (objects missing at the edges when looking far off the travel
  direction): fixed in 1.2, the game is given a frustum around the head (section 25).  The rear-view mirror and the planar
  reflections still cull with the game's own cameras.
* Popups slightly dim; HUD detection is heuristic; menus are a flat screen.
* Cockpit camera, side culling and right-stick look untested.

Ideas (none started):

* Lens-matched / foveated shading in software: render the centre of each eye at higher density than the edges. Planned as a
  focus tile + half-density context tile per eye (four views in the one main-sized target), staged behind measurements;
  section 13 has the measuring tools and the numbers to look at first. OpenVR also has `GetEyeTrackedFoveationCenter`,
  which a later gaze-driven focus could use.
* HUD layer at higher resolution than the back buffer. (Tried and removed: layers with mip-maps and/or at 2x size. The HUD
  is 2160 px wide but shown about 650 px wide per eye, so small text shimmers, but mip-maps were too soft and the game's UI
  textures only exist at 1x, so 2x added no detail; draws with pre-transformed vertices also ignore a viewport scale.)
* Shared-handle texture to skip the CPU round trip in frame transport.

## 11. How to investigate a new rendering problem

This is the loop that produced every per-eye fix:

1. Find the pass: add temporary logging in `RunDraw` for the symptom (draw count, bound VS/PS ids, render target
   size, `g_boundTextures`, the constants from `g_vsConstants` / `g_psConstants`). Identify the shader by constant names
   (`ParseVertexShaderConstants` / `ParsePixelShaderConstants` already record them).
2. Dump the pixel shader bytecode (the token stream in `H_CreatePixelShader`) to a file, then
   `python3 tools/ctab.py file.bin` lists constants with registers, and `python3 tools/dis.py file.bin` disassembles.
3. Work out what the shader assumes about the camera (UV range, centre, frustum corners, view-space light
   positions, matrices in UV space).
4. Express the same transform for one eye (`BuildEyeProjection`, `eyeCameraDelta`), usually as a left-multiplication
   of the constant by a small matrix, and draw the pass once per eye with the eye half as scissor.
5. Ask for a screenshot of the symptom at a fast, cluttered moment (shadows and motion blur show up best at speed
   with a CPU car nearby); compare with the log.

For screenshots the frame dump (Num8, `frame_dump.cpp`) does the job: it saves the finished side-by-side picture and each HUD layer, so a report can show exactly
what was sent to the headset.  For a temporary look at the draws, add logging in `RunDraw`; the logging that was used for that during development is in
`experiments/diagnostics-removed-in-1.1.patch` (section 19).

## 12. Code map

Everything is in `src/`; `build.sh` compiles all `*.cpp` into one `d3d9.dll`.  Dependencies run downwards in this table
(device_hooks and draw_dispatch sit on top of everything else).  `common.h` holds the shared includes and the `DrawCall`
callback type; each module header lists what the module exports (with one-line comments).

| Module | Purpose | Functions (definitions in the .cpp) |
|---|---|---|
| `core` | Module location, log file and logging. | `Log` |
| `config` | BlurVR.ini settings.  Every key has a default; see the settings reference in NOTES.md. | `IniFilePath`, `ReadIniInt`, `ReadIniDouble`, `LoadConfig` |
| `matrix` | 4x4 matrices in double precision, row-vector convention (like D3DX). | `Identity`, `Multiply`, `Invert`, `LoadMatrix`, `StoreMatrix` |
| `d3d_hooks` | Loading the real d3d9.dll, vtable patching, and the Direct3D function-pointer types the hooks use. | `LoadRealD3D9`, `PatchVtable` |
| `shader_tracking` | Shader bookkeeping: constant tables of every vertex/pixel shader the game creates, the constants it uploads, and the shaders currently bound. | `NamesEqual`, `ParseVertexShaderConstants`, `ParsePixelShaderConstants` |
| `game_mode` | Menu / race detection: the game opening files below "levels\" tells us which scene is loading. | `IsRaceMode`, `IsMovieFrame`, `UpdateMovieState`, `OnGameFileOpen`, `H_CreateFileA`, `H_CreateFileW`, `PatchModuleImports`, `InstallFileOpenHooks` |
| `vr_runtime` | OpenVR / SteamVR session: initialisation, head pose, per-eye camera deltas, floating menu screen, and submitting the finished frame to the compositor.  With `[VR] Api=openxr` the runtime calls go through `openxr_runtime`. | `ReleaseStagingResources`, `EnsureStagingResources`, `EnsureFrameTexture`, `GetEyeToHead`, `SplitVerticalTangents`, `VrInitOpenXr`, `VrInit`, `VrUpdatePose`, `ComputeMenuTransform`, `PlaceMenuOverlay`, `SubmitFrameOpenXr`, `SubmitFrameBody`, `VrSubmitFrame`, `StartVrInitThread`, `VrInitThread` |
| `openxr_runtime` | The OpenXR backend (section 27): loader, instance, Direct3D 11 session, head and eye poses, the swapchain and the layers (race picture as a projection layer, menu as a quad). | `XrRuntimeInit`, `XrRuntimeNextPose`, `XrRuntimeEyeToHead`, `XrRuntimeCovered`, `XrRuntimeFocused`, `XrRuntimeSubmitScene`, `XrRuntimeSubmitMenu`, `XrRuntimeFinishFrame` |
| `render_state` | What the game is rendering into right now (render target, viewport, scissor, bound textures) and classification of the current pass (main scene, half-size, reflection). | `RefreshRenderTargetState`, `IsMainSizedTarget`, `IsHalfResPass`, `IsReflectionSizedPass`, `IsMainPass`, `NoteMainSizedCamera`, `IsReflectionPass` (section 29) |
| `camera` | Recovering the game camera/projection from g_mViewProj and building the headset projection for each eye. | `DecomposeViewProjection`, `TryRecoverProjection`, `BuildEyeProjection` |
| `stereo_scene` | Stereo duplication of main-pass draws: rewriting the tracked shader matrices for one eye (the products that do not change from draw to draw are cached, section 14), and carrying the game's reflection clip plane to the eye (section 29). | `ReflectionColumnsMatch`, `GetRewrite`, `RecordRemap`, `ApplyEyeConstants`, `RestoreEyeConstants`, `ApplyCameraShift`, `ApplyEyePixelConstants`, `RestoreEyePixelConstants` |
| `device_state` | What the device holds versus what the game believes (vertex constants, scissor, clip plane 0): per-eye changes are undone lazily and redundant writes skipped (section 14). | `DeviceNoteGameVsConstants`, `DeviceSetVsOverrides`, `DeviceSetEyeScissor`, `DeviceNoteGameClipPlane`, `DeviceSetEyeClipPlane`, `DeviceRestoreAll`, `DeviceStateReset` |
| `mirror_stereo` | Rear-view mirror rendered once per eye (see section 10). | `IsMirrorScenePass`, `DrawMirrorSceneStereo`, `PrepareMirrorEyeTextures`, `NoteMainCameraAxes`, `ReleaseMirrorStereo` |
| `desktop_message` | The window on the desktop with `[VR] DesktopView=off` (section 20): "Playing in VR" and the game's logo, made with GDI, put into the back buffer's corner and presented twice a second instead of the game's picture. | `DesktopMessageWanted`, `DesktopMessageDue`, `DesktopMessagePaint`, `ReleaseDesktopMessage` |
| `photo_stereo` | The main menu's photo viewer: its picture (a side-by-side photo) drawn once per eye into the per-eye layers (see section 6). | `IsPhotoDraw`, `PreparePhotoEyeTextures`, `DrawPhotoStereo`, `ReleasePhotoStereo` |
| `draw_dispatch` | Per-draw-call dispatch: decide how each game draw is handled and run it. | `ClassifyDraw`, `RunDraw`, `ApplyReflectionSwitch`, `TestSkipDraw` |
| `hud` | HUD / UI layers: draw the game's 2D UI into off-screen layers and compose them into each eye as a world-locked panel. | `ComputeHudViewport`, `ReleaseHud`, `EnsureHudTargets`, `HudDrawCoversScreen`, `DrawHudToTexture`, `DrawHudToMultiplyLayer`, `DrawHudStencilOnly`, `OnGameStencilClear`, `ComposeHudLayers`, `IsMultiplicativeBlend` |
| `streaks` | Light-streak sprites: captured in a layer and re-projected into each eye. | `ReleaseStreakLayer`, `EnsureStreakLayer`, `DrawStreakToLayer`, `FlushStreakLayer` |
| `screen_space_passes` | Per-eye versions of the game's screen-space passes: motion blur (or a plain copy of the picture with `MotionBlur=0`), shadow decals, deferred lights, and the bloom blackout for `NoLensFlare`. | `DrawWithoutBloom`, `MotionBlurPerEye`, `DrawMotionBlurAsCopy`, `ScreenZMapping`, `ApplyShadowDecalForEye`, `TransformPoint`, `TransformDirection`, `FindFarCornerRegister`, `DrawLightsPerEye`, `ApplyLightVolumeForEye` |
| `device_hooks` | The IDirect3DDevice9 hooks: state tracking, shader tracking, draw interception, Present/Reset. | `H_Present`, `H_Reset`, `H_Clear`, `H_CreateTexture`, `H_SetRenderTarget`, `H_SetDepthStencilSurface`, `H_SetViewport`, `H_SetRenderState`, `H_SetScissorRect`, `H_SetTexture`, `H_CreateVertexShader`, `H_SetVertexShader`, `H_SetVertexShaderConstantF`, `H_CreatePixelShader`, `H_SetPixelShader`, `H_SetPixelShaderConstantF`, `H_DrawPrimitive`, `H_DrawIndexedPrimitive`, `H_DrawPrimitiveUP`, `H_DrawIndexedPrimitiveUP`, `HookDevice` |
| `perf_probe` | Timing probe (section 13): GPU time per rendering phase from timestamp queries, CPU time of the draw hooks and the present path. | `PerfToggle`, `PerfSetPhase`, `PerfClassifyDraw`, `PerfFrameClose`, `PerfPoll`, `PerfScope`, `PerfDrawScope`, `PerfDriverScope` |
| `lens_model` | Lens analysis (section 13): pixel need across the flat image, hidden area, focus/context layout costs. | `StartLensReportThread` |
| `frame_dump` | Numpad 8 saves the picture sent to the headset and the HUD/top/multiply layers as .bmp files (section 16, 19). | `FrameDumpPoll` |
| `fps_counter` | The FPS counter in the headset (section 28): frames a second counted from `VrSubmitFrame`, a head-locked panel (plate and a built-in 5 x 7 pixel font as solid rectangles) drawn into both eyes of 3D frames (races and the showroom), with a graph of the last 18 seconds under the figure. | `FpsCounterFrame`, `FpsCounterFrameAt`, `FpsCounterDraw`, `FpsCounterColor`, `FpsCounterGlyph`, `FpsCounterGraph`, `ReleaseFpsCounter` |
| `inline_hook` | A minimal x86 inline hook for functions in the game's own code (`InstallInlineHook`: checks the expected bytes, copies them to a trampoline, patches a jump), and the patch of an entry in the executable's import table. | `InstallInlineHook`, `PatchImport` |
| `game_signatures` | Finds the game's two camera hand-over functions in the code of any build by wildcarded byte patterns (section 25); pure code, also used by `tools/camera_sig_check.py`. | `InitGameCameraScan`, `ScanForGameCameraCode`, `ResolveGameCameraCode` |
| `game_camera` | Hooks on the game's camera hand-over to its views (section 25): the game culls with a frustum around the head (VR only), once the main camera is recognised and the frustum layout checked; the planar reflection view gets one around the mirrored head. | `GameCameraPoll`, `GetMainCameraPose`, `ReflectionFlipAxis` |
| `ai_hud` | The game's AI car markers (section 26): hook, detour and patches in the game's UIAiHud update; with `Stereo=1` the markers are shown over their cars (cut out of the HUD layer, billboard per eye); no settings. | `AiHudPoll`, `AiHudComposeMarkers` |
| `window_spoof` | Section 16: answers the game's window-size queries (GetClientRect, WM_SIZE) with the size it asked for when Windows clamped the window to the desktop. | `InstallWindowSizeSpoof`, `WindowSpoofIntendedSize`, `ReadSavedVideoSize` |
| `video_settings` | Where the display mode lives: BlurVR.ini `[VR] Resolution` in VR, settings.xml (restored at exit) for the flat game. | `VideoSettingsStart`, `VideoSettingsEnd`, `InstallExitHook`, `VideoSettingsNoteMode` |
| `input_limiter` | Menu key repeat limiter (`[Input] KeyRepeatMs`): hooks the game's DirectInput keyboard poll and `GetKeyState`, shows a held key as a tap and then a tap every interval, in the menus only. | `InstallInputHooks`, `InputLimiterFrame`, `KeyRepeatShownDown` |
| `display_modes` | Extra display mode for the game's resolution list, and the IDirect3D9 hooks. | `BuildModeList`, `H_GetAdapterModeCount`, `H_EnumAdapterModes`, `AdjustPresentParams`, `H_CreateDevice`, `HookDirect3D` |
| `dllmain` | d3d9.dll exports (forwarded to the real d3d9.dll) and DllMain. | `Direct3DCreate9`, `Direct3DCreate9Ex`, `RealProc`, `D3DPERF_BeginEvent`, `D3DPERF_EndEvent`, `D3DPERF_SetMarker`, `D3DPERF_SetRegion`, `D3DPERF_QueryRepeatFrame`, `D3DPERF_SetOptions`, `D3DPERF_GetStatus`, `DllMain` |

### Source layout

* `src/` is the current code, split into the modules above.
* `archive/` holds earlier single-file versions of the code, for reference only (not built).

## 13. Measuring before foveating: the timing probe and the lens analysis

Both only write `BlurVR.log`; neither changes the picture.

**Timing probe** (`perf_probe.cpp`; `[Perf] Enabled=1`, or Num7 in game to switch it on and off). Once a second, while a race is
running, two lines:

* `perf: <fps>, frame <ms> | cpu per frame (ms): draw hooks (driver, mod = us/draw), hud compose, readback, upload, submit,
  present+limiter, pose wait, game and other | draws per frame: game, issued`. `readback` is `StretchRect` +
  `GetRenderTargetData`, which waits for the GPU, so a large number there means the GPU is the limit; `pose wait` is the
  `WaitGetPoses` pacing wait, so a large number means neither side is the limit. `mod` is the time in `RunDraw` that is not the
  game's own draw call. `issued / game` is the duplication factor.
* `perf: gpu <ms> per frame: <phase> <ms> (<draws>) ...` with the phases `offscreen` (shadow maps, odd-shaped targets),
  `scene-main` (the stereo-duplicated geometry pass), `scene-half` (reflection, glow sprites), `lights`, `motion-blur`,
  `main-other` (composite, tone mapping), `post-low` (bloom chain), `hud`, `hud-compose`, `copy` (the frame for the compositor).

How it works: `RunDraw` classifies every draw (`PerfClassifyDraw`: HUD plan, light/motion-blur pixel shader, target size and
shape), and each change of phase issues a D3D9 timestamp query; the time between two stamps goes to the phase active in
between. Results are read after `Present` without waiting. `VrSubmitFrame` sets `hud-compose` and `copy` by hand
(`PerfSetPhase`), which also stops the classifier for the rest of the frame. A phase that flips back and forth many times
costs stamps (192 per frame, then the frame is reported as `too many phase changes`). The timing itself costs a few
queries and two `QueryPerformanceCounter` reads per draw, so leave it off when not measuring.

**Reflections off (`[VR] Reflections=0`, Num5) and the shadow test (Num4)** (2026-10-26, from the frame-rate chase).  `ApplyReflectionSwitch` (draw_dispatch.cpp) runs once a frame from `SubmitFrameBody`, after `PollSwitchKeys`: while the setting leaves the reflections out and `IsRaceMode()`, the game's own switch for its reflection pass is written 0, otherwise it is what the game had (the showroom keeps its floor's reflection, section 29).  The switch is the tweakable `Amax.Views.ReflectionEnable`, a byte (0x111EB04 in the Steam build) that the reflection function (0x4297F0) tests first (`sub esp,0x208 / cmp byte [flag],0 / push ebx / push ebp / push esi / push edi / mov ebp,ecx / je`) and returns at once on 0: before any culling, state setting or drawing, so the game's own work for the pass goes as well as the mod's doubled draws.  `FindReflectionEnableFlag` finds the function by those bytes once, in the first race that needs the byte off (code unpacked by then; exactly one match, a committed writable page and a value of 0 or 1, else the log says so and `TestSkipDraw` leaves the reflection draws out at the draw call, which saves much less); the write is repeated every frame the byte differs (logged for the first 20), so a level that sets the tweakable again does not bring the pass back.  Num5 flips `g_cfg.skipReflections` live (`switch: planar reflections in races LEFT OUT` / `drawn`); the `Log=1` lines of the setting start with `reflections:`.  **What it saves.**  Measured 2026-10-26 on the LA River at 3584x2016 (Valve Index, the probe on, Num4 and Num5 flipped every ~15 s for 2.5 minutes; the probe's `perf` line carries the state, `perf [reflections left out]: ...`): 109 race windows of a second, frame ms regressed on the main scene's draw count (1.65 ms per 1000, the one number the switches do not touch) with the state as an offset: **reflections off -0.74 +-0.11 ms** of a 13.2 ms frame (draw hooks -0.39, the game's own work -0.55, GPU -0.40; four clean before/after pairs of flips gave -0.6 to -1.2 ms, all the same sign), i.e. about 76 to 81 fps; shadows off (draw calls only) -0.26 +-0.17 ms, within the noise (12 windows in one stretch; pairs -0.22 to +0.36 ms); both off -0.52 +-0.15 ms.  The first test, which only left out the reflection draw calls, showed nothing: the game still culls and sets up each draw, so leaving out calls understates a pass.  Correcting for scene size matters: the busy and the quiet parts of a track differ by 2 ms and more, which a plain average of the states mixes up (reflections off seemed to have 400 fewer game draws a frame; those are the pass itself).  **Num4, the shadow test** (not in the ini, off at every start): `TestSkipDraw` is called right after `ClassifyDraw`, before the perf probe sees the draw; a shadow map is taken for a plan-0 draw into a square render target of 256 or more that is not the back buffer, in a race; the log showed it was the 256x256 target (about 21,000 draws a second, 270 a frame at 78 fps against about 3300 game draws), counted by target and logged every three seconds (`left out: shadow map: N draws into WxH in the last 3 s`).  There is no shadow tweakable in the game's list (only clip distances and `UseDepthStencilTexture`), so this leaves out draw calls only and is a floor for what the shadows cost.  `tools/hotpath_check.py` (`mirror` mode) checks that each switch leaves out its own draws and nothing else, in a race only.

**Headset facts** (`lens_model.cpp`; `[Foveation] Report=1`, on by default, once after VR starts): one `lens:` line with model,
refresh rate, SteamVR's own render size, eye-gaze support / `GetEyeTrackedFoveationCenter`, and the share of the flat image
the hidden-area mesh covers (`GetHiddenAreaMesh` returns its 8-byte struct in EDX:EAX on 32-bit x86, so it is called as
returning `unsigned long long`).

**Lens study, offline** (`tools/lens_dump.cpp`, `tools/lens_layouts.py`). With SteamVR running (the game need not be),
`lens_dump.exe <folder>` writes the runtime's distortion function on a 65x65 panel grid and the hidden-area mesh to CSV. The
script turns that into: the area of flat image per unit of panel area (the Jacobian of `ComputeDistortion`), which is smallest
at the sharpest spot; `need = smallest area / local area` is the pixel density a place really needs relative to that spot.
Cells whose position falls outside the flat image (the explosive corners of the polynomial) or inside the hidden-area mesh are
left out; as a check, the lens shows 75 percent of the flat image and the mesh covers 25. It then finds the cheapest set of
nested rectangular tiles (each at one density, overlapping, no holes) for 2, 3 and 4 tiles per eye, strictly (every place gets
at least its need) and with `--tolerance` (need relaxed with eccentricity: 15 deg full, then down to a quarter of the density at
50 deg; a scenario to judge in the headset, not a measurement). Costs are relative to a flat image whose density is 1:1 with the
panel at the sharpest spot.

### First measurements (Valve Index, RTX 3060, i7-11700K, game at 2160x1071, race with CPU cars)

* The Index wants 4936x2740 side by side from SteamVR (13.5 MP); the mod renders 2.3 MP. A flat image that is 1:1 with the
  panel at the sweet spot has 2.5x the panel's 2.3 MP per eye, about 11.6 MP side by side. The picture is therefore soft mainly
  because it is under-sampled at the centre (0.45x linear), not because the periphery wastes pixels.
* Lens: the sharpest spot is at tangent x -0.23 (left eye) / +0.23 (right eye), y 0, not at the middle of the tangent box. The
  hidden-area mesh is 25 percent of the flat image. The ideal continuous lens-matched warp would cost 32 percent of a flat image.
  Strict-need rectangular tiles: 2 tiles per eye 81 percent (focus box 26 percent of the image at full density, context at 74
  percent linear), 3 tiles 83, 4 tiles 88 (overlap with no holes eats the gain). With the eccentricity tolerance: 2 tiles 59,
  3 tiles 56. So a focus + context inset only pays if the periphery may be rendered clearly below the lens-matched need.
* Frame (race, about 58 fps against the headset's 90 Hz): 17.2 ms. CPU on the render thread about 13.4 ms: the draw hooks 6.3 ms,
  of which the mod's own work is 4.7 ms (1.45 us per game draw, 3261 game draws become 6155 issued), the game and everything else
  7.1 ms; then the end-of-frame stall: `StretchRect` + `GetRenderTargetData` + `LockRect` wait about 2.8 ms (the `readback`
  scope reads 0, the wait lands in `upload`), and a GPU `copy` phase of 3.35 ms for the 9 MB frame. Pose wait is 0.4 ms, so the
  game is late for every 90 Hz frame.
* The GPU timestamps include time the GPU waits for the CPU to issue draws (the draws arrive at the CPU's pace: `scene-main`
  takes about 2.2 us per game draw, the same as the hooks), so the phase times are upper bounds on GPU load. GPU phases:
  scene-main 4.3, main-other 5.1 (7-10 full-screen draws), offscreen 2.0, copy 3.35, hud 1.0, scene-half 0.6, post-low 0.3,
  lights 0.3. The frame is CPU-limited and then serialised by the readback; whether the GPU has headroom is answered by a
  resolution sweep, not by these numbers.
* Consequences: the readback grows with the pixel count (about 3.35 ms of GPU copy per 2.3 MP, so about 7.4 ms at 5 MP and far
  more at 11 MP), so higher resolution needs the shared-texture transport first; the mod's per-draw cost (4.7 ms) has to come
  down before any extra view is added.

## 14. Per-draw cost: where the mod's time goes, and the lazy device state

Measured in game with the timing probe (section 13): the mod's own time in `RunDraw` was 4.7 ms per frame (1.45 us per game draw, 3261
game draws, 6155 issued). The matrix maths is cheap (host micro-benchmarks, section below: a 4x4 `Invert` 57 ns, `Multiply` 18 ns,
`LoadMatrix` 5 ns, `StoreMatrix` 7 ns). What costs is the **number of Direct3D 9 calls** the mod adds around every duplicated draw: per
plan-1 draw, a `SetRenderState` and three `SetScissorRect`/`SetRenderState` calls for the scissor, and two uploads per tracked matrix per
eye (rewrite, then restore), about 31,000 extra device calls per frame, each of them a call into the driver.

**Lazy device state** (`device_state.cpp`). The per-eye draws change two things the game does not know about: the tracked vertex
constants and the scissor test/rectangle. The mod now keeps a record of what it has overridden and of the game's own values (the
shadow `g_vsConstants`, `g_gameScissor*`), and:

* writes a register only when the device does not already hold that value (`DeviceSetVsOverrides`, `DeviceSetEyeScissor`);
* puts the game's values back only when something other than another per-eye draw is about to use the device: `DeviceRestoreAll` runs
  in `RunDraw` for every draw that is not plan 1, and in the `Clear`, `SetRenderTarget`, `Present` and `Reset` hooks. Registers an
  eye draw overrides but the next one does not write are restored by `DeviceSetVsOverrides` itself;
* learns about the game's own writes from the hooks (`DeviceNoteGameVsConstants` from `H_SetVertexShaderConstantF`,
  `DeviceNoteGameScissorEnable/Rect` from the scissor hooks), because a register the game writes holds the game's value again;
* treats whatever may have run in between as unknown after every restore: the first write after a `DeviceRestoreAll` is unconditional.

At every draw the device holds exactly what it held when the game's values were restored after each eye; only the call count differs.
**Rule for new code:** anything that draws or reads device state outside `RunDraw`'s plan-1 path must call `DeviceRestoreAll` first
(the mod's state blocks `Capture()` the device, so they must see the game's state). `RestoreEyeConstants` restores at once and is what
the mirror pass uses.

Two more changes in the same path: the two eyes of a game draw are issued in the opposite order every other draw (the second eye
of one draw and the first of the next are the same eye, so the scissor rectangle and any matrix the game did not change are already
in place; the eyes cover separate halves, so the picture is the same), and the products that depend only on the projection, the
eye box and the camera delta (`inverse(projection) * delta * eye projection`, and the same per reflection record) are cached under
a key of those inputs (`GetRewrite`, `RecordRemap`; a changed input rebuilds them, so no explicit invalidation exists).
Small items: `VertexShaderInfo::ui` replaces the per-draw `otherConstantNames.find("g_Ui")` (four of those ran for every draw),
`H_SetVertexShader/H_SetPixelShader` answer the shader lookup from a 16-entry cache (cleared when a shader is created),
`IntroFrameLogging` reads the clock only while a trigger is pending.

**Checking it: `tools/hotpath_check.cpp` + `tools/hotpath_check.py`.** The harness links every `src/*.cpp` except `dllmain.cpp` against
a fake device, builds shaders through the real constant-table parsers, replays synthetic frames (shadow maps, reflection pass, scene,
decals, light volumes, plain draws mixed into the scene, lights, post passes) through `RunDraw`, and hashes the whole device state
at every draw (the draws of one `RunDraw` call are summed, so eye order does not matter) and at the end of every frame.
`python tools/hotpath_check.py --src src --compare <old src folder>` builds both, runs eight configurations (default, transposed
matrices, mono, seam gutter, a reflection flip mode, no reflection lookup, no view rotation adjustment, game scissor test on) and
reports whether the hashes are equal; with `--no-bench` omitted it also prints nanoseconds per draw, at 0 and at 120 ns per device
call. Mutations of `device_state.cpp` (skipping a restore, skipping the game-write notification) change the hash, so it does catch
that kind of bug. What it does not cover: the hooks themselves (the flush points were checked by reading), anything on the GPU.
Result for this change (synthetic frame of 3300 draws): hashes identical in all eight configurations; device calls per frame 30,800
-> 10,700; modelled frame cost at 120 ns per call 5.8 ms -> 3.2 ms (the compute alone is about 0.1 ms higher). The real figure is
the probe's `mod` value in game. `hotpath_check.exe micro` prints the cost of the matrix building blocks.

Measured in game (Index, RTX 3060, i7-11700K, 2160x1071, race with CPU cars, 19 seconds of probe lines each):

| | before | after the lazy device state |
|---|---|---|
| frame | 17.16 ms (58.4 fps) | 15.81 ms (63.3 fps) |
| draw hooks | 6.32 ms | 5.29 ms |
| of which the mod's own work | 4.72 ms (1.45 us per game draw) | 3.72 ms (1.14 us per game draw) |
| game draws / issued draws per frame | 3261 / 6155 | 3263 / 6172 |
| readback + upload wait | 2.77 ms | 2.74 ms |

The saving is about 1.0 ms in the mod and 1.35 ms in the frame, less than the host model suggested (it assumed 120 ns per device call;
the in-game saving of 20,000 fewer calls fits roughly 50 ns per call). What is left of the mod's 3.7 ms is mostly the per-draw
compute (about 0.4-0.5 us per scene draw on a hot cache in the host harness, more in game where the game's own work evicts it) and the
probe's own clock reads (a few hundredths of a ms per frame per 1000 draws). The frame is still CPU-limited: the game and the other
hooks are 6.9 ms, the mod 3.7, the driver 1.6, and the end-of-frame wait 2.7.

Second round (measured with cycle counters around the functions in the host harness, so only relative sizes count): the cache lookup
`GetRewrite` was the largest single piece at 78 ns per call, because it built a 200-byte key and compared it with `memcmp`; comparing
the inputs word by word against the cached key without building one brought it to 13 ns. `RunDraw` no longer divides for a seam
gutter that is off and no longer calls the shadow-decal and light-volume helpers for pixel shaders that cannot use them. The probe's
own clock is now the time stamp counter (a few ns instead of about 25 ns per read, four reads per draw), calibrated against the
performance counter, so the probe's `mod` figure drops by roughly 0.3 ms from that alone, which is real for the probe but does not
change the frame. Host cost per frame at no device-call cost: 1.54 ms -> 1.21 ms (the old code: 1.50). What is left per duplicated
draw is about 150 ns per eye (matrix rewrite 50, device write 40-60, reflection records 20, cache lookup and checks 30), close to the
floor for this design; the per-draw share of the remaining frame time is now the game's own 6.9 ms, the driver's 1.6 ms and the
2.7 ms end-of-frame readback wait, which the shared-texture transport removes.

Third measurement (after the second round; a different stretch of the race, so draws per frame are 6 percent lower): frame 15.24 ms (65.7
fps), draw hooks 4.71 ms, mod 3.30 ms = 1.08 us per game draw (was 1.45 at the start; part of that is the cheaper probe clock),
game and other 6.76, readback wait 2.85. Over the three runs the frame went 17.16 -> 15.81 -> 15.24 ms.
The draw-by-draw list for the `main-other` phase (now including draws with no shader at all) shows that phase's 5.1 ms is almost
entirely about 4.5 shader-less draws per frame ("ps -1 vs -1", 5.65 ms): fixed-function blits of a 2160x1071 target cannot take
1.25 ms each on this GPU, so the time is the CPU gap that follows them, i.e. the GPU timeline is paced by the CPU and its phase
times say little about GPU load. To measure real GPU busy time, bracket single draws with a stamp before and after.

Fourth look (2026-10-26, race at 3584x2016, mod 2.29 ms = 0.69 us per game draw over 3331 draws, driver 1.0): nothing left here is worth the risk.  The
host harness gives a frame of 1.27 ms at no device-call cost and 2.84 ms at 120 ns per call, so at the 50 ns the game shows the mod's draw work is about
1.3 ms of compute and 0.65 ms of device calls; the remaining 0.3 ms or so is probably the probe's own clock reads and the plan 0 draws.  Per eye draw `ApplyEyeConstants` is 82 ns
for one matrix (load, multiply and store 31, the override bookkeeping 25, the rest checks), `RunDraw` for the simplest scene draw 206 ns for both eyes.
The one idea that looked like a way to cut calls, merging the writes of adjacent registers into one `SetVertexShaderConstantF`, was checked against the
constant tables of all 188 vertex shaders in the game's exe: 110 of them have eye-dependent constants, on average 1.38 writes per eye draw (70 shaders one write,
38 two, 2 three); merged there would be 1.35: the matrix sits at c0, c4 or c12 and the camera position at c24, next to each other only in 4 shaders (a 3x3 view at c8 behind the c4 matrix).  Not worth the staging copy.
The calls per game draw are already about 2.5 vertex-constant writes and 0.75 scissor writes (the alternating eye order skips the rest).  A bit-identical cut of 20 percent
of the compute would be 0.25 ms of a 13.2 ms frame.  The levers that move the frame are fewer game draws and the end-of-frame wait (section 15), not this path.

## 15. Shared-texture transport (`[VR] SharedTransport`, on by default in VR)

**Why.** The frame goes to SteamVR through the CPU: `StretchRect`, `GetRenderTargetData`, a lock that waits for the GPU, a copy into a
Direct3D 11 texture. Measured in game it costs about 2.8 ms of CPU wait plus 3.3 ms of GPU copy at 2160x1071, and it grows with the
pixel count. On this machine (`tools/shared_transport_check.cpp`, no game involved) the old way costs 3.4 ms per frame at 2160x1071,
6.9 ms at 3024x1680 and 11.2 ms at 3840x2160; a shared texture costs 0.08, 0.14 and 0.20 ms.

**What had to be found out first** (all tested on this machine with small programs):

* A plain `IDirect3DDevice9` cannot create a shared texture (`D3DERR_INVALIDCALL` 0x8876086c for `CreateTexture` with a shared handle).
  An `IDirect3DDevice9Ex` can, and Direct3D 11 opens it with `OpenSharedResource` (format B8G8R8A8, render target + shader resource).
* An Ex device has **no managed pool** (`D3DERR_INVALIDCALL` for textures and buffers). A default-pool resource with
  `D3DUSAGE_DYNAMIC` can be locked like a managed one: every format tried (A8R8G8B8, X8R8G8B8, DXT1/3/5, L8, A8, V8U8, A16B16G16R16F,
  R16F, A4R4G4B4), mip chains, partial and read-only locks, cube and volume textures, vertex and index buffers (also with
  `WRITEONLY`, `AUTOGENMIPMAP`). Only `D3DLOCK_DISCARD` on a level other than the top is refused, which a managed texture would refuse too.
  `Reset` works with such resources alive; an Ex device is never lost.
* `StretchRect` from the X8R8G8B8 back buffer into an A8R8G8B8 target works and sets alpha to 255.

**What it does.** With `SharedTransport=1`: `Direct3DCreate9` returns an `IDirect3D9Ex` (same vtable layout for everything the game
calls, `dllmain.cpp`); `H_CreateDevice` makes the device with `CreateDeviceEx` (slot 20; a windowed device needs no display mode, a
fullscreen one gets one from the present parameters; on failure it falls back to a plain device and the old transport, `display_modes.cpp`);
`H_CreateTexture/VolumeTexture/CubeTexture/VertexBuffer/IndexBuffer` turn `D3DPOOL_MANAGED` into default pool + `D3DUSAGE_DYNAMIC`
(the first of each kind and up to 20 failures are logged: `SharedTransport: ...`); `H_Present` turns `S_PRESENT_OCCLUDED` and
`S_PRESENT_MODE_CHANGED` into `D3D_OK`; the mod's own managed 4x4 texture (the `NoLensFlare` black texture) is default pool too.
In `VrSubmitFrame`, a race frame is copied by `StretchRect` into a shared A8R8G8B8 render target (`CopyFrameShared`), an event query is
waited on so the compositor never reads a half-written frame, and `Submit` gets the Direct3D 11 side of that texture. Menus,
loading screens and the photo viewer still use the old path (they need the alpha fix-up of the CPU copy and are not time critical).
Anything that goes wrong (no Ex device, shared texture not creatable, adapters differ so `OpenSharedResource` fails, the fence times out)
logs one line and uses the old path for the rest of the run, per frame or for good.

**Checking it.** `tools/ex_proxy_check.cpp` loads the built proxy like the game, with the setting on, and makes sure managed resources of
every kind come back usable, drawing and `Present` work and `Reset` works. In game: the log has `SharedTransport: Direct3DCreate9Ex`,
`CreateDeviceEx -> ... (an Ex device ...)`, `first managed ... turned into ...`, `shared texture WxH ready` and `first race frame
handed over as a shared texture`; in the perf lines `readback` is now the `StretchRect` and `upload` the wait for the fence, and the
`copy` GPU phase should drop from about 3.3 ms to a fraction of a millisecond. If textures are missing or wrong, a menu behaves
differently, or the log shows `failed`, set `SharedTransport=0`: the game then runs exactly as before.
Not covered by any test here: a real fullscreen Ex device, and whatever the game does with its managed resources beyond locking them.

**First run in the game (SharedTransport=1): pickup icons were noise from far away and formed the right picture up close.** Cause, found
with `t6`-style tests here: on an Ex device, a default-pool dynamic texture is locked with a **row pitch of at least 128 bytes for
the small mip levels of uncompressed formats** (A8R8G8B8, L8, A8L8 ...; cube textures and the rows and slices of volume textures
too), where a managed texture gave the tight pitch (width x bytes per pixel). The game's texture loader fills levels assuming the
tight pitch, so every level whose tight pitch is below 128 bytes (32 pixels and under at 4 bytes) was written with the rows in the wrong
places: the far-away (small) mips were garbage, level 0 was right. Compressed formats are unaffected. `texture_pitch.cpp` fixes it:
the texture classes' `LockRect/UnlockRect` (and `LockBox/UnlockBox`) are hooked once (one vtable per class, shared by all textures), and
for a texture that was remapped from the managed pool, a lock whose real pitch is not the tight one returns a staging buffer with the
tight pitch (filled from the real surface unless `D3DLOCK_DISCARD`); on unlock it is copied back, row by row. Levels that already
match, compressed textures and every other texture are untouched. Locks through `GetSurfaceLevel` surfaces are not covered. The
proxy check (`ex_proxy_check`) fills a managed 2D texture and a managed volume texture level by level with the tight pitch and samples
each level back: without the fix levels 4-7 (2D) and 1-4 (volume) are wrong, with it all are right. The log notes the first staged lock.

The mod now keeps the log of the last real run: a launch that writes less than 20 KB does not replace `BlurVR.previous.log`, which
`BlurVR.log` is copied to at the start of a launch when it is 20 KB or more (a second launch that was closed after seven seconds
had erased the log of the run with the glitch).

**Measured with SharedTransport=1** (Index, RTX 3060, i7-11700K, 2160x1071, 59 seconds of probe lines, after the pitch fix; the log
shows the staged-lock note, `shared texture 2160x1071 ready (Direct3D 11 format 87, bind flags 0x28, misc 0x2)` and no failures):

| | start | CPU rounds done, readback | SharedTransport=1 |
|---|---|---|---|
| frame | 17.16 ms (58 fps) | 15.24 ms (66 fps) | **12.62 ms (80 fps, up to 90)** |
| wait for the GPU (`upload`; now the fence) | 2.77 ms | 2.85 ms | **0.15 ms** |
| `copy` GPU phase | 3.3 ms | 3.4 ms | **0.48 ms** |
| the mod's work per draw | 1.45 us | 1.08 us | 1.06 us |
| pose wait (idle before the headset's next frame) | 0.39 ms | 0.38 ms | 1.07 ms (up to 3.9) |

The frame now reaches the headset's 90 Hz limit part of the time (the pose wait is where it idles). The bracketed `main-other` draws
(`each draw bracketed`) use 0.11 ms of that phase's 6.17 ms, so the rest of the phase is the gaps the CPU leaves between them: the
GPU is mostly waiting for the CPU, not the other way round, and a higher resolution should cost little until the GPU saturates.

**3840x2160 with SharedTransport=1** (55 seconds of probe lines; the game's `Reset` to the new size went through, the shared texture
was rebuilt at 3840x2160, no failures in the log): 8.3 million pixels, 3.6 times the 2160x1071 run, and the frame cost only 1.1 ms more.

| | 2160x1071 | 3840x2160 |
|---|---|---|
| frame | 12.62 ms (80 fps) | 13.70 ms (73 fps, 62-86) |
| wait for the GPU (fence) | 0.15 ms | 0.36 ms |
| `copy` GPU phase | 0.48 ms | 0.71 ms |
| `scene-main` / `main-other` phases | 2.18 / 6.17 ms | 2.70 / 6.06 ms |
| the mod's time per draw | 1.06 us | 1.69 us |
| the bracketed `main-other` draws | 0.11 ms | 0.41 ms (the tone-mapping/colour pass, `ps 654`, is 0.32 ms of it) |

The fence wait is the tail the GPU still has when the CPU has issued the last draw: 0.36 ms means the GPU is keeping up, so at this
size the frame is still limited by the CPU (game 6.6 ms, hooks 5.6 ms of which the mod 4.7). The mod's per-draw time rose with the
resolution (unexplained: more time inside device calls when the GPU has more queued is the likely cause). The GPU's own limit has not
been reached yet; the next steps are to raise the render size further (SteamVR's own size for this headset is 4936x2740) and see
where the fence wait starts to grow.

**4936x2740 with SharedTransport=1** (60 seconds of probe lines, the size SteamVR itself asks for on an Index; `Reset -> backbuffer 4936x2740`,
shared texture rebuilt, no failures): 13.5 million pixels, 5.8 times the 2160x1071 run. Frame 16.51 ms (60.7 fps, 54-69).

| | 2160x1071 | 3840x2160 | 4936x2740 |
|---|---|---|---|
| pixels | 2.3 M | 8.3 M | 13.5 M |
| frame | 12.62 ms | 13.70 ms | 16.51 ms |
| the mod's time per draw | 1.06 us | 1.69 us | 2.63 us |
| fence wait (GPU tail) | 0.15 ms | 0.36 ms | 0.59 ms |
| pose wait (slack) | 1.07 ms | 0.62 ms | 0.44 ms |
| `copy` GPU phase | 0.48 ms | 0.71 ms | 0.93 ms |
| `scene-main` GPU phase | 2.18 ms | 2.70 ms | 3.88 ms |
| the colour/tone-mapping full-screen pass (bracketed) | 0.08 ms | 0.32 ms | 0.57 ms |

Reading: up to roughly 6 million pixels the frame is limited by the CPU (about 12.6 ms) and extra pixels are free; above that the GPU is the
limit. Between the two larger sizes the frame grows by about 0.54 ms per million pixels, which puts 90 fps (11.1 ms) at roughly
3 million pixels without any saving in the pixel work, and the CPU-side floor is 12.6 ms anyway. The mod's own time per draw grows with
the resolution although it does no pixel work: the likely reason is that device calls wait for the GPU when its queue is full, so
those milliseconds are the GPU pacing the CPU, not mod work. In the headset 4936x2740 still felt smooth (60 fps held by SteamVR's
reprojection). What would lower the GPU side now: fewer shaded pixels (the 25 percent of the flat image the lens never shows, tiles at
lower density), and trimming the fixed per-frame GPU work (shadow maps 1.7 ms, the double geometry pass).

## 16. A back buffer wider than 3:1 (the game window clamped to a two-monitor desktop)

> **Since the Resolution rework** `StartAtSavedSize`, `StartWidth` and `StartHeight` are no longer keys.  VR always uses the start spoof described
> here, and the size it starts at is `[VR] Resolution` (the same size that is offered as the extra video mode).  `video_settings.cpp` keeps
> that size in BlurVR.ini and the flat game's size in settings.xml (see the end of section 5, "Custom resolution"); the text below is the history
> of how the clamped-window problem was found.

The game saves its video mode in `%APPDATA%\bizarre creations\blur\settings.xml` (`<shared_graphical_settings width= height= windowed=>`) and,
windowed, asks Windows for a window of that size.  Windows clamps a window to the virtual desktop, and the game then creates its device at the
clamped client size: with two 1920x1080 monitors side by side a saved 4936x2740 started as 3854x1071 and a saved 3840x2160 as 3840x1071,
about 3.6:1.  Picking a size inside the running game (a `Reset` with explicit sizes) is not clamped, so the same sizes worked from the options
menu; only starting the game with a big size saved went wrong.

What went wrong: the rear-view-mirror test (`ClassifyDraw`, "wide, short render-target texture, aspect between 3 and 12") also matched the
game's scene texture, which has the back buffer's own 3.6:1 shape.  The final scene composite was then diverted as "the mirror picture" into the
per-eye HUD layers instead of being drawn onto the back buffer: the menu showed one mono wide picture split over the two eyes (wide double
vision), strips holding two shrunken copies of the side-by-side picture, and a back buffer that was never written so earlier frames showed
through (a void with trails).  `HasBackBufferShape` (render_state.cpp) now keeps back-buffer-shaped textures out of both mirror tests
(`ClassifyDraw`, `IsMirrorScenePass`); a real mirror picture (864x160, 5.4:1) is unaffected.  How it was found: `[Debug] KeyDump` (numpad 8) and
`AutoDumpSeconds` (frame_dump.cpp) save the picture sent to the headset as `BlurVR_dump_*.bmp`, which showed the broken picture at the
clamped start and a good one after a runtime change to 4936x2740.  Other places that assume a shape: the HUD panel keeps the back buffer's
aspect (a 3.6:1 panel is wide and thin), and the 3.6:1 start has its pixels spread unevenly over the eyes (1927x1071 per eye for a field of
view that is about square).  Starting with a size that fits the desktop, or picking the size in the options after the start, avoids both.

**The same mistake after a Reset from the clamped start.**  Fixing the back-buffer shape was not enough: after a switch to 1920x1080 or 3840x2160
inside the game the menu showed the whole side-by-side picture in a small window in each eye, in a void.  The draw capture (`cap f...` lines,
armed by `FrameDumpPoll` at every dump) showed the final composite (`ps=505`) and the glow add (`ps=423`) classified as plan 2 where the
clamped start had them as plan 0, with the same textures bound.  The game keeps (or rebuilds at the old size) its glow target after a Reset:
it stayed 960x267 (the clamped start's quarter size) in a 1920x1080 game, still bound to a sampler when the composite ran, and a 3.6:1 render
target passed the mirror test again.  Two changes: no mirror test at all in the showroom (the menu has no rear-view mirror), and
`HasBackBufferShape` remembers every back buffer shape of the run (up to 8), so a buffer left over from an earlier shape is not a mirror.
Lesson: shape alone is a weak test for "the mirror picture"; if it misfires again in a race, identify the mirror target by what is drawn into
it (the rear camera's scene draws) instead.  `[Debug] AutoDumpSeconds=N` now dumps the picture and the layers N and 3N seconds after every
back buffer size change and logs the draws of the next two frames, which is the quickest way to see where a draw was sent.

**Starting at the saved size (`[Render] StartAtSavedSize`, default 1): what the game believes decides everything.**  With the mirror test fixed,
a clamped start still showed a stretched intro video and a cut-off UI, because the game saves its video mode in settings.xml, Windows shrinks the
window it asks for, and the game reads the shrunken window (the log: `CreateDevice request: back buffer 3854x1071, windowed 1, window client
3854x1071, saved video mode 4936x2740`) and builds its device **and its own render targets** from that.  Three attempts and what they showed:
(1) a back buffer of the saved size for a 0x0 request did nothing (the game passes a size); (2) raising the window's size limit
(`WM_GETMINMAXINFO` through a thread hook) did nothing: the hook saw `max track 3860x1100` and raised it, the window stayed 3854x1071 client; (3) forcing the
saved size as the back buffer although the game had passed the clamped one **broke the stereo**: the game's scene targets stayed 3854x1071 (and 3872x1088)
while the back buffer was 4936x2740, so no target had the back buffer's shape, `IsMainPass` never matched and the scene was drawn once, mono, full
width (log: `scene draws 148` instead of about 540, the capture lines show `rt=3854x1071` draws next to `rt=4936x2740(bb)`).  Lesson: **the game builds its
targets from the size it passes, never from the device, so the device must be the size the game passes.**  The current approach (`window_spoof.cpp`)
therefore changes what the game *reads*: it patches the user32 imports of the executable in DllMain (all modules again in `Direct3DCreate9`) and
answers `GetClientRect` of the game's big top-level window with the saved size while the window is under 90 percent of it in either direction, until
CreateDevice has returned (`WindowSpoofDeviceCreated`).  The game then passes the saved size itself, which is the state a Reset from the options
menu gives.  The same file logs the first calls of `CreateWindowExA/W`, `AdjustWindowRect(Ex)`, `GetWindowRect`, `SetWindowPos` and `GetClientRect`
before the device exists (`GetClientRect(...) -> WxH (answered with the saved size)`), so if the game finds its size another way (the window
messages, `GetWindowRect`) the log shows which call it was.  `display_modes.cpp` logs the `CreateDevice request:` line and only fills in a 0x0 size.

**The game resets itself to the real window 0.9 s after the start.**  The first spoofed run showed the start working (`GetClientRect(...) -> 3854x1071
(answered with the saved size)`, `CreateDevice -> bb 4936x2740`) and then, 0.9 s later and with no call from the mod, `Reset -> backbuffer 3854x1071` with the
intro movies opened again: the game compares its window with its device (or answers the window's first WM_SIZE) and resets to the real client
size, which put the 3.6:1 picture, the stretched video and a UI strip back.  The window facts from the log: the game calls
`AdjustWindowRect(4936x2740, style 16ca0000)` and `CreateWindowExA(... 4942x2769)`; `GetWindowRect` afterwards says 3860x1100 (the system's maximum).
So the spoof now stays on: (1) `GetClientRect` of the game's top-level window answers with the size the game last asked for
(`WindowSpoofIntendedSize`, set from the explicit size of CreateDevice and of every windowed Reset, starting with the saved mode) while the real
client is under 90 percent of it, (2) the game window's procedure is wrapped (`GameProcHook`, installed from the `CreateWindowEx` hook) and its `WM_SIZE`
carries that size too.  When the player picks another size in the options, the Reset's own size becomes the new "asked for" size, so a later
window message or query cannot undo the choice.  Log lines to look for: `game window ...: window procedure wrapped`, `WM_SIZE WxH (passed on ...)`,
`GetClientRect(...) ... [after the device]`, `window size spoof: the game's size is now WxH`, and the absence of a second `Reset -> backbuffer` at the start.

**The game saves the clamped size, so the saved mode cannot be trusted (`[Render] StartWidth/StartHeight`).**  The run after the persistent-spoof build still
started at 3854x1071 and the log said why: `window size spoof: saved video mode read (3854x1071)`.  `settings.xml` had been rewritten at 00:09:55, the second the
game reset itself to the clamped window in the run before, and the game writes the size it was last reset to.  So one clamped start poisons the file and the
next start asks Windows for exactly the clamped size (`AdjustWindowRect(3854x1071)`, `CreateWindowExA ... 3860x1100`), which gives the spoof nothing to answer
with.  `StartWidth`/`StartHeight` in BlurVR.ini (default 0 = use settings.xml) name the size to start at instead; the spoof answers with it whenever the real
window is under 90 percent of it.  Starting the VR session earlier would not help with the superwide intro video: the video is drawn into the back buffer, whose
shape is the clamped window's, in the headset as much as on the desktop.

**Result.**  With `StartWidth=3840`/`StartHeight=2160` the start was confirmed good in the headset: log `CreateDevice -> bb 3840x2160`, the intro video and the menu
right, one later `Reset -> backbuffer 3840x2160` (the game answering the window's first `WM_SIZE`, which the wrapper turned into the size it asked for, so the
size did not change).  The timed dump is left off (`AutoDumpSeconds=0`; it also arms the draw capture, which fills the log); numpad 8 still saves one set on demand.

## 17. Hidden-area cull (tried, no gain, removed from the build)

**The code is kept in `experiments/hidden_area/` (not compiled).  To try it again: copy the two files into `src/`, add the ini keys `[Render] HiddenAreaCull` / `HiddenAreaMargin` (config.h/.cpp), call `HiddenAreaNoteDepthClear(z)` from `H_Clear` for a depth clear of a main-sized target and `HiddenAreaBeforeMainDraw(device)` at the top of the plan-1 branch of `RunDraw` (`IsMainPass()`).  The text below describes it as it was.**

**Idea.**  SteamVR's hidden-area mesh covers 25 percent of each eye's flat image (corners and the nose side); the lens never shows it, so shading it is wasted
GPU work.  Estimate from the measurements of section 15: at most 25 percent of the pixel-bound GPU work, about 1 ms at 3840x2160 (where the CPU floor of
12.6 ms limits the gain to roughly 73 to 79 fps) and about 1.8 ms at 4936x2740 (60 to 68 fps).  **These are estimates; the prototype has not been
measured.**  Nested lower-density tiles were rejected: they save more on paper but double the main-pass draws again and the frame is CPU-limited.

**Mechanism (`hidden_area.cpp`).**  Before the first main-pass scene draw after each depth clear of the main-sized target (`H_Clear` notes the clear,
`RunDraw` calls `HiddenAreaBeforeMainDraw` in the plan-1 branch; also once per frame whatever the game does), the hidden area is drawn into the depth buffer only
(pre-transformed rectangles, depth write on, colour write off, test always) with the depth that fails every later test of the pass.  The game clears to 0 and
tests with a reversed depth (the projection maps near to 1: `c + d/z`), so the stamp is 1.0 (`stampZ = clearZ < 0.5 ? 1 : 0`, so a game that cleared to 1 would get 0).  Geometry
shaders then do not run under the stamp (early depth test).  Depth, not stencil: the stamp costs nothing per draw (a stencil test would need render states on every
draw, and the game's own use of stencil in the scene is unknown), and nothing about the game's draws changes.

**What it must not do.**  An earlier version of this idea (a stamp from the hidden-area mesh) turned the cars black; the likely cause is state left behind by the stamp.
Now the states it touches (z enable/write/func, colour write mask, alpha test, stencil enable, cull, scissor test, fill mode, depth bias) and the vertex/pixel shader, stream 0,
declaration/FVF are read first and put back after, with the original setters (so the mod's own tracking of the game's shaders and states stays in step) and
after `DeviceRestoreAll`.  The mask is built once from the mesh as a 64x64 grid per eye: a cell counts as hidden only if its centre is inside the mesh as given **and**
inside it turned upside down (the convention for v is not certain and the mesh is not exactly symmetric), then eroded by `HiddenAreaMargin` cells so filters in the
later passes and the lens's colour fringes never reach into the stamp.  Simulated on the dumped mesh: mesh 25.0 percent, hidden both ways 23.5, stamped 23.5 / 18.1 / 13.2 percent of each
eye image for margin 0 / 1 (default) / 2.  Rectangles are merged vertically, a few dozen per eye.

**Known risks, to judge in the headset.**  (1) The stamped corners keep the cleared G-buffer; the auto-exposure chain (ps 513-516, downsample of the whole picture)
and anything that samples near the boundary (ambient occlusion, bloom) sees dark corners: a change of brightness would show in the frame dump compared with the flag off.
(2) Draws that test no depth (sky, full-screen passes, the lights) are not culled; only the geometry pass gains, so the saving is below the 25 percent of the estimate.
(3) The depth convention is inferred; the first stamps are logged (`hidden-area cull: stamp #1 ... depth cleared to .. -> stamped .., game z/func ..`).

**How to test.**  `HiddenAreaCull=1` in BlurVR.ini.  On the grid before a race starts (the picture is steady): numpad 8 saves the picture and layers (compare with a run with
the flag off: cars, brightness, the corners of each eye), numpad 7 prints the GPU phases for a while (`scene-main` and the frame time are what should drop).

**Measured (3840x2160, race, the same stretch of track, probe on, 17 one-second samples with the flag off and 30 with it on, matched by issued draws): no gain.**
It works as designed: the log shows `stamp #1 ... depth cleared to 0.00 -> stamped 1.0 ... game z 1/1 func 7` (reversed depth, GREATEREQUAL), 3 stamps per frame (the game clears the
depth of the main target three times a frame), 742 of 4096 cells = 18.1 percent of each eye stamped, 58 rectangles; the frame dump shows black corners with a stepped edge well
outside the lens view, the cars and the HUD intact (no state leak), no visible brightness change between the two pictures.  Timings (off / on): frame 14.08 / 14.37 ms (71.1 / 69.7 fps); scene-main
3.28 / 3.14 ms (draw-count matched buckets: 3.22 / 2.83, 3.31 / 3.31, 3.44 / 3.00, about -0.25 ms); `offscreen` 1.49 / 1.86 ms (the stamps' own GPU time lands in the phase that is
active when they run, about +0.37 ms for three); frame time at equal draw counts 14.06 / 14.13, 14.04 / 14.19, 14.58 / 14.55 ms, i.e. unchanged.  The saving in scene-main is what 18 percent of the
pixel-bound part predicts, and the stamps cost more than that.
**Why it cannot help here: the frame is limited by the CPU.**  The CPU parts of the 13.7 ms frame add up to the frame (game and other 6.74, draw hooks 5.53 of which the mod 4.59 = 1.39 us per
game draw and the driver 0.95, upload wait 0.40, present 0.40, pose wait 0.40, submit 0.18), and the `main-other` phase is 5.85 ms of GPU time for 7 draws whose own busy time is about 0.4 ms (bracketed
earlier): the GPU spends most of the frame waiting for the CPU to issue the next draw.  Cutting GPU pixel work (this cull, or tiles) does not move the frame at 3840x2160; only CPU time does.
At 4936x2740 (13.5 MP) the GPU is the limit, but there the same estimate gives about 0.5 ms saved against 0.2-0.6 ms of stamps.  `HiddenAreaCull` therefore stays off; the code stays as an experiment.
Cheaper variants if it is ever revisited: stamp once per frame (the two later stamps cover far fewer draws), or stamp only the nose side and the outer corners.

## 18. Where the CPU time goes: the detail lines of the probe

The frame at 3840x2160 is limited by the CPU (section 17: the CPU parts add up to the frame; the GPU waits).  The mod's own part is about 4.6 ms of it
(1.39 us per game draw, 6167 issued draws), and the host harness explains only about 1.2 ms of that as compute, so the rest has to be something the harness cannot
see: the real cost of device calls, the hooks on the game's other calls, cache effects, the probe itself.  The probe (numpad 7, `[Perf] Enabled=1`) therefore prints five more
lines every second after the two it always had:

* `perf: draws by plan`: per frame, draws / RunDraw ms / of that the game's own draw call for plan 0 (untouched: shadow maps, post chain, lights), plan 1 (scene draws
  duplicated per eye), plan 2 (UI) and unclassified.  Says where the per-draw cost lives.
* `perf: pieces of RunDraw`: classify, eye constants (`ApplyEyeConstants`), eye scissor, shadow-decal/light-volume helpers, `MotionBlurPerEye`, `DrawLightsPerEye`, the
  `DeviceRestoreAll` at the top.
* `perf: device calls the mod makes itself`: vertex constants, scissor rect, render state: calls per frame and the time inside the call (this includes any wait the call
  does when the GPU's queue is full).
* `perf: the game's other device calls through our hooks`: SetRenderState, SetTexture, SetVSConst, SetPSConst, SetVS, SetPS, SetRenderTarget, SetViewport, SetScissor, Clear:
  calls per frame, ms in the hook and (in brackets) the part that is the real call; hook minus real call is what our bookkeeping costs.  These are in the "game and other" figure of the first line.
* `perf: probe cost`: clock reads per frame, the cost of one, and the total: every number above contains it (a scope reads the clock two to four times), subtract it.

Reading it: if "device calls the mod makes itself" is most of the mod's time the lever is fewer calls (or calls that wait less); if plan 1's non-driver time dominates it is the compute
per duplicated draw (candidates: decompose the projection only when the view-projection block changes, cache the main-pass / half-pass / shape tests per render target change);
if the hooks' own time is large the lever is slimmer bookkeeping in them.

**Measured (3840x2160, race, 43 one-second blocks, probe on; medians): the UI draws are 3.35 ms of the mod's 5.4 ms.**  Frame 15.22 ms (65.7 fps, the probe's own clock reads cost
0.84 ms of it); draw hooks 6.30 ms = driver 0.92 + mod 5.42.  By plan (draws per frame, RunDraw ms, of which the game's own draw call): plan 0 untouched 1011, 1.17 (driver 0.33);
plan 1 scene 2046, 1.82 (0.59); **plan 2 UI 43, 3.35 (0.03)**: about 78 us per UI draw against 0.63 us per duplicated scene draw.  Pieces of the scene path: classify 0.19, eye constants
0.69 (4100 calls), eye scissor 0.15; the mod's own device calls total 0.42 ms (4600 vertex-constant writes at 74 ns, 1900 scissor writes at 47 ns); the game's other calls through our hooks
1.64 ms in total of which the real calls 1.10, i.e. our bookkeeping in them is 0.5 ms or less including the probe's own clock reads.  So the per-eye machinery that sections 14 optimised is no longer the problem.
**The cause in the UI path is the occlusion query of `HudDrawCoversScreen`**: in a race every UI draw whose first texture is 32x32 or smaller (the UI's 16x16 fill texture, used by bars and frames) is a
"candidate" for being a dim/fill piece, so it is drawn once into a scratch target with an occlusion query and the CPU spins on `GetData(FLUSH)` for the result, a round trip to the GPU of
about a quarter of a millisecond each (no query timed out in the log).  A dozen of those a frame is the 3 ms.
**The cut (`[Hud] CoverageEvery`, default 8; 1 = the old behaviour).**  `CoverageOfCandidate` (draw_dispatch.cpp) keeps the last measured coverage per candidate (by its position in the
frame, its shaders and its first texture) and reuses it for up to 8 frames; the refresh is staggered by position, so no frame pays for all of them.  The decision is `coverage >= threshold`
with the current threshold, as before.  The cost: a fill that appears suddenly (results screen, pause dim) is noticed up to 8 frames (90 ms) late, and a fill that disappears is dropped for as
long.  The probe now also times the query (`UI coverage query`) and the UI draw into the layer (`UI draw into the layer`), which overlap, to show what is left of the 3.35 ms.
Candidates after that, from the same profile: the layer switches in `DrawHudToTexture` (two render-target and two depth-stencil switches per UI draw; they could be kept across consecutive UI
draws with the lazy-state scheme of `device_state.cpp`), the per-eye constant writes (0.7 ms, two calls per scene draw, hard to avoid without recording draws), and the probe's own cost.

**First result of CoverageEvery=8 (race, probe on, 43 blocks): small.**  UI draws 2.62 ms (was 3.35), frame 14.70 ms / 68.0 fps (was 15.22 / 65.7), draw hooks 5.5 ms; the new `UI coverage query` piece
shows **1 query per frame costing 2.46 ms** (range 2.0 to 2.9), and the UI draw into the layer itself only 41 calls = 0.10 ms.  So the cost is not per query but per *wait*: the first blocking
`GetData(FLUSH)` of a frame has to wait until the GPU has drained what is queued before it (about 2.5 ms in this CPU-limited frame), later ones in the same frame are cheap.  Spreading the same
queries over frames (1 a frame) keeps the wait in every frame, and the old 3.35 ms was one such wait plus about 40 cheap UI draws.  Hence: **no waiting at all in a steady race frame.**
Second version: each candidate position has its own occlusion query slot (`CoverageSlot`, hud.cpp); every `CoverageEvery` frames the draw runs into the scratch target with its slot's
query and nothing waits; the result is read with `GetData(..., 0)` on a later frame (`HudCoveragePoll`) and kept per position with the signature of the draw it belongs to (shaders, first texture;
a result for a draw that is no longer at that position is dropped); every frame uses the last result.  A position with no result yet is measured with the wait only when it matters (`needNow`: the
race-start window, menus, the first HUD draws of a frame) or the very first time, otherwise it counts as not covering until its result arrives.  Expected: about 2.4 ms less CPU per frame.

**Result of the asynchronous coverage (race, probe on, 58 blocks, medians), three runs side by side.**

| | every draw waits | CoverageEvery=8, waiting | asynchronous |
|---|---|---|---|
| frame / fps | 15.22 ms / 65.7 | 14.70 ms / 68.0 | **13.63 ms / 73.4** (62.9 to 82.2) |
| UI draws (plan 2) | 3.35 ms | 2.62 ms | **0.12 ms** |
| mod's time in the draw hooks | 5.42 ms | 4.61 ms | **1.92 ms** (0.73 us per game draw) |
| plan 1 / plan 0 | 1.82 / 1.17 ms | 1.87 / 0.97 | 1.52 / 1.06 |
| end-of-frame wait for the GPU (`upload`) | 0.4 ms | 0.47 | **2.63 ms** |
| game and other | 7.5 ms | 7.6 | 7.2 |

The CPU part of the mod fell by 3.5 ms but the frame only by 1.6: the CPU now finishes the frame about 2.6 ms before the GPU and waits for the shared-texture fence (the `upload` line), so **the frame is now
limited by the GPU, about 13.6 ms**.  GPU phases (wall time, medians): offscreen 2.27, scene-main 2.59, scene-half 0.39, lights 0.28, **main-other 6.05**, post-low 0.76, hud 0.25, copy 0.73.  `main-other` is 7 draws whose
own GPU time is 0.39 ms; the other 5.4 ms is time the phase spends between them.  It was 5.1 ms at 2160x1071 and is 5.8 to 6.0 ms at 3840x2160: it does not depend on the resolution, and it stays with the CPU no longer the
bottleneck, so it is not the CPU's gap and not pixel work.  Suspect: the game's draws into the back buffer (the shader-less blits at the end of `main-other`, 4.4 a frame) wait for the previous frame's `Present`: a windowed
Present copies the back buffer into the desktop compositor's surface, and the first write to the back buffer of the next frame waits for that copy and the compositor's schedule.  Two things to find out, both in this build:
(1) the probe now prints, for every bracketed `main-other` draw, the interval after it (`the interval after it x ms`), which shows which draw is followed by the wait; (2) `[Render] DesktopPresentEvery=N` makes the real
Present of a race frame happen only every N frames (the others return success without calling it; the VR picture is submitted before it either way), the window on the desktop then updates less often.  If the hypothesis is right,
N=4 should make `main-other` fall by several ms.

**DesktopPresentEvery 1 against 4 (race lap, probe on, 59 and 60 blocks, medians; different laps, so about plus or minus 1 ms of the frame is noise).**  Frame 14.60 -> 13.77 ms (68.5 -> 72.7 fps, range 60-78 -> 62-84);
`present` on the CPU 0.42 -> 0.12 ms; the end-of-frame wait for the GPU (`upload`) 2.65 -> 2.51 ms; hooks, game and other unchanged (3.1 / 7.7 -> 3.0 / 7.5).  The GPU phase lines of the N=4 run
(total 8.2 ms, `main-other` 0.51 instead of 6.18) must **not** be believed: the probe measured only 17-19 GPU frames a second against 66-72 frames, because a frame whose timestamp queries are not back is dropped
silently (`ReadFrames` leaves the frame out without counting it) and with three of four Presents skipped the command buffer is flushed less often, so only some frames (probably the ones ending in a real
Present) were complete.  In the N=1 run the `main-other` detail shows the 4.3 shader-less draws per frame followed by an interval of 6.1 ms, and the 0.33 ms colour pass: the wait sits behind the
shader-less blits, which is where the game writes the back buffer.  What the two runs do show: skipping Present is worth about 0.3 ms of CPU and at most about 0.8 ms of frame, far less than the 5.4 ms that
`main-other` seemed to hold; so either the wait is not (only) the Present or the GPU is not what limits the frame.  Next tests (no code): N=1000 (never present after the first frame) on the grid with the countdown
(a still picture: the same scene for both settings), and the same at 2160x1200 to see where the frame sits when pixels do not matter.  Another way to see if the GPU limits: SteamVR's frame timing graph (GPU ms).

**Status at the end of this round (3840x2160, SharedTransport=1, CoverageEvery=8, DesktopPresentEvery=4; probe-on numbers, laps differ by about 1 ms).**
Frame 17.2 ms at the start of the performance work (2160x1071, 58 fps, copy through the CPU) -> 15.2 (per-draw trims, lazy device state) -> 12.6 at 2160x1071 and 13.7 at 3840x2160 (shared texture) ->
**about 13.6 to 13.8 ms (72-73 fps) after the UI coverage query stopped waiting**; the probe itself costs about 0.7 ms of that, so the real figure is a little better (not measured).  The mod's own CPU time
in the draw hooks went 5.4 -> 1.9 ms (0.73 us per game draw); the frame is now limited by the GPU: the CPU finishes about 2.5 ms before it and waits on the shared-texture fence.  Where the GPU time sits
(wall time, medians): shadow maps and other off-screen 2.3, scene 2.6-3.1, `main-other` 6.0 of which only 0.4 is draws (the rest is an unexplained wait that sits behind the shader-less blits into the back
buffer and does not depend on the resolution), lights/glow/post about 1.5, copy 0.7.  Tried and dropped: hidden-area depth cull (section 17, no gain), skipping desktop Presents (small gain, kept as an option, section 18).
Open questions / next ideas, in the order I would try them: (1) what the 5.4 ms behind the shader-less blits is (never-present test, SteamVR's GPU frame timing, the game's anti-aliasing setting, bracketing the non-draw calls
`StretchRect`/`Clear`/`SetRenderTarget` with GPU stamps); (2) the probe drops frames whose timestamp queries are not back without saying so (`ReadFrames`): count them before trusting a GPU line when
Presents are skipped; (3) keep the layer render target bound across consecutive UI draws (small now: the UI draws cost 0.12 ms); (4) the per-eye constant writes (0.7 ms) need a recorded command stream to avoid.

## 19. Release 1.1: the diagnostics that stay, the ones that were removed

**Stay** (all off by default; nothing runs until asked): logging (`[General] Log=1`; `Log` is 0 by default and the release ini has no log option; what is logged is start-up, device and VR set-up,
mode changes and errors, nothing per frame), the timing probe (`[Perf] Enabled=1` or Num7: the frame / CPU / GPU-phase lines of section 13), the frame dump (Num8, at most 12 sets a run), the headset report
line (`[Foveation] Report`, only runs with the log on).

**Removed** (they were investigation tools): the draw capture (Num0) and the flow / cap / ui-glow / intro-frame / post-pass-shader logs of `RunDraw`; the HUD layer statistics (`DiagLayerStats`, which also
did a read-back from the GPU every 250-700 ms in the menus); the menu-frame sampling and once-a-second menu log of `VrSubmitFrame`, the menu-placement log and the heartbeat line; the watchdog thread
(one log line a second for five minutes); the logging of window calls in `window_spoof.cpp` (only `GetClientRect` and the window creation / procedure hooks remain); the timed dump (`AutoDumpSeconds`); the detail lines
of section 18 (per plan, per piece of `RunDraw`, per hook, the mod's own device calls, the probe's own cost) and the per-frame gap lines of `main-other`.  The hidden-area cull of section 17 is in `experiments/hidden_area/`.
`experiments/diagnostics-removed-in-1.1.patch` is the whole cleanup as a diff (`git apply -R` on the tree it was made from brings the diagnostics back); sections 16 to 18 above describe how they were used.

**Release.**  Version string `BLURVR_VERSION` in core.h (1.1), also in the log's first line.  `release/BlurVR.ini` is the small ini that goes into the zip (resolution, start size, shared transport);
every other option is documented in README.md.  `tools/make_release.py` builds `dist/BlurVR-1.1.zip` from the tree (d3d9.dll, openvr_api.dll, the release ini, README.md, install/uninstall, the OpenVR
licence; no source code: it is in the repository, and `tools/check_release_zip.py` fails on any source file in the zip).  Checks run for the cleanup: the host harness (`tools/hotpath_check.py`) gives the same hashes in all eight
configurations as before it (the draw commands are unchanged), the DLL builds without warnings that matter, and a start, a menu and a race lap were not repeated after the cleanup (the next headset test of the
release build is the check that matters).

## 20. The window on the desktop: one eye instead of the side-by-side picture (`[VR] DesktopView`, default `off` since 1.5, `left` before)

The game's window shows the back buffer, which in a race is the side-by-side picture, stretched to whatever shape the window has (and the window is often clamped to the desktop, section 16): two squashed
eyes.  `VrShowEyeOnDesktop` (vr_runtime.cpp), called from `H_Present` just before the real Present of a race frame that is really going to be presented (not on the frames `DesktopPresentEvery` skips), paints one
eye's view over the back buffer with a single `StretchRect`, after the headset has its copy: `VrSubmitFrame` copied the finished picture (HUD layers included) into the shared texture, or into the staging surface
when SharedTransport is off, and waited for it, and it remembers that copy as `g_vr.desktopSource`.  The blit reads that copy, not the back buffer (a surface cannot be both source and destination), so the headset's
picture, the frame dump and the frame timing are not touched; the game does not use the old back buffer (the swap effect discards it) and draws the next frame over it.
The crop: the eye picture is flat, so the window shows a rectangle of tangent space around the view straight ahead (`-left/(right-left)` of the width, `up/(up+down)` of the height), as large as fits inside the
eye picture, with the window's aspect ratio (the real client size of the focus window, read with this module's own `GetClientRect`, not the spoofed one of section 16).  Written into the whole back buffer (see below for the window-sized version); Present then
stretches the back buffer to the window, and the two stretches cancel, so nothing looks squashed.  For a 16:9 window and the Index's tangents this is about 93 by 61 degrees around straight ahead (half width = the nearest side of the tangent box, 1.047, the nose side of the left eye).
Cost: one blit of the whole back buffer (about 0.1 to 0.2 ms of GPU time at 3840x2160) on each presented frame.  Not applied when the back buffer is multisampled or when the window is minimised.  Menus and loading
screens (the floating-screen path) are always presented whole.  Not measured in the headset yet.

**At the window's own size.**  Painting and presenting the whole 3840x2160 back buffer for a window of 1920x1080 (or 3854x1071) is more work than the mirror needs: a 4K blit, then a Present that reads 8 million pixels and
stretches them down.  The probe had shown a real Present to be worth about 1 ms of frame time (skipping three of four gave 0.8 ms).  Now `VrShowEyeOnDesktop` writes the crop at the window's real client size (at most
the back buffer: scaled down by the same factor in both directions otherwise) into the **top-left corner** of the back buffer and returns that rectangle; `H_Present` hands it to the real Present as its source rectangle, so
the runtime and the desktop compositor handle only those pixels, 1:1.  A Present source rectangle is only guaranteed with the COPY swap effect, so `AdjustPresentParams` (CreateDevice and every Reset) turns a
windowed, non-multisampled request into COPY with one back buffer (`swap effect 1 with 0 back buffers -> copy with one` in the log; `CreateDevice request:` now also shows the game's swap effect, multisample type and
back buffer count) and sets `g_desktopRectUsable`.  If the driver still refuses the rectangle (`Present with a source rectangle failed ...`, once), `g_desktopRectFailed` turns the feature back to painting and presenting the
whole back buffer.  Menus and everything that is not a race frame present the whole back buffer with no rectangle.  Checked with `tools/ex_proxy_check.cpp` (an Ex device through the proxy: creation with COPY, Present with a source
rectangle, Reset); this runtime also accepts the rectangle with DISCARD.  The cost for the headset: the blit and the copy are queued after the headset's copy and the fence, so the CPU never waits for them and the
compositor does not depend on them; the GPU does them before the next frame, now about a quarter of the pixels they were (a 1920x1080 window: 2 million instead of 8.3 million), and with `DesktopPresentEvery=4` on one frame in four.
Not measured in the headset yet: compare the frame time with `DesktopView=0` (old whole-picture present) and `DesktopView=1`.

**`DesktopView=off`** (`desktop_message.cpp`, added in 1.5 and the default since then; `g_cfg.desktopView == kDesktopViewOff` = 3).  The window is the game's, so it has to exist (it is what the keyboard focus belongs to, and the game reads its keyboard through DirectInput and `GetKeyState`; how it sets the cooperative level was not checked, so the window is
not hidden or minimised, which could cost the keyboard), but nothing about it needs the game's picture.  While the headset runs (`g_vr.active`) and a Present can take a source rectangle (`g_desktopRectUsable`, the windowed,
non-multisampled COPY case of the paragraph above), `H_Present` skips the game's Present except when `DesktopMessageDue` says so (the first one, a change of the window's client size, then one every 500 ms), and then paints the message with
`DesktopMessagePaint` instead of the eye crop: a picture the size of the window (at most the back buffer) is built with GDI once per size (`DesktopMessageBuildPicture`: a dark gradient, the logo, "Playing in VR" in Segoe UI, centred
vertically as a group), uploaded through a system-memory surface and `UpdateSurface` into a default-pool surface in the back buffer's format (X8R8G8B8 or A8R8G8B8), and `StretchRect`ed 1:1 into the back buffer's corner, whose rectangle is Present's source.  The cost
is one 1:1 copy and one Present twice a second.  Menus and loading screens get the message as well, the headset shows the game as before.  A minimised window is not presented to.  The surface is released before every Reset (`H_Reset`).
**The logo** is the first icon of the game's executable (`DesktopMessageLoadLogo`: the first `RT_GROUP_ICON`, loaded at 32 by 32; the Steam build's is the green "b" of Blur on a dark square, and it is the only size it has), enlarged with a bilinear filter to 64-96 pixels (a fifth of the
window's height); the icon is drawn on black and on white to tell its colour from its coverage, so that it lays over the gradient whatever kind of icon it is.  A build without an icon shows the text alone.
**Fallbacks.** If the device is not a windowed COPY one when it is created or reset, if the back buffer is not a plain 32-bit one, if the surfaces cannot be made, or if the driver refuses Present's rectangle, the setting is changed to `left` for the rest of the run
and the log says why (`DesktopView=off: ...`).  `tools/desktop_message_check.py` checks the layout of the picture (centring, the logo's size, several window sizes; `--pictures folder` writes them as .bmp to look at) and, on a real Direct3D 9 device in a hidden window, that the corner of the back
buffer is the picture pixel for pixel, the rectangle for a resized window and for a window bigger than the back buffer, the Present timing and the release.  Run in the headset on a non-Steam build (section 25): the window showed the logo and the text and the menus took the keyboard; the frame time and the keyboard with the window unfocused were not measured.

## 21. The window fits the monitor (`[Render] FitDesktopWindow`, default 1)

At a 3840x2160 video mode the game asks for a window with a client area of that size; Windows lets it be as large as the whole virtual desktop (two 1080p monitors gave 3860x1100 in the proxy check), so the mirror
spilled over both monitors.  The window is only a mirror, and `VrShowEyeOnDesktop` (section 20) already crops one eye to the window's real client aspect and presents at the window's own size, so the fix is only to keep
the window itself small; no change to what the game renders.  Three places in `window_spoof.cpp`:
- `FitNewWindow` in `H_CreateWindowExA/W`: a top-level window with a caption whose requested size is bigger than the primary monitor's work area is created at the work area's size and corner.  Log: `the game's window
  (WxH requested) is made ...`.
- `GameProcHook`, `WM_WINDOWPOSCHANGING` (the wrapped window procedure sees every size change of the window, including the game's own `SetWindowPos` calls): `cx`/`cy` are cut to the work area of the monitor the window is on, unless the call has `SWP_NOSIZE`.  The game is not told, so its own idea of the window size
  (the spoofed `GetClientRect`/`WM_SIZE` of section 16) stays the intended one.
- Only while the game is windowed: `g_deviceWindowed` is the `Windowed` flag of the last `CreateDevice`/`Reset` (`WindowSpoofWindowed`, called from `LogCreateDeviceRequest` and `H_Reset`), and before the first device the
  `windowed` attribute of the saved video mode (`ReadSavedVideoSize`, third parameter).  A full-screen mode is never touched.
Checked with `tools/ex_proxy_check.cpp ... big` (a 5000x3000 `CreateWindow` and a 6000x4000 `SetWindowPos`): 1920x1032 (the work area) with the option on, 3860x1100 (Windows' own limit) with `FitDesktopWindow=0`;
an 800x600 window is left alone.  Not yet seen in the game.

## 22. GitHub: history, build and release

The repository on GitHub (https://github.com/TwoAyEm/BlurVR, branch `main`) starts as one squashed commit of the 1.1 tree; the development history before it is in the local branch `history` only (never pushed).
`.github/workflows/build.yml` runs on `windows-latest` for every push to `main`, every pull request and by hand: Python 3.12, `pip install ziglang==0.16.0` (the Zig version the DLL was built and tested with), `source\build.bat nodeploy`,
`tools/make_release.py`, `tools/check_release_zip.py` (the zip has the DLL, openvr_api.dll, openxr_loader.dll, the small ini, the README, the install scripts and the two licence files, no source; with a tag, the tag must be `v` + `BLURVR_VERSION`), and uploads
`BlurVR-<version>.zip` as the workflow artifact `BlurVR-zip`.  A push of a tag `v<version>` also creates the GitHub release with the zip attached (`gh release create` with the workflow's own token, no other secrets).
To release: bump `BLURVR_VERSION` in `src/core.h`, commit, `git tag v<version>`, `git push origin main v<version>`.  The Zig compiler's version matters for the DLL bytes only, not for behaviour as far as tested; change the pin
deliberately.

## 23. The camera's position and the pixel shaders' camera, per eye

Head tracking moved the geometry but not everything the game derives from its camera. The game hands the camera to shaders in
three more places than the matrices of section 3 (all read from the dev source, `mercury/library/graphics/display/source/device/d3d9/D3D9Device.cpp`
`SetViewShaderConstants3D` and `amax/game/Game/GameWorld/View/Main.cpp`):

* **`g_vViewPosition`**, the camera's world position (`m_CameraToWorld` translation, w = 0), in a vertex register (`CCV_DISPLAY_VIEW_POSITION`)
  and a pixel register (`CCP_DISPLAY_VIEW_POSITION`; the fog volumes get it as `CCP_RENDER_SETTINGS_4`). Used for the view vector of
  specular, environment reflections and fresnel (roads, water, puddles, car paint in `vehicle_common.h`, light flares), fog distance,
  billboards turned to the camera, and **the sky dome, which is built around it** (`vsBaseSkyDome.vsh`: `world = vertex + g_vViewPosition`).
  With the game's value the reflections and highlights stayed where the game camera saw them while the surfaces moved with the head,
  and each eye sat off-centre in a dome of finite size, so the sky had parallax when leaning. Also named `g_vCameraPosition` (aux flares,
  fake environment light).
* **`g_vViewPositionPS`**, the same position for pixel shaders (`CCP_VIEW_POSITION`).
* **`g_vViewMatrix0..3`** and **`g_vViewProjMatrix0..3`** in *pixel* registers (`CCP_VIEW_MATRIX..`, `CCP_VIEWPROJ_MATRIX..`; c61-c68 in the
  dev source), **one column per register** (`C_PSConstant(mView._11, _21, _31, _41)`). The road and ocean shaders write their view-space
  normals for the lighting pre-pass with them (`Road*.fx`, `Ocean*.fx`), and water projects its reflected point to the screen with the
  view-projection (`Special/Water.h`). The lights are moved to the eye (section 5), so mono normals made the road's lighting turn with the head.

What the mod does (`stereo_scene.cpp`, both only for plan-1 draws, the stereo-duplicated ones):

* Vertex shader (`ApplyEyeConstants`): `VertexShaderInfo::viewPositionReg` (CTAB name `g_vViewPosition` / `g_vCameraPosition`) gets
  `position + offset`, where `offset` is where the eye's camera is in world space relative to the game's: the eye origin in camera space
  is `-t * transpose(R)` for `C = [R 0; t 1]`, turned into world space with the transpose of the draw's own view rotation (`g_mView`,
  or `g_mViewProj * inverse(P)`). In the reflection pass that is the mirrored camera with the mirrored delta, the same pair the matrices
  get. It goes through the lazy vertex-constant path like the matrices. Not applied when the view rotation is not orthonormal.
* Pixel shader (`ApplyEyePixelConstants` / `RestoreEyePixelConstants`, from `RunDraw`'s eye loop): only the registers the shader's
  CTAB declares are written (`PixelShaderInfo::viewMatrixUsed / viewProjUsed`; the base register is the declared register minus its
  index). The view columns become those of `V * C` if the four registers in the game's copy form a rigid matrix, the view-projection
  columns those of `X * inverse(P) * C * P_eye` (the cached `clipRemap` of `GetRewrite`) if `X * inverse(P)` is rigid, and the positions get
  the eye offset. The game's values are put back once both eyes are drawn. Skipped in the reflection pass: these constants describe
  the main camera there, not the mirrored one being drawn.

Tested in the headset (Quest 2) only together with section 24, which was the visible fix; these changes were not judged on their own. What should change: highlights and reflections on the road and the cars stay on the surface when the
head moves, the sky shows no parallax when leaning, billboards (trees, crowds) turn towards the head, and the lighting on the road no longer
turns with the head. `tools/hotpath_check.py` gives the same hashes as before in all eight configurations (its synthetic shaders do
not declare these constants, so that checks only that nothing else changed); the offset formula was checked against `inverse(V * C)`
for random cameras, mirrored ones included (worst error 2e-10).

**Water, read from the game's shaders** (`cache/shaders-speed.pak` of the game folder: PAK2, zlib blocks, each block one compiled effect with its vs_3_0 / ps_3_0 and constant tables, readable with `tools/ctab.py` and `tools/dis.py` after inflating the blocks).
Which effects a level names is in its `Level.levelrender`: `usa_la_river` has `Water` and `WaterFlow`, `usa_la_longbeachdocks` `Ocean` and `OceanFakeReflect`, `uk_brighton` `Water` and `Ocean`.
`OceanFakeReflect` is screen-space: its pixel shader projects a reflected point with `g_vViewProjMatrix0/1/3` (c65, c66, c68) and takes the colour from the scene picture (`ColourBufferSampler`), with `g_vViewPosition` in the pixel registers;
`Ocean` samples only the environment cube map; `OceanReflect` is planar like `Water`.
The `Water` effects look the planar reflection up in the vertex shader (`g_ReflectionViewMatrix0/1/3` = c145-c147 into `oT4`, so the record rewrite of section 3 covers them) and their pixel shaders carry no camera constants.  The `WaterFlow` effect, which `usa_la_river`
names in its `Level.levelrender`, does no planar lookup at all: its pixel shader reflects the view vector (`P - g_vViewPosition`, from the vertex shader's c24) off the normal-mapped world-space normal and samples the environment cube map (`EnvironmentSampler`, s8), and takes refraction and
depth from the colour and depth buffers at the clip-position varying.  So there the head can only matter through the eye position in c24 and the screen-space samplers.  A report of the water's reflection sliding over the surface when the head turns on that level, and later of a dark band in the far water, was worked out from `Log=1` diagnostics and dumps of the reflection texture
(temporary code, since removed); "The planar reflection view", "The reflection is drawn level" and "The screen-space water" below say what they found.

## 24. The headset's vertical field of view was read upside down

**Symptom** (Quest 2, showroom and races): the periphery squashed as if under a hyperbolic transform, worse when looking sideways.

**Cause.** `GetProjectionRaw` gives the vertical tangents with +y pointing down: OpenVR's own `ComposeProjection` builds
`y_ndc = (2 tan - (bottom + top)) / (bottom - top)`, so the tangent `bottom` lands at the top of the picture. `bottom` is how far the
eye sees upwards and `-top` how far down. `SplitVerticalTangents` took `-top` as up. On the Vive and the Index up and down are nearly
equal and the swap did not show; the Quest 2 gives `top -1.428, bottom 0.966`, i.e. 44 degrees up and 55 down, so every eye was rendered
for 55 up / 44 down and the compositor showed it as 44 / 55. That is an affine error in tangent space (scale and offset in y), which makes
straight lines bend and the periphery stretch or squeeze as the head turns.

**Fix.** `up = bottom`, `down = -top` when `top < bottom`. Everything that reads `g_vr.eyeBox` (eye projections, mono bounds, HUD panel,
streaks, motion blur, lights, mirror, photo viewer, desktop view) follows. Confirmed in the headset (Quest 2): the periphery is right with
the fix. For comparing, the old reading was kept behind `[Tracking] OldVerticalTangents` and a live switch on Num9 (`[Debug] KeyVerticalTangents`); both
were removed once the fix had been confirmed on a Quest and with SteamVR, and `SplitVerticalTangents` only has the fixed reading now. The tangent log line
also prints the raw `top`/`bottom`.

**Ruled out on the way, kept.** (1) Canted displays: each eye's pose now comes from `GetEyeToHeadTransform` (`[Tracking] EyeTransform`,
default 1; rotation included, translation scaled by IpdScale and PositionScale, read every frame so the IPD can change while playing; falls
back to half the IPD along x if the transform is not rigid or the eye is over 10 cm from the head). The function returns a 48-byte struct,
which on 32-bit x86 comes back through a hidden first-argument pointer, so it is called with that pointer spelled out (checked against an
MSVC-built stub). The Quest 2 has no rotation (`eye 0/1 to head: offset -/+30.7 mm, yaw 0.00 pitch 0.00 roll 0.00`), so this changes
nothing there. (2) An off-centre game frustum: `camera2/source/Feed.cpp` can shift the view extents, but in a single view the offset is zero
(`CameraSystem::ScreenOffset`), and the `main projection:` log line now prints the projection's off-centre terms (`off-centre x 0.0000 y 0.0000`).

## 25. Culling with a frustum around the head (VR only; `[Culling] MarginDeg`, `PullBack`)

**Why.**  The game culls with the frustum of the camera it gives its render views, on the CPU, before the first Direct3D call.  That frustum is the game's own camera's: 60 degrees tall and 91 wide at 16:9 (`Camera2.Chase2.Fov` is 60, a vertical angle), and it does not turn with the head.  An eye of the Valve Index shows 103 x 109 degrees (tangents l=-1.54 r=1.05 up=1.41 down=1.41), so even straight ahead small things above and below the game's band were never submitted, and with the head turned away from the direction of travel most of the view was outside it.  Widening the game's FOV (a tweakable variable) helps the edges but cannot follow the head; the only exact fix is to give the game the frustum of the head.  The rendering camera is not touched: only what is submitted changes, so the rest of the mod (`eyeCameraDelta`, the screen-space passes, the motion blur) needs nothing.

**How it was found.**  The Steam build's `Blur.exe` is encrypted on disk; the proxy is loaded after the game has decrypted itself in memory, so the pages of the running module can be copied out of the process and read with a disassembler.  Addresses below are for that Steam build (nothing about a build is hard-coded in the proxy any more, see below).  The function at `0x00425220` (the per-frame update of the main views; it also reads the tweakable `Amax.Views.MainFreeze`) gives the **main view** (`this = [esi+4]`) and the **depth pre-pass view** (`[esi+8]`) their camera with `SetCameraData(view, T)` at `0x42524a` / `0x4252d3` and `SetCamera(view, description, 0)` at `0x425258` / `0x4252f0`.  A third view of the same camera gets it at `0xc3070e` / `0xc3071b`.  The planar reflection view (`0x4279ed`), the rear-view mirror views (`0x425ca1`) and another view (`0x425843`) get cameras of their own.  A sample of what the calls receive (a race in the chase camera, written by a temporary probe) showed:
- `SetCameraData` is `0x007A2CB0` (`__thiscall`, `ret 4`, prologue `55 8B EC 83 E4 F0 83 EC 18`) and copies the 0x3d0-byte camera data `T` into the view at `view+0x2e0`: the rendering camera.  `+0x00` fov in radians, `+0x04` aspect, `+0x0c` near, `+0x10` far, `+0x14` and `+0x24` the camera position in the world, `+0x40` and `+0x80` the view matrix V (rotation + translation in the last row: `p_camera = p_world * R + t`, so the camera's right/up/forward in the world are the columns of R), `+0xc0` and `+0x100` the projection P, `+0x140` and `+0x180` V * P, then rotations, positions and further copies of these.
- `SetCamera` is `0x007A2C40` (`__thiscall`, `ret 8`, prologue `8B C1 53 8B 5C 24 08`) and copies the 0x2b0-byte **description** into the view at `view+0x30`: the world-space frustum the game culls with.  `+0x00` = 6 (planes in use); `+0x10` a plane through the camera facing forward (inward normal = forward, `d = -n . pos`) and `+0x20` its absolute value; 16 plane slots of `{inward normal, d}` from `+0x30` (`n . x + d >= 0` is inside), the first six in use: left `+0x30`, right `+0x40`, bottom `+0x50`, top `+0x60`, far `+0x70` (normal = -forward, `d = forward . pos + far`), near `+0x80` (`d = -forward . pos - near`); `-|normal|` (w = 0) for every slot from `+0x130` (the box test uses it); eight more points at `+0x230` that are not the corners of this frustum (they are far from its apex and are not a perspective frustum's shape) and are left alone.
- The calls come in pairs for one view, `T` first, so the `SetCameraData` hook remembers the camera (position, axes from the view matrix, near, far) for the `SetCamera` that follows.

**What the mod does** (`game_camera.cpp`; `GameCameraPoll`, called from `H_Present`, finds the code and installs the two inline hooks of `inline_hook.cpp` once):
- **Finding the code** (`game_signatures.cpp`; since 1.3 - 1.2 had the Steam build's addresses and a guard on the build's size, entry point and time stamp hard-coded, and left every other build alone, including a non-Steam build of the same code).  Every executable section of the loaded `Blur.exe` (except the extra one of the Steam build) is searched for wildcarded byte patterns of the first instructions of `SetCameraData` (74 bytes) and `SetCamera` (65 bytes): a tight form in which only call targets and absolute addresses are wildcards, and a loose form in which every displacement and immediate is.  A function is used when its pattern matches exactly once (a tight match wins when the loose one also matches elsewhere); the size of the description is read from `SetCamera`'s own `mov ecx, <dwords>` and must be 0x2b0 bytes or a little more.  The first 9 / 7 bytes of the two functions (whole instructions without relative operands, in both forms) are what the hooks steal.  When nothing is found the search is repeated four more times, 120 frames apart (a build that unpacks its code late), and the log says what it saw: `game camera: Blur.exe image 0x... bytes, PE time stamp 0x...: the camera code was not found (SetCameraData: 0 tight / 0 loose match ...)`.  On the Steam build both patterns match once, at `0x007A2CB0` and `0x007A2C40`.  `tools/camera_sig_check.py <Blur.exe or dump>` runs the same scan on a file.
- **Which camera is the main one** (no call-site addresses).  The camera data passed to `SetCameraData` is taken for the main camera when its view matrix is a proper (non-mirrored) rotation, its forward and up axes are within about 11 degrees of those of the last main-pass draw (`g_mainCamera`, set by `NoteMainCameraAxes`), and its projection scales are within 10 % of that pass's (the main pass alternates between 60 and 62.4 degrees vertical; the rear-view mirror's a=1.557 b=9.967, the planar reflection's mirrored camera and a backward camera are excluded).  The struct accepted is remembered by address, so the pre-pass and the third view, which are given the same struct, follow at once; it is checked against the data again every 600 calls.  The snapshot of a camera's data is used by the next `SetCamera` of the same view and then dropped.  Nothing is rewritten until the first main-pass draw has been seen, so the first frames of a race are culled the old way.
- **Layout check.**  Before a description is rewritten for the first time (per description address) the game's own frustum is rebuilt from the camera data (identity head, its own field of view and aspect, no margin, no pull back) and compared, number by number, with what the game passed: the forward plane and its absolute value, the six planes, the six -|n| (tolerance 2e-3; the Steam build differs by 1.6e-7).  A difference, a plane count other than 6, or no field of view and aspect where expected means this build's structs are laid out differently: nothing is written for that description and the log says `head frustum: the layout of this build is not the expected one (...)`.  A pass logs `head frustum: layout verified ...`.
- **The frustum.**  For a camera taken for the main one, `SetCamera` is given a copy of the description with the planes rebuilt around the head instead of around the game camera.  The axes are the head pose (`eyeCameraDelta[2]`: the columns of its 3x3 are the head's axes in the game camera's space, its translation row gives the head's position).  The extent is what both eyes need: the four corner rays of each eye's tangent box, through that eye's own rotation (canted displays), seen from the head, widened by `MarginDeg` (6) on every side.  The apex is moved back by `PullBack` (0.3 m) so the eyes' offsets and a lean are covered, and the near plane sits `PullBack` behind the head.  The far plane is the game's.  Only in a race with a valid head pose; with head tracking off the axes are the game camera's, so the frustum is still the headset's field of view instead of the game's.  The same pose is used for the cull and for the draws of that frame (it is read once per frame, after Present), so there is no lag between them.  The log has `head frustum (use N): head X deg off the game camera, tangents ...` for the first three uses and then every 3000th, and, once the hooks are in, `game camera: N SetCameraData calls (M looked like camera data, K taken for the main camera), P SetCamera calls, R frustums rewritten, L layout failures` at frame 600 and every 12000 frames: on another build this is what shows whether the camera is recognised.

**Checked.**  With the game's own FOV and no head movement the rebuilt planes equal the sample's to 6e-8 (so the layout above is exact); with a head turned 40 degrees and pitched 10, offset a little, and wide eye boxes, 198,000 random points were classified the same by the planes and by a direct test in the head's frame; a hook on a hand-made function with the same prologue forwards correctly.  In the headset (Valve Index, Steam build): objects stay at the edges and when looking away from the direction of travel.  Not measured: the frame time cost of drawing more.

**Other builds.**  Run in the headset: the Steam build (signature scan resolves, layout verified) and a non-Steam build with the same game code (the camera functions are at the same addresses, so the hard-coded version of 1.2 only rejected it because its guard compared the build's size and entry point): the head frustum works there.  A log of that build (1.5) shows both patterns matching once, at the same addresses, `layout verified` (largest difference 1.2e-07) and, after 168,773 `SetCameraData` calls, 60,642 frustums rewritten and 0 layout failures.  A host test (the scratchpad harness, `game_camera.cpp` included directly, real probe sample) checks: the scan on the real bytes of the two functions cut out of the dump (found, ambiguous with two copies, loose-only when a displacement differs, refused when the description is smaller than 0x2b0 bytes, accepted when a little bigger, nothing found in empty code); the layout check passing on the sample and failing for swapped left/right planes, a plane count of 5, a wrong field of view, no field of view and planes shifted one slot; the main-camera test accepting the main camera and its 62.4-degree variant and rejecting the mirror's projection, a mirrored camera, a backward camera and an upside-down one; the hooks against stubs (rewrite for the main camera, nothing for a mirrored one, a lone `SetCamera`, or a failed layout).  For a new build: `tools/camera_sig_check.py` on its `Blur.exe` (a dump of the running module if the file is encrypted on disk), then the headset with `Log=1`; no build counts as supported before that.

**The planar reflection view** (added after 1.5's first headset runs; seen working in the headset on LA River and the docks, 2026-10-25).  The game draws the planar reflection (water, wet roads) with a camera of its own that is the main camera mirrored in the water's plane: position and forward axis mirrored, the **up axis turned
round** (so that the rotation stays proper; read from a record of the pass: right (0.997, 0.002, 0.073), up (0.003, 0.998, -0.068), forward (-0.073, 0.069, 0.995) for a main camera pitched 3.9 degrees down), and a wider field of view (a=0.83, b=1.49 against 0.96, 1.73).  It culls with the frustum it
hands over in `SetCamera` like the main view, so with the head turned the reflection pass left out what the eyes' mirror images need: the water showed the sky where a wall should be, with a straight edge where the game's frustum ends (the dumps of LA River, 2026-10-25), and the edge moved with the head.
The pass is drawn through the mirrored eye cameras (`stereo_scene.cpp`: the eye's delta conjugated by the flip of one axis, which gives each eye's mirror image its place), and the water's lookup uses the same delta, so the lookup and the picture agree whatever the axis is; the axis decides **where the mirrored eye sits**
(the wrong one swaps the eyes and turns a lean upside down: checked in `tools/reflection_cull_check.py`) and **what the mirrored camera sees**, which the culling needs.  **The reflection view is given its frustum before its camera data** (`SetCamera` at 0x4279ED, then `SetCameraData` at 0x4279FE: the other way round from the main view), so a hook that waits for the camera data sees it too late; the first version did that and `game camera:` counted reflection cameras
seen 10,543 and frustums rewritten 0.  At the `SetCamera` the description is all there is of this frame's reflection camera, so the camera is read back out of it (`SnapshotFromDescription`, `ComputeCullPlanes` backwards for a symmetric frustum: the apex is where the left, right and bottom planes meet, the x axis is left minus right
normal, the y axis bottom minus top, the z axis the forward plane's normal, the slopes give the field of view and aspect; false when the planes are not such a frustum; `tools/reflection_cull_check.py` takes 500 random cameras through it).  `game_camera.cpp` then recognises the reflection camera (`ReflectionAxisOf`: not the main camera, straight under it
within 15 m and at least 0.3 m lower, and its three axes the mirror (y negated) of the main camera's with exactly one of them turned round, `MirrorFlipAxis` in `camera.cpp`; the rear-view camera turns round three and is refused), keeps the axis for the pass (`ReflectionFlipAxis`, the old rule of the
view determinant when the hooks have not seen it), and gives the view a frustum made by `ComputeCullPlanes` around the mirrored head: the reflection camera with the head's deltas conjugated by the axis (`FlipConjugate`), the eyes' tangent boxes and the same margin and pull back as the main view.  The layout of the description is checked against the
game's own planes first (`reflection frustum: layout verified ...` or `... not the expected one`), and `Log=1` shows `reflection frustum (use N): ... axis Y ...` and, in the `game camera:` line, how many reflection cameras were seen and rewritten.  It costs frame time on levels with a lot of water: the reflection pass now draws what a frustum
as wide as the main view's needs.
**The reflection is drawn level** (`YawOnlyDelta` in `camera.cpp`, used by the reflection pass in `stereo_scene.cpp` and by the culling frustum above).  After the culling fix, nodding down on LA River still ate the far water's reflection with a dark band that thickened with the nod (user's annotated dumps, 2026-10-25).  A dump of the reflection texture (a temporary addition to
`Num8`, since removed) showed why: the sky and the scenery in it turn with the head's pitch (the sun's glare moved 195 px in the picture and 193 px the other way in the texture), but the dark ground haze below the horizon (cream fading to black, the ground under the mirrored camera) starts at the same row
(0.55 of the texture, in both eyes) in every dump, so the horizon scenery slid below it and was covered, and the far water's lookups fell into the haze.  The game draws it with five screen-space draws into the reflection render target that are not made per eye (a pass-through vertex shader, pixel shaders 428/424 blur taps, 446/445
colour blur, 666 colour and depth) after the scene, laid out for its own camera, which looks straight ahead.  Rather than move those, the pass is drawn the way the game draws it: the head's delta loses its pitch and roll (the eye keeps its place, the direction it faces keeps its yaw, so what is beside you is still
inside the texture) and the lookup uses the same delta as before.  The water's lookup is the projection of the surface point into that camera, so it stays exact; what is lost is vertical coverage when the head nods down far (the nearest water then asks for steeper rays than the texture holds and gets its top row, which is sky).  An earlier guess, that the edge was the mirror's real skyline coming closer as the head dips (the chase camera is only about 0.7 m above the water on LA River), did not hold: a small nod
with no dip moved it just the same.
**The screen-space water** (`OceanFakeReflect`, the docks): its pixel shader projects a reflected point with `g_vViewProjMatrix0/1/3` and reads the scene picture there.  A diagnostic run showed the mod never moved that matrix to the eye (left alone in all 10,296 draws of a run): the test wanted
the matrix times the inverse of the projection **tracked from the draw's vertex shader** to be a camera, which fails when the game's pixel registers carry another projection (the main pass alternates between two fields of view).  `ViewFromProjectionColumns` now reads the x and y scales from the columns themselves, takes the camera
out of the x, y and w columns (the z column of the game's copy is not needed and is not always the one that goes with the others) and writes `camera * delta * eye projection` back (seen working on the docks, 2026-10-25: the reflection stays in place; the hull is not reflected there, which the flat game does the same: the shader samples the scene picture at a point a fixed distance along the reflected ray).

**Open points.**  The eight points at `+0x230` are unexplained and not used by this.  The shadow views were not looked at.  The rear-view mirror view culls with its own camera and is not changed (the planar reflection view is, see "The planar reflection view" above).  `Camera2.Chase2.Fov` and the other tweakable variables of the game (about 3400, `{vtable, next, prev, name, ..., value at +0x24}` static objects in `.data`, the type is the vtable: float `0x00E83B68`, bool `0x00E83B80`, int `0x00E83B50`) can be changed from outside; nothing in the mod does that.

## 19. Menu key repeat (`[Input] KeyRepeatMs`, default 60 ms)

**The problem.** Blur's menus make their own auto-repeat from how long a key has been held, counted in frames (the same cause as the other fast-FPS bugs of
https://github.com/Aib0t/write-ups/blob/main/blur-fps-unlock/readme.md): after a second the held key repeats on *every* frame, so at 300 fps a held arrow key
scrolls a menu about 300 times a second (about 30 was the design). That write-up fixes it in the game's UI broadcast function (it drops auto-fire messages
that come less than 60 ms after the last one, which felt better than 33 ms). BlurVR has no hook there (the addresses are not published), and the request was a limiter on the keyboard, so it works at the poll instead.

**Where the game reads the keyboard.** Blur.exe imports `DINPUT8.dll!DirectInput8Create`, `USER32!GetKeyState`, raw input (`RegisterRawInputDevices`,
`GetRawInputData`) and `XINPUT1_3`. The keyboard is *polled* either way (DirectInput has no key repeat of its own, and a polled `GetKeyState` has none), which
is why the repeat is the game's. `input_limiter.cpp` patches the executable's own import table for `GetKeyState` and `DirectInput8Create` (DllMain, and again
from `Direct3DCreate9`); `DirectInput8Create` is wrapped so that `IDirectInput8::CreateDevice` (vtable slot 3) is hooked, and every device it creates gets
`GetDeviceState` / `GetDeviceData` (slots 9 / 10 of `IDirectInputDevice8`) hooked, once per vtable (the A and W flavours have their own). Only a 256-byte
`GetDeviceState` (the keyboard's format) is touched. The log (Log=1) names the path the game really uses: `input: the game polls the DirectInput keyboard
(GetDeviceState)` and/or `input: the game polls GetKeyState(0x..)`; a keyboard read through `GetDeviceData` (buffered) is reported and **not** limited,
and neither is raw input. If the limiter has no effect, those lines say why.

**The pad** (added after a report that a controller still raced through the menus). Blur.exe imports `XINPUT1_3.dll` by *ordinal*, 2 and 3 (which on the real
32-bit `xinput1_3.dll` are `XInputGetState` and `XInputSetState`), and the log of a real run shows that the only DirectInput devices it creates are keyboards, so
the pad is read with `XInputGetState` once per frame and nothing else. `PatchXInput` finds the import-table entry by *address* (the one equal to
`GetProcAddress(xinput, "XInputGetState")`, the table being bound by the time DllMain runs) and points it at `H_XInputGetState`. In the menus
`LimitPadState` applies the same rule per controller: every defined bit of `wButtons` (D-pad, A B X Y, LB RB, Start, Back, the stick clicks) is a key, and the left
stick is four more (a push beyond the 7849 dead zone is the direction held; while it is not shown the axis reads 0). The triggers and the right stick are
left alone. `dwPacketNumber` is raised by a counter that grows whenever the picture the game is shown changes, so a game that skips an unchanged packet still
sees every tap. Same menu gating as the keyboard (front end, or the pause overlay) and same reset on a scene change. Tested with `test_pad.exe` and a stand-in
`xinput1_3.dll` that the program imports statically: in the menus a held D-pad, stick direction or A gives 21-23 presses in 1.8 s, in a race level 1, and no
packet number ever went stale or backwards. **Confirmed with a real controller** (an Xbox-style pad, flat, Barcelona oval): the log shows `XInput pad hooked`, the pad polled as controller 0, the limiter
on for the A and D-pad presses in the front end, and in the race Start (0x0010) pressed with the limiter off, the pause overlay up about 0.6 s later, and the next
D-pad press logged as `limiter on, race menu up`; menu navigation with the pad and the keyboard then behaved as intended. `Log=1` writes `input: the game polls the XInput pad` and
`input: pad button 0x.... pressed (limiter on/off, race menu up/not seen)`.

**The rule** (`KeyRepeatShownDown`, a pure function of how long the key has been physically down, so any number of polls per frame agree): a press is a *tap*
of half the interval (at least two frames, from the smoothed frame time, so that a poll cannot fall between taps); then nothing until 500 ms; then a tap
every two taps (= the interval). The game therefore never sees the key held: its own repeat (`TimeHeld` > 1 s) never starts, and each tap is an ordinary
press. Measured in `test_input.exe` (a real keyboard device, a key held with `SendInput`, 1.8 s): without the proxy the game sees 1 press, with it 23 (the
first at once, the first repeat at about 520 ms, then every ~60 ms). The interval is stretched below 30 fps, where the game's own rate is already what
it was designed for. Modifiers, locks and the mouse buttons are never filtered.

**Menus only.** In a race the same arrow keys steer and a tap pattern would wreck that, so they pass untouched; the filters are reset whenever the
scene changes. Two things count as a menu (`input_limiter.cpp`, `InputLimiterFrame` once per Present):
* the front end (`InMenuScene()` in `game_mode.cpp`): no level opened yet, or the last level opened is `showroom`. The scene comes from the file-open
  hooks, so it works flat as well as in VR; F8 / `StartMode` force it;
* a pause or results menu on top of a race (`RaceMenuOverlay()` in `draw_dispatch.cpp`). **How it is recognised:** the menu dims the race with a
  blended UI-shader fill that covers most of the screen (the VR path drops exactly those fills: the `known` pieces with a coverage of 0.6 and more). Flat
  there is no scratch target, so the coverage is measured on the real draw: `RaceMenuMeasure` (constructed at the top of `RunDraw`) starts an
  **occlusion query** around a draw that is a candidate, an alpha-blended draw by a UI shader (`g_Ui...`, no scene matrices) onto the back buffer while a level
  other than the showroom is current, and `RaceMenuOverlayEndFrame` (from `InputLimiterFrame`) collects the finished queries without waiting
  (`GetData(..., 0)`, a frame or two later): a draw that wrote at least 60 % of the back buffer's pixels is a covering fill. At most 48 queries are in flight, they
  are released before a Reset. The overlay is **up** once a covering fill has been seen over at least 250 ms (first to last sighting, so a shorter screen
  flash never counts) and **down** 150 ms after the last one. In VR the draws are redirected into the HUD layers, so a count on the real target would see
  nothing, and the HUD path's own pause dim (`g_menuDimLast`) is used instead.
  **The first version of this detector looked for a blended copy of the scene the size of the back buffer, and Blur's real pause menu does not draw one**
  (the log of a real run: every blended draw to the back buffer in a race reads only the 960x540 and 480x270 glow / blur targets, the UI shaders have them
  bound permanently); that is why the limiter did not engage in a pause menu. This version rests on what the VR code says about those menus, and **it worked in a real run** (flat, 1920x1080, Barcelona oval, `Log=1`): the detector went
  up 250 ms after the pause menu appeared and down as soon as the player left to the front end, and the menu keys stopped racing. What the log recorded: the
  pause fill is drawn by UI vertex shader 256 and wrote 100 % of the screen at first, then 65-75 % and falling to 25 % as the menu animated (the 60 % threshold
  held for the part that matters); the Enter presses made while it was up were logged as `limiter on, race menu up`. **It is also up for the whole loading and
  count-in stretch at the start of a race** (here from the level load at 22:16:56 to 22:17:10, about 14 s): the same covering fill is drawn there. That does not
  matter while the car cannot be driven, but a key held down through the start (the accelerator) is shown to the game as taps until the fill is gone, which has
  not been checked at the moment of GO. Also tested with synthetic draws (`test_overlay.exe`: the real proxy, a real device, real vertex shaders with hand-made
  constant tables, F13 held with `SendInput`; the fill gives ~16 presses in 1.7 s, and none of the near misses (a small UI quad, no blending, a non-UI shader,
  a 150 ms flash) switches it on). `Log=1` writes `race menu: up` / `race menu: gone`, `race menu probe:` lines (one per UI vertex shader and
  coverage tenth, from 20 % up: how much of the screen each blended UI draw writes), and `input: key 0x.. pressed (limiter on/off, race menu up/not seen)`
  for Escape (0x01), Enter (0x1C) and Backspace (0x0E), so a log shows whether the detector followed the menu.
  **A way out:** flat, the mode key (F8, `[General] ModeKey`) cycles `g_modeOverride` (auto -> forced on -> forced off) for the limiter, as it does for the
  VR mode detection; `InMenuScene()` honours it.

## 26. The AI car markers over their cars

**The problem.** Over every AI car the game shows a marker (name, race position, weapon, health, lock-on reticle ...). It is a 2D UI element that the game *places in the world*: in the flat game it sits on its car, but BlurVR draws all UI into
one layer and shows that layer as a flat panel (section 6), so with the head turned the markers stay on the panel instead of over the cars. With the VR half running (`Enabled=1`, `Stereo=1`, the headset's runtime up) each marker is shown over its car, at the car's depth, whichever way the head looks; with `Stereo=0` nothing is hooked and the game is untouched.
It has no settings (the values below were tuned in the headset and are constants in `ai_hud.cpp`) and needs the hooks of section 25 (the main camera's world pose comes from them; where they are not installed the markers stay on the panel). Everything below was read from a dump of the Steam build
(`image_dump/Blur_decrypted.exe`, capstone) and checked in the headset (Index, 4032x2240 HUD layer, a race of 20 cars).

**What the game does** (Steam build, image base 0x400000; source file `amax\Library\Ui\Game\Gadgets\Hud\UIAiHud.cpp`):
* The gadget class `UIAiHud` has its per-frame function in vtable slot +0x30 of the table at 0xEEB2E0: **0x00C54180**, `__thiscall`, no arguments, plain `ret`, 12 bytes of whole instructions at its start. `game_signatures.cpp` finds it by a 67-byte
  pattern (call targets are the only wildcards) that matches once; `tools/camera_sig_check.py <exe>` prints it and the places below. It runs on the presenting thread, once per frame, in the same frame as the UI draws that follow. It returns at once unless
  `this+0x144` (visible) is set, `this+0x190` is 0 and `this+0x1AC` (the player's view index) is not -1.
* The gadget keeps a vector of **slots** at `this+0x1B4` (size at `+0x1BC`), **0x34 bytes each, one per car, indexed by car number**. Dword 0 is the car's **root element**; the children are at +0x04 `ai_name`, +0x08 `ai_position`, +0x0C `ai_perks` (the
  weapon), +0x10 `ai_damage` (health), +0x14 `ai_targeting`, +0x18/+0x1C its left/right, +0x20 `ai_winner_icon`, +0x24 `ai_rival_icon`, +0x28 `ai_rival_arrow`, +0x2C `ai_winner_pos`, +0x30 `ai_team_icon`.
* The update starts by working out the camera of its view (`0x00C320A0(view)` gives the camera object, `0x00588030` its rotation matrix and position) into locals of its stack frame: the 3x3 matrix at **+0x98** (its rows are the camera's axes, the third
  is the forward axis) and the position at **+0xBC**. It then goes through `this+0x1C0`, a sorted array of {key, car number} (the cars nearest the player's car first, re-sorted every 16 updates), and for each car **writes the root's world position**
  (`root+0x70/+0x74/+0x78` = the car's position + the template offset `this+0x70..0x78` + a height term), its alpha (`root+0x58`, from the distance to the camera: `fade_near_start/end`, `fade_start/end`), and a **yaw** (`root+0x80`). Cars behind the camera's forward
  plane are skipped (a `ja` at +0x404); at most `max_draw_count` = `this+0x194` (5) markers are drawn: a counter of the markers drawn so far, a local at +0x30 of the stack, is compared with the limit for each car, and a rival or winner marker is drawn past it.
  The children's flag 0x40000 is set when the update starts on a car and cleared when its elements are drawn, so the elements still flagged afterwards are the cars that were not drawn.
* The yaw is chosen at +0x6EF: `cmp byte [UI.HUD.BillboardToCarInsteadOfCamera],0` (the flag is at 0x01244328, on in the Steam build). With the flag set the yaw follows the **car's heading** (atan2 of its matrix's forward x and z, plus 180 degrees when the car
  points away); with it clear it is the camera's own heading.
* The UI renderer (singleton 0x01160F38) records 128-byte items into per-layer lists and draws them later, from one dynamic vertex buffer; the draws cannot be told apart per car (the text of the whole HUD is one or two indexed draws). That is why the
  pictures are taken from the HUD layer by rectangle. A marker is a billboard of real size: bar and icon about 1.45 m wide starting 0.13 m above the anchor, so on the layer its pixels are metres times (W/2 * a / z), a = the camera's `projX` (0.872 at the
  4032x2240 layer: 65 degrees, aspect 1.8; the same for y with `projY`).
* **HudDummy**, the HUD's root gadget, has an update (0x00C72850) that moves, scales and turns the whole HUD with the car's acceleration (`UI.Viewport.*` tweakables); everything under it, the markers' pictures too, carries that.

**How the markers are shown** (`ai_hud.cpp`; the places in the game's code are found by byte patterns, checked once, and the feature stays off, markers staying on the HUD panel, when any of them is not the expected one):
* **Hook and detour.** The update is hooked at its start (`H_AiHudDraw`, an inline hook) and in two more places: a 5-byte detour to an assembly stub (`ai_hud_site_stub`: saves flags, registers and xmm0-7, aligns the stack, calls `AiHudSiteCall`) right after the
  root's world position is written (+0x690), and the 6-byte `ja` of the plane test, which is turned into a nop while the markers are moved so that every car reaches the detour (`UpdatePlanePatch`). At the detour the car number, the markers drawn so far, the alpha
  and the rival / winner bytes are on the game's stack (+0x3C, +0x30, +0x20, +0x1A / +0x1B), so it knows which markers the game will draw (checked against the elements' flags: `the detour's idea ... agrees ... in N of M cases` in the log; a disagreement in a
  quarter of the cases, or a car number that is not where it was expected, switches the moving off).
* **Slots.** The game draws a marker at its car, which puts a near car's marker on top of other UI (a car at 4 m is 1500 px wide), has no marker for a car beside or behind the camera, and makes it as small as the car is far. So at the detour the marker's root
  is moved to a **slot**: `kRelocSlots` = 8 places side by side in a band of the HUD layer (`kBandY`, 0.5 = the middle, where no other UI is), each as wide as its cut-out plus 2 pixels, at a depth (`kSlotDepthMetres`, 14 m) that makes the picture large and sharp
  (about 125 px per metre). The detour writes the world position that the camera sees at the slot's pixel and depth (`SlotCut`, `CutWorld`); the game then draws the marker there, alone, for any car, with the lock-on reticle and the rival arrow. A car keeps its slot for as
  long as its marker is drawn (`SlotForCar`; new cars take the free slots nearest the middle first). The car's real position is kept.
* **The camera used.** The slots are placed with the game's **own camera**, read from the update's stack (`ReadGameCamera`: unit rows, within 25 degrees of the axes of the camera `SetCameraData` gave, within 150 m of it; if it fails the camera `SetCameraData` last
  gave is used, and the log says so). The camera of the last 3D picture is not the one the game's UI uses (it can be a frame behind: at 70 m/s a metre or two, a zoom of 6 to 17 percent of the markers' pictures at the slots' depth), and the cars' positions the game
  writes belong to the game's camera. The compositor therefore keeps each car at the same place **relative to the game's camera** and puts it there relative to the camera that is drawn (`anchor = drawn camera + (car - game's camera)`).
* **Facing the camera.** While the update runs the byte `UI.HUD.BillboardToCarInsteadOfCamera` is held at 0 and then put back: the yaw of every marker is the camera's, so each one is flat to the screen in its slot whichever way its car points (with the game's own
  value the marker in a slot is seen at an angle whenever the car does not point where the camera does).
* **Which cars get a marker** (`PlanPicks`, `kPickAngleDeg`, 80 degrees). The game would mark the nearest cars to the player's car. Before the update the cars of the last one are ranked by their distance from the **head**
  (`eyeCameraDelta[2]`), those inside a cone of 80 degrees around where the head looks (and at least `MinDistance`, 1.5 m, away) are eligible, the nearest `limit` (the game's, 5) are wanted (a car that has a marker counts as 15 % nearer so that two at the same
  distance do not take turns), and at each car the detour makes the game's own test agree by putting the counter on its stack to 0 for a wanted car and to the limit for any other. A rival or winner marker is still drawn past the limit; a car the last update did not know of is drawn
  from the next one on. Looking at the cars beside or behind marks those.
* **The HUD's sway** is held at rest (`H_HudSway` hooks HudDummy's update, 6 bytes taken at its start, checked by a 60-byte start pattern and a 63-byte one at +0x297 that covers the stores of the outputs and the reads of the rest values): after the game has worked the
  sway out the outputs (+0x70/+0x74 position, +0x84 rotation, +0x88/+0x8C scale) are put back to the rest values (+0x190/+0x194, +0x19C, +0x198), so the slots stay where they were put. The flat HUD stops swaying in the headset as well.
* **Compose** (`AiHudComposeMarkers`, called from `ComposeHudLayers` once the UI is in the HUD layer, before the flat panel is drawn). Each marker is cut out of its slot (`FixedMarkerParams`: 1.7 / 1.7 / 1.3 / 1.7 m left / right / up / down of the slot's anchor pixel; the 1.7 m down reaches
  the lock-on reticle) and drawn as a **billboard at the car** in both eyes (`BuildMarkerGeometry`, `DrawMarkers`): the cut-out's size in metres (true perspective), **flat to the head's view** (perpendicular to where the head looks, the game's up as its up) so that all markers have the
  same orientation, scaled about the anchor (vertically about 0.12 m above it, so a marker that is made larger keeps its distance from the car) so that its width never looks smaller than 3 degrees or larger than 20 from the head, projected per eye with `eyeCameraDelta[eye]` and the eye box as `FlushStreakLayer` does, `rhw = 1/z`, the eye's scissor, far to near, no depth test (the flat game does not hide them either). Then the slots are cleared from the layer (`ColorFill`) **and from
  the game's glow buffer of its UI** (the same part of its much smaller picture plus 4 texels for the blur; `NoteSceneBloom`'s texture, passed in by `ComposeHudLayers`), so nothing of a marker is left on the panel. A marker that could not be moved (the detour not installed) is cut out
  where the car is: cars ahead only, and whatever else is inside the rectangle comes along.
* `Log=1` writes the install lines (`ai hud: ...`: the hook, the detour, the plane test, the yaw flag, the sway update), `world markers: the game's own camera is read from the update's stack`, `... the HUD's sway is held at rest`, `... the game turns its markers to face
  their cars ... held off during the update`, and `world markers: N placed, M quads, moved to slots: car ...` now and then.

**What the headset runs taught** (each change below is one thing seen in the dumps): cutting the pictures out in place took the rear-view mirror and the minimap along with a near car's marker, left the UI's glow on the panel and gave no marker for cars beside or behind
(-> the slots); the content of a slot sat off its cut-out's centre by a share of the slot's distance from the middle that changed from dump to dump (-> the HUD's sway, a scale of up to 5 percent); after that the pictures were still zoomed outward about the middle by 6 to
17 percent, markers' sizes too (-> the game's own camera instead of the last picture's); a marker looked turned when its car did not point the way the camera does (-> the yaw flag); and the markers went to the cars nearest the player's car, not the ones near the head (-> the pick).

**Known limits.** If the band has UI of its own at some moment (a popup in the middle of the screen) a slot takes a piece of it along (the band's height is `kBandY`). A reticle or the rival arrow further out than the cut-out is cut off and stays in the band. At most 8 markers can be placed at once (5 by the game's limit plus a rival and a winner). A marker nearer than 1.5 m from the head is not shown. The projection scales are `SetCameraData`'s (the UI's own were not
needed: the pictures sit at their slots). The patch of the skip, the detour and the yaw flag are changes inside the game: a different build whose bytes at these places differ leaves the markers on the HUD panel, as without the feature.
**Other builds.** Run in the headset on the Steam build and on a non-Steam build (the same game code as the Steam build; `tools/camera_sig_check.py` finds the same places there): all five places were installed (`ai hud: ... hooked`, the yaw flag, the sway update, the detour, the skip), markers were placed on the cars, and the detour's idea of the drawn markers agreed with the game's in 208 of 208 cases. No other build has been tried.

**Checks.** `tools/ai_hud_check.py` builds the module with a fake game function made of the real bytes of the update's prologue and of the places that are patched (so the scan, the inline hook, the detour, the stub and the patches go into real code) and checks: that the stub keeps
every register; which markers the detour sees as drawn; the slots (tiling, no overlap, the world position that projects to the slot's pixel and depth, also for a turned and moved camera); the game's camera on the stack (read, refused when it is not a camera or far from the given one,
the cars kept relative to it); the yaw flag held off and put back; the pick by head (the nearest car in the cone, the limit, the cone, the 15 % bonus); the geometry (clamps, the billboard flat to the head, a car behind the camera); the device calls (quads per eye, the order, the fills of
the layer and of the glow); the HUD sway held at rest; a wrong car number giving the detour up. `tools/camera_sig_check.py` prints what the scan finds in a Blur.exe or a dump of the running module (`AI HUD: ... billboard flag found`).

## 27. The OpenXR backend (`[VR] Api=openxr`, new in 1.6)

**What and why.** The headset can be reached through OpenXR instead of OpenVR, chosen by `[VR] Api` (`openvr`, the default, or `openxr`; `g_cfg.vrApi`, `kVrApiOpenVr` / `kVrApiOpenXr`). It was asked for to get at runtimes
that are not SteamVR (Virtual Desktop's VDXR was named) and because OpenXR is the API most runtimes speak. With `Api=openvr` the code path is the old one: `tools/hotpath_check.py` says IDENTICAL against the build before the change, and the OpenVR calls are
the same lines as before, only moved into `if`/`else` forks. A failed OpenXR start leaves the game flat (`VrInit` returns false, `g_vr.active` stays false) and writes the reason to the log; it never falls back to OpenVR by itself.

**32-bit.** The game is a 32-bit process, so OpenXR can be used only through the runtime's 32-bit build: the loader (`openxr_loader.dll`, shipped 32-bit in `BlurVR\`, Khronos release 1.1.65, `LICENSE_openxr.txt`) reads
`HKLM\SOFTWARE\WOW6432Node\Khronos\OpenXR\1` `ActiveRuntime`. SteamVR 2.17 registers `steamxr_win32.json` there (a 32-bit `vrclient.dll`); this was confirmed on the development PC, where `tools/openxr_check.py` shows the 32-bit loader reaching it. Another runtime
without a 32-bit build cannot be used in the game's process; a 64-bit helper process that receives the frames and returns the poses would be a separate project. `[VR] OpenXRRuntime=<path to a runtime .json>` sets `XR_RUNTIME_JSON` for the game's process (before the loader
looks), so a 32-bit manifest that is not the registered one can be tried without changing the system's default. VDXR's 32-bit support is **not verified** (one Virtual Desktop patch note mentions a fix for 32-bit games; nothing says its OpenXR runtime has a 32-bit build).

**Files.** `src/openxr_runtime.cpp/.h` is the whole backend (static state, a narrow interface with no OpenXR types in the header); `src/openxr_wrapper.h` includes the Khronos headers (`third_party/openxr/`, with `XR_USE_GRAPHICS_API_D3D11` and `XR_NO_PROTOTYPES`:
nothing is linked, every function comes from `xrGetInstanceProcAddr`, as `openvr_api.dll` is loaded by name). `vr_runtime.cpp` keeps the maths and the game side and forks only where it calls the runtime: `VrInit` (-> `VrInitOpenXr`), `GetEyeToHead`, `VrUpdatePose`,
the submit block of `SubmitFrameBody` (-> `SubmitFrameOpenXr`) and `ComputeMenuTransform` (the maths of `PlaceMenuOverlay`, shared). `VrSubmitFrame` is a wrapper that runs the body and then `XrRuntimeFinishFrame`, so every way out of the body ends the frame.

**Mapping.**

| OpenVR | OpenXR |
|---|---|
| `VR_InitInternal`, `GetDXGIOutputInfo` | `xrCreateInstance` (API 1.0, `XR_KHR_D3D11_enable`), `xrGetSystem`, `xrGetD3D11GraphicsRequirementsKHR` -> the Direct3D 11 device on the adapter LUID it names (`g_vr.d3d11Device`, as with OpenVR), `xrCreateSession`, `xrBeginSession` on `READY` |
| `GetProjectionRaw` | the field of view of `xrLocateViews`, as tangents `{tan left, tan right, tan up, tan down}` into `g_rawTangents` (`SplitVerticalTangents` takes the "+y up" form as it is) |
| `WaitGetPoses` (after Present) | `xrWaitFrame` + `xrBeginFrame` (after Present, in `VrUpdatePose`), then `xrLocateSpace(VIEW in LOCAL)` for the head and `xrLocateViews` in VIEW (eye-to-head, every frame) and in LOCAL (the eye poses submitted with the frame). The poses are turned into OpenVR's 3x4 layout so the maths after them is shared |
| seated tracking space | the `LOCAL` reference space |
| `GetEyeToHeadTransform`, `GetFloatTrackedDeviceProperty(UserIpd)` | the VIEW-space eye poses; the IPD is the distance between them |
| `GetFloatTrackedDeviceProperty(Prop_DisplayFrequency_Float)` (only the FPS counter asks, section 28) | `predictedDisplayPeriod` of `xrWaitFrame`: `XrRuntimeRefreshHz()` is 1e9 / that period in nanoseconds, 0 before the first frame |
| `ResetZeroPose` (chaperone) | none: `g_vr.chaperone` is NULL, so the mod's own reference (`refYaw`, `refPos`, the old fallback) is used for F9 and for the recentre at the race start. A `XrEventDataReferenceSpaceChangePending` for `LOCAL` (the runtime recentred) sets `recenterPending` and re-places the menu |
| `Submit(eye, texture, bounds)` x2 | one projection layer in `LOCAL`; one swapchain, side by side, the two `imageRect`s are the picture's halves |
| `SetOverlayTexture` + `SetOverlayTransformAbsolute` | one `XrCompositionLayerQuad` in `LOCAL`: the same swapchain, the whole rect, width `[Menu] Width`, height from the picture's aspect, pose from `ComputeMenuTransform` |
| the dark scene texture in menus | nothing: a frame with only a quad layer has a black background |
| `IsDashboardVisible`, dashboard events 502/503 | the session state: changes into or out of `FOCUSED` or `VISIBLE` re-place the menu (for 45 frames), and so does every menu frame in the first 1500 frames while the state is `VISIBLE` (something else has the focus). `SYNCHRONIZED` does not count: SteamVR leaves the session there even while it shows the picture |

**Frame discipline.** `XrRuntimeNextPose` handles the events, ends a frame that is still open (with no layers), waits, begins and locates. `XrRuntimeSubmitScene` / `XrRuntimeSubmitMenu` copy the picture into the swapchain image
(`CopyResource` from `g_vr.sharedTexture11` or `g_vr.frameTexture`, `Flush`) and end the frame with the layer; `XrRuntimeFinishFrame` ends it with no layers when nothing was submitted (the pose was not valid, `shouldRender` was false, a copy failed, an early return of the body).
A session that goes `STOPPING` is ended, `EXITING` / `LOSS_PENDING` stop all submitting (logged once; the game keeps running without the headset).
**SYNCHRONIZED.** SteamVR keeps a new application in `SYNCHRONIZED` with `shouldRender` false (and `xrWaitFrame` not pacing: about 130 frames a second) while the headset is in standby (SteamVR logs "entering standby" during such a run; the Index goes there when it is not worn) or
until the application is shown, so `BeginFrame` treats `shouldRender` as true in that state: the frame is rendered and submitted with its layers, which the runtime accepts (its log shows the two projection views with the bounds of the two halves). Without that a runtime that waits for
a first frame with layers before it makes the session `VISIBLE` would never show anything. In the game (below) the session stayed `synchronized` for three minutes while the headset lay unworn and became `visible` and `focused` in the same millisecond when it was put on, so the state follows the headset; whether the runtime would show the application's frames
without the override while the headset is unworn is not known.

**The field of view submitted is the runtime's own, not the one scaled by `[Render] FovScale`.** The picture is drawn for the scaled boxes (`ComputeEyeBoxes`) and shown over the whole runtime field of view, which is what `FovScale` means with OpenVR too (the compositor stretches the whole texture over the field of view it knows).

**Swapchain format.** `B8G8R8A8_UNORM_SRGB` (91) if the runtime offers it, else `B8G8R8A8_UNORM` (87); the picture is gamma-encoded BGRA, the sRGB format makes the runtime decode it, and both are in the picture's own format family, which `CopyResource` needs. A runtime that offers neither (RGBA only) is refused with
the list of what it offers in the log; a shader blit would be needed for it and is not written. The swapchain is created at the first picture and again when the back buffer changes size (the game's resolution options).

**Not carried over.** The `[Foveation] Report` line (`lens:`) has the headset name, the runtime's own render size, the largest swapchain and the tracking kinds, but not the refresh rate, the hidden-area share or the eye-gaze facts (those are OpenVR properties; `lens_model.cpp` is OpenVR only and returns when there is no `IVRSystem`). The one refresh rate there is comes from `XrRuntimeRefreshHz` (the predicted display period of `xrWaitFrame`, above), which the FPS counter (section 28) uses for its colours.

**Log lines (with `Log=1`).** `VR API: OpenVR (SteamVR)` or, for OpenXR: `OpenXR: runtime <name> <version>`, `lens: headset <name> (OpenXR) ...` (or `OpenXR: system <name>`), `OpenXR: graphics adapter <name>`, `OpenXR: D3D11 device ok`,
`OpenXR: session idle -> ready` and the following state changes (`synchronized`, `visible`, `focused`; later `focused -> visible` when a dashboard is open), `OpenXR: eye N field of view ...`, `OpenXR: display period 11.11 ms (90.0 Hz)` (at the first frame and again when the period changes by more than a percent; 8 lines at most), then the shared lines `eye N tangents ...` (compare them with an OpenVR run: they should match to a
fraction of a percent), `IPD`, `eye N to head`, `VR initialised (OpenXR)`, at the first picture `OpenXR: swapchain formats offered: ... -> <format>`, `OpenXR: swapchain WxH, n images`, `OpenXR: first copy, picture WxH format 87 -> image WxH format 91`, and (SharedTransport) the usual `SharedTransport:` lines.
Failures: `OpenXR: could not load openxr_loader.dll`, `...listing the runtime's extensions failed` / `xrCreateInstance failed` (with the 32-bit registry hint), `...the runtime reports no headset`, `...xrGetSystem failed`, `...the session did not become ready`, `...xrEndFrame failed`.

**Checks.** `python tools/openxr_check.py` builds `tools/openxr_check.cpp` (it links the real `openxr_runtime.cpp`, `core.cpp` and `config.cpp`) and runs: `probe` (the 32-bit loader finds a 32-bit runtime that offers `XR_KHR_D3D11_enable`; no instance is created, so no runtime is started), a `.json` that is no runtime
(OpenXR does not start, cleanly, and the setting is named in the log; this also shows that `XR_RUNTIME_JSON` is picked up), and no `openxr_loader.dll` in the folder. With `--system` it also starts the installed runtime and runs 90 frames of a test picture (60 race frames, 30 menu frames): with
a headset on, that is the nearest thing to a headset run that does not need the game, and it launches the runtime (SteamVR) if it is not running. The conversions between OpenXR's poses and OpenVR's 3x4 matrices were checked by a round trip (quaternion -> matrix -> quaternion, orthonormality, a yaw of +90 degrees turns the forward direction to the left).

**Verified with `--system` (Valve Index, SteamVR/OpenXR 2.18.4, RTX 3060; the headset went into standby during the run).** The 32-bit loader starts the runtime (it launches vrserver by itself); `lens:` reports the runtime's own render size 2468x2740 per eye, 8192x8192 at most;
the graphics adapter and the Direct3D 11 device (FL 11_0) are found; the session goes `idle -> ready -> synchronized`; the fields of view are l -57.0 / r 46.4 / up 54.6 / down 54.7 degrees (left eye) and mirrored (right eye), tangents l=-1.541 r=1.051 up=1.407 down=-1.414, and the eye-to-head
offset is -/+31.5 to 35.0 mm in different runs (the IPD setting of the headset); the swapchain formats offered are 29 91 2 10 24 40 55 45 20 (91 = `B8G8R8A8_UNORM_SRGB` is taken; its images are reported as format 90, `B8G8R8A8_TYPELESS`, and the `CopyResource` from the picture's format 87 works); 60 race frames and 30 menu frames
were submitted with a pose every frame and no error; the runtime's client log shows the projection views with bounds (0, 0, 0.5, 1) and (0.5, 0, 1, 1). The session never left `synchronized`, in a first run of 8 s with the headset in standby and again in a run of 20 s, although the compositor presented every frame at 90 a second (1893 presents, none dropped) and
**the user saw the test gradient in the headset** (the race picture, through the projection layer, is visible). The menu quad (30 frames, a third of a second) was submitted and the compositor created its overlay, but whether it was seen is not known.

**Verified in the game (Api=openxr, Valve Index, one session of about nine minutes, `BlurVR.log`).** `starting VR now (3D menu scene is being drawn)` (`DeferVR`), `VR initialised (OpenXR)` 4.6 s later on the worker thread (`ThreadedInit`; 3.3 s of it the runtime's instance creation), `VR active (threaded start)`, `mode -> 3D (stereo, seated)`;
the shared-texture path (`SharedTransport: first race frame handed over as a shared texture`) and then `OpenXR: swapchain formats offered ... -> 91`, `swapchain 2160x1200, 3 images`, `first copy, picture ... format 87 -> image ... format 90`; the `eye N tangents` lines equal the check's; the head frustum, reflection frustum, mirror pass,
HUD layers, streaks, the AI car markers (`world markers: ... placed`, the detour agreeing in 203 of 203 cases) and the race menu detection all ran as with OpenVR; a race (`spa_barcelona_gracia`) with the floating screen while it loaded (`mode -> BOXED`, 26 s) and `3D scene detected -> stereo`; two resolution changes in the options
(4936x2740, then 4032x2240: `Reset`, new shared texture, `OpenXR: swapchain 4936x2740` and `4032x2240`, the swapchain made again each time); back to the showroom; at the end `settings.xml: the flat game's display mode is back`. No `OpenXR:` failure line and no skipped-frame line. The session was `synchronized` from the start (the headset lay
unworn for three minutes, the game already rendering) and went `synchronized -> visible -> focused` at the moment the headset was put on; the recentre by the mod's own reference (F9 or the race start) printed `recentered (own reference)` each time. The look of it, by the user's eye: as good as OpenVR, no difference noticed (scale, sharpness, colour, eye order, the menu screen, comfort). Not measured: the frame pacing (`[Perf]`, Num7), F9 and the dashboard in a deliberate test, any other runtime or headset.

**Not verified (needs the headset worn, and the game).** Everything past that: that the picture arrives with the right scale, colour and eye order; that the menu quad sits where the overlay did and follows a dashboard and a recentre; that a quad-only frame does not show the runtime's home behind it; frame pacing and latency against `WaitGetPoses`
(`[Perf]`, Num7); the shared-texture path into the runtime's images (`first copy` and the absence of `failed` lines); a resolution change in the game's options (swapchain re-creation); VDXR or any other runtime.

## 28. The FPS counter in the headset (`[VR] FpsCounter`, default 0; numpad 9 in game)

**What it shows.** A small panel in both eyes of a 3D frame (a race or the showroom) with the number of frames the game presents a second (`62 FPS`).  `WaitGetPoses` runs right after `Present` (section 7), so the game is paced by the headset: at 90 Hz the counter reads 90 while the PC keeps up
and 45 when SteamVR has to hold the game to half the rate.  It counts what the game delivers to SteamVR, not what the headset displays (reprojection or motion smoothing are not seen).  With `Api=openxr` (section 27) `xrWaitFrame` runs at the same place (`VrUpdatePose`, right after `Present`) and the
counter is the same code (it counts from `VrSubmitFrame`, which both backends go through, and draws into the picture before it is copied to the headset); how well `xrWaitFrame` paces the game is the runtime's business and has not been looked at.

**Counting** (`FpsCounterFrame`, called at the top of `VrSubmitFrame` for every presented frame while the counter is on).  A window opens at a frame and closes at the first frame at least half a second later; the figure is the number of frame intervals in it divided by the
time they took (`QueryPerformanceCounter`), rounded, and it stays until the next window closes: two updates a second, no flicker.  A gap of two seconds or more (the counter was just switched on, a level was loading) starts over and shows nothing until a window is full; a hitch
of a second and a half only lowers the figure.  The largest figure is 999.  `FpsCounterFrameAt` takes the clock as arguments so that the host check can feed it frames.

**Colour** (`FpsCounterColor`).  Against the headset's refresh rate (`Prop_DisplayFrequency_Float`, asked each time a new figure is taken): green from 95 percent of it, yellow from 70 percent, red below; white when the refresh rate is not known.  With `Api=openxr` (`g_cfg.vrApi`; there is no `IVRSystem` then) the rate is `XrRuntimeRefreshHz()`: 1e9 divided by the `predictedDisplayPeriod` that the last `xrWaitFrame` gave (section 27), and for both APIs a rate outside 20 to 1000 Hz counts as not known.  It is the runtime's own idea of its display's frame period; SteamVR reports 11.11 ms (90.0 Hz) for the Index (`tools/openxr_check.py --system`; the first frame after `ready -> synchronized` once gave 55.56 ms, 18 Hz, which is outside the bounds and so not used, and the next frame was back at 11.11 ms).  That run was not held to half rate, so whether the period doubles then is not known.  If a runtime gave the period it paces the application with and held the game to half the headset's rate, the period would double and the counter would stay green at half the rate: the log line `OpenXR: display period ...` and the counter's `first panel drawn (... headset N Hz ...)` line show the value, and `tools/openxr_check.py --system` prints it.

**The graph** (`BuildGraphRects`).  Under the figure, on the same plate (41 x 22 font pixels in all), is a graph of the frame rate of the last 18 seconds: a sample every quarter of a second (the frames in it divided by its time, from a window of its own next to
the figure's half-second one so that a hitch shows; at most 74, the newest at the right, none left after a gap of two seconds).  It is drawn in cells of half a font pixel: 74 columns and 18 rows inside a margin of 2 font pixels at the sides and 1 above and below.  The line is 2 cells (a
font pixel, 0.29 degrees) thick and can be at 17 heights.  **Scale:** the lowest sample of the graph is at the bottom of the plot and the highest at the top, so the line always fills the box and no values are shown; the span is never less than a tenth of the highest sample (the samples
are then centred), or a rate that hardly changes (89, 90, 91) would swing from the bottom to the top.  Each column goes from the height of its sample to the height of the next one so that the line is joined up, and has the colour of the figure (`FpsCounterColor`) for the lower of the two;
neighbouring columns that look alike are one rectangle.  `FpsCounterGraph` gives the samples to the check.

**Drawing** (`FpsCounterDraw`, from `VrSubmitFrame` after `ComposeHudLayers` and before the copy to the headset; 3D frames only, races and the showroom: `IsRaceMode()`, the mod's 3D mode, and 3D draws in the frame).  The plate and the text are solid-colour rectangles, one `DrawPrimitiveUP` per eye, with no
texture and no system font: the digits (right-aligned, room for three) and the label `FPS` are a built-in 5 x 7 pixel font of 13 glyphs (`kGlyphs`), and each run of lit pixels in a row is one rectangle (40 to 60 for the figure and, for the graph, one for each run of columns that look alike, 74 at most).  The plate is 41 x 11 font pixels; a font pixel is 0.005 as a
tangent (0.29 degrees), so the text is 2 degrees tall and the plate 11.7 x 3.1 degrees, centred 0.34 to the left and 0.30 below straight ahead (about 19 and 17 degrees), at 1.5 m (`kFps...` in `fps_counter.h`).  The graph's plate is the same size and sits right under it, as one plate: the whole panel is 41 x 22 font pixels and the figure stays where it was.

**Why it stays put.**  `eyeCameraDelta[eye]` goes from the game camera's space to the eye's and holds the head's pose (it is `headDelta * headToEye`, section 3), and `eyeCameraDelta[2]` is the head's part alone.  A point in head space therefore goes to the camera's space with the
inverse of `eyeCameraDelta[2]` and into the eye with `eyeCameraDelta[eye]`, and then through the eye's tangent box like the HUD panel (`drawLayerQuads`).  The two eyes get their own offsets (and a canted display its turn) from SteamVR's eye transform, the numbers the scene uses, so the
panel is an object at 1.5 m, not an overlay at infinity.  Head tracking off (F10) makes `eyeCameraDelta[2]` the identity and the panel sits at the same place in the view.

**State.**  Like `ComposeHudLayers`: a `D3DSBT_ALL` state block of its own (made at the first draw, released by `ReleaseFpsCounter` in `H_Reset`) captures what the game set, everything the mod tracks is set through the original functions (`g_deviceOriginals`) so the tracking never
sees it, and afterwards the state block, the render target, the viewport and the scissor are put back.  The blend leaves the picture's alpha alone (separate alpha blend, zero and one).  The mod itself never calls `BeginScene` (the HUD is drawn at the same point and works), but if the draw
fails the counter opens a scene, draws again and closes it, and the log line says so (`FPS counter: first panel drawn (...)`, with `in a scene of its own`; `drawing the panel failed 0x...` when both fail).

**Not drawn:** on loading screens, in videos and before the 3D scene is up (those frames are a flat picture for the floating screen, not a pair of eyes) and with `Stereo=0` (`LoadConfig` sets `fpsCounter` to 0 then).  In the window on the desktop with `DesktopView` left, right or both the panel is in the picture, as part of the
headset's image; with `DesktopView=off` nothing of it is.

**Switch.**  `[VR] FpsCounter` for the start; `[Debug] KeyFps` (0x69, numpad 9) flips it live and writes `switch: FPS counter = n`.  Off, the cost is one branch in `VrSubmitFrame` and the key poll; on, a clock read a frame and, in 3D frames, a state block capture and apply (the same
as the HUD composition's) and two small draws.  The cost was not measured in the headset.

**Checks.**  `tools/fps_counter_check.py` (`--pictures folder` writes the panel as .bmp files).  It feeds frames of 7 to 144 a second, uneven ones, 5000 a second (limit), a gap and a hitch; checks the colours at 90 and 120 Hz and that the 13 glyphs exist, differ and are 5 wide; and on a real
Direct3D 9 and 9Ex device (a 3200 x 1600 back buffer, 90 degree eyes) decodes the figure (62, 7, 100, 999, 45) font pixel by pixel in both eyes at the place the layout says, for a still head and for one turned 20 and 12 degrees and moved, drawn outside a scene and inside one.  It also checks
that a turned head gives the same picture, that no pixel outside the two plates changes, that the device state (render states, FVF, texture stage, viewport, scissor, render target) is as it was, that the counter off, with no figure or without a pose leaves the picture untouched, and that
it draws again after `ReleaseFpsCounter`.  Each of: the head lock left out, the eyes swapped, the digits left-aligned, the state block not applied, and no scene of its own makes the check fail (tried on copies of the source).  The samples of the graph are fed the same way (a hitch, the 74 it holds, a gap empties it, the limit of 999), and on the device the graph is decoded cell by cell (74 x 18 in both eyes)
against a picture worked out from its samples, with a fake headset of 90 Hz for the colours: a steady rate (a flat line in the middle), one that hardly changes (a short wiggle), a mixed one (green, yellow and red, the lowest sample at the bottom of the plot and the highest at the top) and a full
graph, for a still and for a moved head.  Each of these makes the check fail too: the scale starting at zero, no minimum span, the newest sample at the left, the colour of the newer of two samples, steps that are not joined, a gap that keeps the samples, no plate under the graph and no limit of 999.  The refresh rate under `Api=openxr` is checked with a stub of `XrRuntimeRefreshHz` and an OpenVR system of 90 Hz that is present all the time: the figure is coloured against the stub's 72 Hz (green; 90 Hz would make it yellow), is white when the stub has no rate or one of 10 Hz, and with `Api=openvr` the stub is not asked.  Each of: no fork on the API, no bound on the rate, the stub's rate used with OpenVR too and a fall back to the OpenVR system when the stub has none makes the check fail.

**Still open:** the figure was seen working in the headset, the graph was not yet, and nothing was run with `Api=openxr` (the host check starts neither backend; `tools/openxr_check.py --system` shows SteamVR's display period of 90.0 Hz, but not what it reports while the game is held to half rate): whether the game's `Present` comes inside or after its scene (the log line says), how it reads at 4032 x 2240 on a real headset, where it sits against the showroom's menus, how the graph reads (its line is a font pixel, 0.29 degrees, thick) and the frame cost.


## 29. The showroom's floor reflection: a target as big as the picture, and the game's clip plane

**Symptom** (headset): in the menu showroom the floor's reflection of the car followed the pitch of the head.  Looking up, the car's reflection was missing and the shadow went black; looking down, a dark pinch appeared at the bottom of the floor.  The 2D game's reflection does not move with the view like that.

**Cause.**  The showroom floor (the `Road` and `Puddle` effects) looks up a planar reflection like the water of a race (section 25): `g_ReflectionViewMatrix0/1/3` (c145-c147) in the vertex shader, `ScreenReflectionSampler` (s15, full size) and `BlurScreenReflectionSampler` (s7, a half-size blurred copy made by five plan-0 draws) in the pixel shader.  A race draws the reflection into a half-size target, which is how
the mod knew it (`IsHalfResPass() && IsReflectionSizedPass()`, section 3).  The showroom draws it into a 3584x2016 target, the same size as the main picture, so its draws were taken for the main pass: the head's whole delta instead of the mirrored, level one, and the mirrored camera was taken for the main camera (`NoteMainCameraAxes`), while the floor's lookup matrix was remapped as
for any reflection.  Found with a temporary table of target sizes, plans and sampled textures per level (`pass_log`, `Log=1`; removed after) and the shader dumps.

**Fix.**  `NoteMainSizedCamera` (render_state.cpp), called by `ClassifyDraw` for a draw in a main-sized pass whose view-projection can be recovered, once a frame per target.  The camera position is worked out from the view-projection (p . right = -VP[3][0] / xScale, p . up = -VP[3][1] / yScale, p . fwd = -VP[3][3]) and compared with the main camera pose the game camera hooks
published (`GetMainCameraPose`, not older than 3 frames): at least 0.3 m under it, within 15 m sideways, and the axes a mirror of it (`MirrorFlipAxis`, the same test as `ReflectionAxisOf`).  The axes alone are not enough: a level main camera compared with itself looks like a mirror, hence the position test.  A target drawn through such a camera is marked (`IsMainSizedReflectionTarget`; a main-camera draw
into it takes the mark off again; `H_Reset` forgets the surface).  `IsReflectionPass()` is "half-size reflection pass or marked target".  `ApplyEyeConstants` gives such a pass the mirrored, level delta (the flip axis comes from the game camera hooks, or from `MainSizedReflectionAxis()` when they have not seen the view), and draws of a marked target do not publish their camera as the main one.  With `Log=1`:
`reflection target: the 3584x2016 render target ... is drawn through the mirrored camera (1.4 m under the main camera, axis Y turned round): its draws are the planar reflection` (the depth seen is 1.4 to 1.8 m).

**What was tried first.**  With the mark above, tilting the head no longer changed the reflection, but the car was cut: the reflection picture (the target, saved by a temporary addition to Num8, since removed) showed the car without its lower half, with a ragged edge, and the missing part moved with the height of the head.  Attempts that left the cut were all drawing the reflection per eye from the mirror image of the eye: the race's `YawOnlyDelta` with a fixed tilt of 10 degrees found with temporary keys; the head's whole delta (the exact mirror of each eye); a delta level in the world (`WorldLevelDelta`).  Then the showroom's reflection was drawn once from the game's own camera, as the flat game draws it (plan 0, no remap of the floor's lookup): the car was whole, but every other reflection in the menu was out of place (the picture is the view from the game's camera, 0.75 m above the floor, and the floor reads it as if the eye were there; the headset is higher and elsewhere).  That version was taken out again once the cut was understood.

**The cause: the game's own clip plane.**  The earlier note that the game never calls `SetClipPlane` was wrong: it came from a search for the wrong vtable offset (`SetClipPlane` is slot 55, offset 0xDC, `SetRenderState` slot 57, offset 0xE4).  The reflection function of `Blur.exe` (0x4297F0; `this + 0x68` is the reflection camera, whose world plane `g_vPlaneReflection` is at +0x78C) normalises that plane, carries it through the camera's view matrix and then its projection matrix (`D3DXMatrixInverse`, `D3DXMatrixTranspose` and `D3DXPlaneTransform` for each, i.e. the plane in clip space is the world plane times the inverse of view * projection), calls `SetClipPlane(0, plane)` (0x429EE4) and
`SetRenderState(D3DRS_CLIPPLANEENABLE, 1)` (0x429EF6) when the byte at 0x111EBFC is set, draws the scene, and switches the plane off again (0x429F2E).  With vertex shaders the plane is applied to the shader's output position, so it is a plane in the clip space of the game's camera.  The mod gives the vertex shaders the eye's view-projection (the game's times `clipRemap`) and leaves the plane as it was, which is then another plane in the world: one that moves with the head and cuts away whatever is on its far side, the lower half of the car, higher or lower as the head is, with a ragged edge because a plane goes through a 3D object.  That is why tilting or levelling the camera never helped, why the game's own camera was right and why the haze, the first suspect, was not it (it sits at the horizon row, below everything the floor looks up).  The same plane is set for the water of a race: it moved with the head there too.

**Fix.**  `DeviceNoteGameClipPlane` and `DeviceNoteGameClipPlaneEnable` (device_state.cpp, fed by `H_SetClipPlane` and `H_SetRenderState`) keep the game's plane 0 and whether clip planes are on.  `ApplyEyeConstants` carries the plane to the eye it is drawing: the clip coordinates of a point for the eye are the game's times `clipRemap` (the matrix every tracked view-projection is also multiplied by), so a plane `p` on the game's coordinates is `clipRemap^-1 * p` (as a column) on the eye's, the same set of points of the world (`EyeRewrite::clipRemapInverse`, made with the rewrite and cached with it);
`DeviceSetEyeClipPlane` puts it on the device when it differs from what the device holds, and `DeviceRestoreAll` puts the game's back together with the vertex constants and the scissor state.  It is done for every per-eye draw while the game has the plane on, so for the water of a race too.  With `Log=1` the first six planes carried are logged: `reflection clip plane: the game's (...) is (...) for eye N`.  The showroom's reflection is drawn like the water of a race again: no special case in `ClassifyDraw`, `ReflectionCameraForCulling` rewrites the frustum for the mirrored eyes, and the floor's lookup is remapped with the record of the pass.  Not yet seen in the headset.

**Checks.**  `tools/hotpath_check.py` also runs the `mirror` mode: the draws of the mirrored camera mark their target (axis Y), the main camera's do not, a camera 0.1 m under the main one, the rear-view camera (all three axes turned), a stale main pose and a camera 40 m aside are refused, a Reset forgets the surface, a marked main-sized target gets exactly the constants of the half-size pass in 40 of 40 draws (an unmarked one does not), a draw into the marked target is issued once for each eye, rewritten, in the showroom and in a race (20 of 20 each), and the game's clip plane is carried to each eye: with a world plane and the game's clip-space plane worked out the way the engine does it (the inverse of view * projection times the plane), the plane the device holds for each eye and the vertex constants it gets keep exactly the points the world plane keeps (80 of 80 draws, worst relative error 1e-7), it is not the game's own numbers, the game's plane is back after the eyes, and with the game's clip plane off the mod does not touch the device's.  The hashes of the other variants did not change.
`tools/reflection_cull_check.py` runs half of its 200 chains as the showroom and half as a race: every frustum is rewritten and contains the mirrored eyes' view.
