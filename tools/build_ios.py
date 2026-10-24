"""Build the iPhone host (an .app with host64 inside) with Xcode's clang, for arm64.

  python3 tools/build_ios.py app [--sdk iphonesimulator|iphoneos] [--sign-identity name]
        [--profile file.mobileprovision]
        build/ios-<sdk>/XI on Anything.app, from the game prepared by tools/build_posix.py prepare
  python3 tools/build_ios.py run [--device name] [-- host64 arguments ...]
        the simulator build installed on a simulator (booted, else the named or first iPhone) and
        started with its log on this terminal; arguments after -- go to host64 (--game, --server ...)

The same sources as build_posix.py's host64, compiled for iOS into build/ios-<sdk>/obj (the desktop
objects in build/ are not touched), with SDL3's UIKit tree (third_party/sdl3-ios, tools/vendor.py)
and host/ios_main.c as the entry. Generated game code stays in generated/ (ignored by git). A
simulator build is signed ad hoc; a device build needs a signing identity and a provisioning
profile whose app id allows the entitlements below (extended virtual addressing: the guest's 4 GB
window, guest.h). See docs/ios.md.
"""
import argparse
import concurrent.futures
import hashlib
import json
import os
import plistlib
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import build  # noqa: E402
import build_posix  # noqa: E402
import buildinfo  # noqa: E402

ROOT = build.ROOT
MIN_IOS = '17.0'
APP_NAME = 'XI on Anything'
APP_ID = 'com.rubymatrix.xionanything'
LIBS = ('sdl3-ios', 'mbedtls', 'luajit', 'imgui', 'luasocket', 'lfs', 'sqlite')
FRAMEWORKS = ['Metal', 'QuartzCore', 'Foundation', 'Security']  # Security: the sign-in's saved passwords
ENTITLEMENTS = {
    'com.apple.developer.kernel.extended-virtual-addressing': True,
    'com.apple.developer.kernel.increased-memory-limit': True,
}


def run(cmd, **kw):
    print('> ' + ' '.join(map(str, cmd))[:200], flush=True)
    subprocess.run(list(map(str, cmd)), cwd=ROOT, check=True, **kw)


def xcrun(sdk, *what):
    return subprocess.run(['xcrun', '--sdk', sdk] + list(what), capture_output=True, text=True,
                          check=True).stdout.strip()


