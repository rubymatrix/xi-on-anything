#!/usr/bin/env python3
"""Runs recorded scenes with nobody at the screen: tools/replayserver.py, host64 driven through its
control port (FFXI_CONTROL: the lobby's keys, chat commands, frame captures), the xireplay addon's
frame log, and tools/replayreport.py's report.

  python3 tools/replay.py run tools/replay/default.txt          every scene, then a report
  python3 tools/replay.py run suite.txt --play "weather crowd"  some
  python3 tools/replay.py play suite.txt                        a session to watch: !replay in chat
  python3 tools/replay.py shots suite.txt                       a frame capture at each scene's READY
  python3 tools/replay.py prof suite.txt                        a capture and the GPU's ms per effects pass
                                                                at each scene's READY (markets, noon, midnight)
  python3 tools/replay.py record all --server <yours> --user <a GM>   the addon's scenes, recorded
  python3 tools/replay.py keep generated/replay/*.jsonl --lsb <commit> --build <build>
                                                                the recordings, as the reference set

Results go to --out (default generated/runs/<date>-<suite>/): server.log, client.log, frames.csv,
report.txt, report.json, for shots the captures, and for prof the captures with each scene's
NN-<scene>.prof.json beside them and prof.txt / prof.json for the whole run. The addon (tools/replay/xireplay) is copied into
the data folder's ashita/addons and loaded through the control port; the game's settings are its own.
--huffman (or FFXI_HUFFMAN) as for replayserver.py; arguments after "--" go to host64. --auth-port,
--data-port, --view-port and --zone-port move the replay server off LandSandBoat's ports (and another
replay or static server's); --control-port moves host64's control port.
"""
import argparse
import glob
import json
import os
import re
import shutil
import signal
import socket
import struct
import subprocess
import sys
import time
import zlib

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)
import buildinfo  # noqa: E402
import replayreport  # noqa: E402
import replayscene  # noqa: E402
import replayserver  # noqa: E402

SCENES = os.path.join(TOOLS, 'replay', 'scenes')  # the committed recordings tools/replay/default.txt plays
REFERENCE_CHARACTER = 1  # the character id every scene there has (the replay server signs in one for a suite)

SETTLE = 3.0         # seconds after a scene's READY before its capture: effects take a moment to show
LOBBY_TIMEOUT = 180  # seconds from the client's start to the character in a zone
RUN_TIMEOUT = 3600   # seconds the scenes may take once in the zone


def default_data_dir():
    """host64's own default (SDL_GetPrefPath("FFXIRecompile", "FFXI"))."""
    if sys.platform == 'darwin':
        return os.path.expanduser('~/Library/Application Support/FFXIRecompile/FFXI')
    if sys.platform == 'win32':
        return os.path.join(os.environ.get('APPDATA', ''), 'FFXIRecompile', 'FFXI')
    return os.path.join(os.environ.get('XDG_DATA_HOME', os.path.expanduser('~/.local/share')), 'FFXIRecompile', 'FFXI')


def wait_port_free(port, wait=60):
    """Wait until the control port can be listened on, as the game will: a game still closing (the last
    run's) holds it a while, and the new one, unable to listen, would leave this driving the old one."""
    deadline = time.time() + wait
    while True:
        with socket.socket() as s:
            # as the game's listener does (control.lua), so the last run's connections in TIME_WAIT don't
            # count; not on Windows, where it would let this bind a port that is in use
            if sys.platform != 'win32':
                s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            try:
                s.bind(('127.0.0.1', port))
                return
            except OSError:
                pass
        if time.time() > deadline:
            raise SystemExit(f'the control port {port} is taken: is another game still running?')
        time.sleep(1)


def wait_server(server, log_path, timeout=60):
    """Wait until the replay server is listening: it logs "[server] ..." once its ports are open."""
    deadline = time.time() + timeout
    while server.poll() is None:
        with open(log_path, encoding='utf-8', errors='replace') as f:
            if '[server] ' in f.read():
                return
        if time.time() > deadline:
            raise SystemExit(f'the server is not listening after {timeout} s; see {log_path}')
        time.sleep(0.2)
    raise SystemExit(f'the server stopped; see {log_path}')


