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
//  - a small file (under 256 KB) missing is read as its own block, and its pack (webserve.py --bucket-blocks:
//    about 8 MB of its folder's small files in one request) follows in the background into the cache.
//  - the game is answered before its block is put in the cache; until a put lands, the bytes are kept here
//    (fresh). Puts other than the game's go one at a time: Chromium slows matches on a Cache while it puts.
//  - read-ahead and the background download only check a block is there (have, then a match), never read it.
//  - the background download ({prefetch: true} from the page) fills the cache with every file of the
//    install: the packs, then the rest a file at a time, several at once, the game's own reads first; a file
//    done is marked, so it resumes.
//  - each burst of files the game opens (gaps under 3 s) goes to webserve.py (/learn); the first open of a
//    burst asks /follow for the files that came after it before, and fetches them at once.
//  - the downloads overlay (hud.js) hears each fetch, hit and the background download on 'xi-dl'.
const BLOCK = 1 << 20, AHEAD = 3, OPEN_BLOCKS = 16, BG_FILES = 16, BG_PACKS = 16, FOLLOWERS = 8, BURST_MS = 3000;
const CACHE = 'xi-files-v1';
const dl = new BroadcastChannel('xi-dl');
const inflight = new Map(); // key -> Promise<Uint8Array | null>
const links = new Map(); // 'dat/<path>' -> Promise<{ url, blocks?, pack?, until } | null>
const packs = new Map(); // pack id -> Promise<{ got: Map<key, Uint8Array>, written: Promise } | null>, until written
const fresh = new Map(); // cache entry -> its bytes, until its put lands
const have = new Set(); // cache entries known to be there
const HAVE = new Uint8Array(0); // what a read ahead or background get answers for a block already there
let puts = Promise.resolve(); // the non-game puts, one at a time
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
// 2 the background download (not shown one by one). Lanes 1 and 2 answer HAVE for a block already there.
function get(u, k, at, n, lane) {
  const key = entry(k, at);
  if (lane && have.has(key)) return Promise.resolve(HAVE);
  const p = inflight.get(key);
  if (p) return lane ? p : joined(p, key, n);
  const q = fetchBlock(u, k, at, n, lane, key);
  inflight.set(key, q);
  q.finally(() => inflight.delete(key));
  return q;
}

// the game waiting on a read already under way: counted as the game's, so the background download waits
async function joined(p, key, n) {
  game++;
  try {
    const b = await p;
    return b === HAVE ? await hitOf(key, n, 0) : b;
  } finally {
    game--;
  }
}

// the block from here or the cache: its bytes (HAVE, lane > 0), or null if it isn't there
async function hitOf(key, n, lane) {
  const f = fresh.get(key);
  if (f && f.length == n) return lane ? HAVE : f;
  try {
    const hit = cache && (await cache.match(key));
    if (!hit) return null;
    have.add(key);
    if (lane) return HAVE;
    const b = new Uint8Array(await hit.arrayBuffer());
    if (b.length == n) return b;
    have.delete(key);
  } catch (e) {}
  return null;
}

