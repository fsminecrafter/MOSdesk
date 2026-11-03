#include "mxplorer/devtools.h"
#include "mxplorer/css.h"
#include "mxplorer/devlog.h"
#include "mxplorer/html.h"
#include "mxplorer/js.h"
#include "mxplorer/text.h"
#include "string.h"
#include "stdlib.h"

#define HEADER 36
#define ROW 22
#define MONO 12

static int current_tab;
static int panel_height = MX_BROWSER_DEVTOOLS_DEFAULT;
static int panel_x, panel_y, panel_w, panel_h;   /* last drawn rectangle */
static int resizing;
static int resize_offset;
static int close_request;

/* console */
static int console_filter;          /* 0 all, 1 errors, 2 warnings, 3 info, 4 logs */
static int console_scroll;          /* rows scrolled up from the bottom */
static int console_focused;
static char console_input[256];
static int console_cursor;
static char console_history[16][256];
static int history_count, history_index = -1;

/* elements */
static mx_node_t* selected;
static mx_node_t* hovered;
static int tree_scroll;
static int picking;

/* network, sources */
static int network_selected = -1;
static int network_scroll;
static int source_scroll;
static const char* cached_source;
static size_t cached_length;
static uint32_t* line_offsets;
static int line_count;

static const mx_browser_theme_t* T;

static const char* const tab_names[MX_DEVTOOLS_TAB_COUNT] = {
    "Console", "Elements", "Network", "Sources", "Performance"
};
static const mx_icon_t tab_icons[MX_DEVTOOLS_TAB_COUNT] = {
    MX_ICON_TERMINAL, MX_ICON_ACCOUNT_TREE, MX_ICON_SWAP_VERT, MX_ICON_DESCRIPTION, MX_ICON_SPEED
};

void mx_devtools_reset_page(void) {
    selected = hovered = NULL;
    tree_scroll = 0;
    network_selected = -1;
    network_scroll = 0;
    source_scroll = 0;
    console_scroll = 0;
    picking = 0;
    mx_html_set_highlight(NULL);
}

void mx_devtools_set_tab(int tab) {
    if (tab >= 0 && tab < MX_DEVTOOLS_TAB_COUNT) current_tab = tab;
}

int mx_devtools_tab(void) {
    return current_tab;
}

int mx_devtools_height(void) {
    return panel_height;
}

void mx_devtools_set_height(int height) {
    if (height < MX_BROWSER_DEVTOOLS_MIN) height = MX_BROWSER_DEVTOOLS_MIN;
    panel_height = height;
}

int mx_devtools_focused(void) {
    return console_focused;
}

void mx_devtools_focus_console(void) {
    current_tab = MX_DEVTOOLS_CONSOLE;
    console_focused = 1;
}

void mx_devtools_blur(void) {
    console_focused = 0;
}

int mx_devtools_resizing(void) {
    return resizing;
}

int mx_devtools_picking(void) {
    return picking;
}

int mx_devtools_take_close_request(void) {
    int r = close_request;
    close_request = 0;
    return r;
}

/* ====================================================================== */
/* Shared drawing                                                         */
/* ====================================================================== */

static int text_w(const char* text, int px, int flags) {
    return mx_text_width_px(text, (size_t)-1, px, flags);
}

static void label(const mx_canvas_t* c, int x, int y, const char* text, uint32_t color, int px,
                  int flags) {
    mx_text(c, x, y, text, (size_t)-1, color, px, flags);
}

static void chip(const mx_canvas_t* c, int x, int y, const char* text, int on, int* width) {
    int w = text_w(text, 12, MX_TEXT_MEDIUM) + 20;
    if (on) mx_round_rect(c, x, y, w, 22, 11, T->accent_soft);
    else mx_round_rect_stroke(c, x, y, w, 22, 11, 1, T->border);
    label(c, x + 10, y + (22 - mx_text_height_px(12, MX_TEXT_MEDIUM)) / 2, text,
          on ? T->accent : T->muted, 12, MX_TEXT_MEDIUM);
    *width = w;
}

static uint32_t body_color(void) {
    return T->dark ? 0xFF1E1F22u : 0xFFFFFFFFu;
}

static uint32_t stripe_color(void) {
    return T->dark ? 0xFF24252Au : 0xFFF7F8FAu;
}

/* ====================================================================== */
/* Console                                                                */
/* ====================================================================== */

static int entry_visible(const mx_console_entry_t* e) {
    switch (console_filter) {
    case 1: return e->level == MX_LOG_ERROR;
    case 2: return e->level == MX_LOG_WARN;
    case 3: return e->level == MX_LOG_INFO || e->level == MX_LOG_SYSTEM;
    case 4: return e->level == MX_LOG_LOG || e->level == MX_LOG_DEBUG || e->level == MX_LOG_INPUT ||
                   e->level == MX_LOG_RESULT;
    default: return 1;
    }
}

/* Splits a message into up to `max` lines fitting `width`. *cut is set when
 * text remains after the last line. */
static int wrap_lines(const char* text, int width, const char** starts, size_t* lengths, int max,
                      int* cut) {
    int count = 0;
    const char* p = text;
    *cut = 0;
    while (*p && count < max) {
        const char* nl = strchr(p, '\n');
        size_t segment = nl ? (size_t)(nl - p) : strlen(p);
        char buffer[MX_CONSOLE_TEXT];
        size_t n = segment < sizeof(buffer) - 1 ? segment : sizeof(buffer) - 1;
        memcpy(buffer, p, n);
        buffer[n] = '\0';
        size_t fit = mx_text_fit(buffer, MONO, MX_TEXT_MONO, width);
        if (!fit && n) fit = 1;
        if (fit > n) fit = n;
        starts[count] = p;
        lengths[count] = fit;
        count++;
        if (fit < n) {
            p += fit;
        } else {
            p += segment;
            if (*p == '\n') p++;
        }
    }
    *cut = *p != '\0';
    return count;
}

