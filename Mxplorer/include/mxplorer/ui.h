#ifndef MXPLORER_UI_H
#define MXPLORER_UI_H

#include "mxplorer/browser.h"

typedef struct {
    uint32_t window_id;
    uint32_t* pixels;
    uint32_t width;
    uint32_t height;
    mx_browser_tab_t* tabs;
    int tab_count;
    int active_tab;
    int* tab_scroll;
    int address_focused;
    int address_select_all;
    int address_cursor;
    int address_scroll;
    int settings_category;
    int page_loaded;
    int allow_insecure_tls;
    int homepage_mode;
    int homepage_editing;
    const char* custom_homepage;
    int debug_open;
    int debug_tab;
    const char (*debug_logs)[80];
    int debug_log_count;
    const char* debug_source;
    size_t debug_source_length;
    const char* address_text;
    const char* status_message;
    const mx_browser_theme_t* theme;
} mx_ui_state_t;

void mx_ui_draw_chrome(const mx_ui_state_t* state);
void mx_ui_draw_content(const mx_ui_state_t* state);

#endif
