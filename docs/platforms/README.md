# Platforms

How to get XI on Anything running on each platform, from nothing to the title screen. Every
platform needs the same two things from you:

- **Your own `FINAL FANTASY XI` folder** of a supported client build (the README's Rules table),
  with `FFXiMain.dll` and `ROM` in it, and the viewer folder beside it if your install has one. The
  game is recompiled from these files on your machine; none of it comes from this repo.
- **A LandSandBoat server** to sign in to, by name or IP address.

| Platform | Status | How you get it | Guide |
| --- | --- | --- | --- |
| macOS (Apple silicon) | Supported | Launcher, or build from source | [macos.md](macos.md) |
| Linux (x86_64) | Supported | Launcher (AppImage), or build from source | [linux.md](linux.md) |
| Steam Deck | Supported | Launcher (AppImage), played from Game Mode | [steam-deck.md](steam-deck.md) |
| Windows (x64) | Build from source | Visual Studio or llvm-mingw | [windows.md](windows.md) |
| Web browser | Experimental, self-hosted only | Build and host it yourself | [web.md](web.md) |
| iOS / iPadOS | Experimental, in progress | Build in Xcode's toolchain, sideload | [ios.md](ios.md) |
| Xbox One / Series | Experimental | Developer Mode, sideloaded | [xbox-one.md](xbox-one.md) |
| PS4 | Experimental, not yet run on a console | Jailbroken console, fake PKG | [ps4.md](ps4.md) |
| Android (ARM64) | Experimental | Debug APK | [../android.md](../android.md) |

## Where each port's code is

macOS, Linux, the Steam Deck, Windows and Android build from `main`. The other ports are on their
own branches until they are merged; check out that branch before following its guide:

| Port | Branch |
| --- | --- |
| Web browser | `feat/web` |
| iOS / iPadOS | `port/iphone` |
| Xbox One / Series | `xbox/v2-native-signin` |
| PS4 | `port/ps4` |

## Common to every build from source

- Python 3.12 or later, with `capstone` and `pefile` (`pip3 install capstone pefile`).
- `prepare` once per client version: it identifies your build (`meta/builds.json`) and unpacks
  `FFXiMain.dll` and `FFXi.dll` into `generated/`. Run it again when your server hands out a new
  game version. A DLL the repo doesn't know stops here with "is build &lt;sha&gt;, which
  meta/builds.json does not know": that build needs metadata first (README: Supporting a new client
  version).
- `generated/` and `build/` hold code and files derived from the game. They are gitignored; never
  commit them, and never share a built binary.
- Every host takes the same options (`--server`, `--user`, `--dats`, `--fps-divisor`, ...), listed
  in the README under Running: `host64`. Settings are in the game's own Config menu
  ([../options.md](../options.md)).
