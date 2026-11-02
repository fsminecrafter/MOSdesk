#ifndef MXPLORER_BROWSER_H
#define MXPLORER_BROWSER_H

#include <stddef.h>
#include <stdint.h>

#define MX_BROWSER_MAX_TABS 12
#define MX_BROWSER_MAX_HISTORY 16
#define MX_BROWSER_URL_LENGTH 256
#define MX_BROWSER_TAB_WIDTH 196
#define MX_BROWSER_TOOLBAR_Y 52
#define MX_BROWSER_TOOLBAR_HEIGHT 42
#define MX_BROWSER_PAGE_Y 106
#define MX_BROWSER_DEBUG_PANEL_HEIGHT 220

typedef struct {
    char url[MX_BROWSER_URL_LENGTH];
    char title[48];
    char history[MX_BROWSER_MAX_HISTORY][MX_BROWSER_URL_LENGTH];
    char source_url[MX_BROWSER_URL_LENGTH];
    char* page_source;
    size_t page_source_length;
    int history_count;
    int history_index;
    unsigned refresh_count;
} mx_browser_tab_t;

typedef struct {
    uint32_t background;
    uint32_t surface;
    uint32_t border;
    uint32_t text;
    uint32_t muted;
    uint32_t button;
    uint32_t button_hover;
    uint32_t accent;
    uint32_t accent_text;
} mx_browser_theme_t;

#endif
