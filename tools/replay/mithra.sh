#!/bin/sh
# The Mithra shadow scenes at 1920x1080 windowed, made and run in one go:
#   tools/replay/mithra.sh [shots|play|run]      (default shots: a capture per facing; play: watch, !replay in chat)
# Needs the game folder (FFXI_GAME, default ../FINAL FANTASY XI) and a folder with compress.dat
# (FFXI_HUFFMAN, default ../FFXI-Signet/server/res).
set -e
cd "$(dirname "$0")/../.."
: "${FFXI_GAME:=$PWD/../FINAL FANTASY XI}"
: "${FFXI_HUFFMAN:=$PWD/../FFXI-Signet/server/res}"
export FFXI_GAME FFXI_HUFFMAN
[ -f "$FFXI_HUFFMAN/compress.dat" ] || { echo "no compress.dat in $FFXI_HUFFMAN (set FFXI_HUFFMAN)" >&2; exit 1; }
[ -d "$FFXI_GAME" ] || { echo "no game folder $FFXI_GAME (set FFXI_GAME)" >&2; exit 1; }
[ -x build/host64 ] || { echo "no build/host64: build first" >&2; exit 1; }
for r in 0 64 128 192; do
    python3 tools/replay/synth.py generated/replay/mithra-$r.jsonl --zone 103 --at 60.989,-4.898,-151.0,$r \
        --sql "$FFXI_HUFFMAN/../sql" > /dev/null
done
exec python3 tools/replay.py "${1:-shots}" tools/replay/mithra.txt --play shadows -- --reg-final "$PWD/tools/replay/display-1080.reg"
