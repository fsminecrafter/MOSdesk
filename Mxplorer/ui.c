#include "mxplorer/ui.h"
#include "mxplorer/text.h"
#include "string.h"
#include "window.h"

#define TOP MX_BROWSER_PAGE_Y
#define BUTTON 36

/* ====================================================================== */
/* Theme                                                                  */
/* ====================================================================== */

void mx_theme_build(mx_browser_theme_t* t, int dark, uint32_t accent) {
    memset(t, 0, sizeof(*t));
    t->dark = dark;
    accent |= 0xFF000000u;
    if (!dark) {
        t->frame = 0xFFDFE4EBu;
        t->toolbar = 0xFFFFFFFFu;
        t->tab_hover = 0xFFEAEEF3u;
        t->field = 0xFFEEF1F5u;
        t->field_focus = 0xFFFFFFFFu;
        t->surface = 0xFFFFFFFFu;
        t->page = 0xFFF5F7FAu;
        t->border = 0xFFD5DBE3u;
        t->divider = 0xFFE4E8EEu;
        t->text = 0xFF1F2328u;
        t->muted = 0xFF5E6A78u;
        t->subtle = 0xFF8A94A0u;
        t->hover = 0xFFE9ECF0u;
        t->pressed = 0xFFDCE1E7u;
        t->accent = accent;
        t->accent_text = mx_is_dark_color(accent) ? 0xFFFFFFFFu : 0xFF101418u;
        t->accent_soft = mx_mix(0xFFFFFFFFu, accent, 38);
        t->danger = 0xFFD93025u;
        t->danger_soft = 0xFFFCE8E6u;
        t->warning = 0xFFB06000u;
        t->warning_soft = 0xFFFEF7E0u;
        t->success = 0xFF188038u;
        t->code_tag = 0xFF881280u;
        t->code_attr = 0xFF994500u;
        t->code_value = 0xFF1A1AA6u;
        t->code_comment = 0xFF6E7781u;
        t->code_text = 0xFF1F2328u;
    } else {
        t->frame = 0xFF18191Cu;
        t->toolbar = 0xFF27282Cu;
        t->tab_hover = 0xFF222327u;
        t->field = 0xFF1B1C1Fu;
        t->field_focus = 0xFF303136u;
        t->surface = 0xFF2C2D32u;
        t->page = 0xFF1F2023u;
        t->border = 0xFF3D3F45u;
        t->divider = 0xFF35373Cu;
        t->text = 0xFFE8EAEDu;
        t->muted = 0xFFA4AAB2u;
        t->subtle = 0xFF7D838Cu;
        t->hover = 0xFF393B40u;
        t->pressed = 0xFF45474Du;
        /* lift dark accents so they read on dark surfaces */
        t->accent = mx_is_dark_color(accent) ? mx_mix(accent, 0xFFFFFFFFu, 110) : accent;
        t->accent_text = mx_is_dark_color(t->accent) ? 0xFFFFFFFFu : 0xFF101418u;
        t->accent_soft = mx_mix(0xFF27282Cu, t->accent, 60);
        t->danger = 0xFFF28B82u;
        t->danger_soft = 0xFF3C2626u;
        t->warning = 0xFFFDD663u;
        t->warning_soft = 0xFF3A3221u;
        t->success = 0xFF81C995u;
        t->code_tag = 0xFF5DB0D7u;
        t->code_attr = 0xFF9BBBDCu;
        t->code_value = 0xFFF29766u;
        t->code_comment = 0xFF8B949Eu;
        t->code_text = 0xFFE8EAEDu;
    }
}

/* ====================================================================== */
/* Helpers                                                                */
/* ====================================================================== */

static mx_canvas_t canvas_of(const mx_ui_state_t* s) {
    return mx_canvas(s->pixels, (int)s->width, (int)s->height);
}

static int is_hover(const mx_ui_state_t* s, int kind, int index) {
    return s->hover_kind == kind && s->hover_index == index;
}

static void icon_button(const mx_canvas_t* c, const mx_ui_state_t* s, int x, int y,
                        mx_icon_t icon, int kind, int enabled, int active) {
    const mx_browser_theme_t* t = s->theme;
    int hovered = enabled && is_hover(s, kind, 0);
    if (active) mx_circle(c, x + BUTTON / 2, y + BUTTON / 2, BUTTON / 2, t->accent_soft);
    else if (hovered)
        mx_circle(c, x + BUTTON / 2, y + BUTTON / 2, BUTTON / 2,
                  s->pressed_kind == kind ? t->pressed : t->hover);
    uint32_t color = !enabled ? mx_with_alpha(t->muted, 110) : active ? t->accent : t->text;
    mx_icon(c, icon, 20, x + (BUTTON - 20) / 2, y + (BUTTON - 20) / 2, color);
}

static int text_w(const char* text, int px, int flags) {
    return mx_text_width_px(text, (size_t)-1, px, flags);
}

/* ====================================================================== */
/* Geometry                                                               */
/* ====================================================================== */

int mx_ui_tab_width(const mx_ui_state_t* s) {
    int avail = (int)s->width - 16 - 44;
    int w = s->tab_count ? avail / s->tab_count : MX_BROWSER_TAB_WIDTH;
    if (w > MX_BROWSER_TAB_WIDTH) w = MX_BROWSER_TAB_WIDTH;
    if (w < MX_BROWSER_TAB_MIN_WIDTH) w = MX_BROWSER_TAB_MIN_WIDTH;
    return w;
}