class Control:
    """host64's control port: one JSON request a line, one reply a line."""

    def __init__(self, port, timeout):
        deadline = time.time() + timeout
        while True:
            try:
                self.sock = socket.create_connection(('127.0.0.1', port), timeout=30)
                break
            except OSError:
                if time.time() > deadline:
                    raise SystemExit(f'no control port on {port}: is FFXI_CONTROL set and the game running?') from None
                time.sleep(1)
        self.file = self.sock.makefile('rw', encoding='utf-8')
        self.id = 0

    def __call__(self, cmd, **args):
        self.id += 1
        self.file.write(json.dumps({'id': self.id, 'cmd': cmd, 'args': args}) + '\n')
        self.file.flush()
        line = self.file.readline()
        if not line:
            raise RuntimeError(f'{cmd}: the control port closed')
        reply = json.loads(line)
        if not reply.get('ok'):
            raise RuntimeError(f'{cmd}: {reply.get("error")}')
        return reply.get('result')

    def close(self):
        self.file.close()
        self.sock.close()


def stop(proc, wait=15):
    """End a process: politely, then not."""
    if proc is None or proc.poll() is not None:
        return
    proc.terminate()
    try:
        proc.wait(wait)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()


def install_addon(data):
    dest = os.path.join(data, 'ashita', 'addons', 'xireplay')
    os.makedirs(dest, exist_ok=True)
    shutil.copy2(os.path.join(TOOLS, 'replay', 'xireplay', 'xireplay.lua'), dest)
    # a data folder of its own has no Ashita libs (the addon needs them): the default folder's, if it has them
    libs, own = os.path.join(data, 'ashita', 'addons', 'libs'), os.path.join(default_data_dir(), 'ashita', 'addons', 'libs')
    if not os.path.isdir(libs) and os.path.isdir(own) and os.path.realpath(libs) != os.path.realpath(own):
        shutil.copytree(own, libs)


def launch(a, data, log_path, login, env=None):
    """host64 started with the control port on, and connected to: the process and its Control."""
    wait_port_free(a.control_port)
    env = dict(os.environ, FFXI_CONTROL=str(a.control_port), FFXI_DISCORD='0', **(env or {}))
    cmd = [a.client, '--game', a.game, '--data-dir', data] + login + a.client_args
    with open(log_path, 'w') as log:
        client = subprocess.Popen(cmd, env=env, stdout=log, stderr=subprocess.STDOUT,
                                  cwd=os.path.dirname(os.path.dirname(os.path.abspath(a.client))))
    try:
        return client, Control(a.control_port, 120)
    except BaseException:
        stop(client)
        raise


def through_lobby(ctl, timeout=LOBBY_TIMEOUT):
    """Enter until the character is in a zone: the rules, the lobby's first item, the first character
    and the confirmation are each the default."""
    deadline = time.time() + timeout
    time.sleep(5)
    while time.time() < deadline:
        st = ctl('state') or {}
        if (st.get('zone') or {}).get('id'):
            return st
        ctl('keys', keys=['enter'], hold_ms=80)
        time.sleep(2)
    raise SystemExit('still not in a zone after the lobby')


