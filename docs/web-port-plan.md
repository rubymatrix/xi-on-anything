# Web port plan: FFXIRecompile in the browser (WebAssembly + WebGPU)

Status: plan only, nothing built. Written 2026-10-04 from a read of the tree at `dde8f71`.

## Goal

Play the recompiled game in a desktop browser tab, at the same quality as the native Mac build:
60 fps, the Modern effects, keyboard + mouse that feel native, and full controller play. A small
**local server** on the player's own machine serves the build and their game files and bridges
the network. Everything else runs in the browser.

## Why it has to be a *local* server

The recompiled wasm is derived from the player's `FFXiMain.dll`, and the DATs are Square Enix
data. Same rule as every other target: **no Square Enix bytes leave the player's machine, and none
go in this repo.** The local server builds the wasm from the player's own install, and serves it
and the DATs only to that player's browser. We never host a playable build on the public web.

## What we have that helps (facts from the tree)

| Fact | Where | Why it matters for wasm |
|---|---|---|
| Guest memory is `base + (uint32_t)addr` through `GUEST_PTR` | `runtime/guest.h:16-26` | The 32-bit Windows target already runs with `GUEST_BASE = 0`. wasm32 can do the same: guest address == linear-memory address. |
| Host pointers never go into guest memory; imports are synthetic `0xFFF00000+` | `runtime/portable/thunk.h` | No 64-bit pointer leaks to fix. |
| No JIT, no interpreter, no self-modifying code; addon patches are AOT variants | `recomp/recomp.py:20-27` | wasm forbids runtime codegen; we don't need it. |
| No inline asm, intrinsics, `long double`, signals; x87 emulated on `double` | `guest.h:56-305` | Portable C compiles to wasm as-is. CPUID hides SSE, so the 203 SIMD functions stay unreached. |
| Guest threads are pthreads under one FIFO ticket lock, futex waits | `gthread.c`, `plat_posix.c:121-153` | Maps onto Emscripten pthreads + `Atomics.wait` / `memory.atomic.wait32`. |
| `setjmp/longjmp` only for ExitThread | `kobj.c:327,412` | Emscripten wasm-EH longjmp handles it. |
| gfx.h is a narrow, backend-neutral interface; shader generator already has two dialects (MSL, GLSL) | `gfx.h`, `gfx_msl.c:820` | A WebGPU backend is a fourth `gfx_*.c` plus a WGSL dialect. Vulkan backend (dynamic rendering, vertex pulling, fans → lists, no compute) is the closest template. |
| Render-thread queue (`gfx_queue.c`, record on game thread, replay on render thread) | commits `3237f12`/`abc457f` (only on `refs/prepol/perf/game-thread`) | WebGPU *must* run on a thread that returns to its event loop. This design is exactly what we need. |
| Sign-in and lobby screens draw through gfx.h, not D3D8 | `host/uidraw.c` | They come for free with the backend. |
| `tools/staticserver.py` / `replayserver.py` fake LSB servers | `tools/` | Offline test servers for the web build, no live server needed. |

## What fights us

