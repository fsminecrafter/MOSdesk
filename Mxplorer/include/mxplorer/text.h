#ifndef MXPLORER_TEXT_H
#define MXPLORER_TEXT_H

#include <stddef.h>
#include <stdint.h>

/* Font flags for the _px functions. */
#define MX_TEXT_BOLD   1
#define MX_TEXT_ITALIC 2
#define MX_TEXT_MONO   4
#define MX_TEXT_MEDIUM 8

int mx_text_load_fonts(void);

/* Legacy size codes: 1 = 12 px regular, 2+ = 8 * size px medium. */
int mx_text_width(const char* text, int size);
void mx_text_draw(uint32_t* pixels, uint32_t width, uint32_t height,
                  int x, int y, const char* text, uint32_t color, int size);

/* Pixel-size text. `length` may be (size_t)-1 for NUL-terminated text.
 * (x, y) is the top of the line box. */
int mx_text_width_px(const char* text, size_t length, int px, int flags);
int mx_text_ascent_px(int px, int flags);
int mx_text_height_px(int px, int flags);
void mx_text_draw_px(uint32_t* pixels, uint32_t width, uint32_t height,
                     int x, int y, const char* text, size_t length,
                     uint32_t color, int px, int flags);
/* Same, clipped to a rectangle. */
void mx_text_draw_clipped(uint32_t* pixels, uint32_t width, uint32_t height,
                          int clip_x, int clip_y, int clip_w, int clip_h,
                          int x, int y, const char* text, size_t length,
                          uint32_t color, int px, int flags);
/* Number of bytes of `text` that fit in max_width (UTF-8 safe). */
size_t mx_text_fit(const char* text, int px, int flags, int max_width);

#endif
