"""Validate/export diagnostic zone captures; standard library only. See zone_capture/README.md."""
import argparse
import collections
import hashlib
import json
import math
from pathlib import Path
import struct


class Capture:
    def __init__(self, path):
        self.path = Path(path)
        self.records = [json.loads(line) for line in self.path.read_text().splitlines() if line.strip()]
        if not self.records or self.records[0].get('type') != 'header':
            raise ValueError('missing capture header')
        self.header = self.records[0]
        if self.header.get('version') != 1 or self.header.get('endian') != 'little':
            raise ValueError('unsupported capture schema')
        if self.records[-1].get('type') != 'end' or self.records[-1].get('complete') is not True:
            raise ValueError('incomplete capture; repeat with a smaller region/frame')
        self.data = Path(str(self.path) + '.bin').read_bytes()
        if len(self.data) != self.records[-1]['bytes']:
            raise ValueError('binary payload length does not match footer')
        events = self.records[1:-1]
        if [e.get('sequence') for e in events] != list(range(len(events))):
            raise ValueError('missing or reordered capture events')
        self.draws = [r for r in events if r['type'] == 'draw']
        if len(self.draws) != self.records[-1]['draws']:
            raise ValueError('draw count does not match footer')
        self._validate_refs(events)
        for d in self.draws:
            if len(self.blob(d['render_states'])) != 1024 or len(self.blob(d['transforms'])) != 280 * 64:
                raise ValueError('invalid state array size')
            if len(d['layout']) != 17 or len(d['streams']) != 4:
                raise ValueError('invalid vertex layout')

    def _validate_refs(self, value):
        if isinstance(value, dict):
            if set(value) == {'offset', 'size'}:
                self.blob(value)
            else:
                for v in value.values():
                    self._validate_refs(v)
        elif isinstance(value, list):
            for v in value:
                self._validate_refs(v)

    def blob(self, ref):
        if ref is None:
            raise ValueError('missing required payload')
        a, n = ref['offset'], ref['size']
        if type(a) is not int or type(n) is not int or a < 0 or n < 0 or a + n > len(self.data):
            raise ValueError('binary reference outside capture')
        return self.data[a:a + n]

    def words(self, ref):
        b = self.blob(ref)
        if len(b) % 4:
            raise ValueError('unaligned uint32 array')
        return struct.unpack('<' + 'I' * (len(b) // 4), b)

    def attributes(self, d):
        """Decoded input registers, never asserted to be post-shader positions or PBR albedo."""
        formats = ('f', '2f', '3f', '4f', 'I', '4B', '2h', '4h')
        result = {}
        for register, (used, stream, kind, offset) in enumerate(d['layout']):
            if not used:
                continue
            if not 0 <= stream < 4 or not 0 <= kind < len(formats) or offset < 0:
                raise ValueError('unsupported vertex element')
            s = d['streams'][stream]
            if s is None:
                raise ValueError('used vertex stream missing')
            data = self.blob(s['data'])
            fmt = '<' + formats[kind]
            size = struct.calcsize(fmt)
            values = []
            for i in range(d['vertices']):
                at = i * s['stride'] + offset
                if at + size > len(data):
                    raise ValueError('vertex element outside captured stream')
                v = struct.unpack_from(fmt, data, at)
                if any(isinstance(x, float) and not math.isfinite(x) for x in v):
                    raise ValueError('non-finite vertex attribute')
                values.append(v)
            result[str(register)] = values
        return result

    def triangles(self, d):
        n = d['count']
        expected = {4: n * 3, 5: n + 2, 6: n + 2}.get(d['primitive'])
        if expected is None:
            return []
        if d['index_size']:
            if d['index_size'] not in (2, 4):
                raise ValueError('invalid index size')
            b = self.blob(d['indices'])
            if len(b) != expected * d['index_size']:
                raise ValueError('index count does not match primitive')
            indices = [v[0] + d['index_rebase'] for v in struct.iter_unpack(
                '<H' if d['index_size'] == 2 else '<I', b)]
        else:
            indices = list(range(expected))
        if any(i < 0 or i >= d['vertices'] for i in indices):
            raise ValueError('index outside captured vertex window')
        if d['primitive'] == 4:
            triangles = [indices[i:i + 3] for i in range(0, len(indices), 3)]
        elif d['primitive'] == 5:
            triangles = [([indices[i + 1], indices[i], indices[i + 2]] if i % 2 else indices[i:i + 3])
                         for i in range(n)]
        else:
            triangles = [[indices[0], indices[i + 1], indices[i + 2]] for i in range(n)]
        return [t for t in triangles if len(set(t)) == 3]

    def report(self):
        layers = []
        coverage = collections.Counter()
        for d in self.draws:
            rs = self.words(d['render_states'])
            origins = [s['resource']['origin'] for s in d['streams'] if s and s['resource']]
            coverage['draws_with_model_origin' if any(o['load'] for o in origins) else 'draws_without_model_origin'] += 1
            if rs[27] or rs[52] or rs[47]:
                layers.append({'sequence': d['sequence'], 'target': d['target'], 'origins': origins,
                               'blend': [rs[27], rs[19], rs[20], rs[171]],
                               'depth': [rs[7], rs[14], rs[23]], 'bias_bits': rs[47],
                               'alpha_test': [rs[15], rs[25], rs[24]],
                               'stencil': {str(i): rs[i] for i in range(52, 60)},
                               'fog': [rs[28], rs[35], rs[140]], 'color_write_mask': rs[168],
                               'textures': [t['resource'] if t else None for t in d['textures']],
                               'classification': 'unclassified; ordered evidence, not a proven surface dependency'})
        return {'header': self.header, 'sha256_binary': hashlib.sha256(self.data).hexdigest(),
                'draws': len(self.draws), 'coverage': dict(coverage), 'ordered_layer_candidates': layers,
                'target_events': [r for r in self.records[1:-1] if r['type'] != 'draw'],
                'limitations': ['one submitted frame; culled geometry absent',
                                'resource/load IDs are session-local; DAT path and instance identity unresolved',
                                'zone_label is caller-supplied; not a DLL zone epoch',
                                'GPU targets may have pre-frame dependencies; copy_attempt may be rejected by adapter',
                                'original state captured before UI squeezing; host texture inset may already be applied']}

    def export(self, output):
        output = Path(output)
        output.mkdir(parents=True, exist_ok=True)
        scene, obj, offset = [], ['# FFXI source units and world coordinates; no Unreal axis/scale conversion'], 1
        for d in self.draws:
            attrs = self.attributes(d)
            triangles = self.triangles(d)
            record = dict(d, attributes=attrs, triangles=triangles)
            # Fixed-function local positions can be inspected in original world coordinates.
            # Programmable vertices retain inputs/tokens and must not be presented as recovered static meshes.
            positions = attrs.get('0', [])
            if not d['rhw'] and not self.blob(d['vs_tokens']) and positions and d['layout'][0][2] in (2, 3):
                xf = struct.unpack('<4480f', self.blob(d['transforms']))
                w = xf[24 * 16:25 * 16]
                world = []
                for p in positions:
                    v = tuple(p[:3]) + (p[3] if len(p) > 3 else 1.0,)
                    q = [sum(v[j] * w[j * 4 + k] for j in range(4)) for k in range(4)]
                    if not all(math.isfinite(x) for x in q) or abs(q[3]) < 1e-20:
                        raise ValueError('invalid world transform')
                    world.append([x / q[3] for x in q[:3]])
                record['inspection_world_positions'] = world
                obj.append(f"o draw_{d['sequence']}")
                obj.extend('v ' + ' '.join(format(x, '.9g') for x in p) for p in world)
                obj.extend('f ' + ' '.join(str(i + offset) for i in t) for t in triangles)
                offset += len(world)
            scene.append(record)
        (output / 'scene.json').write_text(json.dumps({'header': self.header, 'draws': scene}, indent=2) + '\n')
        (output / 'scene.bin').write_bytes(self.data)
        (output / 'region.obj').write_text('\n'.join(obj) + '\n')
        (output / 'layers.json').write_text(json.dumps(self.report(), indent=2) + '\n')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('capture', type=Path)
    p.add_argument('--out', type=Path, help='export decoded scene, source payloads, OBJ and layer report')
    a = p.parse_args()
    try:
        c = Capture(a.capture)
        if a.out:
            c.export(a.out)
        print(json.dumps(c.report(), indent=2))
    except (ValueError, KeyError, TypeError, OSError, struct.error) as e:
        p.exit(1, f'zone capture: {e}\n')


if __name__ == '__main__':
    main()
