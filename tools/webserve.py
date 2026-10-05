"""The local web server for the browser build (docs/web-port-plan.md): serves the build to the
player's own browser and bridges its network. Standard library only.

  python3 tools/webserve.py --server <game server IP> [--root build/web] [--port 8417]

It prints the address to open: http://127.0.0.1:<port>/#t=<token>. The token is new each run; every
request and the WebSocket need it, so other pages the browser has open can't use this server.

  /            the build (--root): host64.js/.wasm and the page, with the headers that let the page
               share memory between threads (COOP/COEP)
  /net         WebSocket: the game's sockets (runtime/portable/net_web.c has the frame format). Each
               channel opens a real TCP or UDP socket here, to the --server address only

Binds 127.0.0.1 only: the build is translated from the player's own FFXiMain.dll and is never served
to anyone else.
"""
import argparse
import asyncio
import base64
import hashlib
import ipaddress
import mimetypes
import os
import secrets
import socket
import struct
import sys
import urllib.parse

WS_GUID = b'258EAFA5-E914-47DA-95CA-C5AB0DC85B11'
LOG = print


# --- WebSocket framing (RFC 6455), server side --------------------------------------------------------

async def ws_read(reader):
    """One message: (opcode, payload). Fragments are joined; control frames come back on their own."""
    data, first_op = b'', None
    while True:
        h = await reader.readexactly(2)
        fin, op, masked, n = h[0] & 0x80, h[0] & 0x0F, h[1] & 0x80, h[1] & 0x7F
        if n == 126:
            n = struct.unpack('>H', await reader.readexactly(2))[0]
        elif n == 127:
            n = struct.unpack('>Q', await reader.readexactly(8))[0]
        mask = await reader.readexactly(4) if masked else b''
        p = await reader.readexactly(n)
        if masked:
            p = bytes(b ^ mask[i & 3] for i, b in enumerate(p)) if n < 64 else _unmask(p, mask)
        if op >= 8:
            return op, p
        if first_op is None:
            first_op = op
        data += p
        if fin:
            return first_op, data


