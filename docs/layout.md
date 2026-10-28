# Repository layout

```
recomp/            x86c.py (one function -> C), recomp.py (driver: closure or --all, coverage stats)
runtime/           guest.h (state, memory, x87 helpers), runtime.c (dispatch, traps, cpuid)
runtime/win32/     32-bit Windows: loader (retail DLL mapped by Windows, entries patched),
                   bridges (host<->guest on the x86 stack), guest lock, boundary trace, profiler
runtime/portable/  64-bit hosts: plat.h (+ plat_win.c, plat_posix.c), gwin (guest window,
                   pages, heap), gthread (threads, lock, guest_call), thunk (imports -> shims),
                   pe (image loader), k32*/kobj/vfs/reg/ole (Win32; vfs also does the DAT overlays),
                   gamecore* (our own gamecore), user32 + input + dinput + dsound (SDL3),
                   d3d8 (the D3D8 front end), ws2 (sockets), gfx.h (the graphics back end):
                   gfx_metal.m (Metal) + gfx_msl*.c (D3D8 state and shaders -> MSL), gfx_null.c (elsewhere),
                   geometry_* (SSE-order skinning kernels and their guarded adapters, opt-in),
                   Android: gfx_vulkan.cpp (its C++ Vulkan back end), gfx_worker* (render thread),
                   gfx_pass_plan.h, gfx_visibility_storage* (with the GLSL its SPIR-V comes from),
                   gfx_probe_* (keyed visibility), gfx_spirv.c
host/              ffximain.c: the 32-bit stand-in FFXiMain.dll; host64.c: the 64-bit game host;
                   lsb_login.c: the LandSandBoat sign-in; android_main.c (the Android entry) and
                   benchmark.c (its frame timestamps)
android/           the Android app: its manifest and activity (docs/android.md)
xbox/              the Xbox app, its packaging and the console tools (xbox/README.md)
tests/             difftest.c (original vs translation), boot.c (x86), boot64.c (x64),
                   gfx_test.c (the Metal back end), d3d8_test.c (the D3D8 front end on it)
tools/             prepare.py, buildinfo.py, unpack.py, build.py (MSVC), build_posix.py (clang),
                   install.py, trace_report.py; newbuild.py and discover.py (a new client version)
                   setup.py (the source install, run by install-source.sh and setup.command),
                   thirdparty.py (builds third_party/ with clang), vendor.py (refreshes third_party/),
                   staticserver.py (a fixed-data server); replayserver.py, replay.py and replayreport.py
                   (recorded scenes for performance tests: tools/replay/, the reference set in
                   tools/replay/scenes/); android_deps.py, build_android.py, build_android_apk.py,
                   android_translation.py and android_toolchain.py (the APK), visibility_storage_spv.py
third_party/       stb; SDL3 and mbedtls, trimmed to what the build uses, with manifest.json each
discovery/         the discovery pass: Ghidra (Jython) post-scripts, verdicts.py (the manual verdicts
                   per build), notes/ (what each build's run found)
meta/              builds.json, and the per-build metadata the recompiler reads
.claude/skills/    game-version-update (-analyze, -build): a new client version end to end
specs/             the specifications the runtime implements (gamecore slots, D3D8 and gamecore surfaces)
ffxi.reg           the game's registry keys, a starting point for --reg
generated/         (gitignored) unpacked images (images/<build>/), discovery projects, recompiler output
build/             (gitignored)
```
