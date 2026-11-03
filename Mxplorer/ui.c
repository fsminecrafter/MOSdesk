#include "mxplorer/html.h"
#include "mxplorer/ui.h"
#include "mxplorer/text.h"
#include "string.h"
#include "window.h"

static const char* const category_names[] = {
    "General", "Appearance", "Privacy", "About"
};
static const char* const category_descriptions[] = {
    "Choose what Mxplorer opens and how tabs behave.",
    "Customize the look of Mxplorer.",
    "Control how Mxplorer handles browsing data.",
    "Browser framework information."
};

static void fill_rect(const mx_ui_state_t* state, int x, int y, int width, int height,
                      uint32_t color) {
    if (!state->pixels || width <= 0 || height <= 0) return;
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + width > (int)state->width ? (int)state->width : x + width;
    int y1 = y + height > (int)state->height ? (int)state->height : y + height;
    for (int row = y0; row < y1; row++) {
        uint32_t* line = state->pixels + (size_t)row * state->width;
        for (int col = x0; col < x1; col++) line[col] = color;
    }
}

static void stroke_rect(const mx_ui_state_t* state, int x, int y, int width,
                        int height, uint32_t color) {
    fill_rect(state, x, y, width, 1, color);
    fill_rect(state, x, y + height - 1, width, 1, color);
    fill_rect(state, x, y, 1, height, color);
    fill_rect(state, x + width - 1, y, 1, height, color);
}

static void draw_line(const mx_ui_state_t* state, int x0, int y0, int x1, int y1,
                      uint32_t color) {
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0;
    int sy = y0 < y1 ? 1 : -1;
    int error = dx + dy;
    for (;;) {
        if (x0 >= 0 && y0 >= 0 &&
            x0 < (int)state->width && y0 < (int)state->height)
            state->pixels[(size_t)y0 * state->width + (uint32_t)x0] = color;
        if (x0 == x1 && y0 == y1) break;
        int twice_error = 2 * error;
        if (twice_error >= dy) { error += dy; x0 += sx; }
        if (twice_error <= dx) { error += dx; y0 += sy; }
    }
}

static void draw_label(const mx_ui_state_t* state, int x, int y, const char* text,
                       uint32_t color, int size) {
    mx_text_draw(state->pixels, state->width, state->height, x, y, text, color,
                 size);
}

static void draw_centered_label(const mx_ui_state_t* state, int center_x, int y,
                                const char* text, uint32_t color, int size) {
    draw_label(state, center_x - mx_text_width(text, size) / 2, y, text, color,
               size);
}

static void draw_truncated(const mx_ui_state_t* state, int x, int y, int max_width,
                           const char* text, uint32_t color) {
    char clipped[80];
    size_t length = 0;
    while (text && text[length] && length + 1 < sizeof(clipped)) {
        clipped[length] = text[length];
        clipped[length + 1] = '\0';
        if (mx_text_width(clipped, 1) > max_width) {
            clipped[length] = '\0';
            break;
        }
        length++;
    }
    clipped[length] = '\0';
    draw_label(state, x, y, clipped, color, 1);
}

static void draw_button(const mx_ui_state_t* state, int x, int y, int width,
                        int height, const char* label) {
    const mx_browser_theme_t* theme = state->theme;
    fill_rect(state, x, y, width, height, theme->button);
    stroke_rect(state, x, y, width, height, theme->border);
    int label_width = mx_text_width(label, 1);
    draw_label(state, x + (width - label_width) / 2, y + (height - 13) / 2,
               label, theme->text, 1);
}

static void ensure_active_visible(const mx_ui_state_t* state) {
    int visible = ((int)state->width - 64) / MX_BROWSER_TAB_WIDTH;
    if (visible < 1) visible = 1;
    if (state->active_tab < *state->tab_scroll)
        *state->tab_scroll = state->active_tab;
    if (state->active_tab >= *state->tab_scroll + visible)
        *state->tab_scroll = state->active_tab - visible + 1;
    if (*state->tab_scroll < 0) *state->tab_scroll = 0;
}

