#!/usr/bin/env python3
"""Check a release zip made by make_release.py: the right files, and no source code in it.

    python tools/check_release_zip.py ../dist/BlurVR-1.3.zip [tag]

With a tag (v1.3) it also checks that the tag is 'v' + the version in src/core.h.  Exits 1 on any problem.
"""
import os
import re
import struct
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
EXPECTED = {'BlurVR.ini', 'LICENSE_openvr.txt', 'README.md', 'd3d9.dll', 'install.bat', 'openvr_api.dll', 'uninstall.bat'}
SOURCE_EXT = {'.cpp', '.c', '.h', '.hpp', '.py', '.def', '.md', '.patch', '.sh', '.clang-format', '.obj', '.lib', '.pdb'}


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    path = sys.argv[1]
    problems = []
    with zipfile.ZipFile(path) as z:
        names = [n for n in z.namelist() if not n.endswith('/')]
        for n in names:
            parts = n.split('/')
            if parts[0] != 'BlurVR':
                problems.append('not inside the BlurVR folder: ' + n)
            elif len(parts) > 2 or parts[1] == 'source':
                problems.append('unexpected file (only the top level of BlurVR is expected): ' + n)
            if (os.path.splitext(n)[1].lower() in SOURCE_EXT and parts[-1] != 'README.md') or parts[-1] == 'build.bat':
                problems.append('looks like source code: ' + n)
        have = {n.split('/')[1] for n in names if n.count('/') == 1}
        for missing in sorted(EXPECTED - have):
            problems.append('missing: ' + missing)
        for extra in sorted(have - EXPECTED):
            problems.append('not expected: ' + extra)
        if 'BlurVR/d3d9.dll' in names:
            dll = z.read('BlurVR/d3d9.dll')
            pe = struct.unpack_from('<I', dll, 0x3C)[0]
            if dll[:2] != b'MZ' or dll[pe:pe + 4] != b'PE\0\0' or struct.unpack_from('<H', dll, pe + 4)[0] != 0x14C:
                problems.append('d3d9.dll is not a 32-bit (x86) Windows DLL')
    version = re.search(r'#define\s+BLURVR_VERSION\s+"([^"]+)"', open(os.path.join(HERE, '..', 'src', 'core.h')).read()).group(1)
    if os.path.basename(path) != 'BlurVR-%s.zip' % version:
        problems.append('the zip is named %s but the version in core.h is %s' % (os.path.basename(path), version))
    if len(sys.argv) > 2 and sys.argv[2] != 'v' + version:
        problems.append('the tag is %s but the version in core.h is %s (expected v%s)' % (sys.argv[2], version, version))
    if problems:
        print('\n'.join(problems))
        sys.exit(1)
    print('%s: ok (%d files, version %s, no source)' % (os.path.basename(path), len(names), version))


if __name__ == '__main__':
    main()