int mx_ui_visible_tabs(const mx_ui_state_t* s) {
    int avail = (int)s->width - 16 - 44;
    int visible = avail / mx_ui_tab_width(s);
    return visible < 1 ? 1 : visible;
}

static void ensure_active_visible(const mx_ui_state_t* s) {
    int visible = mx_ui_visible_tabs(s);
    if (s->active_tab < *s->tab_scroll) *s->tab_scroll = s->active_tab;
    if (s->active_tab >= *s->tab_scroll + visible) *s->tab_scroll = s->active_tab - visible + 1;
    if (*s->tab_scroll > s->tab_count - visible) *s->tab_scroll = s->tab_count - visible;
    if (*s->tab_scroll < 0) *s->tab_scroll = 0;
}

static int new_tab_x(const mx_ui_state_t* s) {
    int tw = mx_ui_tab_width(s);
    int shown = s->tab_count - *s->tab_scroll;
    int visible = mx_ui_visible_tabs(s);
    if (shown > visible) shown = visible;
    int x = 8 + shown * tw + 4;
    if (x > (int)s->width - 40) x = (int)s->width - 40;
    return x;
}

static void omnibox_rect(const mx_ui_state_t* s, int* x, int* y, int* w, int* h) {
    *x = 8 + 4 * BUTTON + 8;
    *y = MX_BROWSER_TOOLBAR_Y + 6;
    *h = 36;
    *w = (int)s->width - *x - (3 * BUTTON + 16);
    if (*w < 120) *w = 120;
}

static int right_buttons_x(const mx_ui_state_t* s) {
    return (int)s->width - (3 * BUTTON + 8);
}

static int scheme_kind(const char* url) {
    if (!strncmp(url, "https://", 8)) return 2;
    if (!strncmp(url, "http://", 7)) return 1;
    if (!strncmp(url, "mos://", 6)) return 3;
    return 0;
}

static int omnibox_text_x(const mx_ui_state_t* s, int box_x) {
    const char* url = s->tabs[s->active_tab].url;
    if (s->address_focused) return box_x + 40;
    if (scheme_kind(url) == 1) return box_x + 40 + text_w("Not secure", 12, MX_TEXT_MEDIUM) + 14;
    return box_x + 40;
}

void mx_ui_omnibox_text_rect(const mx_ui_state_t* s, int* x, int* width) {
    int bx, by, bw, bh;
    omnibox_rect(s, &bx, &by, &bw, &bh);
    *x = omnibox_text_x(s, bx);
    *width = bx + bw - 16 - *x;
}

int mx_ui_scrollbar(const mx_ui_state_t* s, int* x, int* y, int* w, int* h) {
    if (!s->page_loaded || s->content_height <= s->page_height || s->page_height <= 40) return 0;
    int track = s->page_height - 8;
    int thumb = (int)((long)track * s->page_height / s->content_height);
    if (thumb < 32) thumb = 32;
    int range = s->content_height - s->page_height;
    int pos = range > 0 ? (int)((long)(track - thumb) * s->scroll_y / range) : 0;
    *x = (int)s->width - 10;
    *y = TOP + 4 + pos;
    *w = 6;
    *h = thumb;
    return 1;
}

/* ====================================================================== */
/* Tab strip                                                              */
/* ====================================================================== */

static mx_icon_t tab_icon(const mx_browser_tab_t* tab) {
    if (!strcmp(tab->url, "mos://settings")) return MX_ICON_SETTINGS;
    if (!strcmp(tab->url, "mos://home")) return MX_ICON_HOME;
    if (tab->error) return MX_ICON_ERROR;
    return MX_ICON_PUBLIC;
}

static void draw_tabs(const mx_ui_state_t* s, const mx_canvas_t* c) {
    const mx_browser_theme_t* t = s->theme;
    mx_fill(c, 0, 0, (int)s->width, MX_BROWSER_TAB_HEIGHT, t->frame);
    ensure_active_visible(s);
    int tw = mx_ui_tab_width(s);
    int visible = mx_ui_visible_tabs(s);
    int x = 8;
    for (int i = *s->tab_scroll; i < s->tab_count && i < *s->tab_scroll + visible; i++, x += tw) {
        int active = i == s->active_tab;
        int hovered = is_hover(s, MX_HIT_TAB, i) || is_hover(s, MX_HIT_TAB_CLOSE, i);
        int ty = 6, th = MX_BROWSER_TAB_HEIGHT - ty;
        if (active) {
            mx_round_rect_top(c, x, ty, tw, th, 10, t->toolbar);
            /* soft "feet" joining the toolbar */
            mx_fill(c, x - 6, MX_BROWSER_TAB_HEIGHT - 6, 6, 6, t->toolbar);
            mx_fill(c, x + tw, MX_BROWSER_TAB_HEIGHT - 6, 6, 6, t->toolbar);
            mx_round_rect(c, x - 12, MX_BROWSER_TAB_HEIGHT - 12, 12, 12, 6, t->frame);
            mx_round_rect(c, x + tw, MX_BROWSER_TAB_HEIGHT - 12, 12, 12, 6, t->frame);
        } else if (hovered) {
            mx_round_rect(c, x + 2, ty + 2, tw - 4, th - 6, 8, t->tab_hover);
        } else if (i + 1 != s->active_tab && i + 1 < s->tab_count && i + 1 < *s->tab_scroll + visible &&
                   !is_hover(s, MX_HIT_TAB, i + 1) && !is_hover(s, MX_HIT_TAB_CLOSE, i + 1)) {
            mx_fill(c, x + tw - 1, ty + 9, 1, th - 18, t->border);
        }
        const mx_browser_tab_t* tab = &s->tabs[i];
        int cy = ty + th / 2;
        uint32_t icon_color = active ? (s->loading ? t->accent : t->text) : t->muted;
        mx_icon(c, s->loading && active ? MX_ICON_REFRESH : tab_icon(tab), 16, x + 12, cy - 8,
                icon_color);
        int close_x = x + tw - 30;
        int title_w = close_x - (x + 36) - 4;
        mx_canvas_t clip = mx_canvas_clip(c, x + 36, ty, title_w > 0 ? title_w : 0, th);
        mx_text_ellipsis(&clip, x + 36, cy - 8, title_w, tab->title[0] ? tab->title : "New Tab",
                         active ? t->text : t->muted, 12, active ? MX_TEXT_MEDIUM : 0);
        if (active || hovered || tw > 150) {
            int close_hover = is_hover(s, MX_HIT_TAB_CLOSE, i);
            if (close_hover) mx_circle(c, close_x + 10, cy, 10, t->pressed);
            mx_icon(c, MX_ICON_CLOSE, 16, close_x + 2, cy - 8, close_hover ? t->text : t->muted);
        }
    }
    int nx = new_tab_x(s);
    int ncy = 6 + (MX_BROWSER_TAB_HEIGHT - 6) / 2;
    if (is_hover(s, MX_HIT_NEW_TAB, 0)) mx_circle(c, nx + 14, ncy, 14, t->tab_hover);
    mx_icon(c, MX_ICON_ADD, 20, nx + 4, ncy - 10, t->text);
}

