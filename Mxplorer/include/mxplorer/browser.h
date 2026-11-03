#ifndef MXPLORER_BROWSER_H
#define MXPLORER_BROWSER_H

#include <stddef.h>
#include <stdint.h>

#define MX_BROWSER_MAX_TABS 12
#define MX_BROWSER_MAX_HISTORY 16
#define MX_BROWSER_URL_LENGTH 512

/* Chrome geometry (window client coordinates). */
#define MX_BROWSER_TAB_HEIGHT 40
#define MX_BROWSER_TAB_WIDTH 220
#define MX_BROWSER_TAB_MIN_WIDTH 96
#define MX_BROWSER_TOOLBAR_Y 40
#define MX_BROWSER_TOOLBAR_HEIGHT 48
#define MX_BROWSER_PAGE_Y 89
#define MX_BROWSER_DEVTOOLS_MIN 160
#define MX_BROWSER_DEVTOOLS_DEFAULT 280

typedef struct {
    char url[MX_BROWSER_URL_LENGTH];
    char title[96];
    char history[MX_BROWSER_MAX_HISTORY][MX_BROWSER_URL_LENGTH];
    char source_url[MX_BROWSER_URL_LENGTH];
    char* page_source;
    size_t page_source_length;
    int history_count;
    int history_index;
    int scroll_y;
    int error;            /* MX_PAGE_ERROR_* of the last load */
    char error_detail[160];
    unsigned refresh_count;
} mx_browser_tab_t;

enum {
    MX_PAGE_ERROR_NONE,
    MX_PAGE_ERROR_NETWORK,     /* could not connect / DNS / timeout */
    MX_PAGE_ERROR_HTTPS_OFF,   /* https:// while unsafe HTTPS is disabled */
    MX_PAGE_ERROR_CONTENT,     /* response could not be rendered */
    MX_PAGE_ERROR_OFFLINE      /* no network adapter */
};

typedef struct {
    int dark;
    uint32_t frame;         /* tab strip */
    uint32_t toolbar;       /* active tab and toolbar */
    uint32_t tab_hover;
    uint32_t field;         /* address bar */
    uint32_t field_focus;
    uint32_t surface;       /* cards, popups */
    uint32_t page;          /* internal page background */
    uint32_t border;
    uint32_t divider;
    uint32_t text;
    uint32_t muted;
    uint32_t subtle;
    uint32_t hover;         /* icon button hover */
    uint32_t pressed;
    uint32_t accent;
    uint32_t accent_text;
    uint32_t accent_soft;
    uint32_t danger;
    uint32_t danger_soft;
    uint32_t warning;
    uint32_t warning_soft;
    uint32_t success;
    /* source / DOM colouring */
    uint32_t code_tag;
    uint32_t code_attr;
    uint32_t code_value;
    uint32_t code_comment;
    uint32_t code_text;
} mx_browser_theme_t;

/* Builds the light or dark palette around the OS accent colour. */
void mx_theme_build(mx_browser_theme_t* theme, int dark, uint32_t accent);

#endif
