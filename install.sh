#!/bin/bash
# Install the XI on Anything launcher. Paste into Terminal:
#
#   curl -fsSL https://raw.githubusercontent.com/rubymatrix/xi-on-mac/main/install.sh | bash
#
# Run the same line again to update to the newest release.
#
# It downloads the latest release's XI-on-Mac-macos-arm64.zip from this repository, checks it against
# the release's SHA256SUMS, and puts "XI on Mac.app" into /Applications (~/Applications when
# /Applications is not writable), then opens it.
#
# To build the game from this repo's sources instead (developers), see tools/install-source.sh.
#
# XI_LAUNCHER_VERSION=v0.1.0 installs that release instead of the latest; XI_LAUNCHER_APPS sets where
# the app goes; XI_LAUNCHER_NO_OPEN=1 does not open it.

# Everything is inside main, so a download cut short runs nothing.
main() {
    set -e
    local repo="rubymatrix/xi-on-mac"
    local asset="XI-on-Mac-macos-arm64.zip"
    local app_name="XI on Mac.app"
    local base="https://github.com/$repo/releases"
    local url
    if [ -n "$XI_LAUNCHER_VERSION" ]; then
        url="$base/download/$XI_LAUNCHER_VERSION"
    else
        url="$base/latest/download"
    fi

    if [ "$(uname -s)" != "Darwin" ]; then
        echo "This installer is for macOS." >&2
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

main "$@"