/* ====================================================================== */
/* Toolbar                                                                */
/* ====================================================================== */

static void draw_toolbar(const mx_ui_state_t* s, const mx_canvas_t* c) {
    const mx_browser_theme_t* t = s->theme;
    int y0 = MX_BROWSER_TOOLBAR_Y;
    mx_fill(c, 0, y0, (int)s->width, MX_BROWSER_TOOLBAR_HEIGHT, t->toolbar);
    const mx_browser_tab_t* tab = &s->tabs[s->active_tab];
    int by = y0 + (MX_BROWSER_TOOLBAR_HEIGHT - BUTTON) / 2;
    icon_button(c, s, 8, by, MX_ICON_ARROW_BACK, MX_HIT_BACK, tab->history_index > 0, 0);
    icon_button(c, s, 8 + BUTTON, by, MX_ICON_ARROW_FORWARD, MX_HIT_FORWARD,
                tab->history_index + 1 < tab->history_count, 0);
    icon_button(c, s, 8 + 2 * BUTTON, by, s->loading ? MX_ICON_CLOSE : MX_ICON_REFRESH,
                MX_HIT_RELOAD, 1, 0);
    icon_button(c, s, 8 + 3 * BUTTON, by, MX_ICON_HOME, MX_HIT_HOME, 1, 0);

    int bx, boy, bw, bh;
    omnibox_rect(s, &bx, &boy, &bw, &bh);
    int focused = s->address_focused;
    int hovered = is_hover(s, MX_HIT_OMNIBOX, 0);
    uint32_t field = focused ? t->field_focus : hovered ? mx_mix(t->field, t->hover, 140) : t->field;
    if (focused) {
        mx_shadow(c, bx, boy, bw, bh, bh / 2, 3, t->dark ? 0x60000000u : 0x30000000u);
        mx_round_rect(c, bx, boy, bw, bh, bh / 2, field);
        mx_round_rect_stroke(c, bx, boy, bw, bh, bh / 2, 2, t->accent);
    } else {
        mx_round_rect(c, bx, boy, bw, bh, bh / 2, field);
    }
    int cy = boy + bh / 2;
    const char* url = tab->url;
    int kind = scheme_kind(url);
    if (focused) {
        mx_icon(c, MX_ICON_SEARCH, 20, bx + 12, cy - 10, t->muted);
    } else if (kind == 2) {
        mx_icon(c, s->allow_insecure_tls ? MX_ICON_LOCK_OPEN : MX_ICON_LOCK, 20, bx + 12, cy - 10,
                t->warning);
    } else if (kind == 1) {
        mx_icon(c, MX_ICON_INFO, 20, bx + 12, cy - 10, t->muted);
        mx_text(c, bx + 38, cy - 8, "Not secure", (size_t)-1, t->muted, 12, MX_TEXT_MEDIUM);
        int sx = bx + 38 + text_w("Not secure", 12, MX_TEXT_MEDIUM) + 6;
        mx_fill(c, sx, cy - 8, 1, 16, t->border);
    } else if (kind == 3) {
        mx_icon(c, !strcmp(url, "mos://settings") ? MX_ICON_SETTINGS : MX_ICON_WEB, 20, bx + 12,
                cy - 10, t->accent);
    } else {
        mx_icon(c, MX_ICON_SEARCH, 20, bx + 12, cy - 10, t->muted);
    }
    int text_x = omnibox_text_x(s, bx);
    int text_w_avail = bx + bw - 16 - text_x;
    mx_canvas_t clip = mx_canvas_clip(c, text_x - 2, boy, text_w_avail + 4, bh);
    const char* visible = s->address_text + (focused ? s->address_scroll : 0);
    int ty = cy - mx_text_height_px(14, 0) / 2;
    if (!focused && !s->address_text[0]) {
        mx_text(&clip, text_x, ty, "Search Google or type a URL", (size_t)-1, t->muted, 14, 0);
    } else if (!focused) {
        /* the host stands out, the rest is muted */
        const char* host = strstr(visible, "://");
        const char* host_start = host ? host + 3 : visible;
        const char* host_end = host_start;
        while (*host_end && *host_end != '/' && *host_end != '?' && *host_end != '#') host_end++;
        int x = text_x;
        size_t prefix = (size_t)(host_start - visible);
        mx_text(&clip, x, ty, visible, prefix, t->muted, 14, 0);
        x += mx_text_width_px(visible, prefix, 14, 0);
        mx_text(&clip, x, ty, host_start, (size_t)(host_end - host_start), t->text, 14, 0);
        x += mx_text_width_px(host_start, (size_t)(host_end - host_start), 14, 0);
        mx_text(&clip, x, ty, host_end, (size_t)-1, t->muted, 14, 0);
    } else if (s->address_select_all && s->address_text[0]) {
        int sw = text_w(visible, 14, 0);
        if (sw > text_w_avail) sw = text_w_avail;
        mx_round_rect(&clip, text_x - 1, ty - 1, sw + 2, mx_text_height_px(14, 0) + 2, 3,
                      mx_with_alpha(t->accent, 90));
        mx_text(&clip, text_x, ty, visible, (size_t)-1, t->text, 14, 0);
    } else {
        mx_text(&clip, text_x, ty, visible, (size_t)-1, t->text, 14, 0);
        int count = s->address_cursor - s->address_scroll;
        if (count < 0) count = 0;
        int caret = text_x + mx_text_width_px(visible, (size_t)count, 14, 0);
        mx_fill(&clip, caret, ty - 1, 2, mx_text_height_px(14, 0) + 2, t->accent);
    }

    int rx = right_buttons_x(s);
    icon_button(c, s, rx, by, t->dark ? MX_ICON_LIGHT_MODE : MX_ICON_DARK_MODE, MX_HIT_THEME, 1, 0);
    icon_button(c, s, rx + BUTTON, by, MX_ICON_CODE, MX_HIT_DEVTOOLS, 1, s->devtools_open);
    icon_button(c, s, rx + 2 * BUTTON, by, MX_ICON_SETTINGS, MX_HIT_SETTINGS, 1,
                s->internal_page == 1);
    mx_fill(c, 0, TOP - 1, (int)s->width, 1, t->divider);
    if (s->loading) {
        int w = (int)s->width * (s->loading > 100 ? 100 : s->loading) / 100;
        mx_fill(c, 0, TOP - 2, w, 2, t->accent);
    }
}

