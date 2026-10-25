# BlurVR source

Build: on Windows run `build.bat` (installs the Zig compiler through pip on first use, builds `d3d9.dll`, backs up and replaces the
game's copy; `build.bat nodeploy` only builds). On Linux: `pip install ziglang`, then `./build.sh` -> `d3d9.dll` (32-bit).
Read `NOTES.md` first: architecture, maths, what is known about Blur's renderer, known limits, and the code map (section 12).

    src/            the mod, one module per .cpp/.h pair (see NOTES.md section 12)
    third_party/    openvr_capi.h (OpenVR C API header, BSD licence)
    tools/          dis.py / ctab.py (shader disassembler, constant tables), check_equivalent.py, check_ini_defaults.py (BlurVR.ini vs the built-in defaults), hotpath_check.cpp/.py (replays a synthetic frame through RunDraw against a fake device: checks that two builds draw identically, benchmarks the per-draw path), lens_dump.cpp / lens_layouts.py (headset distortion study, NOTES section 13), shared_transport_check.cpp and ex_proxy_check.cpp (checks of the shared-texture transport, NOTES section 15), camera_sig_check.cpp / .py (scans a Blur.exe or a dump of the running game for the camera code the head frustum hooks, NOTES section 25, and for the AI HUD function of section 26), ai_hud_check.cpp / .py (host check of the AI car markers against a fake game function), desktop_message_check.cpp / .py (host check of the "Playing in VR" window, `DesktopView=off`, on a real Direct3D 9 device), exit_restore_check.cpp / .py (checks that the flat display mode goes back into settings.xml when the game ends, also with TerminateProcess), reflection_cull_check.cpp / .py (host check of the culling frustum of the planar reflection view and of the mirrored eye's place), make_release.py (builds dist/BlurVR-<version>.zip)
    release/        BlurVR.ini as it goes into the release zip (the few options most people need)
    experiments/    code that is not built: the hidden-area cull (NOTES section 17) and the diagnostics removed for 1.1 as a patch (NOTES section 19)
    .clang-format   formatting used for src/

Release: `build.bat nodeploy`, then `python tools/make_release.py` (needs the Python from the build step) and `python tools/check_release_zip.py ../dist/BlurVR-<version>.zip`.
The zip has the DLL, the small ini, the README and the install scripts, no source.  The same steps run on GitHub (`.github/workflows/build.yml`): every push to `main` builds the zip as a
workflow artifact, and pushing a tag `v<version>` (the version in `src/core.h`) also publishes it as a release.
