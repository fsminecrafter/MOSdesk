#ifndef MXPLORER_CSS_H
#define MXPLORER_CSS_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t color;
    uint32_t background;
    int font_size;
    int bold;
    int text_align;
    int has_font_size;
    int has_font_weight;
    int margin_top;
    int margin_bottom;
    int padding;
    int display_none;
    int has_background;
} mx_css_style_t;

void mx_css_reset(void);
void mx_css_add_stylesheet(const char* source, size_t length);
void mx_css_compute(mx_css_style_t* result,
                    const mx_css_style_t* inherited,
                    const char* tag, const char* class_name,
                    const char* id, const char* inline_style);

#endif
