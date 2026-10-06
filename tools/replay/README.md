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
visit (the packets the client got, with their times). Record only on a server you run. The scenes
`default.txt` plays are already recorded, in `scenes/` (below).

## Scenes

The addon also directs scenes: a fixed list of GM commands, with markers for the measured window,
recorded as one file per zone visit. It has seven (`home`, `markets`, `mines`, `lighting`, `weather`,
`crowd`, `effects`); `default.txt` makes these from the recordings in `scenes/`, with scene flags:

| scene | group | what it shows |
| --- | --- | --- |
| `home` | | GM Home, where a session waits |
| `markets`, `mines` | city | Bastok Markets and the Mines plaza at noon, a fixed vantage |
| `dawn`, `noon`, `dusk`, `midnight` | lighting | one North Gustaberg vantage at four times of day |
| `garden` | indoors | the Garden of Ru'Hmet at noon: no sky, so no sun shadows should reach it |
| `hot-spell`, `heat-wave`, `sand-storm`, `sunshine`, `dust-storm`, `wind`, `gales`, `rain`, `squall`, `snow`, `blizzards`, `thunder`, `thunderstorms`, `clouds`, `gloom`, `darkness`, `auroras`, `stellar-glare` | weather | an open spot in a zone whose own weather includes it (Western Altepa, West Sarutabaruta, Konschtat, La Theine, Cape Teriggan, Pashhow, Yuhtunga, Xarcabard, Beaucedine, Zi'Tah, Jugner Forest, Lufaise Meadows, Castle Zvahl Baileys, Ru'Aun Gardens, Qufim Island), in that weather from the zone-in |
| `mob-crowd` | crowd | about forty mobs held around the character, each made four (about 170), all named Monster |
| `player-crowd` | crowd | 150 geared characters in a city |
| `effects` | effects | a red mage's Chainspell, spikes and barriers, echoed by thirty characters casting about thirty different self spells, staggered |
| `mob-spells` | effects | the mob crowd, every mob casting spells at its neighbour: about 33 spell effects a second |

To record them on a LandSandBoat server of yours, with a GM account and the two commands in
`lsb/` copied to its `scripts/commands/` (`!perftime` pins the clock, `!perfcrowd` brings a
zone's mobs to the character):

    python3 tools/replay.py record all --server <your server> --user <GM account> --password ...

A client signs in (Enter through the lobby, by the control port), runs `/xireplay run all` and quits
when it is done; the recordings land in `generated/replay/`, not in `scenes/`.
`--revive-container <database container>` first gives a character left K.O. its HP back, in a
LandSandBoat Docker setup's database (a K.O. character can't run GM commands). Every scene but home
pins the clock and clears the weather at its zone-in.

    python3 tools/replay.py run tools/replay/default.txt

## The reference set

`scenes/` holds the recordings `default.txt` plays, committed so that every machine and CI plays the
same packets. They are LandSandBoat packet captures, which the repository allows (README, "Rules").

They are trimmed to what the scenes measure: no one but the character acts, and every NPC and mob
stays where it first appears, as `--my-actions` and `--hold` play a recording. A mob walking into
view, or its shadow moving under a still camera, would change a scene that measures weather or light.
`--players`, `--echo` and `--mob-spells` add their characters and spells on top.

Each file's first line records what made it and what was trimmed:

    {"meta":{"zone":210,"scene":"home","captured":"2026-10-04T01:36:32Z",
     "server":"LandSandBoat ea0054e7f420","client":"2025-11-12 (30251101_2)","recorder":"xireplay 1.0",
     "trimmed":["my-actions","hold"]}}

### A new reference set

