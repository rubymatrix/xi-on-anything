"""Build XI on Anything for the Nintendo Switch and put it on an SD card, start to finish.
platforms/switch/setup-switch.bat (Windows) and platforms/switch/setup-switch.sh (Linux, macOS) run it.

  python tools/setup_switch.py [--game <FINAL FANTASY XI folder>] [--sd <SD card's root>|emu]
                               [--server <name or IP>] [--layout nintendo|xbox] [--dats <folder>]
                               [--arg "<host options>"] [--no-copy-game] [--no-icon] [--check]

It checks this PC (Python's two packages, devkitPro's), identifies the game's version (meta/builds.json),
prepares its code (tools/prepare.py), finds its frame delta sites when meta/builds.json has none
(tools/version/frame_sites.py: without them the Switch runs at 30 fps, with them uncapped), builds the .nro
(tools/build_switch.py host) and copies the .nro, the game and the settings to the card
(tools/build_switch.py setup). Each step is skipped when it is already done, so run it again after a
game update or a pull: it does only what changed.

On a terminal it asks for what the options don't give, starting from the last answers
(build/switch/setup.json). --no-copy-game leaves the game on the card as it is (after a rebuild: the
.nro and the settings only). --check checks this PC and stops. Everything the steps print goes to
build/switch/setup.log. run.args on the card keeps its other lines: only the options given replace theirs.

The home screen icon (a forwarder) is made on the console, which installs it with its own keys: this puts
Sphaira (github.com/ITotalJustice/sphaira, a homebrew menu that makes forwarders) on the card when the card
has none, checks for signature patches (a forwarder is an unsigned title) and says the one step left on the
console. --no-icon leaves that out.
The manual steps are in docs/platforms/switch.md.
"""
import argparse
import json
import os
import re
import string
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(HERE))
import buildinfo  # noqa: E402

OUT = Path(os.environ.get('XI_SWITCH_BUILD_DIR', str(ROOT / 'build/switch'))).resolve()
LOG = OUT / 'setup.log'
SAVED = OUT / 'setup.json'
CALLER_DIR = os.getcwd()
PACKAGES = ['capstone', 'pefile']
DKP_PACKAGES = 'switch-dev switch-mesa switch-libdrm_nouveau switch-glad switch-zlib switch-libexpat'
STEPS = ['Checking this PC', 'Finding the game', "Preparing the game's code", 'Frame delta sites',
         'Building the app', 'Putting it on the SD card']


# ---- output -------------------------------------------------------------------------------------

def step(n):
    print('\n[%d/%d] %s' % (n + 1, len(STEPS), STEPS[n]), flush=True)


def note(text):
    print('  ' + text, flush=True)


def progress(fraction, detail=''):
    width = 30
    fill = int(fraction * width)
    sys.stdout.write('\r  [%s%s] %3d%%  %s' % ('#' * fill, '.' * (width - fill), fraction * 100, detail[:40].ljust(40)))
    sys.stdout.flush()


class Failure(Exception):
    pass


def log(text):
    OUT.mkdir(parents=True, exist_ok=True)
    with open(LOG, 'a', encoding='utf-8') as f:
        f.write(text if text.endswith('\n') else text + '\n')


def run(cmd, on_line=None, env=None):
    """cmd with its output in the log; on_line(line) sees each line. Raises Failure with the last lines."""
    cmd = [str(c) for c in cmd]
    log('> ' + ' '.join(cmd))
    p = subprocess.Popen(cmd, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                         errors='replace', env=env)
    tail = []
    for line in p.stdout:
        log(line)
        if not line.startswith(('@progress', '@phase')):
            tail = (tail + [line.rstrip()])[-15:]
        if on_line:
            on_line(line)
    if p.wait():
        raise Failure('%s failed. The last lines were:\n%s\n(everything is in %s)'
                      % (' '.join(Path(c).name for c in cmd[1:3]), '\n'.join('    ' + t for t in tail), LOG))


