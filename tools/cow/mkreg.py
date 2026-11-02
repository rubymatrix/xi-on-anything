#!/usr/bin/env python3
"""Build the .reg that tests/boot64.c is handed as argv[3], from the real install.

    python tools/cow/mkreg.py --game "C:\\PhoenixXI\\SquareEnix\\FINAL FANTASY XI" \
        [--lang us] [--ini C:\\PhoenixXI\\config\\boot\\phoenix-launch.ini] \
        [--out build/ffxi.reg] [--fixture ffxi.reg]

Why this exists
---------------
tools/build.py used to fill build/ffxi.reg with `reg export HKLM\\SOFTWARE\\WOW6432Node\\PlayOnlineUS`,
so the boot test silently depended on whatever FFXI registration the build machine happened
to have. On this box that key is a vestigial HorizonXI record (only 0022/0034/0042 + pad
values, no 0001), so boot64.c's `reg_get_dword(..., "0001")` failed and the test exited 1
before any guest code ran.

Where the settings really live
------------------------------
Which vendor key the client reads was established by reading the recompiled code, not guessed:

  f_10018ee0  assembles the subkey with "%s%s" from
              [0x10350fec + 4*lang] and "FinalFantasyXI" (0x10351000), where lang is the
              dword at [0x10456a28] + 4 (the region struct f_100157e0 installs at 0x10456984).
              The table is:
                  [0] 0x1032a000  SOFTWARE\\PlayOnline\\SQUARE\\         JP
                  [1] 0x1032a01c  SOFTWARE\\PlayOnlineUS\\SquareEnix\\    US
                  [2] 0x1032a040  SOFTWARE\\PlayOnlineEU\\SquareEnix\\    EU
  bootloader/xiloader.exe agrees, and documents the same numbering:
                  "--lang  (optional) The language of your FFXI install: JP/US/EU (0/1/2)."
  The Phoenix launcher is Ashita-driven; config/boot/phoenix-launch.ini has
                  [ashita.language] playonline = 2
  which Ashita's own documentation defines as 0=Default, 1=Japanese, 2=English, 3=European.
  So this install runs as the English/US client -> lang 1 -> ...PlayOnlineUS\\SquareEnix\\...
  which is the key tests/boot64.c already checks.

The values themselves come from the launcher's own config: phoenix-launch.ini's [ffxi.registry]
section holds the full 0000-0045 set that the launcher injects into FinalFantasyXI. That is
this machine's real, current Phoenix settings -- preferred over both the stale HKLM record and
the upstream fixture.

Sources, in priority order
--------------------------
  FinalFantasyXI values   [ffxi.registry] in the launcher ini   -> fixture for anything absent
  InstallFolder, Interface, CommonFilesFolder   live HKLM (read-only reg query)
  FinalFantasyXI\\0042 and InstallFolder\\0001   always the --game path
Nothing is ever written to the registry; HKLM is only read.
"""

import argparse
import configparser
import os
import re
import subprocess
import sys

HKLM = 'HKEY_LOCAL_MACHINE'
BS = chr(92)
NL = BS + BS

# lang index -> (vendor root, vendor sub) ; see f_10018ee0's table at 0x10350fec
VENDOR = {
    0: ('SOFTWARE' + BS + 'PlayOnline', 'SQUARE'),
    1: ('SOFTWARE' + BS + 'PlayOnlineUS', 'SquareEnix'),
    2: ('SOFTWARE' + BS + 'PlayOnlineEU', 'SquareEnix'),
}
ALIASES = {'jp': 0, 'ja': 0, 'us': 1, 'en': 1, 'eu': 2, 'fr': 2}

# values boot64.c / the client itself must see, else the boot test cannot get to guest code
REQUIRED_FFXI = ['0000', '0001', '0002', '0034', '0042']
REQUIRED_INTERFACE = ['0001', '0002', '1000']
STRING_VALUES = {'0042'}          # REG_SZ, not a dword


def parse_lang(s):
    s = str(s).strip().lower()
    if s in ALIASES:
        return ALIASES[s]
    try:
        n = int(s)
    except ValueError:
        raise SystemExit('error: bad --lang %r (use jp/us/eu or 0/1/2)' % s)
    if n not in VENDOR:
        raise SystemExit('error: --lang %d out of range 0..2' % n)
    return n


def reg_query(key):
    """Read-only HKLM query -> {name: (type, text)} ('' name == default value)."""
    try:
        out = subprocess.run(['reg', 'query', key], capture_output=True, text=True, timeout=30)
    except Exception:
        return {}
    if out.returncode != 0:
        return {}
    vals = {}
    for line in out.stdout.splitlines():
        m = re.match(r'^\s{4}(\S+|\(Default\))\s+(REG_\w+)\s+(.*)$', line)
        if not m:
            continue
        name = '' if m.group(1) == '(Default)' else m.group(1)
        vals[name] = (m.group(2), m.group(3).strip())
    return vals


