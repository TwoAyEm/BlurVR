#!/usr/bin/env python3
"""Build and run tools/ai_hud_check.cpp: a host check of src/ai_hud.cpp (the AI car markers, NOTES.md section 26).

    python tools/ai_hud_check.py [--src src]

It links every src/*.cpp except dllmain.cpp and ai_hud.cpp (the check includes that one itself) with a fake "game" function that starts with the
real bytes of the UIAiHud update, so the signature scan, the inline hook and the slot reading run for real.  Needs Python with `pip install ziglang`.
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
    sources = [f for f in sorted(glob.glob(os.path.join(src, '*.cpp'))) if os.path.basename(f) not in ('dllmain.cpp', 'ai_hud.cpp')]
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, 'ai_hud_check.exe')
        command = [sys.executable, '-m', 'ziglang', 'c++', '-target', 'x86-windows-gnu', '-O1', '-std=c++17', '-fno-exceptions', '-fno-rtti', '-I', src, '-I',
                   third_party, os.path.join(HERE, 'ai_hud_check.cpp')] + sources + ['-ld3d11', '-ldxgi', '-lgdi32', '-luser32', '-lkernel32', '-o', exe]
        result = subprocess.run(command, capture_output=True, text=True)
        if result.returncode != 0 or not os.path.exists(exe):
            print(result.stdout + result.stderr)
            sys.exit('build failed')
        result = subprocess.run([exe], capture_output=True, text=True)
        sys.stdout.write(result.stdout + result.stderr)
        return result.returncode


if __name__ == '__main__':
    sys.exit(main())