static void console_draw(const mx_canvas_t* c, int x, int y, int w, int h) {
    int fx = x + 10;
    static const char* const filters[] = {"All", "Errors", "Warnings", "Info", "Logs"};
    for (int i = 0; i < 5; i++) {
        int cw;
        chip(c, fx, y + 6, filters[i], console_filter == i, &cw);
        fx += cw + 6;
    }
    char counts[64] = "";
    mx_append_int(counts, sizeof(counts), mx_console_count());
    mx_append(counts, sizeof(counts), mx_console_count() == 1 ? " message" : " messages");
    label(c, x + w - text_w(counts, 12, 0) - 12, y + 10, counts, T->subtle, 12, 0);
    int list_top = y + 34;
    int prompt_h = 30;
    int list_bottom = y + h - prompt_h;
    mx_fill(c, x, list_top - 1, w, 1, T->divider);
    mx_canvas_t list = mx_canvas_clip(c, x, list_top, w, list_bottom - list_top);
    /* draw from the bottom up */
    int row_y = list_bottom;
    int skipped = 0;
    int text_x = x + 34;
    for (int i = mx_console_count() - 1; i >= 0 && row_y > list_top; i--) {
        const mx_console_entry_t* e = mx_console_get(i);
        if (!entry_visible(e)) continue;
        if (skipped < console_scroll) {
            skipped++;
            continue;
        }
        char where[80] = "";
        if (e->source[0]) {
            mx_copy(where, sizeof(where), e->source);
            if (e->line) {
                mx_append(where, sizeof(where), ":");
                mx_append_int(where, sizeof(where), e->line);
            }
        }
        int where_w = where[0] ? text_w(where, 11, 0) + 16 : 0;
        int badge_w = e->repeat > 1 ? 30 : 0;
        const char* starts[4];
        size_t lengths[4];
        int cut = 0;
        int lines = wrap_lines(e->text, w - (text_x - x) - where_w - badge_w - 12, starts, lengths, 4,
                               &cut);
        if (!lines) lines = 1, starts[0] = "", lengths[0] = 0;
        int rh = lines * 16 + 8;
        row_y -= rh;
        uint32_t bg = 0, fg = T->code_text;
        mx_icon_t icon = 0;
        int has_icon = 1;
        switch (e->level) {
        case MX_LOG_ERROR: bg = T->danger_soft; fg = T->danger; icon = MX_ICON_ERROR; break;
        case MX_LOG_WARN: bg = T->warning_soft; fg = T->warning; icon = MX_ICON_WARNING; break;
        case MX_LOG_INFO: icon = MX_ICON_INFO; fg = T->text; break;
        case MX_LOG_INPUT: icon = MX_ICON_KEYBOARD_ARROW_RIGHT; fg = T->accent; break;
        case MX_LOG_RESULT: icon = MX_ICON_CHEVRON_RIGHT; fg = T->muted; break;
        case MX_LOG_SYSTEM: icon = MX_ICON_PUBLIC; fg = T->muted; break;
        case MX_LOG_DEBUG: fg = T->muted; has_icon = 0; break;
        default: has_icon = 0; break;
        }
        if (bg) mx_fill(&list, x, row_y, w, rh, bg);
        mx_fill(&list, x, row_y + rh - 1, w, 1, T->divider);
        if (has_icon) mx_icon(&list, icon, 16, x + 12, row_y + 4, fg);
        for (int l = 0; l < lines; l++) {
            size_t len = lengths[l];
            mx_text(&list, text_x, row_y + 4 + l * 16, starts[l], len,
                    e->level == MX_LOG_LOG ? T->code_text : fg, MONO, MX_TEXT_MONO);
            if (cut && l == lines - 1) {
                int lw = mx_text_width_px(starts[l], len, MONO, MX_TEXT_MONO);
                mx_text(&list, text_x + lw, row_y + 4 + l * 16, "\xE2\x80\xA6", 3, T->muted, MONO,
                        MX_TEXT_MONO);
            }
        }
        int right = x + w - 12;
        if (where[0]) {
            label(&list, right - text_w(where, 11, 0), row_y + 5, where, T->subtle, 11, 0);
            right -= where_w;
        }
        if (badge_w) {
            char n[12] = "";
            mx_append_int(n, sizeof(n), e->repeat);
            int bw = text_w(n, 11, MX_TEXT_MEDIUM) + 12;
            mx_round_rect(&list, right - bw, row_y + 4, bw, 16, 8, T->accent_soft);
            label(&list, right - bw + 6, row_y + 5, n, T->accent, 11, MX_TEXT_MEDIUM);
        }
    }
    if (!mx_console_count())
        label(c, x + 34, list_top + 12, "No messages yet. console.log() output and errors appear here.",
              T->subtle, 12, 0);
    /* prompt */
    int py = list_bottom;
    mx_fill(c, x, py, w, prompt_h, console_focused ? (T->dark ? 0xFF25262Bu : 0xFFF4F7FCu) : body_color());
    mx_fill(c, x, py, w, 1, T->divider);
    mx_icon(c, MX_ICON_KEYBOARD_ARROW_RIGHT, 20, x + 10, py + 5, T->accent);
    mx_canvas_t field = mx_canvas_clip(c, text_x, py, w - (text_x - x) - 12, prompt_h);
    int ty = py + (prompt_h - mx_text_height_px(MONO, MX_TEXT_MONO)) / 2;
    if (!console_input[0] && !console_focused) {
        label(&field, text_x, ty,
              mx_js_active() ? "Type JavaScript here and press Enter" : "JavaScript is not running on this page",
              T->subtle, MONO, MX_TEXT_MONO);
    } else {
        int input_w = text_w(console_input, MONO, MX_TEXT_MONO);
        int avail = w - (text_x - x) - 24;
        int shift = input_w > avail ? input_w - avail : 0;
        mx_text(&field, text_x - shift, ty, console_input, (size_t)-1, T->code_text, MONO, MX_TEXT_MONO);
        if (console_focused) {
            int caret = text_x - shift + mx_text_width_px(console_input, (size_t)console_cursor, MONO,
                                                          MX_TEXT_MONO);
            mx_fill(&field, caret, ty - 1, 2, mx_text_height_px(MONO, MX_TEXT_MONO) + 2, T->accent);
        }
    }
}

static void console_submit(void) {
    if (!console_input[0]) return;
    mx_console_add(MX_LOG_INPUT, "", 0, console_input);
    char result[MX_CONSOLE_TEXT];
    int ok = mx_js_eval(console_input, result, sizeof(result));
    mx_console_add(ok ? MX_LOG_RESULT : MX_LOG_ERROR, "", 0, result);
    if (history_count == 16) {
        memmove(console_history, console_history + 1, sizeof(console_history[0]) * 15);
        history_count--;
    }
    mx_copy(console_history[history_count++], sizeof(console_history[0]), console_input);
    history_index = -1;
    console_input[0] = '\0';
    console_cursor = 0;
    console_scroll = 0;
}

/* ====================================================================== */
/* Elements                                                               */
/* ====================================================================== */

typedef struct {
    mx_node_t* node;
    int depth;
    int closing;
} tree_row;

static int has_visible_children(mx_node_t* n) {
    for (mx_node_t* c = n->first; c; c = c->next) {
        if (c->type == MX_NODE_ELEMENT || c->type == MX_NODE_COMMENT) return 1;
        if (c->type == MX_NODE_TEXT)
            for (size_t i = 0; i < c->text_length; i++)
                if (!mx_is_space(c->text[i])) return 1;
    }
    return 0;
}

static int only_short_text(mx_node_t* n) {
    if (!n->first || n->first != n->last || n->first->type != MX_NODE_TEXT) return 0;
    return n->first->text_length < 60;
}

static int build_tree(mx_node_t* n, int depth, tree_row* rows, int count, int capacity) {
    for (mx_node_t* c = n->first; c && count < capacity; c = c->next) {
        if (c->type == MX_NODE_TEXT) {
            int blank = 1;
            for (size_t i = 0; i < c->text_length && blank; i++) blank = mx_is_space(c->text[i]);
            if (blank) continue;
        }
        rows[count].node = c;
        rows[count].depth = depth;
        rows[count].closing = 0;
        count++;
        if (c->type == MX_NODE_ELEMENT && c->devtools_open && has_visible_children(c) &&
            !only_short_text(c)) {
            count = build_tree(c, depth + 1, rows, count, capacity);
            if (count < capacity) {
                rows[count].node = c;
                rows[count].depth = depth;
                rows[count].closing = 1;
                count++;
            }
        }
    }
    return count;
}

#define TREE_ROWS_MAX 4096
static tree_row* tree_rows;

static int collect_tree_rows(void) {
    mx_document_t* doc = mx_html_document();
    if (!doc) return 0;
    if (!tree_rows) tree_rows = (tree_row*)malloc(sizeof(tree_row) * TREE_ROWS_MAX);
    if (!tree_rows) return 0;
    if (!doc->html->devtools_open) {
        doc->html->devtools_open = 1;
        doc->body->devtools_open = 1;
    }
    return build_tree(doc->root, 0, tree_rows, 0, TREE_ROWS_MAX);
}

