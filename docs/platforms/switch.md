# Nintendo Switch (homebrew, experimental)

The game as a Switch homebrew app (`.nro`). You build it with devkitPro's libnx on your own PC from your
own game files, then copy it to the console's SD card with the game beside it.

Developer notes: [switch-dev.md](switch-dev.md).

> [!IMPORTANT]
> **Status**: experimental, playable on a console. The sign-in screen, the game in the world, battles
> and menus run on a Switch (Atmosphère), drawn with Mesa's OpenGL, with sound (music not yet
> confirmed). Shaders are kept on the card (Mesa 26, which the setup downloads), so each compiles once:
> a short hitch the first time an effect is ever seen, not every session. Not yet: the scene effects
> (Modern FX, water, sun shadows), addons, sleep and resume.

## What you need

- A Windows, Linux or macOS PC. The [Quick start](#quick-start)'s setup installs what follows when it is
  missing (on Windows: Python with winget, devkitPro with its own installer, then the packages; on Linux and
  macOS the packages, once devkitPro's pacman is in). By hand: [devkitPro](https://devkitpro.org/wiki/Getting_Started) and these
  packages: `pacman -S switch-dev switch-mesa switch-libdrm_nouveau switch-glad switch-zlib switch-libexpat`. On Windows, devkitPro's
  msys2 or your own msys2 with devkitPro's pacman repositories both work; set `DEVKITPRO` (or `XI_MSYS2`)
  if it is not in `C:\devkitPro`, `C:\msys64` or `D:\msys64`.
- Python 3.12+ with `capstone` and `pefile`.
- Your `FINAL FANTASY XI` folder and a LandSandBoat server ([README.md](README.md)).
- To run on a console: a Switch that runs homebrew (Atmosphère), started with **full memory**: hold R
  while starting a game to open the homebrew menu over it. The album's homebrew menu has too little memory.
- To run without a console: [Ryubing](https://git.ryujinx.app/projects/Ryubing/releases) with your
  console's `prod.keys` (dumped with Lockpick_RCM) in its `system` folder. Portable mode (a `portable`
  folder beside `Ryujinx.exe`) keeps its data and virtual SD card there. Turn on **Settings > Network >
  Enable guest Internet access**, or no name resolves, not even `127.0.0.1`, and the host exits with
  `--server: cannot resolve`. On macOS, `build_switch.py` finds `Ryujinx.app` in `/Applications` or
  `~/Applications` and its card in `~/Library/Application Support/Ryujinx`.

## Quick start

With the SD card in the PC:

- **Windows**: double-click `setup-switch.bat` in the `platforms\switch` folder.
- **Linux, macOS**: `./platforms/switch/setup-switch.sh`.

A window opens: pick your game folder and the SD card with **Browse...** (a card with Atmosphère on it is
listed by itself, and the server is filled in from your launcher), click **Install** beside *This PC* if it
says something is missing, then **Build and install**. With options (below) or `--console`, or without a
display or tkinter, it asks in the terminal instead.

<img src="../../screenshots/switch_setup.png" alt="The Switch setup window: the game folder, the SD card and the server filled in, building" width="560">

What's missing it offers to install, a yes each: Python (Windows, with winget), its `capstone` and
`pefile` (pip), devkitPro (Windows: it downloads devkitPro's installer and opens it: keep `C:\devkitPro`
and tick **Switch Development**; on Linux and macOS install [devkitPro's pacman](https://devkitpro.org/wiki/devkitPro_pacman)
first) and devkitPro's Switch packages (with its pacman; `sudo` asks for your password on Linux and macOS).

It asks for your `FINAL FANTASY XI` folder, the SD card (it finds a card with `atmosphere/` or `switch/`
on it by itself) and the server's address: the name or IP your launcher signs in to, which it offers
when it finds it near the game folder (an Ashita boot config's `--server`, VanaCore's `[server]`, a launch
script's `--server`); for a CatsEyeXI server it adds `--cexi full` to `run.args` (its own items, gear, spells
and abilities) unless `run.args` sets `--cexi` already. Then it does every step below in order: it checks the PC, identifies your game version, prepares its code,
finds its frame delta sites if this repository doesn't have them yet, builds the `.nro`, and copies the
`.nro`, the game and the settings to the card, and Sphaira for a
[home screen icon](#home-screen-icon). Then [run it](#3-run).

The options answer the questions instead, for a script or a second run:

```
python tools/setup_switch.py --game "<your FINAL FANTASY XI folder>" --sd E:\ --server <server name or IP>
```

`--layout xbox`, `--dats <folder>` and `--arg "<host options>"` are as in [step 2](#2-put-it-on-the-sd-card);
`--check` checks the PC and stops; `--no-copy-game` leaves the game on the card as it is; `--no-icon` (or
unticking **Home screen icon**) leaves Sphaira off the card. The answers are
remembered (`build/switch/setup.json`), and what each step printed is in `build/switch/setup.log`.

**Updating**: after a game update, or a pull of this repository, run it again. Each step skips what is
already done: the build compiles only what changed and the copy sends only files that differ. With only
the `.nro` new, `--no-copy-game` (or `build_switch.py deploy`, [step 3](#3-run)) is quicker.

A game version this repository doesn't know stops it at the start, with its `FFXiMain.dll`'s SHA-256: it
needs its own metadata first ([new-client-version.md](../new-client-version.md)).

## Manual steps

What `setup-switch` does, one command at a time.

### 1. Build

```
python tools/prepare.py --game "<your FINAL FANTASY XI folder>"
python tools/version/frame_sites.py
python tools/build_switch.py host
```

`prepare.py` identifies your game version and unpacks its code into `generated/`. `frame_sites.py` is only
needed when `recomp/meta/builds.json` has no frame delta sites for your version (no `frame_int_sites` in its entry;
a build without them logs `has no frame delta sites` at the start): it finds them and adds them to
`recomp/meta/builds.json`. With them the Switch runs uncapped with the world at its real speed; without them, at
30 fps ([Performance](#performance)). If it can't find them it says so and changes nothing: skip it.

`host` translates the game (the desktop translation, `generated/all`, shared with the PC builds) and
writes `build/switch/xi-on-anything.nro`. The first build compiles about 6 million lines of generated C
and takes a while; later builds compile only what changed. `XI_SWITCH_JOBS` (default 6) is how many
generated files compile at once; lower it if the PC runs out of memory.

### 2. Put it on the SD card

```
python tools/build_switch.py setup --sd E:\ --game "<your FINAL FANTASY XI folder>" --server <server name or IP>
```

`--sd` is the card's root: in a card reader, the console's card over USB (Hekate's USB Mass Storage), or
`emu` for the emulator's card. On Windows, `--mtp` copies to the console over USB instead. It copies:

- the `.nro` to `sdmc:/switch/xi-on-anything/`, where the homebrew menu lists it
- the game to `sdmc:/switch/xi-on-anything/SquareEnix/FINAL FANTASY XI` (and the viewer folder beside it,
  if your install has one): about 14 GB the first time. Run it again after a game update, or if a copy
  stopped part way: only files whose size or date differ are copied.
- `ffxi.reg`, and `switch.reg` over it: the Switch's defaults (a 1920x1080 window and background,
  windowed, menus at 960x540 so the UI is twice its size, readable on a TV and in handheld). What you
  change in game is saved to `settings.reg` beside them and wins over both. Config > Advanced's Display
  page keeps its own `data/settings.reg`, loaded last.
- `run.args` with the options you gave: an option given replaces its line there, and every other line
  (yours, added by hand) is kept

After the first copy, a copy of the game is skipped when the game folder hasn't changed since (a file
`.xi-on-anything-copy` on the card records what was copied); `--full-check` compares every file anyway.
`build_switch.py install` takes the same options and does steps 1's `host` and 2 in one, as
`build_android.py install` does for Android.

All options but `--nro`, `--mtp`, `--storage` and `--extra` go in `run.args`, one host argument per line.
Edit it on the card to change them later. `--layout` becomes `--switch-layout` there.

| Option | What it does |
| --- | --- |
| `--server <name or IP>` | The LandSandBoat server. Without it, the sign-in screen asks. |
| `--layout nintendo\|xbox` | `nintendo` (the default): A confirms and B cancels, as everywhere on the Switch. `xbox`: by position, B (the bottom button) confirms. |
| `--fps-divisor 0\|1\|2` | Leave it out: the default is the best there is (uncapped with frame delta sites, else 30). 1 (60 fps) or 2 (30) turn the frame delta sites off: see [Performance](#performance). |
| `--dats <folder>` | A DAT overlay folder, copied to the card and passed as `--dats`. |
| `--arg "<host options>"` | Any other [host option](../running.md#options), for example `--arg "--user me"`. |
| `--nro <file>` | The `.nro` to copy (default `build/switch/xi-on-anything.nro`). |
| `--mtp [device]` | Copy over USB to a console running DBI's MTP responder, by its name in This PC (default `Switch`). Windows only (`platforms/switch/switch_mtp.ps1`). |
| `--storage <name>` | With `--mtp`: the device's storage (default `1: SD Card`). |
| `--extra <file>` | With `--mtp`: another file to put beside the `.nro` (for example `plat-test.nro`). |

`run.args` is plain text, so keep `--pass` out of it unless the card stays with you. Use the sign-in
screen's **Remember password** instead. It stores the password on the card encrypted with a key only your
console has (`sdmc:/switch/xi-on-anything/data/keychain/`), so a copy of the card is useless elsewhere.

A line `FFXI_NAME=value` in `run.args` sets an environment variable (there is no shell on the Switch),
for example `FFXI_NET_TRACE=1` to log the network.

### 3. Run

- **Console**: hold R while starting any game, then pick **XI on Anything** in the homebrew menu
  ([Settings](#settings): **XI on Anything Settings**).
- **Emulator**: `python tools/build_switch.py run` copies the `.nro` to the emulator's card and starts it.
- **After a rebuild**: `python tools/build_switch.py deploy --ip <console IP>` sends the new `.nro` over
  the network without copying the game again. Press Y in the homebrew menu first.

The log is `sdmc:/switch/xi-on-anything/host64.log`. `python tools/build_switch.py logs --sd <card>` prints it.

### Home screen icon

A forwarder: an icon on the HOME menu that starts the `.nro` on the card with full memory, so you don't
have to hold R over a game. It is an installed title, which only the console can install with its own
keys, so it is made on the Switch, by [Sphaira](https://github.com/ITotalJustice/sphaira) (a homebrew
menu). The setup puts Sphaira's latest release in `sdmc:/switch/sphaira/` unless the card has a Sphaira
already; by hand, copy `switch/sphaira/sphaira.nro` from its release zip there. Then, on the Switch:

1. Hold R while starting any game, and start **Sphaira** from the homebrew menu.
2. Highlight **XI on Anything**, open its menu and pick **Install Forwarder**.
3. Start XI on Anything from its new icon on the HOME menu.

A forwarder is an unsigned title: the console starts it only with signature patches (sys-patch is the
usual one), and the setup says when it finds none on the card. The icon starts whatever `.nro` is on the
card, so updates need nothing more. Confirmed on a console (2026-11-18); holding R over a game works too.

Started from the icon, the game goes back to the sign-in screen when it ends by itself (a lost
connection, its own logout): the app restarts, and **Remember password** signs you in again. Started
with R held over a game it closes instead, as a restart there would start that game. Close it from the
HOME menu, or with B on the sign-in screen. `FFXI_SWITCH_RELOGIN=0` in `run.args` turns the restart off.
Confirmed on a console.

### Settings

**XI on Anything Settings** in the homebrew menu (`sdmc:/switch/xi-on-anything-settings/`, put there by
the setup) changes what the game reads when it starts, on the console. The D-pad moves and changes, + saves
and starts the game, B leaves, **Reset to defaults** puts every setting back to its first choice (saved
when you save).

| Setting | Choices | Where it goes |
| --- | --- | --- |
| Resolution | The screen's (the default: 1280x720 handheld, 1920x1080 docked, following a dock), or 1920x1080, 1600x900, 1280x720, 960x540, 640x480 and 512x448 (the PS2's size) whichever the screen. A 4:3 size is drawn with black bars at the sides. | `data/settings.reg` (0001, 0002); a fixed size also `FFXI_SWITCH_FIT=0` in `run.args` |
| Interface size | Large (2x), Medium (1.5x), Small (1x): the menus' resolution is the game's over it, 512x448 at least | `data/settings.reg` (0037, 0038) |
| Background | The screen's (the default), 2048, 1024, 512: the world's resolution before it is fitted to the screen | `run.args`: `FFXI_SWITCH_BG=<size>` |
| Frame rate | Uncapped (the default), Uncapped with 30 when slow, 60, 30 | `run.args`: `--fps-divisor`; `FFXI_FPS_ADAPT=1` for 30 when slow (an even 30 while frames can't keep above 40 fps, uncapped again when they can) |
| CRT filter | Off (the default), Scanlines, CRT (scanlines and an aperture grille): an old TV's look, best at 512x448 or 640x480 docked, where a row's scanline is two of the TV's lines | `run.args`: `FFXI_CRT=1` or `2` |
| Screen curve | Off (the default), On: with the CRT filter, the glass's curve and darker corners | `run.args`: `FFXI_CRT_CURVE=1` |
| Buttons | Nintendo, Xbox | `run.args`: `--switch-layout` |
| CPU | Boost while loading (the default), No boost (for sys-clk and the like), Full speed | `run.args`: `FFXI_SWITCH_LOADBOOST=0`, `FFXI_SWITCH_CPU=1785` |
| After a lost connection | Back to the sign-in screen (from the icon), Close | `run.args`: `FFXI_SWITCH_RELOGIN=0` |

Every other line of `run.args` is kept. It holds no game code. Make it a HOME menu icon too with Sphaira,
as above.

## Performance

By default the game is drawn at the screen's size (1280x720 in the hand, 1920x1080 on a TV), the world too,
and the CPU runs at its top speed (1785 MHz) while a zone loads. The frame rate:

- If your game version has frame delta sites (`tools/version/frame_sites.py`; the log says `[switch] frame rate: ...` at
  the start), it runs uncapped: as many frames as the console draws, up to the screen's 60 (about 35-60 in the
  world at stock clocks), with the world at its real speed whatever the rate.
- Without them it runs at 30 fps, which it holds. At 60 (`--fps-divisor 1`) a console at stock clocks draws
  around 35-40 frames a second in the world, and the world runs that much slower: its time is counted in
  frames, so animations look like slow motion.

Leave `--fps-divisor` out of `run.args`. `--fps-divisor 2` holds 30 fps and saves some battery, but it turns the
frame delta sites off: the world runs at the game's own pacing, slow when the console dips under 30. These lines in `run.args`
change the rest:

| Line | What it does |
| --- | --- |
| `FFXI_SWITCH_BG=1920x1080` | The world drawn at another size, read at the start (`FFXI_SWITCH_BG=config`: Config › Display's, 2048 and up, which a stock GPU can't draw at 60). |
| `FFXI_SWITCH_CPU=1785` | The CPU held at that speed the whole session: more frames, less battery, a warmer console. Put back when the game ends. |
| `FFXI_SWITCH_GPU=614` | The GPU held at that speed in handheld mode (docked, the system's 768 MHz is kept). Put back when the game ends. |
| `FFXI_SWITCH_LOADBOOST=0` | No CPU boost while a zone loads. |
| `FFXI_SWITCH_FIT=0` | The game's size from Config › Display, not the screen's. |
| `FFXI_D3D_WORKER=0` | The game's D3D calls on its own thread, as before the worker (below). |
| `FFXI_SCENE_CHARS=0` | The characters skinned on the CPU and drawn by the game, as before (below). |
| `FFXI_FRAMELOG=1` | The frame rate in the log every 5 seconds. |
| `FFXI_PROFILE=1` | Where each frame's time goes, and the clocks, in the log. |

With an overclocking tool (sys-clk, Horizon OC), let one thing set the clocks: drop `FFXI_SWITCH_CPU` and add
`FFXI_SWITCH_LOADBOOST=0`. sys-clk puts its own clock back within a moment, so `FFXI_SWITCH_CPU` alongside it
does nothing (the log says so: `the CPU keeps going back to ...`); set the speed in sys-clk's profile instead.
The same goes for `FFXI_SWITCH_GPU`: Horizon OC at its defaults puts the handheld GPU back to 307 MHz within
seconds, so set the GPU in its profile. In a busy town at 307 MHz the GPU is ~72% busy at 25 fps; at 614 MHz
(memory left at 1331) about 1.5 times faster, and a ~600-draw scene that topped out at 52-54 fps holds 60.
At 1785 MHz a busy town holds 30 fps (27-29 ms a frame in Windurst's crowds, against about 39 at 1224).

### The game's thread

In a busy town the frame rate is the game's main thread's: one core runs the game's code, and on it two
things took most of what wasn't the game's own logic. Both are moved off it by default:

- **The D3D calls run on a worker thread** (`FFXI_D3D_WORKER`, on core 2; `runtime/portable/d3d8_cmd.h`). The
  game's thread records each call (a few repeated state calls are dropped there), and the worker builds the draws
  for the render thread. What the game reads back comes from the game's thread's own view of the device's state;
  the few calls that need the worker's results wait for it (`FFXI_PROFILE`: `[d3d8] worker: ... waits by call`).
- **The characters are skinned and drawn on the GPU** (`FFXI_SCENE_CHARS`, `host/scene.c`'s characters with
  `gfx_gl.c`'s `gfx_chars_draw`): each mesh's vertices go to the GPU once, and a frame's bones, lights and
  materials are copied per draw, instead of the game skinning every vertex on the CPU and drawing the result.
  The game still draws a few itself (environment-mapped armour, a fading character, the shine pass);
  `FFXI_SCENELOG=1` counts them. Needs the game version's character addresses in `recomp/meta/builds.json`
  (2025-12-26 has them); without them the game draws its characters.

A busy town at 1020 MHz handheld (1,400-1,600 draws), the GPU at 614 MHz:

| | Frame | The game's thread |
| --- | --- | --- |
| Neither | 48 ms, 21 fps | 46 ms busy (its code 36, the D3D calls ~10) |
| The worker | 40 ms, 25 fps | ~39 ms (skinning 6.5) |
| The GPU's characters | 35 ms, 28-29 fps | its code 27-28 ms, the D3D calls 5 |
| Both (the default) | 30 fps held (`Uncapped, 30 when slow`) or above it uncapped | 13-19 ms |

With both, the GPU was ~41-53% busy at 614 MHz in that town, and the render thread's 17-23 ms of OpenGL calls a
frame become what limits the rate above 30. At the stock
handheld 307 MHz the GPU is the tighter: hold it at 614 (`FFXI_SWITCH_GPU`, or the overclocking tool's profile).

### The cores

Horizon doesn't share a core between threads of one priority: a thread runs until it waits. A thread of a higher
priority takes the core at once. So each core holds:

| Core | Threads |
| --- | --- |
| 0 | The game's main thread. |
| 1 | The render thread alone (`plat_thread_start_render`). `FFXI_SWITCH_RENDER_CORE=0`: the pool's threads take cores 1 and 2 in turn again. |
| 2 | The d3d8 worker, a step below the game's priority; the audio thread, a step above it (`FFXI_SWITCH_AUDIO_CORE`: another core, `-1` the pool's); the game's other threads and the pool's jobs, at its priority. |

On core 1, the audio thread stopped the render thread inside its GL calls for 1-15 ms a few times a frame, and the
game's other threads took the core whenever it waited for its next batch. `FFXI_PROFILE` logs each thread's job (its
offset in the .nro), core and priority as they start.

Mesa's own threads (`pthread_create`: glthread's worker and its shader queue) are logged too, as made: libnx makes
them at priority 0x3B, glthread's on core 1 with the render thread that made it. There it runs when the render thread
waits, and that is the best of what was tried in a busy town at 1785/614 (52-54 fps at 900-1200 draws):

| Tried | fps there | The render thread busy a frame |
| --- | --- | --- |
| glthread's worker on core 1, as made | 52-54 | 11-16 ms |
| `mesa_glthread=false` (the driver's work on the render thread) | 51, 36 at 1200+ | 22 ms |
| `FFXI_SWITCH_PTHREAD_CORE=2` (core 2, its priority) | 33 | 31 ms |
| the same with `FFXI_SWITCH_PTHREAD_PRIO=main` (the game's priority) | 37 | 21 ms: it took the d3d8 worker's time |

`run.args` lines starting `mesa_` or `MESA_` are Mesa's own settings, set as `FFXI_` ones are.

### Where the render thread's time goes

`FFXI_SAMPLE_THREAD=render` (with `FFXI_SAMPLE`) samples the render thread in the game thread's place. In a busy town
it waits for its next batch (~40%), for glthread's worker at the swap (~32%) and for room in that worker's queue
(~22%): the driver's own work per draw, on that worker, is what the frame waits for. With `FFXI_PROFILE` the GL back
end's GPU timestamps (`the GPU, ms a frame`) follow it: a character draw cost ~7 us of them whatever it held (one
triangle, every bind kept), and the game's draws ~5 us, none of them like the one before (nothing to merge). The
characters' mirrored copies are drawn as a second instance and their continuing lists joined (`FFXI_GL_CHARS_MERGE=0`:
an item a draw), half the draws: ~1.6 ms of those timestamps a frame in a crowd.

`FFXI_FILE_TRACE=1` (with `FFXI_PROFILE`) logs every open, read, write and close of the game's files. A town's
hitches when players come into view are their model DATs read the first time: 1.7-1.9 MB each in 256 KB reads of
~4 ms (the card's speed, past the block cache's reads under 256 KB), and in a session little was read twice.

## Controls

The controller works like an Xbox controller on the PC builds, with A/B as chosen above. In handheld mode,
a quick tap on the touch screen clicks there; the D-pad and stick move through menus. Text fields and the
chat line open the Switch's keyboard.
