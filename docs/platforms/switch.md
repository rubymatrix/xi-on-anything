# Nintendo Switch (homebrew, experimental)

The recompiled game as a Switch homebrew app (`.nro`), built with devkitPro's libnx on your own PC from
your own game files, and copied to the console's SD card with the game beside it.

Developer notes are in [../switch.md](../switch.md).

> [!IMPORTANT]
> **Status**: experimental, playable on a console. The sign-in screen, the game in the world, battles
> and menus run on a Switch (Atmosphère), drawn with Mesa's OpenGL, with sound (music not yet
> confirmed). Not yet: the scene effects (Modern FX, water, sun shadows), addons, the forwarder, sleep
> and resume, and a shader cache: each effect's shaders compile the first time it is seen, a short
> hitch.

## What you need

- A Windows, Linux or macOS PC. The [Quick start](#quick-start)'s setup installs what follows when it is
  missing (on Windows: Python with winget, devkitPro with its own installer, then the packages; on Linux and
  macOS the packages, once devkitPro's pacman is in). By hand: [devkitPro](https://devkitpro.org/wiki/Getting_Started) and these
  packages: `pacman -S switch-dev switch-mesa switch-libdrm_nouveau switch-glad`. On Windows, devkitPro's
  msys2 or your own msys2 with devkitPro's pacman repositories both work; set `DEVKITPRO` (or `XI_MSYS2`)
  if it is not in `C:\devkitPro`, `C:\msys64` or `D:\msys64`.
- Python 3.12+ with `capstone` and `pefile`.
- Your `FINAL FANTASY XI` folder and a LandSandBoat server ([README.md](README.md)).
- To run on a console: a Switch that runs homebrew (Atmosphère), launched with **full memory**: hold R
  while starting a game to open the homebrew menu over it. The album's
  homebrew menu has too little memory for the game.
- To run without a console: [Ryubing](https://git.ryujinx.app/projects/Ryubing/releases) with your own
  console's `prod.keys` (dumped with Lockpick_RCM) in its `system` folder. Its portable mode (a
  `portable` folder beside `Ryujinx.exe`) keeps its data and virtual SD card beside it. Turn on
  **Settings > Network > Enable guest Internet access**: without it Ryubing's resolver refuses every name,
  even `127.0.0.1`, and the host exits with `--server: cannot resolve`. On macOS, `build_switch.py` finds
  `Ryujinx.app` in `/Applications` or `~/Applications` and its card in `~/Library/Application Support/Ryujinx`.

## Quick start

With the SD card in the PC:

- **Windows**: double-click `setup-switch.bat` in this folder.
- **Linux, macOS**: `./setup-switch.sh`.

What's missing it offers to install, a yes each: Python (Windows, with winget), its `capstone` and
`pefile` (pip), devkitPro (Windows: it downloads devkitPro's installer and opens it: keep `C:\devkitPro`
and tick **Switch Development**; on Linux and macOS install [devkitPro's pacman](https://devkitpro.org/wiki/devkitPro_pacman)
first) and devkitPro's Switch packages (with its pacman; `sudo` asks for your password on Linux and macOS).

It asks for your `FINAL FANTASY XI` folder, the SD card (it finds a card with `atmosphere/` or `switch/`
on it by itself) and the server's address: the name or IP your launcher signs in to, which it offers
when it finds it near the game folder (an Ashita boot config's `--server`, VanaCore's `[server]`, a launch
script's `--server`). Then it does every step below in order: it checks the PC, identifies your game version, prepares its code,
finds its frame delta sites if this repository doesn't have them yet, builds the `.nro`, and copies the
`.nro`, the game and the settings to the card. Then [run it](#3-run).

The options answer the questions instead, for a script or a second run:

```
python tools/setup_switch.py --game "<your FINAL FANTASY XI folder>" --sd E:\ --server <server name or IP>
```

`--layout xbox`, `--dats <folder>` and `--arg "<host options>"` are as in [step 2](#2-put-it-on-the-sd-card);
`--check` checks the PC and stops; `--no-copy-game` leaves the game on the card as it is. The answers are
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
python tools/frame_sites.py
python tools/build_switch.py host
```

`prepare.py` identifies your game version and unpacks its code into `generated/`. `frame_sites.py` is only
needed when `meta/builds.json` has no frame delta sites for your version (no `frame_int_sites` in its entry;
a build without them logs `has no frame delta sites` at the start): it finds them and adds them to
`meta/builds.json`. With them the Switch runs uncapped with the world at its real speed; without them, at
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
`--sd emu` for the emulator's virtual card. It copies:

- the `.nro` to `sdmc:/switch/xi-on-anything/`, where the homebrew menu lists it
- the game to `sdmc:/switch/xi-on-anything/SquareEnix/FINAL FANTASY XI` (and the viewer folder beside it,
  if your install has one): about 14 GB the first time. Run it again after a game update, or if a copy
  stopped part way: only files whose size or date differ are copied.
- `ffxi.reg`, and `switch.reg` over it: the Switch's defaults (a 1920x1080 window and background,
  windowed, menus at 960x540 so the UI is twice its size, readable on a TV and in handheld). What you
  change in game is saved to `settings.reg` beside them and wins over both.
- `run.args` with the options you gave: an option given replaces its line there, and every other line
  (yours, added by hand) is kept

`setup`'s options, which it writes to `run.args` (one host argument per line; edit the file on the card
to change them later; `--layout` becomes `--switch-layout` there):

| Option | What it does |
| --- | --- |
| `--server <name or IP>` | The LandSandBoat server. Without it, the sign-in screen asks. |
| `--layout nintendo\|xbox` | `nintendo` (the default): A confirms and B cancels, as everywhere on the Switch. `xbox`: by position, B (the bottom button) confirms. |
| `--fps-divisor 0\|1\|2` | Leave it out: the default is the best there is (uncapped with frame delta sites, else 30). 1 (60 fps) or 2 (30) turn the frame delta sites off: see [Performance](#performance). |
| `--dats <folder>` | A DAT overlay folder, copied to the card and passed as `--dats`. |
| `--arg "<host options>"` | Any other [host option](../running.md#options), for example `--arg "--user me"`. |

Don't put `--pass` in `run.args` unless the card stays with you: it is a plain text file. Use the sign-in
screen's **Remember password** instead: the password is kept on the card encrypted with a key only your
console has (`sdmc:/switch/xi-on-anything/data/keychain/`), so a copy of the card can't be read on a
computer or another Switch.

A line `FFXI_NAME=value` in `run.args` sets that environment variable instead of passing an argument
(there is no shell on the Switch), for example `FFXI_NET_TRACE=1` to log the network.

### 3. Run

- **Console**: hold R while starting any game, then pick **XI on Anything** in the homebrew menu.
- **Emulator**: `python tools/build_switch.py run` copies the `.nro` to the emulator's card and starts it.
- **After a rebuild**, without copying the game again: `python tools/build_switch.py deploy --ip <console IP>`
  sends the new `.nro` over the network (press Y in the homebrew menu first, to receive it).

The log is `sdmc:/switch/xi-on-anything/host64.log`; `python tools/build_switch.py logs --sd <card>` prints it.

## Performance

By default the game is drawn at the screen's size (1280x720 in the hand, 1920x1080 on a TV), the world too,
and the CPU runs at its top speed (1785 MHz) while a zone loads. The frame rate:

- If your game version has frame delta sites (`tools/frame_sites.py`; the log says `[switch] frame rate: ...` at
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
| `FFXI_SWITCH_LOADBOOST=0` | No CPU boost while a zone loads. |
| `FFXI_SWITCH_FIT=0` | The game's size from Config › Display, not the screen's. |
| `FFXI_FRAMELOG=1` | The frame rate in the log every 5 seconds. |
| `FFXI_PROFILE=1` | Where each frame's time goes, and the clocks, in the log. |

With an overclocking tool (sys-clk, Horizon OC), let one thing set the clocks: drop `FFXI_SWITCH_CPU` and add
`FFXI_SWITCH_LOADBOOST=0`.

## Controls

The controller works the way an Xbox controller does on the PC builds (the game's own gamepad support),
with A/B as chosen above. In handheld mode a quick tap on the touch screen is a click where you tap (as on
Android); the D-pad and stick move through menus. Text fields (the sign-in) and the chat line open the
Switch's keyboard.
