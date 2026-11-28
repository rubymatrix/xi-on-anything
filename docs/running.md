# Running `host64`

`host64` does what the retail launcher and COM do on Windows: it maps the game's DLLs, starts
them, and runs the game with its own gamecore in place of the retail one. Options are
`--name value` pairs. To build it, see [Building](building.md). Every in-game setting is in the
[options catalog](options.md).

```
build/host64 --game <FINAL FANTASY XI folder> [options]
```

## The sign-in screen

Without a way in on its command line (`--session`, `--pol-status stdio`, or `--user` with a
password or token), `host64` shows its own sign-in screen first. It uses the game's UI art from the
install (window themes, font, title art; `host/ui/datui.c`), in the window the game then takes over.
It signs in to a **LandSandBoat server** (username, password, one-time code). Its **Settings** hold
the server, the window theme, and whether to remember the password (in the macOS Keychain, the
Windows Credential Manager, or the Secret Service on Linux). It keeps its files in its data folder
(or `--data-dir`):

| system | data folder |
| --- | --- |
| macOS | `~/Library/Application Support/FFXIRecompile/FFXI/` |
| Windows | `%APPDATA%\FFXIRecompile\FFXI\` |
| Linux | `~/.local/share/FFXIRecompile/FFXI/` |

There: `signin.cfg`, `settings.reg` (the display settings, written once with defaults), `saved.reg`
(the game's own saves), `modern.cfg`, `addons.cfg` and the addon folders (`xi`, `ashita`,
`windower`), an optional `textures/`, `patch.<version>.ver` for an install without a `patch.ver`,
`entryz.dic` (an empty name dictionary, which character creation needs) for one without the viewer's,
`host64.log` when started from Finder, and an optional `background.png`/`.jpg` behind the screen.
Closing the window or Ctrl+C quits at any point (on macOS also Cmd+Q and the Dock's Quit).

The button art is the screen's own (`tools/make_ui_art.py`, `assets/ui/`);
`third_party/stb/stb_image.h` (public domain) reads pictures.

## Signing in from the command line

To sign in to a LandSandBoat server without the sign-in screen:

```
build/host64 --game ~/SquareEnix/"FINAL FANTASY XI" \
    --server <server name> --user <account> \
    --reg assets/ffxi.reg --reg-overlay build/settings.reg --dats ~/FFXI/DATs
