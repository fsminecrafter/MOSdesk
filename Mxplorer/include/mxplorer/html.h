#ifndef MXPLORER_HTML_H
#define MXPLORER_HTML_H

#include <stddef.h>
#include <stdint.h>

#define MX_HTML_SOURCE_MAX (96u * 1024u)
#define MX_HTML_VIEW_TOP 106u

typedef enum {
    MX_HTML_OK = 0,
    MX_HTML_EMPTY,
    MX_HTML_TOO_LARGE,
    MX_HTML_TOO_COMPLEX
} mx_html_result_t;

mx_html_result_t mx_html_parse(const char* source, size_t length,
                               uint32_t viewport_width);
void mx_html_draw(uint32_t* pixels, uint32_t surface_width,
                  uint32_t surface_height, int scroll_y);
void mx_html_scroll(int delta, uint32_t viewport_height);
int mx_html_scroll_y(void);
int mx_html_content_height(void);
const char* mx_html_error(mx_html_result_t result);

#endif