void mx_ui_draw_chrome(const mx_ui_state_t* s) {
    if (!s || !s->pixels || !s->width || !s->height) return;
    mx_canvas_t c = canvas_of(s);
    mx_canvas_t top = mx_canvas_clip(&c, 0, 0, (int)s->width, TOP);
    draw_tabs(s, &top);
    draw_toolbar(s, &top);
    mos_window_present(s->window_id, s->pixels, s->width * 4u, 0, 0, s->width, TOP);
}

/* ====================================================================== */
/* Page overlays                                                          */
/* ====================================================================== */

void mx_ui_draw_page_overlay(const mx_ui_state_t* s) {
    const mx_browser_theme_t* t = s->theme;
    mx_canvas_t c = canvas_of(s);
    mx_canvas_t page = mx_canvas_clip(&c, 0, TOP, (int)s->width, s->page_height);
    int x, y, w, h;
    if (mx_ui_scrollbar(s, &x, &y, &w, &h)) {
        int hot = is_hover(s, MX_HIT_SCROLLBAR, 0) || s->pressed_kind == MX_HIT_SCROLLBAR;
        uint32_t color = mx_with_alpha(t->dark ? 0xFFFFFFFFu : 0xFF000000u, hot ? 120 : 70);
        mx_round_rect(&page, x - (hot ? 2 : 0), y, w + (hot ? 2 : 0), h, 4, color);
    }
    const char* text = s->hover_link;
    if (text && *text) {
        int px = 12;
        int max = (int)s->width * 6 / 10;
        int tw = text_w(text, px, 0);
        if (tw > max) tw = max;
        int bw = tw + 20, bh = 26;
        int bx = 6, by = TOP + s->page_height - bh - 6;
        mx_shadow(&page, bx, by, bw, bh, 8, 3, 0x30000000u);
        mx_round_rect(&page, bx, by, bw, bh, 8, t->surface);
        mx_round_rect_stroke(&page, bx, by, bw, bh, 8, 1, t->border);
        mx_canvas_t clip = mx_canvas_clip(&page, bx + 10, by, tw, bh);
        mx_text_ellipsis(&clip, bx + 10, by + (bh - mx_text_height_px(px, 0)) / 2, tw, text,
                         t->muted, px, 0);
    }
}

void mx_ui_draw_blank(const mx_ui_state_t* s) {
    const mx_browser_theme_t* t = s->theme;
    mx_canvas_t c = canvas_of(s);
    mx_canvas_t page = mx_canvas_clip(&c, 0, TOP, (int)s->width, s->page_height);
    mx_fill(&page, 0, TOP, (int)s->width, s->page_height, t->page);
    if (s->loading) {
        int cx = (int)s->width / 2, cy = TOP + s->page_height / 2;
        mx_icon(&page, MX_ICON_TRAVEL_EXPLORE, 48, cx - 24, cy - 60, t->accent);
        const char* text = s->status_message && *s->status_message ? s->status_message : "Loading...";
        int tw = text_w(text, 14, 0);
        mx_text(&page, cx - tw / 2, cy, text, (size_t)-1, t->muted, 14, 0);
    }
}

