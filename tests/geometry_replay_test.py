#!/usr/bin/env python3
"""The geometry parent with FFXI_NATIVE_GEOMETRY's wrappers against the game's own x86/SSE code run
in Unicorn: synthetic animated meshes (--frames per single/dual stream, with and without bone
remapping) and, with --capture, every flags-0 record of an XIGEOM1 capture. Writes a JSON report.

Needs your own prepared 2025-11-12 build (generated/all, generated/FFXiMain.unpacked.dll), pefile
and unicorn. The translated functions are copied into a temporary folder only; nothing from the
game is written anywhere else. Checks geometry, not gameplay or frame rate.

  python3 tests/geometry_replay_test.py --out build/geometry-moving.json
  python3 tests/geometry_replay_test.py --capture capture.bin --out build/geometry-capture.json
"""

import argparse
import ctypes as C
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shlex
import struct
import subprocess
import sys
import tempfile
import pefile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE, UC_MEM_READ
from unicorn.x86_const import *
from geometry_capture_selection import Guest, read_capture, reject_output_aliases

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import build  # noqa: E402

REGS = dict(eax=UC_X86_REG_EAX, ebx=UC_X86_REG_EBX, ecx=UC_X86_REG_ECX, edx=UC_X86_REG_EDX,
            esi=UC_X86_REG_ESI, edi=UC_X86_REG_EDI, ebp=UC_X86_REG_EBP, esp=UC_X86_REG_ESP)
FLAGS = dict(cf=0, pf=2, af=4, zf=6, sf=7, df=10, of=11)


def local_library(directory):
    config = build.BUILD  # tools/prepare.py checked the install's hashes
    if config['build'] != '2025-11-12':
        raise ValueError('only the verified layout can be compared')
    # The wrapped functions and everything they call, copied whole and unchanged from the
    # translation into the temporary folder.
    source = '\n'.join(p.read_text() for p in (ROOT / 'generated/all').glob('funcs_*.c'))
    functions = dict(re.findall(r'(?:^|\n)(void (f_[0-9a-f]+(?:_body)?)\(Guest\* g\)\n\{.*?\n\})', source, re.S))
    functions = {name: text for text, name in functions.items()}
    names = {f'f_{int(value, 16):08x}' for key, value in config['wraps'].items() if key.startswith('geometry_')}
    todo = list(names)
    selected = {}
    while todo:
        name = todo.pop()
        if name in selected:
            continue
        text = functions[name]
        selected[name] = text
        todo += [n for n in re.findall(r'\b(f_[0-9a-f]+(?:_body)?)\(g\)', text) if n not in selected]
    # rt_wrap_/rt_orig_ storage from the generated table
    table = (ROOT / 'generated/all/table.c').read_text()
    definitions = '\n'.join(
        line for line in table.splitlines() if re.match(r'(?:const )?GuestFn rt_(?:wrap|orig)_geometry_', line)
    )
    unit = directory / 'local_parent.c'
    unit.write_text('#include "runtime.h"\n#include "funcs.h"\n' + definitions + '\n' + '\n'.join(selected.values()))
    build.BUILD_H = str(directory / 'build.h')
    build.write_build_h()
    library = directory / ('geometry.dylib' if sys.platform == 'darwin' else 'geometry.so')
    command = shlex.split(os.environ.get('CC', 'cc')) + [
        '-O2', '-std=c11', '-fPIC', '-shared', '-D_DEFAULT_SOURCE', '-D_DARWIN_C_SOURCE', '-DRT_GUEST_WINDOW',
        '-Wno-unused-label', '-Wno-unused-variable', '-Wno-unused-function', '-Wno-parentheses-equality',
        '-I' + str(ROOT / 'runtime'), '-I' + str(ROOT / 'runtime/portable'), '-I' + str(ROOT / 'generated/all'),
        '-I' + str(directory), str(unit), str(ROOT / 'tests/geometry_replay_fixture.c')]
    command += [str(ROOT / 'runtime/portable' / name)
                for name in ('geometry_guest.c', 'geometry_simd.c', 'geometry_hooks.c')]
    subprocess.run(command + ['-o', str(library)], check=True)
    lib = C.CDLL(str(library))
    lib.test_page.argtypes = [C.c_uint32, C.c_void_p]
    lib.test_run.argtypes = [C.POINTER(Guest)]
    lib.test_scalar.argtypes = [C.POINTER(Guest)]
    assert lib.test_init()
    return lib, hashlib.sha256(library.read_bytes()).hexdigest()


