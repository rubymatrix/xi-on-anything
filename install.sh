#!/bin/bash
# Install the XI on Anything launcher. Paste into Terminal (on a Steam Deck: Desktop Mode, Konsole):
#
#   curl -fsSL https://raw.githubusercontent.com/rubymatrix/xi-on-anything/main/install.sh | bash
#
# Run the same line again to update to the newest release.
#
# macOS: it downloads the latest release's XI-on-Mac-macos-arm64.zip from this repository, checks it
# against the release's SHA256SUMS, and puts "XI on Mac.app" into /Applications (~/Applications when
# /Applications is not writable), then opens it.
#
# Linux (x86_64; a Steam Deck): XI-on-Anything-linux-x86_64.AppImage, checked the same way, goes to
# ~/Applications, with a menu entry and a Steam shortcut ("XI on Anything" in the Library, under
# Non-Steam), so on a Deck it is played from Game Mode from then on: the launcher updates itself and
# builds the game there. Steam is closed while the shortcut is added (it rewrites its shortcuts file
# when it quits); on a Deck, "Return to Gaming Mode" starts it again.
#
# To build the game from this repo's sources instead (developers), see tools/install-source.sh.
#
# XI_LAUNCHER_VERSION=v0.1.0 installs that release instead of the latest; XI_LAUNCHER_APPS sets where
# the app goes; XI_LAUNCHER_NO_OPEN=1 does not open it.

# Everything is inside main, so a download cut short runs nothing.
main() {
    set -e
    local repo="rubymatrix/xi-on-anything"
    local asset="XI-on-Mac-macos-arm64.zip"
    local app_name="XI on Mac.app"
    local base="https://github.com/$repo/releases"
    local url
    if [ -n "$XI_LAUNCHER_VERSION" ]; then
        url="$base/download/$XI_LAUNCHER_VERSION"
    else
        url="$base/latest/download"
    fi

    if [ "$(uname -s)" = "Linux" ]; then
        linux "$url"
        return
    fi
    if [ "$(uname -s)" != "Darwin" ]; then
        echo "This installer is for macOS and Linux." >&2
        exit 1
    fi
    if [ "$(uname -m)" != "arm64" ]; then
        echo "XI on Anything needs Apple silicon (M1 or later)." >&2
        exit 1
    fi

    local tmp
    tmp="$(mktemp -d)"
    trap 'rm -rf "$tmp"' EXIT
    echo "Downloading XI on Anything…"
    curl -fL --progress-bar -o "$tmp/$asset" "$url/$asset"
    curl -fsSL -o "$tmp/SHA256SUMS" "$url/SHA256SUMS"
    local want got
    want="$(awk -v f="$asset" '$2 == f || $2 == "*"f { print $1 }' "$tmp/SHA256SUMS")"
    got="$(shasum -a 256 "$tmp/$asset" | awk '{ print $1 }')"
    if [ -z "$want" ] || [ "$want" != "$got" ]; then
        echo "The download does not match the release's checksum; nothing was installed." >&2
        exit 1
    fi

    ditto -x -k "$tmp/$asset" "$tmp/unpacked"
    if [ ! -d "$tmp/unpacked/$app_name" ]; then
        echo "The download has no $app_name in it." >&2
        exit 1
    fi

    local apps="${XI_LAUNCHER_APPS:-/Applications}"
    if [ -z "$XI_LAUNCHER_APPS" ] && [ ! -w "$apps" ]; then
        apps="$HOME/Applications"
    fi
    mkdir -p "$apps"
    # a running launcher is asked to quit before it is replaced
    if pgrep -f "$apps/$app_name/Contents/MacOS/" >/dev/null 2>&1; then
        echo "Quitting the running launcher…"
        osascript -e 'tell application "XI on Mac" to quit' >/dev/null 2>&1 || true
        sleep 2
    fi
    rm -rf "$apps/$app_name"
    ditto "$tmp/unpacked/$app_name" "$apps/$app_name"
    echo "Installed $apps/$app_name"

    if [ -z "$XI_LAUNCHER_NO_OPEN" ]; then
        open "$apps/$app_name"
    fi
}

