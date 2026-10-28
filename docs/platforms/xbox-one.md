# Xbox One and Xbox Series (Developer Mode, experimental)

The recompiled game as a UWP app on a console in Developer Mode, drawn with Direct3D 12 straight on a
CoreWindow, with the native sign-in screen driven by the controller. Developed on an Xbox One X; the
Series X|S run the same build.

The app and its tools are in [`xbox/`](../../xbox/README.md); every quirk found along the way is in
[`xbox/docs/xbox-install.md`](../../xbox/docs/xbox-install.md).

Developer Mode builds are sideloaded to your own console only; they aren't distributed.

## What you need

- An **Xbox One** (any model) or **Xbox Series X|S**, on the same network as your computers.
- A **Windows PC** to build the app: Visual Studio (2022 or later) with the **Universal Windows
  Platform development** and **Desktop development with C++** workloads (toolset v145), and the
  Windows SDK 10.0.26100. Plus Python 3.12+ with `capstone` and `pefile`.
- **Any computer** (Mac, Linux or the same PC) with Python 3 to install onto the console:
  `xbox/tools/xbox.py` uses only the standard library.
- Your `FINAL FANTASY XI` folder (about 14 GB). The PlayOnline viewer folder isn't needed.
- Performance: on an Xbox One the CPU is several times slower than a current PC; expect about
  24 to 30 fps in busy scenes. The Series consoles have headroom.

## 1. Put the console in Developer Mode (once)

1. Register as a Microsoft developer in [Partner Center](https://partner.microsoft.com/dashboard)
   (a one-time fee for an individual account).
2. On the console, install **Xbox Dev Mode** (the Dev Mode Activation app) from the Store, open it,
   and follow its steps: it shows a code you enter in Partner Center, then the console restarts into
   Developer Mode. This can take a while and several restarts.
3. In Developer Mode, open **Dev Home**. Note the console's IP address.
4. In Dev Home, **Remote Access Settings**: turn on **Device Portal**. If you set a user name and
   password, export them as `XBOX_USER` and `XBOX_PASS` on the computer you'll install from.

Retail games don't run in Developer Mode; switch back to Retail Mode in Dev Home to play them.

## 2. Build the package (Windows PC)

```bat
git checkout xbox/v2-native-signin
python tools\prepare.py --game "<...>\FINAL FANTASY XI"      & rem once per game version
python tools\build.py uwp-lib                               & rem build\uwp\ffxi_uwp.lib
powershell -File xbox\tools\deploy-xbox.ps1 -PackageOnly     & rem build\xbox\XIonXbox-xbox.msix
```

The package is signed with a self-signed test certificate made on first use
(`build\xbox\XIonXbox.pfx`, not in git): Developer Mode installs test-signed packages. Copy
`XIonXbox-xbox.msix` (about 17 MB) to the computer you'll install from, if it's another one.

To try the same app on the PC first (Windows Developer Mode on): `xbox\tools\deploy.ps1 -Launch`.

## 3. Install on the console (once)

The app is registered as a **loose app** on the console's development drive, with the game uploaded
beside it in `D:\DevelopmentFiles\LooseApps\XIonXbox\SquareEnix\FINAL FANTASY XI`. The game goes up
once; updates replace only the app.

```bash
export XBOX=<console address>                 # remembered after the first use
python3 xbox/tools/xbox.py status             # what's there now
python3 -u xbox/tools/xbox.py setup build/xbox/XIonXbox-xbox.msix "<...>/FINAL FANTASY XI"
```

- It sends the app's files, registers the folder, then uploads the game: about 65,000 files and
  15 GB, about 40 minutes on a good wired LAN. Progress lines look like
  `12000/65252 files, 3.10/15.04 GB, 14.2 MB/s`.
- **Don't move the game folder until it finishes.** If the upload stops, run
  `python3 xbox/tools/xbox.py game "<...>/FINAL FANTASY XI"`: it sends only what's missing.
- If `status` says the app is **installed from a package**, a loose app can't replace it
  (`0x80073CFB`): `python3 xbox/tools/xbox.py uninstall` first (it backs up LocalState to
  `build/xbox/`).

Then:

1. `python3 xbox/tools/xbox.py launch`.
2. In Dev Home, select **XI on Xbox**, **View details**, and set its type to **Game**. As an App it
   gets about 1 GB of RAM, not enough.
3. Check it found the game: `python3 xbox/tools/xbox.py logs build/xbox/logs-check`, and look in
   `host64.log` for `[app] starting the game: D:\DevelopmentFiles\LooseApps\XIonXbox\...`.

## 4. Play

Start **XI on Xbox** from Dev Home or the console's collection. The sign-in screen comes up:

| Button | In the sign-in screen |
| --- | --- |
| D-pad | Move between fields and buttons |
| A | Pick, or open the on-screen keyboard over a field |
| B | Back |
| Start | Sign in |

Set your server in its **Settings** (with remember password, trust this console and the window
theme). Saved passwords and trust tokens go in the Windows credential locker. A USB keyboard works
too. Graphics settings are in the game's Config > Modern page.

## Updating

```bash
python3 xbox/tools/xbox.py deploy build/xbox/XIonXbox-xbox.msix
```

About 45 MB; the game stays where it is. A new game version: `prepare`, `uwp-lib`, the package, then
`deploy` (and `game` if the server's files changed).

## Files on the console

In the app's LocalState (`python3 xbox/tools/xbox.py logs <folder>` copies the logs):

| File | What |
| --- | --- |
| `host64.log` | The game's log and the app's `[app]` lines |
| `signin.cfg`, `settings.reg` | The sign-in screen's choices, the game's display settings |
| `xbox.cfg` | Optional: `Resolution=1920x1080` (else the TV's), `Profile=1`, and app defaults such as `FFXIServer=...` |
| `game.txt` | Optional: another game folder. A leftover one overrides the game beside the app; `status` flags it |
| `USER\` | Characters' settings, seeded from the install's |

## Troubleshooting

| Symptom | Cause and fix |
| --- | --- |
| Dev Home says "Please try again", or launch returns `0x80070002` | The loose folder lacks the exe's `AppxMetadata\CodeIntegrity.cat`. Run `deploy` again; it sends it and re-registers. |
| `0x80073CFB` when registering | A packaged install is in the way. `uninstall`, then `setup`. |
| `0x80073CF0` registering from a network share | Not supported; use `setup` (LooseApps). |
| "cannot open that FINAL FANTASY XI folder (error 65)" | `game.txt` points at an SMB share; the sandbox blocks it. Remove it: `xbox.py rm local:\LocalState game.txt`. |
| The small user drive (`U:`, about 3 GB) is full | An unfinished in-app copy: `xbox.py rmdir local:\LocalState SquareEnix`. |
| Uploads fail with HTTP 500 to LocalState | LocalState doesn't exist until the app's first run; launch it once. |

Not done yet: the sign-in screen's sound effects (the game's sound works).
