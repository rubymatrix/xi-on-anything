# Nintendo Switch (homebrew, experimental)

The game as a Switch homebrew app (`.nro`). You build it with devkitPro's libnx on your own PC from your
own game files, then copy it to the console's SD card with the game beside it.

Developer notes: [../switch.md](switch-dev.md).

> [!IMPORTANT]
> **Status**: experimental, playable on a console. Sign-in, the world, battles and menus run on a Switch
> (Atmosphère), drawn with Mesa's OpenGL, with sound and music. Not yet: scene effects (Modern FX, water,
> sun shadows), addons, the forwarder, sleep and resume, and a shader cache. Each effect's shaders compile
> the first time it is seen, a short hitch.

## What you need

- A Windows, Linux or macOS PC with [devkitPro](https://devkitpro.org/wiki/Getting_Started) and these
  packages: `pacman -S switch-dev switch-mesa switch-libdrm_nouveau switch-glad`. On Windows, devkitPro's
  msys2 or your own msys2 with devkitPro's repositories both work. Set `DEVKITPRO` (or `XI_MSYS2`) if it
  is not in `C:\devkitPro`, `C:\devkitPro\msys2`, `C:\msys64` or `D:\msys64`. On Linux and macOS the
  default is `/opt/devkitpro`.
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

## 1. Build

```
python tools/prepare.py --game "<your FINAL FANTASY XI folder>"
python tools/build_switch.py host
```

`host` translates the game (`generated/all`, shared with the PC builds) and writes
`build/switch/xi-on-anything.nro`. The first build compiles about 6 million lines of C and takes a while;
later builds compile only what changed. `XI_SWITCH_JOBS` (default 6) sets how many files compile at once.
Lower it if the PC runs out of memory.

## 2. Put it on the SD card

```
python tools/build_switch.py setup --sd E:\ --game "<your FINAL FANTASY XI folder>" --server <server name or IP>
```

`--sd` is the card's root: in a card reader, the console's card over USB (Hekate's USB Mass Storage), or
`emu` for the emulator's card. On Windows, `--mtp` copies to the console over USB instead. It copies:

- the `.nro` to `sdmc:/switch/xi-on-anything/`, where the homebrew menu lists it
- the game to `sdmc:/switch/xi-on-anything/SquareEnix/<folder name>`, with the viewer folder if you have
  one: about 14 GB the first time. The app only looks in `SquareEnix/FINAL FANTASY XI`, so the `--game`
  folder must be named `FINAL FANTASY XI`. Run it again after a game update or an interrupted copy: only
  files whose size or date differ are copied.
- `ffxi.reg`, and `switch.reg` over it: the Switch's defaults (1920x1080, windowed, menus at 960x540 so
  the UI is twice its size). Changes you make in game go to `settings.reg` beside them and win over both.
  Config > Advanced's Display page keeps its own `data/settings.reg`, loaded last.
- `run.args` with your options

All options but `--nro`, `--mtp`, `--storage` and `--extra` go in `run.args`, one host argument per line.
Edit it on the card to change them later. `--layout` becomes `--switch-layout` there.

| Option | What it does |
| --- | --- |
| `--server <name or IP>` | The LandSandBoat server. Without it, the sign-in screen asks. |
| `--layout nintendo\|xbox` | `nintendo` (default): A confirms, B cancels. `xbox`: by position, B (the bottom button) confirms. |
| `--fps-divisor 0\|1\|2` | 60 fps (1, default), 30 fps as shipped (2), or uncapped (0). 30 is easier on the console. |
| `--dats <folder>` | A DAT overlay folder, copied to the card and passed as `--dats`. |
| `--arg "<host options>"` | Any other [host option](../running.md#options), for example `--arg "--user me"`. |
| `--nro <file>` | The `.nro` to copy (default `build/switch/xi-on-anything.nro`). |
| `--mtp [device]` | Copy over USB to a console running DBI's MTP responder, by its name in This PC (default `Switch`). Windows only (`tools/switch_mtp.ps1`). |
| `--storage <name>` | With `--mtp`: the device's storage (default `1: SD Card`). |
| `--extra <file>` | With `--mtp`: another file to put beside the `.nro` (for example `plat-test.nro`). |

`run.args` is plain text, so keep `--pass` out of it unless the card stays with you. Use the sign-in
screen's **Remember password** instead. It stores the password on the card encrypted with a key only your
console has (`sdmc:/switch/xi-on-anything/data/keychain/`), so a copy of the card is useless elsewhere.

A line `FFXI_NAME=value` in `run.args` sets an environment variable (there is no shell on the Switch),
for example `FFXI_NET_TRACE=1` to log the network.

## 3. Run

- **Console**: hold R while starting any game, then pick **XI on Anything** in the homebrew menu.
- **Emulator**: `python tools/build_switch.py run` copies the `.nro` to the emulator's card and starts it.
- **After a rebuild**: `python tools/build_switch.py deploy --ip <console IP>` sends the new `.nro` over
  the network without copying the game again. Press Y in the homebrew menu first.

The log is `sdmc:/switch/xi-on-anything/host64.log`. `python tools/build_switch.py logs --sd <card>` prints it.

## Controls

The controller works like an Xbox controller on the PC builds, with A/B as chosen above. In handheld mode,
a quick tap on the touch screen clicks there; the D-pad and stick move through menus. Text fields and the
chat line open the Switch's keyboard.