def frame_log(frames_dir, since, client, timeout=60):
    """The newest frame log the addon started after since, once it appears."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        if client.poll() is not None:
            raise SystemExit('the client exited before the addon logged a frame')
        logs = [p for p in glob.glob(os.path.join(frames_dir, 'frames-*.csv')) if os.path.getmtime(p) >= since]
        if logs:
            return max(logs, key=os.path.getmtime)
        time.sleep(0.5)
    raise SystemExit(f'no frame log in {frames_dir}: is the xireplay addon loaded?')


class Events:
    """The events in a frame log as they are written: "m,<us>,<event>" lines, each read once, and only
    once the line is whole."""

    def __init__(self, path):
        self.path = path
        self.offset = 0

    def new(self):
        with open(self.path, encoding='utf-8') as f:
            f.seek(self.offset)
            text = f.read()
        end = text.rfind('\n') + 1
        self.offset += len(text[:end].encode('utf-8'))
        fields = (line.split(',', 2) for line in text[:end].splitlines())
        return [f[2] for f in fields if len(f) == 3 and f[0] == 'm']


def session(a, autoplay, on_event=None, wait_done=True):
    out = a.out or os.path.join(ROOT, 'generated', 'runs', time.strftime('%Y%m%d-%H%M%S') + '-' +
                                os.path.splitext(os.path.basename(a.suite))[0])
    os.makedirs(out, exist_ok=True)
    data = os.path.abspath(a.data_dir)
    install_addon(data)
    frames_dir = os.path.join(data, 'ashita', 'config', 'addons', 'xireplay')
    started = time.time()
    with open(os.path.join(out, 'server.log'), 'w') as server_log:
        server = subprocess.Popen([sys.executable, '-u', os.path.join(TOOLS, 'replayserver.py'), '--huffman', a.huffman,
                                   '--suite', a.suite, '--autoplay', autoplay, '--auth-port', str(a.auth_port),
                                   '--data-port', str(a.data_port), '--view-port', str(a.view_port),
                                   '--zone-port', str(a.zone_port)], stdout=server_log, stderr=subprocess.STDOUT)
    client = ctl = None
    try:
        wait_server(server, os.path.join(out, 'server.log'))
        client, ctl = launch(a, data, os.path.join(out, 'client.log'),
                             ['--server', '127.0.0.1', '--user', 'replay', '--pass', 'replay',
                              '--authport', str(a.auth_port), '--dataport', str(a.data_port),
                              '--viewport', str(a.view_port)])
        for line in ('/addon load xireplay', '/xireplay record off', '/xireplay quiet on', '/xireplay frames on'):
            ctl('chat_send', line=line)
        through_lobby(ctl)
        print(f'in the world; results in {out}', flush=True)
        if not wait_done:
            print('type "!replay list" in the game; close it to end the session', flush=True)
        frames = frame_log(frames_dir, started, client)
        events = Events(frames)
        deadline = time.time() + RUN_TIMEOUT
        done = False
        while not done and client.poll() is None:
            if server.poll() is not None:
                raise SystemExit(f'the server stopped; see {out}/server.log')
            for e in events.new():
                print('  ' + e, flush=True)
                if on_event:
                    on_event(ctl, e, out)
                done = done or e.startswith('done|')
            if wait_done and time.time() > deadline:
                raise SystemExit(f'the scenes took over {RUN_TIMEOUT} s')
            time.sleep(0.3)
        if not wait_done:  # a watched session: over when the game is closed
            return out
        if not done:
            raise SystemExit(f'the client exited before the scenes were done; see {out}/client.log')
        time.sleep(2)
        shutil.copy2(frames, os.path.join(out, 'frames.csv'))
    finally:
        if ctl:
            ctl.close()
        stop(client)
        stop(server)
    phases = replayreport.compute(*replayreport.load(os.path.join(out, 'frames.csv')))
    with open(os.path.join(out, 'report.json'), 'w') as f:
        json.dump(phases, f, indent=2)
    text = replayreport.table(phases)
    with open(os.path.join(out, 'report.txt'), 'w') as f:
        f.write(text + '\n')
    print(text)
    return out


def png(raw_path, png_path):
    """host64's frame capture (d3d8_capture: "XIF1", width, height, D3D format, then the pixels; 21 and
    22 are B, G, R, A/X bytes) as a PNG. False if it is not one."""
    with open(raw_path, 'rb') as f:
        data = f.read()
    if len(data) < 16:
        return False
    magic, w, h, fmt = struct.unpack_from('<4sIII', data)
    if magic != b'XIF1' or fmt not in (21, 22) or len(data) < 16 + w * h * 4:
        return False
    px = data[16:16 + w * h * 4]
    rows = bytearray()
    for y in range(h):
        row = px[y * w * 4:(y + 1) * w * 4]
        rgb = bytearray(w * 3)
        rgb[0::3], rgb[1::3], rgb[2::3] = row[2::4], row[1::4], row[0::4]
        rows += b'\0' + rgb

    def chunk(kind, body):
        return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body) & 0xFFFFFFFF)
    with open(png_path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
                + chunk(b'IDAT', zlib.compress(rows, 6)) + chunk(b'IEND', b''))
    return True


class Shots:
    """A frame capture at each scene's READY: the first marker after its BEGIN that is not its end."""

    def __init__(self):
        self.pending = False

    def __call__(self, ctl, event, out):
        f = event.split('|')
        if f[0] == 'begin':
            self.pending = True
        elif f[0] == 'mark' and self.pending and len(f) > 3 and f[3] != 'end':
            self.pending = False
            time.sleep(SETTLE)
            self.capture(ctl, os.path.join(os.path.abspath(out), f'{int(f[1]):02d}-{f[2]}'))

    @staticmethod
    def capture(ctl, path):
        """A frame capture to path.png (or path.raw, if not a format png() converts)."""
        ctl('capture', path=path + '.raw')
        for _ in range(20):  # written on a later frame
            if os.path.exists(path + '.raw') and os.path.getsize(path + '.raw') > 16:
                break
            time.sleep(0.25)
        else:
            print(f'  no capture for {os.path.basename(path)}', flush=True)
            return
        time.sleep(0.5)
        if png(path + '.raw', path + '.png'):
            os.remove(path + '.raw')
            print(f'  captured {path}.png', flush=True)
        else:
            print(f'  captured {path}.raw (not a format this converts)', flush=True)


