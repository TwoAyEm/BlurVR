#!/usr/bin/env python3
"""Build and run tools/hotpath_check.cpp against a source folder (see the comment at the top of that file).

    python tools/hotpath_check.py --src src                         # check hash + benchmark of the sources in src
    python tools/hotpath_check.py --src src --compare other_src     # also build other_src and compare the check hashes

Needs Python with `pip install ziglang`.  Prints the hash of everything the mod sent to the fake device over a number of
synthetic frames (equal hashes = identical rendering commands) and the nanoseconds per draw for each kind of draw.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
# tools\dis.py would shadow the standard library module of the same name
sys.path = [p for p in sys.path if os.path.abspath(p or ".") != HERE]

import argparse
import glob
import subprocess
import tempfile


def build(src, exe):
    sources = [f for f in sorted(glob.glob(os.path.join(src, '*.cpp'))) if os.path.basename(f) != 'dllmain.cpp']
    third_party = os.path.join(os.path.dirname(os.path.abspath(src)), 'third_party')
    if not os.path.isdir(third_party):
        third_party = os.path.join(HERE, '..', 'third_party')
    command = [sys.executable, '-m', 'ziglang', 'c++', '-target', 'x86-windows-gnu', '-O2', '-std=c++17', '-fno-exceptions',
               '-fno-rtti', '-I', src, '-I', third_party, os.path.join(HERE, 'hotpath_check.cpp')] + sources + \
              ['-ld3d11', '-ldxgi', '-luser32', '-lkernel32', '-o', exe]
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0 or not os.path.exists(exe):
        print(result.stdout + result.stderr)
        sys.exit('build failed for ' + src)


def run(exe, *args):
    result = subprocess.run([exe] + list(args), capture_output=True, text=True)
    if result.returncode != 0:
        print(result.stdout + result.stderr)
        sys.exit('%s %s failed (%d)' % (exe, ' '.join(args), result.returncode))
    return result.stdout


VARIANTS = ['default', 'transposed matrices', 'mono', 'seam gutter', 'reflection flip mode 2', 'no reflection lookup fix',
            'no view rotation adjustment', 'game scissor test on']


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--src', required=True)
    parser.add_argument('--compare')
    parser.add_argument('--frames', default='60')
    parser.add_argument('--no-bench', action='store_true')
    args = parser.parse_args()
    compared = ('hash', 'draws')
    status = 0
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, 'hotpath_a.exe')
        build(args.src, exe)
        other = None
        if args.compare:
            other = os.path.join(tmp, 'hotpath_b.exe')
            build(args.compare, other)
        for number, name in enumerate(VARIANTS):
            check = run(exe, 'check', args.frames, str(number))
            keep = [l for l in check.splitlines() if l.startswith(compared)]
            line = '%-28s %s' % (name, ', '.join(keep))
            if other:
                other_check = run(other, 'check', args.frames, str(number))
                same = keep == [l for l in other_check.splitlines() if l.startswith(compared)]
                line += '   ' + ('identical' if same else 'DIFFERENT')
                status |= 0 if same else 1
                if number == 0:
                    print('device calls, default variant:\n  [%s] %s\n  [%s] %s' % (
                        args.src, [l for l in check.splitlines() if l.startswith('calls')][0],
                        args.compare, [l for l in other_check.splitlines() if l.startswith('calls')][0]))
            print(line)
        if other:
            print('IDENTICAL in every variant: the builds sent the same state to the device at every draw.' if not status else
                  'DIFFERENT: the builds do not render identically.')
        if not args.no_bench:
            for cost in ('0', '120'):
                print('[%s] bench, device call cost %s ns\n%s' % (args.src, cost, run(exe, 'bench', cost)))
                if other:
                    print('[%s] bench, device call cost %s ns\n%s' % (args.compare, cost, run(other, 'bench', cost)))
    return status


if __name__ == '__main__':
    sys.exit(main())