def interactive():
    """Whether a person can answer: stdin a terminal. (Windows' NUL device says it is one - the window runs this
    with stdin NUL - so there a console's input mode is asked for, which NUL has not.)"""
    if not sys.stdin or not sys.stdin.isatty():
        return False
    if sys.platform == 'win32':
        import ctypes
        import msvcrt
        mode = ctypes.c_uint32()
        try:
            return bool(ctypes.windll.kernel32.GetConsoleMode(msvcrt.get_osfhandle(sys.stdin.fileno()), ctypes.byref(mode)))
        except OSError:
            return False
    return True


def ask(question, default=''):
    """The player's answer, or default (blank, or not on a terminal)."""
    if not interactive():
        return default
    shown = f' [{default}]' if default else ''
    try:
        answer = input(f'  {question}{shown}: ').strip().strip('"')
    except EOFError:
        return default
    return answer or default


def yes(question, default=True):
    answer = ask(question + (' (Y/n)' if default else ' (y/N)')).lower()
    return default if not answer else answer.startswith('y')


def user_path(p):
    return os.path.abspath(os.path.join(CALLER_DIR, os.path.expanduser(p)))


def load_saved():
    try:
        return json.loads(SAVED.read_text())
    except (OSError, ValueError):
        return {}


def save(answers):
    OUT.mkdir(parents=True, exist_ok=True)
    SAVED.write_text(json.dumps(answers, indent=2) + '\n')


# ---- 1. this PC -----------------------------------------------------------------------------------

def check_pc(fix):
    """[problems]: each a line saying what is missing and how to get it. fix: offer to pip install."""
    problems = []
    if sys.version_info < (3, 12):
        problems.append('Python %d.%d: 3.12 or newer is needed (https://www.python.org/downloads/; on Windows run '
                        'this with `py -3.12`)' % sys.version_info[:2])
    missing = [p for p in PACKAGES if subprocess.run([sys.executable, '-c', 'import ' + p], capture_output=True).returncode]
    if missing and fix and yes(f"Python's {' and '.join(missing)} {'is' if len(missing) == 1 else 'are'} missing. "
                               'Install with pip now?'):
        run([sys.executable, '-m', 'pip', 'install', '--disable-pip-version-check', '-q'] + missing)
        missing = [p for p in PACKAGES if subprocess.run([sys.executable, '-c', 'import ' + p],
                                                         capture_output=True).returncode]
    if missing:
        problems.append(f"Python's {', '.join(missing)}: `{Path(sys.executable).name} -m pip install {' '.join(missing)}`")
    problems += check_devkitpro(fix)
    return problems


# devkitPro: the compiler (devkitA64) and the Switch's libraries, installed with devkitPro's pacman

def toolchain():
    """switch_toolchain, looked at afresh (after an install it finds what it did not before)."""
    import importlib
    import switch_toolchain as tc
    return importlib.reload(tc)


def absent_packages(tc):
    if not tc.DKP:
        return DKP_PACKAGES.split()
    want = {'switch-dev': tc.LIBNX / 'lib/libnx.a', 'switch-mesa': tc.PORTLIBS / 'lib/libEGL.a',
            'switch-libdrm_nouveau': tc.PORTLIBS / 'lib/libdrm_nouveau.a', 'switch-glad': tc.PORTLIBS / 'include/glad',
            # Mesa 26's (build_switch.py mesa)
            'switch-zlib': tc.PORTLIBS / 'lib/libz.a', 'switch-libexpat': tc.PORTLIBS / 'lib/libexpat.a'}
    return [pkg for pkg, path in want.items() if not path.exists()]


def find_pacman(tc):
    """The command that runs devkitPro's pacman ([...], then the packages), or None: on Windows the msys2 it
    lives in (devkitPro's installer's C:\\devkitPro\\msys2, or a plain msys2 with devkitPro's repositories),
    elsewhere dkp-pacman (Linux's devkitPro pacman) or devkitPro's own pacman, through sudo."""
    if sys.platform == 'win32':
        roots = []
        if tc.DKP:  # an msys2's /opt/devkitpro, or devkitPro's installer's root
            roots += [tc.DKP.parent.parent] if tc.DKP.parent.name == 'opt' else [tc.DKP / 'msys2']
        roots += [Path('C:/devkitPro/msys2')] + tc._msys_roots()
        for r in roots:
            bash = r / 'usr/bin/bash.exe'
            if bash.exists() and (r / 'usr/bin/pacman.exe').exists():
                return [str(bash), '-lc']
        return None
    import shutil
    for pm in ['dkp-pacman', '/opt/devkitpro/pacman/bin/pacman']:
        if shutil.which(pm) or Path(pm).exists():
            return ['sudo', pm]
    return None


