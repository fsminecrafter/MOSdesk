#include "mxplorer/gfx.h"
#include "mxplorer/text.h"
#include "string.h"

double mx_sqrt(double value) {
    if (value <= 0) return 0;
#if defined(__x86_64__)
    double result;
    __asm__("sqrtsd %1, %0" : "=x"(result) : "x"(value));
    return result;
#else
    double guess = value > 1 ? value / 2 : 1;
    for (int i = 0; i < 30; i++) guess = 0.5 * (guess + value / guess);
    return guess;
#endif
}

mx_canvas_t mx_canvas(uint32_t* pixels, int width, int height) {
    mx_canvas_t c = {pixels, width, height, 0, 0, width, height};
    return c;
}

mx_canvas_t mx_canvas_clip(const mx_canvas_t* canvas, int x, int y, int w,
                           int h) {
    mx_canvas_t c = *canvas;
    if (x > c.clip_x0) c.clip_x0 = x;
    if (y > c.clip_y0) c.clip_y0 = y;
    if (x + w < c.clip_x1) c.clip_x1 = x + w;
    if (y + h < c.clip_y1) c.clip_y1 = y + h;
    if (c.clip_x1 < c.clip_x0) c.clip_x1 = c.clip_x0;
    if (c.clip_y1 < c.clip_y0) c.clip_y1 = c.clip_y0;
    return c;
}

uint32_t mx_mix(uint32_t a, uint32_t b, unsigned t) {
    if (t >= 255) return b;
    unsigned u = 255 - t;
    uint32_t rr = (((a >> 16) & 255u) * u + ((b >> 16) & 255u) * t) / 255u;
    uint32_t gg = (((a >> 8) & 255u) * u + ((b >> 8) & 255u) * t) / 255u;
    uint32_t bb = ((a & 255u) * u + (b & 255u) * t) / 255u;
    uint32_t aa = ((a >> 24) * u + (b >> 24) * t) / 255u;
    return (aa << 24) | (rr << 16) | (gg << 8) | bb;
}

uint32_t mx_with_alpha(uint32_t color, unsigned alpha) {
    unsigned a = ((color >> 24) * alpha) / 255u;
    return (color & 0x00FFFFFFu) | (a << 24);
}

int mx_is_dark_color(uint32_t color) {
    unsigned r = (color >> 16) & 255u, g = (color >> 8) & 255u, b = color & 255u;
    return r * 299 + g * 587 + b * 114 < 128000;
}

static inline void blend_pixel(uint32_t* d, uint32_t color, unsigned coverage) {
    unsigned a = ((color >> 24) * coverage) / 255u;
    if (a >= 255u) {
        *d = color | 0xFF000000u;
        return;
    }
    if (!a) return;
    uint32_t o = *d;
    unsigned r = 255u - a;
    uint32_t rr = (((color >> 16) & 255u) * a + ((o >> 16) & 255u) * r) / 255u;
    uint32_t gg = (((color >> 8) & 255u) * a + ((o >> 8) & 255u) * r) / 255u;
    uint32_t bb = ((color & 255u) * a + (o & 255u) * r) / 255u;
    *d = 0xFF000000u | (rr << 16) | (gg << 8) | bb;
}

void mx_fill(const mx_canvas_t* c, int x, int y, int w, int h, uint32_t color) {
    if (!c->pixels || w <= 0 || h <= 0 || !(color >> 24)) return;
    int x0 = x < c->clip_x0 ? c->clip_x0 : x;
    int y0 = y < c->clip_y0 ? c->clip_y0 : y;
    int x1 = x + w > c->clip_x1 ? c->clip_x1 : x + w;
    int y1 = y + h > c->clip_y1 ? c->clip_y1 : y + h;
    if (x0 >= x1 || y0 >= y1) return;
    int opaque = (color >> 24) == 255u;
    for (int row = y0; row < y1; row++) {
        uint32_t* line = c->pixels + (size_t)row * (size_t)c->width;
        if (opaque) {
            for (int col = x0; col < x1; col++) line[col] = color;
        } else {
            for (int col = x0; col < x1; col++) blend_pixel(&line[col], color, 255);
        }
    }
}

