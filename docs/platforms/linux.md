# Linux (x86_64)

The game recompiled for x86_64 Linux, drawn with Vulkan 1.3, in an SDL3 window (X11 or Wayland).
For a Steam Deck, see [steam-deck.md](steam-deck.md): the same build, set up for Game Mode.

## What you need

- An x86_64 machine with glibc 2.35 or later (Ubuntu 22.04, Fedora 36, Arch, any current
  distribution).
- A Vulkan 1.3 driver: Mesa (AMD, Intel) or NVIDIA's. Check with `vulkaninfo --summary`.
- Your `FINAL FANTASY XI` folder, and the viewer folder beside it if your install has one. An
  install in a Wine or Proton prefix works as it is: point at its `FINAL FANTASY XI` folder. File
  names match in any case.

## Option 1: the launcher (recommended)

```bash
curl -fsSL https://raw.githubusercontent.com/rubymatrix/xi-on-anything/main/install.sh | bash
```

It downloads `XI-on-Anything-linux-x86_64.AppImage`, checks it against the release's checksums
and installs it as `~/Applications/XI-on-Anything.AppImage`. It adds an applications menu entry
and, if Steam is installed, a Non-Steam shortcut (Steam is closed while it does). The installer
needs python3, which most distributions ship.

Open **XI on Anything** from the menu and pick your game folder; it searches your home folder, SD
cards and Proton prefixes. It builds the game from your files, fetching its compiler (zig) and the
prebuilt rest of the client (the Linux kit) itself, and keeps itself and the game up to date.

Without FUSE 2 an AppImage can't mount itself. The installer then starts it with
`APPIMAGE_EXTRACT_AND_RUN=1`, which unpacks it each time instead.

## Option 2: build from source

You need clang, python3 with `capstone` and `pefile`, SDL3 3.2 or later with its development
files (found through pkg-config), glslang, and the Vulkan headers. `tools/linux/Dockerfile` is the
reference. On Ubuntu 24.04:

```bash
sudo apt install clang pkg-config python3-pip git \
    libvulkan-dev glslang-tools glslang-dev spirv-tools
pip3 install --break-system-packages capstone pefile
```

Ubuntu 24.04 has no SDL3 package. If your distribution lacks SDL3 3.2 or later, build it from its
release as the Dockerfile does (CMake, `-DSDL_VULKAN=ON`, then `cmake --install`). Install these
first for its X11, Wayland and audio back ends:

```bash
sudo apt install cmake ninja-build \
    libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev libxfixes-dev libxtst-dev \
    libxkbcommon-dev libwayland-dev wayland-protocols libdecor-0-dev libegl-dev \
    libasound2-dev libpulse-dev libpipewire-0.3-dev libudev-dev libdbus-1-dev libibus-1.0-dev
```

Then:

```bash
git clone https://github.com/rubymatrix/xi-on-anything.git && cd xi-on-anything
python3 tools/build_posix.py prepare --game ~/Games/"FINAL FANTASY XI"
python3 tools/build_posix.py host64  --game ~/Games/"FINAL FANTASY XI"   # build/host64
build/host64 --game ~/Games/"FINAL FANTASY XI" --server <your server>
```

`python3 tools/build_posix.py desktop --game <folder>` adds `build/host64` to the applications menu
(`~/.local/share/applications/xi-on-anything.desktop`), so docks and window switchers show its
icon.

### In Docker instead

`tools/linux/Dockerfile` has every dependency, plus Mesa's software Vulkan for the tests:

```bash
docker build -t xi-linux tools/linux
docker run --rm -v "$PWD":/src -v ~/Games/"FINAL FANTASY XI":/game:ro -w /src xi-linux \
    sh -c 'python3 tools/build_posix.py prepare --game /game && python3 tools/build_posix.py host64 --game /game'
```

The resulting `build/host64` runs on the host if its SDL3 and glibc are recent enough.

## Signing in

The sign-in screen comes first unless `--user` or `--session` is given: username, password and
one-time code, with the server in its **Settings**. "Remember password" and `--trust on` keep
their secrets in the desktop's Secret Service (GNOME Keyring, KWallet, KeePassXC) through
libsecret (`libsecret-1.so.0`, loaded when needed). `secret-tool search service "FINAL FANTASY XI
sign-in"` lists them. Without libsecret or a running Secret Service nothing is kept, and Settings
says so: type the password each time, or set `FFXI_PASSWORD`.

## Files

| Where | What |
| --- | --- |
| `~/.local/share/FFXIRecompile/FFXI/` | `signin.cfg`, `settings.reg`, `saved.reg`, `modern.cfg` (Config > Advanced settings), addons (`ashita/`, `windower/`) |
| `~/.cache/FFXI` (or `$XDG_CACHE_HOME/FFXI`) | `fx.txt` (Config > Advanced's graphics settings) and caches. Deleting it resets those settings |

## Troubleshooting

- **No window, Vulkan errors**: check `vulkaninfo --summary` reports 1.3. `FFXI_VK_VALIDATION=1`
  turns on the Khronos validation layer (install `vulkan-validationlayers`).
- **Test the graphics without the game**: `python3 tools/build_posix.py gfxtest`.
- **The AppImage won't start**: run it from a terminal to see its output, or try
  `APPIMAGE_EXTRACT_AND_RUN=1 ~/Applications/XI-on-Anything.AppImage`.
