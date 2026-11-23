# Repository layout

```
recomp/              x86c.py (one function -> C), recomp.py (driver: closure or --all, coverage stats)
recomp/meta/         builds.json, and the per-build metadata the recompiler reads
recomp/discovery/    the discovery pass: Ghidra (Jython) post-scripts, verdicts.py (the manual verdicts
                     per build), notes/ (what each build's run found)
runtime/             guest.h (state, memory, x87 helpers), runtime.c (dispatch, traps, cpuid)
runtime/specs/       the specifications the runtime implements (gamecore slots, D3D8 and gamecore surfaces)
runtime/win32/       32-bit Windows: loader (retail DLL mapped by Windows, entries patched),
                     bridges (host<->guest on the x86 stack), guest lock, boundary trace, profiler
runtime/portable/    64-bit hosts: plat.h (+ plat_win.c, plat_posix.c), gwin (guest window, pages,
                     heap), gthread (threads, lock, guest_call), vfs (paths and the DAT overlays),
                     gfx.h (the graphics back end's interface), the web and UWP bridges
  winapi/            the Win32 the game calls: k32*, kobj, reg, ole, pe (image loader), thunk
                     (imports -> shims), user32 + input + dinput + dsound (SDL3), ws2 (sockets),
                     d3d8 (the D3D8 front end)
  gamecore/          our own gamecore
  geometry/          SSE-order skinning kernels and their guarded adapters (opt-in)
  gfx/               the back ends: gfx_metal.m + gfx_msl*.c (Metal), gfx_vulkan.c and gfx_vulkan.cpp
                     (Vulkan; the C++ one is Android's, with gfx_worker* its render thread), gfx_d3d12.c
                     + gfx_hlsl*.c (Direct3D 12), gfx_gl.c (OpenGL), gfx_webgpu.c, gfx_null.c;
                     gfx_pass_plan.h, gfx_visibility_storage* (with the GLSL its SPIR-V comes from),
                     gfx_probe_* (keyed visibility), gfx_spirv.c
host/                host64.c: the 64-bit game host; frame, scene, modern (Config > Advanced), padmap,
                     cexi, discord, appdefaults, touch and display_fit (phones), benchmark
  ui/                datui (the game's own windows, drawn by us), uidraw, ui_art, vicons
  signin/            the sign-in screen, lsb_login.c (the LandSandBoat sign-in), keychain
  addons/            Ashita and Windower addons: the native side and lua/
platforms/           each port's own code, packaging and tests (guides: docs/platforms/)
  android/           the Android app (manifest, activity), android_main.c, android_overlay.c, tests/
  ios/               ios_main.c, the app icon
  macos/             the app icon
  linux/             kit.sh (what the launcher builds the Linux client from) and its Dockerfile
  windows/           ffximain.c (the 32-bit stand-in FFXiMain.dll), host64.rc and the icon
  switch/            switch_main.c, sdl_nx.c, tls_nx.c, the NPDM and defaults, tests/
  xbox/              the UWP app, sdl_uwp.c, its packaging and the console tools (platforms/xbox/README.md)
  web/               the page (index.html and its scripts), sdl_web.c, webserve.py, bucket.py
tests/               runtime/ (difftest.c: original vs translation; boot.c, boot64.c; vfs), gfx/,
                     geometry/, host/, addons/ (the test addons and their scripts), tools/
tools/               the build: build.py (MSVC), build_posix.py (clang), build_mingw.py, build_ios.py,
                     build_android.py + build_android_apk.py, build_switch.py, build_web.py; buildinfo.py,
                     thirdparty.py (builds third_party/), vendor.py (refreshes it); prepare.py and unpack.py;
                     setup.py and install.py (the installs, run by install-source.sh and setup.command)
  version/           newbuild.py and discover.py (a new client version), cexi_sites.py, frame_sites.py
  replay/            recorded scenes for performance tests: replay.py, replayserver.py, replayreport.py,
                     staticserver.py (a fixed-data server), the reference set in scenes/
  addons/            addons_fetch.py, the surveys and patches, the generators for host/addons
  debug/             trace_report.py, sample_report.py, xi_mcp.py
third_party/         stb; SDL3 and mbedtls, trimmed to what the build uses, with manifest.json each
assets/              what every port bundles: fonts, textures, UI art; ffxi.reg (the game's registry
                     keys, a starting point for --reg)
docs/                these guides; images/ (the README's screenshots), platforms/ (one guide per port)
.claude/skills/      game-version-update (-analyze, -build): a new client version end to end
generated/           (gitignored) unpacked images (images/<build>/), discovery projects, recompiler output
build/               (gitignored)
```

The launcher runs `tools/setup.py`, `tools/prepare.py`, `tools/build_posix.py`, `tools/build_mingw.py` and
`tools/build_android.py` by those paths and reads `runtime.json` from main, so those stay where they are.
