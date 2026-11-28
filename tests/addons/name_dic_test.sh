#!/bin/sh
# Regression test for the name dictionary character creation checks a new name against
# (host/host64.c, stand_in): an install without the viewer's data\dic\entryz.dic (private servers)
# gets an empty one, written to the data folder and mounted where the game reads it. The game finds
# it as Windows would, by FindFirstFileA (runtime/portable/winapi/k32_io.c: a file mounted on its own,
# its folder not in the install) and CreateFileA, through FFXiMain's own imports. Without one the game
# refuses every name itself (FFXI-3322) and asks the server nothing. An install that has its own keeps it.
#
# The game is reached through a link beside the test's own PlayOnlineViewer folder, so the test never
# reads the machine's viewer. On Windows the link is a junction to the game folder. Elsewhere it is a
# folder of links to the game's files: a link to the folder itself would resolve the game's ".." to the
# real install's parent, and with it the machine's viewer.
#
#   tests/addons/name_dic_test.sh --game <FFXI folder> [--update]
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

data=$(mktemp -d "${TMPDIR:-/tmp}/namedic.XXXXXX")
link="$data/FINAL FANTASY XI"
# the link goes first (cmd's rmdir removes a junction and leaves what it points at), then the folder
unlink_game() {
    if command -v cygpath > /dev/null; then cmd //c rmdir "$(cygpath -w "$link")" 2> /dev/null || true
    elif [ -d "$link" ]; then find "$link" -mindepth 1 -maxdepth 1 -type l -exec rm -f {} + && rmdir "$link"; fi
}
trap 'unlink_game; [ -e "$link" ] || rm -rf "$data"' EXIT
mkdir -p "$data/PlayOnlineViewer"
if command -v cygpath > /dev/null; then
    cmd //c mklink //J "$(cygpath -w "$link")" "$(cygpath -w "$game")" > /dev/null
    viewer="$(cygpath -w "$data")\\PlayOnlineViewer"
else
    mkdir "$link"
    for e in "$game"/*; do ln -s "$e" "$link/"; done
    viewer='C:\PlayOnline\SquareEnix\PlayOnlineViewer'
fi
game=$link

mkdir -p "$data/xi/addons/dic"
cat > "$data/xi/addons/dic/dic.lua" <<'EOF'
local m = xi.memory
-- FFXiMain's import of a kernel32 function, from its import directory
local function import(name)
    local base = m.module('FFXiMain.dll')
    local pe = base + m.read_uint32(base + 0x3c)
    local dir = base + m.read_uint32(pe + 0x80)
    while m.read_uint32(dir + 12) ~= 0 do
        local names, iat = m.read_uint32(dir), base + m.read_uint32(dir + 16)
        names = names ~= 0 and base + names or iat
        for i = 0, 4096 do
            local hint = m.read_uint32(names + 4 * i)
            if hint == 0 then break end
            if hint < 0x80000000 and m.read_string(base + hint + 2) == name then return m.read_uint32(iat + 4 * i) end
        end
        dir = dir + 20
    end
end
function look(path)
    local data = m.alloc(320)
    local find = m.call(import('FindFirstFileA'), 's', 'u:ii', path, data)
    local found = find ~= 0xFFFFFFFF and ('found, ' .. m.read_uint32(data + 32) .. ' bytes') or 'not found'
    if find ~= 0xFFFFFFFF then m.call(import('FindClose'), 's', 'i:i', find) end
    local file = m.call(import('CreateFileA'), 's', 'u:iiiiiii', path, 0x80000000, 1, 0, 3, 0, 0)
    if file ~= 0xFFFFFFFF then m.call(import('CloseHandle'), 's', 'i:i', file) end
    m.free(data)
    return 'FindFirstFileA ' .. found .. '; CreateFileA ' .. (file ~= 0xFFFFFFFF and 'opened' or 'failed')
end
EOF

script="$data/script.txt"
out="$data/out.txt"
run() {
    FFXI_ADDONS_LUA="$root/host/addons/lua" "$root/build/host64" --game "$game" --data-dir "$data" \
        --user-dir "$data/USER" --addon-harness "$script" > "$data/raw.txt" 2>&1 || true
    grep -ao 'entryz.dic: none in the install' "$data/raw.txt" || echo "no stand-in"
    grep -a '^\[harness\]' "$data/raw.txt" || true
}
{
    echo "-- no dictionary in the viewer"
    printf 'load dic xi\nlua dic look([[%s\\data\\dic\\entryz.dic]])\n' "$viewer" > "$script"
    run
    dic=$(od -An -v -tx1 "$data/entryz.dic" 2> /dev/null | tr -d ' \n')
    echo "stand-in: $dic"
    echo "-- the viewer's own dictionary"
    rm -f "$data/entryz.dic"
    mkdir -p "$data/PlayOnlineViewer/data/dic"
    head -c 32 /dev/zero > "$data/PlayOnlineViewer/data/dic/entryz.dic"
    echo "echo -- started" > "$script"
    run
    [ -f "$data/entryz.dic" ] && echo "stand-in written" || echo "none written"
} > "$out"

if [ $update -eq 1 ]; then
    cp "$out" "$here/name_dic_test.expected"
    echo "updated name_dic_test.expected"
    exit 0
fi
if diff -u "$here/name_dic_test.expected" "$out"; then
    echo "name_dic_test: PASS"
else
    echo "name_dic_test: FAIL"
    exit 1
fi
