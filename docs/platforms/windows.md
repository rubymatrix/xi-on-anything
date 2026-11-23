# Windows (x64)

`host64.exe`: the game recompiled for 64-bit Windows, drawn with Direct3D 12, with the same sign-in
screen and Config > Advanced pages as on the Mac. There are two ways to get it:

1. **The launcher.** Download
   [XI-on-Anything-windows-x64-setup.exe](https://github.com/rubymatrix/xi-on-anything/releases/latest/download/XI-on-Anything-windows-x64-setup.exe)
   from the [latest release](https://github.com/rubymatrix/xi-on-anything/releases/latest) and run
   it. It installs and updates the game.
2. **Build from source**, as below.

## What you need

- Windows 10 or 11, x64, with a Direct3D 12 GPU (feature level 11_0 or later). Ray tracing also
  needs DXR and shader model 6.6.
- Python 3.12 or later from python.org (tick "Add python.exe to PATH"), then
  `pip install capstone pefile`.
- Git for Windows.
- One of these compilers:
  - **Visual Studio 2022 or later** (Community is fine) with the **Desktop development with C++**
    workload. The build finds it with `vswhere`; no Developer Prompt is needed.
  - **llvm-mingw** (no Visual Studio): unpack a release from
    [mstorsjo/llvm-mingw](https://github.com/mstorsjo/llvm-mingw/releases) (the `ucrt-x86_64` zip),
    and put its `bin` on `PATH` or set `LLVM_MINGW` to the unpacked folder. It also needs
    `mingw32-make` or `make` on `PATH` to build LuaJIT.
- **SDL3**: the VC development package, `SDL3-devel-3.4.16-VC.zip` from
  [libsdl-org/SDL releases](https://github.com/libsdl-org/SDL/releases), unpacked to
  `C:\Dev\SDL3\SDL3-3.4.16`, or anywhere with `SDL3_DIR` set to it. Both compilers use it.
- Optional: the **Windows SDK's `dxc`** (Visual Studio installs the SDK). The Visual Studio build
  finds it; for llvm-mingw, put it on `PATH`. Without it there is no ray-traced bounce light or
  ambient occlusion.
- Your `FINAL FANTASY XI` folder: a retail install
  (`C:\Program Files (x86)\PlayOnline\SquareEnix\FINAL FANTASY XI`) or one from your server.
  The game writes its `USER\` folder (settings, macros) there, unless you pass `--user-dir`.

## Build

In PowerShell or Command Prompt:

```bat
git clone https://github.com/rubymatrix/xi-on-anything.git
cd xi-on-anything
python tools\prepare.py --game "C:\Program Files (x86)\PlayOnline\SquareEnix\FINAL FANTASY XI"
```

Without `--game`, `prepare` uses the install folder from the registry (a retail install).

Then, with Visual Studio:

```bat
python tools\build.py host64
```

`build\host64.exe` is the game. Beside it: `SDL3.dll`, `runtime.json`, `host64.map`, and AMD's FSR
DLLs, downloaded at build time (about 57 MB). Offline, the build skips them and the game upscales
with FSR 1.

Or with llvm-mingw:

```bat
python tools\build_mingw.py host64
```

`host64.exe`, `SDL3.dll`, `runtime.json` and the FSR DLLs land in `build\mingw` (`--out` to change
it). This build opens no console window.

`build.cmd host64` is a shorthand for `python tools\build.py host64`. Builds are incremental; run
`prepare` again when your server hands out a new game version.

## Run

```bat
build\host64.exe --game "C:\Program Files (x86)\PlayOnline\SquareEnix\FINAL FANTASY XI" --server <your server>
```

The sign-in screen comes first (username, password, one-time code; the server in its Settings). To
skip it: `--user <account>` with the password in `FFXI_PASSWORD`: `set FFXI_PASSWORD=...` in
Command Prompt, `$env:FFXI_PASSWORD="..."` in PowerShell. "Remember password" and `--trust on`
keep their secrets in the Windows Credential Manager (generic credentials named
`FINAL FANTASY XI sign-in:...`). `--trust on` saves nothing without `--server`.

The Visual Studio build logs to the terminal it runs in. The llvm-mingw build has no console: to
keep its log, redirect it (`build\mingw\host64.exe ... > host64.log 2>&1`).

To start it with a double-click, make a shortcut to `host64.exe` and put the options in its
**Target** field.

Every option (`--dats`, `--fps-divisor`, `--ui-aspect`, ...) is in
[Running `host64`](../running.md#options). Graphics settings are in the game's Config > Advanced
pages.

## Files

| Where | What |
| --- | --- |
| `%APPDATA%\FFXIRecompile\FFXI\` | `signin.cfg`, `settings.reg`, `saved.reg`, `modern.cfg`, `addons.cfg`, addons (`xi\`, `ashita\`, `windower\`), optional `textures\`, and `patch.<version>.ver` for an install without a `patch.ver` |
| `%LOCALAPPDATA%\FFXI\` | `fx.txt` (the saved graphics settings) and caches (shaders and the like) |
| beside `host64.exe` | `patch.<version>.ver`, only when there is no data folder (no sign-in screen and no `--data-dir`) |

## The stand-in DLL (developers)

The 32-bit path runs the recompiled code inside the retail client instead of `host64`: a stand-in
`FFXiMain.dll` in the game folder.

```bat
python tools\build.py difftest
python tools\build.py host
python tools\install.py install
```

- `difftest`: x86, the CRT slice and its differential test (expect 0 mismatches).
- `host`: x86, the stand-in `FFXiMain.dll` and the boot test.
- `install`: puts the stand-in in the game folder. `restore` puts the original back; `status` shows
  what is there. Under Program Files, run it from an elevated shell.

It is for testing the translation against the retail DLL, not for playing.

## Troubleshooting

- **"SDL3 not found at ..."**: unpack the VC development package there, or set `SDL3_DIR`.
- **`vswhere` or `vcvarsall.bat` not found**: Visual Studio's C++ workload isn't installed; use
  llvm-mingw instead, or add the workload in the Visual Studio Installer.
- **Graphics problems without the game**: `python tools\build.py gfxtest` draws through the
  Direct3D 12 back end offscreen.
- **SmartScreen warns about `host64.exe`**: it is unsigned and built on your machine; choose
  More info, Run anyway.