/* Draws one element's opening tag with colours; returns the end x. */
static int draw_tag(const mx_canvas_t* c, int x, int y, mx_node_t* n, int closing, int collapsed) {
    char buffer[256];
    if (closing) {
        buffer[0] = '\0';
        mx_append(buffer, sizeof(buffer), "</");
        mx_append(buffer, sizeof(buffer), n->tag);
        mx_append(buffer, sizeof(buffer), ">");
        mx_text(c, x, y, buffer, (size_t)-1, T->code_tag, MONO, MX_TEXT_MONO);
        return x + text_w(buffer, MONO, MX_TEXT_MONO);
    }
    mx_copy(buffer, sizeof(buffer), "<");
    mx_append(buffer, sizeof(buffer), n->tag);
    mx_text(c, x, y, buffer, (size_t)-1, T->code_tag, MONO, MX_TEXT_MONO);
    x += text_w(buffer, MONO, MX_TEXT_MONO);
    int shown = 0;
    for (mx_attr_t* a = n->attrs; a && shown < 6; a = a->next, shown++) {
        mx_copy(buffer, sizeof(buffer), " ");
        mx_append(buffer, sizeof(buffer), a->name);
        mx_text(c, x, y, buffer, (size_t)-1, T->code_attr, MONO, MX_TEXT_MONO);
        x += text_w(buffer, MONO, MX_TEXT_MONO);
        if (a->value && *a->value) {
            mx_copy(buffer, sizeof(buffer), "=\"");
            size_t used = strlen(buffer);
            mx_copy_n(buffer + used, sizeof(buffer) - used, a->value,
                      strlen(a->value) > 60 ? 60 : strlen(a->value));
            if (strlen(a->value) > 60) mx_append(buffer, sizeof(buffer), "\xE2\x80\xA6");
            mx_append(buffer, sizeof(buffer), "\"");
            mx_text(c, x, y, buffer, (size_t)-1, T->code_value, MONO, MX_TEXT_MONO);
            x += text_w(buffer, MONO, MX_TEXT_MONO);
        }
    }
    mx_text(c, x, y, ">", 1, T->code_tag, MONO, MX_TEXT_MONO);
    x += text_w(">", MONO, MX_TEXT_MONO);
    if (only_short_text(n) && !mx_dom_is_void(n->tag)) {
        char text[96];
        mx_copy_n(text, sizeof(text), n->first->text, n->first->text_length);
        for (char* p = text; *p; p++) if (*p == '\n' || *p == '\t') *p = ' ';
        mx_text(c, x, y, text, (size_t)-1, T->code_text, MONO, MX_TEXT_MONO);
        x += text_w(text, MONO, MX_TEXT_MONO);
        x = draw_tag(c, x, y, n, 1, 0);
    } else if (collapsed && has_visible_children(n)) {
        mx_text(c, x, y, "\xE2\x80\xA6", 3, T->muted, MONO, MX_TEXT_MONO);
        x += text_w("\xE2\x80\xA6", MONO, MX_TEXT_MONO);
        x = draw_tag(c, x, y, n, 1, 0);
    }
    return x;
}

static void describe_node(mx_node_t* n, char* out, size_t capacity) {
    mx_copy(out, capacity, n->tag ? n->tag : "#text");
    const char* id = mx_dom_attr(n, "id");
    if (id) {
        mx_append(out, capacity, "#");
        mx_append(out, capacity, id);
    }
    const char* cls = mx_dom_attr(n, "class");
    if (cls && *cls) {
        char first[64];
        size_t i = 0;
        while (cls[i] && !mx_is_space(cls[i]) && i + 1 < sizeof(first)) {
            first[i] = cls[i];
            i++;
        }
        first[i] = '\0';
        mx_append(out, capacity, ".");
        mx_append(out, capacity, first);
    }
}

static void color_text(uint32_t color, char* out, size_t capacity) {
    static const char hex[] = "0123456789abcdef";
    out[0] = '\0';
    if (!(color >> 24)) {
        mx_copy(out, capacity, "transparent");
        return;
    }
    char text[12] = "#";
    for (int i = 0; i < 6; i++) text[1 + i] = hex[(color >> (20 - 4 * i)) & 15];
    text[7] = '\0';
    mx_copy(out, capacity, text);
    if ((color >> 24) < 255) {
        mx_append(out, capacity, " / ");
        mx_append_int(out, capacity, (long)((color >> 24) * 100 / 255));
        mx_append(out, capacity, "%");
    }
}

static void box_model(const mx_canvas_t* c, int x, int y, int w, mx_node_t* n) {
    const mx_style_t* s = n->style;
    int bw = w, bh = 150;
    if (bw > 320) bw = 320;
    uint32_t margin = 0xFFF9CC9Du, border = 0xFFFFEEBCu, padding = 0xFFC3D08Bu, content = 0xFF8CB6C0u;
    if (T->dark) {
        margin = 0xFF7A5A36u;
        border = 0xFF7D6E3Du;
        padding = 0xFF5B6A3Au;
        content = 0xFF3D6670u;
    }
    int ref = n->parent && n->parent->style ? n->box_w : n->box_w;
    int m[4] = {0}, p[4] = {0}, b[4] = {0};
    if (s) {
        for (int i = 0; i < 4; i++) {
            m[i] = s->margin[i].kind == MX_LEN_SET ? mx_len_resolve(s->margin[i], ref) : 0;
            p[i] = mx_len_resolve(s->padding[i], ref);
            b[i] = s->border_width[i];
        }
    }
    int cx = x, cy = y;
    mx_round_rect(c, cx, cy, bw, bh, 4, margin);
    mx_round_rect(c, cx + 22, cy + 22, bw - 44, bh - 44, 3, border);
    mx_round_rect(c, cx + 44, cy + 44, bw - 88, bh - 88, 2, padding);
    mx_round_rect(c, cx + 66, cy + 64, bw - 132, bh - 128, 2, content);
    uint32_t ink = T->dark ? 0xFFF0F0F0u : 0xFF202124u;
    label(c, cx + 4, cy + 3, "margin", ink, 10, 0);
    label(c, cx + 26, cy + 25, "border", ink, 10, 0);
    label(c, cx + 48, cy + 47, "padding", ink, 10, 0);
    char text[48];
    text[0] = '\0';
    int content_w = n->box_w - p[1] - p[3] - b[1] - b[3];
    int content_h = n->box_h - p[0] - p[2] - b[0] - b[2];
    mx_append_int(text, sizeof(text), content_w > 0 ? content_w : 0);
    mx_append(text, sizeof(text), " \xC3\x97 ");
    mx_append_int(text, sizeof(text), content_h > 0 ? content_h : 0);
    label(c, cx + bw / 2 - text_w(text, 11, MX_TEXT_MEDIUM) / 2, cy + bh / 2 - 7, text, ink, 11, MX_TEXT_MEDIUM);
    int values[3][4] = {{m[0], m[1], m[2], m[3]}, {b[0], b[1], b[2], b[3]}, {p[0], p[1], p[2], p[3]}};
    for (int ring = 0; ring < 3; ring++) {
        int inset = ring * 22;
        char v[12];
        for (int side = 0; side < 4; side++) {
            v[0] = '\0';
            if (values[ring][side]) mx_append_int(v, sizeof(v), values[ring][side]);
            else mx_copy(v, sizeof(v), "-");
            int vw = text_w(v, 10, 0);
            int vx = side == 1 ? cx + bw - inset - 11 - vw / 2 : side == 3 ? cx + inset + 11 - vw / 2
                                                                           : cx + bw / 2 - vw / 2;
            int vy = side == 0 ? cy + inset + 6 : side == 2 ? cy + bh - inset - 17 : cy + bh / 2 - 6;
            if (side == 1 || side == 3) vy = cy + bh / 2 - 6;
            label(c, vx, vy, v, ink, 10, 0);
        }
    }
}

