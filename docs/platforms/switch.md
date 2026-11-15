# Nintendo Switch (homebrew, in progress)

The recompiled game as a Switch homebrew app (`.nro`), built with devkitPro's libnx on your own PC from
your own game files, and copied to the console's SD card with the game beside it.

Branch: `port/switch`. Developer notes are in [../switch.md](../switch.md).

> [!IMPORTANT]
> **Status**: experimental, playable on a console. The sign-in screen, the game in the world, battles
> and menus run on a Switch (Atmosphère), drawn with Mesa's OpenGL, with sound (music not yet
> confirmed). Not yet: the scene effects (Modern FX, water, sun shadows), addons, the forwarder, sleep
> and resume, and a shader cache: each effect's shaders compile the first time it is seen, a short
> hitch.

## What you need

- A Windows, Linux or macOS PC with [devkitPro](https://devkitpro.org/wiki/Getting_Started) and these
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
  `portable` folder beside `Ryujinx.exe`) keeps its data and virtual SD card beside it.

## 1. Build

```
python tools/prepare.py --game "<your FINAL FANTASY XI folder>"
python tools/build_switch.py host
```

`host` translates the game (the desktop translation, `generated/all`, shared with the PC builds) and
writes `build/switch/xi-on-anything.nro`. The first build compiles about 6 million lines of generated C
and takes a while; later builds compile only what changed. `XI_SWITCH_JOBS` (default 6) is how many
generated files compile at once; lower it if the PC runs out of memory.

## 2. Put it on the SD card

```
python tools/build_switch.py setup --sd E:\ --game "<your FINAL FANTASY XI folder>" --server <server name or IP>
```

`--sd` is the card's root: in a card reader, the console's card over USB (Hekate's USB Mass Storage), or
`--sd emu` for the emulator's virtual card. It copies:

- the `.nro` to `sdmc:/switch/xi-on-anything/`, where the homebrew menu lists it
- the game to `sdmc:/switch/xi-on-anything/SquareEnix/FINAL FANTASY XI` (and the viewer folder beside it,
  if your install has one): about 14 GB the first time. Run it again after a game update, or if a copy
  stopped part way: only files whose size or date differ are copied.
- `ffxi.reg`, and `run.args` with the options you gave

`setup`'s options, which it writes to `run.args` (one host argument per line; edit the file on the card
to change them later; `--layout` becomes `--switch-layout` there):

| Option | What it does |
| --- | --- |
| `--server <name or IP>` | The LandSandBoat server. Without it, the sign-in screen asks. |
| `--layout nintendo\|xbox` | `nintendo` (the default): A confirms and B cancels, as everywhere on the Switch. `xbox`: by position, B (the bottom button) confirms. |
| `--fps-divisor 1\|2` | 60 fps (1) or 30 fps as shipped (2). 30 is easier on the console. |
| `--dats <folder>` | A DAT overlay folder, copied to the card and passed as `--dats`. |
| `--arg "<host options>"` | Any other [host option](../running.md#options), for example `--arg "--user me"`. |

Don't put `--pass` in `run.args` unless the card stays with you: it is a plain text file. Use the sign-in
screen's **Remember password** instead: the password is kept on the card encrypted with a key only your
console has (`sdmc:/switch/xi-on-anything/data/keychain/`), so a copy of the card can't be read on a
computer or another Switch.

## 3. Run

- **Console**: hold R while starting any game, then pick **XI on Anything** in the homebrew menu.
- **Emulator**: `python tools/build_switch.py run` copies the `.nro` to the emulator's card and starts it.
- **After a rebuild**, without copying the game again: `python tools/build_switch.py deploy --ip <console IP>`
  sends the new `.nro` over the network (press Y in the homebrew menu first, to receive it).

The log is `sdmc:/switch/xi-on-anything/host64.log`; `python tools/build_switch.py logs --sd <card>` prints it.
