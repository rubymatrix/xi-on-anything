# Xbox One and Xbox Series (Developer Mode, experimental)

The recompiled game as a UWP app on a console in Developer Mode, drawn with Direct3D 12 on a
CoreWindow, with the native sign-in screen driven by the controller. Developed on an Xbox One X; the
Series X|S run the same build.

The app and its tools are in [`xbox/`](../../xbox/README.md); every quirk is in
[`xbox/docs/xbox-install.md`](../../xbox/docs/xbox-install.md).

Developer Mode builds are sideloaded to your own console only, not distributed.

## What you need

- An **Xbox One** (any model) or **Xbox Series X|S**, on the same network as your computers.
- A **Windows PC** to build the app: Visual Studio 2026 (toolset v145) with the **Universal Windows
  Platform development** and **Desktop development with C++** workloads, and the Windows SDK
  10.0.26100. Python 3.12+ with `capstone` and `pefile`. SDL3's VC development package, unpacked, for
  its headers (`SDL3_DIR`, default `C:\Dev\SDL3\SDL3-3.4.16`). Optional: the Windows SDK's `dxc` on
  `PATH`, for the ray-traced effects.
- **Any computer** (Mac, Linux or the same PC) with Python 3 and a checkout of this repo, to install
  onto the console. `xbox/tools/xbox.py` needs only the standard library; it writes to `build/xbox/`.
- Your `FINAL FANTASY XI` folder (about 15 GB). The PlayOnline viewer folder isn't needed.
- Performance: an Xbox One's CPU is several times slower than a current PC's; expect 24 to 30 fps
  in busy scenes. The Series consoles have headroom.

## 1. Put the console in Developer Mode (once)

