"""64x64 pixel-art icon definitions for MOSdesk (light from the top-left)."""
import math
from PIL import Image, ImageDraw
from pixlib import Canvas, S, hexc, lighten, darken

# ------------------------------------------------------------------ helpers
def poly_mask(pts):
    m = Image.new('L', (S, S), 0)
    ImageDraw.Draw(m).polygon(pts, fill=255)
    return lambda x, y: 0 <= x < S and 0 <= y < S and m.getpixel((x, y)) > 0

def rrect_mask(x0, y0, x1, y1, r):
    m = Image.new('L', (S, S), 0)
    ImageDraw.Draw(m).rounded_rectangle([x0, y0, x1, y1], r, fill=255)
    return lambda x, y: 0 <= x < S and 0 <= y < S and m.getpixel((x, y)) > 0

def ell_mask(x0, y0, x1, y1):
    m = Image.new('L', (S, S), 0)
    ImageDraw.Draw(m).ellipse([x0, y0, x1, y1], fill=255)
    return lambda x, y: 0 <= x < S and 0 <= y < S and m.getpixel((x, y)) > 0

GLYPH = {  # 5x7 bitmap font for badges
    'P': ["####.", "#...#", "#...#", "####.", "#....", "#....", "#...."],
    'D': ["####.", "#...#", "#...#", "#...#", "#...#", "#...#", "####."],
    'F': ["#####", "#....", "#....", "####.", "#....", "#....", "#...."],
}
def text(c, x, y, s, color, scale=1):
    for ch in s:
        for r, row in enumerate(GLYPH[ch]):
            for k, v in enumerate(row):
                if v == '#':
                    for a in range(scale):
                        for b in range(scale):
                            c.px(x + k * scale + a, y + r * scale + b, color)
        x += 6 * scale

def finish(c, outline, shadow=True):
    c.outline(outline)
    if shadow:
        c.shadow(2, 3, 64)
    return c

# --------------------------------------------------------------- icons
def folder(emblem=None):
    c = Canvas()
    # back panel + tab
    c.poly([(4, 15), (4, 10), (7, 8), (23, 8), (27, 13), (60, 13), (60, 15)], '#c27d22')
    c.rrect(4, 12, 60, 54, 3, '#c27d22')
    c.rect(6, 12, 58, 14, '#d79029')
    c.hline(8, 22, 9, '#efb149')
    c.hline(6, 23, 10, '#e6a13a')
    # papers peeking out
    c.rect(9, 17, 55, 30, '#e9eef1'); c.hline(9, 55, 17, '#ffffff')
    c.rect(11, 20, 40, 21, '#b8c6cf'); c.rect(11, 24, 33, 25, '#c9d5dc')
    c.poly([(14, 16), (50, 14), (52, 17), (14, 18)], '#ffffff')
    # front panel
    ramp = ['#ffe08a', '#fbd272', '#f7c15c', '#f2b04b', '#ea9f3e', '#df8e33']
    c.vgrad(3, 24, 61, 56, ramp, rrect_mask(3, 24, 61, 56, 4))
    c.hline(6, 58, 24, '#fff2bd'); c.hline(5, 59, 25, '#ffe9a0')
    c.dither(4, 26, 60, 27, '#ffe08a', 0, rrect_mask(3, 24, 61, 56, 4))
    c.hline(5, 59, 56, '#c47a24'); c.hline(6, 58, 55, '#d3862b')
    c.vline(3, 28, 52, '#ffeaa8')
    c.vline(61, 28, 52, '#d58a2e')
    if emblem == 'home':
        # little house cut into the front panel
        c.poly([(32, 30), (20, 40), (44, 40)], '#fff6d8')
        c.poly([(32, 32), (23, 40), (41, 40)], '#e8553f')
        c.rect(24, 40, 40, 51, '#fff6d8')
        c.rect(25, 41, 39, 51, '#f4e2b0')
        c.rect(30, 44, 34, 51, '#8a5a2b'); c.px(33, 48, '#f6d36a')
        c.rect(26, 43, 28, 45, '#7fb7e0'); c.rect(36, 43, 38, 45, '#7fb7e0')
        c.rect(38, 30, 40, 35, '#b9442f')
        c.hline(24, 40, 40, '#b9442f')
    return finish(c, '#5a3009')

def drive():
    c = Canvas()
    # top face
    c.poly([(12, 14), (52, 14), (60, 26), (4, 26)], '#f1f5f7')
    c.poly([(14, 16), (50, 16), (55, 24), (9, 24)], '#dde5ea')
    c.poly([(16, 17), (48, 17), (51, 21), (13, 21)], '#cbd6dd')
    c.line([(12, 14), (4, 26)], '#ffffff')
    # front face
    ramp = ['#e4eaee', '#d3dce2', '#bfcbd3', '#a9b8c2', '#93a4af']
    c.vgrad(4, 26, 60, 46, ramp)
    c.hline(4, 60, 26, '#ffffff')
    c.dither(4, 27, 60, 28, '#f3f6f8', 1)
    # bottom plinth
    c.rect(4, 47, 60, 53, '#5d6f7b'); c.hline(4, 60, 47, '#7d909c')
    c.rect(4, 52, 60, 53, '#445560')
    c.rect(8, 53, 14, 55, '#2f3c46'); c.rect(50, 53, 56, 55, '#2f3c46')
    # vents
    for i in range(9):
        x = 9 + i * 3
        c.vline(x, 31, 41, '#6c7e8a'); c.vline(x + 1, 31, 41, '#d9e2e7')
    # label plate + LED
    c.rrect(36, 32, 55, 41, 2, '#2c3a44')
    c.rect(38, 34, 45, 35, '#4f6676'); c.rect(38, 37, 50, 38, '#4f6676')
    c.ell(51, 33, 54, 36, '#67e6a2'); c.px(51, 33, '#d9ffe9')
    c.rect(37, 49, 46, 50, '#7e919d')
    c.px(55, 49, '#6fe8a8'); c.px(56, 49, '#6fe8a8')
    return finish(c, '#26333d')

