#!/usr/bin/env python3
"""runtime/portable/geometry_guest.c's admission guard (tests/geometry_guest_test.c) and
geometry_hooks.c's wrappers (tests/geometry_hooks_test.c), against a build.h written by
tools/build.py for 2025-11-12, with and without its "geometry" layout. No game files. CC and
CFLAGS choose the compiler.

  python3 tests/geometry_guest_test.py
"""

import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import build  # noqa: E402
import buildinfo  # noqa: E402


def main():
    cc = shlex.split(os.environ.get('CC', 'cl' if os.name == 'nt' else 'cc'))
    msvc = Path(cc[0]).stem.lower() in ('cl', 'clang-cl')
    extra = shlex.split(os.environ.get('CFLAGS', ''))
    with tempfile.TemporaryDirectory(prefix='xi-geometry-guest-') as directory:
        tmp = Path(directory)
        includes = [ROOT / 'runtime', ROOT / 'runtime/portable', tmp]
        if msvc:
            flags = ['/nologo', '/O2', '/std:c11', '/W3', '/fp:strict', '/DRT_GUEST_WINDOW']
            flags += ['/I' + str(p) for p in includes]
        else:
            flags = ['-O2', '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-D_DEFAULT_SOURCE',
                     '-D_DARWIN_C_SOURCE', '-DRT_GUEST_WINDOW']
            flags += sum((['-I', str(p)] for p in includes), [])
        # build.h from the writer every build uses
        original = build.BUILD
        configuration = buildinfo.known()['2025-11-12']
        for name, schema in [('geometry_guest_test', 1), ('geometry_hooks_test', 1), ('geometry_hooks_test', 0)]:
            build.BUILD = dict(configuration, build='2025-11-12')
            build.BUILD.setdefault('modern_keys', [])
            if not schema:
                build.BUILD['geometry'] = {}
            build.BUILD_H = str(tmp / 'build.h')
            build.write_build_h()
            sources = [
                ROOT / 'tests' / (name + '.c'),
                ROOT / 'runtime/portable/geometry_guest.c',
                ROOT / 'runtime/portable/geometry_simd.c',
            ]
            if name == 'geometry_hooks_test':
                sources += [ROOT / 'runtime/portable/geometry_hooks.c']
            exe = tmp / (name + str(schema) + ('.exe' if os.name == 'nt' else ''))
            command = cc + flags + extra + list(map(str, sources))
            command += ['/Fe:' + str(exe)] if msvc else ['-o', str(exe)]
            subprocess.run(command, cwd=tmp, check=True)
            subprocess.run([str(exe)], cwd=tmp, check=True)
        build.BUILD = original
    print('geometry_guest: PASS (guard and wrapper correctness; no FPS measurement)')


if __name__ == '__main__':
    main()