PASSES = ('sun maps', 'linz/zmip', 'ao', 'ao rays', 'blur', 'temporal', 'gi', 'bloom', 'rays', 'comp', 'fxaa',
          'present', 'world', 'chars', 'shadow zone', 'shadow chars', 'post bloom', 'post dof',
          'scene ao', 'scene ao trace')  # gfx_fx.c's k_pass_name, in its order (world .. shadow
# chars: the scene renderer's)
PROF_SCENES = 'markets noon midnight'  # prof's scenes when --play isn't given: a city, and the sun up and down
PROF_WINDOW = 2.05  # seconds between /fx prof calls: the client sums two-second windows (gfx_pass_tick)
PROF_HEAD = re.compile(r'fx prof: GPU ms a frame over ([\d.]+) s \((\d+) frames\): frame ([\d.]+), '
                       r'sun maps ([\d.]+), effects ([\d.]+)')


def prof_parse(lines):
    """The windows in /fx prof's lines in a log (gfx_pass_report's, as cmd.c logs them): a header, then
    the passes that ran, a few to a line. Each a dict: secs, frames, frame (the command buffers' GPU time,
    which overcounts), effects, and passes {name: ms}."""
    windows = []
    for line in lines:
        m = PROF_HEAD.search(line)
        if m:
            secs, frames, frame, sun, effects = m.groups()
            windows.append({'secs': float(secs), 'frames': int(frames), 'frame': float(frame),
                            'effects': float(effects), 'passes': {'sun maps': float(sun)} if float(sun) else {}})
        elif windows and 'fx prof:   ' in line:
            for item in line.split('fx prof:   ', 1)[1].split(', '):
                name, _, ms = item.strip().rpartition(' ')
                try:
                    windows[-1]['passes'][name] = float(ms)
                except ValueError:
                    pass
    return windows


def prof_mean(windows):
    """Windows' ms a frame averaged, weighted by their frames: fps, frame, effects and every pass."""
    frames = sum(w['frames'] for w in windows)
    if not frames:
        return {}
    names = [p for p in PASSES if any(p in w['passes'] for w in windows)]
    names += sorted({p for w in windows for p in w['passes']} - set(names))
    mean = {'fps': frames / sum(w['secs'] for w in windows)}
    for key in ('frame', 'effects'):
        mean[key] = sum(w[key] * w['frames'] for w in windows) / frames
    mean['passes'] = {p: sum(w['passes'].get(p, 0.0) * w['frames'] for w in windows) / frames for p in names}
    return mean


