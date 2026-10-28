# XI on Xbox (developer mode)

The natively recompiled FINAL FANTASY XI as a UWP game on an Xbox One or Series console in developer
mode. This folder holds only what the Xbox needs on top of the rest of the repo: the app, its
packaging, and the console tools. The game, the runtime, and the sign-in screen are built from the
repo as `build\uwp\ffxi_uwp.lib`.

The second version, after [xi-on-xbox](https://github.com/rubymatrix/xi-on-xbox):

- **No XAML.** The app is a bare CoreWindow, and the Direct3D 12 back end's swap chain goes straight
  on it.
- **The native sign-in screen** (`host/signin.c`), the same one as on the Mac, in the game's own UI
  art, with its Settings (server, remembering the password, trusting the console, window theme).
  A controller drives it: the D-pad moves, A picks, or opens the on-screen keyboard over a field
  (a `CoreTextEditContext`, no text box), B goes back, and Start signs in. Saved passwords and trust
  tokens go in the Windows credential locker.

## Rules

- Same as the rest of the repo: **no Square Enix bytes in git.** The player supplies their own install.
- Dev mode builds are sideloaded to your own console. They aren't distributed.

## Building

On Windows with Visual Studio (the C++ UWP workload, toolset v145) and the Windows SDK 10.0.26100:

```
python tools\prepare.py                                # once per game version
python tools\build.py uwp-lib                          # build\uwp\ffxi_uwp.lib
powershell -File xbox\tools\deploy-xbox.ps1 -PackageOnly   # build\xbox\XIonXbox-xbox.msix, signed
```

`xbox\tools\deploy.ps1 -Launch` installs and runs the same app on this PC instead (developer mode on).

## Installing on the console

Set it up once, then each build is a small update that leaves the game in place. Why it's done this
way, and every quirk found on the way: [docs/xbox-install.md](docs/xbox-install.md).

```
XBOX=<console address> python3 xbox/tools/xbox.py setup build/xbox/XIonXbox-xbox.msix "<...>/FINAL FANTASY XI"
python3 xbox/tools/xbox.py deploy build/xbox/XIonXbox-xbox.msix     # each new build
python3 xbox/tools/xbox.py logs <folder>                              # host64.log, app.log, samples.bin
```

Claude Code skills for the same: `.claude/skills/xbox-setup`, `xbox-deploy`, `xbox-logs`.

## On the console

The app keeps everything in its LocalState:

| file | what |
|---|---|
| `host64.log` | the game's log, and the app's `[app]` lines |
| `signin.cfg`, `settings.reg` | the sign-in screen's choices, and the game's display settings |
| `xbox.cfg` | optional: `Resolution=1920x1080` (else the TV's), `Profile=1`, and host64's app defaults (`FFXIServer=...`, see `host/appdefaults.h`) |
| `game.txt` | optional: the game folder, over the one beside the app |
| `USER\` | the characters' settings, seeded from the install's |

Frame rate, draw distance and the rest are in the game's own Config > Modern page, as on the Mac.

## Not done yet

- The sign-in screen's sound effects: they want an audio device of their own, which the UWP build
  doesn't have yet. The game's sound is unchanged.
- Copying the game from a computer, which the first version could do: the install goes beside the app
  instead (`xbox.py setup`).

## Measuring

```
python xbox/tools/frame_budget.py build/run.log [--factor 6.5] [--min-draws 500]
```

It reads `host64`'s `FFXI_PROFILE=1` output and projects the game thread's busy time per frame onto a
slower CPU.
