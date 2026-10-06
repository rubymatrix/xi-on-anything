// The file cache: a worker answering the game's data reads (runtime/portable/httpfs_web.c) from Cache
// Storage, which outlives the page, so a file is fetched over the network once.
//  - a request ('xi-cache' channel): {u: url, k: key (path + mtime + size, no token), at, n, size, dst, cap,
//    cell}; the bytes go into the game's memory at dst, then cell = [2, length or -1] and a notify wakes the
//    waiting thread. A thread that gave up marks the cell 3 and is left alone.
//  - each block is one entry, keyed by the file's version so a changed file is fetched again.
//  - the game opening a file ({open} on the same channel) fetches its first blocks at once, and a miss reads
//    the next blocks ahead: a zone's files stream in parallel rather than one round trip at a time.
//  - with a bucket (webserve.py --bucket), a miss reads the block from the bucket, by links webserve.py signs
//    (one ask a file, used for 5 hours): the file's gzipped block when there are blocks (--bucket-blocks;
//    unpacked here), else a range of its plain copy, and through webserve.py only if both fail.
//  - the background download ({prefetch: true} from the page) fills the cache with every file of the
//    install, a few at a time, the game's own reads first; a file done is marked, so it resumes.
//  - the downloads overlay (hud.js) hears each fetch, hit and the background download on 'xi-dl'.
const BLOCK = 1 << 20, AHEAD = 3, OPEN_BLOCKS = 16, BG_FILES = 8;
const CACHE = 'xi-files-v1';
const dl = new BroadcastChannel('xi-dl');
const inflight = new Map(); // key -> Promise<Uint8Array | null>
const links = new Map(); // 'dat/<path>' -> Promise<{ url, blocks?, until } | null>
let mem = null, cache = null, queue = [], seq = 0, bucket = false, token = '', game = 0;
let bg = null; // the background download: { on, files, done, bytes, doneBytes, t0 }

onmessage = async ({ data }) => {
  if (data.mem) {
    if (data.clear) await caches.delete(CACHE).catch(() => {});
    try { cache = await caches.open(CACHE); } catch (e) { cache = null; }
    bucket = !!data.bucket;
    token = data.token || '';
    mem = data.mem;
    for (const r of queue.splice(0)) serve(r);
  }
  if ('prefetch' in data) prefetch(data.prefetch);
};

new BroadcastChannel('xi-cache').onmessage = ({ data }) => (mem ? serve(data) : queue.push(data));

const entry = (k, at) => new URL('/xi-cache/' + k + '&at=' + at, location.origin).href;

// block at..at+n of the file (u its address at webserve.py, k its cache key); lane: 0 the game, 1 read ahead,
// 2 the background download (not shown one by one)
function get(u, k, at, n, lane) {
  const key = entry(k, at);
  let p = inflight.get(key);
  if (p) return p;
  p = (async () => {
    try {
      const hit = cache && (await cache.match(key));
      if (hit) {
        const b = new Uint8Array(await hit.arrayBuffer());
        if (b.length == n) return lane || dl.postMessage({ hit: 1, k: b.length }), b;
      }
    } catch (e) {}
    const id = 'c' + ++seq, t0 = performance.now();
    if (lane < 2) dl.postMessage({ s: 1, id, u, at, n, ahead: lane });
    if (!lane) game++;
    let b = null, from = 'server';
    try {
      const l = bucket ? await signed(u) : null;
      if (l && l.blocks && l.blocks[at / BLOCK]) {
        b = await unzip(l.blocks[at / BLOCK], n);
        from = 'bucket, gzip';
      }
      if (!b && l) {
        b = await read(l.url, at, n, { mode: 'cors', credentials: 'omit' });
        from = 'bucket';
      }
      if (!b) {
        b = await read(u, at, n, {});
        from = 'server';
      }
    } finally {
      if (!lane) game--;
    }
    if (lane < 2) dl.postMessage({ s: 0, id, u, k: b ? b.length : -1, ms: performance.now() - t0, ahead: lane, from });
    if (b && cache) await cache.put(key, new Response(b)).catch(() => {});
    return b;
  })();
  inflight.set(key, p);
  p.finally(() => inflight.delete(key));
  return p;
}

// a range of a file, or null
async function read(url, at, n, opts) {
  try {
    const r = await fetch(url, { ...opts, headers: { Range: 'bytes=' + at + '-' + (at + n - 1) } });
    if (r.status == 206 || (r.status == 200 && at == 0)) {
      const b = new Uint8Array(await r.arrayBuffer());
      return b.length >= n ? b.subarray(0, n) : null;
    }
  } catch (e) {}
  return null;
}

// a gzipped block, unpacked; null unless it is n bytes
async function unzip(url, n) {
  try {
    const r = await fetch(url, { mode: 'cors', credentials: 'omit' });
    if (!r.ok) return null;
    const b = new Uint8Array(await new Response(r.body.pipeThrough(new DecompressionStream('gzip'))).arrayBuffer());
    return b.length == n ? b : null;
  } catch (e) {
    return null;
  }
}