/* ====================================================================== */
/* Error page                                                             */
/* ====================================================================== */

static void error_buttons(const mx_ui_state_t* s, int* x0, int* y0, int* w0, int* x1, int* w1,
                          int* h, int* count) {
    int col_w = (int)s->width - 96;
    if (col_w > 560) col_w = 560;
    int col_x = ((int)s->width - col_w) / 2;
    int cy = TOP + s->page_height / 2;
    *h = 36;
    *y0 = cy + 96;
    *w0 = text_w("Try again", 14, MX_TEXT_MEDIUM) + 40;
    *x0 = col_x;
    const mx_browser_tab_t* tab = &s->tabs[s->active_tab];
    *count = tab->error == MX_PAGE_ERROR_HTTPS_OFF ? 2 : 1;
    *w1 = text_w("Open privacy settings", 14, MX_TEXT_MEDIUM) + 40;
    *x1 = *x0 + *w0 + 12;
}

void mx_ui_draw_error(const mx_ui_state_t* s) {
    const mx_browser_theme_t* t = s->theme;
    const mx_browser_tab_t* tab = &s->tabs[s->active_tab];
    mx_canvas_t c = canvas_of(s);
    mx_canvas_t page = mx_canvas_clip(&c, 0, TOP, (int)s->width, s->page_height);
    mx_fill(&page, 0, TOP, (int)s->width, s->page_height, t->page);
    int col_w = (int)s->width - 96;
    if (col_w > 560) col_w = 560;
    int col_x = ((int)s->width - col_w) / 2;
    int cy = TOP + s->page_height / 2;
    mx_icon_t icon = MX_ICON_ERROR;
    const char* title = "This page couldn't be shown";
    const char* hint = "Mxplorer received a response it could not display.";
    switch (tab->error) {
    case MX_PAGE_ERROR_NETWORK:
        icon = MX_ICON_CLOUD_OFF;
        title = "This site can't be reached";
        hint = "Check the address, or try again in a moment.";
        break;
    case MX_PAGE_ERROR_OFFLINE:
        icon = MX_ICON_WIFI_OFF;
        title = "You're offline";
        hint = "Connect a network adapter and run dhcp, then try again.";
        break;
    case MX_PAGE_ERROR_HTTPS_OFF:
        icon = MX_ICON_LOCK;
        title = "Secure sites are turned off";
        hint = "HTTPS needs \"Unsafe HTTPS\" in privacy settings. Certificates are not verified.";
        break;
    default:
        break;
    }
    mx_circle(&page, col_x + 36, cy - 110, 36, t->accent_soft);
    mx_icon(&page, icon, 48, col_x + 12, cy - 134, t->accent);
    mx_text(&page, col_x, cy - 52, title, (size_t)-1, t->text, 24, MX_TEXT_BOLD);
    mx_canvas_t clip = mx_canvas_clip(&page, col_x, 0, col_w, 100000);
    mx_text_ellipsis(&clip, col_x, cy - 10, col_w, hint, t->muted, 14, 0);
    mx_text_ellipsis(&clip, col_x, cy + 18, col_w, tab->url, t->text, 13, MX_TEXT_MONO);
    if (tab->error_detail[0])
        mx_text_ellipsis(&clip, col_x, cy + 44, col_w, tab->error_detail, t->subtle, 12, 0);
    int x0, y0, w0, x1, w1, h, count;
    error_buttons(s, &x0, &y0, &w0, &x1, &w1, &h, &count);
    uint32_t primary = is_hover(s, MX_HIT_ERROR_ACTION, 0) ? mx_mix(t->accent, t->text, 40) : t->accent;
    mx_round_rect(&page, x0, y0, w0, h, h / 2, primary);
    mx_text(&page, x0 + 20, y0 + (h - mx_text_height_px(14, MX_TEXT_MEDIUM)) / 2, "Try again",
            (size_t)-1, t->accent_text, 14, MX_TEXT_MEDIUM);
    if (count > 1) {
        if (is_hover(s, MX_HIT_ERROR_ACTION, 1)) mx_round_rect(&page, x1, y0, w1, h, h / 2, t->hover);
        mx_round_rect_stroke(&page, x1, y0, w1, h, h / 2, 1, t->border);
        mx_text(&page, x1 + 20, y0 + (h - mx_text_height_px(14, MX_TEXT_MEDIUM)) / 2,
                "Open privacy settings", (size_t)-1, t->accent, 14, MX_TEXT_MEDIUM);
    }
}

/* ====================================================================== */
/* Settings                                                               */
/* ====================================================================== */

enum { ROW_TOGGLE, ROW_RADIO, ROW_BUTTON, ROW_FIELD, ROW_INFO, ROW_CATEGORY, ROW_HEADER };

typedef struct {
    int id;
    int kind;
    int x, y, w, h;
    const char* title;
    const char* detail;
    int on;
    mx_icon_t icon;
} settings_row_t;

static const char* const category_names[MX_SETTINGS_CATEGORIES] = {
    "General", "Appearance", "Privacy and security", "Developer tools", "About Mxplorer"
};
static const mx_icon_t category_icons[MX_SETTINGS_CATEGORIES] = {
    MX_ICON_TUNE, MX_ICON_PALETTE, MX_ICON_SHIELD, MX_ICON_CODE, MX_ICON_INFO
};

#define SIDEBAR 248

