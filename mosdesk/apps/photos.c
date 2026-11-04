/*
 * Photos - shows .img pictures (Minimal-OS's image format, see mosmedia.h).
 *
 *     run 0:/programs/mos_photos.run picture.img
 *
 * mosWM opens .img files with it. The picture fits the window (never
 * enlarged past twice its size); transparent parts show a checkerboard.
 * Left / Right (or the arrows at the sides) go to the previous / next
 * picture in the same folder.
 */
#include "easyui.h"
#include "mosmedia.h"
#include "stdlib.h"
#include "string.h"

#ifndef SYS_LISTDIR
#define SYS_LISTDIR 9
#endif

#define BAR_H 40
#define MAX_FILES 256
#define MAX_FILE_SIZE (64u * 1024u * 1024u)
#define PATH_SIZE 256
#define C_BG    0xFF1B1E22u
#define C_BAR   0xFF14171Au
#define C_TEXT  0xFFF2F5F8u
#define C_DIM   0xFF97A3AFu

static char g_folder[PATH_SIZE];
static char g_files[MAX_FILES][SYSCALL_DIRENT_NAME_SIZE];
static int g_count, g_current = -1;
static uint32_t* g_pixels;
static mimg_info_t g_info;
static char g_note[96];
static int canvas;
static int g_hover_side;            /* -1 left arrow, 1 right, 0 none */

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static int is_img(const char* name) {
    size_t n = strlen(name);
    return n > 4 && name[n - 4] == '.' && lower(name[n - 3]) == 'i' && lower(name[n - 2]) == 'm' &&
           lower(name[n - 1]) == 'g';
}

static int name_before(const char* a, const char* b) {
    for (;; a++, b++) {
        char x = lower(*a), y = lower(*b);
        if (x != y || !x) return x < y;
    }
}

static void join(char* out, const char* folder, const char* name) {
    size_t n = 0;
    for (; folder[n] && n + 1 < PATH_SIZE; n++) out[n] = folder[n];
    if (n && out[n - 1] != '/' && n + 1 < PATH_SIZE) out[n++] = '/';
    for (; *name && n + 1 < PATH_SIZE; name++) out[n++] = *name;
    out[n] = '\0';
}

static void load(int index) {
    free(g_pixels);
    g_pixels = NULL;
    g_note[0] = '\0';
    g_current = index;
    if (index < 0 || index >= g_count) return;
    char path[PATH_SIZE];
    join(path, g_folder, g_files[index]);
    easyui.settitle(g_files[index]);
    long fd = mos_open(path, SYS_O_RDONLY);
    long size = fd >= 0 ? mos_size(fd) : -1;
    if (fd < 0 || size <= 0 || (unsigned long)size > MAX_FILE_SIZE) {
        if (fd >= 0) mos_close(fd);
        strcpy(g_note, size > 0 ? "This picture is too large" : "The picture could not be opened");
        return;
    }
    uint8_t* data = (uint8_t*)malloc((size_t)size);
    long got = 0;
    while (data && got < size) {
        long n = mos_read(fd, data + got, (size_t)(size - got));
        if (n <= 0) break;
        got += n;
    }
    mos_close(fd);
    if (!data || got != size || !mimg_info(data, (size_t)size, &g_info)) {
        free(data);
        strcpy(g_note, "This is not an .img picture, or it is damaged");
        return;
    }
    g_pixels = (uint32_t*)malloc((size_t)g_info.width * g_info.height * 4u);
    if (!g_pixels) strcpy(g_note, "Not enough memory for this picture");
    else if (!mimg_decode(data, (size_t)size, g_pixels, g_info.width)) {
        free(g_pixels);
        g_pixels = NULL;
        strcpy(g_note, "This picture is damaged");
    }
    free(data);
}

