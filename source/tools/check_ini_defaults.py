#!/usr/bin/env python3
"""Compare BlurVR.ini with the defaults compiled into src/config.cpp.

The shipped ini (source/release/BlurVR.ini) is the "standard" configuration: every active line in it should equal the built-in default, so that the
header's promise ("anything left out uses the default shown") is true.  Run from anywhere:

    python tools/check_ini_defaults.py [path\\to\\BlurVR.ini]

Prints the keys whose active value differs from the default, the active keys the code does not read (typos, wrong section)
and the code's keys that are missing from the ini (they use the default).  Exit code 1 when anything differs or is unknown.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CONFIG_CPP = os.path.join(HERE, '..', 'src', 'config.cpp')
DEFAULT_INI = os.path.join(HERE, '..', 'release', 'BlurVR.ini')


def code_defaults(path):
    text = open(path, encoding='utf-8').read()
    found = {}
    # ReadIniInt / ReadIniDouble / ReadIniDesktopView / ReadIniApi / ReadIniString("Section", "Key", default[, "OldSection"]): the third argument is the
    # default, the optional fourth the section an older ini kept the key in (still read, not a place for a new ini to use)
    for m in re.finditer(r'ReadIni(?:Int|Double|DesktopView|Api|String)\(\s*"([^"]+)"\s*,\s*"([^"]+)"\s*,\s*([^,)]+?)\s*[,)]', text):
        found[(m.group(1).lower(), m.group(2).lower())] = m.group(3)
    # ReadIniSize("VR", "Resolution", &w, &h): written WxH, the default is what g_cfg.vrWidth / vrHeight are set to just before
    width = re.search(r'vrWidth\s*=\s*(\d+)\s*;', text)
    height = re.search(r'vrHeight\s*=\s*(\d+)\s*;', text)
    for m in re.finditer(r'ReadIniSize\(\s*"([^"]+)"\s*,\s*"([^"]+)"', text):
        found[(m.group(1).lower(), m.group(2).lower())] = '"%sx%s"' % (width.group(1) if width else '?', height.group(1) if height else '?')
    m = re.search(r'GetPrivateProfileStringA\(\s*"([^"]+)"\s*,\s*"([^"]+)"\s*,\s*"([^"]*)"', text)
    if m:
        found[(m.group(1).lower(), m.group(2).lower())] = '"%s"' % m.group(3)
    return found


def number(text):
    text = text.strip().strip('"')
    # DesktopView is written left / right / both / off (the numbers 1 / 2 / 0 / 3 of older files are read too); a size is written WxH
    text = {'left': '1', 'right': '2', 'both': '0', 'off': '3', 'kdesktopviewoff': '3', 'openvr': '0', 'openxr': '1', 'kvrapiopenvr': '0',
            'kvrapiopenxr': '1'}.get(text.lower(), text)
    text = re.sub(r'\s*[xX]\s*', 'x', text) if re.fullmatch(r'\d+\s*[xX]\s*\d+', text) else text
    try:
        return float(int(text, 0))
    except ValueError:
        try:
            return float(text)
        except ValueError:
            return text.lower()


def ini_values(path):
    section = ''
    values = {}
    for raw in open(path, encoding='utf-8', errors='replace'):
        line = raw.strip()
        if not line or line.startswith(';'):
            continue
        if line.startswith('[') and line.endswith(']'):
            section = line[1:-1].lower()
            continue
        if '=' in line:
            key, value = line.split('=', 1)
            values[(section, key.strip().lower())] = value.split(';')[0].strip()
    return values


def main():
    ini_path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_INI
    defaults = code_defaults(CONFIG_CPP)
    values = ini_values(ini_path)
    different, unknown = [], []
    for key, value in sorted(values.items()):
        if key not in defaults:
            unknown.append(key)
        elif number(value) != number(defaults[key]):
            different.append((key, value, defaults[key]))
    missing = sorted(k for k in defaults if k not in values)
    print('ini: %s' % os.path.normpath(ini_path))
    print('active keys: %d, code keys: %d' % (len(values), len(defaults)))
    if different:
        print('\nactive value differs from the default:')
        for (section, key), value, default in different:
            print('  [%s] %-24s ini %-10s default %s' % (section, key, value, default.strip('"')))
    if unknown:
        print('\nactive keys the code does not read (typo or wrong section):')
        for section, key in unknown:
            print('  [%s] %s' % (section, key))
    if missing:
        print('\nnot in the ini (the default applies):')
        print('  ' + ', '.join('[%s] %s' % k for k in missing))
    if not different and not unknown:
        print('\nOK: every active ini value equals the built-in default.')
    return 1 if (different or unknown) else 0


if __name__ == '__main__':
    sys.exit(main())
