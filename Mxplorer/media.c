/* Pictures (.img) and videos (.vid) in pages. See mxplorer/media.h.
 *
 * Pictures are decoded once per address and kept premultiplied, with one
 * scaled copy for the size they were last drawn at. Each <video> element
 * has its own player: its file (in memory, or an open local file), one
 * decoded frame and a clock. Like the Video app, the soundtrack plays in
 * the kernel straight from the file (mos_audio_open_at) and the picture
 * follows the sound's position; a downloaded video's file is written to
 * 0:/data first, since the kernel plays sound from files. */
#include "mxplorer/media.h"
#include "mxplorer/devlog.h"
#include "mxplorer/http.h"
#include "mxplorer/icon_ids.h"
#include "mxplorer/text.h"
#include "mxplorer/util.h"
#include "mosmedia.h"
#include "string.h"
#include "stdlib.h"

#ifdef MX_HOST
#include <stdio.h>
#else
#include "minimalos.h"
#include "audio.h"
#endif

#define MAX_IMAGES 96
#define MAX_PLAYERS 8
#define URL_MAX 1024
#define IMAGE_FILE_MAX (64u * 1024u * 1024u)
#define PIXEL_BUDGET (40u * 1024u * 1024u)   /* decoded pixels kept, all pictures */
#define SOUND_FILE "0:/data/mxplorer-video.vid"
#define BAR_H 36

enum { ST_EMPTY, ST_READY, ST_FAILED };

typedef struct {
    char url[URL_MAX];
    int state;
    int w, h;
    uint32_t* px;        /* premultiplied ARGB */
    uint32_t* scaled;    /* px at sw x sh */
    int sw, sh;
} image_t;

typedef struct {
    mx_node_t* node;
    char url[URL_MAX];
    int state;
    uint8_t* data;        /* downloaded file */
    size_t size;
    long fd;              /* local file, -1 if none */
    char path[256];       /* local file, or SOUND_FILE once written */
    mvid_reader_t r;
    uint32_t* frame;      /* width x height, straight ARGB */
    int64_t shown;
    int playing, ended, started, muted, autoplay_done;
    uint32_t pos_ms;
    long clock_start;
    int sound;            /* the kernel is playing this video's sound */
    uint32_t audio_pos;
    long audio_seen;
    uint32_t last_second;
} player_t;

static image_t images[MAX_IMAGES];
static int image_count;
static size_t pixels_used;
static player_t players[MAX_PLAYERS];
static int player_count;
static mx_fetch_fn fetch_fn;
static void* fetch_user;
static int allow_local;
#ifndef MX_HOST
static char sound_file_url[URL_MAX];   /* whose video is in SOUND_FILE */
#endif
static uint32_t* xmap;
static int xmap_cap;

/* ====================================================================== */
/* Addresses and files                                                    */
/* ====================================================================== */

int mx_media_kind_of_url(const char* url) {
    if (!url) return MX_MEDIA_NONE;
    size_t n = 0;
    while (url[n] && url[n] != '?' && url[n] != '#') n++;
    if (n < 5 || url[n - 4] != '.') return MX_MEDIA_NONE;
    char ext[4] = {mx_lower(url[n - 3]), mx_lower(url[n - 2]), mx_lower(url[n - 1]), 0};
    if (!strcmp(ext, "img")) return MX_MEDIA_IMAGE;
    if (!strcmp(ext, "vid")) return MX_MEDIA_VIDEO;
    return MX_MEDIA_NONE;
}

#ifdef MX_HOST
/* On the build PC "0:/x" is the host's "/x". */
static const char* host_path(const char* path) {
    return path[0] >= '0' && path[0] <= '9' && path[1] == ':' ? path + 2 : path;
}
#endif

