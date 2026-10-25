# Running `host64`

`host64` does what the retail launcher and COM do for FFXI on Windows: it maps the game's DLLs,
starts them, and runs the game with its own gamecore in place of the retail core library. It
takes its options as `--name value` pairs. To build it, see [Building](building.md); every
in-game setting is in the [options catalog](options.md).

```
build/host64 --game <FINAL FANTASY XI folder> [options]
```

## The sign-in screen

Started without a way in on its command line (no `--session`, nor `--user` with a password or token),
`host64` shows its own sign-in screen before the game, in the game's own UI art read from the
install (window themes, font, title art; `host/datui.c`), in the window the game then takes over.
It signs in to a **LandSandBoat server** (username, password, one-time code), with the server,
whether to remember the password (the system's password store: the macOS Keychain, the Windows
Credential Manager, or the Secret Service on Linux) and the window theme in its **Settings**. It
keeps its files in `~/Library/Application Support/FFXIRecompile/FFXI` (or `--data-dir`):
`signin.cfg`, `settings.reg` (the display settings, written once with defaults), `saved.reg` (the
game's own saves), `host64.log` when started from Finder, and an optional `background.png`/`.jpg` behind the
screen. Cmd+Q, the Dock's Quit and Ctrl+C quit at any point.

The button art is the screen's own
(`tools/make_ui_art.py`, `assets/ui/`); `third_party/stb/stb_image.h` (public domain) reads
pictures.

## Signing in from the command line

To sign in to a LandSandBoat server from the command line instead of the sign-in screen:

```
build/host64 --game ~/SquareEnix/"FINAL FANTASY XI" \
    --server <server name> --user <account> \
    --reg ffxi.reg --reg-overlay build/settings.reg --dats ~/FFXI/DATs
```

## Options

| option | what it does |
| --- | --- |
| `--game <folder>` | **Required.** The `FINAL FANTASY XI` folder, with the viewer folder beside it if the install has one. On macOS both are mounted where a retail Windows install puts them. |
| `--server <name or a.b.c.d>` | Where the game's servers are. The lobby and every other host under the game's domain resolve here instead of through DNS; the LandSandBoat sign-in connects here too. Default `127.0.0.1`. `--lobby` is an older name for it. |
| `--session <V>` | A session value for this sign-in (16 characters, or 32 hex digits), from a launcher that signed in and keeps that sign-in open while the game runs. Skips the sign-in screen; nothing is redirected and `--server` is not used: the game's hosts resolve through DNS. |
| `--auth <block>` | With `--session`: the 0x34-byte authCode block the game sends its lobby (104 hex digits), as the launcher that signed in made it. `host64` passes the bytes to the game unchanged. Without it the block is zeros, which a LandSandBoat lobby accepts. |
| `--user <account>` | Sign in to a LandSandBoat server with this account, before anything is loaded. |
| `--pass <password>` | The account's password. Without it, `host64` reads `FFXI_PASSWORD`, else the sign-in screen asks for it. Prefer those: a password on the command line ends up in your shell history. |
| `--otp <code>` | The two-factor code, for an account that has one. |
| `--login-token <token>` | A single-use launch token from a server's own launcher (for example a Discord login). It stands in for the password. |
| `--trust on` | xiloader's "trust this computer": with an account's two-factor code, the server hands out a token that stands in for the code for 30 days, kept in the keychain per `--server` name and account (the sign-in screen's Settings has the same switch). Where there is no password store (a Linux desktop without a Secret Service) nothing is kept. |
| `--authport`, `--dataport`, `--viewport` | LandSandBoat's ports, by default 54231 (sign-in, TLS), 54230 (data), 54001 (lobby view). |
| `--loader-version <a.b.c>` | The loader version sent at sign-in, `2.2.0` by default (current xiloader). LandSandBoat refuses a version it does not expect; when its refusal names the one it wants (`2.1.x`), `host64` signs in again once with that (an `x` as 0). The sign-in screen remembers the version (`loader_version=` in `signin.cfg`), including one a server asked for. |
| `--reg <file.reg>` | A registry export to load (up to 8; later files win). The game reads its settings (resolution, window mode, sound) from its own registry keys. `ffxi.reg` in this repo is a starting point. |
| `--reg-overlay <file.reg>` | Where the game saves settings it changes. It is loaded after the `--reg` files, and those are never rewritten. |
| `--data-dir <folder>` | Where `host64` writes its own files (the `patch.ver` it makes for an install without one). Default: beside `host64`. |
| `--reg-final <file.reg>` | Loaded after the overlay, so its values win over what the game saved (up to 8). Without one, the sign-in screen loads its `settings.reg` here. |
| `--dats <folder>` | DAT overlays, the way XIPivot does them (up to 8; the first folder given wins). See below. |
| `--fps-divisor <n>` | The game's frame divisor: `1` is 60 fps (the default here), `2` is 30 fps as shipped, `0` uncapped: the world kept at its speed in game builds with frame sites (`tools/frame_sites.py`), else as fast as the frames (for measurements). `--fps-limit <n>` holds uncapped to n a second. |
| `--aspect <auto, off or w:h>` | The 3D scene's aspect ratio. `auto` (the default) follows the window's shape, as Ashita's aspect addon does, so a widescreen or ultrawide window sees more to the sides instead of a 4:3 view stretched across it. `off` leaves it to the game; a shape (`16:9`, `1.778`) fixes it. |
| `--ui-aspect <w:h>` | Keep the interface at this shape, full height and centered, in a wider window (`16:9` on an ultrawide), instead of stretched across it. The 3D world still fills the window. The mouse is mapped to match, so the sides outside the box can't be clicked. Off by default (`off`); an app bundle's `FFXIUIAspect` key is the default. Best with a 16:9 menu resolution (960x540). |
| `--nameplates fix\|off` | The names over characters' heads. The game sizes them across by the window's width and down by its height, so they widen with the window (1.8 times at 3440x1440); `fix`, the default, keeps the shape they have in a 4:3 window. Builds with a `nameplate_scale` hook in `meta/builds.json` only. |
| `--nameplate-scale <s>` | Their size: `1.25`, or across x down (`1x1.2`). 1 by default. |

The install folder is never written. The registry's install paths are set to where the game
actually is, and an install that has no `patch.ver` (common for private-server installs) gets one
for its build's version, kept next to `host64` (or in `--data-dir`).

## DAT overlays (`--dats`)

A private server often ships its own DATs: era item and spell text, zones, menus. `--dats` loads
them without touching the install. A folder can be:

- **one overlay**: it holds `ROM`, `ROM2`, …, `sound`, `sound2`, … folders laid out like the
  install's; or
- **a folder of overlays**: each subfolder that holds those is an overlay, in name order.

```
DATs/
  era-dats/
    ROM/301/12.DAT
    ROM2/14/5.DAT
    ROM255/6/1.DAT
```

When the game opens a path through a `ROM<n>\` or `sound<n>\` folder, `host64` looks for the rest of
the path (`ROM2\14\5.DAT`, ignoring case) in the overlays first, then in the install. Folders that
exist only in an overlay (`ROM255` above) work too. The overlays are indexed once at start-up, and
the log reports each one: `[recomp] dats: era-dats, 163 files`.

## Environment variables

| variable | what it does |
| --- | --- |
| `FFXI_PASSWORD` | The LandSandBoat password when `--pass` is not given. |
| `FFXI_DATS_TRACE=1` | Log every file an overlay supplies: `[dats] <game path> -> <overlay file>`. |
| `FFXI_PROFILE=1` | Every 2 seconds, log a frame breakdown (game code, API calls, draws, GPU time) and the most-called APIs. |
| `FFXI_FPS=0` | Hide the frame-rate overlay. |
| `FFXI_DISCORD=0` | No Discord Rich Presence. |
| `FFXI_DISCORD_NAME=0` | Rich Presence without the character's name (jobs and zone only). |
| `FFXI_DISCORD_APP_ID` | The Discord application the presence is shown as, in place of the built-in one. |
| `FFXI_PROBE=gpu` | Read the game's 16×16 occlusion probe from the GPU. By default it answers "visible" at once, which saves 7–8 ms a frame. |
| `FFXI_DRAWLOG=<file>` | While `<file>.go` exists, write the next frame's draws to `<file>` (return addresses on the guest stack, texture, vertex box), then remove `.go`. For finding which game code draws what. |
| `FFXI_ASYNC_READBACK=1` | Small read-only surface locks take the newest finished copy instead of waiting for the GPU. |
| `FFXI_VSYNC=0` | Never wait for the display, for measuring frame rates past its refresh rate. |
| `FFXI_NATIVE_GEOMETRY=1` | Skin characters with SSE-order kernels (`runtime/portable/geometry_simd.c`) (NEON or SSE2) instead of the translated x87 code, on builds with a verified layout (2025-11-12). The same output as the original's SSE path; no measured FPS gain on desktop. |
| `FFXI_CACHE_DIR` | Where the pipeline cache goes. Default `~/Library/Caches/FFXI`. |
| `FFXI_RECOMP_TRACE=1` | Log every shim call, and every failed `CreateFileA` / `FindFirstFileA` path. |
| `FFXI_RECOMP_MISSING=1` | Log imports that have no shim. |
| `MTL_DEBUG_LAYER=1` | Metal API validation. |