/* Lists the folder's pictures and shows `path`. */
static void open_path(const char* path) {
    const char* slash = strrchr(path, '/');
    size_t n = slash ? (size_t)(slash - path) : 0;
    if (n >= PATH_SIZE) n = PATH_SIZE - 1;
    memcpy(g_folder, path, n);
    g_folder[n] = '\0';
    if (!n) strcpy(g_folder, "0:");
    const char* name = slash ? slash + 1 : path;
    static syscall_dirent_t entries[MAX_FILES];
    long count = mos_syscall(SYS_LISTDIR, (long)(uintptr_t)g_folder, (long)(uintptr_t)entries, MAX_FILES);
    g_count = 0;
    for (long i = 0; i < count && i < MAX_FILES; i++) {
        if (entries[i].type == SYSCALL_DIRENT_TYPE_DIR || !is_img(entries[i].name)) continue;
        /* Sorted by name. */
        int at = g_count++;
        while (at > 0 && name_before(entries[i].name, g_files[at - 1])) {
            strcpy(g_files[at], g_files[at - 1]);
            at--;
        }
        strcpy(g_files[at], entries[i].name);
    }
    int index = -1;
    for (int i = 0; i < g_count; i++)
        if (!strcmp(g_files[i], name)) index = i;
    if (index < 0 && g_count < MAX_FILES) {     /* not listed (yet): show it anyway */
        strcpy(g_files[g_count], name);
        index = g_count++;
    }
    load(index);
}

static void draw(int unused) {
    (void)unused;
    int W = easyui.width(), H = easyui.height() - BAR_H;
    easyui.fillrect(0, 0, W, H, C_BG);
    if (g_pixels) {
        /* Fit: at most twice the real size. */
        uint32_t iw = g_info.width, ih = g_info.height;
        uint64_t sw = (uint64_t)W * 1024u / iw, sh = (uint64_t)H * 1024u / ih;
        uint64_t scale = sw < sh ? sw : sh;
        if (scale > 2048u) scale = 2048u;
        if (!scale) scale = 1;
        int dw = (int)(iw * scale / 1024u), dh = (int)(ih * scale / 1024u);
        if (dw < 1) dw = 1;
        if (dh < 1) dh = 1;
        int x0 = (W - dw) / 2, y0 = (H - dh) / 2;
        uint32_t* px = easyui.pixels();
        for (int y = 0; y < dh; y++) {
            int dy = y0 + y;
            if (dy < 0 || dy >= H) continue;
            const uint32_t* src = g_pixels + (size_t)((uint64_t)y * ih / (uint32_t)dh) * iw;
            uint32_t* dst = px + (size_t)dy * (size_t)W;
            for (int x = 0; x < dw; x++) {
                int dx = x0 + x;
                if (dx < 0 || dx >= W) continue;
                uint32_t c = src[(uint64_t)x * iw / (uint32_t)dw];
                uint32_t a = c >> 24;
                if (a != 255u) {
                    /* Over a checkerboard. */
                    uint32_t bg = ((x / 10 + y / 10) & 1) ? 0xFFCCCCCCu : 0xFFFFFFFFu;
                    uint32_t r = (((c >> 16) & 255u) * a + ((bg >> 16) & 255u) * (255u - a)) / 255u;
                    uint32_t g = (((c >> 8) & 255u) * a + ((bg >> 8) & 255u) * (255u - a)) / 255u;
                    uint32_t b = ((c & 255u) * a + (bg & 255u) * (255u - a)) / 255u;
                    c = 0xFF000000u | r << 16 | g << 8 | b;
                }
                dst[dx] = c;
            }
        }
    }
    if (g_note[0]) {
        int tw = easyui.textwidth(g_note, 2);
        easyui.drawtext((W - tw) / 2, H / 2 - 8, g_note, C_TEXT, 2);
    }
    /* Side arrows when there is more than one picture. */
    if (g_count > 1) {
        for (int side = -1; side <= 1; side += 2) {
            int cx = side < 0 ? 30 : W - 30, cy = H / 2;
            easyui.fillcircle(cx, cy, 18, g_hover_side == side ? 0xC0FFFFFFu : 0x70000000u);
            uint32_t glyph = g_hover_side == side ? 0xFF1B1E22u : 0xFFFFFFFFu;
            for (int i = 0; i < 7; i++) {
                int x = side < 0 ? cx + 3 - i : cx - 3 + i;
                easyui.fillrect(x, cy - (6 - i), 2, 1, glyph);
                easyui.fillrect(x, cy + (6 - i), 2, 1, glyph);
            }
        }
    }
    easyui.fillrect(0, H, W, BAR_H, C_BAR);
    if (g_current >= 0) {
        easyui.drawtext(14, H + 12, g_files[g_current], C_TEXT, 2);
        if (g_pixels) {
            char t[64];
            char* p = t;
            uint32_t vals[2] = {g_info.width, g_info.height};
            for (int k = 0; k < 2; k++) {
                char d[12];
                int n = 0;
                uint32_t v = vals[k];
                do { d[n++] = (char)('0' + v % 10u); v /= 10u; } while (v);
                while (n) *p++ = d[--n];
                if (!k) { *p++ = ' '; *p++ = 'x'; *p++ = ' '; }
            }
            const char* extra = g_info.alpha ? "  transparent" : "";
            while (*extra) *p++ = *extra++;
            *p = '\0';
            int tw = easyui.textwidth(t, 1);
            easyui.drawtext(W - tw - 14, H + 15, t, C_DIM, 1);
        }
    }
}