def pacman_install(cmd, packages):
    if cmd[-1] == '-lc':  # msys2's bash: the whole command one string
        full = cmd + ['pacman -S --needed --noconfirm ' + ' '.join(packages)]
    else:
        full = cmd + ['-S', '--needed', '--noconfirm'] + packages
    print('  > ' + ' '.join(full), flush=True)
    log('> ' + ' '.join(full))
    # on the terminal, not through the log: sudo asks for the password, and pacman shows its downloads
    if subprocess.run(full).returncode:
        raise Failure('pacman could not install ' + ' '.join(packages) + '. Run it again, or install them by hand '
                      '(docs/platforms/switch.md, Manual steps).')


INSTALLER_API = 'https://api.github.com/repos/devkitPro/installer/releases/latest'


def get_installer():
    """devkitPro's Windows installer (devkitProUpdater), downloaded to build/switch: its path."""
    import urllib.request
    req = urllib.request.Request(INSTALLER_API, headers={'User-Agent': 'xi-on-anything-setup'})
    with urllib.request.urlopen(req, timeout=30) as r:
        release = json.load(r)
    asset = next(a for a in release['assets'] if a['name'].lower().endswith('.exe'))
    out = OUT / asset['name']
    if not out.exists():
        note(f"downloading {asset['name']} from github.com/devkitPro/installer")
        OUT.mkdir(parents=True, exist_ok=True)
        urllib.request.urlretrieve(asset['browser_download_url'], out)
    return out


def check_devkitpro(fix):
    tc = toolchain()
    absent = absent_packages(tc)
    if not absent:
        note(f'devkitPro: {tc.DKP}')
        return []
    pacman = find_pacman(tc)
    if fix and not pacman and sys.platform == 'win32' and interactive() and yes(
            'devkitPro (the Switch compiler) is not installed. Download its installer and open it now?'):
        try:
            exe = get_installer()
        except Exception as e:  # noqa: BLE001 - offline, GitHub's limit: say so, and how to get it by hand
            raise Failure(f'Could not download the installer ({e}). Get it from '
                          'https://github.com/devkitPro/installer/releases and run it.')
        note('In the installer: keep the default folder (C:\\devkitPro), and tick "Switch Development"')
        note('when it asks which parts to install. It downloads for a while; then come back here.')
        subprocess.Popen([str(exe)])
        ask('Press Enter when the installer has finished')
        tc = toolchain()
        absent, pacman = absent_packages(tc), find_pacman(tc)
        if not absent:
            note(f'devkitPro: {tc.DKP}')
            return []
    if fix and pacman and yes(f"devkitPro needs {', '.join(absent)}. Install {'it' if len(absent) == 1 else 'them'} now?"):
        pacman_install(pacman, absent)
        tc = toolchain()
        absent = absent_packages(tc)
        if not absent:
            note(f'devkitPro: {tc.DKP}')
            return []
    if not pacman:
        where = ('run this setup again (it downloads the installer), or get it from '
                 'https://github.com/devkitPro/installer/releases' if sys.platform == 'win32'
                 else 'see https://devkitpro.org/wiki/devkitPro_pacman for this system')
        return [f'devkitPro (the Switch compiler): not installed. Install it: {where}. '
                f'Then this setup installs the packages it needs ({DKP_PACKAGES}).']
    return [f"devkitPro's {', '.join(absent)}: run this setup again and answer yes to install "
            f"{'it' if len(absent) == 1 else 'them'}, or by hand: pacman -S {' '.join(absent)}"]


# ---- 2. the game ----------------------------------------------------------------------------------

def is_game(folder):
    return all((Path(folder) / n).exists() for n in ('FFXiMain.dll', 'FFXi.dll', 'ROM'))


def registry_game():
    if sys.platform != 'win32':
        return None
    try:
        import prepare
        return prepare.registry_game()
    except (SystemExit, OSError, ImportError):
        return None


