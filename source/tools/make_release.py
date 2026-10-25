#!/usr/bin/env python3
"""Build the release zip: dist/BlurVR-<version>.zip, one top-level folder BlurVR\\ that is unzipped into the Blur folder.

    python tools/make_release.py            # uses source/d3d9.dll as it is (run build.bat nodeploy first)

Contents: d3d9.dll (the build in source/), openvr_api.dll, openxr_loader.dll (32-bit, for [VR] Api=openxr), the small release ini
(source/release/BlurVR.ini), README.md, install.bat, uninstall.bat, LICENSE_openvr.txt and LICENSE_openxr.txt.  No source code: that is in the repository
(tools/check_release_zip.py checks the finished zip).  Checks first: the release ini only has keys the code reads,
with values that are valid; the DLL is not older than the sources.
"""
import os
import re
import subprocess
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
SOURCE = os.path.abspath(os.path.join(HERE, '..'))
ROOT = os.path.abspath(os.path.join(SOURCE, '..'))
DIST = os.path.join(ROOT, 'dist')


def version():
    text = open(os.path.join(SOURCE, 'src', 'core.h'), encoding='utf-8').read()
    return re.search(r'#define\s+BLURVR_VERSION\s+"([^"]+)"', text).group(1)


def newest_source():
    newest = 0
    for folder, _, files in os.walk(os.path.join(SOURCE, 'src')):
        for name in files:
            newest = max(newest, os.path.getmtime(os.path.join(folder, name)))
    return newest


def check_release_ini(path):
    # unknown keys are typos: reuse the ini checker (it exits 1 for any difference, so look for the 'not read' list only)
    result = subprocess.run([sys.executable, os.path.join(HERE, 'check_ini_defaults.py'), path], capture_output=True, text=True)
    out = result.stdout
    if 'not read' in out.lower() or 'unknown' in out.lower():
        print(out)
        sys.exit('the release ini has keys the code does not read')


def main():
    ver = version()
    dll = os.path.join(SOURCE, 'd3d9.dll')
    if not os.path.exists(dll):
        sys.exit('source/d3d9.dll is missing: run build.bat nodeploy')
    if os.path.getmtime(dll) < newest_source():
        sys.exit('source/d3d9.dll is older than the sources: run build.bat nodeploy')
    release_ini = os.path.join(SOURCE, 'release', 'BlurVR.ini')
    check_release_ini(release_ini)

    files = [
        (dll, 'd3d9.dll'),
        (os.path.join(ROOT, 'openvr_api.dll'), 'openvr_api.dll'),
        (os.path.join(ROOT, 'openxr_loader.dll'), 'openxr_loader.dll'),
        (release_ini, 'BlurVR.ini'),
        (os.path.join(ROOT, 'README.md'), 'README.md'),
        (os.path.join(ROOT, 'install.bat'), 'install.bat'),
        (os.path.join(ROOT, 'uninstall.bat'), 'uninstall.bat'),
        (os.path.join(ROOT, 'LICENSE_openvr.txt'), 'LICENSE_openvr.txt'),
        (os.path.join(ROOT, 'LICENSE_openxr.txt'), 'LICENSE_openxr.txt'),
    ]
    for path, _ in files:
        if not os.path.exists(path):
            sys.exit('missing: ' + path)

    os.makedirs(DIST, exist_ok=True)
    target = os.path.join(DIST, 'BlurVR-%s.zip' % ver)
    if os.path.exists(target):
        os.remove(target)
    with zipfile.ZipFile(target, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for path, name in files:
            z.write(path, 'BlurVR/' + name)
    print('%s: %d files, %.1f KB' % (target, len(files), os.path.getsize(target) / 1024.0))
    with zipfile.ZipFile(target) as z:
        top = [n for n in z.namelist() if n.count('/') == 1]
        print('top level of BlurVR\\: ' + ', '.join(sorted(n.split('/')[1] for n in top)))


if __name__ == '__main__':
    main()
