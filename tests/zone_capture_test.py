"""Run after tools/build_posix.py gfxtest (or the Windows gfxtest target)."""
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from zone_capture import Capture
from zone_capture_suite import make_suite


def recording(zone):
    packet = bytearray(0x104)
    struct.pack_into('<H', packet, 0, 0x00A | ((len(packet) // 4) << 9))
    struct.pack_into('<I', packet, 4, 1)
    struct.pack_into('<I', packet, 0x30, zone)
    return '\n'.join(json.dumps(r) for r in (
        {'meta': {'zone': zone}}, {'t': 0, 'dir': 'in', 'id': 10, 'hex': packet.hex()})) + '\n'


class CaptureTest(unittest.TestCase):
    def setUp(self):
        self.path = ROOT / 'build/d3d8test/zone.jsonl'
        self.capture = Capture(self.path)

    def test_static_dynamic_indices_and_provenance(self):
        draws = [d for d in self.capture.draws if any(
            s and s['resource'] and s['resource']['origin']['resource'] == 0x1234 for s in d['streams'])]
        self.assertEqual(len(draws), 4)
        self.assertEqual([self.capture.triangles(d) for d in draws],
                         [[[0, 1, 2], [2, 1, 3]], [[0, 1, 2]]] * 2)
        # Static and dynamic buffers must yield identical original vertex windows, including colors.
        self.assertEqual(self.capture.attributes(draws[0]), self.capture.attributes(draws[2]))
        self.assertEqual(self.capture.attributes(draws[1]), self.capture.attributes(draws[3]))
        self.assertEqual(self.capture.attributes(draws[1])['0'][0], (0.0, 1.0, 0.5))
        self.assertEqual(self.capture.attributes(draws[0])['5'][0], (0xFFFF8000,))
        self.assertNotEqual(draws[0]['streams'][0]['resource']['id'], draws[2]['streams'][0]['resource']['id'])
        self.assertEqual(draws[0]['streams'][0]['resource']['origin']['fourcc'], 0x74736574)
        self.assertEqual(draws[0]['streams'][0]['resource']['origin']['parents'][0], 0x697a696d)
        self.assertTrue(any(t and t['surfaces'] for d in self.capture.draws for t in d['textures']))

    def test_depth_blend_stencil_recipe_is_lossless(self):
        draw = self.capture.draws[-1]
        rs = self.capture.words(draw['render_states'])
        self.assertEqual([rs[i] for i in (7, 14, 23, 47)], [1, 0, 7, 0xfffffff9])
        layer = self.capture.report()['ordered_layer_candidates'][-1]
        self.assertEqual(layer['blend'], [1, 9, 1, 1])
        self.assertEqual(layer['alpha_test'], [1, 5, 91])
        self.assertEqual(layer['color_write_mask'], 7)
        self.assertEqual([layer['stencil'][str(i)] for i in (52, 57, 58, 59)], [1, 7, 0x5a, 0xa5])
        self.assertEqual(draw['translated']['zbias'], -7)

    def test_export_preserves_payload_and_order(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.capture.export(tmp)
            path = Path(tmp)
            scene = json.loads((path / 'scene.json').read_text())
            self.assertEqual(len(scene['draws']), len(self.capture.draws))
            self.assertEqual((path / 'scene.bin').read_bytes(), self.capture.data)
            self.assertIn('f ', (path / 'region.obj').read_text())
            report = json.loads((path / 'layers.json').read_text())
            ordered = [d['sequence'] for d in report['ordered_layer_candidates']]
            self.assertEqual(ordered, sorted(ordered))
            self.assertEqual(report['header']['zone_label'], 107)

    def test_reject_corrupt_and_incomplete(self):
        for mutation in ('footer', 'payload', 'offset', 'order', 'failed'):
            with self.subTest(mutation=mutation), tempfile.TemporaryDirectory() as tmp:
                p = Path(tmp) / 'capture.jsonl'
                records = json.loads(json.dumps(self.capture.records))
                data = self.capture.data
                if mutation == 'footer':
                    records.pop()
                elif mutation == 'payload':
                    data = data[:-1]
                elif mutation == 'offset':
                    next(r for r in records if r['type'] == 'draw')['render_states']['offset'] = len(data) + 1
                elif mutation == 'order':
                    records[1]['sequence'] = 100
                else:
                    records[-1]['complete'] = False
                p.write_text('\n'.join(json.dumps(r) for r in records))
                Path(str(p) + '.bin').write_bytes(data)
                with self.assertRaises(ValueError):
                    Capture(p)

    def test_suite_uses_real_visits_and_rejects_wrong_zone(self):
        with tempfile.TemporaryDirectory() as tmp:
            paths = [Path(tmp) / 'south visit.jsonl', Path(tmp) / 'valkurm.jsonl']
            for p, zone in zip(paths, (107, 103)):
                p.write_text(recording(zone))
            out = Path(tmp) / 'suite.txt'
            make_suite(paths, out)
            lines = [s for s in out.read_text().splitlines() if not s.startswith('#')]
            self.assertEqual(len(lines), 8)
            self.assertIn('--turn 270', lines[-1])
            self.assertIn('south-gustaberg-0', lines[0])
            self.assertNotIn('--zone', out.read_text())
            with self.assertRaises(ValueError):
                make_suite(paths[::-1], out)


if __name__ == '__main__':
    unittest.main()