def find_game(given, saved):
    default = given or saved.get('game') or registry_game() or ''
    while True:
        folder = user_path(given) if given else ask('The FINAL FANTASY XI folder (the one with FFXiMain.dll in it)', default)
        if not folder:
            raise Failure('No game folder: give it with --game "<FINAL FANTASY XI folder>"')
        folder = os.path.normpath(user_path(folder))
        if is_game(folder):
            return folder
        msg = f'{folder} is not a FINAL FANTASY XI folder (it needs FFXiMain.dll, FFXi.dll and ROM)'
        if given or not interactive():
            raise Failure(msg)
        note(msg)
        default = ''


def identify(folder):
    digest = buildinfo.sha256(os.path.join(folder, 'FFXiMain.dll'))
    label = buildinfo.match(digest)
    if label is None:
        raise Failure(f'This game version is not one XI on Anything knows yet (FFXiMain.dll SHA-256 {digest}).\n'
                      '  It needs its own metadata first: see docs/new-client-version.md, or report the SHA-256 '
                      'to the project.')
    return label


SERVER_ARG = re.compile(r'--server[ =]+"?([A-Za-z0-9][A-Za-z0-9.\-]*(?::\d+)?)')
SERVER_KEY = re.compile(r'server\s*=\s*"?([A-Za-z0-9][A-Za-z0-9.\-]*(?::\d+)?)', re.I)


def read_config(path):
    """A text file in whichever encoding it has (VanaCore's ini is UTF-16)."""
    raw = path.read_bytes()[:1 << 20]
    if raw[:2] in (b'\xff\xfe', b'\xfe\xff'):
        return raw.decode('utf-16', errors='replace')
    return raw.decode('utf-8-sig', errors='replace')


def server_hints(game):
    """[(server, file it was found in)], newest file first: the server the player's launcher already uses, from
    the folders around the game - Ashita's boot configs ([ashita.boot] command = --server <name>), VanaCore's
    ini ([server] server=<name>) and launch scripts (--server <name>). Comment lines and examples are skipped."""
    dirs = [Path(game)] + list(Path(game).parents)[:3]
    patterns = ['config/boot/*.ini', '*/config/boot/*.ini', 'vanacore.ini', '*/vanacore.ini', '*.bat', '*.cmd']
    files = {f for d in dirs for p in patterns for f in d.glob(p) if f.is_file() and 'example' not in f.name.lower()}
    found = []
    for f in sorted(files, key=lambda f: f.stat().st_mtime, reverse=True):
        try:
            text = read_config(f)
        except OSError:
            continue
        section = ''
        for line in text.splitlines():
            line = line.strip()
            if not line or line[0] in ';#' or line.lower().startswith(('rem ', '::')):
                continue
            if line.startswith('['):
                section = line.lower()
                continue
            m = SERVER_ARG.search(line) or (SERVER_KEY.match(line) if section == '[server]' else None)
            if m and m.group(1) not in (s for s, _ in found):
                found.append((m.group(1), f))
    return found


# ---- 3. prepare, 4. frame sites ---------------------------------------------------------------------

def prepared(label, game):
    chosen = buildinfo.current(required=False)
    return (chosen and chosen['build'] == label and os.path.normcase(os.path.normpath(chosen['game'])) == os.path.normcase(game)
            and (ROOT / 'generated/FFXiMain.unpacked.dll').exists())


def has_frame_sites(label):
    return bool(buildinfo.known()[label].get('frame_int_sites'))


# ---- 5. build -----------------------------------------------------------------------------------

def build():
    """build_switch.py host, its @phase and @progress lines (FFXI_PROGRESS, as the launcher reads them) shown as
    notes and a bar."""
    state = {'bar': False}

    def on_line(line):
        if line.startswith('@progress '):
            done, total = map(int, line.split()[1:3])
            state['bar'] = True
            progress(done / total if total else 1.0, f'{done} of {total} files')
            if done >= total:
                sys.stdout.write('\n')
                state['bar'] = False
        elif line.startswith('@phase translate'):
            note("translating the game's code (a few minutes)")
        elif line.startswith('@phase compile'):
            note('compiling (only what changed since the last build)')
        elif line.startswith('built '):
            note(line.strip())

    env = dict(os.environ, FFXI_PROGRESS='1', PYTHONUNBUFFERED='1')
    # Mesa 26, for the shader cache (the first time a download of 12 MB); without it the build links devkitPro's
    # switch-mesa, which plays the same with a hitch the first time each effect is seen in every session
    if os.environ.get('XI_SWITCH_MESA') != '0':
        try:
            run([sys.executable, HERE / 'build_switch.py', 'mesa'], None, env)
            note('Mesa 26: shaders are kept on the card, so each compiles once rather than every session')
        except Failure as e:
            log(str(e))
            note("could not download Mesa 26 (the shader cache): building with devkitPro's Mesa instead")
    try:
        run([sys.executable, HERE / 'build_switch.py', 'host'], on_line, env)
    finally:
        if state['bar']:
            sys.stdout.write('\n')


