"""The private bucket behind webserve.py --bucket: set it up and check it. Standard library only.

  python3 tools/bucket.py cors  --bucket <url> --origin https://xi.example.com [--origin ...]
  python3 tools/bucket.py check --bucket <url> --object '<folder>/<file>' [--origin https://xi.example.com]
  python3 tools/bucket.py blocks --bucket <url> --prefix <folder> --game <install> [--dats <folder>]...

The key is in XI_BUCKET_KEY / XI_BUCKET_SECRET, as for webserve.py: in the environment, ~/.config/xi-web.env,
or the file given with --env.
  cors   lets those page addresses read the bucket cross-origin (GET and HEAD with Range), which the page's
         file cache needs; the bucket stays private (every read still needs a signed link)
  check  signs a link to one object, reads its first 16 bytes the way the page does, and shows that an
         unsigned read is refused
  blocks uploads the install and the overlays (as webserve.py --game/--dats serve them) to <folder> in 1 MB
         gzipped blocks, and <folder>/manifest.txt naming each file's version there; for webserve.py
         --bucket-blocks <folder>. Run again after the files change: it uploads only the files whose size or
         date differ from the manifest's (the key needs write access to the bucket)
"""
import argparse
import base64
import hashlib
import os
import concurrent.futures
import gzip
import sys
import threading
import time
import urllib.error
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from webserve import BLOCK, Bucket, Tree, block_key, load_env, version  # noqa: E402


def keys():
    key, secret = os.environ.get('XI_BUCKET_KEY'), os.environ.get('XI_BUCKET_SECRET')
    if not key or not secret:
        sys.exit('set XI_BUCKET_KEY and XI_BUCKET_SECRET')
    return key, secret


def fetch(url, headers=None, method='GET', data=None):
    req = urllib.request.Request(url, data=data, method=method, headers=headers or {})
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            return r.status, dict(r.headers), r.read()
    except urllib.error.HTTPError as e:
        return e.code, dict(e.headers), e.read()
    except OSError as e:  # the network (a name that didn't resolve, a dropped connection): try again
        return 0, {}, str(e).encode()


def cors(a):
    # set on the bucket itself, not through its CDN address
    b = Bucket(a.bucket.replace('.cdn.', '.'), *keys())
    rules = ''.join('<CORSRule><AllowedOrigin>%s</AllowedOrigin><AllowedMethod>GET</AllowedMethod>'
                    '<AllowedMethod>HEAD</AllowedMethod><AllowedHeader>*</AllowedHeader>'
                    '<ExposeHeader>Content-Range</ExposeHeader><ExposeHeader>Content-Length</ExposeHeader>'
                    '<MaxAgeSeconds>86400</MaxAgeSeconds></CORSRule>' % o.rstrip('/') for o in a.origin)
    body = ('<CORSConfiguration>%s</CORSConfiguration>' % rules).encode()
    md5 = base64.b64encode(hashlib.md5(body).digest()).decode()
    url, hdrs = b.sign('', method='PUT', query={'cors': ''}, headers={'content-md5': md5, 'content-type': 'application/xml'},
                       payload_hash=hashlib.sha256(body).hexdigest(), presign=False)
    hdrs.pop('host')
    code, _, out = fetch(url, hdrs, 'PUT', body)
    print('cors: %d %s' % (code, out.decode(errors='replace')[:300]))
    return code == 200


def check(a):
    """the address given and, for a CDN address, the bucket's own"""
    ok = False
    for url in dict.fromkeys([a.bucket, a.bucket.replace('.cdn.', '.')]):
        b = Bucket(url, *keys())
        link = b.sign(a.object, expires=600)
        hdrs = {'Range': 'bytes=0-15'}
        if a.origin:
            hdrs['Origin'] = a.origin[0]
        code, h, body = fetch(link, hdrs)
        allow = {k.lower(): v for k, v in h.items()}.get('access-control-allow-origin')
        print('%s (region %s)' % (b.base, b.region))
        print('  signed read:   %d, %d bytes, Access-Control-Allow-Origin: %s' % (code, len(body), allow))
        if code not in (200, 206):
            print('  ' + body.decode(errors='replace')[:400])
        code2, _, _ = fetch(link.split('?')[0], {'Range': 'bytes=0-15'})
        print('  unsigned read: %d (should be 403)' % code2)
        ok = ok or (code in (200, 206) and code2 == 403 and (not a.origin or allow))
    return ok


