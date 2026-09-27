"""Set up FINAL FANTASY XI on this Mac, start to finish. install.sh and setup.command run it.

  python3 tools/setup.py [--game <FINAL FANTASY XI folder>] [--install-dir <folder>] [--no-install] [--open]
  python3 tools/setup.py --find            list the game folders found on this Mac, and stop

It finds the game folder the player got from their server (or takes --game), checks that its
version is one meta/builds.json knows, makes .venv with the recompiler's two Python packages,
translates the game's code and compiles it (the vendored SDL3 and mbedtls too), signs
"Final Fantasy XI.app" with a code-signing certificate of this Mac's own (made on the first run),
and copies the app to /Applications (~/Applications if that is not writable).

Run it again after the server hands out a new game version: it rebuilds what changed. Clang
comes from Xcode's command line tools; install.sh and setup.command get them first when missing.
The whole build's output goes to build/setup.log.
"""
import argparse
import os
import platform
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import buildinfo  # noqa: E402
import thirdparty  # noqa: E402

ROOT = os.path.dirname(HERE)
LOG = os.path.join(ROOT, 'build', 'setup.log')
VENV = os.path.join(ROOT, '.venv')
APP_NAME = 'Final Fantasy XI'
PACKAGES = ['capstone>=5,<6', 'pefile>=2023']
IDENTITY = 'FFXI Local Code Signing'

STEPS = ['Checking this Mac', 'Finding the game', 'Getting the recompiler ready', 'Building libraries',
         "Translating the game's code", 'Compiling', 'Making the app', 'Installing']
PHASES = {'translate': 4, 'compile': 5, 'app': 6}  # build_posix.py's @phase names -> STEPS


def step(n):
    print('\n[%d/%d] %s' % (n + 1, len(STEPS), STEPS[n]), flush=True)


def note(text):
    print('  ' + text, flush=True)


def progress(fraction, detail=''):
    width = 30
    fill = int(fraction * width)
    sys.stdout.write('\r  [%s%s] %3d%%  %s' % ('#' * fill, '.' * (width - fill), fraction * 100, detail[:40].ljust(40)))
    if fraction >= 1:
        sys.stdout.write('\n')
    sys.stdout.flush()


class Failure(Exception):
    pass


def log(text):
    os.makedirs(os.path.dirname(LOG), exist_ok=True)
    with open(LOG, 'a') as f:
        f.write(text if text.endswith('\n') else text + '\n')


def run(cmd, progress=None, **kw):
    """Runs cmd with its output in the log; progress(line) sees each line. Raises Failure."""
    log('> ' + ' '.join(cmd))
    p = subprocess.Popen(cmd, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                         errors='replace', **kw)
    tail = []
    for line in p.stdout:
        log(line)
        tail = (tail + [line.rstrip()])[-15:]
        if progress:
            progress(line)
    if p.wait():
        raise Failure('%s failed. The last lines were:\n%s\n(everything is in %s)'
                      % (os.path.basename(cmd[0]) if cmd[0] != sys.executable else ' '.join(cmd[1:3]),
                         '\n'.join('    ' + t for t in tail), LOG))


# ---- the game folder ----------------------------------------------------------------------------

def is_game(folder):
    """A FINAL FANTASY XI folder: both DLLs and the ROM folder with the game's data."""
    return all(os.path.exists(os.path.join(folder, n)) for n in ('FFXiMain.dll', 'FFXi.dll', 'ROM'))


def identify(folder):
    """{'path', 'build' (a label, or None when unsupported), 'sha256'}."""
    digest = buildinfo.sha256(os.path.join(folder, 'FFXiMain.dll'))
    return {'path': folder, 'build': buildinfo.match(digest), 'sha256': digest}


def candidates():
    """Folders on this Mac that hold FFXiMain.dll: the usual places, then Spotlight."""
    home = os.path.expanduser('~')
    rel = [os.path.join('PlayOnline', 'SquareEnix', 'FINAL FANTASY XI'),
           os.path.join('SquareEnix', 'FINAL FANTASY XI'), 'FINAL FANTASY XI']
    places = [home, os.path.join(home, 'Games'), os.path.join(home, 'Downloads'), os.path.join(home, 'Desktop'),
              os.path.join(home, 'Documents'), '/Applications', os.path.join(home, 'Applications')]
    # Wine and CrossOver installs
    for pf in ('Program Files (x86)', 'Program Files'):
        places.append(os.path.join(home, '.wine', 'drive_c', pf))
        bottles = os.path.join(home, 'Library', 'Application Support', 'CrossOver', 'Bottles')
        if os.path.isdir(bottles):
            places += [os.path.join(bottles, b, 'drive_c', pf) for b in os.listdir(bottles)]
    found = []
    for p in places:
        for r in rel:
            found.append(os.path.join(p, r))
    try:
        out = subprocess.run(['mdfind', 'kMDItemFSName == "FFXiMain.dll"'], capture_output=True, text=True,
                             timeout=20).stdout
        found += [os.path.dirname(l) for l in out.splitlines() if l.strip()]
    except (OSError, subprocess.TimeoutExpired):
        pass
    seen, result = set(), []
    for f in found:
        f = os.path.realpath(f)
        # our own copies of the game's DLLs (generated/) are not a game folder
        if f in seen or f.startswith(os.path.realpath(ROOT) + os.sep) or '/.Trash/' in f:
            continue
        seen.add(f)
        if is_game(f):
            result.append(identify(f))
    return result