async function fetchBlock(u, k, at, n, lane, key) {
  const hit = await hitOf(key, n, lane);
  if (hit) {
    if (!lane) dl.postMessage({ hit: 1, k: hit.length });
    return hit;
  }
  const id = 'c' + ++seq, t0 = performance.now();
  if (lane < 2) dl.postMessage({ s: 1, id, u, at, n, ahead: lane });
  if (!lane) game++;
  let b = null, from = 'server';
  try {
    const l = bucket ? await signed(u) : null;
    if (l && l.pack && at == 0) {
      if (l.blocks && l.blocks[0]) packGet(l.pack); // its own block below, the rest of the pack behind it
      else {
        const r = await packGet(l.pack);
        b = r && r.got.get(k);
        if (b && b.length != n) b = null;
        if (b) from = 'bucket, pack';
      }
    }
    if (!b && l && l.blocks && l.blocks[at / BLOCK]) {
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
  if (b && from != 'bucket, pack') { // a pack puts its own
    const put = store(key, b, !lane);
    if (lane == 2) await put; // the background download goes no faster than its puts
  }
  return b;
}

// b into the cache as key, readable from here until the put lands; the game's put now, the rest in turn
function store(key, b, now) {
  if (!cache) return Promise.resolve();
  fresh.set(key, b);
  const put = () => cache.put(key, new Response(b.slice())).then(() => have.add(key), () => {})
    .finally(() => fresh.get(key) === b && fresh.delete(key));
  return now ? put() : (puts = puts.then(put));
}

// an empty entry marking something done (a file of the background download, a pack), in turn
function mark(key) {
  if (!cache) return Promise.resolve();
  return (puts = puts.then(() => cache.put(key, new Response('')).then(() => have.add(key), () => {})));
}

// a mark (or block) there?
async function marked(key) {
  if (have.has(key)) return true;
  try {
    if (cache && (await cache.match(key))) return have.add(key), true;
  } catch (e) {}
  return false;
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

// a gzipped block or pack, unpacked; null unless it is n bytes (any, n < 0)
async function unzip(url, n) {
  try {
    const r = await fetch(url, { mode: 'cors', credentials: 'omit' });
    if (!r.ok) return null;
    const b = new Uint8Array(await new Response(r.body.pipeThrough(new DecompressionStream('gzip'))).arrayBuffer());
    return n < 0 || b.length == n ? b : null;
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

// a file as webserve.py describes it ([route, path as written, mtime, size]): its cache key and address
const keyOf = (d) => d[0] + '/' + enc(d[1]) + '?v=' + d[2] + '.' + d[3];
const urlOf = (d) => location.origin + '/' + d[0] + '/' + enc(d[1]) + '?t=' + encodeURIComponent(token);

// a pack ({id, url, members: [[route, path, mtime, size, offset]]}): fetched once, each member put in the cache
// (and marked done for the background download), then the pack marked done and dropped from here; got maps its
// members' keys to their bytes, written settles once they are all in the cache
function packGet(pack) {
  let p = packs.get(pack.id);
  if (p) return p;
  p = (async () => {
    const raw = await unzip(pack.url, -1);
    if (!raw) return null;
    const got = new Map(), waits = [];
    for (const m of pack.members) {
      const k = keyOf(m), b = raw.subarray(m[4], m[4] + m[3]);
      if (b.length != m[3]) continue;
      got.set(k, b);
      waits.push(store(entry(k, 0), b, false), mark(entry('done/' + k, 0)));
    }
    const written = Promise.all(waits).then(() => mark(entry('pack/' + pack.id, 0)))
      .finally(() => packs.delete(pack.id));
    return { got, written };
  })();
  packs.set(pack.id, p);
  p.then((r) => r || packs.delete(pack.id), () => packs.delete(pack.id));
  return p;
}

function blocksOf(size, first, count) {
  const out = [];
  for (let at = first * BLOCK; at < size && out.length < count; at += BLOCK) out.push([at, Math.min(BLOCK, size - at)]);
  return out;
}

async function serve(r) {
  if (r.ping) {
    const c = new Int32Array(mem.buffer, r.cell, 2);
    if (Atomics.compareExchange(c, 0, 0, 2) == 0) Atomics.notify(c, 0);
    return;
  }
  if (r.open) {
    // the game opened the file: its first blocks, all at once
    for (const [at, n] of blocksOf(r.size, 0, OPEN_BLOCKS)) get(r.u, r.k, at, n, 1);
    opened(r.u);
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

// --- bursts: what the game opens together -------------------------------------------------------------------
let burst = [], burstTimer = 0;
function opened(u) {
  const p = decodeURIComponent(new URL(u).pathname.slice(1));
  if (!burst.length) follow(p);
  burst.push(p);
  clearTimeout(burstTimer);
  burstTimer = setTimeout(() => {
    const files = burst;
    burst = [];
    if (files.length >= 5 && token)
      fetch('/learn?t=' + encodeURIComponent(token), { method: 'POST', body: JSON.stringify(files.slice(0, 2000)) }).catch(() => {});
  }, BURST_MS);
}

// the files that came after p before: fetched now, a few at a time (a pack fetched answers its neighbours)
async function follow(p) {
  if (!token || !mem) return;
  let list = [];
  try {
    const r = await fetch('/follow?t=' + encodeURIComponent(token) + '&p=' + encodeURIComponent(p));
    if (r.ok) list = await r.json();
  } catch (e) {}
  if (!list.length) return;
  dl.postMessage({ follow: list.length, p });
  let i = 0;
  await Promise.all(Array.from({ length: FOLLOWERS }, async () => {
    while (i < list.length) {
      const d = list[i++], k = keyOf(d);
      if (await marked(entry('done/' + k, 0))) continue;
      await Promise.all(blocksOf(d[3], 0, OPEN_BLOCKS).map(([at, n]) => get(urlOf(d), k, at, n, 1)));
    }
  }));
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
    rate: bg.fetched / Math.max(el, 1), finished: bg.finished, failed: bg.failed || 0, error: bg.error,
    packs: bg.packs ? bg.packs.length : 0, packsDone: bg.packsDone || 0 } });
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
  if (!bg.packs) {
    try {
      const r = await fetch('/packs?t=' + encodeURIComponent(token));
      bg.packs = r.ok ? await r.json() : [];
    } catch (e) {
      bg.packs = [];
    }
    bg.packs.sort((a, b) => (a.members[0]?.[0] == 'dats' ? 0 : 1) - (b.members[0]?.[0] == 'dats' ? 0 : 1));
    bg.packsDone = 0;
    bg.packNext = 0;
  }
  try {
    const est = await navigator.storage.estimate();
    if (est.quota - est.usage < bg.bytes - bg.doneBytes) bg.error = `the browser allows ${(est.quota / 1e9).toFixed(1)} GB here, ${((est.quota - est.usage) / 1e9).toFixed(1)} GB free`;
  } catch (e) {}
  const timer = setInterval(report, 1000);
  // a file: false if paused (it goes back in the queue) or failed (left for the game to ask for)
  const one = async (f) => {
    const done = entry('done/' + f.k, 0);
    if (!(await marked(done))) {
      for (const [at, n] of blocksOf(f.size, 0, Infinity)) {
        while (game > 0 && bg.on) await new Promise((r) => setTimeout(r, 50)); // the game's reads first
        if (!bg.on) return bg.again.push(f), false;
        const b = await get(f.u, f.k, at, n, 2);
        if (!b) return (bg.failed = (bg.failed || 0) + 1), false;
        if (b !== HAVE) bg.fetched += n;
      }
      await mark(done);
    }
    bg.doneBytes += f.size;
    bg.done++;
    return true;
  };
  // the packs: a few thousand requests for the small files
  await Promise.all(Array.from({ length: BG_PACKS }, async () => {
    while (bg.on && bg.packNext < bg.packs.length) {
      const pk = bg.packs[bg.packNext++];
      while (game > 0 && bg.on) await new Promise((r) => setTimeout(r, 50));
      if (!(await marked(entry('pack/' + pk.id, 0)))) {
        const r = await packGet(pk);
        if (!r) continue;
        await r.written;
        for (const b of r.got.values()) bg.fetched += b.length;
      }
      bg.packsDone++;
    }
  }));
  const take = () => bg.again.pop() || (bg.next < bg.files.length ? bg.files[bg.next++] : null);
  await Promise.all(Array.from({ length: BG_FILES }, async () => {
    for (let f; bg.on && (f = take()); ) await one(f);
  }));
  clearInterval(timer);
  if (bg.on && bg.next >= bg.files.length && !bg.again.length) bg.finished = true, bg.on = false;
  report();
}
