"""Build driver for Windows without Visual Studio: clang from llvm-mingw.

  python tools/build_mingw.py host64 [--out build\\mingw]

The same game host as tools/build.py host64 (Direct3D 12, SChannel, SDL3), compiled by llvm-mingw's
clang and linked by lld, so a player's PC needs no Visual Studio: the launcher can carry this
toolchain (it may be redistributed; MSVC may not). Everything needed is in llvm-mingw except SDL3,
which comes from its development package (SDL3_DIR, the same one build.py uses: lld links its
import library as it is).

Finds clang on PATH, or in LLVM_MINGW (the unpacked llvm-mingw folder). Objects go to build\\mingw\\obj,
apart from build.py's; host64.exe, SDL3.dll and runtime.json to --out (default build\\mingw).
Needs generated/ from tools/prepare.py, as build.py does.
"""
import argparse
import concurrent.futures
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import build  # noqa: E402  (constants and source lists; nothing runs on import)
import buildinfo  # noqa: E402

ROOT = build.ROOT
OBJ = os.path.join('build', 'mingw', 'obj')
CFLAGS = ['-O2', '-DRT_GUEST_WINDOW', '-fno-strict-aliasing', '-D_CRT_SECURE_NO_WARNINGS', '-I', 'runtime',
          '-I', 'runtime/portable', '-I', 'generated']
# the generated C: every label and local is emitted whether used or not (as build_posix.py)
GEN_WARNINGS = ['-Wno-unused-label', '-Wno-unused-variable', '-Wno-unused-but-set-variable', '-Wno-unused-function',
                '-Wno-parentheses-equality', '-Wno-unreachable-code']
LIBS = ['-lsynchronization', '-lws2_32', '-ladvapi32', '-lbcrypt', '-lsecur32', '-ld3d12', '-ldxgi', '-ld3dcompiler',
        '-ldxguid', '-lole32', '-luser32', '-lgdi32', '-lshell32', '-lwinmm']

# FFXI_PROGRESS=1 (the launcher): the compile's progress as lines of their own, as build_posix.py
PROGRESS = bool(os.environ.get('FFXI_PROGRESS'))


def posix(p):
    return p.replace('\\', '/')


def gnu(flags):
    """build.py's cl flags (/I dir, /DNAME) as clang's."""
    out = []
    for f in flags:
        if f == '/I':
            out.append('-I')
        elif posix(f) == 'third_party/luasocket/src' and out[-1:] == ['-I']:
            # after the system headers: its io.h would stand in for the CRT's, which libc++ includes
            # (build.py keeps it off lfs's path for the same reason; MSVC's STL never includes io.h)
            out[-1] = '-idirafter'
            out.append(posix(f))
        elif f.startswith('/D'):
            out.append('-D' + f[2:])
        else:
            out.append(posix(f))
    return out


def toolchain():
    """The environment with llvm-mingw's bin first on PATH."""
    env = dict(os.environ)
    root = env.get('LLVM_MINGW')
    if root:
        env['PATH'] = os.path.join(root, 'bin') + os.pathsep + env['PATH']
    if not shutil.which('clang', path=env['PATH']):
        raise SystemExit('clang not found: put llvm-mingw\'s bin on PATH, or set LLVM_MINGW to its folder')
    # LuaJIT's makefile runs the tools it builds (minilua, buildvm) from the current folder
    env.pop('NoDefaultCurrentDirectoryInExePath', None)
    return env


def which(env, name):
    return shutil.which(name, path=env['PATH'])


def run(env, cmd, cwd=ROOT):
    line = ' '.join(cmd)
    print('>', line if len(line) < 200 else line[:200] + ' ...', flush=True)
    subprocess.check_call([which(env, cmd[0]) or cmd[0]] + cmd[1:], cwd=cwd, env=env)


def stale(src, obj, headers):
    if not os.path.exists(obj):
        return True
    stamp = os.path.getmtime(src)
    if not os.path.relpath(src, ROOT).startswith('generated'):
        stamp = max(stamp, headers)
    return os.path.getmtime(obj) < stamp


def newest_header():
    newest = os.path.getmtime(build.BUILD_H)
    for d in ('runtime', 'runtime/portable', 'host', 'host/addons'):
        full = os.path.join(ROOT, d)
        for f in os.listdir(full):
            if f.endswith('.h'):
                newest = max(newest, os.path.getmtime(os.path.join(full, f)))
    return newest


def compile_stale(env, sources, objdir, extra):
    """Compiles, in parallel, every source whose object is missing or out of date; returns the objects."""
    full_dir = os.path.join(ROOT, objdir)
    os.makedirs(full_dir, exist_ok=True)
    headers = newest_header()
    clang, clangxx = which(env, 'clang'), which(env, 'clang++')
    objs, jobs = [], []
    for s in sources:
        s = posix(s)
        obj = os.path.join(objdir, os.path.splitext(os.path.basename(s))[0] + '.o')
        objs.append(obj)
        if stale(os.path.join(ROOT, s), os.path.join(ROOT, obj), headers):
            if s.endswith('.cpp'):
                cmd = [clangxx, '-c', '-std=c++17'] + CFLAGS + extra
            else:
                cmd = [clang, '-c', '-std=c11'] + CFLAGS + extra + (GEN_WARNINGS if s.startswith('generated/') else [])
            jobs.append(cmd + [s, '-o', obj])
    if jobs:
        print('compiling %d of %d' % (len(jobs), len(sources)), flush=True)
        failed = []
        with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as ex:
            results = ex.map(lambda c: subprocess.run(c, cwd=ROOT, env=env, capture_output=True, text=True), jobs)
            for n, (cmd, r) in enumerate(zip(jobs, results), 1):
                if r.returncode:
                    failed.append(cmd[-3])
                    sys.stderr.write(r.stderr[-3000:])
                if PROGRESS:
                    print('@progress %d %d' % (n, len(jobs)), flush=True)
        if failed:
            raise SystemExit('failed: ' + ', '.join(failed))
    return objs


