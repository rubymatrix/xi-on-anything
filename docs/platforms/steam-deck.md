# Steam Deck

The [Linux build](linux.md), installed once from Desktop Mode and played from Game Mode with the
controller. Works on the LCD and OLED Decks. SteamOS has what it needs (glibc 2.35+, Vulkan 1.3).

## What you need

- Your `FINAL FANTASY XI` folder (about 14 GB) on the Deck: in your home folder, on an SD card, or
  in a Proton prefix, such as an install you made through Steam as a Non-Steam game. To copy it from
  a PC, use a USB drive, an SD card, or a network share in Desktop Mode's Dolphin file manager.
- About 3 GB free for the build.
- An internet connection for the install and the first build.

## Install (once, in Desktop Mode)

1. Press the **Steam** button, then **Power**, then **Switch to Desktop**.
2. Open **Konsole** (the application launcher, bottom left: System, Konsole).
3. Paste (Ctrl+Shift+V) and press Enter:

   ```bash
   curl -fsSL https://raw.githubusercontent.com/rubymatrix/xi-on-anything/main/install.sh | bash
   ```

   It installs the launcher as `~/Applications/XI-on-Anything.AppImage` and adds **XI on Anything**
   to the applications menu and to Steam as a Non-Steam game. Steam closes for a moment while it
   does. On-screen keyboard in Desktop Mode: Steam + X.
4. Double-click **Return to Gaming Mode** on the desktop.

Nothing else to install: the launcher fetches its compiler (zig) and the prebuilt rest of the client
itself. No `sudo`, no read-only filesystem changes, and SteamOS updates leave it in place.

## First run (Game Mode)

1. **Library**, then the **Non-Steam** tab, then **XI on Anything**, then **Play**.
2. Pick your game folder. The launcher lists the installs it found in your home folder, on SD cards
   and in Proton prefixes.
3. Sign in to your server and let it build. It translates the game from your files, which takes a
   while, once per game update. Keep the Deck plugged in.
4. It starts the game full screen.

After that, **Play** starts the game. The launcher updates itself and rebuilds when your server
hands out a new game version.

## Controls

The controller works through SDL3's gamepad support, as on any Linux PC. For typing (chat, the
sign-in fields), **Steam + X** opens the on-screen keyboard. Steam Input's per-game layout works on
the shortcut, so you can map the back buttons or trackpads to the game's keys.

## Tips

- **Performance**: in the game's Config > Advanced, Display has Frame Rate (30, 60, 120, 144 or
  Uncapped; past 60 only on some game builds), Graphics has Draw Distance, and Shadows, Lighting,
  Atmosphere and Water have the effects ([options](../options.md)). The Deck's native resolution is
  1280x800.
- **Battery**: a 30 fps cap (Config > Advanced > Display > Frame Rate, or the Deck's own frame
  limit in the Quick Access menu) roughly halves the GPU's work.
- **Logs**: the game logs to its terminal. To read it, start the AppImage from Konsole in Desktop
  Mode (below).

## Updating and removing

- Updates are automatic. To reinstall, run the install line again in Desktop Mode.
- To remove: delete `~/Applications/XI-on-Anything.AppImage`, remove the shortcut in Steam (its
  gear, Manage, Remove non-Steam game from your library), and delete
  `~/.local/share/FFXIRecompile` and `~/.cache/FFXI`. Your game folder is never written to.

## Troubleshooting

- **"No Steam here"** during install: the installer found no `userdata` folder in `~/.steam/steam`
  or `~/.local/share/Steam`, so Steam is missing or was never signed in to. Sign in to Steam and run
  the install line again. Or start it from the applications menu, or add the AppImage to Steam by
  hand (Steam, Add a Game, Add a Non-Steam Game).
- **The launcher doesn't find the game**: in the folder picker, browse to the `FINAL FANTASY XI`
  folder itself (the one with `FFXiMain.dll` and `ROM`).
- **It won't start from Game Mode**: start it from Konsole in Desktop Mode to see its output:
  `~/Applications/XI-on-Anything.AppImage`.
