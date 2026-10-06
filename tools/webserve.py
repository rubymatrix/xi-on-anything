"""The local web server for the browser build (docs/web-port-plan.md): serves the build to the
player's own browser and bridges its network. Standard library only.

  python3 tools/webserve.py --server <game server IP> [--root build/web] [--port 8417]

It prints the address to open: http://127.0.0.1:<port>/#t=<token>. The token is new each run; every
request and the WebSocket need it, so other pages the browser has open can't use this server.

  /            the build (--root): host64.js/.wasm and the page, with the headers that let the page
               share memory between threads (COOP/COEP)
  /net         WebSocket: the game's sockets (runtime/portable/net_web.c has the frame format). Each
               channel opens a real TCP or UDP socket here, to the --server address only
  /dat/...     the game install (--game), read only, with Range; /dat/index lists it
  /dats/N/...  the DAT overlay folders (--dats, first wins), the same way
  /config      what the page needs to start the game: the server's address, how many overlays
  /app/...     what host64 brings with it: ffxi.reg and the texture packs (assets/textures)
  /sign        with --bucket: a signed link (6 hours) to a /dat or /dats file's copy in the bucket

A private S3-compatible bucket holding a copy of the install and overlays (DigitalOcean Spaces,
Cloudflare R2, ...) takes the file reads off this machine's upload: --bucket is its address with the bucket
in the host name (https://<bucket>.sfo3.digitaloceanspaces.com, or the CDN one), --bucket-game the folder
the install is in, --bucket-dats each overlay's folder (in --dats order), and XI_BUCKET_KEY /
XI_BUCKET_SECRET an access key. The page's file cache (tools/web/dlcache.js) then reads blocks straight from
the bucket with links signed here, so only this server's sign-in can read it. Keep the bucket private, and
let the page's address read it cross-origin (tools/bucket.py cors sets that up).

Behind a reverse proxy or tunnel of your own (your devices only, with its own sign-in in front: Cloudflare
Access, Caddy with forward_auth, and the like), give the address it serves under with --origin
https://xi.example.com, and a fixed --token so a bookmark keeps working. The proxy must pass WebSocket
upgrades through (/net) and serve HTTPS: the page needs a secure context for shared memory.

Binds 127.0.0.1 only: the build is translated from the player's own FFXiMain.dll and is never served
to anyone else.
"""
import argparse
import asyncio
import base64
import datetime
import hmac
import hashlib
import json
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

class Tree:
    """A read-only folder served under a prefix: files by path (any case, as Windows finds them), and
    an index of every file and folder for runtime/portable/httpfs_web.c (FindFirstFile, stat)."""

    def __init__(self, roots):
        self.files = {}  # lower-case relative path -> host path
        self.where = {}  # lower-case relative path -> (prefix, the path under its root as written)
        lines = []
        for prefix, root in roots:
            root = os.path.realpath(root)
            if os.path.isfile(root):
                self.files[prefix.lower()] = root
                self.where[prefix.lower()] = (prefix, os.path.basename(root))
                st = os.stat(root)
                lines.append('%s\t%d\t%d' % (prefix, st.st_size, int(st.st_mtime)))
                continue
            for d, dirs, files in os.walk(root):
                dirs.sort()
                rel = os.path.relpath(d, root).replace(os.sep, '/')
                rel = prefix if rel == '.' else (prefix + '/' + rel if prefix else rel)
                if rel:
                    lines.append('%s\t-1' % rel)
                for f in sorted(files):
                    if f.startswith('.'):
                        continue
                    r = rel + '/' + f if rel else f
                    full = os.path.join(d, f)
                    self.files[r.lower()] = full
                    self.where[r.lower()] = (prefix, os.path.relpath(full, root).replace(os.sep, '/'))
                    st = os.stat(full)
                    lines.append('%s\t%d\t%d' % (r, st.st_size, int(st.st_mtime)))
        self.index = ('\n'.join(lines) + '\n').encode('utf-8')

    def path(self, rel):
        return self.files.get(rel.strip('/').lower())


