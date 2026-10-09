#!/usr/bin/env python3
"""host/android_main.c's argument handling, compiled with the host compiler against stub host and
SDL functions (tests/android_entry_test.c), in the four render-worker/geometry build variants.

  python3 tests/android_entry_test.py
"""

import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
LOGIN = ['--user', 'tester', '--pass', 'synthetic-test-only']
BOOLEANS = {
    'native-geometry': 'FFXI_ANDROID_NATIVE_GEOMETRY',
    'readback': 'FFXI_ASYNC_READBACK',
    'encode-cache': 'FFXI_ANDROID_ENCODE_CACHE',
    'pass-plan': 'FFXI_ANDROID_PASS_PLAN',
    'render-worker': 'FFXI_RENDER_WORKER',
    'worker-stats': 'FFXI_RENDER_WORKER_DIAGNOSTICS',
    'worker-mailbox': 'FFXI_RENDER_WORKER_MAILBOX',
    'worker-const-fx': 'FFXI_RENDER_WORKER_CONST_FX',
    'visibility-storage': 'FFXI_ANDROID_VISIBILITY_STORAGE',
    'visibility-storage-stats': 'FFXI_ANDROID_VISIBILITY_STORAGE_DIAGNOSTICS',
    'cache-sampled': 'FFXI_ANDROID_CACHE_SAMPLED',
    'bounded-area': 'FFXI_ANDROID_BOUNDED_AREA',
    'fast-sync': 'FFXI_ANDROID_FAST_SYNC',
    'trim-uniforms': 'FFXI_ANDROID_TRIM_UNIFORMS',
    'dont-care-loads': 'FFXI_ANDROID_DONT_CARE_LOADS',
}


def cases(worker, geometry):
    # Each tuple specifies argv, exit code, and optionally an environment key/value.
    yield [], 3, None
    yield ['--user', 'tester'], 3, None
    yield ['--user', 'tester', '--pass', ''], 3, None
    yield ['--user', '', '--pass', 'synthetic'], 3, None
    yield ['--user', 'other', '--pass', 'synthetic-test-only'], 42, None
    yield LOGIN, 42, None
    for option in ('session', 'auth'):
        yield LOGIN + ['--' + option, 'synthetic'], 3, None
    for option, key in BOOLEANS.items():
        flag = '--android-' + option
        for value in ('0', '1', '2', '', '01', '-1', 'true'):
            args = LOGIN + [flag, value]
            available = True
            if option in ('render-worker', 'worker-stats', 'worker-mailbox', 'worker-const-fx'):
                available = worker
            elif option == 'native-geometry':
                available = geometry
            if value == '1':
                if option in ('worker-mailbox', 'worker-const-fx'):
                    args += ['--android-render-worker', '1', '--android-readback', '1']
                elif option in ('visibility-storage', 'visibility-storage-stats'):
                    args += ['--android-visibility-storage', '1', '--android-readback', '1']
            expected = 42 if value == '0' or (value == '1' and available) else 4
            yield args, expected, (key, value) if expected == 42 else None
        yield LOGIN + [flag], 4, None
        on = option == 'encode-cache' or (option == 'native-geometry' and geometry)
        yield LOGIN, 42, (key, '1' if on else '0')
    for value in ('0', '1', '2', '3', '4', '5', '54300', '54301', '', '-1', '01', '1x'):
        expected = 42 if value in ('0', '54300') else 4
        yield LOGIN + ['--android-control', value], expected, ('FFXI_CONTROL', value) if expected == 42 else None
    for option, key in (
        ('pass-trace', 'FFXI_ANDROID_PASS_TRACE'),
        ('probe-query-diag', 'FFXI_ANDROID_PROBE_QUERY_DIAG'),
        ('bench-dir', 'FFXI_BENCH_DIR'),
        ('addons', 'FFXI_ADDONS'),
    ):
        yield LOGIN + ['--android-' + option, 'test value'], 42, (key, 'test value')
        yield LOGIN + ['--android-' + option], 4, None
    for option in (
        'unknown',
        'policy-snapshot',
        'frontend-policy-snapshot',
        'shadow-diagnostic',
        'shadow-map-budget',
        'shadow-map-interval',
        'diagnostics',
        'sample-hz',
        'backend-cost',
        'mesh-cost',
        'mesh-cost-detail',
        'mesh-cost-low-overhead',
        'native-geometry-capture',
        'native-geometry-stats',
        'scalar-geometry',
    ):
        for suffix in ([], ['0'], ['1']):
            yield LOGIN + ['--android-' + option] + suffix, 4, None
    storage = ['--android-visibility-storage', '1']
    yield LOGIN + storage, 4, None
    yield LOGIN + ['--android-visibility-storage-stats', '1'], 4, None
    for conflict in ('render-worker', 'pass-plan', 'bounded-area', 'pass-trace', 'probe-query-diag'):
        yield LOGIN + storage + ['--android-readback', '1', '--android-' + conflict, '1'], 4, None
    yield LOGIN + ['--android-worker-const-fx', '1'], 4, None
    yield LOGIN + ['--android-worker-mailbox', '1'], 4, None
    yield LOGIN + ['--android-worker-mailbox', '1', '--android-render-worker', '1'], 4, None
    # All offscreen gates dispatch without a login; mailbox's game-only prerequisite follows them.
    for code, option in enumerate(('gfx', 'format', 'state', 'async', 'area', 'pass-plan', 'visibility-storage'), 43):
        args = ['--android-' + option + '-test']
        yield args, code, None
        if worker:
            yield ['--android-worker-mailbox', '1', '--android-render-worker', '1'] + args, code, None
    yield ['--android-fast-sync', '1'] + LOGIN + ['--android-fast-sync', '0'], 42, ('FFXI_ANDROID_FAST_SYNC', '0')
    yield LOGIN + ['--port', '54001'], 42, None


