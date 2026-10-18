"""Bounded native zone-reader tests: synthetic data only; no client installation required."""
import ctypes as C
from pathlib import Path
import struct
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from export_zone import DatFile, DatImage, Batch, Loader, chunks, lods, topology, world_matrix


def mesh():
    # Empty first part and populated second part exercise sparse part tables.
    b = bytearray(68 + 32 + 20 + 108 + 4 + 8)
    struct.pack_into('<I', b, 0, len(b) | 4 << 24)
    b[4] = 1
    b[16:32] = b'fixtureh        '
    struct.pack_into('<I', b, 32, 2)
    struct.pack_into('<II', b, 60, 0, 68)
    struct.pack_into('<I', b, 68, 1)
    b[100:116] = b'model   rock    '
    struct.pack_into('<I', b, 116, 0xb0000003)
    for i, p in enumerate(((0, 0, 0), (1, 0, 0), (0, 1, 0))):
        struct.pack_into('<3f3fI2f', b, 120 + i * 36, *p, 0, 0, 1, 0xff804020, 0, 0)
    struct.pack_into('<I3H', b, 228, 3, 0, 1, 2)
    return b


class NativeResourceTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.loader = Loader()

    def test_native_texture_alpha_preserves_source_bytes(self):
        payload = bytearray(57 + 16)
        payload[1:9] = b'fixture '
        payload[9:17] = b'alpha   '
        struct.pack_into('<IiiHH', payload, 17, 40, 4, 1, 1, 32)
        for i, alpha in enumerate((0, 64, 128, 255)):
            payload[57+i*4:61+i*4] = bytes((10, 20, 30, alpha))
        size = (16 + len(payload) + 15) & ~15
        raw = bytearray(size)
        struct.pack_into('<I', raw, 4, 0x20 | (size // 16) << 7)
        raw[16:16+len(payload)] = payload
        buf = C.create_string_buffer(bytes(raw), len(raw))
        file = DatFile(C.cast(buf, C.c_void_p), len(raw))
        for method, expected in (('dat_image_native', [0, 64, 128, 255]),
                                 ('dat_image', [0, 128, 255, 255])):
            image = DatImage()
            self.assertEqual(getattr(self.loader.lib, method)(C.byref(file), b'fixture', b'alpha', C.byref(image)), 1)
            try:
                pixels = C.string_at(image.rgba, 16)
                self.assertEqual(list(pixels[3::4]), expected)
                self.assertEqual(pixels[:3], bytes((30, 20, 10)))
            finally:
                self.loader.lib.dat_image_free(C.byref(image))

    def test_sparse_parts_preserve_flags_and_vertex_window(self):
        raw = mesh()
        buf = C.create_string_buffer(bytes(raw), len(raw))
        parts = self.loader.batches(buf)
        self.assertEqual(len(parts), 1)
        part, submesh, batch = parts[0]
        self.assertEqual((part, submesh), (1, 0))
        self.assertEqual(batch['flags'], 0xb0000003)
        self.assertEqual((batch['vertex_count'], batch['vertex_stride'], batch['primitive']), (3, 36, 5))
        self.assertEqual(batch['index_offset'], 232)
        self.assertEqual(struct.unpack_from('<I', raw, batch['vertex_offset'] + 24)[0], 0xff804020)

    def test_native_rejects_truncation_and_overflowing_counts(self):
        for mutation in ('header', 'parts', 'vertices', 'indices'):
            with self.subTest(mutation=mutation):
                raw = mesh()
                if mutation == 'header':
                    raw = raw[:7]
                else:
                    at = {'parts': 32, 'vertices': 116, 'indices': 228}[mutation]
                    struct.pack_into('<I', raw, at, 0xffffffff)
                buf = C.create_string_buffer(bytes(raw), len(raw))
                result = self.loader.lib.xi_zone_mmb_batch(buf, len(raw), 1, 0, C.byref(Batch()))
                self.assertEqual(result, -1)

    def test_decode_owns_data_and_round_trips_encrypted_vertices(self):
        original = mesh()
        original[3] = 5
        table = bytes(range(256))
        encrypted = self.loader.decode(bytes(original), 0x2e, (table, table))
        self.assertNotEqual(encrypted.raw[8:], original[8:])
        decoded = self.loader.decode(encrypted.raw, 0x2e, (table, table))
        self.assertEqual(decoded.raw, original)
        self.assertEqual(self.loader.batches(decoded)[0][2]['vertex_count'], 3)

    def test_mzb_bounds_and_names(self):
        raw = bytearray(132)
        struct.pack_into('<II', raw, 0, 132 | 27 << 24, 1)
        raw[32:48] = b'fixtureh        '
        tables = (bytes([1]) * 256,)
        encoded = self.loader.decode(bytes(raw), 0x1c, tables)
        decoded = self.loader.decode(encoded.raw, 0x1c, tables)
        self.assertEqual(decoded.raw, raw)
        raw[4:8] = b'\xff\xff\xff\x00'
        with self.assertRaises(ValueError):
            self.loader.decode(bytes(raw), 0x1c, tables)

    def test_topology_and_lod_fallback(self):
        self.assertEqual(topology([0, 1, 2, 2, 3, 4], 5, 5), [(0, 1, 2), (3, 2, 4)])
        with self.assertRaises(ValueError):
            topology([0, 1, 5], 4, 3)
        self.assertEqual(lods('rockh', {'rockm': ['middle'], 'rockl': ['far']}),
                         {'near': ['middle'], 'middle': ['middle'], 'far': ['far']})

    def test_transform_and_container_identity(self):
        matrix = world_matrix([10, 20, 30], [0, 0, 0], [2, 3, 4])
        self.assertEqual(matrix[3][:3], [10, 20, 30])
        self.assertEqual([matrix[i][i] for i in range(3)], [2, 3, 4])
        data = b'root' + struct.pack('<I', 1 | 1 << 7) + bytes(8)
        data += b'mesh' + struct.pack('<I', 0x2e | 1 << 7) + bytes(8)
        data += b'root' + struct.pack('<I', 1 << 7) + bytes(8)
        rs = chunks(data, 207)
        self.assertEqual(rs[1]['parents'], [rs[0]['key']])
        self.assertEqual(len(set(r['key'] for r in rs)), 3)
        with self.assertRaises(ValueError):
            chunks(data[:-16], 207)


if __name__ == '__main__':
    unittest.main()
