"""XIGEOM1 geometry captures for tests/geometry_replay_test.py: 32 records, each a header, the Guest
before and after the parent, and the pages it touched, before and after. The capture is only read,
never rewritten. Guest mirrors runtime/guest.h's struct.
"""

import ctypes as C
import hashlib
import struct


class Guest(C.Structure):
    _fields_ = (
        [(x, C.c_uint32) for x in ['eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi']]
        + [(x, C.c_uint8) for x in ['cf', 'pf', 'af', 'zf', 'sf', 'of', 'df']]
        + [('fs_base', C.c_uint32), ('st', C.c_double * 8), ('top', C.c_uint32), ('fcw', C.c_uint16)]
        + [(x, C.c_uint8) for x in ['c0', 'c1', 'c2', 'c3']]
    )


class Header(C.Structure):
    _fields_ = (
        [('magic', C.c_char * 8)]
        + [(n, C.c_uint32) for n in ['version', 'header_bytes', 'guest_bytes', 'reloc_delta', 'page_count', 'flags',
                                     'ordinal', 'reserved']]
        + [('begin_ns', C.c_uint64), ('end_ns', C.c_uint64), ('before', Guest), ('after', Guest)]
    )


def read_capture(raw, *, select_flags0=False):
    offset = 0
    records = []
    provenance = []
    for ordinal in range(1, 33):
        start = offset
        if offset + C.sizeof(Header) > len(raw):
            raise ValueError('truncated header')
        h = Header.from_buffer_copy(raw[offset : offset + C.sizeof(Header)])
        offset += C.sizeof(Header)
        if not (
            raw[start : start + 8] == b'XIGEOM1\0'
            and h.version == 1
            and h.header_bytes == C.sizeof(Header)
            and h.guest_bytes == C.sizeof(Guest)
            and h.ordinal == ordinal
            and h.reserved == 0
            and 0 < h.page_count <= 256
            and 0 < h.begin_ns <= h.end_ns
        ):
            raise ValueError(f'invalid header {ordinal}')
        if h.flags and not select_flags0:
            raise ValueError(f'rejected record {ordinal}: flags={h.flags}')
        pages = {}
        for _ in range(h.page_count):
            if offset + 8196 > len(raw):
                raise ValueError('truncated page')
            a = struct.unpack_from('<I', raw, offset)[0]
            offset += 4
            if a % 4096 or a in pages:
                raise ValueError('unaligned or duplicate page')
            pages[a] = (raw[offset : offset + 4096], raw[offset + 4096 : offset + 8192])
            offset += 8192
        selected = h.flags == 0
        provenance.append(
            {
                'ordinal': ordinal,
                'flags': h.flags,
                'selected': selected,
                'pages': h.page_count,
                'byte_offset': start,
                'byte_length': offset - start,
                'record_sha256': hashlib.sha256(raw[start:offset]).hexdigest(),
            }
        )
        if selected:
            records.append((h, pages))
    if offset != len(raw):
        raise ValueError('trailing data')
    if not records:
        raise ValueError('no flags0 records')
    report = {
        'capture_sha256': hashlib.sha256(raw).hexdigest(),
        'capture_bytes': len(raw),
        'source_records': 32,
        'selected_records': len(records),
        'excluded_records': 32 - len(records),
        'selected_ordinals': [h.ordinal for h, _ in records],
        'selection_policy': 'all and only original flags0 records' if select_flags0 else 'strict all32 flags0',
        'source_records_detail': provenance,
        'whole_capture_admitted': not select_flags0,
        'whole_frame_admitted': False,
        'FPS_claim': False,
    }
    return records, report


def reject_output_aliases(output, inputs):
    """Keep reports from replacing preserved inputs, including hardlink aliases."""
    output = output.resolve()
    for source in inputs:
        source = source.resolve()
        if output == source or (output.exists() and source.exists() and output.samefile(source)):
            raise ValueError(f'report output aliases protected input: {source}')
