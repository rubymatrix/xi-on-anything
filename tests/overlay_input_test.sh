#!/bin/sh
# Regression test for the overlay's input capture (host/addons/keys.c, gui.cpp): runs
# tests/overlay_input_test.txt in the addon harness with the test addon tests/addons/inputtest and
# compares the output with tests/overlay_input_test.expected.
#
#   tests/overlay_input_test.sh --game <FFXI folder> [--update]
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(dirname "$here")
game=""
update=0
while [ $# -gt 0 ]; do
    case "$1" in
        --game) game="$2"; shift 2 ;;
        --update) update=1; shift ;;
        *) echo "usage: $0 --game <FFXI folder> [--update]"; exit 2 ;;
    esac
done
[ -n "$game" ] || { echo "usage: $0 --game <FFXI folder> [--update]"; exit 2; }

data=$(mktemp -d "${TMPDIR:-/tmp}/inputtest.XXXXXX")
trap 'rm -rf "$data"' EXIT
mkdir -p "$data/xi/addons"
cp -R "$here/addons/inputtest" "$data/xi/addons/"

out="$data/out.txt"
FFXI_ADDONS_LUA="$root/host/addons/lua" "$root/build/host64" --game "$game" --data-dir "$data" \
    --addon-harness "$here/overlay_input_test.txt" > "$data/raw.txt" 2>&1 || true
grep -E '^\[chat |^\[game\]|^\[harness\]|unbalanced|error' "$data/raw.txt" | grep -v 'patch.ver' > "$out"

if [ $update -eq 1 ]; then
    cp "$out" "$here/overlay_input_test.expected"
    echo "updated overlay_input_test.expected"
    exit 0
fi
if diff -u "$here/overlay_input_test.expected" "$out"; then
    echo "overlay_input_test: PASS"
else
    echo "overlay_input_test: FAIL"
    exit 1
fi
