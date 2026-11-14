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

The translation is the desktop one (`generated/all`, `generated/ffxi`): the generated C is portable, and
the Switch adds no wraps or hooks. Compile flags: `-D__SWITCH__ -DRT_GUEST_WINDOW -DFFXI_SWITCH
-DFFXI_GUEST_2GB`, libnx's `-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE`, GCC (devkitA64).
The addon host is not built (`host/addons_none.c`), as in the browser build; Discord is off.

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
- The host (runtime, host64, sign-in screen, mbedtls) compiles for the Switch.

## Not yet

- **Physical mode** has not run: Ryubing gives a bare `.nro` no system resource, and loads neither an
  ExeFS folder nor a bare PFS0 from its command line. A proper NSP (hacbrewpack, with the player's keys) is
  the way to test it there; on a console, hbloader's NPDM should give it.
- **Graphics**: `gfx_null.c` for now. Next: a Mesa (nouveau) OpenGL 4.3 back end, `gfx_gl.c`, built and
  checked on the desktop first against the replay scenes, then on EGL here. deko3d after it.
- The forwarder NSP (`build_switch.py nsp`), audio on a console, HD rumble, sleep and resume (the
  connections end), the sign-in screen with the controller.

## Testing in the emulator

`build_switch.py run [file.nro]` copies the `.nro` to the emulator's virtual SD card and starts it (a
portable Ryubing under `XI_EMU_DIR`, default `D:/Emulator/Ryubing`, or `XI_SWITCH_EMU`). The app's files
are then in `<emulator>/portable/sdcard/switch/xi-on-anything/`, and the emulator's own log in
`portable/Logs`. Turn off Ryubing's "expand DRAM" option so the memory matches a real 4 GB console.