// the file's signed links in the bucket ({url, blocks?}), from webserve.py's /sign; null if it has none
function signed(u) {
  const url = new URL(u, location.origin), p = url.pathname.slice(1);
  const have = links.get(p);
  if (have) return have.then((l) => (!l || l.until > Date.now() ? l : (links.delete(p), signed(u))));
  const got = fetch('/sign?t=' + encodeURIComponent(url.searchParams.get('t') || token) + '&p=' + encodeURIComponent(decodeURIComponent(p)))
    .then((r) => (r.ok ? r.json() : null))
    .then((j) => (j && j.url ? { ...j, until: Date.now() + 5 * 3600e3 } : null))
    .catch(() => null);
  links.set(p, got);
  return got;
}

function blocksOf(size, first, count) {
  const out = [];
  for (let at = first * BLOCK; at < size && out.length < count; at += BLOCK) out.push([at, Math.min(BLOCK, size - at)]);
  return out;
}

async function serve(r) {
  if (r.open) {
    // the game opened the file: its first blocks, all at once
    for (const [at, n] of blocksOf(r.size, 0, OPEN_BLOCKS)) get(r.u, r.k, at, n, 1);
    return;
  }
  const b = get(r.u, r.k, r.at, r.n, 0);
  for (const [at, n] of blocksOf(r.size, r.at / BLOCK + 1, AHEAD)) get(r.u, r.k, at, n, 1);
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

// --- the background download ------------------------------------------------------------------------------
// a path as httpfs_web.c's url_of writes it: each byte but letters, digits and . - _ as %XX
const enc = (rel) =>
  rel.split('/').map((c) => Array.from(new TextEncoder().encode(c), (b) =>
    /[A-Za-z0-9._-]/.test(String.fromCharCode(b)) ? String.fromCharCode(b) : '%' + b.toString(16).toUpperCase().padStart(2, '0')).join('')).join('/');

async function list(route) {
  const r = await fetch('/' + route + '/index?t=' + encodeURIComponent(token));
  if (!r.ok) return [];
  const out = [];
  for (const line of (await r.text()).split('\n')) {
    const [rel, size, mtime] = line.split('\t');
    if (!rel || !(+size >= 0)) continue;
    const e = enc(rel);
    out.push({ u: location.origin + '/' + route + '/' + e + '?t=' + encodeURIComponent(token),
      k: route + '/' + e + '?v=' + (mtime || 0) + '.' + size, size: +size, rel: rel.toLowerCase() });
  }
  return out;
}

// the order: the overlays (small, and what the server changed), the game's folders, sound last
const rank = (f) => (f.u.includes('/dats/') ? 0 : /^(rom|ftable|vtable)/.test(f.rel) ? 1 : /^sound/.test(f.rel) ? 3 : 2);

function report() {
  if (!bg) return;
  const el = (performance.now() - bg.t0) / 1000;
  dl.postMessage({ pf: { on: bg.on, files: bg.files.length, done: bg.done, bytes: bg.bytes, doneBytes: bg.doneBytes,
    rate: bg.fetched / Math.max(el, 1), finished: bg.finished, failed: bg.failed || 0, error: bg.error } });
}

async function prefetch(on) {
  if (!on) {
    if (bg) bg.on = false, report();
    return;
  }
  if (bg && bg.on) return;
  if (!bg || bg.finished) {
    const files = [...(await list('dats')), ...(await list('dat'))].sort((a, b) => rank(a) - rank(b));
    bg = { files, again: [], done: 0, bytes: files.reduce((a, f) => a + f.size, 0), doneBytes: 0, fetched: 0, next: 0 };
  }
  bg.on = true;
  bg.t0 = performance.now();
  bg.fetched = 0;
  try {
    const est = await navigator.storage.estimate();
    if (est.quota - est.usage < bg.bytes - bg.doneBytes) bg.error = `the browser allows ${(est.quota / 1e9).toFixed(1)} GB here, ${((est.quota - est.usage) / 1e9).toFixed(1)} GB free`;
  } catch (e) {}
  const timer = setInterval(report, 1000);
  // a file: false if paused (it goes back in the queue) or failed (left for the game to ask for)
  const one = async (f) => {
    const mark = entry('done/' + f.k, 0);
    if (!(cache && (await cache.match(mark)))) {
      for (const [at, n] of blocksOf(f.size, 0, Infinity)) {
        while (game > 0 && bg.on) await new Promise((r) => setTimeout(r, 50)); // the game's reads first
        if (!bg.on) return bg.again.push(f), false;
        if (!(cache && (await cache.match(entry(f.k, at))))) {
          const b = await get(f.u, f.k, at, n, 2);
          if (!b) return (bg.failed = (bg.failed || 0) + 1), false;
          bg.fetched += n;
        }
      }
      if (cache) await cache.put(mark, new Response('')).catch(() => {});
    }
    bg.doneBytes += f.size;
    bg.done++;
    return true;
  };
  const take = () => bg.again.pop() || (bg.next < bg.files.length ? bg.files[bg.next++] : null);
  await Promise.all(Array.from({ length: BG_FILES }, async () => {
    for (let f; bg.on && (f = take()); ) await one(f);
  }));
  clearInterval(timer);
  if (bg.on && bg.next >= bg.files.length && !bg.again.length) bg.finished = true, bg.on = false;
  report();
}
