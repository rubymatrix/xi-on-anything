"""Build the iPhone host (an .app with host64 inside) with Xcode's clang, for arm64.

  python3 tools/build_ios.py app [--sdk iphonesimulator|iphoneos] [--sign-identity name]
        [--profile file.mobileprovision] [--wallpaper picture]
        build/ios-<sdk>/XI on Anything.app, from the game prepared by tools/build_posix.py prepare
  python3 tools/build_ios.py install [--device name] [--bundle-id id]
        the device build signed (the identity and the profile found in the keychain and Xcode's
        profiles when not given) and installed on a connected iPhone
  python3 tools/build_ios.py run [--device name] [-- host64 arguments ...]
        the simulator build installed on a simulator (booted, else the named or first iPhone) and
        started with its log on this terminal; arguments after -- go to host64 (--game, --server ...)

The same sources as build_posix.py's host64, compiled for iOS into build/ios-<sdk>/obj (the desktop
objects in build/ are not touched), with SDL3's UIKit tree (third_party/sdl3-ios, tools/vendor.py)
and platforms/ios/ios_main.c as the entry. Generated game code stays in generated/ (ignored by git). A
simulator build is signed ad hoc; a device build needs a signing identity and a provisioning
profile whose app id allows the entitlements below (extended virtual addressing: the guest's 4 GB
window, guest.h). See docs/platforms/ios-dev.md.
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
# Security: the sign-in's saved passwords; CoreText: the touch controls' labels (gfx_metal.m)
FRAMEWORKS = ['Metal', 'QuartzCore', 'Foundation', 'Security', 'CoreText', 'CoreGraphics']
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

    def __init__(self, sdk, bundle_id=APP_ID):
        self.sdk = sdk
        self.bundle_id = bundle_id
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


def host(t, embed=False, split=False):
    """host64 for iOS: build/ios-<sdk>/host64. embed: instead the game as a library for the launcher's
    app (its Play calls xi_ios_play, platforms/ios/ios_main.c): build/ios-<sdk>/embed/libxi.a, with
    embed/link.json, what else the app links (the libraries' archives and the frameworks)."""
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
    base = ['-O2', '-std=c11', '-g', '-DRT_GUEST_WINDOW', '-DFFXI_IOS', '-DFFXI_MOBILE', '-fno-strict-aliasing', '-I', 'runtime',
            '-I', 'runtime/portable', '-I', 'generated', '-I', 'third_party/stb']
    game = {part: t.compile(build_posix.generated(part), part, base + build_posix.GEN_WARNINGS + ['-I', 'generated/' + part], True)
            for part in ('all', 'ffxi')}
    objs = [] if split else game['all'] + game['ffxi']
    # the PlayOnline client, when XI_POL names it (build_posix.pol)
    pol_sources, pol_flags, pol_libs = build_posix.pol(
        'aarch64-apple-ios' + ('-sim' if t.sdk == 'iphonesimulator' else ''), t.out, {'IPHONEOS_DEPLOYMENT_TARGET': MIN_IOS})
    if pol_sources:
        objs += t.compile(pol_sources, 'pol', base + pol_flags + build_posix.POL_CFLAGS)
    ours = base + inc + pol_flags + ['-I', 'host/addons', '-DIMGUI_USER_CONFIG="imconfig_xi.h"']
    sources = build_posix.PORTABLE + build_posix.HOST_SOURCES + build_posix.ADDON_SOURCES
    # split: the sources that name a game build's addresses (build.h) are compiled on the phone with the
    # player's build's build.h (game_kit), as Android's are (build_android.py --split-game)
    device = [s for s in sources if split and not s.startswith('generated/') and build_posix.names_build(s)]
    objs += t.compile([s for s in sources if s != 'host/host64.c' and s not in device], 'host', ours)
    if 'host/host64.c' not in device:
        objs += t.compile(['host/host64.c'], 'host64', ours + ['-Dmain=xi_host_main'])
    device_objs = t.compile([s for s in device if s != 'host/host64.c'], 'host', ours)
    if 'host/host64.c' in device:
        device_objs += t.compile(['host/host64.c'], 'host64', ours + ['-Dmain=xi_host_main'])
    objs += t.compile(['platforms/ios/ios_main.c', 'host/touch.c', 'host/display_fit.c'], 'embed' if embed else 'entry',
                      ours + (['-DXI_IOS_EMBED'] if embed else []))  # iOS only
    fw = sum((['-framework', f] for f in FRAMEWORKS), [])
    # the upscaler (iOS 16 on), where the SDK has it: Xcode 27's simulator SDK has none (gfx_metal.m does without)
    sdk = subprocess.run(['xcrun', '--sdk', t.sdk, '--show-sdk-path'], capture_output=True, text=True).stdout.strip()
    if os.path.isdir(os.path.join(sdk, 'System/Library/Frameworks/MetalFX.framework')):
        fw += ['-weak_framework', 'MetalFX']
    hidden = ['-Wl,-load_hidden,' + a for a in pol_libs]  # the crate's symbols, not exported
    sim = []
    if t.sdk == 'iphonesimulator':
        # The simulator reads an app's entitlements from this section, as Xcode links them; an ad-hoc
        # signature can't carry them. Without an application-identifier the keychain refuses every item
        # (errSecMissingEntitlement), so Remember Password saved nothing.
        path = os.path.join(t.out, 'simulator-entitlements.plist')
        with open(path, 'wb') as f:
            plistlib.dump({'application-identifier': 'SIMULATOR.' + t.bundle_id}, f)
        sim = ['-Wl,-sectcreate,__TEXT,__entitlements,' + path]
    if split:
        return game_kit(t, objs, game, device, device_objs, base, ours, archives + list(pol_libs),
                        frameworks + fw + ['-lc++', '-lm'])
    if embed:
        out = os.path.join(t.out, 'embed')
        os.makedirs(out, exist_ok=True)
        lib = os.path.join(out, 'libxi.a')
        if os.path.exists(lib):
            os.remove(lib)
        run([t.ar, 'rcs', lib] + objs)
        with open(os.path.join(out, 'link.json'), 'w') as f:
            json.dump({'library': lib, 'archives': archives + list(pol_libs), 'flags': frameworks + fw + ['-lc++', '-lm']}, f, indent=2)
        return lib, bool(pol_libs)
    exe = os.path.join(t.out, 'host64')
    run(t.cc + ['-o', exe] + objs + archives + hidden + sim + ['-lc++'] + frameworks + fw + ['-lm', '-lpthread',
                                                                                  '-Wl,-export_dynamic'])
    return exe, bool(pol_libs)


LLVM_RESOURCE = os.path.join(ROOT, 'build', 'ios-llvm', 'ios', 'lib', 'clang', '18', 'include')


def tbd_for_arm64(text):
    """An SDK stub (.tbd) that lists arm64 wherever it lists arm64e: iOS 27's SDK stubs name only arm64e
    (and arm64e.x1, which LLVM 18 doesn't know), which Xcode's linker accepts for an arm64 link and lld
    doesn't."""
    import re

    def fix(m):
        items = [x.strip() for x in m.group(1).split(',') if x.strip()]
        items = [x for x in items if '.x1' not in x]
        for x in list(items):
            if x.startswith('arm64e-') and x.replace('arm64e-', 'arm64-') not in items:
                items.append(x.replace('arm64e-', 'arm64-'))
        return 'targets: [ ' + ', '.join(items) + ' ]'
    return re.sub(r'targets:\s*\[([^\]]*)\]', fix, text)


def game_kit(t, objs, game, device, device_objs, base, ours, archives, link_flags):
    """kit: what the phone needs to make the game library (XIGame.dylib) itself, none of it game code.

    build/ios-<sdk>/kit: lib/ (libxi.a, the host without the game and without the sources that name a
    build's addresses, and the libraries' archives), sdk/ (the SDK headers those compiles include and
    the stubs the link reads), resource/ (LLVM 18's own headers: the phone's compiler is LLVM 18),
    tree/ and inc/ (our headers and the build.h sources, as in this checkout), cflags.txt (the
    translation's), host.json (the build.h sources and their flags), link.json, kit.id; the phone's
    translation writes gen/build.h and src/. build/ios-<sdk>/phone: the translator (recomp/, unpack.py,
    phone_kit.py and what it imports, recomp/meta). build/ios-<sdk>/XIGame.dylib: the same library linked
    here from this checkout's translation, which checks that the kit links, and is what a player's Mac
    puts in the app instead (Sideloadly's dylib injection, docs/platforms/ios.md)."""
    import re
    import shlex
    kit = os.path.join(t.out, 'kit')
    shutil.rmtree(kit, ignore_errors=True)
    os.makedirs(os.path.join(kit, 'lib'))
    sdk = os.path.realpath(xcrun(t.sdk, '--show-sdk-path'))
    resource = os.path.realpath(subprocess.run(t.cc + ['-print-resource-dir'], capture_output=True, text=True,
                                               check=True).stdout.strip())
    root = os.path.realpath(ROOT)
    generated = os.path.join(root, 'generated')
    flags_all = base + ours
    outside = sorted({os.path.realpath(os.path.join(ROOT, f[2:] if len(f) > 2 else ''))
                      for f in flags_all if f.startswith('-I') and len(f) > 2}
                     | {os.path.realpath(os.path.join(ROOT, flags_all[i + 1])) for i, f in enumerate(flags_all) if f == '-I'},
                     key=lambda d: -len(d))
    outside = [d for d in outside if not (d + os.sep).startswith(root + os.sep)]

    def place(h):
        h = os.path.realpath(h)
        if h.startswith(sdk + os.sep):
            return os.path.join(kit, 'sdk', os.path.relpath(h, sdk))
        if h.startswith(resource + os.sep):
            return os.path.join(kit, 'resource', os.path.relpath(h, resource))
        if os.path.dirname(h) == generated:
            return None if os.path.basename(h) == 'build.h' else os.path.join(kit, 'gen', os.path.basename(h))
        if h.startswith(generated + os.sep):
            return None  # a part's own headers (funcs.h): the phone's translation makes them
        if os.path.basename(h) == 'build.h':
            raise RuntimeError('%s: a build.h other than generated/build.h' % h)
        if h.startswith(root + os.sep):
            return os.path.join(kit, 'tree', os.path.relpath(h, root))
        for n, d in enumerate(outside):
            if h.startswith(d + os.sep):
                return os.path.join(kit, 'inc', str(n), os.path.relpath(h, d))
        raise RuntimeError('%s: in no include folder' % h)

    def listed(cmd):
        r = subprocess.run(cmd + ['-M', '-E', '-o', os.devnull, '-MF', '/dev/stdout'], cwd=ROOT, capture_output=True,
                           text=True, check=True)
        deps = shlex.split(r.stdout.replace('\\\n', ' ').split(':', 1)[-1])
        return [d if os.path.isabs(d) else os.path.join(ROOT, d) for d in deps]

    def kit_flags(flags):
        """flags with this Mac's paths as the kit's (@KIT@), -g and the include folders' order kept."""
        out, i = [], 0
        while i < len(flags):
            f = flags[i]
            if f == '-I' or (f.startswith('-I') and len(f) > 2):
                d = flags[i + 1] if f == '-I' else f[2:]
                i += 2 if f == '-I' else 1
                full = os.path.realpath(os.path.join(ROOT, d))
                if full == generated:
                    out.append('-I@KIT@/gen')
                elif full.startswith(generated + os.sep):
                    out.append('-I@KIT@/src/' + os.path.relpath(full, generated))
                elif (full + os.sep).startswith(root + os.sep):
                    out.append('-I@KIT@/tree/' + os.path.relpath(full, root) if full != root else '-I@KIT@/tree')
                else:
                    out.append('-I@KIT@/inc/%d' % outside.index(full))
                continue
            if f != '-g':
                out.append(f)
            i += 1
        return out

    wanted = set()
    for part, part_objs in game.items():
        srcs = build_posix.generated(part)
        wanted.update(listed(t.cc + base + build_posix.GEN_WARNINGS + ['-I', 'generated/' + part, srcs[0]])[1:])
    host = []
    for s in device:
        extra = ['-Dmain=xi_host_main'] if s == 'host/host64.c' else []
        wanted.update(listed(t.cc + ours + extra + [s])[1:])
        host.append({'source': s, 'flags': kit_flags(ours + extra)})
    # and every header beside ours: what this build's build.h leaves out (#if) another build's includes
    for folder in {os.path.dirname(os.path.realpath(f)) for f in wanted}:
        if folder.startswith(root + os.sep) and not folder.startswith(generated):
            wanted.update(os.path.join(folder, f) for f in os.listdir(folder) if f.endswith(('.h', '.inc', '.hpp')))
    llvm = os.path.isdir(LLVM_RESOURCE)
    for h in sorted(wanted):
        dest = place(h)
        if dest is None:
            continue
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        src = h
        if dest.startswith(os.path.join(kit, 'resource', 'include') + os.sep) and llvm:
            src = os.path.join(LLVM_RESOURCE, os.path.relpath(dest, os.path.join(kit, 'resource', 'include')))
            if not os.path.exists(src):
                continue  # Apple's clang's own (the LLVM 18 one includes no such header)
        shutil.copyfile(src, dest)
    if llvm:  # the whole of LLVM 18's but the x86 intrinsics: its headers include each other
        os.makedirs(os.path.join(kit, 'resource', 'include'), exist_ok=True)
        for f in os.listdir(LLVM_RESOURCE):
            full = os.path.join(LLVM_RESOURCE, f)
            if os.path.isfile(full) and f.endswith('.h') and not re.search(r'intrin|^avx|^x86|^amx|^ia32|^sse|^mm|^pmm|^tmm|^emm|^xmm|^smm|^nmm|^wmm|^ammintrin|^f16c|^fma|^bmi|^lzcnt|^popcnt|^rtm|^xsave|^xop|^tbm|^sha|^adx|^cet|^clflush|^clwb|^clzero|^enqcmd|^gfni|^hreset|^invpcid|^keylocker|^lwp|^movdir|^mwaitx|^pconfig|^pku|^prfchw|^ptwrite|^rdpru|^rdseed|^serialize|^sgx|^shstk|^tsxldtrk|^uintr|^vaes|^vpclmul|^waitpkg|^wbnoinvd|^cldemote|^cmpccxadd|^crc32|^raoint|^prfchi|^usermsr|^avxifma|^avxvnni', f):
                dest = os.path.join(kit, 'resource', 'include', f)
                if not os.path.exists(dest):
                    shutil.copyfile(full, dest)
    else:
        print('no build/ios-llvm (build it first): the kit has Xcode clang\'s headers, not LLVM 18\'s')

    # the phone's clang: this SDK's target, the kit's headers in place of the SDK's and Xcode's
    prefix = ['-target', t.arch[1], '-isysroot', '@KIT@/sdk', '-resource-dir', '@KIT@/resource', '-Wno-unknown-warning-option']
    for h in host:
        h['flags'] = prefix + h['flags']
    for f in ('SDKSettings.json', 'SDKSettings.plist'):  # the driver reads the SDK's version here
        if os.path.exists(os.path.join(sdk, f)):
            os.makedirs(os.path.join(kit, 'sdk'), exist_ok=True)
            shutil.copyfile(os.path.join(sdk, f), os.path.join(kit, 'sdk', f))
    with open(os.path.join(kit, 'cflags.txt'), 'w') as f:
        f.write('\n'.join(prefix + kit_flags(base + build_posix.GEN_WARNINGS)) + '\n')
    with open(os.path.join(kit, 'host.json'), 'w') as f:
        json.dump(host, f, indent=1)

    # the libraries: ours whole (-force_load: addons find functions by name), the rest as archives
    xi = os.path.join(kit, 'lib', 'libxi.a')
    run([t.ar, 'rcs', xi] + objs)
    names = []
    for a in archives:
        dest = os.path.join(kit, 'lib', os.path.basename(a))
        shutil.copyfile(a, dest)
        names.append(os.path.basename(a))
    rt = os.path.join(resource, 'lib', 'darwin', 'libclang_rt.%s.a' % ('iossim' if t.sdk == 'iphonesimulator' else 'ios'))
    shutil.copyfile(rt, os.path.join(kit, 'lib', os.path.basename(rt)))
    names.append(os.path.basename(rt))
    for a in [xi] + [os.path.join(kit, 'lib', n) for n in names if n != os.path.basename(rt)]:  # no debug information on the phone
        subprocess.run(['xcrun', 'strip', '-S', a], check=True, capture_output=True)

    # the stubs: libSystem, libc++ and the frameworks, and what each re-exports
    stubs, todo = {}, ['/usr/lib/libSystem.B.dylib', '/usr/lib/libc++.1.dylib', '/usr/lib/libobjc.A.dylib']
    i = 0
    while i < len(link_flags):
        if link_flags[i] in ('-framework', '-weak_framework'):
            todo.append('/System/Library/Frameworks/%s.framework/%s' % (link_flags[i + 1], link_flags[i + 1]))
            i += 2
        else:
            i += 1
    while todo:
        name = todo.pop()
        if name in stubs:
            continue
        path = os.path.join(sdk, name.lstrip('/'))
        path = os.path.splitext(path)[0] + '.tbd' if path.endswith('.dylib') else path + '.tbd'
        if not os.path.exists(path):
            stubs[name] = None
            continue
        text = open(path).read()
        stubs[name] = path
        dest = os.path.join(kit, 'sdk', os.path.relpath(path, sdk))
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        with open(dest, 'w') as f:
            f.write(tbd_for_arm64(text) if t.sdk == 'iphoneos' else text)
        for block in re.findall(r're-?exported-libraries:(.*?)(?:\n\S|\Z)', text, re.S):
            todo += re.findall(r"'?(/[^',\s\]]+)'?", block)
    # libSystem.B is the stub under libSystem's name, -lSystem's file
    for alias, real in (('usr/lib/libSystem.tbd', 'usr/lib/libSystem.B.tbd'), ('usr/lib/libc++.tbd', 'usr/lib/libc++.1.tbd'),
                        ('usr/lib/libobjc.tbd', 'usr/lib/libobjc.A.tbd')):
        if os.path.exists(os.path.join(kit, 'sdk', real)):
            shutil.copyfile(os.path.join(kit, 'sdk', real), os.path.join(kit, 'sdk', alias))
    sdk_version = xcrun(t.sdk, '--show-sdk-version')
    with open(os.path.join(kit, 'link.json'), 'w') as f:
        json.dump({'force_load': ['libxi.a'], 'archives': names, 'flags': link_flags, 'min': MIN_IOS,
                   'sdk_version': sdk_version, 'simulator': t.sdk == 'iphonesimulator'}, f, indent=1)

    # the translator's side
    phone = os.path.join(t.out, 'phone')
    shutil.rmtree(phone, ignore_errors=True)
    os.makedirs(os.path.join(phone, 'recomp'))
    os.makedirs(os.path.join(phone, 'tools'))
    for f in ('recomp/recomp.py', 'recomp/x86c.py', 'tools/unpack.py', 'tools/phone_kit.py', 'tools/build.py',
              'tools/buildinfo.py', 'tools/android_translation.py'):
        shutil.copyfile(os.path.join(ROOT, f), os.path.join(phone, f))
    shutil.copytree(os.path.join(ROOT, 'recomp', 'meta'), os.path.join(phone, 'recomp', 'meta'),
                    ignore=shutil.ignore_patterns('.*', 'discovery'))
    shutil.copyfile(os.path.join(ROOT, 'runtime.json'), os.path.join(phone, 'runtime.json'))

    digest = hashlib.sha256()
    for b in (kit, phone):
        for d, _, files in sorted(os.walk(b)):
            for f in sorted(files):
                full = os.path.join(d, f)
                digest.update(os.path.relpath(full, t.out).encode() + b'\0' + open(full, 'rb').read())
    with open(os.path.join(kit, 'kit.id'), 'w') as f:
        f.write(digest.hexdigest()[:16] + '\n')

    # the kit's headers are enough: each of its sources (and a translated one) checked with its flags
    # here, the kit's paths for @KIT@ and this checkout's translation for the phone's own
    def here(flags, part=None):
        out = [f.replace('@KIT@/gen', generated).replace('@KIT@/src', generated).replace('@KIT@', kit) for f in flags]
        return out + (['-I' + os.path.join(generated, part)] if part else [])
    checks = [(here(h['flags']), os.path.join(kit, 'tree', h['source'])) for h in host]
    with open(os.path.join(kit, 'cflags.txt')) as f:
        cflags = f.read().split()
    checks += [(here(cflags, part), os.path.join(ROOT, build_posix.generated(part)[0])) for part in game]
    bad = []
    for flags, src in checks:
        r = subprocess.run(t.cc[:1] + ['-fsyntax-only'] + flags + [src], capture_output=True, text=True)
        if r.returncode:
            bad.append('%s:\n%s' % (src, r.stderr[-1500:]))
    if bad:
        raise SystemExit('the kit misses headers its sources need:\n' + '\n'.join(bad[:3]))

    # the check, and the Mac-built game library
    dylib = os.path.join(t.out, 'XIGame.dylib')
    every = game['all'] + game['ffxi'] + device_objs
    run(t.cc + ['-dynamiclib', '-install_name', '@rpath/XIGame.dylib', '-o', dylib] + every
        + ['-Wl,-force_load,' + xi] + [os.path.join(kit, 'lib', n) for n in names] + link_flags)
    size = sum(os.path.getsize(os.path.join(d, f)) for d, _, fs in os.walk(kit) for f in fs)
    print('kit %s (%s, %.0f MB): %d host sources for the phone; %s' % (kit, open(os.path.join(kit, 'kit.id')).read().strip(),
                                                                       size / 1e6, len(host), dylib))
    return dylib, False


LLVM_SRC = os.path.join(ROOT, 'build', 'android-llvm', 'src', 'llvm-project-18.1.8.src')
PYTHON_XCF = os.path.join(ROOT, 'build', 'ios-python', 'cross-build', 'iOS', 'Python.xcframework')
CAPSTONE = os.path.join(ROOT, 'build', 'ios-capstone')


def toolchain(t):
    """What the launcher's app needs to make the game on the phone, none of it game code, in
    build/ios-<sdk>/bundle: libxitoolchain.a (platforms/ios/ios_toolchain.cpp, ios_sign.c,
    ios_python.c) and toolchain.json (the LLVM archives and the rest it links), Python.xcframework
    (CPython for iOS, build/ios-python: CPython's Apple/ build), site/ (pefile and capstone's binding),
    libcapstone.dylib, and the kit and the translator (game_kit) as xi/. The launcher's Xcode project
    links and copies them in (its scripts/ios-bundle.sh)."""
    llvm = os.path.join(ROOT, 'build', 'ios-llvm', 'sim' if t.sdk == 'iphonesimulator' else 'ios')
    if not os.path.isdir(os.path.join(llvm, 'lib')):
        raise SystemExit('no %s: build LLVM for iOS first (build/ios-llvm/build%s.sh)' % (llvm, '-sim' if t.sdk == 'iphonesimulator' else ''))
    if not os.path.isdir(PYTHON_XCF):
        raise SystemExit('no %s: build CPython for iOS first (its Apple/ build)' % PYTHON_XCF)
    out = os.path.join(t.out, 'bundle')
    os.makedirs(out, exist_ok=True)
    slice_ = 'ios-arm64_x86_64-simulator' if t.sdk == 'iphonesimulator' else 'ios-arm64'
    pyheaders = os.path.join(PYTHON_XCF, slice_, 'Python.framework', 'Headers')
    llvm_inc = ['-I' + os.path.join(LLVM_SRC, d) for d in ('llvm/include', 'clang/include', 'lld/include')]
    llvm_inc += ['-I' + os.path.join(llvm, d) for d in ('include', 'tools/clang/include', 'tools/lld/include')]
    objs = t.compile(['platforms/ios/ios_toolchain.cpp'], 'toolchain-cxx', ['-O2', '-fno-rtti', '-fno-exceptions', '-w'] + llvm_inc)
    objs += t.compile(['platforms/ios/ios_sign.c', 'platforms/ios/ios_python.c'], 'toolchain-c',
                      ['-O2', '-std=c11', '-I', pyheaders, '-I', 'platforms/ios'])
    lib = os.path.join(out, 'libxitoolchain.a')
    if os.path.exists(lib):
        os.remove(lib)
    run([t.ar, 'rcs', lib] + objs)
    archives = sorted(os.path.join(llvm, 'lib', f) for f in os.listdir(os.path.join(llvm, 'lib')) if f.endswith('.a'))
    # the Python framework the app embeds: this SDK's slice, copied whole
    xcf = os.path.join(out, 'Python.xcframework')
    shutil.rmtree(xcf, ignore_errors=True)
    shutil.copytree(PYTHON_XCF, xcf, symlinks=True)
    # the standard library but its tests, GUI, docs and installer (most of its size)
    for d, _, files in list(os.walk(xcf)):
        base = os.path.basename(d)
        if base in ('test', 'tests', 'idle_test', 'idlelib', 'tkinter', 'turtledemo', 'ensurepip', 'pydoc_data',
                    '__pycache__', '_pyrepl', 'venv') and 'python3.' in d:
            shutil.rmtree(d, ignore_errors=True)
            continue
        for f in files:
            if base == 'lib-dynload' and f.startswith(('_test', 'xx', '_ctypes_test', '_xxtest')):
                os.remove(os.path.join(d, f))
    utils = os.path.join(xcf, 'build', 'utils.sh')
    with open(utils) as f:
        text = f.read()
    # an app named with spaces ("XI on Anything.app"): CPython's script leaves one path unquoted
    with open(utils, 'w') as f:
        f.write(text.replace('> ${FULL_EXT%.so}.fwork', '> "${FULL_EXT%.so}.fwork"'))
    with open(os.path.join(out, 'toolchain.json'), 'w') as f:
        json.dump({'library': lib, 'archives': archives,
                   'flags': ['-F' + os.path.join(xcf, slice_), '-framework', 'Python', '-framework', 'Security', '-lc++', '-lz'],
                   'python_slice': slice_}, f, indent=1)
    # the translator's Python packages: pefile, and capstone's binding finding libcapstone.dylib in Frameworks
    import capstone as host_capstone
    import pefile as host_pefile
    site = os.path.join(out, 'site')
    shutil.rmtree(site, ignore_errors=True)
    os.makedirs(site)
    shutil.copyfile(host_pefile.__file__, os.path.join(site, 'pefile.py'))
    shutil.copytree(os.path.join(os.path.dirname(host_pefile.__file__), 'ordlookup'), os.path.join(site, 'ordlookup'),
                    ignore=shutil.ignore_patterns('__pycache__'))
    shutil.copytree(os.path.dirname(host_capstone.__file__), os.path.join(site, 'capstone'),
                    ignore=shutil.ignore_patterns('__pycache__', 'lib', 'include'))
    init = os.path.join(site, 'capstone', '__init__.py')
    with open(init) as f:
        text = f.read()
    # iOS's Python says 'ios', and the library is a .dylib there too (LIBCAPSTONE_PATH: the app's Frameworks)
    with open(init, 'w') as f:
        f.write(text.replace("if sys.platform == 'darwin':", "if sys.platform in ('darwin', 'ios'):", 1))
    dylib = os.path.join(CAPSTONE, t.sdk, 'libcapstone.5.0.9.dylib')
    shutil.copyfile(dylib, os.path.join(out, 'libcapstone.dylib'))
    run(['install_name_tool', '-id', '@rpath/libcapstone.dylib', os.path.join(out, 'libcapstone.dylib')])
    # the kit and the translator
    xi = os.path.join(out, 'xi')
    shutil.rmtree(xi, ignore_errors=True)
    os.makedirs(xi)
    shutil.copytree(os.path.join(t.out, 'kit'), os.path.join(xi, 'kit'))
    shutil.copytree(os.path.join(t.out, 'phone'), os.path.join(xi, 'phone'))
    print('toolchain in %s' % out)


def info_plist(t):
    return {
        'CFBundleExecutable': APP_NAME,
        'CFBundleIdentifier': t.bundle_id,
        'CFBundleName': APP_NAME,
        'CFBundleDisplayName': APP_NAME,
        'CFBundlePackageType': 'APPL',
        'CFBundleInfoDictionaryVersion': '6.0',
        'CFBundleShortVersionString': buildinfo.runtime()['version'],
        # the game build it is made for, as digits (2026-11-02: 20261102): the launcher reads it off a
        # device to say whether the app there is current
        'CFBundleVersion': (buildinfo.runtime()['build'] or '').replace('-', '') or '1',
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
        # the player copies the game folder in with the Files app or Finder (docs/platforms/ios-dev.md)
        'UIFileSharingEnabled': True,
        'LSSupportsOpeningDocumentsInPlace': True,
        'LSApplicationCategoryType': 'public.app-category.role-playing-games',
        'GCSupportsControllerUserInteraction': True,
        'GCSupportedGameControllers': [{'ProfileName': 'ExtendedGamepad'}],
        'FFXIIOS': '1',
    }


def app_icon(t, bundle):
    """platforms/ios/icon-ios.png (platforms/ios/make_ios_icon.swift) as the app's icon: an asset catalog compiled into
    the bundle with actool; returns the Info.plist keys that name it."""
    cat = os.path.join(t.out, 'Assets.xcassets')
    icon = os.path.join(cat, 'AppIcon.appiconset')
    shutil.rmtree(cat, ignore_errors=True)
    os.makedirs(icon)
    shutil.copy(os.path.join(ROOT, 'platforms', 'ios', 'icon-ios.png'), os.path.join(icon, 'icon.png'))
    with open(os.path.join(icon, 'Contents.json'), 'w') as f:
        json.dump({'images': [{'filename': 'icon.png', 'idiom': 'universal', 'platform': 'ios', 'size': '1024x1024'}],
                   'info': {'author': 'xcode', 'version': 1}}, f)
    with open(os.path.join(cat, 'Contents.json'), 'w') as f:
        json.dump({'info': {'author': 'xcode', 'version': 1}}, f)
    partial = os.path.join(t.out, 'icon-info.plist')
    run(['xcrun', 'actool', cat, '--compile', bundle, '--platform', t.sdk, '--minimum-deployment-target', MIN_IOS,
         '--app-icon', 'AppIcon', '--target-device', 'iphone', '--target-device', 'ipad',
         '--output-partial-info-plist', partial], stdout=subprocess.DEVNULL)
    with open(partial, 'rb') as f:
        return plistlib.load(f)


def app(t, a):
    exe, pol = host(t)
    bundle = os.path.join(t.out, APP_NAME + '.app')
    shutil.rmtree(bundle, ignore_errors=True)
    os.makedirs(bundle)
    shutil.copy(exe, os.path.join(bundle, APP_NAME))
    if pol:  # the shipped copy names none of the PlayOnline client's functions (build_posix.POL_CFLAGS)
        run(['xcrun', 'strip', '-S', '-x', os.path.join(bundle, APP_NAME)])
    shutil.copy(os.path.join(ROOT, 'assets', 'ffxi.reg'), bundle)
    shutil.copytree(os.path.join(ROOT, 'assets', 'textures'), os.path.join(bundle, 'textures'))
    if a.wallpaper:  # behind the black bars (platforms/ios/ios_main.c): PNG or JPEG, anything else as a PNG
        src, ext = os.path.expanduser(a.wallpaper), os.path.splitext(a.wallpaper)[1].lower()
        if ext in ('.png', '.jpg', '.jpeg'):
            shutil.copy(src, os.path.join(bundle, 'wallpaper' + ext))
        else:
            run(['sips', '-s', 'format', 'png', src, '--out', os.path.join(bundle, 'wallpaper.png')])
    info = info_plist(t)
    info.update(app_icon(t, bundle))
    with open(os.path.join(bundle, 'Info.plist'), 'wb') as f:
        plistlib.dump(info, f, fmt=plistlib.FMT_BINARY)
    with open(os.path.join(bundle, 'PkgInfo'), 'w') as f:
        f.write('APPL????')
    if t.sdk == 'iphonesimulator':
        run(['codesign', '--force', '--sign', '-', '--timestamp=none', bundle])
    else:
        a.profile = a.profile or find_profile(t.bundle_id)
        a.sign_identity = a.sign_identity or find_identity()
        if not a.sign_identity or not a.profile:
            raise SystemExit('a device build needs a signing identity and a provisioning profile for %s '
                             '(--sign-identity, --profile; docs/platforms/ios-dev.md)' % t.bundle_id)
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


def find_profile(bundle_id):
    """An unexpired provisioning profile Xcode made for bundle_id (a free Personal Team's too), newest first."""
    import datetime
    found = []
    for d in ('~/Library/Developer/Xcode/UserData/Provisioning Profiles', '~/Library/MobileDevice/Provisioning Profiles'):
        d = os.path.expanduser(d)
        for f in os.listdir(d) if os.path.isdir(d) else []:
            path = os.path.join(d, f)
            r = subprocess.run(['security', 'cms', '-D', '-i', path], capture_output=True)
            if r.returncode:
                continue
            prof = plistlib.loads(r.stdout)
            app_id = prof.get('Entitlements', {}).get('application-identifier', '')
            expires = prof.get('ExpirationDate')
            if app_id.split('.', 1)[-1] == bundle_id and expires and expires > datetime.datetime.now():
                found.append((expires, path))
    return max(found)[1] if found else None


def find_identity():
    """The first Apple Development signing identity in the keychain."""
    out = subprocess.run(['security', 'find-identity', '-v', '-p', 'codesigning'], capture_output=True, text=True).stdout
    for line in out.splitlines():
        if '"Apple Development:' in line:
            return line.split('"')[1]
    return None


def iphone(name):
    """The identifier of a connected, paired iPhone or iPad (the named one, else the first)."""
    out = os.path.join(ROOT, 'build', 'devices.json')
    subprocess.run(['xcrun', 'devicectl', 'list', 'devices', '--json-output', out], capture_output=True, check=True)
    with open(out) as f:
        devices = json.load(f)['result']['devices']
    for d in devices:
        props = d.get('deviceProperties', {})
        # a simulator says 'simulated'; a real device 'physical', or (iOS 26) nothing at all
        if d.get('hardwareProperties', {}).get('reality') == 'simulated' or d.get('visibilityClass') == 'simulators':
            continue
        if not name or name in (props.get('name'), d.get('identifier'), d.get('hardwareProperties', {}).get('udid')):
            return d['identifier']
    raise SystemExit('no iPhone connected%s (cable or Wi-Fi, paired, Developer Mode on)' % (' named ' + name if name else ''))


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
    ap.add_argument('target', choices=['app', 'run', 'install', 'lib', 'kit', 'toolchain'])
    ap.add_argument('--sdk', choices=['iphonesimulator', 'iphoneos'], default='iphonesimulator')
    ap.add_argument('--sign-identity')
    ap.add_argument('--profile')
    ap.add_argument('--device', help='simulator name for run, iPhone name for install')
    ap.add_argument('--bundle-id', default=APP_ID, help='the app id (a free Personal Team needs one of its own)')
    ap.add_argument('--wallpaper', help='a picture behind the black bars beside the game (yours: not in the source)')
    argv = sys.argv[1:]
    host_args = argv[argv.index('--') + 1:] if '--' in argv else []
    a = ap.parse_args(argv[:argv.index('--')] if '--' in argv else argv)
    if a.target == 'run':
        a.sdk = 'iphonesimulator'
    if a.target == 'install':
        a.sdk = 'iphoneos'
    t = Target(a.sdk, a.bundle_id)
    if a.target == 'lib':  # the game for the launcher's app (host's embed)
        print(host(t, embed=True)[0])
        return
    if a.target == 'kit':  # the launcher's app builds the game itself (game_kit)
        host(t, embed=True, split=True)
        return
    if a.target == 'toolchain':  # and what it builds with (toolchain): the kit first
        host(t, embed=True, split=True)
        toolchain(t)
        return
    bundle = app(t, a)
    if a.target == 'install':
        run(['xcrun', 'devicectl', 'device', 'install', 'app', '--device', iphone(a.device), bundle])
    if a.target == 'run':
        udid = simulator(a.device)
        run(['xcrun', 'simctl', 'install', udid, bundle])
        run(['xcrun', 'simctl', 'launch', '--console-pty', '--terminate-running-process', udid, t.bundle_id] + host_args)


if __name__ == '__main__':
    main()
