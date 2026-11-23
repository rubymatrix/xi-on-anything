#!/usr/bin/env python3
"""Build XI on Anything for the Nintendo Switch (homebrew, libnx) and put it on an SD card.

  python tools/build_switch.py plat-test
        the platform layer's self-test (platforms/switch/tests/switch_plat_test.c) as build/switch/plat-test.nro
  python tools/build_switch.py gfx-test
        the graphics back end's test (tests/gfx/gfx_test.c on OpenGL, through EGL) as build/switch/gfx-test.nro:
        its lines to gfx-test.log on the card, then green on the screen when every check passed, red when not
  python tools/build_switch.py setup --sd <SD root> [--nro file.nro] [--game "<FINAL FANTASY XI folder>"]
        [--server <name or IP>] [--fps-divisor N] [--dats <folder>] [--layout nintendo|xbox] [--arg ...]
        copy the .nro to sdmc:/switch/xi-on-anything/ and, with --game, the game beside it (only the
        files that changed since the last copy); write run.args from the options. <SD root> is the card
        in a reader, the console's SD mounted over USB (Hekate's UMS), or an emulator's virtual SD card
        (--sd emu: the one of the emulator `run` uses). --mtp [device] instead: the console over USB as an
        MTP device (DBI's MTP responder), on Windows (platforms/switch/switch_mtp.ps1).
  python tools/build_switch.py run [file.nro]
        run an .nro in the emulator (switch_toolchain.emulator()): copies it to the emulator's SD card first
  python tools/build_switch.py deploy --ip <console IP> [file.nro]
        send an .nro to hbmenu over the network (nxlink; press Y in hbmenu first), the game data left alone
  python tools/build_switch.py logs --sd <SD root>
        print the logs the app left on the card (host64.log, plat-test.log)

Needs devkitPro with switch-dev (switch_toolchain.py says where it looks). Outputs go to build/switch
(XI_SWITCH_BUILD_DIR). The game's own code is never in this repository: an .nro built from it is built
from your FFXiMain.dll, on your machine, and is never shared (docs/platforms/README.md).
"""

import argparse
import concurrent.futures
import hashlib
import json
import os
import shlex
import shutil
import subprocess
import sys
import time
from pathlib import Path

import switch_toolchain as tc
from switch_toolchain import OUT, ROOT

APP_DIR = 'switch/xi-on-anything'  # on the SD card: the .nro, run.args, the game, the logs
TITLE = 'XI on Anything'
AUTHOR = 'xi-on-anything'

ARCH = ['-march=armv8-a+crc+crypto', '-mtune=cortex-a57', '-mtp=soft', '-fPIE']


def flags():
    return ARCH + [
        '-O2',
        '-g',
        '-ffunction-sections',
        '-fno-strict-aliasing',
        '-D__SWITCH__',
        '-DRT_GUEST_WINDOW',
        '-DFFXI_SWITCH',
        '-DFFXI_GUEST_2GB',
        '-I' + str(ROOT / 'runtime'),
        '-I' + str(ROOT / 'runtime/portable'),
        '-I' + str(ROOT / 'generated'),
        '-I' + str(ROOT / 'third_party/stb'),
        '-I' + str(tc.PORTLIBS / 'include'),
        '-I' + str(tc.LIBNX / 'include'),
    ]


# Ours, not the translation's: devkitA64's GCC 16.1 drops the stores into a calloc'd struct a function returns
# beside another pointer (gfx_queue.c's batch_get: the first batch had cap 0 and p NULL, rec doubled 0 for
# ever - the black screen at start, which any added code could hide). Its points-to analysis is the pass at
# fault. The translated code works in guest memory, not on objects like these, and keeps it.
OWN = ['-fno-tree-pta']


def run(args, **kw):
    args = [str(a) for a in args]
    line = ' '.join(args)
    print('> ' + (line if len(line) < 200 else line[:200] + ' ...'), flush=True)
    kw.setdefault('cwd', ROOT)
    subprocess.run(args, check=True, env=tc.env(), **kw)


