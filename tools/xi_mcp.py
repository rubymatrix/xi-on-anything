#!/usr/bin/env python3
"""An MCP server (stdio) that drives a running game through its control port.

The game listens when started with FFXI_CONTROL set (host/addons/lua/control.lua): FFXI_CONTROL=1
for 127.0.0.1:54300, or FFXI_CONTROL=<port>. Everything happens inside the game process: keys go
to the game without its window having focus, and screenshots are read from its frame.

  claude mcp add xi -- python3 /path/to/FFXIRecompile/tools/xi_mcp.py

Environment:
  XI_CONTROL_PORT   the game's control port (54300)
  XI_GAME           for game_launch: the game's executable (build/host64, or the app's
                    "Contents/MacOS/Final Fantasy XI")
  XI_GAME_ARGS      for game_launch: its arguments when the call gives none (shell-style)

Standard library only.
"""
import base64
import json
import os
import shlex
import socket
import struct
import subprocess
import sys
import threading
import time
import zlib

PORT = int(os.environ.get('XI_CONTROL_PORT', '54300'))
PROTOCOL = '2025-06-18'


def log(msg):
    print(f'[xi_mcp] {msg}', file=sys.stderr, flush=True)


# --- the game's control port --------------------------------------------------------------------

class Game:
    def __init__(self, port):
        self.port = port
        self.sock = None
        self.buf = b''
        self.next_id = 1
        self.lock = threading.Lock()

    def close(self):
        if self.sock:
            try:
                self.sock.close()
            except OSError:
                pass
        self.sock, self.buf = None, b''

    def connect(self):
        s = socket.create_connection(('127.0.0.1', self.port), timeout=5)
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.sock, self.buf = s, b''

    def call(self, cmd, args=None, timeout=30.0):
        with self.lock:
            for attempt in (0, 1):
                try:
                    if not self.sock:
                        self.connect()
                    rid = self.next_id
                    self.next_id += 1
                    self.sock.sendall((json.dumps({'id': rid, 'cmd': cmd, 'args': args or {}}) + '\n').encode())
                    deadline = time.monotonic() + timeout
                    while True:
                        while b'\n' in self.buf:
                            line, self.buf = self.buf.split(b'\n', 1)
                            if not line.strip():
                                continue
                            msg = json.loads(line)
                            if msg.get('id') != rid:
                                continue  # a late answer to a request that timed out
                            if not msg.get('ok'):
                                raise GameError(msg.get('error') or 'the game refused')
                            return msg.get('result')
                        left = deadline - time.monotonic()
                        if left <= 0:
                            raise GameError(f'{cmd}: no answer in {timeout:.0f} s')
                        self.sock.settimeout(left)
                        data = self.sock.recv(65536)
                        if not data:
                            raise ConnectionError('the game closed the control port')
                        self.buf += data
                except (ConnectionError, OSError) as e:
                    self.close()
                    if attempt:
                        raise GameError(f'cannot reach the game on 127.0.0.1:{self.port} ({e}); '
                                        'is it running with FFXI_CONTROL set?')


class GameError(Exception):
    pass


GAME = Game(PORT)
LAUNCHED = []


# --- frames to PNG --------------------------------------------------------------------------------

