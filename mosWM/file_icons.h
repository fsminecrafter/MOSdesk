#ifndef MOSWM_FILE_ICONS_H
#define MOSWM_FILE_ICONS_H

/* MOSdesk icon set: 64x64 pixel-art icons (see mosdesk/tools/icons/).
 * The art lives in file_icon_data.h as a palette + run-length encoding and is
 * decoded on demand; any size can be requested, 64 is pixel-perfect and the
 * other sizes are area-averaged from the 64x64 source. */

#include <stddef.h>
#include <stdint.h>

#include "file_icon_data.h"

typedef enum {
    MOSWM_ICON_FOLDER,
    MOSWM_ICON_DRIVE,
    MOSWM_ICON_DOCUMENT,
    MOSWM_ICON_PDF,
    MOSWM_ICON_IMAGE,
    MOSWM_ICON_AUDIO,
    MOSWM_ICON_VIDEO,
    MOSWM_ICON_CODE,
    MOSWM_ICON_ARCHIVE,
    MOSWM_ICON_SETTINGS,
    MOSWM_ICON_NETWORK,
    MOSWM_ICON_POINTER,
    MOSWM_ICON_HOME,
    MOSWM_ICON_COMPUTER,
    MOSWM_ICON_APPEARANCE,
    MOSWM_ICON_MOUSE,
    MOSWM_ICON_TRASH,
    MOSWM_ICON_LOCK,
    MOSWM_ICON_DESKTOP,
    MOSWM_ICON_APP,
    MOSWM_ICON_EDIT,
    MOSWM_ICON_COPY,
    MOSWM_ICON_PASTE,
    MOSWM_ICON_REFRESH,
    MOSWM_ICON_UP,
    MOSWM_ICON_NEWFOLDER,
    MOSWM_ICON_INFO,
    MOSWM_ICON_TEXT,
    MOSWM_ICON_TERMINAL,
    MOSWM_ICON_COUNT
} moswm_file_icon_t;

#define MOSWM_ICON_SOURCE_SIZE 64

static uint32_t moswm_icon_scratch[MOSWM_ICON_SOURCE_SIZE * MOSWM_ICON_SOURCE_SIZE];

static void moswm_icon_pixel(uint32_t* pixels, uint32_t surface_width,
                             uint32_t surface_height, int32_t x, int32_t y,
                             uint32_t color) {
    if (x < 0 || y < 0 || (uint32_t)x >= surface_width ||
        (uint32_t)y >= surface_height)
        return;
    uint32_t* destination = pixels + (size_t)y * surface_width + (uint32_t)x;
    uint32_t alpha = color >> 24u;
    if (alpha == 255u) {
        *destination = color;
        return;
    }
    if (!alpha) return;
    uint32_t inverse = 255u - alpha;
    uint32_t red = (((color >> 16u) & 255u) * alpha +
                    ((*destination >> 16u) & 255u) * inverse) / 255u;
    uint32_t green = (((color >> 8u) & 255u) * alpha +
                      ((*destination >> 8u) & 255u) * inverse) / 255u;
    uint32_t blue = ((color & 255u) * alpha +
                     (*destination & 255u) * inverse) / 255u;
    *destination = 0xFF000000u | (red << 16u) | (green << 8u) | blue;
}

static void moswm_icon_rect(uint32_t* pixels, uint32_t surface_width,
                            uint32_t surface_height, int32_t x, int32_t y,
                            int32_t width, int32_t height, uint32_t color) {
    for (int32_t row = 0; row < height; row++)
        for (int32_t column = 0; column < width; column++)
            moswm_icon_pixel(pixels, surface_width, surface_height,
                             x + column, y + row, color);
}

static void moswm_icon_decode(moswm_file_icon_t type, uint32_t* out) {
    if ((unsigned)type >= MOSWM_ICON_COUNT) type = MOSWM_ICON_DOCUMENT;
    const uint32_t* palette = moswm_icon_table[type].palette;
    const uint8_t* runs = moswm_icon_table[type].runs;
    uint32_t bytes = moswm_icon_table[type].run_bytes;
    uint32_t at = 0;
    for (uint32_t i = 0; i + 1u < bytes && at < MOSWM_ICON_SOURCE_SIZE *
                                                 MOSWM_ICON_SOURCE_SIZE; i += 2u) {
        uint32_t color = palette[runs[i + 1u]];
        for (uint32_t n = runs[i]; n && at < MOSWM_ICON_SOURCE_SIZE *
                                              MOSWM_ICON_SOURCE_SIZE; n--)
            out[at++] = color;
    }
    while (at < MOSWM_ICON_SOURCE_SIZE * MOSWM_ICON_SOURCE_SIZE) out[at++] = 0;
}

/* Draws icon `type` into a size x size box whose top-left is (x, y). */
static void moswm_draw_file_icon_sized(uint32_t* pixels, uint32_t surface_width,
                                       uint32_t surface_height, int32_t x,
                                       int32_t y, moswm_file_icon_t type,
                                       uint32_t size) {
    if (!size) return;
    moswm_icon_decode(type, moswm_icon_scratch);
    const uint32_t src = MOSWM_ICON_SOURCE_SIZE;
    for (uint32_t dy = 0; dy < size; dy++) {
        uint32_t y0 = dy * src / size;
        uint32_t y1 = (dy + 1u) * src / size;
        if (y1 <= y0) y1 = y0 + 1u;
        for (uint32_t dx = 0; dx < size; dx++) {
            uint32_t x0 = dx * src / size;
            uint32_t x1 = (dx + 1u) * src / size;
            if (x1 <= x0) x1 = x0 + 1u;
            uint32_t color;
            if (size == src) {
                color = moswm_icon_scratch[dy * src + dx];
            } else {
                /* premultiplied box average */
                uint32_t a = 0, r = 0, g = 0, b = 0, n = 0;
                for (uint32_t sy = y0; sy < y1; sy++)
                    for (uint32_t sx = x0; sx < x1; sx++) {
                        uint32_t c = moswm_icon_scratch[sy * src + sx];
                        uint32_t ca = c >> 24u;
                        a += ca;
                        r += ((c >> 16u) & 255u) * ca;
                        g += ((c >> 8u) & 255u) * ca;
                        b += (c & 255u) * ca;
                        n++;
                    }
                if (!a) continue;
                color = ((a / n) << 24u) | ((r / a) << 16u) |
                        ((g / a) << 8u) | (b / a);
            }
            if (color >> 24u)
                moswm_icon_pixel(pixels, surface_width, surface_height,
                                 x + (int32_t)dx, y + (int32_t)dy, color);
        }
    }
}

/* Convenience: the classic 32x32 footprint. */
static void moswm_draw_file_icon(uint32_t* pixels, uint32_t surface_width,
                                 uint32_t surface_height, int32_t x, int32_t y,
                                 moswm_file_icon_t type) {
    moswm_draw_file_icon_sized(pixels, surface_width, surface_height, x, y,
                               type, 32u);
}

#endif
