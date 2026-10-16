#!/usr/bin/env python3
"""The addons' Direct3D 8, D3DX and Win32 through ffi (host/addons/d3d_ffi.c, win32_ffi.c) and the
gdifonts renderer (gdifont_ffi.c), in the addon harness with Ashita's own libs/d3d8 declarations.

    python3 tests/addons/d3d8_ffi.py --game <FINAL FANTASY XI> [--ashita <dir with addons/libs>]
                                     [--gdifonts <ThornyFFXI's gdifonts folder>]...

--ashita defaults to the installed data dir's ashita/ (tools/addons_fetch.py puts Ashita's addons
and libs there). gdifonts is not in this repository: without --gdifonts (or /tmp/gdifonts) its test
is skipped. Addons carry versions of it (tTimers ThornyFFXI's, fancychat's with colour markers,
balloon's with regions and clipping): each --gdifonts is tested in turn. A scratch data dir gets the libs and the test addons (tests/addons/d3d8test, gditest);
build/host64 runs d3d8_ffi.txt and gdifonts.txt. Fails on any FAIL line, a Lua error, or a DONE
line that isn't all passed."""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
HERE = os.path.join(ROOT, 'tests', 'addons')


def default_ashita():
    if sys.platform == 'darwin':
        return os.path.expanduser('~/Library/Application Support/FFXIRecompile/FFXI/ashita')
    return os.path.join(os.environ.get('APPDATA', ''), 'FFXIRecompile', 'FFXI', 'ashita')


def run(a, data, script):
    r = subprocess.run([a.host, '--game', a.game, '--data-dir', data, '--addon-harness', os.path.join(HERE, script)],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace', timeout=300)
    out = r.stdout
    fails = [l for l in out.splitlines() if 'FAIL ' in l or (l.startswith(('[chat', '[harness]')) and 'error' in l.lower())]
    done = re.search(r'DONE (\d+)/(\d+)', out)
    for l in out.splitlines():
        if 'PASS ' in l or 'FAIL ' in l or 'DONE ' in l or l in fails:
            print(l)
    if r.returncode or fails or not done or done.group(1) != done.group(2):
        raise SystemExit('%s: failed (exit %d)' % (script, r.returncode))
    print('%s: %s checks passed' % (script, done.group(1)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--game', required=True)
    ap.add_argument('--ashita', default=default_ashita())
    ap.add_argument('--gdifonts', action='append', help='a gdifonts folder, once for each to test (/tmp/gdifonts)')
    ap.add_argument('--host', default=os.path.join(ROOT, 'build', 'host64'))
    ap.add_argument('--keep', action='store_true', help='keep the scratch data dir')
    a = ap.parse_args()
    libs = os.path.join(a.ashita, 'addons', 'libs')
    if not os.path.isdir(os.path.join(libs, 'd3d8')):
        raise SystemExit('no Ashita libs at %s (tools/addons_fetch.py, or --ashita)' % libs)
    data = tempfile.mkdtemp(prefix='d3d8ffi-')
    try:
        shutil.copytree(libs, os.path.join(data, 'ashita', 'addons', 'libs'))
        addons = os.path.join(data, 'xi', 'addons')
        shutil.copytree(os.path.join(HERE, 'd3d8test'), os.path.join(addons, 'd3d8test'))
        run(a, data, 'd3d8_ffi.txt')
        for gdifonts in a.gdifonts or ['/tmp/gdifonts']:
            if os.path.isfile(os.path.join(gdifonts, 'include.lua')):
                test = os.path.join(addons, 'gditest')
                shutil.rmtree(test, ignore_errors=True)
                shutil.copytree(os.path.join(HERE, 'gditest'), test)
                shutil.copytree(gdifonts, os.path.join(test, 'gdifonts'))
                print('gdifonts.txt with %s' % gdifonts)
                run(a, data, 'gdifonts.txt')
            else:
                print('gdifonts.txt: skipped (no gdifonts at %s)' % gdifonts)
    finally:
        if a.keep:
            print('data dir kept: ' + data)
        else:
            shutil.rmtree(data, ignore_errors=True)


if __name__ == '__main__':
    main()
