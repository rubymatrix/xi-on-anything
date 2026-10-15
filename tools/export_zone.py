"""Export installed zone resources through a bounded C loader, without launching the game."""
import argparse
import collections
import ctypes as C
import hashlib
import json
import math
import os
from pathlib import Path
import shlex
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
PROFILE = '2026-09-03'


def u32(data, offset=0):
    return struct.unpack_from('<I', data, offset)[0]


def text(data):
    return bytes(data).decode('ascii', errors='backslashreplace').rstrip(' \0')


def checked(data, offset, size):
    if offset < 0 or size < 0 or offset + size > len(data):
        raise ValueError(f'resource range {offset:#x}+{size:#x} outside {len(data):#x}')
    return data[offset:offset + size]


class Batch(C.Structure):
    _fields_ = [(k, C.c_uint32) for k in (
        'part_offset', 'part_flags', 'submesh_offset', 'flags', 'vertex_offset',
        'vertex_count', 'vertex_stride', 'index_offset', 'index_count', 'primitive', 'next_offset')]


class DatFile(C.Structure):
    _fields_ = [('data', C.c_void_p), ('size', C.c_size_t)]


class DatImage(C.Structure):
    _fields_ = [('w', C.c_uint32), ('h', C.c_uint32), ('rgba', C.c_void_p)]


class Loader:
    def __init__(self, library=None):
        sources = [ROOT / 'host' / n for n in ('zone_resource.c', 'zone_resource.h', 'datui.c', 'datui.h')]
        suffix = '.dylib' if sys.platform == 'darwin' else '.dll' if os.name == 'nt' else '.so'
        lib = Path(library) if library else ROOT / 'build' / ('libzone_resource' + suffix)
        if library is None and (not lib.exists() or max(p.stat().st_mtime for p in sources) > lib.stat().st_mtime):
            lib.parent.mkdir(parents=True, exist_ok=True)
            compiler = shlex.split(os.environ.get('XI_CC', 'clang'))
            subprocess.run(compiler + ['-std=c11', '-O2', '-shared', '-fPIC', str(sources[0]),
                                      str(sources[2]), '-o', str(lib)], check=True)
        self.lib = C.CDLL(str(lib.resolve()))
        ptr = C.c_void_p
        self.lib.xi_zone_decode_mmb.argtypes = [ptr, C.c_size_t, ptr, ptr]
        self.lib.xi_zone_decode_mzb.argtypes = [ptr, C.c_size_t, ptr]
        self.lib.xi_zone_mmb_batch.argtypes = [ptr, C.c_size_t, C.c_uint32, C.c_uint32, C.POINTER(Batch)]
        self.lib.dat_image.argtypes = [C.POINTER(DatFile), C.c_char_p, C.c_char_p, C.POINTER(DatImage)]
        self.lib.dat_image_native.argtypes = self.lib.dat_image.argtypes
        self.lib.dat_image_free.argtypes = [C.POINTER(DatImage)]

    def decode(self, payload, kind, tables):
        if not payload or kind not in (0x1c, 0x2e) or len(tables) != (2 if kind == 0x2e else 1) or any(len(t) != 256 for t in tables):
            raise ValueError('invalid native decoder inputs')
        buf = C.create_string_buffer(payload, len(payload))
        fn = self.lib.xi_zone_decode_mmb if kind == 0x2e else self.lib.xi_zone_decode_mzb
        keys = [C.create_string_buffer(t, len(t)) for t in tables]
        if not fn(buf, len(payload), *keys):
            raise ValueError(f'malformed or unsupported resource type {kind:#x}')
        return buf

    def batches(self, buf):
        data = buf.raw
        parts = 9 if data[3] < 2 else u32(data, 32)
        if parts > len(data) // 4:
            raise ValueError('invalid mesh part count')
        result = []
        for part in range(parts):
            submesh = 0
            while True:
                batch = Batch()
                status = self.lib.xi_zone_mmb_batch(buf, len(data), part, submesh, C.byref(batch))
                if status < 0:
                    raise ValueError(f'invalid mesh part {part}, submesh {submesh}')
                if status == 0:
                    break
                result.append((part, submesh, {k: getattr(batch, k) for k, _ in batch._fields_}))
                submesh += 1
        return result