def frame_png(path, max_width):
    with open(path, 'rb') as f:
        data = f.read()
    magic, w, h, fmt = struct.unpack_from('<4I', data)
    if magic != 0x31464958:
        raise GameError('not a frame capture')
    px = memoryview(data)[16:]
    if fmt in (21, 22):  # A8R8G8B8, X8R8G8B8: B G R A in memory
        bpp = 4
    elif fmt == 23:  # R5G6B5
        bpp = 2
    else:
        raise GameError(f'frame format {fmt} is not handled')
    step = max(1, -(-w // max_width)) if max_width else 1
    ow, oh = len(range(0, w, step)), len(range(0, h, step))
    rows = []
    for y in range(0, h, step):
        row = px[y * w * bpp:(y + 1) * w * bpp]
        out = bytearray(ow * 3)
        if bpp == 4:
            src = bytes(row[0::step * 4]), bytes(row[1::step * 4]), bytes(row[2::step * 4])
            out[0::3], out[1::3], out[2::3] = src[2][:ow], src[1][:ow], src[0][:ow]
        else:
            for i, x in enumerate(range(0, w, step)):
                v = row[x * 2] | (row[x * 2 + 1] << 8)
                out[i * 3:i * 3 + 3] = bytes(((v >> 11) << 3, ((v >> 5) & 63) << 2, (v & 31) << 3))
        rows.append(b'\0' + bytes(out))

    def chunk(kind, body):
        return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body) & 0xFFFFFFFF)

    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', ow, oh, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(b''.join(rows), 6)) + chunk(b'IEND', b'')
    return png, ow, oh, w, h


# --- tools -------------------------------------------------------------------------------------

def text(v):
    return [{'type': 'text', 'text': v if isinstance(v, str) else json.dumps(v, ensure_ascii=False, indent=1)}]


def t_state(a):
    return text(GAME.call('state'))


def t_chat_send(a):
    GAME.call('chat_send', {'line': a['line']})
    if a.get('since') is None:
        return text('sent')
    time.sleep(float(a.get('wait_ms', 500)) / 1000)
    return text(GAME.call('chat_log', {'since': a['since'], 'limit': 20}))


def t_chat_log(a):
    return text(GAME.call('chat_log', {'since': a.get('since', 0), 'limit': a.get('limit', 50)}))


def t_chat_input(a):
    args = {'text': a['text']} if 'text' in a else {}
    return text(GAME.call('chat_input', args))


def t_keys(a):
    keys = a['keys']
    hold, gap = a.get('hold_ms', 80), a.get('gap_ms', 120)
    waits = sum(int(k[5:]) for k in keys if isinstance(k, str) and k.startswith('wait:'))
    timeout = 10 + (waits + len(keys) * (hold + gap + 50)) / 1000
    return text(GAME.call('keys', {'keys': keys, 'hold_ms': hold, 'gap_ms': gap}, timeout=timeout))


def t_key(a):
    return text(GAME.call('key', {'key': a['key'], 'down': a.get('down', True)}))


def t_release_keys(a):
    return text(GAME.call('release_keys'))


def t_screenshot(a):
    path = os.path.join(os.environ.get('TMPDIR', '/tmp'), f'xi_frame_{os.getpid()}.xif')
    r = GAME.call('capture', {'path': path}, timeout=15)
    png, ow, oh, w, h = frame_png(r['path'], int(a.get('max_width', 1280)))
    try:
        os.remove(r['path'])
    except OSError:
        pass
    out = [{'type': 'image', 'data': base64.b64encode(png).decode(), 'mimeType': 'image/png'}]
    if a.get('save_to'):
        with open(a['save_to'], 'wb') as f:
            f.write(png)
    out += text(f'frame {w}x{h}, shown at {ow}x{oh}')
    return out


def t_entities(a):
    return text(GAME.call('entities', {'radius': a.get('radius', 50), 'limit': a.get('limit', 40)}))


def t_target(a):
    return text(GAME.call('target', {'index': a['index']}))


def t_wait(a):
    ms = int(a.get('ms', 1000))
    return text(GAME.call('wait', {'ms': ms}, timeout=10 + ms / 1000))


def t_lua(a):
    return text(GAME.call('lua', {'code': a['code']}, timeout=float(a.get('timeout_s', 30))))