# ---- 6. the SD card -------------------------------------------------------------------------------

def card_candidates():
    """Mounted folders that look like a Switch's SD card (atmosphere/ or switch/ at the root)."""
    if sys.platform == 'win32':
        roots = [Path(f'{d}:/') for d in string.ascii_uppercase if d not in 'AC' and os.path.exists(f'{d}:/')]
    else:
        roots = []
        for base in ['/Volumes', '/media', '/run/media', '/mnt']:
            b = Path(base)
            if not b.is_dir():
                continue
            for d in b.iterdir():
                roots += [d] + ([s for s in d.iterdir() if s.is_dir()] if base != '/Volumes' and d.is_dir() else [])
    found = []
    for r in roots:
        try:
            if (r / 'atmosphere').is_dir() or (r / 'switch').is_dir():
                found.append(str(r))
        except OSError:
            continue
    return found


def find_card(given, saved):
    if given:
        return given if given == 'emu' else user_path(given)
    found = card_candidates()
    default = found[0] if len(found) == 1 else saved.get('sd', '')
    if len(found) > 1:
        note('These look like Switch SD cards: ' + ', '.join(found))
    elif found:
        note(f'Found a Switch SD card at {found[0]}')
    while True:
        card = ask("The SD card's folder (its root, e.g. E:\\), or emu for the emulator's", default)
        if not card:
            raise Failure("No SD card: put it in the PC (or the console's over USB with Hekate's UMS) and give it with --sd")
        if card == 'emu' or Path(user_path(card)).is_dir():
            return card if card == 'emu' else user_path(card)
        note(f'{card}: not a folder (is the card in?)')
        if not interactive():
            raise Failure(f'{card}: not a folder (is the card in?)')
        default = ''


def drop_old_fps(card, done):
    """An --fps-divisor line in run.args from before the default was chosen for the player (the old setup wrote
    `--fps-divisor 2`): taken out, so the default (uncapped with frame delta sites, else 30) applies. Once a card:
    done (setup.json's list) remembers it, so a line put back by hand afterwards stays."""
    import build_switch
    root = build_switch.sd_root(card)
    if str(root) in done:
        return
    path = root / build_switch.APP_DIR / 'run.args'
    if path.exists():
        old = path.read_text().splitlines()
        new = build_switch.merge_args(old, ['--fps-divisor'])[:-1]  # (merge_args drops it with its value)
        if new != [l.strip() for l in old if l.strip()]:
            path.write_text('\n'.join(new) + ('\n' if new else ''))
            note('took the old --fps-divisor line out of run.args: the default is better (uncapped with frame delta sites)')
    done.append(str(root))


def server_args(card, a):
    """Host options a server needs that the player can't be expected to know: a CatsEyeXI server's own items,
    gear, spells and abilities (--cexi full). Added only when neither --arg nor run.args has --cexi already."""
    if not a.server or not re.search(r'(^|\.)catseyexi\.com$', a.server.split(':')[0], re.I):
        return
    if any('--cexi' in x for x in a.arg or []):
        return
    import build_switch
    path = build_switch.sd_root(card) / build_switch.APP_DIR / 'run.args'
    if path.exists() and '--cexi' in [l.strip() for l in path.read_text().splitlines()]:
        return
    a.arg = (a.arg or []) + ['--cexi full']
    note("a CatsEyeXI server: adding --cexi full to run.args (its own items, gear, spells and abilities)")


