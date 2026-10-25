#!/usr/bin/env python3
"""Host check of the OpenXR backend (src/openxr_runtime.cpp, NOTES.md section 27), without the game.

    python tools/openxr_check.py            # the cases that need no headset and start no runtime
    python tools/openxr_check.py --system   # also start the installed OpenXR runtime and run it with test frames (SteamVR with a headset on, or any 32-bit runtime)

It builds tools/openxr_check.cpp (a stand-in for the game that links openxr_loader.dll's caller, openxr_runtime.cpp, as the mod has it) and runs it in scratch
BlurVR folders holding the 32-bit openxr_loader.dll of the repository and a BlurVR.ini:
  probe       the 32-bit loader finds a 32-bit OpenXR runtime and that runtime offers XR_KHR_D3D11_enable (needs one registered under WOW6432Node; no
              instance is created, so no runtime is started),
  bogus json  [VR] OpenXRRuntime= pointing at a file that is no runtime: OpenXR does not start, cleanly, and the log names the setting,
  no loader   a BlurVR folder without openxr_loader.dll: OpenXR does not start, cleanly, and the log says so,
  --system    the whole start (instance, system, Direct3D 11 device, session, field of view) and frames until the runtime delivers a pose (up to 25 s), then 60 race frames, 30 menu
              frames.  Without a headset this must end at a logged reason (never a crash); with one it must run and print a pose.
Needs Python with `pip install ziglang`.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SOURCE = os.path.abspath(os.path.join(HERE, '..'))
ROOT = os.path.abspath(os.path.join(SOURCE, '..'))
# tools\dis.py would shadow the standard library module of the same name
sys.path = [p for p in sys.path if os.path.abspath(p or ".") != HERE]

failures = []


def check(ok, text):
    print(('  ok   ' if ok else '  FAIL ') + text)
    if not ok:
        failures.append(text)


def build(out):
    src = os.path.join(SOURCE, 'src')
    command = [sys.executable, '-m', 'ziglang', 'c++', '-target', 'x86-windows-gnu', '-O1', '-Wall', '-std=c++17', '-fno-exceptions', '-fno-rtti',
               '-I', src, '-I', os.path.join(SOURCE, 'third_party'), os.path.join(HERE, 'openxr_check.cpp'), os.path.join(src, 'openxr_runtime.cpp'),
               os.path.join(src, 'core.cpp'), os.path.join(src, 'config.cpp'), '-ld3d11', '-ldxgi', '-lgdi32', '-luser32', '-lkernel32', '-o', out]
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0 or not os.path.exists(out):
        print(result.stdout + result.stderr)
        sys.exit('building the check failed')
    if result.stderr.strip():
        print(result.stderr)


def run_case(exe, tmp, name, mode, ini, loader=True, timeout=120):
    print(name + ':')
    folder = os.path.join(tmp, name.replace(' ', '_'), 'BlurVR')
    os.makedirs(folder)
    if loader:
        shutil.copy(os.path.join(ROOT, 'openxr_loader.dll'), folder)
    with open(os.path.join(folder, 'BlurVR.ini'), 'w') as f:
        f.write(ini)
    result = subprocess.run([exe, folder, mode], capture_output=True, text=True, timeout=timeout)
    log_path = os.path.join(folder, 'BlurVR.log')
    log = open(log_path, errors='replace').read() if os.path.exists(log_path) else ''
    for line in (result.stdout + log).splitlines():
        print('    ' + line)
    return result.returncode, result.stdout, log


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--system', action='store_true', help='also start the installed OpenXR runtime')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, 'openxr_check.exe')
        build(exe)
        base = '[General]\nEnabled=1\nStereo=1\nLog=1\n[VR]\nApi=openxr\n'

        code, out, log = run_case(exe, tmp, 'probe', 'probe', base)
        check(code == 0 and 'XR_KHR_D3D11_enable yes' in out, 'the 32-bit loader reaches a 32-bit runtime that offers Direct3D 11')

        bogus = os.path.join(tmp, 'not_a_runtime.json')
        open(bogus, 'w').write('{}')
        code, out, log = run_case(exe, tmp, 'bogus json', 'full', base + 'OpenXRRuntime=' + bogus + '\n')
        check(code == 1 and 'runtime from [VR] OpenXRRuntime' in log and 'XrRuntimeInit failed' in out, 'a json that is no runtime: OpenXR does not start, the log names the setting')

        code, out, log = run_case(exe, tmp, 'no loader', 'full', base, loader=False)
        check(code == 1 and 'could not load openxr_loader.dll' in log, 'no openxr_loader.dll: OpenXR does not start and the log says so')

        if args.system:
            code, out, log = run_case(exe, tmp, 'system runtime', 'full', base)
            if code == 0:
                check('race frames submitted: 60, menu frames submitted: 30' in out and 'first head pose' in out and 'failed' not in log,
                      'the runtime started, poses came and the test frames were submitted without an error')
            else:
                known = ('reports no headset', 'xrCreateInstance failed', 'listing the runtime', 'did not become ready', 'did not deliver')
                check(code == 1 and any(k in log for k in known), 'it did not start, with a reason in the log (no headset / no runtime): ' + str(code))
    if failures:
        print('\n%d check(s) failed' % len(failures))
        return 1
    print('\nOK')
    return 0


if __name__ == '__main__':
    sys.exit(main())
