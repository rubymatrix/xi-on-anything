#!/usr/bin/env python3
"""Download and build the Android dependencies: SDL3 3.4.16, and glslang 16.5.0 with SPIRV-Tools
and SPIRV-Headers (the HLSL front end and the SPIR-V optimizer), for ARM64.

  python3 tools/android_deps.py [--ndk <dir>] [--jobs N]

The sources are pinned by SHA-256. Downloads and builds stay in the dependency cache
(XI_ANDROID_DEPS, default build/android-deps); nothing is installed on a device.
"""

import argparse
import hashlib
import json
import subprocess
import tarfile
import urllib.request
from pathlib import Path

from android_toolchain import CACHE, NDK, SDL

SDL_SHA256 = '7322236cd12090c3eb40b9728be4d49c76f66ad17d04369584d4ecad5cf77c68'  # release-3.4.16 (android_toolchain.SDL)
# (archive name, GitHub repository, ref, archive SHA-256, where it goes in the glslang tree)
SOURCES = [
    (
        'glslang-16.5.0',
        'KhronosGroup/glslang',
        '16.5.0',
        '01af17195fbeb59e39e31e9506de35bb39dfd35807ea0c9a1a99d7d1183ddd45',
        '',
    ),
    (
        'spirv-tools',
        'KhronosGroup/SPIRV-Tools',
        'b707790a898e44038547df54580022fc1cf89c3d',
        '05d8af89737bde57571c48dbd36714c9f520a69623e14de72c3be6b600e277d6',
        'External/spirv-tools',
    ),
    (
        'spirv-tools-external-spirv-headers',
        'KhronosGroup/SPIRV-Headers',
        '29981f65241605e08b0ede4cfeb999fe3b723c6a',
        '232899f1ad4104fb5bc377b94596c7621575eee62ad9a9e8f929b63a7dd8a7ad',
        'External/spirv-tools/external/spirv-headers',
    ),
]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def download(url, archive, expected):
    """archive, downloaded from url once and checked against its pinned hash every time."""
    if not archive.exists():
        partial = archive.with_suffix('.partial')
        urllib.request.urlretrieve(url, partial)
        if sha(partial) != expected:
            raise RuntimeError('downloaded source hash differs: ' + archive.name)
        partial.rename(archive)
    if sha(archive) != expected:
        raise RuntimeError('cached source hash differs: ' + archive.name)


def fetch():
    CACHE.mkdir(parents=True, exist_ok=True)
    source = CACHE / 'glslang-16.5.0'
    records = []
    for name, repo, ref, expected, relative in SOURCES:
        archive = CACHE / (name + '.tar.gz')
        url = 'https://codeload.github.com/' + repo + '/tar.gz/' + ref
        download(url, archive, expected)
        target = source / relative if relative else source
        if not target.exists():
            temp = CACHE / (name + '-extraction')
            temp.mkdir(exist_ok=False)
            with tarfile.open(archive) as data:
                data.extractall(temp, filter='data')
            entries = list(temp.iterdir())
            assert len(entries) == 1
            target.parent.mkdir(parents=True, exist_ok=True)
            entries[0].rename(target)
            temp.rmdir()
        records.append({'repo': repo, 'ref': ref, 'url': url, 'sha256': expected})
    return source, records


def build_sdl(toolchain, jobs):
    archive = CACHE / (SDL.name + '.tar.gz')
    download('https://github.com/libsdl-org/SDL/releases/download/release-3.4.16/' + archive.name, archive, SDL_SHA256)
    if not SDL.exists():
        with tarfile.open(archive) as data:
            data.extractall(CACHE, filter='data')
    output = CACHE / 'sdl-build'
    subprocess.run(
        [
            'cmake',
            '-S',
            str(SDL),
            '-B',
            str(output),
            '-G',
            'Ninja',
            '-DCMAKE_TOOLCHAIN_FILE=' + str(toolchain),
            '-DANDROID_ABI=arm64-v8a',
            '-DANDROID_PLATFORM=android-28',
            '-DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON',
            '-DCMAKE_BUILD_TYPE=Release',
            '-DSDL_SHARED=ON',
            '-DSDL_STATIC=OFF',
            '-DSDL_TESTS=OFF',
            '-DSDL_TEST_LIBRARY=OFF',
        ],
        check=True,
    )
    subprocess.run(['cmake', '--build', str(output), '--parallel', str(jobs)], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('--ndk', type=Path, default=NDK)
    parser.add_argument('--api', type=int, choices=[28], default=28)
    parser.add_argument('--jobs', type=int, default=6)
    args = parser.parse_args()
    toolchain = args.ndk / 'build/cmake/android.toolchain.cmake'
    if not toolchain.exists():
        raise SystemExit('NDK toolchain missing: ' + str(toolchain))
    source, sources = fetch()
    build_sdl(toolchain, args.jobs)
    build, install = CACHE / 'spirv-build', CACHE / 'spirv-install'
    flags = [
        'cmake',
        '-S',
        str(source),
        '-B',
        str(build),
        '-G',
        'Ninja',
        '-DCMAKE_TOOLCHAIN_FILE=' + str(toolchain),
        '-DANDROID_ABI=arm64-v8a',
        '-DANDROID_PLATFORM=android-' + str(args.api),
        '-DANDROID_STL=c++_static',
        '-DCMAKE_BUILD_TYPE=Release',
        '-DCMAKE_POSITION_INDEPENDENT_CODE=ON',
        '-DCMAKE_C_FLAGS=-ffixed-x18 -mbranch-protection=standard',
        '-DCMAKE_CXX_FLAGS=-ffixed-x18 -mbranch-protection=standard',
        '-DCMAKE_INSTALL_PREFIX=' + str(install),
        '-DBUILD_SHARED_LIBS=OFF',
        '-DENABLE_HLSL=ON',
        '-DENABLE_GLSLANG_BINARIES=OFF',
        '-DENABLE_SPIRV=ON',
        '-DENABLE_OPT=ON',
        '-DGLSLANG_TESTS=OFF',
        '-DSPIRV_SKIP_TESTS=ON',
        '-DSPIRV_SKIP_EXECUTABLES=ON',
        '-DSPIRV_WERROR=OFF',
    ]
    for command in [
        flags,
        ['cmake', '--build', str(build), '--parallel', str(args.jobs)],
        ['cmake', '--install', str(build)],
    ]:
        subprocess.run(command, check=True)
    libraries = sorted((install / 'lib').glob('*.a'))
    manifest = {
        'sources': sources,
        'configure': flags,
        'include': str(install / 'include'),
        'library_dir': str(install / 'lib'),
        'libraries': {p.name: sha(p) for p in libraries},
        'cmake_prefix': str(install),
        'target': 'Android ARM64 API' + str(args.api),
        'static_cpp_runtime': True,
        'x18_reserved': True,
        'optimizer_and_hlsl_legalization': True,
        'phone_test_performed': False,
    }
    (CACHE / 'android-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    link_names = ['glslang', 'SPIRV', 'glslang-default-resource-limits', 'SPIRV-Tools-opt', 'SPIRV-Tools']
    link = {
        'cflags': ['-I' + str(install / 'include')],
        'libs': [str(install / 'lib' / ('lib' + name + '.a')) for name in link_names],
    }
    (CACHE / 'spirv-build.json').write_text(json.dumps(link, indent=2) + '\n')
    print('Android dependencies ready in', CACHE)


if __name__ == '__main__':
    main()
