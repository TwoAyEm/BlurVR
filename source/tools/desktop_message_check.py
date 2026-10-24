#!/usr/bin/env python3
"""Build and run tools/desktop_message_check.cpp: a host check of src/desktop_message.cpp (the "Playing in VR" window, [VR] DesktopView=off, NOTES.md section 20).

    python tools/desktop_message_check.py [--src src] [--exe ..\\..\\Blur.exe] [--pictures folder]

Checks the layout of the picture (with the logo taken from the game's executable when it is given) and, on a real Direct3D 9 device in a hidden window, that it reaches the
back buffer pixel for pixel.  --pictures writes the pictures as .bmp files to look at.  Needs Python with `pip install ziglang`.
"""
import argparse
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
    parser.add_argument('--exe', default=os.path.join(HERE, '..', '..', '..', 'Blur.exe'))
    parser.add_argument('--pictures', default=None)
    args = parser.parse_args()
    src = os.path.abspath(args.src)
    third_party = os.path.join(os.path.dirname(src), 'third_party')
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, 'desktop_message_check.exe')
        command = [sys.executable, '-m', 'ziglang', 'c++', '-target', 'x86-windows-gnu', '-O1', '-std=c++17', '-fno-exceptions', '-fno-rtti', '-I', src, '-I', third_party,
                   os.path.join(HERE, 'desktop_message_check.cpp'), os.path.join(src, 'desktop_message.cpp'), '-ld3d9', '-ld3d11', '-ldxgi', '-lgdi32', '-luser32', '-lkernel32',
                   '-o', exe]
        result = subprocess.run(command, capture_output=True, text=True)
        if result.returncode != 0 or not os.path.exists(exe):
            print(result.stdout + result.stderr)
            sys.exit('build failed')
        run = [exe]
        exe_path = os.path.abspath(args.exe)
        run.append(exe_path if os.path.exists(exe_path) else '')
        if args.pictures:
            os.makedirs(args.pictures, exist_ok=True)
            run.append(os.path.abspath(args.pictures))
        result = subprocess.run(run, capture_output=True, text=True)
        sys.stdout.write(result.stdout + result.stderr)
        return result.returncode


if __name__ == '__main__':
    sys.exit(main())
