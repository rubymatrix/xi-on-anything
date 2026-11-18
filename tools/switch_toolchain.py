"""Paths shared by the Nintendo Switch tools (build_switch.py).

DEVKITPRO is devkitPro's root, as devkitPro's own installer and msys2 set it (`/opt/devkitpro` inside
msys2). On Windows the msys2 form is translated with the msys2 root (XI_MSYS2, default C:/devkitPro/msys2
for devkitPro's installer, then C:/msys64 and D:/msys64 for a plain msys2 with devkitPro's pacman repos);
without DEVKITPRO, C:/devkitPro and the msys2 roots' opt/devkitpro are tried in turn.
XI_SWITCH_BUILD_DIR is the output (default build/switch). XI_SWITCH_EMU is an emulator to run builds in
(build_switch.py run), default a portable Ryubing found under XI_EMU_DIR.
"""

import os
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EXE = '.exe' if sys.platform == 'win32' else ''
OUT = Path(os.environ.get('XI_SWITCH_BUILD_DIR', str(ROOT / 'build/switch'))).resolve()


def _msys_roots():
    roots = [os.environ.get('XI_MSYS2'), 'C:/devkitPro/msys2', 'C:/msys64', 'D:/msys64']
    return [Path(r) for r in roots if r]


def devkitpro():
    """devkitPro's root as a host path, or None."""
    env = os.environ.get('DEVKITPRO')
    candidates = []
    if env:
        if sys.platform == 'win32' and env.startswith('/'):
            # msys2's /opt/devkitpro, inside each msys2 root
            candidates += [r / env.lstrip('/') for r in _msys_roots()]
        else:
            candidates.append(Path(env))
    if sys.platform == 'win32':
        candidates += [Path('C:/devkitPro')] + [r / 'opt/devkitpro' for r in _msys_roots()]
    else:
        candidates.append(Path('/opt/devkitpro'))
    for c in candidates:
        if (c / 'devkitA64/bin' / ('aarch64-none-elf-gcc' + EXE)).exists():
            return c.resolve()
    return None


DKP = devkitpro()
A64 = DKP / 'devkitA64/bin' if DKP else None
LIBNX = DKP / 'libnx' if DKP else None
PORTLIBS = DKP / 'portlibs/switch' if DKP else None
TOOLS = DKP / 'tools/bin' if DKP else None


def env():
    """The environment for devkitPro's tools: switch.specs finds libnx through DEVKITPRO, which must be
    a host path for the native compiler (not msys2's /opt/devkitpro)."""
    e = dict(os.environ)
    if DKP:
        e['DEVKITPRO'] = DKP.as_posix()
    return e


def tool(name):
    return str(A64 / ('aarch64-none-elf-' + name + EXE))


def dkp_tool(name):
    return str(TOOLS / (name + EXE))


def require():
    if not DKP:
        raise SystemExit(
            'devkitPro not found: install it (https://devkitpro.org/wiki/Getting_Started) with '
            '`pacman -S switch-dev switch-mesa switch-libdrm_nouveau switch-glad switch-zlib switch-libexpat`, and set DEVKITPRO '
            '(or XI_MSYS2 to the msys2 it is in)'
        )


def emulator():
    """An emulator's executable to run an .nro in, or None: XI_SWITCH_EMU, else a portable Ryubing
    under XI_EMU_DIR (default D:/Emulator/Ryubing), newest version folder first."""
    env = os.environ.get('XI_SWITCH_EMU')
    if env:
        return Path(env)
    if sys.platform == 'darwin':  # Ryubing's macOS app
        for app in (Path('/Applications/Ryujinx.app'), Path.home() / 'Applications/Ryujinx.app'):
            exe = app / 'Contents/MacOS/Ryujinx'
            if exe.exists():
                return exe
    base = Path(os.environ.get('XI_EMU_DIR', 'D:/Emulator/Ryubing'))
    if base.is_dir():
        for d in sorted((d for d in base.iterdir() if d.is_dir()), reverse=True):
            exe = d / ('Ryujinx' + EXE)
            if exe.exists():
                return exe
    return None


def emulator_sd(exe):
    """The emulator's virtual SD card: a portable Ryubing keeps it beside itself, else its user folder."""
    portable = Path(exe).parent / 'portable'
    if portable.is_dir():
        return portable / 'sdcard'
    if sys.platform == 'win32':
        return Path(os.environ.get('APPDATA', '')) / 'Ryujinx/sdcard'
    if sys.platform == 'darwin':
        return Path.home() / 'Library/Application Support/Ryujinx/sdcard'
    return Path.home() / '.config/Ryujinx/sdcard'
