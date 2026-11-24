"""Build uamtest.nro into build/uamtest/ (py -3.12 platforms/switch/tests/uamtest/build.py).

The deko3d back end's first step: uam, devkitPro's GLSL-to-Maxwell compiler, as a library on the console, checked
against uam.exe on the PC. The shaders are drawcost's (py -3.12 platforms/switch/tests/drawcost/build.py dk first):
their GLSL (build/drawcost/glsl/) and the DKSH uam.exe made of it (build/drawcost/romfs/shaders/) go into this
program's romfs. XI_UAM_LIB: the uam library (libuam.a, with uam_api.h beside it in its source); default the
scratch build in D:/work/xi-scratch/uam.
"""

import os
import shutil
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / 'drawcost'))
import build as dc  # drawcost's flags, tools and link step

ROOT = dc.ROOT
OUT = ROOT / 'build' / 'uamtest'
UAM = Path(os.environ.get('XI_UAM_DIR', 'D:/work/xi-scratch/uam'))
UAM_LIB = Path(os.environ.get('XI_UAM_LIB', str(UAM / 'build-nx' / 'libuam.a')))


def main():
    glsl_dir = ROOT / 'build' / 'drawcost' / 'glsl'
    ref_dir = ROOT / 'build' / 'drawcost' / 'romfs' / 'shaders'
    if not glsl_dir.exists() or not ref_dir.exists():
        raise SystemExit('no drawcost shaders: run py -3.12 platforms/switch/tests/drawcost/build.py dk first')
    if not UAM_LIB.exists():
        raise SystemExit(f'no uam library at {UAM_LIB} (XI_UAM_LIB)')
    romfs = OUT / 'romfs'
    shutil.rmtree(romfs, ignore_errors=True)
    (romfs / 'glsl').mkdir(parents=True)
    (romfs / 'ref').mkdir(parents=True)
    lines = []
    for src in sorted(glsl_dir.glob('*.glsl')):
        name = src.stem
        stage = 0 if name.startswith(('vs', 'gvs', 'cvs')) else 4
        shutil.copyfile(src, romfs / 'glsl' / src.name)
        ref = ref_dir / (name + '.dksh')
        if ref.exists():
            shutil.copyfile(ref, romfs / 'ref' / ref.name)
        lines.append(f'{name} {stage}')
    (romfs / 'list.txt').write_text('\n'.join(lines) + '\n')
    dc.OUT = OUT  # (drawcost's link writes beside its OUT)
    obj = dc.compile_c(HERE / 'main.c', OUT / 'obj' / 'main.o', ['-I' + str(UAM / 'source')])
    dc.link('uamtest', 'uamtest', [obj, UAM_LIB], ['-lstdc++', '-lnx', '-lm'],
            [dc.PORTLIBS / 'lib', dc.LIBNX / 'lib'], romfs=romfs)


if __name__ == '__main__':
    main()
