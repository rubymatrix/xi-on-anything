#!/usr/bin/env python3
"""Write a DAT overlay that shows the lobby's gamepad prompts in place of its keyboard ones.

The PC lobby's sprite sheet (`lobbywin`) still carries the Xbox 360 client's sprites: each prompt
that names a key ("Press the +Key to generate a random name.", "+ : Settings Management") has a
twin drawn with the `abxy360` button art. The PC code only draws the keyboard ones. This copies
each gamepad sprite over its keyboard twin (the twin is the sprite whose first parts are the
same), so the game draws the button without knowing. host64 reads the result as a DAT overlay
(--pad-dats, while a gamepad is connected); client/launch.sh in WorldSim runs this.

    python3 tools/controller_dats.py --game <FINAL FANTASY XI folder> --out <overlay folder>

The overlay is made from the install's files and stays on this machine (it is the game's data).
Standard library only.
"""
import argparse
import os
import struct
import sys

# the menu files that hold the PC lobby sheet (the English one and its copies)
CANDIDATES = ['ROM/91/16.DAT', 'ROM/119/50.DAT', 'ROM/176/74.DAT', 'ROM/178/13.DAT']
SHEET = 0x31  # RES_TYPE of a sprite sheet
PAD_ART = b'menu    abxy360 '


def chunks(b):
    """(offset, size, type) of each top-level resource chunk"""
    o, out = 0, []
    while o + 16 <= len(b):
        info = struct.unpack_from('<I', b, o + 4)[0]
        size = ((info >> 7) & 0x7FFFF) * 16
        if not size:
            break
        out.append((o, size, info & 0x7F))
        o += size
    return out


def sprites(d):
    """the payload of a sheet: (prefix bytes, [sprite byte spans]); a sprite is a part count and
    61-byte parts (corners, source rect, colours, flags, the image's name)"""
    o = 17 + 16 * d[16]
    count = struct.unpack_from('<H', d, o)[0]
    o += 2
    prefix, spans = d[:o], []
    for _ in range(count):
        n = 1 + 61 * d[o]
        spans.append(d[o:o + n])
        o += n
    return prefix, spans, d[o:]


def swap_sheet(d):
    prefix, spans, rest = sprites(d)
    swapped = []
    for i, pad in enumerate(spans):
        if PAD_ART not in pad:
            continue
        head = pad[1:1 + 61 * 8]
        for j, key in enumerate(spans):
            if j != i and PAD_ART not in key and key[1:1 + 61 * 8] == head:
                spans[j] = pad
                swapped.append((j, i))
    return prefix + b''.join(spans) + rest, swapped


def patch(b):
    out, swapped = bytearray(), []
    for o, size, kind in chunks(b):
        chunk = b[o:o + size]
        if kind == SHEET and chunk[24:32] == b'lobbywin':
            body, s = swap_sheet(chunk[16:])
            swapped += s
            size = 16 + (len(body) + 15) // 16 * 16
            info = struct.unpack_from('<I', chunk, 4)[0]
            info = (info & ~(0x7FFFF << 7)) | (size // 16) << 7
            chunk = chunk[:4] + struct.pack('<I', info) + chunk[8:16] + body.ljust(size - 16, b'\0')
        out += chunk
    return bytes(out), swapped


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--game', required=True, help='the FINAL FANTASY XI folder')
    ap.add_argument('--out', required=True, help='the overlay folder to write')
    a = ap.parse_args()
    for rel in CANDIDATES:
        src, dst = os.path.join(a.game, rel), os.path.join(a.out, rel)
        if not os.path.isfile(src):
            continue
        if os.path.isfile(dst) and os.path.getmtime(dst) >= os.path.getmtime(src):
            continue  # up to date
        with open(src, 'rb') as f:
            b = f.read()
        patched, swapped = patch(b)
        if not swapped:
            continue
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, 'wb') as f:
            f.write(patched)
        print('%s: lobby sprites %s now show the gamepad button' % (rel, ', '.join('%d<-%d' % s for s in swapped)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
