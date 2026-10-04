#!/usr/bin/env python3
"""A replay server for repeatable performance tests: recorded zone visits, played the same every time.

It is tools/staticserver.py's server (any sign-in, one character, the lobby and zone protocol) with
recorded scenes in place of its empty zones: the NPCs, mobs, players, weather, time of day and
effects of a zone visit the xireplay addon recorded on a server you run (tools/replay/xireplay), played
back on their recorded timing. Nothing depends on a live server's state.

  python3 tools/replayserver.py --huffman <dir with compress.dat> --suite tools/replay/default.txt
  python3 tools/replayserver.py --huffman <dir> --scene captures/markets-1.jsonl [scene flags]

then start the game against it:

  build/host64 ... --server 127.0.0.1 --user anything --pass x --authport 55231 --dataport 55230 --viewport 55001

A session starts in the suite's home scene (one marked --home: GM Home, say) and waits there. Ask for
scenes in chat (the "!" is optional):

  !replay list          the scenes, numbered, with their groups
  !replay 3             scene 3                !replay rain       by name
  !replay weather       a group                !replay all        every scene
  !replay rain ice 2    several, in turn       !replay stop       home after this scene

Each scene is reached by a zone change to this server and the next zone-in; the session goes home
after the last one asked for. --autoplay queues scenes at the first zone-in, for unattended runs. A
suite without a home scene plays its scenes in order.

A suite file has one scene per line: a recording, then its scene flags (--label, --group, --home,
--chat); blank lines and # comments are skipped, and paths are relative to the suite's folder.

Events travel in-band as chat lines from "xireplay" ("begin|3|rain|weather|109",
"mark|3|rain|start", "end|3|rain", "done|3", "home", "queue|rain,ice", "list|3|rain|weather",
"info|...", "error|..."): the xireplay addon hides them and shows each as a line of its own; a client
without it shows them as chat. Recorded chat and other text is dropped from scenes unless --chat.

Ports default to 55231 auth, 55230 data, 55001 lobby and 55232 zone, so it can run beside a
LandSandBoat server. --huffman as for staticserver.py.
"""
import argparse
import os
import shlex
import socket
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import staticserver as ss  # noqa: E402
from replayscene import Scene  # noqa: E402

HOME = -1   # the home scene's index
TAIL = 2.0  # seconds after a scene's end before the zone change
MAX_MESSAGES = 32                 # in one packet, at most
AUTH_PORT, DATA_PORT, VIEW_PORT, ZONE_PORT = 55231, 55230, 55001, 55232  # beside a LandSandBoat server's


def event(text):
    """A chat line (0x017) from "xireplay": an event for the addon."""
    msg = text.encode('ascii', 'replace')[:120]
    return ss.sub(0x017, struct.pack('<BBH', 6, 0, 0) + ss.cstr('xireplay', 15) + msg + b'\0')


# --- scenes and suites -------------------------------------------------------------------------

class FlagParser(argparse.ArgumentParser):
    """Scene flags' parser: a mistake is an error in the suite's line, not the end of the program."""

    def error(self, message):
        raise ValueError(message)


def scene_flags():
    ap = FlagParser(prog='scene', add_help=False)
    ap.add_argument('--label', help="the scene's name in events and !replay (default: the file's)")
    ap.add_argument('--group', default='', help='a group !replay can ask for it by')
    ap.add_argument('--home', action='store_true', help='the home scene, where a session waits')
    ap.add_argument('--chat', action='store_true', help='keep the recorded chat and other text')
    return ap


def load_scene(words, name, base):
    """A recording (words[0]) with its scene flags (words[1:])."""
    path = words[0] if os.path.isabs(words[0]) else os.path.join(base, words[0])
    a = scene_flags().parse_args(words[1:])
    s = Scene.load(path)
    s.label = a.label or os.path.splitext(os.path.basename(path))[0]
    s.group, s.home = a.group, a.home
    s.rename(name)
    notes = []
    if not a.chat:
        notes.append(f'{s.quiet()} text packets dropped')
    ss.log('scene', f'{s.label}: {len(s.packets)} packets over {s.duration():.1f} s' +
           (f' ({", ".join(notes)})' if notes else ''))
    return s


def load_suite(path, name):
    base = os.path.dirname(os.path.abspath(path))
    scenes = []
    with open(path, encoding='utf-8') as f:
        for n, line in enumerate(f, 1):
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            try:
                scenes.append(load_scene(shlex.split(line), name, base))
            except (OSError, ValueError) as e:
                raise SystemExit(f'{path}:{n}: {e}') from None
    return scenes


def resolve(scenes, words):
    """Scene indices for request words (numbers, names, groups, all; any case), and the words that match
    none."""
    found, unknown = [], []
    for w in (w.lower() for w in words):
        hit = False
        num = w.lstrip('#')
        if num.isdigit() and 1 <= int(num) <= len(scenes):
            found.append(int(num) - 1)
            hit = True
        for i, sc in enumerate(scenes):
            if w == 'all' or w == sc.label.lower() or (sc.group and w == sc.group.lower()):
                found.append(i)
                hit = True
        if not hit:
            unknown.append(w)
    return found, unknown