class ClientProfile:
    """Addresses only are checked in. Table bytes stay in the user's matching unpacked image."""
    def __init__(self, game, image):
        metadata = json.loads((ROOT / 'meta/builds.json').read_text())['builds'][PROFILE]
        self.retail_hash = hashlib.sha256((game / 'FFXiMain.dll').read_bytes()).hexdigest()
        if self.retail_hash != metadata['FFXiMain.dll']['sha256']:
            raise ValueError(f'zone loader profile requires client {PROFILE}; do not use offsets from another build')
        self.image = Path(image).read_bytes()
        self.image_hash = hashlib.sha256(self.image).hexdigest()
        pe = u32(self.image, 0x3c)
        if checked(self.image, pe, 4) != b'PE\0\0':
            raise ValueError('invalid unpacked PE image')
        optional = pe + 24
        if struct.unpack_from('<H', self.image, optional)[0] != 0x10b:
            raise ValueError('expected 32-bit unpacked client')
        self.base = u32(self.image, optional + 28)
        self.timestamp = u32(self.image, pe + 8)
        retail = (game / 'FFXiMain.dll').read_bytes()
        retail_pe = u32(retail, 0x3c)
        if self.base != 0x10000000 or self.timestamp != u32(retail, retail_pe + 8):
            raise ValueError('unpacked image metadata differs from the pinned retail client')
        self.sections = []
        section = optional + struct.unpack_from('<H', self.image, pe + 20)[0]
        for i in range(struct.unpack_from('<H', self.image, pe + 6)[0]):
            at = section + i * 40
            virtual, raw_size, raw = u32(self.image, at + 12), u32(self.image, at + 16), u32(self.image, at + 20)
            self.sections.append((virtual, raw_size, raw))
        # Confirm profile table references exist in translated C rather than silently selecting offsets.
        source = (ROOT / 'generated/all/funcs_026.c').read_text()
        for va in ('10376AC0', '10376DE8', '10376FEF'):
            if va.lower() not in source.lower():
                raise ValueError('translated loader source does not match required profile')
        self.xor = self.at(0x10376ac0, 256)
        self.swap = self.at(0x10376de8, 256)
        self.invert = self.at(0x10376ef0, 256)

    def at(self, va, size):
        rva = va - self.base
        for virtual, raw_size, raw in self.sections:
            if virtual <= rva and rva + size <= virtual + raw_size:
                return checked(self.image, raw + rva - virtual, size)
        raise ValueError(f'table address {va:#x} absent from unpacked client')


def resolve_dat(game, file_id):
    """The merged VTABLE/FTABLE chooses the expansion and 7-bit directory/file split."""
    candidates = [(game / 'VTABLE.DAT', game / 'FTABLE.DAT')]
    for rom in range(2, 10):
        candidates.append((game / f'ROM{rom}/VTABLE{rom}.DAT', game / f'ROM{rom}/FTABLE{rom}.DAT'))
    entry = None
    for vpath, fpath in candidates:
        if not vpath.exists() or not fpath.exists():
            continue
        v, f = vpath.read_bytes(), fpath.read_bytes()
        if file_id < len(v) and v[file_id] and file_id * 2 + 2 <= len(f):
            entry = v[file_id], struct.unpack_from('<H', f, file_id * 2)[0]
    if entry is None:
        raise ValueError(f'DAT ID {file_id} not installed')
    rom, slot = entry
    folder = 'ROM' if rom == 1 else f'ROM{rom}'
    return game / folder / str(slot >> 7) / f'{slot & 127}.DAT'


def chunks(data, file_id):
    at, parents, result = 0, [], []
    while at < len(data):
        header = checked(data, at, 16)
        info = u32(header, 4)
        size, kind = ((info >> 7) & 0x7ffff) * 16, info & 127
        if size < 16:
            raise ValueError(f'invalid DAT chunk size at {at:#x}')
        checked(data, at, size)
        if kind == 0:
            if not parents:
                raise ValueError('unbalanced DAT container end')
            parents.pop()
        item = {'key': f'dat:{file_id}/chunk:{at:08x}', 'offset': at, 'size': size,
                'type': kind, 'fourcc_hex': header[:4].hex(), 'name': text(header[:4]),
                'parents': list(parents)}
        result.append(item)
        if kind == 1:
            parents.append(item['key'])
        at += size
    if parents:
        raise ValueError('unterminated DAT containers')
    return result


def uints(data, offset, count):
    payload = checked(data, offset, count * 4)
    return [v[0] for v in struct.iter_unpack('<I', payload)]