static int settings_content(const mx_ui_state_t* s, int* x, int* w) {
    *x = SIDEBAR + 40;
    *w = (int)s->width - *x - 40;
    if (*w > 720) *w = 720;
    if (*w < 280) *w = 280;
    return *x;
}

/* Lays out the visible settings rows (both drawing and hit-testing). */
static int settings_layout(const mx_ui_state_t* s, settings_row_t* rows, int capacity) {
    int count = 0;
#define ROW(...) do { if (count < capacity) { settings_row_t r = {__VA_ARGS__}; rows[count++] = r; } } while (0)
    for (int i = 0; i < MX_SETTINGS_CATEGORIES; i++)
        ROW(MX_SET_CATEGORY + i, ROW_CATEGORY, 12, TOP + 84 + i * 48, SIDEBAR - 24, 44,
            category_names[i], NULL, s->settings_category == i, category_icons[i]);
    int cx, cw;
    settings_content(s, &cx, &cw);
    int y = TOP + 92;
    switch (s->settings_category) {
    case 0:
        ROW(-1, ROW_HEADER, cx, y, cw, 30, "Home page", "Opened by new tabs and the Home button.", 0, 0);
        y += 54;
        ROW(MX_SET_HOME_GOOGLE, ROW_RADIO, cx, y, cw, 56, "Google", "https://google.com",
            s->homepage_mode == 0, 0);
        y += 56;
        ROW(MX_SET_HOME_MXPLORER, ROW_RADIO, cx, y, cw, 56, "Mxplorer start page",
            "Built in, works offline", s->homepage_mode == 1, 0);
        y += 56;
        ROW(MX_SET_HOME_CUSTOM, ROW_RADIO, cx, y, cw, 56, "A specific page",
            "Type the address in the address bar, then press Enter", s->homepage_mode == 2, 0);
        y += 56;
        ROW(MX_SET_HOME_FIELD, ROW_FIELD, cx, y, cw, 56,
            s->homepage_editing ? "Editing in the address bar..." : "Custom address",
            s->custom_homepage && s->custom_homepage[0] ? s->custom_homepage : "Not set",
            s->homepage_editing, 0);
        break;
    case 1:
        ROW(-1, ROW_HEADER, cx, y, cw, 30, "Theme", "Mxplorer's colours. Dark mode also asks sites for their dark style.", 0, 0);
        y += 54;
        ROW(MX_SET_THEME_LIGHT, ROW_RADIO, cx, y, cw, 56, "Light", "Bright surfaces", !s->theme_dark, 0);
        y += 56;
        ROW(MX_SET_THEME_DARK, ROW_RADIO, cx, y, cw, 56, "Dark", "Easier on the eyes at night", s->theme_dark, 0);
        y += 72;
        ROW(-1, ROW_HEADER, cx, y, cw, 30, "Pages", NULL, 0, 0);
        y += 40;
        ROW(MX_SET_DARK_PAGES, ROW_TOGGLE, cx, y, cw, 64, "Dark pages",
            "Sites with a prefers-color-scheme style use it in dark mode", s->dark_pages, 0);
        y += 80;
        ROW(-1, ROW_INFO, cx, y, cw, 56, "Accent colour", "Follows the accent chosen in MOSdesk Settings", 0, 0);
        break;
    case 2:
        ROW(-1, ROW_HEADER, cx, y, cw, 30, "Site content", NULL, 0, 0);
        y += 40;
        ROW(MX_SET_JAVASCRIPT, ROW_TOGGLE, cx, y, cw, 64, "JavaScript",
            "Let sites run scripts. Takes effect on the next page load", s->javascript, 0);
        y += 64;
        ROW(MX_SET_SUBRESOURCES, ROW_TOGGLE, cx, y, cw, 64, "Load site files",
            "Fetch stylesheets, scripts and fetch() requests from pages", s->subresources, 0);
        y += 80;
        ROW(-1, ROW_HEADER, cx, y, cw, 30, "Security", NULL, 0, 0);
        y += 40;
        ROW(MX_SET_UNSAFE_HTTPS, ROW_TOGGLE, cx, y, cw, 64, "Unsafe HTTPS",
            "TLS 1.3 without certificate checks. Never enter passwords or private data",
            s->allow_insecure_tls, 0);
        y += 64;
        ROW(MX_SET_CLEAR_DATA, ROW_BUTTON, cx, y, cw, 64, "Clear browsing data",
            "Cookies, site storage and cached pages", 0, 0);
        break;
    case 3:
        ROW(-1, ROW_HEADER, cx, y, cw, 30, "Developer tools", "Inspect pages, run JavaScript and watch requests.", 0, 0);
        y += 54;
        ROW(MX_SET_DEVTOOLS, ROW_TOGGLE, cx, y, cw, 64, "Show developer tools",
            "Console, Elements, Network, Sources and Performance", s->devtools_open, 0);
        y += 80;
        ROW(-1, ROW_INFO, cx, y, cw, 40, "F12 or Ctrl+Shift+I", "Open or close the panel", 0, 0);
        y += 40;
        ROW(-1, ROW_INFO, cx, y, cw, 40, "Ctrl+Shift+J", "Jump to the console", 0, 0);
        y += 40;
        ROW(-1, ROW_INFO, cx, y, cw, 40, "Inspect button", "Pick an element on the page", 0, 0);
        break;
    default:
        ROW(-1, ROW_HEADER, cx, y, cw, 30, "Mxplorer 0.3", "A native web browser for Minimal-OS.", 0, 0);
        y += 54;
        ROW(-1, ROW_INFO, cx, y, cw, 56, "Engine", "HTML5 parsing, CSS selectors, flexbox, grid, tables, forms", 0, 0);
        y += 56;
        ROW(-1, ROW_INFO, cx, y, cw, 56, "JavaScript", "ES2020 interpreter with DOM, events, timers, fetch and modules", 0, 0);
        y += 56;
        ROW(-1, ROW_INFO, cx, y, cw, 56, "Not yet supported", "Images, video, canvas, WebSockets, certificate checks", 0, 0);
        y += 56;
        ROW(-1, ROW_INFO, cx, y, cw, 56, "Fonts and icons",
            "Inter and Roboto Mono (OFL), Material Symbols by Google (Apache 2.0)", 0, 0);
        break;
    }
#undef ROW
    return count;
}

