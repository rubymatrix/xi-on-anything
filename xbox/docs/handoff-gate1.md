# Handoff: Gate 1 on Windows (measure the CPU budget on the Xbox One)

For a Claude Code session on the user's Windows machine. Read `README.md` and
`docs/feasibility.md` in this repo first. They take two minutes and explain why this gate
exists.

## The goal in one sentence

Run the recompiled game headless (no graphics, no SDL) as a UWP app on the Xbox One in
developer mode, get a character into a busy zone, and collect `FFXI_PROFILE=1` frame
timings that `xbox/tools/frame_budget.py --factor 1` can read.

**Pass:** the median busy time per frame on the console is at most 33 ms (30 fps), or the
game code alone is at most about 25 ms, so that a render thread can close the gap.
**Fail:** the game code alone is well over 33 ms. Then stop and report back. The port isn't
worth building.

Report the numbers either way. Add a "Gate 1 results" section to `docs/feasibility.md`
with the table from `frame_budget.py`.

## Background you need

- **xi-on-anything** (`git@github.com:rubymatrix/xi-on-anything.git`, branch `main`, clean at
  `253b23c` or later) statically recompiles `FFXiMain.dll` and `FFXi.dll` from x86-32 to C.
  Its portable runtime (`runtime/portable/`) emulates the Win32 surface the game uses:
  files, registry, threads, sockets, DirectInput, DirectSound, polcore and D3D8. It sits on
  a small platform layer, `plat.h`, implemented in `plat_win.c` and `plat_posix.c`.
- `host/host64.c` is the game host. On Windows it builds with
  `python tools\build.py host64` (MSVC x64, SDL3 at `C:\Dev\SDL3\SDL3-3.4.16`, mbedtls at
  `C:\Dev\mbedtls`). **On Windows it links `gfx_null.c`: it has no graphics at all yet.**
  That's fine for this gate.
- **No Square Enix bytes in either repo, ever.** No DLLs, DATs or generated C. `generated/`
  and `build/` are gitignored. The player's install is the input.
- **This FFXIXbox repo** will hold the UWP shell and the packaging. Changes to the
  runtime (the profile line in `gfx_null.c`, the UWP fixes in `plat_win.c`, a new SDL stub)
  go into **xi-on-anything**, in their own commits, so macOS and Windows keep building.
  Rebuild on macOS isn't possible from Windows: keep changes out of `plat_posix.c` and
  `gfx_metal.m`, and anything shared must stay plain C11.

## Why each piece is needed

