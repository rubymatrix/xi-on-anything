# Nintendo Switch port: developer notes

The player's guide is [platforms/switch.md](switch.md). This page covers how the port is built,
what has been verified, and what is left.

## Layout

| Path | What it is | Modeled on |
| --- | --- | --- |
| `tools/build_switch.py` | `plat-test`, `gfx-test`, `host`, `setup`, `run`, `deploy`, `logs` | `build_android.py`, `xbox.py` |
| `tools/switch_toolchain.py` | finds devkitPro (and its msys2 on Windows) and the emulator | `android_toolchain.py` |
| `platforms/switch/switch_mtp.ps1` | `setup --mtp`: copies to a console over USB MTP (DBI's MTP responder), on Windows | — |
| `platforms/switch/switch_main.c` | libnx entry: sockets, the screen's size, `run.args`, the log; calls `xi_host_main` | `platforms/ios/ios_main.c` |
| `platforms/switch/sdl_nx.c`, `nx_bridge.h` | the SDL3 calls the runtime makes, over libnx (HID, touch, swkbd, audout) | `sdl_web.c`, `sdl_uwp.c` |
| `runtime/portable/plat_posix.c` (`__SWITCH__`) | memory, address waits, thread ids, time | the Emscripten branch |
| `platforms/switch/tls_nx.c`, `platforms/switch/mbedtls_config.h` | mbedtls' clock and entropy | — |
| `platforms/switch/app.json` | the application NPDM (for the forwarder) | hbloader's |
| `platforms/switch/tests/switch_plat_test.c` | the platform layer's self-test, as its own `.nro` | — |
| `runtime/portable/gfx/gfx_gl.c`, `gfx_gl_api.h` | graphics: OpenGL 4.3 core on Mesa's nouveau through EGL; also builds on the desktop (SDL3's GL context) for testing | `gfx_webgpu.c`, `gfx_vulkan.c` |
| `platforms/switch/tests/switch_gfx_test.c` | `tests/gfx/gfx_test.c` on GL as its own `.nro` (`build_switch.py gfx-test`) | — |

The translation is the desktop one (`generated/all`, `generated/ffxi`). The generated C is portable, and
the Switch adds no wraps or hooks. Compile flags: `-D__SWITCH__ -DRT_GUEST_WINDOW -DFFXI_SWITCH
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

## Not yet

- **Physical mode** has not run. Ryubing gives a bare `.nro` no system resource, and loads neither an ExeFS
  folder nor a bare PFS0 from its command line. A proper NSP (hacbrewpack, with the player's keys) would
  test it there; on a console, hbloader's NPDM should give it.
- **Graphics**: `gfx-test.nro` in Ryubing still fails its read-back checks: reads after a draw come back
  mostly zero (the emulator's GPU-to-CPU sync, or ours). Also missing on GL: the scene effects, the scene
  renderer, water and sun shadows, and a shader cache (above). deko3d after that.
- A forwarder NSP (a HOME menu icon): tried 2026-11-16 (nx-hbloader set to our `.nro`, packed by
  hacBrewPack) and dropped for now; it failed to start on the console.
- Addons (`addons_none.c`), HD rumble, sleep and resume (the connections end).

## Testing in the emulator

`build_switch.py run [file.nro]` copies the `.nro` to the emulator's virtual SD card and starts it. It uses
`XI_SWITCH_EMU` if set; on macOS, `Ryujinx.app` in `/Applications` or `~/Applications`; otherwise the
newest portable Ryubing at `<XI_EMU_DIR>/<version>/Ryujinx.exe` (`XI_EMU_DIR` defaults to
`D:/Emulator/Ryubing`). The app's files are in `<emulator>/portable/sdcard/switch/xi-on-anything/`, and the
emulator's log in `portable/Logs`. Turn off Ryubing's "expand DRAM" so memory matches a real 4 GB console,
and turn on "Enable guest Internet access" (Settings > Network), or no name resolves.

The display defaults are `platforms/switch/switch.reg`, loaded after the desktop `ffxi.reg` and before what the game
saves (`settings.reg`, the overlay). The desktop file's 3440x1440 window and 6880x2880 background drew the
lobby 21:9 squeezed into 16:9, and the game saves its whole registry on the first run, so a bad default
sticks. Config > Advanced's Display page reads and writes `data/settings.reg` (loaded last), as on the
desktop with the sign-in screen.

The back end's lines, and anything else written with `svcOutputDebugString`, are in Ryubing's log as
`KernelSvc OutputDebugString`. `build_switch.py gfx-test` builds the graphics test. Run it as any `.nro`: it
writes `gfx-test.log` beside it and ends green (every check passed) or red.