static void draw_toggle(const mx_canvas_t* c, const mx_browser_theme_t* t, int x, int y, int on) {
    int w = 40, h = 22;
    mx_round_rect(c, x, y, w, h, h / 2, on ? t->accent : (t->dark ? 0xFF55585Eu : 0xFFB8C0CAu));
    int knob = h - 6;
    int kx = on ? x + w - knob - 3 : x + 3;
    mx_circle(c, kx + knob / 2, y + h / 2, knob / 2, 0xFFFFFFFFu);
}

static void draw_radio(const mx_canvas_t* c, const mx_browser_theme_t* t, int x, int y, int on) {
    int r = 10;
    mx_ring(c, x + r, y + r, r, 2, on ? t->accent : t->subtle);
    if (on) mx_circle(c, x + r, y + r, 5, t->accent);
}

void mx_ui_draw_settings(const mx_ui_state_t* s) {
    const mx_browser_theme_t* t = s->theme;
    mx_canvas_t c = canvas_of(s);
    mx_canvas_t page = mx_canvas_clip(&c, 0, TOP, (int)s->width, s->page_height);
    mx_fill(&page, 0, TOP, (int)s->width, s->page_height, t->page);
    mx_icon(&page, MX_ICON_SETTINGS, 20, 26, TOP + 30, t->accent);
    mx_text(&page, 54, TOP + 28, "Settings", (size_t)-1, t->text, 22, MX_TEXT_BOLD);
    settings_row_t rows[40];
    int count = settings_layout(s, rows, 40);
    int cx, cw;
    settings_content(s, &cx, &cw);
    mx_text(&page, cx, TOP + 28, category_names[s->settings_category], (size_t)-1, t->text, 22,
            MX_TEXT_BOLD);
    /* group consecutive control rows into cards */
    for (int i = 0; i < count; i++) {
        settings_row_t* r = &rows[i];
        if (r->kind == ROW_CATEGORY || r->kind == ROW_HEADER) continue;
        int start = i;
        while (i + 1 < count && rows[i + 1].kind != ROW_HEADER && rows[i + 1].kind != ROW_CATEGORY &&
               rows[i + 1].y == rows[i].y + rows[i].h)
            i++;
        int cy0 = rows[start].y, cy1 = rows[i].y + rows[i].h;
        mx_round_rect(&page, cx, cy0, cw, cy1 - cy0, 12, t->surface);
        mx_round_rect_stroke(&page, cx, cy0, cw, cy1 - cy0, 12, 1, t->divider);
        for (int k = start + 1; k <= i; k++) mx_fill(&page, cx + 16, rows[k].y, cw - 32, 1, t->divider);
    }
    for (int i = 0; i < count; i++) {
        settings_row_t* r = &rows[i];
        int hovered = r->id >= 0 && is_hover(s, MX_HIT_SETTING, r->id);
        switch (r->kind) {
        case ROW_CATEGORY: {
            if (r->on) mx_round_rect(&page, r->x, r->y, r->w, r->h, r->h / 2, t->accent_soft);
            else if (hovered) mx_round_rect(&page, r->x, r->y, r->w, r->h, r->h / 2, t->hover);
            mx_icon(&page, r->icon, 20, r->x + 16, r->y + (r->h - 20) / 2, r->on ? t->accent : t->muted);
            mx_text(&page, r->x + 50, r->y + (r->h - mx_text_height_px(14, 0)) / 2, r->title,
                    (size_t)-1, r->on ? t->text : t->muted, 14, r->on ? MX_TEXT_MEDIUM : 0);
            break;
        }
        case ROW_HEADER:
            mx_text(&page, r->x, r->y, r->title, (size_t)-1, t->text, 16, MX_TEXT_BOLD);
            if (r->detail) mx_text(&page, r->x, r->y + 24, r->detail, (size_t)-1, t->muted, 13, 0);
            break;
        default: {
            if (hovered && r->kind != ROW_INFO) {
                mx_canvas_t clip = mx_canvas_clip(&page, r->x + 1, r->y + 1, r->w - 2, r->h - 2);
                mx_fill(&clip, r->x, r->y, r->w, r->h, mx_with_alpha(t->hover, 200));
            }
            int text_x = r->x + 20;
            if (r->kind == ROW_RADIO) {
                draw_radio(&page, t, r->x + 18, r->y + (r->h - 20) / 2, r->on);
                text_x = r->x + 54;
            }
            if (r->kind == ROW_FIELD) text_x = r->x + 54;
            int right = r->x + r->w - 20;
            int title_y = r->y + (r->detail ? r->h / 2 - 18 : (r->h - 16) / 2);
            mx_canvas_t clip = mx_canvas_clip(&page, text_x, r->y, right - 60 - text_x, r->h);
            mx_text(&clip, text_x, title_y, r->title, (size_t)-1, t->text, 14, MX_TEXT_MEDIUM);
            if (r->detail)
                mx_text_ellipsis(&clip, text_x, title_y + 21, right - 60 - text_x, r->detail,
                                 r->kind == ROW_FIELD ? t->accent : t->muted, 12,
                                 r->kind == ROW_FIELD ? MX_TEXT_MONO : 0);
            if (r->kind == ROW_TOGGLE) draw_toggle(&page, t, right - 40, r->y + (r->h - 22) / 2, r->on);
            if (r->kind == ROW_BUTTON) {
                const char* label = "Clear";
                int bw = text_w(label, 13, MX_TEXT_MEDIUM) + 32;
                int bx = right - bw, by = r->y + (r->h - 32) / 2;
                mx_round_rect_stroke(&page, bx, by, bw, 32, 16, 1, t->border);
                mx_text(&page, bx + 16, by + (32 - mx_text_height_px(13, MX_TEXT_MEDIUM)) / 2, label,
                        (size_t)-1, t->accent, 13, MX_TEXT_MEDIUM);
            }
            if (r->kind == ROW_FIELD)
                mx_icon(&page, MX_ICON_LINK, 20, r->x + 18, r->y + (r->h - 20) / 2,
                        r->on ? t->accent : t->subtle);
            if (r->kind == ROW_INFO && !strcmp(r->title, "Accent colour")) {
                mx_circle(&page, right - 12, r->y + r->h / 2, 12, t->accent);
                mx_ring(&page, right - 12, r->y + r->h / 2, 12, 1, t->border);
            }
            break;
        }
        }
    }
}

