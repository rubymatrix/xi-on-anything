"""Translate the player's own FFXiMain.dll and FFXi.dll into the kit's sources, on the phone.

  python phone_kit.py --game "<FINAL FANTASY XI folder>" --kit <kit folder> --config <phone-kit.json>

What the Mac does with tools/prepare.py and tools/android_translation.py, with the same unpack.py and
recomp.py, for the one game build the APK's host was made for. phone-kit.json (written by
tools/build_android.py --split-game) holds that build's metadata: its label, the two DLLs' SHA-256,
the metadata files (beside it, by name), and the Android wraps, hooks and patches. Nothing in it, or in
the APK, is Square Enix code: the code is translated here from the player's own install.

Output: <kit>/src/android-all and <kit>/src/ffxi (the C and its funcs.h), and <kit>/src.id, the
digest of the config and the DLLs, which BuildActivity compares to know the sources are current. The
unpacked images go to <kit>/work and are removed after.
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


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def step(args, what):
    start = time.monotonic()
    print('>', what, flush=True)
    subprocess.run([sys.executable] + args, check=True)
    print('  %s: %.1f s' % (what, time.monotonic() - start), flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--game', required=True, help='the FINAL FANTASY XI folder')
    ap.add_argument('--kit', required=True)
    ap.add_argument('--config', required=True, help='phone-kit.json, with the metadata files beside it')
    ap.add_argument('--recomp', default=os.path.join(HERE, '..', 'recomp', 'recomp.py'))
    a = ap.parse_args()
    with open(a.config) as f:
        config = json.load(f)
    meta_dir = os.path.dirname(os.path.abspath(a.config))

    dlls = {}
    for name, key in (('FFXiMain.dll', 'ffximain_sha'), ('FFXi.dll', 'ffxi_sha')):
        path = os.path.join(a.game, name)
        if not os.path.isfile(path):
            raise SystemExit('no %s in %s' % (name, a.game))
        digest = sha256(path)
        if digest != config[key]:
            raise SystemExit('%s is not build %s (this app is for %s; the install has %s)'
                             % (name, config['build'], config[key][:12], digest[:12]))
        dlls[name] = path
    print('build %s: FFXiMain.dll and FFXi.dll match' % config['build'], flush=True)

    work = os.path.join(a.kit, 'work')
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    unpack = os.path.join(HERE, 'unpack.py')
    main_image = os.path.join(work, 'FFXiMain.unpacked.dll')
    ffxi_image = os.path.join(work, 'FFXi.unpacked.dll')
    step([unpack, dlls['FFXiMain.dll'], main_image], 'unpack FFXiMain.dll')
    step([unpack, dlls['FFXi.dll'], ffxi_image], 'unpack FFXi.dll')

    src = os.path.join(a.kit, 'src')
    shutil.rmtree(src, ignore_errors=True)
    args = [a.recomp, '--meta', os.path.join(meta_dir, config['ffximain_meta']), '--image', main_image,
            '--retail', dlls['FFXiMain.dll'], '--out', os.path.join(src, 'android-all'), '--all']
    for key in ('wraps', 'hooks'):
        if config[key]:
            args += ['--' + key, ','.join(k + '=' + v for k, v in config[key].items())]
    if config['patches']:
        patches = os.path.join(work, 'patches.json')
        with open(patches, 'w') as f:
            json.dump(config['patches'], f, indent=2)
        args += ['--patches', patches]
    step(args, 'translate FFXiMain.dll')
    step([a.recomp, '--meta', os.path.join(meta_dir, config['ffxi_meta']), '--image', ffxi_image,
          '--retail', dlls['FFXi.dll'], '--module', 'ffxi', '--out', os.path.join(src, 'ffxi'), '--all'],
         'translate FFXi.dll')
    shutil.rmtree(work)
    with open(a.config, 'rb') as f:
        ident = hashlib.sha256(f.read() + config['ffximain_sha'].encode() + config['ffxi_sha'].encode())
    with open(os.path.join(a.kit, 'src.id'), 'w') as f:
        f.write(ident.hexdigest()[:16] + '\n')
    print('sources ready', flush=True)


if __name__ == '__main__':
    main()