def compile_one(job):
    src, obj, extra = job
    s = ROOT / src
    obj.parent.mkdir(parents=True, exist_ok=True)
    cxx = s.suffix == '.cpp'
    compiler = tc.tool('g++' if cxx else 'gcc')
    f = flags() + ([] if src.startswith('generated/') else OWN) + list(extra) + (
        ['-std=gnu++17', '-fno-rtti'] if cxx else ['-std=gnu11'])
    key = hashlib.sha256(s.read_bytes() + json.dumps([compiler, f]).encode()).hexdigest()
    stamp, deps = obj.with_suffix('.stamp'), obj.with_suffix('.d')
    stale = not obj.exists() or not stamp.exists() or not deps.exists() or stamp.read_text() != key
    if not stale:
        # a header changed since: the depfile lists every file the object was built from
        for dep in shlex.split(deps.read_text().replace('\\\n', ' ').split(':', 1)[-1]):
            p = Path(dep)
            p = p if p.is_absolute() else ROOT / p
            if not p.exists() or p.stat().st_mtime_ns > obj.stat().st_mtime_ns:
                stale = True
                break
    if stale:
        cmd = [compiler] + f + ['-MMD', '-MF', str(deps), '-c', str(s), '-o', str(obj)]
        r = subprocess.run(cmd, cwd=ROOT, text=True, capture_output=True)
        if r.returncode:
            return src, r.stdout + r.stderr
        stamp.write_text(key)
    return str(obj), None


def compile_many(srcs, group, extra=(), workers=None):
    """workers: at most this many compilers at once (the generated files are ~3 MB of C each, and GCC at
    -O2 takes gigabytes for one: XI_SWITCH_JOBS for those, default 6)"""
    jobs = [(s, OUT / 'obj' / group / (s.replace('/', '_') + '.o'), extra) for s in srcs]
    print(f'{group}: {len(jobs)} files (compiling the ones changed since the last build)', flush=True)
    errors = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=workers or os.cpu_count() or 4) as ex:
        for name, err in ex.map(compile_one, jobs):
            if err:
                errors.append((name, err))
    if errors:
        for name, err in errors:
            print(name + '\n' + err[-6000:], file=sys.stderr)
        raise SystemExit(1)
    return [j[1] for j in jobs]


def version():
    return json.loads((ROOT / 'runtime.json').read_text())['version']


def icon():
    own = ROOT / 'platforms/switch/icon.jpg'
    return own if own.exists() else tc.LIBNX / 'default_icon.jpg'


def link_nro(objs, name, libs, title=TITLE):
    """objs into build/switch/<name>.elf, then .nro with its NACP (title, author, version) and icon."""
    elf, nacp, nro = OUT / (name + '.elf'), OUT / (name + '.nacp'), OUT / (name + '.nro')
    rsp = OUT / (name + '.rsp')  # the object list in a response file: Windows' command line is short
    rsp.write_text('\n'.join('"' + str(o).replace('\\', '/') + '"' for o in objs) + '\n')
    run(
        [tc.tool('g++'), '-specs=' + str(tc.LIBNX / 'switch.specs'), '-g'] + ARCH
        + ['-Wl,-Map,' + str(OUT / (name + '.map')), '@' + str(rsp)]
        + ['-L' + str(tc.PORTLIBS / 'lib'), '-L' + str(tc.LIBNX / 'lib')] + libs + ['-o', elf]
    )
    run([tc.dkp_tool('nacptool'), '--create', title, AUTHOR, version(), nacp])
    run([tc.dkp_tool('elf2nro'), elf, nro, '--nacp=' + str(nacp), '--icon=' + str(icon())])
    print('built', nro, f'({nro.stat().st_size / 1e6:.1f} MB)')
    return nro


# --- targets -------------------------------------------------------------------------------------------
PLAT_TEST = [
    'runtime/portable/plat_posix.c',
    'runtime/portable/gwin.c',
    'platforms/switch/tests/switch_plat_test.c',
]


