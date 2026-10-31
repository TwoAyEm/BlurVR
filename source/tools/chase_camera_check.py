#!/usr/bin/env python3
"""Build and run tools/chase_camera_check.cpp: a host check of src/chase_camera.cpp (the cockpit view's seat in the car's frame, [VR] CockpitView, NOTES.md section 29),
and, when a dump of the game is given, that the pattern of the chase camera's update matches it exactly once.

    python tools/chase_camera_check.py [--src src] [--exe path\\to\\Blur-Decrypted.exe]

Needs Python 3 and the Zig compiler (`pip install ziglang`).
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
# tools\dis.py would shadow the standard library module of the same name
sys.path = [p for p in sys.path if os.path.abspath(p or ".") != HERE]


def pattern_matches(src, exe):
    text = open(os.path.join(src, 'chase_camera.cpp'), encoding='utf-8').read()
    m = re.search(r'kUpdatePattern\s*=\s*((?:\s*"[^"]*")+)\s*;', text)
    if not m:
        sys.exit('the pattern was not found in chase_camera.cpp')
    words = ''.join(re.findall(r'"([^"]*)"', m.group(1))).split()
    regex = b''.join(b'.' if w == '??' else re.escape(bytes([int(w, 16)])) for w in words)
    data = open(exe, 'rb').read()
    found = [hex(x.start()) for x in re.finditer(regex, data, re.S)]
    print('pattern: %d bytes, %d match(es) in %s at file offset %s' % (len(words), len(found), os.path.basename(exe), ', '.join(found)))
    return len(found) == 1


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--src', default=os.path.join(HERE, '..', 'src'))
    parser.add_argument('--exe', default=None)
    args = parser.parse_args()
    src = os.path.abspath(args.src)
    third_party = os.path.join(os.path.dirname(src), 'third_party')
    ok = True
    if args.exe:
        ok = pattern_matches(src, args.exe)
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, 'chase_camera_check.exe')
        command = [sys.executable, '-m', 'ziglang', 'c++', '-target', 'x86-windows-gnu', '-O1', '-Wall', '-std=c++17', '-fno-exceptions', '-fno-rtti', '-I', src, '-I', third_party,
                   os.path.join(HERE, 'chase_camera_check.cpp'), os.path.join(src, 'matrix.cpp'), '-luser32', '-lkernel32', '-o', exe]
        result = subprocess.run(command, capture_output=True, text=True)
        if result.returncode != 0 or not os.path.exists(exe):
            print(result.stdout + result.stderr)
            sys.exit('build failed')
        if result.stderr.strip():
            print(result.stderr)
        result = subprocess.run([exe] + ([os.path.abspath(args.exe)] if args.exe else []), capture_output=True, text=True)
        sys.stdout.write(result.stdout + result.stderr)
        return 0 if ok and result.returncode == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
