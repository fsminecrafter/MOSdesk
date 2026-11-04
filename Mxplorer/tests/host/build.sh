#!/bin/sh
# Builds the host renderer from the Mxplorer directory: tests/host/build.sh [js]
set -e
cd "$(dirname "$0")/../.."
OUT=${OUT:-/tmp/mxplorer-render}
JS=tests/host/js_stub.c
[ "${1:-}" = js ] && JS="js.c js_runtime.c js_dom.c"
gcc -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter -DMX_HOST \
    -I include -idirafter ../../Minimal-OS-SDK/include \
    tests/host/render.c html.c media.c css.c dom.c util.c text.c gfx.c icons_data.c devlog.c $JS \
    -o "$OUT" -lm
