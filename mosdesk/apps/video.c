/*
 * Video - plays .vid videos (Minimal-OS's video format, see mosmedia.h).
 *
 *     run 0:/programs/mos_video.run file.vid
 *
 * mosWM opens .vid files with it. Frames are read from the file as they
 * are needed (never the whole video at once) and decoded on the CPU. The
 * soundtrack plays in the kernel straight from the same file
 * (mos_audio_open_at), and the picture follows the sound's clock, so they
 * stay in step; without a sound card the picture keeps its own time.
 *
 * Space or a click on the picture: play / pause. Left / Right: 5 seconds
 * back / forward. Click or drag the line under the picture to seek.
 */
#include "easyui.h"
#include "audio.h"
#include "mosmedia.h"
#include "stdlib.h"
#include "string.h"

#define BAR_H 52
#define C_BG     0xFF000000u
#define C_BAR    0xFF15181Cu
#define C_TEXT   0xFFF2F5F8u
#define C_DIM    0xFF97A3AFu
#define C_TRACK  0xFF3A424Bu

static char g_path[SYSCALL_AUDIO_PATH_MAX];
static long g_fd = -1;
static mvid_reader_t g_vid;
static uint32_t* g_frame;           /* decoded frame, width x height */
static int64_t g_shown = -1;        /* frame number in g_frame */
static char g_note[96];
static int canvas;

/* Time. With sound the kernel's playback position is the clock;
 * otherwise uptime since g_clock_start. */
static int g_has_sound;             /* the soundtrack is playing in the kernel */
static int g_playing;
static uint32_t g_pos_ms;           /* position while paused / last computed */
static long g_clock_start;          /* uptime at position 0 (no sound) */
static uint32_t g_audio_pos, g_audio_seen;   /* last kernel position, uptime then */
static int g_seeking;
static uint32_t g_seek_ms;

/* Scaling: each output column's source column. */
static int g_view_x, g_view_y, g_view_w, g_view_h;
static uint32_t* g_xmap;
static int g_xmap_w;

static void append(char* dst, size_t size, const char* src) {
    size_t n = strlen(dst);
    while (*src && n + 1 < size) dst[n++] = *src++;
    dst[n] = '\0';
}

static void append_time(char* dst, size_t size, uint32_t ms) {
    uint32_t s = ms / 1000u;
    char t[16];
    int n = 0;
    uint32_t m = s / 60u;
    char digits[12];
    int d = 0;
    do { digits[d++] = (char)('0' + m % 10u); m /= 10u; } while (m);
    while (d) t[n++] = digits[--d];
    t[n++] = ':';
    t[n++] = (char)('0' + s % 60u / 10u);
    t[n++] = (char)('0' + s % 10u);
    t[n] = '\0';
    append(dst, size, t);
}

static bool file_read(void* ctx, uint64_t offset, void* buf, size_t len) {
    long fd = *(long*)ctx;
    if (offset > 0x7FFFFFFFull || mos_seek(fd, (long)offset) < 0) return false;
    uint8_t* p = (uint8_t*)buf;
    while (len) {
        long got = mos_read(fd, p, len);
        if (got <= 0) return false;
        p += got;
        len -= (size_t)got;
    }
    return true;
}

static void* app_alloc(size_t n) { return malloc(n); }
static void app_free(void* p) { free(p); }

static uint32_t duration(void) { return g_vid.info.duration_ms; }

/* The current position in milliseconds. */
static uint32_t position(void) {
    if (!g_playing) return g_pos_ms;
    long now = mos_uptime();
    uint32_t pos;
    if (g_has_sound) {
        syscall_audio_status_t st;
        mos_audio_status(&st);
        if (st.state == SYSCALL_AUDIO_ENDED || st.state == SYSCALL_AUDIO_IDLE) {
            pos = duration();
        } else {
            /* The kernel reports in steps; fill in between them. */
            if (st.position_ms != g_audio_pos) { g_audio_pos = st.position_ms; g_audio_seen = (uint32_t)now; }
            uint32_t since = (uint32_t)now - g_audio_seen;
            pos = g_audio_pos + (since < 500u ? since : 500u);
        }
    } else {
        pos = (uint32_t)(now - g_clock_start);
    }
    if (pos >= duration()) {
        pos = duration();
        g_playing = 0;
    }
    g_pos_ms = pos;
    return pos;
}

static void start_at(uint32_t ms) {
    if (ms >= duration()) ms = 0;
    g_pos_ms = ms;
    g_playing = 1;
    g_clock_start = mos_uptime() - (long)ms;
    g_audio_pos = ms;
    g_audio_seen = (uint32_t)mos_uptime();
    if (g_has_sound) {
        syscall_audio_status_t st;
        mos_audio_status(&st);
        if (st.state == SYSCALL_AUDIO_IDLE || st.state == SYSCALL_AUDIO_ENDED ||
            strcmp(st.path, g_path) != 0) {
            if (mos_audio_open_at(g_path, (uint32_t)g_vid.info.audio_offset) != (long)SYS_SUCCESS) {
                g_has_sound = 0;
                return;
            }
        }
        mos_audio_seek(ms);
        mos_audio_resume();
    }
}

