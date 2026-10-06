"""The private bucket behind webserve.py --bucket: set it up and check it. Standard library only.

  python3 tools/bucket.py cors  --bucket <url> --origin https://xi.example.com [--origin ...]
  python3 tools/bucket.py check --bucket <url> --object '<folder>/<file>' [--origin https://xi.example.com]

The key is in XI_BUCKET_KEY / XI_BUCKET_SECRET, as for webserve.py.
  cors   lets those page addresses read the bucket cross-origin (GET and HEAD with Range), which the page's
         file cache needs; the bucket stays private (every read still needs a signed link)
  check  signs a link to one object, reads its first 16 bytes the way the page does, and shows that an
         unsigned read is refused
"""
import argparse
import base64
import hashlib
import os
import sys
import urllib.error
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from webserve import Bucket  # noqa: E402


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
    b = Bucket(a.bucket, *keys())
    link = b.sign(a.object, expires=600)
    hdrs = {'Range': 'bytes=0-15'}
    if a.origin:
        hdrs['Origin'] = a.origin[0]
    code, h, body = fetch(link, hdrs)
    allow = {k.lower(): v for k, v in h.items()}.get('access-control-allow-origin')
    print('signed read:   %d, %d bytes, Access-Control-Allow-Origin: %s' % (code, len(body), allow))
    code2, _, _ = fetch(link.split('?')[0], {'Range': 'bytes=0-15'})
    print('unsigned read: %d (should be 403)' % code2)
    return code in (200, 206) and code2 == 403 and (not a.origin or allow)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('what', choices=('cors', 'check'))
    ap.add_argument('--bucket', required=True, help='https://<bucket>.<region>[.cdn].digitaloceanspaces.com')
    ap.add_argument('--origin', action='append', default=[], help="the page's address, as the browser sees it")
    ap.add_argument('--object', help='check: an object in the bucket')
    a = ap.parse_args()
    if a.what == 'cors' and not a.origin:
        sys.exit('cors needs --origin')
    if a.what == 'check' and not a.object:
        sys.exit('check needs --object')
    sys.exit(0 if (cors if a.what == 'cors' else check)(a) else 1)


if __name__ == '__main__':
    main()