static void elements_draw(const mx_canvas_t* c, int x, int y, int w, int h) {
    mx_document_t* doc = mx_html_document();
    /* toolbar */
    int picker_on = picking;
    if (picker_on) mx_round_rect(c, x + 8, y + 5, 26, 24, 6, T->accent_soft);
    mx_icon(c, MX_ICON_ARROW_SELECTOR_TOOL, 20, x + 11, y + 7, picker_on ? T->accent : T->muted);
    if (selected) {
        char path[200] = "";
        mx_node_t* chain[8];
        int depth = 0;
        for (mx_node_t* n = selected; n && n->type == MX_NODE_ELEMENT && depth < 8; n = n->parent)
            chain[depth++] = n;
        for (int i = depth - 1; i >= 0; i--) {
            char part[64];
            describe_node(chain[i], part, sizeof(part));
            mx_append(path, sizeof(path), part);
            if (i) mx_append(path, sizeof(path), "  /  ");
        }
        mx_canvas_t clip = mx_canvas_clip(c, x + 44, y, w - 54, 34);
        mx_text_ellipsis(&clip, x + 44, y + 10, w - 54, path, T->muted, 12, 0);
    } else {
        label(c, x + 44, y + 10, picking ? "Click an element on the page" : "Select an element in the tree or with the picker",
              T->subtle, 12, 0);
    }
    int top = y + 34;
    mx_fill(c, x, top - 1, w, 1, T->divider);
    if (!doc) {
        label(c, x + 16, top + 14, "This page has no document tree.", T->subtle, 12, 0);
        return;
    }
    int tree_w = w * 58 / 100;
    int body_h = y + h - top;
    mx_fill(c, x + tree_w, top, 1, body_h, T->divider);
    int count = collect_tree_rows();
    int visible = body_h / ROW;
    if (tree_scroll > count - visible) tree_scroll = count - visible;
    if (tree_scroll < 0) tree_scroll = 0;
    mx_canvas_t tree = mx_canvas_clip(c, x, top, tree_w, body_h);
    for (int i = tree_scroll; i < count && i < tree_scroll + visible + 1; i++) {
        tree_row* r = &tree_rows[i];
        int ry = top + (i - tree_scroll) * ROW;
        mx_node_t* n = r->node;
        if (n == selected && !r->closing) mx_fill(&tree, x, ry, tree_w, ROW, T->accent_soft);
        else if (n == hovered && !r->closing) mx_fill(&tree, x, ry, tree_w, ROW, mx_with_alpha(T->hover, 200));
        int tx = x + 8 + r->depth * 14;
        int ty = ry + (ROW - mx_text_height_px(MONO, MX_TEXT_MONO)) / 2;
        if (n->type == MX_NODE_ELEMENT) {
            if (!r->closing && has_visible_children(n) && !only_short_text(n))
                mx_icon(&tree, n->devtools_open ? MX_ICON_ARROW_DROP_DOWN : MX_ICON_ARROW_RIGHT, 16,
                        tx, ry + 3, T->muted);
            draw_tag(&tree, tx + 16, ty, n, r->closing, !n->devtools_open);
        } else if (n->type == MX_NODE_TEXT) {
            char text[120] = "\"";
            size_t start = 0;
            while (start < n->text_length && mx_is_space(n->text[start])) start++;
            size_t len = n->text_length - start;
            if (len > 100) len = 100;
            size_t used = strlen(text);
            mx_copy_n(text + used, sizeof(text) - used, n->text + start, len);
            for (char* p = text; *p; p++) if (*p == '\n' || *p == '\t' || *p == '\r') *p = ' ';
            size_t end = strlen(text);
            while (end > 1 && text[end - 1] == ' ') text[--end] = '\0';
            mx_append(text, sizeof(text), "\"");
            mx_text(&tree, tx + 16, ty, text, (size_t)-1, T->code_text, MONO, MX_TEXT_MONO);
        } else if (n->type == MX_NODE_COMMENT) {
            char text[120] = "<!--";
            size_t used = strlen(text);
            mx_copy_n(text + used, sizeof(text) - used, n->text, n->text_length > 80 ? 80 : n->text_length);
            mx_append(text, sizeof(text), "-->");
            for (char* p = text; *p; p++) if (*p == '\n') *p = ' ';
            mx_text(&tree, tx + 16, ty, text, (size_t)-1, T->code_comment, MONO, MX_TEXT_MONO);
        }
    }
    /* details */
    int dx = x + tree_w + 14, dw = w - tree_w - 28;
    mx_canvas_t side = mx_canvas_clip(c, x + tree_w + 1, top, w - tree_w - 1, body_h);
    if (!selected || selected->type != MX_NODE_ELEMENT) {
        label(&side, dx, top + 14, "No element selected", T->subtle, 12, 0);
        return;
    }
    mx_html_ensure_layout();
    int sy = top + 10;
    label(&side, dx, sy, "Box model", T->text, 12, MX_TEXT_BOLD);
    sy += 20;
    box_model(&side, dx, sy, dw, selected);
    sy += 162;
    label(&side, dx, sy, "Computed", T->text, 12, MX_TEXT_BOLD);
    sy += 20;
    const mx_style_t* s = selected->style;
    if (s) {
        static const char* const displays[] = {"inline", "block", "inline-block", "list-item", "flex",
                                               "inline-flex", "grid", "table", "table-row",
                                               "table-cell", "table-row-group", "table-caption",
                                               "none", "contents"};
        static const char* const positions[] = {"static", "relative", "absolute", "fixed", "sticky"};
        char value[96];
        struct { const char* name; char text[96]; uint32_t swatch; } props[8];
        int pc = 0;
        mx_copy(props[pc].text, 96, displays[s->display < 14 ? s->display : 1]);
        props[pc].name = "display"; props[pc++].swatch = 0;
        mx_copy(props[pc].text, 96, positions[s->position < 5 ? s->position : 0]);
        props[pc].name = "position"; props[pc++].swatch = 0;
        value[0] = '\0';
        mx_append_int(value, sizeof(value), selected->box_w);
        mx_append(value, sizeof(value), " \xC3\x97 ");
        mx_append_int(value, sizeof(value), selected->box_h);
        mx_append(value, sizeof(value), " at ");
        mx_append_int(value, sizeof(value), selected->box_x);
        mx_append(value, sizeof(value), ", ");
        mx_append_int(value, sizeof(value), selected->box_y);
        mx_copy(props[pc].text, 96, value);
        props[pc].name = "box"; props[pc++].swatch = 0;
        color_text(s->color, props[pc].text, 96);
        props[pc].name = "color"; props[pc++].swatch = s->color | 0xFF000000u;
        color_text(s->background, props[pc].text, 96);
        props[pc].name = "background"; props[pc++].swatch = s->background;
        value[0] = '\0';
        mx_append_int(value, sizeof(value), s->font_size);
        mx_append(value, sizeof(value), "px ");
        mx_append_int(value, sizeof(value), s->font_weight);
        mx_append(value, sizeof(value), s->mono ? " monospace" : " sans-serif");
        if (s->italic) mx_append(value, sizeof(value), " italic");
        mx_copy(props[pc].text, 96, value);
        props[pc].name = "font"; props[pc++].swatch = 0;
        for (int i = 0; i < pc && sy < top + body_h; i++) {
            label(&side, dx, sy, props[i].name, T->code_attr, MONO, MX_TEXT_MONO);
            int vx = dx + 96;
            if (props[i].swatch >> 24) {
                mx_round_rect(&side, vx, sy + 1, 12, 12, 3, props[i].swatch);
                mx_round_rect_stroke(&side, vx, sy + 1, 12, 12, 3, 1, T->border);
                vx += 18;
            }
            label(&side, vx, sy, props[i].text, T->code_text, MONO, MX_TEXT_MONO);
            sy += 18;
        }
    }
    sy += 8;
    if (sy < top + body_h) {
        label(&side, dx, sy, "Matched rules", T->text, 12, MX_TEXT_BOLD);
        sy += 20;
        static char rules[4096];
        mx_css_describe_rules(selected, rules, sizeof(rules));
        const char* inline_style = mx_dom_attr(selected, "style");
        if (inline_style && *inline_style) {
            char line[200] = "style=\"";
            mx_append(line, sizeof(line), inline_style);
            mx_append(line, sizeof(line), "\"");
            mx_text_ellipsis(&side, dx, sy, dw, line, T->code_value, MONO, MX_TEXT_MONO);
            sy += 16;
        }
        if (!rules[0]) label(&side, dx, sy, "No page rules (browser defaults only)", T->subtle, 12, 0);
        const char* p = rules;
        while (*p && sy < top + body_h) {
            const char* end = strchr(p, '\n');
            size_t n = end ? (size_t)(end - p) : strlen(p);
            char line[200];
            mx_copy_n(line, sizeof(line), p, n);
            uint32_t color = line[0] == ' ' ? T->code_text : line[0] == '}' ? T->muted : T->code_tag;
            mx_text_ellipsis(&side, dx, sy, dw, line, color, MONO, MX_TEXT_MONO);
            sy += 16;
            p = end ? end + 1 : p + n;
        }
    }
}