| Problem on the Xbox | Why | What to do |
|---|---|---|
| No SDL | SDL3 has no UWP/WinRT back end (it only supports Xbox through the GDK, which needs an ID@Xbox license) | Stub it (step 2) |
| `gfx_null.c` prints no profile line | The profile is only implemented in `gfx_metal.m` (`prof_frame`, around line 240) | Port it (step 1) |
| Win32 APIs outside the UWP app partition | `plat_win.c` calls `CreateFileW`, `FindFirstFileW`, `VirtualAlloc` and others | Fix under `WINAPI_FAMILY_APP` (step 3) |
| No input for the login screens | Headless: no window, no controller | Scripted key presses (step 2) |
| No stdout/stderr on the console | UWP | Log to a file in LocalState (step 4) |
| The install folder is read-only | UWP package install dir | Anything written (`patch.ver`, `USER\`) goes to LocalState (step 4) |

## Steps

### 1. A profile line for `gfx_null.c` (xi-on-anything)

Port the frame profile from `gfx_metal.m` (`g_prof`, `gfx_prof_front`, `gfx_prof_shim`,
`prof_frame`) into `gfx_null.c`, which today stubs them out (lines 57–61). Print exactly
the format `xbox/tools/frame_budget.py` parses, with `GPU 0.00 ms`:

```
[gfx] 58.4 fps: frame 17.11 ms = game code 2.53 + API calls 14.58 (draws 1.31 [encode 1.16], probe wait 0.00, present 0.06 [drawable 0.03], other 13.21) | GPU 0.00 ms | 753 draws, 3049 KB up, 0 new pipelines
```

Count draws in the null back end's draw entry points, because `frame_budget.py` uses the
draw count to tell in-zone frames (>= 500 draws) from menus. `host64.c`'s `present_hook`
already emits the `API time (2 s): Sleep ...` line through `thunk_prof_report()`.
`gfx_prof_shim` must only count time on the thread that calls Present, as the Metal
version does. Use `QueryPerformanceCounter` for `gfx_now_ns` on Windows (or
`rt_monotonic_ns`, which already exists).

**Check:** desktop `build\host64.exe` with `FFXI_PROFILE=1` prints the line every 2 s,
and `frame_budget.py` reads the log.

### 2. An SDL stub and scripted input (xi-on-anything)

`user32.c`, `input.c`, `dsound.c` and `dinput.c` call about 40 distinct SDL functions
(`grep -n "SDL_[A-Za-z]*(" runtime/portable/*.c`). Write
`runtime/portable/sdl_stub.c`, compiled against the real SDL3 **headers** but linked
**instead of** `SDL3.lib`:

- Windows and events: create returns a dummy non-NULL pointer; `SDL_PollEvent` returns
  events from the input script (below), then false.
- Mutexes: wrap a Windows `SRWLOCK` or `CRITICAL_SECTION`.
- **Audio: don't just return false.** `dsound.c` advances the DirectSound play cursors,
  and signals `IDirectSoundNotify` events, from the SDL audio callback in 480-frame (10 ms)
  steps. The game may wait on those events. Give `SDL_OpenAudioDeviceStream` a thread that
  calls the callback every 10 ms and throws the data away.
- Gamepads: none.

**The input script.** Getting from the title screen to a zone takes keystrokes (Enter
through the title, the lobby, character select and the confirm). Read a script from
`FFXI_INPUT_SCRIPT` (a file path), one step per line, for example:

```
wait 20000
key RETURN
wait 3000
key RETURN
```

Emit `SDL_EVENT_KEY_DOWN` and `SDL_EVENT_KEY_UP` with the matching `SDL_Scancode`.
Work out the real sequence on the Mac, where there's a picture, or ask the user which keys
it takes. The zone-in is confirmed when the profile's draw count goes above 500.

Add a `host64-headless` target to `tools\build.py`: the same sources, plus `sdl_stub.c`,
without `SDL3.lib`. **Check:** it reaches a zone on the desktop, by the draw count.

### 3. The UWP app partition (xi-on-anything)

Compile the runtime with `/DWINAPI_FAMILY=WINAPI_FAMILY_APP` first. The compiler then names
every call outside the partition, without deploying anything. Expected changes in
`plat_win.c`:

- `CreateFileW` becomes `CreateFile2`.
- `FindFirstFileW` becomes `FindFirstFileExW`.
- `VirtualAlloc` becomes `VirtualAllocFromApp` if the SDK flags it. The guest window is one
  4 GB reservation (`gwin.c:37`), followed by page commits. It must never ask for executable
  memory, and it doesn't.
- `GlobalMemoryStatusEx` and `GetTimeZoneInformation` may need alternatives.
- In `ws2.c` and `lsb_login.c`, check `gethostbyname` and friends. Winsock itself is allowed
  in UWP.

Keep everything behind `#if WINAPI_FAMILY == WINAPI_FAMILY_APP` or use the UWP-allowed
call everywhere if it works on desktop too. Desktop `host64` must keep building and running.

### 4. The UWP shell (this repo)

A C++/WinRT `CoreApplication` app (Visual Studio 2022, "Universal Windows Platform
development" workload, a recent Windows 10/11 SDK), x64 only:

- `IFrameworkView::Run` starts the host on its own thread, then runs the `CoreWindow`
  event loop until the host exits. Rename `host64`'s `main` for this build
  (`/Dmain=host_main`) and link the xi-on-anything objects: the generated C, the runtime,
  `gfx_null.c`, `sdl_stub.c` and `lsb_login.c`.
- **Arguments** come from `LocalState\args.txt`, one argument per line. That keeps the
  password out of the package.
- **Logging:** `freopen` stdout and stderr onto `LocalState\host64.log` and flush often.
  `plat_debug` already goes to stderr.
- **Game data:** `--game` must point at a folder the app can read. The simplest option is to
  add the user's `SquareEnix` folder (`FINAL FANTASY XI` and the viewer folder beside it) as loose content in the
  Visual Studio deployment layout (never committed), then read them from
  `Package.Current.InstalledLocation`. The first deploy copies about 14 GB over the network;
  later ones are incremental. The Device Portal file browser (upload into LocalState) is the
  fallback.
- **Writes:** `host64.c` writes `patch.<version>.ver` next to `argv[0]`, and the game
  writes `USER\` under its own folder. The install location is read-only. Point both at
  LocalState with `vfs_mount`. Make a `USER` copy in LocalState on the first run if the
  install has one.
- **Manifest:** `TargetDeviceFamily` `Windows.Universal` (or `Windows.Xbox`), plus the
  capabilities `internetClient` and `privateNetworkClientServer`. The latter is needed to
  reach a server on the LAN.
- **After installing:** in Dev Home on the console, open the app's details and set its
  type to **Game**. As an "App" it gets about 1 GB of RAM and a reduced CPU and GPU share,
  and the measurement would be meaningless.
- Build it with the options xi-on-anything uses (`/O2`). **Don't use `/arch:AVX2`**: the
  Jaguar has AVX but not AVX2. The default (SSE2) is safe.

### 5. Server and login

Use the LandSandBoat path: `--server <LAN address> --user <account> --pass <password>`.
`lsb_login.c` signs in before the game starts.

**Ask the user which server to use.** The Mac runs used a local LSB server
(`127.0.0.1`, account `tester`) and a server at `100.76.91.127`, which is a Tailscale
address. **The Xbox can't join Tailscale.** It needs a server it can reach on the
LAN, or a Tailscale subnet router on the LAN. Never commit credentials. They only go in
`LocalState\args.txt` on the console.

### 6. Measure

Deploy with `FFXI_PROFILE=1`. Environment variables can't be set for a UWP app, so let
`args.txt` carry them: for example, a line `env FFXI_PROFILE=1` that the shell applies with
`_putenv` before starting the host. Also `env FFXI_INPUT_SCRIPT=...`.

Stand in a busy place: a city at a busy time, as in the Mac runs. Let it run 5+ minutes.
Pull `LocalState\host64.log` through Device Portal, then:

```
python tools\frame_budget.py host64.log --factor 1
python tools\frame_budget.py host64.log --factor 1 --min-draws 1500
```

Add the results to `docs/feasibility.md` under "Gate 1 results", with the console model,
the OS build, and the app type (it must say Game).

## What the user needs to supply

- An Xbox One in developer mode, paired with Visual Studio or reachable through Device
  Portal. (Dev mode needs a Microsoft Partner Center developer account, a one-time $19.)
- Their FFXI install on the Windows machine. xi-on-anything's `python tools\prepare.py`
  unpacks it into `generated\`, if that hasn't been done there already.
- The server and the account to log in with (step 5).
- The key sequence from the title screen to the zone, if it can't be worked out.

## Useful references in xi-on-anything

| what | where |
|---|---|
| Host entry point and argument parsing | `host/host64.c` |
| Profile line (to port) | `runtime/portable/gfx_metal.m` around lines 205–270 (`prof_frame`) |
| Null back end | `runtime/portable/gfx_null.c` |
| Platform layer | `runtime/portable/plat.h`, `plat_win.c` |
| Windows build | `tools/build.py` (`host64()`, around line 159) |
| Guest window reservation | `runtime/portable/gwin.c:37` |
| Audio callback and cursors | `runtime/portable/dsound.c` around lines 180–215 |
| Profiling knobs | `FFXI_PROFILE=1`, `FFXI_FPS=0` (overlay off, Metal only), `FFXI_RECOMP_MISSING=1` |
