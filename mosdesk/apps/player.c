/*
 * Player - plays .adi audio files.
 *
 *     run 0:/programs/mos_player.run [file.adi | folder]
 *
 * Bundled with MOSdesk: mosWM opens .adi files with it when you
 * double-click them in Files or on the desktop.
 *
 * One canvas, drawn by hand: a gradient header with a glowing disc, the
 * track title, a seek line, round transport buttons (repeat and shuffle
 * on the sides) and the other .adi files of the folder as rows. Click a
 * row to play it; when a track ends the next one starts. The menu button
 * (top right) opens another folder. Volume is the panel's speaker menu.
 *
 * Playback itself runs in the kernel (SYS_AUDIO, see audio.h), on the
 * Intel HD Audio or AC97 sound card, whichever the machine has.
 */
#include "easyui.h"
#include "audio.h"
#include "stdlib.h"
#include "string.h"
#if defined(__has_include)
#if __has_include("../settings.h")
#include "../settings.h"         /* MOSdesk: the user's home folder */
#define PLAYER_HAS_MOSDESK 1
#endif
#endif

#ifndef SYS_IS_DIR
#define SYS_IS_DIR 10
#endif

#define MAX_TRACKS 128
#define PATH_SIZE  SYSCALL_AUDIO_PATH_MAX
#define W0 400
#define H0 600
#define ROW_H 46

/* Colors (the reference: navy body, purple-magenta header, pink accent). */
#define C_BODY      0xFF1F2A44u
#define C_HEAD_TOP  0xFF22304Fu
#define C_HEAD_LOW  0xFF8A2D8Cu
#define C_PILL      0xFF34405Cu
#define C_TEXT      0xFFEEF1F7u
#define C_DIM       0xFF9AA6BFu
#define C_LINE      0xFF4A5675u
#define C_PINK      0xFFEC4C8Au
#define C_ROW_L     0xFF2B3653u
#define C_ROW_R     0xFF3C4968u

static char g_folder[PATH_SIZE];
static char g_tracks[MAX_TRACKS][SYSCALL_DIRENT_NAME_SIZE];
static int  g_track_count;
static int  g_current = -1;          /* row of the file we started */
static char g_playing[PATH_SIZE];    /* full path of the file we started */
static uint32_t g_last_state;
static syscall_audio_status_t g_st;  /* last status */
static char g_note[96];              /* a problem to show, "" if none */
static int g_repeat, g_shuffle;
static int g_list_scroll;            /* first visible row */
static int g_seeking;                /* dragging the seek line */
static uint32_t g_seek_ms;
static uint32_t g_rand = 12345u;

static int canvas;

/* Where things were drawn, for clicks. */
typedef struct { int x, y, w, h; } box_t;
enum { HIT_NONE, HIT_MENU, HIT_SEEK, HIT_REPEAT, HIT_PREV, HIT_PLAY, HIT_NEXT,
       HIT_SHUFFLE, HIT_ROW };
static box_t g_hit_menu, g_hit_seek, g_hit_repeat, g_hit_prev, g_hit_play, g_hit_next,
             g_hit_shuffle, g_hit_list;
static int g_hover = HIT_NONE, g_hover_row = -1;

/* ---- small helpers ---- */

static void append(char* dst, size_t size, const char* src) {
    size_t n = strlen(dst);
    while (*src && n + 1 < size) dst[n++] = *src++;
    dst[n] = '\0';
}

static void append_number(char* dst, size_t size, uint32_t v) {
    char t[12];
    int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    char out[12];
    int i = 0;
    while (n) out[i++] = t[--n];
    out[i] = '\0';
    append(dst, size, out);
}

static void format_time(char* out, size_t size, uint32_t ms) {
    uint32_t s = ms / 1000;
    out[0] = '\0';
    append_number(out, size, s / 60);
    append(out, size, ":");
    if (s % 60 < 10) append(out, size, "0");
    append_number(out, size, s % 60);
}

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static int is_adi(const char* name) {
    size_t n = strlen(name);
    if (n < 5) return 0;
    const char* e = name + n - 4;
    return e[0] == '.' && lower(e[1]) == 'a' && lower(e[2]) == 'd' && lower(e[3]) == 'i';
}