class Prof(Shots):
    """At each scene's READY: the GPU's ms a frame per effects pass (/fx prof, from host64's log), then a
    frame capture. The first /fx prof (at the first scene's BEGIN) turns the timing on; at READY, after
    SETTLE, it is asked windows + 1 times, PROF_WINDOW apart, the first dropped (it may hold frames from
    before READY), and the windows saved as NN-<scene>.prof.json beside the capture."""

    def __init__(self, windows):
        super().__init__()
        self.windows = windows
        self.on = False
        self.scenes = []

    def ask(self, ctl, log):
        """One /fx prof: the window it logs, or None if none came within two seconds."""
        start = os.path.getsize(log)
        ctl('chat_send', line='/fx prof')
        deadline = time.time() + 2
        while time.time() < deadline:
            time.sleep(0.2)
            with open(log, encoding='utf-8', errors='replace') as f:
                f.seek(start)
                found = prof_parse(f.read().splitlines())
            if found:
                time.sleep(0.1)  # the rest of the report's lines
                with open(log, encoding='utf-8', errors='replace') as f:
                    f.seek(start)
                    return prof_parse(f.read().splitlines())[0]
        return None

    def __call__(self, ctl, event, out):
        f = event.split('|')
        if f[0] == 'begin':
            self.pending = True
            if not self.on:
                ctl('chat_send', line='/fx prof')
                self.on = True
        elif f[0] == 'mark' and self.pending and len(f) > 3 and f[3] != 'end':
            self.pending = False
            time.sleep(SETTLE)
            path = os.path.join(os.path.abspath(out), f'{int(f[1]):02d}-{f[2]}')
            windows, last = [], None
            for i in range(self.windows + 1):
                time.sleep(PROF_WINDOW)
                w = self.ask(ctl, os.path.join(out, 'client.log'))
                if i and w and w != last:
                    windows.append(w)
                last = w or last
            scene = {'index': int(f[1]), 'scene': f[2], 'windows': windows, 'mean': prof_mean(windows)}
            with open(path + '.prof.json', 'w') as fp:
                json.dump(scene, fp, indent=2)
            self.scenes.append(scene)
            m = scene['mean']
            if m:
                print(f"  {f[2]}: {len(windows)} windows, {m['fps']:.1f} fps, effects {m['effects']:.2f} ms, "
                      + ', '.join(f'{p} {v:.2f}' for p, v in m['passes'].items()), flush=True)
            else:
                print(f'  {f[2]}: no GPU pass timing in the log (FFXI_FX=0, or a back end without it?)', flush=True)
            self.capture(ctl, path)


def prof_table(scenes):
    """Passes down, scenes across: ms a frame on the GPU, averaged over each scene's windows."""
    names = [p for p in PASSES if any(p in s['mean'].get('passes', {}) for s in scenes)]
    names += sorted({p for s in scenes for p in s['mean'].get('passes', {})} - set(names))
    cols = [f"{s['index']:02d}-{s['scene']}" for s in scenes]
    width = max([10] + [len(c) for c in cols]) + 2
    rows = [('pass (GPU ms a frame)', cols)]

    def row(label, get, fmt='{:.2f}'):
        rows.append((label, [fmt.format(get(s['mean'])) if s['mean'] else '-' for s in scenes]))
    for p in names:
        row(p, lambda m, p=p: m['passes'].get(p, 0.0))
    row('effects (sum)', lambda m: m['effects'])
    row('frame (CB sum)', lambda m: m['frame'])
    row('fps', lambda m: m['fps'], '{:.1f}')
    rows.append(('windows', [str(len(s['windows'])) for s in scenes]))
    lines = []
    for i, (label, vals) in enumerate(rows):
        lines.append(f'{label:<22}' + ''.join(f'{v:>{width}}' for v in vals))
        if i == 0 or i == len(names):  # under the header, and under the passes
            lines.append('-' * (22 + width * len(cols)))
    lines.append('effects is linz/zmip..fxaa (not sun maps or present); frame is the command buffers\' GPU time summed '
                 '(it overcounts on Apple GPUs: read the passes)')
    return '\n'.join(lines)


def revive(container, user):
    """A character left K.O. (a scene gone wrong) can't run GM commands: its HP back while it is logged
    out, in the server's database (a LandSandBoat Docker setup's container)."""
    if not re.fullmatch(r'[A-Za-z0-9_]+', user):
        raise SystemExit(f'--revive-container: {user!r} is not an account name this can quote')
    sql = ("UPDATE char_stats s JOIN chars c USING(charid) JOIN accounts a ON a.id=c.accid "
           f"SET s.hp=GREATEST(s.hp,9999), s.death=0 WHERE a.login='{user}'")
    r = subprocess.run(['docker', 'exec', container, 'sh', '-c', 'mariadb -uroot -p"$MARIADB_ROOT_PASSWORD" xidb -e "$1"',
                        'sh', sql])
    if r.returncode != 0:
        print(f'warning: reviving {user} in {container} failed ({r.returncode})', flush=True)