def ask_for_folder():
    """A Finder dialog, for a person running setup.command."""
    script = ('POSIX path of (choose folder with prompt "Choose the FINAL FANTASY XI folder '
              '(the one with FFXiMain.dll in it) from your server\'s game files:")')
    r = subprocess.run(['osascript', '-e', script], capture_output=True, text=True)
    return r.stdout.strip().rstrip('/') or None


def pick_game(given):
    if given:
        folder = os.path.abspath(os.path.expanduser(given))
        if is_game(folder):
            return identify(folder)
        inner = os.path.join(folder, 'FINAL FANTASY XI')  # the SquareEnix folder, or the one above it
        for f in (inner, os.path.join(folder, 'SquareEnix', 'FINAL FANTASY XI'),
                  os.path.join(folder, 'PlayOnline', 'SquareEnix', 'FINAL FANTASY XI')):
            if is_game(f):
                return identify(f)
        raise Failure('%s is not a FINAL FANTASY XI folder (it needs FFXiMain.dll, FFXi.dll and ROM). Choose the FINAL FANTASY XI folder from your '
                      "server's game files." % folder)
    note('Looking for the game files on this Mac...')
    found = candidates()
    good = [c for c in found if c['build']]
    if len(good) == 1:
        return good[0]
    if len(good) > 1 and sys.stdin.isatty():
        print('  Found more than one copy of the game:')
        for i, c in enumerate(good, 1):
            print('    %d. %s' % (i, c['path']))
        while True:
            a = input('  Which one? [1-%d] ' % len(good)).strip()
            if a.isdigit() and 1 <= int(a) <= len(good):
                return good[int(a) - 1]
    if good:
        return good[0]
    if found:
        note('Found the game at %s, but not a version this build supports.' % found[0]['path'])
    note('Choose the FINAL FANTASY XI folder in the window that opens.')
    chosen = ask_for_folder()
    if not chosen:
        raise Failure('No game folder chosen.')
    return pick_game(chosen)


# ---- the steps ----------------------------------------------------------------------------------

def check_mac():
    if sys.platform != 'darwin':
        raise Failure('This setup is for macOS. On Windows, see the README.')
    if platform.machine() != 'arm64':
        raise Failure('This needs a Mac with Apple silicon (M1 or later).')
    major = int(platform.mac_ver()[0].split('.')[0] or 0)
    if major and major < 12:
        raise Failure('This needs macOS 12 (Monterey) or later.')
    if subprocess.run(['xcrun', '--find', 'clang'], capture_output=True).returncode:
        raise Failure("Xcode's command line tools are missing. Run: xcode-select --install, then setup again.")
    free = shutil.disk_usage(ROOT).free
    if free < 3 << 30:
        raise Failure('Setup needs about 3 GB free space; this disk has %.1f GB.' % (free / (1 << 30)))


def venv_python():
    return os.path.join(VENV, 'bin', 'python3')


def ensure_python():
    py = venv_python()
    ok = os.path.exists(py) and subprocess.run([py, '-c', 'import capstone, pefile'], capture_output=True).returncode == 0
    if ok:
        return py
    if not os.path.exists(py):
        run(['/usr/bin/python3' if os.path.exists('/usr/bin/python3') else sys.executable, '-m', 'venv', VENV])
    run([py, '-m', 'pip', 'install', '--disable-pip-version-check', '-q'] + PACKAGES)
    return py


def build_libraries():
    total = sum(sum(len(g['sources']) for g in thirdparty.manifest(n)['groups']) for n in thirdparty.NAMES)
    done = [0]

    def one(name, n, of):
        done[0] += 1
        progress(done[0] / total, name)
    for name in thirdparty.NAMES:
        before = done[0]
        thirdparty.build(name, one)
        if done[0] == before:  # already built
            done[0] += sum(len(g['sources']) for g in thirdparty.manifest(name)['groups'])
            progress(done[0] / total, name)


