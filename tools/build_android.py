#!/usr/bin/env python3
"""Build the Android host (libmain.so) and its device-side tests with the NDK, for ARM64.

  python3 tools/build_android.py deps                 the third-party libraries
  python3 tools/build_android.py host [--native-tls]  libmain.so and build-config.json, from the
        game prepared by tools/build_posix.py prepare
  python3 tools/build_android.py gfxtest --test <name> [--native-tls]   one offscreen GPU fixture
        (tests/<name>_test.c), without the game
  python3 tools/build_android.py tls-test [--native-tls]   the ELF TLS check (platforms/android/tests/android_tls_test.c)
  python3 tools/build_android.py install --device SERIAL [--keystore file.p12 --key-alias xi]
        everything the app needs, from the dependencies (android_deps.py, once) to the API 29 host and
        its APK (build_android_apk.py, signed with --keystore, its password in
        XI_ANDROID_KEYSTORE_PASS), installed over adb on the device SERIAL (`adb devices`) as an
        update that keeps the app's files. The launcher's phone page runs this.

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
import tempfile
from pathlib import Path

# tools/ itself: the launcher's Python on Windows (the embeddable one, with a ._pth) does not put the
# script's folder on sys.path
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import android_translation  # noqa: E402
from android_toolchain import CACHE, MAKE, ROOT, SDK, SDL, TC  # noqa: E402
import build  # noqa: E402
import build_posix  # noqa: E402

MIN_API = 28
WINDOWS = sys.platform == 'win32'


def clang(api):
    """The NDK's compiler driver for API `api` (a .cmd wrapper on Windows)."""
    return str(TC / f'aarch64-linux-android{api}-clang') + ('.cmd' if WINDOWS else '')


