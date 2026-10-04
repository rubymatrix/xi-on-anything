"""Scenes for tools/replayserver.py: zone visits recorded by the xireplay addon (tools/replay/xireplay).

A recording is JSON lines, one per packet or marker:

  {"meta": {"zone": 106, "scene": "crowd", "captured": "..."}}
  {"t": <ms>, "dir": "in"|"out", "id": <packet type>, "hex": "<the packet, header included>"}
  {"t": <ms>, "mark": "start"}

A scene is the server's packets ("in") from the first zone-in (0x00A) up to, not including, the
zone-out (0x00B), each with its time from the zone-in, and the markers between. Packets are the zone
protocol's messages as the client got them: 9-bit type and size (in 4-byte units) in the first two
bytes, the sync number in the next two. Offsets here count from the start of that header, so they
are 4 more than tools/staticserver.py's, which count from the body.
"""
import json
import struct

ZONE_IN = 0x00A
ZONE_OUT = 0x00B

# Packets that only put text in the chat log: server chat and system lines (0x017, GM command replies
# among them), standard and battle messages (0x009, 0x029, 0x02A), experience (0x02D), NPC text
# (0x036), the server message (0x04D, the login welcome), system messages (0x053) and the treasure
# pool (0x0D2, 0x0D3: "You find ...").
TEXT = {0x009, 0x017, 0x029, 0x02A, 0x02D, 0x036, 0x04D, 0x053, 0x0D2, 0x0D3}


def ptype(data):
    return struct.unpack_from('<H', data, 0)[0] & 0x1FF


class Scene:
    def __init__(self, path):
        self.path = path
        self.label = ''       # how a suite and !replay name it
        self.group = ''       # the group !replay can ask for it by
        self.home = False     # where a session waits for !replay
        self.character = 0    # the character id the zone-in was for
        self.packets = []     # [seconds from the zone-in, bytearray]
        self.marks = []       # [seconds from the zone-in, label]
        self.meta = {}

    @classmethod
    def load(cls, path):
        s = cls(path)
        start = None
        with open(path, encoding='utf-8') as f:
            for n, line in enumerate(f, 1):
                try:
                    rec = json.loads(line)
                except ValueError as e:
                    raise ValueError(f'{path}:{n}: {e}') from None
                if 'meta' in rec:
                    s.meta = rec['meta']
                    continue
                t = rec.get('t')
                if t is None:
                    continue
                if 'mark' in rec:
                    if start is not None:
                        s.marks.append([(t - start) / 1000, rec['mark']])
                    continue
                if rec.get('dir') != 'in':
                    continue
                data = bytearray.fromhex(rec['hex'])
                if len(data) < 4:
                    raise ValueError(f'{path}:{n}: a packet of {len(data)} bytes')
                if start is None:
                    if ptype(data) != ZONE_IN:
                        continue  # before the first zone-in
                    start = t
                    if len(data) >= 8:
                        s.character = struct.unpack_from('<I', data, 4)[0]
                elif ptype(data) == ZONE_OUT:
                    break  # zoning out: the end of this scene
                s.packets.append([(t - start) / 1000, data])
        if start is None:
            raise ValueError(f'{path}: no zone-in (0x00A)')
        return s

    @property
    def zone(self):
        return int(self.meta.get('zone') or 0)

    def duration(self):
        """When the scene ends: its last packet or marker."""
        d = self.packets[-1][0] if self.packets else 0.0
        return max([d] + [at for at, _ in self.marks])

    def sort(self):
        self.packets.sort(key=lambda p: p[0])

    def name(self):
        """The recorded character's name, from the zone-in's name field (+0x84)."""
        if not self.packets or len(self.packets[0][1]) < 0x84 + 16:
            return ''
        return bytes(self.packets[0][1][0x84:0x94]).split(b'\0')[0].decode('ascii', 'replace')

    def rename(self, name):
        """The lobby's name in place of the recorded one wherever it is a whole field (the name, then a
        zero byte, after a zero or a non-text byte); a longer name is cut to the recorded one's length."""
        old = self.name()
        if not old or old == name:
            return 0
        want = old.encode('ascii') + b'\0'
        repl = name.encode('ascii', 'replace')[:len(want) - 1].ljust(len(want), b'\0')
        n = 0
        for _, d in self.packets:
            i = 0
            while True:
                at = d.find(want, i)
                if at < 0:
                    break
                if at == 0 or d[at - 1] == 0 or not 0x20 <= d[at - 1] <= 0x7E:
                    d[at:at + len(repl)] = repl
                    n += 1
                i = at + len(want)
        return n

    def quiet(self):
        """The recorded chat and other text (TEXT) dropped."""
        before = len(self.packets)
        self.packets = [p for p in self.packets if ptype(p[1]) not in TEXT]
        return before - len(self.packets)
