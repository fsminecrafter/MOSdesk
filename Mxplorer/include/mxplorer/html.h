#ifndef MXPLORER_HTML_H
#define MXPLORER_HTML_H

#include <stddef.h>
#include <stdint.h>
#include "mxplorer/dom.h"
#include "mxplorer/gfx.h"

#define MX_HTML_SOURCE_MAX (2u * 1024u * 1024u)
#define MX_HTML_VIEW_TOP 92u

typedef enum {
    MX_HTML_OK = 0,
    MX_HTML_EMPTY,
    MX_HTML_TOO_LARGE,
    MX_HTML_TOO_COMPLEX
} mx_html_result_t;

/* Fetches a subresource. On success *body is malloc()ed and owned by the
 * caller. */
typedef int (*mx_fetch_fn)(void* user, const char* url, const char* kind,
                           char** body, size_t* length);

typedef struct {
    const char* url;
    int viewport_width;
    int viewport_height;
    int dark;                 /* prefers-color-scheme: dark */
    int scripts;              /* run JavaScript */
    int subresources;         /* fetch external CSS and scripts */
    mx_fetch_fn fetch;
    void* fetch_user;
    void (*navigate)(const char* url, void* user);
    void (*alert)(const char* text, void* user);
    void* host_user;
} mx_html_options_t;

typedef struct {
    long parse_ms;
    long style_ms;
    long layout_ms;
    long script_ms;
    long fetch_ms;
    unsigned elements;
    unsigned items;
    unsigned rules;
    unsigned scripts;
    unsigned stylesheets;
    size_t dom_bytes;
    size_t js_bytes;
    unsigned layouts;
} mx_html_stats_t;

mx_html_result_t mx_html_load(const char* source, size_t length,
                              const mx_html_options_t* options);
void mx_html_unload(void);
int mx_html_loaded(void);
/* Recomputes styles and layout (viewport or theme change). */
void mx_html_relayout(int width, int height, int dark);
/* Lays out again if scripts changed the document. Returns 1 if it did. */
int mx_html_update(void);
void mx_html_draw(const mx_canvas_t* canvas, int top, int height,
                  int scroll_y);

void mx_html_scroll(int delta, uint32_t viewport_height);
void mx_html_set_scroll(int y, uint32_t viewport_height);
int mx_html_scroll_y(void);
int mx_html_content_height(void);
uint32_t mx_html_background(void);
const char* mx_html_title(void);
mx_document_t* mx_html_document(void);
const mx_html_stats_t* mx_html_stats(void);
const char* mx_html_error(mx_html_result_t result);

/* Interaction. Coordinates are page coordinates (scroll applied). */
mx_node_t* mx_html_hit(int x, int y);
/* Resolved href of the link under the point, or NULL. */
const char* mx_html_link_at(int x, int y);
int mx_html_pointer_at(int x, int y);
/* Handles a click. Returns 1 and fills `navigate` when the page asks to
 * load another URL. */
int mx_html_click(int x, int y, char* navigate, size_t capacity);
int mx_html_has_focus(void);
void mx_html_blur(void);
/* Text input to the focused form control. Returns 1 when consumed;
 * `navigate` is filled when Enter submits a form. */
int mx_html_key(int usb_key, char ascii, int mods, char* navigate,
                size_t capacity);
/* Resolves a URL against the document base. */
void mx_html_resolve(const char* reference, char* out, size_t capacity);
/* Devtools element highlight (NULL clears). */
void mx_html_set_highlight(mx_node_t* node);
void mx_html_viewport(int* width, int* height);
/* Ensures layout is current (JS geometry queries). */
void mx_html_ensure_layout(void);
/* Runs due JavaScript timers. Returns 1 when the page needs a redraw. */
int mx_html_tick(long now_ms);

#endif