def paper_base(c, accent=None):
    pts = [(12, 4), (39, 4), (52, 17), (52, 59), (12, 59)]
    mask = poly_mask(pts)
    c.vgrad(12, 4, 52, 59, ['#ffffff', '#f8fafb', '#f1f5f7', '#e8eef2', '#dfe7ec'], mask)
    c.vline(12, 5, 58, '#ffffff')
    c.vline(51, 18, 58, '#cdd8df')
    c.hline(13, 51, 58, '#c2cfd7')
    # folded corner
    c.poly([(39, 4), (52, 17), (39, 17)], '#c6d3db')
    c.poly([(39, 4), (39, 17), (52, 17)], '#d7e1e7')
    c.line([(40, 6), (40, 16)], '#eef3f6'); c.line([(41, 16), (50, 16)], '#eef3f6')
    c.line([(39, 17), (52, 17)], '#aab9c3'); c.line([(39, 4), (39, 17)], '#aab9c3')

def document():
    c = Canvas(); paper_base(c)
    c.rect(18, 22, 33, 24, '#4f8fc7'); c.hline(18, 33, 22, '#7db1de')
    for i, w in enumerate([28, 31, 24, 30, 27, 17]):
        y = 29 + i * 5
        c.rect(18, y, 18 + w - 1, y + 1, '#8fa3b0')
        c.hline(18, 18 + w - 1, y, '#a9bac5')
    return finish(c, '#34444f')

def pdf():
    c = Canvas(); paper_base(c)
    for i, w in enumerate([22, 26]):
        y = 20 + i * 4
        c.rect(18, y, 18 + w - 1, y + 1, '#a3b4bf')
    # red ribbon badge
    c.rect(8, 30, 56, 46, '#c8383d')
    c.rect(8, 30, 56, 32, '#e5575b'); c.hline(8, 56, 30, '#ff8b8b')
    c.rect(8, 44, 56, 46, '#9c2328')
    c.poly([(8, 47), (12, 47), (12, 51)], '#6f1519')
    c.poly([(56, 47), (52, 47), (52, 51)], '#6f1519')
    text(c, 17, 35, 'PDF', '#ffffff', 1)
    # stretch letters to 2x width by drawing second layer shadow
    text(c, 16, 35, 'PDF', '#7a1a1e', 1) if False else None
    for i, w in enumerate([22, 26, 14]):
        y = 49 + i * 3
        if y + 1 < 58:
            c.rect(18, y, 18 + w - 1, y, '#a3b4bf')
    return finish(c, '#34444f')

def image():
    c = Canvas()
    c.rrect(5, 9, 58, 55, 4, '#f6f9fa')
    c.vgrad(5, 9, 58, 55, ['#ffffff', '#f2f6f8', '#e4ebef'], rrect_mask(5, 9, 58, 55, 4))
    # picture
    pm = rrect_mask(10, 14, 53, 49, 1)
    c.vgrad(10, 14, 53, 49, ['#5eb4f0', '#79c4f5', '#9cd5f8', '#c1e5fa', '#e3f4fc'], pm)
    # sun
    c.ell(39, 17, 49, 27, '#ffd45e'); c.ell(40, 18, 47, 25, '#ffe98d'); c.px(41, 19, '#fff8cf')
    # cloud
    c.ell(14, 20, 24, 26, '#ffffff'); c.ell(19, 17, 28, 25, '#ffffff'); c.hline(15, 26, 26, '#dff0fb')
    # far mountains
    mm = pm
    c.poly([(10, 40), (21, 25), (27, 33), (31, 28), (44, 42), (44, 49), (10, 49)], '#6d8fb8')
    c.poly([(21, 25), (17, 31), (20, 30), (22, 33), (24, 30), (25, 29)], '#f3f8fc')
    c.poly([(31, 28), (28, 33), (31, 32), (33, 34), (34, 31)], '#f3f8fc')
    c.poly([(10, 40), (21, 25), (27, 33), (31, 28), (44, 42)], '#7ea1c9') if False else None
    # near hills
    c.poly([(10, 41), (22, 36), (34, 41), (46, 36), (53, 38), (53, 49), (10, 49)], '#58b36c')
    c.poly([(10, 44), (20, 41), (32, 45), (44, 41), (53, 44), (53, 49), (10, 49)], '#3f9a58')
    c.dither(10, 36, 53, 38, '#7cca8a', 0, lambda x, y: y >= 36 and c.img.getpixel((x, y))[:3] == hexc('#58b36c')[:3])
    c.hline(10, 53, 49, '#2f7b46')
    # frame bevel
    c.hline(10, 53, 14, '#3d7fb0'); c.vline(10, 14, 49, '#4a8cbb')
    c.hline(10, 53, 50, '#c8d3da'); c.vline(54, 14, 50, '#cdd8df')
    return finish(c, '#34444f')

