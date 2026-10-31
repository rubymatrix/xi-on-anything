# PS4 (homebrew, experimental)

The recompiled game for a jailbroken PS4 that installs fake PKGs, built with the OpenOrbis toolchain.
The PS4's CPU is x86-64, so the translation is the desktop one; the platform under it changes.

Branch: `port/ps4` (`git checkout port/ps4`). Developer notes, the GPU back end and every finding are
in `docs/ps4.md` and `docs/ps4-gnm-research.md` on that branch.

> [!IMPORTANT]
> **Status**: experimental. It has run on a real console, and in the **shadPS4 emulator** it signs in
> to LandSandBoat, draws the title, lobby and character creation at about 58 fps, makes a character
> and logs in to Southern San d'Oria (about 38 fps in town). Expect rough edges. The PS5 is not a
> target.

## What you need

- A Mac or Linux machine to build on.
- The [OpenOrbis PS4 toolchain](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain) (built with
  v0.5.4), with `OO_PS4_TOOLCHAIN` set to it.
- LLVM clang with the x86 back end (Apple's has it) and `ld.lld` (`brew install lld llvm` on macOS).
- Python 3.12+ with `capstone` and `pefile`.
- For the GPU renderer: opengnm and opengnm-psbc, glslang and `glslc` (below). Without them, the CPU
  renderer works (`XI_PS4_GFX=soft`), but slowly.
- Your `FINAL FANTASY XI` folder and a LandSandBoat server reachable **by IP address**.
- To run on a console: a PS4 on a firmware with a public jailbreak, a homebrew enabler that installs
  fake PKGs and serves FTP (for example GoldHEN), and a DualShock 4.
- To run without a console: [shadPS4](https://github.com/shadps4-emu/shadPS4).

## 1. Build the GPU tools (once)

```bash
mkdir -p ~/Projects/OpenOrbis && cd ~/Projects/OpenOrbis   # or set XI_OPENGNM / XI_PSBC
git clone https://github.com/PS4-OpenGNM/opengnm
git clone https://github.com/PS4-OpenGNM/opengnm-psbc
(cd opengnm && cp config.orbis.mak config.mak && \
    make lib CC=$(brew --prefix llvm)/bin/clang AR=$(brew --prefix llvm)/bin/llvm-ar)
(cd opengnm-psbc && git apply /path/to/xi-on-anything/tools/ps4/psbc-table-pointer.patch && make)
(cd opengnm-psbc && make -f Makefile.orbis \
    CC="$(brew --prefix llvm)/bin/clang -Wno-incompatible-pointer-types" CXX=$(brew --prefix llvm)/bin/clang++)
```

opengnm-psbc's host build also wants `Vulkan-Headers` and `SPIRV-Headers` cloned beside it, Python's
`mako` and `pyyaml`, and a `config.mak`. The game's shaders are compiled on the console from GLSL, so
the build also needs a glslang source tree (`XI_GLSLANG`).

## 2. Build the game

```bash
cd /path/to/xi-on-anything && git checkout port/ps4
export OO_PS4_TOOLCHAIN=/path/to/OpenOrbis/PS4Toolchain
python3 tools/build_posix.py prepare --game /path/to/"FINAL FANTASY XI"

# the local data folder: the build makes the PlayStation button prompts from the game found here
mkdir -p build/ps4/data
cp generated/FFXiMain.retail.dll build/ps4/data/
ln -s /path/to/"FINAL FANTASY XI" "build/ps4/data/FINAL FANTASY XI"

XI_CXX=$(brew --prefix llvm)/bin/clang++ python3 tools/build_ps4.py host
```

Output in `build/ps4/`: `xi-on-anything.pkg`, and the unpacked `pkg/eboot.bin` with its `sce_sys/`.
With the game in `build/ps4/data`, `build_ps4.py host` also writes `build/ps4/data/pad-dats/`: the
lobby's button prompts redrawn as PlayStation buttons, made from your game files. Without it, it says
so and skips them.

For the CPU renderer instead: `XI_PS4_GFX=soft python3 tools/build_ps4.py host`.

## 3. The data folder

Everything the game reads lives in `/data/xi-on-anything` on the console, never in the PKG:

| File | What |
| --- | --- |
| `FINAL FANTASY XI/` | The game install, as `--game` names it on a desktop |
| `FFXiMain.retail.dll` | From `generated/` after `prepare` |
| `pad-dats/` | From `build/ps4/data/` (the PlayStation button prompts; optional) |
| `args.txt` | The sign-in, below |

`args.txt`, one argument per line:

```
--server
192.168.1.20
--user
<account>
--pass
<password>
```

There is no sign-in screen on the PS4: without a password host64 stops at its sign-in check. Give the
server as an IP address (no DNS under shadPS4). `args.txt` holds your password: keep it local, never
commit it.

## 4a. Install on a console (untested)

1. Jailbreak the console and start your homebrew enabler with its FTP server on.
2. Copy the data folder's contents over FTP to `/data/xi-on-anything` (about 14 GB; a wired
   connection helps). Copy the game itself, not the link `build/ps4/data` holds.
3. Copy `build/ps4/xi-on-anything.pkg` to the console (FTP, or a USB drive formatted exFAT) and install
   it with the enabler's package installer.
4. Start **XI on Anything** from the home screen. Its console shows the output until the game takes
   the screen; the log is `/data/xi-on-anything/host64.log`.

Report what happens: the console's kernel alignment rules, direct memory limits and TLS have only been
checked against stand-ins.

## 4b. Run in shadPS4

shadPS4 runs the unpacked build; its `/data` is `data/` in its user folder
(`~/Library/Application Support/shadPS4` on macOS). Link the local data folder from step 2 in rather
than copying it:

```bash
$EDITOR build/ps4/data/args.txt
ln -s "$PWD/build/ps4/data" ~/Library/Application\ Support/shadPS4/data/xi-on-anything
/path/to/shadps4 -g build/ps4/pkg/eboot.bin
```

Under shadPS4, `--mount` and `-e` don't reach the program. Missing optional modules it reports
(`libc.prx`, `libSceFios2.prx`) are fine.

## Controls

Played from the pad, as on the PS2 (`--controller-primary on`). While the chat line is typing, square
opens the game's own soft keyboard. The lobby shows PlayStation buttons; the in-game help still names
keys.

## Checks without a console

```bash
python3 tools/build_ps4.py check                                  # FFXI_PS4 code, memory and waits, on this machine
python3 tools/build_ps4.py hostboot --game /path/to/"FINAL FANTASY XI"   # the PS4 boot path on the real game, here
python3 tests/plat_wait_test.py
```

## Not done

DXT textures are stored decoded (four to eight times the memory), cube maps are untested, points have
no size, and the scene renderer and its effects are off (they need ray queries). No addons, no Discord.
Audio is silent until `sceAudioOut` is wired up.
