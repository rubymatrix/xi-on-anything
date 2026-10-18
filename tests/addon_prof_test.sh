#!/bin/sh
# Regression test for the addons' profile (host/addons/prof.c) and cached pattern scans: runs
# tests/addon_prof_test.txt in the addon harness with the test addon tests/addons/proftest and
# compares the output with tests/addon_prof_test.expected, every number in it written N (times vary).
#
#   tests/addon_prof_test.sh --game <FFXI folder> [--update]
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

data=$(mktemp -d "${TMPDIR:-/tmp}/proftest.XXXXXX")
trap 'rm -rf "$data"' EXIT
mkdir -p "$data/xi/addons"
cp -R "$here/addons/proftest" "$data/xi/addons/"

out="$data/out.txt"
FFXI_ADDON_SLOW=100 FFXI_ADDONS_LUA="$root/host/addons/lua" "$root/build/host64" --game "$game" --data-dir "$data" \
    --addon-harness "$here/addon_prof_test.txt" > "$data/raw.txt" 2>&1 || true
grep -E '^\[chat |^\[game\]|^\[harness\]|^\[addons\] slow|unbalanced|error' "$data/raw.txt" | grep -v 'patch.ver' |
    grep -v 'codepatch' | sed -E 's/[0-9]+([.:][0-9]+)*/N/g; s/  +/ /g' > "$out"

if [ $update -eq 1 ]; then
    cp "$out" "$here/addon_prof_test.expected"
    echo "updated addon_prof_test.expected"
    exit 0
fi
if diff -u "$here/addon_prof_test.expected" "$out"; then
    echo "addon_prof_test: PASS"
else
    echo "addon_prof_test: FAIL"
    exit 1
fi