def plat_test(_a):
    tc.require()
    OUT.mkdir(parents=True, exist_ok=True)
    objs = compile_many(PLAT_TEST, 'plat-test')
    return link_nro(objs, 'plat-test', ['-lnx', '-lm'], title='XI on Anything platform test')


def gfx_test(_a):
    """tests/gfx/gfx_test.c on the OpenGL back end, through EGL (platforms/switch/tests/switch_gfx_test.c): build/switch/gfx-test.nro"""
    tc.require()
    OUT.mkdir(parents=True, exist_ok=True)
    sdl = ['-I' + str(ROOT / 'third_party/sdl3/include')]
    objs = compile_many(GFX + ['platforms/switch/sdl_nx.c', 'runtime/portable/plat_posix.c', 'runtime/portable/gwin.c',
                               'platforms/switch/tests/switch_gfx_test.c'], 'gfx-test', sdl)
    objs += compile_many(['tests/gfx/gfx_test.c'], 'gfx-test-main', sdl + ['-DGFX_GL', '-Dmain=gfx_test_main'])
    return link_nro(objs, 'gfx-test', GFX_LIBS + ['-lnx', '-lm'], title='XI on Anything graphics test')


# mbedtls' time and entropy from platforms/switch/tls_nx.c (platforms/switch/mbedtls_config.h), for it and its includers
MBEDTLS_CONFIG = ['-I' + (ROOT / 'platforms' / 'switch').as_posix(), '-DTF_PSA_CRYPTO_USER_CONFIG_FILE=<mbedtls_config.h>',
                  '-DMBEDTLS_USER_CONFIG_FILE=<mbedtls_config.h>']


def desktop_tools():
    """build.py / build_posix.py / thirdparty.py, with the third-party libraries (mbedtls) compiled by
    devkitA64 into build/switch/third_party: thirdparty.py reads its compilers when it is imported."""
    # forward slashes: thirdparty.py shlex-splits these, which would eat Windows' backslashes
    os.environ['XI_CC'] = Path(tc.tool('gcc')).as_posix()
    os.environ['XI_CXX'] = Path(tc.tool('g++')).as_posix()
    os.environ['XI_AR'] = Path(tc.tool('ar')).as_posix()
    os.environ['XI_TP_DIR'] = str(OUT / 'third_party')
    os.environ['XI_TP_CFLAGS'] = ' '.join(ARCH + ['-D__SWITCH__', '-ffunction-sections', '-I' + (tc.LIBNX / 'include').as_posix()]
                                          + [shlex.quote(f) for f in MBEDTLS_CONFIG])
    os.environ.update({k: v for k, v in tc.env().items() if k == 'DEVKITPRO'})
    import build
    import build_posix
    import thirdparty
    return build, build_posix, thirdparty


# the graphics: OpenGL 4.3 core (gfx_gl.c) on Mesa's nouveau, through EGL (switch-mesa, switch-libdrm_nouveau)
GFX = ['runtime/portable/gfx/gfx_gl.c', 'runtime/portable/gfx/gfx_msl.c', 'runtime/portable/gfx/gfx_msl_shaders.c',
       'runtime/portable/gfx/gfx_fx.c']
GFX_LIBS = ['-lEGL', '-lglapi', '-ldrm_nouveau']


