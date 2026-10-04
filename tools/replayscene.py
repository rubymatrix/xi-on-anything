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
import math
import os
import struct

ZONE_IN = 0x00A
ZONE_OUT = 0x00B
CHARACTER = 0x00D
ENTITY = 0x00E
ACTION = 0x028
WEATHER = 0x057

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
        self.added = []       # the ids of the characters --players added

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


# --- packets -----------------------------------------------------------------------------------
#
# Entity updates (0x00E): +0x04 id, +0x08 index (ActIndex: 0..1023 a zone's NPCs and mobs, 1792..2303
# dynamic entities), +0x0A update mask (0x01 position, 0x02 claim, 0x04 HP and animation, 0x08 name,
# 0x10 model, 0x20 despawn), +0x0B direction, +0x0C x, +0x10 height, +0x14 z, +0x18 moving, +0x25 0x08
# for a live mob, +0x30 the model (kind 0 a creature, 1 a geared character: face, race and eight
# equipment models), +0x34 the name when the mask has it (LandSandBoat src/map/packets/entity_update.cpp).
#
# Actions (0x028) are a bit stream from +5, least significant bit first, as LandSandBoat packs them
# (src/map/packets/s2c/0x028_battle2.cpp): the actor's id (32 bits), the target count (6), a result
# count the client ignores (4), the category (4), its argument (32: a spell, an ability, ...), 32 bits
# more, then each target: its id (32), its result count (4), and each result: 3+2 bits, the animation
# (12), 5+2+3 bits, a value (17: the damage), a message id (10), 31 bits, then an added effect (1
# bit; if set 6+4+17 bits and a message id) and a reaction (1 bit; if set 6+4+14 bits and a message id).

ACTOR_BIT = 8 * 5
TARGETS_BIT = ACTOR_BIT + 32
CATEGORY_BIT = TARGETS_BIT + 6 + 4
ARG_BIT = CATEGORY_BIT + 4
FIRST_TARGET_BIT = ARG_BIT + 32 + 32
RESULT_ANIMATION = 3 + 2                         # a result's fields, from its start
RESULT_VALUE = RESULT_ANIMATION + 12 + 5 + 2 + 3
RESULT_MESSAGE = RESULT_VALUE + 17
RESULT_BITS = RESULT_MESSAGE + 10 + 31           # then the added effect and the reaction
ADDED_EFFECT_BITS = 6 + 4 + 17                   # each, when its flag is set, then a message id
REACTION_BITS = 6 + 4 + 14

CAST_START, CAST_FINISH = 8, 4                   # categories
CAST_BEGINS = 0x6B626163                         # a cast start's argument ('cabk')
BEGINS_CASTING = 327                             # "<caster> starts casting <spell> on <target>."


def get_bits(d, off, n):
    v = 0
    for i in range(n):
        if d[(off + i) >> 3] >> ((off + i) & 7) & 1:
            v |= 1 << i
    return v


def set_bits(d, off, n, v):
    for i in range(n):
        if v >> i & 1:
            d[(off + i) >> 3] |= 1 << ((off + i) & 7)
        else:
            d[(off + i) >> 3] &= ~(1 << ((off + i) & 7)) & 255


def is_entity(d):
    return ptype(d) == ENTITY and len(d) >= 0x1C


def is_action(d):
    """An action long enough to name its actor and first target."""
    return ptype(d) == ACTION and len(d) * 8 >= FIRST_TARGET_BIT + 32


def entity_id(d):
    return struct.unpack_from('<I', d, 4)[0]


def action_walk(d):
    """The bit offsets of an action's target ids and of its message ids, or None if it does not parse."""
    bits = len(d) * 8
    off = FIRST_TARGET_BIT
    targets, messages = [], []
    for _ in range(get_bits(d, TARGETS_BIT, 6)):
        if off + 36 > bits:
            return None
        targets.append(off)
        off += 32
        results = get_bits(d, off, 4)
        off += 4
        for _ in range(results):
            if off + RESULT_BITS + 1 > bits:
                return None
            messages.append(off + RESULT_MESSAGE)
            off += RESULT_BITS
            for extra in (ADDED_EFFECT_BITS, REACTION_BITS):
                if off + 1 > bits:
                    return None
                if get_bits(d, off, 1):
                    off += 1 + extra
                    if off + 10 > bits:
                        return None
                    messages.append(off)
                    off += 10
                else:
                    off += 1
    return targets, messages


