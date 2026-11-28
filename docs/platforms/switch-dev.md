# Nintendo Switch port: developer notes

The player's guide is [switch.md](switch.md). This page covers how the port is built,
what has been verified, and what is left.

## Layout

| Path | What it is | Modeled on |
| --- | --- | --- |
| `tools/build_switch.py` | `plat-test`, `host`, `setup`, `install` (host then setup, the launcher's entry, as `build_android.py install`; `FFXI_PROGRESS=1` prints `@phase`/`@progress` as `build_posix.py`; `runtime.json` stamped beside the `.nro` on the card), `run`, `deploy`, `logs` | `build_android.py`, `xbox.py` |
| `tools/setup_switch.py`, `setup_switch_gui.py`, `platforms/switch/setup-switch.bat`/`.sh` | the player's one command (the GUI, tkinter, runs it with the window's answers and installs what the PC lacks): checks the PC, then runs `prepare.py`, `frame_sites.py` (when the build has none), `build_switch.py host` (its `@progress` lines, `FFXI_PROGRESS=1`, drive the bar) and `build_switch.setup()`; answers kept in `build/switch/setup.json`, output in `build/switch/setup.log` | `tools/setup.py` (the Mac's) |
| `tools/switch_toolchain.py` | finds devkitPro (and its msys2 on Windows) and the emulator | `android_toolchain.py` |
| `platforms/switch/switch_mtp.ps1` | `setup --mtp`: copies to a console over USB MTP (DBI's MTP responder), on Windows | — |
| `platforms/switch/switch_main.c` | libnx entry: sockets, the screen's size, `run.args`, the log; calls `xi_host_main` | `platforms/ios/ios_main.c` |
| `platforms/switch/switch_settings.c` | the settings app (`build_switch.py settings`, `xi-on-anything-settings.nro`, its own folder on the card): libnx's text console over `data/settings.reg` (resolution, interface, background, as modern.c's `save_display`) and `run.args` (the other lines kept); + starts the game (`envSetNextLoad`) | |
| `platforms/switch/sdl_nx.c`, `nx_bridge.h` | the SDL3 calls the runtime makes, over libnx (HID, touch, swkbd, audout) | `sdl_web.c`, `sdl_uwp.c` |
| `runtime/portable/plat_posix.c` (`__SWITCH__`) | memory, address waits, thread ids, time | the Emscripten branch |
| `platforms/switch/tls_nx.c`, `platforms/switch/mbedtls_config.h` | mbedtls' clock and entropy | — |
| `platforms/switch/app.json` | the application NPDM (for the forwarder) | hbloader's |
| `platforms/switch/tests/switch_plat_test.c` | the platform layer's self-test, as its own `.nro` | — |
| `runtime/portable/gfx/gfx_gl.c`, `gfx_gl_api.h` | graphics: OpenGL 4.3 core on Mesa's nouveau through EGL; also builds on the desktop (SDL3's GL context) for testing | `gfx_webgpu.c`, `gfx_vulkan.c` |
| `runtime/portable/gfx/gfx_deko3d.c`, `dk_uam.c`, `dk_shaders/` | graphics on deko3d, the default (OpenGL with `XI_SWITCH_GFX=gl`, below) | `gfx_gl.c` |
| `third_party/uam` | deko3d's shader compiler as a library for the console; its changes are in `XI_CHANGES.md` | devkitPro's uam 1.1.0 |
| `platforms/switch/tests/switch_gfx_test.c` | `tests/gfx/gfx_test.c` on GL as its own `.nro` (`build_switch.py gfx-test`) | — |

The translation is the Switch's own (`generated/switch-all`, `generated/switch-ffxi`): the desktop one with the
faster forms measured on the console turned on (`XI_TRANSLATE_FAST=1`, `recomp/x86c.py`: native hot loops and matrix
products, x87 in fixed locals, registers kept across calls, ftol inline), which the other ports leave out. The
generated C is portable, and the Switch adds no wraps or hooks. Compile flags: `-D__SWITCH__ -DRT_GUEST_WINDOW -DFFXI_SWITCH
-DFFXI_GUEST_2GB`, libnx's `-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE`, GCC (devkitA64).
The addon host is not built (`host/addons_none.c`), as in the browser build. Discord is off.