CC = clang(MIN_API)
CXX = CC.replace('-clang.cmd', '-clang++.cmd') if WINDOWS else CC + '++'
AR = str(TC / 'llvm-ar') + ('.exe' if WINDOWS else '')
STRIP = str(TC / 'llvm-strip') + ('.exe' if WINDOWS else '')
ADB = SDK / 'platform-tools' / ('adb.exe' if WINDOWS else 'adb')
OUT = Path(os.environ.get('XI_ANDROID_BUILD_DIR', str(ROOT / 'build/android'))).resolve()
FLAGS = [
    '-O2',
    '-g',
    '-fPIC',
    '-ffixed-x18',
    '-fno-strict-aliasing',
    '-DRT_GUEST_WINDOW',
    '-DFFXI_ANDROID_VULKAN',
    '-DFFXI_MOBILE',
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
    print('> ' + ' '.join(map(str, args))[:200], flush=True)
    args = list(map(str, args))
    # the NDK's drivers are .cmd wrappers on Windows, and cmd.exe takes at most 8191 characters: a
    # long command (libmain.so's link) passes its arguments to clang in a response file instead
    if WINDOWS and args[0].endswith('.cmd') and len(subprocess.list2cmdline(args)) > 7000:
        rsp = OUT / (Path(args[0]).stem + '.rsp')
        rsp.parent.mkdir(parents=True, exist_ok=True)
        rsp.write_text('\n'.join('"' + x.replace('\\', '/') + '"' for x in args[1:]) + '\n')
        args = [args[0], '@' + str(rsp)]
    subprocess.run(args, cwd=ROOT, check=True, **kw)


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
    print(f'{group}: compiling {len(jobs)} files (the ones changed since the last build)', flush=True)
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


def luajit_make(work, tc, cc):
    """LuaJIT's own makefile, cross-compiling with the NDK's compilers in tc (cc: the API's clang)."""
    run(
        [
            MAKE if MAKE.exists() else 'make',
            '-C',
            work / 'src',
            '-j6',
            'BUILDMODE=static',
            'TARGET_SYS=Linux',
            'HOST_CC=' + os.environ.get('XI_HOST_CC', 'clang'),
            'CROSS=' + str(tc) + '/',
            'CC=' + cc,
            'STATIC_CC=' + cc,
            'DYNAMIC_CC=' + cc + ' -fPIC',
            'TARGET_LD=' + cc,
            'TARGET_AR=' + str(tc / Path(AR).name) + ' rcus',
            'TARGET_STRIP=' + str(tc / Path(STRIP).name),
            'XCFLAGS=-DLUAJIT_ENABLE_LUA52COMPAT -fPIC',
            'libluajit.a',
        ]
    )


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
        # make splits its variables at spaces: an NDK under one (the launcher's, in macOS's
        # "Application Support") is reached through a link without any, for this build
        tc, link = TC, None
        if ' ' in str(TC) and not WINDOWS:
            link = Path(tempfile.mkdtemp(prefix='xi-ndk-'))
            (link / 'bin').symlink_to(TC, target_is_directory=True)
            tc = link / 'bin'
        cc = str(tc / Path(CC).name)
        try:
            luajit_make(work, tc, cc)
        finally:
            if link:
                shutil.rmtree(link)
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


def game_kit(game_objs, libmain, configuration):
    """--split-game: what the phone needs to make libgame.so itself, none of it game code.

    <OUT>/kit: the compiler's side (cflags.txt, the runtime's and C library's headers the translation
    includes, compiler-rt's builtins, kit.id). <OUT>/phone: the translator's side (recomp/, unpack.py,
    phone_kit.py, and phone-kit.json with this build's metadata files beside it). Both go into the APK
    as assets; on the phone, phone_kit.py translates the player's own DLLs into kit/src and
    BuildActivity compiles it. <OUT>/libgame.so is linked here from the same translation, to check
    that the two libraries resolve each other. The headers are the ones the objects' depfiles name."""
    kit = OUT / 'kit'
    shutil.rmtree(kit, ignore_errors=True)
    sysroot = TC.parent / 'sysroot'
    resource = Path(subprocess.run([CC, '-print-resource-dir'], capture_output=True, text=True, check=True).stdout.strip())
    headers = set()
    for part, objs in game_objs.items():
        for o in objs:
            deps = shlex.split(o.with_suffix('.d').read_text().replace('\\\n', ' ').split(':', 1)[-1])
            src = Path(deps[0]) if Path(deps[0]).is_absolute() else ROOT / deps[0]
            for d in deps[1:]:
                headers.add((Path(d) if Path(d).is_absolute() else ROOT / d).resolve())
        # the depfiles (-MMD) leave out system headers: one full list (-M) for the part, whose files all
        # include the same ones
        full = OUT / 'obj' / (part + '-system.d')
        run([CC] + FLAGS + WARN + ['-std=c11', '-I' + str(src.parent), '-M', '-MF', full, '-E', '-o', os.devnull, src])
        for d in shlex.split(full.read_text().replace('\\\n', ' ').split(':', 1)[-1])[1:]:
            headers.add((Path(d) if Path(d).is_absolute() else ROOT / d).resolve())
    for h in sorted(headers):
        if h.is_relative_to(sysroot.resolve()):
            dest = kit / 'sysroot' / h.relative_to(sysroot.resolve())
        elif h.is_relative_to(resource.resolve()):
            dest = kit / 'resource' / h.relative_to(resource.resolve())
        elif h.parent.name in game_objs:  # a part's own funcs.h: the phone's translation makes it
            continue
        else:
            dest = kit / 'runtime' / h.name
            if dest.exists() and dest.read_bytes() != h.read_bytes():
                raise RuntimeError('two runtime headers named ' + h.name)
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(h, dest)
    builtins = resource / 'lib/linux/libclang_rt.builtins-aarch64-android.a'
    (kit / 'lib').mkdir()
    shutil.copyfile(builtins, kit / 'lib' / builtins.name)
    flags = (
        [f'--target=aarch64-linux-android{MIN_API}', '--sysroot=@KIT@/sysroot', '-resource-dir', '@KIT@/resource']
        + [f for f in FLAGS if not f.startswith('-I') and f != '-g']
        + WARN
        + ['-std=c11', '-I@KIT@/runtime', '-I.']
    )
    (kit / 'cflags.txt').write_text('\n'.join(flags) + '\n')

    phone = OUT / 'phone'
    shutil.rmtree(phone, ignore_errors=True)
    (phone / 'recomp').mkdir(parents=True)
    (phone / 'tools').mkdir()
    for f in ('recomp/recomp.py', 'recomp/x86c.py', 'tools/unpack.py', 'tools/phone_kit.py'):
        shutil.copyfile(ROOT / f, phone / f)
    config = {
        'build': configuration['build'],
        'ffximain_sha': configuration['ffximain_sha'],
        'ffxi_sha': configuration['ffxi_sha'],
        'ffximain_meta': Path(configuration['ffximain_meta']).name,
        'ffxi_meta': Path(configuration['ffxi_meta']).name,
        'wraps': configuration['wraps'],
        'hooks': configuration['hooks'],
        'patches': configuration['patches'],
    }
    for key in ('ffximain_meta', 'ffxi_meta'):
        shutil.copyfile(configuration[key], phone / config[key])
    (phone / 'phone-kit.json').write_text(json.dumps(config, indent=1, sort_keys=True) + '\n')

    python_runtime()
    # what the phone has, before the translation: a new one (another APK) translates and builds again
    digest = hashlib.sha256()
    for base in (kit, phone, OUT / 'pyhome'):
        for f in sorted(base.rglob('*')):
            if f.is_file():
                digest.update(str(f.relative_to(OUT)).encode() + b'\0' + f.read_bytes())
    (kit / 'kit.id').write_text(digest.hexdigest()[:16] + '\n')
    all_objs = [o for objs in game_objs.values() for o in objs]
    run(
        [CC, '-shared', '-Wl,--no-undefined', '-Wl,-soname,libgame.so', '-Wl,-z,max-page-size=16384', '-o', OUT / 'libgame.so']
        + all_objs
        + [libmain, '-lm']
    )
    llvm = ROOT / 'build/android-llvm/android/bin/llvm'
    if llvm.is_file():  # the phone's compiler (LLVM's multi-call driver, cross-built for Android)
        shutil.copyfile(llvm, OUT / 'lib_llvm.so')
        run([STRIP, '--strip-all', OUT / 'lib_llvm.so'])
    print('kit', kit, (kit / 'kit.id').read_text().strip())


# The standard library goes in whole but for these (tests, the GUI and docs): tracing what the scripts
# import on a Mac misses Android's own paths (uuid imports platform there).
PY_SKIP = ('__pycache__', 'test', 'tests', 'idle_test', 'idlelib', 'tkinter', 'turtledemo', 'ensurepip',
           'pydoc_data', 'config-3.14*', 'site-packages', 'lib-dynload', 'venv', '_pyrepl')
# Extension modules that need only what every Android has (the rest need OpenSSL or SQLite: hashlib
# falls back to its own SHA-256)
PY_SYSTEM_LIBS = {'libc.so', 'libm.so', 'libdl.so', 'liblog.so', 'libz.so', 'libpython3.14.so'}


def python_runtime():
    """The phone's Python, for phone_kit.py: CPython for Android (build/android-python, made by
    CPython's own Android/android.py) as lib_python.so (the interpreter) and libpython3.14.so in the
    APK's libraries, and <OUT>/pyhome (asset): the standard library phone_kit.py needs, its extension
    modules, pefile, and capstone's Python binding with libcapstone.so built for Android (x86 only)."""
    prefix = ROOT / 'build/android-python/src/Python-3.14.7/cross-build/aarch64-linux-android/prefix'
    capstone_so = ROOT / 'build/android-python/capstone/build/libcapstone.so'
    if not (prefix / 'lib/libpython3.14.so').is_file() or not capstone_so.is_file():
        print('no Android Python (build/android-python): the APK will not translate on the phone')
        for stale in ('pyhome', 'lib_python.so', 'libpython3.14.so'):
            shutil.rmtree(OUT / stale, ignore_errors=True) if stale == 'pyhome' else (OUT / stale).unlink(missing_ok=True)
        return
    import capstone as host_capstone  # the same binding the Mac's translation uses
    import pefile as host_pefile

    stdlib = prefix / 'lib/python3.14'
    home = OUT / 'pyhome'
    shutil.rmtree(home, ignore_errors=True)
    lib = home / 'lib/python3.14'
    ignore = shutil.ignore_patterns('__pycache__', 'test', 'tests', 'idle_test')
    shutil.copytree(stdlib, lib, ignore=shutil.ignore_patterns(*PY_SKIP))
    (lib / 'lib-dynload').mkdir()
    readelf = TC / ('llvm-readelf' + ('.exe' if WINDOWS else ''))
    for so in sorted((stdlib / 'lib-dynload').glob('*.so')):
        if so.name.startswith(('_test', 'xx', '_ctypes_test')):
            continue
        dynamic = subprocess.run([readelf, '-d', so], capture_output=True, text=True, check=True).stdout
        needed = {line.split('[')[1].split(']')[0] for line in dynamic.splitlines() if '(NEEDED)' in line}
        if needed <= PY_SYSTEM_LIBS:
            shutil.copyfile(so, lib / 'lib-dynload' / so.name)
            run([STRIP, '--strip-unneeded', lib / 'lib-dynload' / so.name])
    site = home / 'site-packages'
    site.mkdir()
    shutil.copyfile(host_pefile.__file__, site / 'pefile.py')
    ordlookup = Path(host_pefile.__file__).parent / 'ordlookup'
    shutil.copytree(ordlookup, site / 'ordlookup', ignore=ignore)
    binding = Path(host_capstone.__file__).parent
    shutil.copytree(binding, site / 'capstone', ignore=shutil.ignore_patterns('__pycache__', 'lib', 'include'))
    (site / 'capstone/lib').mkdir()
    shutil.copyfile(capstone_so, site / 'capstone/lib/libcapstone.so')
    run([STRIP, '--strip-unneeded', site / 'capstone/lib/libcapstone.so'])
    shutil.copyfile(prefix / 'bin/python3.14', OUT / 'lib_python.so')
    shutil.copyfile(prefix / 'lib/libpython3.14.so', OUT / 'libpython3.14.so')
    run([STRIP, '--strip-unneeded', OUT / 'libpython3.14.so'])


def main():
    global CC, CXX, MIN_API
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('target', choices=['deps', 'gfxtest', 'host', 'tls-test', 'install'])
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
    ap.add_argument('--device', help='install: the adb serial of the device')
    ap.add_argument('--keystore', help='install: the key to sign with (build_android_apk.py --keystore)')
    ap.add_argument('--key-alias', default='xi', help="install: the key's alias")
    ap.add_argument(
        '--split-game',
        action='store_true',
        help='host: libmain.so without the game, and the kit the phone builds libgame.so from (BuildActivity)',
    )
    a = ap.parse_args()
    if a.target == 'install':
        if not a.device:
            ap.error('install needs --device (a serial from adb devices)')
        a.native_tls = True  # API 29 (Android 10) and its ELF TLS, as the docs build
        # the dependencies once (SDL3, glslang): android_deps.py's outputs are what the host links
        if not (CACHE / 'sdl-build/libSDL3.so').exists() or not (CACHE / 'spirv-build.json').exists():
            run([sys.executable, ROOT / 'tools/android_deps.py'])
    if a.native_tls:
        MIN_API = 29
        CC = clang(MIN_API)
        CXX = CC.replace('-clang.cmd', '-clang++.cmd') if WINDOWS else CC + '++'
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / 'generated').mkdir(exist_ok=True)
    if a.target == 'tls-test':
        source = ['platforms/android/tests/android_tls_test.c']
        module = compile_many(source, 'tls-module', ['-DXI_TLS_TEST_MODULE'])
        runner = compile_many(source, 'tls-runner', ['-DXI_TLS_TEST_DRIVER'])
        run([CC, '-shared', '-Wl,-z,max-page-size=16384', '-o', OUT / 'libtls-test.so'] + module)
        run([CC, '-Wl,-z,max-page-size=16384', '-o', OUT / 'tls-test'] + runner + ['-ldl'])
        return
    libs = []
    extra = ['-I' + str(ROOT / 'host/addons'), '-DIMGUI_USER_CONFIG="imconfig_xi.h"']
    if a.target in ('deps', 'host', 'install'):
        for n in ['mbedtls', 'luajit', 'imgui', 'luasocket', 'lfs', 'sqlite']:
            lib, inc = library(n)
            libs.append(lib)
            extra += inc
    if a.target == 'deps':
        return
    spv = json.loads((CACHE / 'spirv-build.json').read_text())
    extra += spv['cflags']
    gfx = [
        'runtime/portable/gfx/gfx_hlsl.c',
        'runtime/portable/gfx/gfx_hlsl_shaders.c',
        'runtime/portable/gfx/gfx_spirv.c',
        'runtime/portable/gfx/gfx_vulkan.cpp',
        'runtime/portable/gfx/gfx_worker.cpp',
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
            gfx + ['tests/gfx/' + a.test + '_test.' + ('cpp' if a.test == 'gfx_visibility_storage' else 'c')],
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
    objs, game_objs = [], {}
    for part in ['android-all', 'ffxi']:
        sources = sorted(str(x.relative_to(ROOT)) for x in (ROOT / 'generated' / part).glob('*.c'))
        if not sources:
            raise RuntimeError('missing translation: generated/' + part)
        game_objs[part] = compile_many(sources, part, ['-I' + str(ROOT / 'generated' / part)])
        if not a.split_game:
            objs += game_objs[part]
    # the PlayOnline client, when XI_POL names it (build_posix.pol); the sign-in screen offers it
    pol_sources, pol_flags, pol_libs = build_posix.pol('aarch64-linux-android', str(OUT))
    if pol_sources:
        objs += compile_many(pol_sources, 'pol', pol_flags + build_posix.POL_CFLAGS)
    if pol_libs:  # the client's symbols stay out of libmain.so's exports, built here or prebuilt
        link = pol_libs + ['-Wl,--exclude-libs,' + os.path.basename(a) for a in pol_libs] + link
    objs += compile_many(
        PORTABLE
        + HOST
        + ADDONS
        + gfx
        + ['platforms/android/android_main.c', 'platforms/android/android_overlay.c', 'host/touch.c', 'host/display_fit.c', 'host/benchmark.c'],
        'host',
        extra + pol_flags + ['-Dmain=xi_host_main'],
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
        objs += compile_many(['tests/gfx/' + source], 'host-' + entry, extra + ['-Dmain=' + entry])
    library_path = OUT / 'libmain.so'
    run(
        [
            CXX,
            '-shared',
        ]
        # split: the game's symbols come from libgame.so, which loads this library as its dependency
        + ([] if a.split_game else ['-Wl,--no-undefined'])
        + [
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
    if pol_libs:  # the shipped library names none of the PlayOnline client's functions (build_posix.POL_CFLAGS)
        shutil.copyfile(library_path, OUT / 'libmain.debug.so')
        run([STRIP, '--strip-all', library_path])
    metadata = {
        'schema': 1,
        'minimum_api': MIN_API,
        'native_tls': MIN_API >= 29,
        'shared_geometry': True,
        'compiler': CC,
        'libmain_sha256': hashlib.sha256(library_path.read_bytes()).hexdigest(),
    }
    if a.split_game:
        metadata['split_game'] = True
        game_kit(game_objs, library_path, configuration)
    else:
        for stale in ('lib_llvm.so', 'libgame.so'):
            (OUT / stale).unlink(missing_ok=True)
    (OUT / 'build-config.json').write_text(json.dumps(metadata, indent=2) + '\n')
    print('built', library_path)
    if a.target == 'install':
        package = [sys.executable, ROOT / 'tools/build_android_apk.py', '--build-dir', OUT]
        if a.keystore:
            package += ['--keystore', a.keystore, '--key-alias', a.key_alias]
        run(package)
        # -r: an update, the app's files kept; -d: an older game build too (the app is debuggable)
        run([ADB, '-s', a.device, 'install', '-r', '-d', OUT / 'xi-native-test.apk'])


if __name__ == '__main__':
    try:
        main()
    except subprocess.CalledProcessError as e:
        # the step's own error is just above; a traceback here would push it out of the launcher's view
        print('failed: ' + ' '.join(map(str, e.cmd))[:300], file=sys.stderr)
        raise SystemExit(1)
