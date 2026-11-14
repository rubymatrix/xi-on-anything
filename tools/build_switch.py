#!/usr/bin/env python3
"""Build XI on Anything for the Nintendo Switch (homebrew, libnx) and put it on an SD card.

  python tools/build_switch.py plat-test
        the platform layer's self-test (tests/switch_plat_test.c) as build/switch/plat-test.nro
  python tools/build_switch.py setup --sd <SD root> [--nro file.nro] [--game "<FINAL FANTASY XI folder>"]
        [--server <name or IP>] [--fps-divisor N] [--dats <folder>] [--layout nintendo|xbox] [--arg ...]
        copy the .nro to sdmc:/switch/xi-on-anything/ and, with --game, the game beside it (only the
        files that changed since the last copy); write run.args from the options. <SD root> is the card
        in a reader, the console's SD mounted over USB (Hekate's UMS), or an emulator's virtual SD card
        (--sd emu: the one of the emulator `run` uses).
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


def run(args, **kw):
    args = [str(a) for a in args]
    line = ' '.join(args)
    print('> ' + (line if len(line) < 200 else line[:200] + ' ...'), flush=True)
    subprocess.run(args, cwd=ROOT, check=True, env=tc.env(), **kw)


def compile_one(job):
    src, obj, extra = job
    s = ROOT / src
    obj.parent.mkdir(parents=True, exist_ok=True)
    cxx = s.suffix == '.cpp'
    compiler = tc.tool('g++' if cxx else 'gcc')
    f = flags() + list(extra) + (['-std=gnu++17', '-fno-rtti'] if cxx else ['-std=gnu11'])
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


def compile_many(srcs, group, extra=()):
    jobs = [(s, OUT / 'obj' / group / (s.replace('/', '_') + '.o'), extra) for s in srcs]
    print(f'{group}: {len(jobs)} files (compiling the ones changed since the last build)', flush=True)
    errors = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as ex:
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
    own = ROOT / 'switch/icon.jpg'
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
    'tests/switch_plat_test.c',
]


def plat_test(_a):
    tc.require()
    OUT.mkdir(parents=True, exist_ok=True)
    objs = compile_many(PLAT_TEST, 'plat-test')
    return link_nro(objs, 'plat-test', ['-lnx', '-lm'], title='XI on Anything platform test')


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


def copy_tree(src, dst):
    """src into dst, only files whose size or modification time differ: a copy that stopped part way, or
    a game update, sends only what is missing or changed. Returns (files copied, bytes copied)."""
    copied = size = 0
    files = [p for p in src.rglob('*') if p.is_file()]
    t0 = time.time()
    for i, p in enumerate(files, 1):
        out = dst / p.relative_to(src)
        st = p.stat()
        if out.exists():
            o = out.stat()
            if o.st_size == st.st_size and abs(o.st_mtime - st.st_mtime) < 2:  # FAT keeps 2 s
                continue
        out.parent.mkdir(parents=True, exist_ok=True)
        tmp = out.with_name(out.name + '.part')
        shutil.copyfile(p, tmp)
        os.utime(tmp, (st.st_atime, st.st_mtime))
        os.replace(tmp, out)
        copied += 1
        size += st.st_size
        if copied % 500 == 0:
            rate = size / max(time.time() - t0, 0.1) / 1e6
            print(f'  {i}/{len(files)} files checked, {copied} copied ({size / 1e9:.2f} GB, {rate:.0f} MB/s)', flush=True)
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


def setup(a):
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
    shutil.copyfile(ROOT / 'ffxi.reg', app / 'ffxi.reg')
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
    s = sub.add_parser('setup')
    s.add_argument('--sd', required=True, help="the SD card's root folder, or emu")
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
    {'plat-test': plat_test, 'setup': setup, 'run': emu_run, 'deploy': deploy, 'logs': logs}[a.target](a)


if __name__ == '__main__':
    try:
        main()
    except subprocess.CalledProcessError as e:
        print('failed: ' + ' '.join(map(str, e.cmd))[:300], file=sys.stderr)
        raise SystemExit(1)
