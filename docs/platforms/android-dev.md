# Android (experimental)

These are the developer notes. To install and play, see [android.md](android.md).

The Android build runs the recompiled client natively on ARM64, through SDL3 and a C++ Vulkan back
end. It uses no Wine, GameNative, FEX or DXVK. It shares the geometry adapter with the desktop
hosts; the desktop Metal, D3D12 and C Vulkan back ends keep their own build paths.

The APK is a debug client. It opens on the sign-in screen, as the desktop and iPhone builds do, or
signs in to a LandSandBoat server with `--user` and `--pass` from `run.args`. `--session` and
`--auth` are refused. Remember Password encrypts each password with an AES key that the Android
Keystore makes and holds (`GameActivity.keychainSet`, `host/signin/keychain.c`). Only the ciphertext is
stored, in the app's private preferences; no password is written in the clear.

The APK contains no password, game files, captures or generated game code: bring your own client
installation and server. The C++ back end does not implement the Modern FX extensions; the game's
own D3D8 lighting, fog, textures and shadows work. Config > Advanced shows Display (Frame Rate,
Interface Shape and the window and resolution settings), Interface and Gamepad. The addon host is
off on Android, so Addons is shown only with `--android-addons 1`. You play with the touch controls
below, a controller, or a keyboard.

## From the launcher

The launcher's phone page (Phone > Android) does everything below for a player, on macOS, Linux or
Windows. It fetches the Android tools: the NDK, SDK platform, build tools, platform tools and CMake
from Google's repository, pinned by checksum, and Java 17 if there is none. It lists the devices
`adb` sees, and says what to do about USB or wireless debugging. Then it runs

```sh
python3 tools/build_android.py install --device SERIAL --keystore KEY.p12 --key-alias xi
```

with the key's password in `XI_ANDROID_KEYSTORE_PASS`. `install` builds the dependencies once
(`android_deps.py`), the API 29 host and the APK, then installs it with `adb install -r -d`, which
keeps the app's files. The launcher makes one signing key per player and asks them to back it up:
Android installs an update only when it is signed with the same key as the installed app. Without
`--keystore`, `build_android_apk.py` signs with a test key in the dependency cache, for development
only.

The APK's `versionName` is `runtime.json`'s version and its `versionCode` the game build's digits
(20261102). The launcher reads them back (`dumpsys package`) to tell whether the phone's app is
current, as it does with the iPhone app's `CFBundleShortVersionString` and `CFBundleVersion`.

Without `run.args`, the app runs the game in its external files folder
(`/sdcard/Android/data/com.rubymatrix.xionanything/files/FINAL FANTASY XI`, the PlayOnline viewer
beside it; an older `game` folder there still works). If `game-dir.txt` is there, it names the
folder instead (one line: the folder holding `FINAL FANTASY XI`). The app uses the base registry it
carries (`ffxi.reg`) and passes no `--server`, so the sign-in screen's server is used.

The launcher's Game Location picks that folder: the app's own (removed with the app), or
`XI on Anything` on internal storage or an SD card, which outlive the app and show in file managers.
The game reads it with plain paths. For a folder outside its own, the app asks for All files access
(`MANAGE_EXTERNAL_STORAGE`; the storage permission before Android 11) and waits for it before the
game starts.

The launcher copies the game there with `adb push`. Updates copy only the files changed since the
phone's `patch.ver`, with `patch.cfg` and then `patch.ver` last. An account sent from the launcher
goes into the app's private files through `adb shell run-as` and its stdin (this needs the
debuggable APK), never into shared storage. `host/signin/signin.c` reads that `import.json` once at start,
puts its passwords in the Keystore, removes it and fills in the sign-in screen. Phones never sign in
by themselves: the player taps Sign in. An account that needs a one-time password gets its code
field focused.

## Build from source

