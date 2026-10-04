# Recorded scenes for performance tests

`tools/replayserver.py` plays recorded zone visits ("scenes") to the game, the same way every time:
the NPCs, mobs, players, weather, time of day and effects of a visit recorded on a server you run,
with nothing depending on a live server. It is `tools/staticserver.py`'s server (any sign-in, one
character, the lobby and zone protocol) with recordings in place of its empty zones.

## Record

`xireplay/` is an Ashita v4 addon (Ashita on Windows, and this project's addon host). Copy it to the
addons folder and load it on a LandSandBoat server of your own:

    /addon load xireplay

Every zone visit is then recorded to Ashita's `config/addons/xireplay/`, one JSON-lines file per
visit (the packets the client got, with their times). Nothing recorded is part of this repository:
recordings are yours, made on your server.

## Play

A suite names the scenes to play, one per line: a recording, then its scene flags.

    # suite.txt
    captures/home-1.jsonl --home --label home
    captures/markets-1.jsonl --label markets --group city
    captures/mines-1.jsonl --label mines --group city

| flag | |
| --- | --- |
| `--label NAME` | the scene's name in events and `!replay` (default: the file's) |
| `--group NAME` | a group `!replay` can ask for |
| `--home` | the home scene, where a session waits |
| `--chat` | keep the recorded chat, battle-log and other text (dropped by default) |

    python3 tools/replayserver.py --huffman <dir with compress.dat> --suite suite.txt
    build/host64 --game "<FINAL FANTASY XI>" --server 127.0.0.1 --user replay --pass x \
        --authport 55231 --dataport 55230 --viewport 55001

The session waits in the home scene. Ask for scenes in chat (the `!` is optional):

    !replay list         !replay 3         !replay markets         !replay city         !replay all
    !replay stop         (home after this scene)

Each scene is reached by a zone change to the server itself; the session goes home after the last.
`--autoplay "all"` queues scenes at the first zone-in. `--scene <recording> [scene flags]` plays one.
The ports (55231 auth, 55230 data, 55001 lobby, 55232 zone) leave a LandSandBoat server's free.

## Events

The server's events reach the client as chat lines from "xireplay"; the addon hides them and shows
each as one line:

    [17:18:37] xireplay BEGIN #2 mines (city) zone 234
    [17:18:46] xireplay READY #2 mines
    [17:19:18] xireplay END   #2 mines: played
    [17:19:18] xireplay DONE  1 scenes played
    [17:19:21] xireplay HOME  waiting: !replay <#|name|group|all|list|stop>

QUEUE, SCENE (from `!replay list`), PHASE (a recorded marker), INFO and ERROR have the same shape.