def action_packet(actor, category, arg, targets):
    """An action (0x028) from actor: targets are (id, [(animation, value, message), ...]), each result a
    hit with no added effect or reaction."""
    bits = FIRST_TARGET_BIT + sum(36 + len(rs) * (RESULT_BITS + 2) for _, rs in targets)
    n = ((bits + 7) // 8 + 3) & ~3
    d = bytearray(n)
    struct.pack_into('<H', d, 0, ACTION | (n // 4) << 9)
    d[4] = (bits + 7) // 8  # the bit stream's length in bytes, as the server sets it
    set_bits(d, ACTOR_BIT, 32, actor)
    set_bits(d, TARGETS_BIT, 6, len(targets))
    set_bits(d, CATEGORY_BIT, 4, category)
    set_bits(d, ARG_BIT, 32, arg)
    off = FIRST_TARGET_BIT
    for tid, rs in targets:
        set_bits(d, off, 32, tid)
        set_bits(d, off + 32, 4, len(rs))
        off += 36
        for animation, value, message in rs:
            set_bits(d, off + RESULT_ANIMATION, 12, animation)
            set_bits(d, off + RESULT_VALUE, 17, value)
            set_bits(d, off + RESULT_MESSAGE, 10, message)
            off += RESULT_BITS + 2
    return d


def mobs(s):
    """The scene's mobs: an HP update of a live mob, or a creature's model; and, since a zone's mobs come
    before its NPCs in id order, anything below the last of those."""
    found = set()
    for _, d in s.packets:
        if is_entity(d) and len(d) >= 0x34:
            alive = d[0x0A] & 0x04 and d[0x25] & 0x08
            creature = d[0x0A] & 0x10 and struct.unpack_from('<H', d, 0x30)[0] == 0
            if alive or creature:
                found.add(entity_id(d))
    last = max((m & 0xFFF for m in found), default=0)
    for _, d in s.packets:
        if is_entity(d) and entity_id(d) >> 24 == 0x01 and entity_id(d) & 0xFFF <= last:
            found.add(entity_id(d))
    return found


def humanoids(s):
    """The scene's geared characters (NPCs with a model of kind 1), one update per distinct look."""
    out, seen = [], set()
    for _, d in s.packets:
        if is_entity(d) and len(d) >= 0x48 and d[0x0A] & 0x10 and struct.unpack_from('<H', d, 0x30)[0] == 1:
            look = bytes(d[0x32:0x44])
            if look not in seen:
                seen.add(look)
                out.append(bytes(d))
    return out


def used_indices(s):
    return {struct.unpack_from('<H', d, 8)[0] & 0x3FF for _, d in s.packets if is_entity(d)}


def mark(s, label):
    return next((at for at, m in s.marks if m == label), None)


# --- names -------------------------------------------------------------------------------------

WEATHERS = ['none', 'sunshine', 'clouds', 'fog', 'hot spell', 'heat wave', 'rain', 'squall', 'dust storm',
            'sand storm', 'wind', 'gales', 'snow', 'blizzards', 'thunder', 'thunderstorms', 'auroras',
            'stellar glare', 'gloom', 'darkness']

# Spells --mob-spells can use (LandSandBoat's spell ids, which are also their animations), with the
# message a hit carries: damage (2), drain (227), aspir (228), or an effect (236)
SPELLS = {name: (i, 2) for i, name in [
    (147, 'fire iv'), (152, 'blizzard iv'), (157, 'aero iv'), (162, 'stone iv'), (167, 'thunder iv'),
    (172, 'water iv'), (204, 'flare'), (206, 'freeze'), (208, 'tornado'), (210, 'quake'), (212, 'burst'),
    (214, 'flood'), (21, 'holy'), (30, 'banish iii'), (24, 'dia ii'), (231, 'bio ii'), (216, 'gravity'),
    (235, 'burn'), (236, 'frost'), (237, 'choke'), (238, 'rasp'), (239, 'shock'), (240, 'drown')]}
SPELLS.update({'drain': (245, 227), 'aspir': (247, 228)})
SPELLS.update({name: (i, 236) for i, name in [(221, 'poison ii'), (56, 'slow'), (58, 'paralyze'), (59, 'silence'),
                                               (253, 'sleep'), (254, 'blind'), (258, 'bind')]})

# Spells --echo-spells can use, cast on oneself: (LandSandBoat's spell id, its animation, the message a
# hit carries: HP recovered (7), or an effect gained (230))
SELF_SPELLS = {name: (i, i, 230) for i, name in [
    (46, 'protect iv'), (47, 'protect v'), (51, 'shell iv'), (52, 'shell v'), (57, 'haste'), (109, 'refresh'),
    (106, 'phalanx'), (54, 'stoneskin'), (53, 'blink'), (55, 'aquaveil'), (60, 'barfire'), (61, 'barblizzard'),
    (62, 'baraero'), (63, 'barstone'), (64, 'barthunder'), (65, 'barwater'), (100, 'enfire'), (101, 'enblizzard'),
    (102, 'enaero'), (103, 'enstone'), (104, 'enthunder'), (105, 'enwater'), (249, 'blaze spikes'),
    (250, 'ice spikes'), (251, 'shock spikes')]}
SELF_SPELLS.update({'cure iv': (4, 4, 7), 'cure v': (5, 5, 7), 'regen ii': (110, 139, 230), 'regen iii': (111, 140, 230),
                    'auspice': (96, 142, 230), 'reraise ii': (141, 493, 230), 'invisible': (136, 498, 230),
                    'sneak': (137, 499, 230), 'deodorize': (138, 500, 230)})


def weather_id(name):
    """A weather's id from its name ("hot-spell" or "hot spell") or id."""
    w = name.replace('-', ' ').lower()
    if w in WEATHERS:
        return WEATHERS.index(w)
    if w.isdigit() and int(w) < len(WEATHERS):
        return int(w)
    raise ValueError(f'no weather {name!r}')


def spell(name, table=None):
    """A spell's entry in table (SPELLS, the default: (id, message); or SELF_SPELLS) from its name
    ("fire-iv" or "fire iv")."""
    table = SPELLS if table is None else table
    n = name.replace('-', ' ').replace('_', ' ').lower()
    if n in table:
        return table[n]
    if n.isdigit() and table is SPELLS:
        return int(n), 2
    raise ValueError(f'no spell {name!r} (one of {", ".join(sorted(table))}{", or an id" if table is SPELLS else ""})')


# --- transforms, in the order transform() applies them -----------------------------------------

def _length(s, length):
    """End the scene length seconds after its start marker (or its zone-in)."""
    end = (mark(s, 'start') or 0) + length
    s.packets = [p for p in s.packets if p[0] <= end]
    s.marks = [m for m in s.marks if m[0] < end and m[1] != 'end'] + [[end, 'end']]


# what --zone drops: the recorded zone's other characters (0x00D, all but the recorded one), NPCs and
# mobs (0x00E), actions (0x028), action messages (0x029) and weather (0x057)
ZONE_BOUND = {CHARACTER, ENTITY, ACTION, 0x029, WEATHER}


def _zone(s, zone, at):
    """The scene plays in another zone, the character at (x, y, z, rotation): the zone-in names that zone
    (+0x30, +0x42), puts the character there and plays no zone music (+0x56); the recorded zone's
    NPCs, mobs, other players, actions and weather go. Its markers, and so its timing, stay."""
    x, y, z, rot = at
    d = s.packets[0][1]
    d[0x0B] = int(rot) & 255
    struct.pack_into('<fff', d, 0x0C, x, y, z)
    struct.pack_into('<I', d, 0x30, zone)
    struct.pack_into('<H', d, 0x42, zone)
    struct.pack_into('<4H', d, 0x56, 0, 0, 0, 0)
    s.meta['zone'] = zone
    before = len(s.packets)
    s.packets = s.packets[:1] + [p for p in s.packets[1:] if ptype(p[1]) not in ZONE_BOUND
                                 or (ptype(p[1]) == CHARACTER and entity_id(p[1]) == s.character)]
    return before - len(s.packets)


def _turn(s, degrees):
    """Turn the character at the zone-in (the camera starts behind it)."""
    d = s.packets[0][1]
    d[0x0B] = (d[0x0B] + round(degrees / 360 * 256)) & 255


def _my_actions(s):
    """Only the character's own actions: everyone else's attacks, spells and abilities go."""
    before = len(s.packets)
    s.packets = [p for p in s.packets if not (is_action(p[1]) and get_bits(p[1], ACTOR_BIT, 32) != s.character)]
    return before - len(s.packets)


def _hold(s):
    """Every NPC and mob stays where it first appears: later updates keep that position and stop
    moving, and despawns go. A crowd stays in view whatever the recorded server's AI did."""
    first, keep, n = {}, [], 0
    for p in s.packets:
        d = p[1]
        if is_entity(d):
            eid, mask = entity_id(d), d[0x0A]
            if eid in first:
                if mask & 0x20:
                    n += 1
                    continue
                d[0x0C:0x18] = first[eid]
                d[0x18:0x1A] = b'\0\0'
                n += 1
            elif mask & 0x01:
                first[eid] = bytes(d[0x0C:0x18])
        keep.append(p)
    s.packets = keep
    return n


def _clone(s, copies, radius=2.5):
    """Each mob becomes copies mobs: its updates again under a free entity index, on a ring around it
    (radius yalms for the first six copies, half as far again for each six after)."""
    if copies < 2:
        return 0
    found = mobs(s)
    order = list(dict.fromkeys(entity_id(d) for _, d in s.packets if is_entity(d) and entity_id(d) in found))
    used = used_indices(s)
    free = [i for i in range(1023, 0, -1) if i not in used][:len(order) * (copies - 1)]
    clones = {}
    k = 0
    for eid in order:
        for c in range(1, copies):
            if k == len(free):
                break
            a = 2 * math.pi * c / copies
            r = radius * (1 + 0.5 * ((c - 1) // 6))
            clones.setdefault(eid, []).append((eid & ~0xFFF | free[k], free[k], r * math.cos(a), r * math.sin(a)))
            k += 1
    out = []
    for at, d in s.packets:
        out.append([at, d])
        if is_entity(d):
            for cid, idx, dx, dz in clones.get(entity_id(d), ()):
                c = bytearray(d)
                struct.pack_into('<IH', c, 4, cid, idx)
                if c[0x0A] & 0x01:
                    x, = struct.unpack_from('<f', c, 0x0C)
                    z, = struct.unpack_from('<f', c, 0x14)
                    struct.pack_into('<f', c, 0x0C, x + dx)
                    struct.pack_into('<f', c, 0x14, z + dz)
                out.append([at, c])
    s.packets = out
    return k


MOB_CAST_TIME = 2.0  # seconds from a mob's cast start to its finish


def _mob_spells(s, spells, every):
    """From a second after the start marker to the end, every mob casts spells at the next mob round
    the character, one every every seconds, the mobs' turns spread evenly over that time so effects
    are always under way: a cast start, then MOB_CAST_TIME later its finish. Each mob takes the spells
    in turn, from its own place in the list. The mobs are those in place at the start marker."""
    start, end = mark(s, 'start') or 0, mark(s, 'end') or s.duration()
    found = mobs(s)
    cx, _, cz = struct.unpack_from('<fff', s.packets[0][1], 0x0C)
    where = {}
    for t, d in s.packets:
        if t > start:
            break
        if is_entity(d) and entity_id(d) in found:
            if d[0x0A] & 0x20:
                where.pop(entity_id(d), None)
            elif d[0x0A] & 0x01:
                where[entity_id(d)] = struct.unpack_from('<f', d, 0x0C)[0], struct.unpack_from('<f', d, 0x14)[0]
    ring = sorted(where, key=lambda e: math.atan2(where[e][1] - cz, where[e][0] - cx))
    out = []
    for j, mob in enumerate(ring):
        target = ring[(j + 1) % len(ring)]
        at, k = start + 1 + every * j / len(ring), j
        while at + MOB_CAST_TIME <= end:
            sid, message = spells[k % len(spells)]
            damage = 100 + k * 37 % 300  # varied, so the numbers don't all read the same
            out.append([at, action_packet(mob, CAST_START, CAST_BEGINS, [(target, [(0, sid, BEGINS_CASTING)])])])
            out.append([at + MOB_CAST_TIME, action_packet(mob, CAST_FINISH, sid, [(target, [(sid, damage, message)])])])
            at, k = at + every, k + 1
    s.packets += out
    s.sort()
    return len(out) // 2


def _mob_name(s, name):
    """Every mob shows as name: the client names entities 0..1023 from the zone's list and dynamic ones
    (1792..2303) from the packet, so the mobs move there, with their name; actions follow them."""
    found = mobs(s)
    zone = s.zone
    dyn = {}
    for _, d in s.packets:
        if is_entity(d) and entity_id(d) in found and entity_id(d) not in dyn and len(dyn) < 512:
            dyn[entity_id(d)] = 0x700 + len(dyn)
    new = {eid: (0x01000000 | zone << 12 | idx, idx) for eid, idx in dyn.items()}
    for _, d in s.packets:
        if is_action(d):
            actor = get_bits(d, ACTOR_BIT, 32)
            if actor in new:
                set_bits(d, ACTOR_BIT, 32, new[actor][0])
            walk = action_walk(d)
            for off in (walk[0] if walk else ()):
                t = get_bits(d, off, 32)
                if t in new:
                    set_bits(d, off, 32, new[t][0])
    n = 0
    for p in s.packets:
        d = p[1]
        if not is_entity(d) or entity_id(d) not in new:
            continue
        eid = entity_id(d)
        struct.pack_into('<IH', d, 4, *new[eid])
        if d[0x0A] & 0x20:
            continue
        # a mob's model is 4 bytes at +0x30, so the name fits at +0x34 in a 0x48-byte update
        c = bytearray(0x48)
        c[:min(len(d), 0x34)] = d[:min(len(d), 0x34)]
        c[0x34:0x44] = name.encode('ascii', 'replace')[:15].ljust(16, b'\0')
        struct.pack_into('<H', c, 0, ENTITY | (0x48 // 4) << 9)
        c[0x0A] |= 0x08
        p[1] = c
        n += 1
    return n


def _players(s, n, looks):
    """n geared characters in rings 3 yalms apart around the zone-in, a second into the scene, each a copy
    of one of looks (in turn) under a free entity index; they stand still."""
    x, h, z = struct.unpack_from('<fff', s.packets[0][1], 0x0C)
    used = used_indices(s)
    s.added = []
    index = 1023
    for i in range(n):
        while index > 0 and index in used:
            index -= 1
        if index == 0:
            break
        used.add(index)
        ring, left = 1, i
        while left >= int(math.pi * 3 * ring):  # about one character per 2 yalms of a ring
            left -= int(math.pi * 3 * ring)
            ring += 1
        a = 2 * math.pi * left / int(math.pi * 3 * ring)
        d = bytearray(looks[i % len(looks)])
        eid = 0x01000000 | s.zone << 12 | index
        struct.pack_into('<IH', d, 4, eid, index)
        d[0x0A] = 0x17  # position, claim, HP and animation, model: the name comes from the zone's list
        d[0x0B] = int(a / (2 * math.pi) * 256 + 128) & 255  # facing the middle
        struct.pack_into('<fff', d, 0x0C, x + 3 * ring * math.cos(a), h, z + 3 * ring * math.sin(a))
        d[0x18:0x1A] = b'\0\0'
        s.packets.append([1.0, d])
        s.added.append(eid)
    s.sort()
    return len(s.added)


def _echo(s, spread=0.0, spells=None):
    """The characters _players added do what the recorded one does to itself: each action whose actor and
    only target is the character, again from each of them (actor and target), k*0.37 s later within
    spread, so they do not all act in one frame. With spells ((id, animation, message) each), each
    copy of a spell cast casts one of those instead: the character's n-th spell (in the order they
    first appear) is spells[n + k] for the k-th of them, so they cast different spells at a time."""
    if not s.added:
        return 0
    order = {}  # recorded spell id -> its place among the character's spells
    out, n = [], 0
    for at, d in s.packets:
        out.append([at, d])
        if not is_action(d) or get_bits(d, ACTOR_BIT, 32) != s.character or get_bits(d, TARGETS_BIT, 6) != 1 \
                or get_bits(d, FIRST_TARGET_BIT, 32) != s.character:
            continue
        for k, eid in enumerate(s.added):
            c = bytearray(d)
            set_bits(c, ACTOR_BIT, 32, eid)
            set_bits(c, FIRST_TARGET_BIT, 32, eid)
            if spells:
                _swap_spell(c, spells, order, k)
            n += 1
            out.append([at + ((n * 0.37) % spread if spread > 0 else 0), c])
    s.packets = out
    s.sort()
    return n


def _swap_spell(d, spells, order, k):
    """A spell cast's start or finish made spells[place of its spell + k] (see _echo)."""
    result = FIRST_TARGET_BIT + 36
    category = get_bits(d, CATEGORY_BIT, 4)
    if category == CAST_START and get_bits(d, ARG_BIT, 32) == CAST_BEGINS:
        recorded = get_bits(d, result + RESULT_VALUE, 17)
    elif category == CAST_FINISH:
        recorded = get_bits(d, ARG_BIT, 32)
    else:
        return
    sid, animation, message = spells[(order.setdefault(recorded, len(order)) + k) % len(spells)]
    if category == CAST_START:
        set_bits(d, result + RESULT_VALUE, 17, sid)
    else:
        set_bits(d, ARG_BIT, 32, sid)
        set_bits(d, result + RESULT_ANIMATION, 12, animation)
        set_bits(d, result + RESULT_MESSAGE, 10, message)


def _weather(s, w):
    """The zone is in weather w when the character arrives. The client starts a weather's effects (rain,
    snow, sand) only at a zone-in; a change while in the zone moves the sky but draws none of them. So
    the zone-in carries it: +0x68 the weather and +0x6A the one before, +0x6C when it began in
    Vana'diel seconds, dated five minutes (25 of them an Earth second) before the zone-in's clock
    (+0x3C) so it shows established, and +0x70, +0x74 (a transition under way) zero. The scene's own
    weather changes (0x057) go."""
    d = s.packets[0][1]
    game_time = struct.unpack_from('<I', d, 0x3C)[0]
    struct.pack_into('<HHIII', d, 0x68, w, w, (game_time - 300 * 25) & 0xFFFFFFFF, 0, 0)
    before = len(s.packets)
    s.packets = [p for p in s.packets if ptype(p[1]) != WEATHER]
    return before - len(s.packets)


def _silence(s):
    """No battle-log text for actions: every message id in them is zeroed; the actions still play."""
    n = 0
    for _, d in s.packets:
        if ptype(d) == ACTION:
            walk = action_walk(d)
            if walk:
                for off in walk[1]:
                    set_bits(d, off, 10, 0)
                n += 1
    return n


def check(a):
    """Flags that mean nothing without another, and --at's form: a ValueError for each mistake."""
    if (a.looks or a.echo) and not a.players:
        raise ValueError('--looks and --echo go with --players')
    if (a.echo_spread or a.echo_spells) and not a.echo:
        raise ValueError('--echo-spread and --echo-spells go with --echo')
    if bool(a.zone) != bool(a.at):
        raise ValueError('--zone and --at go together')
    if a.at and len(a.at.split(',')) not in (3, 4):
        raise ValueError(f'--at {a.at}: x,y,z or x,y,z,rotation')
    if a.mob_spells_every <= 0:
        raise ValueError('--mob-spells-every: seconds, more than 0')


def transform(s, a, base):
    """Apply scene flags (an argparse namespace); notes on what each did. The order is fixed, whatever
    the flags' order: --mob-spells before --mob-name, so the new casts follow the renamed mobs, and
    --chat's silencing last, so it covers every action the others added."""
    check(a)
    notes = []
    if a.length:
        _length(s, a.length)
        notes.append(f'{a.length:g} s long')
    if a.zone:
        at = [float(v) for v in a.at.split(',')]
        dropped = _zone(s, a.zone, (at + [0])[:4])
        notes.append(f'moved to zone {a.zone} ({dropped} packets of the old one dropped)')
    if a.turn:
        _turn(s, a.turn)
        notes.append(f'turned {a.turn:g} degrees')
    if a.my_actions:
        notes.append(f"{_my_actions(s)} others' actions dropped")
    if a.hold:
        notes.append(f'{_hold(s)} updates held')
    if a.clone > 1:
        notes.append(f'{_clone(s, a.clone)} mob copies')
    if a.mob_spells:
        names = sorted(SPELLS) if a.mob_spells == 'all' else a.mob_spells.split(',')
        notes.append(f'{_mob_spells(s, [spell(n) for n in names], a.mob_spells_every)} mob spells')
    if a.mob_name:
        notes.append(f'{_mob_name(s, a.mob_name)} mob updates named {a.mob_name!r}')
    if a.players:
        looks = humanoids(s)
        if a.looks:
            looks = []
            for p in a.looks.split(','):
                looks += humanoids(Scene.load(p if os.path.isabs(p) else os.path.join(base, p)))
        if not looks:
            raise ValueError('--players: no geared NPCs to copy looks from (--looks a city recording)')
        notes.append(f'{_players(s, a.players, looks)} players ({len(looks)} looks)')
        if a.echo:
            spells = None
            if a.echo_spells:
                names = sorted(SELF_SPELLS) if a.echo_spells == 'all' else a.echo_spells.split(',')
                spells = [spell(n, SELF_SPELLS) for n in names]
            notes.append(f'{_echo(s, a.echo_spread, spells)} actions echoed' + (f' ({len(spells)} spells)' if spells else ''))
    if a.weather:
        _weather(s, weather_id(a.weather))
        notes.append(f'weather {a.weather} from the zone-in')
    if not a.chat:
        notes.append(f'{_silence(s)} actions silenced')
    return notes