def _unmask(p, mask):
    m = int.from_bytes(mask * (len(p) // 4 + 1), 'little')
    return (int.from_bytes(p, 'little') ^ (m & ((1 << (8 * len(p))) - 1))).to_bytes(len(p), 'little')


def ws_frame(op, payload):
    n = len(payload)
    if n < 126:
        h = struct.pack('>BB', 0x80 | op, n)
    elif n < 65536:
        h = struct.pack('>BBH', 0x80 | op, 126, n)
    else:
        h = struct.pack('>BBQ', 0x80 | op, 127, n)
    return h + payload


# --- the socket bridge ---------------------------------------------------------------------------------

def ip_of(b):
    return socket.inet_ntoa(bytes(b))


class Udp(asyncio.DatagramProtocol):
    def __init__(self, bridge, ch):
        self.bridge, self.ch = bridge, ch

    def datagram_received(self, data, addr):
        self.bridge.out(0x83, self.ch, socket.inet_aton(addr[0]) + struct.pack('<H', addr[1]) + data)

    def error_received(self, exc):
        pass  # ICMP unreachable and the like: UDP carries on, as the game expects


class Bridge:
    """One page's /net: channel -> its real socket."""

    def __init__(self, writer, allowed):
        self.writer, self.allowed = writer, allowed
        self.chans = {}  # ch -> {'kind': 1|2, 'reader_task', 'writer', 'udp'}

    def out(self, op, ch, payload=b''):
        if not self.writer.is_closing():
            self.writer.write(ws_frame(2, struct.pack('<BI', op, ch) + payload))

    def allow(self, ip):
        return ip in self.allowed

    async def frame(self, f):
        if len(f) < 5:
            return
        op, ch = struct.unpack_from('<BI', f)
        body = f[5:]
        c = self.chans.get(ch)
        if op == 1:  # OPEN
            kind = body[0] if body else 1
            self.chans[ch] = c = {'kind': kind}
            if kind == 2:
                loop = asyncio.get_running_loop()
                tr, _ = await loop.create_datagram_endpoint(lambda: Udp(self, ch), local_addr=('0.0.0.0', 0))
                c['udp'] = tr
        elif op == 2 and c and c['kind'] == 1:  # CONNECT
            ip, port = ip_of(body[:4]), struct.unpack_from('<H', body, 4)[0]
            if not self.allow(ip):
                LOG('net: refused %s:%d (not the game server)' % (ip, port))
                self.out(0x81, ch, b'\x01')
                return
            try:
                r, w = await asyncio.wait_for(asyncio.open_connection(ip, port), 10)
            except (OSError, asyncio.TimeoutError) as e:
                LOG('net: %s:%d: %s' % (ip, port, e))
                self.out(0x81, ch, b'\x01')
                return
            if self.chans.get(ch) is not c:  # closed while connecting
                w.close()
                return
            sock = w.get_extra_info('socket')
            if sock:
                sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            c['writer'] = w
            self.out(0x81, ch, b'\x00')
            c['reader_task'] = asyncio.create_task(self.pump(ch, r))
        elif op == 3 and c and c.get('writer'):  # SEND
            c['writer'].write(body)
        elif op == 4 and c and c.get('udp'):  # SENDTO
            ip, port = ip_of(body[:4]), struct.unpack_from('<H', body, 4)[0]
            if self.allow(ip):
                c['udp'].sendto(body[6:], (ip, port))
        elif op == 5 and c:  # CLOSE
            self.close(ch)

    async def pump(self, ch, reader):
        err = 0
        try:
            while True:
                data = await reader.read(65536)
                if not data:
                    break
                self.out(0x82, ch, data)
        except OSError:
            err = 1
        if ch in self.chans:
            self.out(0x84, ch, bytes([err]))

    def close(self, ch):
        c = self.chans.pop(ch, None)
        if not c:
            return
        if c.get('reader_task'):
            c['reader_task'].cancel()
        if c.get('writer'):
            c['writer'].close()
        if c.get('udp'):
            c['udp'].close()

    def close_all(self):
        for ch in list(self.chans):
            self.close(ch)


# --- HTTP ----------------------------------------------------------------------------------------------

class Server:
    def __init__(self, a):
        self.a = a
        self.root = os.path.realpath(a.root)
        self.token = a.token or secrets.token_urlsafe(18)
        self.allowed = set(a.server)

    async def handle(self, reader, writer):
        try:
            head = await reader.readuntil(b'\r\n\r\n')
        except (asyncio.IncompleteReadError, asyncio.LimitOverrunError, ConnectionError):
            writer.close()
            return
        lines = head.decode('latin-1').split('\r\n')
        try:
            method, target, _ = lines[0].split(' ', 2)
        except ValueError:
            writer.close()
            return
        hdr = {}
        for line in lines[1:]:
            if ':' in line:
                k, v = line.split(':', 1)
                hdr[k.strip().lower()] = v.strip()
        url = urllib.parse.urlsplit(target)
        q = urllib.parse.parse_qs(url.query)
        origin = hdr.get('origin')
        if origin and urllib.parse.urlsplit(origin).hostname not in ('127.0.0.1', 'localhost'):
            return self.reply(writer, 403, b'other origins may not use this server')
        if url.path == '/net':
            if q.get('t', [''])[0] != self.token:
                return self.reply(writer, 403, b'bad token')
            return await self.websocket(reader, writer, hdr)
        if method not in ('GET', 'HEAD'):
            return self.reply(writer, 405, b'')
        await self.static(writer, url.path, method == 'HEAD')

    def reply(self, writer, code, body, ctype='text/plain', extra=()):
        reason = {200: 'OK', 403: 'Forbidden', 404: 'Not Found', 405: 'Method Not Allowed'}.get(code, 'Error')
        h = ['HTTP/1.1 %d %s' % (code, reason), 'Content-Type: ' + ctype, 'Content-Length: %d' % len(body),
             'Cross-Origin-Opener-Policy: same-origin', 'Cross-Origin-Embedder-Policy: require-corp',
             'Cross-Origin-Resource-Policy: same-origin', 'Cache-Control: no-cache', 'Connection: close'] + list(extra)
        writer.write(('\r\n'.join(h) + '\r\n\r\n').encode() + body)
        writer.close()

    async def static(self, writer, path, head_only):
        rel = urllib.parse.unquote(path).lstrip('/') or 'index.html'
        full = os.path.realpath(os.path.join(self.root, rel))
        if not full.startswith(self.root + os.sep) or not os.path.isfile(full):
            return self.reply(writer, 404, b'not found')
        ctype = 'application/wasm' if full.endswith('.wasm') else mimetypes.guess_type(full)[0] or 'application/octet-stream'
        with open(full, 'rb') as f:
            body = f.read()
        self.reply(writer, 200, b'' if head_only else body, ctype)

    async def websocket(self, reader, writer, hdr):
        key = hdr.get('sec-websocket-key', '').encode()
        accept = base64.b64encode(hashlib.sha1(key + WS_GUID).digest()).decode()
        writer.write(('HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n'
                      'Sec-WebSocket-Accept: %s\r\n\r\n' % accept).encode())
        bridge = Bridge(writer, self.allowed)
        LOG('net: page connected')
        try:
            while True:
                op, p = await ws_read(reader)
                if op == 8:  # close
                    break
                if op == 9:  # ping
                    writer.write(ws_frame(10, p))
                elif op == 2:
                    await bridge.frame(p)
                await writer.drain()
        except (asyncio.IncompleteReadError, ConnectionError):
            pass
        finally:
            bridge.close_all()
            writer.close()
            LOG('net: page disconnected')


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--server', action='append', required=True,
                    help="the game server's IPv4 address (repeat for more); the only place /net connects to")
    ap.add_argument('--root', default=os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'build', 'web'))
    ap.add_argument('--port', type=int, default=8417)
    ap.add_argument('--token', default=os.environ.get('XI_WEB_TOKEN'), help='fixed token (tests); default: a new one each run')
    a = ap.parse_args()
    for s in a.server:
        ipaddress.IPv4Address(s)
    srv = Server(a)

    async def run():
        server = await asyncio.start_server(srv.handle, '127.0.0.1', a.port)
        LOG('serving %s on http://127.0.0.1:%d/#t=%s (game server %s)' % (srv.root, a.port, srv.token, ', '.join(a.server)))
        sys.stdout.flush()
        async with server:
            await server.serve_forever()

    try:
        asyncio.run(run())
    except KeyboardInterrupt:
        pass


if __name__ == '__main__':
    main()