class Target:
    """One SDK's compilers and flags, and its output folder."""

    def __init__(self, sdk):
        self.sdk = sdk
        self.out = os.path.join(ROOT, 'build', 'ios-' + sdk)
        triple = 'arm64-apple-ios' + MIN_IOS + ('-simulator' if sdk == 'iphonesimulator' else '')
        self.arch = ['-target', triple, '-isysroot', xcrun(sdk, '--show-sdk-path')]
        self.cc = [xcrun(sdk, '-f', 'clang')] + self.arch
        self.cxx = [xcrun(sdk, '-f', 'clang++')] + self.arch
        self.ar = xcrun(sdk, '-f', 'ar')

    def compile(self, sources, group, extra, generated=False):
        """Every source whose object is missing or was built from other bytes or flags (a hash per
        object), in parallel; returns the objects."""
        objdir = os.path.join(self.out, 'obj', group)
        os.makedirs(objdir, exist_ok=True)
        headers = build_posix.newest_header()
        jobs, objs = [], []
        for s in sources:
            obj = os.path.join(objdir, s.replace('/', '_') + '.o')
            objs.append(obj)
            flags = list(extra)
            cc = self.cc
            if s.endswith('.m'):  # ours by hand (gfx_metal.m) unless the group says ARC (SDL's)
                flags = [f for f in flags if f != '-std=c11'] + ([] if '-fobjc-arc' in flags else ['-fno-objc-arc'])
            elif s.endswith('.cpp'):
                flags = [f for f in flags if f != '-std=c11'] + ['-std=c++17']
                cc = self.cxx
            cmd = cc + ['-c'] + flags + [s, '-o', obj]
            src = os.path.join(ROOT, s)
            key = hashlib.sha256(json.dumps(cmd).encode()).hexdigest()
            stamp = obj + '.key'
            fresh = (os.path.exists(obj) and os.path.exists(stamp) and open(stamp).read() == key
                     and os.path.getmtime(obj) >= os.path.getmtime(src)
                     and (generated or os.path.getmtime(obj) >= headers))
            if not fresh:
                jobs.append((cmd, stamp, key))
        if jobs:
            print('%s: compiling %d of %d' % (group, len(jobs), len(sources)), flush=True)
            failed = []

            def one(job):
                cmd, stamp, key = job
                r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
                if r.returncode:
                    return cmd[-3], r.stderr
                with open(stamp, 'w') as f:
                    f.write(key)
                return None
            with concurrent.futures.ThreadPoolExecutor(os.cpu_count() or 4) as ex:
                for err in ex.map(one, jobs):
                    if err:
                        failed.append(err)
            for name, text in failed[:12]:
                print('%s:\n%s' % (name, text[-4000:]), file=sys.stderr)
            if failed:
                raise SystemExit('%s: %d failed' % (group, len(failed)))
        return objs

    def library(self, name):
        """third_party/<name> as an archive for this SDK: (archive, include flags, frameworks)."""
        lib = os.path.join(ROOT, 'third_party', name)
        with open(os.path.join(lib, 'manifest.json')) as f:
            m = json.load(f)
        archive = os.path.join(self.out, 'lib', 'lib%s.a' % name)
        os.makedirs(os.path.dirname(archive), exist_ok=True)
        if m.get('kind') == 'make':
            self.luajit(lib, archive)
        else:
            objs = []
            for n, g in enumerate(m['groups']):
                extra = (['-O2', '-DNDEBUG', '-w'] + g['flags']
                         + ['-I' + os.path.join(lib, d) for d in g['include']]
                         + sum((['-idirafter', os.path.join(lib, d)] for d in g['idirafter']), []))
                arc = [s for s in g['sources'] if s.endswith('.m')]
                rest = [s for s in g['sources'] if not s.endswith('.m')]
                rel = os.path.relpath(lib, ROOT)
                objs += self.compile([os.path.join(rel, s) for s in rest], '%s-%d' % (name, n), extra, True)
                # SDL's Objective-C counts references with ARC (ours, gfx_metal.m, by hand)
                objs += self.compile([os.path.join(rel, s) for s in arc], '%s-%d-arc' % (name, n),
                                     extra + ['-fobjc-arc'], True)
            if os.path.exists(archive):
                os.remove(archive)
            subprocess.run([self.ar, 'rcs', archive] + objs, check=True)
        frameworks = (sum((['-framework', f] for f in m.get('frameworks', [])), [])
                      + sum((['-weak_framework', f] for f in m.get('weak_frameworks', [])), []))
        return archive, ['-I' + os.path.join(lib, d) for d in m['public']], frameworks

    def luajit(self, lib, archive):
        """LuaJIT's makefile, cross: its host tools (minilua, buildvm) for this Mac, the library for
        iOS. iOS forbids writable code, so LuaJIT builds as its interpreter (LJ_OS_NOJIT)."""
        work = os.path.join(self.out, 'luajit-src')
        stamp = archive + '.stamp'
        key = build_posix.thirdparty.tree_stamp('luajit') + ' '.join(self.cc)
        if os.path.exists(archive) and os.path.exists(stamp) and open(stamp).read() == key:
            return
        shutil.rmtree(work, ignore_errors=True)
        shutil.copytree(lib, work)
        cc = self.cc[0]
        run(['make', '-C', os.path.join(work, 'src'), '-j%d' % (os.cpu_count() or 4), 'BUILDMODE=static',
             'HOST_CC=clang', 'CROSS=' + os.path.dirname(cc) + '/', 'CC=clang',
             'TARGET_FLAGS=' + ' '.join(self.arch), 'TARGET_SYS=iOS',
             'XCFLAGS=-DLUAJIT_ENABLE_LUA52COMPAT', 'libluajit.a'])
        shutil.copyfile(os.path.join(work, 'src', 'libluajit.a'), archive)
        with open(stamp, 'w') as f:
            f.write(key)