SPHAIRA_API = 'https://api.github.com/repos/ITotalJustice/sphaira/releases/latest'
SPHAIRA_NRO = 'switch/sphaira/sphaira.nro'  # (where its release zip puts it)


def find_sphaira(root):
    """A Sphaira already on the card (the player's, any version), or None."""
    sw = root / 'switch'
    hits = list(sw.glob('sphaira.nro')) + list(sw.glob('*/sphaira.nro')) if sw.is_dir() else []
    return hits[0] if hits else None


def get_sphaira():
    """Sphaira's latest release, downloaded to build/switch once: (its .nro's bytes, its version)."""
    import urllib.request
    import zipfile
    req = urllib.request.Request(SPHAIRA_API, headers={'User-Agent': 'xi-on-anything-setup'})
    with urllib.request.urlopen(req, timeout=30) as r:
        release = json.load(r)
    asset = next(x for x in release['assets'] if x['name'].lower() == 'sphaira.zip')
    out = OUT / ('sphaira-' + re.sub(r'[^\w.-]', '_', release['tag_name']) + '.zip')
    if not out.exists():
        note(f"downloading Sphaira {release['tag_name']} from github.com/ITotalJustice/sphaira")
        OUT.mkdir(parents=True, exist_ok=True)
        part = out.with_suffix('.part')
        urllib.request.urlretrieve(asset['browser_download_url'], part)
        part.replace(out)
    with zipfile.ZipFile(out) as z:
        return z.read(SPHAIRA_NRO), release['tag_name']


def has_sigpatches(root):
    """Whether the card looks to have signature patches, without which the console won't start a forwarder:
    sys-patch (patches as the system runs), or patch files for Atmosphère (exefs_patches, kip_patches) or
    Hekate (bootloader/patches.ini)."""
    if (root / 'atmosphere/contents/420000000000000B').is_dir():
        return True
    for d in ('atmosphere/exefs_patches', 'atmosphere/kip_patches'):
        base = root / d
        if base.is_dir() and any(f.is_file() for f in base.rglob('*.ips')):
            return True
    return (root / 'bootloader/patches.ini').is_file()


def put_icon_tools(card):
    """Sphaira on the card for the home screen icon, and a word on signature patches. Never stops the setup:
    the game plays without the icon."""
    import build_switch
    if card == 'emu':
        return
    root = build_switch.sd_root(card)
    have = find_sphaira(root)
    if have:
        note(f'Sphaira is on the card already ({have.relative_to(root).as_posix()}): kept as it is')
    else:
        try:
            nro, tag = get_sphaira()
            dst = root / SPHAIRA_NRO
            dst.parent.mkdir(parents=True, exist_ok=True)
            dst.write_bytes(nro)
            note(f'put Sphaira {tag} on the card ({SPHAIRA_NRO}): it makes the home screen icon')
        except Exception as e:  # (no network, GitHub's rate limit: the rest of the setup is done)
            log(f'Sphaira: {e!r}')
            note('could not download Sphaira, for the home screen icon (the game is installed). Put\n'
                 f'  sphaira.nro from github.com/ITotalJustice/sphaira/releases in {SPHAIRA_NRO} on the card.')
            return
    if not has_sigpatches(root):
        note('no signature patches found on the card: the console may refuse to start the home screen icon\n'
             '  without them (sys-patch is the usual one). Holding R over a game works either way.')


ICON_STEPS = ('For a home screen icon: start Sphaira from the homebrew menu, highlight XI on Anything,\n'
              'open its menu and pick Install Forwarder.')


def put_on_card(card, game, a):
    import build_switch
    ns = argparse.Namespace(mtp=None, sd=card, nro=None, game=game, server=a.server or None, fps_divisor=None,
                            dats=a.dats, layout=a.layout, arg=a.arg, full_check=False)
    try:
        build_switch.setup(ns)
    except SystemExit as e:
        if e.code not in (None, 0):
            raise Failure(str(e.code))