void mx_devtools_pick(mx_node_t* node, int commit) {
    while (node && node->type != MX_NODE_ELEMENT) node = node->parent;
    if (!commit) {
        hovered = node;
        mx_html_set_highlight(node);
        return;
    }
    selected = node;
    picking = 0;
    mx_js_set_inspected(node);
    for (mx_node_t* p = node ? node->parent : NULL; p; p = p->parent) p->devtools_open = 1;
    /* scroll the tree to the selection */
    int count = collect_tree_rows();
    for (int i = 0; i < count; i++)
        if (tree_rows[i].node == node && !tree_rows[i].closing) {
            int visible = (panel_h - HEADER - 34) / ROW;
            if (i < tree_scroll || i >= tree_scroll + visible) tree_scroll = i - visible / 2;
            break;
        }
    mx_html_set_highlight(node);
}

/* ====================================================================== */
/* Network                                                                */
/* ====================================================================== */

static mx_icon_t kind_icon(const char* kind) {
    if (!strcmp(kind, "stylesheet")) return MX_ICON_CSS;
    if (!strcmp(kind, "script")) return MX_ICON_JAVASCRIPT;
    if (!strcmp(kind, "fetch")) return MX_ICON_DATA_OBJECT;
    return MX_ICON_DESCRIPTION;
}

static void size_text(size_t bytes, char* out, size_t capacity) {
    out[0] = '\0';
    if (bytes < 1024) {
        mx_append_int(out, capacity, (long)bytes);
        mx_append(out, capacity, " B");
    } else if (bytes < 1024 * 1024) {
        mx_append_int(out, capacity, (long)(bytes / 1024));
        mx_append(out, capacity, ".");
        mx_append_int(out, capacity, (long)(bytes % 1024 * 10 / 1024));
        mx_append(out, capacity, " kB");
    } else {
        mx_append_int(out, capacity, (long)(bytes / (1024 * 1024)));
        mx_append(out, capacity, ".");
        mx_append_int(out, capacity, (long)(bytes % (1024 * 1024) * 10 / (1024 * 1024)));
        mx_append(out, capacity, " MB");
    }
}

static void network_draw(const mx_canvas_t* c, int x, int y, int w, int h) {
    int count = mx_network_count();
    int cols[6] = {x + 12, 0, 0, 0, 0, 0};
    int name_w = w * 34 / 100;
    cols[1] = cols[0] + name_w;
    cols[2] = cols[1] + 70;
    cols[3] = cols[2] + 90;
    cols[4] = cols[3] + 80;
    cols[5] = cols[4] + 70;
    static const char* const heads[] = {"Name", "Status", "Type", "Size", "Time", "Waterfall"};
    mx_fill(c, x, y, w, 26, stripe_color());
    for (int i = 0; i < 6; i++) label(c, cols[i], y + 7, heads[i], T->muted, 12, MX_TEXT_MEDIUM);
    mx_fill(c, x, y + 26, w, 1, T->divider);
    int detail_h = network_selected >= 0 && network_selected < count ? 70 : 0;
    int top = y + 27;
    int bottom = y + h - 24 - detail_h;
    long first = 0, last = 1;
    size_t total = 0;
    for (int i = 0; i < count; i++) {
        const mx_network_entry_t* e = mx_network_get(i);
        if (!i || e->started_ms < first) first = e->started_ms;
        if (e->started_ms + e->duration_ms > last) last = e->started_ms + e->duration_ms;
        total += e->size;
    }
    long span = last - first > 1 ? last - first : 1;
    int visible = (bottom - top) / ROW;
    if (network_scroll > count - visible) network_scroll = count - visible;
    if (network_scroll < 0) network_scroll = 0;
    mx_canvas_t list = mx_canvas_clip(c, x, top, w, bottom - top);
    for (int i = network_scroll; i < count && i < network_scroll + visible + 1; i++) {
        const mx_network_entry_t* e = mx_network_get(i);
        int ry = top + (i - network_scroll) * ROW;
        if (i == network_selected) mx_fill(&list, x, ry, w, ROW, T->accent_soft);
        else if (i % 2) mx_fill(&list, x, ry, w, ROW, stripe_color());
        int ty = ry + (ROW - mx_text_height_px(12, 0)) / 2;
        int failed = e->status == 0 || e->status >= 400;
        mx_icon(&list, kind_icon(e->kind), 16, cols[0], ry + 3, failed ? T->danger : T->muted);
        const char* name = strrchr(e->url, '/');
        name = name && name[1] ? name + 1 : e->url;
        mx_canvas_t cell = mx_canvas_clip(&list, cols[0] + 22, ry, name_w - 28, ROW);
        mx_text_ellipsis(&cell, cols[0] + 22, ty, name_w - 28, name, failed ? T->danger : T->text, 12, 0);
        char text[48] = "";
        if (e->status) mx_append_int(text, sizeof(text), e->status);
        else mx_copy(text, sizeof(text), "failed");
        label(&list, cols[1], ty, text, failed ? T->danger : e->status >= 300 ? T->muted : T->success, 12, 0);
        label(&list, cols[2], ty, e->kind, T->muted, 12, 0);
        if (e->from_cache) mx_copy(text, sizeof(text), "(cache)");
        else size_text(e->size, text, sizeof(text));
        label(&list, cols[3], ty, text, T->text, 12, 0);
        text[0] = '\0';
        mx_append_int(text, sizeof(text), e->duration_ms);
        mx_append(text, sizeof(text), " ms");
        label(&list, cols[4], ty, text, T->text, 12, 0);
        int wf_w = x + w - 12 - cols[5];
        if (wf_w > 20) {
            int bx = cols[5] + (int)((e->started_ms - first) * wf_w / span);
            int bw = (int)(e->duration_ms * wf_w / span);
            if (bw < 3) bw = 3;
            mx_round_rect(&list, bx, ry + 7, bw, 8, 3, failed ? T->danger : T->accent);
        }
    }
    if (!count) label(c, x + 16, top + 12, "No requests yet. Load a page to see its network activity.",
                      T->subtle, 12, 0);
    if (detail_h) {
        const mx_network_entry_t* e = mx_network_get(network_selected);
        int dy = bottom;
        mx_fill(c, x, dy, w, detail_h, stripe_color());
        mx_fill(c, x, dy, w, 1, T->divider);
        mx_canvas_t clip = mx_canvas_clip(c, x + 12, dy, w - 24, detail_h);
        mx_text_ellipsis(&clip, x + 12, dy + 8, w - 24, e->url, T->text, 12, MX_TEXT_MONO);
        char line[200] = "Content type: ";
        mx_append(line, sizeof(line), e->content_type[0] ? e->content_type : "unknown");
        mx_append(line, sizeof(line), "    Redirects: ");
        mx_append_int(line, sizeof(line), e->redirects);
        label(&clip, x + 12, dy + 28, line, T->muted, 12, 0);
        if (e->error[0]) mx_text_ellipsis(&clip, x + 12, dy + 46, w - 24, e->error, T->danger, 12, 0);
    }
    int fy = y + h - 24;
    mx_fill(c, x, fy, w, 24, stripe_color());
    mx_fill(c, x, fy, w, 1, T->divider);
    char footer[96] = "";
    mx_append_int(footer, sizeof(footer), count);
    mx_append(footer, sizeof(footer), count == 1 ? " request  \xC2\xB7  " : " requests  \xC2\xB7  ");
    char size[24];
    size_text(total, size, sizeof(size));
    mx_append(footer, sizeof(footer), size);
    mx_append(footer, sizeof(footer), " transferred  \xC2\xB7  ");
    mx_append_int(footer, sizeof(footer), span > 1 ? span : 0);
    mx_append(footer, sizeof(footer), " ms");
    label(c, x + 12, fy + 5, footer, T->muted, 12, 0);
}

