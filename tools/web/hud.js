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

  window.xiHud = { sizes, follow, gauge };
})();
