#include "mxplorer/text.h"
#include "string.h"
#include "stdlib.h"

#ifdef MX_HOST
#include "easyui_text.h"
#include <stdio.h>
#define MX_FONT_DIR "Resources/fonts/"   /* run host tools from Mxplorer/ */
#else
#include "font.h"
#define MX_FONT_DIR "0:/programs/Mxplorer/Resources/fonts/"
#endif

enum { FACE_REGULAR, FACE_MEDIUM, FACE_BOLD, FACE_MONO, FACE_COUNT };

static const char* const face_files[FACE_COUNT] = {
    MX_FONT_DIR "Inter-Regular.otf",
    MX_FONT_DIR "Inter-Medium.otf",
    MX_FONT_DIR "Inter-Bold.ttf",
    MX_FONT_DIR "RobotoMono-Regular.ttf",
};

static ttrend_outline_font_t faces[FACE_COUNT];
static int face_ok[FACE_COUNT];

static void* read_whole_file(const char* path, size_t* size_out) {
#ifdef MX_HOST
    FILE* file = fopen(path, "rb");
    if (!file) return NULL;
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    void* data = size > 0 ? malloc((size_t)size) : NULL;
    if (data && fread(data, 1, (size_t)size, file) != (size_t)size) {
        free(data);
        data = NULL;
    }
    fclose(file);
    *size_out = (size_t)size;
    return data;
#else
    long fd = mos_open(path, SYS_O_RDONLY);
    if (fd <= 0) return NULL;
    long size = mos_size(fd);
    uint8_t* data = size > 0 && size <= 4L * 1024 * 1024
                        ? (uint8_t*)malloc((size_t)size) : NULL;
    long got = 0;
    while (data && got < size) {
        long n = mos_read(fd, data + got, (size_t)(size - got));
        if (n <= 0) break;
        got += n;
    }
    mos_close(fd);
    if (data && got != size) {
        free(data);
        data = NULL;
    }
    *size_out = (size_t)size;
    return data;
#endif
}

int mx_text_load_fonts(void) {
    int loaded = 0;
    for (int i = 0; i < FACE_COUNT; i++) {
        size_t size = 0;
        void* data = read_whole_file(face_files[i], &size);
        if (!data) continue;
        if (ttrend_outline_font_load_memory(&faces[i], data, size, 12u) == 0) {
            face_ok[i] = 1;
            loaded++;
        } else {
            free(data);
        }
    }
#ifndef MX_HOST
    /* Keep the SDK's draw_text() (used for the bitmap fallback) on Inter. */
    font_load(face_files[FACE_REGULAR], 0);
    font_load(face_files[FACE_MEDIUM], 1);
#endif
    return face_ok[FACE_REGULAR] && face_ok[FACE_MEDIUM];
}

static ttrend_outline_font_t* pick_face(int px, int flags) {
    int want = FACE_REGULAR;
    if (flags & MX_TEXT_MONO) want = FACE_MONO;
    else if (flags & MX_TEXT_BOLD) want = FACE_BOLD;
    else if (flags & MX_TEXT_MEDIUM) want = FACE_MEDIUM;
    if (!face_ok[want]) {
        if (want == FACE_BOLD && face_ok[FACE_MEDIUM]) want = FACE_MEDIUM;
        else want = FACE_REGULAR;
    }
    if (!face_ok[want]) return NULL;
    ttrend_outline_font_t* face = &faces[want];
    if (px < 6) px = 6;
    if (px > 96) px = 96;
    face->pixel_size = (uint32_t)px;
    face->italic = (flags & MX_TEXT_ITALIC) ? 1u : 0u;
    face->tracking = 0;
    return face;
}

/* Copies a slice into a NUL-terminated buffer for the renderer. */
static const char* terminated(const char* text, size_t length, char* buffer,
                              size_t capacity) {
    if (length == (size_t)-1) return text;
    if (length >= capacity) length = capacity - 1;
    memcpy(buffer, text, length);
    buffer[length] = '\0';
    return buffer;
}

static int bitmap_scale(int px) {
    int scale = (px + 4) / 8;
    return scale < 1 ? 1 : scale;
}