The tools run on macOS, Linux and Windows. Requirements: Python 3.12+ (`capstone`, `pefile`), CMake
and Ninja (the SDK's `cmake;3.22.1` package has both), JDK 17, Android SDK platform 36 and
build-tools 36.0.0, and NDK 27.0.12077973. Set the paths for your machine. `ANDROID_HOME` defaults to
`~/Library/Android/sdk`, the macOS location, so Linux and Windows must set it:

```sh
export ANDROID_HOME=/path/to/android-sdk
export ANDROID_NDK_HOME="$ANDROID_HOME/ndk/27.0.12077973"
export JAVA_HOME=/path/to/jdk17
python3 tools/build_posix.py prepare --game /path/to/FINAL-FANTASY-XI
python3 tools/android_deps.py
python3 tools/build_android.py host --native-tls
python3 tools/build_android_apk.py [--keystore your-key.p12 --key-alias xi]
```

`--keystore` needs the key's password in `XI_ANDROID_KEYSTORE_PASS`. Without it, packaging creates a
test signing key in the dependency directory. Packaging uses no Gradle: it runs `javac`, `d8`,
`aapt`, `zipalign` and `apksigner` directly.

LuaJIT is built with the NDK's own `make`, and its two host tools with `clang` (`XI_HOST_CC` names
another compiler). On Windows, MSVC's `cl` does not take LuaJIT's GCC-style flags. Point
`XI_HOST_CC` at llvm-mingw's clang, with forward slashes and no spaces in the path, for example
`XI_HOST_CC=C:/llvm-mingw-20250114-ucrt-x86_64/bin/clang.exe`.

`android_deps.py` downloads hash-pinned SDL3 3.4.16, glslang 16.5.0 and its SPIRV-Tools and
SPIRV-Headers dependencies, and builds them for ARM64. `--ndk` names another NDK and `--jobs` sets
the parallel jobs (default 6); `--api` takes only 28. Downloads and build products stay in
`build/android-deps` (ignored by git); `XI_ANDROID_DEPS` changes that directory.
`build_android.py deps` builds only the host's own libraries (mbedTLS, LuaJIT, ImGui, LuaSocket,
LuaFileSystem, SQLite); `host` and `install` build them too. `XI_ANDROID_BUILD_DIR` changes the
native output directory; pass the same directory to `build_android_apk.py --build-dir`.

The Android translation of FFXiMain.dll goes to `generated/android-all`; the desktop `generated/all`
is left as it is. Builds with `android_wraps` in `recomp/meta/builds.json` (only 2025-11-12) also get the
occlusion-probe wraps. FFXi.dll is translated from the same prepared files. All generated game code
stays ignored. `--native-tls` targets API 29 (Android 10) and its ELF TLS; without it the build
targets API 28 (Android 9) with emulated TLS. The object cache keys include the compiler command.
Packaging checks the library's hash against its build metadata and sets the matching minimum API.
Neither the build nor packaging installs an APK or changes device settings.

