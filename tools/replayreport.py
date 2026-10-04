#!/usr/bin/env python3
"""Frame times per scene and phase, from a replay's frame log (the xireplay addon's frames-*.csv).

The log has each Present ("f,<us>"), each zone-in ("z,<us>") and each event tools/replayserver.py
sent ("m,<us>,<event>": begin|N|scene|..., mark|N|scene|label, end|N|scene, ...), all on the
client's clock. A scene's phases run from each of its markers to the next; "measured" is its start
marker to its end marker (else its first marker, past the zone-in's loading, to its end).

  python3 tools/replayreport.py frames-20261003-165743.csv [--json]
"""
import argparse
import json
import sys


def load(path):
    presents, events = [], []
    with open(path, encoding='utf-8') as f:
        for line in f:
            parts = line.rstrip('\n').split(',', 2)
            if len(parts) < 2:
                continue
            try:
                at = int(float(parts[1]))  # microseconds, written as an integer or a float (1.79e+15)
            except ValueError:
                continue
            if parts[0] == 'f':
                presents.append(at)
            elif parts[0] == 'm' and len(parts) == 3:
                events.append((at, parts[2].split('|')))
    return presents, events


FRAME_33MS = 33.4  # a frame over two of the game's 60 Hz refreshes (16.7 ms each)


def percentile(ordered, q):
    return ordered[min(len(ordered) - 1, int(q * len(ordered)))]


def measure(presents, scene, name, label, begin, lo, hi):
    """One phase: the frame times of the presents between lo and hi (microseconds on the log's clock),
    and its window in seconds from the scene's begin."""
    ms = [(b - a) / 1000 for a, b in zip(presents, presents[1:]) if a >= lo and b <= hi]
    p = {'scene': scene, 'name': name, 'phase': label, 'from_s': (lo - begin) / 1e6, 'to_s': (hi - begin) / 1e6,
         'frames': len(ms)}
    if ms:
        s = sorted(ms)
        mean = sum(ms) / len(ms)
        p.update(fps=1000 / mean, mean_ms=mean, p50_ms=percentile(s, 0.5), p95_ms=percentile(s, 0.95),
                 p99_ms=percentile(s, 0.99), max_ms=s[-1], frames_over_33ms=sum(v > FRAME_33MS for v in ms),
                 frames_over_100ms=sum(v > 100 for v in ms))
    return p


def compute(presents, events):
    """Every scene play's phases, in the order they played: each begin starts a play of its own, so a
    scene asked for twice is measured twice."""
    plays = []
    current = {}  # scene number -> its play under way
    for at, f in events:
        if f[0] not in ('begin', 'mark', 'end') or len(f) < 3 or not f[1].isdigit():
            continue
        n = int(f[1])
        if f[0] == 'begin' or n not in current:
            current[n] = {'n': n, 'name': f[2], 'begin': at if f[0] == 'begin' else None, 'end': None, 'marks': []}
            plays.append(current[n])
        sc = current[n]
        if f[0] == 'end':
            sc['end'] = at
        elif f[0] == 'mark':
            sc['marks'].append((at, f[3] if len(f) > 3 else ''))
    out = []
    for sc in plays:
        n = sc['n']
        if sc['begin'] is None:
            continue
        stop = sc['end'] if sc['end'] is not None else (presents[-1] if presents else sc['begin'])
        marks = sc['marks']
        for i, (at, label) in enumerate(marks):
            if label == 'end' or (label == 'start' and i + 1 < len(marks) and marks[i + 1][1] == 'end'):
                continue
            out.append(measure(presents, n, sc['name'], label, sc['begin'], at, marks[i + 1][0] if i + 1 < len(marks) else stop))
        start = next((at for at, label in marks if label == 'start'), marks[0][0] if marks else sc['begin'])
        end = next((at for at, label in marks if label == 'end'), stop)
        out.append(measure(presents, n, sc['name'], 'measured', sc['begin'], start, end))
    return out


def table(phases):
    lines = [f'{"#":<3} {"scene":<22} {"phase":<14} {"window (s)":>13} {"frames":>6} {"fps":>6} {"mean ms":>8} '
             f'{"p50":>7} {"p95":>7} {"p99":>7} {"max":>8} {">33ms":>5}']
    for p in phases:
        stats = (f'{p["fps"]:6.1f} {p["mean_ms"]:8.2f} {p["p50_ms"]:7.2f} {p["p95_ms"]:7.2f} {p["p99_ms"]:7.2f} '
                 f'{p["max_ms"]:8.2f} {p["frames_over_33ms"]:5d}') if p['frames'] else ' ' * 6 + ' no frames'
        lines.append(f'{p["scene"]:<3} {p["name"][:22]:<22} {p["phase"][:14]:<14} {p["from_s"]:5.1f}-{p["to_s"]:<7.1f} '
                     f'{p["frames"]:6d} {stats}')
    return '\n'.join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('frames', help="a replay's frame log")
    ap.add_argument('--json', action='store_true')
    a = ap.parse_args()
    phases = compute(*load(a.frames))
    if a.json:
        json.dump(phases, sys.stdout, indent=2)
        print()
    else:
        print(table(phases))


if __name__ == '__main__':
    main()
