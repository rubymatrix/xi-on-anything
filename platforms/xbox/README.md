# XI on Xbox (developer mode)

The natively recompiled FINAL FANTASY XI as a UWP game on an Xbox One or Series console in developer
mode. This folder holds only what the Xbox adds to the repo: the app, its packaging and the console
tools. The game, the runtime and the sign-in screen build from the repo as `build\uwp\ffxi_uwp.lib`.

The second version, after [xi-on-xbox](https://github.com/rubymatrix/xi-on-xbox):

- **No XAML.** The app is a bare CoreWindow, with the Direct3D 12 swap chain straight on it.
- **The native sign-in screen** (`host/signin/signin.c`), the same as on the Mac. Its Settings: server,
  Remember Password, Trust This Computer, Window Theme, Full Screen. The D-pad or left stick moves,
  A picks or opens the on-screen keyboard over a field (a `CoreTextEditContext`), B goes back, Start
  signs in. Saved passwords and trust tokens go in the Windows credential locker.
- **The controller in the game**: the first one, with buttons, sticks, triggers and rumble.

## Rules

- **No Square Enix bytes in git.** The player supplies their own install.
- Dev mode builds are sideloaded to your own console, not distributed.

## Building

On Windows with Visual Studio 2026 (toolset v145; the UWP and Desktop C++ workloads), the Windows SDK
10.0.26100, Python 3.12+ with `capstone` and `pefile`, and SDL3's headers (`SDL3_DIR`, default
`C:\Dev\SDL3\SDL3-3.4.16`). Details: [docs/platforms/xbox-one.md](../../docs/platforms/xbox-one.md).

```
python tools\prepare.py --game "<...>\FINAL FANTASY XI"   # once per game version
python tools\build.py uwp-lib                          # build\uwp\ffxi_uwp.lib
powershell -File platforms\xbox\tools\deploy-xbox.ps1 -PackageOnly   # build\xbox\XIonXbox-xbox.msix, signed
```

`powershell -File platforms\xbox\tools\deploy.ps1 -Launch` runs the same app on this PC (developer mode on). To
reach a server on this PC, run the `CheckNetIsolation LoopbackExempt` line it prints, once, as
administrator.

## Installing on the console

Set it up once; each build after that is a small update that leaves the game in place. Why, and every
quirk: [docs/xbox-install.md](docs/xbox-install.md).

```
XBOX=<console address> python3 platforms/xbox/tools/xbox.py setup build/xbox/XIonXbox-xbox.msix "<...>/FINAL FANTASY XI"
python3 platforms/xbox/tools/xbox.py deploy build/xbox/XIonXbox-xbox.msix     # each new build
python3 platforms/xbox/tools/xbox.py logs <folder>                              # the logs, samples.bin, settings.reg
```

Claude Code skills: `.claude/skills/xbox-setup`, `xbox-deploy`, `xbox-logs`.

## On the console

In the app's LocalState:

| file | what |
|---|---|
| `host64.log` | the game's log, and the app's `[app]` lines |
| `host64.out.log` | the game's standard output |
| `app.log` | the app's errors from before the game's log opens |
| `samples.bin` | with `Profile=1`: where the game thread's time goes |
| `pipelines.d3d12.v1` | the Direct3D 12 pipeline cache |
| `signin.cfg`, `settings.reg` | the sign-in screen's choices, the game's display settings |
| `xbox.cfg` | optional: `Resolution=1920x1080` (else the TV's), `Profile=1`, and app defaults (`FFXIServer=...`, see `host/appdefaults.h`) |
| `game.txt` | optional: a game folder. Wins over the one beside the app, even if unreadable; `status` flags it |
| `USER\` | the characters' settings, seeded from the install's |

Frame rate, draw distance and the rest are in the game's Config > Advanced page, as on the Mac.

## Not done yet

- The sign-in screen's sound effects. They need their own audio device, which the UWP build lacks.
  The game's sound works.

Dropped on purpose: the first version's copy of the game into LocalState. The game doesn't fit on the
console's 3 GB user drive, so it goes beside the app (`xbox.py setup`).

## Measuring

```
python platforms/xbox/tools/frame_budget.py <folder>/host64.log [--factor 6.5] [--min-draws 500]
```

Set `Profile=1` in `xbox.cfg`, play, then pull `host64.log` with `xbox.py logs <folder>`. The tool
projects the game thread's busy time per frame onto a slower CPU.