static int name_before(const char* a, const char* b) {
    for (;; a++, b++) {
        char x = lower(*a), y = lower(*b);
        if (x != y || !x) return x < y;
    }
}

static const char* base_name(const char* path) {
    const char* slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

/* "Song Name.adi" -> "Song Name" */
static void track_title(char* out, size_t size, const char* name) {
    strlcpy_safe(out, base_name(name), size);
    size_t n = strlen(out);
    if (n > 4 && is_adi(out)) out[n - 4] = '\0';
}

static int is_dir(const char* path) {
    return mos_syscall(SYS_IS_DIR, (long)(uintptr_t)path, 0, 0) == 1;
}

static void join(char* out, size_t size, const char* folder, const char* name) {
    out[0] = '\0';
    append(out, size, folder);
    size_t n = strlen(out);
    if (n && out[n - 1] != '/') append(out, size, "/");
    append(out, size, name);
}

static int inside(const box_t* b, int x, int y) {
    return x >= b->x && y >= b->y && x < b->x + b->w && y < b->y + b->h;
}

/* ---- anti-aliased drawing straight into the window ---- */

static uint32_t mix(uint32_t a, uint32_t b, uint32_t t) {   /* t 0..255 toward b */
    uint32_t r = (((a >> 16) & 255u) * (255u - t) + ((b >> 16) & 255u) * t) / 255u;
    uint32_t g = (((a >> 8) & 255u) * (255u - t) + ((b >> 8) & 255u) * t) / 255u;
    uint32_t bl = ((a & 255u) * (255u - t) + (b & 255u) * t) / 255u;
    return 0xFF000000u | (r << 16) | (g << 8) | bl;
}

static void blend_px(int x, int y, uint32_t color, uint32_t cover) {
    int w = easyui.width(), h = easyui.height();
    if (x < 0 || y < 0 || x >= w || y >= h || !cover) return;
    uint32_t* p = easyui.pixels() + (size_t)y * (size_t)w + (size_t)x;
    uint32_t a = ((color >> 24) * cover) / 255u;
    *p = mix(*p, color, a);
}

/* Fills the pixels where inside(px, py) holds, 4x4 samples each. */
typedef int (*shape_fn)(float x, float y, const float* k);

static void fill_shape(int x0, int y0, int x1, int y1, uint32_t color, shape_fn in,
                       const float* k) {
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            uint32_t hits = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++)
                    hits += (uint32_t)in((float)x + (sx + 0.5f) * 0.25f,
                                         (float)y + (sy + 0.5f) * 0.25f, k);
            if (hits) blend_px(x, y, color, hits * 255u / 16u);
        }
}

/* k: ax ay bx by cx cy */
static int in_triangle(float x, float y, const float* k) {
    float d1 = (x - k[2]) * (k[1] - k[3]) - (k[0] - k[2]) * (y - k[3]);
    float d2 = (x - k[4]) * (k[3] - k[5]) - (k[2] - k[4]) * (y - k[5]);
    float d3 = (x - k[0]) * (k[5] - k[1]) - (k[4] - k[0]) * (y - k[1]);
    int neg = d1 < 0 || d2 < 0 || d3 < 0, pos = d1 > 0 || d2 > 0 || d3 > 0;
    return !(neg && pos);
}

static void triangle(float ax, float ay, float bx, float by, float cx, float cy,
                     uint32_t color) {
    float k[6] = { ax, ay, bx, by, cx, cy };
    float minx = ax < bx ? (ax < cx ? ax : cx) : (bx < cx ? bx : cx);
    float maxx = ax > bx ? (ax > cx ? ax : cx) : (bx > cx ? bx : cx);
    float miny = ay < by ? (ay < cy ? ay : cy) : (by < cy ? by : cy);
    float maxy = ay > by ? (ay > cy ? ay : cy) : (by > cy ? by : cy);
    fill_shape((int)minx - 1, (int)miny - 1, (int)maxx + 2, (int)maxy + 2, color,
               in_triangle, k);
}

/* k: cx cy r_inner^2 r_outer^2 */
static int in_ring(float x, float y, const float* k) {
    float dx = x - k[0], dy = y - k[1], d = dx * dx + dy * dy;
    return d >= k[2] && d <= k[3];
}