| Problem | Where | Plan |
|---|---|---|
| 4 GB + page guest reservation | `gwin.c:37` | wasm32 tops out at 4 GB *total*. See **Memory model** below. |
| Host stack depth == guest call depth (C calls per guest call; 8 MB host stacks today) | `x86c.py:247-250`, `plat_posix.c:90` | Browser wasm stacks are ~1 MB and not configurable from the page. Must measure (Gate 0). |
| 161 MB of generated C, 22k functions | `generated/all` | Large but fine for AOT; check per-function engine limits (V8: 7.6 MB body, 50k locals). |
| Raw TCP + UDP sockets (lobby 54001/54230, zone UDP 54230, TLS auth 54231) | `ws2.c`, `host/lsb_login.c` | Browsers can't open sockets. WebSocket bridge in the local server. |
| 14 GB / 65k DAT files, read with synchronous `ReadFile` | `k32_io.c:225` | Synchronous reads are fine from a worker. Range-fetch + OPFS cache. |
| Case-insensitive paths assumed past the mount prefix | `vfs.c`, `k32_io.c:548` | Server manifest gives a case-folded index (fixes it for Linux too). |
| SDL3 owns window, events, audio, gamepad; video + events tied to the main thread | `user32.c:213`, `dsound.c` | Replace with a thin web platform layer (below). The game must not run on the browser main thread. |
| Blocking GPU readbacks (`gfx_tex_read`: CopyRects, screenshots) | `d3d8.c:1470,1130` | WebGPU readback is async (`mapAsync`). OK once the game thread is separate from the render thread: it blocks on an atomic while the render worker maps. |
| `SetCursorPos` warps the OS cursor | `user32.c:1416` | Browsers can't warp. Pointer Lock + software cursor when the game warps. |
| LuaJIT addons | `host/addons` | LuaJIT can't JIT in wasm and its interpreter is hand-written asm. The browser gets its own addons instead: HTML and JS over the canvas, with a JS API reading game state from wasm memory (the page's own JS engine runs them; no Lua, no ImGui). `host/addons_none.c` stands in for the addon host. |
| Keychain, Discord IPC | `host/keychain.c`, `host/discord.c` | Move to the local server (it has the OS keychain and the Discord socket). |

## Architecture

```
 player's machine
┌──────────────────────────────────────────┐        ┌──────────────────────────┐
│ xi-web (local server, 127.0.0.1 only)    │        │ LandSandBoat server      │
│  build:  prepare → recomp → emcc → wasm  │        │  54231 TLS auth          │
│  GET  /            shell + wasm (COOP/   │  TCP   │  54230 data / 54001 lobby│
│                    COEP, brotli, hashed) │ ─────▶ │  54230/udp zone          │
│  GET  /dat/...     DATs, Range, overlays │  UDP   └──────────────────────────┘
│  GET  /dat/index   case-folded manifest  │
│  GET/PUT /user/... settings.reg, USER/   │
│  WS   /net         socket bridge (allow- │
│                    listed game host only)│
│  POST /log, /presence (Discord), /secret │
└───────────────▲──────────────────────────┘
                │ http://127.0.0.1:<port>/#t=<token>
┌───────────────┴───────────────────────────────────────────────────────────┐
│ browser tab (crossOriginIsolated → SharedArrayBuffer)                     │
│                                                                           │
│  main thread (JS shell, never blocks)                                     │
│   - DOM: canvas, start/settings/crash overlays, OSK                       │
│   - keyboard / mouse / wheel / IME / paste → input ring (SAB)             │
│   - Gamepad API poll every rAF → pad snapshot (SAB)  (main-thread only)   │
│   - Fullscreen, Keyboard Lock, Pointer Lock, Wake Lock, visibility        │
│                                                                           │
│  game worker (pthread, PROXY_TO_PTHREAD)   + guest pthreads (pool ~24)    │
│   - GameStart(); recompiled code; Win32 shims; blocks freely              │
│   - ReadFile → sync XHR Range (v1) / IO worker + OPFS cache (v2)          │
│   - ws2 → /net WebSocket via IO worker, Atomics.wait for blocking calls   │
│   - gfx_queue: records gfx.h calls into a SAB ring                        │
│                                                                           │
│  render worker (OffscreenCanvas, WebGPU, event-loop driven by rAF)        │
│   - gfx_webgpu.c replays the queue; presents on return to event loop      │
│   - readbacks: mapAsync then Atomics.notify the waiting guest thread      │
│                                                                           │
│  IO worker (JS): WebSocket mux, fetch, OPFS                               │
│  AudioWorklet: pulls 48 kHz F32 from SAB ring filled by dsound mixer      │
└───────────────────────────────────────────────────────────────────────────┘
```

