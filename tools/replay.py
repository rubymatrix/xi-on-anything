#!/usr/bin/env python3
"""Runs recorded scenes with nobody at the screen: tools/replayserver.py, host64 driven through its
control port (FFXI_CONTROL: the lobby's keys, chat commands, frame captures), the xireplay addon's
frame log, and tools/replayreport.py's report.

  python3 tools/replay.py run tools/replay/default.txt          every scene, then a report
  python3 tools/replay.py run suite.txt --play "weather crowd"  some
  python3 tools/replay.py play suite.txt                        a session to watch: !replay in chat
  python3 tools/replay.py shots suite.txt                       a frame capture at each scene's READY
  python3 tools/replay.py record all --server <yours> --user <a GM>   the addon's scenes, recorded

Results go to --out (default generated/runs/<date>-<suite>/): server.log, client.log, frames.csv,
report.txt, report.json, and for shots the captures. The addon (tools/replay/xireplay) is copied into
the data folder's ashita/addons and loaded through the control port; the game's settings are its own.
--huffman (or FFXI_HUFFMAN) as for replayserver.py; arguments after "--" go to host64.
"""
import argparse
import glob
import json
import os
import re
import shutil
import socket
import struct
import subprocess
import sys
import time
import zlib

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)
import replayreport  # noqa: E402
import replayserver  # noqa: E402

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
            try:
                s.bind(('127.0.0.1', port))
                return
            except OSError:
                pass
        if time.time() > deadline:
            raise SystemExit(f'the control port {port} is taken: is another game still running?')
        time.sleep(1)


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
                                   '--suite', a.suite, '--autoplay', autoplay], stdout=server_log, stderr=subprocess.STDOUT)
    client = ctl = None
    try:
        time.sleep(1.5)
        if server.poll() is not None:
            raise SystemExit(f'the server stopped; see {out}/server.log')
        client, ctl = launch(a, data, os.path.join(out, 'client.log'),
                             ['--server', '127.0.0.1', '--user', 'replay', '--pass', 'replay',
                              '--authport', str(replayserver.AUTH_PORT), '--dataport', str(replayserver.DATA_PORT),
                              '--viewport', str(replayserver.VIEW_PORT)])
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
            path = os.path.join(os.path.abspath(out), f'{int(f[1]):02d}-{f[2]}')
            time.sleep(SETTLE)
            ctl('capture', path=path + '.raw')
            for _ in range(20):  # written on a later frame
                if os.path.exists(path + '.raw') and os.path.getsize(path + '.raw') > 16:
                    break
                time.sleep(0.25)
            else:
                print(f'  no capture for {f[2]}', flush=True)
                return
            time.sleep(0.5)
            if png(path + '.raw', path + '.png'):
                os.remove(path + '.raw')
                print(f'  captured {path}.png', flush=True)
            else:
                print(f'  captured {path}.raw (not a format this converts)', flush=True)


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


def main():
    argv = sys.argv[1:]
    client_args = []
    if '--' in argv:
        i = argv.index('--')
        argv, client_args = argv[:i], argv[i + 1:]
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('what', choices=['run', 'play', 'shots', 'record'])
    ap.add_argument('suite', nargs='+', metavar='SUITE|SCENE',
                    help='a suite file; for record, the addon scenes to record (or all)')
    ap.add_argument('--server', help='with record: your LandSandBoat server')
    ap.add_argument('--user', help='with record: a GM account on it')
    ap.add_argument('--password', default=os.environ.get('FFXI_PASSWORD'), help='with record: its password (or FFXI_PASSWORD)')
    ap.add_argument('--revive-container',
                    help="with record: the server's database container (Docker), to revive a K.O. character first")
    ap.add_argument('--play', default='all', help='with run and shots: what to play, as !replay words')
    ap.add_argument('--client', default=os.path.join(ROOT, 'build', 'host64'), help='host64')
    ap.add_argument('--game', default=os.environ.get('FFXI_GAME'), help='the FINAL FANTASY XI folder (or FFXI_GAME)')
    ap.add_argument('--data-dir', default=default_data_dir(), help="the client's data folder")
    ap.add_argument('--huffman', default=os.environ.get('FFXI_HUFFMAN'), help='folder with compress.dat')
    ap.add_argument('--control-port', type=int, default=54300)
    ap.add_argument('--out', help='where the results go')
    a = ap.parse_args(argv)
    a.client_args = client_args
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
    if a.what == 'run':
        session(a, a.play)
    elif a.what == 'shots':
        session(a, a.play, on_event=Shots())
    else:
        session(a, '', wait_done=False)


if __name__ == '__main__':
    main()
