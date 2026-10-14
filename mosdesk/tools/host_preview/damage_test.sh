#!/bin/sh
# Builds and runs damage_test.c: the real mosWM compositor on the host, with
# every partially repainted frame compared against a full repaint.
# Needs: gcc. No SDK, no Minimal-OS.
#   ./mosdesk/tools/host_preview/damage_test.sh [steps]
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$HERE/../../.." && pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
CF="-std=gnu17 -O1 -Wall -Wno-unused-function -iquote $HERE/stubs -I$ROOT -I$ROOT/mosdesk/libraries/ttrend/include"
for f in damage_test sdk_impl pwhash_stub; do gcc $CF -c "$HERE/$f.c" -o "$WORK/$f.o"; done
gcc "$WORK"/*.o -o "$WORK/damage_test"
for fonts in 1 0; do
  S="$WORK/run$fonts"
  mkdir -p "$S/0:/programs/mosWM/assets/fonts" \
    "$S/0:/programs/mosWM/assets/cursors/Posy"
  cp "$ROOT/mosWM/assets/cursors/Posy/arrow.cur" \
    "$S/0:/programs/mosWM/assets/cursors/Posy/"
  if [ "$fonts" = 1 ]; then
    cp "$ROOT"/mosdesk/fonts/*.otf "$S/0:/programs/mosWM/assets/fonts/"
  fi
  (cd "$S" && "$WORK/damage_test" "${1:-600}")
done
(cd "$WORK/run1" && "$WORK/damage_test" "${1:-600}" "" nolayer)
