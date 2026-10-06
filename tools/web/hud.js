// The page's view of the game: its size following the window's, and the fps gauge.
//  - size: the canvas fills the window; the game's resolution is the canvas in device pixels, and the menus'
//    is that over a UI scale picked from the window (at least the pixel ratio, so the interface keeps its
//    size on a high-density screen; more on a very tall window, so it never shrinks below ~1200 lines).
//    A resize reaches the game when the window has stopped moving for a moment (web_resize).
//  - the gauge: frames the render thread presented, the worst gap between two, twice a second.
(function () {
  function sizes() {
    const dpr = window.devicePixelRatio || 1;
    const w = Math.max(320, Math.round(innerWidth * dpr)) & ~1, h = Math.max(240, Math.round(innerHeight * dpr)) & ~1;
    const scale = Math.max(dpr, h / 1200);
    return { w, h, mw: Math.round(w / scale) & ~1, mh: Math.round(h / scale) & ~1 };
  }

  function follow(M) {
    let timer = 0, last = sizes();
    const apply = () => {
      const s = sizes();
      if (s.w === last.w && s.h === last.h && s.mw === last.mw && s.mh === last.mh) return;
      last = s;
      M._web_resize(s.w, s.h, s.mw, s.mh);
    };
    window.addEventListener('resize', () => { clearTimeout(timer); timer = setTimeout(apply, 150); });
    matchMedia(`(resolution: ${window.devicePixelRatio}dppx)`).addEventListener?.('change', apply);
  }

  function gauge(M) {
    const el = document.createElement('div');
    el.id = 'fps';
    el.title = 'Frames a second (the worst frame of the last half second). Click to hide.';
    el.style.cssText = 'position:fixed;top:8px;right:10px;z-index:3;padding:3px 8px;border-radius:4px;' +
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
    bar.style.cssText = 'display:flex;gap:8px;align-items:center;white-space:nowrap';
    const btn = document.createElement('button');
    btn.style.cssText = 'font:600 11px ui-monospace,Menlo,monospace;padding:2px 8px;border-radius:4px;border:0;' +
      'background:#c9a54a;color:#111;cursor:pointer';
    const prog = document.createElement('span');
    bar.append(btn, prog);
    el.append(text, bar);
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
    let total = 0, count = 0, hits = 0, hitBytes = 0, lastHit = -1e9;
    const name = (u) => decodeURIComponent(u.replace(/^.*?\/(dat|app|dats)\//, '$1/').replace(/\?.*$/, ''));
    const mb = (b) => (b / 1048576).toFixed(b < 10485760 ? 2 : 1);
    const gb = (b) => (b / 1073741824).toFixed(1);
    new BroadcastChannel('xi-dl').onmessage = ({ data: d }) => {
      const now = performance.now();
      if (d.pf) return void (pf = d.pf);
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
      text.style.display = lines.length ? '' : 'none';
      if (pf && pf.finished) {
        btn.style.display = 'none';
        prog.textContent = `all game data in this browser (${gb(pf.bytes)} GB)` + (pf.failed ? `, ${pf.failed} files failed` : '');
      } else {
        btn.style.display = '';
        btn.textContent = want ? 'Pause download' : 'Download all game data';
        prog.textContent = pf
          ? `${gb(pf.doneBytes)} of ${gb(pf.bytes)} GB · ${pf.done}/${pf.files} files` +
            (pf.on ? ` · ${(pf.rate / 1048576).toFixed(1)} MB/s` : ' · paused') + (pf.error ? ' · ' + pf.error : '')
          : want ? 'starting…' : '';
      }
    }
    setInterval(draw, 250);
    draw();
  }

  window.xiHud = { sizes, follow, gauge, downloads };
})();
