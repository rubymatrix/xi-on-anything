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

--slots <checkout>: and what --cexi full needs, the spell and job-ability ceilings (0x400 spells,
0xB00 commands, raised to 0x1000), from the site list of a local checkout of
github.com/CatsAndBoats/cexidats (src/cexislots/sites.h: a byte pattern per site, the constant's place
in it, its retail and new value). The checkout is read where it is and nothing of it is copied here
(it is not open source); what is written is this build's addresses and values:

  cexi_slots          (patches) the constants the ceilings move (loop bounds, sizes, the known-spell
                      and known-command lists' layout, the record decode loops), the recast array's
                      references and the known-bits getters' operands (aimed at host/cexi.c's buffers
                      at the fixed cexi_region), and the /ja search's end. The reset callback and the
                      lists' allocation and constructors stay retail: the game builds and registers
                      its retail lists, and host/cexi.c swaps larger ones in as they are stored.
  cexi_*              (hooks) the list stores, the /ja search, the four effect file-id adds, the
                      weapon-skill bank's compare and join, and the file table loader's end
  cexi_cmd_fill       (wraps) the known-command list's fill
  cexi_*              (addresses) the getters, the player, the game's malloc and free, the retail
                      recast array, the region

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


# --slots: the host's buffers sit at a fixed guest address, so the code that names them can be
# patched like any constant (host/cexi.c reserves the range; keep the two in step):
#   +0x0000 the spell getter's "player": a word holding spell bits - its bits displacement
#   +0x0004 the same for commands
#   +0x0100 known-spell bits (0x200 bytes for 0x1000 ids), then two "data arrived" bytes set to 1
#   +0x0400 known-command bits, the same
#   +0x1000 spell recasts: 0x10002 words (the usability check reads ids it never bounds, up to 0xFFFF)
REGION = 0xFE000000
REGION_SPELL_PLAYER, REGION_CMD_PLAYER, REGION_SPELL_BITS, REGION_CMD_BITS, REGION_RECAST = 0x0, 0x4, 0x100, 0x400, 0x1000
BITS = 0x200  # 0x1000 ids

# cexislots' sites this port leaves retail:
#  - the reset callback's (reset_*): the game registered it at start with the retail list objects,
#    which stay its own (see host/cexi.c: the larger lists are swapped in after them)
#  - the list objects' allocation and constructors: the game builds and registers its retail-size
#    lists, and host/cexi.c swaps the larger ones in as each is stored
SLOTS_RETAIL = re.compile(r'^(spell\.reset_\w+|cmd\.reset_\w+|spell\.listobj_alloc|spell\.ctor_\w+|cmd\.listobj_alloc[12]|'
                          r'cmd\.ctor_count|cmd\.listobj_ids_lea|cmd\.listobj_count_store)$')