```

## Options

| option | what it does |
| --- | --- |
| `--game <folder>` | **Required**, except in a macOS app bundle with an `FFXIGameFolder` default. The `FINAL FANTASY XI` folder, with the viewer folder beside it if the install has one. On macOS both are mounted where a retail Windows install puts them. |
| `--server <name or a.b.c.d>` | Where the game's servers are. The lobby and every other host under the game's domain resolve here, not through DNS; the LandSandBoat sign-in connects here too. Default `127.0.0.1`. `--lobby` is an older name. With `--session` or `--pol-status` it names a private PlayOnline server: the launcher signed in there, and the game's hosts go there too. |
| `--session <V>` | A session value (16 characters, or 32 hex digits) from a launcher that signed in and keeps the sign-in open while the game runs. Skips the sign-in screen. The game's hosts resolve through DNS, unless `--server` is given (a private PlayOnline server). |
| `--auth <block>` | With `--session`: the 0x34-byte authCode block the game sends its lobby (104 hex digits), as the launcher made it. Passed to the game unchanged. Without it the block is zeros, which a LandSandBoat lobby accepts. |
| `--pol-status stdio` | For a launcher that holds a PlayOnline session, in place of `--session`. The game's own PlayOnline requests go to the launcher, as retail's core sends them: the status requests (the lobby's, which brings the session value; the one naming the character at character select; the one on leaving the world) and the message client's (list, read and delete messages, which the game keeps under the Viewer's `pub\homeNN\msg` folders). One line out on stdout per request, `[pol-req] begin <id> <verb> <data hex>`, and one back on stdin, `pol-req <id> <result> [<payload hex>]`. `FFXI_POL_CONTENTS` carries the member's content table (64 records of 0x68 bytes, 13312 hex digits), `FFXI_POL_HANDLES` its handle table (64 records of 0x28 bytes, 5120 hex digits) and `FFXI_POL_STATUS` its status, id and the key its ids are kept under (`<handle> <chr> <mode> <openstat> <class> <id hex> <key hex>`), as the game's core hands them out. The game's hosts resolve through DNS, unless `--server` is given (a private PlayOnline server). |
| `--user <account>` | Sign in to a LandSandBoat server with this account before anything loads. |
| `--pass <password>` | The account's password. Without it, `host64` reads `FFXI_PASSWORD`, else the sign-in screen asks. Prefer those: a password on the command line lands in your shell history. |
| `--otp <code>` | The two-factor code, for an account that has one. |
| `--login-token <token>` | A single-use token from a server's own launcher (a Discord login, say), in place of the password. |
| `--trust on\|off` | xiloader's "trust this computer": after a two-factor code, the server hands out a token that replaces the code for 30 days. It is kept in the keychain per `--server` name and account (the sign-in screen's Settings has the same switch). Without `--server`, or without a password store (a Linux desktop with no Secret Service), nothing is kept. |
| `--authport`, `--dataport`, `--viewport` | LandSandBoat's ports, by default 54231 (sign-in, TLS), 54230 (data), 54001 (lobby view). |
| `--loader-version <a.b.c>` | The loader version sent at sign-in, `2.2.0` by default (current xiloader). LandSandBoat refuses a version it does not expect. When the refusal names the one it wants (`2.1.x`), `host64` retries once with that (an `x` as 0). The sign-in screen remembers it (`loader_version=` in `signin.cfg`). |
| `--reg <file.reg>` | A registry export to load (up to 8; later files win). The game reads its settings (resolution, window mode, sound) from its registry keys. Start from this repo's `assets/ffxi.reg`. |
| `--reg-overlay <file.reg>` | Where the game saves settings it changes. Loaded after the `--reg` files, which are never rewritten. |
| `--data-dir <folder>` | Where `host64` keeps its own files (see above). Default: the sign-in screen's data folder; with no sign-in screen, none (the `patch.<version>.ver` and `entryz.dic` then go beside `host64`). |
| `--reg-final <file.reg>` | Loaded after the overlay, so it wins over what the game saved (up to 8). Without one, `<data dir>/settings.reg` is loaded here. |
| `--dats <folder>` | DAT overlays, the way XIPivot does them (up to 8; the first folder given wins). See below. |
| `--textures <folder>` | A texture pack: high-resolution replacements for the game's textures (up to 8; the first with a texture wins). Default `<data dir>/textures`. The bundled packs come after these. |
| `--user-dir <folder>` | The game's `USER` folder (settings, macros) here instead of in the install. |
| `--fps-divisor <n>` | The game's frame divisor: `1` is 60 fps (the default here), `2` is 30 fps as shipped, `0` uncapped. Uncapped keeps the world at its speed in game builds with frame sites (`tools/version/frame_sites.py`); in others the world runs as fast as the frames (for measurements). `--fps-limit <n>` holds uncapped to n a second. The 2009 and 2010 clients have no divisor: `1` runs their frame loop at one sixtieth in the world (`frame60`), the title and lobby at 30. |
| `--features` | Print which of `host64`'s additions this game build has, as JSON (`id`, `title`, `on`, `why`), and exit. From `recomp/meta/features.json`; see [Features by client build](options.md#features-by-client-build). |
| `--ui-aspect <w:h>` | Keep the interface at this shape, full height and centered, in a wider window (`16:9` on an ultrawide), not stretched. The 3D world still fills the window. The mouse is mapped to match, so the sides can't be clicked. Off by default; an app bundle's `FFXIUIAspect` key sets the default. Best with a 16:9 menu resolution (960x540). |
| `--nameplates fix\|off` | The names over characters' heads. The game sizes them by the window's width and height, so they widen with the window (1.8 times at 3440x1440). `fix`, the default, keeps their 4:3 shape. Only builds with a `nameplate_scale` hook in `recomp/meta/builds.json`. |
| `--nameplate-scale <s>` | Their size: `1.25`, or across x down (`1x1.2`). 1 by default. |
| `--draw-distance <k>` | How far the world is drawn: a factor from 0.5 to 20 (1 as the game draws), or world x characters (`3x1.5`). |
| `--lod near\|game` | `near`, the default, draws the world's most detailed models at every distance; `game` lets the game pick. |
| `--cexi off\|items\|full` | A CatsEyeXI-style server's custom item and gear ids (`items`), and its spells and abilities too (`full`). Off by default. |
| `--addon-harness <script>` | Developers: run addons with this script, without the game (`host/addons/harness.c`). |

The game writes its `USER` folder in the install unless `--user-dir` moves it; nothing else there
is written. The registry's install paths are set to where the game actually is. An install with no
`patch.ver` (common for private servers) gets one for its build's version, `patch.<version>.ver`,
in the data folder, else next to `host64`.

## DAT overlays (`--dats`)

Private servers often ship their own DATs: era item and spell text, zones, menus. `--dats` loads
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

When the game opens a path through a `ROM<n>\` or `sound<n>\` folder, `host64` looks for the rest
(`ROM2\14\5.DAT`, ignoring case) in the overlays first, then in the install. Folders only in an
overlay (`ROM255` above) work too. Overlays are indexed once at start-up, and the log lists each:
`[recomp] dats: era-dats, 163 files`.

## Environment variables

| variable | what it does |
| --- | --- |
| `FFXI_PASSWORD` | The LandSandBoat password when `--pass` is not given. |
| `FFXI_DATS_TRACE=1` | Log every file an overlay supplies: `[dats] <game path> -> <overlay file>`. |
| `FFXI_PROFILE=1` | Every 2 seconds, log a frame breakdown (game code, API calls, draws, GPU time) and the most-called APIs. |
| `FFXI_FPS=0` | Hide the frame-rate overlay. A saved fps setting from Config > Advanced has the last word. |
| `FFXI_FPS_LIMIT=<n>` | Hold uncapped frames to n a second, over the setting. |
| `FFXI_FX_FILE=<file>` | The saved graphics settings file, in place of `fx.txt` in the cache folder. |
| `FFXI_ADDONS=0` | No addons. |
| `FFXI_BUNDLED_TEXTURES=0` | Skip the bundled texture packs. |
| `FFXI_GPU=<name>` | Vulkan: prefer the GPU whose name contains this. |
| `FFXI_RT_OFF=1` | Vulkan: no ray tracing. |
| `FFXI_NET_TRACE=1` | Log every connect and the first bytes of every packet; `2` logs whole packets. It logs sign-in hashes too. |
| `FFXI_D3D12_DEBUG=1` | Direct3D 12 debug layer. |
| `FFXI_VK_VALIDATION=1` | Vulkan: the Khronos validation layer. |
| `FFXI_DISCORD=0` | No Discord Rich Presence. |
| `FFXI_DISCORD_NAME=0` | Rich Presence without the character's name (jobs and zone only). |
| `FFXI_DISCORD_APP_ID` | The Discord application the presence is shown as, in place of the built-in one. |
| `FFXI_PROBE=gpu` | Read the game's 16×16 occlusion probe from the GPU. By default it answers "visible" at once, saving 7–8 ms a frame. |
| `FFXI_PROBE_DRAW=1` | Draw the characters' occlusion probes though nothing reads them. By default the 2025-11-12 build skips them: up to 3 ms of GPU time a frame in crowds. |
| `FFXI_FADE_SCISSOR=0` | Composite a fading character's scratch target over the whole scene, not just the part it was drawn in. |
| `FFXI_DRAWLOG=<file>` | While `<file>.go` exists, write the next frame's draws to `<file>` (guest-stack return addresses, texture, vertex box), then remove `.go`. Shows which game code draws what. |
| `FFXI_GPU_TIMES=<file>` | Vulkan: GPU timestamps around each run of draws from one game call site, each effects pass, the sun maps and present. Each second, `<file>` gets the GPU time and draws per site (`t <epoch> frames <n> gpu_ms <ms>`, then `<site> <ns> <draws> <runs>`; a site is the draw's return address, with the next return address up the stack above it). |
| `FFXI_ASYNC_READBACK=1` | Small read-only surface locks take the newest finished copy instead of waiting for the GPU. |
| `FFXI_VSYNC=0` | Never wait for the display, to measure frame rates past its refresh rate. |
| `FFXI_NATIVE_GEOMETRY=1` | Skin characters with SSE-order kernels (NEON or SSE2, `runtime/portable/geometry/geometry_simd.c`) instead of the translated x87 code, on builds with a verified layout (2025-11-12). Same output as the original's SSE path; no measured FPS gain on desktop. |
| `FFXI_CACHE_DIR` | Where `fx.txt` and the pipeline cache go. Default `~/Library/Caches/FFXI` on macOS, `$XDG_CACHE_HOME/FFXI` (or `~/.cache/FFXI`) on Linux, `%LOCALAPPDATA%\FFXI` on Windows. |
| `FFXI_RECOMP_TRACE=1` | Log every shim call, and every failed `CreateFileA` / `FindFirstFileA` path. |
| `FFXI_RECOMP_MISSING=1` | Log imports that have no shim. |
| `MTL_DEBUG_LAYER=1` | Metal API validation. |
