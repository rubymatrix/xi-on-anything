"""Build uamtest.nro into build/uamtest/ (py -3.12 platforms/switch/tests/uamtest/build.py).

The deko3d back end's first step: uam, devkitPro's GLSL-to-Maxwell compiler (third_party/uam), as a library on the
console, checked against uam.exe on the PC. Two sets of shaders:

- drawcost's (py -3.12 platforms/switch/tests/drawcost/build.py dk first): their GLSL (build/drawcost/glsl/) and
  the DKSH uam.exe made of it (build/drawcost/romfs/shaders/) go into this program's romfs;
- the game's, recorded on the console with FFXI_GL_KEYLOG=sdmc:/switch/uamtest/keys.bin: generated in the
  deko3d dialect (gfx_dk_generate) and compiled on the console, their GLSL and DKSH written to the card.

uam comes from tools/build_switch.py's uam_archive (thirdparty.py's build, its output calls renamed to
runtime/portable/gfx/dk_uam.c's).
"""

import importlib.util
import shutil
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
# drawcost's flags, tools and link step, under a name of its own (tools/build.py, which build_switch imports, is "build")
_spec = importlib.util.spec_from_file_location('drawcost_build', HERE.parent / 'drawcost' / 'build.py')
dc = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(dc)

ROOT = dc.ROOT
OUT = ROOT / 'build' / 'uamtest'
UAM = ROOT / 'third_party' / 'uam'


def uam_lib():
    sys.path.insert(0, str(ROOT / 'tools'))
    import build_switch as bs
    _, _, tp = bs.desktop_tools()
    return bs.uam_archive(tp)


def main():
    glsl_dir = ROOT / 'build' / 'drawcost' / 'glsl'
    ref_dir = ROOT / 'build' / 'drawcost' / 'romfs' / 'shaders'
    if not glsl_dir.exists() or not ref_dir.exists():
        raise SystemExit('no drawcost shaders: run py -3.12 platforms/switch/tests/drawcost/build.py dk first')
    lib = uam_lib()
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
    inc = ['-I' + str(UAM / 'source'), '-I' + str(ROOT / 'runtime' / 'portable')]
    objs = [dc.compile_c(HERE / 'main.c', OUT / 'obj' / 'main.o', inc)]
    # the game's shader generator (its deko3d dialect, gfx_dk_generate) and uam's glue (its messages kept)
    for src in ('gfx_msl.c', 'gfx_msl_shaders.c', 'dk_uam.c'):
        objs.append(dc.compile_c(ROOT / 'runtime' / 'portable' / 'gfx' / src, OUT / 'obj' / (src[:-2] + '.o'), inc))
    dc.link('uamtest', 'uamtest', objs + [lib], ['-lstdc++', '-lnx', '-lm'],
            [dc.PORTLIBS / 'lib', dc.LIBNX / 'lib'], romfs=romfs)


if __name__ == '__main__':
    main()
