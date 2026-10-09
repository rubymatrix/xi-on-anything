#!/usr/bin/env python3
"""runtime/portable/geometry_simd.c, built twice (SIMD, and GEOMETRY_SIMD_SCALAR) with
tests/geometry_simd_test.c into a temporary folder: each must match the test's own per-lane
reference, and the two must produce the same digest of every output byte. No game data. CC and
CFLAGS choose the compiler.

  python3 tests/geometry_simd_test.py
  CC='clang -arch x86_64' python3 tests/geometry_simd_test.py
  CC=cl python tests/geometry_simd_test.py  # x64 Native Tools prompt
  CFLAGS='-fsanitize=address,undefined' python3 tests/geometry_simd_test.py
"""

import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    cc = shlex.split(os.environ.get('CC', 'cl' if os.name == 'nt' else 'cc'))
    msvc = Path(cc[0]).stem.lower() in ('cl', 'clang-cl')
    extra = shlex.split(os.environ.get('CFLAGS', ''))
    source = ROOT / 'runtime/portable/geometry_simd.c'
    test = ROOT / 'tests/geometry_simd_test.c'
    results = []
    with tempfile.TemporaryDirectory(prefix='xi-geometry-') as directory:
        tmp = Path(directory)
        for scalar in (False, True):
            exe = tmp / ('scalar' if scalar else 'simd')
            if os.name == 'nt':
                exe = exe.with_suffix('.exe')
            if msvc:
                flags = ['/nologo', '/O2', '/std:c11', '/W4', '/WX', '/fp:strict', '/I' + str(source.parent)]
                defines = ['/DGEOMETRY_SIMD_SCALAR=1'] if scalar else []
                command = cc + flags + extra + defines + [str(source), str(test), '/Fe:' + str(exe)]
            else:
                flags = ['-O2', '-std=c11', '-Wall', '-Wextra', '-Werror', '-fno-fast-math', '-ffp-contract=off',
                         '-I', str(source.parent)]
                defines = ['-DGEOMETRY_SIMD_SCALAR=1'] if scalar else []
                command = cc + flags + extra + defines + [str(source), str(test), '-o', str(exe)]
            subprocess.run(command, cwd=tmp, check=True)
            result = subprocess.run([str(exe)], cwd=tmp, text=True, capture_output=True)
            if result.returncode:
                sys.stdout.write(result.stdout)
                sys.stderr.write(result.stderr)
                result.check_returncode()
            record = json.loads(result.stdout)
            results.append(record)
            print(json.dumps({'path': 'scalar' if scalar else 'simd', **record}), flush=True)
        if results[0]['digest'] != results[1]['digest']:
            raise SystemExit('SIMD/scalar byte digests differ')
    print('geometry_simd: PASS (synthetic correctness only; no game FPS measurement)')


if __name__ == '__main__':
    main()