def ensure_identity():
    """This Mac's own code-signing certificate, so macOS knows each rebuild as the same app and
    keeps the keychain's "Always Allow" for the saved password (README)."""
    if subprocess.run(['security', 'find-certificate', '-c', IDENTITY], capture_output=True).returncode == 0:
        return
    work = os.path.join(ROOT, 'build', 'signing')
    os.makedirs(work, exist_ok=True)
    cnf = os.path.join(work, 'cs.cnf')
    with open(cnf, 'w') as f:
        f.write('[req]\ndistinguished_name = dn\nx509_extensions = ext\nprompt = no\n[dn]\nCN = %s\n'
                '[ext]\nbasicConstraints = critical, CA:false\nkeyUsage = critical, digitalSignature\n'
                'extendedKeyUsage = critical, codeSigning\n' % IDENTITY)
    key, cert, p12 = (os.path.join(work, n) for n in ('key.pem', 'cert.pem', 'cs.p12'))
    try:
        run(['/usr/bin/openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-keyout', key, '-out', cert,
             '-days', '3650', '-config', cnf])
        run(['/usr/bin/openssl', 'pkcs12', '-export', '-inkey', key, '-in', cert, '-name', IDENTITY, '-out', p12,
             '-passout', 'pass:x'])
        run(['security', 'import', p12, '-k', os.path.expanduser('~/Library/Keychains/login.keychain-db'),
             '-P', 'x', '-T', '/usr/bin/codesign'])
    except Failure as e:
        log('signing certificate: %s' % e)
        note('Could not make a signing certificate; the app is signed ad hoc instead.')
    finally:
        shutil.rmtree(work, ignore_errors=True)


def build_app(py, game):
    """build_posix.py prepare and app; with FFXI_PROGRESS set it marks its phases (@phase name) and
    the compile's progress (@progress done total), shown here as steps."""
    def watch(line):
        if line.startswith('@phase ') and line.split()[1] in PHASES:
            step(PHASES[line.split()[1]])
        elif line.startswith('@progress '):
            done, total = map(int, line.split()[1:3])
            progress(done / total if total else 1.0, '%d of %d files' % (done, total))
    env = dict(os.environ, FFXI_PROGRESS='1')
    step(PHASES['translate'])
    run([py, os.path.join('tools', 'build_posix.py'), 'prepare', '--game', game], env=env)
    run([py, os.path.join('tools', 'build_posix.py'), 'app', '--game', game], progress=watch, env=env)
    return os.path.join(ROOT, 'build', APP_NAME + '.app')


def install(app, where):
    targets = [where] if where else ['/Applications', os.path.expanduser('~/Applications')]
    for d in targets:
        try:
            os.makedirs(d, exist_ok=True)
            dest = os.path.join(d, APP_NAME + '.app')
            if os.path.exists(dest):
                shutil.rmtree(dest)
            # ditto keeps the signature and extended attributes intact
            subprocess.run(['ditto', app, dest], check=True, capture_output=True)
            return dest
        except (OSError, subprocess.CalledProcessError) as e:
            log('install to %s: %s' % (d, e))
    raise Failure('Could not copy the app to %s.' % ' or '.join(targets))


def main():
    ap = argparse.ArgumentParser(description='Set up FINAL FANTASY XI on this Mac.')
    ap.add_argument('--game', help='the FINAL FANTASY XI folder (found automatically when left out)')
    ap.add_argument('--install-dir', help='where the app goes (default: /Applications, else ~/Applications)')
    ap.add_argument('--no-install', action='store_true', help='leave the app in build/')
    ap.add_argument('--find', action='store_true', help='list the game folders found on this Mac, and stop')
    ap.add_argument('--open', action='store_true', help='start the game when done')
    a = ap.parse_args()
    if a.find:
        for c in candidates():
            print('%s  %s' % (c['build'] or 'unsupported', c['path']))
        return 0
    os.makedirs(os.path.dirname(LOG), exist_ok=True)
    with open(LOG, 'w') as f:
        f.write('FFXI setup, %s\n' % time.strftime('%Y-%m-%d %H:%M:%S'))
    print('Setting up FINAL FANTASY XI. The first time takes a few minutes.')
    try:
        step(0)
        check_mac()

        step(1)
        game = pick_game(a.game)
        if not game['build']:
            raise Failure('The game at %s is a version this setup does not support yet (FFXiMain.dll %s...). '
                          'Supported: %s. Run the install command again later for a newer setup, or ask your '
                          'server which client version it uses.'
                          % (game['path'], game['sha256'][:12], ', '.join(buildinfo.known())))
        note('%s (version %s)' % (game['path'], game['build']))

        step(2)
        py = ensure_python()

        step(3)
        build_libraries()
        ensure_identity()

        app = build_app(py, game['path'])

        step(7)
        dest = app if a.no_install else install(app, a.install_dir)
    except Failure as e:
        print('\n  FAILED: %s' % e)
        return 1
    except KeyboardInterrupt:
        print('\n  Stopped.')
        return 130
    print('\nDone. %s is in %s. When it opens, pick your server in Settings and sign in.'
          % (APP_NAME, os.path.dirname(dest)))
    if a.open:
        subprocess.run(['open', dest])
    return 0


if __name__ == '__main__':
    sys.exit(main())