Rule that drives all of it: **the browser main thread and the WebGPU thread must keep returning to
their event loops; the game thread never does.** So the game runs in a worker where it can block
(Sleep, waits, sync file reads, blocking sockets), and everything that needs the event loop
(WebGPU present, WebSocket, fetch, Gamepad API, DOM) lives elsewhere, talking through
SharedArrayBuffer rings and `Atomics.wait/notify`. No Asyncify (it would bloat 22k functions), and
no JSPI needed.

## Memory model

**Decided (2026-10-04): wasm32, identity mapped.** Build with `GUEST_BASE = 0`, the same as the existing
32-bit Windows target, so a guest address *is* a linear-memory address: no add per access, no
64-bit bounds checks.
- Host code (Emscripten static data, shadow stack, `malloc`) must live where the guest never
  allocates. The guest image sits at `0x0F000000` (FFXi.dll) / `0x10000000` (FFXiMain); gwin
  already keeps the low 1 MB and `0xFFF00000+` out of its heap. Give the host a fixed region
  (e.g. `0x00100000-0x0EFFFFFF`, ~238 MB; set `GLOBAL_BASE` and route `sbrk`/`emmalloc` there) and
  tell gwin it's reserved.
- gwin hands out addresses bottom-up and calls `memory.grow` as the high-water mark rises, so the
  tab only pays for what the game uses. `-sMAXIMUM_MEMORY=4GB` (Chrome, Firefox and Safari all
  allow 4 GB wasm32 memories on 64-bit).
- `0xFFF00000+` thunk addresses are call targets only; check nothing dereferences them, or memory
  would have to grow to the very top.
