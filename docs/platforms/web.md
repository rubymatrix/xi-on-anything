# Web browser (experimental, self-hosted only)

The game as 32-bit WebAssembly, drawn with WebGPU, in a browser tab. A small server you run yourself,
`tools/webserve.py`, serves the page and your game files, and carries the game's traffic to your
LandSandBoat server over a WebSocket.

> [!WARNING]
> **There is no public instance, and there must never be one.** The WebAssembly is translated from
> your own `FFXiMain.dll`, and the server hands out your game files. Build it from your own install
> and host it for yourself only. `webserve.py` listens on `127.0.0.1` and wants a token on every
> request; to reach it from other devices, put it behind a private network or a sign-in you control
> (below), never on the open internet.

Branch: `feat/web` (`git checkout feat/web`). The full reference, with the private bucket and every
option, is `docs/web.md` on that branch; the design is `docs/web-port-plan.md`.

## What you need

- **To build and host**: a Mac (Apple silicon) or Linux machine. Hosting from Windows is untested.
- Python 3.12 or later with `capstone` and `pefile`.
- [Emscripten](https://emscripten.org/docs/getting_started/downloads.html) 6.0.11.
- Your `FINAL FANTASY XI` folder and a LandSandBoat server.
- **To play**: desktop Chrome or Edge with WebGPU. Safari 26 and Firefox are untested; phones and
  tablets are out of scope for now.

## 1. Install Emscripten

```bash
git clone https://github.com/emscripten-core/emsdk.git ../emsdk
../emsdk/emsdk install 6.0.11 && ../emsdk/emsdk activate 6.0.11
source ../emsdk/emsdk_env.sh        # in every new terminal you build from
```

## 2. Build

```bash
git checkout feat/web
python3 tools/build_posix.py prepare --game /path/to/"FINAL FANTASY XI"
python3 tools/build_web.py --game /path/to/"FINAL FANTASY XI"
```

The page and the WebAssembly go to `build/web/` (about 33 MB).

## 3. Run it on this machine

```bash
python3 tools/webserve.py --server <your server, name or IP> --game /path/to/"FINAL FANTASY XI"
```

It prints an address like `http://127.0.0.1:8417/#t=<token>`. Open it in Chrome or Edge; a sign-in
form for your server's account comes up.

| Option | What it does |
| --- | --- |
| `--server name` | The game server. Repeat it for each host the server sends you to (lobby, map servers): the bridge connects only to these. |
| `--dats folder` | A DAT overlay folder (repeatable; the first wins). |
| `--port 8417` | The port on `127.0.0.1`. |
| `--token value` | A fixed token, so a bookmark keeps working (or `XI_WEB_TOKEN`). |
| `--origin https://...` | The address a proxy of yours serves it under (repeatable). |
| `--env file` | `KEY=value` lines read into the environment; `~/.config/xi-web.env` by default if it exists. |

Keep the token like a password. Settings changed in game aren't kept between visits yet.

## 4. Play from your other devices (privately)

The page needs HTTPS (or `127.0.0.1`) for its threads' shared memory, and WebSockets for the network.
Use a fixed `--token` and set `--origin` to the address you'll open.

**Tailscale** (your devices only):

```bash
tailscale serve --bg 8417
python3 tools/webserve.py --server ... --game ... \
    --origin https://<machine>.<tailnet>.ts.net --token <your token>
```

Open `https://<machine>.<tailnet>.ts.net/#t=<your token>` on any device in your tailnet.

**Cloudflare Tunnel + Cloudflare Access**: route a hostname of yours to `http://127.0.0.1:8417`
(an `ingress` entry in `~/.cloudflared/config.yml`), add a self-hosted Access application for it
that allows only you, add no cache rules, and run `webserve.py --origin https://<that hostname>`.

**Any other reverse proxy** (Caddy, nginx): it must serve HTTPS, pass WebSocket upgrades on `/net`,
keep the `Cross-Origin-Opener-Policy` and `Cross-Origin-Embedder-Policy` headers, and put a sign-in
in front. Never expose `webserve.py` with only the token in front of it.

For faster loading far from home, files can come from a private S3-compatible bucket
(`tools/bucket.py`); see `docs/web.md` on the branch. In the page, **Download all game data** (the
gear, top right) fills the browser's storage so zoning reads nothing over the network.

## Page options

Add them after the token: `#t=<token>&ui=1.25`.

| Option | What it does |
| --- | --- |
| `ui=1.25` | Interface size (0.5 to 3). |
| `shape=16:9` | Keep the interface 16:9 (or `4:3`, `off`) in a wide window. |
| `maxpx=8.3` | The most pixels rendered, in millions (default about 4K). |
| `cache=clear` | Empty the file cache once. `cache=0` reads everything from the network. |
| `fps`, `dl`, `log`, `status` `=0`/`=1` | Hide or show the fps counter, downloads list, log, status line. |

The game follows the window's size; resize, move between screens or zoom, and it adapts once the
window stops changing.

## Updating

- New code: `git pull`, `build_web.py`, restart `webserve.py`.
- New client version: `prepare`, then `build_web.py`.

## Troubleshooting

- **"This page needs cross-origin isolation"**: not on HTTPS or `127.0.0.1`, or a proxy dropped the
  COOP/COEP headers.
- **403 "bad token"**: use the token `webserve.py` just printed, or set a fixed `--token`.
- **403 "other origins may not use this server"**: set `--origin` to the address in the address bar.
- **`net: refused <address> (not the game server)`**: the server sent you to a host not in `--server`;
  add it.
- **Black screen at 0 fps**: open the log (gear, or `&log=1`); a crash prints `thread crashed:` and
  its stack there, as do WebGPU errors.
- **Slow zoning**: the overlay at the top left lists each file read and where it came from. Use a
  bucket, or Download all game data.