static void pause_video(void) {
    g_pos_ms = position();
    g_playing = 0;
    if (g_has_sound) mos_audio_pause();
}

static void play_pause(void) {
    if (g_playing) pause_video();
    else start_at(g_pos_ms);
}

static void seek_to(uint32_t ms) {
    if (ms > duration()) ms = duration();
    if (g_playing) start_at(ms);
    else {
        g_pos_ms = ms;
        if (g_has_sound) mos_audio_seek(ms);
    }
}

static void layout(void) {
    int w = easyui.width(), h = easyui.height() - BAR_H;
    if (h < 1) h = 1;
    uint32_t vw = g_vid.info.width, vh = g_vid.info.height;
    if (!vw || !vh) { g_view_w = g_view_h = 0; return; }
    /* Fit inside, keeping the shape. */
    int fw = w, fh = (int)((uint64_t)vh * (uint32_t)w / vw);
    if (fh > h) { fh = h; fw = (int)((uint64_t)vw * (uint32_t)h / vh); }
    g_view_w = fw; g_view_h = fh;
    g_view_x = (w - fw) / 2; g_view_y = (h - fh) / 2;
    if (g_xmap_w != fw) {
        free(g_xmap);
        g_xmap = (uint32_t*)malloc((size_t)(fw > 0 ? fw : 1) * 4u);
        g_xmap_w = g_xmap ? fw : 0;
        for (int x = 0; g_xmap && x < fw; x++) g_xmap[x] = (uint32_t)((uint64_t)x * vw / (uint32_t)fw);
    }
}

static void draw_picture(void) {
    uint32_t* px = easyui.pixels();
    int stride = easyui.width(), H = easyui.height() - BAR_H;
    if (!px || !g_frame || g_shown < 0 || !g_xmap || g_xmap_w != g_view_w) return;
    uint32_t vw = g_vid.info.width, vh = g_vid.info.height;
    for (int y = 0; y < g_view_h; y++) {
        int dy = g_view_y + y;
        if (dy < 0 || dy >= H) continue;
        const uint32_t* src = g_frame + (size_t)((uint64_t)y * vh / (uint32_t)g_view_h) * vw;
        uint32_t* dst = px + (size_t)dy * (size_t)stride + g_view_x;
        for (int x = 0; x < g_view_w; x++) dst[x] = src[g_xmap[x]] | 0xFF000000u;
    }
}

/* Where the seek line is. */
static void seek_line(int* x0, int* x1, int* y) {
    *x0 = 64;
    *x1 = easyui.width() - 120;
    *y = easyui.height() - BAR_H / 2;
}

static void draw(int unused) {
    (void)unused;
    int W = easyui.width(), H = easyui.height();
    easyui.fillrect(0, 0, W, H - BAR_H, C_BG);
    draw_picture();
    if (g_note[0]) {
        int tw = easyui.textwidth(g_note, 2);
        easyui.drawtext((W - tw) / 2, (H - BAR_H) / 2 - 8, g_note, C_TEXT, 2);
    }
    /* Control bar: play button, seek line, time. */
    easyui.fillrect(0, H - BAR_H, W, BAR_H, C_BAR);
    int cy = H - BAR_H / 2;
    easyui.fillcircle(32, cy, 17, easyui.accent());
    if (g_playing) {
        easyui.fillrect(26, cy - 7, 4, 14, 0xFFFFFFFFu);
        easyui.fillrect(34, cy - 7, 4, 14, 0xFFFFFFFFu);
    } else {
        /* A triangle pointing right, one column at a time. */
        for (int x = 0; x < 13; x++) {
            int half = 7 - x * 7 / 13;
            easyui.fillrect(27 + x, cy - half, 1, 2 * half, 0xFFFFFFFFu);
        }
    }
    int x0, x1, ly;
    seek_line(&x0, &x1, &ly);
    uint32_t pos = g_seeking ? g_seek_ms : g_pos_ms;
    int at = duration() ? x0 + (int)((uint64_t)(x1 - x0) * pos / duration()) : x0;
    easyui.fillroundrect(x0, ly - 2, x1 - x0, 4, 2, C_TRACK);
    easyui.fillroundrect(x0, ly - 2, at - x0, 4, 2, easyui.accent());
    easyui.fillcircle(at, ly, 7, 0xFFFFFFFFu);
    char t[32] = "";
    append_time(t, sizeof t, pos);
    append(t, sizeof t, " / ");
    append_time(t, sizeof t, duration());
    easyui.drawtext(x1 + 16, ly - 8, t, C_DIM, 2);
}

/* Shows the frame for the current time; true when it changed. */
static int show_current(void) {
    if (!g_frame) return 0;
    uint32_t n = mvid_frame_at(&g_vid.info, g_seeking ? g_seek_ms : position());
    if ((int64_t)n == g_shown) return 0;
    if (mvid_decode(&g_vid, n, g_frame)) {
        g_shown = n;
        g_note[0] = '\0';
    } else {
        strcpy(g_note, "This video is damaged");
        g_shown = -1;
    }
    return 1;
}