## Graphics

`gfx_gl.c` is gfx.h on OpenGL 4.3 core. Its shaders are the generator's GL dialect (`gfx_gl_generate`,
gfx_msl.c): Vulkan's GLSL without descriptor sets, clip-space y negated, and z from glClipControl when the
driver has it. Row 0 of every texture and target is D3D's top row, so only the present is flipped. Vertex
streams are texture buffers read with `texelFetch`, because Mesa compiles storage-buffer reads to the
generic `LD`, which Ryubing's shader translator lacks. Every D3D8 format the game uses has a GL match
(BGRA, packed 16-bit types, swizzles, S3TC), so nothing is converted on the CPU.

A GL context is current on one thread, but the game makes buffers and reads occlusion probes from its own
threads. So the game uses the render queue (`GFX_QUEUE`, gfx_queue.c). With `GFX_GL` it brings the device
up on the render thread and makes every call there (`GFXQ_OWNED`, as WebGPU does in the browser).

Test on a desktop first: `tools/build.py gfxtest-gl` (tests/gfx/gfx_test.c, offscreen) and
`tools/build.py host64-gl` (the game; `FFXI_GL_DEBUG=1` for the debug context and checks). On the Switch
the surface is 1920x1080, cropped to the screen at each present. The back end logs through
`svcOutputDebugString` (in Ryubing's log).

Threads: `plat_thread_start` puts them on cores 1 and 2 at the main thread's priority; the main thread
(the game's) has core 0. Horizon doesn't share a core between threads of one priority, so a thread that
never waits keeps the others off its core. Finished threads are kept for reuse (a pool in plat_posix.c).
FFXiMain starts a thread for every sound, and a Horizon thread that ends unjoined keeps its 8 MB stack and
its place in the thread limit: without the pool, CreateThread failed after 73 sounds and later effects
were silent. The `[audio]` line counts the pool (`threads N (M idle)`).

## Graphics on deko3d

`python tools/build_switch.py host` builds the game on deko3d, the default since 2026-11-26, with no Mesa in it, as
`build/switch/xi-on-anything.nro`. `XI_SWITCH_GFX=gl` builds the OpenGL one instead, as `xi-on-anything-gl.nro`, for a
folder of its own on the card (the homebrew menu shows one `.nro` a folder); it uses the same `sdmc:/switch/xi-on-anything`
data. `XI_SWITCH_DK_DEBUG=1` links
deko3d's debug library, which checks every call.

- **Same shape as OpenGL's**: the render thread owns the device (`GFXQ_OWNED`), streams are buffer textures, and the
  shaders are the generator's GL text in a dialect of its own (`gfx_dk_generate`). deko3d's conventions are D3D's, so
  nothing is flipped or remapped. The scene effects, water, shadows and ray tracing are not drawn, as on OpenGL.
- **Programs** are compiled by uam on a thread (75-160 ms each on the console) and kept in
  `shader-cache/dk/<hash>.bin`; a draw whose program isn't ready is skipped. Change `CACHE_MAGIC` when uam's output
  changes. `FFXI_DK_SYNC=1` waits for programs; `FFXI_DK_CACHE=<folder>` moves the cache.
- **Two frames in flight**, each with its own commands, staging and ring. What a frame frees is reused once its fence
  has passed.
- **The log**: `[dk]` lines every 5 seconds (`FFXI_PROFILE=1`), and `[switch] heap:` every 30. The run before's log is
  `host64.prev.log`.

Bugs found in the libraries, worked around or fixed here:

| Where | What | Here |
| --- | --- | --- |
| uam (Mesa 19's scheduler) | a block's last instruction that sets a dependency barrier stalled one clock, not two: the next block could read its result too early. In Mhaura, slivers of lamp light for single frames | `nv50_ir_emit_gm107.cpp`: two clocks |
| deko3d, `dkCmdBufClear` | keeps only the newest of a list's 1 KB control chunks; the rest leak, ~3 KB a frame | taken back in `cbAlloc` (`ctl_reclaim`) |
| deko3d, the copy engine | the first level's tile height isn't shrunk as the sampler shrinks it: images 6-8 rows (of blocks) tall came out wrong in their right part | a custom tile size at creation |
| deko3d, `dkCmdBufCopyImage` | takes a compressed image's rectangle in blocks | converted in `gfx_copy` |

Measured in Mhaura at 1020/307 MHz, uncapped, in scenes of 400-700 character draw items (2026-11-25; one short
run each, the camera not the same, OpenGL's only 5 samples):

| | OpenGL | deko3d |
| --- | --- | --- |
| fps | 32 | 38 |
| GPU busy | 77% | 46% |
| idle over those runs, cores 0 / 1 / 2 | 8 / 27 / 42% | 6 / 85 / 54% |

The game's own thread (core 0) is the limit on both. Zcull and depth compression made no difference.

## Audio

One device, libnx's audout (48 kHz stereo s16, four 10 ms buffers), fed by one thread in sdl_nx.c. Each
10 ms block mixes two clients:
- dsound.c's callback stream (`SDL_OpenAudioDeviceStream`, the game's DirectSound mix)
- up to 16 streams bound to the device (`SDL_OpenAudioDevice` + `SDL_CreateAudioStream` +
  `SDL_BindAudioStream`): host/sewave.c's voices and the sign-in screen's effects, s16 at the sound's
  own rate, resampled linearly.

The thread waits on audout for every block and runs one priority step above the game's threads, so a
returned buffer takes its core from a busy thread.

Two lines in host64.log, both written on the main thread (stdio from other threads isn't safe here):
- `[audio] blocks N underruns N mix max N us gap max N ms`, every 10 s while the device runs. Underruns are
  blocks the device had nothing for (clock time since the first block against blocks queued). The maxima
  are since the previous line.
- `[dsound] ...`, at most every 10 s once the game has made DirectSound (user32's pump, `user32_pump_hook`),
  skipped when nothing changed and no buffer is live: buffers made, duplicated, refused (and the last refused
  format), live and peak, Play calls, the most playing at once, failed Locks, notifications set and refused,
  interfaces dsound.c lacks (the IID's first dword), and the guest's `SetThreadPriority` requests by level
  (not applied). On a desktop build: `FFXI_AUDIO_STATS=1`.

## Guest memory

The guest has a 2 GB window (`FFXI_GUEST_2GB`, gwin.c, as on iOS). Horizon has no overcommit: every
committed page is physical memory, and an application has about 3.2 GB. `__libnx_initheap` (replaced in
plat_posix.c) picks one of two modes, by whether the process has a system resource:

- **Physical**: the C heap is capped at `FFXI_SWITCH_HEAP_MB` (1024). The window is in the alias region and
  commit is `svcMapPhysicalMemory` (decommit unmaps; the next commit is zero). hbloader hands over its heap
  at all of memory: it is shrunk at start and grown back at exit. Needs an NPDM with a
  `system_resource_size` (hbloader's has one; `platforms/switch/app.json` asks for 16 MB).
- **Heap**: no system resource (Ryubing running a bare `.nro`, an applet). The heap takes all of memory and
  the window is its top 2 GB, always backed; decommit zeroes.

Without a system resource, `svcMapPhysicalMemory` fails with 0xFA01 (kernel 125, invalid state). That is
what Ryubing gives a bare `.nro`.

## Verified

- `build_switch.py plat-test` in Ryubing 1.3.3 (heap mode): 31 of 31. Files and directories on the SD
  card, threads, `plat_wait32`/`plat_wake_all32` across threads and with a timeout, atomics, time, the 2 GB
  window, zeroed commits, a partly committed range keeping its contents, decommit + commit zero, the top
  page of the window, 1.9 GB committed.
- The network (with `net-test.host` beside it on the card: `<server> 54231`): DNS and a TCP connection to
  a LandSandBoat server's sign-in port, from the emulator (34 of 34 with it).
- `build_switch.py host`: the whole game (the 2025-12-26 build's translation, runtime, host64, sign-in
  screen, mbedtls) compiles and links, a 28.8 MB `.nro`.
- The OpenGL back end (2026-11-15), on the desktop (RTX 4070, NVIDIA's GL 4.6): `gfxtest-gl` passes every
  check, also with the fallbacks `FFXI_GL_CLIP=0` and `FFXI_GL_RING_MAP=0`. That includes a new D3D
  winding check that passes on Direct3D 12 too. `host64-gl` plays the 30 replay scenes
  `tools/replay/default.txt` had then at Direct3D 12's frame rate; the captures match but for camera and
  animation timing.
- In Ryubing: the game brings the device up ("OpenGL 4.3 on NV120 (nouveau), S3TC, clip control, persistent
  ring") and draws the sign-in screen at 60 fps, docked, on every start since the miscompile (below) was fixed.
- **On a console** (Mariko, Atmosphère, launched over a game with R): the sign-in screen with the controller
  and the system keyboard, sign-in to a LandSandBoat server, the game in the world (West Sarutabaruta) drawn
  right and playable, battles with their menus. The two bugs it found are fixed (929143f). Sound: the
  sign-in's effects, the game's (5485963, the thread pool) and the music.
- In Ryubing 1.3.3 (Windows and macOS hosts, guest Internet access on): sign-in, the lobby, and into the
  world (Port Jeuno). Against a server across the internet (2026-11-17): sign-in from `run.args`, the lobby,
  the character list and into the world.

## Found: the lobby in Ryubing

These stopped the lobby in the emulator. A console has none of them.

- `gethostname` on libnx returns the console's address as text, and the lobby looks it up with
  `gethostbyname` before it connects. Ryubing's resolver fails on it. When the lookup fails, `ws2.c` now
  answers a dotted address, and the machine's own name (127.0.0.1), as Winsock does.
- Ryubing answers `ioctl(FIONREAD)` with `EOPNOTSUPP`, so the lobby always saw 0 bytes waiting. On a
  failure, `ws2.c` counts with `recv(MSG_PEEK | MSG_DONTWAIT)`.
- The freeze: Ryubing serves the sockets (bsd) service on one thread. Its `Poll`, which libnx's `select()`
  and `poll()` both use, sleeps 50 ms whenever nothing is ready, even with timeout 0. With `ws2.c`'s
  watcher polling every 5 ms and `lsb_login.c`'s `data_thread` in a blocking `recv`, each of the lobby's
  `socket()`/`fcntl()`/`getsockopt()` calls waited seconds with the guest lock held, stalling every guest
  thread. On the Switch, `host_select()` calls the service's `Select` (`bsdSelect`, answered at once) with
  a zero timeout and sleeps between tries outside it; `data_thread` peeks with `MSG_DONTWAIT` and sleeps
  20 ms. `sh_socket` and its kind still hold the guest lock across service calls (to look at).
- Against a remote server, two more. Ryubing ends a read under `SO_RCVTIMEO` after ~70 ms with
  `ETIMEDOUT` (asked for 15 s), so the sign-in's TLS handshake failed (`-0x6800`). On the Switch,
  `lsb_login.c` sets no timeout and `bio_recv` peeks with `MSG_DONTWAIT`, sleeping 20 ms, for up to 15 s.
  And Ryubing's `Select` marks a socket still in a non-blocking connect as ready, so the lobby's first read
  got `ENOTCONN` and it gave up the link. `ws2.c`'s `still_connecting()` (a peer and no `SO_ERROR`) keeps
  such a socket out of the watcher's events and `select`'s sets. A server on 127.0.0.1 hides both: it
  answers within 70 ms and connects at once.

Found with Ryubing's GDB stub and `FFXI_NET_TRACE` / `FFXI_RECOMP_TRACE` set from `run.args`
(`FFXI_NAME=value` lines).

## Saved passwords

host/signin/keychain.c's Switch store. There is no keychain, so each remembered password is a file on the card:
`data/keychain/<key>.bin`, holding a random counter, the password padded to 128 bytes and encrypted with
AES-CTR, and an AES-CMAC of both. The keys come from the security engine, derived from the console's device
key through spl:mig (`splCryptoGenerateAesKek` with the device-unique option, then `splCryptoLoadAesKey`
into a locked keyslot). They never leave the engine, so the file is useless on a computer or another
console. Where spl:mig won't answer (Ryubing), `keychain_available` is 0 and the sign-in says the password
isn't remembered. Confirmed on a console (2026-11-16).

## No shader cache: switch-mesa has no disk cache

A GL program-binary cache was tried and dropped (2026-11-16). switch-mesa is built without Mesa's disk
shader cache (`libEGL.a` has the `st_*_disk_cache` hooks but no `disk_cache_create`), so Mesa offers no
program binary formats (`GL_NUM_PROGRAM_BINARY_FORMATS` 0 in Ryubing). On the desktop, NVIDIA's binaries
loaded without error but drew some programs wrong (gfx_test's cull and depth checks). Each program
compiles when first drawn. The way past it is deko3d's precompiled shaders, or a switch-mesa with its disk
cache.

## Mesa 26: mesa-switch, with the disk cache (branch switch/mesa-cache)

[mesa-switch](https://github.com/danfromtico/mesa-switch) (Mesa 26.2.4; Cemu-nx, PPSSPP-nx and dolphin-nx
use it) has a Horizon GPU backend over libnx, Gallium NVC0 for GL, no libdrm_nouveau, and Mesa's disk cache
on the Switch (`MESA_SHADER_CACHE_DIR`, else `$HOME/.mesa`, else `sdmc:/.mesa`). Built from source, GL only
(no Rust), with its `build-opengl.sh` in MSYS2's own shell (`MSYSTEM=MSYS`; `pacman -S meson bison flex
python-mako python-packaging python-yaml switch-zlib switch-libexpat`) and its MSYS2 cross file with the
programs as MSYS paths (`/d/msys64/...`: MSYS's Meson reads `D:/...` as relative) and `C:/msys64` changed
to where MSYS2 is. `XI_SWITCH_MESA=<DESTDIR>/opt/devkitpro/portlibs/switch` builds against it
(`build_switch.py`: gfx_gl.c with its headers, its libraries ahead of portlibs'); the `.nro` is 46.5 MB
(34.5 with switch-mesa). `switch_main.c` puts the cache in `sdmc:/switch/xi-on-anything/shader-cache`.
Published as our build, `mesa-switch-26.2.4-0ff5207.zip` (12 MB, reproducible, its SHA-256 in `build_switch.py`) on
the fork's release `switch-mesa-26.2.4`: `build_switch.py mesa` downloads and checks it into `build/switch/mesa/`,
the setup runs it before the build (devkitPro's Mesa when it can't), and the build links it when it is there.
It needs devkitPro's `switch-zlib` and `switch-libexpat`.

- Ryubing: no display (the backend's first GPU mapping fails, `VA bind ... 0x195c`).
- On a console (2026-11-18, 512x448, a 512 background, uncapped): the sign-in and the world as before, 44-54
  fps. The first start wrote 436 KB of cache (`foz_cache.foz`, Fossilize's single file); a second start in
  the same places added 24 KB, its first seconds' longest frame 165 ms (2468 the first time), the worst hitch
  outside a zone load 223 ms (572), zone loads unchanged (~2.5 s: the game's files, not shaders).

## Found: the black screen at start was a miscompile

The black screen at start (the main thread spinning in gfx_queue.c's `rec`, the log stopping after
"render thread on") was not a race. devkitA64's GCC 16.1 compiled `batch_get` without its two stores after
`calloc` (`cap`, and `p` from `malloc`), so the first batch had `cap` 0 and `rec` doubled 0 forever. Its
points-to analysis is at fault: `-fno-tree-pta` keeps the stores; `-fno-builtin-malloc` does not. Any extra
code there (a print, a probe) changed the compile and hid it, which looked like timing. `build_switch.py`
now compiles our own sources with `-fno-tree-pta`; the translated code keeps it.

Found with the object's disassembly and Ryubing's GDB stub: `enable_gdb_stub` in its Config.json, then
`aarch64-none-elf-gdb` with `add-symbol-file <elf> -o <module base>` from `monitor get info`. If the default
port 55555 is taken (it can be on macOS), Ryubing crashes at start with `Address already in use`: pick another.

## Performance (measured 2026-11-17, handheld, branch switch/perf)

`FFXI_PROFILE=1` splits each frame (gfx_gl.c's line, handed to the game's thread to log; gfx_queue.c's render
thread line; the API time). What it showed, in the world (about 1,200 draws a frame):

- **The game's thread** (the recompiled game, plus ~4 ms of d3d8 recording) is 26 ms a frame at 1020 MHz: 38 fps
  at best. At 1785 MHz (`FFXI_SWITCH_CPU`) it is 10-13 ms.
- **A busy town** (2026-11-19, Windurst's crowds, 1,100-1,500 draws): game + d3d about 39 ms at 1224 MHz (sys-clk's
  profile; 22-26 fps), 27-29 ms at 1785 (a steady 30, the adaptive cap's). Of the game thread's working time about
  57% is the game's own code (spread thin; its hottest functions wait on its memory), 31% d3d8 and the queue's
  recording, 12% the runtime. Storing only the registers that cross calls (regs_across_calls.py) and the constants'
  versions (GfxDraw.u_ver) each gave under a millisecond.
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
(`nx_present_rect`). The settings app sets it. Checked in Ryubing on the sign-in screen, and on a console
(2026-11-18, 512x448 with a 512 background, uncapped): about 47 fps in the world with CRT and the curve, 43
without, two runs in different scenes - its cost is under the noise; the game's own thread sets the rate.

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

Build for it with devkitPro's Mesa, `XI_SWITCH_MESA=0`: Mesa 26's Horizon backend gets no display in Ryubing
(above), and the build takes Mesa 26 whenever `build_switch.py mesa` (or the setup) has downloaded it.

`build_switch.py run [file.nro]` copies the `.nro` to the emulator's virtual SD card and starts it (a
portable Ryubing under `XI_EMU_DIR`, default `D:/Emulator/Ryubing`; on macOS `Ryujinx.app` in
`/Applications` or `~/Applications`; or `XI_SWITCH_EMU`). The app's files
are then in `<emulator>/portable/sdcard/switch/xi-on-anything/`, and the emulator's own log in
`portable/Logs`. Turn off Ryubing's "expand DRAM" option so the memory matches a real 4 GB console, and
turn on "Enable guest Internet access" (Settings > Network), without which no name resolves.

The display defaults are `platforms/switch/switch.reg`, loaded after the desktop `ffxi.reg` and before what the
game saves (`settings.reg`, the overlay): the desktop file's 3440x1440 window and 6880x2880 background
drew the lobby 21:9 squeezed into 16:9, and the game saves its whole registry on the first run, so a bad
default stays. Config > Advanced's Display page reads and writes `data/settings.reg` (loaded last), as on
the desktop with the sign-in screen.

The back end's lines, and anything written with `svcOutputDebugString`, are in Ryubing's own log, as
`KernelSvc OutputDebugString`. `build_switch.py gfx-test` builds the graphics test; run it as any `.nro`;
it writes `gfx-test.log` beside it and ends green (every check passed) or red.