# Linux: the AppImage into ~/Applications, a menu entry, and a Steam shortcut.
linux() {
    local url="$1" asset="XI-on-Anything-linux-x86_64.AppImage"
    if [ "$(uname -m)" != "x86_64" ]; then
        echo "XI on Anything for Linux needs an x86_64 machine (a Steam Deck is one)." >&2
        exit 1
    fi
    local tmp
    tmp="$(mktemp -d)"
    trap 'rm -rf "$tmp"' EXIT
    echo "Downloading XI on Anything…"
    curl -fL --progress-bar -o "$tmp/$asset" "$url/$asset"
    curl -fsSL -o "$tmp/SHA256SUMS" "$url/SHA256SUMS"
    local want got
    want="$(awk -v f="$asset" '$2 == f || $2 == "*"f { print $1 }' "$tmp/SHA256SUMS")"
    got="$(sha256sum "$tmp/$asset" | awk '{ print $1 }')"
    if [ -z "$want" ] || [ "$want" != "$got" ]; then
        echo "The download does not match the release's checksum; nothing was installed." >&2
        exit 1
    fi

    local apps="${XI_LAUNCHER_APPS:-$HOME/Applications}" image
    image="$apps/XI-on-Anything.AppImage"
    mkdir -p "$apps"
    pkill -f "$image" >/dev/null 2>&1 || true
    install -m 755 "$tmp/$asset" "$image"
    echo "Installed $image"

    # without FUSE 2 an AppImage cannot mount itself: it unpacks itself each time instead
    local launch=""
    if ! ldconfig -p 2>/dev/null | grep -q 'libfuse\.so\.2'; then
        launch="APPIMAGE_EXTRACT_AND_RUN=1 %command%"
    fi

    mkdir -p "$HOME/.local/share/applications"
    cat > "$HOME/.local/share/applications/xi-on-anything.desktop" <<DESKTOP
[Desktop Entry]
Type=Application
Name=XI on Anything
Comment=FINAL FANTASY XI, recompiled for this machine
Exec=env ${launch:+APPIMAGE_EXTRACT_AND_RUN=1 }"$image"
Categories=Game;
Terminal=false
DESKTOP

    local steam=""
    for d in "$HOME/.steam/steam" "$HOME/.local/share/Steam"; do
        if [ -d "$d/userdata" ]; then
            steam="$d"
            break
        fi
    done
    if [ -z "$steam" ]; then
        echo "No Steam here: start XI on Anything from your applications menu."
        return
    fi
    if pgrep -x steam >/dev/null 2>&1 || pgrep -x steamwebhelper >/dev/null 2>&1; then
        echo "Closing Steam to add the shortcut…"
        steam -shutdown >/dev/null 2>&1 || true
        for _ in $(seq 60); do
            pgrep -x steam >/dev/null 2>&1 || break
            sleep 1
        done
    fi
    python3 - "$steam" "$image" "$launch" <<'PY'
# Adds (or updates) the "XI on Anything" shortcut in every Steam user's shortcuts.vdf, a binary
# KeyValues file: 0x00 a map, 0x01 a string, 0x02 an int32, 0x08 the end of a map.
import glob, os, struct, sys, zlib

steam, image, launch = sys.argv[1:4]
NAME = "XI on Anything"
exe = '"%s"' % image

def read(b):
    def node(i):
        out = []
        while True:
            t = b[i]; i += 1
            if t == 8:
                return out, i
            k = b.index(0, i); key = b[i:k].decode("utf-8", "replace"); i = k + 1
            if t == 0:
                v, i = node(i)
            elif t == 1:
                k = b.index(0, i); v = b[i:k].decode("utf-8", "replace"); i = k + 1
            elif t == 2:
                v = struct.unpack_from("<i", b, i)[0]; i += 4
            elif t == 7:
                v = struct.unpack_from("<Q", b, i)[0]; i += 8
            else:
                raise ValueError("type %d" % t)
            out.append((key, t, v))
    if not b:
        return [("shortcuts", 0, [])]
    top, _ = node(0)
    return top

def write(items):
    out = bytearray()
    for key, t, v in items:
        out.append(t); out += key.encode() + b"\0"
        if t == 0:
            out += write(v); out.append(8)
        elif t == 1:
            out += v.encode() + b"\0"
        elif t == 2:
            out += struct.pack("<i", v)
        elif t == 7:
            out += struct.pack("<Q", v)
    return bytes(out)

appid = (zlib.crc32((exe + NAME).encode()) & 0xFFFFFFFF) | 0x80000000
entry = [
    ("appid", 2, struct.unpack("<i", struct.pack("<I", appid))[0]),
    ("AppName", 1, NAME), ("Exe", 1, exe), ("StartDir", 1, '"%s"' % os.path.dirname(image)),
    ("icon", 1, ""), ("ShortcutPath", 1, ""), ("LaunchOptions", 1, launch),
    ("IsHidden", 2, 0), ("AllowDesktopConfig", 2, 1), ("AllowOverlay", 2, 1), ("OpenVR", 2, 0),
    ("Devkit", 2, 0), ("DevkitGameID", 1, ""), ("DevkitOverrideAppID", 2, 0), ("LastPlayTime", 2, 0),
    ("FlatpakAppID", 1, ""), ("tags", 0, []),
]
users = [d for d in glob.glob(os.path.join(steam, "userdata", "*")) if os.path.basename(d).isdigit() and os.path.basename(d) != "0"]
for user in users:
    path = os.path.join(user, "config", "shortcuts.vdf")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    data = open(path, "rb").read() if os.path.exists(path) else b""
    top = read(data)
    shortcuts = next((v for k, t, v in top if k.lower() == "shortcuts"), None)
    if shortcuts is None:
        shortcuts = []; top.append(("shortcuts", 0, shortcuts))
    # an earlier shortcut to the launcher is replaced, the rest renumbered
    keep = [v for k, t, v in shortcuts if not any(kk == "AppName" and vv == NAME for kk, tt, vv in v)]
    keep.append(entry)
    shortcuts[:] = [(str(i), 0, v) for i, v in enumerate(keep)]
    if data:
        open(path + ".bak", "wb").write(data)
    open(path, "wb").write(write(top) + b"\x08")
    print("Steam shortcut added for user %s" % os.path.basename(user))
PY
    if [ -e /etc/os-release ] && grep -qi steamos /etc/os-release; then
        echo
        echo "Done. Double-click \"Return to Gaming Mode\" on the desktop: XI on Anything is in your Library under Non-Steam."
    else
        echo
        echo "Done. Start Steam again: XI on Anything is in your Library under Non-Steam, and in your applications menu."
    fi
}

main "$@"
