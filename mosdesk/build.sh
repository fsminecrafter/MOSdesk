#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PROJECT_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
# The Minimal-OS build passes its own SDK and package tool, so MOSdesk is
# built against the same headers as the kernel it runs on.
SDK_ROOT="${MOSDESK_SDK_DIR:-$PROJECT_ROOT/Minimal-OS-SDK}"
SDK_BUILD="$SDK_ROOT/build.sh"
PACKAGE_TOOL="${MKPKG_TOOL:-$PROJECT_ROOT/Minimal-OS/tools/mkpkg/mkpkg.py}"
DIST="$SCRIPT_DIR/dist"
PACKAGE="$DIST/mosdesk"
# The package installs at 0:/: MOSdesk itself under programs/mosdesk, the
# built-in apps (mosdesk/apps) straight into programs as mos_<name>.run.
INSTALL="$PACKAGE/programs/mosdesk"
APPS="$SCRIPT_DIR/apps"

if [ ! -x "$SDK_BUILD" ]; then
    echo "MOSdesk: SDK build script missing at $SDK_BUILD" >&2
    echo "Initialize the Minimal-OS-SDK submodule before building." >&2
    exit 1
fi
if [ ! -f "$PACKAGE_TOOL" ]; then
    echo "MOSdesk: Minimal-OS package tool missing at $PACKAGE_TOOL" >&2
    exit 1
fi

# Start from an empty package directory: everything in it gets installed.
rm -rf "$PACKAGE"
mkdir -p "$DIST" "$INSTALL"
BUILD_SOURCE=$(mktemp -d "$DIST/.mosdesk-build-source.XXXXXX")
trap 'rm -rf "$BUILD_SOURCE"' EXIT HUP INT TERM
mkdir -p "$BUILD_SOURCE/mosdesk" "$BUILD_SOURCE/mosWM" \
    "$BUILD_SOURCE/Minimal-OS-SDK"
cp "$SCRIPT_DIR/main.c" "$SCRIPT_DIR/settings.h" "$SCRIPT_DIR/input.h" \
    "$SCRIPT_DIR/password_hash.c" "$SCRIPT_DIR/password_hash.h" \
    "$BUILD_SOURCE/mosdesk/"
cp -R "$SCRIPT_DIR/libraries" "$BUILD_SOURCE/mosdesk/libraries"
cp "$PROJECT_ROOT/mosWM/file_icons.h" \
    "$PROJECT_ROOT/mosWM/file_icon_data.h" "$BUILD_SOURCE/mosWM/"
ln -s "$SDK_ROOT/libraries" \
    "$BUILD_SOURCE/Minimal-OS-SDK/libraries"
"$SDK_BUILD" --output "$DIST/mosdesk.run" "$BUILD_SOURCE/mosdesk"
"$SDK_BUILD" --slib --output "$DIST/ttrend_raster.slib" \
    "$SCRIPT_DIR/libraries/ttrend_raster"
"$SDK_BUILD" --slib --output "$DIST/ttrend_outline.slib" \
    "$SCRIPT_DIR/libraries/ttrend_outline"

# Built-in apps: apps/<name>.c, or apps/<name>/ with a main.c, becomes
# 0:/programs/mos_<name>.run (the same as `./build.sh mos_<name>.c`).
if [ -d "$APPS" ]; then
    for app in "$APPS"/*; do
        [ -e "$app" ] || continue
        name=$(basename -- "$app")
        case "$name" in
            *.c|*.cpp) name=${name%.*} ;;
            *) [ -d "$app" ] || continue ;;
        esac
        # One broken app should not stop the desktop from building.
        if ! "$SDK_BUILD" --output "$PACKAGE/programs/mos_$name.run" "$app"; then
            echo "MOSdesk: WARNING: app '$name' failed to build and is left out" >&2
            SKIPPED_APPS="${SKIPPED_APPS:-}${SKIPPED_APPS:+ }$name"
        fi
    done
fi

mkdir -p "$SCRIPT_DIR/fonts" "$INSTALL/fonts"
if [ -f "$PROJECT_ROOT/Open_Sans.zip" ]; then
    python3 "$SCRIPT_DIR/tools/generate_font.py" "$SCRIPT_DIR/fonts/arial.font" "$PROJECT_ROOT/Open_Sans.zip"
    python3 "$SCRIPT_DIR/tools/generate_font.py" "$SCRIPT_DIR/fonts/roboto.font" "$PROJECT_ROOT/Open_Sans.zip"
    python3 "$SCRIPT_DIR/tools/generate_font.py" "$SCRIPT_DIR/fonts/default.font" "$PROJECT_ROOT/Open_Sans.zip"
else
    for font in arial roboto default; do
        if [ ! -s "$SCRIPT_DIR/fonts/$font.font" ]; then
            echo "MOSdesk: missing $font.font and Open_Sans.zip; cannot build fonts." >&2
            exit 1
        fi
    done
    printf 'Using bundled TTREND fonts (Open_Sans.zip not present).\n'
fi
cp "$SCRIPT_DIR/fonts/arial.font" "$INSTALL/fonts/arial.font"
cp "$SCRIPT_DIR/fonts/roboto.font" "$INSTALL/fonts/roboto.font"
cp "$SCRIPT_DIR/fonts/default.font" "$INSTALL/fonts/default.font"
for font in Inter-Regular.otf Inter-Medium.otf Inter-LICENSE.txt; do
    cp "$SCRIPT_DIR/fonts/$font" "$INSTALL/fonts/$font"
done
cp "$DIST/mosdesk.run" "$INSTALL/mosdesk.run"
cp "$DIST/ttrend_raster.slib" "$INSTALL/ttrend_raster.slib"
cp "$DIST/ttrend_outline.slib" "$INSTALL/ttrend_outline.slib"
cp "$SCRIPT_DIR/mosdesk.pkg" "$PACKAGE/mosdesk.pkg"
python3 "$PACKAGE_TOOL" "$PACKAGE" "$DIST/mosdesk.mpkg"
"$PROJECT_ROOT/mosWM/build.sh"

printf 'MOSdesk deliver package: %s\n' "$DIST/mosdesk.mpkg"
printf 'Package directory:       %s\n' "$PACKAGE"
for app in "$PACKAGE"/programs/mos_*.run; do
    if [ -e "$app" ]; then
        printf 'Built-in app:            0:/programs/%s\n' \
            "$(basename -- "$app")"
    fi
done
printf 'Desktop package:         %s\n' "$PROJECT_ROOT/mosWM/dist/mosWM.mpkg"
if [ -n "${SKIPPED_APPS:-}" ]; then
    printf 'Apps that failed to build: %s\n' "$SKIPPED_APPS" >&2
fi