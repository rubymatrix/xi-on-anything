# Building

Building by hand. Players don't need this: the [launcher](install.md) builds it for them (on
Windows, the
[setup](https://github.com/rubymatrix/xi-on-anything/releases/latest/download/XI-on-Anything-windows-x64-setup.exe)).
Each platform has a [guide](platforms/README.md). To run the result, see [Running `host64`](running.md).

## Inputs

Everything the build reads is in this repository.

| input | here |
| --- | --- |
| per-build metadata `ffxi-recomp-meta/1` (addresses and shapes, no bytes) | `meta/<module>.<build>.meta.json` |
| the builds, their hashes, and the few addresses the runtime and tests name | `meta/builds.json` (written to `generated/build.h`) |
| static unpacker for the packed `.text` | `tools/unpack.py` |
| the specifications the runtime implements: gamecore slots, the gamecore and D3D8 surfaces | `runtime/specs/` |

The metadata comes from a Ghidra discovery pass over each build's unpacked DLLs: `discovery/`
(Ghidra post-scripts and per-build manual verdicts), run by `tools/discover.py`. The build uses the
committed metadata; you need the pass only for a [new build](new-client-version.md).

## macOS (arm64)

Copy the Windows install's `SquareEnix` folder (`FINAL FANTASY XI`, plus the viewer folder if
there is one) to `~/SquareEnix` on the Mac. The build tools look there by default.

```
pip3 install capstone pefile     # SDL3 and mbedtls are vendored in third_party/, built with clang
python3 tools/build_posix.py prepare --game ~/SquareEnix/"FINAL FANTASY XI"
python3 tools/build_posix.py boot64  --game ~/SquareEnix/"FINAL FANTASY XI"   # boot test
python3 tools/build_posix.py host64  --game ~/SquareEnix/"FINAL FANTASY XI"   # build/host64
python3 tools/build_posix.py gfxtest     # Metal back end + D3D8 front end, offscreen, no game needed
build/gfx_test --window                  # the same, then two seconds of frames to a window
    # MTL_DEBUG_LAYER=1: Metal API validation
```

Run `prepare` again when the install changes (a new game version). `host64` and `boot64` rebuild
only what changed.

### `Final Fantasy XI.app`

`python3 tools/build_posix.py app` makes `build/Final Fantasy XI.app`: `host64` with its libraries,
`ffxi.reg`, `icon.icns`, `runtime.json` and the bundled `assets/textures`. First-run defaults go in
its `Info.plist` (`host/appdefaults.h`), so it starts from Finder with no command line:

```
python3 tools/build_posix.py app --game <FINAL FANTASY XI folder> --server <name> \
    --resolution 2560x1440 --menu-resolution 1280x720 --window-mode 3 --background <picture>
```

More defaults: `--fullscreen-space 0|1`, `--nameplates fix|off`, `--nameplate-scale <s>`,
`--ui-aspect <w:h|off>`, `--draw-distance <k>`, `--lod near|game`, `--cexi off|items|full`,
`--dats <folder>`.

The values go into the built app only. The app is signed with `--sign-identity`,
`FFXI_SIGN_IDENTITY`, or `FFXI Local Code Signing` if the login keychain has it; else ad hoc. Ad
hoc, macOS sees every rebuild as a new app and asks again before it reads the saved password. With
one certificate, "Always Allow" holds. `tools/setup.py` makes the certificate on its first run. To
make it by hand (once; self-signed, no trust settings needed), write a `cs.cnf`:

```
[req]
distinguished_name = dn
x509_extensions = ext
prompt = no
[dn]
CN = FFXI Local Code Signing
[ext]
basicConstraints = critical, CA:false
keyUsage = critical, digitalSignature
extendedKeyUsage = critical, codeSigning
```

then:

```
/usr/bin/openssl req -x509 -newkey rsa:2048 -nodes -keyout key.pem -out cert.pem -days 3650 -config cs.cnf
/usr/bin/openssl pkcs12 -export -inkey key.pem -in cert.pem -name "FFXI Local Code Signing" -out cs.p12 -passout pass:x
security import cs.p12 -k ~/Library/Keychains/login.keychain-db -P x -T /usr/bin/codesign
rm key.pem cs.p12
```