void mx_stroke(const mx_canvas_t* c, int x, int y, int w, int h,
               uint32_t color) {
    mx_fill(c, x, y, w, 1, color);
    mx_fill(c, x, y + h - 1, w, 1, color);
    mx_fill(c, x, y + 1, 1, h - 2, color);
    mx_fill(c, x + w - 1, y + 1, 1, h - 2, color);
}

static inline void plot(const mx_canvas_t* c, int x, int y, uint32_t color,
                        unsigned coverage) {
    if (x < c->clip_x0 || y < c->clip_y0 || x >= c->clip_x1 || y >= c->clip_y1)
        return;
    blend_pixel(&c->pixels[(size_t)y * (size_t)c->width + (size_t)x], color,
                coverage);
}

/* Coverage (0..255) of a pixel at distance `d` from an edge of radius r. */
static unsigned edge_coverage(double distance, double radius) {
    double v = radius - distance + 0.5;
    if (v <= 0) return 0;
    if (v >= 1) return 255;
    return (unsigned)(v * 255.0);
}

/* Shape test shared by the filled shapes: rounded rectangle with per-side
 * corner selection. */
static void fill_round(const mx_canvas_t* c, int x, int y, int w, int h,
                       int radius, int top_only, uint32_t color) {
    if (w <= 0 || h <= 0) return;
    if (radius * 2 > w) radius = w / 2;
    if (radius * 2 > h && !top_only) radius = h / 2;
    if (radius > h) radius = h;
    if (radius <= 0) {
        mx_fill(c, x, y, w, h, color);
        return;
    }
    /* middle band without corners */
    mx_fill(c, x, y + radius, w, h - radius - (top_only ? 0 : radius), color);
    for (int row = 0; row < radius; row++) {
        /* top corners: rows y .. y+radius-1 */
        double cy = radius - 0.5;
        double py = row + 0.5;
        double dy = cy - py;
        int full_start = x + radius, full_end = x + w - radius;
        mx_fill(c, full_start, y + row, full_end - full_start, 1, color);
        if (!top_only)
            mx_fill(c, full_start, y + h - 1 - row, full_end - full_start, 1,
                    color);
        for (int col = 0; col < radius; col++) {
            double dx = (radius - 0.5) - (col + 0.5);
            unsigned cov = edge_coverage(mx_sqrt(dx * dx + dy * dy), radius - 0.5);
            if (!cov) continue;
            plot(c, x + col, y + row, color, cov);
            plot(c, x + w - 1 - col, y + row, color, cov);
            if (!top_only) {
                plot(c, x + col, y + h - 1 - row, color, cov);
                plot(c, x + w - 1 - col, y + h - 1 - row, color, cov);
            }
        }
    }
}

void mx_round_rect(const mx_canvas_t* c, int x, int y, int w, int h,
                   int radius, uint32_t color) {
    fill_round(c, x, y, w, h, radius, 0, color);
}

void mx_round_rect_top(const mx_canvas_t* c, int x, int y, int w, int h,
                       int radius, uint32_t color) {
    fill_round(c, x, y, w, h, radius, 1, color);
}

void mx_round_rect_stroke(const mx_canvas_t* c, int x, int y, int w, int h,
                          int radius, int thickness, uint32_t color) {
    if (w <= 0 || h <= 0 || thickness <= 0) return;
    if (radius * 2 > w) radius = w / 2;
    if (radius * 2 > h) radius = h / 2;
    if (radius <= 0) {
        for (int i = 0; i < thickness; i++)
            mx_stroke(c, x + i, y + i, w - 2 * i, h - 2 * i, color);
        return;
    }
    mx_fill(c, x + radius, y, w - 2 * radius, thickness, color);
    mx_fill(c, x + radius, y + h - thickness, w - 2 * radius, thickness, color);
    mx_fill(c, x, y + radius, thickness, h - 2 * radius, color);
    mx_fill(c, x + w - thickness, y + radius, thickness, h - 2 * radius, color);
    double outer = radius - 0.5;
    double inner = radius - thickness - 0.5;
    for (int row = 0; row < radius; row++) {
        for (int col = 0; col < radius; col++) {
            double dx = (radius - 0.5) - (col + 0.5);
            double dy = (radius - 0.5) - (row + 0.5);
            double d = mx_sqrt(dx * dx + dy * dy);
            unsigned cov_outer = edge_coverage(d, outer);
            unsigned cov_inner = inner > 0 ? edge_coverage(d, inner) : 0;
            unsigned cov = cov_outer > cov_inner ? cov_outer - cov_inner : 0;
            if (!cov) continue;
            plot(c, x + col, y + row, color, cov);
            plot(c, x + w - 1 - col, y + row, color, cov);
            plot(c, x + col, y + h - 1 - row, color, cov);
            plot(c, x + w - 1 - col, y + h - 1 - row, color, cov);
        }
    }
}