def identity():
    return [[float(i == j) for j in range(4)] for i in range(4)]


def multiply(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(4)) for j in range(4)] for i in range(4)]


def world_matrix(translation, rotation, scale):
    # Original row-vector S * Rx * Ry * Rz * T order. No UE coordinate conversion here.
    out = identity()
    for i in range(3):
        out[i][i] = scale[i]
    for axis, angle in enumerate(rotation):
        r = identity()
        i, j = ((1, 2), (2, 0), (0, 1))[axis]
        c, s = math.cos(angle), math.sin(angle)
        r[i][i] = r[j][j] = c
        r[i][j], r[j][i] = s, -s
        out = multiply(out, r)
    t = identity()
    t[3][:3] = translation
    return multiply(out, t)


def topology(indices, primitive, vertices):
    if any(i >= vertices for i in indices):
        raise ValueError('mesh index outside source vertices')
    if primitive == 4:
        if len(indices) % 3:
            raise ValueError('triangle list has incomplete primitive')
        ts = [indices[i:i + 3] for i in range(0, len(indices), 3)]
    else:
        ts = [(indices[i + 1], indices[i], indices[i + 2]) if i % 2 else indices[i:i + 3]
              for i in range(max(0, len(indices) - 2))]
    return [tuple(t) for t in ts if len(set(t)) == 3]


def lods(name, models):
    first = models.get(name, [])
    found = {k: list(first) for k in ('near', 'middle', 'far')}
    if name and name[-1] in 'hml':
        middle = models.get(name[:-1] + 'm', [])
        if middle:
            found = {k: list(middle) for k in found}
        for suffix, level in (('h', 'near'), ('l', 'far')):
            mesh = models.get(name[:-1] + suffix, [])
            if mesh:
                found[level] = list(mesh)
                for k in found:
                    if not found[k]:
                        found[k] = list(mesh)
    return found


