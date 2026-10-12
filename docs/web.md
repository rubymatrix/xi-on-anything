# Browser (experimental, self-hosted only)

The browser build runs the recompiled client in a browser tab: the game as 32-bit WebAssembly, drawn
with WebGPU. A small server you run yourself, `tools/webserve.py`, serves the page and your game
files, and carries the game's network traffic to your LandSandBoat server over a WebSocket.

**There is no public instance, and there must never be one.** The WebAssembly is translated from your
own `FFXiMain.dll` (Square Enix code), and the server hands out your game files. You build it from
your own install and host it for yourself, on your own machine, reachable only by you. That is why
`webserve.py` listens on `127.0.0.1` only and every request needs a token: to use it from your other
devices you put it behind a private network or a sign-in you control (below), never on the open
internet.

How it is built and why is in [web-port-plan.md](web-port-plan.md).

## What you need

- A Mac (Apple silicon) or Linux machine to build and host it. Windows hosting is untested.
- Python 3.12 or later, with `capstone` and `pefile` (`pip3 install capstone pefile`).
- [Emscripten](https://emscripten.org/docs/getting_started/downloads.html) (tested with 6.0.11).
- A `FINAL FANTASY XI` folder of a supported client build (README: Rules), and a LandSandBoat server
  to play on.
- To play: a desktop Chrome or Edge with WebGPU. Safari 26 and Firefox are untested; phones and
  tablets are out of scope for now.

## Build

```sh
git clone https://github.com/emscripten-core/emsdk.git ../emsdk
../emsdk/emsdk install 6.0.11 && ../emsdk/emsdk activate 6.0.11
source ../emsdk/emsdk_env.sh

python3 tools/build_posix.py prepare --game /path/to/"FINAL FANTASY XI"
python3 tools/build_web.py --game /path/to/"FINAL FANTASY XI"
```

The page and the WebAssembly go to `build/web/` (about 33 MB). Run `prepare` again when the install
changes (a new client version), and `build_web.py` after pulling new code. Each build's files are
requested with the build's own id, so a browser never mixes an old page with a new game.

## Run it on your own machine

```sh
python3 tools/webserve.py --server <your server, by name or IP> --game /path/to/"FINAL FANTASY XI"
```

It prints the address to open, `http://127.0.0.1:8417/#t=<token>`, and a sign-in form for the
server's account comes up. Useful options:

| Option | What it does |
| --- | --- |
| `--server name` | The game server. Repeat it for each host the server sends you to (lobby, map servers): the WebSocket bridge connects to these addresses only. |
| `--dats folder` | A DAT overlay folder (repeat; the first one given wins), as host64's `--dats`. |
| `--port 8417` | The port on `127.0.0.1`. |
| `--token value` | A fixed token instead of a new one each run, so a bookmark keeps working. Also `XI_WEB_TOKEN`. |
| `--origin https://…` | The address a proxy of yours serves it under (below); repeat for more. |
| `--env file` | A file of `KEY=value` lines read into the environment; `~/.config/xi-web.env` is read by default if it exists. |

Keep the token like a password: anyone who has it and can reach the server can play through your
server and read your game files. Settings you change in game are not kept between visits yet (the
data folder lives in the page's memory).

## Play from your other devices (privately)

The page needs HTTPS (or `127.0.0.1` itself) for the shared memory its threads use, and WebSockets
for the network. Put `webserve.py` behind something that gives you both and only lets you in, give
`webserve.py` that address with `--origin`, and set a fixed `--token`. Any of these works:

**Tailscale (your devices only, no public address).** On the machine running `webserve.py`:

```sh
tailscale serve --bg 8417
python3 tools/webserve.py --server … --game … --origin https://<machine>.<tailnet>.ts.net --token <your token>
```

Open `https://<machine>.<tailnet>.ts.net/#t=<your token>` on any device in your tailnet.

**Cloudflare Tunnel with Cloudflare Access (a hostname of yours, behind a sign-in).** Create a tunnel
and route a hostname to `http://127.0.0.1:8417` (in `~/.cloudflared/config.yml`: an `ingress` entry
with `hostname: xi.example.com` and `service: http://127.0.0.1:8417`, before the catch-all). Then, in
Zero Trust, add a self-hosted Access application for that hostname with a policy that allows only
you. Don't add cache rules for it. Run `webserve.py` with `--origin https://xi.example.com`.

**Any other reverse proxy** (Caddy, nginx) works the same way if it serves HTTPS, passes WebSocket
upgrades through on `/net`, keeps the server's `Cross-Origin-Opener-Policy` and
`Cross-Origin-Embedder-Policy` headers, and puts a sign-in in front. Never expose `webserve.py` with
only the token in front of it.

## Faster loading: your own private bucket (optional)

Without a bucket every file comes from your machine, so a device far from home waits on your upload
speed. With a private S3-compatible bucket (tested with DigitalOcean Spaces; Cloudflare R2 and other S3-compatible stores should work the same way)
holding a copy of the files, the page downloads them from the bucket instead, by links `webserve.py`
signs for six hours at a time; only those small requests reach your machine. The bucket stays
private: a request without a signature gets 403.

1. Create a **private** bucket (no public read, no public listing) and an access key for it. The key
   needs write access for the upload in step 4; a read-only key is enough for `webserve.py` afterwards.
2. Put the key in `~/.config/xi-web.env`, one `KEY=value` per line, and `chmod 600` it:

   ```
   XI_BUCKET_KEY=…
   XI_BUCKET_SECRET=…
   XI_WEB_TOKEN=…
   ```

3. Let your page's address read it cross-origin (or set the same rule in the provider's console:
   origin your address, methods GET and HEAD, allowed header `Range`):

   ```sh
   python3 tools/bucket.py cors --bucket https://<bucket>.<region>.digitaloceanspaces.com --origin https://xi.example.com
   ```

4. Upload the files as gzipped 1 MB blocks, with the small files in 8 MB packs (about half the size
   of the install; run it again after the files change and it uploads only what changed):

   ```sh
   python3 tools/bucket.py blocks --bucket https://<bucket>.<region>.digitaloceanspaces.com \
       --prefix <folder>/blocks --game /path/to/"FINAL FANTASY XI" [--dats /path/to/overlay]
   ```

   Optionally also copy the install and overlays as they are (with `rclone` or `s3cmd`) to
   `<folder>/game` and `<folder>/dats-N`: a file missing from the blocks is then read from those.

5. Start `webserve.py` with the bucket. A CDN address works as well as the bucket's own:

   ```sh
   python3 tools/webserve.py --server … --game … [--dats …] --origin https://xi.example.com \
       --bucket https://<bucket>.<region>.cdn.digitaloceanspaces.com \
       --bucket-blocks <folder>/blocks [--bucket-game <folder>/game --bucket-dats <folder>/dats-0]
   ```

   It prints `file reads from the bucket at …` and how many files and packs the bucket has.
   `python3 tools/bucket.py check --bucket … --object <some object> --origin …` shows a signed read
   working and an unsigned one refused.

In the page, **Download all game data** (under the gear at the top right) fills the browser's own storage with every file,
small files first: minutes rather than hours from a bucket (the 55,000 small files took about a minute
in testing). After that, zoning reads nothing over the network. The page also learns which files the game opens together, and fetches the rest of a set when
its first file opens.

## Page options

Add these after the token in the address, e.g. `#t=<token>&ui=1.25`.

| Option | What it does |
| --- | --- |
| `cache=clear` | Empties the browser's file cache first (once; leave it off afterwards). `cache=0` reads everything from the network. |
| `ui=1.25` | Interface size: 1.25 is a quarter bigger, 0.8 smaller (0.5 to 3). |
| `shape=16:9` | The interface's shape on a wide window: `16:9`, `4:3`, or `off` (the whole window). As Config > Modern > Interface Shape. |
| `maxpx=8.3` | The most pixels the game renders, in millions (default 8.3, about 4K). |
| `fps`, `dl`, `log`, `status` `=0` or `=1` | Hide or show the fps counter, the file downloads list, the log, the status line. By default the fps counter and status line are on and the log and downloads list off; the gear at the top right switches each, and the browser remembers it. The log turns itself on when an error is logged. |

## Window size and the interface

The game follows the window: change its size, drag it to another screen or zoom the page, and the
game's resolution and interface follow once the window stops changing (the last frame is
letterboxed meanwhile).