def synthetic(frame, dual, remap):
    memory = bytearray(0x20000)
    globals_ = bytearray(0x20000)

    def write(a, fmt, *v):
        dst, offset = (memory, a) if a < len(memory) else (globals_, a - 0x10454000)
        struct.pack_into('<' + fmt, dst, offset, *v)

    for a, v in [
        (0x10454538, 0x2400),
        (0x104553A0, 0x4000),
        (0x10459468, 2 | (2 << 16)),
        (0x1042, 0x2800),
        (0x1048, 0x3000),
        (0x105E, 0x2000),
        (0x103C, 0x7000),
        (0x205C, 0x2900),
        (0x2060, 2),
        (0x206E, 0x8000),
        (0x2076, 0xA000),
        (0x208E, 0xC000),
        (0x2092, 0xD000),
        (0x2096, 0xE000),
        (0x209A, 0xF000),
    ]:
        write(a, 'I', v)
    write(0x2800, 'H', 2)
    write(0x1032, 'H', 0x80 if remap else 0)
    write(0x1034, 'B', dual)
    for i in range(128):
        write(0x7000 + 2 * i, 'H', (i + 3) % 128 if remap else i)
    for a, v in [
        (0x3000, 1 | (2 << 7)),
        (0x3004, 3 | (4 << 7)),
        (0x3008, 5 | (6 << 7)),
        (0x300A, 7 | (8 << 7)),
        (0x300C, 9 | (10 << 7)),
        (0x300E, 11 | (12 << 7)),
        (0x2900, 0),
        (0x2902, 3),
    ]:
        write(a, 'H', v)
    for bone in range(128):
        theta = (frame + bone) * 0.03125
        c = math.cos(theta)
        s = math.sin(theta)
        write(0x4000 + 64 * bone, '16f', c, s, 0, 0, -s, c, 0, 0, 0, 0, 1, 0,
              frame * 0.25 + bone, math.sin(frame * 0.1) * 8, bone * 0.125, 1)
    for source in (0x8000, 0xA000):
        for i in range(2):
            write(source + 24 * i, '6f', 1 + i, 2 + i, 3 + i, 0, 1, 0)
        for i in range(2):
            write(source + 48 + 56 * i, '14f', 1 + i, 2 + i, 3 + i, 0, 1, 0, 0.25, 4 + i, 5 + i, 6 + i, 1, 0, 0, 0.75)
    g = Guest()
    g.ecx = 0x1000
    g.esp = 0x1E000
    g.fcw = 0x023F
    pages = {a: bytes(memory[a : a + 4096]) for a in range(0, len(memory), 4096)}
    pages.update({0x10454000 + a: bytes(globals_[a : a + 4096]) for a in range(0, len(globals_), 4096)})
    return g, pages