def export_zone(game, zone, out, profile, loader):
    if zone not in (103, 107):
        raise ValueError('initial zone profile supports Valkurm Dunes (103) and South Gustaberg (107)')
    file_id = 100 + zone
    path = resolve_dat(game, file_id)
    raw = path.read_bytes()
    resources = chunks(raw, file_id)
    out.mkdir(parents=True, exist_ok=True)
    manifest = {'schema': 'ffxi-zone-resources/1', 'zone': zone,
                'name': {103: 'Valkurm Dunes', 107: 'South Gustaberg'}[zone],
                'source': {'dat_id': file_id, 'path': str(path.relative_to(game)),
                           'sha256': hashlib.sha256(raw).hexdigest(), 'size': len(raw),
                           'client_profile': PROFILE, 'client_sha256': profile.retail_hash,
                           'unpacked_image_sha256': profile.image_hash},
                'coordinate_system': 'original row-vector coordinates; UE conversion pending',
                'render_contract': {
                    'batch_order': 'original part index, then submesh index; no material sorting or merging',
                    'default_blend': {'src': 5, 'dst': 6, 'evidence': 'f_1017b510:1017b7eb-1017b80b'},
                    'default_alpha_test': {'func': 5, 'ref': 96, 'evidence': 'f_1017b510:1017b7bf-1017b7df'},
                    'transparent_depth': {'write': False, 'bias': 'runtime global 0x10631f18',
                                          'initial_bias_values': [8, 1], 'evidence': 'f_1017c9c0:1017daf8-1017db60; f_101781d0'},
                    'opaque_depth': {'write': True, 'bias': 'runtime global 0x10632038',
                                     'initial_bias': 0, 'evidence': 'f_1017c9c0:1017db2a-1017db60'},
                    'depth_compare_and_stencil': 'inherited runtime state; not synthesized from resource flags',
                    'transparency_flag': 'bit 31; requires original depth-write and bias policy',
                    'disable_culling_flag': 'bit 29',
                    'uninterpreted_flags': 'bits 30 and 28 retained; do not discard or replace with defaults',
                    'scope': 'static zone resources; runtime area/weather/effect state is not evaluated',
                    'unknown_chunks': 'preserved in source.dat and resources.bin, not silently interpreted'},
                'resources': resources, 'models': [], 'layouts': [], 'textures': [], 'issues': []}
    by_key = {r['key']: r for r in resources}
    decoded, triangles = bytearray(), bytearray()
    for resource in resources:
        payload = checked(raw, resource['offset'] + 16, resource['size'] - 16)
        kind = resource['type']
        buf = None
        if kind in (0x1c, 0x2e):
            tables = (profile.xor, profile.swap) if kind == 0x2e else (profile.invert,)
            buf = loader.decode(payload, kind, tables)
            payload = buf.raw
        base = len(decoded)
        resource['payload'] = {'file': 'resources.bin', 'offset': base, 'size': len(payload)}
        decoded += payload
        if kind == 0x2e:
            model = {'key': resource['key'], 'name': text(checked(payload, 16, 16)),
                     'version': payload[3], 'kind': payload[4], 'raw_flags': u32(payload, 4),
                     'bounds': list(struct.unpack_from('<6f', payload, 36)), 'batches': [],
                     'parent_names': [by_key[k]['name'] for k in resource['parents']]}
            for part, submesh, view in loader.batches(buf):
                key = f"{resource['key']}/part:{part}/batch:{submesh}"
                v_at, v_count, stride = view['vertex_offset'], view['vertex_count'], view['vertex_stride']
                v_data = checked(payload, v_at, v_count * stride)
                index_data = checked(payload, view['index_offset'], view['index_count'] * 2)
                idx = [v[0] for v in struct.iter_unpack('<H', index_data)]
                ts = topology(idx, view['primitive'], v_count)
                tri_at = len(triangles)
                for t in ts:
                    triangles += struct.pack('<3I', *t)
                positions = [struct.unpack_from('<3f', v_data, i * stride) for i in range(v_count)]
                if not all(math.isfinite(c) for p in positions for c in p):
                    raise ValueError(f'nonfinite geometry in {key}')
                texture = checked(payload, view['submesh_offset'], 16)
                batch = dict(view, key=key, texture_id_hex=texture.hex(), texture_name=text(texture),
                             vertices={'file': 'resources.bin', 'offset': base + v_at, 'size': len(v_data)},
                             indices={'file': 'resources.bin', 'offset': base + view['index_offset'], 'size': len(index_data)},
                             triangles={'file': 'triangles.bin', 'offset': tri_at, 'count': len(ts)},
                             attributes={'position': [0, 'float3'], 'normal': [24 if stride == 48 else 12, 'float3'],
                                         'color': [36 if stride == 48 else 24, 'D3DCOLOR'],
                                         'uv0': [40 if stride == 48 else 28, 'float2']},
                             source_order=[part, submesh], transparent=bool(view['flags'] & 0x80000000),
                             disable_culling=bool(view['flags'] & 0x20000000),
                             other_flag_bits=view['flags'] & 0x50000000)
                if stride == 48:
                    batch['attributes']['animation_delta'] = [12, 'float3']
                model['batches'].append(batch)
            manifest['models'].append(model)
        elif kind == 0x1c:
            count, version = u32(payload, 4) & 0xffffff, payload[3]
            layout = {'key': resource['key'], 'version': version, 'header_words': list(struct.unpack_from('<8I', payload)),
                      'placements': [], 'groups': [], 'spatial_nodes': []}
            for i in range(count):
                at = 32 + i * 100
                entry = checked(payload, at, 100)
                t = list(struct.unpack_from('<3f', entry, 16))
                r = list(struct.unpack_from('<3f', entry, 28))
                s = list(struct.unpack_from('<3f', entry, 40))
                if not all(math.isfinite(v) for v in t + r + s):
                    raise ValueError('nonfinite placement transform')
                name = text(entry[:16])
                layout['placements'].append({'key': f"{resource['key']}/placement:{i}", 'index': i,
                    'model_name': name, 'model_id_hex': entry[:16].hex(), 'translation': t, 'rotation': r, 'scale': s,
                    'world_matrix': world_matrix(t, r, s), 'raw_words': list(struct.unpack('<25I', entry)),
                    'block_id_hex': entry[52:56].hex(), 'lod_distances': list(struct.unpack_from('<3f', entry, 56)),
                    'render_flags': list(entry[68:72]), 'light_references': list(struct.unpack_from('<4I', entry, 84)),
                    'texture_mode': 'alpha_wrap' if name.startswith('_') else 'opaque_clamp' if name.startswith('#') else 'opaque_wrap'})
            # Preserve file indices rather than rewriting them to runtime pointers as the game does.
            group_at = u32(payload, 20)
            if group_at:
                groups = u32(payload, group_at)
                group_at += 4
                if groups > len(payload) // 4:
                    raise ValueError('invalid group count')
                for _ in range(groups):
                    n = u32(payload, group_at)
                    group_at += 4
                    refs = uints(payload, group_at, n)
                    if any(i >= count for i in refs):
                        raise ValueError('invalid group placement reference')
                    layout['groups'].append(refs)
                    group_at += n * 4
            todo, visited = [u32(payload, 16)], set()
            while todo:
                at = todo.pop()
                if not at or at in visited:
                    continue
                visited.add(at)
                node = checked(payload, at, 120)
                ref_at, n = u32(node, 96), u32(node, 100)
                children = list(struct.unpack_from('<4I', node, 104))
                refs = uints(payload, ref_at, n) if ref_at else []
                if any(i >= count for i in refs):
                    raise ValueError('invalid spatial placement reference')
                layout['spatial_nodes'].append({'offset': at, 'bounds_corners': list(struct.unpack_from('<24f', node)),
                                               'placements': refs, 'children': children})
                if not ref_at:
                    todo.extend(children)
            manifest['layouts'].append(layout)
        elif kind == 0x20:
            texture = {'key': resource['key'], 'id_hex': checked(payload, 1, 16).hex(),
                       'category': text(payload[1:9]), 'name': text(payload[9:17]), 'payload': resource['payload']}
            if len(payload) >= 57:
                texture.update(width=u32(payload, 21), height=u32(payload, 25), flag=payload[0])
            manifest['textures'].append(texture)
    models_by_name = collections.defaultdict(list)
    for model in manifest['models']:
        models_by_name[model['name']].append(model['key'])
    for layout in manifest['layouts']:
        for p in layout['placements']:
            p['lod_models'] = lods(p['model_name'], models_by_name)
            if not any(p['lod_models'].values()):
                manifest['issues'].append({'kind': 'unresolved_model', 'placement': p['key'], 'name': p['model_name']})
            if any(len(v) > 1 for v in p['lod_models'].values()):
                manifest['issues'].append({'kind': 'ambiguous_model_scope', 'placement': p['key']})
    texture_ids = {t['id_hex'] for t in manifest['textures']}
    for model in manifest['models']:
        for batch in model['batches']:
            if batch['texture_name'] and batch['texture_id_hex'] not in texture_ids:
                manifest['issues'].append({'kind': 'external_or_unresolved_texture', 'batch': batch['key'], 'texture': batch['texture_name']})
    manifest['summary'] = {'models': len(manifest['models']), 'batches': sum(len(m['batches']) for m in manifest['models']),
                           'placements': sum(len(l['placements']) for l in manifest['layouts']),
                           'triangles_all_source_lods': len(triangles) // 12, 'textures': len(manifest['textures']),
                           'transparent_batches': sum(b['transparent'] for m in manifest['models'] for b in m['batches']),
                           'issues': dict(collections.Counter(i['kind'] for i in manifest['issues']))}
    export_texture_previews(loader, raw, manifest, out)
    (out / 'source.dat').write_bytes(raw)
    (out / 'resources.bin').write_bytes(decoded)
    (out / 'triangles.bin').write_bytes(triangles)
    (out / 'zone.json').write_text(json.dumps(manifest, indent=2, allow_nan=False) + '\n')
    return manifest