static void ring(float cx, float cy, float r, float thick, uint32_t color) {
    float ri = r - thick * 0.5f, ro = r + thick * 0.5f;
    float k[4] = { cx, cy, ri > 0 ? ri * ri : 0, ro * ro };
    fill_shape((int)(cx - ro) - 1, (int)(cy - ro) - 1, (int)(cx + ro) + 2, (int)(cy + ro) + 2,
               color, in_ring, k);
}

static void disc(float cx, float cy, float r, uint32_t color) {
    easyui.fillcircle((int)(cx + 0.5f), (int)(cy + 0.5f), (int)(r + 0.5f), color);
}

/* k: x0 y0 x1 y1 half-thickness^2 */
static int in_segment(float x, float y, const float* k) {
    float dx = k[2] - k[0], dy = k[3] - k[1];
    float t = ((x - k[0]) * dx + (y - k[1]) * dy) / (dx * dx + dy * dy + 1e-6f);
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    float ex = k[0] + t * dx - x, ey = k[1] + t * dy - y;
    return ex * ex + ey * ey <= k[4];
}

static void line(float x0, float y0, float x1, float y1, float thick, uint32_t color) {
    float h = thick * 0.5f, k[5] = { x0, y0, x1, y1, h * h };
    int ax = (int)((x0 < x1 ? x0 : x1) - h) - 1, bx = (int)((x0 > x1 ? x0 : x1) + h) + 2;
    int ay = (int)((y0 < y1 ? y0 : y1) - h) - 1, by = (int)((y0 > y1 ? y0 : y1) + h) + 2;
    fill_shape(ax, ay, bx, by, color, in_segment, k);
}

static void text_center(int cx, int y, const char* t, uint32_t color, int px) {
    int w = easyui.textwidthfont(t, EASYUI_FONT_MEDIUM, px);
    easyui.drawtextfont(cx - w / 2, y, t, color, EASYUI_FONT_MEDIUM, px);
}

/* Shortens `t` with "..." to fit `max` pixels. */
static void fit(char* out, size_t size, const char* t, int max, int px) {
    strlcpy_safe(out, t, size);
    if (easyui.textwidthfont(out, EASYUI_FONT_MEDIUM, px) <= max) return;
    size_t n = strlen(out);
    while (n > 1) {
        out[--n] = '\0';
        char tmp[160];
        strlcpy_safe(tmp, out, sizeof tmp);
        append(tmp, sizeof tmp, "...");
        if (easyui.textwidthfont(tmp, EASYUI_FONT_MEDIUM, px) <= max) {
            strlcpy_safe(out, tmp, size);
            return;
        }
    }
}

/* ---- icons ---- */

static void icon_prev(float cx, float cy, uint32_t c) {
    line(cx - 6, cy - 6, cx - 6, cy + 6, 2.2f, c);
    triangle(cx + 6, cy - 6, cx + 6, cy + 6, cx - 4, cy, c);
}

static void icon_next(float cx, float cy, uint32_t c) {
    line(cx + 6, cy - 6, cx + 6, cy + 6, 2.2f, c);
    triangle(cx - 6, cy - 6, cx - 6, cy + 6, cx + 4, cy, c);
}

static void icon_play(float cx, float cy, float s, uint32_t c) {
    triangle(cx - s * 0.45f, cy - s * 0.6f, cx - s * 0.45f, cy + s * 0.6f, cx + s * 0.65f, cy, c);
}

static void icon_pause(float cx, float cy, float s, uint32_t c) {
    easyui.fillroundrect((int)(cx - s * 0.5f), (int)(cy - s * 0.6f), (int)(s * 0.32f),
                         (int)(s * 1.2f), 2, c);
    easyui.fillroundrect((int)(cx + s * 0.18f), (int)(cy - s * 0.6f), (int)(s * 0.32f),
                         (int)(s * 1.2f), 2, c);
}