def host(a):
    """The game: build/switch/xi-on-anything.nro."""
    tc.require()
    OUT.mkdir(parents=True, exist_ok=True)
    build, bp, thirdparty = desktop_tools()
    if build.BUILD is None:
        raise SystemExit('prepare the game first: python tools/prepare.py --game "<FINAL FANTASY XI folder>"')
    print(f'game build {build.BUILD["build"]}', flush=True)
    if not a.no_translate:
        # the desktop translation, shared with host64 (generated/all) and FFXi.dll's (generated/ffxi)
        bp.translate()
        bp.run([sys.executable, 'recomp/recomp.py', '--meta', build.FFXI_META, '--image', bp.GEN_FFXI_IMAGE,
                '--retail', build.FFXI_RETAIL, '--module', 'ffxi', '--out', 'generated/ffxi', '--all'])
    build.write_build_h()  # generated/build.h: this game build's addresses (translate() writes it too)
    thirdparty.build('mbedtls')
    tls = thirdparty.flags('mbedtls') + MBEDTLS_CONFIG
    # ours first: its errors show in seconds, the translation's compile takes a while
    sdl = ['-I' + str(ROOT / 'third_party/sdl3/include')]
    hosts = [s for s in bp.HOST_SOURCES if s not in bp.GFX_SOURCES and s != 'host/discord.c'] + GFX + [
        'platforms/switch/sdl_nx.c', 'platforms/switch/tls_nx.c', 'host/addons_none.c']
    # the render queue: GL's context is its render thread's, and the game calls from several threads
    queue = ['-DGFX_GL', '-DGFX_QUEUE']
    objs = compile_many(bp.PORTABLE + hosts + ['runtime/portable/gfx/gfx_queue.c'], 'host',
                        sdl + tls + queue + ['-Dmain=xi_host_main'])
    objs += compile_many(['platforms/switch/switch_main.c'], 'entry', sdl)
    # GCC's names for the generated C's warnings (GEN_WARNINGS is clang's)
    warn = ['-w']
    jobs = int(os.environ.get('XI_SWITCH_JOBS', '6'))
    objs += compile_many(bp.generated('all'), 'all', ['-I' + str(ROOT / 'generated/all')] + warn, jobs)
    objs += compile_many(bp.generated('ffxi'), 'ffxi', ['-I' + str(ROOT / 'generated/ffxi')] + warn, jobs)
    return link_nro(objs, 'xi-on-anything', [thirdparty.archive('mbedtls')] + GFX_LIBS + ['-lnx', '-lm'])


# --- the SD card ---------------------------------------------------------------------------------------
def sd_root(value):
    if value == 'emu':
        exe = tc.emulator()
        if not exe:
            raise SystemExit('no emulator found: set XI_SWITCH_EMU to its executable')
        return tc.emulator_sd(exe)
    root = Path(value)
    if not root.is_dir():
        raise SystemExit(f'{root}: not a folder (the SD card is not mounted?)')
    return root


def copy_tree(src, dst, workers=4):
    """src into dst, only files whose size or modification time differ: a copy that stopped part way, or
    a game update, sends only what is missing or changed. Returns (files copied, bytes copied).

    Written in place, a few at a time: on an SD card's FAT32 the time goes on each file's directory
    updates, not its bytes (the game is 65k small files), and a file cut short still differs in size or
    time from its source, so the next run sends it again."""
    files = [p for p in src.rglob('*') if p.is_file()]
    todo = []
    made = set()
    for p in files:
        out = dst / p.relative_to(src)
        st = p.stat()
        try:
            o = out.stat()
            if o.st_size == st.st_size and abs(o.st_mtime - st.st_mtime) < 2:  # FAT keeps 2 s
                continue
        except FileNotFoundError:
            pass
        if out.parent not in made:
            out.parent.mkdir(parents=True, exist_ok=True)
            made.add(out.parent)
        todo.append((p, out, st))
    print(f'  {len(files)} files, {len(todo)} to copy', flush=True)

    def one(job):
        p, out, st = job
        shutil.copyfile(p, out)
        os.utime(out, (st.st_atime, st.st_mtime))
        return st.st_size

    copied = size = 0
    t0 = time.time()
    with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as ex:
        for n in ex.map(one, todo):
            copied += 1
            size += n
            if copied % 1000 == 0:
                rate = copied / max(time.time() - t0, 0.1)
                print(f'  {copied}/{len(todo)} files copied ({size / 1e9:.2f} GB, {rate:.0f} files/s)', flush=True)
    return copied, size


def run_args(a):
    """run.args: one argument per line, as the app reads it (and as Android's run.args)."""
    lines = []
    if a.server:
        lines += ['--server', a.server]
    if a.fps_divisor is not None:
        lines += ['--fps-divisor', str(a.fps_divisor)]
    if a.dats:
        lines += ['--dats', 'sdmc:/' + APP_DIR + '/dats']
    if a.layout:
        lines += ['--switch-layout', a.layout]
    for extra in a.arg or []:
        lines += shlex.split(extra)
    return lines


