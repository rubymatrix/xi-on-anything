#!/usr/bin/env python3
"""A tiny FFXI server with fixed data: sign in, pick the one character, be in a zone.

It is for performance testing. There is no database, no NPCs, no mobs and no game logic: one
account with a fixed session hash, one simulated character, and enough of the lobby and the zone
protocol to log in, stand in a zone and change zones.

  python3 tools/staticserver.py --huffman <dir with compress.dat> [--zone 246] [--name Tester]
                                [--config char.json] [--sql <dir with zonelines.sql, zone_settings.sql>]

then start the game against it:

  FFXI_LSB_SESSION=1:<hash> build/host64 ... --server 127.0.0.1      (no auth exchange), or
  build/host64 ... --server 127.0.0.1 --user anything                 (any name and password work)

Ports (a LandSandBoat server's): 54231 auth (TLS, needs the openssl command for its certificate),
54230 login data (TCP), 54001 lobby (TCP), 54230 zone (UDP).

In the zone, chat commands (say them, e.g. "!zone 245"):
  !zone <id> [x y z [rot]]   change zone (to a known position for the zone, or the one given)
  !pos <x> <y> <z> [rot]     move within the zone (re-enters the zone at that position)
  !where                     print the zone and position
FFXI_STATIC_DEBUG=1 logs every zone packet the client sends.
Zone lines work when --sql points at a LandSandBoat sql/ folder (zonelines.sql); its
zone_settings.sql adds each zone's music.

The zone protocol's compression is a fixed Huffman code that the client builds in memory. It is
not shipped here: --huffman (or FFXI_HUFFMAN) names a folder holding it as compress.dat, the 2048
byte table every LandSandBoat install has in res/.

Protocol layouts follow atom0s's XiPackets documentation and the LandSandBoat server.
"""
import argparse
import hashlib
import json
import math
import os
import re
import socket
import ssl
import struct
import subprocess
import sys
import threading
import time

LOG_LOCK = threading.Lock()
DEBUG = bool(os.environ.get('FFXI_STATIC_DEBUG'))


def log(tag, msg):
    with LOG_LOCK:
        print(f'{time.strftime("%H:%M:%S")} [{tag}] {msg}', flush=True)


def md5(b):
    return hashlib.md5(b).digest()


# --- the character ------------------------------------------------------------------------------

# Where a character can stand in a few zones (x, y = height, z, rotation 0-255), for !zone without a
# position when there is no zonelines.sql to find one.
KNOWN_POSITIONS = {
    50: (-11.0, 2.0, -142.0, 192),   # Aht Urhgan Whitegate
    100: (-126.0, -62.0, 273.0, 99),  # West Ronfaure
    231: (0.0, 0.0, -13.0, 192),     # Northern San d'Oria
    232: (-1.0, 0.0, 44.0, 128),     # Port San d'Oria
    236: (-36.0, 7.0, -58.0, 194),   # Port Bastok
    246: (-87.0, 12.0, 116.0, 128),  # Port Jeuno
}

DEFAULT_CHAR = {
    'name': 'Tester',
    'char_id': 0x10001,     # also the content id
    'account_id': 1,
    'race': 1,              # 1 Hume male .. 8 Galka
    'face': 1,
    'size': 0,              # 0 small, 1 medium, 2 large
    'nation': 0,            # 0 San d'Oria, 1 Bastok, 2 Windurst
    'main_job': 1,          # 1 WAR
    'main_level': 99,
    'sub_job': 0,
    'sub_level': 0,
    'hp': 1500, 'mp': 0,
    'zone': 246,
    'pos': None,            # [x, y, z, rot]; None: a known position for the zone
    # equipment model ids (the game's look ids, not item ids)
    'head': 0, 'body': 8, 'hands': 8, 'legs': 8, 'feet': 8, 'main': 0, 'sub': 0, 'ranged': 0,
    'speed': 50,            # 40 normal walk speed, 50 a little faster
    'server_name': 'Static',
}


class Zones:
    """What --sql gives: zone lines and zone music, from a LandSandBoat sql/ folder."""

    def __init__(self, sql_dir):
        self.lines = {}  # zone line id -> (to_zone, x, y, z, rot)
        self.music = {}  # zone -> (day, night, solo, party)
        self.names = {}
        if not sql_dir:
            return
        num = r'(-?[\d.]+)'
        try:
            with open(os.path.join(sql_dir, 'zonelines.sql'), encoding='utf-8', errors='replace') as f:
                pat = re.compile(r'INSERT INTO `zonelines` VALUES \(' + ','.join([num] * 12) + r'\)')
                for m in pat.finditer(f.read()):
                    v = [float(x) for x in m.groups()]
                    self.lines[int(v[0])] = (int(v[5]), v[6], v[7], v[8], int(v[11] / (2 * math.pi) * 256) & 255)
        except OSError as e:
            log('zone', f'no zone lines: {e}')
        try:
            with open(os.path.join(sql_dir, 'zone_settings.sql'), encoding='utf-8', errors='replace') as f:
                pat = re.compile(r"INSERT INTO `zone_settings` VALUES \((\d+),\d+,'[^']*',\d+,'([^']*)',(\d+),(\d+),(\d+),(\d+)")
                for m in pat.finditer(f.read()):
                    z = int(m.group(1))
                    self.names[z] = m.group(2)
                    self.music[z] = tuple(int(x) for x in m.groups()[2:6])
        except OSError as e:
            log('zone', f'no zone settings: {e}')
        log('zone', f'{len(self.lines)} zone lines, {len(self.music)} zones from {sql_dir}')

    def position(self, zone):
        if zone in KNOWN_POSITIONS:
            return KNOWN_POSITIONS[zone]
        for to_zone, x, y, z, rot in self.lines.values():
            if to_zone == zone:
                return (x, y, z, rot)
        return (0.0, 0.0, 0.0, 0)

    def name(self, zone):
        return self.names.get(zone, f'zone {zone}')