static void icon_repeat(float cx, float cy, uint32_t c) {
    line(cx - 8, cy - 4, cx + 6, cy - 4, 1.8f, c);
    line(cx - 8, cy - 4, cx - 8, cy + 1, 1.8f, c);
    triangle(cx + 5, cy - 8, cx + 5, cy, cx + 10, cy - 4, c);
    line(cx + 8, cy + 4, cx - 6, cy + 4, 1.8f, c);
    line(cx + 8, cy + 4, cx + 8, cy - 1, 1.8f, c);
    triangle(cx - 5, cy + 8, cx - 5, cy, cx - 10, cy + 4, c);
}

static void icon_shuffle(float cx, float cy, uint32_t c) {
    line(cx - 9, cy - 5, cx + 5, cy + 5, 1.8f, c);
    line(cx - 9, cy + 5, cx + 5, cy - 5, 1.8f, c);
    triangle(cx + 4, cy - 9, cx + 4, cy - 1, cx + 10, cy - 5, c);
    triangle(cx + 4, cy + 1, cx + 4, cy + 9, cx + 10, cy + 5, c);
}

static void icon_menu(float cx, float cy, uint32_t c) {
    for (int i = -1; i <= 1; i++) line(cx - 9, cy + i * 6, cx + 9, cy + i * 6, 1.8f, c);
}

/* A compact disc in a sleeve, as in the reference's album view. */
static void icon_album(float cx, float cy, float s, uint32_t c) {
    float h = s * 0.5f;
    line(cx - h, cy - h, cx + h, cy - h, 2.0f, c);
    line(cx + h, cy - h, cx + h, cy + h, 2.0f, c);
    line(cx + h, cy + h, cx - h, cy + h, 2.0f, c);
    line(cx - h, cy + h, cx - h, cy - h, 2.0f, c);
    line(cx - h + 6, cy - h, cx - h + 6, cy + h, 1.5f, c);
    ring(cx + 3, cy, s * 0.36f, 1.8f, c);
    ring(cx + 3, cy, s * 0.2f, 1.2f, c);
    disc(cx + 3, cy, 2.5f, c);
}

/* ---- folder and tracks ---- */

static void scan_folder(const char* folder) {
    static syscall_dirent_t entries[256];
    strlcpy_safe(g_folder, folder, sizeof g_folder);
    g_track_count = 0;
    long n = mos_listdir(folder, entries, 256);
    for (long i = 0; i < n && g_track_count < MAX_TRACKS; i++) {
        if (entries[i].type == SYSCALL_DIRENT_TYPE_DIR || !is_adi(entries[i].name)) continue;
        int at = g_track_count;                     /* insertion sort by name */
        while (at > 0 && name_before(entries[i].name, g_tracks[at - 1])) {
            memcpy(g_tracks[at], g_tracks[at - 1], SYSCALL_DIRENT_NAME_SIZE);
            at--;
        }
        strlcpy_safe(g_tracks[at], entries[i].name, SYSCALL_DIRENT_NAME_SIZE);
        g_track_count++;
    }
    g_current = -1;
    g_list_scroll = 0;
    easyui.redraw();
}

static void play_track(int index) {
    if (index < 0 || index >= g_track_count) return;
    char path[PATH_SIZE];
    join(path, sizeof path, g_folder, g_tracks[index]);
    long r = mos_audio_open(path);
    if (r != SYS_SUCCESS) {
        syscall_audio_status_t st;
        mos_audio_status(&st);
        strlcpy_safe(g_note, !st.driver[0] ? "No sound card found"
                             : r == (long)SYS_ERR_NOTFOUND ? "File not found"
                             : "This file could not be played", sizeof g_note);
        easyui.redraw();
        return;
    }
    g_note[0] = '\0';
    g_current = index;
    strlcpy_safe(g_playing, path, sizeof g_playing);
    easyui.redraw();
}

/* A file opens and plays; a folder is listed. */
static void open_path(const char* path) {
    if (!path || !path[0]) return;
    if (is_dir(path)) {
        scan_folder(path);
        return;
    }
    char folder[PATH_SIZE];
    strlcpy_safe(folder, path, sizeof folder);
    char* slash = strrchr(folder, '/');
    if (slash) {
        if (slash > folder && slash[-1] == ':') slash[1] = '\0';   /* keep "0:/" */
        else *slash = '\0';
    }
    scan_folder(folder);
    const char* name = base_name(path);
    for (int i = 0; i < g_track_count; i++)
        if (!strcmp(g_tracks[i], name)) { play_track(i); return; }
    if (mos_audio_open(path) == SYS_SUCCESS) strlcpy_safe(g_playing, path, sizeof g_playing);
    else strlcpy_safe(g_note, "This file could not be played", sizeof g_note);
}