Record the whole set again (above), so that every scene comes from one server, client and addon.
Then:

    python3 tools/replay.py keep generated/replay/*.jsonl --lsb <LandSandBoat commit> --build <game build>

`keep` writes what playback reads into `scenes/`: the server's packets from the zone-in to the
zone-out and the markers, with the character renamed `Replay`, trimmed, and the meta line above
(`--build` as `meta/builds.json` names it). The client's own packets, including the GM commands it
typed, are left out. Keep the raw recordings, since trimmed packets can't be restored from `scenes/`,
and pass only the recordings `default.txt` names.

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

What reshapes a scene, applied in this order whatever the order on the line:

| flag | |
| --- | --- |
| `--length S` | end the scene S seconds after its start marker |
| `--zone Z --at x,y,z,r` | the scene plays in zone Z instead, the character at x,y,z facing r, with none of the recorded zone's NPCs, mobs or actions (and no zone music) |
| `--turn D` | turn the character, and so the camera, by D degrees |
| `--my-actions` | keep only the character's own actions: nobody else attacks or casts |
| `--hold` | every NPC and mob stays where it first appears: no walking off, no despawns |
| `--clone N` | each mob becomes N mobs, on a ring around it |
| `--mob-spells a,b --mob-spells-every S` | every mob casts these spells (names like `fire-iv`, `drain`, `bio-ii`, ids, or `all`) in turn at the next mob, one every S seconds, the mobs' turns spread so effects are always under way |
| `--mob-name NAME` | every mob shows NAME (they move to the client's dynamic entities, which take their name from the packet) |
| `--players N` | N geared characters in rings around the zone-in, looks copied from recorded NPCs (`--looks a.jsonl,b.jsonl`) |
| `--echo` | with `--players`: they repeat what the character does to itself (`--echo-spread S` staggers them over S seconds; `--echo-spells a,b` or `all`: each casts these self spells, like `haste`, `regen-iii` or `enfire`, in place of the recorded ones, so they cast different spells at a time) |
| `--weather W` | the zone is in weather W (a name or id) when the character arrives: the client starts a weather's effects only at a zone-in, and only in a zone that has that weather |

A flag that needs another (`--echo` without `--players`, `--at` without `--zone`) is an error in its
line, as is an unknown flag, weather or spell.

A server's AI walks a spawned crowd home, so crowds need `--hold`; it can't put many players in one
place, so `--players` makes them.

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
    [17:19:18] xireplay END   #2 mines: 1751 frames, 58.4 fps, p99 22.3 ms, max 23.3 ms
    [17:19:18] xireplay DONE  1 scenes played
    [17:19:21] xireplay HOME  waiting: !replay <#|name|group|all|list|stop>

QUEUE, SCENE (from `!replay list`), PHASE (a recorded marker), INFO and ERROR have the same shape.
READY (the scene's start marker) means it is loaded and settled: the time for a screenshot.

END carries the scene's numbers, measured from its start marker to its end. `/xireplay quiet on`
leaves only these lines in the chat log; `/xireplay frames on` also logs every frame's time and every
event to `frames-<date>.csv`.

The latest event is also in `state.json` beside the recordings (`{"event":"READY","scene":2,...}`)
for a script to poll; it holds only the latest, so a script that must see every event reads the
frame log's `m,` lines.

## Measure

    python3 tools/replayreport.py frames-20261003-165743.csv [--json]

prints each scene's phases (one marker to the next) and its measured window (start marker to end
marker): frames, fps, mean, p50/p95/p99, max, frames over 33 ms.

## Unattended

`tools/replay.py` runs it all with nobody at the screen: the server, host64 with its control port
(`FFXI_CONTROL`) to get through the lobby (Enter, as each choice is the default) and to load the
addon, the frame log, and the report.

    python3 tools/replay.py run suite.txt [--play "city effects"]   # then generated/runs/<date>-suite/report.txt
    python3 tools/replay.py shots suite.txt                          # a frame capture at each READY
    python3 tools/replay.py play suite.txt                           # a session to watch: !replay in chat

`--game` (or `FFXI_GAME`), `--huffman` (or `FFXI_HUFFMAN`), `--client` (default `build/host64`),
`--data-dir` (default host64's); arguments after `--` go to host64. The game's own frame cap and
display settings are what it measures under.
