# Nintendo Switch port: developer notes

The player's guide is [platforms/switch.md](platforms/switch.md). This page is how the port is put
together, what has been verified, and what is left.

## Layout

| Path | What it is | Modeled on |
| --- | --- | --- |
| `tools/build_switch.py` | `plat-test`, `host`, `setup`, `install` (host then setup, the launcher's entry, as `build_android.py install`; `FFXI_PROGRESS=1` prints `@phase`/`@progress` as `build_posix.py`; `runtime.json` stamped beside the `.nro` on the card), `run`, `deploy`, `logs` | `build_android.py`, `xbox.py` |
| `tools/setup_switch.py`, `setup_switch_gui.py`, `switch/setup-switch.bat`/`.sh` | the player's one command (the GUI, tkinter, runs it with the window's answers and installs what the PC lacks): checks the PC, then runs `prepare.py`, `frame_sites.py` (when the build has none), `build_switch.py host` (its `@progress` lines, `FFXI_PROGRESS=1`, drive the bar) and `build_switch.setup()`; answers kept in `build/switch/setup.json`, output in `build/switch/setup.log` | `tools/setup.py` (the Mac's) |
| `tools/switch_toolchain.py` | finds devkitPro (and its msys2 on Windows) and the emulator | `android_toolchain.py` |
| `host/switch_main.c` | libnx entry: sockets, the screen's size, `run.args`, the log; calls `xi_host_main` | `host/ios_main.c` |
| `host/switch_settings.c` | the settings app (`build_switch.py settings`, `xi-on-anything-settings.nro`, its own folder on the card): libnx's text console over `data/settings.reg` (resolution, interface, background, as modern.c's `save_display`) and `run.args` (the other lines kept); + starts the game (`envSetNextLoad`) | |
| `runtime/portable/sdl_nx.c`, `nx_bridge.h` | the SDL3 calls the runtime makes, over libnx (HID, touch, swkbd, audout) | `sdl_web.c`, `sdl_uwp.c` |
| `runtime/portable/plat_posix.c` (`__SWITCH__`) | memory, address waits, thread ids, time | the Emscripten branch |
| `runtime/portable/tls_nx.c`, `switch/mbedtls_config.h` | mbedtls' clock and entropy | — |
| `switch/app.json` | the application NPDM (for the forwarder) | hbloader's |
| `tests/switch_plat_test.c` | the platform layer's self-test, as its own `.nro` | — |
| `runtime/portable/gfx_gl.c`, `gfx_gl_api.h` | the graphics: OpenGL 4.3 core on Mesa's nouveau through EGL; also builds on the desktop (SDL3's GL context) to be tested there | `gfx_webgpu.c`, `gfx_vulkan.c` |
| `tests/switch_gfx_test.c` | `tests/gfx_test.c` on GL as its own `.nro` (`build_switch.py gfx-test`) | — |

The translation is the desktop one (`generated/all`, `generated/ffxi`): the generated C is portable, and
the Switch adds no wraps or hooks. Compile flags: `-D__SWITCH__ -DRT_GUEST_WINDOW -DFFXI_SWITCH
-DFFXI_GUEST_2GB`, libnx's `-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE`, GCC (devkitA64).
The addon host is not built (`host/addons_none.c`), as in the browser build; Discord is off.

## Graphics

`gfx_gl.c` is gfx.h on OpenGL 4.3 core, its shaders the generator's GL dialect (`gfx_gl_generate`, gfx_msl.c):
Vulkan's GLSL without descriptor sets, clip-space y negated (row 0 of every texture and target is D3D's top
row, so nothing is flipped but the present) and z from glClipControl when the driver has it. The vertex
streams are texture buffers read with `texelFetch`: Mesa compiles storage-buffer reads to the generic `LD`,
which Ryubing's shader translator lacks. Every D3D8 format the game uses is a GL one (BGRA, the packed
16-bit types, swizzles, S3TC), so nothing is converted on the CPU.

GL's context is current on one thread, and the game makes buffers and reads its occlusion probes from
threads of its own, so the game is built with the render queue (`GFX_QUEUE`, gfx_queue.c), which with
`GFX_GL` brings the device up on its render thread and makes every call there (`GFXQ_OWNED`, as WebGPU's
in the browser).

