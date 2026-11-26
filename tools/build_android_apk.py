#!/usr/bin/env python3
"""Package the Android host built by tools/build_android.py into a signed APK, with the SDK's own
tools (javac, d8, aapt, zipalign, apksigner) and no Gradle.

  python3 tools/build_android_apk.py [--build-dir build/android] [--keystore file.p12 [--key-alias xi]]

The APK is <build dir>/xi-native-test.apk: SDL's Java and platforms/android/, libSDL3.so, and libmain.so with its
debug info stripped. Its minimum API follows the library's build-config.json; its versionName is
runtime.json's version and its versionCode the game build's digits (20261102), what the launcher reads
back from a phone. --keystore signs it with that key (its password in XI_ANDROID_KEYSTORE_PASS): a
player's own, which every later update must be signed with too, or Android refuses it. Without it, a
test key made once in the dependency cache signs it. Nothing is installed on a device.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile

# tools/ itself: the launcher's Python on Windows (the embeddable one, with a ._pth) does not put the
# script's folder on sys.path
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from android_toolchain import CACHE, ROOT, SDK, SDL, TC  # noqa: E402
import buildinfo  # noqa: E402

BT = SDK / 'build-tools/36.0.0'
ANDROID_JAR = SDK / 'platforms/android-36/android.jar'
JDK = Path(os.environ.get('JAVA_HOME', str(Path(shutil.which('javac') or '/missing/javac').resolve().parents[1])))


def run(args):
    args = list(map(str, args))
    if sys.platform == 'win32' and not Path(args[0]).suffix:
        # build-tools on Windows: d8 and apksigner are .bat, aapt and zipalign .exe
        for ext in ('.exe', '.bat'):
            if Path(args[0] + ext).exists():
                args[0] += ext
                break
    subprocess.run(args, check=True, cwd=ROOT)


def build_metadata(directory):
    """(minimum API, libmain.so's SHA-256) from build-config.json, which must describe this very library."""
    build_config = directory / 'build-config.json'
    if not build_config.is_file():
        raise RuntimeError('matching Android build metadata is required before packaging')
    config = json.loads(build_config.read_text())
    actual = hashlib.sha256((directory / 'libmain.so').read_bytes()).hexdigest()
    if config.get('libmain_sha256') != actual:
        raise RuntimeError('build metadata does not match libmain.so')
    minimum_api = config.get('minimum_api')
    if type(minimum_api) is not int or minimum_api not in (28, 29):
        raise RuntimeError('unsupported minimum API')
    if not isinstance(config.get('native_tls'), bool) or config['native_tls'] != (minimum_api >= 29):
        raise RuntimeError('native TLS metadata disagrees with minimum API')
    return minimum_api, actual


def version():
    """(versionCode, versionName): the game build's digits (20261102; 1 before tools/prepare.py) and
    the client's version (runtime.json), as the iPhone app's CFBundleVersion and short version."""
    info = buildinfo.runtime()
    digits = (info['build'] or '').replace('-', '')
    return (int(digits) if digits.isdigit() else 1), info['version']


def bundle(directory, to, minimum_api):
    """The game for an app that has its own (the launcher, built with Gradle): its Java (SDL's and
    platforms/android/'s: GameActivity, BuildActivity), its libraries (libmain.so stripped of debug info,
    libSDL3.so, and with --split-game the phone's compiler and Python) and its assets (with
    --split-game the kit, the translator and its Python library; the app brings ffxi.reg). The app's
    manifest declares GameActivity and BuildActivity; bundle.json says what this is."""
    shutil.rmtree(to, ignore_errors=True)
    java = to / 'java'
    shutil.copytree(CACHE / SDL.name / 'android-project/app/src/main/java', java)
    shutil.copytree(ROOT / 'platforms/android/app/src/main/java', java, dirs_exist_ok=True)
    libs = to / 'jniLibs/arm64-v8a'
    libs.mkdir(parents=True)
    shutil.copyfile(CACHE / 'sdl-build/libSDL3.so', libs / 'libSDL3.so')
    shutil.copyfile(directory / 'libmain.so', libs / 'libmain.so')
    run([TC / 'llvm-strip', '--strip-debug', libs / 'libmain.so'])
    for extra in ('lib_llvm.so', 'lib_python.so', 'libpython3.14.so'):
        if (directory / extra).is_file():
            shutil.copyfile(directory / extra, libs / extra)
    # the base registry is the app's own asset (the launcher bundles its copy, ffxi.reg)
    assets = to / 'assets'
    assets.mkdir()
    for part in ('kit', 'phone', 'pyhome'):
        if (directory / part).is_dir():
            shutil.copytree(directory / part, assets / part)
    code, name = version()
    (to / 'bundle.json').write_text(
        json.dumps({'version_code': code, 'version_name': name, 'minimum_api': minimum_api}, indent=1) + '\n'
    )
    print(json.dumps({'bundle': str(to), 'version_code': code, 'version_name': name}))


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument(
        '--build-dir',
        type=Path,
        default=ROOT / 'build/android',
        help='Exact input library and build-config directory; outputs stay within it',
    )
    parser.add_argument('--keystore', type=Path, help='PKCS#12 key to sign with; its password in XI_ANDROID_KEYSTORE_PASS')
    parser.add_argument('--key-alias', default='xi', help="the key's alias in --keystore")
    parser.add_argument(
        '--bundle',
        type=Path,
        help='instead of an APK, the game\'s part of one for another app to build in (the launcher\'s '
        'Android project): java/, jniLibs/arm64-v8a/, assets/ and bundle.json in this folder',
    )
    args = parser.parse_args()
    directory = args.build_dir.resolve()
    minimum_api, actual = build_metadata(directory)
    if args.bundle:
        return bundle(directory, args.bundle.resolve(), minimum_api)
    intermediates = directory / 'apk'
    intermediates.mkdir(parents=True, exist_ok=True)
    # javac leaves removed/renamed classes behind when its output directory is reused.
    with tempfile.TemporaryDirectory(prefix='staging-', dir=intermediates) as staging:
        out = Path(staging)
        manifest = ROOT / 'platforms/android/app/src/main/AndroidManifest.xml'
        if minimum_api != 28:
            contents, count = re.subn(
                r'android:minSdkVersion="28"', f'android:minSdkVersion="{minimum_api}"', manifest.read_text()
            )
            if count != 1:
                raise RuntimeError('default manifest minimum API changed')
            manifest = out / 'AndroidManifest.xml'
            manifest.write_text(contents)

        java = sorted((CACHE / SDL.name / 'android-project/app/src/main/java').rglob('*.java')) + sorted(
            (ROOT / 'platforms/android/app/src/main/java').rglob('*.java')
        )
        classes = out / 'classes'
        classes.mkdir()
        run(
            [
                JDK / 'bin/javac',
                '-encoding',  # SDL's sources are UTF-8; javac's default is the system code page on Windows
                'UTF-8',
                '-source',
                '8',
                '-target',
                '8',
                '-bootclasspath',
                ANDROID_JAR,
                '-d',
                classes,
            ]
            + java
        )
        dex = out / 'dex'
        dex.mkdir()
        run(
            [
                BT / 'd8',
                '--min-api',
                str(minimum_api),
                '--lib',
                ANDROID_JAR,
                '--output',
                dex,
            ]
            + sorted(classes.rglob('*.class'))
        )
        unsigned = out / 'unsigned.apk'
        code, name = version()
        # the base registry the app starts the game with (GameActivity.getArguments), as the iPhone
        # app's bundle has it
        assets = out / 'assets'
        assets.mkdir()
        if (ROOT / 'assets' / 'ffxi.reg').is_file():
            shutil.copyfile(ROOT / 'assets' / 'ffxi.reg', assets / 'ffxi.reg')
        # a host built with --split-game: what the phone translates and builds the game with
        # (BuildActivity): the compiler's kit, the translator and its metadata, and its Python
        for part in ('kit', 'phone', 'pyhome'):
            if (directory / part).is_dir():
                shutil.copytree(directory / part, assets / part)
        run(
            [
                BT / 'aapt',
                'package',
                '-f',
                '--version-code',
                str(code),
                '--version-name',
                name,
                '--replace-version',
                '-M',
                manifest,
                '-S',
                ROOT / 'platforms/android/app/src/main/res',
                '-A',
                assets,
                # aapt's default also drops directories named _*, which the Python library has
                '--ignore-assets',
                '!.svn:!.git:!.DS_Store:!__pycache__',
                '-I',
                ANDROID_JAR,
                '-F',
                unsigned,
            ]
        )
        library = out / 'libmain.so'
        shutil.copyfile(directory / 'libmain.so', library)
        run([TC / 'llvm-strip', '--strip-debug', library])
        with zipfile.ZipFile(unsigned, 'a', compression=zipfile.ZIP_DEFLATED) as archive:
            archive.write(dex / 'classes.dex', 'classes.dex')
            archive.write(CACHE / 'sdl-build/libSDL3.so', 'lib/arm64-v8a/libSDL3.so')
            archive.write(library, 'lib/arm64-v8a/libmain.so')
            # a host built with --split-game: the compiler the phone builds libgame.so with (BuildActivity)
            for extra in ('lib_llvm.so', 'lib_python.so', 'libpython3.14.so'):
                if (directory / extra).is_file():
                    archive.write(directory / extra, 'lib/arm64-v8a/' + extra)

        if args.keystore:
            if not args.keystore.is_file() or 'XI_ANDROID_KEYSTORE_PASS' not in os.environ:
                raise RuntimeError('--keystore needs the key file and XI_ANDROID_KEYSTORE_PASS')
            keystore, alias, password = args.keystore, args.key_alias, 'env:XI_ANDROID_KEYSTORE_PASS'
        else:
            keystore, alias, password = CACHE / 'test-signing.p12', 'localtest', 'pass:android'
            if not keystore.exists():
                run(
                    [
                        JDK / 'bin/keytool',
                        '-genkeypair',
                        '-keystore',
                        keystore,
                        '-storepass',
                        'android',
                        '-keypass',
                        'android',
                        '-alias',
                        'localtest',
                        '-dname',
                        'CN=XI Native Local Test',
                        '-keyalg',
                        'RSA',
                        '-validity',
                        '3650',
                    ]
                )
        aligned = out / 'aligned.apk'
        run([BT / 'zipalign', '-f', '-P', '16', '4', unsigned, aligned])
        apk = directory / 'xi-native-test.apk'
        run(
            [
                BT / 'apksigner',
                'sign',
                '--ks',
                keystore,
                '--ks-pass',
                password,
                '--ks-key-alias',
                alias,
                '--out',
                apk,
                aligned,
            ]
        )
        run([BT / 'apksigner', 'verify', apk])
        print(
            json.dumps(
                {
                    'apk': str(apk),
                    'sha256': hashlib.sha256(apk.read_bytes()).hexdigest(),
                    'bytes': apk.stat().st_size,
                    'build_dir': str(directory),
                    'unstripped_lib_sha256': actual,
                    'packed_lib_sha256': hashlib.sha256(library.read_bytes()).hexdigest(),
                    'version_code': code,
                    'version_name': name,
                }
            )
        )


if __name__ == '__main__':
    main()
