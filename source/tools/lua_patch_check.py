#!/usr/bin/env python3
"""Build and run tools/lua_patch_check.cpp: a host check of src/lua_patch.cpp (the patch of the game's fan screen script, NOTES.md section 31).

    python tools/lua_patch_check.py [--src src] [--exe path\\to\\the decrypted game exe]

The game keeps its UI scripts (Lua 5.1 bytecode) as zlib streams in its image; this finds the one with the fan screen (it has "hasUnlockedVehicles" and "ranked_up"), writes it to a
temporary file and runs the check on it.  Needs the decrypted exe (default: Blur-Decrypted.exe in the game folder) and Python with `pip install ziglang`.
"""
import argparse
import os
import subprocess
import sys
import tempfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
# tools\dis.py would shadow the standard library module of the same name
sys.path = [p for p in sys.path if os.path.abspath(p or ".") != HERE]


def find_script(exe_path):
    data = open(exe_path, 'rb').read()
    pos = 0
    while True:
        pos = data.find(b'x\xda', pos)
        if pos < 0:
            return None
        pos += 1
        try:
            z = zlib.decompressobj()
            out = z.decompress(data[pos - 1:pos - 1 + (1 << 20)])
        except zlib.error:
            continue
        if out[:5] == b'\x1bLuaQ' and b'hasUnlockedVehicles' in out and b'ranked_up' in out:
            return out


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--src', default=os.path.join(HERE, '..', 'src'))
    parser.add_argument('--exe', default=os.path.join(HERE, '..', '..', '..', 'Blur-Decrypted.exe'))
    args = parser.parse_args()
    src = os.path.abspath(args.src)
    if not os.path.exists(args.exe):
        sys.exit('the decrypted game exe is not at ' + args.exe + ' (pass --exe)')
    exe_bytes = open(args.exe, 'rb').read()
    sig = bytes([0x8B, 0x4C, 0x24, 0x10, 0x8A, 0x01, 0x83, 0xEC, 0x14, 0x3C, 0x40, 0x74, 0x12, 0x3C, 0x3D, 0x74, 0x0E, 0x3C, 0x1B, 0x75, 0x0B])  # as in src/lua_patch.cpp
    if exe_bytes.count(sig) != 1:
        sys.exit('the Lua loader signature of src/lua_patch.cpp is found %d times in %s (expected 1)' % (exe_bytes.count(sig), args.exe))
    print('loader signature: found once')
    script = find_script(args.exe)
    if script is None:
        sys.exit('the fan screen script was not found in ' + args.exe)
    with tempfile.TemporaryDirectory() as tmp:
        chunk = os.path.join(tmp, 'unlocks.luac')
        open(chunk, 'wb').write(script)
        exe = os.path.join(tmp, 'lua_script_check.exe')
        command = [sys.executable, '-m', 'ziglang', 'c++', '-target', 'x86-windows-gnu', '-O1', '-Wall', '-std=c++17', '-fno-exceptions', '-fno-rtti', '-I', src,
                   os.path.join(HERE, 'lua_patch_check.cpp'), '-lkernel32', '-o', exe]
        result = subprocess.run(command, capture_output=True, text=True)
        if result.returncode != 0 or not os.path.exists(exe):
            print(result.stdout + result.stderr)
            sys.exit('build failed')
        if result.stderr.strip():
            print(result.stderr)
        result = subprocess.run([exe, chunk], capture_output=True, text=True)
        sys.stdout.write(result.stdout + result.stderr)
        return result.returncode


if __name__ == '__main__':
    sys.exit(main())