def main():
    count = 0
    with tempfile.TemporaryDirectory(prefix='xi-entry-') as directory:
        out = Path(directory)
        (out / 'SDL3').mkdir()
        (out / 'SDL3/SDL.h').write_text(
            '#define SDL_HINT_ENABLE_SCREEN_KEYBOARD "keyboard"\n'
            'static inline int SDL_SetHint(const char* k, const char* v) {(void)k; (void)v; return 1;}\n'
            'static inline void SDL_SetMainReady(void) {}\n'
        )
        (out / 'SDL3/SDL_main.h').touch()
        (out / 'build.h').touch()
        env = {key: value for key, value in os.environ.items() if not key.startswith(('FFXI_', 'XI_ENTRY_TEST_'))}
        for worker, geometry in ((False, False), (False, True), (True, False), (True, True)):
            exe = out / f'entry-{int(worker)}-{int(geometry)}'
            subprocess.run(
                shlex.split(os.environ.get('CC', 'cc'))
                + ['-std=c11', '-D_DARWIN_C_SOURCE', '-D_DEFAULT_SOURCE', f'-DFFXI_GEOMETRY_LAYOUT={int(geometry)}']
                + (['-DFFXI_RENDER_WORKER_BUILD'] if worker else [])
                + shlex.split(os.environ.get('CFLAGS', ''))
                + [
                    '-I' + str(out),
                    '-I' + str(ROOT / 'runtime/portable'),
                    str(ROOT / 'host/android_main.c'),
                    str(ROOT / 'tests/android_entry_test.c'),
                    '-o',
                    str(exe),
                ],
                check=True,
            )
            missing_dir = subprocess.run([str(exe)] + LOGIN, env=env, capture_output=True, timeout=10)
            assert missing_dir.returncode == 2, missing_dir
            count += 1
            for args, expected, setting in cases(worker, geometry):
                data = out / f'case-{count}'
                data.mkdir()
                result = subprocess.run(
                    [str(exe), '--data-dir', str(data)] + args,
                    env={**env, 'XI_ENTRY_TEST_ENV': setting[0] if setting else ''},
                    capture_output=True,
                    timeout=10,
                )
                log = (data / 'host64.log').read_text()
                assert result.returncode == expected, (worker, geometry, args, result.returncode, expected, log)
                if expected == 42:
                    assert 'test:arg=tester\n' in log or 'test:arg=other\n' in log, log
                    assert 'test:arg=synthetic-test-only\n' in log, log
                    if setting:
                        assert f'test:env={setting[1]}\n' in log, (setting, log)
                if expected >= 42 and worker:
                    assert 'test:shutdown\n' in log, log
                if expected < 42:
                    assert 'test:arg=' not in log and 'test:shutdown' not in log, log
                count += 1
    print(f'android entry: PASS ({count} cases, four feature builds; no game/device access)')


if __name__ == '__main__':
    main()
