"""Build drawcost's two .nro files into build/drawcost/ (py -3.12 platforms/switch/tests/drawcost/build.py):

  drawcost-gl.nro  OpenGL 4.3 core on Mesa 26 (the game's build in build/switch/mesa/<MESA_NAME>), through EGL
  drawcost-dk.nro  deko3d, with its shaders compiled by uam into romfs:/shaders/

The flags and libraries are tools/build_switch.py's (ARCH, link_nro, the Mesa folder and GFX_LIBS), copied here so
that script and build/switch/ are not touched. Options: gl, dk (one of them), clean.
"""

import os
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]
OUT = ROOT / 'build' / 'drawcost'
DKP = Path(os.environ.get('XI_DEVKITPRO', 'D:/msys64/opt/devkitpro'))
A64 = DKP / 'devkitA64' / 'bin'
LIBNX = DKP / 'libnx'
PORTLIBS = DKP / 'portlibs' / 'switch'
TOOLS = DKP / 'tools' / 'bin'
MESA_NAME = 'mesa-switch-26.2.4-0ff5207'
MESA = Path(os.environ.get('XI_SWITCH_MESA', str(ROOT / 'build' / 'switch' / 'mesa' / MESA_NAME)))
SCRATCH = Path(os.environ.get('XI_SCRATCH_TMP', 'D:/work/xi-scratch/tmp'))

ARCH = ['-march=armv8-a+crc+crypto', '-mtune=cortex-a57', '-mtp=soft', '-fPIE']
CFLAGS = ARCH + ['-O2', '-g', '-Wall', '-ffunction-sections', '-fno-strict-aliasing', '-fno-tree-pta', '-std=gnu11',
                 '-D__SWITCH__', '-I' + str(HERE), '-I' + str(OUT / 'gen'),
                 '-I' + str(PORTLIBS / 'include'), '-I' + str(LIBNX / 'include')]
GL_LIBS = ['-lEGL', '-lglapi', '-lz', '-lexpat', '-lstdc++']  # build_switch.py's GFX_LIBS with Mesa 26
WRAP = ['-Wl,--wrap=__syscall_thread_create']
AUTHOR = 'xi-on-anything'
VERSION = '1.0'


def env():
    e = dict(os.environ)
    e['DEVKITPRO'] = DKP.as_posix()
    for k in ('TMP', 'TEMP', 'TMPDIR'):
        e[k] = str(SCRATCH)
    return e


def run(args):
    args = [str(a) for a in args]
    print('> ' + ' '.join(args), flush=True)
    subprocess.run(args, check=True, env=env())


def tool(name):
    return A64 / ('aarch64-none-elf-' + name + '.exe')


def compile_c(src, obj, extra=()):
    obj.parent.mkdir(parents=True, exist_ok=True)
    run([tool('gcc')] + list(extra) + CFLAGS + ['-c', src, '-o', obj])
    return obj


def c_string(text):
    out = []
    for line in text.splitlines():
        out.append('"' + line.replace('\\', '\\\\').replace('"', '\\"') + '\\n"')
    return '\n'.join(out)


def shaders():
    """gen/shader_src.h for the GL path, and romfs/shaders/{vs,fs}<p>.dksh by uam for deko3d, from vs.glsl and fs.glsl."""
    vs, fs = (HERE / 'vs.glsl').read_text(), (HERE / 'fs.glsl').read_text()
    gen = OUT / 'gen'
    gen.mkdir(parents=True, exist_ok=True)
    (gen / 'shader_src.h').write_text('/* made by build.py from vs.glsl and fs.glsl */\n'
                                      'static const char dc_vs_src[] =\n' + c_string(vs) + ';\n'
                                      'static const char dc_fs_src[] =\n' + c_string(fs) + ';\n')
    glsl, romfs = OUT / 'glsl', OUT / 'romfs' / 'shaders'
    glsl.mkdir(parents=True, exist_ok=True)
    romfs.mkdir(parents=True, exist_ok=True)
    for p in range(4):
        for name, stage, body in (('vs', 'vert', vs), ('fs', 'frag', fs)):
            g = glsl / f'{name}{p}.glsl'
            g.write_text(f'#version 460\n#define PROG {p}\n' + body)
            run([TOOLS / 'uam.exe', '-s', stage, '-o', romfs / f'{name}{p}.dksh', g])


def link(name, title, objs, libs, lib_dirs, romfs=None):
    elf, nacp, nro = OUT / (name + '.elf'), OUT / (name + '.nacp'), OUT / (name + '.nro')
    run([tool('g++'), '-specs=' + str(LIBNX / 'switch.specs'), '-g'] + ARCH + ['-Wl,-Map,' + str(OUT / (name + '.map'))]
        + objs + WRAP + ['-L' + str(d) for d in lib_dirs] + libs + ['-o', elf])
    run([TOOLS / 'nacptool.exe', '--create', title, AUTHOR, VERSION, nacp])
    args = [TOOLS / 'elf2nro.exe', elf, nro, '--nacp=' + str(nacp), '--icon=' + str(LIBNX / 'default_icon.jpg')]
    if romfs:
        args.append('--romfsdir=' + str(romfs))
    run(args)
    print('built', nro, f'({nro.stat().st_size / 1e6:.1f} MB)', flush=True)
    return nro


def main():
    which = set(sys.argv[1:]) or {'gl', 'dk'}
    SCRATCH.mkdir(parents=True, exist_ok=True)
    if 'clean' in which:
        shutil.rmtree(OUT, ignore_errors=True)
        which.discard('clean')
        if not which:
            return
    OUT.mkdir(parents=True, exist_ok=True)
    shaders()
    common = compile_c(HERE / 'common.c', OUT / 'obj' / 'common.o')
    if 'gl' in which:
        if not (MESA / 'lib' / 'libEGL.a').exists():
            raise SystemExit(f'no Mesa 26 at {MESA} (tools/build_switch.py mesa downloads it; XI_SWITCH_MESA another)')
        o = compile_c(HERE / 'gl_main.c', OUT / 'obj' / 'gl_main.o', ['-I' + str(MESA / 'include')])
        link('drawcost-gl', 'drawcost GL', [common, o], GL_LIBS + ['-lnx', '-lm'],
             [MESA / 'lib', PORTLIBS / 'lib', LIBNX / 'lib'])
    if 'dk' in which:
        o = compile_c(HERE / 'dk_main.c', OUT / 'obj' / 'dk_main.o')
        link('drawcost-dk', 'drawcost deko3d', [common, o], ['-ldeko3d', '-lnx', '-lm'],
             [PORTLIBS / 'lib', LIBNX / 'lib'], romfs=OUT / 'romfs')


if __name__ == '__main__':
    main()