static uint8_t* read_local(const char* path, size_t limit, size_t* size) {
#ifdef MX_HOST
    FILE* f = fopen(host_path(path), "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* d = n > 0 && (size_t)n <= limit ? (uint8_t*)malloc((size_t)n) : NULL;
    if (d && fread(d, 1, (size_t)n, f) != (size_t)n) { free(d); d = NULL; }
    fclose(f);
    if (d) *size = (size_t)n;
    return d;
#else
    long fd = mos_open(path, SYS_O_RDONLY);
    if (fd < 0) return NULL;
    long n = mos_size(fd);
    uint8_t* d = n > 0 && (size_t)n <= limit ? (uint8_t*)malloc((size_t)n) : NULL;
    size_t got = 0;
    while (d && got < (size_t)n) {
        long r = mos_read(fd, d + got, (size_t)n - got);
        if (r <= 0) { free(d); d = NULL; break; }
        got += (size_t)r;
    }
    mos_close(fd);
    if (d) *size = (size_t)n;
    return d;
#endif
}

/* The whole file at `url`; NULL when it cannot be had. */
static uint8_t* load_file(const char* url, const char* kind, size_t limit, size_t* size) {
    char path[256];
    if (mx_url_local_path(url, path, sizeof(path))) {
        if (!allow_local) {
            mx_console_add(MX_LOG_WARN, "Media", 0,
                           "Pages from the network cannot show local files.");
            return NULL;
        }
        uint8_t* d = read_local(path, limit, size);
        mx_network_entry_t* e = mx_network_add(url, kind);
        e->status = d ? 200 : 0;
        e->size = d ? *size : 0;
        mx_copy(e->content_type, sizeof(e->content_type), "local file");
        if (!d) mx_copy(e->error, sizeof(e->error), "The file could not be read.");
        return d;
    }
    if (!fetch_fn) return NULL;
    char* body = NULL;
    size_t length = 0;
    if (!fetch_fn(fetch_user, url, kind, &body, &length) || !body) return NULL;
    *size = length;
    return (uint8_t*)body;
}

static void report(const char* what, const char* url) {
    char text[300];
    mx_copy(text, sizeof(text), what);
    mx_append(text, sizeof(text), ": ");
    mx_append(text, sizeof(text), url);
    mx_console_add(MX_LOG_WARN, "Media", 0, text);
}

/* ====================================================================== */
/* Pictures                                                               */
/* ====================================================================== */

static uint32_t premultiply(uint32_t c) {
    uint32_t a = c >> 24;
    if (a == 255) return c;
    if (a == 0) return 0;
    uint32_t r = ((c >> 16) & 255u) * a / 255u, g = ((c >> 8) & 255u) * a / 255u,
             b = (c & 255u) * a / 255u;
    return a << 24 | r << 16 | g << 8 | b;
}

static image_t* find_image(const char* url) {
    for (int i = 0; i < image_count; i++)
        if (!strcmp(images[i].url, url)) return &images[i];
    return NULL;
}

static image_t* get_image(const char* url) {
    image_t* img = find_image(url);
    if (img) return img;
    if (image_count >= MAX_IMAGES) return NULL;
    img = &images[image_count++];
    memset(img, 0, sizeof(*img));
    mx_copy(img->url, sizeof(img->url), url);
    img->state = ST_FAILED;
    size_t size = 0;
    uint8_t* data = load_file(url, "image", IMAGE_FILE_MAX, &size);
    if (!data) {
        report("Could not load the picture", url);
        return img;
    }
    mimg_info_t info;
    if (!mimg_info(data, size, &info)) {
        report("Not a .img picture, or damaged", url);
        free(data);
        return img;
    }
    size_t count = (size_t)info.width * info.height;
    if (pixels_used + count > PIXEL_BUDGET) {
        report("Too many pictures on this page; skipped", url);
        free(data);
        return img;
    }
    img->px = (uint32_t*)malloc(count * 4u);
    if (!img->px || !mimg_decode(data, size, img->px, info.width)) {
        report(img->px ? "Damaged .img picture" : "Not enough memory for the picture", url);
        free(img->px);
        img->px = NULL;
        free(data);
        return img;
    }
    free(data);
    if (info.alpha)
        for (size_t i = 0; i < count; i++) img->px[i] = premultiply(img->px[i]);
    img->w = (int)info.width;
    img->h = (int)info.height;
    img->state = ST_READY;
    pixels_used += count;
    return img;
}

/* Area average (shrinking) or bilinear (enlarging) resampling of
 * premultiplied pixels. */
static void resample(const uint32_t* src, int sw, int sh, uint32_t* dst, int dw, int dh) {
    if (dw <= sw && dh <= sh) {
        for (int y = 0; y < dh; y++) {
            int y0 = (int)((int64_t)y * sh / dh), y1 = (int)((int64_t)(y + 1) * sh / dh);
            if (y1 <= y0) y1 = y0 + 1;
            for (int x = 0; x < dw; x++) {
                int x0 = (int)((int64_t)x * sw / dw), x1 = (int)((int64_t)(x + 1) * sw / dw);
                if (x1 <= x0) x1 = x0 + 1;
                uint32_t a = 0, r = 0, g = 0, b = 0, n = 0;
                for (int yy = y0; yy < y1; yy++) {
                    const uint32_t* row = src + (size_t)yy * sw;
                    for (int xx = x0; xx < x1; xx++) {
                        uint32_t c = row[xx];
                        a += c >> 24; r += (c >> 16) & 255u; g += (c >> 8) & 255u; b += c & 255u;
                        n++;
                    }
                }
                dst[(size_t)y * dw + x] = (a / n) << 24 | (r / n) << 16 | (g / n) << 8 | (b / n);
            }
        }
        return;
    }
    for (int y = 0; y < dh; y++) {
        /* source position in 1/256 pixels, pixel centres aligned */
        int fy = (int)(((int64_t)(2 * y + 1) * sh * 128) / dh) - 128;
        if (fy < 0) fy = 0;
        int y0 = fy >> 8, ty = fy & 255;
        int y1 = y0 + 1 < sh ? y0 + 1 : sh - 1;
        if (y0 >= sh) y0 = y1 = sh - 1;
        for (int x = 0; x < dw; x++) {
            int fx = (int)(((int64_t)(2 * x + 1) * sw * 128) / dw) - 128;
            if (fx < 0) fx = 0;
            int x0 = fx >> 8, tx = fx & 255;
            int x1 = x0 + 1 < sw ? x0 + 1 : sw - 1;
            if (x0 >= sw) x0 = x1 = sw - 1;
            uint32_t c00 = src[(size_t)y0 * sw + x0], c01 = src[(size_t)y0 * sw + x1];
            uint32_t c10 = src[(size_t)y1 * sw + x0], c11 = src[(size_t)y1 * sw + x1];
            uint32_t out = 0;
            for (int s = 0; s < 32; s += 8) {
                uint32_t top = ((c00 >> s) & 255u) * (256u - tx) + ((c01 >> s) & 255u) * (uint32_t)tx;
                uint32_t bot = ((c10 >> s) & 255u) * (256u - tx) + ((c11 >> s) & 255u) * (uint32_t)tx;
                uint32_t v = (top * (256u - ty) + bot * (uint32_t)ty) >> 16;
                out |= (v > 255u ? 255u : v) << s;
            }
            dst[(size_t)y * dw + x] = out;
        }
    }
}

/* Blends premultiplied `src` (stride sw, drawn at x, y) into the canvas. */
static void blit(const mx_canvas_t* c, const uint32_t* src, int sw, int w, int h, int x, int y,
                 unsigned opacity) {
    int x0 = x > c->clip_x0 ? x : c->clip_x0, y0 = y > c->clip_y0 ? y : c->clip_y0;
    int x1 = x + w < c->clip_x1 ? x + w : c->clip_x1, y1 = y + h < c->clip_y1 ? y + h : c->clip_y1;
    for (int py = y0; py < y1; py++) {
        const uint32_t* s = src + (size_t)(py - y) * sw + (x0 - x);
        uint32_t* d = c->pixels + (size_t)py * c->width + x0;
        for (int px = x0; px < x1; px++, s++, d++) {
            uint32_t v = *s;
            if (opacity < 255) {
                uint32_t rb = ((v & 0x00FF00FFu) * opacity >> 8) & 0x00FF00FFu;
                uint32_t ag = (((v >> 8) & 0x00FF00FFu) * opacity >> 8) & 0x00FF00FFu;
                v = ag << 8 | rb;
            }
            uint32_t a = v >> 24;
            if (a == 255) { *d = v; continue; }
            if (!a) continue;
            uint32_t k = 255u - a, o = *d;
            uint32_t rb = ((o & 0x00FF00FFu) * k / 255u) & 0x00FF00FFu;
            uint32_t g = ((o & 0x0000FF00u) * k / 255u) & 0x0000FF00u;
            *d = 0xFF000000u | ((rb + (v & 0x00FF00FFu)) & 0x00FF00FFu) |
                 ((g + (v & 0x0000FF00u)) & 0x0000FF00u);
        }
    }
}

static void draw_image(const mx_canvas_t* c, image_t* img, int x, int y, int w, int h,
                       unsigned opacity) {
    if (w <= 0 || h <= 0 || img->state != ST_READY) return;
    if (w == img->w && h == img->h) {
        blit(c, img->px, img->w, w, h, x, y, opacity);
        return;
    }
    if (img->sw != w || img->sh != h) {
        free(img->scaled);
        img->scaled = NULL;
        img->sw = img->sh = 0;
        if ((size_t)w * h <= 16u * 1024u * 1024u)
            img->scaled = (uint32_t*)malloc((size_t)w * h * 4u);
        if (!img->scaled) return;
        resample(img->px, img->w, img->h, img->scaled, w, h);
        img->sw = w;
        img->sh = h;
    }
    blit(c, img->scaled, w, w, h, x, y, opacity);
}

/* ====================================================================== */
/* Videos                                                                 */
/* ====================================================================== */

static bool read_memory(void* ctx, uint64_t offset, void* buf, size_t len) {
    player_t* p = (player_t*)ctx;
    if (offset > p->size || len > p->size - offset) return false;
    memcpy(buf, p->data + offset, len);
    return true;
}

#ifndef MX_HOST
static bool read_fd(void* ctx, uint64_t offset, void* buf, size_t len) {
    player_t* p = (player_t*)ctx;
    if (offset > 0x7FFFFFFFull || mos_seek(p->fd, (long)offset) < 0) return false;
    uint8_t* d = (uint8_t*)buf;
    while (len) {
        long got = mos_read(p->fd, d, len);
        if (got <= 0) return false;
        d += got;
        len -= (size_t)got;
    }
    return true;
}
#endif

static void* media_alloc(size_t n) { return malloc(n); }
static void media_free(void* p) { free(p); }

static int open_player(player_t* p) {
    char path[256];
    p->fd = -1;
    if (mx_url_local_path(p->url, path, sizeof(path))) {
        if (!allow_local) {
            mx_console_add(MX_LOG_WARN, "Media", 0,
                           "Pages from the network cannot show local files.");
            return 0;
        }
        mx_network_entry_t* e = mx_network_add(p->url, "video");
        mx_copy(e->content_type, sizeof(e->content_type), "local file");
#ifdef MX_HOST
        p->data = read_local(path, (size_t)1 << 31, &p->size);
        e->status = p->data ? 200 : 0;
        e->size = p->size;
        if (!p->data) return 0;
        return mvid_open(&p->r, read_memory, p, p->size, media_alloc);
#else
        /* local videos are read as they play */
        p->fd = mos_open(path, SYS_O_RDONLY);
        long size = p->fd >= 0 ? mos_size(p->fd) : -1;
        e->status = size > 0 ? 200 : 0;
        e->size = size > 0 ? (size_t)size : 0;
        if (size <= 0) return 0;
        mx_copy(p->path, sizeof(p->path), path);
        return mvid_open(&p->r, read_fd, p, (uint64_t)size, media_alloc);
#endif
    }
    p->data = load_file(p->url, "video", MX_HTTP_MEDIA_MAX, &p->size);
    if (!p->data) return 0;
    return mvid_open(&p->r, read_memory, p, p->size, media_alloc);
}

#ifndef MX_HOST
static int owns_sound(player_t* p, syscall_audio_status_t* st);
#endif

static void close_player(player_t* p) {
#ifndef MX_HOST
    syscall_audio_status_t st;
    if (p->sound && owns_sound(p, &st)) mos_audio_stop();   /* not another program's */
    if (p->fd >= 0) mos_close(p->fd);
#endif
    p->sound = 0;
    if (p->state == ST_READY) mvid_close(&p->r, media_free);
    free(p->frame);
    free(p->data);
    memset(p, 0, sizeof(*p));
    p->fd = -1;
}

static player_t* find_player(mx_node_t* n) {
    for (int i = 0; i < player_count; i++)
        if (players[i].node == n) return &players[i];
    return NULL;
}

/* The video's address: src="" or the first <source> naming a .vid. */
static int video_url(mx_node_t* n, char* out, size_t capacity) {
    const char* src = mx_dom_attr(n, "src");
    if (src && *src) {
        mx_html_resolve(src, out, capacity);
        return mx_media_kind_of_url(out) == MX_MEDIA_VIDEO;
    }
    for (mx_node_t* c = n->first; c; c = c->next) {
        if (!mx_dom_is(c, "source")) continue;
        src = mx_dom_attr(c, "src");
        if (!src || !*src) continue;
        mx_html_resolve(src, out, capacity);
        if (mx_media_kind_of_url(out) == MX_MEDIA_VIDEO) return 1;
    }
    return 0;
}

static player_t* get_player(mx_node_t* n) {
    char url[URL_MAX];
    if (!video_url(n, url, sizeof(url))) {
        player_t* old = find_player(n);
        if (old) close_player(old);   /* the source went away */
        return NULL;
    }
    player_t* p = find_player(n);
    if (p && !strcmp(p->url, url)) return p;
    if (p) {
        close_player(p);   /* the source changed */
    } else {
        for (int i = 0; i < player_count && !p; i++)
            if (!players[i].node) p = &players[i];
        if (!p) {
            if (player_count >= MAX_PLAYERS) return NULL;
            p = &players[player_count++];
        }
        memset(p, 0, sizeof(*p));
    }
    p->node = n;
    mx_copy(p->url, sizeof(p->url), url);
    p->state = ST_FAILED;
    p->shown = -1;
    p->muted = mx_dom_attr(n, "muted") != NULL;
    if (!open_player(p)) {
        report("Could not load the video (.vid)", url);
        return p;
    }
    p->state = ST_READY;
    p->frame = (uint32_t*)calloc((size_t)p->r.info.width * p->r.info.height, 4u);
    if (!p->frame) {
        report("Not enough memory for the video", url);
        mvid_close(&p->r, media_free);
        p->state = ST_FAILED;
        return p;
    }
    if (mvid_decode(&p->r, 0, p->frame)) p->shown = 0;
    return p;
}

static uint32_t duration_of(const player_t* p) { return p->r.info.duration_ms; }

#ifndef MX_HOST
/* The kernel's sound is this video's (nobody else took it). */
static int owns_sound(player_t* p, syscall_audio_status_t* st) {
    mos_audio_status(st);
    return st->state != SYSCALL_AUDIO_IDLE && !strcmp(st->path, p->path);
}

/* A file the kernel can play the soundtrack from. */
static int sound_path(player_t* p) {
    if (p->fd >= 0) return p->path[0] != '\0';
    if (!strcmp(sound_file_url, p->url)) {
        mx_copy(p->path, sizeof(p->path), SOUND_FILE);
        return 1;
    }
    if (!mos_exists("0:/data") && mos_mkdir("0:/data") != SYS_SUCCESS) return 0;
    if (mos_exists(SOUND_FILE)) mos_syscall(SYS_DELETE, (long)(uintptr_t)SOUND_FILE, 0, 0);
    sound_file_url[0] = '\0';
    if (mos_create(SOUND_FILE, "video", "vid") < 0) return 0;
    long fd = mos_open(SOUND_FILE, SYS_O_RDWR);
    if (fd < 0) return 0;
    long written = mos_fwrite(fd, p->data, p->size);
    mos_close(fd);
    if (written != (long)p->size) return 0;
    mx_copy(sound_file_url, sizeof(sound_file_url), p->url);
    mx_copy(p->path, sizeof(p->path), SOUND_FILE);
    return 1;
}
#endif

static void start_sound(player_t* p, uint32_t ms) {
#ifdef MX_HOST
    (void)p;
    (void)ms;
#else
    p->sound = 0;
    if (p->muted || !p->r.info.has_audio) return;
    for (int i = 0; i < player_count; i++) players[i].sound = 0;   /* one sound at a time */
    if (!sound_path(p)) return;
    if (mos_audio_open_at(p->path, (uint32_t)p->r.info.audio_offset) != (long)SYS_SUCCESS) return;
    mos_audio_seek(ms);
    mos_audio_resume();
    p->sound = 1;
#endif
}

static void start_at(player_t* p, uint32_t ms) {
    if (ms >= duration_of(p)) ms = 0;
    long now = mx_now_ms();
    p->pos_ms = ms;
    p->playing = 1;
    p->ended = 0;
    p->started = 1;
    p->clock_start = now - (long)ms;
    p->audio_pos = ms;
    p->audio_seen = now;
    start_sound(p, ms);
}

static void pause_sound(player_t* p) {
#ifdef MX_HOST
    (void)p;
#else
    syscall_audio_status_t st;
    if (p->sound && owns_sound(p, &st)) mos_audio_pause();
#endif
}

/* The position now, in milliseconds. */
static uint32_t position(player_t* p) {
    if (!p->playing) return p->pos_ms;
    long now = mx_now_ms();
    uint32_t pos = (uint32_t)(now - p->clock_start);
#ifndef MX_HOST
    if (p->sound) {
        syscall_audio_status_t st;
        if (!owns_sound(p, &st)) {
            p->sound = 0;   /* another program took the sound: keep our own time */
        } else if (st.state == SYSCALL_AUDIO_ENDED) {
            pos = duration_of(p);
        } else {
            /* the kernel reports in steps; fill in between them */
            if (st.position_ms != p->audio_pos) { p->audio_pos = st.position_ms; p->audio_seen = now; }
            long since = now - p->audio_seen;
            pos = p->audio_pos + (uint32_t)(since < 500 ? since : 500);
            p->clock_start = now - (long)pos;
        }
    }
#endif
    if (pos >= duration_of(p)) {
        if (p->node && mx_dom_attr(p->node, "loop")) {
            start_at(p, 0);
            return 0;
        }
        pos = duration_of(p);
        p->playing = 0;
        p->ended = 1;
        pause_sound(p);
    }
    p->pos_ms = pos;
    return pos;
}

static void pause_player(player_t* p) {
    if (p->playing) {
        p->pos_ms = position(p);   /* may end the video */
        p->playing = 0;
    }
    pause_sound(p);
}

static void seek_player(player_t* p, uint32_t ms) {
    if (ms > duration_of(p)) ms = duration_of(p);
    p->ended = 0;
    if (p->playing) {
        start_at(p, ms);
    } else {
        p->pos_ms = ms;
#ifndef MX_HOST
        syscall_audio_status_t st;
        if (p->sound && owns_sound(p, &st)) mos_audio_seek(ms);
#endif
    }
}

/* Decodes the frame for the current time; 1 when it changed. */
static int update_frame(player_t* p) {
    if (p->state != ST_READY || !p->frame) return 0;
    uint32_t n = mvid_frame_at(&p->r.info, position(p));
    if ((int64_t)n == p->shown) return 0;
    if (!mvid_decode(&p->r, n, p->frame)) {
        report("Damaged frame in the video", p->url);
        pause_player(p);
        return 0;
    }
    p->shown = n;
    return 1;
}

static void format_time(char* out, size_t capacity, uint32_t ms) {
    uint32_t s = ms / 1000u;
    out[0] = '\0';
    mx_append_int(out, capacity, (int)(s / 60u));
    mx_append(out, capacity, s % 60u < 10u ? ":0" : ":");
    mx_append_int(out, capacity, (int)(s % 60u));
}

/* Fits sw x sh inside the box, keeping the shape. */
static void contain(int sw, int sh, int x, int y, int w, int h, int* ox, int* oy, int* ow, int* oh) {
    int fw = w, fh = sw ? (int)((int64_t)sh * w / sw) : h;
    if (fh > h) { fh = h; fw = sh ? (int)((int64_t)sw * h / sh) : w; }
    *ow = fw; *oh = fh;
    *ox = x + (w - fw) / 2;
    *oy = y + (h - fh) / 2;
}

static void draw_frame(const mx_canvas_t* c, const player_t* p, int x, int y, int w, int h,
                       unsigned opacity) {
    int vw = (int)p->r.info.width, vh = (int)p->r.info.height;
    if (w <= 0 || h <= 0 || p->shown < 0) return;
    if (xmap_cap < w) {
        free(xmap);
        xmap = (uint32_t*)malloc((size_t)w * 4u);
        xmap_cap = xmap ? w : 0;
        if (!xmap) return;
    }
    for (int i = 0; i < w; i++) xmap[i] = (uint32_t)((int64_t)i * vw / w);
    int x0 = x > c->clip_x0 ? x : c->clip_x0, y0 = y > c->clip_y0 ? y : c->clip_y0;
    int x1 = x + w < c->clip_x1 ? x + w : c->clip_x1, y1 = y + h < c->clip_y1 ? y + h : c->clip_y1;
    for (int py = y0; py < y1; py++) {
        const uint32_t* src = p->frame + (size_t)((int64_t)(py - y) * vh / h) * vw;
        uint32_t* d = c->pixels + (size_t)py * c->width;
        for (int px = x0; px < x1; px++) {
            uint32_t s = src[xmap[px - x]];
            uint32_t a = (s >> 24) * opacity / 255u;
            if (a == 255) { d[px] = s | 0xFF000000u; continue; }
            d[px] = mx_mix(d[px], s | 0xFF000000u, a);
        }
    }
}

static int has_controls(mx_node_t* n) { return mx_dom_attr(n, "controls") != NULL; }

/* The control bar: the seek line runs from *t0 to *t1; the time text
 * (*tw wide, 0 when there is no room for it) sits at the right. */
static void bar_layout(const player_t* p, int x, int w, int* t0, int* t1, int* tw) {
    char a[16], text[40];
    format_time(a, sizeof(a), duration_of(p));
    mx_copy(text, sizeof(text), a);
    mx_append(text, sizeof(text), " / ");
    mx_append(text, sizeof(text), a);
    *tw = mx_text_width_px(text, (size_t)-1, 12, 0) + 6;
    *t0 = x + 38;
    *t1 = x + w - *tw - 22;
    if (*t1 - *t0 < 48) {
        *tw = 0;
        *t1 = x + w - 14;
    }
}

static void draw_controls(const mx_canvas_t* c, player_t* p, int x, int y, int w, int h,
                          uint32_t accent) {
    int big = !p->started || p->ended;
    if (big && w >= 72 && h >= BAR_H * 2 + 64) {
        int cx = x + w / 2, cy = y + h / 2;
        mx_circle(c, cx, cy, 30, 0xB0000000u);
        /* a triangle pointing right, one column at a time */
        for (int i = 0; i < 22; i++) {
            int half = 12 - i * 12 / 22;
            mx_fill(c, cx - 8 + i, cy - half, 1, 2 * half, 0xFFFFFFFFu);
        }
    }
    if (h < BAR_H + 8 || w < 120) return;
    int by = y + h - BAR_H;
    mx_fill(c, x, by, w, BAR_H, 0x99000000u);
    mx_icon(c, p->playing ? MX_ICON_PAUSE : MX_ICON_PLAY_ARROW, 20, x + 8, by + (BAR_H - 20) / 2,
            0xFFFFFFFFu);
    char now[16], total[16], text[40];
    uint32_t pos = p->playing ? position(p) : p->pos_ms;
    int t0, t1, tw, ly = by + BAR_H / 2;
    bar_layout(p, x, w, &t0, &t1, &tw);
    if (tw) {
        format_time(now, sizeof(now), pos);
        format_time(total, sizeof(total), duration_of(p));
        mx_copy(text, sizeof(text), now);
        mx_append(text, sizeof(text), " / ");
        mx_append(text, sizeof(text), total);
        int ty = by + (BAR_H - mx_text_height_px(12, 0)) / 2;
        mx_text(c, x + w - tw - 10, ty, text, strlen(text), 0xFFF2F5F8u, 12, 0);
    }
    if (t1 - t0 < 8) return;
    int at = duration_of(p) ? t0 + (int)((int64_t)(t1 - t0) * pos / duration_of(p)) : t0;
    mx_round_rect(c, t0, ly - 2, t1 - t0, 4, 2, 0x66FFFFFFu);
    if (at > t0) mx_round_rect(c, t0, ly - 2, at - t0, 4, 2, accent | 0xFF000000u);
    mx_circle(c, at, ly, 6, 0xFFFFFFFFu);
}

/* ====================================================================== */
/* Public API                                                             */
/* ====================================================================== */

void mx_media_begin(mx_fetch_fn fetch, void* user, int local) {
    mx_media_end();
    fetch_fn = fetch;
    fetch_user = user;
    allow_local = local;
}

void mx_media_end(void) {
    for (int i = 0; i < image_count; i++) {
        free(images[i].px);
        free(images[i].scaled);
    }
    image_count = 0;
    pixels_used = 0;
    for (int i = 0; i < player_count; i++) close_player(&players[i]);
    player_count = 0;
    fetch_fn = NULL;
    fetch_user = NULL;
    allow_local = 0;
}

static image_t* element_image(mx_node_t* n, const char* attr) {
    const char* src = mx_dom_attr(n, attr);
    if (!src || !*src) return NULL;
    char url[URL_MAX];
    mx_html_resolve(src, url, sizeof(url));
    if (mx_media_kind_of_url(url) != MX_MEDIA_IMAGE) return NULL;
    return get_image(url);
}

int mx_media_natural_size(mx_node_t* n, int* w, int* h) {
    if (!n || n->type != MX_NODE_ELEMENT) return 0;
    if (mx_dom_is(n, "img")) {
        image_t* img = element_image(n, "src");
        if (!img || img->state != ST_READY) return 0;
        *w = img->w;
        *h = img->h;
        return 1;
    }
    if (mx_dom_is(n, "video")) {
        player_t* p = get_player(n);
        if (p && p->state == ST_READY) {
            *w = (int)p->r.info.width;
            *h = (int)p->r.info.height;
            return 1;
        }
        image_t* poster = element_image(n, "poster");
        if (poster && poster->state == ST_READY) {
            *w = poster->w;
            *h = poster->h;
            return 1;
        }
    }
    return 0;
}

int mx_media_failed(mx_node_t* n) {
    if (!n || !mx_dom_is(n, "img")) return 0;
    const char* src = mx_dom_attr(n, "src");
    if (!src || !*src) return 0;
    char url[URL_MAX];
    mx_html_resolve(src, url, sizeof(url));
    image_t* img = find_image(url);
    return img && img->state == ST_FAILED;
}

void mx_media_paint(const mx_canvas_t* c, mx_node_t* n, int x, int y, int w, int h,
                    unsigned opacity, uint32_t accent) {
    if (mx_dom_is(n, "img")) {
        image_t* img = element_image(n, "src");
        if (img) draw_image(c, img, x, y, w, h, opacity);
        return;
    }
    if (!mx_dom_is(n, "video")) return;
    mx_fill(c, x, y, w, h, mx_with_alpha(0xFF000000u, opacity));
    player_t* p = get_player(n);
    image_t* poster = element_image(n, "poster");
    int ox, oy, ow, oh;
    if (poster && poster->state == ST_READY && (!p || !p->started)) {
        contain(poster->w, poster->h, x, y, w, h, &ox, &oy, &ow, &oh);
        draw_image(c, poster, ox, oy, ow, oh, opacity);
    } else if (p && p->state == ST_READY) {
        contain((int)p->r.info.width, (int)p->r.info.height, x, y, w, h, &ox, &oy, &ow, &oh);
        draw_frame(c, p, ox, oy, ow, oh, opacity);
    }
    if (!p || p->state != ST_READY) {
        if (w >= 24 && h >= 24)
            mx_icon(c, MX_ICON_BROKEN_IMAGE, w >= 60 && h >= 60 ? 48 : 16,
                    x + (w - (w >= 60 && h >= 60 ? 48 : 16)) / 2,
                    y + (h - (w >= 60 && h >= 60 ? 48 : 16)) / 2, 0x99FFFFFFu);
        return;
    }
    if (has_controls(n)) draw_controls(c, p, x, y, w, h, accent);
}

int mx_media_click(mx_node_t* n, int x, int y, int bx, int by, int bw, int bh) {
    if (!n || !mx_dom_is(n, "video") || !has_controls(n)) return 0;
    player_t* p = get_player(n);
    if (!p || p->state != ST_READY) return 0;
    if (y >= by + bh - BAR_H && bh >= BAR_H + 8 && bw >= 120) {
        int t0, t1, tw;
        bar_layout(p, bx, bw, &t0, &t1, &tw);
        if (x >= t0 - 6 && x <= t1 + 6 && t1 > t0) {
            int at = x < t0 ? 0 : x > t1 ? t1 - t0 : x - t0;
            seek_player(p, (uint32_t)((int64_t)duration_of(p) * at / (t1 - t0)));
            update_frame(p);
            return 1;
        }
        if (x >= bx + 38) return 1;   /* the time: nothing to do */
    }
    if (p->playing) pause_player(p);
    else start_at(p, p->ended ? 0 : p->pos_ms);
    return 1;
}

int mx_media_pointer(mx_node_t* n) {
    return n && mx_dom_is(n, "video") && has_controls(n);
}

int mx_media_tick(long now_ms) {
    (void)now_ms;
    int dirty = 0;
    for (int i = 0; i < player_count; i++) {
        player_t* p = &players[i];
        if (!p->node || p->state != ST_READY) continue;
        if (!p->autoplay_done) {
            p->autoplay_done = 1;
            if (mx_dom_attr(p->node, "autoplay")) {
                start_at(p, 0);
                dirty = 1;
            }
        }
        if (!p->playing) continue;
        if (update_frame(p)) dirty = 1;
        uint32_t second = p->pos_ms / 1000u;
        if (second != p->last_second || !p->playing) {
            p->last_second = second;
            dirty = 1;   /* the time in the controls, or it just ended */
        }
    }
    return dirty;
}

int mx_media_playing(void) {
    for (int i = 0; i < player_count; i++)
        if (players[i].node && players[i].playing) return 1;
    return 0;
}

static player_t* ready_player(mx_node_t* n) {
    if (!n || !mx_dom_is(n, "video")) return NULL;
    player_t* p = get_player(n);
    return p && p->state == ST_READY ? p : NULL;
}

void mx_media_play(mx_node_t* n) {
    player_t* p = ready_player(n);
    if (p && !p->playing) start_at(p, p->ended ? 0 : p->pos_ms);
}

void mx_media_pause(mx_node_t* n) {
    player_t* p = ready_player(n);
    if (p) pause_player(p);
}

int mx_media_paused(mx_node_t* n) {
    player_t* p = ready_player(n);
    return !p || !p->playing;
}

int mx_media_ended(mx_node_t* n) {
    player_t* p = ready_player(n);
    return p && p->ended;
}

double mx_media_current_time(mx_node_t* n) {
    player_t* p = ready_player(n);
    return p ? position(p) / 1000.0 : 0.0;
}

void mx_media_seek(mx_node_t* n, double seconds) {
    player_t* p = ready_player(n);
    if (!p) return;
    if (seconds < 0) seconds = 0;
    seek_player(p, (uint32_t)(seconds * 1000.0));
    update_frame(p);
}

double mx_media_duration(mx_node_t* n) {
    player_t* p = ready_player(n);
    return p ? duration_of(p) / 1000.0 : 0.0;
}

void mx_media_set_muted(mx_node_t* n, int muted) {
    player_t* p = ready_player(n);
    if (!p || p->muted == !!muted) return;
    p->muted = !!muted;
    if (!p->playing) return;
    uint32_t pos = position(p);
    if (p->muted) {
#ifndef MX_HOST
        syscall_audio_status_t st;
        if (p->sound && owns_sound(p, &st)) mos_audio_stop();
#endif
        p->sound = 0;
        p->clock_start = mx_now_ms() - (long)pos;
    } else {
        start_at(p, pos);
    }
}

int mx_media_muted(mx_node_t* n) {
    player_t* p = ready_player(n);
    return p ? p->muted : mx_dom_attr(n, "muted") != NULL;
}