- Decommit becomes "zero the range" (wasm can't give pages back).

Rejected: wasm64 (memory64) with `base + offset`. Least code change, but a 64-bit index can't be
covered by guard pages, so the engine adds a bounds check (compare + branch) to every load and
store. Every guest access goes through `GUEST_PTR`, so nearly every guest instruction would pay it,
plus the base add. It also drops Safari, which doesn't ship memory64.

## The local server (`xi-web`)

Prototype as `tools/webserve.py` (stdlib only, like `staticserver.py`, with a small hand-rolled
WebSocket). Later move it into the launcher (Rust, already shipped, already owns the keychain and
settings) so players get it with one click: "Play in browser".

Jobs:
1. **Build.** `webserve.py build --game <install>`: `prepare` → `recomp` → `emcc` (pinned emsdk,
   fetched like zig is for the Deck kit) → `build/web/`. Output named by content hash, cached by the
   DLL's patch version, so a game update rebuilds once.
2. **Serve the app.** `Cross-Origin-Opener-Policy: same-origin` and
   `Cross-Origin-Embedder-Policy: require-corp` (needed for SharedArrayBuffer), precompressed brotli
   wasm, `Cache-Control: immutable` on hashed files so V8 keeps its compiled-code cache.
3. **Serve DATs.** `GET /dat/<path>` with HTTP Range; `--dats` overlays applied server-side (same
   longest-wins rules as `vfs.c`); never anything outside the install and overlay roots.
   `GET /dat/index` returns a manifest (case-folded path → real path, size, mtime) that backs
   `FindFirstFile` and fixes case sensitivity.
4. **User data.** `GET/PUT /user/...` for `saved.reg`, `settings.reg`, `signin.cfg` and the game's
   `USER/` folder, stored in the *same* data dir the native app uses, so macros, keybinds and
   settings carry over between native and browser.
5. **Network bridge.** `WS /net`, one socket multiplexed: `open(tcp|udp)`, `connect`, `send`,
   `sendto`, `recv` frames, `close`, error codes. UDP rides inside the WebSocket only for the
   loopback hop, then goes out as real UDP, so head-of-line blocking costs ~nothing.
6. **Extras.** `POST /log` (host64.log to disk for bug reports), `/presence` (Discord IPC, reusing
   `discord.c` logic), `/secret` (OS keychain for the password and trust token).

Security (this is a local process that opens sockets on request, so it must not become an open
proxy for every web page the player visits):
- Bind `127.0.0.1` only. Random per-run token in the URL fragment, required on every request and
  the WebSocket; check `Origin`.
- `/net` connects only to the server the player configured (and its LSB ports). Nothing else.
- LAN play (serve to an iPad or another PC) is opt-in and needs HTTPS (SharedArrayBuffer needs a
  secure context; `localhost` counts, a LAN IP doesn't): mkcert or a Tailscale cert.

## Platform layer for the web (`runtime/web/`)

Keep the Win32 shims; replace what sits under them.

| Native piece | Web replacement |
|---|---|
| `plat_posix.c` reserve/commit | `plat_web.c`: fixed host region, `memory.grow`, zero-on-decommit |
| pthreads, futex, `os_sync_wait` | Emscripten pthreads, `PTHREAD_POOL_SIZE≈24` (workers can't be spawned while the main thread is busy; timers make one thread each via `timeSetEvent`), `emscripten_futex_wait` |
| `_Thread_local` | works as-is under Emscripten pthreads |
| `nanosleep`, monotonic clock | `emscripten_futex_wait` timeout, `emscripten_get_now()` (5 µs resolution when cross-origin isolated) |
| `vfs.c` → `open/read` | `plat_file_*` over the `/dat` and `/user` endpoints |
| `ws2.c` → BSD sockets | same exports over the `/net` bridge; the `select` watcher thread becomes "IO worker signals kobj events" |
| `user32.c` `pump()` → `SDL_PollEvent` | `pump()` drains the input ring from the main thread into the same paths `input_sdl_event` feeds today |
| `dsound.c` → SDL audio | same mixer, output into a SAB ring read by an AudioWorklet |
| `gfx_metal.m` / `gfx_vulkan.c` | `gfx_webgpu.c` behind `gfx_queue.c` |
| mbedtls (auth) | compiles to wasm unchanged; TLS goes through the `/net` pipe |
| keychain, Discord | local-server endpoints |
| SDL3 | not linked in the web build |

Why not Emscripten's SDL3 port: we need exact control of Pointer Lock, Keyboard Lock, the Gamepad
API and IME, all from the main thread while the game sits in a worker. A thin shell is less code
than fighting SDL's threading assumptions.

## Graphics: `gfx_webgpu.c` + WGSL

Start from `gfx_vulkan.c` (4.7k lines); it maps closely.

| Vulkan backend today | WebGPU |
|---|---|
| GLSL from `gfx_msl.c` + glslang | third dialect in the same generator: **WGSL** text, no glslang, no Tint/naga in the page |
| Vertex pulling from storage buffers + `gl_VertexIndex` | read-only storage buffers in the vertex stage + `vertex_index` (core WebGPU; not "compat" mode) |
| Push descriptors per draw | one big uniform ring with dynamic offsets for `GfxU` (3.5 KB/draw); bind groups for textures cached by (texture set, sampler set) |
| Dynamic depth bias, stencil masks, depth compare, topology | baked into pipeline keys (WebGPU has no dynamic versions); stencil ref, viewport, scissor stay dynamic |
| `CLAMP_TO_BORDER`, `MIRROR_CLAMP_TO_EDGE` | not in WebGPU: emulate in the generated shader (clamp + border-color compare, mirror math) |
| Combined `sampler2D` / `sampler2DShadow` | separate texture + sampler / `sampler_comparison` |
| `gl_PointSize` | not in WebGPU (points are 1 px): expand to quads in the vertex-pulling shader |
| Triangle fans → lists | keep the existing rewrite |
| `POLYGON_MODE_LINE` wireframe | drop (debug only) |
| BC1-3 (DXT) | `texture-compression-bc` feature on desktop; CPU decode to RGBA8 where missing |
| D32_S8 / D24S8 | `depth24plus-stencil8` (core); `depth32float-stencil8` when the feature exists |
| 16-bit colour widened on CPU, X8/A8/L8 via view swizzles | widen on CPU too; WebGPU has no view swizzle in core, so swizzle in the shader per format key |
| `vkCmdBlitImage` mips | small down-sample render pass |
| RGBA16F / R32F effect targets, 12 FX passes | all core formats; port `FX_GLSL` to WGSL (now three copies: MSL, GLSL, WGSL; worth generating them from one source later) |
| glslang on worker threads + pipeline key file | `createRenderPipelineAsync`, keep the key file (in OPFS) to prewarm at start; skip draws until ready, as now |
| GPU timestamps | `timestamp-query` feature where present |

Threading: the render worker owns the `OffscreenCanvas` and the `GPUDevice` and replays
`gfx_queue` batches each `requestAnimationFrame`. `getCurrentTexture` presents when the worker's
task ends, which is why it must not block. Synchronous results (`gfx_tex_read`, probe readback if
`FFXI_PROBE=gpu`) post a request and `Atomics.wait` on the game thread; the render worker
`mapAsync`s and notifies. The default "fully visible" occlusion probe needs no readback at all.

Cost to watch: every WebGPU call crosses wasm → JS. A busy town is a few thousand draws; keep it to
~4-6 calls per draw (setPipeline only on change, one setBindGroup with a dynamic offset, setVertex
/ index only on change, draw). Batch uploads into one `writeBuffer` per frame from the upload ring.

## Input

### Keyboard
- `KeyboardEvent.code` → SDL-style scancode → the existing `vk_of` / `dik_of` tables, so
  DirectInput, `GetAsyncKeyState` and WM_KEY* all keep working. `preventDefault` on everything
  while the game has focus.
- Keys browsers fight over, and the fix:

| Keys | Problem | Fix |
|---|---|---|
| Esc | exits fullscreen and pointer lock | Keyboard Lock (`navigator.keyboard.lock(['Escape'])`, Chromium, fullscreen only) makes Esc a normal key and needs a long-press to exit. Elsewhere: re-enter fullscreen on the next click and offer an alternate cancel key. |
| Ctrl+1-9, Ctrl+W/T/N, Ctrl+Tab | browser tab shortcuts, not cancellable; Ctrl+1-0 are FFXI macro bars | Keyboard Lock captures them in fullscreen. Installed PWA window drops the tab shortcuts. `beforeunload` guards Ctrl+W. Last resort: shell offers Alt-based macro alias. |
| F1-F12 | F5 reload, F11 fullscreen, F12 devtools; FFXI targets with F1-F6 | `preventDefault` (works for F1-F10); F11/F12 left to the browser. |
| Alt, Alt+letter | Firefox/Windows menu bar | `preventDefault` on keydown and keyup |
| Cmd (mac) | Cmd+Q / Cmd+W quit/close | `beforeunload` while zoned in |

- Focus loss (`blur`, `visibilitychange`) releases every key, as `user32.c` does on SDL focus loss.

### Text, IME, clipboard
- Chat text from `KeyboardEvent.key` → cp1252 → WM_CHAR (same as SDL_EVENT_TEXT_INPUT today).
- IME (Japanese): a hidden `<input>` takes focus while the game is in text entry and forwards
  `compositionend`. v2; IMM32 stays "never open" in v1.
- Paste: the `paste` event (Ctrl/Cmd+V) gives text without a permission prompt; inject as WM_CHAR
  and back the missing `GetClipboardData`. Copy: `navigator.clipboard.writeText` from
  `SetClipboardData`.

### Mouse
- Canvas CSS px × `devicePixelRatio` → back-buffer px, then the existing UI-aspect un-squeeze
  (`user32_ui_hit`). Wheel: normalise `deltaMode` to 120 per notch. `contextmenu` and middle-click
  autoscroll suppressed.
- DirectInput relative motion from `movementX/Y`.
- The game warps the cursor (`SetCursorPos`): the browser can't. When the game warps, hides the
  cursor, or a camera drag starts, take Pointer Lock (`{unadjustedMovement: true}` for raw motion,
  requested in the mousedown that starts the drag) and draw the cursor in the overlay; release on
  mouse-up. Otherwise use the hardware cursor (`ShowCursor` → CSS `cursor`).
- Chrome refuses re-lock for ~1 s after the user leaves lock with Esc; Keyboard Lock removes that
  case in fullscreen.

### Gamepad
- The Gamepad API exists only on the main thread (not in workers). The shell polls
  `navigator.getGamepads()` every rAF and writes a snapshot (buttons, analog triggers, sticks,
  timestamp, connected) into a SAB slot per pad. `input_pad_state` / `input_xpad` read it instead of
  SDL. The fake WMI Xbox 360 entry (`ole.c`) stays, so the game uses XInput as it does on Mac.
- `mapping === "standard"` (Xbox, DualShock/DualSense, Switch Pro on Chromium) maps straight to
  XInput bits. Non-standard pads: a built-in mapping table keyed on the id string (ported from
  SDL's `gamecontrollerdb`, zlib-licensed) plus a remap screen in the shell.
- Browsers hide pads until a button is pressed: the start screen says "press any button" and shows
  the pad once seen. Hot-plug via `gamepadconnected/disconnected`.
- Rumble: `vibrationActuator.playEffect('dual-rumble')` (Chromium, Firefox); no-op on Safari.
- Controller-only play: chat needs text. An HTML on-screen keyboard over the canvas, opened by a
  configurable chord, pad-navigable, injecting WM_CHAR (same idea as the launcher's `pad.js` +
  Steam OSK). All shell screens (start, settings, remap, errors) are pad-navigable.
- Pads stop reporting in background tabs (browser rule); release all buttons on hide.

## Audio

Keep the `dsound.c` mixer (48 kHz stereo F32, 10 ms chunks). It writes into a SAB ring; an
AudioWorklet pulls 128-frame quanta and signals the mixer's notify events back via the ring
cursors. The `AudioContext` is created and resumed in the "Play" click (autoplay policy). Target
~30-40 ms total latency. `sewave.c` (sign-in sounds) goes through the same mixer.

## Files and caching

- v1: guest `ReadFile` → synchronous `XMLHttpRequest` with a Range header, straight from the game
  worker (sync XHR is allowed in workers). On loopback, ~0.2-1 ms per uncached read. Read-ahead
  in 256 KB blocks into a host-side block cache.
- v2: IO worker + **OPFS** cache (`FileSystemSyncAccessHandle`, fast synchronous reads in
  workers): blocks persist across sessions, zone loads stop touching the server. Optional "install
  to browser" copies ROM/sound into OPFS for play without the server's file endpoint (needs
  `navigator.storage.persist()` and ~14 GB quota).
- `VTABLE.DAT` / `FTABLE.DAT` (330 KB) and the manifest are fetched up front.
- Writes go to `/user` through a write-behind queue; `saved.reg` already writes whole-file
  (tmp + rename), so that's one PUT.
- wasm: streaming compile (`WebAssembly.instantiateStreaming`); after first run V8 serves its
  compiled-code cache. Pipeline key file prewarms WebGPU pipelines behind the loading bar.

## Player experience

- **Start page**: preflight before downloading anything big. Checks `crossOriginIsolated`,
  WebGPU adapter + `texture-compression-bc`, memory, browser version; plain-language message for
  each failure ("Firefox on macOS doesn't have WebGPU yet: use Chrome, Edge or Safari 26").
- **First load**: progress for download, compile and pipeline prewarm, with sizes. Second load
  should be seconds.
- **Play button** = the user gesture that unlocks everything at once: fullscreen, Keyboard Lock,
  AudioContext, Wake Lock.
- **Sign-in**: the existing lobby-style `signin.c` screen (drawn through gfx, so it comes with the
  backend). Password through the server's keychain endpoint.
- **Install as an app (PWA)**: manifest + icons, `display: standalone`, so it gets its own window,
  dock/taskbar icon, and no tab shortcuts. Recommended in the start page for Chromium.
- **Window modes** map to the browser: Fullscreen ↔ Fullscreen API; windowed/borderless ↔ canvas
  fills the tab/window. Resolution follows the canvas at device pixels
  (`ResizeObserver` `devicePixelContentBoxSize`) with a render-scale option; resize → WM_SIZE as
  `user32_set_window` does. UI aspect (16:9 on ultrawide) works unchanged.
- **Background tab**: rAF stops, so the render worker drops frames instead of stalling the game
  thread; the game, network keepalives and audio keep going; pads and keys release.
- **Leaving**: `beforeunload` prompt while zoned in ("log out first").
- **Errors**: `RT_UNIMPL`, `rt_fatal`, device loss → an overlay with the message, "copy report",
  and the log already POSTed to the server. WebGPU device loss → recreate device, re-upload
  (keep CPU copies the backend already keeps for sun caches).
- **Settings shell**: controller remap, OSK chord, render scale, cursor mode, server, data folder.

## Browser support (first release)

| Browser | WebGPU | wasm32 4 GB | Keyboard Lock | Verdict |
|---|---|---|---|---|
| Chrome / Edge (desktop) | yes | yes | yes | primary target |
| Safari 26 (macOS) | yes | yes | no | second; Esc/Ctrl key workarounds |
| Firefox (Windows) | yes (141+) | yes | no | second |
| Firefox (macOS/Linux) | partial / behind flag | yes | no | later |
| iPad / phones | Safari 26 only; no BC textures; tight memory | ? | no | out of scope v1 |

No WebGL2 fallback: the FX pipeline and vertex pulling need WebGPU; WebGL2 would be a second
backend for shrinking returns.

## Performance budget

- Native game thread is ~5.8 ms/frame busy in a busy town on an M1 Max (3.1 ms game code). wasm
  runs this kind of code at ~1.3-2× native → ~8-12 ms. Fine for 60 fps on desktop, tight on
  low-end laptops; 30 fps mode exists.
- The guest lock + `RT_SAFEPOINT` clock checks call `emscripten_get_now` (JS); check the cost in
  the profile and widen the check interval if needed.
- The 4096-entry indirect-call cache is `_Thread_local`; under wasm TLS that is a global-base add,
  cheap.
- Render worker: WebGPU call overhead (above) plus uploads. Keep the queue ring large enough for
  one frame ahead.

## Gate 0 results (2026-10-05): go

**Compile.** All of `generated/all` + `generated/ffxi` (22,297 functions, 56 files) builds with
emsdk 6.0.11, `emcc -O2 -pthread`, `GUEST_BASE = 0` (no `RT_GUEST_WINDOW`), with no source changes
and no errors: 73 s on 10 cores, slowest file 42 s. Linked with `runtime.c`: **32.8 MB wasm,
5.7 MB brotli**. In V8 (Node 24), eager compile of the whole module takes **0.16 s** with Liftoff
and 6.4 s with TurboFan (background tier-up), and it validates, so no function exceeds the JS-API
limits all engines share (largest body 574 KB vs 7.6 MB; most locals 126 vs 50,000).

**Stack.** Measured on macOS with `XI_CFLAGS=-DRT_WATERMARK` (`runtime/portable/watermark.c`: a
probe on every `rt_call_indirect`) over a static-server tour of 12 zones (Port and Lower Jeuno,
Whitegate, Bastok Markets, Windurst Woods, Southern San d'Oria, Western Adoulin, Tavnazian
Safehold, Western Altepa, Jugner Forest, Garden of Ru'Hmet) with camera turns: **host stack peak
20 KB, guest stack peak 17 KB**. In the wasm, functions have a median of 13 locals and at most 126,
so even Liftoff frames (a slot per local) stay around 1-2 KB. Against a ~1 MB browser stack that is
about 50x headroom: the worst risk is retired. (The probe misses chains with no indirect call; the
headroom covers them.) Set Emscripten's shadow stack (`STACK_SIZE`) to 1 MB per thread anyway.

**Memory.** Guest commit peaks at **314 MB**; committed stays ~300 MB across zone changes. The
highest reservation (`top`, what wasm memory must cover) reached 0x1A690000 (~423 MB) and creeps up
with each zone change, because gwin's allocator is next-fit from a hint. For wasm, make it first-fit
from the bottom so memory size tracks the working set. Either way the 4 GB ceiling is far off.

**Thunks.** `0xFFF00000+` is never mapped natively (PROT_NONE), so any access there would already
fault on macOS: nothing the game runs dereferences thunk addresses.

Not covered: busy crowds (the static server has no NPCs; the replay recordings aren't on this
machine). Re-run the watermark build on the replay `crowd` and `effects` scenes when they are.

## Phases

| # | Gate / phase | Work | Size |
|---|---|---|---|
| **G0** | **Feasibility on the desk: done, go (see above)** | (1) `emcc -O2` the generated C as a library: compile time, wasm size, largest functions vs V8 limits. (2) On Mac, log gwin's high-water mark and max guest call depth / host stack use in a busy town and a zone change. (3) Check nothing dereferences `0xFFF00000+`. → confirm the guest + host fit in 4 GB with the host region below the image, decide on stack mitigation. | 3-5 days |
| **G1** | **Headless boot** | `runtime/web/` platform (memory, threads, time, files via sync XHR), `gfx_null`, `webserve.py` with `/dat`, `/user`, `/net`. Run first in Node (fast iteration), then in Chrome. Sign in to `staticserver.py`, reach character select and zone in, per host64.log. | 2-3 wk |
| 3 | WebGPU backend | Restore `gfx_queue.c` onto main, WGSL dialect, `gfx_webgpu.c` (core draw path first, FX passes second), render worker, sign-in screen. `gfx_test` / `d3d8_test` ported to run in the browser against the same expected images. | 3-5 wk |
| 4 | Input | Shell input ring, keyboard tables + browser-key workarounds, mouse + Pointer Lock/software cursor, Gamepad API snapshot + mapping DB, paste. | 1.5-2 wk |
| 5 | Audio | AudioWorklet ring, latency tuning. | 3-5 days |
| 6 | PX + caching | Start page/preflight, PWA, fullscreen/Keyboard Lock/Wake Lock, OPFS block cache, pipeline prewarm, crash overlay, OSK, remap UI. | 2-3 wk |
| 7 | Hardening | Natsumi-style replay scenes in the browser (fps, 360 pans), Safari + Firefox passes, device-loss, background-tab soak, 2-hour play session. | 1-2 wk |
| later | Extras | Launcher "Play in browser" button (server in Rust), LAN play over HTTPS, IME, Discord via server, addons (PUC Lua 5.1 + an FFI shim; large), iPad. | — |

Testing at every phase uses `tools/staticserver.py` / `replayserver.py` so no live server is
needed, and `replayscene.py` scenes for repeatable fps numbers.

## Risks, worst first

1. **Stack depth.** Each guest call is a C call, and browsers give wasm ~1 MB of native stack
   that the page can't raise. If the game's deepest chains overflow it, mitigations are: trim
   frame size in generated code (fewer locals per C frame; `-O2` vs `-Os` per file), or have the
   recompiler turn the few deep or recursive paths into tail calls / a trampoline. wasm64 doesn't
   help here. Gate 0 measures it before anything else is built.
2. **4 GB wasm32 ceiling.** The identity map depends on carving a host region below the image and
   on gwin growing memory bottom-up. If the game's working set plus our host data nears ~3.5 GB,
   shrink host use (DAT block cache, upload staging) before anything else; memory64 is rejected.
3. **wasm → JS call overhead** for WebGPU in busy scenes. Mitigation is batching in the backend;
   measure in Phase 3 with the replay scenes.
4. **Browser keys** (Esc, Ctrl+number) outside Chromium fullscreen. Workarounds exist but play
   is better in Chrome/Edge or the installed PWA; say so on the start page.
5. **Big generated functions** hitting engine limits or slow tier-up. Gate 0 checks; the
   recompiler can split functions if needed.
6. **Licence/hygiene**: the built wasm is Square Enix-derived. Never upload it, never cache it on a
   CDN, never ship a hosted demo. Keep that rule in the server's README and the build's output
   folder name.
