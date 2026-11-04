#!/bin/sh
# Copies a built AsciiDepth.plugin into After Effects' shared plug-in folder.
#   scripts/install.sh [path/to/AsciiDepth.plugin]
# The folder is admin-writable on most Macs; if not, re-run with sudo.

set -e
cd "$(dirname "$0")/.."

SRC="${1:-build/Build/Products/Release/AsciiDepth.plugin}"
DEST="${AE_PLUGIN_INSTALL_DIR:-/Library/Application Support/Adobe/Common/Plug-ins/7.0/MediaCore}"

if [ ! -d "$SRC" ]; then
    echo "error: $SRC not found - build first (make)" >&2
    exit 1
fi

# Unsigned arm64 code will not load in After Effects, so (re)sign ad hoc first.
codesign --force --deep --sign - "$SRC"

rm -rf "$DEST/AsciiDepth.plugin"
cp -R "$SRC" "$DEST/"
echo "Installed AsciiDepth.plugin -> $DEST"
echo "Restart After Effects, then find it under Effect > Stylize > ASCII Depth."
