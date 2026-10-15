"""Generate the first modern-zone replay suite from real visits, preserving their spawn positions."""
import argparse
from pathlib import Path
import shlex

from replayscene import Scene

ZONES = (('south-gustaberg', 107), ('valkurm-dunes', 103))


def make_suite(recordings, output):
    output = Path(output).resolve()
    lines = ['# Four fixed player headings per real zone visit. Camera matrices must be checked in captures.',
             '# Keep game input and camera mode fixed. This does not synthesize a deterministic camera path.']
    for (name, zone), recording in zip(ZONES, recordings):
        path = Path(recording).resolve()
        scene = Scene.load(str(path))
        if scene.zone != zone:
            raise ValueError(f'{name} requires a recorded visit to zone {zone}, got {scene.zone}')
        for turn in (0, 90, 180, 270):
            lines.append(f'{shlex.quote(str(path))} --turn {turn} --hold --label {name}-{turn} --group modern-zone')
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text('\n'.join(lines) + '\n')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--south', required=True, type=Path)
    p.add_argument('--valkurm', required=True, type=Path)
    p.add_argument('--out', required=True, type=Path)
    a = p.parse_args()
    try:
        make_suite((a.south, a.valkurm), a.out)
    except (ValueError, OSError) as e:
        p.exit(1, f'zone suite: {e}\n')


if __name__ == '__main__':
    main()
