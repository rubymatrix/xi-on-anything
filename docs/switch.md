# Nintendo Switch port: developer notes

The player's guide is [platforms/switch.md](platforms/switch.md). This page is how the port is put
together, what has been verified, and what is left.

## Layout

| Path | What it is | Modeled on |
| --- | --- | --- |
| `tools/build_switch.py` | `plat-test`, `host`, `setup`, `run`, `deploy`, `logs` | `build_android.py`, `xbox.py` |
| `tools/switch_toolchain.py` | finds devkitPro (and its msys2 on Windows) and the emulator | `android_toolchain.py` |
| `host/switch_main.c` | libnx entry: sockets, the screen's size, `run.args`, the log; calls `xi_host_main` | `host/ios_main.c` |
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
  ring") and draws the sign-in screen at 60 fps, docked - but not on every start (below).

- **On a console** (Mariko, Atmosphère, launched over a game with R): the sign-in screen with the controller and
  the system keyboard, sign-in to a LandSandBoat server, the game in the world (West Sarutabaruta), drawn
  right, playable, battles with their menus. The two bugs it found are fixed (929143f). Sound is partly
  missing (some effects, maybe music): phase 4.

## Found: the black screen at start was a miscompile

The "start-up race" (a black screen, the main thread spinning in gfx_queue.c's `rec`, the log stopping after
"render thread on") was not a race. devkitA64's GCC 16.1 compiled `batch_get` without its two stores after
`calloc` (`cap`, and `p` from `malloc`): the first batch had `cap` 0 and `rec` doubled 0 for ever. Its points-to
analysis is at fault (`-fno-tree-pta` keeps the stores; `-fno-builtin-malloc` does not). Builds with any extra
code there (debug probes, a print) compiled differently and worked, which looked like timing. Found with
Ryubing's GDB stub (`enable_gdb_stub` in its Config.json; `aarch64-none-elf-gdb`, `add-symbol-file <elf> -o
<module base>` from `monitor get info`) and the object's disassembly. `build_switch.py` now compiles our own
sources with `-fno-tree-pta`; the translated code keeps it.

## Not yet

- **Physical mode** has not run: Ryubing gives a bare `.nro` no system resource, and loads neither an
  ExeFS folder nor a bare PFS0 from its command line. A proper NSP (hacbrewpack, with the player's keys) is
  the way to test it there; on a console, hbloader's NPDM should give it.
- **Graphics**: `gfx-test.nro` in Ryubing still fails its read-back checks (reads
  after a draw come back mostly zero there: the emulator's GPU-to-CPU sync, or ours - to be found); the scene
  effects, the scene renderer, water and sun shadows on GL; a program cache file. deko3d after it.
- **Audio**: the sign-in's effects play on the console (2026-11-15). In game, hit effects were missing:
  the console's log showed no underruns and 94 buffers made but 73 played, the rest lost to CreateThread
  failing (the thread pool above fixes that); not yet confirmed on the console.
- The forwarder NSP (`build_switch.py nsp`), HD rumble, sleep and resume (the connections end).

## Testing in the emulator

`build_switch.py run [file.nro]` copies the `.nro` to the emulator's virtual SD card and starts it (a
portable Ryubing under `XI_EMU_DIR`, default `D:/Emulator/Ryubing`, or `XI_SWITCH_EMU`). The app's files
are then in `<emulator>/portable/sdcard/switch/xi-on-anything/`, and the emulator's own log in
`portable/Logs`. Turn off Ryubing's "expand DRAM" option so the memory matches a real 4 GB console.

The back end's lines, and anything written with `svcOutputDebugString`, are in Ryubing's own log, as
`KernelSvc OutputDebugString`. `build_switch.py gfx-test` builds the graphics test; run it as any `.nro`;
it writes `gfx-test.log` beside it and ends green (every check passed) or red.
