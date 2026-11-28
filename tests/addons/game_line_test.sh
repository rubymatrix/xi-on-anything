#!/bin/sh
# Regression test for the length of a line the game's command parser gets (host/addons/hooks.c,
# xi_game_line_len): the parser copies a line into a 248-byte stack buffer with no bound, and a 300-byte
# line the host ran crashed the game. Lines no addon handles reach the game cut to 247 bytes at a
# whole character (a Shift-JIS pair is not split); an addon's command handler still sees the whole line;
# a 4,000-byte line passes text_out's 1 KB buffer. The script is written here, its lines being long.
#
#   tests/addons/game_line_test.sh --game <FFXI folder> [--update]
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(dirname "$(dirname "$here")")
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

data=$(mktemp -d "${TMPDIR:-/tmp}/gameline.XXXXXX")
trap 'rm -rf "$data"' EXIT
mkdir -p "$data/ashita/addons/lineprobe"
cat > "$data/ashita/addons/lineprobe/lineprobe.lua" <<'EOF'
addon.name = 'lineprobe'
addon.author = 'tests'
addon.version = '1.0'
ashita.events.register('command', 'lineprobe_command', function (e)
    if e.command:sub(1, 7) == '/probe ' then
        print('lineprobe: ' .. #e.command .. ' bytes')
        e.blocked = true
    end
end)
EOF

a() { head -c "$1" /dev/zero | tr '\0' a; }
script="$data/script.txt"
{
    echo "frames 1"
    echo "command /none $(a 294)"
    echo "command /echo $(a 2000)"
    printf 'command /echo %s\202\240b\n' "$(a 240)"
    echo "command /say $(a 4000)"
    echo "load lineprobe"
    echo "command /probe $(a 593)"
    echo "lua lineprobe AshitaCore:GetChatManager():QueueCommand(1, '/echo ' .. string.rep('b', 400))"
    echo "frames 2"
    echo "unload lineprobe"
} > "$script"

out="$data/out.txt"
FFXI_ADDONS_LUA="$root/host/addons/lua" "$root/build/host64" --game "$game" --data-dir "$data" \
    --user-dir "$data/USER" --addon-harness "$script" > "$data/raw.txt" 2>&1 || true
# each line as its length and first and last bytes, so the expected file stays readable
grep -aE '^\[chat |^\[game\]|^\[harness\]|error' "$data/raw.txt" | grep -av 'patch.ver' |
    LC_ALL=C awk '{ n = length($0); print n ": " (n > 40 ? substr($0, 1, 20) " .. " substr($0, n - 9) : $0) }' \
    > "$out" || true

if [ $update -eq 1 ]; then
    cp "$out" "$here/game_line_test.expected"
    echo "updated game_line_test.expected"
    exit 0
fi
if diff -u "$here/game_line_test.expected" "$out"; then
    echo "game_line_test: PASS"
else
    echo "game_line_test: FAIL"
    exit 1
fi
