#!/usr/bin/env python3
"""Build the Android host (libmain.so) and its device-side tests with the NDK, for ARM64.

  python3 tools/build_android.py deps                 the third-party libraries
  python3 tools/build_android.py host [--native-tls]  libmain.so and build-config.json, from the
        game prepared by tools/build_posix.py prepare
  python3 tools/build_android.py gfxtest --test <name> [--native-tls]   one offscreen GPU fixture
        (tests/<name>_test.c), without the game
  python3 tools/build_android.py tls-test [--native-tls]   the ELF TLS check (tests/android_tls_test.c)

FFXiMain.dll is translated into generated/android-all (tools/android_translation.py); FFXi.dll and
the addons are rebuilt from the same prepared files. Desktop outputs are not touched. Run
tools/android_deps.py first. Outputs go to build/android (XI_ANDROID_BUILD_DIR). API 28 uses emulated
TLS; --native-tls targets API 29 and its ELF TLS.
"""

import argparse
import concurrent.futures
import hashlib
import json
import os
import shlex
import shutil
import subprocess
import sys
from pathlib import Path

import android_translation
from android_toolchain import CACHE, ROOT, SDL, TC
import build
import build_posix

MIN_API = 28
CC = str(TC / f'aarch64-linux-android{MIN_API}-clang')
CXX = CC + '++'
AR = str(TC / 'llvm-ar')
OUT = Path(os.environ.get('XI_ANDROID_BUILD_DIR', str(ROOT / 'build/android'))).resolve()
FLAGS = [
    '-O2',
    '-g',
    '-fPIC',
    '-ffixed-x18',
    '-fno-strict-aliasing',
    '-DRT_GUEST_WINDOW',
    '-DFFXI_ANDROID_VULKAN',
    '-I' + str(ROOT / 'runtime'),
    '-I' + str(ROOT / 'runtime/portable'),
    '-I' + str(OUT / 'generated'),
    '-I' + str(ROOT / 'generated'),
    '-I' + str(ROOT / 'third_party/stb'),
    '-I' + str(SDL / 'include'),
]
# the desktop builders' source lists; Android has no Windows sampler, and links its frame collector below
PORTABLE = build_posix.PORTABLE
HOST = [build_posix.posix(s) for s in build.HOST_BASE if s != 'runtime\\portable\\sampler_win.c']
ADDONS = build_posix.ADDON_SOURCES
WARN = build_posix.GEN_WARNINGS


def run(args, **kw):
    subprocess.run(list(map(str, args)), cwd=ROOT, check=True, **kw)


def compile_one(job):
    src, obj, extra = job
    s = Path(src)
    s = s if s.is_absolute() else ROOT / s
    obj.parent.mkdir(parents=True, exist_ok=True)
    flags = FLAGS + list(extra) + (['-std=c++17'] if s.suffix == '.cpp' else ['-std=c11'])
    if str(src).startswith('generated/'):
        flags += WARN
    compiler = CXX if s.suffix == '.cpp' else CC
    # the compiler and flags are in the key: API 28 emutls and API 29 ELF TLS objects never mix
    key = hashlib.sha256(s.read_bytes() + json.dumps([compiler, flags]).encode()).hexdigest()
    stamp = obj.with_suffix('.stamp')
    # a missing or newer dependency rebuilds it too; depfiles escape spaces and may use paths relative to ROOT
    deps = obj.with_suffix('.d')
    stale = not obj.exists() or not stamp.exists() or not deps.exists() or stamp.read_text() != key
    if not stale:
        dependencies = shlex.split(deps.read_text().replace('\\\n', ' ').split(':', 1)[-1])
        for dependency in dependencies:
            path = Path(dependency)
            if not path.is_absolute():
                path = ROOT / path
            if not path.exists() or path.stat().st_mtime_ns > obj.stat().st_mtime_ns:
                stale = True
                break
    if stale:
        cmd = [compiler] + flags + ['-MMD', '-MF', str(deps), '-c', str(s), '-o', str(obj)]
        r = subprocess.run(cmd, cwd=ROOT, text=True, capture_output=True)
        if r.returncode:
            return str(s), r.stdout + r.stderr
        stamp.write_text(key)
    return str(obj), None