def export_texture_previews(loader, raw, manifest, out):
    folder = out / 'textures'
    folder.mkdir(exist_ok=True)
    by_key = {r['key']: r for r in manifest['resources']}
    for texture in manifest['textures']:
        resource = by_key[texture['key']]
        single = checked(raw, resource['offset'], resource['size'])
        buf = C.create_string_buffer(single, len(single))
        file = DatFile(C.cast(buf, C.c_void_p), len(single))
        image = DatImage()
        ok = loader.lib.dat_image(C.byref(file), texture['category'].encode('ascii'),
                                  texture['name'].encode('ascii'), C.byref(image))
        if not ok:
            texture['inspection_image'] = None
            continue
        try:
            pixels = bytearray(C.string_at(image.rgba, image.w * image.h * 4))
            pixels[0::4], pixels[2::4] = pixels[2::4], pixels[0::4]
            filename = f"textures/{resource['offset']:08x}.tga"
            header = struct.pack('<BBBHHBHHHHBB', 0, 0, 2, 0, 0, 0, 0, 0, image.w, image.h, 32, 0x28)
            (out / filename).write_bytes(header + pixels)
            texture['inspection_image'] = filename
            texture['inspection_alpha_policy'] = 'existing datui decoder doubles legacy alpha; raw payload preserved'
        finally:
            loader.lib.dat_image_free(C.byref(image))
        native = DatImage()
        if not loader.lib.dat_image_native(C.byref(file), texture['category'].encode('ascii'),
                                           texture['name'].encode('ascii'), C.byref(native)):
            raise ValueError('native-alpha texture decode failed: ' + texture['key'])
        try:
            pixels = bytearray(C.string_at(native.rgba, native.w * native.h * 4))
            pixels[0::4], pixels[2::4] = pixels[2::4], pixels[0::4]
            filename = f"textures/{resource['offset']:08x}_native.tga"
            (out / filename).write_bytes(header + pixels)
            texture['render_image'] = filename
            texture['render_image_alpha_policy'] = 'source alpha bytes; no UI doubling; stage alpha operation belongs to material'
        finally:
            loader.lib.dat_image_free(C.byref(native))