- **Resolution:** the window in device pixels, capped at `maxpx` (a large Retina window would
  otherwise render about 15 million pixels a frame).
- **Interface:** one interface pixel per CSS pixel (times `ui`), the size the native game draws it at
  100%. The game's windows are laid out for at least 512 x 448, so the interface never gets less than
  640 x 480: a smaller window gets a smaller interface rather than one cut off. On a very tall window
  it stops at 1200 lines, so it doesn't turn tiny.
- **Wide windows:** by default the interface uses the whole width, as on native; `shape=16:9` keeps it
  in a centred 16:9 box.

Checked in Chromium at 1280 x 720, 1920 x 1080, 800 x 600, 2560 x 720 (with and without
`shape=16:9`), 600 x 900 and 480 x 320, resizing live while in a zone.

## Updating

- New code: `git pull`, then `build_web.py`, and restart `webserve.py`.
- A new client version: `prepare`, then `build_web.py`.
- The server changed its DATs: with a bucket, run `bucket.py blocks` again (`webserve.py` picks the
  changes up within five minutes); either way the page fetches only the files that changed.

## When something goes wrong

- **"This page needs cross-origin isolation"**: the page isn't on HTTPS or `127.0.0.1`, or a proxy
  dropped the `Cross-Origin-Opener-Policy` / `Cross-Origin-Embedder-Policy` headers.
- **403 "bad token"**: the address lacks the current `#t=`; with a new token each run, use the one
  `webserve.py` just printed, or set a fixed `--token`.
- **403 "other origins may not use this server"**: start `webserve.py` with `--origin` set to the
  address in the browser's address bar.
- **`net: refused <address> (not the game server)`** in `webserve.py`'s output: the game server sent you to a host not in
  `--server`; add it.
- **Black screen at 0 fps**: read the log under the game (it opens by itself on an error; or turn it on under the gear, or add `&log=1`). A crash prints `thread crashed:` and its
  stack there; WebGPU errors show up there too.
- **Slow zoning**: the overlay at the top left lists each file read, how long it took and where from
  (`bucket`, `bucket, gzip`, `bucket, pack` or `server`). Use a bucket, or **Download all game data**.
- **A bucket read failing**: `bucket.py check` prints the bucket's own error (`SignatureDoesNotMatch`,
  `AccessDenied`, `NoSuchKey`) and whether the CORS rule is answering.