def audio():
    c = Canvas()
    m = rrect_mask(6, 6, 57, 57, 8)
    c.vgrad(6, 6, 57, 57, ['#7cc1f8', '#62adec', '#4a96dc', '#387fc8', '#2b69b0', '#255a9b'], m)
    c.rrect(6, 6, 57, 8, 2, '#a9dcfd') if False else None
    c.hline(11, 52, 6, '#c0e5ff'); c.vline(6, 11, 52, '#98d0fa')
    c.dither(7, 7, 56, 12, '#8fcbf9', 0, m)
    c.hline(11, 52, 57, '#1e4a82'); c.vline(57, 11, 52, '#1f4f8c')
    # two beamed notes
    c.ell(14, 40, 26, 49, '#ffffff'); c.ell(15, 40, 24, 46, '#f1f8ff'); c.hline(17, 22, 41, '#ffffff')
    c.ell(34, 36, 46, 45, '#ffffff'); c.ell(35, 36, 44, 42, '#f1f8ff')
    c.rect(24, 17, 26, 44, '#ffffff'); c.vline(24, 18, 43, '#dbeeff')
    c.rect(44, 13, 46, 40, '#ffffff'); c.vline(44, 14, 39, '#dbeeff')
    c.poly([(24, 16), (46, 11), (46, 20), (24, 25)], '#ffffff')
    c.poly([(24, 16), (46, 11), (46, 13), (24, 18)], '#ffffff')
    c.poly([(24, 22), (46, 17), (46, 20), (24, 25)], '#d3e9fb')
    c.line([(24, 16), (46, 11)], '#ffffff')
    return finish(c, '#183a66')