/* ====================================================================== */
/* Sources                                                                */
/* ====================================================================== */

static void index_lines(const char* source, size_t length) {
    if (source == cached_source && length == cached_length) return;
    free(line_offsets);
    line_offsets = NULL;
    line_count = 0;
    cached_source = source;
    cached_length = length;
    if (!source) return;
    int lines = 1;
    for (size_t i = 0; i < length; i++) lines += source[i] == '\n';
    line_offsets = (uint32_t*)malloc(sizeof(uint32_t) * (size_t)(lines + 1));
    if (!line_offsets) return;
    line_offsets[0] = 0;
    int n = 1;
    for (size_t i = 0; i < length; i++)
        if (source[i] == '\n') line_offsets[n++] = (uint32_t)(i + 1);
    line_offsets[n] = (uint32_t)length + 1;
    line_count = n;
}

/* HTML syntax colouring for one line; `state` carries comments/tags across. */
static void draw_source_line(const mx_canvas_t* c, int x, int y, const char* s, size_t n, int* state) {
    size_t i = 0;
    while (i < n) {
        size_t start = i;
        uint32_t color = T->code_text;
        if (*state == 2) {   /* inside a comment */
            while (i < n && !(i + 2 < n && s[i] == '-' && s[i + 1] == '-' && s[i + 2] == '>')) i++;
            if (i < n) {
                i += 3;
                *state = 0;
            }
            color = T->code_comment;
        } else if (*state == 1) {   /* inside a tag */
            if (s[i] == '>') {
                i++;
                *state = 0;
                color = T->code_tag;
            } else if (s[i] == '"' || s[i] == '\'') {
                char q = s[i++];
                while (i < n && s[i] != q) i++;
                if (i < n) i++;
                color = T->code_value;
            } else if (mx_is_space(s[i]) || s[i] == '=' || s[i] == '/') {
                i++;
                color = T->code_tag;
            } else {
                while (i < n && !mx_is_space(s[i]) && s[i] != '=' && s[i] != '>') i++;
                color = T->code_attr;
            }
        } else if (s[i] == '<' && i + 3 < n && s[i + 1] == '!' && s[i + 2] == '-' && s[i + 3] == '-') {
            *state = 2;
            i += 4;
            color = T->code_comment;
        } else if (s[i] == '<') {
            i++;
            while (i < n && (s[i] == '/' || s[i] == '!' || (!mx_is_space(s[i]) && s[i] != '>'))) i++;
            *state = 1;
            color = T->code_tag;
        } else {
            while (i < n && s[i] != '<') i++;
        }
        if (i == start) i++;
        char buffer[256];
        size_t len = i - start;
        size_t k = 0;
        for (size_t j = 0; j < len && k + 4 < sizeof(buffer); j++) {
            if (s[start + j] == '\t') {
                memcpy(buffer + k, "    ", 4);
                k += 4;
            } else if (s[start + j] != '\r') {
                buffer[k++] = s[start + j];
            }
        }
        buffer[k] = '\0';
        mx_text(c, x, y, buffer, k, color, MONO, MX_TEXT_MONO);
        x += mx_text_width_px(buffer, k, MONO, MX_TEXT_MONO);
        if (x > c->clip_x1) return;
    }
}

static void sources_draw(const mx_canvas_t* c, int x, int y, int w, int h,
                         const mx_devtools_context_t* ctx) {
    mx_fill(c, x, y, w, 26, stripe_color());
    const char* name = ctx->url ? strrchr(ctx->url, '/') : NULL;
    char header[200] = "";
    mx_append(header, sizeof(header), name && name[1] ? name + 1 : ctx->url ? ctx->url : "page");
    index_lines(ctx->source, ctx->source_length);
    char info[64] = "";
    mx_append_int(info, sizeof(info), line_count);
    mx_append(info, sizeof(info), " lines  \xC2\xB7  ");
    char size[24];
    size_text(ctx->source_length, size, sizeof(size));
    mx_append(info, sizeof(info), size);
    mx_icon(c, MX_ICON_HTML, 20, x + 8, y + 3, T->accent);
    label(c, x + 34, y + 6, header, T->text, 12, MX_TEXT_MEDIUM);
    label(c, x + w - text_w(info, 12, 0) - 12, y + 6, info, T->muted, 12, 0);
    mx_fill(c, x, y + 26, w, 1, T->divider);
    int top = y + 27;
    if (!ctx->source || !line_offsets) {
        label(c, x + 16, top + 12, "No source for this page.", T->subtle, 12, 0);
        return;
    }
    int line_h = 17;
    int visible = (y + h - top) / line_h;
    if (source_scroll > line_count - visible) source_scroll = line_count - visible;
    if (source_scroll < 0) source_scroll = 0;
    char digits[16] = "";
    mx_append_int(digits, sizeof(digits), line_count);
    int gutter = text_w(digits, MONO, MX_TEXT_MONO) + 24;
    mx_fill(c, x, top, gutter, y + h - top, stripe_color());
    mx_canvas_t text = mx_canvas_clip(c, x + gutter, top, w - gutter, y + h - top);
    /* colouring state from the top of the file would be exact; approximate
     * by scanning a little before the first visible line */
    int state = 0;
    int scan_from = source_scroll > 40 ? source_scroll - 40 : 0;
    for (int l = scan_from; l < source_scroll; l++) {
        const char* s = ctx->source + line_offsets[l];
        size_t n = line_offsets[l + 1] - line_offsets[l] - 1;
        for (size_t i = 0; i < n; i++) {
            if (state == 2) {
                if (i + 2 < n && s[i] == '-' && s[i + 1] == '-' && s[i + 2] == '>') state = 0;
            } else if (s[i] == '<') {
                state = i + 3 < n && s[i + 1] == '!' && s[i + 2] == '-' && s[i + 3] == '-' ? 2 : 1;
            } else if (state == 1 && s[i] == '>') {
                state = 0;
            }
        }
    }
    for (int l = source_scroll; l < line_count && l < source_scroll + visible + 1; l++) {
        int ly = top + (l - source_scroll) * line_h + 1;
        char number[16] = "";
        mx_append_int(number, sizeof(number), l + 1);
        label(c, x + gutter - 12 - text_w(number, MONO, MX_TEXT_MONO), ly, number, T->subtle, MONO,
              MX_TEXT_MONO);
        const char* s = ctx->source + line_offsets[l];
        size_t n = line_offsets[l + 1] - line_offsets[l] - 1;
        if (n > 400) n = 400;
        draw_source_line(&text, x + gutter + 8, ly, s, n, &state);
    }
}