def host(t):
    """host64 for iOS: build/ios-<sdk>/host64."""
    if build.BUILD is None:
        raise SystemExit('run: python3 tools/build_posix.py prepare --game <folder>')
    build_posix.translate()
    run([sys.executable, 'recomp/recomp.py', '--meta', build.FFXI_META, '--image', build_posix.GEN_FFXI_IMAGE,
         '--retail', build.FFXI_RETAIL, '--module', 'ffxi', '--out', 'generated/ffxi', '--all'])
    run([sys.executable, 'tools/embed_lua.py'])
    archives, inc, frameworks = [], [], []
    for name in LIBS:
        a, i, f = t.library(name)
        archives.append(a)
        inc += i
        frameworks += [x for x in f if x not in frameworks or x in ('-framework', '-weak_framework')]
    base = ['-O2', '-std=c11', '-g', '-DRT_GUEST_WINDOW', '-DFFXI_IOS', '-fno-strict-aliasing', '-I', 'runtime',
            '-I', 'runtime/portable', '-I', 'generated', '-I', 'third_party/stb']
    objs = t.compile(build_posix.generated('all'), 'all', base + build_posix.GEN_WARNINGS + ['-I', 'generated/all'], True)
    objs += t.compile(build_posix.generated('ffxi'), 'ffxi', base + build_posix.GEN_WARNINGS + ['-I', 'generated/ffxi'], True)
    ours = base + inc + ['-I', 'host/addons', '-DIMGUI_USER_CONFIG="imconfig_xi.h"']
    sources = build_posix.PORTABLE + build_posix.HOST_SOURCES + build_posix.ADDON_SOURCES
    objs += t.compile([s for s in sources if s != 'host/host64.c'], 'host', ours)
    objs += t.compile(['host/host64.c'], 'host64', ours + ['-Dmain=xi_host_main'])
    objs += t.compile(['host/ios_main.c'], 'entry', ours)
    exe = os.path.join(t.out, 'host64')
    fw = sum((['-framework', f] for f in FRAMEWORKS), [])
    run(t.cc + ['-o', exe] + objs + archives + ['-lc++'] + frameworks + fw + ['-lm', '-lpthread',
                                                                              '-Wl,-export_dynamic'])
    return exe


def info_plist(t):
    return {
        'CFBundleExecutable': APP_NAME,
        'CFBundleIdentifier': APP_ID,
        'CFBundleName': APP_NAME,
        'CFBundleDisplayName': APP_NAME,
        'CFBundlePackageType': 'APPL',
        'CFBundleInfoDictionaryVersion': '6.0',
        'CFBundleShortVersionString': buildinfo.runtime()['version'],
        'CFBundleVersion': '1',
        'CFBundleSupportedPlatforms': ['iPhoneSimulator' if t.sdk == 'iphonesimulator' else 'iPhoneOS'],
        'MinimumOSVersion': MIN_IOS,
        'UIDeviceFamily': [1, 2],
        'LSRequiresIPhoneOS': True,
        'UIRequiresFullScreen': True,
        'UIStatusBarHidden': True,
        'UILaunchScreen': {},
        'UISupportedInterfaceOrientations': ['UIInterfaceOrientationLandscapeLeft',
                                             'UIInterfaceOrientationLandscapeRight'],
        'UISupportedInterfaceOrientations~ipad': ['UIInterfaceOrientationLandscapeLeft',
                                                  'UIInterfaceOrientationLandscapeRight'],
        # the player copies the game folder in with the Files app or Finder (docs/ios.md)
        'UIFileSharingEnabled': True,
        'LSSupportsOpeningDocumentsInPlace': True,
        'LSApplicationCategoryType': 'public.app-category.role-playing-games',
        'GCSupportsControllerUserInteraction': True,
        'GCSupportedGameControllers': [{'ProfileName': 'ExtendedGamepad'}],
        'FFXIIOS': '1',
    }