void mx_circle(const mx_canvas_t* c, int cx, int cy, int radius,
               uint32_t color) {
    mx_round_rect(c, cx - radius, cy - radius, radius * 2, radius * 2, radius,
                  color);
}

void mx_ring(const mx_canvas_t* c, int cx, int cy, int radius, int thickness,
             uint32_t color) {
    mx_round_rect_stroke(c, cx - radius, cy - radius, radius * 2, radius * 2,
                         radius, thickness, color);
}

void mx_shadow(const mx_canvas_t* c, int x, int y, int w, int h, int radius,
               int spread, uint32_t color) {
    unsigned alpha = color >> 24;
    for (int i = spread; i >= 1; i--) {
        unsigned a = alpha * (unsigned)(spread - i + 1) / (unsigned)(spread * 3);
        mx_round_rect(c, x - i, y - i + i / 2 + 1, w + 2 * i, h + 2 * i,
                      radius + i, mx_with_alpha(color | 0xFF000000u, a));
    }
}

void mx_icon(const mx_canvas_t* c, mx_icon_t icon, int size, int x, int y,
             uint32_t color) {
    const mx_icon_bitmap_t* best = NULL;
    for (unsigned i = 0; i < mx_icon_bitmap_count; i++) {
        const mx_icon_bitmap_t* b = &mx_icon_bitmaps[i];
        if (b->icon != icon) continue;
        if (!best) best = b;
        int db = b->size - size, dbest = best->size - size;
        if (db < 0) db = -db;
        if (dbest < 0) dbest = -dbest;
        if (db < dbest) best = b;
    }
    if (!best) return;
    int s = best->size;
    x += (size - s) / 2;
    y += (size - s) / 2;
    for (int row = 0; row < s; row++) {
        int py = y + row;
        if (py < c->clip_y0 || py >= c->clip_y1) continue;
        const unsigned char* src = best->data + row * s;
        for (int col = 0; col < s; col++) {
            if (!src[col]) continue;
            plot(c, x + col, py, color, src[col]);
        }
    }
}

void mx_text(const mx_canvas_t* c, int x, int y, const char* text,
             size_t length, uint32_t color, int px, int flags) {
    mx_text_draw_clipped(c->pixels, (uint32_t)c->width, (uint32_t)c->height,
                         c->clip_x0, c->clip_y0, c->clip_x1 - c->clip_x0,
                         c->clip_y1 - c->clip_y0, x, y, text, length, color, px,
                         flags);
}

void mx_text_ellipsis(const mx_canvas_t* c, int x, int y, int max_width,
                      const char* text, uint32_t color, int px, int flags) {
    if (!text || max_width <= 0) return;
    size_t length = strlen(text);
    if (mx_text_width_px(text, length, px, flags) <= max_width) {
        mx_text(c, x, y, text, length, color, px, flags);
        return;
    }
    static const char dots[] = "\xE2\x80\xA6";  /* U+2026 */
    int dots_width = mx_text_width_px(dots, 3, px, flags);
    char buffer[512];
    size_t fit = 0;
    {
        size_t limit = length < sizeof(buffer) - 4 ? length : sizeof(buffer) - 4;
        memcpy(buffer, text, limit);
        buffer[limit] = '\0';
        fit = mx_text_fit(buffer, px, flags, max_width - dots_width);
    }
    while (fit > 0 && buffer[fit - 1] == ' ') fit--;
    memcpy(buffer + fit, dots, 3);
    buffer[fit + 3] = '\0';
    mx_text(c, x, y, buffer, fit + 3, color, px, flags);
}