int mx_text_width_px(const char* text, size_t length, int px, int flags) {
    if (!text) return 0;
    char buffer[512];
    const char* line = terminated(text, length, buffer, sizeof(buffer));
    ttrend_outline_font_t* face = pick_face(px, flags);
    if (!face) return (int)strlen(line) * 8 * bitmap_scale(px);
    return (int)ttrend_outline_text_width(line, face);
}

int mx_text_ascent_px(int px, int flags) {
    ttrend_outline_font_t* face = pick_face(px, flags);
    return face ? (int)ttrend_outline_ascent(face) : 8 * bitmap_scale(px);
}

int mx_text_height_px(int px, int flags) {
    ttrend_outline_font_t* face = pick_face(px, flags);
    if (!face) return 8 * bitmap_scale(px) + 2;
    return (int)(ttrend_outline_ascent(face) + ttrend_outline_descent(face));
}

void mx_text_draw_clipped(uint32_t* pixels, uint32_t width, uint32_t height,
                          int clip_x, int clip_y, int clip_w, int clip_h,
                          int x, int y, const char* text, size_t length,
                          uint32_t color, int px, int flags) {
    if (!pixels || !text) return;
    if (clip_x < 0) { clip_w += clip_x; clip_x = 0; }
    if (clip_y < 0) { clip_h += clip_y; clip_y = 0; }
    if (clip_x + clip_w > (int)width) clip_w = (int)width - clip_x;
    if (clip_y + clip_h > (int)height) clip_h = (int)height - clip_y;
    if (clip_w <= 0 || clip_h <= 0) return;
    char buffer[512];
    const char* line = terminated(text, length, buffer, sizeof(buffer));
    ttrend_outline_font_t* face = pick_face(px, flags);
    uint32_t* origin = pixels + (size_t)clip_y * width + (size_t)clip_x;
    if (face) {
        ttrend_outline_surface_t surface = {
            origin, (uint32_t)clip_w, (uint32_t)clip_h, width
        };
        int32_t baseline = y - clip_y + ttrend_outline_ascent(face);
        ttrend_outline_draw_text_baseline(&surface, x - clip_x, baseline, line,
                                          color, face);
        if ((flags & MX_TEXT_BOLD) && !face_ok[FACE_BOLD])
            ttrend_outline_draw_text_baseline(&surface, x - clip_x + 1,
                                              baseline, line, color, face);
        return;
    }
#ifndef MX_HOST
    /* Bitmap fallback through the SDK, clipped by drawing into the full
     * surface only when the text lies inside the clip vertically. */
    if (y >= clip_y && y + 8 * bitmap_scale(px) <= clip_y + clip_h)
        draw_text(pixels, width, height, x, y, line, color, bitmap_scale(px));
#endif
}

void mx_text_draw_px(uint32_t* pixels, uint32_t width, uint32_t height,
                     int x, int y, const char* text, size_t length,
                     uint32_t color, int px, int flags) {
    mx_text_draw_clipped(pixels, width, height, 0, 0, (int)width, (int)height,
                         x, y, text, length, color, px, flags);
}

size_t mx_text_fit(const char* text, int px, int flags, int max_width) {
    size_t length = strlen(text);
    if (mx_text_width_px(text, length, px, flags) <= max_width) return length;
    /* character boundaries, then binary search the longest fitting prefix */
    size_t bounds[512];
    int count = 0;
    for (size_t i = 0; i <= length && count < 512; i++)
        if (i == length || ((unsigned char)text[i] & 0xC0) != 0x80)
            bounds[count++] = i;
    int low = 0, high = count - 1;
    while (low < high) {
        int mid = (low + high + 1) / 2;
        if (mx_text_width_px(text, bounds[mid], px, flags) <= max_width)
            low = mid;
        else
            high = mid - 1;
    }
    return bounds[low];
}

int mx_text_width(const char* text, int size) {
    return size <= 1 ? mx_text_width_px(text, (size_t)-1, 12, 0)
                     : mx_text_width_px(text, (size_t)-1, 8 * size,
                                        MX_TEXT_MEDIUM);
}

void mx_text_draw(uint32_t* pixels, uint32_t width, uint32_t height,
                  int x, int y, const char* text, uint32_t color, int size) {
    if (size <= 1)
        mx_text_draw_px(pixels, width, height, x, y, text, (size_t)-1, color,
                        12, 0);
    else
        mx_text_draw_px(pixels, width, height, x, y, text, (size_t)-1, color,
                        8 * size, MX_TEXT_MEDIUM);
}