It is built and tested on a desktop first: `tools/build.py gfxtest-gl` (tests/gfx_test.c, offscreen) and
`tools/build.py host64-gl` (the game; `FFXI_GL_DEBUG=1` for the debug context and checks). On the Switch
the surface is 1920x1080, cropped to the screen's size at each present; the log lines of the back end go
out as debug strings (`svcOutputDebugString`, in Ryubing's log).

Threads on the Switch: `plat_thread_start` puts them on cores 1 and 2 (the main thread, the game's, has
core 0) at the main thread's priority. Horizon doesn't share a core between threads of one priority, so a
thread that never waits - sdl_nx.c's audio thread did, with nothing mixed - keeps the others off its core.
Finished threads are kept and reused (a pool in plat_posix.c): FFXiMain starts a thread for every sound
it plays, and a Horizon thread that ends unjoined keeps its 8 MB stack and its place in the thread limit - on
the console CreateThread failed 73 sounds in, and every effect after it was silent. The `[audio]` line
counts the pool (`threads N (M idle)`).

## Audio

One device, libnx's audout (48 kHz stereo s16, four 10 ms buffers), fed by one thread in sdl_nx.c. Each
10 ms block mixes two clients: dsound.c's callback stream (`SDL_OpenAudioDeviceStream`, the game's
DirectSound mix) and the streams bound to the device (`SDL_OpenAudioDevice` + `SDL_CreateAudioStream` +
`SDL_BindAudioStream`, up to 16: host/sewave.c's voices, the sign-in screen's effects, s16 at the sound's
own rate, resampled linearly). The thread waits on audout for every block, and runs one priority step above
the game's threads, so a buffer coming back takes its core from a busy one.

