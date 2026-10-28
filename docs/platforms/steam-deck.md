# Steam Deck

The [Linux build](linux.md), installed once from Desktop Mode and played from Game Mode with the
controller. Works on the LCD and OLED Decks; SteamOS meets its needs (glibc 2.35+, Vulkan 1.3).

## What you need

- Your `FINAL FANTASY XI` folder on the Deck: in your home folder, on an SD card, or inside a Proton
  prefix (an install you made through Steam as a Non-Steam game counts). To copy it from a PC, use a
  USB drive, an SD card, or a network share in Desktop Mode's Dolphin file manager. It is about 14 GB.
- About 3 GB free for the build.
- An internet connection for the install and the first build.

## Install (once, in Desktop Mode)

1. Press the **Steam** button, then **Power**, then **Switch to Desktop**.
2. Open **Konsole** (the application launcher, bottom left: System, Konsole).
3. Paste (Ctrl+Shift+V) and press Enter:

   ```bash
   curl -fsSL https://raw.githubusercontent.com/rubymatrix/xi-on-anything/main/install.sh | bash
   ```

   It installs the launcher as `~/Applications/XI-on-Anything.AppImage`, adds **XI on Anything** to
   the applications menu, and adds it to Steam as a Non-Steam game. Steam is closed for a moment
   while the shortcut is added. On-screen keyboard in Desktop Mode: Steam + X.
4. Double-click **Return to Gaming Mode** on the desktop.

Nothing else to install: the launcher fetches its compiler (zig) and the prebuilt rest of the client
itself. No `sudo`, no read-only filesystem changes, and SteamOS updates don't remove it.

## First run (Game Mode)

1. **Library**, then the **Non-Steam** tab, then **XI on Anything**, then **Play**.
2. Pick your game folder. The launcher lists the installs it found in your home folder, on SD cards
   and in Proton prefixes.
3. Sign in to your server and let it build. It translates the game from your files, which takes a
   while, once per game update. Keep the Deck on power.
4. It starts the game full screen.

From then on, **Play** starts the game. The launcher updates itself, and rebuilds when your server
hands out a new game version.

## Controls

The controller works through SDL3's gamepad support, as on any Linux PC. For keyboard input (chat,
the sign-in fields) press **Steam + X** for the on-screen keyboard. Steam Input's per-game layout
works on the shortcut as on any Non-Steam game: map back buttons or the trackpads to keys the game
uses if you want them.

## Tips

- **Performance**: Config > Modern in the game has the frame rate (30 or 60), draw distance and each
  effect. On the Deck's screen, 1280x800 is the native resolution.
- **Battery**: a 30 fps cap (Config > Modern, or the Deck's own frame limit in the Quick Access
  menu) roughly halves the GPU's work.
- **Logs**: the game logs to its terminal output. To read it, start the AppImage from Konsole in
  Desktop Mode (below).

## Updating and removing

- Updates are automatic. To reinstall, run the install line again in Desktop Mode.
- To remove: delete `~/Applications/XI-on-Anything.AppImage`, remove the shortcut in Steam (its
  gear, Manage, Remove non-Steam game from your library), and delete
  `~/.local/share/FFXIRecompile` and `~/.cache/FFXI`. Your game folder is never written to.

## Troubleshooting

- **"No Steam here"** during install: the installer didn't find `~/.steam/steam` or
  `~/.local/share/Steam`. Start it from the applications menu, or add the AppImage to Steam by hand
  (Steam, Add a Game, Add a Non-Steam Game).
- **The launcher doesn't find the game**: in the folder picker, browse to the `FINAL FANTASY XI`
  folder itself (the one with `FFXiMain.dll` and `ROM`).
- **It won't start from Game Mode**: start it once from Desktop Mode, from Konsole, to see its output:
  `~/Applications/XI-on-Anything.AppImage`.
