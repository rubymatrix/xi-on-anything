#!/bin/bash
# Build XI on Anything for the Nintendo Switch and put it on an SD card: platforms/switch/setup-switch.sh opens the setup's
# window (with a display and tkinter); with tools/setup_switch.py's options (--game, --sd, --server; --help),
# or --console, it asks in the terminal instead.
# Needs Python 3.12+ and devkitPro (docs/platforms/switch.md, Quick start).
cd "$(dirname "$0")/../.." || exit 1

for py in python3.14 python3.13 python3.12 python3; do
    if command -v "$py" >/dev/null 2>&1 && "$py" -c 'import sys; sys.exit(sys.version_info < (3, 12))' 2>/dev/null; then
        if [ $# -eq 0 ] && { [ -n "$DISPLAY" ] || [ -n "$WAYLAND_DISPLAY" ] || [ "$(uname)" = Darwin ]; } &&
            "$py" -c 'import tkinter' 2>/dev/null; then
            exec "$py" tools/setup_switch_gui.py
        fi
        [ "$1" = --console ] && shift
        exec "$py" tools/setup_switch.py "$@"
    fi
done
echo "Python 3.12 or newer is needed: https://www.python.org/downloads/ (or your system's package manager)" >&2
exit 1