def export_obj(manifest, out):
    """Inspection only: source coordinates, near LOD, animation phase zero, no blend emulation."""
    models = {m['key']: m for m in manifest['models']}
    data = (out / 'resources.bin').read_bytes()
    tris = (out / 'triangles.bin').read_bytes()
    textures = {t['id_hex']: t for t in manifest['textures']}
    with (out / 'zone.mtl').open('w') as material:
        material.write('newmtl untextured\nKd 1 1 1\n')
        for i, t in enumerate(manifest['textures']):
            t['obj_material'] = f'texture_{i}'
            material.write(f'newmtl texture_{i}\nKd 1 1 1\n')
            if t.get('inspection_image'):
                material.write('map_Kd ' + t['inspection_image'] + '\n')
    total = 0
    with (out / 'zone.obj').open('w') as obj:
        obj.write('# Original coordinates; near LOD; alpha/depth blending and vertex colors require zone.json\nmtllib zone.mtl\n')
        for layout in manifest['layouts']:
            for placement in layout['placements']:
                candidates = placement['lod_models']['near']
                if len(candidates) != 1:
                    continue
                matrix = placement['world_matrix']
                for b in models[candidates[0]]['batches']:
                    obj.write('o instance_' + str(placement['index']) + '_' + str(b['submesh_offset']) + '\n')
                    texture = textures.get(b['texture_id_hex'])
                    obj.write('usemtl ' + (texture.get('obj_material', 'untextured') if texture else 'untextured') + '\n')
                    for i in range(b['vertex_count']):
                        at = b['vertices']['offset'] + i * b['vertex_stride']
                        v = struct.unpack_from('<3f', data, at) + (1.0,)
                        q = [sum(v[j] * matrix[j][k] for j in range(4)) for k in range(3)]
                        obj.write('v ' + ' '.join(format(x, '.9g') for x in q) + '\n')
                    for i in range(b['vertex_count']):
                        at = b['vertices']['offset'] + i * b['vertex_stride'] + b['attributes']['uv0'][0]
                        uv = struct.unpack_from('<2f', data, at)
                        obj.write(f'vt {uv[0]:.9g} {1 - uv[1]:.9g}\n')
                    for i in range(b['triangles']['count']):
                        face = struct.unpack_from('<3I', tris, b['triangles']['offset'] + i * 12)
                        obj.write('f ' + ' '.join(f'{j + total + 1}/{j + total + 1}' for j in face) + '\n')
                    total += b['vertex_count']
    return total


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--game', type=Path, required=True)
    p.add_argument('--image', type=Path, default=ROOT / 'generated/FFXiMain.unpacked.dll')
    p.add_argument('--zone', type=int, choices=(103, 107), required=True)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--library', type=Path)
    p.add_argument('--inspect-obj', action='store_true', help='also expand near-LOD placements into an inspection OBJ')
    a = p.parse_args()
    try:
        profile = ClientProfile(a.game, a.image)
        result = export_zone(a.game, a.zone, a.out, profile, Loader(a.library))
        if a.inspect_obj:
            result['summary']['obj_vertices'] = export_obj(result, a.out)
        print(json.dumps(result['summary'], indent=2))
    except (OSError, ValueError, struct.error, subprocess.CalledProcessError) as e:
        p.exit(1, f'zone export: {e}\n')


if __name__ == '__main__':
    main()