def compare(lib, g, pages, delta, image, ordinal):
    base = C.c_void_p.in_dll(lib, 'rt_guest_base').value
    C.c_uint32.in_dll(lib, 'rt_reloc_delta').value = delta
    lib.test_reset_pages()
    for a, data in pages.items():
        lib.test_page(a, data)
    native = Guest.from_buffer_copy(bytes(g))
    if not lib.test_run(C.byref(native)):
        assert bytes(native) == bytes(g)
        assert all(C.string_at(base + a, 4096) == data for a, data in pages.items())
        return False, 0
    imagebase = 0x10000000 + delta
    u = Uc(UC_ARCH_X86, UC_MODE_32)
    size = (len(image) + 4095) & ~4095
    u.mem_map(imagebase, size)
    u.mem_write(imagebase, image)
    mapped = set(range(imagebase, imagebase + size, 4096))
    for a, data in pages.items():
        if a not in mapped:
            u.mem_map(a, 4096)
            mapped.add(a)
        u.mem_write(a, data)
    info = struct.unpack('<I', C.string_at(base + 0x10454538 + delta, 4))[0]
    u.mem_write(info + 9, b'\1')
    for k, r in REGS.items():
        u.reg_write(r, getattr(g, k))
    u.reg_write(UC_X86_REG_EFLAGS, 2 | sum(getattr(g, k) << n for k, n in FLAGS.items()))
    u.reg_write(UC_X86_REG_MXCSR, 0x1F80)
    u.reg_write(UC_X86_REG_FPCW, g.fcw)
    stop = 0x10000
    while stop in mapped:
        stop += 4096
    u.mem_map(stop, 4096)
    oldret = bytes(u.mem_read(g.esp, 4))
    u.mem_write(g.esp, struct.pack('<I', stop))
    errors = []
    returns = {imagebase + 0x301B2, imagebase + 0x307B1}

    def coverage(uc, access, address, size, value, user):
        if address < g.esp + 4 and g.esp < address + size:
            if access == UC_MEM_READ and address == g.esp and size == 4 and uc.reg_read(UC_X86_REG_EIP) in returns:
                return
            errors.append(('synthetic return used as data', hex(address)))
            uc.emu_stop()
            return
        for page in range(address & ~4095, (address + size + 4095) & ~4095, 4096):
            if page not in pages:
                errors.append(('uncovered data page', hex(page)))
                uc.emu_stop()
                return

    u.hook_add(UC_HOOK_MEM_READ | UC_HOOK_MEM_WRITE, coverage)
    u.emu_start(imagebase + 0x2EFE0, stop, count=10000000)
    assert not errors, (ordinal, errors)
    assert u.reg_read(UC_X86_REG_EIP) == stop, (ordinal, 'did not return')
    u.mem_write(g.esp, oldret)
    u.mem_write(info + 9, b'\0')
    for a in pages:
        expected = bytes(u.mem_read(a, 4096))
        actual = C.string_at(base + a, 4096)
        if expected != actual:
            k = next(k for k in range(4096) if expected[k] != actual[k])
            raise AssertionError(
                (ordinal, 'memory mismatch', hex(a + k), expected[k : k + 16].hex(), actual[k : k + 16].hex())
            )
    for k, r in REGS.items():
        assert getattr(native, k) == u.reg_read(r), (ordinal, k, getattr(native, k), u.reg_read(r))
    for k, n in FLAGS.items():
        assert getattr(native, k) == ((u.reg_read(UC_X86_REG_EFLAGS) >> n) & 1), (ordinal, k)
    # the SSE route leaves the guest's x87 registers and control state alone
    for k in ('fs_base', 'top', 'fcw', 'c0', 'c1', 'c2', 'c3'):
        assert getattr(native, k) == getattr(g, k), (ordinal, k)
    assert bytes(native.st) == bytes(g.st)
    return True, len(pages) * 4096


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--frames', type=int, default=120)
    parser.add_argument('--capture', type=Path)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    if not 1 <= args.frames <= 10000:
        parser.error('--frames must be 1..10000')
    reject_output_aliases(
        args.out, [ROOT / 'generated/FFXiMain.unpacked.dll'] + ([args.capture] if args.capture else [])
    )
    with tempfile.TemporaryDirectory(prefix='xi-geometry-oracle-') as directory:
        lib, digest = local_library(Path(directory))
        pe = pefile.PE(str(ROOT / 'generated/FFXiMain.unpacked.dll'))
        image = pe.get_memory_mapped_image()
        total = 0
        checked = 0
        for dual in (0, 1):
            for remap in (0, 1):
                for frame in range(args.frames):
                    g, pages = synthetic(frame, dual, remap)
                    admitted, n = compare(lib, g, pages, 0, image, (dual, remap, frame))
                    assert admitted, 'synthetic frame rejected'
                    total += 1
                    checked += n
        capture = None
        if args.capture:
            raw = args.capture.read_bytes()
            records, selection = read_capture(raw, select_flags0=True)
            accepted = []
            fallback = []
            for h, pages in records:
                pe = pefile.PE(str(ROOT / 'generated/FFXiMain.unpacked.dll'))
                if h.reloc_delta:
                    pe.relocate_image(0x10000000 + h.reloc_delta)
                before = {a: v[0] for a, v in pages.items()}
                admitted, n = compare(lib, h.before, before, h.reloc_delta, pe.get_memory_mapped_image(), h.ordinal)
                checked += n
                if admitted:
                    accepted.append(h.ordinal)
                else:
                    # Rejected entries must still match the captured scalar execution.
                    scalar = Guest.from_buffer_copy(bytes(h.before))
                    lib.test_scalar(C.byref(scalar))
                    assert bytes(scalar) == bytes(h.after), (h.ordinal, 'scalar fallback Guest')
                    base = C.c_void_p.in_dll(lib, 'rt_guest_base').value
                    assert all(C.string_at(base + a, 4096) == v[1] for a, v in pages.items()), (
                        h.ordinal,
                        'scalar fallback pages',
                    )
                    fallback.append(h.ordinal)
            assert args.capture.read_bytes() == raw
            capture = {'selection': selection, 'admitted': accepted, 'scalar_fallback': fallback}
        lib.test_destroy()
        report = {
            'synthetic_moving_parents': total,
            'page_bytes_compared': checked,
            'mismatches': 0,
            'capture': capture,
            'candidate_library_sha256': digest,
            'strict_original_data_access_coverage': True,
            'original': 'x86 SSE in Unicorn',
            'moving_gameplay_validated': False,
            'FPS_claim': False,
        }
        args.out.write_text(json.dumps(report, indent=2) + '\n')
        print(json.dumps({k: v for k, v in report.items() if k != 'capture'}, indent=2))


if __name__ == '__main__':
    main()