1. Register as a Microsoft developer in [Partner Center](https://partner.microsoft.com/dashboard)
   (a one-time fee for an individual account).
2. On the console, install **Xbox Dev Mode** (the Dev Mode Activation app) from the Store, open it,
   and follow its steps: you enter its code in Partner Center, then the console restarts into
   Developer Mode. This can take a while and several restarts.
3. In Developer Mode, open **Dev Home**. Note the console's IP address.
4. In Dev Home, **Remote Access Settings**: turn on **Device Portal**. If you set a user name and
   password, export them as `XBOX_USER` and `XBOX_PASS` on the computer you'll install from.

Retail games don't run in Developer Mode; switch back to Retail Mode in Dev Home for them.

## 2. Build the package (Windows PC)

```bat
python tools\prepare.py --game "<...>\FINAL FANTASY XI"      & rem once per game version
python tools\build.py uwp-lib                               & rem build\uwp\ffxi_uwp.lib
powershell -File xbox\tools\deploy-xbox.ps1 -PackageOnly     & rem build\xbox\XIonXbox-xbox.msix
```

The package is signed with a self-signed test certificate made on first use
(`build\xbox\XIonXbox.pfx`, not in git); Developer Mode accepts that. Copy `XIonXbox-xbox.msix`
(about 17 MB) to the computer you'll install from, if it's another one.

To try the app on the PC first (Windows Developer Mode on):
`powershell -File xbox\tools\deploy.ps1 -Launch`. To reach a server on the same PC, run the
`CheckNetIsolation LoopbackExempt` command it prints, once, as administrator.

## 3. Install on the console (once)

The app is registered as a **loose app** on the console's development drive, with the game beside it
in `D:\DevelopmentFiles\LooseApps\XIonXbox\SquareEnix\FINAL FANTASY XI`. The game goes up once;
updates replace only the app.

```bash
export XBOX=<console address>                 # remembered after the first use
python3 xbox/tools/xbox.py status             # what's there now
python3 -u xbox/tools/xbox.py setup build/xbox/XIonXbox-xbox.msix "<...>/FINAL FANTASY XI"
```

- It sends the app's files, registers the folder, then uploads the game: about 65,000 files and
  15 GB, about 40 minutes on a good wired LAN. Progress looks like
  `12000/65252 files, 3.10/15.04 GB, 14.2 MB/s`.
- **Don't move the game folder until it finishes.** If the upload stops, run
  `python3 xbox/tools/xbox.py game "<...>/FINAL FANTASY XI"`: it sends only what's missing.
- If `status` says **installed from a package**, a loose app can't replace it (`0x80073CFB`). Run
  `python3 xbox/tools/xbox.py uninstall` first; it backs up LocalState to `build/xbox/`.

Then:

1. `python3 xbox/tools/xbox.py launch`.
2. In Dev Home, select **XI on Xbox**, **View details**, and set its type to **Game**. As an App it
   gets about 1 GB of RAM, too little.
3. Check it found the game: `python3 xbox/tools/xbox.py logs build/xbox/logs-check`, and look in
   `host64.log` for `[app] starting the game: D:\DevelopmentFiles\LooseApps\XIonXbox\...`.

## 4. Play

Start **XI on Xbox** from Dev Home or the console's collection. The sign-in screen comes up:

| Button | In the sign-in screen |
| --- | --- |
| D-pad or left stick | Move between fields and buttons (held, it repeats) |
| A | Pick, or open the on-screen keyboard over a field |
| B | Back |
| Start | Sign in |

The server is `game.phoenix-xi.com`. Change it in **Settings** or with `FFXIServer=` in `xbox.cfg`.
Settings also has Remember Password, Trust This Computer, Window Theme and Full Screen. Saved
passwords and trust tokens go in the Windows credential locker. A USB keyboard works too.

In the game, the first controller works: buttons, sticks, triggers and rumble. So does a mouse. Set
the buttons in Config > Gamepad (an F12 window on clients without Config > Advanced). Graphics
settings are in Config > Advanced.

## Updating

```bash
python3 xbox/tools/xbox.py deploy build/xbox/XIonXbox-xbox.msix
```

It stops the app, sends about 45 MB, registers it again and launches it. The game stays put. It
refuses an app installed from a package.

A new game version: `prepare`, `uwp-lib`, the package, `deploy`, then `game`. `game` skips files already there at the same size, so a changed file that kept its size
isn't sent. To send every file again (all 15 GB):
`python3 xbox/tools/xbox.py push "<...>/FINAL FANTASY XI" 'dev:\LooseApps\XIonXbox\SquareEnix\FINAL FANTASY XI' --all`.

## Files on the console

In the app's LocalState (`python3 xbox/tools/xbox.py logs <folder>` copies the logs):

| File | What |
| --- | --- |
| `host64.log` | The game's log and the app's `[app]` lines |
| `host64.out.log` | The game's standard output |
| `app.log` | The app's errors from before the game's log opens |
| `samples.bin` | With `Profile=1`: where the game thread's time goes |
| `pipelines.d3d12.v1` | The Direct3D 12 pipeline cache |
| `signin.cfg`, `settings.reg` | The sign-in screen's choices, the game's display settings |
| `xbox.cfg` | Optional: `Resolution=1920x1080` (else the TV's), `Profile=1`, and app defaults such as `FFXIServer=...` |
| `game.txt` | Optional: another game folder. A leftover one wins over the game beside the app; `status` flags it |
| `USER\` | Characters' settings, seeded from the install's |

## Troubleshooting

| Symptom | Cause and fix |
| --- | --- |
| Dev Home says "Please try again", or launch returns `0x80070002` | The loose folder lacks the exe's `AppxMetadata\CodeIntegrity.cat`. Run `deploy` again. |
| `0x80073CFB` when registering | A packaged install is in the way. `uninstall`, then `setup`. |
| `0x80073CF0` registering from a network share | Not supported; use `setup`. |
| "The app cannot open the FINAL FANTASY XI folder ... (error 65)" | `game.txt` points at an SMB share; the sandbox blocks it. Remove it: `xbox.py rm 'local:\LocalState' game.txt`. |
| The small user drive (`U:`, about 3 GB) is full | A game copy left by the first XI on Xbox: `xbox.py rmdir 'local:\LocalState' SquareEnix`. |
| Uploads fail with HTTP 500 to LocalState | LocalState doesn't exist until the app's first run; launch it once. |

Not done yet: the sign-in screen's sound effects. The game's sound works.
