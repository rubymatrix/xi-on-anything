#!/usr/bin/env python3
"""Game/device-free Android renderer policy and worker lifecycle checks."""

import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    with tempfile.TemporaryDirectory(prefix='xi-policy-') as directory:
        out = Path(directory)
        tests = [
            ('android_frontend_policy_test.c', [], [[]]),
            ('gfx_launch_policy_test.cpp', [], [[]]),
            ('gfx_pass_plan_test.cpp', [], [[]]),
            ('gfx_visibility_storage_lifecycle_test.cpp', [], [[]]),
            ('gfx_worker_test.cpp', [], [[]]),
            ('gfx_probe_mailbox_test.cpp', [], [[]]),
            ('gfx_worker_api_test.cpp', ['runtime/portable/gfx_worker.cpp'], [['off'], ['on']]),
            (
                'gfx_worker_mailbox_test.cpp',
                ['runtime/portable/gfx_worker.cpp'],
                [
                    [m]
                    for m in (
                        'off',
                        'worker-only',
                        'mailbox',
                        'mailbox-issue-failed',
                        'mailbox-poll-failed',
                        'mailbox-exact-failed',
                        'const-fx-off',
                        'const-fx-on',
                        'const-fx-unsupported',
                        'const-fx-worker-off',
                    )
                ],
            ),
        ]
        for name, extra, modes in tests:
            cpp = name.endswith('.cpp')
            cc = shlex.split(os.environ.get('CXX' if cpp else 'CC', 'c++' if cpp else 'cc'))
            exe = out / Path(name).stem
            args = [
                '-O2',
                '-std=c++17' if cpp else '-std=c11',
                '-D_DEFAULT_SOURCE',
                '-D_DARWIN_C_SOURCE',
                '-DFFXI_ANDROID_VULKAN',
                '-DFFXI_RENDER_WORKER_BUILD',
                '-I' + str(ROOT / 'runtime/portable'),
                '-pthread',
            ]
            subprocess.run(
                cc
                + args
                + shlex.split(os.environ.get('CFLAGS', ''))
                + [str(ROOT / 'tests' / name)]
                + [str(ROOT / s) for s in extra]
                + ['-o', str(exe)],
                check=True,
            )
            for mode in modes:
                subprocess.run([str(exe)] + mode, check=True, timeout=60)
        exe = out / 'benchmark_test'
        subprocess.run(
            shlex.split(os.environ.get('CC', 'cc'))
            + [
                '-O2',
                '-std=c11',
                '-D_DEFAULT_SOURCE',
                '-D_DARWIN_C_SOURCE',
                '-I' + str(ROOT / 'runtime'),
                '-I' + str(ROOT / 'runtime/portable'),
                '-I' + str(ROOT / 'host'),
                str(ROOT / 'tests/benchmark_test.c'),
                str(ROOT / 'host/benchmark.c'),
                '-o',
                str(exe),
            ]
            + shlex.split(os.environ.get('CFLAGS', '')),
            check=True,
        )
        for mode in ('off', 'existing-frame', 'existing-marker', 'write-fail', 'clock-back', 'records'):
            case = out / mode
            case.mkdir()
            subprocess.run([str(exe), mode, str(case)], check=True, timeout=60)
    print('android policy: PASS (no game or FPS measurement)')


if __name__ == '__main__':
    main()