static void step(int dir) {
    if (g_count < 2) return;
    load((g_current + dir + g_count) % g_count);
    easyui.redraw();
}

static int side_at(int x, int y) {
    if (g_count < 2 || x < 0 || y < 0 || y >= easyui.height() - BAR_H) return 0;
    if (x < 64) return -1;
    if (x >= easyui.width() - 64) return 1;
    return 0;
}

static void on_press(int unused) {
    (void)unused;
    int x, y, b;
    easyui.mouse(&x, &y, &b);
    int side = side_at(x, y);
    if (side) step(side);
}

static void on_key(int value) {
    int key = value & 0xFF;
    if (key == EASYUI_KEY_LEFT || key == EASYUI_KEY_UP) step(-1);
    else if (key == EASYUI_KEY_RIGHT || key == EASYUI_KEY_DOWN || key == EASYUI_KEY_SPACE) step(1);
}

static void on_frame(int unused) {
    (void)unused;
    int x, y, b;
    easyui.mouse(&x, &y, &b);
    int side = side_at(x, y);
    if (side != g_hover_side) { g_hover_side = side; easyui.redraw(); }
}

static void on_resize(int unused) {
    (void)unused;
    easyui.setsize(canvas, easyui.width(), easyui.height());
}

int main(int argc, char** argv) {
    if (argc > 1 && argv[1] && argv[1][0]) open_path(argv[1]);
    else strcpy(g_note, "Open an .img picture from Files");
    int w = 800, h = 560;
    if (g_pixels) {
        /* About the picture's size, within reason. */
        uint32_t iw = g_info.width, ih = g_info.height;
        if (iw > 960u) { ih = ih * 960u / iw; iw = 960u; }
        if (ih > 620u) { iw = iw * 620u / ih; ih = 620u; }
        w = iw < 420u ? 420 : (int)iw;
        h = (ih < 240u ? 240 : (int)ih) + BAR_H;
    }
    if (!easyui.init(g_current >= 0 ? g_files[g_current] : "Photos", w, h)) return 1;
    canvas = easyui.createcanvas(0, 0, w, h);
    easyui.setanchor(canvas, EASYUI_ANCHOR_LEFT | EASYUI_ANCHOR_RIGHT |
                             EASYUI_ANCHOR_TOP | EASYUI_ANCHOR_BOTTOM);
    easyui.recallfunc(canvas, ONDRAW, draw);
    easyui.recallfunc(canvas, ONPRESS, on_press);
    easyui.recallfunc(EASYUI_WINDOW, ONKEY, on_key);
    easyui.recallfunc(EASYUI_WINDOW, ONFRAME, on_frame);
    easyui.recallfunc(EASYUI_WINDOW, ONRESIZE, on_resize);
    return easyui.run();
}
