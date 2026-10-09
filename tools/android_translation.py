"""The Android translation, for tools/build_android.py: FFXiMain.dll into generated/android-all with
the occlusion probe's constructor and destructor wrapped (meta/builds.json "android_wraps"), and
FFXi.dll into generated/ffxi as on the desktop.

The wraps are accepted only for the validated 2025-11-12 build and unpacked image. The hashes and
addresses checked here are metadata; no game code is embedded in this tool or the runtime.
"""

import copy
import hashlib
import json
from pathlib import Path
import subprocess
import sys

import buildinfo

ROOT = Path(__file__).resolve().parents[1]


def android_build(original):
    """The desktop build configuration (buildinfo.current()) with this build's Android wraps added."""
    if not original:
        raise ValueError('prepare the local game translation first')
    result = copy.deepcopy(original)
    extra = buildinfo.known()[original['build']].get('android_wraps', {})
    if extra:
        if original['build'] != '2025-11-12':
            raise ValueError('no validated Android probe layout for this client')
        image = ROOT / 'generated/FFXiMain.unpacked.dll'
        retail = ROOT / 'generated/FFXiMain.retail.dll'
        if (
            hashlib.sha256(image.read_bytes()).hexdigest()
            != '85398cd8ddb142e8d7435c9e78370e0504218da409d8f4abde0e29615aebffe5'
        ):
            raise ValueError('unpacked image differs from validated probe layout')
        if hashlib.sha256(retail.read_bytes()).hexdigest() != original['ffximain_sha']:
            raise ValueError('prepared retail image differs from selected build')
        if extra != {'probe_ctor': '0x1006c070', 'probe_dtor': '0x1006c080'}:
            raise ValueError('unvalidated probe metadata')
        result['wraps'].update(extra)
    return result


def generate(configuration):
    args = [
        sys.executable,
        str(ROOT / 'recomp/recomp.py'),
        '--meta',
        configuration['ffximain_meta'],
        '--image',
        str(ROOT / 'generated/FFXiMain.unpacked.dll'),
        '--retail',
        str(ROOT / 'generated/FFXiMain.retail.dll'),
        '--out',
        str(ROOT / 'generated/android-all'),
        '--all',
    ]
    for key in ('wraps', 'hooks'):
        if configuration[key]:
            args += ['--' + key, ','.join(k + '=' + v for k, v in configuration[key].items())]
    if configuration['patches']:
        patch = ROOT / 'generated/android-patches.json'
        patch.write_text(json.dumps(configuration['patches'], indent=2) + '\n')
        args += ['--patches', str(patch)]
    subprocess.run(args, cwd=ROOT, check=True)
    subprocess.run(
        [
            sys.executable,
            str(ROOT / 'recomp/recomp.py'),
            '--meta',
            configuration['ffxi_meta'],
            '--image',
            str(ROOT / 'generated/FFXi.unpacked.dll'),
            '--retail',
            str(ROOT / 'generated/FFXi.retail.dll'),
            '--module',
            'ffxi',
            '--out',
            str(ROOT / 'generated/ffxi'),
            '--all',
        ],
        cwd=ROOT,
        check=True,
    )