static void default_folder(char* out, size_t size) {
#ifdef PLAYER_HAS_MOSDESK
    mosdesk_settings_t s = mosdesk_settings_load();
    if (mosdesk_user_path(out, size, s.username, 0) && is_dir(out)) return;
#endif
    strlcpy_safe(out, "0:/", size);
}

static int next_index(void) {
    if (!g_track_count) return -1;
    if (g_shuffle && g_track_count > 1) {
        g_rand = g_rand * 1103515245u + 12345u;
        int pick = (int)((g_rand >> 16) % (uint32_t)(g_track_count - 1));
        return pick >= g_current ? pick + 1 : pick;
    }
    if (g_current + 1 < g_track_count) return g_current + 1;
    return g_repeat ? 0 : -1;
}

/* ---- refresh from the kernel's player ---- */

static void refresh(int unused) {
    (void)unused;
    syscall_audio_status_t st;
    if (mos_audio_status(&st) != SYS_SUCCESS) return;
    /* Track finished: go on with the next one. */
    if (st.state == SYSCALL_AUDIO_ENDED && g_last_state == SYSCALL_AUDIO_PLAYING &&
        !strcmp(st.path, g_playing) && g_current >= 0) {
        g_last_state = st.state;
        int next = next_index();
        if (next >= 0) { play_track(next); return; }
    }
    g_last_state = st.state;
    g_st = st;
    easyui.redraw();
}

/* ---- drawing ---- */

