// The page's input for the game (runtime/portable/sdl_web.c, web_bridge.h): keys by their place on the
// keyboard (KeyboardEvent.code, as SDL scancodes), typed text, the mouse in the canvas's device pixels, and
// the first standard-mapped gamepad, polled each animation frame. Loaded by index.html before host64.js;
// xiInput.start(Module, canvas) once the runtime is up.
(function () {
  // KeyboardEvent.code -> SDL_Scancode (USB HID usage ids)
  const SC = {};
  'ABCDEFGHIJKLMNOPQRSTUVWXYZ'.split('').forEach((c, i) => (SC['Key' + c] = 4 + i));
  '1234567890'.split('').forEach((c, i) => (SC['Digit' + c] = 30 + i));
  Object.assign(SC, {
    Enter: 40, Escape: 41, Backspace: 42, Tab: 43, Space: 44, Minus: 45, Equal: 46, BracketLeft: 47, BracketRight: 48,
    Backslash: 49, Semicolon: 51, Quote: 52, Backquote: 53, Comma: 54, Period: 55, Slash: 56, CapsLock: 57,
    PrintScreen: 70, ScrollLock: 71, Pause: 72, Insert: 73, Home: 74, PageUp: 75, Delete: 76, End: 77, PageDown: 78,
    ArrowRight: 79, ArrowLeft: 80, ArrowDown: 81, ArrowUp: 82, NumLock: 83, NumpadDivide: 84, NumpadMultiply: 85,
    NumpadSubtract: 86, NumpadAdd: 87, NumpadEnter: 88, Numpad0: 98, NumpadDecimal: 99, IntlBackslash: 100,
    ContextMenu: 101, NumpadEqual: 103, ControlLeft: 224, ShiftLeft: 225, AltLeft: 226, MetaLeft: 227,
    ControlRight: 228, ShiftRight: 229, AltRight: 230, MetaRight: 231,
  });
  for (let i = 1; i <= 12; i++) SC['F' + i] = 57 + i;
  for (let i = 1; i <= 9; i++) SC['Numpad' + i] = 88 + i;
  // keys left to the browser even while playing
  const KEEP = new Set(['F11', 'F12']);

  // Gamepad API standard mapping -> SDL_GamepadButton bits
  const PAD = [0, 1, 2, 3, 9, 10, -1, -1, 4, 6, 7, 8, 11, 12, 13, 14, 5];

  // the sound: an AudioContext may only start from a click or a key, so the first one starts it
  let audio = null;
  function startAudio(M) {
    if (audio) {
      if (audio.state === 'suspended') audio.resume();
      return;
    }
    audio = new AudioContext({ sampleRate: 48000, latencyHint: 'interactive' });
    audio.audioWorklet.addModule('audio-worklet.js?v=__XI_BUILD__').then(() => {
      const node = new AudioWorkletNode(audio, 'xi-audio', { outputChannelCount: [2] });
      node.port.postMessage({ mem: M.HEAPU8.buffer, ring: M._web_audio_ring() });
      node.connect(audio.destination);
    });
  }

  function start(M, canvas) {
    const dpr = () => window.devicePixelRatio || 1;
    window.addEventListener('pointerdown', () => startAudio(M));
    window.addEventListener('keydown', () => startAudio(M));
    const down = new Set();
    const key = (e, isDown) => {
      const sc = SC[e.code];
      if (KEEP.has(e.code)) return;
      e.preventDefault();
      if (sc === undefined) return;
      if (isDown) down.add(sc); else down.delete(sc);
      M._web_key(sc, isDown ? 1 : 0, e.repeat ? 1 : 0);
      // typed text: one character, not a shortcut
      if (isDown && e.key.length === 1 && !e.ctrlKey && !e.metaKey && !e.altKey) {
        const p = M.stringToNewUTF8(e.key);
        M._web_text(p);
        M._free(p);
      }
    };
    window.addEventListener('keydown', (e) => key(e, true));
    window.addEventListener('keyup', (e) => key(e, false));
    const releaseAll = () => {
      for (const sc of down) M._web_key(sc, 0, 0);
      down.clear();
    };
    window.addEventListener('blur', () => { releaseAll(); M._web_focus(0); });
    window.addEventListener('focus', () => M._web_focus(1));
    document.addEventListener('visibilitychange', () => { if (document.hidden) releaseAll(); });

    const pos = (e) => {
      const r = canvas.getBoundingClientRect();
      return [(e.clientX - r.left) * dpr(), (e.clientY - r.top) * dpr()];
    };
    canvas.addEventListener('mousemove', (e) => { const [x, y] = pos(e); M._web_mouse_move(x, y); });
    canvas.addEventListener('mousedown', (e) => {
      e.preventDefault();
      const [x, y] = pos(e);
      M._web_mouse_move(x, y);
      M._web_mouse_button([1, 2, 3, 4, 5][e.button] || 1, 1);
      window.focus();
    });
    window.addEventListener('mouseup', (e) => M._web_mouse_button([1, 2, 3, 4, 5][e.button] || 1, 0));
    canvas.addEventListener('contextmenu', (e) => e.preventDefault());
    canvas.addEventListener('wheel', (e) => {
      e.preventDefault();
      // notches: a line or page mode wheel moves one a step; pixels, about 100 a notch
      const d = e.deltaMode === 0 ? e.deltaY / 100 : e.deltaY;
      M._web_mouse_wheel(-d);
    }, { passive: false });

    let last = '';
    const poll = () => {
      const pads = navigator.getGamepads ? navigator.getGamepads() : [];
      let p = null;
      for (const g of pads) if (g && g.connected && g.mapping === 'standard') { p = g; break; }
      let state;
      if (!p) state = '0';
      else {
        let bits = 0;
        p.buttons.forEach((b, i) => { if (b.pressed && PAD[i] >= 0) bits |= 1 << PAD[i]; });
        const ax = (v) => Math.max(-32768, Math.min(32767, Math.round(v * 32767)));
        const tr = (b) => (b ? Math.round(b.value * 32767) : 0);
        state = [1, bits >>> 0, ax(p.axes[0] || 0), ax(p.axes[1] || 0), ax(p.axes[2] || 0), ax(p.axes[3] || 0),
          tr(p.buttons[6]), tr(p.buttons[7])].join(',');
      }
      if (state !== last) {
        last = state;
        const v = state.split(',').map(Number);
        if (v[0]) M._web_gamepad(1, v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
        else M._web_gamepad(0, 0, 0, 0, 0, 0, 0, 0);
      }
      requestAnimationFrame(poll);
    };
    requestAnimationFrame(poll);
  }
  window.xiInput = { start };
})();