/* ====================================================================== */
/* Hit testing                                                            */
/* ====================================================================== */

mx_hit_t mx_ui_hit(const mx_ui_state_t* s, int x, int y) {
    mx_hit_t hit = {MX_HIT_NONE, 0};
    if (y < MX_BROWSER_TAB_HEIGHT) {
        int nx = new_tab_x(s);
        if (x >= nx && x < nx + 28 && y >= 6) {
            hit.kind = MX_HIT_NEW_TAB;
            return hit;
        }
        int tw = mx_ui_tab_width(s);
        int index = x < 8 ? -1 : (x - 8) / tw + *s->tab_scroll;
        if (index >= *s->tab_scroll && index < s->tab_count &&
            index < *s->tab_scroll + mx_ui_visible_tabs(s) && y >= 6) {
            int tx = 8 + (index - *s->tab_scroll) * tw;
            hit.index = index;
            hit.kind = x >= tx + tw - 32 && x < tx + tw - 6 ? MX_HIT_TAB_CLOSE : MX_HIT_TAB;
        }
        return hit;
    }
    if (y < TOP) {
        int by = MX_BROWSER_TOOLBAR_Y + (MX_BROWSER_TOOLBAR_HEIGHT - BUTTON) / 2;
        if (y >= by && y < by + BUTTON) {
            if (x >= 8 && x < 8 + BUTTON) hit.kind = MX_HIT_BACK;
            else if (x >= 8 + BUTTON && x < 8 + 2 * BUTTON) hit.kind = MX_HIT_FORWARD;
            else if (x >= 8 + 2 * BUTTON && x < 8 + 3 * BUTTON) hit.kind = MX_HIT_RELOAD;
            else if (x >= 8 + 3 * BUTTON && x < 8 + 4 * BUTTON) hit.kind = MX_HIT_HOME;
            int rx = right_buttons_x(s);
            if (x >= rx && x < rx + BUTTON) hit.kind = MX_HIT_THEME;
            else if (x >= rx + BUTTON && x < rx + 2 * BUTTON) hit.kind = MX_HIT_DEVTOOLS;
            else if (x >= rx + 2 * BUTTON && x < rx + 3 * BUTTON) hit.kind = MX_HIT_SETTINGS;
        }
        int bx, boy, bw, bh;
        omnibox_rect(s, &bx, &boy, &bw, &bh);
        if (x >= bx && x < bx + bw && y >= boy && y < boy + bh) hit.kind = MX_HIT_OMNIBOX;
        return hit;
    }
    if (y >= TOP + s->page_height) return hit;
    int sx, sy, sw, sh;
    if (mx_ui_scrollbar(s, &sx, &sy, &sw, &sh) && x >= sx - 4 && x < (int)s->width) {
        hit.kind = MX_HIT_SCROLLBAR;
        return hit;
    }
    if (s->internal_page == 1) {
        settings_row_t rows[40];
        int count = settings_layout(s, rows, 40);
        for (int i = 0; i < count; i++) {
            settings_row_t* r = &rows[i];
            if (r->id < 0 || r->kind == ROW_HEADER) continue;
            if (x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h) {
                hit.kind = MX_HIT_SETTING;
                hit.index = r->id;
                return hit;
            }
        }
        return hit;
    }
    if (s->internal_page == 2) {
        int x0, y0, w0, x1, w1, h, count;
        error_buttons(s, &x0, &y0, &w0, &x1, &w1, &h, &count);
        if (y >= y0 && y < y0 + h) {
            if (x >= x0 && x < x0 + w0) {
                hit.kind = MX_HIT_ERROR_ACTION;
                hit.index = 0;
            } else if (count > 1 && x >= x1 && x < x1 + w1) {
                hit.kind = MX_HIT_ERROR_ACTION;
                hit.index = 1;
            }
        }
        return hit;
    }
    hit.kind = MX_HIT_PAGE;
    return hit;
}