static void draw_tabs(const mx_ui_state_t* state) {
    const mx_browser_theme_t* theme = state->theme;
    fill_rect(state, 0, 0, (int)state->width, 44, theme->background);
    int x = 8;
    int available = (int)state->width - 56;
    for (int i = *state->tab_scroll;
         i < state->tab_count && x + MX_BROWSER_TAB_WIDTH <= available; i++) {
        int selected = i == state->active_tab;
        fill_rect(state, x, 5, MX_BROWSER_TAB_WIDTH - 4, 34,
                  selected ? theme->surface : theme->background);
        stroke_rect(state, x, 5, MX_BROWSER_TAB_WIDTH - 4, 34,
                    selected ? theme->border : theme->background);
        if (selected) fill_rect(state, x + 1, 5, MX_BROWSER_TAB_WIDTH - 6, 3,
                                theme->accent);
        fill_rect(state, x + 12, 17, 8, 8,
                  selected ? theme->accent : theme->muted);
        draw_truncated(state, x + 30, 16, MX_BROWSER_TAB_WIDTH - 78,
                       state->tabs[i].title,
                       selected ? theme->text : theme->muted);
        draw_label(state, x + MX_BROWSER_TAB_WIDTH - 28, 15, "x",
                   theme->muted, 1);
        x += MX_BROWSER_TAB_WIDTH;
    }
    draw_button(state, (int)state->width - 42, 8, 30, 28, "+");
}

static void draw_navigation_icons(const mx_ui_state_t* state) {
    const mx_browser_theme_t* theme = state->theme;
    const mx_browser_tab_t* tab = &state->tabs[state->active_tab];
    const int cy = MX_BROWSER_TOOLBAR_Y + MX_BROWSER_TOOLBAR_HEIGHT / 2;
    uint32_t back_color = tab->history_index > 0 ? theme->text : theme->muted;
    uint32_t forward_color = tab->history_index + 1 < tab->history_count
                                 ? theme->text : theme->muted;
    draw_line(state, 31, cy, 15, cy, back_color);
    draw_line(state, 15, cy, 23, cy - 7, back_color);
    draw_line(state, 15, cy, 23, cy + 7, back_color);
    draw_line(state, 60, cy, 76, cy, forward_color);
    draw_line(state, 76, cy, 68, cy - 7, forward_color);
    draw_line(state, 76, cy, 68, cy + 7, forward_color);

    draw_line(state, 101, cy - 2, 103, cy - 7, theme->text);
    draw_line(state, 103, cy - 7, 108, cy - 10, theme->text);
    draw_line(state, 108, cy - 10, 114, cy - 9, theme->text);
    draw_line(state, 114, cy - 9, 119, cy - 5, theme->text);
    draw_line(state, 119, cy - 5, 121, cy, theme->text);
    draw_line(state, 121, cy, 116, cy - 1, theme->text);
    draw_line(state, 121, cy, 120, cy - 5, theme->text);
    draw_line(state, 119, cy + 2, 117, cy + 7, theme->text);
    draw_line(state, 117, cy + 7, 112, cy + 10, theme->text);
    draw_line(state, 112, cy + 10, 106, cy + 9, theme->text);
    draw_line(state, 106, cy + 9, 101, cy + 5, theme->text);
    draw_line(state, 101, cy + 5, 99, cy, theme->text);
    draw_line(state, 99, cy, 104, cy + 1, theme->text);
    draw_line(state, 99, cy, 100, cy + 5, theme->text);

    draw_line(state, 136, cy - 1, 145, cy - 9, theme->text);
    draw_line(state, 145, cy - 9, 154, cy - 1, theme->text);
    draw_line(state, 139, cy - 3, 139, cy + 8, theme->text);
    draw_line(state, 151, cy - 3, 151, cy + 8, theme->text);
    draw_line(state, 139, cy + 8, 151, cy + 8, theme->text);
    draw_line(state, 143, cy + 8, 143, cy + 2, theme->text);
    draw_line(state, 147, cy + 2, 147, cy + 8, theme->text);
}

