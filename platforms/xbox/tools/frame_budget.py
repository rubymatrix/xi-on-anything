#!/usr/bin/env python3
"""Project host64's per-frame CPU cost onto a slower CPU (Xbox One's Jaguar by default).

Reads the FFXI_PROFILE=1 output of xi-on-anything's host64: pairs of lines

    [gfx] 58.4 fps: frame 17.11 ms = game code 2.53 + API calls 14.58 (...) | GPU 7.28 ms | 753 draws, ...
    [gfx]   API time (2 s): Sleep 1497 ms/1324 IDirect3DDevice8::DrawPrimitiveUP 101 ms/77571 ...

Both cover the game thread only. Busy time per frame = frame time - Sleep per frame: the
game's own limiter (Sleep(1) loops) is idle, not work. Busy time times the slowdown factor
is the projected frame time on the target; FFXI's retail frame rate is 30 fps (33.3 ms).

    python3 tools/frame_budget.py <run.log> [--factor 6.5] [--min-draws 500]
"""
import argparse
import re
import statistics
import sys

FPS = re.compile(r"\[gfx\] ([\d.]+) fps: frame ([\d.]+) ms = game code ([\d.]+) .*?\| GPU ([\d.]+) ms \| (\d+) draws")
API = re.compile(r"\[gfx\]\s+API time \((\d+) s\): Sleep (\d+) ms/")


def parse(path):
    rows, pending = [], None
    with open(path, errors="replace") as f:
        for line in f:
            m = FPS.search(line)
            if m:
                fps, frame, game, gpu, draws = (float(x) for x in m.groups())
                pending = dict(fps=fps, frame=frame, game=game, gpu=gpu, draws=int(draws))
                continue
            m = API.search(line)
            if m and pending:
                secs, sleep_ms = float(m.group(1)), float(m.group(2))
                sleep = sleep_ms / (pending["fps"] * secs)
                pending["sleep"] = sleep
                pending["busy"] = max(pending["frame"] - sleep, 0.0)
                rows.append(pending)
                pending = None
    return rows


def pct(xs, p):
    xs = sorted(xs)
    return xs[min(len(xs) - 1, int(round(p / 100 * (len(xs) - 1))))]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--factor", type=float, default=6.5,
                    help="per-core slowdown of the target vs. the measuring machine (M1 Max -> Jaguar: ~5-8)")
    ap.add_argument("--min-draws", type=int, default=500, help="keep only busy scenes (in-zone, not menus)")
    ap.add_argument("--budget", type=float, default=1000 / 30, help="target frame time, ms (default 30 fps)")
    a = ap.parse_args()

    rows = [r for r in parse(a.log) if r["draws"] >= a.min_draws]
    if not rows:
        sys.exit(f"no profile samples with >= {a.min_draws} draws in {a.log} (run host64 with FFXI_PROFILE=1)")

    busy = [r["busy"] for r in rows]
    print(f"{len(rows)} samples (2 s each) with >= {a.min_draws} draws, {a.log}")
    print(f"  measured busy ms/frame   median {statistics.median(busy):5.2f}   p95 {pct(busy, 95):5.2f}   max {max(busy):5.2f}")
    print(f"  of which game code       median {statistics.median(r['game'] for r in rows):5.2f}")
    print(f"  GPU ms/frame (measuring) median {statistics.median(r['gpu'] for r in rows):5.2f}")
    print(f"  budget {a.budget:.1f} ms/frame")
    for f in sorted({a.factor * 0.75, a.factor, a.factor * 1.25}):
        med, p95 = statistics.median(busy) * f, pct(busy, 95) * f
        verdict = "OK" if p95 <= a.budget else ("tight" if med <= a.budget else "over")
        print(f"  x{f:4.1f}: projected median {med:5.1f} ms ({1000 / med:4.0f} fps)   p95 {p95:5.1f} ms ({1000 / p95:4.0f} fps)   {verdict}")


if __name__ == "__main__":
    main()