/* ====================================================================== */
/* Performance                                                            */
/* ====================================================================== */

static void performance_draw(const mx_canvas_t* c, int x, int y, int w, int h,
                             const mx_devtools_context_t* ctx) {
    const mx_html_stats_t* s = mx_html_stats();
    struct { const char* name; long ms; } timings[] = {
        {"Parse HTML", s->parse_ms}, {"Style", s->style_ms}, {"Layout", s->layout_ms},
        {"Scripts", s->script_ms}, {"Network (subresources)", s->fetch_ms},
        {"Last frame", ctx->last_frame_ms},
    };
    int n = (int)(sizeof(timings) / sizeof(timings[0]));
    long max = 1;
    for (int i = 0; i < n; i++) if (timings[i].ms > max) max = timings[i].ms;
    int col_w = w / 2 - 24;
    int sy = y + 12;
    label(c, x + 16, sy, "Timings", T->text, 13, MX_TEXT_BOLD);
    sy += 24;
    for (int i = 0; i < n && sy + 20 < y + h; i++) {
        label(c, x + 16, sy, timings[i].name, T->muted, 12, 0);
        char value[32] = "";
        mx_append_int(value, sizeof(value), timings[i].ms);
        mx_append(value, sizeof(value), " ms");
        label(c, x + 16 + col_w - text_w(value, 12, MX_TEXT_MEDIUM), sy, value, T->text, 12, MX_TEXT_MEDIUM);
        int bar_w = col_w - 150;
        if (bar_w > 20) {
            mx_round_rect(c, x + 160, sy + 4, bar_w - 70, 8, 4, T->divider);
            int fill = (int)((bar_w - 70) * timings[i].ms / max);
            if (fill > 0) mx_round_rect(c, x + 160, sy + 4, fill < 8 ? 8 : fill, 8, 4, T->accent);
        }
        sy += 22;
    }
    int cx = x + w / 2 + 8;
    sy = y + 12;
    label(c, cx, sy, "Page", T->text, 13, MX_TEXT_BOLD);
    sy += 24;
    char size_a[24], size_b[24];
    size_text(s->dom_bytes, size_a, sizeof(size_a));
    size_text(s->js_bytes, size_b, sizeof(size_b));
    struct { const char* name; long value; const char* text; } counters[] = {
        {"Elements", (long)s->elements, NULL}, {"CSS rules", (long)s->rules, NULL},
        {"Display items", (long)s->items, NULL}, {"Scripts", (long)s->scripts, NULL},
        {"Stylesheets", (long)s->stylesheets, NULL}, {"Layout passes", (long)s->layouts, NULL},
        {"Active timers", (long)mx_js_timer_count(), NULL}, {"Document memory", 0, size_a},
        {"Script heap", 0, size_b},
    };
    int cn = (int)(sizeof(counters) / sizeof(counters[0]));
    for (int i = 0; i < cn && sy + 18 < y + h; i++) {
        label(c, cx, sy, counters[i].name, T->muted, 12, 0);
        char value[32] = "";
        if (counters[i].text) mx_copy(value, sizeof(value), counters[i].text);
        else mx_append_int(value, sizeof(value), counters[i].value);
        label(c, cx + col_w - text_w(value, 12, MX_TEXT_MEDIUM), sy, value, T->text, 12, MX_TEXT_MEDIUM);
        sy += 20;
    }
}

/* ====================================================================== */
/* Panel                                                                  */
/* ====================================================================== */

static void tab_geometry(int index, int* tx, int* tw) {
    int x = panel_x + 8;
    for (int i = 0; i < MX_DEVTOOLS_TAB_COUNT; i++) {
        int w = 16 + 6 + text_w(tab_names[i], 12, MX_TEXT_MEDIUM) + 24;
        if (i == index) {
            *tx = x;
            *tw = w;
            return;
        }
        x += w;
    }
    *tx = x;
    *tw = 0;
}

void mx_devtools_draw(const mx_canvas_t* canvas, int x, int y, int w, int h,
                      const mx_devtools_context_t* ctx) {
    T = ctx->theme;
    panel_x = x;
    panel_y = y;
    panel_w = w;
    panel_h = h;
    mx_canvas_t c = mx_canvas_clip(canvas, x, y, w, h);
    mx_fill(&c, x, y, w, h, body_color());
    mx_fill(&c, x, y, w, HEADER, T->toolbar);
    mx_fill(&c, x, y, w, 1, resizing ? T->accent : T->border);
    mx_fill(&c, x, y + HEADER - 1, w, 1, T->divider);
    for (int i = 0; i < MX_DEVTOOLS_TAB_COUNT; i++) {
        int tx, tw;
        tab_geometry(i, &tx, &tw);
        int on = i == current_tab;
        uint32_t color = on ? T->accent : T->muted;
        mx_icon(&c, tab_icons[i], 16, tx + 12, y + (HEADER - 16) / 2, color);
        label(&c, tx + 34, y + (HEADER - mx_text_height_px(12, MX_TEXT_MEDIUM)) / 2, tab_names[i],
              on ? T->text : T->muted, 12, MX_TEXT_MEDIUM);
        if (on) mx_round_rect(&c, tx + 8, y + HEADER - 3, tw - 16, 3, 1, T->accent);
    }
    /* right side: counts, clear, close */
    int rx = x + w - 34;
    mx_icon(&c, MX_ICON_CLOSE, 20, rx + 7, y + 8, T->muted);
    rx -= 32;
    if (current_tab == MX_DEVTOOLS_CONSOLE || current_tab == MX_DEVTOOLS_NETWORK) {
        mx_icon(&c, MX_ICON_BLOCK, 20, rx + 6, y + 8, T->muted);
        rx -= 16;
    }
    int errors = mx_console_count_level(MX_LOG_ERROR), warnings = mx_console_count_level(MX_LOG_WARN);
    char text[16];
    if (warnings) {
        text[0] = '\0';
        mx_append_int(text, sizeof(text), warnings);
        int tw = text_w(text, 12, MX_TEXT_MEDIUM);
        rx -= tw + 6;
        label(&c, rx + 20, y + 11, text, T->warning, 12, MX_TEXT_MEDIUM);
        mx_icon(&c, MX_ICON_WARNING, 16, rx, y + 10, T->warning);
        rx -= 24;
    }
    if (errors) {
        text[0] = '\0';
        mx_append_int(text, sizeof(text), errors);
        int tw = text_w(text, 12, MX_TEXT_MEDIUM);
        rx -= tw + 6;
        label(&c, rx + 20, y + 11, text, T->danger, 12, MX_TEXT_MEDIUM);
        mx_icon(&c, MX_ICON_ERROR, 16, rx, y + 10, T->danger);
    }
    int by = y + HEADER, bh = h - HEADER;
    switch (current_tab) {
    case MX_DEVTOOLS_CONSOLE: console_draw(&c, x, by, w, bh); break;
    case MX_DEVTOOLS_ELEMENTS: elements_draw(&c, x, by, w, bh); break;
    case MX_DEVTOOLS_NETWORK: network_draw(&c, x, by, w, bh); break;
    case MX_DEVTOOLS_SOURCES: sources_draw(&c, x, by, w, bh, ctx); break;
    default: performance_draw(&c, x, by, w, bh, ctx); break;
    }
}