static uint32_t seek_ms_at(int x) {
    int x0, x1, y;
    seek_line(&x0, &x1, &y);
    if (x < x0) x = x0;
    if (x > x1) x = x1;
    return (uint32_t)((uint64_t)duration() * (uint32_t)(x - x0) / (uint32_t)(x1 - x0 > 0 ? x1 - x0 : 1));
}

static void on_press(int unused) {
    (void)unused;
    if (!g_frame) return;
    int x, y, b;
    easyui.mouse(&x, &y, &b);
    int x0, x1, ly;
    seek_line(&x0, &x1, &ly);
    if (y >= easyui.height() - BAR_H) {
        if (x < 56) play_pause();
        else if (x >= x0 - 10 && x <= x1 + 10) { g_seeking = 1; g_seek_ms = seek_ms_at(x); }
    } else {
        play_pause();
    }
    easyui.redraw();
}

static void on_key(int value) {
    if (!g_frame) return;
    int key = value & 0xFF;
    if (key == EASYUI_KEY_SPACE || key == EASYUI_KEY_ENTER) play_pause();
    else if (key == EASYUI_KEY_LEFT) seek_to(position() > 5000u ? position() - 5000u : 0);
    else if (key == EASYUI_KEY_RIGHT) seek_to(position() + 5000u);
    else if (key == EASYUI_KEY_HOME) seek_to(0);
    else return;
    easyui.redraw();
}

static void on_frame(int unused) {
    (void)unused;
    if (!g_frame) return;
    int x, y, b;
    easyui.mouse(&x, &y, &b);
    if (g_seeking) {
        g_seek_ms = seek_ms_at(x);
        if (!(b & 1)) {
            g_seeking = 0;
            seek_to(g_seek_ms);
        }
        show_current();
        easyui.redraw();
        return;
    }
    int was_playing = g_playing;
    if (show_current() || g_playing || was_playing) easyui.redraw();
}

static void on_resize(int unused) {
    (void)unused;
    easyui.setsize(canvas, easyui.width(), easyui.height());
    layout();
}

static void on_close(int unused) {
    (void)unused;
    if (g_has_sound) mos_audio_stop();
    easyui.quit();
}

static const char* base_name(const char* path) {
    const char* slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

/* Opens the video; on failure g_note says why. */
static void open_video(const char* path) {
    size_t i = 0;
    for (; path[i] && i + 1 < sizeof g_path; i++) g_path[i] = path[i];
    g_path[i] = '\0';
    g_fd = mos_open(g_path, SYS_O_RDONLY);
    if (g_fd < 0) { strcpy(g_note, "The video could not be opened"); return; }
    long size = mos_size(g_fd);
    if (size <= 0 || !mvid_open(&g_vid, file_read, &g_fd, (uint64_t)size, app_alloc)) {
        strcpy(g_note, "This is not a .vid video, or it is damaged");
        return;
    }
    g_frame = (uint32_t*)calloc((size_t)g_vid.info.width * g_vid.info.height, 4u);
    if (!g_frame) { strcpy(g_note, "Not enough memory for this video"); return; }
    /* The soundtrack, if the machine can play it. */
    g_has_sound = 0;
    if (g_vid.info.has_audio) {
        if (mos_audio_open_at(g_path, (uint32_t)g_vid.info.audio_offset) == (long)SYS_SUCCESS) {
            g_has_sound = 1;
            mos_audio_pause();
        }
    }
    show_current();
    start_at(0);
}

int main(int argc, char** argv) {
    int w = 800, h = 450 + BAR_H;
    if (argc > 1 && argv[1] && argv[1][0]) open_video(argv[1]);
    else strcpy(g_note, "Open a .vid file from Files to play it");
    if (g_frame) {
        /* The video's size, within reason. */
        uint32_t vw = g_vid.info.width, vh = g_vid.info.height;
        if (vw > 960u) { vh = vh * 960u / vw; vw = 960u; }
        if (vh > 600u) { vw = vw * 600u / vh; vh = 600u; }
        if (vw < 400u) vw = 400u;
        w = (int)vw;
        h = (int)vh + BAR_H;
    }
    if (!easyui.init(g_frame ? base_name(g_path) : "Video", w, h)) return 1;
    canvas = easyui.createcanvas(0, 0, w, h);
    easyui.setanchor(canvas, EASYUI_ANCHOR_LEFT | EASYUI_ANCHOR_RIGHT |
                             EASYUI_ANCHOR_TOP | EASYUI_ANCHOR_BOTTOM);
    easyui.recallfunc(canvas, ONDRAW, draw);
    easyui.recallfunc(canvas, ONPRESS, on_press);
    easyui.recallfunc(EASYUI_WINDOW, ONKEY, on_key);
    easyui.recallfunc(EASYUI_WINDOW, ONFRAME, on_frame);
    easyui.recallfunc(EASYUI_WINDOW, ONRESIZE, on_resize);
    easyui.recallfunc(EASYUI_WINDOW, ONCLOSE, on_close);
    layout();
    int result = easyui.run();
    if (g_fd >= 0) mos_close(g_fd);
    mvid_close(&g_vid, app_free);
    return result;
}
