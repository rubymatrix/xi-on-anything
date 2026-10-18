#!/usr/bin/env python3
"""Write a recording by hand, no server to record from: tools/replayserver.py plays it like any other.

One character standing in a zone at a Vana'diel hour, with what staticserver.py sends a zone-in and
nothing else (no NPCs, no mobs, no weather). Made for looking at one thing - the character's own
shadow, say - at a fixed vantage, so the race, looks, zone, place and hour are arguments:

  python3 tools/replay/synth.py generated/replay/mithra-1.jsonl --race 7 --hour 17

The packets are the ones a staticserver.py session queues (its zone-in, then what follows the
client's 0x00C), at the times a recording from a server would have them, then the xireplay addon's
`start` marker and the scene's end.
"""
import argparse
import json
import os
import struct
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..'))
import staticserver as ss  # noqa: E402

RACES = {'hume-m': 1, 'hume-f': 2, 'elvaan-m': 3, 'elvaan-f': 4, 'tarutaru-m': 5, 'tarutaru-f': 6,
         'mithra': 7, 'galka': 8}


class NoHuffman:
    """The scene is not sent here: nothing compresses."""


def vana_now(hour):
    """An Earth time, now or a little before, at which the Vana'diel clock reads this hour (a Vana'diel
    day is 3456 Earth seconds: the clock runs 25 times as fast)."""
    now = int(time.time())
    day = 86400 // 25
    want = int(hour * 3600) // 25
    return now - ((now - ss.VANA_EPOCH) % day) + want


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('out', help='the recording to write (.jsonl)')
    ap.add_argument('--race', default='mithra', help='1-8 or ' + ', '.join(RACES))
    ap.add_argument('--face', type=int, default=1)
    ap.add_argument('--size', type=int, default=1, help='0 small, 1 medium, 2 large')
    ap.add_argument('--name', default='Tester')
    ap.add_argument('--zone', type=int, default=100)
    ap.add_argument('--at', help='x,y,z[,rotation (0-255)]: where the character stands (default: the zone\'s known spot)')
    ap.add_argument('--hour', type=float, default=17.0, help="the Vana'diel hour (default 17: a low sun, long shadows)")
    ap.add_argument('--length', type=float, default=30.0, help='seconds the scene lasts after its start marker')
    ap.add_argument('--start', type=float, default=8.0, help='seconds from the zone-in to the start marker')
    ap.add_argument('--sql', help='a LandSandBoat sql/ folder (zone music and lines)')
    a = ap.parse_args()

    race = RACES.get(a.race.lower()) or int(a.race)
    char = dict(ss.DEFAULT_CHAR, name=a.name, race=race, face=a.face, size=a.size, zone=a.zone,
                pos=[float(v) for v in a.at.split(',')] if a.at else None)
    if char['pos'] and len(char['pos']) == 3:
        char['pos'].append(0)
    srv_args = argparse.Namespace(public_ip='127.0.0.1', session_hash='5354415449435345535349304e000001', motd='',
                                  zone_port=0)
    srv = ss.Server(srv_args, char, ss.Zones(a.sql), NoHuffman())
    s = ss.ZoneSession(srv, None)
    s.zone = a.zone
    s.pos = list(char['pos']) if char['pos'] else list(srv.zones.position(a.zone))

    # the zone-in with the clock at the hour
    zone_in = s.login_packet()
    struct.pack_into('<II', zone_in, 4 + 0x2C + 8, vana_now(a.hour), vana_now(a.hour) - ss.VANA_EPOCH)
    s.queue = []
    s.after_zone_in()
    follow = list(s.queue)

    def rec(t, p):
        return {'t': t, 'dir': 'in', 'id': struct.unpack_from('<H', p, 0)[0] & 0x1FF, 'hex': bytes(p).hex()}

    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    with open(a.out, 'w', encoding='utf-8') as f:
        f.write(json.dumps({'meta': {'zone': a.zone, 'scene': 'synth', 'captured': time.strftime('%Y-%m-%d %H:%M:%S'),
                                     'race': race, 'hour': a.hour}}) + '\n')
        f.write(json.dumps(rec(1000, zone_in)) + '\n')
        for i, p in enumerate(follow):
            f.write(json.dumps(rec(1000 + 20 * (i + 1), p)) + '\n')
        f.write(json.dumps({'t': 1000 + int(a.start * 1000), 'mark': 'start'}) + '\n')
        f.write(json.dumps({'t': 1000 + int((a.start + a.length) * 1000), 'mark': 'end'}) + '\n')
    print(f'{a.out}: {len(follow) + 1} packets, race {race}, zone {a.zone}, hour {a.hour}')


if __name__ == '__main__':
    main()
