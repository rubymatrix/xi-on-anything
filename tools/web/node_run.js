// Runs the Node build (tools/build_web.py --node) headless, for tests: the game's arguments as given,
// the network through XI_NET_URL (tools/webserve.py's /net), and XI_ENTER=<seconds>: press Enter every
// 2 s for that long (through the lobby to the zone, as tools/replay.py does with the control port), and
// XI_QUIT=<seconds>: close the window then.
//   XI_NET_URL='ws://127.0.0.1:8417/net?t=...' node tools/web/node_run.js build/web-node/host64.js --game ...
const path = require('path');
const entry = path.resolve(process.argv[2]);
process.argv.splice(2, 1);
const enterFor = Number(process.env.XI_ENTER || 0) * 1000;
globalThis.xiModule = {
  onRuntimeInitialized() {
    const Module = this;
    if (process.env.XI_DEBUG) console.error("[node_run] runtime ready");
    Module._web_focus(1); // the page has focus, as a window that was just shown
    // XI_QUIT=<seconds>: close the window then (the game exits as from Cmd+Q), for profiles written at exit
    if (process.env.XI_QUIT) setTimeout(() => Module._web_close(), Number(process.env.XI_QUIT) * 1000);
    if (!enterFor) return;
    const start = Date.now();
    const SDL_SCANCODE_RETURN = 40;
    const t = setInterval(() => {
      if (Date.now() - start > enterFor) return clearInterval(t);
      if (process.env.XI_DEBUG) console.error("[node_run] Enter");
      Module._web_key(SDL_SCANCODE_RETURN, 1, 0);
      setTimeout(() => Module._web_key(SDL_SCANCODE_RETURN, 0, 0), 80);
    }, 2000);
  },
};
require(entry);
