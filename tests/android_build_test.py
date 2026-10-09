#!/usr/bin/env python3
"""tools/build_android.py's object cache and tools/build_android_apk.py's packaging, with the host
compiler and stub SDK tools: no NDK, game or device.

  python3 tests/android_build_test.py [-v]
"""

import contextlib
import hashlib
import io
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
import zipfile
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import build_android  # noqa: E402
import build_android_apk  # noqa: E402


class ObjectCacheTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='xi build cache ')
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        self.source = self.directory / 'source.c'
        self.header = self.directory / 'header with spaces.h'
        self.obj = self.directory / 'source.o'
        self.header.write_text('#define VALUE 1\n')
        self.source.write_text('#include "header with spaces.h"\nint value(void) { return VALUE; }\n')
        for name, value in [('ROOT', self.directory), ('CC', shutil.which('cc')), ('FLAGS', [])]:
            replacement = patch.object(build_android, name, value)
            replacement.start()
            self.addCleanup(replacement.stop)
        self.compile()

    def compile(self):
        _, error = build_android.compile_one(('source.c', self.obj, []))
        self.assertIsNone(error)

    def test_unchanged_object_is_reused(self):
        before = self.obj.stat().st_mtime_ns
        self.compile()
        self.assertEqual(before, self.obj.stat().st_mtime_ns)

    def test_changed_relative_header_with_spaces_rebuilds(self):
        before = self.obj.read_bytes()
        self.header.write_text('#define VALUE 2\n')
        timestamp = self.obj.stat().st_mtime_ns + 1_000_000_000
        os.utime(self.header, ns=(timestamp, timestamp))
        self.compile()
        self.assertNotEqual(before, self.obj.read_bytes())

    def test_missing_depfile_rebuilds(self):
        deps = self.obj.with_suffix('.d')
        deps.unlink()
        self.compile()
        self.assertTrue(deps.is_file())

    def test_deleted_header_is_reported(self):
        self.header.unlink()
        _, error = build_android.compile_one(('source.c', self.obj, []))
        self.assertIsNotNone(error)
        self.assertIn('header with spaces.h', error)

    def test_changed_compiler_flags_rebuild(self):
        stamp = self.obj.with_suffix('.stamp')
        before = stamp.read_text()
        with patch.object(build_android, 'FLAGS', ['-DTEST_CONFIGURATION=1']):
            self.compile()
        self.assertNotEqual(before, stamp.read_text())


class PackageMetadataTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='xi-apk-metadata-')
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        library = self.directory / 'libmain.so'
        library.write_bytes(b'test library identity, not an ELF')
        self.digest = hashlib.sha256(library.read_bytes()).hexdigest()

    def metadata(self, api=29, native_tls=True, digest=None):
        (self.directory / 'build-config.json').write_text(
            json.dumps(
                {
                    'minimum_api': api,
                    'native_tls': native_tls,
                    'libmain_sha256': self.digest if digest is None else digest,
                }
            )
        )
        return build_android_apk.build_metadata(self.directory)

    def test_matching_api_and_tls(self):
        self.assertEqual((28, self.digest), self.metadata(28, False))
        self.assertEqual((29, self.digest), self.metadata())

    def test_missing_metadata(self):
        with self.assertRaisesRegex(RuntimeError, 'metadata is required'):
            build_android_apk.build_metadata(self.directory)

    def test_mismatched_library(self):
        with self.assertRaisesRegex(RuntimeError, 'does not match'):
            self.metadata(digest='0' * 64)

    def test_invalid_api(self):
        for api in (27, 30, '29', 29.0, True, None):
            with self.subTest(api=api), self.assertRaisesRegex(RuntimeError, 'minimum API'):
                self.metadata(api=api)

    def test_invalid_tls(self):
        for api, tls in ((28, True), (29, False), (29, 1), (29, 'true'), (29, None)):
            with self.subTest(api=api, tls=tls), self.assertRaisesRegex(RuntimeError, 'TLS metadata'):
                self.metadata(api=api, native_tls=tls)


class PackageRebuildTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='xi-apk-rebuild-')
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        self.build = self.directory / 'build'
        self.build.mkdir()
        library = b'synthetic native library'
        (self.build / 'libmain.so').write_bytes(library)
        (self.build / 'build-config.json').write_text(
            json.dumps({'minimum_api': 29, 'native_tls': True, 'libmain_sha256': hashlib.sha256(library).hexdigest()})
        )
        self.sources = self.directory / 'android/app/src/main/java'
        self.sources.mkdir(parents=True)
        (self.sources.parent / 'AndroidManifest.xml').write_text('<uses-sdk android:minSdkVersion="28" />')
        cache = self.directory / 'deps'
        (cache / 'sdl-build').mkdir(parents=True)
        (cache / 'sdl-build/libSDL3.so').write_bytes(b'synthetic SDL library')
        (cache / 'test-signing.p12').touch()
        for name, value in [('ROOT', self.directory), ('CACHE', cache), ('run', self.tool)]:
            replacement = patch.object(build_android_apk, name, value)
            replacement.start()
            self.addCleanup(replacement.stop)
        self.dex_inputs = []
        self.fail_dex = False

    def tool(self, args):
        # Stub SDK commands only: exercise the packager's actual paths and archive creation.
        command = Path(args[0]).name
        if command == 'javac':
            classes = Path(args[args.index('-d') + 1])
            for source in args:
                source = Path(str(source))
                if source.suffix == '.java':
                    (classes / (source.stem + '.class')).write_bytes(source.read_bytes())
        elif command == 'd8':
            classes = [Path(arg) for arg in args if str(arg).endswith('.class')]
            self.dex_inputs.append(sorted(path.name for path in classes))
            if self.fail_dex:
                raise RuntimeError('synthetic dex failure')
            (Path(args[args.index('--output') + 1]) / 'classes.dex').write_bytes(b'dex')
        elif command == 'aapt':
            with zipfile.ZipFile(args[args.index('-F') + 1], 'w'):
                pass
        elif command == 'zipalign':
            shutil.copyfile(args[-2], args[-1])
        elif command == 'apksigner' and args[1] == 'sign':
            shutil.copyfile(args[-1], args[args.index('--out') + 1])
        else:
            self.assertTrue(command == 'llvm-strip' or (command == 'apksigner' and args[1] == 'verify'), args)

    def package(self):
        with patch.object(sys, 'argv', ['build_android_apk.py', '--build-dir', str(self.build)]):
            with contextlib.redirect_stdout(io.StringIO()):
                build_android_apk.main()

    def test_rebuild_excludes_removed_java_classes(self):
        current = self.sources / 'Current.java'
        removed = self.sources / 'Removed.java'
        current.write_text('current')
        removed.write_text('removed')
        self.package()
        self.assertEqual(['Current.class', 'Removed.class'], self.dex_inputs[-1])
        removed.unlink()
        self.package()
        self.assertEqual(['Current.class'], self.dex_inputs[-1])
        self.assertEqual([], list((self.build / 'apk').iterdir()))
        with zipfile.ZipFile(self.build / 'xi-native-test.apk') as archive:
            self.assertEqual(
                ['classes.dex', 'lib/arm64-v8a/libSDL3.so', 'lib/arm64-v8a/libmain.so'], archive.namelist()
            )

    def test_dex_failure_preserves_existing_outputs_and_cleans_staging(self):
        (self.sources / 'Current.java').write_text('current')
        apk = self.build / 'xi-native-test.apk'
        apk.write_bytes(b'previous package')
        legacy = self.build / 'apk/classes/Old.class'
        legacy.parent.mkdir(parents=True)
        legacy.write_bytes(b'previous intermediate')
        self.fail_dex = True
        with self.assertRaisesRegex(RuntimeError, 'synthetic dex failure'):
            self.package()
        self.assertEqual(['Current.class'], self.dex_inputs[-1])
        self.assertEqual(b'previous package', apk.read_bytes())
        self.assertEqual(b'previous intermediate', legacy.read_bytes())
        self.assertEqual([legacy.parent], list((self.build / 'apk').iterdir()))


if __name__ == '__main__':
    unittest.main()