static void draw_toolbar(const mx_ui_state_t* state) {
    const mx_browser_theme_t* theme = state->theme;
    const int cy = MX_BROWSER_TOOLBAR_Y + MX_BROWSER_TOOLBAR_HEIGHT / 2;
    fill_rect(state, 0, 44, (int)state->width, 56, theme->background);
    draw_navigation_icons(state);
    int address_x = 164;
    int settings_x = (int)state->width - 86;
    int address_width = settings_x - address_x - 12;
    fill_rect(state, address_x, MX_BROWSER_TOOLBAR_Y, address_width,
              MX_BROWSER_TOOLBAR_HEIGHT, theme->surface);
    stroke_rect(state, address_x, MX_BROWSER_TOOLBAR_Y, address_width,
                MX_BROWSER_TOOLBAR_HEIGHT,
                state->address_focused ? theme->accent : theme->border);
    draw_line(state, 183, cy - 7, 187, cy - 5, theme->muted);
    draw_line(state, 187, cy - 5, 189, cy, theme->muted);
    draw_line(state, 189, cy, 187, cy + 5, theme->muted);
    draw_line(state, 187, cy + 5, 183, cy + 7, theme->muted);
    draw_line(state, 183, cy + 7, 179, cy + 5, theme->muted);
    draw_line(state, 179, cy + 5, 177, cy, theme->muted);
    draw_line(state, 177, cy, 179, cy - 5, theme->muted);
    draw_line(state, 179, cy - 5, 183, cy - 7, theme->muted);
    draw_line(state, 183, cy - 7, 183, cy + 7, theme->muted);
    draw_line(state, 177, cy, 189, cy, theme->muted);
    draw_line(state, 179, cy - 4, 187, cy - 4, theme->muted);
    draw_line(state, 179, cy + 4, 187, cy + 4, theme->muted);
    const char* visible_address = state->address_text + state->address_scroll;
    int text_x = address_x + 36;
    int text_width_available = address_width - 48;
    if (!state->address_focused && !state->address_text[0])
        draw_label(state, text_x, MX_BROWSER_TOOLBAR_Y + 14,
                   "Search or enter address", theme->muted, 1);
    else
        draw_truncated(state, text_x, MX_BROWSER_TOOLBAR_Y + 14,
                       text_width_available, visible_address, theme->text);
    if (state->address_focused && state->address_select_all) {
        int selection_width = mx_text_width(visible_address, 1);
        if (selection_width > text_width_available)
            selection_width = text_width_available;
        fill_rect(state, text_x - 2, MX_BROWSER_TOOLBAR_Y + 12,
                  selection_width + 2, 18, theme->accent);
        draw_truncated(state, text_x, MX_BROWSER_TOOLBAR_Y + 14,
                       text_width_available, visible_address,
                       theme->accent_text);
    } else if (state->address_focused) {
        char caret_prefix[MX_BROWSER_URL_LENGTH];
        int count = state->address_cursor - state->address_scroll;
        if (count < 0) count = 0;
        if (count >= (int)sizeof(caret_prefix)) count = sizeof(caret_prefix) - 1;
        memcpy(caret_prefix, visible_address, (size_t)count);
        caret_prefix[count] = '\0';
        int caret_x = text_x + mx_text_width(caret_prefix, 1);
        if (caret_x <= text_x + text_width_available)
            fill_rect(state, caret_x, MX_BROWSER_TOOLBAR_Y + 12, 1, 18,
                      theme->accent);
    }
    draw_button(state, settings_x, MX_BROWSER_TOOLBAR_Y, 74,
                MX_BROWSER_TOOLBAR_HEIGHT, "Settings");
    fill_rect(state, 0, 100, (int)state->width, 1, theme->border);
}

