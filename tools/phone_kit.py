"""Translate the player's own FFXiMain.dll and FFXi.dll into the kit's sources, on the phone.

  python phone_kit.py --game "<FINAL FANTASY XI folder>" --kit <kit folder>

What the Mac does with tools/prepare.py, tools/build.py's build.h and tools/android_translation.py,
with the same unpack.py, recomp.py and recomp/meta/ (beside this folder, as in a checkout), for whichever
build recomp/meta/builds.json knows the install to be. Nothing in the APK is Square Enix code: the code is
translated here from the player's own install.

Output: <kit>/src/android-all and <kit>/src/ffxi (the C and its funcs.h); <kit>/gen/build.h (the
build's addresses, which the host's sources in <kit>/tree include: BuildActivity compiles both);
<kit>/src.id, the build and the two DLLs' digests, which BuildActivity puts in libgame.id; and
<kit>/src.game, the DLLs' paths, sizes and dates, which it compares to know when the install has
changed (the game updated) without hashing them on every start. The unpacked images go to
<kit>/work and are removed after.
"""

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import android_translation  # noqa: E402
import build  # noqa: E402
import buildinfo  # noqa: E402


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def step(args, what):
    start = time.monotonic()
    print('>', what, flush=True)
    if sys.platform == 'ios':
        # iOS starts no program of an app's: the script runs in this interpreter (ios_python.c)
        import runpy
        saved = sys.argv[:], sys.path[:]
        sys.argv = list(args)
        sys.path.insert(0, os.path.dirname(os.path.abspath(args[0])))
        try:
            runpy.run_path(args[0], run_name='__main__')
        except SystemExit as e:
            if e.code not in (None, 0):
                raise SystemExit('%s failed: %s' % (what, e.code))
        finally:
            sys.argv, sys.path[:] = saved[0], saved[1]
    else:
        subprocess.run([sys.executable] + args, check=True)
    print('  %s: %.1f s' % (what, time.monotonic() - start), flush=True)


def stamp(dlls):
    """The line BuildActivity.installStamp writes for the same files: path, size and date (ms) of each."""
    return ' '.join('%s %d %d' % (p, os.path.getsize(p), os.stat(p).st_mtime_ns // 1000000) for p in dlls)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--game', required=True, help='the FINAL FANTASY XI folder')
    ap.add_argument('--kit', required=True)
    ap.add_argument('--recomp', default=os.path.join(HERE, '..', 'recomp', 'recomp.py'))
    ap.add_argument('--target', choices=['android', 'ios'], default='android',
                    help='android: android-all, with the Android wraps; ios: all, as the Mac\'s (Metal)')
    a = ap.parse_args()

    paths = [os.path.join(a.game, name) for name in ('FFXiMain.dll', 'FFXi.dll')]
    for path in paths:
        if not os.path.isfile(path):
            raise SystemExit('no %s in %s' % (os.path.basename(path), a.game))
    main_sha, ffxi_sha = sha256(paths[0]), sha256(paths[1])
    label = buildinfo.match(main_sha)
    if label is None:
        raise SystemExit('FFXiMain.dll is a build this app does not know (%s): a newer app may' % main_sha[:12])
    known = buildinfo.known()[label]
    if known['FFXi.dll']['sha256'] != ffxi_sha:
        raise SystemExit('FFXi.dll is not build %s\'s (%s): the install is half updated' % (label, ffxi_sha[:12]))
    print('build %s: FFXiMain.dll and FFXi.dll match' % label, flush=True)

    work = os.path.join(a.kit, 'work')
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    # buildinfo.current() reads the choice tools/prepare.py records: here, in work/
    buildinfo.CHOSEN = os.path.join(work, 'build.json')
    buildinfo.record(label, a.game)
    unpack = os.path.join(HERE, 'unpack.py')
    main_image = os.path.join(work, 'FFXiMain.unpacked.dll')
    ffxi_image = os.path.join(work, 'FFXi.unpacked.dll')
    step([unpack, paths[0], main_image], 'unpack FFXiMain.dll')
    step([unpack, paths[1], ffxi_image], 'unpack FFXi.dll')
    if a.target == 'ios':
        import copy
        config = copy.deepcopy(buildinfo.current())
    else:
        config = android_translation.android_build(buildinfo.current(), image=main_image, retail=paths[0])
    part = 'all' if a.target == 'ios' else 'android-all'

    for f in ('src.id', 'src.game'):  # gone until the sources are whole again
        try:
            os.remove(os.path.join(a.kit, f))
        except OSError:
            pass
    os.makedirs(os.path.join(a.kit, 'gen'), exist_ok=True)
    build.BUILD = config
    build.BUILD_H = os.path.join(a.kit, 'gen', 'build.h')
    build.write_build_h()

    src = os.path.join(a.kit, 'src')
    shutil.rmtree(src, ignore_errors=True)
    args = [a.recomp, '--meta', config['ffximain_meta'], '--image', main_image,
            '--retail', paths[0], '--out', os.path.join(src, part), '--all']
    for key in ('wraps', 'hooks'):
        if config[key]:
            args += ['--' + key, ','.join(k + '=' + v for k, v in config[key].items())]
    if config['patches']:
        patches = os.path.join(work, 'patches.json')
        with open(patches, 'w') as f:
            json.dump(config['patches'], f, indent=2)
        args += ['--patches', patches]
    step(args, 'translate FFXiMain.dll')
    step([a.recomp, '--meta', config['ffxi_meta'], '--image', ffxi_image,
          '--retail', paths[1], '--module', 'ffxi', '--out', os.path.join(src, 'ffxi'), '--all'],
         'translate FFXi.dll')
    shutil.rmtree(work)
    with open(build.BUILD_H, 'rb') as f:
        ident = hashlib.sha256(f.read() + main_sha.encode() + ffxi_sha.encode())
    with open(os.path.join(a.kit, 'src.id'), 'w') as f:
        f.write(ident.hexdigest()[:16] + '\n')
    with open(os.path.join(a.kit, 'src.game'), 'w') as f:
        f.write(stamp(paths) + '\n')
    print('sources ready: build %s' % label, flush=True)


if __name__ == '__main__':
    main()
