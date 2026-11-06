#!/usr/bin/env python3
"""Build and run tools/upscale_check.cpp: a host check of src/upscale_d3d11.cpp (the upscaling of the race picture on the Direct3D 11 side, NOTES.md section 32).

    python tools/upscale_check.py [--src src]

Runs the real shaders on the software rasteriser (WARP): a smooth gradient stays a gradient at the new size, a step edge stays one step, the two eyes do not leak into each
other, a flat picture stays flat, the sharpening does not overshoot, and the same size gives the picture back.  Needs Python with `pip install ziglang`.
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
    args = parser.parse_args()
    src = os.path.abspath(args.src)
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, 'upscale_check.exe')
        command = [sys.executable, '-m', 'ziglang', 'c++', '-target', 'x86-windows-gnu', '-O1', '-Wall', '-std=c++17', '-fno-exceptions', '-fno-rtti', '-I', src,
                   os.path.join(HERE, 'upscale_check.cpp'), '-ld3d11', '-ldxgi', '-lgdi32', '-luser32', '-lkernel32', '-o', exe]
        result = subprocess.run(command, capture_output=True, text=True)
        if result.returncode != 0 or not os.path.exists(exe):
            print(result.stdout + result.stderr)
            sys.exit('build failed')
        if result.stderr.strip():
            print(result.stderr)
        result = subprocess.run([exe], capture_output=True, text=True)
        sys.stdout.write(result.stdout + result.stderr)
        return result.returncode


if __name__ == '__main__':
    sys.exit(main())
