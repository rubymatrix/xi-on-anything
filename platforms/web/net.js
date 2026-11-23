// The browser build's network (runtime/portable/net_web.c): one WebSocket to the local server's /net,
// on the main thread, carrying every socket's frames. C hands frames over with wn_js_send (proxied
// here); frames from the server go back with _wn_deliver. The URL: Module.netUrl, else XI_NET_URL
// (Node), else /net on the page's own server with the page's token (#t=...).
addToLibrary({
  $wnNet: {
    ws: null,
    pending: [],
    url() {
      if (Module['netUrl']) return Module['netUrl'];
      if (ENVIRONMENT_IS_NODE && process.env['XI_NET_URL']) return process.env['XI_NET_URL'];
      const t = new URLSearchParams(location.hash.slice(1)).get('t') || '';
      return (location.protocol === 'https:' ? 'wss://' : 'ws://') + location.host + '/net?t=' + encodeURIComponent(t);
    },
    deliver(bytes) {
      const p = _malloc(bytes.length);
      new Uint8Array(wasmMemory.buffer).set(bytes, p);
      _wn_deliver(p, bytes.length);
    },
    open() {
      const ws = new WebSocket(wnNet.url());
      ws.binaryType = 'arraybuffer';
      ws.onopen = () => {
        for (const f of wnNet.pending) ws.send(f);
        wnNet.pending = [];
      };
      ws.onmessage = (e) => wnNet.deliver(new Uint8Array(e.data));
      ws.onclose = ws.onerror = () => {
        if (wnNet.ws !== ws) return;
        wnNet.ws = null;
        wnNet.pending = [];
        wnNet.deliver(new Uint8Array([0x85, 0, 0, 0, 0])); // every socket fails
      };
      wnNet.ws = ws;
    },
    send(bytes) {
      if (!wnNet.ws) wnNet.open();
      if (wnNet.ws.readyState === 1) wnNet.ws.send(bytes);
      else wnNet.pending.push(bytes);
    },
  },
  wn_js_send__proxy: 'async',
  wn_js_send__deps: ['$wnNet', 'malloc', 'free', 'wn_deliver'],
  wn_js_send: (p, n) => {
    const bytes = new Uint8Array(wasmMemory.buffer).slice(p, p + n);
    _free(p);
    wnNet.send(bytes);
  },
});