# --- the zone protocol's ciphers ----------------------------------------------------------------

def _pi_hex_words(count):
    """The first count 32-bit words of pi's fraction: Blowfish's initial P-array and S-boxes."""
    bits = 32 * count + 64
    one = 1 << bits

    def atan_inv(x):  # atan(1/x) * one
        total, term, x2, n, sign = 0, one // x, x * x, 1, 1
        while term:
            total += sign * (term // n)
            term //= x2
            n += 2
            sign = -sign
        return total

    pi = 16 * atan_inv(5) - 4 * atan_inv(239)
    frac = (pi - 3 * one) >> 64
    return [(frac >> (32 * (count - 1 - i))) & 0xFFFFFFFF for i in range(count)]


_PI = _pi_hex_words(18 + 1024)
M32 = 0xFFFFFFFF


class Blowfish:
    """The client's Blowfish. Its round function differs from the standard one: the second and
    fourth S-box lookups add only their lowest bit (plus 32), and nothing is XORed."""

    def __init__(self, key16):
        P = list(_PI[:18])
        S = list(_PI[18:])
        self.P, self.S = P, S
        j = 0
        for i in range(18):
            d = 0
            for _ in range(4):
                b = key16[j]
                if b >= 0x80:  # the key is signed chars: sign extension spills into the word
                    b |= 0xFFFFFF00
                d = ((d << 8) | b) & M32
                j = (j + 1) % len(key16)
            P[i] ^= d
        l = r = 0
        for i in range(0, 18, 2):
            l, r = self.encipher(l, r)
            P[i], P[i + 1] = l, r
        for i in range(0, 1024, 2):
            l, r = self.encipher(l, r)
            S[i], S[i + 1] = l, r

    def _f(self, x):
        S = self.S
        return ((((S[256 + ((x >> 8) & 255)] & 1) ^ 32) + ((S[768 + (x >> 24)] & 1) ^ 32)
                 + S[512 + ((x >> 16) & 255)] + S[x & 255]) & M32)

    def encipher(self, l, r):
        P, f = self.P, self._f
        for i in range(16):
            l ^= P[i]
            r ^= f(l)
            l, r = r, l
        l, r = r, l
        return l ^ P[17], r ^ P[16]

    def decipher(self, l, r):
        P, f = self.P, self._f
        for i in range(17, 1, -1):
            l ^= P[i]
            r ^= f(l)
            l, r = r, l
        l, r = r, l
        return l ^ P[0], r ^ P[1]

    def run(self, buf, start, words, enc):
        """ECB over words (even) 32-bit words of buf from start, in place"""
        op = self.encipher if enc else self.decipher
        for o in range(start, start + words * 4, 8):
            l, r = struct.unpack_from('<II', buf, o)
            struct.pack_into('<II', buf, o, *op(l, r))


def zone_cipher(key20):
    """The Blowfish for a 20-byte session key: MD5 of it, cut at its first zero byte."""
    h = bytearray(md5(bytes(key20)))
    if 0 in h:
        z = h.index(0)
        h[z:] = bytes(16 - z)
    return Blowfish(bytes(h))


class Huffman:
    """The zone protocol's compression: one fixed prefix code per byte, bits LSB first. Encoded data
    is a 1 byte, then the bit stream; its length is the stream's bits + 8."""

    def __init__(self, folder):
        path = os.path.join(folder, 'compress.dat')
        with open(path, 'rb') as f:
            t = f.read()
        if len(t) != 2048:
            raise ValueError(f'{path}: {len(t)} bytes, not 2048')
        w = struct.unpack('<512I', t)
        # the table is indexed by the byte as a signed char + 0x80: codes, then lengths at + 0x100
        self.code = [w[(b + 128) & 255] for b in range(256)]
        self.bits = [w[256 + ((b + 128) & 255)] for b in range(256)]
        self.decode_map = {(self.bits[b], self.code[b] & ((1 << self.bits[b]) - 1)): b for b in range(256)}

    def compress(self, data):
        acc = n = 0
        out = bytearray([1])
        total = 0
        for b in data:
            k = self.bits[b]
            acc |= (self.code[b] & ((1 << k) - 1)) << n
            n += k
            total += k
            while n >= 8:
                out.append(acc & 255)
                acc >>= 8
                n -= 8
        if n:
            out.append(acc & 255)
        return bytes(out), total + 8

    def decompress(self, data, nbits):
        if not data or data[0] != 1:
            return None
        out = bytearray()
        code = length = 0
        limit = min(nbits - 8, (len(data) - 1) * 8)
        for i in range(max(limit, 0)):
            code |= ((data[1 + (i >> 3)] >> (i & 7)) & 1) << length
            length += 1
            b = self.decode_map.get((length, code))
            if b is not None:
                out.append(b)
                code = length = 0
            elif length > 24:
                break
        return bytes(out)


# --- lobby (TCP) --------------------------------------------------------------------------------

IXFF = b'IXFF'
VIEW_KEY = 0xAD5DE04F


def lobby_packet(command, body, size=None):
    """A lobby reply: size, "IXFF", command, MD5 of the whole packet (taken with it zeroed), body."""
    n = size if size is not None else 28 + len(body)
    assert 28 + len(body) <= n, f'lobby 0x{command:02X}: {len(body)} bytes of body in {n}'
    p = bytearray(n)
    struct.pack_into('<I4sI', p, 0, n, IXFF, command)
    p[28:28 + len(body)] = body
    p[12:28] = md5(bytes(p))
    return bytes(p)


def cstr(s, n):
    b = s.encode('ascii', 'replace')[:n - 1]
    return b + bytes(n - len(b))


class Server:
    def __init__(self, args, char, zones, huff):
        self.args = args
        self.char = char
        self.zones = zones
        self.huff = huff
        self.host_ip = args.public_ip
        self.hash = bytes.fromhex(args.session_hash)
        # The key the client's zone cipher starts from: 16 bytes of the account service's session
        # value (zero without one), then the view key with the client's own adjustment (+9).
        # A login data connection that sends 0xA2 replaces it with what the client said.
        self.key = bytearray(16) + struct.pack('<I', (VIEW_KEY + 9) & M32)
        self.sessions = {}  # (ip, port) -> ZoneSession
        self.by_char = {}   # char id -> ZoneSession waiting for its next 0x00A

    # The character's lobby record (lpkt_chr_info_sub2, 140 bytes)
    def char_record(self):
        c = self.char
        r = bytearray(140)
        cid = c['char_id']
        struct.pack_into('<IHHHBB', r, 0, cid, cid & 0xFFFF, 0, 1, 0, (cid >> 16) & 0xFF)
        r[12:28] = cstr(c['name'], 16)
        r[28:44] = cstr(c['server_name'], 16)
        m = 44  # TC_OPERATION_MAKE
        struct.pack_into('<HBBHBBBBH', r, m, c['race'], c['main_job'], c['sub_job'], c['face'], c['nation'], 0,
                         c['face'], c['size'], 0)
        looks = [c['face'], c['head'], c['body'], c['hands'], c['legs'], c['feet'], c['main'], c['sub']]
        struct.pack_into('<8H', r, m + 12, *looks)
        r[m + 28] = c['zone'] & 255
        r[m + 29] = c['main_level']
        r[m + 35] = (c['zone'] >> 8) & 1
        struct.pack_into('<I', r, m + 52, 0xFFFFFFFF)  # every job unlocked
        r[m + 56 + c['main_job']] = c['main_level']
        return bytes(r)

    def view_reply(self, cmd, pkt):
        c = self.char
        if cmd == 0x26:  # version and expansions -> the key
            return lobby_packet(0x05, struct.pack('<III', VIEW_KEY, 0x0FFF, 0x00FC), 0x28)
        if cmd == 0x1F:  # the character list
            return lobby_packet(0x20, struct.pack('<I', 1) + self.char_record())
        if cmd == 0x24:  # the world list
            return lobby_packet(0x23, struct.pack('<II', 1, 0x20) + cstr(c['server_name'], 16))
        if cmd == 0x07:  # this character: where its zone is
            cid = c['char_id']
            ip = socket.inet_aton(self.host_ip)
            body = struct.pack('<II', cid, cid & 0xFFFF) + cstr(c['name'], 16) + struct.pack(
                '<I4sI4sI', (cid >> 16) & 0xFF, ip, self.args.zone_port, ip, 54002)  # zone, then search
            self.prepare_zone_in()
            return lobby_packet(0x0B, body, 0x48)
        if cmd in (0x14, 0x21, 0x22, 0x28):  # delete, create, name check, rename: all fine
            return lobby_packet(0x03, b'', 0x20)
        return None

    def prepare_zone_in(self):
        c = self.char
        s = ZoneSession(self, None)
        s.zone = c['zone']
        s.pos = list(c['pos']) if c['pos'] else list(self.zones.position(c['zone']))
        s.set_key(self.key)
        self.by_char[c['char_id']] = s

    # --- servers ---
    def serve_view(self, conn, addr):
        log('lobby', f'{addr[0]} connected')
        buf = b''
        try:
            while True:
                d = conn.recv(4096)
                if not d:
                    break
                buf += d
                while len(buf) >= 28:
                    n = struct.unpack_from('<I', buf)[0]
                    if n < 28 or n > 0x10000:
                        n = len(buf)
                    if len(buf) < n:
                        break
                    pkt, buf = buf[:n], buf[n:]
                    cmd = pkt[8]
                    reply = self.view_reply(cmd, pkt)
                    log('lobby', f'0x{cmd:02X} -> ' + (f'0x{reply[8]:02X}' if reply else 'nothing'))
                    if reply:
                        conn.sendall(reply)
                    if cmd == 0x07:
                        return  # the client waits for the lobby to close
        except OSError as e:
            log('lobby', f'{e}')
        finally:
            conn.close()

    def serve_data(self, conn, addr):
        log('data', f'{addr[0]} connected')
        try:
            while True:
                d = conn.recv(4096)
                if not d:
                    break
                if d[0] == 0xA2 and len(d) >= 21:  # the client's zone key
                    self.key = bytearray(d[1:21])
                    log('data', f'zone key {self.key.hex()}')
        except OSError:
            pass
        finally:
            conn.close()

    def serve_auth(self, conn, addr):
        try:
            req = conn.recv(8192).decode('utf-8', 'replace')
            user = re.search(r'"username"\s*:\s*"([^"]*)"', req)
            log('auth', f'sign-in as {user.group(1) if user else "?"}')
            reply = {'result': 1, 'account_id': self.char['account_id'],
                     'session_hash': [b - 256 if b >= 128 else b for b in self.hash]}
            conn.sendall(json.dumps(reply).encode())
        except (OSError, ssl.SSLError) as e:
            log('auth', f'{e}')
        finally:
            try:
                conn.close()
            except OSError:
                pass

    def serve_zone(self, sock):
        while True:
            data, addr = sock.recvfrom(4096)
            try:
                s = self.sessions.get(addr)
                out = ZoneSession.incoming(self, s, addr, bytearray(data))
                if out:
                    sock.sendto(out, addr)
            except Exception as e:  # one bad packet must not stop the zone
                log('zone', f'{addr}: {type(e).__name__}: {e}')


# --- zone (UDP) ---------------------------------------------------------------------------------

HDR = 28  # the zone protocol's packet header
TARGID = 0x400  # the character's index among the zone's entities
VANA_EPOCH = 1009810800  # the game clock counts Earth seconds from here


class Msg(bytearray):
    zone_out = False  # 0x00B: build() moves the key on after sending it


def sub(ptype, body):
    """One message inside a zone packet: 9-bit type, size in 4-byte units, sync (set when sent)."""
    body = bytes(body)
    n = (4 + len(body) + 3) & ~3
    p = Msg(n)
    p[4:4 + len(body)] = body
    struct.pack_into('<H', p, 0, (ptype & 0x1FF) | ((n // 4) << 9))
    return p


class ZoneSession:
    def __init__(self, srv, addr):
        self.srv = srv
        self.addr = addr
        self.zone = 0
        self.pos = [0.0, 0.0, 0.0, 0]
        self.server_id = 0
        self.client_id = 0
        self.cipher = self.prev_cipher = None
        self.key = None
        self.queue = []
        self.last = None  # the last packet sent, enciphered, until the client acknowledges it
        self.logged_in = False
        self.zoning = False
        self.seen = set()  # message types the client has sent

    def set_key(self, key):
        self.key = bytearray(key)
        self.cipher = zone_cipher(self.key)

    def bump_key(self):
        """After 0x00B the client moves its key on by 2 (the last word, as a number)."""
        self.prev_cipher = self.cipher
        v = (struct.unpack_from('<I', self.key, 16)[0] + 2) & M32
        struct.pack_into('<I', self.key, 16, v)
        self.cipher = zone_cipher(self.key)

    @staticmethod
    def incoming(srv, s, addr, buf):
        n = len(buf)
        if n <= HDR + 16:
            return None
        # A login (0x00A) arrives in the clear: MD5 of the body sits in the last 16 bytes.
        if md5(bytes(buf[HDR:n - 16])) == bytes(buf[n - 16:]) and (struct.unpack_from('<H', buf, HDR)[0] & 0x1FF) == 0x0A:
            cid = struct.unpack_from('<I', buf, HDR + 12)[0]
            s = srv.by_char.pop(cid, None) or (s if s and s.key else None)
            if s is None:
                s = ZoneSession(srv, addr)
                s.zone = srv.char['zone']
                s.pos = list(srv.zones.position(s.zone))
                s.set_key(srv.key)
                log('zone', f'login from {addr} for {cid:#x} without the lobby: using the default key')
            s.addr = addr
            srv.sessions[addr] = s
            s.client_id = struct.unpack_from('<H', buf, 0)[0]
            s.queue.clear()
            s.last = None
            log('zone', f'{addr} char {cid:#x} logs in to {srv.zones.name(s.zone)} at '
                        f'({s.pos[0]:.1f}, {s.pos[1]:.1f}, {s.pos[2]:.1f})')
            s.on_login()
            # A client that changed zones says which sync it expects next; a new one acknowledges
            # packet 0, so the reply is packet 1.
            sync = struct.unpack_from('<H', buf, HDR + 2)[0]
            if sync > 1:
                s.server_id = sync
                return s.build(buf, next_id=False)
            s.server_id = 0
            return s.build(buf)
        if s is None:
            return None
        body = s.decrypt(buf)
        if body is None:
            return None
        s.parse(buf, body)
        # Delivery is one packet at a time: until the client acknowledges the last one, it is sent again.
        if s.last is not None and struct.unpack_from('<H', buf, 2)[0] != s.server_id:
            return s.resend()
        return s.build(buf)

    def decrypt(self, buf):
        n = len(buf)
        words = ((n - HDR) // 4) & ~1
        for c in (self.cipher, self.prev_cipher):
            if c is None:
                continue
            b = bytearray(buf)
            c.run(b, HDR, words, False)
            if md5(bytes(b[HDR:n - 16])) == bytes(b[n - 16:]):
                nbits = struct.unpack_from('<I', b, n - 20)[0]
                body = self.srv.huff.decompress(bytes(b[HDR:n - 20]), nbits)
                if body is None:
                    log('zone', f'{self.addr}: a packet that does not decompress')
                return body
        log('zone', f'{self.addr}: a packet neither key opens')
        return None

    def parse(self, hdr, body):
        code = struct.unpack_from('<H', hdr, 0)[0]
        if DEBUG:
            types = []
            o = 0
            while o + 4 <= len(body) and body[o + 1] & 0xFE:
                types.append(f'{struct.unpack_from("<H", body, o)[0] & 0x1FF:03X}@{struct.unpack_from("<H", body, o + 2)[0]}')
                o += (body[o + 1] & 0xFE) * 2
            log('debug', f'in #{code} ack {struct.unpack_from("<H", hdr, 2)[0]} (sent #{self.server_id}): {" ".join(types)}')
        o = 0
        while o + 4 <= len(body):
            size = (body[o + 1] & 0xFE) * 2
            if size == 0 or o + size > len(body):
                break
            ptype = struct.unpack_from('<H', body, o)[0] & 0x1FF
            sync = struct.unpack_from('<H', body, o + 2)[0]
            if self.client_id < sync <= code:
                self.handle(ptype, body[o:o + size])
            o += size
        self.client_id = code

    def handle(self, t, p):
        handled = t in self.seen
        self.seen.add(t)
        if t == 0x00C:  # the client has the zone: send it everything else
            self.after_zone_in()
        elif t == 0x015 and not self.zoning:  # position (the old zone's, while changing zones)
            x, y, z = struct.unpack_from('<fff', p, 4)
            self.pos = [x, y, z, struct.unpack_from('<b', p, 20)[0] & 255]
        elif t == 0x05E:  # a zone line
            rect = struct.unpack_from('<I', p, 4)[0]
            line = self.srv.zones.lines.get(rect)
            if line:
                self.change_zone(line[0], list(line[1:]))
            else:
                self.say(f'zone line {rect:#x}: unknown (start the server with --sql for zone lines)')
                self.queue.append(sub(0x052, bytes(4)))  # release the client's controls
        elif t == 0x0B5:  # chat
            text = bytes(p[6:]).split(b'\0')[0].decode('ascii', 'replace')
            if text.startswith('!'):
                self.command(text[1:].split())
        elif t == 0x04B:  # the server message, which the client asks for until it has all of it
            kind, lang = p[6], p[7]
            stamp, offset = struct.unpack_from('<i', p, 8)[0], struct.unpack_from('<i', p, 16)[0]
            if kind == 1:
                self.queue.append(self.server_message(lang, stamp, offset))
        elif t == 0x016 and struct.unpack_from('<H', p, 4)[0] == TARGID:  # the character asks about itself
            self.queue += [self.char_update(), self.char_status()]
        elif t == 0x061:  # its stats
            self.queue += [self.char_stats(), self.party_member()]
        elif t == 0x05A:  # conquest and Besieged standings: none
            self.queue.append(sub(0x05E, bytes(0xB0)))
        elif t == 0x03C:  # the blacklist
            self.queue.append(self.blacklist())
        elif t == 0x0E7:  # /logout, /shutdown
            self.queue.append(self.logout_packet(1))
            log('zone', f'{self.addr} logs out')
        elif not handled:  # the rest needs no answer here; each is logged once
            log('zone', f'0x{t:03X} ({len(p)} bytes) ignored')

    def command(self, words):
        try:
            if words and words[0] == 'zone' and len(words) >= 2:
                zone = int(words[1], 0)
                pos = [float(v) for v in words[2:5]] + [int(words[5]) if len(words) > 5 else 0] if len(words) >= 5 else None
                self.change_zone(zone, pos)
            elif words and words[0] == 'pos' and len(words) >= 4:
                self.change_zone(self.zone, [float(v) for v in words[1:4]] + [int(words[4]) if len(words) > 4 else self.pos[3]])
            elif words and words[0] == 'where':
                self.say(f'{self.srv.zones.name(self.zone)} ({self.zone}): {self.pos[0]:.2f} {self.pos[1]:.2f} '
                         f'{self.pos[2]:.2f} rot {self.pos[3]}')
            else:
                self.say('commands: !zone <id> [x y z [rot]], !pos <x> <y> <z> [rot], !where')
        except ValueError:
            self.say('numbers, please')

    def change_zone(self, zone, pos):
        self.zone = zone
        self.pos = pos if pos else list(self.srv.zones.position(zone))
        self.zoning = True
        log('zone', f'{self.addr} -> {self.srv.zones.name(zone)} ({zone}) at {self.pos}')
        self.queue.append(self.logout_packet(2, zone_change=True))

    def logout_packet(self, state, zone_change=False):
        ip = socket.inet_aton(self.srv.host_ip) if zone_change else bytes(4)
        port = self.srv.args.zone_port if zone_change else 0
        p = sub(0x00B, struct.pack('<B3x4sI8xI', state, ip, port, 0))
        p.zone_out = True
        return p

    def say(self, text):
        msg = text.encode('ascii', 'replace')[:150]
        self.queue.append(sub(0x017, struct.pack('<BBH', 6, 0, self.zone) + bytes(15) + msg + b'\0'))

    # --- what the client is sent ---
    def on_login(self):
        self.logged_in = True
        self.zoning = False
        self.queue += [sub(0x04F, bytes(4)), self.grap_list(), self.item_max(), self.login_packet()]

    def after_zone_in(self):
        c = self.srv.char
        q = [sub(0x008, b'\xff' * 48),  # every zone visited: every map viewable
             self.item_max(), self.grap_list(), self.job_info(), self.char_status(), self.char_stats(),
             self.party_member(), self.char_update()]
        q += [sub(0x055, bytes(128) + struct.pack('<HH', table, 0)) for table in range(7)]  # no key items
        # the quest and mission logs, all empty: quests offered and completed per area, missions
        offered = (0x50, 0x58, 0x60, 0x68, 0x70, 0x78, 0x80, 0x88)
        logs = offered + tuple(x + 0x40 for x in offered) + (0xD0, 0xD8, 0x30, 0x38, 0xE0, 0xE8, 0xF0, 0xF8, 0x100, 0x108)
        q += [sub(0x056, bytes(32) + struct.pack('<HH', port, 0)) for port in logs]
        q.append(sub(0x056, bytes(36) + struct.pack('<HH', 0xFFFF, 0)))
        q += [sub(0x0AA, bytes(128)),          # no spells
              sub(0x0AE, bytes(8)),            # no mounts
              sub(0x0AC, bytes(224)),          # no abilities or weapon skills
              self.char_sync(),
              sub(0x08C, bytes(8)),            # no merits
              self.blacklist()]
        # the item containers, each empty, in the order the game expects them, then "all loaded"
        flags = 0
        for cid in (0, 1, 9, 2, 17, 8, 10, 11, 12, 13, 14, 15, 16, 3, 4, 5, 6, 7):
            flags |= 1 << cid
            q.append(sub(0x01D, struct.pack('<BB2xI', 0, cid, flags)))
        q.append(sub(0x01D, struct.pack('<BB2xI', 1, 18, flags)))
        self.queue += q
        self.say(f'{self.srv.zones.name(self.zone)} ({self.zone}). Say !help for commands.')

    def server_message(self, lang, stamp, offset):
        """0x04D: the server message (the login message), up to 236 bytes of it at offset"""
        text = self.srv.args.motd.encode('ascii', 'replace') + b'\0'
        part = text[offset:offset + 236]
        return sub(0x04D, struct.pack('<BbBBiiii', 1 if offset == 0 else 2, 1, 1, lang, stamp or int(time.time()),
                                      len(text), offset, len(part)) + part)

    def blacklist(self):
        """0x041: an empty blacklist (reset, last packet)"""
        return sub(0x041, bytes(240) + struct.pack('<bbH', 3, 0, 0))

    def female(self):
        return self.srv.char['race'] in (2, 4, 6, 7)

    def looks(self):
        c = self.srv.char
        return [c['face'] | (c['race'] << 8), c['head'] + 0x1000, c['body'] + 0x2000, c['hands'] + 0x3000,
                c['legs'] + 0x4000, c['feet'] + 0x5000, c['main'] + 0x6000, c['sub'] + 0x7000, c['ranged'] + 0x8000]

    def grap_list(self):
        return sub(0x051, struct.pack('<9H2x', *self.looks()))

    def item_max(self):
        b = bytearray(96)  # 18 container sizes, then 18 usable sizes
        for i in range(18):
            b[i] = 81
            struct.pack_into('<H', b, 32 + 2 * i, 81)
        return sub(0x01C, b)

    def login_packet(self):
        """0x00A: the zone, where the character stands in it, the clock, the music"""
        c = self.srv.char
        now = int(time.time())
        x, y, z, rot = self.pos
        b = bytearray(0x100)
        struct.pack_into('<IHBbfff', b, 0x00, c['char_id'], TARGID, 0, (int(rot) + 128) % 256 - 128, x, y, z)
        struct.pack_into('<BBBB', b, 0x18, c['speed'], 40, 100, 0)  # speed, animation speed, HP%, status
        struct.pack_into('<I', b, 0x1C, (self.female() * 128 + (1 << c['size'])) << 8)
        struct.pack_into('<I', b, 0x24, 0x0100)
        struct.pack_into('<IIII', b, 0x2C, self.zone, 0, now, now - VANA_EPOCH)
        struct.pack_into('<HH9H', b, 0x3C, 0, self.zone, *self.looks())
        music = self.srv.zones.music.get(self.zone, (0, 0, 0, 0))
        struct.pack_into('<5H', b, 0x52, *music, 0xD4)
        struct.pack_into('<I', b, 0x7C, 2)  # login state: in the game, not the Mog House
        b[0x80:0x90] = cstr(c['name'], 16)
        struct.pack_into('<II', b, 0x9C, 3600, 60 * 360)  # play time, death counter
        struct.pack_into('<H', b, 0xA6, 0x01FF)  # no Mog House
        b[0xAC:0xF0] = self.dancer()
        struct.pack_into('<I', b, 0xFC, 1)
        return sub(0x00A, b)

    def dancer(self):
        """the job and stats summary 0x00A and 0x01B share (0x44 bytes)"""
        c = self.srv.char
        b = bytearray(0x44)
        struct.pack_into('<HHBBBBI', b, 0, c['race'], c['face'], c['main_job'], c['face'], c['size'], c['sub_job'],
                         0xFFFFFFFE)
        b[0x0C + c['main_job']] = c['main_level']
        struct.pack_into('<7H', b, 0x1C, *([50] * 7))
        struct.pack_into('<ii', b, 0x38, c['hp'], c['mp'])
        b[0x40] = 1
        return b

    def job_info(self):
        """0x01B: jobs and levels"""
        return sub(0x01B, self.dancer() + bytes(0x68 - 0x44))

    def party_member(self):
        """0x0DF: the character's HP/MP/TP line in the party list"""
        c = self.srv.char
        b = bytearray(0x24)
        struct.pack_into('<IIIIHBBBBH', b, 0, c['char_id'], c['hp'], c['mp'], 0, TARGID, 100, 100, 0, 0, self.zone)
        struct.pack_into('<BBBB', b, 0x1C, c['main_job'], c['main_level'], c['sub_job'], c['sub_level'])
        return sub(0x0DF, b)

    def char_stats(self):
        """0x061: HP, MP, jobs, base stats, home point"""
        c = self.srv.char
        b = bytearray(0x6C)
        struct.pack_into('<iiBBBBhh', b, 0, c['hp'], c['mp'], c['main_job'], c['main_level'], c['sub_job'],
                         c['sub_level'], 0, 1000)
        struct.pack_into('<7H', b, 0x10, *([50] * 7))
        struct.pack_into('<hh', b, 0x2C, 100, 100)
        struct.pack_into('<HHHH', b, 0x40, 0, 1, 0, self.zone)
        b[0x4C] = c['nation']
        return sub(0x061, b)

    def char_status(self):
        """0x037: status icons, flags, speed"""
        c = self.srv.char
        b = bytearray(0x5C)
        b[0:32] = b'\xff' * 32  # no status effects
        flags0 = (self.female() << 8) | ((c['size'] & 3) << 11) | (100 << 16)
        flags1 = (c['speed'] & 0xFFF) | (40 << 17)
        struct.pack_into('<III', b, 0x20, c['char_id'], flags0, flags1)
        struct.pack_into('<II', b, 0x38, 60 * 360, int(time.time()) - VANA_EPOCH + 360)
        b[0x54] = 0x10
        return sub(0x037, b)

    def char_update(self):
        """0x00D: the character as an entity in the zone (position, flags, model, name)"""
        c = self.srv.char
        x, y, z, rot = self.pos
        b = bytearray(0x68)
        struct.pack_into('<IHBBfff', b, 0, c['char_id'], TARGID, 0x1F, int(rot) & 255, x, y, z)
        struct.pack_into('<BBBB', b, 0x18, c['speed'], 40, 100, 0)
        struct.pack_into('<I', b, 0x1C, ((c['size'] & 3) << 9) | (self.female() << 15))
        b[0x3F] = 10  # hitbox size, tenths
        struct.pack_into('<9H', b, 0x44, *self.looks())
        b[0x56:0x66] = cstr(c['name'], 16)
        return sub(0x00D, b)

    def char_sync(self):
        """0x067 kind 2: the character's own entry"""
        c = self.srv.char
        b = bytearray(0x24)
        struct.pack_into('<BBHI', b, 0, 2, 9, TARGID, c['char_id'])
        b[0x21] = c['main_level']
        return sub(0x067, b)

    def build(self, client_hdr, next_id=True):
        """The next packet: what is queued, compressed, signed and enciphered."""
        if next_id:
            self.server_id = (self.server_id + 1) & 0xFFFF
        h = bytearray(client_hdr[:HDR])
        struct.pack_into('<HH', h, 0, self.server_id, self.client_id)
        struct.pack_into('<I', h, 8, int(time.time()))
        body = bytearray()
        zone_out = False
        while self.queue:
            p = self.queue[0]
            if len(body) + len(p) > 700 and body:  # Huffman can grow it; the client takes 1300
                break
            self.queue.pop(0)
            struct.pack_into('<H', p, 2, self.server_id)
            body += p
            zone_out = zone_out or p.zone_out
        data, nbits = self.srv.huff.compress(bytes(body))
        data += struct.pack('<I', nbits)
        data += md5(data)
        out = h + data
        self.cipher.run(out, HDR, (len(data) // 4) & ~1, True)
        self.last = out
        if zone_out:
            self.bump_key()
            self.srv.by_char[self.srv.char['char_id']] = self
            self.queue.clear()
        return bytes(out)

    def resend(self):
        """The last packet again, its header brought up to date (the header is not enciphered)."""
        struct.pack_into('<H', self.last, 2, self.client_id)
        struct.pack_into('<I', self.last, 8, int(time.time()))
        return bytes(self.last)


# --- start --------------------------------------------------------------------------------------

def tcp_listener(bind, port, handler, wrap=None):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((bind, port))
    s.listen(8)

    def loop():
        while True:
            conn, addr = s.accept()
            if wrap:
                try:
                    conn = wrap.wrap_socket(conn, server_side=True)
                except (ssl.SSLError, OSError) as e:
                    log('auth', f'TLS: {e}')
                    conn.close()
                    continue
            threading.Thread(target=handler, args=(conn, addr), daemon=True).start()

    threading.Thread(target=loop, daemon=True).start()


def tls_context():
    """TLS for the auth port, with a self-signed certificate made once by the openssl command."""
    cache = os.path.join(os.path.expanduser('~'), '.cache', 'ffxi-staticserver')
    cert, key = os.path.join(cache, 'cert.pem'), os.path.join(cache, 'key.pem')
    if not (os.path.exists(cert) and os.path.exists(key)):
        os.makedirs(cache, exist_ok=True)
        try:
            subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '3650',
                            '-subj', '/CN=staticserver', '-keyout', key, '-out', cert],
                           check=True, capture_output=True)
        except (OSError, subprocess.CalledProcessError) as e:
            log('auth', f'no certificate ({e}): the auth port is off, use FFXI_LSB_SESSION')
            return None
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.minimum_version = ssl.TLSVersion.TLSv1_2
    ctx.load_cert_chain(cert, key)
    return ctx


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--huffman', default=os.environ.get('FFXI_HUFFMAN'), help='folder with compress.dat')
    ap.add_argument('--config', help='JSON with character fields (see DEFAULT_CHAR)')
    ap.add_argument('--sql', help='a LandSandBoat sql/ folder: zone lines and music')
    ap.add_argument('--name')
    ap.add_argument('--zone', type=int)
    ap.add_argument('--pos', help='x,y,z[,rot]')
    ap.add_argument('--motd', default='Static server: no NPCs, no monsters. Say !help for commands.')
    ap.add_argument('--bind', default='0.0.0.0')
    ap.add_argument('--public-ip', default='127.0.0.1', help='the address the client reaches this server at')
    ap.add_argument('--session-hash', default='5354415449435345535349304e000001', help='32 hex digits')
    ap.add_argument('--auth-port', type=int, default=54231)
    ap.add_argument('--data-port', type=int, default=54230)
    ap.add_argument('--view-port', type=int, default=54001)
    ap.add_argument('--zone-port', type=int, default=54230)
    args = ap.parse_args()

    if not args.huffman:
        ap.error('--huffman (or FFXI_HUFFMAN): the folder holding compress.dat')
    huff = Huffman(args.huffman)
    char = dict(DEFAULT_CHAR)
    if args.config:
        with open(args.config) as f:
            char.update(json.load(f))
    if args.name:
        char['name'] = args.name
    if args.zone is not None:
        char['zone'] = args.zone
        char['pos'] = None
    if args.pos:
        v = [float(x) for x in args.pos.split(',')]
        char['pos'] = v[:3] + [int(v[3]) if len(v) > 3 else 0]
    if len(bytes.fromhex(args.session_hash)) != 16:
        ap.error('--session-hash: 32 hex digits')

    srv = Server(args, char, Zones(args.sql), huff)
    ctx = tls_context()
    if ctx:
        tcp_listener(args.bind, args.auth_port, srv.serve_auth, ctx)
    tcp_listener(args.bind, args.data_port, srv.serve_data)
    tcp_listener(args.bind, args.view_port, srv.serve_view)
    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp.bind((args.bind, args.zone_port))
    log('server', f'{char["name"]} ({char["char_id"]:#x}) in {srv.zones.name(char["zone"])}; '
                  f'FFXI_LSB_SESSION={char["account_id"]}:{args.session_hash}')
    try:
        srv.serve_zone(udp)
    except KeyboardInterrupt:
        pass


if __name__ == '__main__':
    main()
