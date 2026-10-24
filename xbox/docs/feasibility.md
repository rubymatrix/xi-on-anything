# Feasibility: CPU budget on Xbox One

Gate 0 of the plan: before building anything, check whether the recompiled game can hold
30 fps (FFXI's retail frame rate, 33.3 ms per frame) on Xbox One's CPU.

## Method

`xbox/tools/frame_budget.py` reads the `FFXI_PROFILE=1` output of xi-on-anything's `host64` on
macOS (M1 Max). The profile covers the game thread only. Busy time per frame is
frame time minus the time in `Sleep`, because the game's frame limiter spins in `Sleep(1)`
loops that are idle time, not work. Busy time times a per-core slowdown factor gives the
projected frame time on the Jaguar.

Slowdown factor, M1 Max P-core to Xbox One Jaguar (1.75 GHz): about 5–8x single-threaded,
6.5x used as the central estimate. This is the weakest input; only running on the console
settles it (gate 1).

## Results (2026-09-25, logs from 2026-09-24 runs, in-zone samples with >= 500 draws)

| log | samples | busy ms/frame median / p95 | game code median | projected at 6.5x, median / p95 |
|---|---|---|---|---|
| `run_phoenix_0903b.log` | 623 | 5.85 / 7.01 | 3.14 | 38.0 ms (26 fps) / 45.6 ms (22 fps) |
| `run_phoenix_dats.log` | 59 | 5.78 / 10.38 | 2.74 | 37.6 ms (27 fps) / 67.5 ms (15 fps) |
| `run_phoenix_dats2.log` | 36 | 5.14 / 10.13 | 3.53 | 33.4 ms (30 fps) / 65.9 ms (15 fps) |

Busiest scenes only (>= 1500 draws, `run_phoenix_0903b.log`): busy 6.45 ms median,
41.9 ms (24 fps) projected at 6.5x.

The high p95 values in the `dats` runs come from zoning and first-time asset loads, not
steady-state play.

## Reading

- **As built today, it's close but short:** about 24–30 fps in busy scenes at 6.5x.
  At 5x it fits 30 fps; at 8x it doesn't.
- **About half of busy time isn't the game.** Game code is ~3.1 ms; the other ~2.7 ms is
  our D3D8 front end and back end on the game thread (for example ~150k `SetRenderState`
  calls per second, and draw encoding). The console has several cores. Moving command
  encoding to a render thread leaves ~3.1 ms x 6.5 = ~20 ms on the game thread, which fits
  30 fps with headroom.
- **GPU isn't the constraint.** 6–12 ms on the M1 Max, but at a 4096x4096 background
  buffer. At 1920x1080 that's about 8x fewer pixels, which roughly offsets the Xbox One
  GPU being ~8x slower.

**Verdict: go, with the render thread as a required piece of the D3D11 back end, not an
optimization for later.**

## Gate 1 (on the console)

Build a minimal UWP package: null graphics, scripted login into a busy zone, with
`FFXI_PROFILE=1` output written to LocalState. Run `frame_budget.py` on it with
`--factor 1`. This replaces the estimated factor with a measurement, and it also shows
how well MSVC's x64 codegen for the generated C runs on the Jaguar.