# ---- the whole -----------------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--game', help='the FINAL FANTASY XI folder')
    ap.add_argument('--sd', help="the SD card's root folder, or emu (the emulator's virtual card)")
    ap.add_argument('--server', help='the LandSandBoat server, by name or IP (without it the sign-in screen asks)')
    ap.add_argument('--layout', choices=['nintendo', 'xbox'], help='A/B as on the Switch (default), or swapped as on Xbox')
    ap.add_argument('--dats', help='a DAT overlay folder to copy')
    ap.add_argument('--arg', action='append', help='more host options for run.args, as one string (--arg "--cexi full")')
    ap.add_argument('--no-copy-game', action='store_true', help='leave the game on the card as it is')
    ap.add_argument('--no-icon', action='store_true', help="don't put Sphaira (for a home screen icon) on the card")
    ap.add_argument('--check', action='store_true', help='check this PC and stop')
    ap.add_argument('--console', action='store_true', help=argparse.SUPPRESS)  # (setup-switch's: not the window)
    a = ap.parse_args()
    saved = load_saved()
    log('=== setup_switch.py ' + ' '.join(sys.argv[1:]))

    step(0)
    problems = check_pc(fix=not a.check)
    for p in problems:
        note('missing: ' + p)
    if problems:
        raise Failure('Install what is missing above, then run this again.')
    note('this PC has everything the build needs')
    if a.check:
        return

    step(1)
    game = find_game(a.game, saved)
    label = identify(game)
    note(f'{game}: game version {label}')

    # the questions up front, so the long steps run without waiting on the player
    card = find_card(a.sd, saved)
    if a.server is None:
        default = saved.get('server', '') if saved.get('game') == game else ''
        if not default:
            hints = server_hints(game)
            if hints:
                default = hints[0][0]
                note(f'your launcher uses the server {default} ({hints[0][1]})')
                for other, where in hints[1:]:
                    note(f'  (also {other}, in {where})')
        a.server = ask("The server's address, as your launcher uses it (a name like play.example.com, or an IP;\n"
                       '  blank: the sign-in screen asks)', default)
    a.layout = a.layout or saved.get('layout')
    a.dats = user_path(a.dats) if a.dats else None
    fps_done = saved.get('fps_cleaned', [])
    save({'game': game, 'sd': card, 'server': a.server, 'layout': a.layout, 'fps_cleaned': fps_done})

    step(2)
    if prepared(label, game):
        note('already prepared')
    else:
        run([sys.executable, HERE / 'prepare.py', '--game', game])
        note('done')

    step(3)
    if has_frame_sites(label):
        note(f'meta/builds.json has them for {label}: the Switch runs uncapped, the world at its real speed')
    else:
        note(f'finding them in {label} (tools/version/frame_sites.py)')
        try:
            run([sys.executable, HERE / 'version' / 'frame_sites.py'])
            note('found and recorded in meta/builds.json: the Switch runs uncapped. (Worth sending upstream:\n'
                 '  a pull request with meta/builds.json gives every player of this version the same.)')
        except Failure as e:
            note('not found, so the Switch keeps 30 fps (it plays fine). Details in ' + str(LOG))
            log(str(e))

    step(4)
    note('The first build compiles about 6 million lines of C and takes a while (an hour or more on some PCs);\n'
         '  later builds compile only what changed.')
    build()

    step(5)
    if a.no_copy_game:
        note('leaving the game on the card as it is (--no-copy-game)')
    else:
        note('the game is about 14 GB the first time; after that only what changed is copied')
    server_args(card, a)
    drop_old_fps(card, fps_done)  # (first, so the run.args setup prints is the one left on the card)
    put_on_card(card, None if a.no_copy_game else game, a)
    if not a.no_icon:
        put_icon_tools(card)
    save({'game': game, 'sd': card, 'server': a.server, 'layout': a.layout, 'fps_cleaned': fps_done})

    print('\nDone. On the Switch: hold R while starting any game to open the homebrew menu with full memory,\n'
          'then pick XI on Anything. Its log is sdmc:/switch/xi-on-anything/host64.log.\n'
          'After a game update or a new version of this repository, run this again: it does only what changed.')
    if not a.no_icon and card != 'emu':
        print(ICON_STEPS)


if __name__ == '__main__':
    try:
        main()
    except Failure as e:
        print('\n' + str(e), file=sys.stderr)
        raise SystemExit(1)
    except KeyboardInterrupt:
        print('\nStopped. Run it again to carry on from where it was.', file=sys.stderr)
        raise SystemExit(130)
