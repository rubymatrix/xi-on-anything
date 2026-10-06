"""tools/replayserver.py and tools/replayscene.py: recordings read into scenes, what the scene flags
do to them, and the server's events and requests; tools/replayreport.py's phases and tools/replay.py's
frame-log reading and capture conversion. The recordings are made up here; no game data.

  python3 tests/replay_test.py [-v]
"""
import json
import os
import struct
import sys
import tempfile
import unittest
import zlib

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'tools'))
import replayscene  # noqa: E402
import replay  # noqa: E402
import replayreport  # noqa: E402
import replayserver  # noqa: E402

ME = 1  # the recorded character's id


def packet(ptype, body):
    """A zone message: type and size (4-byte units) in the first two bytes, then sync, then body."""
    n = (4 + len(body) + 3) & ~3
    p = bytearray(n)
    struct.pack_into('<H', p, 0, ptype | ((n // 4) << 9))
    p[4:4 + len(body)] = body
    return p


def zone_in(char=ME, name=b'Recorder', zone=106):
    b = bytearray(0x100)
    struct.pack_into('<I', b, 0, char)
    struct.pack_into('<I', b, 0x2C, zone)
    b[0x80:0x80 + len(name)] = name
    return packet(0x00A, b)


def recording(packets, marks=(), zone=106):
    """A recording's lines: (ms, packet) and (ms, mark) after the meta line."""
    recs = [(t, {'t': t, 'dir': 'in', 'id': p[0] | (p[1] & 1) << 8, 'hex': bytes(p).hex()}) for t, p in packets]
    recs += [(t, {'t': t, 'mark': m}) for t, m in marks]
    recs.sort(key=lambda r: r[0])  # in time order, as the addon writes them
    return '\n'.join([json.dumps({'meta': {'zone': zone}})] + [json.dumps(r) for _, r in recs]) + '\n'


class Folder(unittest.TestCase):
    """A temporary folder of the test's own."""

    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.dir = tmp.name

    def write(self, name, text):
        path = os.path.join(self.dir, name)
        with open(path, 'w', encoding='utf-8') as f:
            f.write(text)
        return path


class Recordings(Folder):
    def test_scene_runs_from_zone_in_to_zone_out(self):
        s = replayscene.Scene.load(self.write('scene.jsonl', recording(
            [(500, packet(0x017, b'before')), (1000, zone_in()), (1500, packet(0x037, b'x')),
             (3000, packet(0x00B, b'')), (4000, packet(0x037, b'after'))], [(2000, 'start')])))
        self.assertEqual(s.character, ME)
        self.assertEqual([replayscene.ptype(p) for _, p in s.packets], [0x00A, 0x037])
        self.assertEqual(s.packets[1][0], 0.5)
        self.assertEqual(s.marks, [[1.0, 'start']])
        self.assertEqual(s.duration(), 1.0)

    def test_rename_replaces_whole_name_fields(self):
        chat = packet(0x017, b'\x06\x00\x00\x00' + b'Recorder\0' + b'Recorders')
        s = replayscene.Scene.load(self.write('scene.jsonl', recording([(0, zone_in()), (10, chat)])))
        self.assertEqual(s.name(), 'Recorder')
        self.assertEqual(s.rename('Replay'), 2)
        self.assertEqual(s.name(), 'Replay')
        self.assertIn(b'Recorders', bytes(s.packets[1][1]))  # not a whole field: left alone

    def test_quiet_drops_text(self):
        s = replayscene.Scene.load(self.write('scene.jsonl', recording(
            [(0, zone_in()), (10, packet(0x017, b'hi')), (20, packet(0x02D, b'xp')), (30, packet(0x00E, bytes(0x44)))])))
        self.assertEqual(s.quiet(), 2)
        self.assertEqual([replayscene.ptype(p) for _, p in s.packets], [0x00A, 0x00E])


def scenes(*labels_and_groups):
    out = []
    for label, group in labels_and_groups:
        s = replayscene.Scene(label + '.jsonl')
        s.label, s.group = label, group
        out.append(s)
    return out


class Server(Folder):
    SCENES = scenes(('markets', 'city'), ('mines', 'city'), ('rain', 'weather'))

    def test_event_is_a_chat_line_from_xireplay(self):
        e = replayserver.event('begin|1|markets|city|235')
        self.assertEqual(replayscene.ptype(e), 0x017)
        self.assertEqual(bytes(e[8:16]), b'xireplay')
        self.assertEqual(bytes(e[23:]).split(b'\0')[0], b'begin|1|markets|city|235')

    def test_requests_resolve_numbers_names_groups_and_all(self):
        resolve = replayserver.resolve
        self.assertEqual(resolve(self.SCENES, ['3', 'city']), ([2, 0, 1], []))
        self.assertEqual(resolve(self.SCENES, ['all']), ([0, 1, 2], []))
        self.assertEqual(resolve(self.SCENES, ['#2', 'snow']), ([1], ['snow']))
        self.assertEqual(resolve(self.SCENES, ['Rain', 'CITY']), ([2, 0, 1], []))

    def test_a_flag_mistake_is_an_error_in_the_line(self):
        path = self.write('suite.txt', 'scene.jsonl --no-such-flag\n')
        with self.assertRaises(SystemExit) as e:
            replayserver.load_suite(path, 'Replay')
        self.assertIn('suite.txt:1: unrecognized arguments: --no-such-flag', str(e.exception))


def entity(eid, index, mask, x=0.0, alive=True, look=0):
    d = bytearray(0x44)
    struct.pack_into('<IHB', d, 0, eid, index, mask)
    struct.pack_into('<f', d, 0x08, x)
    d[0x21] = 0x08 if alive else 0
    struct.pack_into('<H', d, 0x2C, look)
    d[0x2F] = 3  # race, for a geared character
    return packet(0x00E, d)


def action(actor, target, message=100):
    """An action with one target and one result: actor, target and message where the stream puts them."""
    d = packet(replayscene.ACTION, bytes(56))
    sc = replayscene
    sc.set_bits(d, sc.ACTOR_BIT, 32, actor)
    sc.set_bits(d, sc.TARGETS_BIT, 6, 1)
    sc.set_bits(d, sc.FIRST_TARGET_BIT, 32, target)
    sc.set_bits(d, sc.FIRST_TARGET_BIT + 32, 4, 1)
    sc.set_bits(d, sc.FIRST_TARGET_BIT + 36 + sc.RESULT_MESSAGE, 10, message)
    return d


def actions(s):
    return [(at, d) for at, d in s.packets if replayscene.ptype(d) == replayscene.ACTION]


def field(d, bit, n=32):
    return replayscene.get_bits(d, bit, n)


class Transforms(unittest.TestCase):
    def scene(self, packets, marks=(), zone=106):
        s = replayscene.Scene('made up')
        s.meta = {'zone': zone}
        s.character = ME
        s.packets = [[0.0, zone_in(zone=zone)]] + [[t, p] for t, p in packets]
        s.marks = [list(m) for m in marks]
        return s

    def test_length_ends_the_scene_after_its_start(self):
        s = self.scene([(3, packet(0x037, b'in')), (9, packet(0x037, b'out'))], [(2, 'start'), (30, 'end')])
        replayscene._length(s, 5)
        self.assertEqual(len(s.packets), 2)
        self.assertEqual(s.marks, [[2, 'start'], [7, 'end']])

    def test_turn_turns_the_character_at_the_zone_in(self):
        s = self.scene([])
        replayscene._turn(s, 90)
        self.assertEqual(s.packets[0][1][0x0B], 64)

    def test_my_actions_drops_everyone_elses(self):
        s = self.scene([(5, action(0x0106A001, ME)), (6, action(ME, ME))])
        self.assertEqual(replayscene._my_actions(s), 1)
        self.assertEqual([field(d, replayscene.ACTOR_BIT) for _, d in actions(s)], [ME])

    def test_hold_keeps_position_and_drops_despawn(self):
        s = self.scene([(1, entity(0x0106A001, 1, 0x1F, x=10)), (2, entity(0x0106A001, 1, 0x01, x=20)),
                        (3, entity(0x0106A001, 1, 0x30, x=20))])
        self.assertEqual(replayscene._hold(s), 2)
        self.assertEqual(len(s.packets), 3)
        self.assertEqual(struct.unpack_from('<f', s.packets[2][1], 0x0C)[0], 10)

    def test_clone_uses_free_indices_in_the_zone(self):
        s = self.scene([(1, entity(0x0106A001, 1, 0x1F))])
        self.assertEqual(replayscene._clone(s, 3), 2)
        ids = [replayscene.entity_id(d) for _, d in s.packets[1:]]
        idx = [struct.unpack_from('<H', d, 8)[0] for _, d in s.packets[1:]]
        self.assertEqual(len(set(idx)), 3)
        for eid, i in zip(ids, idx):
            self.assertEqual((eid & ~0xFFF, eid & 0x3FF), (0x0106A000, i))

    def test_mob_spells_every_mob_casts_at_the_next(self):
        mobs = [entity(0x0106A000 + i, i, 0x1F, x=float(i)) for i in range(1, 5)]
        s = self.scene([(1, m) for m in mobs], [(2, 'start'), (12, 'end')])
        fire, drain = replayscene.spell('fire-iv'), replayscene.spell('drain')
        self.assertEqual(replayscene._mob_spells(s, [fire, drain], 4), 8)  # 4 mobs, twice each in 10 s
        sc = replayscene
        starts = [(at, field(d, sc.ACTOR_BIT) & 0xFFF) for at, d in actions(s) if field(d, sc.CATEGORY_BIT, 4) == sc.CAST_START]
        self.assertEqual(starts[:4], [(3, 1), (4, 2), (5, 3), (6, 4)])  # turns spread over the 4 s
        finishes = [d for _, d in actions(s) if field(d, sc.CATEGORY_BIT, 4) == sc.CAST_FINISH]
        first = finishes[0]
        targets, messages = sc.action_walk(first)
        self.assertEqual((field(first, sc.ACTOR_BIT) & 0xFFF, [field(first, o) & 0xFFF for o in targets]), (1, [2]))
        self.assertEqual((field(first, sc.ARG_BIT), field(first, messages[0], 10)), (147, 2))
        self.assertEqual(field(finishes[1], sc.ARG_BIT), 245)  # the next mob starts one further down the list

    def test_mob_name_moves_actions_with_the_mobs(self):
        s = self.scene([(1, entity(0x0106A005, 5, 0x1F)), (2, action(ME, 0x0106A005))])
        replayscene._mob_name(s, 'Monster')
        mob = s.packets[1][1]
        self.assertEqual(struct.unpack_from('<H', mob, 8)[0], 0x700)
        self.assertEqual(bytes(mob[0x34:0x3B]), b'Monster')
        self.assertEqual(field(s.packets[2][1], replayscene.FIRST_TARGET_BIT), replayscene.entity_id(mob))

    def test_players_echo_what_the_character_does_to_itself(self):
        s = self.scene([(1, entity(0x010EA001, 1, 0x1F, look=1)), (2, action(ME, ME))], zone=234)
        looks = replayscene.humanoids(s)
        self.assertEqual(len(looks), 1)
        self.assertEqual(replayscene._players(s, 3, looks), 3)
        self.assertEqual(replayscene._echo(s, 1.0), 3)
        acts = actions(s)
        self.assertEqual(len(acts), 4)
        for _, d in acts:
            self.assertEqual(field(d, replayscene.ACTOR_BIT), field(d, replayscene.FIRST_TARGET_BIT))
        self.assertEqual(len({at for at, _ in acts}), 4)  # staggered

    def test_echo_spells_give_each_character_its_own(self):
        sc = replayscene
        blaze = 249
        start = sc.action_packet(ME, sc.CAST_START, sc.CAST_BEGINS, [(ME, [(0, blaze, sc.BEGINS_CASTING)])])
        finish = sc.action_packet(ME, sc.CAST_FINISH, blaze, [(ME, [(blaze, 34, 230)])])
        s = self.scene([(1, entity(0x010EA001, 1, 0x1F, look=1)), (2, start), (4, finish)], zone=234)
        sc._players(s, 3, sc.humanoids(s))
        spells = [sc.spell(n, sc.SELF_SPELLS) for n in ('haste', 'regen-iii', 'cure-iv')]
        self.assertEqual(sc._echo(s, 0, spells), 6)
        result = sc.FIRST_TARGET_BIT + 36
        cast = {}  # character -> (the spell its start names, the spell its finish casts, the finish's animation)
        for _, d in actions(s):
            who = field(d, sc.ACTOR_BIT)
            if who == ME:
                continue
            if field(d, sc.CATEGORY_BIT, 4) == sc.CAST_START:
                cast[who] = (field(d, result + sc.RESULT_VALUE, 17),)
            else:
                cast[who] += (field(d, sc.ARG_BIT), field(d, result + sc.RESULT_ANIMATION, 12))
        self.assertEqual(sorted(cast.values()), [(4, 4, 4), (57, 57, 57), (111, 111, 140)])

    def test_silence_zeroes_message_ids(self):
        s = self.scene([(2, action(ME, ME))])
        self.assertEqual(replayscene._silence(s), 1)
        d = actions(s)[0][1]
        self.assertEqual(field(d, replayscene.action_walk(d)[1][0], 10), 0)

    def test_zone_moves_the_zone_in_and_drops_the_old_zone(self):
        s = self.scene([(1, entity(0x0106A001, 1, 0x1F)), (2, action(0x0106A001, ME)), (3, packet(0x037, b'me'))],
                       [(2, 'start'), (12, 'end')])
        self.assertEqual(replayscene._zone(s, 104, (49.5, 0.25, 3.5, 64)), 2)
        d = s.packets[0][1]
        self.assertEqual((struct.unpack_from('<I', d, 0x30)[0], struct.unpack_from('<H', d, 0x42)[0], s.zone),
                         (104, 104, 104))
        self.assertEqual((struct.unpack_from('<fff', d, 0x0C), d[0x0B]), ((49.5, 0.25, 3.5), 64))
        self.assertEqual([replayscene.ptype(p) for _, p in s.packets], [0x00A, 0x037])
        self.assertEqual(s.marks, [[2, 'start'], [12, 'end']])

    def test_weather_rides_the_zone_in(self):
        s = self.scene([(1, packet(0x057, struct.pack('<IHH', 1000000, 0, 8)))])
        struct.pack_into('<I', s.packets[0][1], 0x3C, 2000000)  # the zone-in's clock
        self.assertEqual(replayscene._weather(s, replayscene.weather_id('squall')), 1)  # its 0x057 goes
        w, before, began = struct.unpack_from('<HHI', s.packets[0][1], 0x68)
        self.assertEqual((w, before, began), (7, 7, 2000000 - 300 * 25))


class SceneFlags(Folder):
    """Suite lines: flags parsed, checked and applied by tools/replayserver.py's load_scene."""

    def setUp(self):
        super().setUp()
        self.write('scene.jsonl', recording([(0, zone_in()), (500, packet(0x037, b'x'))], [(100, 'start'), (900, 'end')]))

    def load(self, flags):
        return replayserver.load_scene(['scene.jsonl'] + flags.split(), 'Replay', self.dir)

    def test_flags_reach_the_scene(self):
        s = self.load('--label rain --group weather --zone 104 --at 1,2,3 --weather hot-spell --length 0.5')
        d = s.packets[0][1]
        self.assertEqual((s.label, s.group, s.zone), ('rain', 'weather', 104))
        self.assertEqual(struct.unpack_from('<fff', d, 0x0C), (1, 2, 3))
        self.assertEqual(struct.unpack_from('<H', d, 0x68)[0], replayscene.WEATHERS.index('hot spell'))
        self.assertEqual(s.marks[-1], [0.6, 'end'])

    def test_a_negative_position_is_a_value_not_a_flag(self):
        s = self.load('--zone 35 --at -322.5,5,-362.75,219 --turn -90')
        self.assertEqual(struct.unpack_from('<fff', s.packets[0][1], 0x0C), (-322.5, 5, -362.75))

    def test_flags_that_need_another(self):
        for flags in ('--echo', '--looks a.jsonl', '--echo-spells haste', '--at 1,2,3', '--zone 104', '--zone 104 --at 1,2',
                      '--weather fog-bank', '--mob-spells no-such-spell'):
            with self.subTest(flags=flags), self.assertRaises(ValueError):
                self.load(flags)


class Report(Folder):
    def frames(self, lines):
        return replayreport.compute(*replayreport.load(self.write('frames.csv', '\n'.join(lines) + '\n')))

    def test_phases_from_the_frame_log(self):
        lines = ['m,0,begin|1|crowd|crowd|106', 'm,1000000,mark|1|crowd|start']
        lines += [f'f,{1000000 + 20000 * i}' for i in range(101)]  # 50 fps for 2 s
        lines += ['m,3000000,mark|1|crowd|end', 'm,3100000,end|1|crowd', 'm,3100001,done|1']
        phases = self.frames(lines)
        self.assertEqual([p['phase'] for p in phases], ['measured'])
        m = phases[0]
        self.assertEqual((m['from_s'], m['to_s'], m['frames']), (1.0, 3.0, 100))
        self.assertAlmostEqual(m['fps'], 50.0)
        self.assertEqual(m['frames_over_33ms'], 0)

    def test_a_scene_played_twice_is_measured_twice(self):
        lines = []
        for begin, ms in ((0, 20), (10000000, 40)):
            lines += [f'm,{begin},begin|1|rain|weather|109', f'm,{begin + 1000000},mark|1|rain|start']
            lines += [f'f,{begin + 1000000 + ms * 1000 * i}' for i in range(2000000 // (ms * 1000) + 1)]
            lines += [f'm,{begin + 3000000},mark|1|rain|end', f'm,{begin + 3100000},end|1|rain']
        fps = [round(p['fps']) for p in self.frames(lines)]
        self.assertEqual(fps, [50, 25])


class Runner(Folder):
    def test_events_are_read_once_and_whole(self):
        path = self.write('frames.csv', 'f,1\nm,2,begin|1|rain|weather|109\nm,3,mark|1|ra')
        events = replay.Events(path)
        self.assertEqual(events.new(), ['begin|1|rain|weather|109'])
        with open(path, 'a', encoding='utf-8') as f:
            f.write('in|start\nf,4\n')
        self.assertEqual(events.new(), ['mark|1|rain|start'])
        self.assertEqual(events.new(), [])

    def test_a_capture_becomes_a_png(self):
        raw = self.write('shot.raw', '')
        with open(raw, 'wb') as f:
            f.write(b'XIF1' + struct.pack('<III', 2, 1, 22) + bytes([1, 2, 3, 255, 4, 5, 6, 255]))  # B, G, R, X
        self.assertTrue(replay.png(raw, os.path.join(self.dir, 'shot.png')))
        with open(os.path.join(self.dir, 'shot.png'), 'rb') as f:
            data = f.read()
        self.assertEqual(data[:8], b'\x89PNG\r\n\x1a\n')
        idat = data.index(b'IDAT')
        size = struct.unpack_from('>I', data, idat - 4)[0]
        self.assertEqual(zlib.decompress(data[idat + 4:idat + 4 + size]), bytes([0, 3, 2, 1, 6, 5, 4]))
        self.assertFalse(replay.png(self.write('other.raw', 'not a capture'), os.path.join(self.dir, 'x.png')))


if __name__ == '__main__':
    unittest.main()