void mx_ui_draw_chrome(const mx_ui_state_t* state) {
    if (!state || !state->pixels || !state->width || !state->height) return;
    fill_rect(state, 0, 0, (int)state->width, (int)MX_HTML_VIEW_TOP,
              state->theme->background);
    ensure_active_visible(state);
    draw_tabs(state);
    draw_toolbar(state);
    mos_window_present(state->window_id, state->pixels, state->width * 4u,
                       0, 0, state->width, MX_HTML_VIEW_TOP);
}

static int ui_is_settings_page(const mx_ui_state_t* state) {
    return strcmp(state->tabs[state->active_tab].url, "mos://settings") == 0;
}

static void draw_circle_outline(const mx_ui_state_t* state, int center_x,
                                int center_y, int radius, uint32_t color) {
    int x = radius;
    int y = 0;
    int error = 1 - radius;
    while (x >= y) {
        fill_rect(state, center_x + x, center_y + y, 1, 1, color);
        fill_rect(state, center_x + y, center_y + x, 1, 1, color);
        fill_rect(state, center_x - y, center_y + x, 1, 1, color);
        fill_rect(state, center_x - x, center_y + y, 1, 1, color);
        fill_rect(state, center_x - x, center_y - y, 1, 1, color);
        fill_rect(state, center_x - y, center_y - x, 1, 1, color);
        fill_rect(state, center_x + y, center_y - x, 1, 1, color);
        fill_rect(state, center_x + x, center_y - y, 1, 1, color);
        y++;
        if (error < 0) error += 2 * y + 1;
        else {
            x--;
            error += 2 * (y - x + 1);
        }
    }
}

static void draw_globe(const mx_ui_state_t* state, int center_x, int center_y) {
    const uint32_t accent = state->theme->accent;
    draw_circle_outline(state, center_x, center_y, 18, accent);
    draw_line(state, center_x - 17, center_y, center_x + 17, center_y, accent);
    draw_line(state, center_x, center_y - 17, center_x, center_y + 17, accent);
    draw_line(state, center_x - 13, center_y - 10, center_x + 13, center_y - 10,
              accent);
    draw_line(state, center_x - 13, center_y + 10, center_x + 13, center_y + 10,
              accent);
}

static void draw_external_page(const mx_ui_state_t* state) {
    const mx_browser_theme_t* theme = state->theme;
    int top = MX_BROWSER_PAGE_Y;
    fill_rect(state, 0, top, (int)state->width,
              (int)state->height - top, theme->surface);
    if (state->page_loaded) {
        mx_html_draw(state->pixels, state->width, state->height,
                     mx_html_scroll_y());
        return;
    }
    int center_x = (int)state->width / 2;
    int center_y = top + ((int)state->height - top) / 2;
    int card_width = (int)state->width - 48;
    if (card_width > 600) card_width = 600;
    int card_x = center_x - card_width / 2;
    int card_y = center_y - 116;
    fill_rect(state, card_x, card_y, card_width, 232, theme->surface);
    stroke_rect(state, card_x, card_y, card_width, 232, theme->border);
    fill_rect(state, card_x + 1, card_y + 1, card_width - 2, 3,
              theme->accent);
    draw_globe(state, center_x, card_y + 45);
    draw_centered_label(state, center_x, card_y + 78, "Page not loaded",
                        theme->text, 2);
    draw_centered_label(state, center_x, card_y + 108,
                        "Enter an address or search from the bar above.",
                        theme->muted, 1);
    fill_rect(state, card_x + 28, card_y + 140, card_width - 56, 1,
              theme->border);
    draw_label(state, card_x + 28, card_y + 154, "CURRENT ADDRESS",
               theme->muted, 1);
    draw_truncated(state, card_x + 28, card_y + 174, card_width - 56,
                   state->tabs[state->active_tab].url, theme->text);
    draw_truncated(state, card_x + 28, card_y + 202, card_width - 56,
                   state->status_message, theme->muted);
}

