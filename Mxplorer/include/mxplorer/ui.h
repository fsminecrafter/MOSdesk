#ifndef MXPLORER_UI_H
#define MXPLORER_UI_H

#include "mxplorer/browser.h"
#include "mxplorer/gfx.h"

/* What is under the pointer in the browser chrome or an internal page. */
typedef enum {
    MX_HIT_NONE,
    MX_HIT_TAB,
    MX_HIT_TAB_CLOSE,
    MX_HIT_NEW_TAB,
    MX_HIT_BACK,
    MX_HIT_FORWARD,
    MX_HIT_RELOAD,
    MX_HIT_HOME,
    MX_HIT_OMNIBOX,
    MX_HIT_THEME,
    MX_HIT_DEVTOOLS,
    MX_HIT_SETTINGS,
    MX_HIT_PAGE,
    MX_HIT_SETTING,         /* index: MX_SET_* */
    MX_HIT_ERROR_ACTION,    /* index: 0 retry, 1 open settings */
    MX_HIT_SCROLLBAR
} mx_hit_kind_t;

typedef struct {
    int kind;
    int index;
} mx_hit_t;

enum {
    MX_SET_CATEGORY = 0,      /* + category index (0..4) */
    MX_SET_HOME_GOOGLE = 10,
    MX_SET_HOME_MXPLORER,
    MX_SET_HOME_CUSTOM,
    MX_SET_HOME_FIELD,
    MX_SET_THEME_LIGHT,
    MX_SET_THEME_DARK,
    MX_SET_DARK_PAGES,
    MX_SET_JAVASCRIPT,
    MX_SET_SUBRESOURCES,
    MX_SET_UNSAFE_HTTPS,
    MX_SET_CLEAR_DATA,
    MX_SET_DEVTOOLS,
    MX_SET_COUNT
};

#define MX_SETTINGS_CATEGORIES 5

typedef struct {
    uint32_t window_id;
    uint32_t* pixels;
    uint32_t width;
    uint32_t height;
    mx_browser_tab_t* tabs;
    int tab_count;
    int active_tab;
    int* tab_scroll;
    /* address bar */
    int address_focused;
    int address_select_all;
    int address_cursor;
    int address_scroll;
    const char* address_text;
    /* state */
    int loading;
    int page_loaded;
    int internal_page;        /* 1 settings */
    const char* status_message;
    const char* hover_link;
    int hover_kind;
    int hover_index;
    int pressed_kind;
    int page_height;          /* viewport under the chrome, above devtools */
    int content_height;
    int scroll_y;
    /* settings */
    int settings_category;
    int theme_dark;
    int dark_pages;
    int javascript;
    int subresources;
    int allow_insecure_tls;
    int homepage_mode;
    int homepage_editing;
    const char* custom_homepage;
    int devtools_open;
    const mx_browser_theme_t* theme;
} mx_ui_state_t;

/* Tab strip and toolbar (rows 0 .. MX_BROWSER_PAGE_Y). */
void mx_ui_draw_chrome(const mx_ui_state_t* state);
/* Internal pages, error pages and overlays in the page area. The web page
 * itself is drawn by the caller before this when state->page_loaded. */
void mx_ui_draw_page_overlay(const mx_ui_state_t* state);
void mx_ui_draw_settings(const mx_ui_state_t* state);
void mx_ui_draw_error(const mx_ui_state_t* state);
void mx_ui_draw_blank(const mx_ui_state_t* state);

mx_hit_t mx_ui_hit(const mx_ui_state_t* state, int x, int y);
/* Address bar text area, for caret placement. */
void mx_ui_omnibox_text_rect(const mx_ui_state_t* state, int* x, int* width);
int mx_ui_visible_tabs(const mx_ui_state_t* state);
int mx_ui_tab_width(const mx_ui_state_t* state);
/* Scrollbar thumb in window coordinates; returns 0 when not shown. */
int mx_ui_scrollbar(const mx_ui_state_t* state, int* x, int* y, int* w, int* h);

#endif
