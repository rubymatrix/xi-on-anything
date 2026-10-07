# Android (experimental)

The Android build runs the recompiled client natively on ARM64, through SDL3 and
a C++ Vulkan back end. It uses no Wine, GameNative, FEX or DXVK. It shares the
geometry adapter with the desktop hosts; the desktop Metal, D3D12 and C Vulkan
back ends keep their own build paths.

The APK is a debug client. It signs in to a LandSandBoat server with the
account given in `run.args` as `--user` and `--pass`; the sign-in screen and
saved sessions are not offered. It contains no password, game files, captures or generated game code:
bring your own compatible client installation and local test server. The C++
back end does not implement the Modern FX extensions; the game's own D3D8
lighting, fog, textures and shadows work. A controller or keyboard is
recommended. On-screen buttons are enabled by the activity's `touch_controls`
boolean extra.

## Build on macOS or Linux

Requirements: Python 3.12+ (`capstone`, `pefile`), CMake, Ninja, JDK 17, Android SDK
platform 36 and build-tools 36.0.0, and NDK 27.0.12077973. Set the paths for your machine:

```sh
export ANDROID_HOME=/path/to/android-sdk
export ANDROID_NDK_HOME="$ANDROID_HOME/ndk/27.0.12077973"
export JAVA_HOME=/path/to/jdk17
python3 tools/build_posix.py prepare --game /path/to/FINAL-FANTASY-XI
python3 tools/android_deps.py
python3 tools/build_android.py host --native-tls
python3 tools/build_android_apk.py
```

`android_deps.py` downloads hash-pinned SDL3 3.4.16, glslang 16.5.0 and its
SPIRV-Tools and SPIRV-Headers dependencies, and builds them for ARM64. Downloads
and build products stay in `build/android-deps` (ignored by git);
`XI_ANDROID_DEPS` changes that directory. `XI_ANDROID_BUILD_DIR` changes the
native output directory; pass the same directory to `build_android_apk.py
--build-dir` when packaging.

The Android translation of FFXiMain.dll, with the occlusion-probe wraps, goes to
`generated/android-all`; the desktop `generated/all` is left as it is. FFXi.dll is
translated from the same prepared files. All generated game code stays ignored.
`--native-tls` targets API 29 and its ELF TLS; without it the build targets API 28
with emulated TLS. The object cache keys include the compiler command. Packaging
checks the library's hash against its build metadata and sets the matching
minimum API. The signing key is created in the dependency directory. Neither the
build nor packaging installs an APK or changes device settings.

The APK is `build/android/xi-native-test.apk`, package `dev.rdbell.xionandroid`.
Before replacing an existing installation, keep a copy of its APK, data and
configuration. The app reads `run.args` from its external files directory, one
literal argument per line (no shell expansion). Give `--game`, `--data-dir`, the
registry paths, `--server`, `--user` and `--pass` there. Without both,
the app exits instead of showing the sign-in screen. Never commit that file. Without `run.args`, the built-in command line uses the server 127.0.0.1
(loopback), not a public game service.

## Controls and defaults

The options below are off by default and take `0` or `1` unless stated
otherwise. When comparing, change one setting per trial and keep the scene,
resolution and effects the same. The Android entry consumes these arguments
before the portable host starts.

| Argument | Behavior and limits |
| --- | --- |
| `--android-native-geometry 0` | On by default where the build has the verified 2025-11-12 layout: the shared NEON geometry kernels, which compute what the original's SSE path does (the translated code keeps x87 intermediates). Unsupported state falls back. 0 turns it off. |
| `--android-readback 1` | Forces Delayed for the occlusion check (below), for the keyed visibility experiments that require it. |
| `--android-encode-cache 0` | On by default: reuses identical command bindings and descriptors, with invalidation. 0 turns it off. |
| `--android-pass-plan 1` | Experimental pass planner, from pass dependencies. It runs fewer passes but has not shown a useful FPS gain on its own. |
| `--android-visibility-storage 1` | Experimental visibility transform (preserved masks, full pixels). Requires readback 1, worker, planner and bounded area 0, and no pass or query trace. Crowd scenes ran at about the same speed in earlier trials. |
| `--android-render-worker 1` | Experimental FIFO render thread. Slower in earlier game trials. |
| `--android-worker-mailbox 1` | Mailbox for completed keyed visibility; requires worker 1 and readback 1. |
| `--android-worker-const-fx 1` | Skip worker drains for this back end's fixed Modern FX responses; requires worker 1. |
| `--android-cache-sampled 1`, `--android-bounded-area 1` | Experimental cache of sampled-image layouts, and bounded render areas. |
| `--android-fast-sync 1`, `--android-trim-uniforms 1`, `--android-dont-care-loads 1` | Experimental fence, uniform and attachment-load controls. Each needs its own correctness and timing checks. |
| `--android-bench-dir PATH` | Write frame times and markers (`frames.csv`, `markers.jsonl`) to PATH; refuses to overwrite either file. One branch per frame when off. |
| `--android-control 54300` | Enable the existing control port, loopback only; default 0. |
| `--fps-divisor 0` | Uncapped, for measurements; divisor 2 is 30 FPS. |

