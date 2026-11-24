#!/usr/bin/env python3
"""Embeds prelude/*.js into js_dom.c as js_lang_prelude.

The preludes are JavaScript that runs before every page's scripts (newer
built-ins, Promise combinators, MutationObserver, ...). Edit the .js files,
then run this from the Mxplorer directory: python3 tools/gen_prelude.py
"""
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SOURCES = ["lang.js", "dom.js"]
BEGIN = "/* BEGIN GENERATED PRELUDE (tools/gen_prelude.py) */\n"
END = "/* END GENERATED PRELUDE */\n"


def c_literal(text):
    lines = []
    for line in text.strip("\n").split("\n"):
        escaped = line.replace("\\", "\\\\").replace('"', '\\"')
        lines.append('"' + escaped + '\\n"')
    return "\n".join(lines)


def main():
    parts = []
    for name in SOURCES:
        with open(os.path.join(ROOT, "prelude", name), encoding="utf-8") as f:
            parts.append("/* prelude/%s */\n%s" % (name, c_literal(f.read())))
    block = BEGIN + "static const char js_lang_prelude[] =\n" + "\n".join(parts) + ";\n" + END
    path = os.path.join(ROOT, "js_dom.c")
    with open(path, encoding="utf-8") as f:
        source = f.read()
    pattern = re.compile(re.escape(BEGIN) + ".*?" + re.escape(END), re.S)
    if not pattern.search(source):
        raise SystemExit("markers not found in js_dom.c")
    source = pattern.sub(lambda m: block, source)
    with open(path, "w", encoding="utf-8") as f:
        f.write(source)


if __name__ == "__main__":
    main()