static void draw_homepage_option(const mx_ui_state_t* state, int x, int y,
                                 const char* label, int selected) {
    fill_rect(state, x, y + 1, 15, 15, state->theme->surface);
    stroke_rect(state, x, y + 1, 15, 15,
                selected ? state->theme->accent : state->theme->border);
    if (selected)
        fill_rect(state, x + 4, y + 5, 7, 7, state->theme->accent);
    draw_label(state, x + 25, y + 1, label,
               selected ? state->theme->text : state->theme->muted, 1);
}

static void draw_settings_page(const mx_ui_state_t* state) {
    const mx_browser_theme_t* theme = state->theme;
    int top = MX_BROWSER_PAGE_Y;
    int sidebar_width = 206;
    fill_rect(state, 0, top, (int)state->width,
              (int)state->height - top, theme->background);
    fill_rect(state, 0, top, sidebar_width,
              (int)state->height - top, theme->background);
    fill_rect(state, sidebar_width, top, 1,
              (int)state->height - top, theme->border);
    draw_label(state, 24, top + 22, "Settings", theme->text, 3);

    for (int i = 0; i < 4; i++) {
        int y = top + 78 + i * 44;
        if (i == state->settings_category) {
            fill_rect(state, 12, y, sidebar_width - 24, 36,
                      theme->surface);
            fill_rect(state, 12, y, 3, 36, theme->accent);
        }
        draw_label(state, 28, y + 11, category_names[i],
                   i == state->settings_category ? theme->text : theme->muted, 1);
    }

    int content_x = sidebar_width + 32;
    int content_width = (int)state->width - content_x - 28;
    int content_y = top + 20;
    draw_label(state, content_x, content_y,
               category_names[state->settings_category], theme->text, 3);
    draw_label(state, content_x, content_y + 38,
               category_descriptions[state->settings_category], theme->muted, 1);
    fill_rect(state, content_x, content_y + 64, content_width, 1,
              theme->border);

    if (state->settings_category == 0) {
        draw_label(state, content_x, content_y + 90,
                   "Homepage", theme->text, 2);
        draw_homepage_option(state, content_x, content_y + 116,
                             "Google  -  https://google.com",
                             state->homepage_mode == 0);
        draw_homepage_option(state, content_x, content_y + 144,
                             "Mxplorer alternative  -  built-in HTML",
                             state->homepage_mode == 1);
        draw_homepage_option(state, content_x, content_y + 172,
                             "Custom address",
                             state->homepage_mode == 2);
        fill_rect(state, content_x + 20, content_y + 202,
                  content_width - 20, 34, theme->surface);
        stroke_rect(state, content_x + 20, content_y + 202,
                    content_width - 20, 34,
                    state->homepage_editing ? theme->accent : theme->border);
        const char* custom_value = state->homepage_editing
                                       ? state->address_text
                                       : state->custom_homepage;
        if (custom_value && custom_value[0])
            draw_truncated(state, content_x + 30, content_y + 212,
                           content_width - 42, custom_value, theme->text);
        else
            draw_label(state, content_x + 30, content_y + 212,
                       "Click to enter a website address", theme->muted, 1);
        draw_label(state, content_x + 20, content_y + 245,
                   "Select Custom, edit the address bar above, then press Enter.",
                   theme->muted, 1);
        draw_label(state, content_x, content_y + 286,
                   "New tabs and the Home button use this homepage.",
                   theme->muted, 1);
    } else if (state->settings_category == 1) {
        draw_label(state, content_x, content_y + 90,
                   "OS accent color", theme->text, 2);
        fill_rect(state, content_x, content_y + 126, 28, 28,
                  theme->accent);
        stroke_rect(state, content_x, content_y + 126, 28, 28,
                    theme->border);
        draw_label(state, content_x + 42, content_y + 133,
                   "Mxplorer follows the accent saved in MOSdesk Settings.",
                   theme->muted, 1);
        draw_label(state, content_x, content_y + 182,
                   "Interface", theme->text, 2);
        draw_label(state, content_x, content_y + 212,
                   "Native UI; browser pages use the HTML/CSS renderer.",
                   theme->muted, 1);
    } else if (state->settings_category == 2) {
        draw_label(state, content_x, content_y + 90,
                   "Unsafe HTTPS transport", theme->text, 2);
        draw_label(state, content_x, content_y + 120,
                   "Allow insecure TLS (unsafe)", theme->text, 1);
        fill_rect(state, (int)state->width - 72, content_y + 115, 40, 22,
                  state->allow_insecure_tls ? theme->accent : theme->button);
        stroke_rect(state, (int)state->width - 72, content_y + 115, 40, 22,
                    theme->border);
        fill_rect(state, state->allow_insecure_tls
                             ? (int)state->width - 50
                             : (int)state->width - 68,
                  content_y + 119, 14, 14, theme->surface);
        draw_label(state, content_x, content_y + 151,
                   state->allow_insecure_tls
                       ? "Enabled for this session. Never send passwords or private data."
                       : "Off by default. Click this row to permit HTTPS attempts.",
                   theme->muted, 1);
        draw_label(state, content_x, content_y + 181,
                   "SDK TLS has no certificates and exposes session keys.",
                   theme->accent, 1);
        draw_label(state, content_x, content_y + 207,
                   "It provides no reliable encryption or server identity.",
                   theme->accent, 1);
        draw_label(state, content_x, content_y + 250,
                   "Scripts are disabled; remote images and stylesheets are not loaded.",
                   theme->muted, 1);
    } else {
        draw_label(state, content_x, content_y + 90,
                   "Mxplorer", theme->text, 2);
        draw_label(state, content_x, content_y + 120,
                   "Version 0.2.0 - HTML/CSS renderer preview", theme->muted, 1);
        draw_label(state, content_x, content_y + 162,
                   "Rendering", theme->text, 2);
        draw_label(state, content_x, content_y + 192,
                   "HTML Living Standard subset with embedded CSS.",
                   theme->muted, 1);
    }
}