def compile_many(srcs, group, extra=()):
    jobs = [(s, OUT / 'obj' / group / (str(s).replace('/', '_').replace('\\', '_') + '.o'), extra) for s in srcs]
    errors = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=6) as ex:
        for name, err in ex.map(compile_one, jobs):
            if err:
                errors.append((name, err))
    if errors:
        for name, err in errors:
            print(name + '\n' + err[-6500:], file=sys.stderr)
        raise SystemExit(1)
    return [j[1] for j in jobs]


def library(name):
    lib = ROOT / 'third_party' / name
    m = json.loads((lib / 'manifest.json').read_text())
    archive = OUT / 'lib' / ('lib' + name + '.a')
    archive.parent.mkdir(parents=True, exist_ok=True)
    if name == 'luajit':
        # LuaJIT's makefiles do not track compiler changes, so API 29 builds, whose TLS and unwind
        # objects differ, get a source and build tree of their own
        work = OUT / ('luajit-src' if MIN_API == 28 else f'luajit-src-api{MIN_API}')
        if not work.exists():
            shutil.copytree(lib, work)
        run(
            [
                'make',
                '-C',
                work / 'src',
                '-j6',
                'BUILDMODE=static',
                'TARGET_SYS=Linux',
                'HOST_CC=clang',
                'CROSS=' + str(TC) + '/',
                'CC=' + CC,
                'STATIC_CC=' + CC,
                'DYNAMIC_CC=' + CC + ' -fPIC',
                'TARGET_LD=' + CC,
                'TARGET_AR=' + AR + ' rcus',
                'TARGET_STRIP=' + str(TC / 'llvm-strip'),
                'XCFLAGS=-DLUAJIT_ENABLE_LUA52COMPAT -fPIC',
                'libluajit.a',
            ]
        )
        shutil.copyfile(work / 'src/libluajit.a', archive)
    else:
        objs = []
        for i, g in enumerate(m['groups']):
            extra = (
                g['flags']
                + ['-I' + str(lib / d) for d in g['include']]
                + sum((['-idirafter', str(lib / d)] for d in g['idirafter']), [])
            )
            objs += compile_many([str(lib / s) for s in g['sources']], name + str(i), extra)
        run([AR, 'rcs', archive] + objs)
    return archive, ['-I' + str(lib / d) for d in m['public']]


