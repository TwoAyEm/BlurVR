#!/usr/bin/env python3
"""Run the camera-function scan of src/game_signatures.cpp (the code the proxy uses to find the game's camera hand-over) on Blur.exe files.

    python tools/camera_sig_check.py <Blur.exe or dump.exe> [more files]

Prints, for each file: size, PE time stamp, sections, how often each pattern matches and, when both functions are found exactly once,
their addresses.  Exit code 1 when any file is not resolved.  A dump of the running module (every section at its own address in the file)
works like an executable; an executable that is encrypted on disk is reported as such and has to be dumped from the running game.
Needs Python with `pip install ziglang`.
"""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, '..', 'src')


def build(exe):
    command = [sys.executable, '-m', 'ziglang', 'c++', '-O2', '-std=c++17', '-fno-exceptions', '-fno-rtti', os.path.join(HERE, 'camera_sig_check.cpp'),
               os.path.join(SRC, 'game_signatures.cpp'), '-o', exe]
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0 or not os.path.exists(exe):
        print(result.stdout + result.stderr)
        sys.exit('build failed')


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    with tempfile.TemporaryDirectory() as folder:
        exe = os.path.join(folder, 'camera_sig_check.exe')
        build(exe)
        result = subprocess.run([exe] + sys.argv[1:], capture_output=True, text=True)
        sys.stdout.write(result.stdout + result.stderr)
        return result.returncode


if __name__ == '__main__':
    sys.exit(main())