def mtp_mirror(a, src, dest):
    """src to dest (a path on the device's storage) over MTP (platforms/switch/switch_mtp.ps1)."""
    run(['powershell', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', ROOT / 'platforms/switch/switch_mtp.ps1',
         '-Source', src, '-Dest', dest, '-Device', a.mtp, '-Storage', a.storage])


def setup_mtp(a):
    """setup over MTP: the app's files are staged in build/switch/sd, then everything is mirrored."""
    if os.name != 'nt':
        raise SystemExit('--mtp is for Windows; on Linux or macOS mount the card (or use Hekate UMS) and use --sd')
    stage = OUT / 'sd' / APP_DIR
    stage.mkdir(parents=True, exist_ok=True)
    nro = Path(a.nro) if a.nro else OUT / 'xi-on-anything.nro'
    if nro.exists():
        shutil.copyfile(nro, stage / nro.name)
    elif a.nro:
        raise SystemExit(f'{nro}: no such file')
    shutil.copyfile(ROOT / 'assets' / 'ffxi.reg', stage / 'ffxi.reg')
    shutil.copyfile(ROOT / 'platforms/switch/switch.reg', stage / 'switch.reg')  # the Switch's defaults over it
    lines = run_args(a)
    if lines or not (stage / 'run.args').exists():
        (stage / 'run.args').write_text('\n'.join(lines) + ('\n' if lines else ''))
        print('run.args:', ' '.join(lines) or '(empty: the sign-in screen asks)')
    for extra in a.extra or []:
        shutil.copyfile(extra, stage / Path(extra).name)
    mtp_mirror(a, stage, APP_DIR)
    if a.game:
        game = Path(a.game)
        if not (game / 'FFXiMain.dll').exists():
            raise SystemExit(f'{game}: not a FINAL FANTASY XI folder (no FFXiMain.dll)')
        for folder in [game] + [p for p in [game.parent / 'PlayOnlineViewer'] if p.is_dir()]:
            mtp_mirror(a, folder, APP_DIR + '/SquareEnix/' + folder.name)
    if a.dats:
        mtp_mirror(a, Path(a.dats), APP_DIR + '/dats')
    print('done:', f'{a.mtp}:/{APP_DIR}')


def setup(a):
    if a.mtp:
        return setup_mtp(a)
    if not a.sd:
        raise SystemExit('setup needs --sd <the card\'s folder> (or emu), or --mtp <device> for a Switch over USB MTP')
    root = sd_root(a.sd)
    app = root / APP_DIR
    app.mkdir(parents=True, exist_ok=True)
    nro = Path(a.nro) if a.nro else OUT / 'xi-on-anything.nro'
    if nro.exists():
        shutil.copyfile(nro, app / nro.name)
        print('copied', nro.name, '->', app)
    elif a.nro:
        raise SystemExit(f'{nro}: no such file')
    else:
        print(f'no {nro.name} built yet: the game data and settings only')
    if a.game:
        game = Path(a.game)
        if not (game / 'FFXiMain.dll').exists():
            raise SystemExit(f'{game}: not a FINAL FANTASY XI folder (no FFXiMain.dll)')
        # the layout the game expects: SquareEnix/FINAL FANTASY XI, the viewer folder beside it
        dest = app / 'SquareEnix'
        for folder in [game] + [p for p in [game.parent / 'PlayOnlineViewer'] if p.is_dir()]:
            print(f'copying {folder} -> {dest / folder.name} (only what changed)', flush=True)
            n, size = copy_tree(folder, dest / folder.name)
            print(f'  {n} files copied, {size / 1e9:.2f} GB')
    if a.dats:
        n, size = copy_tree(Path(a.dats), app / 'dats')
        print(f'DAT overlay: {n} files copied, {size / 1e6:.0f} MB')
    shutil.copyfile(ROOT / 'assets' / 'ffxi.reg', app / 'ffxi.reg')
    shutil.copyfile(ROOT / 'platforms/switch/switch.reg', app / 'switch.reg')  # the Switch's defaults over it
    lines = run_args(a)
    if lines or not (app / 'run.args').exists():
        (app / 'run.args').write_text('\n'.join(lines) + ('\n' if lines else ''))
        print('run.args:', ' '.join(lines) or '(empty: the sign-in screen asks)')
    print('done:', app)


