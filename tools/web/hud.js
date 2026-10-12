// The page's view of the game: its size following the window's, and the fps gauge.
//  - resolution: the window in device pixels, at most MAX_PIXELS of them (about 4K; #maxpx=<millions>
//    changes it), so a large high-density window doesn't render 15 million pixels a frame.
//  - the interface (the game's menu size, which its windows are laid out in): one interface pixel a CSS
//    pixel, times the player's interface size (#ui=1.25 is a quarter bigger), as the native game is at
//    100%. Never under MIN_UI (the game's layouts need 512 x 448; a smaller window gets a smaller interface,
//    not a cut-off one) and never over MAX_UI_H lines (a very tall window doesn't make it tiny). A wide
//    window's interface keeps the shape Config > Modern > Interface Shape gives it (host/modern.c).
//  - a resize reaches the game once the window has stopped moving for a moment (web_resize); meanwhile the
//    last frame is letterboxed in the window (the canvas's object-fit), not stretched.
//  - the gauge: frames the render thread presented, the worst gap between two, twice a second.
(function () {
  const MIN_UI_W = 640, MIN_UI_H = 480, MAX_UI_H = 1200;
  const hash = new URLSearchParams(location.hash.slice(1));
  const uiSize = Math.min(3, Math.max(0.5, Number(hash.get('ui')) || 1));
  const MAX_PIXELS = Math.max(0.5, Number(hash.get('maxpx')) || 8.3) * 1e6;

  function sizes() {
    const dpr = window.devicePixelRatio || 1;
    let cw = innerWidth, ch = innerHeight;
    if (!cw || !ch) cw = screen.availWidth || 1280, ch = screen.availHeight || 720; // a page opened hidden
    let px = dpr;
    if (cw * ch * dpr * dpr > MAX_PIXELS) px = Math.sqrt(MAX_PIXELS / (cw * ch));
    const w = Math.max(320, Math.round(cw * px)) & ~1, h = Math.max(240, Math.round(ch * px)) & ~1;
    let u = uiSize; // CSS pixels an interface pixel
    if (ch / u > MAX_UI_H) u = ch / MAX_UI_H;
    if (cw / u < MIN_UI_W || ch / u < MIN_UI_H) u = Math.min(cw / MIN_UI_W, ch / MIN_UI_H);
    return { w, h, mw: Math.round(cw / u) & ~1, mh: Math.round(ch / u) & ~1 };
  }

  // from the size the game started with (M.startSizes, index.html), not the window's now: it may have changed
  // while the game loaded
  function follow(M) {
    let timer = 0, last = M.startSizes || sizes();
    const apply = () => {
      const s = sizes();
      if (s.w === last.w && s.h === last.h && s.mw === last.mw && s.mh === last.mh) return;
      last = s;
      M.viewSize = [s.w, s.h];
      M._web_resize(s.w, s.h, s.mw, s.mh);
    };
    window.addEventListener('resize', () => { clearTimeout(timer); timer = setTimeout(apply, 150); });
    // the window moved to a screen of another density (or the page was zoomed): asked again each time,
    // for the density it has now
    const density = () => {
      matchMedia(`(resolution: ${window.devicePixelRatio}dppx)`).addEventListener('change', () => { apply(); density(); }, { once: true });
    };
    density();
    apply();
  }

  function gauge(M) {
    const el = document.createElement('div');
    el.id = 'fps';
    el.title = 'Frames a second (the worst frame of the last half second). Click to hide.';
    el.style.cssText = 'position:fixed;top:8px;right:40px;z-index:3;padding:3px 8px;border-radius:4px;' +
      'font:600 12px/1.3 ui-monospace,Menlo,monospace;background:rgba(0,0,0,.55);color:#9be59b;cursor:pointer;' +
      'user-select:none;pointer-events:auto';
    el.addEventListener('click', () => (el.style.opacity = el.style.opacity === '0.15' ? '1' : '0.15'));
    document.body.appendChild(el);
    let frames = M._web_hud_frames(), at = performance.now();
    setInterval(() => {
      const f = M._web_hud_frames(), now = performance.now();
      const fps = ((f - frames) * 1000) / (now - at);
      const worst = M._web_hud_worst_us() / 1000;
      frames = f, at = now;
      el.textContent = `${fps.toFixed(0)} fps · worst ${worst.toFixed(1)} ms`;
      el.style.color = fps >= 55 ? '#9be59b' : fps >= 40 ? '#e5d49b' : '#e59b9b';
    }, 500);
  }

  // the downloads overlay: the file reads in flight (dlcache.js, or httpfs_web.c without the cache, announces
  // each on the 'xi-dl' channel; » a read ahead), the last few finished with their time and where from, the
  // rate over the last two seconds, the reads the cache answered, and the background download's button and
  // progress.
  function downloads(worker) {
    const el = document.createElement('div');
    el.id = 'dl';
    el.style.cssText = 'position:fixed;top:8px;left:10px;z-index:3;padding:4px 8px;border-radius:4px;max-width:46vw;' +
      'font:500 11px/1.35 ui-monospace,Menlo,monospace;background:rgba(0,0,0,.55);color:#cfd8e3;' +
      'user-select:none;pointer-events:auto;overflow:hidden';
    const text = document.createElement('div');
    text.style.cssText = 'white-space:pre;cursor:pointer';
    text.title = 'File downloads. Click to dim.';
    text.addEventListener('click', () => (el.style.opacity = el.style.opacity === '0.15' ? '1' : '0.15'));
    // the background download of every game file (dlcache.js), remembered for the next visit
    const bar = document.createElement('div');
    bar.style.cssText = 'display:flex;flex-wrap:wrap;gap:6px 8px;align-items:center';
    const btn = document.createElement('button');
    btn.style.cssText = 'font:600 11px ui-monospace,Menlo,monospace;padding:2px 8px;border-radius:4px;border:0;' +
      'background:#c9a54a;color:#111;cursor:pointer';
    const prog = document.createElement('span');
    bar.append(btn, prog);
    el.append(text);
    document.body.appendChild(el);
    let pf = null, want = false;
    try { want = localStorage.getItem('xi.prefetch') === '1'; } catch (e) {}
    const set = (on) => {
      want = on;
      try { localStorage.setItem('xi.prefetch', on ? '1' : '0'); } catch (e) {}
      worker?.postMessage({ prefetch: on });
      draw();
    };
    btn.addEventListener('click', () => set(!want));
    if (!worker) bar.style.display = 'none';
    else if (want) setTimeout(() => worker.postMessage({ prefetch: true }), 3000);

    const live = new Map(), done = [], hist = [];
    let total = 0, count = 0, hits = 0, hitBytes = 0, lastHit = -1e9, followed = null;
    const name = (u) => decodeURIComponent(u.replace(/^.*?\/(dat|app|dats)\//, '$1/').replace(/\?.*$/, ''));
    const mb = (b) => (b / 1048576).toFixed(b < 10485760 ? 2 : 1);
    const gb = (b) => (b / 1073741824).toFixed(1);
    new BroadcastChannel('xi-dl').onmessage = ({ data: d }) => {
      const now = performance.now();
      if (d.pf) return void (pf = d.pf);
      if (d.follow) return void (followed = { n: d.follow, p: d.p, t: now });
      if (d.hit) return void (hits++, (hitBytes += d.k), (lastHit = now));
      if (d.s) return void live.set(d.id, { ...d, t: now });
      live.delete(d.id);
      if (d.k > 0) total += d.k, hist.push([now, d.k]);
      count++;
      done.unshift({ ...d, t: now });
      done.length = Math.min(done.length, 5);
    };
    function draw() {
      const now = performance.now();
      while (hist.length && now - hist[0][0] > 2000) hist.shift();
      const rate = hist.reduce((a, h) => a + h[1], 0) / 2;
      const lines = [];
      const busy = live.size || now - Math.max(done[0]?.t ?? -1e9, lastHit) < 4000;
      if (followed && now - followed.t < 6000) lines.push(`learned: ${name('/' + followed.p)} → fetching ${followed.n} files that follow it`);
      if (busy) {
        lines.push(`files: ${live.size} loading · ${(rate / 1048576).toFixed(2)} MB/s · ${count} fetched, ${mb(total)} MB` +
          ` · cache ${hits} hits, ${mb(hitBytes)} MB`);
        for (const r of live.values())
          lines.push(`  ${r.ahead ? '»' : '⇣'} ${name(r.u)}  ${r.n >= 0 ? mb(r.n) + ' MB @' + mb(r.at) : 'all'}  ${((now - r.t) / 1000).toFixed(1)} s`);
        for (const r of done)
          if (now - r.t < 4000)
            lines.push(`  ${r.k < 0 ? '✗' : '✓'} ${name(r.u)}  ${r.k > 0 ? mb(r.k) + ' MB ' : ''}${r.ms.toFixed(0)} ms${r.from ? ' · ' + r.from : ''}`);
      }
      text.textContent = lines.join('\n');
      el.style.display = lines.length ? '' : 'none';
      if (pf && pf.finished) {
        btn.style.display = 'none';
        prog.textContent = `all game data in this browser (${gb(pf.bytes)} GB)` + (pf.failed ? `, ${pf.failed} files failed` : '');
      } else {
        btn.style.display = '';
        btn.textContent = want ? 'Pause download' : 'Download all game data';
        prog.textContent = pf
          ? (pf.packs && pf.packsDone < pf.packs ? `small files: pack ${pf.packsDone}/${pf.packs} · ` : '') +
            `${gb(pf.doneBytes)} of ${gb(pf.bytes)} GB · ${pf.done}/${pf.files} files` +
            (pf.on ? ` · ${(pf.rate / 1048576).toFixed(1)} MB/s` : ' · paused') + (pf.error ? ' · ' + pf.error : '')
          : want ? 'starting…' : '';
      }
    }
    setInterval(draw, 250);
    draw();
    return bar;
  }

  // --- what the page shows over the game ----------------------------------------------------------------------
  // The fps gauge, the file downloads, the log and the status line, each on or off: a gear at the top right
  // opens the switches (and the game data download), remembered in this browser. By default the fps gauge and
  // the status line are on, the log and the file downloads off (the log comes back on its own for an error:
  // index.html). The address's options win: #fps, #dl, #log, #status =0 or =1 (#nolog=1 too).
  const OVERLAYS = [['fps', 'FPS counter'], ['dl', 'File downloads'], ['log', 'Log'], ['status', 'Status line']];
  const DEFAULTS = { fps: true, dl: false, log: false, status: true };
  const shown = (() => {
    let saved = {};
    try { saved = JSON.parse(localStorage.getItem('xi.overlays') || '{}'); } catch (e) {}
    const out = {};
    for (const [k] of OVERLAYS) {
      const q = hash.get(k), no = k === 'log' ? hash.get('nolog') : null;
      out[k] = q != null ? q !== '0' : no != null ? no === '0' : typeof saved[k] === 'boolean' ? saved[k] : DEFAULTS[k];
    }
    return out;
  })();
  const applyShown = () => { for (const [k] of OVERLAYS) document.body.classList.toggle('hide-' + k, !shown[k]); };
  if (document.body) applyShown();
  else document.addEventListener('DOMContentLoaded', applyShown);

  function overlays(bar) {
    const gear = document.createElement('button');
    gear.id = 'xi-gear';
    gear.title = 'Overlays and game data';
    gear.textContent = '\u2699';
    gear.style.cssText = 'position:fixed;top:6px;right:8px;z-index:5;width:24px;height:24px;padding:0;border:0;border-radius:4px;' +
      'font:16px/24px system-ui,sans-serif;background:rgba(0,0,0,.55);color:#cfd8e3;cursor:pointer;opacity:.45';
    gear.addEventListener('mouseenter', () => (gear.style.opacity = '1'));
    gear.addEventListener('mouseleave', () => (gear.style.opacity = isOpen() ? '1' : '.45'));
    const panel = document.createElement('div');
    panel.style.cssText = 'position:fixed;top:36px;right:8px;z-index:5;width:250px;padding:10px 12px;border-radius:6px;' +
      'font:12px/1.4 system-ui,sans-serif;background:rgba(14,17,24,.94);color:#d8dbe2;border:1px solid #2c3242;' +
      'box-shadow:0 6px 24px rgba(0,0,0,.5);display:none;gap:6px';
    const isOpen = () => panel.style.display !== 'none';
    const head = (t) => {
      const h = document.createElement('div');
      h.textContent = t;
      h.style.cssText = 'font-weight:600;color:#c9a24a';
      return h;
    };
    panel.append(head('Show over the game'));
    for (const [k, label] of OVERLAYS) {
      const row = document.createElement('label');
      row.style.cssText = 'display:flex;gap:8px;align-items:center;cursor:pointer';
      const box = document.createElement('input');
      box.type = 'checkbox';
      box.checked = shown[k];
      box.addEventListener('change', () => {
        shown[k] = box.checked;
        applyShown();
        try { localStorage.setItem('xi.overlays', JSON.stringify(shown)); } catch (e) {}
      });
      row.append(box, label);
      panel.append(row);
    }
    if (bar) {
      panel.append(head('Game data'));
      bar.style.font = '11px/1.4 ui-monospace,Menlo,monospace';
      panel.append(bar);
    }
    const toggle = (open) => {
      panel.style.display = open ? 'grid' : 'none';
      gear.style.opacity = open ? '1' : '.45';
    };
    gear.addEventListener('click', (e) => { e.stopPropagation(); toggle(!isOpen()); });
    document.addEventListener('mousedown', (e) => { if (isOpen() && !panel.contains(e.target) && e.target !== gear) toggle(false); });
    // keys typed here are the panel's, not the game's
    panel.addEventListener('keydown', (e) => e.stopPropagation());
    panel.addEventListener('keyup', (e) => e.stopPropagation());
    document.body.append(gear, panel);
  }

  window.xiHud = { sizes, follow, gauge, downloads, overlays };
})();