def video():
    c = Canvas()
    # clapper body
    c.rect(7, 25, 57, 56, '#2d3b4f')
    c.vgrad(7, 25, 57, 56, ['#4a5f7a', '#3e526b', '#334559', '#2b3a4c'])
    c.hline(7, 57, 25, '#6d86a3'); c.hline(7, 57, 56, '#1e2937')
    c.dither(8, 26, 56, 28, '#586f8b', 0)
    # film sprocket holes
    for i in range(6):
        c.rect(10 + i * 8, 28, 13 + i * 8, 30, '#1b2431')
        c.rect(10 + i * 8, 51, 13 + i * 8, 53, '#1b2431')
    # play button
    c.ell(24, 31, 41, 48, '#f4f8fb'); c.ell(25, 31, 40, 46, '#ffffff')
    c.poly([(30, 35), (30, 45), (38, 40)], '#e8553f'); c.poly([(30, 35), (30, 38), (34, 40)], '#ff8a73')
    # striped clap bars
    top = [(7, 12), (57, 6), (59, 17), (9, 23)]
    m = poly_mask(top)
    for y in range(S):
        for x in range(S):
            if m(x, y):
                s = ((x + y // 2) // 7) % 2
                c.px(x, y, '#f4f7fa' if s == 0 else '#27323f')
    c.line([(7, 12), (57, 6)], '#ffffff'); c.line([(9, 23), (59, 17)], '#141b24')
    c.poly([(7, 24), (59, 18), (59, 24), (7, 24)], '#2d3b4f') if False else None
    c.rect(7, 23, 57, 25, '#8aa0b8'); c.hline(7, 57, 23, '#c3d3e4')
    return finish(c, '#141b24')

def code():
    c = Canvas()
    m = rrect_mask(5, 9, 58, 55, 4)
    c.vgrad(5, 9, 58, 55, ['#22313f', '#1e2b38', '#1a2530', '#16202a'], m)
    c.rrect(5, 9, 58, 18, 4, '#51687d'); c.rect(5, 14, 58, 18, '#51687d')
    c.vgrad(5, 9, 58, 18, ['#6c859b', '#5b7389', '#4d6377'], rrect_mask(5, 9, 58, 18, 4))
    c.hline(8, 55, 9, '#8ea6bb')
    c.rect(5, 18, 58, 18, '#2c3b49')
    for i, col in enumerate(['#ff6d66', '#ffc65a', '#57d28f']):
        c.ell(10 + i * 7, 11, 14 + i * 7, 15, col); c.px(10 + i * 7, 11, lighten(col, .6))
    # "</>"
    cy = '#79d7ff'; hi = '#c8f0ff'
    c.line([(25, 28), (15, 37), (25, 46)], cy, 4); c.line([(24, 29), (16, 37), (24, 45)], hi, 1)
    c.line([(39, 28), (49, 37), (39, 46)], cy, 4); c.line([(40, 29), (48, 37), (40, 45)], hi, 1)
    c.line([(35, 27), (29, 47)], '#ffc65a', 3); c.line([(35, 28), (30, 46)], '#ffe6a3', 1)
    c.hline(6, 57, 55, '#0e1620')
    return finish(c, '#0c141c')

def archive():
    c = Canvas()
    # box body
    c.rect(10, 28, 53, 57, '#b57a42')
    c.vgrad(10, 28, 53, 57, ['#d39a5c', '#c68a4d', '#b97d43', '#a96f39', '#98602f'])
    c.vline(10, 29, 56, '#e6b680'); c.vline(53, 29, 56, '#7f4e24'); c.hline(10, 53, 57, '#6d4119')
    c.rect(26, 33, 37, 36, '#6b4220'); c.rect(27, 34, 36, 35, '#3d2410')
    # lid
    c.rect(5, 14, 58, 30, '#e3b274')
    c.vgrad(5, 14, 58, 30, ['#f6d49c', '#efc78b', '#e7ba7c', '#dba96b'])
    c.hline(5, 58, 14, '#fff0cf'); c.hline(5, 58, 30, '#9a6733'); c.hline(5, 58, 29, '#c28c52')
    c.vline(5, 15, 29, '#fbe3b3'); c.vline(58, 15, 29, '#b98048')
    c.dither(6, 31, 57, 32, '#8e5c2b', 0)
    # zipper
    for y in range(12, 54):
        k = (y // 2) % 2
        c.px(30 + k, y, '#d8dde0'); c.px(33 - k, y, '#8f9aa1'); c.px(31, y, '#f5f8f9') if k == 0 else None
        c.px(32, y, '#5c666d')
    c.rrect(27, 32, 36, 46, 2, '#aeb7bd'); c.rrect(28, 33, 35, 44, 2, '#e3e8eb')
    c.rect(30, 37, 33, 42, '#6f7a82'); c.rect(31, 38, 32, 41, '#2c353b')
    c.hline(29, 34, 33, '#ffffff')
    return finish(c, '#4a2c10')

def gear_points(cx, cy, r_out, r_in, teeth, tw=0.5):
    pts = []
    for i in range(teeth):
        a = 2 * math.pi * i / teeth
        step = 2 * math.pi / teeth
        for da, r in ((-step * .5 * tw - step * .08, r_in), (-step * .5 * tw * .75, r_out),
                      (step * .5 * tw * .75, r_out), (step * .5 * tw + step * .08, r_in)):
            pts.append((cx + r * math.cos(a + da), cy + r * math.sin(a + da)))
    return pts

def settings():
    c = Canvas()
    base = gear_points(31.5, 31.5, 28, 21, 8, .9)
    c.poly(base, '#6f8392')
    hl = gear_points(30.5, 30.5, 27, 20, 8, .9)
    c.poly(hl, '#aab9c4')
    gm = poly_mask(base)
    # banded shading across the gear body
    for y in range(S):
        for x in range(S):
            if gm(x, y):
                t = (x + y) / 126
                col = '#cfd9e0' if t < .30 else '#b5c3cd' if t < .45 else '#9aabb7' if t < .62 else '#7f93a1' if t < .80 else '#68798a'
                c.px(x, y, col)
    # inner ring
    c.ell(14, 14, 49, 49, '#8497a5'); c.ell(15, 15, 48, 48, '#c3cfd8')
    c.ell(17, 17, 46, 46, '#aebcc7'); c.ell(19, 19, 44, 44, '#9aabb7')
    # hub hole
    c.ell(23, 23, 40, 40, '#4f6272'); c.ell(24, 24, 39, 39, '#2f3d4a'); c.ell(25, 25, 38, 38, '#223039')
    c.ell(26, 26, 37, 37, '#2e82c9'); c.ell(27, 27, 36, 36, '#4aa4ee'); c.ell(28, 28, 33, 33, '#8fd0ff')
    c.px(29, 29, '#e6f6ff')
    # tooth highlights
    for i in range(8):
        a = 2 * math.pi * i / 8 - math.pi / 2
        x = 31.5 + 25 * math.cos(a); y = 31.5 + 25 * math.sin(a)
        if math.cos(a) + math.sin(a) < 0.3:
            c.px(int(x), int(y), '#ecf2f6')
    return finish(c, '#23313c')

def network():
    c = Canvas()
    m = ell_mask(5, 5, 58, 58)
    c.ell(5, 5, 58, 58, '#2570c0')
    c.vgrad(5, 5, 58, 58, ['#62bdf6', '#4aa8ec', '#3a95de', '#2c80cb', '#2169b0', '#1b5896'], m)
    # continents
    land = ['#6ccf7a', '#52b866']
    c.poly([(14, 22), (20, 15), (28, 14), (31, 19), (27, 24), (30, 30), (24, 35), (19, 31), (15, 28)], land[0])
    c.poly([(26, 38), (33, 36), (37, 43), (33, 52), (28, 51), (27, 44)], land[0])
    c.poly([(37, 15), (46, 14), (52, 22), (47, 29), (42, 26), (39, 21)], land[0])
    c.poly([(42, 36), (50, 35), (53, 43), (47, 49), (43, 44)], land[0])
    for pts in ([(16, 24), (20, 18), (27, 17), (24, 24), (18, 29)],
                [(28, 40), (33, 39), (35, 46), (30, 49)],
                [(40, 17), (46, 16), (49, 23), (43, 22)]):
        c.poly(pts, land[1]) if False else None
    green = hexc(land[0])[:3]
    c.dither(12, 12, 56, 56, '#46ad5b', 1, lambda x, y: c.img.getpixel((x, y))[:3] == green and (x + 2 * y) % 5 == 0)
    # grid lines
    for r in (16, 26):
        pass
    c.line([(32, 6), (32, 57)], '#8fd1fb') if False else None
    for yy in (21, 32, 43):
        for x in range(8, 57):
            if m(x, yy) and x % 2 == 0: c.px(x, yy, '#a9dcff')
    for xx in (20, 44):
        for y in range(8, 57):
            if m(xx, y) and y % 2 == 1: c.px(xx, y, '#a9dcff')
    # glint + terminator shade
    c.dither(35, 40, 56, 56, '#17508a', 0, lambda x, y: m(x, y) and (x - 32) ** 2 + (y - 32) ** 2 > 22 ** 2)
    c.ell(10, 10, 21, 17, '#cdeeff'); c.ell(11, 11, 19, 15, '#ffffff')
    for x in range(8, 57):
        if m(x, 56) and not m(x, 57): c.px(x, 56, '#17508a')
    return finish(c, '#0e3158')

def pointer():
    c = Canvas()
    # arrow with chunky outline + drop shadow (Posy-style)
    arrow = [(17, 8), (17, 54), (28, 44), (36, 60), (44, 56), (36, 41), (51, 41)]
    c.poly(arrow, '#ffffff')
    am = poly_mask(arrow)
    for y in range(S):
        for x in range(S):
            if am(x, y):
                t = (y - 8) / 52
                c.px(x, y, '#ffffff' if t < .30 else '#eef3f7' if t < .55 else '#dbe5ec' if t < .8 else '#c6d4de')
    c.vline(17, 9, 53, '#ffffff')
    c.poly([(19, 15), (19, 40), (26, 33), (24, 30)], '#f7fafc') if False else None
    # accent click burst
    for y in range(S):
        for x in range(S):
            if am(x, y) and not am(x + 1, y + 1) and x > 20:
                c.px(x, y, '#aebfcc')
    return finish(c, '#14222c')

def computer():
    c = Canvas()
    # monitor bezel
    c.rrect(5, 6, 58, 42, 4, '#46586a')
    c.vgrad(5, 6, 58, 42, ['#6a7f93', '#566b7e', '#46586a', '#3b4b5a'], rrect_mask(5, 6, 58, 42, 4))
    c.hline(8, 55, 6, '#9ab0c4')
    # screen: MOSdesk wallpaper
    sm = rrect_mask(9, 10, 54, 37, 1)
    c.vgrad(9, 10, 54, 37, ['#2a76ad', '#256aa0', '#205c92', '#1b4f82', '#164171'], sm)
    c.poly([(33, 37), (54, 14), (54, 20), (40, 37)], '#3a8fc4')
    c.poly([(26, 37), (54, 8), (54, 11), (30, 37)], '#2f82b9') if False else None
    # mini window + taskbar
    c.rect(14, 15, 33, 28, '#f1f5f7'); c.rect(14, 15, 33, 17, '#263945')
    c.rect(16, 20, 24, 21, '#9fb2bf'); c.rect(16, 23, 29, 24, '#9fb2bf')
    c.rect(9, 33, 54, 37, '#151f29'); c.rect(11, 34, 14, 36, '#55a8dc'); c.rect(17, 34, 24, 36, '#4a5d6e')
    # glass glint
    c.line([(11, 11), (22, 11)], '#6fb6e8'); c.line([(11, 12), (17, 12)], '#4f9bd0')
    # neck + base
    c.rect(26, 42, 37, 49, '#8497a6'); c.vline(26, 42, 49, '#b6c4cf'); c.vline(37, 42, 49, '#5f7384')
    c.rrect(15, 49, 48, 56, 3, '#a9b8c4')
    c.vgrad(15, 49, 48, 56, ['#d4dee5', '#b7c5cf', '#8fa2b0'], rrect_mask(15, 49, 48, 56, 3))
    c.hline(17, 46, 49, '#f2f6f9'); c.hline(17, 46, 56, '#667b8b')
    c.px(50, 38, '#6ee7a5') if False else None
    c.rect(52, 40, 54, 40, '#6ee7a5')
    return finish(c, '#1c2833')

def appearance():
    c = Canvas()
    # palette body
    pal = [(10, 30), (13, 18), (24, 9), (40, 7), (53, 13), (59, 25), (57, 38), (47, 45), (41, 43),
           (37, 49), (39, 55), (30, 58), (17, 54), (9, 44)]
    c.poly(pal, '#e2c08a')
    pm = poly_mask(pal)
    for y in range(S):
        for x in range(S):
            if pm(x, y):
                t = (x + y) / 118
                col = '#f6dfb2' if t < .30 else '#ecd0a0' if t < .50 else '#e0bd88' if t < .70 else '#cfa56d'
                c.px(x, y, col)
    c.hline(16, 40, 9, '#fff3d6')
    # thumb hole
    c.ell(14, 36, 24, 46, '#00000000') if False else None
    for y in range(36, 47):
        for x in range(14, 25):
            if (x - 19) ** 2 / 25 + (y - 41) ** 2 / 25 <= 1:
                c.img.putpixel((x, y), (0, 0, 0, 0))
    # dabs
    for (x, y, col) in ((20, 21, '#e8453c'), (31, 16, '#ffc93c'), (43, 18, '#38b96a'),
                        (50, 28, '#2f88e0'), (31, 30, '#9b59d6')):
        c.ell(x - 5, y - 4, x + 5, y + 4, darken(col, .25))
        c.ell(x - 5, y - 5, x + 4, y + 3, col)
        c.ell(x - 3, y - 4, x, y - 2, lighten(col, .55))
    # brush
    c.poly([(36, 56), (56, 36), (60, 40), (40, 60)], '#7b4a22')
    c.poly([(36, 56), (56, 36), (58, 38), (38, 58)], '#b97a3a')
    c.poly([(44, 48), (50, 42), (53, 45), (47, 51)], '#c6d0d8')
    c.poly([(44, 48), (50, 42), (51, 43), (45, 49)], '#eef3f6')
    c.poly([(33, 59), (36, 56), (44, 48), (47, 51), (39, 59)], '#2f88e0') if False else None
    c.poly([(34, 60), (41, 53), (46, 52), (47, 51), (44, 48), (40, 49), (35, 55)], '#2f88e0')
    c.poly([(34, 60), (38, 56), (41, 53), (36, 55)], '#6fb4f4')
    return finish(c, '#4a2f0f')

def mouse():
    c = Canvas()
    body = (18, 12, 45, 58)
    m = rrect_mask(*body, 13)
    c.vgrad(*body, ['#f8fbfc', '#eaf0f3', '#d9e2e8', '#c4d0d9', '#aebcc7', '#9aaab6'], m)
    # button split
    c.vline(31, 13, 30, '#7f909c'); c.vline(32, 13, 30, '#f2f6f8')
    c.hline(19, 44, 30, '#8798a5'); c.hline(19, 44, 31, '#f4f8fa')
    # left button highlight (pressed accent)
    lm = lambda x, y: m(x, y) and x < 31 and y < 30
    c.vgrad(19, 13, 30, 29, ['#7bc0f6', '#5eaeed', '#4a9ae0', '#3b87cf'], lm)
    c.hline(21, 30, 13, '#b8e0fc')
    # wheel
    c.rrect(29, 17, 34, 27, 2, '#2d3b47'); c.rrect(30, 18, 33, 22, 1, '#5d7384'); c.px(30, 18, '#a9bccb')
    # shine
    c.vline(20, 36, 52, '#ffffff'); c.vline(21, 38, 50, '#f6f9fb')
    c.dither(36, 40, 44, 56, '#8fa0ad', 0, m)
    # cable
    c.line([(32, 12), (32, 8), (36, 5), (44, 5)], '#38495a', 2)
    c.line([(32, 12), (32, 8), (36, 5), (44, 5)], '#6b8194', 1)
    return finish(c, '#2b3a46')


def badge_plus(c, cx, cy):
    c.ell(cx - 9, cy - 9, cx + 9, cy + 9, '#2f9e57')
    c.ell(cx - 8, cy - 9, cx + 7, cy + 6, '#49c274')
    c.rect(cx - 1, cy - 5, cx + 1, cy + 5, '#ffffff')
    c.rect(cx - 5, cy - 1, cx + 5, cy + 1, '#ffffff')

def newfolder():
    c = folder()
    badge_plus(c, 46, 45)
    c.d.ellipse([36, 35, 56, 55], outline=hexc('#14522b'))
    return c

def trash():
    c = Canvas()
    # can body (tapered)
    body = [(14, 20), (49, 20), (46, 58), (17, 58)]
    bm = poly_mask(body)
    c.vgrad(14, 20, 49, 58, ['#dfe7ec', '#cdd8df', '#b9c7d0', '#a3b3be', '#8d9eab'], bm)
    for x in (23, 31, 39):
        c.rect(x, 26, x + 2, 52, '#6f8290'); c.vline(x + 3, 26, 52, '#eef3f6')
    c.line([(14, 20), (17, 58)], '#f4f8fa'); c.line([(49, 20), (46, 58)], '#73858f')
    c.hline(18, 45, 58, '#5f717c')
    # lid + handle
    c.rrect(10, 13, 53, 20, 2, '#7d909d')
    c.vgrad(10, 13, 53, 20, ['#b4c2cc', '#9aabb7', '#7f93a1'], rrect_mask(10, 13, 53, 20, 2))
    c.hline(12, 51, 13, '#e3ebf0'); c.hline(11, 52, 20, '#566873')
    c.rrect(25, 6, 38, 13, 2, '#7d909d'); c.rect(28, 9, 35, 13, '#00000000')
    for y in range(9, 14):
        for x in range(28, 36):
            c.img.putpixel((x, y), (0, 0, 0, 0))
    c.hline(26, 37, 6, '#c9d5dc')
    return finish(c, '#26333d')

def lock():
    c = Canvas()
    # shackle
    for y in range(S):
        for x in range(S):
            d2 = (x - 31.5) ** 2 + (y - 22) ** 2
            if 10.5 ** 2 <= d2 <= 15.5 ** 2 and y <= 22:
                c.px(x, y, '#c9d5dc' if x < 30 else '#93a4af')
    c.rect(16, 22, 20, 32, '#c9d5dc'); c.rect(43, 22, 47, 32, '#93a4af')
    c.vline(17, 14, 31, '#f4f8fa')
    # body
    m = rrect_mask(10, 28, 53, 58, 5)
    c.vgrad(10, 28, 53, 58, ['#ffe08a', '#fbd272', '#f5c055', '#eeae42', '#e39a33', '#d58828'], m)
    c.hline(14, 49, 28, '#fff4c8'); c.vline(10, 33, 53, '#ffeaa8')
    c.hline(14, 49, 58, '#a6641a'); c.vline(53, 33, 53, '#b97722')
    c.dither(11, 30, 52, 31, '#ffe9a0', 0, m)
    # keyhole
    c.ell(27, 36, 36, 45, '#5a3a12'); c.poly([(29, 42), (34, 42), (36, 52), (27, 52)], '#5a3a12')
    c.ell(29, 37, 32, 40, '#8a6126')
    return finish(c, '#4a2f0f')

def desktop():
    c = Canvas()
    c.rrect(4, 9, 59, 51, 4, '#46586a')
    c.vgrad(4, 9, 59, 51, ['#6a7f93', '#566b7e', '#46586a', '#3b4b5a'], rrect_mask(4, 9, 59, 51, 4))
    c.hline(7, 56, 9, '#9ab0c4')
    sm = rrect_mask(8, 13, 55, 47, 1)
    c.vgrad(8, 13, 55, 47, ['#2a76ad', '#256aa0', '#205c92', '#1b4f82', '#164171'], sm)
    c.poly([(30, 47), (55, 20), (55, 28), (38, 47)], '#3a8fc4')
    # icon grid
    cols = ['#ffd45e', '#6ccf7a', '#ff8a73', '#c9a2f2', '#7fd6ff', '#f1f5f7']
    k = 0
    for gx in range(2):
        for gy in range(3):
            x = 12 + gx * 11; y = 16 + gy * 9
            c.rect(x, y, x + 6, y + 5, cols[k]); c.hline(x, x + 6, y, lighten(cols[k], .5)); k += 1
    c.rect(8, 43, 55, 47, '#151f29'); c.rect(10, 44, 13, 46, '#55a8dc'); c.rect(16, 44, 25, 46, '#4a5d6e')
    c.rect(23, 52, 40, 54, '#8497a6'); c.rrect(17, 54, 46, 58, 2, '#a9b8c4'); c.hline(19, 44, 54, '#f2f6f9')
    return finish(c, '#1c2833')

def app():
    c = Canvas()
    m = rrect_mask(6, 6, 57, 57, 10)
    c.vgrad(6, 6, 57, 57, ['#8f7bf2', '#7c68e6', '#6a56d6', '#5a47c2', '#4b3aab', '#3f3095'], m)
    c.hline(13, 50, 6, '#c9bfff'); c.vline(6, 13, 50, '#a99bf7')
    c.dither(7, 7, 56, 11, '#a08ef6', 0, m)
    c.hline(13, 50, 57, '#2c2170'); c.vline(57, 13, 50, '#33287e')
    # window glyph
    c.rrect(15, 17, 48, 46, 3, '#ffffff')
    c.rect(15, 17, 48, 24, '#dcd6ff'); c.rrect(15, 17, 48, 22, 3, '#dcd6ff')
    for i, col in enumerate(['#ff6d66', '#ffc65a', '#57d28f']):
        c.rect(18 + i * 5, 19, 20 + i * 5, 21, col)
    c.rect(19, 28, 30, 30, '#8f7bf2'); c.rect(19, 33, 44, 34, '#b9c1cc'); c.rect(19, 37, 39, 38, '#b9c1cc')
    c.rect(19, 41, 42, 42, '#b9c1cc')
    c.hline(16, 47, 46, '#c8cdd6')
    return finish(c, '#20174f')

def edit():
    c = Canvas()
    # pencil pointing bottom-left
    c.poly([(40, 8), (56, 24), (24, 56), (8, 40)], '#f2b04b')
    c.poly([(40, 8), (45, 13), (13, 45), (8, 40)], '#ffd98a')
    c.poly([(51, 19), (56, 24), (24, 56), (19, 51)], '#d58a2e')
    c.poly([(44, 4), (60, 20), (56, 24), (40, 8)], '#e8667a')
    c.poly([(44, 4), (48, 8), (44, 12), (40, 8)], '#ff97a8')
    c.poly([(38, 10), (54, 26), (51, 29), (35, 13)], '#c6d0d8')
    c.poly([(38, 10), (41, 13), (38, 16), (35, 13)], '#eef3f6')
    c.poly([(8, 40), (24, 56), (5, 59)], '#f3dcb3')
    c.poly([(5, 59), (12, 57), (7, 52)], '#34444f')
    return finish(c, '#4a2f0f')

def copy():
    c = Canvas()
    def sheet(x, y, top):
        m = rrect_mask(x, y, x + 33, y + 41, 3)
        c.rrect(x, y, x + 33, y + 41, 3, '#f1f5f7')
        c.vgrad(x, y, x + 33, y + 41, ['#ffffff', '#f6f9fa', '#eaf0f3', '#dde6eb'], m)
        c.hline(x + 3, x + 30, y + 41, '#b8c6cf'); c.vline(x + 33, y + 3, y + 38, '#c6d2d9')
        if top:
            c.rect(x + 6, y + 8, x + 20, y + 10, '#4f8fc7')
            for i, w in enumerate([22, 18, 22, 14]):
                c.rect(x + 6, y + 16 + i * 6, x + 6 + w, y + 17 + i * 6, '#8fa3b0')
    sheet(8, 6, False)
    for x in range(21, 56):
        c.px(x, 15, '#34444f')
    for y in range(15, 58):
        c.px(20, y, '#34444f')
    sheet(21, 16, True)
    return finish(c, '#34444f')

def paste():
    c = Canvas()
    m = rrect_mask(9, 10, 54, 58, 4)
    c.vgrad(9, 10, 54, 58, ['#c99255', '#bb8448', '#ab753c', '#9a6631', '#895827'], m)
    c.vline(9, 14, 54, '#e0b27a'); c.hline(13, 50, 58, '#5f3a16')
    c.rrect(15, 17, 48, 53, 2, '#f6f9fa')
    c.vgrad(15, 17, 48, 53, ['#ffffff', '#f4f7f9', '#e6edf1'], rrect_mask(15, 17, 48, 53, 2))
    for i, w in enumerate([24, 20, 26, 16, 22]):
        c.rect(19, 26 + i * 5, 19 + w, 27 + i * 5, '#8fa3b0')
    # clip
    c.rrect(22, 5, 41, 17, 3, '#8497a5')
    c.vgrad(22, 5, 41, 17, ['#d4dee5', '#b7c5cf', '#93a5b2'], rrect_mask(22, 5, 41, 17, 3))
    c.rect(27, 8, 36, 11, '#4f6272'); c.hline(24, 39, 5, '#f2f6f9'); c.hline(23, 40, 17, '#5f7384')
    return finish(c, '#3d2410')

def refresh():
    c = Canvas()
    m = ell_mask(5, 5, 58, 58)
    c.vgrad(5, 5, 58, 58, ['#62bdf6', '#4aa8ec', '#3a95de', '#2c80cb', '#2169b0', '#1b5896'], m)
    c.ell(10, 9, 22, 16, '#bfe6ff')
    # ring arrow
    for y in range(S):
        for x in range(S):
            dx, dy = x - 31.5, y - 31.5
            d2 = dx * dx + dy * dy
            if 11.5 ** 2 <= d2 <= 17.5 ** 2:
                a = math.degrees(math.atan2(dy, dx)) % 360
                if not (285 <= a <= 345) and not (105 <= a <= 165):
                    c.px(x, y, '#ffffff')
    c.poly([(40, 8), (52, 19), (37, 23)], '#ffffff')
    c.poly([(23, 55), (11, 44), (26, 40)], '#ffffff')
    return finish(c, '#0e3158')

def up():
    c = Canvas()
    m = rrect_mask(6, 6, 57, 57, 10)
    c.vgrad(6, 6, 57, 57, ['#7fdc9a', '#63cc84', '#4cb96f', '#3aa55c', '#2c8f4b', '#237a3e'], m)
    c.hline(13, 50, 6, '#c4f3d2'); c.vline(6, 13, 50, '#9ae6b1')
    c.hline(13, 50, 57, '#17592b'); c.vline(57, 13, 50, '#1b6432')
    c.poly([(31, 13), (32, 13), (49, 31), (39, 31), (39, 49), (24, 49), (24, 31), (14, 31)], '#ffffff')
    c.poly([(39, 31), (39, 49), (36, 49), (36, 31)], '#d5efdd')
    c.hline(24, 39, 49, '#bfe3ca')
    return finish(c, '#12451f')

def info():
    c = Canvas()
    m = ell_mask(5, 5, 58, 58)
    c.vgrad(5, 5, 58, 58, ['#62bdf6', '#4aa8ec', '#3a95de', '#2c80cb', '#2169b0', '#1b5896'], m)
    c.ell(10, 9, 23, 17, '#bfe6ff'); c.ell(11, 10, 20, 15, '#ffffff')
    c.ell(27, 14, 36, 23, '#ffffff')
    c.rect(26, 27, 35, 46, '#ffffff'); c.rect(23, 27, 35, 31, '#ffffff'); c.rect(22, 45, 40, 49, '#ffffff')
    c.vline(35, 28, 44, '#d3e9fb')
    return finish(c, '#0e3158')

def textfile():
    c = Canvas(); paper_base(c)
    c.rect(19, 24, 44, 28, '#4f6272'); c.rect(29, 28, 34, 50, '#4f6272')
    c.rect(19, 28, 21, 31, '#4f6272'); c.rect(42, 28, 44, 31, '#4f6272'); c.rect(25, 50, 38, 52, '#4f6272')
    c.hline(19, 44, 24, '#7d909c')
    return finish(c, '#34444f')

def terminal():
    c = Canvas()
    m = rrect_mask(5, 9, 58, 55, 4)
    c.vgrad(5, 9, 58, 55, ['#22313f', '#1e2b38', '#1a2530', '#16202a'], m)
    c.vgrad(5, 9, 58, 18, ['#6c859b', '#5b7389', '#4d6377'], rrect_mask(5, 9, 58, 18, 4))
    c.rect(5, 15, 58, 18, '#4d6377'); c.hline(8, 55, 9, '#8ea6bb')
    for i, col in enumerate(['#ff6d66', '#ffc65a', '#57d28f']):
        c.ell(10 + i * 7, 11, 14 + i * 7, 15, col)
    c.line([(13, 26), (24, 34), (13, 42)], '#57d28f', 4); c.line([(14, 27), (23, 34)], '#b4f2cf', 1)
    c.rect(28, 41, 46, 44, '#e6edf1')
    c.hline(6, 57, 55, '#0e1620')
    return finish(c, '#0c141c')

ICONS = [  # order == C enum order
    ('FOLDER', lambda: folder()), ('DRIVE', drive), ('DOCUMENT', document), ('PDF', pdf),
    ('IMAGE', image), ('AUDIO', audio), ('VIDEO', video), ('CODE', code), ('ARCHIVE', archive),
    ('SETTINGS', settings), ('NETWORK', network), ('POINTER', pointer),
    ('HOME', lambda: folder('home')), ('COMPUTER', computer), ('APPEARANCE', appearance), ('MOUSE', mouse),
    ('TRASH', trash), ('LOCK', lock), ('DESKTOP', desktop), ('APP', app), ('EDIT', edit),
    ('COPY', copy), ('PASTE', paste), ('REFRESH', refresh), ('UP', up), ('NEWFOLDER', newfolder),
    ('INFO', info), ('TEXT', textfile), ('TERMINAL', terminal),
]
