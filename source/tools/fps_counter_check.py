#!/usr/bin/env python3
"""Build and run tools/fps_counter_check.cpp: a host check of src/fps_counter.cpp (the frame rate counter in the headset, [VR] FpsCounter, NOTES.md section 28).

    python tools/fps_counter_check.py [--src src] [--pictures folder]

Checks how frames are counted, the colours and the font and, on a real Direct3D 9 / 9Ex device, that the panel is drawn into both eyes where it should be, reads back as the figure,
stays put when the head moves, touches nothing outside itself and leaves the device state as it was.  --pictures writes the panel as .bmp files to look at.  Needs Python with
`pip install ziglang`.
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
    parser.add_argument('--pictures', default=None)
    args = parser.parse_args()
    src = os.path.abspath(args.src)
    third_party = os.path.join(os.path.dirname(src), 'third_party')
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, 'fps_counter_check.exe')
        command = [sys.executable, '-m', 'ziglang', 'c++', '-target', 'x86-windows-gnu', '-O1', '-Wall', '-std=c++17', '-fno-exceptions', '-fno-rtti', '-I', src, '-I', third_party,
                   os.path.join(HERE, 'fps_counter_check.cpp'), os.path.join(src, 'fps_counter.cpp'), os.path.join(src, 'matrix.cpp'), '-ld3d9', '-ld3d11', '-ldxgi', '-lgdi32',
                   '-luser32', '-lkernel32', '-o', exe]
        result = subprocess.run(command, capture_output=True, text=True)
        if result.returncode != 0 or not os.path.exists(exe):
            print(result.stdout + result.stderr)
            sys.exit('build failed')
        if result.stderr.strip():
            print(result.stderr)
        run = [exe]
        if args.pictures:
            os.makedirs(args.pictures, exist_ok=True)
            run.append(os.path.abspath(args.pictures))
        result = subprocess.run(run, capture_output=True, text=True)
        sys.stdout.write(result.stdout + result.stderr)
        return result.returncode


if __name__ == '__main__':
    sys.exit(main())
