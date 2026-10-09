#!/usr/bin/env python3
"""Package the Android host built by tools/build_android.py into a signed test APK, with the SDK's
own tools (javac, d8, aapt, zipalign, apksigner) and no Gradle.

  python3 tools/build_android_apk.py [--build-dir build/android]

The APK is <build dir>/xi-native-test.apk: SDL's Java and android/, libSDL3.so, and libmain.so with its
debug info stripped. Its minimum API follows the library's build-config.json. The signing key is made
once, in the dependency cache. Nothing is installed on a device.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import zipfile

from android_toolchain import CACHE, ROOT, SDK, SDL, TC

BT = SDK / 'build-tools/36.0.0'
ANDROID_JAR = SDK / 'platforms/android-36/android.jar'
JDK = Path(os.environ.get('JAVA_HOME', str(Path(shutil.which('javac') or '/missing/javac').resolve().parents[1])))


def run(args):
    subprocess.run(list(map(str, args)), check=True, cwd=ROOT)


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


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument(
        '--build-dir',
        type=Path,
        default=ROOT / 'build/android',
        help='Exact input library and build-config directory; outputs stay within it',
    )
    args = parser.parse_args()
    directory = args.build_dir.resolve()
    minimum_api, actual = build_metadata(directory)
    intermediates = directory / 'apk'
    intermediates.mkdir(parents=True, exist_ok=True)
    # javac leaves removed/renamed classes behind when its output directory is reused.
    with tempfile.TemporaryDirectory(prefix='staging-', dir=intermediates) as staging:
        out = Path(staging)
        manifest = ROOT / 'android/app/src/main/AndroidManifest.xml'
        if minimum_api != 28:
            contents, count = re.subn(
                r'android:minSdkVersion="28"', f'android:minSdkVersion="{minimum_api}"', manifest.read_text()
            )
            if count != 1:
                raise RuntimeError('default manifest minimum API changed')
            manifest = out / 'AndroidManifest.xml'
            manifest.write_text(contents)

        java = sorted((CACHE / SDL.name / 'android-project/app/src/main/java').rglob('*.java')) + sorted(
            (ROOT / 'android/app/src/main/java').rglob('*.java')
        )
        classes = out / 'classes'
        classes.mkdir()
        run(
            [
                JDK / 'bin/javac',
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
        run(
            [
                BT / 'aapt',
                'package',
                '-f',
                '-M',
                manifest,
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

        keystore = CACHE / 'test-signing.p12'
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
                'pass:android',
                '--ks-key-alias',
                'localtest',
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
                }
            )
        )


if __name__ == '__main__':
    main()
