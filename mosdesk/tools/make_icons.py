#!/usr/bin/env python3
"""Draws the launcher icons of the built-in apps: mosdesk/apps/icons/<app>.bmp.

Each icon is a rounded tile with a gradient and a white symbol, drawn at 8x
and scaled down to 32 x 32 (SYSCALL_WINDOW_ICON_SIZE), saved as a 32-bit
BMP with alpha (what mosWM reads from a .run bundle's icon.bmp).

    python3 mosdesk/tools/make_icons.py            # every icon
    python3 mosdesk/tools/make_icons.py calc code   # some
"""
import math
import struct
import sys
from pathlib import Path

from PIL import Image, ImageDraw

SIZE = 32
SS = 8                      # supersampling
S = SIZE * SS
OUT = Path(__file__).resolve().parent.parent / "apps" / "icons"
WHITE = (255, 255, 255, 255)


def tile(top, bottom):
    """The rounded tile with a vertical gradient."""
    grad = Image.new("RGBA", (S, S))
    gd = ImageDraw.Draw(grad)
    for y in range(S):
        t = y / (S - 1)
        c = tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(3)) + (255,)
        gd.line([(0, y), (S, y)], fill=c)
    mask = Image.new("L", (S, S), 0)
    m = 1 * SS
    ImageDraw.Draw(mask).rounded_rectangle([m, m, S - 1 - m, S - 1 - m], radius=7 * SS, fill=255)
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    img.paste(grad, (0, 0), mask)
    # A light edge at the top.
    hl = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    ImageDraw.Draw(hl).rounded_rectangle([m, m, S - 1 - m, S // 2], radius=7 * SS, fill=(255, 255, 255, 28))
    hm = Image.new("L", (S, S), 0)
    hm.paste(mask)
    img = Image.alpha_composite(img, Image.composite(hl, Image.new("RGBA", (S, S), (0, 0, 0, 0)), hm))
    return img


def u(v):                   # icon units (0..32) -> supersampled pixels
    return v * SS


def line(d, pts, w, fill=WHITE):
    d.line([(u(x), u(y)) for x, y in pts], fill=fill, width=int(u(w)), joint="curve")
    for x, y in (pts[0], pts[-1]):              # round caps
        r = u(w) / 2
        d.ellipse([u(x) - r, u(y) - r, u(x) + r, u(y) + r], fill=fill)


def rrect(d, box, r, fill=None, outline=None, width=0):
    d.rounded_rectangle([u(box[0]), u(box[1]), u(box[2]), u(box[3])], radius=u(r), fill=fill, outline=outline,
                        width=int(u(width)) if width else 0)


def glyph_calc(d):
    rrect(d, (9, 7, 23, 25), 2.2, fill=WHITE)
    rrect(d, (11, 9, 21, 13), 1, fill=(232, 120, 40, 255))
    for row in range(3):
        for col in range(3):
            x, y = 11.2 + col * 3.6, 15 + row * 3.4
            rrect(d, (x, y, x + 2.4, y + 2.2), 0.6, fill=(232, 120, 40, 255))


def glyph_edit(d):
    rrect(d, (9, 6, 21, 26), 1.6, fill=WHITE)
    blue = (40, 110, 220, 255)
    for i, w in enumerate((8, 9, 6, 8, 5)):
        line(d, [(11.5, 10 + i * 3), (11.5 + w, 10 + i * 3)], 1.1, fill=blue)
    # a pencil over the corner
    line(d, [(17.5, 24.5), (25, 17)], 3.2, fill=(255, 214, 90, 255))
    line(d, [(23.5, 18.5), (25, 17)], 3.2, fill=(250, 140, 160, 255))
    d.polygon([(u(16.0), u(26.0)), (u(16.6), u(23.3)), (u(18.7), u(25.4))], fill=(60, 60, 70, 255))


def glyph_code(d):
    line(d, [(12, 10), (7.5, 16), (12, 22)], 2.4)
    line(d, [(20, 10), (24.5, 16), (20, 22)], 2.4)
    line(d, [(17.6, 8.5), (14.4, 23.5)], 2.2)


def glyph_tasks(d):
    for i, h in enumerate((6, 11, 8, 14)):
        x = 8.5 + i * 4.3
        rrect(d, (x, 24 - h, x + 2.8, 24), 1, fill=WHITE)
    line(d, [(7, 13), (11, 13), (13, 9), (16, 17), (18.5, 11), (20.5, 13), (25, 13)], 1.3,
         fill=(255, 255, 255, 170))


def glyph_terminal(d):
    rrect(d, (6, 8, 26, 24), 2, outline=WHITE, width=1.4)
    line(d, [(10, 13), (13, 16), (10, 19)], 1.8, fill=(120, 230, 150, 255))
    line(d, [(15, 19.5), (21, 19.5)], 1.8)


def glyph_clock(d):
    d.ellipse([u(7), u(7), u(25), u(25)], outline=WHITE, width=int(u(2)))
    for k in range(12):
        a = k * math.pi / 6
        r0, r1 = (6.2, 7.3) if k % 3 else (5.4, 7.3)
        line(d, [(16 + r0 * math.sin(a), 16 - r0 * math.cos(a)), (16 + r1 * math.sin(a), 16 - r1 * math.cos(a))],
             0.9)
    line(d, [(16, 16), (16, 10.5)], 1.6)
    line(d, [(16, 16), (20, 18)], 1.6)


def glyph_paint(d):
    d.ellipse([u(6.5), u(7.5), u(25.5), u(24.5)], fill=WHITE)
    d.ellipse([u(17), u(17), u(22), u(22)], fill=(0, 0, 0, 0))
    for (x, y, c) in ((11, 12, (240, 70, 90)), (15.5, 10, (250, 200, 50)), (20.5, 11.5, (60, 170, 240)),
                      (10.5, 17, (90, 200, 110))):
        d.ellipse([u(x - 1.8), u(y - 1.8), u(x + 1.8), u(y + 1.8)], fill=c + (255,))


def glyph_photos(d):
    rrect(d, (6, 8, 26, 24), 2, fill=WHITE)
    green = (40, 160, 90, 255)
    d.polygon([(u(8), u(22)), (u(14), u(14)), (u(18), u(19)), (u(20.5), u(16.5)), (u(24), u(22))], fill=green)
    d.ellipse([u(19), u(10), u(23), u(14)], fill=(250, 190, 40, 255))


def glyph_player(d):
    line(d, [(13, 21.5), (13, 9), (23, 7), (23, 19.5)], 1.8)
    d.ellipse([u(8.5), u(18.5), u(13.5), u(23.5)], fill=WHITE)
    d.ellipse([u(18.5), u(16.5), u(23.5), u(21.5)], fill=WHITE)


def glyph_video(d):
    rrect(d, (6, 8, 26, 24), 2.5, outline=WHITE, width=1.4)
    d.polygon([(u(13.5), u(11.5)), (u(13.5), u(20.5)), (u(21), u(16))], fill=WHITE)


def glyph_snake(d):
    pts = [(8, 21), (11, 21), (13, 18), (13, 14), (16, 11), (19, 14), (19, 18), (21, 21), (24, 21)]
    line(d, pts, 3)
    d.ellipse([u(22), u(19), u(26), u(23)], fill=WHITE)
    d.ellipse([u(24.2), u(20.4), u(25.2), u(21.4)], fill=(40, 120, 60, 255))


def glyph_golf(d):
    d.ellipse([u(9), u(21), u(23), u(25)], fill=(255, 255, 255, 120))
    line(d, [(16, 23), (16, 7)], 1.4)
    d.polygon([(u(16.5), u(7)), (u(24), u(10)), (u(16.5), u(13))], fill=(240, 80, 80, 255))
    d.ellipse([u(10), u(19), u(13), u(22)], fill=WHITE)


def glyph_cube(d):
    top = [(16, 7), (24, 11.5), (16, 16), (8, 11.5)]
    left = [(8, 11.5), (16, 16), (16, 25), (8, 20.5)]
    right = [(16, 16), (24, 11.5), (24, 20.5), (16, 25)]
    for poly, a in ((top, 255), (left, 190), (right, 140)):
        d.polygon([(u(x), u(y)) for x, y in poly], fill=(255, 255, 255, a))


def glyph_installer(d):
    line(d, [(16, 7), (16, 18)], 2.2)
    line(d, [(11.5, 13.5), (16, 18), (20.5, 13.5)], 2.2)
    line(d, [(8, 19), (8, 24), (24, 24), (24, 19)], 2)


ICONS = {
    "calc": ((255, 160, 70), (232, 100, 30), glyph_calc),
    "edit": ((80, 150, 255), (40, 100, 220), glyph_edit),
    "code": ((140, 110, 250), (95, 65, 210), glyph_code),
    "tasks": ((50, 200, 190), (20, 140, 150), glyph_tasks),
    "terminal": ((70, 80, 100), (35, 40, 55), glyph_terminal),
    "clock": ((110, 120, 250), (70, 70, 200), glyph_clock),
    "paint": ((250, 110, 170), (210, 60, 130), glyph_paint),
    "photos": ((80, 210, 140), (30, 150, 100), glyph_photos),
    "player": ((255, 100, 110), (215, 50, 70), glyph_player),
    "video": ((255, 130, 80), (220, 70, 40), glyph_video),
    "snake": ((100, 210, 110), (40, 140, 60), glyph_snake),
    "golf": ((150, 220, 90), (80, 160, 50), glyph_golf),
    "cube": ((70, 200, 240), (30, 130, 210), glyph_cube),
    "installer": ((120, 140, 170), (70, 85, 115), glyph_installer),
}


def write_bmp(img, path):
    """32-bit BI_RGB with alpha, bottom-up (mosWM's decode_bmp_icon)."""
    img = img.convert("RGBA")
    w, h = img.size
    px = img.load()
    rows = []
    for y in range(h - 1, -1, -1):
        row = bytearray()
        for x in range(w):
            r, g, b, a = px[x, y]
            row += bytes((b, g, r, a))
        rows.append(bytes(row))
    data = b"".join(rows)
    header = struct.pack("<2sIHHI", b"BM", 14 + 40 + len(data), 0, 0, 54)
    info = struct.pack("<IiiHHIIiiII", 40, w, h, 1, 32, 0, len(data), 2835, 2835, 0, 0)
    path.write_bytes(header + info + data)


def render(name):
    top, bottom, glyph = ICONS[name]
    img = tile(top, bottom)
    layer = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    glyph(ImageDraw.Draw(layer))
    img = Image.alpha_composite(img, layer)
    # Scaled with premultiplied alpha: no colour from fully transparent pixels.
    return img.convert("RGBa").resize((SIZE, SIZE), Image.LANCZOS).convert("RGBA")


def from_png(src, name):
    """An icon from an existing logo (for apps that have one)."""
    img = Image.open(src).convert("RGBa").resize((SIZE, SIZE), Image.LANCZOS).convert("RGBA")
    write_bmp(img, OUT / f"{name}.bmp")


def main(names):
    OUT.mkdir(parents=True, exist_ok=True)
    for name in names or sorted(ICONS):
        write_bmp(render(name), OUT / f"{name}.bmp")
        print(f"wrote {OUT / (name + '.bmp')}")


if __name__ == "__main__":
    main(sys.argv[1:])