# --- the server --------------------------------------------------------------------------------

class Character:
    """What outlives a zone change: what the next zone-in plays and what is queued. Without a home scene
    the session starts with the scenes --autoplay names, else every scene, in order."""

    def __init__(self, srv):
        order = [] if srv.home else srv.resolve(srv.autoplay)[0] if srv.autoplay else list(range(len(srv.scenes)))
        self.target = HOME if srv.home else order[0]
        self.queue = order[1:]
        self.batch = 0  # scenes played since the queue was last empty
        self.started = False


class ReplayServer(ss.Server):
    def __init__(self, args, scenes, home, huff):
        first = home or scenes[0]
        char = dict(ss.DEFAULT_CHAR, name=args.name, char_id=first.character or 1, zone=first.zone,
                    server_name='Replay')
        super().__init__(args, char, ss.Zones(None), huff)
        self.scenes, self.home = scenes, home
        self.autoplay = args.autoplay.split() if args.autoplay else []
        self.character = Character(self)

    def scene(self, i):
        return self.home if i == HOME else self.scenes[i]

    def resolve(self, words):
        return resolve(self.scenes, words)


class ReplaySession(ss.ZoneSession):
    """A zone visit that plays a scene: each packet the client is sent carries what is due by now."""

    def __init__(self, srv, addr):
        super().__init__(srv, addr)
        self.playing = None  # the scene index, HOME, or None before the first zone-in
        self.events = []     # event chat lines, sent ahead of the scene's packets
        self.start = 0.0     # when the scene began (time.monotonic())
        self.next = 0        # the scene's next packet
        self.mark = 0        # the scene's next marker
        # playing; idle (home, played through: waiting for a request); ended (the zone change waits for
        # what is queued to go); zoning; finished (no home: everything asked for has played)
        self.state = None

    def on_login(self):
        # a zone-in: the scene the character's target names, from its start
        srv = self.srv
        c = srv.character
        self.playing, self.start, self.next, self.mark = c.target, time.monotonic(), 0, 0
        self.state = 'playing'
        self.logged_in, self.zoning = True, False
        sc = srv.scene(self.playing)
        if self.playing == HOME:
            ss.log('replay', 'home')
            self.events.append(event('home'))
            if not c.started and srv.autoplay:
                self.request(srv.autoplay)
        else:
            ss.log('replay', f'scene {self.playing + 1} {sc.label}')
            self.events.append(event(f'begin|{self.playing + 1}|{sc.label}|{sc.group}|{sc.zone}'))
        c.started = True

    def after_zone_in(self):
        pass  # the scene has what the zone sends after the client's 0x00C

    def handle(self, t, p):
        if t == 0x0B5:  # chat: a !replay request, else nothing
            text = bytes(p[6:]).split(b'\0')[0].decode('ascii', 'replace').strip().lower().split()
            if text and text[0].lstrip('!') == 'replay':
                self.command(text[1:])
        elif t == 0x0E7:  # /logout, /shutdown
            super().handle(t, p)

    def command(self, words):
        srv = self.srv
        ss.log('replay', 'replay ' + ' '.join(words))
        if not words or words[0] in ('list', 'help'):
            for i, sc in enumerate(srv.scenes):
                self.events.append(event(f'list|{i + 1}|{sc.label}|{sc.group}'))
            self.events.append(event('info|!replay <#|name|group|all> ... to play, !replay stop to go home'))
        elif words[0] == 'stop':
            srv.character.queue.clear()
            self.events.append(event('info|queue cleared: home after this scene'))
        else:
            self.request(words)

    def request(self, words):
        srv = self.srv
        found, unknown = srv.resolve(words)
        for w in unknown:
            self.events.append(event(f'error|no scene or group "{w}" (!replay list)'))
        if found:
            c = srv.character
            c.queue += found
            self.events.append(event('queue|' + ','.join(srv.scenes[i].label for i in c.queue)))

    def due(self):
        """Move what is due into the queue: events first, then the scene's packets; or, once the scene
        is over and everything has gone, the zone change to the next one."""
        if self.playing is None:
            return
        srv = self.srv
        sc = srv.scene(self.playing)
        c = srv.character
        now = time.monotonic() - self.start
        while self.mark < len(sc.marks) and sc.marks[self.mark][0] <= now:
            if self.playing != HOME:
                self.events.append(event(f'mark|{self.playing + 1}|{sc.label}|{sc.marks[self.mark][1]}'))
            self.mark += 1
        over = self.next == len(sc.packets) and now >= sc.duration()
        if self.state == 'playing' and over and self.playing == HOME:
            self.state = 'idle'
        elif self.state == 'playing' and over and now >= sc.duration() + TAIL:
            ss.log('replay', f'scene {self.playing + 1} {sc.label} played in {now:.1f} s')
            self.events.append(event(f'end|{self.playing + 1}|{sc.label}'))
            c.batch += 1
            self.state = 'ended'
            if not c.queue:
                self.events.append(event(f'done|{c.batch}'))
                c.batch = 0
                ss.log('replay', 'all requested scenes played')
                if srv.home is None:
                    self.state = 'finished'
        self.queue += self.events
        self.events = []
        if self.state == 'playing':
            while self.next < len(sc.packets) and sc.packets[self.next][0] <= now:
                self.queue.append(ss.Msg(sc.packets[self.next][1]))
                self.next += 1
        # a request waiting, or a scene over: the zone change, alone, once the rest has gone
        if not self.queue and (self.state == 'ended' or (self.state in ('idle', 'finished') and c.queue)):
            c.target = c.queue.pop(0) if c.queue else HOME
            self.zone = srv.scene(c.target).zone
            self.state = 'zoning'
            self.zoning = True
            self.queue.append(self.logout_packet(2, zone_change=True))

    def build(self, client_hdr, next_id=True):
        self.due()
        return super().build(client_hdr, next_id)


