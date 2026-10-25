# Windows (x64)

`host64.exe`: the game recompiled for 64-bit Windows, drawn with Direct3D 12, with the same sign-in
screen and Config > Modern page as on the Mac. There is no Windows launcher yet: you build it from
source.

## What you need

- Windows 10 or 11, x64, with a Direct3D 12 GPU (feature level 12_0 or later).
- Python 3.12 or later from python.org (tick "Add python.exe to PATH"), then
  `pip install capstone pefile`.
- Git for Windows.
- One of these compilers:
  - **Visual Studio 2022 or later** (Community is fine) with the **Desktop development with C++**
    workload. The build finds it with `vswhere`; no Developer Prompt is needed.
  - **llvm-mingw** (no Visual Studio): unpack a release from
    [mstorsjo/llvm-mingw](https://github.com/mstorsjo/llvm-mingw/releases) (the `ucrt-x86_64` zip),
    and put its `bin` on `PATH` or set `LLVM_MINGW` to the unpacked folder.
- **SDL3**: the VC development package, `SDL3-devel-3.4.16-VC.zip` from
  [libsdl-org/SDL releases](https://github.com/libsdl-org/SDL/releases), unpacked to
  `C:\Dev\SDL3\SDL3-3.4.16`, or anywhere with `SDL3_DIR` set to it. Both compilers use this one.
- Optional: the **Windows SDK's `dxc`** on `PATH` (Visual Studio installs the SDK). Without it the
  build still works, but the effects that trace rays (Config > Modern's scene renderer) stay off.
- Your `FINAL FANTASY XI` folder: a retail install
  (`C:\Program Files (x86)\PlayOnline\SquareEnix\FINAL FANTASY XI`) or one from your server.
  It is only read, never written.

## Build

In PowerShell or Command Prompt:

```bat
git clone https://github.com/rubymatrix/xi-on-anything.git
cd xi-on-anything
python tools\prepare.py --game "C:\Program Files (x86)\PlayOnline\SquareEnix\FINAL FANTASY XI"
```

Without `--game`, `prepare` uses the install folder in the game's registry key (a retail install).

Then, with Visual Studio:

```bat
python tools\build.py host64
```

`build\host64.exe` is the game, with `SDL3.dll` beside it.

Or with llvm-mingw:

```bat
python tools\build_mingw.py host64
```

`build\mingw\host64.exe`, `SDL3.dll` and `runtime.json` land in `build\mingw` (`--out` to change it).

`build.cmd host64` is a shorthand for `python tools\build.py host64`. Builds are incremental; run
`prepare` again when your server hands out a new game version.

## Run

```bat
build\host64.exe --game "C:\Program Files (x86)\PlayOnline\SquareEnix\FINAL FANTASY XI" --server <your server>
```

The sign-in screen comes first (username, password, one-time code; the server in its Settings). To
skip it: `--user <account>` with the password in `FFXI_PASSWORD` (`set FFXI_PASSWORD=...`), so it
stays out of your shell history. "Remember password" and `--trust on` keep their secrets in the
Windows Credential Manager (generic credentials named `FINAL FANTASY XI sign-in:...`).

To start it with a double-click, make a shortcut to `host64.exe` and put the options in its
**Target** field.

Every option (`--dats`, `--fps-divisor`, `--ui-aspect`, ...) is in
[Running `host64`](../running.md#options). Graphics settings are in the game's Config > Modern page.

## Files

| Where | What |
| --- | --- |
| `%APPDATA%\FFXIRecompile\FFXI\` | `signin.cfg`, `settings.reg`, `saved.reg`, addons (`ashita\`, `windower\`) |
| `%LOCALAPPDATA%\FFXI\` | Caches (shaders and the like); safe to delete |
| beside `host64.exe` (or `--data-dir`) | the `patch.ver` made for an install without one |

## The stand-in DLL (developers)

The 32-bit path runs the recompiled code inside the retail client instead of `host64`: a stand-in
`FFXiMain.dll` in the game folder.

```bat
python tools\build.py difftest      & rem x86: the CRT slice, differential test (expect 0 mismatches)
python tools\build.py host          & rem x86: the stand-in FFXiMain.dll and the boot test
python tools\install.py install     & rem put it in the game folder; "undo" restores the original
```

It is for testing the translation against the retail DLL, not for playing.

## Troubleshooting

- **"SDL3 not found at ..."**: unpack the VC development package there, or set `SDL3_DIR`.
- **`vswhere` or `vcvarsall.bat` not found**: Visual Studio's C++ workload isn't installed; use
  llvm-mingw instead, or add the workload in the Visual Studio Installer.
- **Graphics problems without the game**: `python tools\build.py gfxtest` draws through the
  Direct3D 12 back end offscreen.
- **SmartScreen warns about `host64.exe`**: it is unsigned and built on your machine; choose
  More info, Run anyway.
