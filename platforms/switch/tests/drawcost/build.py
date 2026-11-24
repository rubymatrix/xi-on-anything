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


# (the source, its C name in gen/shader_src.h, its .dksh name, uam's stage, how many PROG variants)
SHADERS = [('vs.glsl', 'dc_vs_src', 'vs', 'vert', 4), ('fs.glsl', 'dc_fs_src', 'fs', 'frag', 4),
           ('game_vs.glsl', 'dc_game_vs_src', 'gvs', 'vert', 8), ('game_fs.glsl', 'dc_game_fs_src', 'gfs', 'frag', 8),
           ('char_vs.glsl', 'dc_char_vs_src', 'cvs', 'vert', 1), ('char_fs.glsl', 'dc_char_fs_src', 'cfs', 'frag', 1)]


def shaders():
    """gen/shader_src.h for the GL path, and romfs/shaders/<name><p>.dksh by uam for deko3d, from the .glsl files."""
    gen = OUT / 'gen'
    gen.mkdir(parents=True, exist_ok=True)
    glsl, romfs = OUT / 'glsl', OUT / 'romfs' / 'shaders'
    glsl.mkdir(parents=True, exist_ok=True)
    romfs.mkdir(parents=True, exist_ok=True)
    header = '/* made by build.py from the .glsl files */\n'
    for src, cname, name, stage, n in SHADERS:
        body = (HERE / src).read_text()
        header += f'static const char {cname}[] =\n' + c_string(body) + ';\n'
        for p in range(n):
            g = glsl / f'{name}{p}.glsl'
            text = f'#version 460\n#define PROG {p}\n' + body
            out = romfs / f'{name}{p}.dksh'
            if g.exists() and g.read_text() == text and out.exists():
                continue
            g.write_text(text)
            run([TOOLS / 'uam.exe', '-s', stage, '-o', out, g])
    (gen / 'shader_src.h').write_text(header)


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