Two lines in host64.log, both written on the main thread (stdio from other threads isn't safe here):
- `[audio] blocks N underruns N mix max N us gap max N ms`, every 10 s while the device runs. Underruns are
  blocks the device had nothing for (the clock since the first block against the blocks put); the maxima are
  since the line before.
- `[dsound] ...`, every 10 s once the game has made DirectSound (user32's pump, `user32_pump_hook`): buffers
  made, duplicated, refused (and the last refused format), live and peak, Play calls, the most playing at
  once, failed Locks, notifications set and refused, interfaces asked for that dsound.c lacks (the IID's
  first dword), and the guest's `SetThreadPriority` requests by level (not applied). On a desktop build:
  `FFXI_AUDIO_STATS=1`.

## Guest memory

The guest has a 2 GB window (`FFXI_GUEST_2GB`, gwin.c; the same as iOS), because Horizon has no
overcommit: every committed page is physical memory, of about 3.2 GB an application has. Two modes, chosen
in `__libnx_initheap` (replaced in plat_posix.c) from whether the process has a system resource:

- **Physical**: the C heap is capped at `FFXI_SWITCH_HEAP_MB` (1024); the window is in the alias region
  and commit is `svcMapPhysicalMemory` (decommit unmaps; the next commit is zero). hbloader hands its heap
  over at all of memory: it is shrunk at start and grown back at exit. Needs an NPDM with a
  `system_resource_size` (hbloader's has one; `switch/app.json` asks for 16 MB).
- **Heap**: no system resource (Ryubing running a bare `.nro`, an applet): the heap takes all of memory and
  the window is its top 2 GB, always backed; decommit zeroes.

`svcMapPhysicalMemory` without a system resource fails with 0xFA01 (kernel 125, invalid state); that is
what Ryubing gives a bare `.nro`.

## Verified

- `build_switch.py plat-test` in Ryubing 1.3.3 (heap mode): 31 of 31. Files and directories on the SD
  card, threads, `plat_wait32`/`plat_wake_all32` across threads and with a timeout, atomics, time, the 2 GB
  window, zeroed commits, a partly committed range keeping its contents, decommit + commit zero, the top
  page of the window, 1.9 GB committed.
- The network (with `net-test.host` beside it on the card: `<server> 54231`): DNS and a TCP connection to
  a LandSandBoat server's sign-in port, from the emulator (34 of 34 with it).
- `build_switch.py host`: the whole game (the 2025-12-26 build's translation, runtime, host64, sign-in
  screen, mbedtls) compiles and links, a 28.8 MB `.nro`. In Ryubing it starts, reads `run.args`, finds the
  game on the card, and stops at the sign-in screen, which needs a graphics back end to draw:
  `[signin] no window to draw in`.

- The OpenGL back end (2026-11-15), on the desktop (RTX 4070, NVIDIA's GL 4.6): `gfxtest-gl` passes every
  check (also with `FFXI_GL_CLIP=0` and `FFXI_GL_RING_MAP=0`, the fallbacks), including a new one of D3D's
  winding that passes on Direct3D 12 too; `host64-gl` plays all 30 replay scenes (`tools/replay/default.txt`)
  at the same frame rate as Direct3D 12, the captures alike but for camera and animation timing.
- In Ryubing: the game brings the device up ("OpenGL 4.3 on NV120 (nouveau), S3TC, clip control, persistent
  ring") and draws the sign-in screen at 60 fps, docked, on every start since the miscompile (below) was found.

- **On a console** (Mariko, Atmosphère, launched over a game with R): the sign-in screen with the controller and
  the system keyboard, sign-in to a LandSandBoat server, the game in the world (West Sarutabaruta), drawn
  right, playable, battles with their menus. The two bugs it found are fixed (929143f). Sound: the
  sign-in's effects, the game's (5485963, the thread pool) and the music.
- In Ryubing 1.3.3 (Windows and macOS hosts, with guest Internet access on): sign-in, the lobby, and into
  the world (Port Jeuno), after the lobby fixes below. Against a server across the internet
  (2026-11-17): sign-in from `run.args`, the lobby, the character list and into the world.

## Found: the lobby in Ryubing

Three things stopped the lobby in the emulator, none of which a console shows:

- `gethostname` on libnx gives the console's address as text, and the lobby looks it up with
  `gethostbyname` before it connects. Ryubing's resolver fails on it. `ws2.c` now answers a dotted
  address, and the machine's own name (127.0.0.1), when the lookup fails, as Winsock does.
- Ryubing answers `ioctl(FIONREAD)` with `EOPNOTSUPP`; the lobby saw 0 bytes waiting for ever. On a
  failure, `ws2.c` counts with `recv(MSG_PEEK | MSG_DONTWAIT)`.
- The freeze: Ryubing serves the sockets (bsd) service on one thread, and its `Poll`, which libnx's
  `select()` and `poll()` both use, sleeps 50 ms in the service whenever nothing is ready, timeout 0 or
  not. With `ws2.c`'s watcher polling every 5 ms and `lsb_login.c`'s `data_thread` parked in a blocking
  `recv`, the lobby's own `socket()`/`fcntl()`/`getsockopt()` waited seconds each, with the guest lock
  held, and every guest thread stopped behind it. On the Switch, `host_select()` asks the service's
  `Select` (`bsdSelect`, answered at once) with a zero timeout and sleeps between tries outside it, and
  `data_thread` peeks with `MSG_DONTWAIT` and sleeps 20 ms. `sh_socket` and its kind still hold the guest
  lock across service calls (to look at).
- Against a remote server, two more. Ryubing ends a read under `SO_RCVTIMEO` after ~70 ms with
  `ETIMEDOUT` (asked for 15 s), so the sign-in's TLS handshake failed (`-0x6800`): on the Switch,
  `lsb_login.c` sets no timeout and `bio_recv` peeks with `MSG_DONTWAIT`, sleeping 20 ms, for up to
  15 s. And its `Select` marks a socket whose non-blocking connect is still under way as ready, so
  the lobby's first read got `ENOTCONN` and it took the link for dead: `ws2.c`'s `still_connecting()`
  (a peer and no `SO_ERROR`) keeps such a socket out of the watcher's events and `select`'s sets.
  A server on 127.0.0.1 hides both: it answers within 70 ms and connects at once.

Found with Ryubing's GDB stub and `FFXI_NET_TRACE` / `FFXI_RECOMP_TRACE` set from `run.args`
(`FFXI_NAME=value` lines). Ryubing also needs **Enable guest Internet access** (Settings > Network) or it
resolves nothing, even `127.0.0.1`.

## Saved passwords

host/keychain.c's Switch store: no keychain here, so each remembered password is a file on the card
(`data/keychain/<key>.bin`: a random counter, the password padded to 128 bytes and encrypted with AES-CTR,
an AES-CMAC of both), under keys the security engine derives from the console's device key through spl:mig
(`splCryptoGenerateAesKek` with the device-unique option, then `splCryptoLoadAesKey` into a locked keyslot).
The keys never leave the engine: the file is useless on a computer or another console. Where spl:mig won't
answer (Ryubing), `keychain_available` is 0 and the sign-in says the password isn't remembered.
Confirmed on a console (2026-11-16): saved at sign-in, filled in at the next start.

## No shader cache: switch-mesa has no disk cache

A program cache file (GL program binaries) was written and dropped (2026-11-16): switch-mesa is built
without Mesa's disk shader cache (`libEGL.a` has the `st_*_disk_cache` hooks but no `disk_cache_create`), and
without it Mesa offers no program binary formats (`GL_NUM_PROGRAM_BINARY_FORMATS` 0 in Ryubing). On the
desktop, NVIDIA's binaries loaded without error but drew some programs wrong (gfx_test's cull and depth
checks). Each program compiles when first drawn; deko3d's precompiled shaders are the way past it, or a
switch-mesa with its disk cache.

## Found: the black screen at start was a miscompile

The "start-up race" (a black screen, the main thread spinning in gfx_queue.c's `rec`, the log stopping after
"render thread on") was not a race. devkitA64's GCC 16.1 compiled `batch_get` without its two stores after
`calloc` (`cap`, and `p` from `malloc`): the first batch had `cap` 0 and `rec` doubled 0 for ever. Its points-to
analysis is at fault (`-fno-tree-pta` keeps the stores; `-fno-builtin-malloc` does not). Builds with any extra
code there (debug probes, a print) compiled differently and worked, which looked like timing. Found with
Ryubing's GDB stub (`enable_gdb_stub` in its Config.json; `aarch64-none-elf-gdb`, `add-symbol-file <elf> -o
<module base>` from `monitor get info`; if its default port 55555 is taken, as it can be on macOS, Ryubing
crashes at start with `Address already in use`: pick another) and the object's disassembly. `build_switch.py` now compiles our own
sources with `-fno-tree-pta`; the translated code keeps it.

## Performance (measured 2026-11-17, handheld, branch switch/perf)

`FFXI_PROFILE=1` splits each frame (gfx_gl.c's line, handed to the game's thread to log; gfx_queue.c's render
thread line; the API time). What it showed, in the world (about 1,200 draws a frame):

- **The game's thread** (the recompiled game, plus ~4 ms of d3d8 recording) is 26 ms a frame at 1020 MHz: 38 fps
  at best. At 1785 MHz (`FFXI_SWITCH_CPU`) it is 10-13 ms.
- **The render thread** is ~19 ms (GL ~15 of it), in parallel: not the limit at 1020 MHz, so deko3d would not
  have helped then.
- **The GPU** is the limit once the CPU is faster: a 2048x2048 background (Config's smallest) is 4.5 times the
  handheld's pixels, and the swap waited 11 ms a frame (27 at stock clocks). At 1280x720 it waited 0.9 ms:
  32 -> 41 fps at 1224/614 MHz. So the background follows the screen by default (host64.c, `FFXI_SWITCH_BG`).
- **Zone changes** were 3.3-3.7 s. The CPU boost while loading (switch_main.c: a watcher turns on
  `appletSetCpuBoostMode(FastLoad)` when no frame has come for 300 ms, off after 5 frames, as FastLoad also
  holds the GPU at 76 MHz) makes them 2.5-2.7 s.
- **The frame rate by default** (switch_main.c, when run.args has no `--fps-divisor`): uncapped when the build
  has frame delta sites (`frame_supported()`), else 30. At 60 without the sites the world ran visibly slow on a
  console, as the game counts its time in frames; 30 holds, so its time is right. With the sites (build
  2025-12-26's, mapped 2026-11-17) uncapped kept the world's clock at 59-61 sixtieths a second while the
  console drew 35-60 fps in handheld (2026-11-18), and played smoothly.
- **Files**: an open was 6.7 ms (stat for the case, stat again, open, fstat: each a trip to the SD card). The
  case match is skipped on the Switch (FAT finds any case), CreateFileA stats only for the dispositions that
  report it, and read-only `.DAT`s are kept open once closed (k32_io.c, 32 at most): 2.5 ms an open.

## The CRT filter

`FFXI_CRT=1` (scanlines) or `2` (with an aperture grille), `FFXI_CRT_CURVE=1` (the glass's curve): the
present (`gfx_gl.c`, `crt_present`) draws the back buffer through a shader of its own instead of the blit,
compiled once at the first present (a blit again if it doesn't compile). A row's beam is a Gaussian, wider
for a brighter row, mixed with the next row's; the grille is red, green and blue every 3 of the screen's
columns. The lines' depth follows the screen's lines per row: none at one (a picture as tall as the
screen), full at two, so they don't beat against the screen's rows; a picture of half the screen's height
or less is shown at a whole multiple of its height (512x448 at 1024x896 docked), touches following
(`nx_present_rect`). The settings app sets it. Checked in Ryubing on the sign-in screen (2026-11-18); its cost
on a console not yet measured (`FFXI_FRAMELOG`, the present's share).

## Back to the sign-in screen

When the game ends by itself (a lost connection, its logout), `switch_main.c` restarts the application
(`appletRestartProgram`) if it is the forwarder (the running application's NACP name is the `.nro`'s):
the forwarder's hbloader then loads the `.nro` at the sign-in screen again. Not by loading the `.nro` again
in the same process (`envSetNextLoad`): the game's threads (plat_posix.c's pool, the render thread) outlive
`GameStart` and only the process's end stops them. Not after a close from HOME (`nx_exit_requested`),
nor when the sign-in screen is left without a game (`xi_host_game_started`, host64.c), nor over a game held
with R (a restart there starts that game). `FFXI_SWITCH_RELOGIN=0` turns it off. Confirmed on a console
(2026-11-18): a lost connection came back to the sign-in screen.

## Not yet

- **Physical mode** has not run: Ryubing gives a bare `.nro` no system resource, and loads neither an
  ExeFS folder nor a bare PFS0 from its command line. A proper NSP (hacbrewpack, with the player's keys) is
  the way to test it there; on a console, hbloader's NPDM should give it.
- **Graphics**: `gfx-test.nro` in Ryubing still fails its read-back checks (reads
  after a draw come back mostly zero there: the emulator's GPU-to-CPU sync, or ours - to be found); the scene
  effects, the scene renderer, water and sun shadows on GL; no shader cache (above). deko3d after it.
- **Audio**: the sign-in's effects play on the console (2026-11-15). In game, hit effects were missing:
  the console's log showed no underruns and 94 buffers made but 73 played, the rest lost to CreateThread
  failing. With the thread pool, confirmed on the console: 880 made, 880 played in ~9 minutes, 24 threads.
- A forwarder NSP (a HOME menu icon) built on the PC: tried 2026-11-16 (nx-hbloader set to our `.nro`,
  packed by hacBrewPack with the emulator's keys) and dropped: it failed to start on the console (no error
  code seen), and every player would need their own keys and tools. The setup now puts Sphaira on the card
  instead, which makes the forwarder on the console with its own keys (the player guide's *Home screen
  icon*); confirmed on a console, 2026-11-18.
- Addons (`addons_none.c`), HD rumble, sleep and resume (the
  connections end).

## Testing in the emulator

`build_switch.py run [file.nro]` copies the `.nro` to the emulator's virtual SD card and starts it (a
portable Ryubing under `XI_EMU_DIR`, default `D:/Emulator/Ryubing`; on macOS `Ryujinx.app` in
`/Applications` or `~/Applications`; or `XI_SWITCH_EMU`). The app's files
are then in `<emulator>/portable/sdcard/switch/xi-on-anything/`, and the emulator's own log in
`portable/Logs`. Turn off Ryubing's "expand DRAM" option so the memory matches a real 4 GB console, and
turn on "Enable guest Internet access" (Settings > Network), without which no name resolves.

The display defaults are `switch/switch.reg`, loaded after the desktop `ffxi.reg` and before what the
game saves (`settings.reg`, the overlay): the desktop file's 3440x1440 window and 6880x2880 background
drew the lobby 21:9 squeezed into 16:9, and the game saves its whole registry on the first run, so a bad
default stays. Config > Modern's Display page reads and writes `data/settings.reg` (loaded last), as on
the desktop with the sign-in screen.

The back end's lines, and anything written with `svcOutputDebugString`, are in Ryubing's own log, as
`KernelSvc OutputDebugString`. `build_switch.py gfx-test` builds the graphics test; run it as any `.nro`;
it writes `gfx-test.log` beside it and ends green (every check passed) or red.
