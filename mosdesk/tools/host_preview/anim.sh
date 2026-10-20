#!/bin/sh
# Scripted session on the REAL mosWM + Settings client, rendered to an mp4/gif:
# first-run onboarding, Start menu with the built-in apps, the Files window,
# photo wallpapers, Settings (theme, wallpaper fit, cursor pack, Debug FPS
# meter), window drag/maximize, and the sign-in screen after a restart.
# Needs: gcc, python3 + Pillow, ffmpeg (for mp4/gif).
# Output: <outdir>/mosdesk_demo.mp4 (copy it to the repo root), a gif, and a
# still per scene.
#   ./mosdesk/tools/host_preview/anim.sh [outdir]
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$HERE/../../.." && pwd)
OUT=${1:-$ROOT/preview-out}
WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
CF="-std=gnu17 -O1 -w -iquote $HERE/stubs -I$ROOT -I$HERE -I$ROOT/mosdesk/libraries/ttrend/include"
for f in anim_tu settings_tu sdk_impl pwhash_stub anim_driver; do gcc $CF -c "$HERE/$f.c" -o "$WORK/$f.o"; done
gcc $CF -c "$ROOT/mosdesk/libraries/ttrend/src/ttrend.c" -o "$WORK/ttrend.o"
gcc "$WORK"/*.o -lm -o "$WORK/harness"
S="$WORK/sandbox"
P="$S/0:/programs"
mkdir -p "$P/mosdesk/fonts" "$P/mosWM/assets/fonts" "$P/mosWM/assets/cursors" "$S/0:/users"
cp "$ROOT"/mosdesk/fonts/*.otf "$ROOT/mosdesk/fonts/default.font" "$S/0:/programs/mosdesk/fonts/"
cp "$ROOT"/mosdesk/fonts/*.otf "$S/0:/programs/mosWM/assets/fonts/"
cp -r "$ROOT"/mosWM/assets/cursors/* "$S/0:/programs/mosWM/assets/cursors/"
# What a normal install has in 0:/programs: mosdesk, mosWM and the six apps.
# (Placeholders only: the host build lists them, it cannot run them.)
: > "$P/mosdesk/mosdesk.run"; : > "$P/mosWM/mosWM.run"
for app in calc clock edit golf paint snake; do : > "$P/mos_$app.run"; done
# Photo wallpapers + thumbnails exactly as shipped.
ln -s "$ROOT/mosWM/assets/wallpapers" "$P/mosWM/assets/wallpapers"
# A little sample content so Files has something to browse.
U="$S/0:/users/demo"
mkdir -p "$U/Desktop" "$U/Documents" "$U/Pictures" "$U/Music" "$U/Downloads"
printf 'Welcome to MOSdesk.\n' > "$U/Desktop/welcome.txt"
printf 'Meeting notes\n- ship the demo\n' > "$U/Documents/notes.txt"
printf '# Todo\n- [x] boot\n- [ ] sleep\n' > "$U/Documents/todo.md"
printf '#include <stdio.h>\nint main(void){puts("hi");}\n' > "$U/Documents/hello.c"
ln -s "$ROOT/mosWM/assets/wallpapers/jonatan-pie-3l3RwQdHRHg-unsplash.qoi" "$U/Pictures/aurora.qoi"
ln -s "$ROOT/mosWM/assets/wallpapers/cristina-gottardi-CSpjU6hYo_0-unsplash.qoi" "$U/Pictures/mountains.qoi"
mkdir -p "$OUT"
MP4=
if command -v ffmpeg >/dev/null 2>&1; then MP4="$OUT/mosdesk_demo.mp4"; fi
(cd "$S" && "$WORK/harness" "$OUT" $MP4)
python3 - "$OUT" <<'PY'
import sys, glob
from PIL import Image
for f in glob.glob(sys.argv[1] + "/*.ppm"):
    Image.open(f).save(f[:-4] + ".png")
PY
rm -f "$OUT"/*.ppm
if [ -n "$MP4" ]; then
  ffmpeg -loglevel error -y -i "$MP4" -vf "fps=15,scale=854:-1:flags=lanczos,split[a][b];[a]palettegen=max_colors=128:stats_mode=diff[p];[b][p]paletteuse=dither=bayer:bayer_scale=4:diff_mode=rectangle" "$OUT/mosdesk_demo.gif"
fi
echo "output in $OUT"