ReplayServer.session_class = ReplaySession


# --- start -------------------------------------------------------------------------------------

def main():
    argv = sys.argv[1:]
    scene_words = []
    if '--scene' in argv:  # everything after --scene's recording is that scene's flags
        i = argv.index('--scene')
        scene_words, argv = argv[i + 1:], argv[:i]
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--huffman', default=os.environ.get('FFXI_HUFFMAN'), help='folder with compress.dat')
    ap.add_argument('--suite', help='a suite file: one scene per line, a recording and its scene flags')
    ap.add_argument('--scene', help='one recording to play (the rest of the command line is its scene flags)')
    ap.add_argument('--name', default='Replay', help="the character's name in the lobby and the scenes")
    ap.add_argument('--autoplay', default='', help='queue these at the first zone-in, as "!replay <these>" would')
    ap.add_argument('--info', action='store_true', help='describe the scenes and exit')
    ap.add_argument('--bind', default='127.0.0.1')
    ap.add_argument('--public-ip', default='127.0.0.1', help='the address the client reaches this server at')
    ap.add_argument('--session-hash', default='5245504c4159534553534930000000a1', help='32 hex digits')
    ap.add_argument('--auth-port', type=int, default=AUTH_PORT)
    ap.add_argument('--data-port', type=int, default=DATA_PORT)
    ap.add_argument('--view-port', type=int, default=VIEW_PORT)
    ap.add_argument('--zone-port', type=int, default=ZONE_PORT)
    args = ap.parse_args(argv)
    if scene_words:
        args.scene = scene_words[0]
    try:
        if len(bytes.fromhex(args.session_hash)) != 16:
            raise ValueError
    except ValueError:
        ap.error('--session-hash: 32 hex digits')

    if args.suite:
        scenes = load_suite(args.suite, args.name)
    elif args.scene:
        try:
            scenes = [load_scene(scene_words, args.name, os.getcwd())]
        except (OSError, ValueError) as e:
            ap.error(str(e))
    else:
        ap.error('--suite or --scene')
    home = next((s for s in scenes if s.home), None)
    scenes = [s for s in scenes if not s.home]
    if not scenes:
        ap.error('no scenes to play')
    if args.autoplay:
        unknown = resolve(scenes, args.autoplay.split())[1]
        if unknown:
            ap.error(f'--autoplay: no scene or group {", ".join(unknown)}')
    if args.info:
        numbered = [('home', home)] if home else []
        for n, s in numbered + [(f'#{i + 1}', s) for i, s in enumerate(scenes)]:
            print(f'{n} {s.label} [{s.group}] {s.path}: zone {s.zone}, character {s.name()!r} ({s.character}), '
                  f'{len(s.packets)} packets over {s.duration():.1f} s')
            for at, label in s.marks:
                print(f'    mark {label!r} +{at:.1f} s')
        return
    chars = {s.character for s in scenes + ([home] if home else [])}
    if len(chars) > 1:
        ap.error(f'the scenes were recorded for different characters ({sorted(chars)}): a suite needs one')
    if not args.huffman:
        ap.error('--huffman (or FFXI_HUFFMAN): the folder holding compress.dat')

    srv = ReplayServer(args, scenes, home, ss.Huffman(args.huffman))
    ctx = ss.tls_context()
    if ctx:
        ss.tcp_listener(args.bind, args.auth_port, srv.serve_auth, ctx)
    ss.tcp_listener(args.bind, args.data_port, srv.serve_data)
    ss.tcp_listener(args.bind, args.view_port, srv.serve_view)
    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp.bind((args.bind, args.zone_port))
    ss.log('server', f'{len(scenes)} scenes{", home " + home.label if home else ""}; '
                     f'FFXI_LSB_SESSION={srv.char["account_id"]}:{args.session_hash}')
    try:
        srv.serve_zone(udp)
    except KeyboardInterrupt:
        pass


if __name__ == '__main__':
    main()