def app(t, a):
    exe = host(t)
    bundle = os.path.join(t.out, APP_NAME + '.app')
    shutil.rmtree(bundle, ignore_errors=True)
    os.makedirs(bundle)
    shutil.copy(exe, os.path.join(bundle, APP_NAME))
    shutil.copy(os.path.join(ROOT, 'ffxi.reg'), bundle)
    shutil.copytree(os.path.join(ROOT, 'assets', 'textures'), os.path.join(bundle, 'textures'))
    with open(os.path.join(bundle, 'Info.plist'), 'wb') as f:
        plistlib.dump(info_plist(t), f, fmt=plistlib.FMT_BINARY)
    with open(os.path.join(bundle, 'PkgInfo'), 'w') as f:
        f.write('APPL????')
    if t.sdk == 'iphonesimulator':
        run(['codesign', '--force', '--sign', '-', '--timestamp=none', bundle])
    else:
        if not a.sign_identity or not a.profile:
            raise SystemExit('a device build needs --sign-identity and --profile (docs/ios.md)')
        shutil.copy(a.profile, os.path.join(bundle, 'embedded.mobileprovision'))
        prof = plistlib.loads(subprocess.run(['security', 'cms', '-D', '-i', a.profile], capture_output=True,
                                             check=True).stdout)
        ent = dict(ENTITLEMENTS)
        allowed = prof.get('Entitlements', {})
        ent['application-identifier'] = allowed['application-identifier']
        ent['com.apple.developer.team-identifier'] = allowed['com.apple.developer.team-identifier']
        if allowed.get('get-task-allow'):
            ent['get-task-allow'] = True
        ent = {k: v for k, v in ent.items() if k in allowed}  # what the profile does not grant fails install
        path = os.path.join(t.out, 'entitlements.plist')
        with open(path, 'wb') as f:
            plistlib.dump(ent, f)
        run(['codesign', '--force', '--sign', a.sign_identity, '--entitlements', path, bundle])
    print('built %s' % bundle)
    return bundle


def simulator(name):
    """The UDID of a booted simulator, else of the named (or first available) iPhone, booted."""
    devs = json.loads(subprocess.run(['xcrun', 'simctl', 'list', 'devices', 'available', '-j'],
                                     capture_output=True, text=True, check=True).stdout)['devices']
    phones = [d for rt, ds in devs.items() if 'iOS' in rt for d in ds]
    booted = [d for d in phones if d['state'] == 'Booted' and (not name or d['name'] == name)]
    if booted:
        return booted[0]['udid']
    pick = [d for d in phones if (d['name'] == name if name else d['name'].startswith('iPhone'))]
    if not pick:
        raise SystemExit('no simulator %s' % (name or 'iPhone'))
    run(['xcrun', 'simctl', 'boot', pick[-1]['udid']])
    return pick[-1]['udid']


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('target', choices=['app', 'run'])
    ap.add_argument('--sdk', choices=['iphonesimulator', 'iphoneos'], default='iphonesimulator')
    ap.add_argument('--sign-identity')
    ap.add_argument('--profile')
    ap.add_argument('--device', help='simulator name for run')
    argv = sys.argv[1:]
    host_args = argv[argv.index('--') + 1:] if '--' in argv else []
    a = ap.parse_args(argv[:argv.index('--')] if '--' in argv else argv)
    if a.target == 'run':
        a.sdk = 'iphonesimulator'
    t = Target(a.sdk)
    bundle = app(t, a)
    if a.target == 'run':
        udid = simulator(a.device)
        run(['xcrun', 'simctl', 'install', udid, bundle])
        run(['xcrun', 'simctl', 'launch', '--console-pty', '--terminate-running-process', udid, APP_ID] + host_args)


if __name__ == '__main__':
    main()
