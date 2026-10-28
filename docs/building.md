# Building

Building by hand, per platform. Players don't need this: the [launcher](install.md) does it for
them. Once built, see [Running `host64`](running.md).

## Inputs

Everything the build reads is in this repository.

| input | here |
| --- | --- |
| per-build metadata `ffxi-recomp-meta/1` (addresses and shapes, no bytes) | `meta/<module>.<build>.meta.json` |
| the builds, their hashes, and the few addresses the runtime and tests name | `meta/builds.json` (written to `generated/build.h`) |
| static unpacker for the packed `.text` | `tools/unpack.py` |
| the specifications the runtime implements: gamecore slots, the gamecore and D3D8 surfaces | `specs/` |

The metadata comes from a Ghidra-based discovery pass over each build's unpacked DLLs:
`discovery/` (Ghidra post-scripts and the per-build manual verdicts), run by `tools/discover.py`.
The committed metadata is what the build uses; the pass is only needed for a
[new build](new-client-version.md).

## macOS (arm64)

Copy the Windows install's `SquareEnix` folder (`FINAL FANTASY XI`, and the viewer folder beside
it if the install has one) to the Mac, e.g. `~/SquareEnix`, where the build tools look by default.

```
pip3 install capstone pefile     # SDL3 and mbedtls are vendored in third_party/, built with clang
python3 tools/build_posix.py prepare --game ~/SquareEnix/"FINAL FANTASY XI"
python3 tools/build_posix.py boot64  --game ~/SquareEnix/"FINAL FANTASY XI"   # boot test
python3 tools/build_posix.py host64  --game ~/SquareEnix/"FINAL FANTASY XI"   # build/host64
python3 tools/build_posix.py gfxtest     # Metal back end + D3D8 front end, offscreen, no game needed
build/gfx_test --window                  # the same, then two seconds of frames to a window
    # MTL_DEBUG_LAYER=1: Metal API validation
```

Run `prepare` again whenever the install changes (a new game version); `host64` and `boot64`
re-translate and rebuild what changed.

### `Final Fantasy XI.app`

`python3 tools/build_posix.py app` makes `build/Final Fantasy XI.app`: `host64` with its libraries
and `ffxi.reg`, and first-run defaults in its `Info.plist` so it starts from Finder with no
command line (`host/appdefaults.h`):

```
python3 tools/build_posix.py app --game <FINAL FANTASY XI folder> --server <name> \
    --resolution 2560x1440 --menu-resolution 1280x720 --window-mode 3 --background <picture>
```

The values go into the built app only. It is signed with the code-signing identity
`FFXI Local Code Signing` when the login keychain has one (or `--sign-identity`), else ad hoc. Ad hoc,
every rebuild is a new app to macOS, which then asks again before the app reads its saved password;
signed with one certificate, "Always Allow" holds. `tools/setup.py` makes it on its first run. By hand, make the certificate once (self-signed; it needs no
trust settings), with a `cs.cnf` of:

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

Vulkan 1.3 (`runtime/portable/gfx_vulkan.c`), SDL3 from the system (3.2 or later, through
pkg-config), glslang for the shaders. `tools/linux/Dockerfile` is a box with all of it, and Mesa's
software Vulkan for the tests:

```
docker build -t xi-linux tools/linux
docker run --rm -v "$PWD":/src -w /src xi-linux python3 tools/build_posix.py gfxtest
docker run --rm -v "$PWD":/src -v "<FINAL FANTASY XI>":/game:ro -w /src xi-linux \
    sh -c 'python3 tools/build_posix.py prepare --game /game && python3 tools/build_posix.py host64 --game /game'
    # FFXI_VK_VALIDATION=1: the Khronos validation layer
```

`python3 tools/build_posix.py desktop --game <FINAL FANTASY XI>` then puts `build/host64` in the
applications menu (`~/.local/share/applications/xi-on-anything.desktop`, named for host64's app id),
so docks and window switchers show the game's icon for its windows.

The kit the launcher builds the Steam Deck's client from is `tools/linux/kit.sh` (zig, static SDL3
and glslang; CI publishes it as the `kit-linux` pre-release): everything that is not the game,
prebuilt, so the player's machine needs only zig and python3 to make `host64` from their files
(`build_posix.py host64 --kit`).

## Windows

```
python tools\prepare.py [--game "<FINAL FANTASY XI>"]   # identify the build, unpack into generated\
python tools\build.py difftest   # x86: translate the CRT slice, differential test
python tools\build.py host       # x86: the stand-in FFXiMain.dll + boot test
python tools\build.py boot64     # x64: the portable runtime + boot test
python tools\install.py install  # put the stand-in in the game folder (restore: undo)
python tools\trace_report.py <FFXiMain.trace.txt> [--seq <FFXiMain.seq.txt>]   # resolve a boundary trace
python tools\build.py host64     # x64: the game host (SDL3 at C:\Dev\SDL3)
```

## Android (experimental)

See [android.md](android.md).