def parse_ini(ini_path):
    """[ffxi.registry] from the launcher ini, case-preserving."""
    if not ini_path or not os.path.exists(ini_path):
        return None
    cp = configparser.ConfigParser(strict=False, interpolation=None)
    cp.optionxform = str
    cp.read(ini_path, encoding='utf-8-sig')
    for sec in cp.sections():
        if sec.lower() == 'ffxi.registry':
            return {k.strip(): v.strip() for k, v in cp.items(sec)}
    return None


def parse_fixture(path):
    """Very small .reg reader: {key: {name: (type, text)}} with type in DWORD/SZ/BINARY."""
    out = {}
    if not path or not os.path.exists(path):
        return out
    cur = None
    for raw in open(path, encoding='utf-8-sig', errors='replace'):
        line = raw.strip()
        if line.startswith('[') and line.endswith(']'):
            cur = line[1:-1]
            out[cur] = {}
        elif cur and line.startswith('"') and '=' in line:
            name, rest = line[1:].split('"', 1)
            rest = rest[1:] if rest.startswith('=') else rest
            if rest.startswith('dword:'):
                out[cur][name] = ('REG_DWORD', rest[6:])
            elif rest.startswith('hex(') or rest.startswith('hex:'):
                out[cur][name] = ('REG_BINARY', re.sub(r'^hex(\([^)]*\))?:', '', rest))
            else:
                out[cur][name] = ('REG_SZ', rest.strip('"').replace(NL, BS))
    return out


def q(s):
    return '"%s"' % str(s).replace('"', '\\"').replace(BS, NL)


def as_hex(txt):
    """DWORD text may arrive as '0x...' (reg query) or bare hex (fixture/ini). int(x, 0)
    rejects leading zeros, so parse as hex explicitly."""
    t = str(txt).strip()
    if t.lower().startswith('0x'):
        t = t[2:]
    return int(t, 16)


