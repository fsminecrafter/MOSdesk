#!/bin/sh
# Builds the Code editor (mosdesk/apps/code) for the host and runs a scripted
# session: it feeds keys and mouse events, checks the editor's behaviour
# (prints PASS / FAIL lines) and saves a screenshot.
# Needs: gcc, python3 with Pillow. No SDK build, no Minimal-OS.
#
#   ./mosdesk/tools/host_preview/code_shot.sh [outdir]      (default: docs/screenshots)
#
# The SDK headers are used unchanged except that their `int $0x80` system call
# instructions are replaced by calls into code_host.c (a copy in a temp folder).
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$HERE/../../.." && pwd)
SDK=${MOSDESK_SDK_DIR:-$ROOT/Minimal-OS-SDK}
[ -d "$SDK/include" ] || SDK=/home/user/Minimal-OS-SDK
OUT=${1:-$ROOT/docs/screenshots}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$OUT"

# --- SDK headers with the system calls redirected ---
cp -R "$SDK/include" "$WORK/inc"
python3 - "$WORK/inc" <<'PY'
import re, sys
d = sys.argv[1]
def patch(name, subs):
    p = f"{d}/{name}"
    s = open(p).read()
    s = "extern long host_syscall(long, long, long, long);\nextern long host_syscall4(long, long, long, long, long);\n" + s
    for pat, rep in subs:
        s, n = re.subn(pat, rep, s, count=1, flags=re.S)
        assert n == 1, (name, pat)
    open(p, "w").write(s)
asm = r'__asm__ volatile\(\s*"int \$0x80".*?\);'
patch("x86_64/user_common.h", [
    (r'long ret;\s*' + asm, 'long ret = host_syscall(num, a1, a2, a3);'),
    (r'register long r10 __asm__\("r10"\) = a4;\s*long ret;\s*' + asm, 'long ret = host_syscall4(num, a1, a2, a3, a4);'),
])
patch("x86_64/user_syscalls.h", [(r'uint64_t result;\s*' + asm, 'uint64_t result = (uint64_t)host_syscall((long)number, (long)arg1, (long)arg2, (long)arg3);')])
patch("window.h", [(r'uint64_t result;\s*' + asm, 'uint64_t result = (uint64_t)host_syscall((long)SYS_WINDOW, (long)(uintptr_t)request, 0, 0);')])
PY

# --- a sandbox "0:/" with a small project and the fonts ---
R="$WORK/root"
mkdir -p "$R/proj/src" "$R/tmp" "$R/programs/mosdesk/fonts" "$R/programs/mosWM/assets/fonts"
cp "$ROOT/mosdesk/fonts/JetBrainsMono-Regular.ttf" "$R/programs/mosdesk/fonts/"
cp "$ROOT/mosdesk/fonts/Inter-Regular.otf" "$ROOT/mosdesk/fonts/Inter-Medium.otf" "$R/programs/mosWM/assets/fonts/"
cat > "$R/proj/main.c" <<'SRC'
/*
 * wordcount - counts words, lines and bytes of the files it is given.
 * A small sample for the MOSdesk Code editor.
 */
#include <stdio.h>
#include <string.h>
#include "util.h"

#define MAX_FILES 16
#define BUFFER    4096

typedef struct {
    const char* name;
    size_t bytes, words, lines;
} Stats;

static int count_words(const char* text, size_t len) {
    int words = 0, inside = 0;
    for (size_t i = 0; i < len; i++) {
        char c = text[i];
        if (c == ' ' || c == '\n' || c == '\t') {
            inside = 0;
        } else if (!inside) {
            inside = 1;
            words++;
        }
    }
    return words;   // one pass, no allocation
}

static void report(const Stats* s, int verbose) {
    if (verbose) {
        printf("%-20s %6zu bytes %5zu words %4zu lines\n",
               s->name, s->bytes, s->words, s->lines);
    } else {
        printf("%s: %zu\n", s->name, s->words);
    }
}

int main(int argc, char** argv) {
    Stats all[MAX_FILES];
    char buffer[BUFFER];
    int verbose = 0x1, n = 0;

    for (int i = 1; i < argc && n < MAX_FILES; i++) {
        if (strcmp(argv[i], "-q") == 0) { verbose = 0; continue; }
        size_t got = read_file(argv[i], buffer, sizeof buffer - 1);
        buffer[got] = '\0';
        all[n].name = argv[i];
        all[n].bytes = got;
        all[n].words = count_words(buffer, got);
        all[n].lines = count_lines(buffer);
        report(&all[n], verbose);
        n++;
    }
    if (n == 0) {
        fprintf(stderr, "usage: wordcount [-q] file...\n");
        return 1;
    }
    return 0;
}
SRC
cat > "$R/proj/util.h" <<'SRC'
#ifndef UTIL_H
#define UTIL_H

#include <stddef.h>