## Linux (x86_64)

Needs Vulkan 1.3 (`runtime/portable/gfx_vulkan.c`), the system's SDL3 (3.2 or later, through
pkg-config) and glslang. `platforms/linux/Dockerfile` has all of it, plus Mesa's software Vulkan for the
tests:

```
docker build -t xi-linux platforms/linux
docker run --rm -v "$PWD":/src -w /src xi-linux python3 tools/build_posix.py gfxtest
docker run --rm -v "$PWD":/src -v "<FINAL FANTASY XI>":/game:ro -w /src xi-linux \
    sh -c 'python3 tools/build_posix.py prepare --game /game && python3 tools/build_posix.py host64 --game /game'
    # FFXI_VK_VALIDATION=1: the Khronos validation layer
```

`python3 tools/build_posix.py desktop --game <FINAL FANTASY XI>` then adds `build/host64` to the
applications menu (`~/.local/share/applications/xi-on-anything.desktop`, named for host64's app
id), so docks and window switchers show the game's icon.

The launcher builds the Steam Deck's client from a kit, `platforms/linux/kit.sh` (zig, static SDL3 and
glslang; CI publishes it as the `kit-linux` pre-release). It holds everything but the game,
prebuilt, so the player needs only zig and python3 to build `host64` (`build_posix.py host64 --kit`).

## Windows

Step by step: [platforms/windows.md](platforms/windows.md).

```
python tools\prepare.py [--game "<FINAL FANTASY XI>"]   # identify the build, unpack into generated\
python tools\build.py difftest   # x86: translate the CRT slice, differential test
python tools\build.py host       # x86: the stand-in FFXiMain.dll + boot test
python tools\build.py boot64     # x64: the portable runtime + boot test
python tools\install.py install  # put the stand-in in the game folder (status, restore)
python tools\trace_report.py <FFXiMain.trace.txt> [--seq <FFXiMain.seq.txt>]   # resolve a boundary trace
python tools\build.py host64     # x64: the game host (SDL3 at C:\Dev\SDL3\SDL3-3.4.16, or SDL3_DIR)
python tools\build_mingw.py host64 [--out <folder>]   # the same with llvm-mingw (PATH or LLVM_MINGW)
```

`build.py` also builds `gfxtest` (the Direct3D 12 back end offscreen, no game), `host64-gl` and
`gfxtest-gl` (the OpenGL back end), `host64-headless` (no graphics) and `uwp-lib` (the Xbox app's
library). The Windows SDK's `dxc` is optional; without it, no ray tracing. `host64` downloads AMD's
FSR DLLs (about 57 MB); offline, it uses FSR 1.

## Android (experimental)

See [android.md](platforms/android-dev.md).

## PlayOnline sign-in on iPhone, iPad and Android

With no launcher to sign in first, the game's own sign-in screen can sign in to PlayOnline. First
it updates the install, as the launcher and the PlayOnline Viewer do, because PlayOnline's lobby
takes only the newest client. That sign-in and update are not in this repository. A build includes
them when `XI_POL` names their folder. Its `xi-build.json` lists the C sources to compile (one
implements [`host/signin_pol.h`](../host/signin_pol.h)) and a Rust crate to link.

```sh
XI_POL=/path/to/pol python3 tools/build_ios.py app
XI_POL=/path/to/pol python3 tools/build_android.py host --native-tls
```

`cargo` builds the crate for the build's target, so add the targets once
(`rustup target add aarch64-apple-ios aarch64-apple-ios-sim aarch64-linux-android`). Without
`XI_POL`, the screen signs in to LandSandBoat servers only.

Settings then has a **Sign-in Method** row; PlayOnline has no server to pick. The sign-in page
asks for the **Square Enix ID** and password, the **PlayOnline ID** and password, and the one-time
password if the account has one. Remember Password keeps both passwords in the keychain (on Android,
encrypted with a key the Android Keystore holds).

A phone can't rebuild the client. The app is recompiled from one `FFXiMain.dll` (`FFXI_SHA256` in
`generated/build.h`). The updater still updates the files, but if an update brings a new
`FFXiMain.dll`, the sign-in stops and asks for a new build of the app. Patch the Mac's install (the
launcher does), run `prepare`, and build the app again.
