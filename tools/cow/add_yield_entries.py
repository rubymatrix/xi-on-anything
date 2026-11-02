"""Register coroutine resume points as translation entries.

FFXiMain runs a small family of yield/suspend helpers in the 0x103161xx-0x103163xx range. Each one
saves the guest context into a caller-supplied block and, crucially, stashes the RETURN ADDRESS
into it:

    mov eax, dword ptr [esp]
    mov dword ptr [edx + 0x14], eax
    mov dword ptr [edx + 0x20], 0x56433230      ; '02CV' tag

The scheduler later RESUMES by jumping to that saved address - i.e. straight into the middle of a
translated function, at the instruction after the call. Discovery registers continuations it sees
stored as `mov [mem], imm32`, but not these implicit ones, so the resume lands on an address with
no translation and the stand-in dies:

    [recomp] FATAL at 0487bbe4: indirect call/jump to an address with no translation
    [recomp] FATAL at 102dbb59: jump to an address with no translation

Two helpers are known by address (0x10316224 and 0x103161a8); rather than list them, find them:
scan .text for functions that contain the "stash the return address" idiom, then register the
return address of every `call` to each of them as its own entry, splitting the containing range
the way the existing continuations (0x102EF059, 0x102DBB5B, ...) are stored.

  python tools/cow/add_yield_entries.py             # show what would change
  python tools/cow/add_yield_entries.py --write     # update this build's meta file
  python tools/cow/add_yield_entries.py --helpers   # just list discovered helpers
"""
import argparse
import json
import os
import re
import sys

import pefile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import buildinfo  # noqa: E402

# These helpers stamp a context block with the tag '02CV':  mov dword ptr [reg+0x20], 0x56433230
# (little-endian bytes in .text). The tag is specific to them, so it is a far better signature
# than matching the "mov eax,[esp] / mov [edx+0x14],eax" idiom, which fires on the instruction
# inside the body rather than the function entry the calls actually target.
CONTEXT_TAG = bytes.fromhex('30324356')


def text_image(image_path):
    pe = pefile.PE(image_path, fast_load=True)
    text = next(s for s in pe.sections if s.Name.rstrip(b'\0') == b'.text')
    base = pe.OPTIONAL_HEADER.ImageBase
    img = pe.get_memory_mapped_image()
    return base, text.VirtualAddress, text.Misc_VirtualSize, img[text.VirtualAddress:text.VirtualAddress + text.Misc_VirtualSize]


def find_yield_helpers(base, lo, size, data, meta):
    """Entries of the functions whose body stamps a '02CV' context block.

    The tag is found mid-body, so each hit is mapped back to the entry of the function that
    contains it - that entry is what `call rel32` targets, and therefore what we need.
    """
    spans = sorted((r[0], r[1], f['entry']) for f in meta['functions'] for r in f['ranges'])
    helpers = set()
    i = 0
    while True:
        p = data.find(CONTEXT_TAG, i)
        if p < 0:
            break
        at = base + lo + p
        owner = None
        for s_lo, s_hi, entry in spans:
            if s_lo <= at < s_hi:
                owner = entry
                break
        if owner is not None:
            helpers.add(owner)
        i = p + 1
    return sorted(helpers)


def calls_to(base, lo, size, data, targets):
    """Return addresses (the instruction after each `call rel32` that lands on a target)."""
    targets = set(targets)
    rets = set()
    for i in range(len(data) - 5):
        if data[i] != 0xE8:
            continue
        here = base + lo + i
        rel = int.from_bytes(data[i + 1:i + 5], 'little', signed=True)
        if here + 5 + rel in targets:
            rets.add(here + 5)
    return sorted(rets)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--write', action='store_true')
    ap.add_argument('--helpers', action='store_true')
    a = ap.parse_args()

    build = buildinfo.current()['build']
    meta_path = os.path.join(ROOT, 'meta', 'FFXiMain.%s.meta.json' % build)
    image = os.path.join(ROOT, 'generated', 'FFXiMain.unpacked.dll')
    meta = json.load(open(meta_path))
    base, lo, size, data = text_image(image)

    helpers = find_yield_helpers(base, lo, size, data, meta)
    print('build %s: %d functions stash a return address (yield/suspend helpers)' % (build, len(helpers)))
    for h in helpers[:12]:
        print('   helper 0x%08x' % h)
    if len(helpers) > 12:
        print('   ... %d more' % (len(helpers) - 12))
    if a.helpers:
        return

    rets = calls_to(base, lo, size, data, helpers)
    entries = {f['entry'] for f in meta['functions']}
    missing = [r for r in rets if r not in entries]
    print('call sites to them: %d, of which %d have no entry' % (len(rets), len(missing)))

    if not missing:
        print('nothing to add')
        return

    # Ranges do not always tile .text: discovery can leave a tail such as `pop esi; ret` between
    # two entries (0x102DBB59 sits in exactly such a gap). A resume can land there, so bridge to
    # the next known boundary rather than skipping it.
    bounds = sorted({f['entry'] for f in meta['functions']} |
                    {r[1] for f in meta['functions'] for r in f['ranges']})

    added = 0
    for addr in missing:
        owner = None
        for f in meta['functions']:
            for r in f['ranges']:
                if r[0] <= addr < r[1]:
                    owner = (f, r[0], r[1])
                    break
            if owner:
                break
        if owner is None:
            nxt = next((b for b in bounds if b > addr), None)
            if nxt is None or nxt - addr > 0x400:
                print('   !! 0x%08x unreachable: no boundary within 0x400 - skipped' % addr)
                continue
            meta['functions'].append({'ranges': [[addr, nxt]], 'thunk': False, 'entry': addr})
            added += 1
            if added <= 15:
                print('   + 0x%08x (bridged across a gap to 0x%08x)' % (addr, nxt))
            continue
        f, lo2, hi2 = owner
        if addr == lo2:
            continue
        f['ranges'] = [[lo2, addr] if r == [lo2, hi2] else r for r in f['ranges']]
        meta['functions'].append({'ranges': [[addr, hi2]], 'thunk': False, 'entry': addr})
        added += 1
        if added <= 15:
            print('   + 0x%08x (from 0x%08x-0x%08x)' % (addr, addr, hi2))
    if added > 15:
        print('   ... %d more' % (added - 15))

    if not a.write:
        print('\ndry run: %d entries would be added. Pass --write.' % added)
        return

    meta['functions'].sort(key=lambda f: f['entry'])
    with open(meta_path, 'w', newline='') as fh:
        fh.write(json.dumps(meta, separators=(',', ':')))
    print('\nwrote %s: %d -> %d functions' % (os.path.relpath(meta_path, ROOT),
                                              len(entries), len(meta['functions'])))
    print('now: python tools/build.py host')


if __name__ == '__main__':
    main()
