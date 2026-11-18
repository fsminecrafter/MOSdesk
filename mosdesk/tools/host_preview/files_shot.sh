#!/bin/sh
# Renders the real mosWM Files window on the host and writes two PNGs.
# Needs: gcc, python3 with Pillow. No SDK, no Minimal-OS.
#   ./mosdesk/tools/host_preview/files_shot.sh [outdir]
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$HERE/../../.." && pwd)
OUT=${1:-$ROOT/preview-out}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
CF="-std=gnu17 -O1 -Wall -Wno-unused-function -iquote $HERE/stubs -I$ROOT -I$ROOT/mosdesk/libraries/ttrend/include"
for f in files_shot sdk_impl pwhash_stub; do gcc $CF -c "$HERE/$f.c" -o "$WORK/$f.o"; done
gcc "$WORK"/*.o -o "$WORK/files_shot"
S="$WORK/sandbox"
H="$S/0:/users/demo"
mkdir -p "$S/0:/programs/mosWM/assets/fonts" "$S/0:/programs/mosWM/assets/cursors/Posy" \
  "$H/Desktop" "$H/Documents" "$H/Downloads" "$H/Pictures" "$H/Music" \
  "$H/Projects/farcade/src" "$H/Projects/farcade/assets" "$H/Projects/farcade/docs"
cp "$ROOT"/mosdesk/fonts/*.otf "$S/0:/programs/mosWM/assets/fonts/"
cp "$ROOT/mosWM/assets/cursors/Posy/arrow.cur" "$S/0:/programs/mosWM/assets/cursors/Posy/"
P="$H/Projects/farcade"
head -c 18342 /dev/zero > "$P/main.c"
head -c 2210 /dev/zero > "$P/scene.h"
head -c 912 /dev/zero > "$P/Makefile"
head -c 4120 /dev/zero > "$P/README.md"
head -c 1630000 /dev/zero > "$P/game.run"
head -c 96000 /dev/zero > "$P/splash.qoi"
head -c 51000 /dev/zero > "$P/theme.adi"
head -c 333 /dev/zero > "$P/project.cfg"
head -c 7120 /dev/zero > "$P/script.lua"
head -c 120 /dev/zero > "$P/notes.txt"
mkdir -p "$OUT"
(cd "$S" && "$WORK/files_shot" "$WORK/a.ppm" "$WORK/b.ppm" >/dev/null)
python3 - "$WORK" "$OUT" <<'PY'
import sys
from PIL import Image
w, o = sys.argv[1:3]
for n in ("a", "b"):
    Image.open(f"{w}/{n}.ppm").save(f"{o}/files_{n}.png")
PY
echo "screenshots in $OUT"