The diagnostic statistics (`--android-worker-stats`,
`--android-visibility-storage-stats`), pass traces and query diagnostics add
overhead; leave them out of performance comparisons. The older geometry
diagnostics in the generated code (scalar, capture) are not included, and the
Android entry rejects their options.

The game's occlusion check is Config > Modern's Occlusion Check, live, and kept
as `occlusion` in `modern.cfg` in the data folder: 0 Off (the default, as on desktop) answers that
everything is visible, without waiting for the GPU; 1 Delayed uses each probe's
answer the GPU finished in an earlier frame, up to 16 frames old, and reads
exactly where the probe's lifetime is unknown; 2 Exact waits for the GPU every
probe. On the Pixel Fold, in a camera sweep through Markets and the crowd and
spell scenes (alternating runs), Off was 3.5-5.4% faster than Delayed in every
scene, with a shorter tail (player-crowd's 99th-percentile frame 38 ms against
63-68 ms); Exact runs at a fifth to a third of their speed.

## Correctness checks

Without the game or a device:

```sh
python3 tests/geometry_simd_test.py
python3 tests/geometry_guest_test.py
python3 tests/android_policy_test.py
python3 tests/android_build_test.py
python3 tests/android_entry_test.py
```

The policy test covers pass dependencies, fixed controls, resource versions,
worker snapshots and queues, completion, failure and shutdown, and the frame
collector. The build test covers object-cache invalidation, agreement between
the API level, TLS mode and library metadata, a fresh Java staging directory, and
cleanup after a packaging failure. The entry test covers the account
restriction, option mapping, build-feature checks and offscreen-test dispatch,
using stub host and SDL functions. None of them measures FPS. The shared
geometry CI runs on macOS, Linux (GCC and Clang) and Windows (MSVC); the macOS
and Linux jobs also run the policy, build and entry tests.

To build the offscreen fixtures for a device, without installing the app:

```sh
python3 tools/build_android.py gfxtest --native-tls --test gfx
python3 tools/build_android.py gfxtest --native-tls --test gfx_format
python3 tools/build_android.py gfxtest --native-tls --test gfx_state
python3 tools/build_android.py gfxtest --native-tls --test gfx_async
python3 tools/build_android.py gfxtest --native-tls --test gfx_area
python3 tools/build_android.py gfxtest --native-tls --test gfx_pass_plan_gpu
python3 tools/build_android.py gfxtest --native-tls --test gfx_visibility_storage
python3 tools/build_android.py tls-test --native-tls
```

Copy the executables and `libSDL3.so` to a temporary directory of your own on
the device, run them with `LD_LIBRARY_PATH` set to that directory, then remove
the files. Set `FFXI_ANDROID_PASS_PLAN=1` for the planner fixture. The storage
fixture needs both `FFXI_ANDROID_VISIBILITY_STORAGE=1` and
`FFXI_ASYNC_READBACK=1`; without the second, storage stays off and the fixture's
checks fail, by design. The worker-mailbox checks use `FFXI_RENDER_WORKER=1`,
`FFXI_RENDER_WORKER_MAILBOX=1` and `FFXI_ASYNC_READBACK=1`. `tls-test` takes the
full path of its `libtls-test.so`.

On a Pixel Fold, with the installed app stopped, the fixtures pass: format and
state, keyed history and lifetime checks, bounded area, planner, encode cache,
and preserved-mask storage (44,093,573 checks per enabled run, three runs). The
keyed readback and worker-mailbox cases also pass in three separate processes.
These do not show that visibility in live, moving scenes is equivalent.

## Earlier performance results

Three earlier runs of all 29 scenes on a Pixel Fold, alternating between the
two builds, compared the upstream C renderer (as built for Android) with the
C++, NEON and keyed-readback build:

| Scene | Upstream C FPS | C++/NEON build FPS |
| --- | ---: | ---: |
| Bastok Markets / NPCs | 24.82 | 60.16 |
| Bastok Mines / NPCs | 22.96 | 57.72 |
| Mob crowd | 7.59 | 13.87 |
| 150-player crowd | 11.49 | 13.84 |
| Player effects | 9.03 | 10.46 |
| Mob spells | 7.52 | 14.26 |

These are per-scene medians, with the same resolution and textures, Normal
shadows and no active cooling; the sampled clocks and temperatures differed
between runs. Seven lighting and weather scenes were slower, including dawn
(125.48 to 90.60 FPS) and sunshine (81.66 to 61.52 FPS). The build changes
several things at once, so these figures are neither gains from the geometry
adapter alone nor predictions for the desktop. Heavy crowds still run below
30 FPS. The checks above are correctness and integration results, not new speed
results; desktop FPS benchmarks are on hold.