def record(a):
    """The xireplay addon's scenes, recorded on a LandSandBoat server of yours by a GM character, with
    nobody at the screen; the recordings land in --out (default generated/replay/)."""
    out = a.out or os.path.join(ROOT, 'generated', 'replay')
    os.makedirs(out, exist_ok=True)
    data = os.path.abspath(a.data_dir)
    install_addon(data)
    rec_dir = os.path.join(data, 'ashita', 'config', 'addons', 'xireplay')
    done = os.path.join(rec_dir, 'last-run.txt')
    if os.path.exists(done):
        os.remove(done)
    if a.revive_container:
        revive(a.revive_container, a.user)
    started = time.time()
    client, ctl = launch(a, data, os.path.join(out, 'record-client.log'), ['--server', a.server, '--user', a.user],
                         {'FFXI_PASSWORD': a.password} if a.password else None)
    try:
        for line in ('/addon load xireplay', '/xireplay quiet off', '/xireplay record on',
                     '/xireplay run ' + ' '.join(a.scenes)):
            ctl('chat_send', line=line)
        through_lobby(ctl)
        print('in the world: recording ' + ' '.join(a.scenes), flush=True)
        deadline = time.time() + RUN_TIMEOUT
        while not os.path.exists(done):
            if client.poll() is not None:
                raise SystemExit('the client exited before the scenes were done')
            if time.time() > deadline:
                raise SystemExit(f'the scenes took over {RUN_TIMEOUT} s')
            time.sleep(1)
        time.sleep(2)
    finally:
        ctl.close()
        stop(client)
    for path in sorted(glob.glob(os.path.join(rec_dir, '*-*.jsonl'))):
        if os.path.getmtime(path) >= started and not os.path.basename(path).startswith('capture-'):
            shutil.copy2(path, out)
            print(f'recorded {os.path.basename(path)}')


def recorder_version():
    """The xireplay addon's version (its addon.version)."""
    with open(os.path.join(TOOLS, 'replay', 'xireplay', 'xireplay.lua'), encoding='utf-8') as f:
        return re.search(r"^addon\.version\s*=\s*'([^']+)'", f.read(), re.M).group(1)


def client_build(label):
    """A game build as meta/builds.json names it: '<label> (<client version>)'."""
    builds = buildinfo.known()
    if label not in builds:
        raise SystemExit(f'--build: {label!r} is not a build in meta/builds.json ({", ".join(builds)})')
    return f'{label} ({builds[label]["version"]})'


def keep_file(src, dst, meta):
    """A recording as the reference set keeps it: what playback reads (Scene.load), the character named
    Replay, trimmed (replayscene.trim), its id the set's (REFERENCE_CHARACTER, once trimmed: the trim
    tells its actions by it), and meta added to its meta line. Returns the counts."""
    s = replayscene.Scene.load(src)
    renamed = s.rename('Replay')
    dropped, held = replayscene.trim(s)
    renumbered, left = s.renumber(REFERENCE_CHARACTER)
    s.meta.update(meta)
    s.save(dst)
    return renamed, dropped, held, renumbered, left


def keep(a):
    """Recordings made the reference set, in tools/replay/scenes/ (or --out)."""
    meta = {'server': f'LandSandBoat {a.lsb}', 'client': client_build(a.build),
            'recorder': f'xireplay {recorder_version()}', 'trimmed': list(replayscene.TRIM)}
    out = a.out or SCENES
    os.makedirs(out, exist_ok=True)
    for src in a.suite:
        renamed, dropped, held, renumbered, left = keep_file(src, os.path.join(out, os.path.basename(src)), meta)
        print(f"kept {os.path.basename(src)}: {renamed} packets renamed Replay, {dropped} others' actions dropped, "
              f'{held} updates held, {renumbered} character ids made {REFERENCE_CHARACTER}')
        if left:
            print(f'  warning: the recorded character id still stands {left} times, in fields keep does not know')


