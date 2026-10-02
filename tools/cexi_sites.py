"""Find what host/cexi.c changes for a CatsEyeXI-style server in this build, and record it in meta/builds.json.

  python3 tools/cexi_sites.py [--dry-run] [--slots <cexidats checkout>]

A server whose items run past the retail ids (custom ids 0x7800-0xDFFF, read from an extended
Monstrosity item DAT, ROM/288/80.DAT) needs four things from the client, all found here by their
shape in generated/FFXiMain.unpacked.dll, so a new client build only needs this run again:

  cexi_item_ranges    (addresses) the item id range table in .data: 20-byte entries {file (the game
                      fills it in at start), first id, end id, 0, handler}, ended by first id 0xFFFFFFFF;
                      the game takes the first entry an id falls in (0x10159eb0 in 2026-09-03).
                      host/cexi.c widens the Monstrosity entry (0x7400-0x7800) to 0x7400-0xE000 and
                      points it at the general items' handler.
  cexi_gear_groups    (addresses) the gear model table in .data: 8 races x 9 slots x 6 groups of
                      {base file id, count}. host/cexi.c points the last group of head..sub at the
                      expanded gear file ids (128240 + (race * 9 + slot) * 4096 + model id).
  cexi_item_general   (wraps) the general items' handler: host/cexi.c sends a custom id to the
                      handler of its kind (usable, armour, weapon, furnishing) by its sub-range.
  cexi_items          (patches) two code constants, the translated variants chosen while host/cexi.c
                      has the bytes in place: the inventory's id gate (cmp eax,0x7400 -> 0xE000) and
                      the auction house list's cursor (movsx -> movzx of the last item id, so ids of
                      0x8000 and up keep their place).

Each is found once and checked against what the retail client holds; anything else stops the run
and nothing is written.

--slots: the spell and job-ability ceilings too (0x400 spells and 0xB00 commands, raised to 0x1000),
from the site list of a local checkout of github.com/CatsAndBoats/cexidats (src/cexislots/sites.h: a
byte pattern per site, the constant's place in it, its retail and new value). The checkout is read
where it is and nothing of it is copied here (it is not open source); what is written is this
build's addresses and values:

  cexi_slots          (patches) every constant the ceilings move (loop bounds, sizes, the known-spell
                      and known-command lists' layout), except those its site list keeps retail
                      (the record decode loops and the reset callback's sizes)
  cexi_slots          (its own section) where the rest are, for host/cexi.c to take over: the
                      recast array's references, the known-bits getters and fill, the /ja search
                      and the effect file-id stubs, and the globals it reads

Every site must match its expected count in .text and hold its retail value.
"""
import argparse
import json
import os
import re
import struct
import sys

import pefile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import buildinfo  # noqa: E402

IMAGE = os.path.join(buildinfo.ROOT, 'generated', 'FFXiMain.unpacked.dll')

# gear groups: G0-G4 counts of head..feet add to 608, of main and sub to 1196 (the rest is G5)
GEAR_STARTS = (608, 608, 608, 608, 608, 1196, 1196)


def die(msg):
    raise SystemExit('cexi_sites: ' + msg)


def one(hits, what):
    if len(hits) != 1:
        die('%s: %d matches, want 1' % (what, len(hits)))
    return hits[0]


# the sites cexislots finds but keeps retail (src/cexislots/cexislots.cpp): it decodes the records past
# retail itself, and the reset callback works on the retail-size list object
SLOTS_KEPT = re.compile(r'\.(decode_loop_end|reset_\w+)$')


