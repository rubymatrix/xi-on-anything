#!/usr/bin/env python3
"""runtime/portable/gfx_visibility_storage.inc's SPIR-V arrays from their GLSL sources beside it
(gfx_visibility_storage_*.vert/.frag), compiled with glslang for Vulkan 1.1 (SPIR-V 1.3). The arrays
were made with glslang 16.5.0, the version tools/android_deps.py pins; another version may emit
different words.

  python3 tools/visibility_storage_spv.py --glslang <glslang> [--write]

Without --write it only checks that the arrays match the sources (exit 1 if not).
"""

import argparse
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
INC = ROOT / 'runtime/portable/gfx_visibility_storage.inc'
ARRAYS = {  # array: source
    'visibilityQuadSpv': 'gfx_visibility_storage_quad.vert',
    'visibilitySampleSpv': 'gfx_visibility_storage_sample.vert',
    'visibilityMaskSpv': 'gfx_visibility_storage_mask_mrt.frag',
    'visibilityStoreSpv': 'gfx_visibility_storage_sample.frag',
}


def compile_words(glslang, source, out):
    result = subprocess.run([glslang, '-V', '--target-env', 'vulkan1.1', '-o', str(out), str(source)],
                            capture_output=True, text=True)
    if result.returncode:
        sys.exit('%s\n%s%s' % (source.name, result.stdout, result.stderr))
    data = out.read_bytes()
    return struct.unpack('<%dI' % (len(data) // 4), data)


def array_text(name, words):
    rows = [', '.join('0x%x' % w for w in words[i:i + 9]) for i in range(0, len(words), 9)]
    return 'static const uint32_t %s[] = {\n    %s,\n};' % (name, ',\n    '.join(rows))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--glslang', default='glslang')
    ap.add_argument('--write', action='store_true', help='rewrite the arrays in the .inc')
    args = ap.parse_args()
    text = INC.read_text()
    stale = []
    with tempfile.TemporaryDirectory(prefix='xi-visibility-spv-') as tmp:
        for name, source in ARRAYS.items():
            words = compile_words(args.glslang, ROOT / 'runtime/portable' / source, Path(tmp) / (name + '.spv'))
            match = re.search(r'static const uint32_t %s\[\] = \{(.*?)\};' % name, text, re.S)
            if [int(w, 16) for w in re.findall(r'0x[0-9a-fA-F]+', match.group(1))] != list(words):
                stale.append(name)
                text = text.replace(match.group(0), array_text(name, words))
    if args.write and stale:
        INC.write_text(text)
    print('%s: %s' % (INC.relative_to(ROOT), ('rewrote ' if args.write else 'stale: ') + ', '.join(stale) if stale
                                             else 'every array matches its source'))
    return 1 if stale and not args.write else 0


if __name__ == '__main__':
    sys.exit(main())
