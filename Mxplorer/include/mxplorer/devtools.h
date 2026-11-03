#ifndef MXPLORER_DEVTOOLS_H
#define MXPLORER_DEVTOOLS_H

#include <stddef.h>
#include "mxplorer/browser.h"
#include "mxplorer/dom.h"
#include "mxplorer/gfx.h"

/* Developer tools panel docked under the page. */

enum {
    MX_DEVTOOLS_CONSOLE,
    MX_DEVTOOLS_ELEMENTS,
    MX_DEVTOOLS_NETWORK,
    MX_DEVTOOLS_SOURCES,
    MX_DEVTOOLS_PERFORMANCE,
    MX_DEVTOOLS_TAB_COUNT
};

typedef struct {
    const mx_browser_theme_t* theme;
    const char* source;          /* page HTML */
    size_t source_length;
    const char* url;
    long last_frame_ms;
} mx_devtools_context_t;

void mx_devtools_reset_page(void);
void mx_devtools_set_tab(int tab);
int mx_devtools_tab(void);
int mx_devtools_height(void);
void mx_devtools_set_height(int height);

/* Draws the panel into the rectangle. */
void mx_devtools_draw(const mx_canvas_t* canvas, int x, int y, int w, int h,
                      const mx_devtools_context_t* context);

/* Input. Coordinates are window coordinates. These return 1 when the panel
 * needs to be redrawn (and the page, for element highlights). */
int mx_devtools_mouse(int x, int y, int left_down, int pressed, int released);
int mx_devtools_wheel(int x, int y, int delta);
int mx_devtools_key(int usb_key, char ascii, int mods);
int mx_devtools_focused(void);
void mx_devtools_focus_console(void);
void mx_devtools_blur(void);
/* 1 while dragging the panel's top edge. */
int mx_devtools_resizing(void);
/* Element picker: page clicks select elements while it is on. */
int mx_devtools_picking(void);
void mx_devtools_pick(mx_node_t* node, int commit);
/* Request from the panel to close it (its close button). */
int mx_devtools_take_close_request(void);

#endif
