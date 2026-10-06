// The file cache: a worker answering the game's data reads (runtime/portable/httpfs_web.c) from Cache
// Storage, which outlives the page, so a file is fetched over the network once.
//  - a request ('xi-cache' channel): {u: url, k: key (path + mtime + size, no token), at, n, size, dst, cap,
//    cell}; the bytes go into the game's memory at dst, then cell = [2, length or -1] and a notify wakes the
//    waiting thread. A thread that gave up marks the cell 3 and is left alone.
//  - each block is one entry, keyed by the file's version so a changed file is fetched again.
//  - a miss also reads the file's next blocks ahead (the game reads most files front to back), all fetches
//    running at once, so a zone's files stream in parallel rather than one round trip at a time.
//  - with a bucket (webserve.py --bucket), a miss reads the block from the bucket's copy, by a link webserve.py
//    signs (one a file, used for 5 hours), and the network path through webserve.py only if that fails.
//  - the downloads overlay (hud.js) hears each fetch and hit on the 'xi-dl' channel.
const BLOCK = 1 << 20, AHEAD = 3;
const dl = new BroadcastChannel('xi-dl');
const inflight = new Map(); // key -> Promise<Uint8Array | null>
let mem = null, cache = null, queue = [], seq = 0, bucket = false;
const links = new Map(); // 'dat/<path>' -> Promise<{ url, until } | null>

onmessage = async ({ data }) => {
  if (!data.mem) return;
  if (data.clear) await caches.delete('xi-files-v1').catch(() => {});
  try { cache = await caches.open('xi-files-v1'); } catch (e) { cache = null; }
  bucket = !!data.bucket;
  mem = data.mem;
  for (const r of queue.splice(0)) serve(r);
};

new BroadcastChannel('xi-cache').onmessage = ({ data }) => (mem ? serve(data) : queue.push(data));

function get(u, k, at, n, ahead) {
  const key = new URL('/xi-cache/' + k + '&at=' + at, location.origin).href;
  let p = inflight.get(key);
  if (p) return p;
  p = (async () => {
    try {
      const hit = cache && (await cache.match(key));
      if (hit) {
        const b = new Uint8Array(await hit.arrayBuffer());
        if (b.length == n) return ahead || dl.postMessage({ hit: 1, k: b.length }), b;
      }
    } catch (e) {}
    const id = 'c' + ++seq, t0 = performance.now();
    dl.postMessage({ s: 1, id, u, at, n, ahead });
    const range = { headers: { Range: 'bytes=' + at + '-' + (at + n - 1) } };
    const read = async (url, opts) => {
      try {
        const r = await fetch(url, opts);
        if (r.status == 206 || (r.status == 200 && at == 0)) {
          const b = new Uint8Array(await r.arrayBuffer());
          return b.length >= n ? b.subarray(0, n) : b;
        }
      } catch (e) {}
      return null;
    };
    const link = bucket ? await signed(u) : null;
    let b = link ? await read(link, { ...range, mode: 'cors', credentials: 'omit' }) : null;
    const from = b ? 'bucket' : 'server';
    if (!b) b = await read(u, range);
    dl.postMessage({ s: 0, id, u, k: b ? b.length : -1, ms: performance.now() - t0, ahead, from });
    if (b && cache) cache.put(key, new Response(b)).catch(() => {});
    return b;
  })();
  inflight.set(key, p);
  p.finally(() => inflight.delete(key));
  return p;
}

// a signed link to the file's copy in the bucket, from webserve.py's /sign; null if it has none
function signed(u) {
  const url = new URL(u, location.origin), p = url.pathname.slice(1);
  const have = links.get(p);
  if (have) return have.then((l) => (l && l.until > Date.now() ? l.url : (links.delete(p), signed(u))));
  const got = fetch('/sign?t=' + encodeURIComponent(url.searchParams.get('t') || '') + '&p=' + encodeURIComponent(decodeURIComponent(p)))
    .then((r) => (r.ok ? r.json() : null))
    .then((j) => (j && j.url ? { url: j.url, until: Date.now() + 5 * 3600e3 } : null))
    .catch(() => null);
  links.set(p, got);
  return got.then((l) => (l ? l.url : null));
}

async function serve(r) {
  const b = get(r.u, r.k, r.at, r.n, 0);
  for (let i = 1; i <= AHEAD; ++i) {
    const at = r.at + i * BLOCK;
    if (at >= r.size) break;
    get(r.u, r.k, at, Math.min(BLOCK, r.size - at), 1);
  }
  const bytes = await b;
  const c = new Int32Array(mem.buffer, r.cell, 2);
  if (Atomics.compareExchange(c, 0, 0, 1) != 0) return; // the thread gave up
  let len = -1;
  if (bytes) {
    len = Math.min(bytes.length, r.cap);
    new Uint8Array(mem.buffer).set(bytes.subarray(0, len), r.dst);
  }
  Atomics.store(c, 1, len);
  Atomics.store(c, 0, 2);
  Atomics.notify(c, 0);
}