/* Reads up to `cap` bytes of a file into `out`; returns how many. */
size_t read_file(const char* path, char* out, size_t cap);

static inline size_t count_lines(const char* text) {
    size_t lines = 0;
    while (*text) if (*text++ == '\n') lines++;
    return lines;
}

#endif
SRC
printf 'CC = gcc\nCFLAGS = -O2 -Wall\n\nall: wordcount\n\nwordcount: main.c util.h\n\t$(CC) $(CFLAGS) -o $@ main.c\n' > "$R/proj/Makefile"
printf '# wordcount\n\nCounts **words**, lines and bytes. See `main.c`.\n\n- fast\n- tiny\n' > "$R/proj/README.md"
printf 'int answer(void) { return 42; }\n' > "$R/proj/src/answer.c"
cat > "$R/proj/src/tool.py" <<'SRC'
#!/usr/bin/env python3
"""Tiny tool: prints a greeting for every name given."""
import sys
from dataclasses import dataclass

MAX_NAMES = 8


@dataclass
class Greeting:
    name: str
    loud: bool = False

    def render(self, width=0x10):
        text = f"Hello, {self.name}!"  # f-string
        return text.upper() if self.loud else text


def main(argv):
    names = [a for a in argv[1:] if not a.startswith('-')]
    if not names:
        print("usage: tool.py NAME...", file=sys.stderr)
        return 1
    for n in names[:MAX_NAMES]:
        print(Greeting(n, loud=True).render())
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
SRC
cat > "$R/proj/src/build.sh" <<'SRC'
#!/bin/sh
# Build and install.
set -eu
PREFIX=${PREFIX:-/usr/local}
for tool in gcc make; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "missing: $tool" >&2
        exit 1
    fi
done
make -j"$(nproc)" CC="gcc" all && echo "built $(date +%F)"
[ "$1" = "install" ] && install -m 755 wordcount "$PREFIX/bin/"
SRC
cat > "$R/proj/src/init.lua" <<'SRC'
-- Greets people.
local names = { "ada", "lin", "kay" }

--[[ a block comment
     over two lines ]]
local function greet(name, times)
  times = times or 2
  for i = 1, times do
    print(("Hello, %s! (%d)"):format(name:upper(), i))
  end
end

for _, n in ipairs(names) do
  if #n > 2 then greet(n) end
end
SRC
cat > "$R/proj/src/data.json" <<'SRC'
{
  "name": "wordcount",
  "version": "1.2.0",
  "tags": ["cli", "text"],
  "limits": { "files": 16, "buffer": 4096, "ratio": 0.75 },
  "verbose": true,
  "license": null
}
SRC
cat > "$R/proj/src/app.js" <<'SRC'
// Fetches a list and renders it.
const API = 'https://example.org/items';

async function load(limit = 10) {
  const res = await fetch(`${API}?limit=${limit}`);
  if (!res.ok) throw new Error("bad status " + res.status);
  return (await res.json()).map(item => ({ ...item, seen: false }));
}

class List extends Array {
  render(el) { el.innerHTML = this.map(i => `<li>${i.name}</li>`).join(''); }
}
SRC


# --- build ---
CF="${CODE_SHOT_CFLAGS:-} -std=gnu11 -O1 ${CODE_SHOT_WARN:--w} -ffreestanding -fno-builtin -DTTREND_OUTLINE_NO_FILE"
gcc $CF -I"$WORK/inc" -I"$ROOT/mosdesk/apps/code" -I"$HERE" -c "$HERE/code_shot.c" -o "$WORK/shot.o"
gcc ${CODE_SHOT_CFLAGS:-} -std=gnu11 -O1 -w -iquote "$SDK/include" -I"$HERE" -c "$HERE/code_host.c" -o "$WORK/host.o"
gcc ${CODE_SHOT_CFLAGS:-} "$WORK/shot.o" "$WORK/host.o" -o "$WORK/code_shot"

# --- run ---
CODE_HOST_ROOT="$R" CODE_SHOT_DIR="$WORK" "$WORK/code_shot" && echo "all checks passed" || { echo "SOME CHECKS FAILED" >&2; STATUS=1; }
# The harness writes PPM files; convert them. code.png is the one that is kept
# (CODE_SHOT_KEEP=<dir> keeps the extra screenshots of the other languages and dialogs too).
python3 - "$WORK" "$OUT" "${CODE_SHOT_KEEP:-}" <<'PY'
import glob, os, sys
from PIL import Image
work, out, keep = sys.argv[1:4]
for p in sorted(glob.glob(work + "/*.png")):
    name = os.path.basename(p)
    dest = out if name == "code.png" else keep
    if not dest:
        continue
    os.makedirs(dest, exist_ok=True)
    Image.open(p).save(os.path.join(dest, name))
PY
echo "screenshot: $OUT/code.png"
exit ${STATUS:-0}
