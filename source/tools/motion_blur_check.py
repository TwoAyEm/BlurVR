#!/usr/bin/env python3
"""Build and run tools/motion_blur_check.cpp: a host check of the motion blur switch ([VR] MotionBlur, NOTES.md section 5) on a real Direct3D 9 device.

    python tools/motion_blur_check.py [--src src]

The mod is linked as it is (every src/*.cpp except dllmain.cpp).  A stand-in for the game's blur shader (ps_2_0 and ps_3_0, with the constant table the mod recognises the blur by) is drawn through
MotionBlurPerEye: with MotionBlur=0 the pass has to copy the picture (alpha forced to 1) and leave the game's shader, the tracking and the device state as they were; with MotionBlur=1, or when
the bound shader is not known, the blur has to be drawn once per eye as before.  Needs Python with `pip install ziglang` and a Direct3D 9 device (it prints SKIP without one).
"""
import argparse
import glob
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
# tools\dis.py would shadow the standard library module of the same name
sys.path = [p for p in sys.path if os.path.abspath(p or ".") != HERE]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--src', default=os.path.join(HERE, '..', 'src'))
    args = parser.parse_args()
    src = os.path.abspath(args.src)
    third_party = os.path.join(os.path.dirname(src), 'third_party')
    sources = [f for f in sorted(glob.glob(os.path.join(src, '*.cpp'))) if os.path.basename(f) != 'dllmain.cpp']
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, 'motion_blur_check.exe')
        command = [sys.executable, '-m', 'ziglang', 'c++', '-target', 'x86-windows-gnu', '-O1', '-std=c++17', '-fno-exceptions', '-fno-rtti', '-I', src, '-I', third_party,
                   os.path.join(HERE, 'motion_blur_check.cpp')] + sources + ['-ld3d9', '-ld3d11', '-ldxgi', '-lgdi32', '-luser32', '-lkernel32', '-o', exe]
        result = subprocess.run(command, capture_output=True, text=True)
        if result.returncode != 0 or not os.path.exists(exe):
            print(result.stdout + result.stderr)
            sys.exit('build failed')
        result = subprocess.run([exe], capture_output=True, text=True)
        sys.stdout.write(result.stdout + result.stderr)
        return result.returncode


if __name__ == '__main__':
    sys.exit(main())
