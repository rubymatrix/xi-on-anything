# Installing

Two ways in: the **launcher**, for players, which installs, builds and updates everything; or a
**source install** from this repo. The [platform table](../README.md#platforms) shows which
platforms each covers.

## The launcher (players)

### Windows (x64)

Download and run the installer,
[XI-on-Anything-windows-x64-setup.exe](https://github.com/rubymatrix/xi-on-anything/releases/latest/download/XI-on-Anything-windows-x64-setup.exe),
from the [latest release](https://github.com/rubymatrix/xi-on-anything/releases/latest).

### macOS (Apple silicon)

Paste into Terminal:

```bash
curl -fsSL https://raw.githubusercontent.com/rubymatrix/xi-on-anything/main/install.sh | bash
```

It downloads the latest launcher from [Releases](https://github.com/rubymatrix/xi-on-anything/releases),
checks it against the release's checksums, installs **XI on Mac.app** into `/Applications`
(`~/Applications` if that isn't writable) and opens it. The launcher signs you in, builds the game
from your own game files and keeps both up to date. Run the line again to reinstall. The app has no
Apple Developer ID signature yet, so macOS blocks a copy downloaded in a browser.

### Steam Deck and Linux (x86_64)

Once, in Desktop Mode, paste the same line into a terminal. On a Deck: Steam menu, Power, Switch to
Desktop, then open Konsole. It installs the launcher as an AppImage in `~/Applications` and adds
**XI on Anything** to Steam (Library, Non-Steam) and to the applications menu.

The rest happens in Game Mode, with the controller. Pick your game folder; the launcher finds
installs in your home folder, on an SD card and in Proton prefixes. It builds the game from your
files (a while, once per game update), keeps itself up to date and starts the game full screen.

The launcher fetches its compiler (zig) and the prebuilt rest of the client (the Linux kit,
`platforms/linux/kit.sh`) itself. You need python3 (the installer uses it for the Steam shortcut),
glibc 2.35 or later and Vulkan 1.3. A Deck and any current distribution with Mesa or NVIDIA's
driver have all three.

## Install from source (macOS)

You need a Mac with Apple silicon (M1 or later), macOS 12 or later, about 3 GB free, and your
server's game files: a `FINAL FANTASY XI` folder with `FFXiMain.dll` and `ROM` in it. Put the
folder anywhere, e.g. `~/Games`, then paste into Terminal:

```bash
curl -fsSL https://raw.githubusercontent.com/rubymatrix/xi-on-anything/main/tools/install-source.sh | bash
```

It installs Apple's command line tools if missing (a dialog: click Install), finds the game folder
or asks for it, and builds the game from your files (a few minutes the first time). Then it puts
**Final Fantasy XI** in `/Applications` (`~/Applications` if that isn't writable) and starts it.

Before building it asks for:

- **Server**: the name or address to sign in to (blank: `127.0.0.1`).
- **Resolution** and **window mode** (windowed, borderless window, borderless full screen, full
  screen). The menus are the window's height. Wider than 16:9, the interface stays 16:9 in the
  middle (`--ui-aspect`).
- **DAT overlay folder**: a server's own DATs ([DAT overlays](running.md#dat-overlays---dats); blank: none).

Each answer starts from your current setting, so Enter keeps it on an update. `--server`,
`--resolution WxH`, `--window-mode 0-3` and `--dats <folder>|none` answer instead, e.g.
`curl -fsSL .../tools/install-source.sh | bash -s -- --server play.example.net --resolution 2560x1440`.

Run the line again to update, or when your server hands out a new game version. To name the
folder: `curl -fsSL .../tools/install-source.sh | bash -s -- --game ~/Games/"FINAL FANTASY XI"`.
The build log is `~/Library/Application Support/FFXIRecompile/source/build/setup.log`. In a clone
of this repo, `./setup.command` does the same with the clone.

There is no source installer on Linux or Windows: see [Building](building.md).

### Testing the installer

These test the installer from a clone, uncommitted changes included, without touching your own app
or settings. `python3 tests/setup_test.py` checks the questions and the saved files they update.
`tools/test_install.sh` runs `tools/install-source.sh` piped to bash, as a player would, inside a
sandbox folder. It stops after the questions unless given `--full` (build and install into the
sandbox). `--saved` starts from copies of your saved settings (an update), `--sandbox <folder>`
reuses one. `--help` lists the options.