def main():
    # SIGTERM as an exit, through the finally blocks that stop the client and the server
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(128 + signal.SIGTERM))
    argv = sys.argv[1:]
    client_args = []
    if '--' in argv:
        i = argv.index('--')
        argv, client_args = argv[:i], argv[i + 1:]
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('what', choices=['run', 'play', 'shots', 'prof', 'record', 'keep'])
    ap.add_argument('suite', nargs='+', metavar='SUITE|SCENE',
                    help='a suite file; for record, the addon scenes to record (or all); for keep, recordings')
    ap.add_argument('--server', help='with record: your LandSandBoat server')
    ap.add_argument('--user', help='with record: a GM account on it')
    ap.add_argument('--password', default=os.environ.get('FFXI_PASSWORD'), help='with record: its password (or FFXI_PASSWORD)')
    ap.add_argument('--revive-container',
                    help="with record: the server's database container (Docker), to revive a K.O. character first")
    ap.add_argument('--play', help=f'with run, shots and prof: what to play, as !replay words (default all; prof: {PROF_SCENES})')
    ap.add_argument('--windows', type=int, default=3, help='with prof: two-second windows to average at each scene')
    ap.add_argument('--client', default=os.path.join(ROOT, 'build', 'host64'), help='host64')
    ap.add_argument('--game', default=os.environ.get('FFXI_GAME'), help='the FINAL FANTASY XI folder (or FFXI_GAME)')
    ap.add_argument('--data-dir', default=default_data_dir(), help="the client's data folder")
    ap.add_argument('--huffman', default=os.environ.get('FFXI_HUFFMAN'), help='folder with compress.dat')
    ap.add_argument('--control-port', type=int, default=54300,
                    help="host64's control port (FFXI_CONTROL); a WorldSim map server's xi_agents bridge also has 54300")
    ap.add_argument('--auth-port', type=int, default=replayserver.AUTH_PORT, help="the replay server's sign-in port")
    ap.add_argument('--data-port', type=int, default=replayserver.DATA_PORT, help="the replay server's lobby data port")
    ap.add_argument('--view-port', type=int, default=replayserver.VIEW_PORT, help="the replay server's lobby view port")
    ap.add_argument('--zone-port', type=int, default=replayserver.ZONE_PORT, help="the replay server's zone port")
    ap.add_argument('--out', help='where the results go (keep: default tools/replay/scenes/)')
    ap.add_argument('--lsb', help='with keep: the LandSandBoat commit the recordings were made on')
    ap.add_argument('--build', help='with keep: the game build they were made with, as meta/builds.json names it')
    a = ap.parse_args(argv)
    a.client_args = client_args
    if a.what == 'keep':
        if not (a.lsb and a.build):
            ap.error('keep: --lsb and --build (the LandSandBoat commit and game build they were recorded with)')
        return keep(a)
    if not a.game:
        ap.error('--game (or FFXI_GAME): the FINAL FANTASY XI folder')
    if a.what == 'record':
        if not (a.server and a.user):
            ap.error('record: --server and --user (a GM account on your LandSandBoat server)')
        a.scenes = a.suite
        return record(a)
    if len(a.suite) > 1:
        ap.error(f'{a.what}: one suite file')
    a.suite = a.suite[0]
    if not a.huffman:
        ap.error('--huffman (or FFXI_HUFFMAN): the folder holding compress.dat')
    if a.play is None:
        a.play = PROF_SCENES if a.what == 'prof' else 'all'
    if a.what == 'run':
        session(a, a.play)
    elif a.what == 'shots':
        session(a, a.play, on_event=Shots())
    elif a.what == 'prof':
        os.environ.setdefault('FFXI_FX', '1')  # the scene effects are what it times
        p = Prof(a.windows)
        out = session(a, a.play, on_event=p)
        with open(os.path.join(out, 'prof.json'), 'w') as f:
            json.dump({'fx': os.environ.get('FFXI_FX'), 'fx_file': os.environ.get('FFXI_FX_FILE'),
                       'client_args': a.client_args, 'scenes': p.scenes}, f, indent=2)
        text = prof_table(p.scenes)
        with open(os.path.join(out, 'prof.txt'), 'w') as f:
            f.write(text + '\n')
        print(text)
    else:
        session(a, '', wait_done=False)


if __name__ == '__main__':
    main()
