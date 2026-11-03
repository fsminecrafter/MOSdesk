#ifndef MXPLORER_GFX_H
#define MXPLORER_GFX_H

#include <stddef.h>
#include <stdint.h>
#include "mxplorer/icon_ids.h"

/* An ARGB8888 surface plus a clip rectangle. Every primitive alpha-blends
 * (the color's top byte is its opacity) and stays inside the clip. */
typedef struct {
    uint32_t* pixels;
    int width;
    int height;
    int clip_x0, clip_y0, clip_x1, clip_y1;
} mx_canvas_t;

mx_canvas_t mx_canvas(uint32_t* pixels, int width, int height);
/* Intersects the clip with a rectangle; returns the previous clip. */
mx_canvas_t mx_canvas_clip(const mx_canvas_t* canvas, int x, int y, int w,
                           int h);

uint32_t mx_mix(uint32_t a, uint32_t b, unsigned t);   /* t: 0 = a, 255 = b */
uint32_t mx_with_alpha(uint32_t color, unsigned alpha);
int mx_is_dark_color(uint32_t color);

void mx_fill(const mx_canvas_t* c, int x, int y, int w, int h, uint32_t color);
void mx_stroke(const mx_canvas_t* c, int x, int y, int w, int h,
               uint32_t color);
void mx_round_rect(const mx_canvas_t* c, int x, int y, int w, int h,
                   int radius, uint32_t color);
void mx_round_rect_stroke(const mx_canvas_t* c, int x, int y, int w, int h,
                          int radius, int thickness, uint32_t color);
/* Rounded only on the top corners (browser tabs). */
void mx_round_rect_top(const mx_canvas_t* c, int x, int y, int w, int h,
                       int radius, uint32_t color);
void mx_circle(const mx_canvas_t* c, int cx, int cy, int radius,
               uint32_t color);
void mx_ring(const mx_canvas_t* c, int cx, int cy, int radius, int thickness,
             uint32_t color);
void mx_shadow(const mx_canvas_t* c, int x, int y, int w, int h, int radius,
               int spread, uint32_t color);
/* sizes 16, 20 and (some icons) 48 are stored; others use the nearest. */
void mx_icon(const mx_canvas_t* c, mx_icon_t icon, int size, int x, int y,
             uint32_t color);
void mx_text(const mx_canvas_t* c, int x, int y, const char* text,
             size_t length, uint32_t color, int px, int flags);
/* Draws text cut to max_width with a trailing ellipsis when needed. */
void mx_text_ellipsis(const mx_canvas_t* c, int x, int y, int max_width,
                      const char* text, uint32_t color, int px, int flags);

double mx_sqrt(double value);

typedef struct {
    mx_icon_t icon;
    int size;
    const unsigned char* data;
} mx_icon_bitmap_t;

extern const mx_icon_bitmap_t mx_icon_bitmaps[];
extern const unsigned mx_icon_bitmap_count;

#endif
