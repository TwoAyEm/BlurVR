#!/usr/bin/env python3
"""Check that the flat display mode is put back in settings.xml when the game ends (src/video_settings.cpp, NOTES.md section 5).

    python tools/exit_restore_check.py [--dll path\\to\\d3d9.dll]

Blur ends with TerminateProcess(GetCurrentProcess(), 0), which skips DLL_PROCESS_DETACH, so the proxy hooks that import.  This builds the proxy
(or takes --dll) and a stand-in executable (tools/exit_restore_check.cpp) that loads it in a scratch folder with its own APPDATA, as the game would,
and ends in three ways: TerminateProcess of itself, a normal exit, and TerminateProcess of another process first.  After each, settings.xml must hold the
flat mode again (width, height, windowed, x, y; its other attributes untouched) and display_backup.ini must say the session is closed.  The other-process
case must leave the VR mode in place until the process itself ends.  Needs Python with `pip install ziglang`.
"""
import argparse
import glob
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SOURCE = os.path.abspath(os.path.join(HERE, '..'))
# tools\dis.py would shadow the standard library module of the same name
sys.path = [p for p in sys.path if os.path.abspath(p or ".") != HERE]

SETTINGS = ('<?xml version="1.0"?>\n<settings>\n'
            '<shared_graphical_settings width="1920" height="1080" x="10" y="20" multisample="3" windowed="0" vsync="1" graphical_detail="4"/>\n'
            '</settings>\n')
FLAT = ('width="1920"', 'height="1080"', 'x="10"', 'y="20"', 'multisample="3"', 'windowed="0"', 'vsync="1"', 'graphical_detail="4"')
INI = '[General]\nEnabled=1\nStereo=1\nLog=1\n[VR]\nResolution=4032x2240\n'

failures = []


def check(ok, text):
    print(('  ok   ' if ok else '  FAIL ') + text)
    if not ok:
        failures.append(text)


def build_proxy(out):
    sources = sorted(glob.glob(os.path.join(SOURCE, 'src', '*.cpp')))
    command = [sys.executable, '-m', 'ziglang', 'c++', '-target', 'x86-windows-gnu', '-O1', '-shared', '-fno-exceptions', '-fno-rtti', '-std=c++17',
               '-I', os.path.join(SOURCE, 'third_party'), '-o', out] + sources + [os.path.join(SOURCE, 'd3d9.def'), '-ld3d11', '-ldxgi', '-lgdi32', '-luser32',
                                                                                  '-lkernel32']
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0 or not os.path.exists(out):
        print(result.stdout + result.stderr)
        sys.exit('building the proxy failed')


def build_host(out):
    command = [sys.executable, '-m', 'ziglang', 'c++', '-target', 'x86-windows-gnu', '-O1', '-o', out, os.path.join(HERE, 'exit_restore_check.cpp'), '-lkernel32']
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0 or not os.path.exists(out):
        print(result.stdout + result.stderr)
        sys.exit('building the stand-in executable failed')


def run_case(proxy, host, how):
    print(how + ':')
    with tempfile.TemporaryDirectory() as tmp:
        game = os.path.join(tmp, 'game')
        os.makedirs(os.path.join(game, 'BlurVR'))
        shutil.copy(proxy, os.path.join(game, 'd3d9.dll'))
        shutil.copy(host, os.path.join(game, 'exit_restore_check.exe'))
        with open(os.path.join(game, 'BlurVR', 'BlurVR.ini'), 'w') as f:
            f.write(INI)
        appdata = os.path.join(tmp, 'appdata')
        folder = os.path.join(appdata, 'bizarre creations', 'blur')
        os.makedirs(folder)
        settings = os.path.join(folder, 'settings.xml')
        with open(settings, 'w') as f:
            f.write(SETTINGS)
        env = dict(os.environ, APPDATA=appdata)
        result = subprocess.run([os.path.join(game, 'exit_restore_check.exe'), os.path.join(game, 'd3d9.dll'), how, settings], capture_output=True, text=True,
                                env=env, cwd=game, timeout=60)
        if result.stdout.strip():
            print('  ' + result.stdout.strip().replace('\n', '\n  '))
        with open(settings) as f:
            text = f.read()
        with open(os.path.join(game, 'BlurVR', 'display_backup.ini')) as f:
            sidecar = f.read()
        log = ''
        if os.path.exists(os.path.join(game, 'BlurVR', 'BlurVR.log')):
            with open(os.path.join(game, 'BlurVR', 'BlurVR.log')) as f:
                log = f.read()
        check('the game starts at 4032x2240' in log, 'the start put the VR mode in (log)')
        missing = [a for a in FLAT if a not in text]
        check(not missing, 'settings.xml holds the flat mode again and its other attributes' + (' (missing: %s)' % ' '.join(missing) if missing else ''))
        check('Open=0' in sidecar, 'display_backup.ini says the session is closed')
        check('display mode 1920x1080 is back' in log, 'the log says the flat mode is back')
        if how == 'terminate' or how == 'other':
            check('TerminateProcess hooked' in log, 'the log says TerminateProcess was hooked')
        if how == 'other':
            check('still holds the VR mode' in result.stdout, 'the end of another process left the VR mode in settings.xml')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--dll', default=None, help='a built d3d9.dll to check instead of building one')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as tmp:
        proxy = os.path.abspath(args.dll) if args.dll else os.path.join(tmp, 'd3d9.dll')
        if not args.dll:
            build_proxy(proxy)
        host = os.path.join(tmp, 'exit_restore_check.exe')
        build_host(host)
        for how in ('terminate', 'exit', 'other'):
            run_case(proxy, host, how)
    if failures:
        print('exit_restore_check: %d check(s) failed' % len(failures))
        return 1
    print('exit_restore_check: all checks passed')
    return 0


if __name__ == '__main__':
    sys.exit(main())