def main():
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    ap = argparse.ArgumentParser(description='build the .reg for tests/boot64.c')
    ap.add_argument('--game', required=True, help='the FINAL FANTASY XI folder (-> 0042, InstallFolder)')
    ap.add_argument('--lang', default='us', help='jp|us|eu or 0|1|2 (default us: Phoenix runs English)')
    ap.add_argument('--ini', default=r'C:\PhoenixXI\config\boot\phoenix-launch.ini')
    ap.add_argument('--fixture', default=os.path.join(root, 'ffxi.reg'))
    ap.add_argument('--out', default=os.path.join(root, 'build', 'ffxi.reg'))
    args = ap.parse_args()

    game = os.path.abspath(args.game)
    if not os.path.isdir(game):
        raise SystemExit('error: --game %r is not a folder' % game)
    lang = parse_lang(args.lang)
    root_name, sub = VENDOR[lang]
    # A 32-bit guest sees SOFTWARE\PlayOnlineUS\..., but the host-side path that reg.exe
    # exposes -- and that the upstream fixture uses -- carries the WOW6432Node redirection
    # prefix. reg.c folds it back away when it parses, so emit and query the host form.
    host_root = root_name.replace('SOFTWARE' + BS, 'SOFTWARE' + BS + 'WOW6432Node' + BS, 1)
    base_key = '%s\\%s' % (HKLM, host_root)
    ffxi_key = '%s\\%s\\FinalFantasyXI' % (base_key, sub)
    inst_key = '%s\\InstallFolder' % base_key
    iface_key = '%s\\Interface' % base_key

    ini = parse_ini(args.ini)
    fix = parse_fixture(args.fixture)
    if ini is None:
        print('note: launcher ini %r has no [ffxi.registry]; using HKLM + fixture' % args.ini)

    fallbacks = []
    sections = {}

    # ---- FinalFantasyXI: ini first, then the live HKLM key, then the fixture ----
    ff = {}
    live = reg_query(ffxi_key)
    fx_ff = {}
    for k, v in fix.items():
        if k.lower() == ffxi_key.lower():
            fx_ff = v
    names = list(ini.keys()) if ini else []
    names += [n for n in live if n not in names]
    names += [n for n in fx_ff if n not in names]
    for n in names:
        if ini and n in ini:
            txt = ini[n]
            if n in STRING_VALUES or n.startswith('pad'):
                ff[n] = ('REG_SZ', txt)
            elif n == 'bFirst':
                ff[n] = ('REG_BINARY', '00')
            else:
                try:
                    ff[n] = ('REG_DWORD', '%08x' % (int(txt, 0) & 0xFFFFFFFF))
                except ValueError:
                    ff[n] = ('REG_SZ', txt)
            continue
        if n in live:
            t, txt = live[n]
            ff[n] = ({'REG_DWORD': 'REG_DWORD'}.get(t, t), txt)
            fallbacks.append('%s\\%s (from live HKLM)' % (ffxi_key, n))
            continue
        if n in fx_ff:
            ff[n] = fx_ff[n]
            fallbacks.append('%s\\%s (from upstream fixture)' % (ffxi_key, n))
    # the install path always wins
    ff['0042'] = ('REG_SZ', game)
    sections[ffxi_key] = ff

    # ---- InstallFolder / Interface / root value: the real install ----
    live_inst = reg_query(inst_key)
    live_iface = reg_query(iface_key)
    fx_inst, fx_iface, fx_root = {}, {}, {}
    for k, v in fix.items():
        kl = k.lower()
        if kl == inst_key.lower():
            fx_inst = v
        elif kl == iface_key.lower():
            fx_iface = v
        elif kl == base_key.lower():
            fx_root = v

    inst = {}
    for n in list(live_inst) + [x for x in fx_inst if x not in live_inst]:
        inst[n] = live_inst[n] if n in live_inst else fx_inst[n]
        if n not in live_inst:
            fallbacks.append('%s\\%s (from upstream fixture)' % (inst_key, n))
    inst['0001'] = ('REG_SZ', game)                      # the game folder we were given
    viewer = os.path.join(os.path.dirname(game), 'PlayOnlineViewer')
    inst['1000'] = ('REG_SZ', viewer if os.path.isdir(viewer) else game)
    sections[inst_key] = inst

    iface = {}
    for n in list(live_iface) + [x for x in fx_iface if x not in live_iface]:
        iface[n] = live_iface[n] if n in live_iface else fx_iface[n]
        if n not in live_iface:
            fallbacks.append('%s\\%s (from upstream fixture)' % (iface_key, n))
    sections[iface_key] = iface

    root_vals = dict(fx_root)
    live_root = reg_query(base_key)
    for n, t_v in live_root.items():
        if n:
            root_vals[n] = t_v
    if root_vals:
        sections[base_key] = root_vals

    # ---- fail loudly if the test cannot possibly pass ----
    missing = [n for n in REQUIRED_FFXI if n not in ff]
    if missing:
        raise SystemExit('error: no source for %s in %s (ini=%s, live=%s, fixture=%s)'
                         % (missing, ffxi_key, bool(ini), bool(live), bool(fx_ff)))
    miss_i = [n for n in REQUIRED_INTERFACE if n not in iface]
    if miss_i:
        print('warning: %s missing %s -- the patch.ver interface-id check may reject the boot'
              % (iface_key, miss_i))

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    # reg.c only narrows UTF-16 when the FF FE BOM is present (runtime/portable/reg.c:143),
    # so 'utf-16' (BOM + LE) it is -- plain utf-16-le would be parsed as ANSI garbage.
    with open(args.out, 'w', encoding='utf-16', newline='') as fh:
        fh.write('Windows Registry Editor Version 5.00\r\n\r\n')
        for key in sorted(sections):
            fh.write('[%s]\r\n' % key)
            for name in sorted(sections[key]):
                t, txt = sections[key][name]
                if t == 'REG_DWORD':
                    try:
                        fh.write('%s=dword:%08x\r\n' % (q(name), as_hex(txt) & 0xFFFFFFFF))
                    except ValueError:
                        fh.write('%s=%s\r\n' % (q(name), q(txt)))
                elif t == 'REG_BINARY':
                    fh.write('%s=hex:%s\r\n' % (q(name), txt.replace(' ', '')))
                else:
                    fh.write('%s=%s\r\n' % (q(name), q(txt)))
            fh.write('\r\n')

    print('wrote %s' % args.out)
    print('client region : lang=%d -> %s\\%s (Phoenix runs English per [ashita.language] playonline=2)'
          % (lang, root_name, sub))
    print('FinalFantasyXI: %d values (%s)' % (len(ff), 'from launcher ini' if ini else 'from HKLM/fixture'))
    print('InstallFolder : %d values, 0001 -> %s' % (len(inst), game))
    print('Interface     : %d values %s' % (len(iface), sorted(iface)))
    if fallbacks:
        print('fell back for %d value(s):' % len(fallbacks))
        for f in fallbacks:
            print('  - %s' % f)
    else:
        print('fell back for 0 values (every value came from the real install)')


if __name__ == '__main__':
    main()