class Bucket:
    """S3 signature version 4, as query-string links: GET of one object, for a while."""

    def __init__(self, url, key, secret):
        u = urllib.parse.urlsplit(url.rstrip('/'))
        self.base, self.host = '%s://%s' % (u.scheme, u.netloc), u.netloc
        labels = u.hostname.split('.')
        self.region = labels[1] if len(labels) > 2 else 'us-east-1'  # <bucket>.<region>[.cdn].digitaloceanspaces.com
        self.key, self.secret = key, secret

    def _key(self, day):
        k = ('AWS4' + self.secret).encode()
        for part in (day, self.region, 's3', 'aws4_request'):
            k = hmac.new(k, part.encode(), hashlib.sha256).digest()
        return k

    def sign(self, obj, method='GET', expires=6 * 3600, query=None, headers=None, payload_hash='UNSIGNED-PAYLOAD',
             presign=True):
        now = datetime.datetime.now(datetime.timezone.utc)
        amz, day = now.strftime('%Y%m%dT%H%M%SZ'), now.strftime('%Y%m%d')
        scope = '%s/%s/s3/aws4_request' % (day, self.region)
        path = '/' + urllib.parse.quote(obj, safe='/~')
        q = dict(query or {})
        hdrs = {'host': self.host}
        hdrs.update({k.lower(): v for k, v in (headers or {}).items()})
        if presign:
            q.update({'X-Amz-Algorithm': 'AWS4-HMAC-SHA256', 'X-Amz-Credential': self.key + '/' + scope,
                      'X-Amz-Date': amz, 'X-Amz-Expires': str(expires), 'X-Amz-SignedHeaders': ';'.join(sorted(hdrs))})
        else:
            hdrs.update({'x-amz-date': amz, 'x-amz-content-sha256': payload_hash})
        names = ';'.join(sorted(hdrs))
        cq = '&'.join('%s=%s' % (urllib.parse.quote(k, safe='~'), urllib.parse.quote(v, safe='~')) for k, v in sorted(q.items()))
        creq = '\n'.join([method, path, cq, ''.join('%s:%s\n' % (k, hdrs[k].strip()) for k in sorted(hdrs)), names, payload_hash])
        sts = '\n'.join(['AWS4-HMAC-SHA256', amz, scope, hashlib.sha256(creq.encode()).hexdigest()])
        sig = hmac.new(self._key(day), sts.encode(), hashlib.sha256).hexdigest()
        if presign:
            return '%s%s?%s&X-Amz-Signature=%s' % (self.base, path, cq, sig)
        hdrs['authorization'] = 'AWS4-HMAC-SHA256 Credential=%s/%s, SignedHeaders=%s, Signature=%s' % (self.key, scope, names, sig)
        return '%s%s%s' % (self.base, path, '?' + cq if cq else ''), hdrs


