# Installing

Two ways in: the **launcher** (for players; it installs, builds and updates everything) or a
**source install** straight from this repo. Which platforms each covers is in the
[platform table](../README.md#platforms).

## The launcher (players)

### macOS (Apple silicon)

Paste into Terminal:

```bash
curl -fsSL https://raw.githubusercontent.com/rubymatrix/xi-on-anything/main/install.sh | bash
```

It downloads the launcher's latest release from this repo's
[Releases](https://github.com/rubymatrix/xi-on-anything/releases), checks it against the release's
checksums, installs **XI on Mac.app** into Applications and opens it. The launcher signs you in,
builds the game from your own game files and keeps both up to date. Run the same line again to
reinstall. The app is not signed with an Apple Developer ID yet, so a copy downloaded in a browser
is blocked by macOS: use the line above instead.

### Steam Deck and Linux (x86_64)

Once, in Desktop Mode (on a Deck: Steam menu, Power, Switch to Desktop; then open Konsole), paste the
same line. It installs the launcher as an AppImage in `~/Applications`, adds **XI on Anything** to
Steam (Library, Non-Steam) and to the applications menu. Back in Game Mode, everything else happens
there, with the controller: pick your game folder (the launcher finds installs in your home folder,
on an SD card and in Proton prefixes), and it builds the game for your Deck from your own files
(a while, once per game update), keeps itself up to date, and starts the game full screen.

Nothing to install first: the launcher fetches its compiler (zig) and the prebuilt rest of the client
(the Linux kit, `tools/linux/kit.sh`) itself. Needs glibc 2.35 or later and Vulkan 1.3 (a Deck,
and any current distribution with Mesa or NVIDIA's driver).

## Install from source (macOS)

To build the game straight from this repo without the launcher, you need a Mac with Apple silicon
(M1 or later), macOS 12 or later, about 3 GB free, and the game files your private server gives
you: a `FINAL FANTASY XI` folder (with `FFXiMain.dll` and
`ROM` in it). Put that folder anywhere, e.g. in
`~/Games`, then paste this into Terminal:

```bash
curl -fsSL https://raw.githubusercontent.com/rubymatrix/xi-on-anything/main/tools/install-source.sh | bash
```

It installs Apple's command line tools if they are missing (a dialog: click Install), finds the
game folder (or asks you to choose it), builds the game for your Mac from your own game files
(a few minutes the first time), puts **Final Fantasy XI** in `/Applications` and starts it.

Before building it asks:

- **Server**: the name or address to sign in to (blank: `127.0.0.1`).
- **Resolution** and **window mode** (windowed, borderless window, borderless full screen, full
  screen). The menus' resolution and the interface's shape follow: menus at half the window's
  height, and wider than 16:9 the interface stays 16:9 in the middle (`--ui-aspect`).
- **DAT overlay folder**: a server's own DATs (see [DAT overlays](running.md#dat-overlays---dats); blank: none).

Each question starts from what you have now, so on an update Enter keeps it. The options
`--server`, `--resolution WxH`, `--window-mode 0-3` and `--dats <folder>|none` answer instead, e.g.
`curl -fsSL .../tools/install-source.sh | bash -s -- --server play.example.net --resolution 2560x1440`.

Run the same line again to update, or after your server hands out a new game version. To name the
folder yourself: `curl -fsSL .../tools/install-source.sh | bash -s -- --game ~/Games/"FINAL FANTASY XI"`. The
build's output is in `~/Library/Application Support/FFXIRecompile/source/build/setup.log`. From a
clone of this repo, `./setup.command` does the same with the clone.

On Linux and Windows there is no source installer: see [Building](building.md).

### Testing the installer

To test the installer from a clone, uncommitted changes included, without touching your own app
or settings: `python3 tests/setup_test.py` checks the questions and the saved files they update,
and `tools/test_install.sh` runs `tools/install-source.sh` as a player would, piped to bash, with everything in a
sandbox folder. By default it stops after the questions; `--full` builds and installs into the
sandbox, `--saved` starts from copies of your saved settings (an update), `--sandbox <folder>`
reuses one (`--help`).
