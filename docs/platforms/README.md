# Platforms

How to get XI on Anything to the title screen on each platform. Every platform needs two things
from you:

- **Your own `FINAL FANTASY XI` folder** of a supported client build ([Supported builds](../new-client-version.md#supported-builds)),
  with `FFXiMain.dll` and `ROM` in it, and the viewer folder beside it if your install has one. The
  game is recompiled from these files on your machine. None of it comes from this repo.
- **A server to sign in to**: a LandSandBoat server, by name or IP address, or PlayOnline through
  the launcher ([Running](../running.md#options)).

| Platform | Status | How you get it | Guide |
| --- | --- | --- | --- |
| macOS (Apple silicon) | Supported | Launcher, or build from source | [macos.md](macos.md) |
| Linux (x86_64) | Supported | Launcher (AppImage), or build from source | [linux.md](linux.md) |
| Steam Deck | Supported | Launcher (AppImage), played from Game Mode | [steam-deck.md](steam-deck.md) |
| Windows (x64) | Supported | Launcher installer, or build from source | [windows.md](windows.md) |
| Web browser | Experimental, self-hosted only | Build and host it yourself | [web.md](web.md) |
| iPhone / iPad | Supported | Build on a Mac, install with a free Apple account | [ios.md](ios.md) |
| Xbox One / Series | Experimental | Developer Mode, sideloaded | [xbox-one.md](xbox-one.md) |
| PS4 | Experimental | Jailbroken console, fake PKG | [ps4.md](ps4.md) |
| Android (ARM64) | Experimental | Debug APK | [android.md](android.md) |
| Nintendo Switch | Experimental | Homebrew `.nro`, built on your PC | [switch.md](switch.md) |

## Where each port's code is

Everything builds from `main` except the PS4 port, which is on the `port/ps4` branch until it is
merged. Check it out before following its guide.

## Common to every build from source

- Python 3 with `capstone` and `pefile`: `pip3 install capstone pefile`. Where pip refuses
  (Ubuntu 24.04 and other PEP 668 systems), add `--break-system-packages`. On macOS Apple's
  `/usr/bin/python3` will do; on Windows, see [windows.md](windows.md).
- Run `prepare` once per client version. It identifies your build (`recomp/meta/builds.json`) and unpacks
  `FFXiMain.dll` and `FFXi.dll` into `generated/`. Run it again when your server hands out a new
  game version. An unknown DLL stops here with "is build &lt;sha&gt;, which recomp/meta/builds.json does
  not know": that build needs metadata first
  ([Supporting a new client version](../new-client-version.md#supporting-a-new-client-version)).
- `generated/` and `build/` hold code and files derived from the game. They are gitignored. Never
  commit them or share a built binary.
- Every host takes the same options (`--server`, `--user`, `--dats`, `--fps-divisor`, ...); see
  [Running `host64`](../running.md#options). Settings are in the game's Config menu
  ([../options.md](../options.md)).