For PlayOnline sign-in, set `XI_POL` and add the `aarch64-linux-android` Rust target: see
[PlayOnline sign-in](../building.md#playonline-sign-in-on-iphone-ipad-and-android).

The APK is `build/android/xi-native-test.apk`, package `com.rubymatrix.xionanything`, named XI on
Anything on the home screen, with the iPhone app's icon. Before replacing an installed app, keep a
copy of its APK, data and configuration.

The app reads `run.args` from its external files folder: one literal argument per line, no shell
expansion. It replaces the whole default command line:

```
--game <game folder> --data-dir <files>/data --reg <app private files>/ffxi.reg
--reg-overlay <files>/data/settings.reg --user-dir <files>/data/USER --aspect off
```

So `run.args` must give `--data-dir` (without it the app exits with code 2), and `--game`, `--reg`,
`--reg-overlay`, `--user-dir` and `--aspect off` if you want them. Add `--server`, and `--user` and
`--pass` to skip the sign-in screen. Never commit that file.

## Screen, touch controls and keyboard

These are the iPhone build's ([ios.md](ios-dev.md)), shared through `FFXI_MOBILE`:

- **Screen:** `host/display_fit.c` sizes the game for the screen it is on: the back buffer at the
  safe area's pixels (beside a camera cutout) within a pixel budget (about 2.4 MP on a phone, 4 MP
  on a tablet or a Fold's inner screen; `FFXI_MAX_PIXELS` overrides it), and the interface at about a
  dp a pixel. It follows window changes, such as a Fold opened or closed. The frame is presented into
  the safe area and the pointer is mapped to match.
- **Touch controls:** `host/touch.c`, from the sign-in screen on: SDL's virtual gamepad, so the game
  sees an Xbox pad. A floating left stick, a drag elsewhere for the camera, PlayStation-shaped face
  buttons, the D-pad, L1/R1, L2/R2, SELECT/START and Target (F8); a quick tap off the controls is a
  click. The gamepad toggle hides them. The lock beside it (or a long press on the toggle) unlocks
  the layout, so you can drag each group where you like; it is kept per window shape in
  `touch-layout.txt` beside `run.args`. Pad, past the lock, opens Config > Gamepad (the button and
  stick mapping) over whatever is up, once the game's menus are in, and hides with the rest. Using a real controller hides the controls; a touch brings them
  back. On a tall screen they gather at the bottom.
- **Controller sticks:** Config > Gamepad's Sticks rows say which of the controller's own axes each
  stick axis is (`host/padmap.c`), for a controller SDL reads wrong (a handheld's built-in sticks).
  Select a row and push that stick right or down, or pick the axis with left and right; Back
  cancels. The button rows record the same way, from the next button pressed. The game ignores the
  controller while a row records. The result is an SDL gamepad mapping for that controller, applied
  at once and kept in `gamepads.txt` in the data folder. The page scrolls when it is taller than the
  interface.
- **Drawing:** the controls are drawn by a view over the game's surface (`GameActivity.Overlay`,
  fed by `platforms/android/android_overlay.c`), not by the Vulkan back end. They keep the display's rate when
  the game runs slower, and their labels use the system font. Touches pass through to SDL.
- **Back:** Android's Back, gesture or button, is Escape. It never closes the app, and Escape on the
  sign-in screen does not quit.
- **Keyboard:** the on-screen keyboard shows on the sign-in screen's fields, and in game only while
  you type in the chat line. A tap outside the text hides it and the next tap brings it back. The
  view pans to keep the field or chat line above it. The game's own soft keyboard (the PS2
  client's) is off.
- **Wallpaper:** `wallpaper.png` or `.jpg` beside `run.args` fills the bars beside the game,
  cropped to fill and dimmed. None is in the APK.

## Controls and defaults

The options below are off by default and take `0` or `1` unless stated otherwise. When comparing,
change one setting per trial and keep the scene, resolution and effects the same. The Android entry
consumes these arguments before the portable host starts.

| Argument | Behavior and limits |
| --- | --- |
| `--android-native-geometry 0` | On by default where the build has the verified 2025-11-12 layout: the shared NEON geometry kernels, which compute what the original's SSE path does (the translated code keeps x87 intermediates). Unsupported state falls back. 0 turns it off. |
| `--android-addons 1` | The addon host and Config > Advanced's Addons page (`FFXI_ADDONS`). Default 0. |
| `--android-readback 1` | Forces Delayed for the occlusion check (below), for the keyed visibility experiments that need it. |
| `--android-encode-cache 0` | On by default: reuses identical command bindings and descriptors, with invalidation. 0 turns it off. |
| `--android-pass-plan 1` | Experimental pass planner, from pass dependencies. It runs fewer passes but has not shown a useful FPS gain on its own. |
| `--android-visibility-storage 1` | Experimental visibility transform (preserved masks, full pixels). Needs readback 1 with the render worker, pass planner and bounded area all 0, and no pass trace or query diagnostics. Crowd scenes ran at about the same speed in earlier trials. |
| `--android-render-worker 1` | Experimental FIFO render thread. Slower in earlier game trials. |
| `--android-worker-mailbox 1` | Mailbox for completed keyed visibility; needs worker 1 and readback 1. |
| `--android-worker-const-fx 1` | Skips worker drains for this back end's fixed Modern FX responses; needs worker 1. |
| `--android-cache-sampled 1`, `--android-bounded-area 1` | Experimental cache of sampled-image layouts, and bounded render areas. |
| `--android-fast-sync 1`, `--android-trim-uniforms 1`, `--android-dont-care-loads 1` | Experimental fence, uniform and attachment-load controls. Each needs its own correctness and timing checks. |
| `--android-pass-trace FILE` | A one-frame pass trace, as JSON lines, to FILE. Creating `FILE.arm` traces the next frame. |
| `--android-probe-query-diag FILE` | One-frame occlusion-probe query diagnostics, as JSON lines, to FILE. Create `FILE.arm` once. |
| `--android-bench-dir PATH` | Writes frame times and markers (`frames.csv`, `markers.jsonl`) to PATH; refuses to overwrite either file. One branch per frame when off. |
| `--android-control 54300` | Enables the existing control port, loopback only; default 0. |
| `--fps-divisor 0` | Uncapped, the world kept at its speed where the game build has frame sites (`tools/version/frame_sites.py`), else as fast as the frames (for measurements); divisor 2 is 30 FPS. |

The diagnostic statistics (`--android-worker-stats`, `--android-visibility-storage-stats`), pass
traces and query diagnostics add overhead; leave them out of performance comparisons. The older
geometry diagnostics in the generated code (scalar, capture) are not included, and the Android
entry rejects their options.

The game's occlusion check has no menu setting. It is Off, as on desktop: it answers that
everything is visible, without waiting for the GPU. Delayed (1) uses each probe's answer that the
GPU finished in an earlier frame, up to 16 frames old, and reads exactly where the probe's lifetime
is unknown. Lifetimes are known only with the occlusion-probe wraps, so on builds without
`android_wraps` (all but 2025-11-12) Delayed reads every probe exactly. Exact (2) waits for the GPU
on every probe. On the Pixel Fold, in a camera sweep through Markets and the crowd and spell scenes
(alternating runs), Off was 3.5-5.4% faster than Delayed in every scene, with a shorter tail
(player-crowd's 99th-percentile frame 38 ms against 63-68 ms); Exact runs at a fifth to a third of
their speed. Delayed and Exact were Config's Occlusion Check, now removed. Desktop never had
Delayed: there the late answer is the render target's newest copy, not each probe's, and makes
characters flicker.

## Correctness checks

Without the game or a device:

```sh
python3 tests/geometry/geometry_simd_test.py
python3 tests/geometry/geometry_guest_test.py
python3 platforms/android/tests/android_policy_test.py
python3 platforms/android/tests/android_build_test.py
python3 platforms/android/tests/android_entry_test.py
```

Visibility storage's shaders are compiled into `gfx_visibility_storage.inc` from the GLSL beside
it. After changing one, rebuild the arrays with glslang 16.5.0 (the version `android_deps.py`
pins). Without `--write` the tool only checks them:

```sh
python3 tools/visibility_storage_spv.py --glslang /path/to/glslang --write
```

The policy test covers pass dependencies, fixed controls, resource versions, worker snapshots and
queues, completion, failure and shutdown, and the frame collector. The build test covers
object-cache invalidation, agreement between the API level, TLS mode and library metadata, a fresh
Java staging directory, and cleanup after a packaging failure. The entry test covers the sign-in
paths (the screen, an account, a refused session), the touch controls' start, option mapping,
build-feature checks and offscreen-test dispatch, using stub host and SDL functions. None of them
measures FPS.

CI runs these only when a release is published. `.github/workflows/android.yml`'s `policy` job runs
the policy, build and entry tests on macOS (Clang) and Linux (GCC and Clang). Its `android-build`
job installs the NDK, runs `android_deps.py`, and builds the `gfx_async` fixture and `tls-test`. The
geometry tests run separately, in `geometry-simd.yml`, on macOS, Linux (GCC and Clang) and Windows
(MSVC).

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

Copy the executables and `libSDL3.so` to a temporary directory of your own on the device, run them
with `LD_LIBRARY_PATH` set to that directory, then remove the files. Set `FFXI_ANDROID_PASS_PLAN=1`
for the planner fixture. The storage fixture needs both `FFXI_ANDROID_VISIBILITY_STORAGE=1` and
`FFXI_ASYNC_READBACK=1`; without the second, storage stays off and the fixture's checks fail, by
design. The worker-mailbox checks use `FFXI_RENDER_WORKER=1`, `FFXI_RENDER_WORKER_MAILBOX=1` and
`FFXI_ASYNC_READBACK=1`. `tls-test` takes the full path of its `libtls-test.so`.

On a Pixel Fold, with the installed app stopped, the fixtures pass: format and state, keyed history
and lifetime checks, bounded area, planner, encode cache, and preserved-mask storage (44,093,573
checks per enabled run, three runs). The keyed readback and worker-mailbox cases also pass in three
separate processes. This does not show that visibility in live, moving scenes is equivalent.

## Earlier performance results

Three earlier runs of all 29 scenes on a Pixel Fold, alternating between the two builds, compared
the upstream C renderer (as built for Android) with the C++, NEON and keyed-readback build:

| Scene | Upstream C FPS | C++/NEON build FPS |
| --- | ---: | ---: |
| Bastok Markets / NPCs | 24.82 | 60.16 |
| Bastok Mines / NPCs | 22.96 | 57.72 |
| Mob crowd | 7.59 | 13.87 |
| 150-player crowd | 11.49 | 13.84 |
| Player effects | 9.03 | 10.46 |
| Mob spells | 7.52 | 14.26 |

These are per-scene medians, with the same resolution and textures, Normal shadows and no active
cooling; the sampled clocks and temperatures differed between runs. Seven lighting and weather
scenes were slower, including dawn (125.48 to 90.60 FPS) and sunshine (81.66 to 61.52 FPS). The
build changes several things at once, so these figures are neither gains from the geometry adapter
alone nor predictions for the desktop. Heavy crowds still run below 30 FPS. The checks above are
correctness and integration results, not new speed results; desktop FPS benchmarks are on hold.
