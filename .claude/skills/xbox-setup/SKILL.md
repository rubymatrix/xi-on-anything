---
name: xbox-setup
description: First-time setup of XI on Xbox on an Xbox in Dev Mode - register the app as a loose app on the console's development drive and upload the player's FINAL FANTASY XI install beside it (about 14 GB, resumable). Also for resuming or repairing the game upload, or moving a console from a packaged install to the loose setup.
---

# Set up XI on Xbox on a console

Goal: the app registered from `D:\DevelopmentFiles\LooseApps\XIonXbox`, with the game in its
`SquareEnix\FINAL FANTASY XI` subfolder. After that, every update is the `xbox-deploy` skill (about 45 MB).
Read `docs/xbox-install.md` first. It explains why every alternative (network share, LocalState, the in-app
copy, the with-game package) doesn't work or is much worse on this console.

## Before you start

- **Console address** (Dev Home shows it). `export XBOX=<address>`; `xbox/tools/xbox.py` remembers it.
- **Device Portal on** (Dev Home > Remote Access Settings). If it asks for a login, set `XBOX_USER` / `XBOX_PASS`.
- **The app-only package**, `XIonXbox-xbox.msix` (about 17 MB, from `xbox/tools/deploy-xbox.ps1 -PackageOnly`),
  usually in `~/Downloads`.
- **The player's `FINAL FANTASY XI` folder** (contains `FFXiMain.dll`). Never copy it into this repo.
  `PlayOnlineViewer` isn't needed.

## Steps

1. `python3 xbox/tools/xbox.py status`, and act on what it says:
   - **Installed from a package** (e.g. after `deploy-xbox.ps1` or the with-game package): a loose app can't
     replace it (`0x80073CFB`). Tell the user it will be uninstalled, then run `python3 xbox/tools/xbox.py uninstall`.
     It backs LocalState up to `build/xbox/localstate-backup-*` first. On this console LocalState survived the
     uninstall anyway (settings, the game's `USER` folder, the shader cache).
   - **`game.txt` in LocalState:** it would override the game beside the app. Ask, then
     `python3 xbox/tools/xbox.py rm local:\LocalState game.txt`.
   - **`LocalState\SquareEnix`** (an unfinished in-app copy): it fills the small user drive. Ask, then
     `python3 xbox/tools/xbox.py rmdir local:\LocalState SquareEnix`.
2. **Tell the user to leave the local game folder where it is until the upload finishes.** Moving it mid-upload
   fails every remaining file (this has happened twice).
3. Run the setup **in the background** (the game upload takes about 40 minutes on a good LAN):
   `python3 -u xbox/tools/xbox.py setup "<msix>" "<FINAL FANTASY XI folder>"`
   It sends the app's files (with `AppxMetadata/CodeIntegrity.cat`: without it the app won't launch), registers
   the folder, then uploads the game, skipping files already there.
4. Check progress now and then (lines like `  12000/65252 files, 3.10/15.04 GB, 14.2 MB/s`), and report it when asked.
5. **When it finishes:** if files failed, run `python3 xbox/tools/xbox.py game "<FINAL FANTASY XI folder>"` to send just
   those. If they failed with "No such file", the folder moved: find it, and resume from the new place.
6. Run `python3 xbox/tools/xbox.py launch`, then `status`. Ask the user to set the app's type to **Game** in Dev Home
   (XI on Xbox > View details); as an App it gets about 1 GB of RAM.
7. To confirm the game was found: `python3 xbox/tools/xbox.py logs build/xbox/logs-check` and look for
   `[app] starting the game: D:\DevelopmentFiles\LooseApps\XIonXbox\SquareEnix\FINAL FANTASY XI` in `host64.log`.

## Don't

- Don't use Dev Home's "register from network share". It fails with `0x80073CF0` from both macOS File Sharing
  and Samba.
- Don't point `game.txt` at an SMB share. The app gets error 65; the sandbox blocks it.
- Don't use the in-app "Copy the game from a computer" on a console whose user drive is about 3 GB. It can't fit.
- Don't upload the with-game package through the browser. It failed every time.
