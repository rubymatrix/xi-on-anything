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

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'tools'))
import replayscene  # noqa: E402
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


if __name__ == '__main__':
    unittest.main()
