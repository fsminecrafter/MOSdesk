#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PROJECT_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
SDK_BUILD="$PROJECT_ROOT/Minimal-OS-SDK/build.sh"
PACKAGE_TOOL="$PROJECT_ROOT/Minimal-OS/tools/mkpkg/mkpkg.py"
DIST="$SCRIPT_DIR/dist"
PACKAGE="$DIST/mosWM"

if [ ! -x "$SDK_BUILD" ]; then
    echo "mosWM: SDK build script missing at $SDK_BUILD" >&2
    echo "Initialize the Minimal-OS-SDK submodule before building." >&2
    exit 1
fi
if [ ! -f "$PACKAGE_TOOL" ]; then
    echo "mosWM: Minimal-OS package tool missing at $PACKAGE_TOOL" >&2
    exit 1
fi

mkdir -p "$DIST" "$PACKAGE/programs/mosWM/assets/fonts" \
    "$PACKAGE/programs/mosWM/assets/cursors" \
    "$PACKAGE/programs/mosWM/assets/wallpapers" "$PACKAGE/services"
"$SDK_BUILD" --output "$DIST/mosWM.run" "$SCRIPT_DIR"
cp "$DIST/mosWM.run" "$PACKAGE/programs/mosWM/mosWM.run"
cp -R "$SCRIPT_DIR/assets/cursors/." \
    "$PACKAGE/programs/mosWM/assets/cursors/"
rm -f "$PACKAGE/programs/mosWM/assets/posy.cur"
cp "$SCRIPT_DIR/assets/NOTICE.txt" "$PACKAGE/programs/mosWM/assets/NOTICE.txt"
cp "$SCRIPT_DIR/assets/wallpapers/"*.qoi \
    "$PACKAGE/programs/mosWM/assets/wallpapers/"
mkdir -p "$PACKAGE/programs/mosWM/assets/wallpapers/thumbs"
cp "$SCRIPT_DIR/assets/wallpapers/thumbs/"*.thm \
    "$PACKAGE/programs/mosWM/assets/wallpapers/thumbs/"
cp "$SCRIPT_DIR/assets/fonts/"Inter-* "$PACKAGE/programs/mosWM/assets/fonts/"
cp "$SCRIPT_DIR/mosWM.pkg" "$PACKAGE/mosWM.pkg"
cp "$SCRIPT_DIR/mosWM.service" "$PACKAGE/services/mosWM.service"
python3 "$PACKAGE_TOOL" "$PACKAGE" "$DIST/mosWM.mpkg" --store

printf 'mosWM deliver package: %s\n' "$DIST/mosWM.mpkg"
printf 'Program bundle:        %s\n' "$DIST/mosWM.run"
