#!/bin/sh
# Renders the window open/close/minimize animations on the REAL mosWM
# compositor (host build) as filmstrips, one sheet per window style, and,
# when ffmpeg is present, a short video of each effect in turn.
# Needs: gcc, ffmpeg (for PNG/MP4 output). No SDK, no Minimal-OS.
#   ./mosdesk/tools/host_preview/window_anim.sh [outdir]
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$HERE/../../.." && pwd)
OUT=${1:-$ROOT/preview-out}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
CF="-std=gnu17 -O2 -Wall -Wno-unused-function -iquote $HERE/stubs -I$ROOT -I$ROOT/mosdesk/libraries/ttrend/include"
for f in window_anim_test sdk_impl pwhash_stub; do gcc $CF -c "$HERE/$f.c" -o "$WORK/$f.o"; done
gcc "$WORK"/*.o -o "$WORK/window_anim_test"
S="$WORK/sandbox"
mkdir -p "$S/0:/programs/mosWM/assets/fonts" \
  "$S/0:/programs/mosWM/assets/cursors/Posy" "$OUT"
cp "$ROOT/mosWM/assets/cursors/Posy/arrow.cur" \
  "$S/0:/programs/mosWM/assets/cursors/Posy/"
cp "$ROOT"/mosdesk/fonts/*.otf "$S/0:/programs/mosWM/assets/fonts/"
VIDEO=
if command -v ffmpeg >/dev/null 2>&1; then VIDEO="$WORK/session.rgba"; fi
(cd "$S" && "$WORK/window_anim_test" "$OUT" $VIDEO)
if command -v ffmpeg >/dev/null 2>&1; then
  for f in "$OUT"/window_anims_*.ppm; do
    ffmpeg -loglevel error -y -i "$f" "${f%.ppm}.png" && rm -f "$f"
  done
  ffmpeg -loglevel error -y -f rawvideo -pixel_format bgra -video_size 1024x640 \
    -framerate 60 -i "$VIDEO" -pix_fmt yuv420p "$OUT/window_anims.mp4"
fi
echo "output in $OUT"