def blocks(a):
    b = Bucket(a.bucket.replace('.cdn.', '.'), *keys())
    prefix = a.prefix.strip('/')
    trees = {'dat': Tree([('', a.game)]), 'dats': Tree([(str(i), d) for i, d in enumerate(a.dats)])}
    code, _, text = fetch(b.sign(prefix + '/manifest.txt', expires=300))
    manifest = dict(line.split('\t')[:2] for line in text.decode('utf-8').splitlines() if '\t' in line) if code == 200 else {}
    todo = []
    for top, t in trees.items():
        for rel, full in sorted(t.files.items()):
            key, ver = top + '/' + rel, version(full)
            if manifest.get(key) != ver:
                todo.append((key, ver, full))
    total = sum(int(v.split('.')[0]) for _, v, _ in todo)
    print('%d files in the manifest; %d to upload (%.1f GB)' % (len(manifest), len(todo), total / 1e9))
    lock, done = threading.Lock(), {'files': 0, 'raw': 0, 'sent': 0, 'saved': time.time()}
    t0 = time.time()

    def save():
        body = ''.join('%s\t%s\n' % kv for kv in sorted(manifest.items())).encode()
        c, _, out = fetch(b.sign(prefix + '/manifest.txt', method='PUT', expires=600), {'Content-Type': 'text/plain'}, 'PUT', body)
        if c != 200:
            print('manifest: %d %s' % (c, out[:200]))

    def one(job):
        key, ver, full = job
        with open(full, 'rb') as f:
            n = 0
            while True:
                d = f.read(BLOCK)
                if not d and n:
                    break
                z = gzip.compress(d, 6, mtime=0)
                for attempt in range(6):
                    c, _, out = fetch(b.sign(block_key(prefix, key, ver, n), method='PUT', expires=3600),
                                      {'Content-Type': 'application/gzip'}, 'PUT', z)
                    if c == 200:
                        break
                    time.sleep(2 ** attempt)
                else:
                    raise RuntimeError('%s block %d: %d %s' % (key, n, c, out[:200]))
                with lock:
                    done['raw'] += len(d)
                    done['sent'] += len(z)
                n += 1
                if len(d) < BLOCK:
                    break
        with lock:
            manifest[key] = ver
            done['files'] += 1
            el = time.time() - t0
            print('\r%d/%d files, %.2f of %.2f GB (sent %.2f GB, %.0f%%), %.1f MB/s   ' % (
                done['files'], len(todo), done['raw'] / 1e9, total / 1e9, done['sent'] / 1e9,
                100 * done['sent'] / max(1, done['raw']), done['sent'] / 1e6 / max(el, 1e-3)), end='', flush=True)
            if time.time() - done['saved'] > 60:
                done['saved'] = time.time()
                save()

    try:
        with concurrent.futures.ThreadPoolExecutor(a.jobs) as ex:
            for f in concurrent.futures.as_completed([ex.submit(one, j) for j in todo]):
                f.result()
    finally:
        with lock:
            save()
        print('\nmanifest saved: %d files' % len(manifest))
    return True


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('what', choices=('cors', 'check', 'blocks'))
    ap.add_argument('--bucket', required=True, help='https://<bucket>.<region>[.cdn].digitaloceanspaces.com')
    ap.add_argument('--origin', action='append', default=[], help="the page's address, as the browser sees it")
    ap.add_argument('--object', help='check: an object in the bucket')
    ap.add_argument('--prefix', help='blocks: the folder in the bucket for the blocks')
    ap.add_argument('--game', help='blocks: the FINAL FANTASY XI folder (as webserve.py --game)')
    ap.add_argument('--dats', action='append', default=[], help='blocks: each overlay folder (as webserve.py --dats, same order)')
    ap.add_argument('--jobs', type=int, default=12, help='blocks: files uploaded at once')
    ap.add_argument('--env', help='a file of KEY=VALUE lines (default: ~/.config/xi-web.env, if there is one)')
    a = ap.parse_args()
    load_env(a.env)
    if a.what == 'cors' and not a.origin:
        sys.exit('cors needs --origin')
    if a.what == 'check' and not a.object:
        sys.exit('check needs --object')
    if a.what == 'blocks' and not (a.prefix and a.game):
        sys.exit('blocks needs --prefix and --game')
    sys.exit(0 if {'cors': cors, 'check': check, 'blocks': blocks}[a.what](a) else 1)


if __name__ == '__main__':
    main()