def luajit(env):
    """LuaJIT as a static library by its own makefile (mingw32-make), in a copy of third_party/luajit,
    with Lua 5.2 compatibility as build.py's msvcbuild.bat lua52compat."""
    work = os.path.join(ROOT, 'build', 'mingw', 'luajit')
    lib = os.path.join(work, 'src', 'libluajit.a')
    src_tree = os.path.join(ROOT, 'third_party', 'luajit')
    newest = max(os.path.getmtime(os.path.join(d, f)) for d, _, fs in os.walk(src_tree) for f in fs)
    if os.path.exists(lib) and os.path.getmtime(lib) >= newest:
        return lib
    if os.path.exists(work):
        shutil.rmtree(work)
    shutil.copytree(src_tree, work)
    make = which(env, 'mingw32-make') or which(env, 'make')
    # SHELL=cmd.exe: the makefile's Windows rules are cmd's, and make would take any sh on PATH (Git's)
    run(env, [make, '-C', 'src', '-j%d' % (os.cpu_count() or 4), 'SHELL=cmd.exe', 'HOST_SYS=Windows', 'TARGET_SYS=Windows',
              'BUILDMODE=static', 'CC=clang', 'XCFLAGS=-DLUAJIT_ENABLE_LUA52COMPAT', 'libluajit.a'], cwd=work)
    return lib


def addon_objects(env, sdl_inc):
    """The addon host and the libraries it links, as build.py's addon_objects."""
    run(env, [sys.executable, 'tools/embed_lua.py'])
    names = sorted(os.listdir(build.ADDON_DIR))
    sources = ['host/addons/' + f for f in names if f.endswith('.c') or f.endswith('.cpp')] + ['generated/addons_lua.c']
    inc = gnu(build.ADDON_INCLUDES)
    objs = compile_stale(env, sources, OBJ + '/addons', inc + sdl_inc + gnu(build.HOST_INCLUDES) + ['-I', 'runtime/portable'])
    objs += compile_stale(env, build.IMGUI, OBJ + '/imgui', inc)
    objs += compile_stale(env, build.LUASOCKET, OBJ + '/luasocket', inc)
    # without LuaSocket's src/ on the path: its io.h would stand in for the CRT's (build.py)
    objs += compile_stale(env, ['third_party/lfs/lfs.c'], OBJ + '/lfs', ['-I', 'third_party/luajit/src'])
    objs += compile_stale(env, ['third_party/sqlite/sqlite3.c', 'third_party/sqlite/lsqlite3/lsqlite3.c'], OBJ + '/sqlite',
                          inc + ['-I', 'third_party/sqlite', '-DSQLITE_THREADSAFE=0', '-DSQLITE_OMIT_LOAD_EXTENSION'])
    return objs + [luajit(env)]


def host64(env, out):
    if build.BUILD is None:
        buildinfo.current()  # exits: run tools/prepare.py first
    build.write_build_h()
    build.translate()
    gen = lambda sub: ['generated/%s/%s' % (sub, f) for f in sorted(os.listdir(os.path.join(ROOT, 'generated', sub)))
                       if f.endswith('.c')]
    sdl_inc, sdl_lib = build.sdl3()
    sdl_inc = gnu(sdl_inc)
    if PROGRESS:
        print('@phase compile', flush=True)
    objs = compile_stale(env, gen('all'), OBJ + '/all64', ['-I', 'generated/all'])
    objs += compile_stale(env, gen('ffxi'), OBJ + '/ffxi64', ['-I', 'generated/ffxi'])
    objs += compile_stale(env, build.PORTABLE + build.HOST_SOURCES, OBJ + '/host64', sdl_inc + gnu(build.HOST_INCLUDES))
    objs += addon_objects(env, sdl_inc)
    res = os.path.join(OBJ, 'host64.res.o')
    run(env, ['llvm-windres', '-O', 'coff', '-o', res, 'host/host64.rc'])
    objs.append(res)
    os.makedirs(os.path.join(ROOT, out), exist_ok=True)
    exe = os.path.join(out, 'host64.exe')
    # -static: libc++ and libunwind inside the exe (the UCRT is Windows' own). The D3DX and gdifonts
    # stand-ins are __declspec(dllexport), so host64.exe exports them for addons' ffi.C, as with link.
    rsp = os.path.join(ROOT, OBJ, 'host64.rsp')
    with open(rsp, 'w') as f:
        f.write('\n'.join('"%s"' % posix(o) for o in objs))
    # -mwindows: a GUI program, so Windows opens no console beside the game; its output still reaches
    # whoever started it through inherited handles (the launcher reads it from pipes)
    run(env, ['clang++', '-static', '-mwindows', '-o', exe, '@' + rsp, posix(sdl_lib)] + LIBS)
    shutil.copy(os.path.join(build.SDL3, 'lib', 'x64', 'SDL3.dll'), os.path.join(ROOT, out))
    buildinfo.stamp(os.path.join(ROOT, out, 'runtime.json'))
    print('built %s; run: %s --game "%s" ...' % (exe, exe, build.BUILD['game']))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('target', choices=['host64'])
    ap.add_argument('--out', default=os.path.join('build', 'mingw'))
    a = ap.parse_args()
    host64(toolchain(), a.out)


if __name__ == '__main__':
    main()