def slot_sites(checkout, img, base, text):
    path = os.path.join(checkout, 'src', 'cexislots', 'sites.h')
    try:
        with open(path) as f:
            src = f.read()
    except OSError as e:
        die('--slots: %s' % e)
    num = r'(0x[0-9a-fA-F]+|\d+)[uU]?'
    sites = re.findall(r'\{\s*"([^"]+)",\s*"[^"]*",\s*SiteKind::(\w+),\s*"([^"]+)",\s*' + r',\s*'.join([num] * 5), src)
    if not sites:
        die('--slots: no sites in %s' % path)
    patches, rest = {}, {}
    for name, kind, pattern, off, size, old, new, hits_wanted in sites:
        off, size, old, new, hits_wanted = (int(v, 0) for v in (off, size, old, new, hits_wanted))
        rx = b''.join(b'.' if t == '??' else re.escape(bytes([int(t, 16)])) for t in pattern.split())
        lo, hi = text
        hits = [lo + m.start() for m in re.finditer(rx, img[lo:hi], re.S)]
        if len(hits) != hits_wanted:
            die('--slots: %s: %d matches in .text, want %d' % (name, len(hits), hits_wanted))
        for n, h in enumerate(hits):
            at = h + off
            key = name if len(hits) == 1 else '%s.%d' % (name, n)
            if kind in ('Imm', 'Disp'):
                held = int.from_bytes(img[at:at + size], 'little')
                if held != old:
                    die('--slots: %s at %#x holds %#x, want %#x' % (key, base + at, held, old))
                if not SLOTS_KEPT.search(name):
                    patches['0x%08x' % (base + at)] = new.to_bytes(size, 'little').hex()
            else:
                # detour: the function or code at the pattern; global and array: the operand's address
                rest[key] = '0x%08x' % (base + (h if kind == 'Detour' else at))
    print('cexi_sites: --slots: %d sites, %d constants patched, %d for the host' % (len(sites), len(patches), len(rest)),
          file=sys.stderr)
    return patches, rest


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--dry-run', action='store_true', help='print what was found, write nothing')
    ap.add_argument('--slots', metavar='CHECKOUT', help='also the spell/ability ceilings, from a cexidats checkout')
    args = ap.parse_args()

    build = buildinfo.current()
    pe = pefile.PE(IMAGE)
    base = pe.OPTIONAL_HEADER.ImageBase
    img = pe.get_memory_mapped_image()
    sec = {s.Name.rstrip(b'\0').decode(): (s.VirtualAddress, s.VirtualAddress + s.Misc_VirtualSize) for s in pe.sections}
    text, data = sec['.text'], sec['.data']

    def find(rx, lo_hi):
        lo, hi = lo_hi
        return [lo + m.start() for m in re.finditer(rx, img[lo:hi], re.S)]

    # the item range table: {file, 0, 0x1000, 0, h} then {file, 0x2200, 0x2800, 0, h} then {file, 0x1000, 0x2000, ...}
    rx = b'.{4}' + re.escape(struct.pack('<3I', 0, 0x1000, 0)) + b'.{8}' + re.escape(struct.pack('<3I', 0x2200, 0x2800, 0)) + \
        b'.{8}' + re.escape(struct.pack('<2I', 0x1000, 0x2000))
    ranges = one([o for o in find(rx, data) if o % 4 == 0], 'item range table')
    entries = []
    for i in range(64):
        e = struct.unpack_from('<5I', img, ranges + 20 * i)
        if e[1] == 0xFFFFFFFF:
            break
        entries.append(e)
    else:
        die('item range table: no end')
    by_first = {e[1]: e for e in entries}
    for first, end in ((0, 0x1000), (0x1000, 0x2000), (0x2800, 0x4000), (0x4000, 0x5A00), (0x7000, 0x7400), (0x7400, 0x7800)):
        if first not in by_first or by_first[first][2] != end:
            die('item range table: no %#x-%#x entry' % (first, end))
    general = by_first[0][4]
    if not text[0] <= general - base < text[1]:
        die('item range table: general handler %#x is not code' % general)
    # the general handler: mov eax,[esp+4]; push esi; ...; lea ecx,[eax+0xb0] (the record is at +0xb0)
    if img[general - base:general - base + 5] != bytes.fromhex('8b44240456') or \
            bytes.fromhex('8d88b0000000') not in img[general - base:general - base + 0x10]:
        die('general item handler at %#x: not the expected prologue' % general)

    # the gear groups: HumeMale head G0 (7112, 256) and G1 (63323, 48), 48 bytes (face) into the table
    sig = struct.pack('<4I', 7112, 256, 63323, 48)
    gear = one([o - 48 for o in find(re.escape(sig), data) if o % 4 == 0], 'gear group table')
    for race in range(8):
        for slot, start in zip(range(1, 8), GEAR_STARTS):
            at = gear + ((race * 9 + slot) * 6) * 8
            counts = [struct.unpack_from('<I', img, at + 8 * g + 4)[0] for g in range(5)]
            if sum(counts) != start:
                die('gear group table: race %d slot %d G0-G4 add to %d, want %d' % (race, slot, sum(counts), start))

    # the inventory's gate: cmp eax,0x7400 then (5-16 bytes on) cmp eax,0xffff
    gate = one(find(re.escape(bytes.fromhex('3d00740000')) + b'.{0,11}' + re.escape(bytes.fromhex('3dffff0000')), text),
               'inventory id gate')
    # the auction house list: movsx edx, word [esi+last item]; push edi; xor edi,edi; mov di,[eax+4]; cmp edi,edx; pop edi; jne
    ah = one(find(re.escape(bytes.fromhex('0fbf96')) + b'..' + re.escape(bytes.fromhex('00005733ff668b78043bfa5f75')),
                  text), 'auction house cursor')

    out = {
        'addresses': {'cexi_item_ranges': '0x%08x' % (base + ranges), 'cexi_gear_groups': '0x%08x' % (base + gear)},
        'wraps': {'cexi_item_general': '0x%08x' % general},
        'patches': {'cexi_items': {'0x%08x' % (base + gate): '3d00e00000', '0x%08x' % (base + ah): '0fb7'}},
    }
    if args.slots:
        patches, rest = slot_sites(args.slots, img, base, text)
        out['patches']['cexi_slots'] = patches
        out['cexi_slots'] = rest
    print(json.dumps(out, indent=2))
    if args.dry_run:
        return
    with open(buildinfo.BUILDS) as f:
        meta = json.load(f)
    b = meta['builds'][build['build']]
    for section, values in out.items():
        b.setdefault(section, {}).update(values)
    with open(buildinfo.BUILDS, 'w') as f:
        json.dump(meta, f, indent=2)
        f.write('\n')
    print('cexi_sites: wrote build %s in %s' % (build['build'], os.path.relpath(buildinfo.BUILDS, buildinfo.ROOT)))


if __name__ == '__main__':
    main()