def emu_run(a):
    exe = tc.emulator()
    if not exe:
        raise SystemExit('no emulator found: set XI_SWITCH_EMU to its executable')
    nro = Path(a.nro) if a.nro else OUT / 'xi-on-anything.nro'
    app = tc.emulator_sd(exe) / APP_DIR
    app.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(nro, app / nro.name)
    # the emulator runs it from the SD card, where it finds run.args and the game beside it
    print(f'running {app / nro.name} in {exe}')
    # its own process group, no handles of ours: the command returns while it runs (DETACHED_PROCESS
    # stops Ryubing from starting at all)
    detached = {'creationflags': subprocess.CREATE_NEW_PROCESS_GROUP} if os.name == 'nt' else {'start_new_session': True}
    subprocess.Popen([str(exe), str(app / nro.name)], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                     stderr=subprocess.DEVNULL, **detached)


def deploy(a):
    tc.require()
    nro = Path(a.nro) if a.nro else OUT / 'xi-on-anything.nro'
    run([tc.dkp_tool('nxlink'), '-a', a.ip, '-p', APP_DIR + '/' + nro.name, nro])


def logs(a):
    app = sd_root(a.sd) / APP_DIR
    for name in ['plat-test.log', 'host64.log']:
        p = app / name
        if p.exists():
            print(f'--- {p} ({time.ctime(p.stat().st_mtime)})')
            print(p.read_text(errors='replace'))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    sub = ap.add_subparsers(dest='target', required=True)
    sub.add_parser('plat-test')
    sub.add_parser('gfx-test')
    h = sub.add_parser('host')
    h.add_argument('--no-translate', action='store_true', help='keep generated/all and generated/ffxi as they are')
    s = sub.add_parser('setup')
    s.add_argument('--sd', help="the SD card's root folder, or emu")
    s.add_argument('--mtp', nargs='?', const='Switch', help="the console over USB MTP (DBI's MTP responder), by its "
                   "name in This PC (default Switch); Windows only")
    s.add_argument('--storage', default='1: SD Card', help="--mtp: the device's storage (default '1: SD Card')")
    s.add_argument('--extra', action='append', help='--mtp: another file to put beside the .nro (plat-test.nro)')
    s.add_argument('--nro')
    s.add_argument('--game', help='the FINAL FANTASY XI folder to copy')
    s.add_argument('--server', help='the LandSandBoat server, by name or IP')
    s.add_argument('--fps-divisor', type=int, choices=[0, 1, 2])
    s.add_argument('--dats', help='a DAT overlay folder to copy (--dats)')
    s.add_argument('--layout', choices=['nintendo', 'xbox'], help='A/B as on the Switch, or swapped as on Xbox')
    s.add_argument('--arg', action='append', help='more host options, as one string')
    r = sub.add_parser('run')
    r.add_argument('nro', nargs='?')
    d = sub.add_parser('deploy')
    d.add_argument('--ip', required=True)
    d.add_argument('nro', nargs='?')
    lg = sub.add_parser('logs')
    lg.add_argument('--sd', required=True)
    a = ap.parse_args()
    {'plat-test': plat_test, 'gfx-test': gfx_test, 'host': host, 'setup': setup, 'run': emu_run, 'deploy': deploy, 'logs': logs}[a.target](a)


if __name__ == '__main__':
    try:
        main()
    except subprocess.CalledProcessError as e:
        print('failed: ' + ' '.join(map(str, e.cmd))[:300], file=sys.stderr)
        raise SystemExit(1)