static void draw(int unused) {
    (void)unused;
    int w = easyui.width(), h = easyui.height();
    int head = h * 44 / 100;
    if (head > 300) head = 300;
    if (head < 200) head = 200;
    uint32_t now = (uint32_t)mos_uptime();
    int playing = g_st.state == SYSCALL_AUDIO_PLAYING;

    /* Header: navy at the top fading to magenta. */
    for (int y = 0; y < head; y++)
        easyui.fillrect(0, y, w, 1, mix(C_HEAD_TOP, C_HEAD_LOW, (uint32_t)(y * 255 / head)));
    easyui.fillrect(0, head, w, h - head, C_BODY);

    g_hit_menu = (box_t){ w - 48, 8, 40, 40 };
    icon_menu((float)(w - 28), 28, g_hover == HIT_MENU ? 0xFFFFFFFFu : 0xD0FFFFFFu);
    char folder[64];
    fit(folder, sizeof folder, g_folder, w - 120, 14);
    easyui.drawtextfont(20, 20, folder, 0xB0FFFFFFu, EASYUI_FONT_MEDIUM, 14);

    /* The glowing disc; it breathes while music plays. */
    float cx = (float)w / 2, cy = (float)head * 0.56f;
    float r = (float)(head < w ? head : w) * 0.27f;
    float pulse = 0;
    if (playing) {
        uint32_t t = now % 2000u;
        pulse = (t < 1000u ? (float)t : (float)(2000u - t)) / 1000.0f;
    }
    disc(cx, cy, r * (1.42f + 0.05f * pulse), 0x22FFFFFFu);
    disc(cx, cy, r * 1.2f, 0x26FFFFFFu);
    disc(cx, cy, r, 0x30FFFFFFu);
    if (g_st.state == SYSCALL_AUDIO_IDLE) icon_play(cx + 3, cy, r * 0.55f, 0xF0FFFFFFu);
    else icon_album(cx, cy, r * 0.8f, 0xF0FFFFFFu);

    /* Title and seek line. */
    int y = head + 16;
    char line_text[128];
    line_text[0] = '\0';
    if (g_note[0]) strlcpy_safe(line_text, g_note, sizeof line_text);
    else if (!g_st.driver[0]) strlcpy_safe(line_text, "No sound card found", sizeof line_text);
    else if (g_st.state == SYSCALL_AUDIO_PAUSED) strlcpy_safe(line_text, "-  Paused  -", sizeof line_text);
    else if (g_st.state != SYSCALL_AUDIO_IDLE) {
        append(line_text, sizeof line_text, "-  ");
        append_number(line_text, sizeof line_text, g_st.sample_rate / 1000u);
        append(line_text, sizeof line_text, " kHz ");
        append(line_text, sizeof line_text, g_st.channels == 1 ? "mono" : "stereo");
        append(line_text, sizeof line_text, "  -");
    }
    text_center(w / 2, y, line_text, g_note[0] ? C_PINK : C_DIM, 13);

    char title[96], shown[96];
    if (g_st.state == SYSCALL_AUDIO_IDLE) strlcpy_safe(title, "Nothing playing", sizeof title);
    else track_title(title, sizeof title, g_st.path);
    fit(shown, sizeof shown, title, w - 110, 17);
    int tw = easyui.textwidthfont(shown, EASYUI_FONT_MEDIUM, 17) + 48;
    easyui.fillroundrect(w / 2 - tw / 2, y + 24, tw, 34, 17, C_PILL);
    text_center(w / 2, y + 32, shown, C_TEXT, 17);

    int sx0 = 24, sx1 = w - 24, sy = y + 82;
    uint32_t dur = g_st.duration_ms, pos = g_seeking ? g_seek_ms : g_st.position_ms;
    int knob = sx0 + (dur ? (int)((uint64_t)(sx1 - sx0) * pos / dur) : 0);
    easyui.fillroundrect(sx0, sy - 1, sx1 - sx0, 3, 1, C_LINE);
    if (knob > sx0) easyui.fillroundrect(sx0, sy - 1, knob - sx0, 3, 1, C_PINK);
    disc((float)knob, (float)sy + 0.5f, g_seeking || g_hover == HIT_SEEK ? 7.0f : 5.0f, C_PINK);
    g_hit_seek = (box_t){ sx0 - 8, sy - 12, sx1 - sx0 + 16, 24 };
    char t[16];
    format_time(t, sizeof t, pos);
    easyui.drawtextfont(sx0, sy + 10, t, C_DIM, EASYUI_FONT_MEDIUM, 12);
    format_time(t, sizeof t, dur);
    easyui.drawtextfont(sx1 - easyui.textwidthfont(t, EASYUI_FONT_MEDIUM, 12), sy + 10, t,
                        C_DIM, EASYUI_FONT_MEDIUM, 12);

    /* Transport: repeat, previous, play/pause, next, shuffle. */
    int by = sy + 58, mid = w / 2;
    int gap = w < 360 ? 56 : 68;
    g_hit_repeat = (box_t){ mid - 2 * gap - 18, by - 18, 36, 36 };
    g_hit_prev = (box_t){ mid - gap - 20, by - 20, 40, 40 };
    g_hit_play = (box_t){ mid - 32, by - 32, 64, 64 };
    g_hit_next = (box_t){ mid + gap - 20, by - 20, 40, 40 };
    g_hit_shuffle = (box_t){ mid + 2 * gap - 18, by - 18, 36, 36 };
    icon_repeat((float)(mid - 2 * gap), (float)by,
                g_repeat ? C_PINK : g_hover == HIT_REPEAT ? C_TEXT : C_DIM);
    icon_shuffle((float)(mid + 2 * gap), (float)by,
                 g_shuffle ? C_PINK : g_hover == HIT_SHUFFLE ? C_TEXT : C_DIM);
    for (int side = -1; side <= 1; side += 2) {
        int hover = g_hover == (side < 0 ? HIT_PREV : HIT_NEXT);
        float bx = (float)(mid + side * gap);
        if (hover) disc(bx, (float)by, 17, 0x30FFFFFFu);
        ring(bx, (float)by, 17, 1.6f, hover ? C_TEXT : 0xC0EEF1F7u);
        if (side < 0) icon_prev(bx, (float)by, C_TEXT);
        else icon_next(bx, (float)by, C_TEXT);
    }
    /* The big button: a soft halo and a glossy face. */
    disc((float)mid, (float)by, 34, 0x30000000u);
    disc((float)mid, (float)by, 30, g_hover == HIT_PLAY ? 0xFF6E7C9Cu : 0xFF5A6788u);
    for (int i = 0; i < 12; i++)                    /* lighter toward the top */
        disc((float)mid, (float)by - 6 - i, 22 - (float)i * 1.6f, 0x0CFFFFFFu);
    ring((float)mid, (float)by, 30, 1.4f, 0x60FFFFFFu);
    if (playing) icon_pause((float)mid, (float)by, 22, 0xFFFFFFFFu);
    else icon_play((float)mid + 2, (float)by, 22, 0xFFFFFFFFu);

    /* Tracks. */
    int ly = by + 52;
    g_hit_list = (box_t){ 16, ly, w - 32, h - ly - 10 };
    int rows = g_hit_list.h / ROW_H;
    if (rows < 0) rows = 0;
    if (g_list_scroll > g_track_count - rows) g_list_scroll = g_track_count - rows;
    if (g_list_scroll < 0) g_list_scroll = 0;
    if (!g_track_count) {
        text_center(w / 2, ly + 14, "No .adi files here", C_DIM, 14);
        text_center(w / 2, ly + 36, "Open another folder with the menu button", C_DIM, 12);
    }
    for (int i = 0; i < rows && g_list_scroll + i < g_track_count; i++) {
        int row = g_list_scroll + i, ry = ly + i * ROW_H;
        int rw = w - 32;
        for (int x = 0; x < rw; x += 4)             /* left-to-right gradient */
            easyui.fillrect(16 + x, ry + 2, rw - x < 4 ? rw - x : 4, ROW_H - 6,
                            mix(C_ROW_L, C_ROW_R, (uint32_t)(x * 255 / rw)));
        if (row == g_hover_row) easyui.fillrect(16, ry + 2, rw, ROW_H - 6, 0x18FFFFFFu);
        int current = row == g_current && g_st.state != SYSCALL_AUDIO_IDLE;
        if (current) easyui.fillrect(16, ry + 2, 3, ROW_H - 6, C_PINK);
        char name[96];
        track_title(name, sizeof name, g_tracks[row]);
        fit(shown, sizeof shown, name, rw - 70, 14);
        easyui.drawtextfont(30, ry + 8, shown, current ? C_PINK : C_TEXT,
                            EASYUI_FONT_MEDIUM, 14);
        easyui.drawtextfont(30, ry + 26, current && playing ? "Playing" : "ADI audio",
                            C_DIM, EASYUI_FONT_MEDIUM, 11);
        float ix = (float)(16 + rw - 26), iy = (float)(ry + ROW_H / 2 - 1);
        if (current && playing) icon_pause(ix, iy, 13, C_PINK);
        else icon_play(ix, iy, 13, row == g_hover_row ? C_TEXT : C_DIM);
    }
    if (g_track_count > rows && rows > 0) {         /* scroll position */
        int track = rows * ROW_H - 6;
        int thumb = track * rows / g_track_count;
        if (thumb < 16) thumb = 16;
        int ty = ly + 2 + (track - thumb) * g_list_scroll / (g_track_count - rows);
        easyui.fillroundrect(w - 12, ty, 3, thumb, 1, 0x60FFFFFFu);
    }
}