def t_launch(a):
    exe = a.get('executable') or os.environ.get('XI_GAME')
    if not exe:
        raise GameError('no executable: pass executable or set XI_GAME')
    args = a.get('args')
    if args is None:
        args = shlex.split(os.environ.get('XI_GAME_ARGS', ''))
    env = dict(os.environ)
    env['FFXI_CONTROL'] = str(PORT)
    env.update({k: str(v) for k, v in (a.get('env') or {}).items()})
    logf = open(os.path.join(os.environ.get('TMPDIR', '/tmp'), 'xi_game_stdout.log'), 'w')
    p = subprocess.Popen([exe] + list(args), env=env, stdout=logf, stderr=subprocess.STDOUT,
                         cwd=a.get('cwd') or os.path.dirname(os.path.abspath(exe)), start_new_session=True)
    LAUNCHED.append(p)
    GAME.close()
    deadline = time.monotonic() + float(a.get('timeout_s', 60))
    while time.monotonic() < deadline:
        if p.poll() is not None:
            raise GameError(f'the game exited at once (code {p.returncode}); see {logf.name}')
        try:
            st = GAME.call('state', timeout=5)
            return text({'pid': p.pid, 'state': st})
        except GameError:
            time.sleep(1)
    return text({'pid': p.pid, 'state': None, 'note': 'started, the control port has not answered yet'})


def t_quit(a):
    try:
        GAME.call('chat_send', {'line': '/shutdown'})
    except GameError as e:
        return text(f'not sent: {e}')
    return text('sent /shutdown')


KEYS_HELP = ('Key names: enter, escape, up, down, left, right, tab, space, backspace, f1-f12, '
             'a-z, 0-9, numpad0-numpad9, numpadenter, insert, delete, home, end, pageup, pagedown, '
             'lctrl, lshift, lalt; combine with + (ctrl+t, shift+tab). "wait:<ms>" pauses.')

TOOLS = [
    ('game_state', t_state, 'Sign-in state (login_status 2 = in a zone), player (name, HP/MP/TP, job, position: '
     'z is height), zone, target, the open menu\'s name (e.g. "loby2win", "menuwind"; empty when none) and the '
     'chat input text.', {}),
    ('game_screenshot', t_screenshot, 'The game frame as it is now, as an image (works with the window in the '
     'background). Use it to see dialogs, menus and the lobby.',
     {'max_width': {'type': 'integer', 'description': 'shrink to at most this many pixels wide (default 1280)'},
      'save_to': {'type': 'string', 'description': 'also write the PNG here'}}),
    ('game_keys', t_keys, 'Press keys in the game, in order (dialogs, menus, the lobby, movement). ' + KEYS_HELP,
     {'keys': {'type': 'array', 'items': {'type': 'string'}},
      'hold_ms': {'type': 'integer', 'description': 'how long each key is held (default 80)'},
      'gap_ms': {'type': 'integer', 'description': 'pause after each key (default 120)'}}, ['keys']),
    ('game_key', t_key, 'Hold a key down (down=true) or let it up (down=false), e.g. to walk. ' + KEYS_HELP,
     {'key': {'type': 'string'}, 'down': {'type': 'boolean'}}, ['key']),
    ('game_release_keys', t_release_keys, 'Let up every key the tools are holding.', {}),
    ('game_chat_send', t_chat_send, 'Send a chat line as if typed and entered: /commands (/say, /tell, /echo, '
     '/target, /ma, /shutdown...) or //addon commands; a line without a slash is said (/say). The game '
     'refuses chat lines sent less than about a second apart ("A command error occurred").',
     {'line': {'type': 'string'},
      'since': {'type': 'integer', 'description': 'if given, return chat log lines after this sequence number'},
      'wait_ms': {'type': 'integer', 'description': 'wait before reading the log (default 500)'}}, ['line']),
    ('game_chat_log', t_chat_log, 'Chat lines (UTF-8) after a sequence number: what the server sends as chat '
     '(say, tells, system messages; mode = chat kind, sender) and the addon host\'s lines. NPC dialog and battle '
     'messages are not in it: use game_screenshot. "last" is the newest sequence number.',
     {'since': {'type': 'integer'}, 'limit': {'type': 'integer'}}),
    ('game_chat_input', t_chat_input, 'Read the chat input line, or replace its text.', {'text': {'type': 'string'}}),
    ('game_entities', t_entities, 'Entities (players, NPCs, monsters) near the player, nearest first.',
     {'radius': {'type': 'number'}, 'limit': {'type': 'integer'}}),
    ('game_target', t_target, 'Target an entity by its index (from game_entities).', {'index': {'type': 'integer'}}, ['index']),
    ('game_wait', t_wait, 'Wait in game time (frames keep drawing).', {'ms': {'type': 'integer'}}),
    ('game_lua', t_lua, 'Run Lua inside the game (the addon host\'s xi.* API: xi.game, xi.memory, xi.chat, '
     'xi.packets, xi.res...); returns what the code returns, as JSON. coroutine.sleep(s) works.',
     {'code': {'type': 'string'}, 'timeout_s': {'type': 'number'}}, ['code']),
    ('game_launch', t_launch, 'Start the game with its control port on (XI_GAME / XI_GAME_ARGS, or the given '
     'executable and arguments) and wait until it answers.',
     {'executable': {'type': 'string'}, 'args': {'type': 'array', 'items': {'type': 'string'}},
      'env': {'type': 'object', 'additionalProperties': {'type': 'string'}}, 'cwd': {'type': 'string'},
      'timeout_s': {'type': 'number'}}),
    ('game_quit', t_quit, 'Shut the game down the game\'s own way (/shutdown).', {}),
]
BY_NAME = {t[0]: t for t in TOOLS}