/* ====================================================================== */
/* Input                                                                  */
/* ====================================================================== */

static int in_panel(int x, int y) {
    return x >= panel_x && x < panel_x + panel_w && y >= panel_y && y < panel_y + panel_h;
}

int mx_devtools_mouse(int x, int y, int left_down, int pressed, int released) {
    if (resizing) {
        if (left_down) {
            int new_top = y - resize_offset;
            int bottom = panel_y + panel_h;
            int height = bottom - new_top;
            if (height < MX_BROWSER_DEVTOOLS_MIN) height = MX_BROWSER_DEVTOOLS_MIN;
            if (height > bottom - MX_BROWSER_PAGE_Y - 80) height = bottom - MX_BROWSER_PAGE_Y - 80;
            panel_height = height;
            return 1;
        }
        if (released || !left_down) {
            resizing = 0;
            return 1;
        }
    }
    if (!in_panel(x, y)) {
        if (hovered && current_tab == MX_DEVTOOLS_ELEMENTS && !picking) {
            hovered = NULL;
            mx_html_set_highlight(selected);
            return 1;
        }
        return 0;
    }
    if (pressed && y < panel_y + 5) {
        resizing = 1;
        resize_offset = y - panel_y;
        return 1;
    }
    if (y < panel_y + HEADER) {
        if (!pressed) return 0;
        if (x >= panel_x + panel_w - 34) {
            close_request = 1;
            return 1;
        }
        if ((current_tab == MX_DEVTOOLS_CONSOLE || current_tab == MX_DEVTOOLS_NETWORK) &&
            x >= panel_x + panel_w - 66 && x < panel_x + panel_w - 34) {
            if (current_tab == MX_DEVTOOLS_CONSOLE) mx_console_clear();
            else mx_network_clear();
            network_selected = -1;
            return 1;
        }
        for (int i = 0; i < MX_DEVTOOLS_TAB_COUNT; i++) {
            int tx, tw;
            tab_geometry(i, &tx, &tw);
            if (x >= tx && x < tx + tw) {
                current_tab = i;
                console_focused = 0;
                return 1;
            }
        }
        return 0;
    }
    int body_y = panel_y + HEADER;
    switch (current_tab) {
    case MX_DEVTOOLS_CONSOLE: {
        if (!pressed) return 0;
        if (y < body_y + 34) {
            int fx = panel_x + 10;
            static const char* const filters[] = {"All", "Errors", "Warnings", "Info", "Logs"};
            for (int i = 0; i < 5; i++) {
                int cw = text_w(filters[i], 12, MX_TEXT_MEDIUM) + 20;
                if (x >= fx && x < fx + cw) {
                    console_filter = i;
                    console_scroll = 0;
                    return 1;
                }
                fx += cw + 6;
            }
            return 0;
        }
        console_focused = y >= panel_y + panel_h - 30 || console_focused;
        if (y >= panel_y + panel_h - 30) {
            console_focused = 1;
            console_cursor = (int)strlen(console_input);
        }
        return 1;
    }
    case MX_DEVTOOLS_ELEMENTS: {
        if (y < body_y + 34) {
            if (pressed && x >= panel_x + 6 && x < panel_x + 38) {
                picking = !picking;
                if (!picking) mx_html_set_highlight(selected);
                return 1;
            }
            return 0;
        }
        int tree_w = panel_w * 58 / 100;
        if (x >= panel_x + tree_w) return 0;
        int count = collect_tree_rows();
        int index = tree_scroll + (y - (body_y + 34)) / ROW;
        mx_node_t* n = index >= 0 && index < count ? tree_rows[index].node : NULL;
        if (!pressed) {
            if (n != hovered && !picking) {
                hovered = n && n->type == MX_NODE_ELEMENT ? n : n ? n->parent : NULL;
                mx_html_set_highlight(hovered ? hovered : selected);
                return 1;
            }
            return 0;
        }
        if (!n) return 0;
        int depth = tree_rows[index].depth;
        int arrow_x = panel_x + 8 + depth * 14;
        if (n->type == MX_NODE_ELEMENT && x >= arrow_x - 2 && x < arrow_x + 18 && !tree_rows[index].closing) {
            n->devtools_open = !n->devtools_open;
            return 1;
        }
        selected = n->type == MX_NODE_ELEMENT ? n : n->parent;
        mx_js_set_inspected(selected);
        mx_html_set_highlight(selected);
        return 1;
    }
    case MX_DEVTOOLS_NETWORK: {
        if (!pressed) return 0;
        int index = network_scroll + (y - (body_y + 27)) / ROW;
        if (y >= body_y + 27 && index >= 0 && index < mx_network_count()) {
            network_selected = network_selected == index ? -1 : index;
            return 1;
        }
        return 0;
    }
    default:
        return 0;
    }
}

int mx_devtools_wheel(int x, int y, int delta) {
    if (!in_panel(x, y)) return 0;
    int lines = delta < 0 ? 3 : -3;   /* wheel down scrolls down */
    switch (current_tab) {
    case MX_DEVTOOLS_CONSOLE:
        console_scroll -= lines > 0 ? 1 : -1;
        if (console_scroll < 0) console_scroll = 0;
        if (console_scroll > mx_console_count() - 1) console_scroll = mx_console_count() - 1;
        if (console_scroll < 0) console_scroll = 0;
        break;
    case MX_DEVTOOLS_ELEMENTS: tree_scroll += lines; break;
    case MX_DEVTOOLS_NETWORK: network_scroll += lines; break;
    case MX_DEVTOOLS_SOURCES: source_scroll += lines * 2; break;
    default: break;
    }
    return 1;
}

int mx_devtools_key(int key, char ascii, int mods) {
    if (!console_focused) return 0;
    size_t length = strlen(console_input);
    if (key == 40) {   /* Enter */
        console_submit();
        return 1;
    }
    if (key == 41) {   /* Escape */
        console_focused = 0;
        return 1;
    }
    if (key == 42) {   /* Backspace */
        if (console_cursor > 0) {
            memmove(console_input + console_cursor - 1, console_input + console_cursor,
                    length - (size_t)console_cursor + 1);
            console_cursor--;
        }
        return 1;
    }
    if (key == 76) {   /* Delete */
        if (console_cursor < (int)length)
            memmove(console_input + console_cursor, console_input + console_cursor + 1,
                    length - (size_t)console_cursor);
        return 1;
    }
    if (key == 80) { if (console_cursor > 0) console_cursor--; return 1; }
    if (key == 79) { if (console_cursor < (int)length) console_cursor++; return 1; }
    if (key == 74) { console_cursor = 0; return 1; }
    if (key == 77) { console_cursor = (int)length; return 1; }
    if (key == 82 || key == 81) {   /* history */
        if (!history_count) return 1;
        if (key == 82) history_index = history_index < 0 ? history_count - 1 : history_index > 0 ? history_index - 1 : 0;
        else history_index = history_index < 0 ? -1 : history_index + 1 >= history_count ? -1 : history_index + 1;
        if (history_index < 0) console_input[0] = '\0';
        else mx_copy(console_input, sizeof(console_input), console_history[history_index]);
        console_cursor = (int)strlen(console_input);
        return 1;
    }
    if ((mods & 2) && key == 15) {   /* Ctrl+L clears like browsers */
        mx_console_clear();
        return 1;
    }
    if (ascii >= 32 && ascii < 127 && length + 1 < sizeof(console_input)) {
        memmove(console_input + console_cursor + 1, console_input + console_cursor,
                length - (size_t)console_cursor + 1);
        console_input[console_cursor++] = ascii;
        return 1;
    }
    return 1;
}