static void draw_debug_panel(const mx_ui_state_t* state) {
    enum { PANEL_HEADER = 38, PANEL_CONTENT_TOP = 46 };
    const uint32_t panel = 0xFF172334u;
    const uint32_t panel_header = 0xFF111B29u;
    const uint32_t panel_border = 0xFF344357u;
    const uint32_t panel_text = 0xFFE2EAF3u;
    const uint32_t panel_muted = 0xFF9BAEC3u;
    const uint32_t panel_accent = 0xFF56B4F5u;
    const int panel_height = MX_BROWSER_DEBUG_PANEL_HEIGHT;
    int top = (int)state->height - panel_height;
    if (top < (int)MX_HTML_VIEW_TOP) top = (int)MX_HTML_VIEW_TOP;
    fill_rect(state, 0, top, (int)state->width, panel_height, panel);
    fill_rect(state, 0, top, (int)state->width, PANEL_HEADER, panel_header);
    fill_rect(state, 0, top, (int)state->width, 2, panel_accent);
    fill_rect(state, 0, top + PANEL_HEADER - 1, (int)state->width, 1,
              panel_border);
    draw_label(state, 16, top + 13, "Developer tools", panel_text, 1);

    static const char* const tab_names[] = {"Console", "Network", "Elements"};
    for (int i = 0; i < 3; i++) {
        int x = 170 + i * 112;
        if (state->debug_tab == i)
            fill_rect(state, x, top + PANEL_HEADER - 3, 92, 3, panel_accent);
        draw_label(state, x + 8, top + 13, tab_names[i],
                   state->debug_tab == i ? panel_text : panel_muted, 1);
    }
    draw_label(state, (int)state->width - 28, top + 13, "x", panel_muted, 1);

    int y = top + PANEL_CONTENT_TOP;
    if (state->debug_tab == 0) {
        draw_label(state, 16, y, "Console", panel_accent, 1);
        y += 24;
        int first = state->debug_log_count > 6 ? state->debug_log_count - 6 : 0;
        if (first == state->debug_log_count) {
            draw_label(state, 16, y, "No console messages yet.",
                       panel_muted, 1);
            y += 20;
        }
        for (int i = first; i < state->debug_log_count && y < (int)state->height - 10;
             i++, y += 21)
            draw_truncated(state, 16, y, (int)state->width - 32,
                           state->debug_logs[i], panel_text);
        draw_label(state, 16, (int)state->height - 24,
                   "JavaScript console is unavailable; scripts are not executed.",
                   panel_muted, 1);
    } else if (state->debug_tab == 1) {
        draw_label(state, 16, y, "Request overview", panel_accent, 1);
        y += 24;
        draw_label(state, 16, y, "URL", panel_muted, 1);
        draw_truncated(state, 74, y, (int)state->width - 94,
                       state->tabs[state->active_tab].url, panel_text);
        y += 23;
        draw_label(state, 16, y, "Status", panel_muted, 1);
        draw_truncated(state, 74, y, (int)state->width - 94,
                       state->status_message, panel_text);
        y += 23;
        draw_label(state, 16, y,
                   "Redirects, external assets, and certificate validation are not implemented.",
                   panel_muted, 1);
        y += 22;
        draw_label(state, 16, y,
                   state->allow_insecure_tls
                       ? "Unsafe TLS transport is enabled for this session."
                       : "Unsafe TLS transport is disabled.",
                   state->allow_insecure_tls ? 0xFFFFC46Bu : panel_muted, 1);
    } else {
        draw_label(state, 16, y, "Document outline (source tags)",
                   panel_accent, 1);
        y += 24;
        if (!state->debug_source || !state->debug_source_length) {
            draw_label(state, 16, y, "No HTML source is available for this page.",
                       panel_muted, 1);
        } else {
            size_t pos = 0;
            int shown = 0;
            while (pos < state->debug_source_length && shown < 7 &&
                   y < (int)state->height - 8) {
                if (state->debug_source[pos++] != '<') continue;
                if (pos >= state->debug_source_length) break;
                char first = state->debug_source[pos];
                if (first == '/' || first == '!' || first == '?') continue;
                char element[28];
                size_t length = 0;
                while (pos < state->debug_source_length &&
                       length + 1 < sizeof(element)) {
                    char c = state->debug_source[pos];
                    if (!((c >= 'a' && c <= 'z') ||
                          (c >= 'A' && c <= 'Z') ||
                          (c >= '0' && c <= '9') || c == '-'))
                        break;
                    element[length++] = c;
                    pos++;
                }
                if (!length) continue;
                element[length] = '\0';
                char label[40] = "  <";
                size_t label_length = strlen(label);
                memcpy(label + label_length, element, length);
                label[label_length + length] = '>';
                label[label_length + length + 1] = '\0';
                draw_label(state, 16, y, label, panel_text, 1);
                y += 19;
                shown++;
            }
            if (!shown)
                draw_label(state, 16, y, "No element tags found.",
                           panel_muted, 1);
        }
        draw_label(state, 250, top + PANEL_CONTENT_TOP,
                   state->debug_source_length
                       ? "HTML source structure"
                       : "No captured HTML source",
                   panel_muted, 1);
    }
}

void mx_ui_draw_content(const mx_ui_state_t* state) {
    if (!state || !state->pixels || !state->width || !state->height) return;
    if (ui_is_settings_page(state))
        draw_settings_page(state);
    else
        draw_external_page(state);
    if (state->debug_open) draw_debug_panel(state);
    mos_window_present(state->window_id, state->pixels, state->width * 4u,
                       0, MX_HTML_VIEW_TOP, state->width,
                       state->height - MX_HTML_VIEW_TOP);
}
