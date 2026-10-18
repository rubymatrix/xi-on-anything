"""Export a zone's collision world (the MZB collision section) as the client loads it.

The client keeps its zone collision in the MZB resource (type 0x1c). At zone load the MZB loader
(f_10179430) reads the header, then CollisionWorld::Load (f_10167f50, `this` = map model manager
+0x178) relocates the collision section in place and uses it directly:

    MZB +0x03   u8   version (instances are 0xc0 bytes from 0x1a on, 0x80 before)
    MZB +0x08   u32  collision section offset (0 = no collision)
    MZB +0x0c   u8 x4  a, b, c, d: grid is (a*c)/4 by (b*d)/4 cells of 4 yalms, centred on 0,0
    MZB +0x1c   u8   0 when the section carries a prebuilt cell table
    MZB +0x1d   u8   flags: bit 2 wider sweep query (5x5 cells), bit 3 wider ray query

    section +0x00 mesh count, +0x04 meshes, +0x08 block count, +0x0c blocks, +0x10 cell table,
            +0x14 instances, +0x18 instance count            (offsets relative to the MZB start)

    mesh      u32 vertices, u32 normals, u32 triangles, i16 triangle count
    triangle  u16 x4: vertex a (& 0x7fff), b (& 0x3fff), c (& 0x3fff), normal (& 0x7fff); the top
              bit of each is a surface flag; bit 14 of b marks a degenerate triangle (set at load)
    instance  0xc0 bytes: 4x4 world matrix, its inverse, 3x3 normal matrix, u32 flags +0xa4,
              u8 materials +0xac..+0xaf, f32 y min +0xb4, y max +0xb8 (row vectors, y down)
    block     u32 header (count & 0x7ff, flag 0x2000 = skipped by queries), count x (u32 instance,
              u32 mesh), u32 next block (0 ends the cell's chain)

The collision queries that read this are described in ffxi-pc client/systems/movement-collision.md.
Output (local only; derived from game data, never checked in):

    collision.bin   'XICOL1\\0\\0' header, then meshes, instances, cell index and cell entries
    collision.json  counts, header bytes and per-section hashes for comparison with a live client
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import export_zone as ez  # noqa: E402

MAGIC = b'XICOL1\0\0'


def parse(d):
    w = lambda o: struct.unpack_from('<I', d, o)[0]
    version, section = d[3], w(8)
    if not section:
        return None
    if version < 0x1a:
        raise ValueError(f'MZB version {version:#x}: 0x80-byte instances not supported')
    a, b, c, e = d[0xc:0x10]
    grid_w, grid_h = (a * c) // 4, (b * e) // 4
    mesh_count, meshes_at, block_count, blocks_at, cells_at, inst_at, inst_count = \
        struct.unpack_from('<7I', d, section)
    if d[0x1c] != 0 or not cells_at:
        raise ValueError('MZB has no prebuilt cell table (byte 0x1c set); runtime build not supported')

    mesh_index, meshes = {}, []
    at = meshes_at
    for i in range(mesh_count):
        verts, norms, tris = struct.unpack_from('<3I', d, at)
        n_tri = struct.unpack_from('<h', d, at + 12)[0]
        raw = [struct.unpack_from('<4H', d, tris + t * 8) for t in range(n_tri)]
        n_vert = 1 + max([max(r[0] & 0x7fff, r[1] & 0x3fff, r[2] & 0x3fff) for r in raw], default=-1)
        n_norm = 1 + max([r[3] & 0x7fff for r in raw], default=-1)
        mesh_index[at] = i
        meshes.append((ez.checked(d, verts, n_vert * 12), ez.checked(d, norms, n_norm * 12),
                       ez.checked(d, tris, n_tri * 8), n_vert, n_norm, n_tri))
        at = tris + n_tri * 8

    instances = [ez.checked(d, inst_at + i * 0xc0, 0xc0) for i in range(inst_count)]

    def instance(off):
        if (off - inst_at) % 0xc0 or not 0 <= (off - inst_at) // 0xc0 < inst_count:
            raise ValueError(f'cell entry instance {off:#x} is not in the instance table')
        return (off - inst_at) // 0xc0

    cells, entries, blocks_seen = [], [], set()
    for cell in range(grid_w * grid_h):
        start, block = len(entries), w(cells_at + cell * 4)
        while block:
            blocks_seen.add(block)
            header = w(block)
            for k in range(header & 0x7ff):
                inst, mesh = struct.unpack_from('<2I', d, block + 4 + k * 8)
                if inst == 0 or mesh == 0:
                    continue  # the loader reports these as broken entries
                if mesh not in mesh_index:
                    raise ValueError(f'cell entry mesh {mesh:#x} is not in the mesh table')
                entries.append((instance(inst), mesh_index[mesh], header & ~0x7ff))
            block = w(block + 4 + (header & 0x7ff) * 8)
        cells.append((start, len(entries) - start))

    return dict(version=version, grid=(grid_w, grid_h), grid_bytes=(a, b, c, e), flags=d[0x1d],
                origin=(a * c * 0.5, b * e * 0.5), meshes=meshes, instances=instances, cells=cells,
                entries=entries, block_count=block_count, blocks_reached=len(blocks_seen),
                offsets=dict(section=section, meshes=meshes_at, blocks=blocks_at, cells=cells_at,
                             instances=inst_at))


def write(world, out, zone):
    blob = bytearray(MAGIC)
    blob += struct.pack('<IIIIffIIII', zone, world['version'], *world['grid'], *world['origin'], world['flags'],
                        len(world['meshes']), len(world['instances']), len(world['entries']))
    for verts, norms, tris, n_vert, n_norm, n_tri in world['meshes']:
        blob += struct.pack('<3I', n_vert, n_norm, n_tri) + verts + norms + tris
    for inst in world['instances']:
        blob += inst
    for start, count in world['cells']:
        blob += struct.pack('<2I', start, count)
    for inst, mesh, flags in world['entries']:
        blob += struct.pack('<3I', inst, mesh, flags)
    (out / 'collision.bin').write_bytes(blob)
    return hashlib.sha256(blob).hexdigest()


def export(game, zone, out, profile, loader):
    file_id = 100 + zone
    raw = ez.resolve_dat(game, file_id).read_bytes()
    found = []
    for r in ez.chunks(raw, file_id):
        if r['type'] != 0x1c:
            continue
        d = loader.decode(ez.checked(raw, r['offset'] + 16, r['size'] - 16), 0x1c, (profile.invert,)).raw
        world = parse(d)
        if world:
            found.append((r, world))
    if len(found) != 1:
        raise ValueError(f'zone {zone}: expected one MZB with collision, found {len(found)}')
    resource, world = found[0]
    out.mkdir(parents=True, exist_ok=True)
    digest = write(world, out, zone)
    triangles = sum(m[5] for m in world['meshes'])
    summary = {'schema': 'ffxi-zone-collision/1', 'zone': zone, 'resource': resource['key'],
               'client_profile': ez.PROFILE, 'mzb_version': world['version'],
               'grid': world['grid'], 'grid_bytes': world['grid_bytes'], 'origin_offset': world['origin'],
               'flags': world['flags'], 'meshes': len(world['meshes']), 'triangles': triangles,
               'instances': len(world['instances']), 'cells_nonempty': sum(1 for _, n in world['cells'] if n),
               'entries': len(world['entries']), 'blocks': world['block_count'],
               'blocks_reached': world['blocks_reached'], 'mzb_offsets': world['offsets'],
               'collision_sha256': digest}
    (out / 'collision.json').write_text(json.dumps(summary, indent=2) + '\n')
    return summary


def main():
    p = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    p.add_argument('--game', type=Path, required=True)
    p.add_argument('--image', type=Path, default=ez.ROOT / 'generated/FFXiMain.unpacked.dll')
    p.add_argument('--zone', type=int, nargs='+', required=True)
    p.add_argument('--out', type=Path, required=True, help='one subfolder per zone id is written here')
    p.add_argument('--library', type=Path)
    a = p.parse_args()
    try:
        profile, loader = ez.ClientProfile(a.game, a.image), ez.Loader(a.library)
        for zone in a.zone:
            print(json.dumps(export(a.game, zone, a.out / str(zone), profile, loader)))
    except (OSError, ValueError, struct.error, subprocess.CalledProcessError) as e:
        p.exit(1, f'collision export: {e}\n')


if __name__ == '__main__':
    main()