class Server:
    def __init__(self, a):
        self.a = a
        self.root = os.path.realpath(a.root)
        self.token = a.token or secrets.token_urlsafe(18)
        # the game server by name or address: the page gets an address (it can't look names up), and
        # /net connects only to the addresses the names have
        self.allowed, self.server_ip = set(), None
        for name in a.server:
            ips = sorted({ai[4][0] for ai in socket.getaddrinfo(name, None, socket.AF_INET)})
            self.allowed.update(ips)
            self.server_ip = self.server_ip or ips[0]
        self.server_name = a.server[0]
        self.ndats = len(a.dats or [])
        self.origins = {o.rstrip('/') for o in a.origin or []}
        repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        self.trees = {'dat': Tree([('', a.game)]),
                      'app': Tree([('ffxi.reg', os.path.join(repo, 'ffxi.reg')),
                                   ('textures', os.path.join(repo, 'assets', 'textures'))]),
                      'dats': Tree([(str(i), d) for i, d in enumerate(a.dats or [])])}
        # the bucket's copy: (tree, its prefix) -> the folder in the bucket
        self.bucket, self.folders = None, {}
        if a.bucket:
            key, secret = os.environ.get('XI_BUCKET_KEY'), os.environ.get('XI_BUCKET_SECRET')
            if not key or not secret:
                sys.exit('--bucket needs XI_BUCKET_KEY and XI_BUCKET_SECRET')
            self.bucket = Bucket(a.bucket, key, secret)
            self.folders[('dat', '')] = (a.bucket_game or '').strip('/')
            for i, f in enumerate(a.bucket_dats or []):
                self.folders[('dats', str(i))] = f.strip('/')

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
        if origin and urllib.parse.urlsplit(origin).hostname not in ('127.0.0.1', 'localhost') and \
                origin.rstrip('/') not in self.origins:
            return self.reply(writer, 403, b'other origins may not use this server')
        if url.path == '/net':
            if q.get('t', [''])[0] != self.token:
                return self.reply(writer, 403, b'bad token')
            return await self.websocket(reader, writer, hdr)
        if method not in ('GET', 'HEAD'):
            return self.reply(writer, 405, b'')
        top, _, rest = url.path.lstrip('/').partition('/')
        if url.path == '/config':
            if q.get('t', [''])[0] != self.token:
                return self.reply(writer, 403, b'bad token')
            body = json.dumps({'server': self.server_ip, 'server_name': self.server_name, 'dats': self.ndats,
                               'bucket': bool(self.bucket)}).encode()
            return self.reply(writer, 200, body, 'application/json')
        if url.path == '/sign':
            if q.get('t', [''])[0] != self.token:
                return self.reply(writer, 403, b'bad token')
            link = self.sign(q.get('p', [''])[0])
            if not link:
                return self.reply(writer, 404, b'not in the bucket')
            return self.reply(writer, 200, json.dumps({'url': link}).encode(), 'application/json')
        if top in self.trees:
            if q.get('t', [''])[0] != self.token:
                return self.reply(writer, 403, b'bad token')
            return self.tree(writer, self.trees[top], urllib.parse.unquote(rest), hdr.get('range'), method == 'HEAD')
        await self.static(writer, url.path, method == 'HEAD')

    def tree(self, writer, t, rel, rng, head_only):
        if rel == 'index':
            return self.reply(writer, 200, t.index)
        full = t.path(rel)
        if not full:
            return self.reply(writer, 404, b'not found')
        size = os.path.getsize(full)
        start, end = 0, size - 1
        if rng and rng.startswith('bytes='):
            a, _, b = rng[6:].partition('-')
            start = int(a or 0)
            end = min(int(b), size - 1) if b else size - 1
        if start > end and size:
            return self.reply(writer, 416, b'', extra=['Content-Range: bytes */%d' % size])
        with open(full, 'rb') as f:
            f.seek(start)
            body = f.read(end - start + 1) if size else b''
        if rng:
            return self.reply(writer, 206, b'' if head_only else body, 'application/octet-stream',
                              ['Content-Range: bytes %d-%d/%d' % (start, end, size)])
        self.reply(writer, 200, b'' if head_only else body, 'application/octet-stream')

    def sign(self, p):
        """'dat/<path>' or 'dats/<path>' -> a signed link to its copy in the bucket, or None"""
        top, _, rel = p.partition('/')
        t = self.trees.get(top) if self.bucket and top in ('dat', 'dats') else None
        w = t and t.where.get(rel.strip('/').lower())
        folder = w and self.folders.get((top, w[0]))
        if folder is None or not w:
            return None
        return self.bucket.sign((folder + '/' if folder else '') + w[1])

    def reply(self, writer, code, body, ctype='text/plain', extra=()):
        reason = {200: 'OK', 206: 'Partial Content', 403: 'Forbidden', 404: 'Not Found', 405: 'Method Not Allowed',
                  416: 'Range Not Satisfiable'}.get(code, 'Error')
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
                    help="the game server, by name or IPv4 address (repeat for more); the only place /net connects to")
    ap.add_argument('--dats', action='append', help='a DAT overlay folder (repeat; the first given wins)')
    ap.add_argument('--game', required=True, help='the FINAL FANTASY XI folder')
    ap.add_argument('--root', default=os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'build', 'web'))
    ap.add_argument('--port', type=int, default=8417)
    ap.add_argument('--bucket', help='a private bucket with a copy of the files: https://<bucket>.<region>.digitaloceanspaces.com '
                    '(keys in XI_BUCKET_KEY, XI_BUCKET_SECRET)')
    ap.add_argument('--bucket-game', help="the install's folder in the bucket")
    ap.add_argument('--bucket-dats', action='append', help="each --dats folder's folder in the bucket, in the same order")
    ap.add_argument('--token', default=os.environ.get('XI_WEB_TOKEN'),
                    help='a fixed token (or XI_WEB_TOKEN): for a bookmark behind a proxy, or tests; default: a new one each run')
    ap.add_argument('--origin', action='append',
                    help='an address a proxy of yours serves this under, as the browser sees it (https://xi.example.com); '
                         'repeat for more')
    a = ap.parse_args()
    srv = Server(a)
    LOG('indexed %d game files' % len(srv.trees['dat'].files))

    async def run():
        server = await asyncio.start_server(srv.handle, '127.0.0.1', a.port)
        LOG('serving %s on http://127.0.0.1:%d/#t=%s (game server %s: %s)' % (srv.root, a.port, srv.token, ', '.join(a.server),
                                                                           ', '.join(sorted(srv.allowed))))
        for o in sorted(srv.origins):
            LOG('  and through your proxy: %s/#t=%s' % (o, srv.token))
        if srv.bucket:
            LOG('  file reads from the bucket at %s' % srv.bucket.base)
        sys.stdout.flush()
        async with server:
            await server.serve_forever()

    try:
        asyncio.run(run())
    except KeyboardInterrupt:
        pass


if __name__ == '__main__':
    main()