/* ---- input ---- */

static int hit_at(int x, int y, int* row) {
    *row = -1;
    if (inside(&g_hit_menu, x, y)) return HIT_MENU;
    if (inside(&g_hit_seek, x, y)) return HIT_SEEK;
    if (inside(&g_hit_repeat, x, y)) return HIT_REPEAT;
    if (inside(&g_hit_prev, x, y)) return HIT_PREV;
    if (inside(&g_hit_play, x, y)) return HIT_PLAY;
    if (inside(&g_hit_next, x, y)) return HIT_NEXT;
    if (inside(&g_hit_shuffle, x, y)) return HIT_SHUFFLE;
    if (inside(&g_hit_list, x, y)) {
        int r = g_list_scroll + (y - g_hit_list.y) / ROW_H;
        if (r < g_track_count && (y - g_hit_list.y) / ROW_H < g_hit_list.h / ROW_H) {
            *row = r;
            return HIT_ROW;
        }
    }
    return HIT_NONE;
}

static uint32_t seek_ms_at(int x) {
    int x0 = g_hit_seek.x + 8, x1 = g_hit_seek.x + g_hit_seek.w - 8;
    if (x < x0) x = x0;
    if (x > x1) x = x1;
    return (uint32_t)((uint64_t)g_st.duration_ms * (uint32_t)(x - x0) /
                      (uint32_t)(x1 - x0 > 0 ? x1 - x0 : 1));
}