def tool_list():
    out = []
    for t in TOOLS:
        name, _, desc, props = t[:4]
        schema = {'type': 'object', 'properties': props}
        if len(t) > 4:
            schema['required'] = t[4]
        out.append({'name': name, 'description': desc, 'inputSchema': schema})
    return out


# --- MCP over stdio ------------------------------------------------------------------------------

def reply(rid, result=None, error=None):
    msg = {'jsonrpc': '2.0', 'id': rid}
    if error:
        msg['error'] = error
    else:
        msg['result'] = result
    sys.stdout.write(json.dumps(msg) + '\n')
    sys.stdout.flush()


def handle(msg):
    method, rid, params = msg.get('method'), msg.get('id'), msg.get('params') or {}
    if rid is None:
        return  # notifications: initialized, cancelled
    if method == 'initialize':
        reply(rid, {'protocolVersion': params.get('protocolVersion', PROTOCOL),
                    'capabilities': {'tools': {}},
                    'serverInfo': {'name': 'xi-game', 'version': '0.1.0'},
                    'instructions': 'Drives a running FINAL FANTASY XI client (FFXIRecompile) through its control '
                                    'port. Look with game_screenshot and game_state; act with game_keys, '
                                    'game_chat_send and game_lua.'})
    elif method == 'ping':
        reply(rid, {})
    elif method == 'tools/list':
        reply(rid, {'tools': tool_list()})
    elif method == 'tools/call':
        t = BY_NAME.get(params.get('name'))
        if not t:
            reply(rid, error={'code': -32602, 'message': f'no tool {params.get("name")}'})
            return
        try:
            reply(rid, {'content': t[1](params.get('arguments') or {})})
        except (GameError, KeyError, ValueError) as e:
            reply(rid, {'content': text(f'{type(e).__name__ if isinstance(e, KeyError) else "error"}: {e}'), 'isError': True})
    else:
        reply(rid, error={'code': -32601, 'message': f'no method {method}'})


def main():
    for line in sys.stdin:
        if not line.strip():
            continue
        try:
            msg = json.loads(line)
        except ValueError:
            continue
        try:
            handle(msg)
        except Exception as e:  # keep serving
            log(f'{type(e).__name__}: {e}')
            if msg.get('id') is not None:
                reply(msg['id'], error={'code': -32603, 'message': str(e)})


if __name__ == '__main__':
    main()