def slot_sites(checkout, img, base, text):
    """{patches, hooks, wraps, addresses} for --cexi full, from cexislots' site list."""
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
    lo, hi = text
    found = {}  # name -> (kind, [hit], offset, size, old, new)
    for name, kind, pattern, off, size, old, new, hits_wanted in sites:
        off, size, old, new, hits_wanted = (int(v, 0) for v in (off, size, old, new, hits_wanted))
        rx = b''.join(b'.' if t == '??' else re.escape(bytes([int(t, 16)])) for t in pattern.split())
        hits = [lo + m.start() for m in re.finditer(rx, img[lo:hi], re.S)]
        if len(hits) != hits_wanted:
            die('--slots: %s: %d matches in .text, want %d' % (name, len(hits), hits_wanted))
        if kind in ('Imm', 'Disp'):
            for h in hits:
                held = int.from_bytes(img[h + off:h + off + size], 'little')
                if held != old:
                    die('--slots: %s at %#x holds %#x, want %#x' % (name, base + h + off, held, old))
        found[name] = (kind, hits, off, size, old, new)

    u32 = lambda o: struct.unpack_from('<I', img, o)[0]
    va = lambda o: '0x%08x' % (base + o)
    patches, hooks, wraps, addrs = {}, {}, {}, {}

    def put(o, value, size=4, want=None):
        if want is not None and int.from_bytes(img[o:o + size], 'little') != want:
            die('--slots: %#x holds %#x, want %#x' % (base + o, int.from_bytes(img[o:o + size], 'little'), want))
        patches[va(o)] = value.to_bytes(size, 'little').hex()

    def at(name, extra=0):
        kind, hits, off, size, old, new = found[name]
        return hits[0] + (0 if kind == 'Detour' else off) + extra

    # the constants (the record decode loops too: the game decodes the larger tables at start, once
    # host/cexi.c has seen the menu DAT carries them)
    for name, (kind, hits, off, size, old, new) in found.items():
        if kind in ('Imm', 'Disp') and not SLOTS_RETAIL.match(name):
            for h in hits:
                put(h + off, new, size)
    # the recast array's references: into the host's, one past its end for the tick loop's end
    recast = u32(at('spell.recast_base'))
    for name, (kind, hits, off, size, old, new) in found.items():
        if kind == 'Array':
            delta = u32(hits[0] + off) - recast
            if delta == 0x401 * 2:
                delta = 0x1001 * 2
            elif delta >= 0x400 * 2:
                die('--slots: %s is %#x into the recast array' % (name, delta))
            put(hits[0] + off, REGION + REGION_RECAST + delta)
    # the known-bits getters: mov eax,[player]; ... mov cl,[eax+flag] ...; add eax,bits; ret. The
    # player becomes the region's word, so eax+bits lands on the host's bits; the flags on its 1s
    for name, player, length, bits_at, flags_at in (('spell.known_bitmap_getter', REGION_SPELL_PLAYER, 0x1D, 0x18, (0x0C,)),
                                                    ('cmd.known_bitmap_getter', REGION_CMD_PLAYER, 0x26, 0x1E, (0x0B, 0x15))):
        fn = at(name)
        if img[fn] != 0xA1 or img[fn + bits_at - 1] != 0x05 or any(img[fn + f - 2:fn + f] != b'\x8a\x88' for f in flags_at):
            die('--slots: %s at %#x is not the getter shape' % (name, base + fn))
        bits = u32(fn + bits_at)
        put(fn + 1, REGION + player)
        for i, f in enumerate(flags_at):
            put(fn + f, bits + BITS + i)
        addrs['cexi_%s_getter' % name.split('.')[0]] = va(fn)
    addrs['cexi_player'] = '0x%08x' % u32(at('spell.known_bitmap_getter') + 1)
    # the /ja name search: its second pass ends at the ceiling; a hook skips 0x600..0xAFF
    search = at('cmd.find_name_loop2_end')
    if img[search:search + 2] != b'\x81\xfd':
        die('--slots: cmd.find_name_loop2_end at %#x is not cmp ebp,imm32' % (base + search))
    put(search + 2, 0x1000 * 0x30, want=0x600 * 0x30)
    hooks['cexi_ja_search'] = va(search)
    # the effect file ids: hooks before each add
    for name, hook in (('fx.spell_add_edi', 'cexi_fx_spell_edi'), ('fx.spell_add_eax', 'cexi_fx_spell_eax'),
                       ('fx.ja_add_eax', 'cexi_fx_ja_eax'), ('fx.dispatch_add', 'cexi_fx_dispatch')):
        hooks[hook] = va(at(name))
    # the weapon-skill bank: before its cmp ecx,0x100, and where its paths meet again (lea eax,[esp+0x14])
    ws = at('fx.ws_bank_split')
    if img[ws + 0x9D:ws + 0xA1] != b'\x8d\x44\x24\x14':
        die('--slots: fx.ws_bank_split at %#x: no join at +0x9D' % (base + ws))
    hooks['cexi_ws_bank'] = va(ws)
    hooks['cexi_ws_join'] = va(ws + 0x9D)
    wraps['cexi_cmd_fill'] = va(at('cmd.known_bitmap_fill'))
    # the list objects: hooks on the stores of the new objects into their globals (mov [g],eax)
    for name, hook in (('spell.listobj_ptr', 'cexi_spell_list'), ('cmd.listobj_ptr', 'cexi_cmd_list')):
        o = at(name) - 1
        if img[o] != 0xA3:
            die('--slots: %s at %#x is not mov [global],eax' % (name, base + o))
        hooks[hook] = va(o)
    kind, hits, off, size, old, new = found['spell.listobj_alloc']  # push 0x884; call <the game's malloc>
    call = hits[0] + off + 4
    if img[call] != 0xE8:
        die('--slots: no call after the list allocation at %#x' % (base + call))
    addrs['cexi_malloc'] = va(call + 5 + struct.unpack_from('<i', img, call + 1)[0])
    addrs['cexi_spell_recast'] = '0x%08x' % recast
    addrs['cexi_region'] = '0x%08x' % REGION
    print('cexi_sites: --slots: %d sites -> %d bytes runs patched, %d hooks, %d wraps' % (len(found), len(patches), len(hooks), len(wraps)),
          file=sys.stderr)
    return patches, hooks, wraps, addrs


def file_tables(img, base, text):
    """The file table loader's end (the ROMn merge loop done): a hook that grows the tables."""
    lo, hi = text
    # lea edx,[edi+3]; cmp edx,0xe; jl <loop>; mov edx,[esp+0x14]; push edx; call <free>
    rx = re.escape(bytes.fromhex('8d570383fa0e0f8c')) + b'.{4}' + re.escape(bytes.fromhex('8b54241452e8'))
    h = one([lo + m.start() for m in re.finditer(rx, img[lo:hi], re.S)], 'file table loader')
    free = h + 17 + 5 + struct.unpack_from('<i', img, h + 18)[0]
    return {'cexi_file_tables': '0x%08x' % (base + h + 12)}, {'cexi_free': '0x%08x' % (base + free)}


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
        patches, hooks, wraps, addrs = slot_sites(args.slots, img, base, text)
        fhooks, faddrs = file_tables(img, base, text)
        out['patches']['cexi_slots'] = patches
        out['hooks'] = dict(hooks, **fhooks)
        out['wraps'].update(wraps)
        out['addresses'].update(addrs)
        out['addresses'].update(faddrs)
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