static void on_folder_picked(int ok) {
    if (ok && easyui.pickedpath()[0]) scan_folder(easyui.pickedpath());
}

static void play_pause(void) {
    if (g_st.state == SYSCALL_AUDIO_PLAYING) mos_audio_pause();
    else if (g_st.state == SYSCALL_AUDIO_PAUSED || g_st.state == SYSCALL_AUDIO_ENDED)
        mos_audio_resume();
    else play_track(g_current >= 0 ? g_current : 0);
}

static void on_press(int unused) {
    (void)unused;
    int x, y, b, row;
    easyui.mouse(&x, &y, &b);
    switch (hit_at(x, y, &row)) {
        case HIT_MENU: easyui.pickfolder("Open a music folder", g_folder, on_folder_picked); break;
        case HIT_SEEK:
            if (g_st.duration_ms) { g_seeking = 1; g_seek_ms = seek_ms_at(x); }
            break;
        case HIT_REPEAT: g_repeat = !g_repeat; break;
        case HIT_SHUFFLE: g_shuffle = !g_shuffle; break;
        case HIT_PLAY: play_pause(); break;
        case HIT_PREV:
            if (g_st.position_ms > 3000 || g_current <= 0) mos_audio_seek(0);
            else play_track(g_current - 1);
            break;
        case HIT_NEXT: {
            int next = next_index();
            if (next < 0 && g_track_count) next = 0;
            play_track(next);
            break;
        }
        case HIT_ROW: play_track(row); break;
        default: break;
    }
    refresh(0);
}

static void on_frame(int unused) {
    (void)unused;
    int x, y, b, row;
    easyui.mouse(&x, &y, &b);
    if (g_seeking) {
        g_seek_ms = seek_ms_at(x);
        if (!(b & 1)) {
            g_seeking = 0;
            mos_audio_seek(g_seek_ms);
            refresh(0);
        }
        easyui.redraw();
        return;
    }
    int hit = hit_at(x, y, &row);
    if (hit != g_hover || row != g_hover_row) {
        g_hover = hit;
        g_hover_row = row;
        easyui.redraw();
    }
    /* The disc breathes while playing. */
    if (g_st.state == SYSCALL_AUDIO_PLAYING && (mos_uptime() / 50) % 2 == 0) easyui.redraw();
}

static void on_wheel(int delta) {
    g_list_scroll -= delta;
    easyui.redraw();
}

static void on_resize(int unused) {
    (void)unused;
    easyui.setsize(canvas, easyui.width(), easyui.height());
}

static void on_close(int v) {
    (void)v;
    mos_audio_stop();
    easyui.quit();
}

int main(int argc, char** argv) {
    if (!easyui.init("Player", W0, H0)) return 1;
    easyui.setsmoothtext(1);
    g_rand ^= (uint32_t)mos_uptime();
    canvas = easyui.createcanvas(0, 0, W0, H0);
    easyui.setanchor(canvas, EASYUI_ANCHOR_LEFT | EASYUI_ANCHOR_RIGHT |
                             EASYUI_ANCHOR_TOP | EASYUI_ANCHOR_BOTTOM);
    easyui.recallfunc(canvas, ONDRAW, draw);
    easyui.recallfunc(canvas, ONPRESS, on_press);
    easyui.recallfunc(canvas, ONWHEEL, on_wheel);
    easyui.recallfunc(EASYUI_WINDOW, ONFRAME, on_frame);
    easyui.recallfunc(EASYUI_WINDOW, ONRESIZE, on_resize);
    easyui.recallfunc(EASYUI_WINDOW, ONCLOSE, on_close);

    if (argc > 1 && argv[1] && argv[1][0]) {
        open_path(argv[1]);
    } else {
        char folder[PATH_SIZE];
        default_folder(folder, sizeof folder);
        scan_folder(folder);
    }
    refresh(0);
    easyui.settimer(250, refresh);
    return easyui.run();
}
