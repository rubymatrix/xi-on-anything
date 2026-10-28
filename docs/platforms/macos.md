# macOS (Apple silicon)

The main platform: the game recompiled to arm64, drawn with Metal, with no Wine or Rosetta.

## What you need

- A Mac with Apple silicon (M1 or later), macOS 12 or later.
- About 3 GB free for the build, plus the game itself.
- Your `FINAL FANTASY XI` folder (and the viewer folder beside it, if your install has one), copied
  from a Windows install or handed out by your server. Put it anywhere, e.g. `~/Games` or
  `~/SquareEnix`.

## Option 1: the launcher (recommended)

1. Open Terminal and paste:

   ```bash
   curl -fsSL https://raw.githubusercontent.com/rubymatrix/xi-on-anything/main/install.sh | bash
   ```

   It downloads the latest launcher release, checks it against the release's checksums, installs
   **XI on Mac.app** into Applications and opens it.
2. In the launcher, sign in and point it at your game folder. It builds the game from your files
   (a few minutes the first time) and keeps both itself and the game up to date.
3. Play from the launcher.

The app is not signed with an Apple Developer ID yet. A copy downloaded in a browser is blocked by
macOS, so always install with the line above. Run it again to reinstall.

## Option 2: build from source, one line

```bash
curl -fsSL https://raw.githubusercontent.com/rubymatrix/xi-on-anything/main/tools/install-source.sh | bash
```

1. It installs Apple's command line tools if missing (a dialog: click **Install**).
2. It finds your game folder, or asks you to choose it. To name it yourself:
   `... | bash -s -- --game ~/Games/"FINAL FANTASY XI"`.
3. It asks:
   - **Server**: the name or address to sign in to (blank: `127.0.0.1`).
   - **Resolution** and **window mode** (windowed, borderless window, borderless full screen,
     full screen).
   - **DAT overlay folder**: your server's own DATs, if it has any (blank: none).

   Each answer starts from what you have now, so Enter keeps it on an update. `--server`,
   `--resolution WxH`, `--window-mode 0-3` and `--dats <folder>|none` answer instead.
4. It builds the game, puts **Final Fantasy XI** in `/Applications` and starts it.

Run the same line again to update, or after your server hands out a new game version. The build's
log is `~/Library/Application Support/FFXIRecompile/source/build/setup.log`. From a clone,
`./setup.command` does the same.

## Option 3: build by hand

```bash
git clone https://github.com/rubymatrix/xi-on-anything.git && cd xi-on-anything
pip3 install capstone pefile        # SDL3 and mbedtls are vendored and built with clang
python3 tools/build_posix.py prepare --game ~/SquareEnix/"FINAL FANTASY XI"
python3 tools/build_posix.py host64  --game ~/SquareEnix/"FINAL FANTASY XI"   # build/host64
```

Run it:

```bash
build/host64 --game ~/SquareEnix/"FINAL FANTASY XI" --server <your server>
```

Or make an app that starts from Finder with your defaults baked in:

```bash
python3 tools/build_posix.py app --game ~/SquareEnix/"FINAL FANTASY XI" --server <your server> \
    --resolution 2560x1440 --window-mode 3
open "build/Final Fantasy XI.app"
```

Signing: the app is signed with the identity `FFXI Local Code Signing` when your login keychain has
one, else ad hoc. Ad hoc, macOS treats every rebuild as a new app and asks again before it reads the
saved password. `tools/setup.py` makes the certificate on its first run; [Building](../building.md#final-fantasy-xiapp)
shows how by hand.

## Signing in

Without `--user` or `--session`, the sign-in screen comes first, in the game's own UI art: username,
password and one-time code, with the server, "remember password" (macOS Keychain) and the window
theme in its **Settings**. Cmd+Q, the Dock's Quit and Ctrl+C quit at any point.

## Files

| Where | What |
| --- | --- |
| `~/Library/Application Support/FFXIRecompile/FFXI/` | `signin.cfg`, `settings.reg`, `saved.reg` (the game's saves), `host64.log` when started from Finder, an optional `background.png` |
| `.../FFXI/ashita`, `.../FFXI/windower` | Ashita and Windower addons (`tools/setup.py` fetches them) |
| `~/Library/Caches/FFXI` | Caches (shaders and the like); safe to delete |

## Troubleshooting

- **"is build &lt;sha&gt;, which meta/builds.json does not know"**: your client version isn't
  supported yet. See
  [Supporting a new client version](../new-client-version.md#supporting-a-new-client-version).
- **The app asks for Keychain access after every update**: it is signed ad hoc; make the
  `FFXI Local Code Signing` certificate (above) and rebuild.
- **Graphics problems**: `build/gfx_test --window` checks the Metal back end without the game;
  `MTL_DEBUG_LAYER=1` turns on Metal's validation.