def main():
    global CC, CXX, MIN_API
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('target', choices=['deps', 'gfxtest', 'host', 'tls-test'])
    ap.add_argument(
        '--test',
        choices=[
            'gfx',
            'gfx_format',
            'gfx_state',
            'gfx_async',
            'gfx_area',
            'gfx_pass_plan_gpu',
            'gfx_visibility_storage',
        ],
        default='gfx',
        help='Offscreen GPU fixture for gfxtest target',
    )
    ap.add_argument('--native-tls', action='store_true', help='API 29 ELF TLS (default: API 28, emulated TLS)')
    a = ap.parse_args()
    if a.native_tls:
        MIN_API = 29
        CC = str(TC / f'aarch64-linux-android{MIN_API}-clang')
        CXX = CC + '++'
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / 'generated').mkdir(exist_ok=True)
    if a.target == 'tls-test':
        source = ['tests/android_tls_test.c']
        module = compile_many(source, 'tls-module', ['-DXI_TLS_TEST_MODULE'])
        runner = compile_many(source, 'tls-runner', ['-DXI_TLS_TEST_DRIVER'])
        run([CC, '-shared', '-Wl,-z,max-page-size=16384', '-o', OUT / 'libtls-test.so'] + module)
        run([CC, '-Wl,-z,max-page-size=16384', '-o', OUT / 'tls-test'] + runner + ['-ldl'])
        return
    libs = []
    extra = ['-I' + str(ROOT / 'host/addons'), '-DIMGUI_USER_CONFIG="imconfig_xi.h"']
    if a.target in ('deps', 'host'):
        for n in ['mbedtls', 'luajit', 'imgui', 'luasocket', 'lfs', 'sqlite']:
            lib, inc = library(n)
            libs.append(lib)
            extra += inc
    if a.target == 'deps':
        return
    spv = json.loads((CACHE / 'spirv-build.json').read_text())
    extra += spv['cflags']
    gfx = [
        'runtime/portable/gfx_hlsl.c',
        'runtime/portable/gfx_hlsl_shaders.c',
        'runtime/portable/gfx_spirv.c',
        'runtime/portable/gfx_vulkan.cpp',
        'runtime/portable/gfx_worker.cpp',
    ]
    extra += ['-DFFXI_RENDER_WORKER_BUILD']
    sdl_lib = CACHE / 'sdl-build/libSDL3.so'
    link = (
        ['-Wl,--start-group']
        + libs
        + spv['libs']
        + ['-Wl,--end-group', str(sdl_lib), '-lvulkan', '-llog', '-landroid', '-lm', '-ldl']
    )
    if a.target == 'gfxtest':
        objs = compile_many(
            gfx + ['tests/' + a.test + '_test.' + ('cpp' if a.test == 'gfx_visibility_storage' else 'c')],
            'gfxtest',
            extra,
        )
        run([CXX, '-Wl,-z,max-page-size=16384', '-static-libstdc++', '-o', OUT / (a.test + '_test')] + objs + link)
        return
    configuration = android_translation.android_build(build.BUILD)
    android_translation.generate(configuration)
    build.BUILD = configuration
    build.BUILD_H = str(OUT / 'generated/build.h')
    build.write_build_h()
    run([sys.executable, ROOT / 'tools/embed_lua.py'])
    objs = []
    for part in ['android-all', 'ffxi']:
        sources = sorted(str(x.relative_to(ROOT)) for x in (ROOT / 'generated' / part).glob('*.c'))
        if not sources:
            raise RuntimeError('missing translation: generated/' + part)
        objs += compile_many(sources, part, ['-I' + str(ROOT / 'generated' / part)])
    objs += compile_many(
        PORTABLE + HOST + ADDONS + gfx + ['host/android_main.c', 'host/benchmark.c'],
        'host',
        extra + ['-Dmain=xi_host_main'],
    )
    for source, entry in [
        ('gfx_test.c', 'xi_gfx_test_main'),
        ('gfx_format_test.c', 'xi_format_test_main'),
        ('gfx_state_test.c', 'xi_state_test_main'),
        ('gfx_async_test.c', 'xi_async_test_main'),
        ('gfx_area_test.c', 'xi_area_test_main'),
        ('gfx_pass_plan_gpu_test.c', 'xi_pass_plan_test_main'),
        ('gfx_visibility_storage_test.cpp', 'xi_visibility_storage_test_main'),
    ]:
        objs += compile_many(['tests/' + source], 'host-' + entry, extra + ['-Dmain=' + entry])
    library_path = OUT / 'libmain.so'
    run(
        [
            CXX,
            '-shared',
            '-Wl,--no-undefined',
            '-Wl,-soname,libmain.so',
            '-Wl,-z,max-page-size=16384',
            '-static-libstdc++',
            '-Wl,--export-dynamic',
            '-o',
            library_path,
        ]
        + objs
        + link
    )
    metadata = {
        'schema': 1,
        'minimum_api': MIN_API,
        'native_tls': MIN_API >= 29,
        'shared_geometry': True,
        'compiler': CC,
        'libmain_sha256': hashlib.sha256(library_path.read_bytes()).hexdigest(),
    }
    (OUT / 'build-config.json').write_text(json.dumps(metadata, indent=2) + '\n')
    print('built', library_path)


if __name__ == '__main__':
    main()
