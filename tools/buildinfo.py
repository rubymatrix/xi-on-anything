"""Which retail build this checkout is working on, and which version of the client it makes.

recomp/meta/builds.json lists every build the recompiler knows. tools/prepare.py matches the install's
FFXiMain.dll against it and records the choice in generated/build.json; everything after that
(build.py, install.py) reads the choice from there, so nothing depends on the install again.

runtime.json is the client's own version. stamp() writes it, with the commit and the game build,
beside what a build makes, so the launcher can tell a client built from older sources.
"""
import hashlib
import json
import os
import subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
METADIR = os.path.join(ROOT, 'recomp', 'meta')
BUILDS = os.path.join(METADIR, 'builds.json')
FEATURES = os.path.join(METADIR, 'features.json')
CHOSEN = os.path.join(ROOT, 'generated', 'build.json')
RUNTIME = os.path.join(ROOT, 'runtime.json')


def sha256(path):
    with open(path, 'rb') as f:
        return hashlib.sha256(f.read()).hexdigest()


def known():
    with open(BUILDS) as f:
        return json.load(f)['builds']


def match(ffximain_sha):
    """The build label whose FFXiMain.dll has this hash, or None."""
    for label, b in known().items():
        if b['FFXiMain.dll']['sha256'] == ffximain_sha:
            return label
    return None


def record(label, game):
    with open(CHOSEN, 'w') as f:
        json.dump({'build': label, 'game': game}, f, indent=2)
        f.write('\n')


def current(required=True):
    """{'build', 'game', 'ffximain_meta', 'ffxi_meta', 'ffximain_sha', 'ffxi_sha', 'addresses', 'offsets', 'hooks', 'crt',
    'modern', 'modern_keys', 'features'}, ('features': buildinfo.features; 'modern_keys': every name a build's modern section has, so build.h
    names them all whichever this build is)
    or None if tools/prepare.py has not run and not required."""
    try:
        with open(CHOSEN) as f:
            chosen = json.load(f)
    except OSError:
        if not required:
            return None
        raise SystemExit('%s missing: run tools/prepare.py first' % CHOSEN)
    b = known()[chosen['build']]
    return {
        'build': chosen['build'],
        'game': chosen['game'],
        'ffximain_meta': os.path.join(METADIR, b['FFXiMain.dll']['meta']),
        'ffxi_meta': os.path.join(METADIR, b['FFXi.dll']['meta']),
        'ffximain_sha': b['FFXiMain.dll']['sha256'],
        'ffxi_sha': b['FFXi.dll']['sha256'],
        'version': b['version'],
        'addresses': b['addresses'],
        'offsets': b.get('offsets', {}),
        'hooks': b.get('hooks', {}),
        'wraps': b.get('wraps', {}),
        'geometry': b.get('geometry', {}),
        'patches': b.get('patches', {}),
        'frame_int_sites': b.get('frame_int_sites', []),
        'frame_wait_sites': b.get('frame_wait_sites', []),
        'frame_cam_once': b.get('frame_cam_once', []),
        'frame_look_once': b.get('frame_look_once', []),
        'frame_net_clock': b.get('frame_net_clock'),
        'crt': b['crt'],
        'modern': b.get('modern', {}),
        'modern_keys': sorted({k for other in known().values() for k in other.get('modern', {})}),
        'features': features(chosen['build']),
    }


def catalog():
    """recomp/meta/features.json's features, in its order: {id: {title, what, code, min_build, ...}}."""
    with open(FEATURES) as f:
        return json.load(f)['features']


def features(label):
    """Which of recomp/meta/features.json's features the build labelled so has: [{'id', 'title', 'on', 'why'}],
    why being '' when on, else the first rule that failed (or the build's own override)."""
    builds = known()
    b = builds[label]
    every_modern = {k for other in builds.values() for k in other.get('modern', {})}
    out = []
    for fid, f in catalog().items():
        why = ''
        if f.get('min_build') and label < f['min_build']:
            why = 'needs a client from %s or later' % f['min_build']
        elif f.get('max_build') and label > f['max_build']:
            why = 'needs a client from %s or earlier' % f['max_build']
        elif f.get('builds') and label not in f['builds']:
            why = 'only on builds %s' % ', '.join(f['builds'])
        else:
            for need in f.get('needs', []):
                if need == 'modern':
                    have = bool(every_modern) and all(str(b.get('modern', {}).get(k, '')).startswith('0x')
                                                      for k in every_modern)
                else:
                    section, _, key = need.partition('.')
                    have = key in (b.get(section) or {})
                if not have:
                    why = 'not found in this client build (%s)' % need
                    break
        override = b.get('features', {}).get(fid)
        if override is True:
            why = ''
        elif isinstance(override, str):
            why = override
        out.append({'id': fid, 'title': f['title'], 'on': not why, 'why': why})
    return out


def runtime():
    """{'version', 'commit', 'build', 'features'}: what a client built from this checkout is, and which of
    recomp/meta/features.json's features it has (buildinfo.features). commit is None outside a git checkout,
    build None (and features empty) before tools/prepare.py."""
    with open(RUNTIME) as f:
        version = json.load(f)['version']
    try:
        commit = subprocess.run(['git', 'rev-parse', 'HEAD'], cwd=ROOT, capture_output=True, text=True,
                                check=True).stdout.strip() or None
    except (OSError, subprocess.CalledProcessError):
        commit = None
    chosen = current(required=False)
    return {'version': version, 'commit': commit, 'build': chosen['build'] if chosen else None,
            'features': chosen['features'] if chosen else []}


def stamp(path):
    """runtime() as JSON at path (the launcher reads it to decide whether to rebuild)."""
    with open(path, 'w') as f:
        json.dump(dict(format='xi-runtime-stamp/1', **runtime()), f, indent=2)
        f.write('\n')
