#include "mxplorer/html.h"
#include "mxplorer/css.h"
#include "mxplorer/devlog.h"
#include "mxplorer/js.h"
#include "mxplorer/media.h"
#include "mxplorer/text.h"
#include "string.h"
#include "stdlib.h"

/* ====================================================================== */
/* Display list                                                           */
/* ====================================================================== */

enum {
    IT_RECT,        /* background (radius) */
    IT_BORDER,      /* uniform border: px = width */
    IT_TEXT,
    IT_ICON,        /* px = icon id, w = size */
    IT_CHECKBOX,
    IT_RADIO,
    IT_CARET,
    IT_MEDIA        /* picture or video of `node`, drawn by media.c */
};

typedef struct {
    uint8_t kind;
    uint8_t flags;      /* MX_TEXT_* */
    uint8_t deco;       /* MX_DECO_* */
    uint8_t part;       /* decoration role: PART_* */
    int clip;
    int x, y, w, h;
    uint32_t color;
    int px;
    int radius;
    const char* text;
    uint32_t length;
    mx_node_t* node;
} item_t;

typedef struct {
    int x0, y0, x1, y1;
    unsigned created_at;   /* item_count when the clip was made */
} clip_t;

enum { PART_NONE, PART_FULL, PART_TOP, PART_BOTTOM, PART_LEFT, PART_RIGHT };

static mx_document_t doc;
static int doc_loaded;
static mx_arena_t layout_arena;
static item_t* items;
static unsigned item_count, item_capacity;
static clip_t* clips;
static int clip_count, clip_capacity;
static mx_node_t** laid;
static unsigned laid_count, laid_capacity;
static int current_clip;
static unsigned current_opacity = 255;
static uint32_t layout_generation = 1;
static int viewport_w = 1024, viewport_h = 768, prefers_dark;
static int content_height;
static int scroll_y;
static uint32_t page_background = 0xFFFFFFFFu;
static mx_html_stats_t stats;
static mx_node_t* focused;
static mx_node_t* highlighted;
static int scripts_enabled;
static char resolved_link[2048];
static uint32_t accent = 0xFF1A73E8u;

/* containing blocks for positioned elements */
typedef struct {
    mx_node_t* node;
    int x, y, w, h;
} cb_t;
static cb_t cb_stack[32];
static int cb_depth;

static void* grow(void* old, unsigned count, unsigned* capacity, size_t size) {
    unsigned next = *capacity ? *capacity * 2 : 256;
    void* fresh = mx_arena_alloc(&layout_arena, size * next);
    if (!fresh) return NULL;
    if (old && count) memcpy(fresh, old, size * count);
    *capacity = next;
    return fresh;
}

static int push_item(uint8_t kind, int x, int y, int w, int h, uint32_t color,
                     mx_node_t* node) {
    if (item_count == item_capacity) {
        item_t* bigger = (item_t*)grow(items, item_count, &item_capacity,
                                       sizeof(item_t));
        if (!bigger) return -1;
        items = bigger;
    }
    if (current_opacity < 255) color = mx_with_alpha(color, current_opacity);
    item_t* it = &items[item_count];
    memset(it, 0, sizeof(*it));
    it->kind = kind;
    it->x = x;
    it->y = y;
    it->w = w;
    it->h = h;
    it->color = color;
    it->node = node;
    it->clip = current_clip;
    return (int)item_count++;
}

static void record_laid(mx_node_t* node) {
    if (laid_count == laid_capacity) {
        mx_node_t** bigger = (mx_node_t**)grow(laid, laid_count, &laid_capacity,
                                               sizeof(mx_node_t*));
        if (!bigger) return;
        laid = bigger;
    }
    laid[laid_count++] = node;
}

static int push_clip(int x, int y, int w, int h) {
    if (clip_count == clip_capacity) {
        unsigned capacity = (unsigned)clip_capacity;
        clip_t* bigger = (clip_t*)grow(clips, (unsigned)clip_count, &capacity,
                                       sizeof(clip_t));
        if (!bigger) return current_clip;
        clips = bigger;
        clip_capacity = (int)capacity;
    }
    clip_t parent = clips[current_clip];
    clip_t c = {x, y, x + w, y + h, item_count};
    if (c.x0 < parent.x0) c.x0 = parent.x0;
    if (c.y0 < parent.y0) c.y0 = parent.y0;
    if (c.x1 > parent.x1) c.x1 = parent.x1;
    if (c.y1 > parent.y1) c.y1 = parent.y1;
    clips[clip_count] = c;
    return clip_count++;
}

static void shift_range(unsigned item_start, unsigned item_end,
                        unsigned laid_start, unsigned laid_end, int dx, int dy) {
    if (!dx && !dy) return;
    for (unsigned i = item_start; i < item_end; i++) {
        items[i].x += dx;
        items[i].y += dy;
    }
    for (unsigned i = laid_start; i < laid_end; i++) {
        laid[i]->box_x += dx;
        laid[i]->box_y += dy;
    }
    /* clips created inside the range move with it */
    for (int i = 1; i < clip_count && item_end > item_start; i++) {
        if (clips[i].created_at < item_start || clips[i].created_at >= item_end) continue;
        clips[i].x0 += dx;
        clips[i].x1 += dx;
        clips[i].y0 += dy;
        clips[i].y1 += dy;
    }
}

/* ====================================================================== */
/* Styles                                                                 */
/* ====================================================================== */

static mx_style_t root_parent_style;

static void compute_styles(mx_node_t* node, const mx_style_t* parent) {
    for (mx_node_t* c = node->first; c; c = c->next) {
        if (c->type != MX_NODE_ELEMENT) {
            c->style = (mx_style_t*)parent;
            continue;
        }
        mx_style_t* s = (mx_style_t*)mx_arena_alloc(&layout_arena, sizeof(*s));
        if (!s) return;
        mx_css_compute(&layout_arena, c, parent, s);
        c->style = s;
        if (s->display == MX_DISPLAY_NONE) continue;
        mx_css_push_ancestor(c);
        compute_styles(c, s);
        mx_css_pop_ancestor(c);
    }
}

static int is_hidden(const mx_node_t* n) {
    return !n->style || n->style->display == MX_DISPLAY_NONE;
}

static int is_replaced(const mx_node_t* n) {
    const char* t = n->tag;
    return !strcmp(t, "img") || !strcmp(t, "video") || !strcmp(t, "input") || !strcmp(t, "select") ||
           !strcmp(t, "textarea") || !strcmp(t, "progress") || !strcmp(t, "meter");
}

static int is_out_of_flow(const mx_node_t* n) {
    return n->type == MX_NODE_ELEMENT && n->style &&
           (n->style->position == MX_POS_ABSOLUTE ||
            n->style->position == MX_POS_FIXED);
}

static int is_inline_level(const mx_node_t* n) {
    if (n->type == MX_NODE_TEXT) return 1;
    if (n->type != MX_NODE_ELEMENT || is_hidden(n)) return 0;
    if (is_out_of_flow(n)) return 0;
    const mx_style_t* s = n->style;
    if (s->float_side) return 1;
    return s->display == MX_DISPLAY_INLINE || s->display == MX_DISPLAY_INLINE_BLOCK ||
           s->display == MX_DISPLAY_INLINE_FLEX;
}

static int is_atomic_inline(const mx_node_t* n) {
    const mx_style_t* s = n->style;
    return is_replaced(n) || s->display == MX_DISPLAY_INLINE_BLOCK ||
           s->display == MX_DISPLAY_INLINE_FLEX || s->float_side ||
           !strcmp(n->tag, "button");
}

static int text_flags(const mx_style_t* s) {
    int flags = 0;
    if (s->mono) flags |= MX_TEXT_MONO;
    if (s->font_weight >= 600) flags |= MX_TEXT_BOLD;
    else if (s->font_weight >= 500) flags |= MX_TEXT_MEDIUM;
    if (s->italic) flags |= MX_TEXT_ITALIC;
    return flags;
}

static int font_px(const mx_style_t* s) {
    int px = s->font_size;
    if (px < 6) px = 6;
    if (px > 96) px = 96;
    return px;
}

static int normal_line_height(const mx_style_t* s) {
    int px = font_px(s);
    return px + (px * 5 + 9) / 20;   /* about 1.25 */
}

static int line_height_of(const mx_style_t* s) {
    if (s->line_height < 0) return font_px(s) * -s->line_height / 100;
    return s->line_height > 0 ? s->line_height : normal_line_height(s);
}

/* ====================================================================== */
/* Intrinsic widths                                                       */
/* ====================================================================== */

static void measure(mx_node_t* n, int* min_out, int* max_out);

static void measure_text(const char* text, size_t length, const mx_style_t* s,
                         int* min_w, int* line_w) {
    int px = font_px(s), flags = text_flags(s);
    int space = mx_text_width_px(" ", 1, px, flags);
    size_t i = 0;
    int pre = s->white_space == MX_WS_PRE || s->white_space == MX_WS_PRE_WRAP;
    int nowrap = s->white_space == MX_WS_NOWRAP || s->white_space == MX_WS_PRE;
    while (i < length) {
        if (pre && text[i] == '\n') {
            i++;
            if (*line_w > *min_w && nowrap) *min_w = *line_w;
            *line_w = 0;
            continue;
        }
        if (mx_is_space(text[i])) {
            *line_w += space;
            while (i < length && mx_is_space(text[i]) && !(pre && text[i] == '\n')) i++;
            continue;
        }
        size_t start = i;
        while (i < length && !mx_is_space(text[i])) i++;
        int w = mx_text_width_px(text + start, i - start, px, flags);
        if (w > *min_w && !nowrap) *min_w = w;
        *line_w += w;
    }
    if (nowrap && *line_w > *min_w) *min_w = *line_w;
}

static void replaced_size(mx_node_t* n, int container_w, int* w, int* h);

static int box_extra(const mx_style_t* s, int reference) {
    return mx_len_resolve(s->padding[1], reference) +
           mx_len_resolve(s->padding[3], reference) + s->border_width[1] +
           s->border_width[3] + mx_len_resolve(s->margin[1], reference) +
           mx_len_resolve(s->margin[3], reference);
}

static void measure_children(mx_node_t* n, int* min_w, int* max_w) {
    int line = 0;
    for (mx_node_t* c = n->first; c; c = c->next) {
        if (c->type == MX_NODE_TEXT) {
            int child_min = 0;
            measure_text(c->text, c->text_length, n->style, &child_min, &line);
            if (child_min > *min_w) *min_w = child_min;
            continue;
        }
        if (c->type != MX_NODE_ELEMENT || is_hidden(c) || is_out_of_flow(c))
            continue;
        if (mx_dom_is(c, "br")) {
            if (line > *max_w) *max_w = line;
            line = 0;
            continue;
        }
        int cmin = 0, cmax = 0;
        measure(c, &cmin, &cmax);
        if (cmin > *min_w) *min_w = cmin;
        if (is_inline_level(c)) {
            line += cmax;
        } else {
            if (line > *max_w) *max_w = line;
            line = 0;
            if (cmax > *max_w) *max_w = cmax;
        }
    }
    if (line > *max_w) *max_w = line;
}

static void table_measure(mx_node_t* table, int* min_out, int* max_out);

static void measure(mx_node_t* n, int* min_out, int* max_out) {
    *min_out = *max_out = 0;
    if (n->type != MX_NODE_ELEMENT || is_hidden(n)) return;
    if (n->measure_gen == layout_generation) {
        *min_out = n->measure_min;
        *max_out = n->measure_max;
        return;
    }
    const mx_style_t* s = n->style;
    int min_w = 0, max_w = 0;
    if (is_replaced(n)) {
        int w, h;
        replaced_size(n, viewport_w, &w, &h);
        /* w has padding and border already; they are added below */
        min_w = max_w = w - (box_extra(s, 0) - mx_len_resolve(s->margin[1], 0) -
                             mx_len_resolve(s->margin[3], 0));
    } else if (s->width.kind == MX_LEN_SET && !s->width.pct) {
        min_w = max_w = s->width.px;
        if (s->border_box) min_w = max_w = s->width.px - (box_extra(s, 0) -
            mx_len_resolve(s->margin[1], 0) - mx_len_resolve(s->margin[3], 0));
    } else if (s->display == MX_DISPLAY_TABLE) {
        table_measure(n, &min_w, &max_w);
    } else if (s->display == MX_DISPLAY_TABLE_ROW ||
               ((s->display == MX_DISPLAY_FLEX || s->display == MX_DISPLAY_INLINE_FLEX) &&
                s->flex_direction != MX_FLEX_COLUMN &&
                s->flex_direction != MX_FLEX_COLUMN_REVERSE)) {
        /* items sit side by side */
        int count = 0;
        for (mx_node_t* c = n->first; c; c = c->next) {
            int cmin = 0, cmax = 0;
            if (c->type == MX_NODE_TEXT) {
                int line = 0;
                measure_text(c->text, c->text_length, s, &cmin, &line);
                cmax = line;
            } else if (c->type == MX_NODE_ELEMENT && !is_hidden(c) &&
                       !is_out_of_flow(c)) {
                measure(c, &cmin, &cmax);
                count++;
            }
            max_w += cmax;
            if (s->flex_wrap) {
                if (cmin > min_w) min_w = cmin;
            } else {
                min_w += cmin;
            }
        }
        if (count > 1) max_w += s->column_gap * (count - 1);
    } else {
        measure_children(n, &min_w, &max_w);
    }
    if (s->max_width.kind == MX_LEN_SET && !s->max_width.pct &&
        max_w > s->max_width.px)
        max_w = s->max_width.px;
    if (s->min_width.kind == MX_LEN_SET && !s->min_width.pct) {
        if (min_w < s->min_width.px) min_w = s->min_width.px;
        if (max_w < s->min_width.px) max_w = s->min_width.px;
    }
    if (min_w > max_w) max_w = min_w;
    int extra = box_extra(s, 0);
    if (!(s->width.kind == MX_LEN_SET && !s->width.pct && s->border_box) || 1) {
        min_w += extra;
        max_w += extra;
    }
    n->measure_gen = layout_generation;
    n->measure_min = min_w;
    n->measure_max = max_w;
    *min_out = min_w;
    *max_out = max_w;
}

/* ====================================================================== */
/* Replaced elements                                                      */
/* ====================================================================== */

static int attr_int(const mx_node_t* n, const char* name, int fallback) {
    const char* v = mx_dom_attr(n, name);
    if (!v) return fallback;
    int value = 0, digits = 0;
    while (*v == ' ') v++;
    while (*v >= '0' && *v <= '9') {
        value = value * 10 + (*v++ - '0');
        digits++;
    }
    return digits ? value : fallback;
}

static const char* input_type(const mx_node_t* n) {
    const char* t = mx_dom_attr(n, "type");
    return t && *t ? t : "text";
}

static int is_text_input(const mx_node_t* n) {
    if (mx_dom_is(n, "textarea")) return 1;
    if (!mx_dom_is(n, "input")) return 0;
    const char* t = input_type(n);
    return mx_streq_nocase(t, "text") || mx_streq_nocase(t, "search") ||
           mx_streq_nocase(t, "email") || mx_streq_nocase(t, "password") ||
           mx_streq_nocase(t, "url") || mx_streq_nocase(t, "tel") ||
           mx_streq_nocase(t, "number");
}

static int is_button_input(const mx_node_t* n) {
    if (!mx_dom_is(n, "input")) return 0;
    const char* t = input_type(n);
    return mx_streq_nocase(t, "submit") || mx_streq_nocase(t, "button") ||
           mx_streq_nocase(t, "reset") || mx_streq_nocase(t, "image");
}

static const char* control_value(const mx_node_t* n) {
    if (n->value) return n->value;
    if (mx_dom_is(n, "textarea")) {
        return n->first && n->first->type == MX_NODE_TEXT ? n->first->text : "";
    }
    const char* v = mx_dom_attr(n, "value");
    return v ? v : "";
}

static mx_node_t* selected_option(mx_node_t* select) {
    mx_node_t* first = NULL;
    for (mx_node_t* o = select; o; o = mx_dom_next(select, o)) {
        if (!mx_dom_is(o, "option")) continue;
        if (!first) first = o;
        if (o->checked || (mx_dom_attr(o, "selected") && !o->value)) return o;
    }
    return first;
}

static const char* button_label(const mx_node_t* n) {
    const char* v = mx_dom_attr(n, "value");
    if (v && *v) return v;
    const char* t = input_type(n);
    if (mx_streq_nocase(t, "reset")) return "Reset";
    if (mx_streq_nocase(t, "submit") || mx_streq_nocase(t, "image")) return "Submit";
    return "";
}

static void replaced_size(mx_node_t* n, int container_w, int* w, int* h) {
    const mx_style_t* s = n->style;
    int fs = font_px(s);
    int pad_h = mx_len_resolve(s->padding[1], container_w) +
                mx_len_resolve(s->padding[3], container_w) + s->border_width[1] +
                s->border_width[3];
    int pad_v = mx_len_resolve(s->padding[0], container_w) +
                mx_len_resolve(s->padding[2], container_w) + s->border_width[0] +
                s->border_width[2];
    int cw = -1, ch = -1;
    if (s->width.kind == MX_LEN_SET) {
        cw = mx_len_resolve(s->width, container_w);
        if (s->border_box) cw -= pad_h;
    }
    if (s->height.kind == MX_LEN_SET && !s->height.pct) {
        ch = s->height.px;
        if (s->border_box) ch -= pad_v;
    }
    if (mx_dom_is(n, "img") || mx_dom_is(n, "video")) {
        int aw = attr_int(n, "width", -1), ah = attr_int(n, "height", -1);
        /* the picture's or video's own size (.img / .vid) */
        int iw = 0, ih = 0;
        int natural = mx_media_natural_size(n, &iw, &ih) && iw > 0 && ih > 0;
        if (!natural && mx_dom_is(n, "video")) {
            iw = 300;   /* the HTML default */
            ih = 150;
            natural = 1;
        }
        if (cw < 0) cw = aw;
        if (ch < 0) ch = ah;
        if (cw < 0 && ch < 0 && natural) {
            cw = iw;
            ch = ih;
        } else if (cw < 0 && ch < 0) {
            const char* alt = mx_dom_attr(n, "alt");
            cw = alt && *alt ? mx_text_width_px(alt, (size_t)-1, fs, 0) + 26 : 20;
            ch = alt && *alt ? fs + 6 : 20;
        } else if (cw < 0) {
            cw = natural ? ch * iw / ih : aw > 0 && ah > 0 ? ch * aw / ah : ch;
        } else if (ch < 0) {
            ch = natural ? cw * ih / iw
                         : aw > 0 && ah > 0 ? cw * ah / aw : (cw > 300 ? cw * 9 / 16 : cw);
        }
        if (s->max_height.kind == MX_LEN_SET && !s->max_height.pct) {
            int max = s->max_height.px;
            if (ch > max && max > 0) {
                cw = cw * max / ch;
                ch = max;
            }
        }
        if (s->max_width.kind == MX_LEN_SET) {
            int max = mx_len_resolve(s->max_width, container_w);
            if (cw > max && max > 0) {
                ch = ch * max / cw;
                cw = max;
            }
        }
    } else if (mx_dom_is(n, "input")) {
        const char* t = input_type(n);
        if (mx_streq_nocase(t, "checkbox") || mx_streq_nocase(t, "radio")) {
            if (cw < 0) cw = 14;
            if (ch < 0) ch = 14;
        } else if (is_button_input(n)) {
            if (cw < 0) cw = mx_text_width_px(button_label(n), (size_t)-1, fs,
                                              text_flags(s));
            if (ch < 0) ch = fs + 4;
        } else if (mx_streq_nocase(t, "range")) {
            if (cw < 0) cw = 130;
            if (ch < 0) ch = 16;
        } else if (mx_streq_nocase(t, "color")) {
            if (cw < 0) cw = 40;
            if (ch < 0) ch = 20;
        } else {
            if (cw < 0) cw = attr_int(n, "size", 0) > 0
                                 ? attr_int(n, "size", 20) * fs * 55 / 100 + 8
                                 : 170;
            if (ch < 0) ch = fs + 6;
        }
    } else if (mx_dom_is(n, "select")) {
        if (cw < 0) {
            cw = 60;
            for (mx_node_t* o = n; o; o = mx_dom_next(n, o)) {
                if (!mx_dom_is(o, "option")) continue;
                char* label = mx_dom_text_content(&doc, o);
                int lw = mx_text_width_px(label, (size_t)-1, fs, 0) + 28;
                if (lw > cw) cw = lw;
            }
            if (cw > 320) cw = 320;
        }
        if (ch < 0) ch = fs + 6;
    } else if (mx_dom_is(n, "textarea")) {
        if (cw < 0) cw = attr_int(n, "cols", 30) * fs * 55 / 100 + 8;
        if (ch < 0) ch = attr_int(n, "rows", 2) * (fs + 4) + 4;
    } else {
        if (cw < 0) cw = 160;
        if (ch < 0) ch = 12;
    }
    if (cw < 0) cw = 0;
    if (ch < 0) ch = 0;
    *w = cw + pad_h;
    *h = ch + pad_v;
}

static void emit_box_decorations(mx_node_t* n, int x, int y, int w, int h,
                                 int* bg_index);

static void emit_control(mx_node_t* n, int x, int y, int w, int h) {
    const mx_style_t* s = n->style;
    int fs = font_px(s);
    int pl = mx_len_resolve(s->padding[3], w) + s->border_width[3];
    int pt = mx_len_resolve(s->padding[0], w) + s->border_width[0];
    int pr = mx_len_resolve(s->padding[1], w) + s->border_width[1];
    int pb = mx_len_resolve(s->padding[2], w) + s->border_width[2];
    int cx = x + pl, cy = y + pt, cw = w - pl - pr, ch = h - pt - pb;
    int text_y = cy + (ch - mx_text_height_px(fs, 0)) / 2;
    int iw, ih;
    if (mx_dom_is(n, "video") || (mx_dom_is(n, "img") && mx_media_natural_size(n, &iw, &ih))) {
        push_item(IT_MEDIA, cx, cy, cw, ch, 0xFFFFFFFFu, n);
        return;
    }
    if (mx_dom_is(n, "img")) {
        uint32_t muted = mx_with_alpha(s->color, 150);
        int idx = push_item(IT_RECT, x, y, w, h, 0x10808080u, n);
        if (idx >= 0) items[idx].radius = 3;
        const char* alt = mx_dom_attr(n, "alt");
        int icon = w >= 60 && h >= 60 ? 48 : 16;
        if (w < 18 || h < 18) return;
        int iy = alt && *alt && h < 60 ? y + (h - 16) / 2 : y + (h - icon) / 2;
        int ix = alt && *alt && h < 60 ? x + 4 : x + (w - icon) / 2;
        idx = push_item(IT_ICON, ix, iy, icon, icon, muted, n);
        if (idx >= 0) items[idx].px = mx_media_failed(n) ? MX_ICON_BROKEN_IMAGE : MX_ICON_IMAGE;
        if (alt && *alt) {
            int tx = h < 60 ? x + 24 : x + 6;
            int ty = h < 60 ? y + (h - mx_text_height_px(fs, 0)) / 2 : iy + icon + 4;
            int clip = current_clip;
            current_clip = push_clip(x, y, w, h);
            idx = push_item(IT_TEXT, tx, ty, w, fs, muted, n);
            if (idx >= 0) {
                items[idx].text = alt;
                items[idx].length = (uint32_t)strlen(alt);
                items[idx].px = fs;
            }
            current_clip = clip;
        }
        return;
    }
    if (mx_dom_is(n, "input")) {
        const char* t = input_type(n);
        if (mx_streq_nocase(t, "checkbox") || mx_streq_nocase(t, "radio")) {
            int idx = push_item(mx_streq_nocase(t, "radio") ? IT_RADIO : IT_CHECKBOX,
                                x, y, w, h, accent, n);
            if (idx >= 0) items[idx].px = n->checked;
            return;
        }
        if (mx_streq_nocase(t, "range")) {
            int idx = push_item(IT_RECT, x, y + h / 2 - 2, w, 4, 0xFFC9D1DAu, n);
            if (idx >= 0) items[idx].radius = 2;
            idx = push_item(IT_RECT, x + w / 2 - 7, y + h / 2 - 7, 14, 14, accent, n);
            if (idx >= 0) items[idx].radius = 7;
            return;
        }
        const char* text;
        uint32_t color = s->color;
        char* masked = NULL;
        if (is_button_input(n)) {
            text = button_label(n);
            int tw = mx_text_width_px(text, (size_t)-1, fs, text_flags(s));
            int idx = push_item(IT_TEXT, cx + (cw - tw) / 2, text_y, tw, fs, color, n);
            if (idx >= 0) {
                items[idx].text = text;
                items[idx].length = (uint32_t)strlen(text);
                items[idx].px = fs;
                items[idx].flags = (uint8_t)text_flags(s);
            }
            return;
        }
        text = control_value(n);
        if (!*text) {
            const char* ph = mx_dom_attr(n, "placeholder");
            text = ph ? ph : "";
            color = mx_with_alpha(s->color, 120);
        } else if (mx_streq_nocase(t, "password")) {
            size_t count = strlen(text);
            if (count > 64) count = 64;
            masked = (char*)mx_arena_alloc(&layout_arena, count * 3 + 1);
            if (masked) {
                for (size_t i = 0; i < count; i++) memcpy(masked + i * 3, "\xE2\x80\xA2", 3);
                masked[count * 3] = '\0';
                text = masked;
            }
        }
        int clip = current_clip;
        current_clip = push_clip(cx, y, cw, h);
        int tw = mx_text_width_px(text, (size_t)-1, fs, 0);
        int tx = cx;
        if (n == focused && tw > cw - 4) tx = cx + cw - 4 - tw;  /* keep the end visible */
        int idx = push_item(IT_TEXT, tx, text_y, tw, fs, color, n);
        if (idx >= 0) {
            items[idx].text = text;
            items[idx].length = (uint32_t)strlen(text);
            items[idx].px = fs;
        }
        if (n == focused) {
            int caret_x = control_value(n)[0] ? tx + tw + 1 : cx + 1;
            push_item(IT_CARET, caret_x, text_y, 1, mx_text_height_px(fs, 0), accent, n);
        }
        current_clip = clip;
        return;
    }
    if (mx_dom_is(n, "select")) {
        mx_node_t* option = selected_option(n);
        const char* label = option ? mx_dom_text_content(&doc, option) : "";
        int clip = current_clip;
        current_clip = push_clip(cx, y, cw - 18, h);
        int idx = push_item(IT_TEXT, cx, text_y, cw, fs, s->color, n);
        if (idx >= 0) {
            items[idx].text = label;
            items[idx].length = (uint32_t)strlen(label);
            items[idx].px = fs;
        }
        current_clip = clip;
        idx = push_item(IT_ICON, x + w - 22, y + (h - 20) / 2, 20, 20, s->color, n);
        if (idx >= 0) items[idx].px = MX_ICON_ARROW_DROP_DOWN;
        return;
    }
    if (mx_dom_is(n, "textarea")) {
        const char* text = control_value(n);
        uint32_t color = s->color;
        if (!*text) {
            const char* ph = mx_dom_attr(n, "placeholder");
            text = ph ? ph : "";
            color = mx_with_alpha(s->color, 120);
        }
        int clip = current_clip;
        current_clip = push_clip(cx, cy, cw, ch);
        int line_y = cy + 2, step = fs + 4;
        const char* p = text;
        while (*p && line_y < cy + ch) {
            const char* end = p;
            while (*end && *end != '\n') end++;
            size_t length = (size_t)(end - p);
            int idx = push_item(IT_TEXT, cx, line_y, cw, fs, color, n);
            if (idx >= 0) {
                items[idx].text = p;
                items[idx].length = (uint32_t)length;
                items[idx].px = fs;
            }
            line_y += step;
            p = *end ? end + 1 : end;
        }
        current_clip = clip;
        (void)cw;
        return;
    }
}

/* ====================================================================== */
/* Boxes                                                                  */
/* ====================================================================== */

static int push_part(uint8_t part, int x, int y, int w, int h, uint32_t color,
                     mx_node_t* n) {
    int idx = push_item(IT_RECT, x, y, w, h, color, n);
    if (idx >= 0) items[idx].part = part;
    return idx;
}

static void emit_box_decorations(mx_node_t* n, int x, int y, int w, int h,
                                 int* bg_index) {
    const mx_style_t* s = n->style;
    *bg_index = -1;
    if (s->visibility_hidden) return;
    uint32_t bg = s->background;
    /* the root background paints the whole canvas instead */
    if ((n == doc.html || n == doc.body) && (bg >> 24) &&
        page_background == bg)
        bg = 0;
    unsigned first = item_count;
    if (bg >> 24) {
        int idx = push_part(PART_FULL, x, y, w, h, bg, n);
        if (idx >= 0) items[idx].radius = s->radius;
    }
    int bw[4] = {s->border_width[0], s->border_width[1], s->border_width[2],
                 s->border_width[3]};
    if (bw[0] | bw[1] | bw[2] | bw[3]) {
        int uniform = bw[0] == bw[1] && bw[1] == bw[2] && bw[2] == bw[3] &&
                      s->border_color[0] == s->border_color[1] &&
                      s->border_color[1] == s->border_color[2] &&
                      s->border_color[2] == s->border_color[3];
        if (uniform) {
            if (s->border_color[0] >> 24) {
                int idx = push_part(PART_FULL, x, y, w, h, s->border_color[0], n);
                if (idx >= 0) {
                    items[idx].kind = IT_BORDER;
                    items[idx].px = bw[0];
                    items[idx].radius = s->radius;
                }
            }
        } else {
            if (bw[0] && (s->border_color[0] >> 24))
                push_part(PART_TOP, x, y, w, bw[0], s->border_color[0], n);
            if (bw[2] && (s->border_color[2] >> 24))
                push_part(PART_BOTTOM, x, y + h - bw[2], w, bw[2], s->border_color[2], n);
            if (bw[3] && (s->border_color[3] >> 24))
                push_part(PART_LEFT, x, y, bw[3], h, s->border_color[3], n);
            if (bw[1] && (s->border_color[1] >> 24))
                push_part(PART_RIGHT, x + w - bw[1], y, bw[1], h, s->border_color[1], n);
        }
    }
    if (item_count > first) *bg_index = (int)first;
}

/* Gives the decoration items of `n` (starting at `first`) a new height. */
static void resize_decorations(int first, mx_node_t* n, int old_h, int new_h) {
    if (first < 0) return;
    for (unsigned i = (unsigned)first; i < item_count; i++) {
        item_t* it = &items[i];
        if (it->node != n || !it->part) break;
        switch (it->part) {
        case PART_FULL: case PART_LEFT: case PART_RIGHT:
            it->h += new_h - old_h;
            break;
        case PART_BOTTOM:
            it->y += new_h - old_h;
            break;
        default:
            break;
        }
    }
}

static int layout_block(mx_node_t* n, int x, int y, int container_w,
                        int forced_w);
static int layout_flow(mx_node_t* n, int x, int y, int w);

typedef struct {
    int pending;
    int x;
    int number;
    const mx_style_t* style;
    mx_node_t* node;
} marker_t;
static marker_t marker;

static void emit_marker(int line_x, int baseline, int line_top, int line_h) {
    if (!marker.pending) return;
    marker.pending = 0;
    const mx_style_t* s = marker.style;
    if (s->list_style == MX_LIST_NONE || s->visibility_hidden) return;
    int px = font_px(s);
    (void)line_top;
    (void)line_h;
    if (mx_dom_is(marker.node, "summary")) {
        int open = marker.node->parent && mx_dom_attr(marker.node->parent, "open");
        int idx = push_item(IT_ICON, line_x - 20, baseline - mx_text_ascent_px(px, 0) / 2 - 10,
                            20, 20, s->color, marker.node);
        if (idx >= 0) items[idx].px = open ? MX_ICON_ARROW_DROP_DOWN : MX_ICON_ARROW_RIGHT;
        return;
    }
    if (s->list_style <= MX_LIST_SQUARE) {
        int size = px / 3 + 1;
        if (size < 4) size = 4;
        int cy = baseline - mx_text_ascent_px(px, 0) * 4 / 10;
        int mx = line_x - size - 8;
        int idx = push_item(s->list_style == MX_LIST_CIRCLE ? IT_BORDER : IT_RECT,
                            mx, cy - size / 2, size, size, s->color, marker.node);
        if (idx >= 0) {
            items[idx].radius = s->list_style == MX_LIST_SQUARE ? 0 : size;
            items[idx].px = 1;
        }
        return;
    }
    char label[24];
    int number = marker.number;
    label[0] = '\0';
    if (s->list_style == MX_LIST_DECIMAL) {
        mx_append_int(label, sizeof(label), number);
    } else if (s->list_style == MX_LIST_LOWER_ALPHA || s->list_style == MX_LIST_UPPER_ALPHA) {
        char c[2] = {(char)((s->list_style == MX_LIST_LOWER_ALPHA ? 'a' : 'A') +
                            (number - 1) % 26), 0};
        mx_copy(label, sizeof(label), c);
    } else {
        static const char* const lower[] = {"m", "cm", "d", "cd", "c", "xc", "l",
                                            "xl", "x", "ix", "v", "iv", "i"};
        static const int values[] = {1000, 900, 500, 400, 100, 90, 50, 40, 10, 9,
                                     5, 4, 1};
        int v = number;
        for (int i = 0; i < 13; i++)
            while (v >= values[i] && strlen(label) < 16) {
                mx_append(label, sizeof(label), lower[i]);
                v -= values[i];
            }
        if (s->list_style == MX_LIST_UPPER_ROMAN)
            for (char* p = label; *p; p++) *p = (char)(*p - 'a' + 'A');
    }
    mx_append(label, sizeof(label), ".");
    char* text = mx_arena_strdup(&layout_arena, label);
    if (!text) return;
    int w = mx_text_width_px(text, (size_t)-1, px, text_flags(s));
    int idx = push_item(IT_TEXT, line_x - w - 6, baseline - mx_text_ascent_px(px, text_flags(s)),
                        w, px, s->color, marker.node);
    if (idx >= 0) {
        items[idx].text = text;
        items[idx].length = (uint32_t)strlen(text);
        items[idx].px = px;
        items[idx].flags = (uint8_t)text_flags(s);
    }
}

/* ====================================================================== */
/* Inline formatting                                                      */
/* ====================================================================== */

enum { F_TEXT, F_BOX };

typedef struct {
    uint8_t kind;
    uint8_t float_right;
    int x, w;
    int ascent, descent;        /* above / below the baseline */
    int font_ascent;
    const char* text;
    uint32_t length;
    const mx_style_t* style;
    mx_node_t* node;            /* element owning the text / the box */
    mx_node_t* deco;            /* inline ancestor with a background */
    unsigned item_start, item_end, laid_start, laid_end;
    int box_h;
    int middle;
} frag_t;

#define MAX_FRAGS 512

typedef struct {
    mx_node_t* block;
    const mx_style_t* block_style;
    int x0, width;
    int y;
    int cur_x;
    int lines;
    frag_t frags[MAX_FRAGS];
    int count;
    int pending_space;
    int indent;
    mx_node_t* deco_stack[16];
    int deco_depth;
} ifc_t;

static ifc_t* ifc_pool[8];
static int ifc_depth;

static int frag_can_merge(const frag_t* f, const char* text, size_t length,
                          const mx_style_t* s, mx_node_t* node, mx_node_t* deco,
                          int space) {
    if (f->kind != F_TEXT || f->style != s || f->node != node || f->deco != deco)
        return 0;
    const char* end = f->text + f->length;
    if (space) return end + 1 == text && *end == ' ' && f->length + length < 400;
    return end == text && f->length + length < 400;
}

static void ifc_finish_line(ifc_t* c, int forced) {
    if (!c->count && !forced) return;
    const mx_style_t* bs = c->block_style;
    int strut_px = font_px(bs);
    int strut_lh = line_height_of(bs);
    int strut_a = mx_text_ascent_px(strut_px, 0);
    int strut_h = mx_text_height_px(strut_px, 0);
    int half = (strut_lh - strut_h) / 2;
    int max_a = strut_a + half, max_d = strut_h - strut_a + (strut_lh - strut_h - half);
    int only_boxes = c->count > 0;
    for (int i = 0; i < c->count; i++) {
        frag_t* f = &c->frags[i];
        if (f->kind == F_TEXT) only_boxes = 0;
        if (f->kind == F_BOX && f->middle) continue;
        if (f->ascent > max_a) max_a = f->ascent;
        if (f->descent > max_d) max_d = f->descent;
    }
    if (only_boxes) {
        /* a line of only boxes needs no text strut below them */
        int box_a = 0, box_d = 0;
        for (int i = 0; i < c->count; i++) {
            if (c->frags[i].ascent > box_a) box_a = c->frags[i].ascent;
            if (c->frags[i].descent > box_d) box_d = c->frags[i].descent;
        }
        max_a = box_a;
        max_d = box_d;
    }
    int line_h = max_a + max_d;
    for (int i = 0; i < c->count; i++) {
        frag_t* f = &c->frags[i];
        if (f->kind == F_BOX && f->middle && f->box_h > line_h) line_h = f->box_h;
    }
    int baseline = c->y + max_a;
    /* width used, excluding right floats */
    int used = 0, right_used = 0;
    for (int i = 0; i < c->count; i++) {
        frag_t* f = &c->frags[i];
        if (f->float_right) right_used += f->w;
        else if (f->x + f->w > used) used = f->x + f->w;
    }
    int offset = 0;
    int free_space = c->width - used - right_used;
    if (bs->text_align == MX_ALIGN_CENTER || bs->text_align == MX_ALIGN_LEGACY_CENTER)
        offset = free_space / 2;
    else if (bs->text_align == MX_ALIGN_RIGHT) offset = free_space;
    if (offset < 0) offset = 0;

    /* inline backgrounds */
    for (int i = 0; i < c->count;) {
        mx_node_t* deco = c->frags[i].deco;
        int j = i;
        while (j < c->count && c->frags[j].deco == deco && c->frags[j].kind == F_TEXT)
            j++;
        if (deco && j > i && !deco->style->visibility_hidden) {
            const mx_style_t* ds = deco->style;
            int pl = mx_len_resolve(ds->padding[3], c->width) + ds->border_width[3];
            int pr = mx_len_resolve(ds->padding[1], c->width) + ds->border_width[1];
            int pt = mx_len_resolve(ds->padding[0], c->width) + ds->border_width[0];
            int pb = mx_len_resolve(ds->padding[2], c->width) + ds->border_width[2];
            int fa = c->frags[i].font_ascent;
            int fh = mx_text_height_px(font_px(ds), text_flags(ds));
            int x = c->x0 + offset + c->frags[i].x - pl;
            int w = c->frags[j - 1].x + c->frags[j - 1].w - c->frags[i].x + pl + pr;
            int y = baseline - fa - pt;
            int h = fh + pt + pb;
            int dummy;
            int saved_box_x = deco->box_x, saved_box_y = deco->box_y;
            emit_box_decorations(deco, x, y, w, h, &dummy);
            deco->box_x = saved_box_x;
            deco->box_y = saved_box_y;
        }
        i = j > i ? j : i + 1;
    }

    int right_x = c->width;
    for (int i = 0; i < c->count; i++) {
        frag_t* f = &c->frags[i];
        int fx;
        if (f->float_right) {
            right_x -= f->w;
            fx = c->x0 + right_x;
        } else {
            fx = c->x0 + offset + f->x;
        }
        if (f->kind == F_TEXT) {
            int ty = baseline - f->font_ascent;
            if (!f->style->visibility_hidden) {
                int idx = push_item(IT_TEXT, fx, ty, f->w,
                                    mx_text_height_px(font_px(f->style),
                                                      text_flags(f->style)),
                                    f->style->color, f->node);
                if (idx >= 0) {
                    items[idx].text = f->text;
                    items[idx].length = f->length;
                    items[idx].px = font_px(f->style);
                    items[idx].flags = (uint8_t)text_flags(f->style);
                    items[idx].deco = f->style->decoration;
                }
            }
            /* grow the boxes of inline ancestors */
            for (mx_node_t* a = f->node; a && a != c->block && a->type == MX_NODE_ELEMENT;
                 a = a->parent) {
                int top = ty, bottom = ty + line_h;
                if (a->layout_gen != layout_generation) {
                    a->layout_gen = layout_generation;
                    a->box_x = fx;
                    a->box_y = top;
                    a->box_w = f->w;
                    a->box_h = bottom - top;
                    record_laid(a);
                } else {
                    int x1 = a->box_x + a->box_w, y1 = a->box_y + a->box_h;
                    if (fx < a->box_x) a->box_x = fx;
                    if (top < a->box_y) a->box_y = top;
                    if (fx + f->w > x1) x1 = fx + f->w;
                    if (bottom > y1) y1 = bottom;
                    a->box_w = x1 - a->box_x;
                    a->box_h = y1 - a->box_y;
                }
            }
        } else {
            int top = f->middle ? c->y + (line_h - f->box_h) / 2 : baseline - f->ascent;
            shift_range(f->item_start, f->item_end, f->laid_start, f->laid_end, fx, top);
        }
    }
    emit_marker(c->x0 + offset, baseline, c->y, line_h);
    c->y += line_h;
    c->lines++;
    c->count = 0;
    c->cur_x = 0;
    c->pending_space = 0;
    c->indent = 0;
}

static void ifc_add_text_piece(ifc_t* c, const char* text, size_t length,
                               const mx_style_t* s, mx_node_t* node, int wrap) {
    int px = font_px(s), flags = text_flags(s);
    int w = mx_text_width_px(text, length, px, flags);
    int space = 0;
    if (c->pending_space && c->count) space = mx_text_width_px(" ", 1, px, flags);
    if (c->cur_x + space + w > c->width && c->count && wrap) {
        ifc_finish_line(c, 0);
        space = 0;
    }
    /* words wider than the line are broken */
    if (w > c->width && wrap && length > 1 && c->width > px) {
        while (length) {
            char buffer[512];
            size_t n = length < sizeof(buffer) - 1 ? length : sizeof(buffer) - 1;
            memcpy(buffer, text, n);
            buffer[n] = '\0';
            size_t fit = mx_text_fit(buffer, px, flags, c->width - c->cur_x);
            if (!fit) {
                if (c->count) {
                    ifc_finish_line(c, 0);
                    continue;
                }
                fit = 1;
                while (fit < n && ((unsigned char)text[fit] & 0xC0) == 0x80) fit++;
            }
            c->pending_space = 0;
            ifc_add_text_piece(c, text, fit, s, node, 0);
            text += fit;
            length -= fit;
            if (length) ifc_finish_line(c, 0);
        }
        return;
    }
    mx_node_t* deco = c->deco_depth ? c->deco_stack[c->deco_depth - 1] : NULL;
    if (c->count && frag_can_merge(&c->frags[c->count - 1], text, length, s, node,
                                   deco, space)) {
        frag_t* f = &c->frags[c->count - 1];
        f->length += (uint32_t)(length + (space ? 1 : 0));
        f->w = mx_text_width_px(f->text, f->length, px, flags);
        c->cur_x = f->x + f->w;
        c->pending_space = 0;
        return;
    }
    if (c->count >= MAX_FRAGS) ifc_finish_line(c, 0);
    frag_t* f = &c->frags[c->count++];
    memset(f, 0, sizeof(*f));
    f->kind = F_TEXT;
    f->x = c->cur_x + space + (c->count == 1 ? c->indent : 0);
    f->w = w;
    f->text = text;
    f->length = (uint32_t)length;
    f->style = s;
    f->node = node;
    f->deco = deco;
    int lh = line_height_of(s);
    int fh = mx_text_height_px(px, flags);
    f->font_ascent = mx_text_ascent_px(px, flags);
    int half = (lh - fh) / 2;
    f->ascent = f->font_ascent + half;
    f->descent = fh - f->font_ascent + (lh - fh - half);
    c->cur_x = f->x + w;
    c->pending_space = 0;
}

static const char* transformed(const char* text, size_t length,
                               const mx_style_t* s, int capitalize_start) {
    if (s->transform == MX_TT_NONE) return text;
    char* copy = mx_arena_strndup(&layout_arena, text, length);
    if (!copy) return text;
    int start = capitalize_start;
    for (size_t i = 0; i < length; i++) {
        char ch = copy[i];
        if (s->transform == MX_TT_UPPER && ch >= 'a' && ch <= 'z') copy[i] = (char)(ch - 32);
        else if (s->transform == MX_TT_LOWER && ch >= 'A' && ch <= 'Z') copy[i] = (char)(ch + 32);
        else if (s->transform == MX_TT_CAPITALIZE) {
            if (start && ch >= 'a' && ch <= 'z') copy[i] = (char)(ch - 32);
            start = mx_is_space(ch);
        }
    }
    return copy;
}

static void ifc_text(ifc_t* c, const char* text, size_t length,
                     const mx_style_t* s, mx_node_t* node) {
    if (!length) return;
    int ws = s->white_space;
    if (ws == MX_WS_PRE || ws == MX_WS_PRE_WRAP) {
        /* expand tabs; keep spaces; newline breaks the line */
        size_t i = 0;
        while (i < length) {
            size_t end = i;
            while (end < length && text[end] != '\n') end++;
            if (end > i) {
                size_t tabs = 0;
                for (size_t k = i; k < end; k++) tabs += text[k] == '\t';
                const char* piece = text + i;
                size_t n = end - i;
                if (tabs) {
                    char* copy = (char*)mx_arena_alloc(&layout_arena, n + tabs * 3 + 1);
                    if (copy) {
                        size_t used = 0;
                        for (size_t k = i; k < end; k++) {
                            if (text[k] == '\t') {
                                memcpy(copy + used, "    ", 4);
                                used += 4;
                            } else {
                                copy[used++] = text[k];
                            }
                        }
                        copy[used] = '\0';
                        piece = copy;
                        n = used;
                    }
                }
                piece = transformed(piece, n, s, 1);
                if (ws == MX_WS_PRE) {
                    ifc_add_text_piece(c, piece, n, s, node, 0);
                } else {
                    /* pre-wrap: break at spaces */
                    size_t p = 0;
                    while (p < n) {
                        size_t q = p;
                        while (q < n && piece[q] != ' ') q++;
                        while (q < n && piece[q] == ' ') q++;
                        ifc_add_text_piece(c, piece + p, q - p, s, node, 1);
                        p = q;
                    }
                }
            }
            if (end < length) ifc_finish_line(c, 1);
            i = end + 1;
        }
        return;
    }
    int wrap = ws != MX_WS_NOWRAP;
    size_t i = 0;
    while (i < length) {
        if (mx_is_space(text[i]) ||
            ((unsigned char)text[i] == 0xC2 && i + 1 < length &&
             (unsigned char)text[i + 1] == 0xA0 && 0)) {
            if (c->count || c->cur_x > 0) c->pending_space = 1;
            i++;
            continue;
        }
        size_t start = i;
        while (i < length && !mx_is_space(text[i])) i++;
        int capitalize = start == 0 || mx_is_space(text[start - 1]);
        const char* word = transformed(text + start, i - start, s, capitalize);
        if (word != text + start) {
            /* copies cannot merge with neighbours */
            int pending = c->pending_space;
            ifc_add_text_piece(c, word, i - start, s, node, wrap);
            (void)pending;
        } else {
            ifc_add_text_piece(c, word, i - start, s, node, wrap);
        }
    }
}

static void ifc_box(ifc_t* c, mx_node_t* e) {
    const mx_style_t* s = e->style;
    int space = 0;
    if (c->pending_space && c->count && !s->float_side)
        space = mx_text_width_px(" ", 1, font_px(c->block_style), 0);
    int ml = s->margin[3].kind == MX_LEN_SET ? mx_len_resolve(s->margin[3], c->width) : 0;
    int mr = s->margin[1].kind == MX_LEN_SET ? mx_len_resolve(s->margin[1], c->width) : 0;
    int mt = s->margin[0].kind == MX_LEN_SET ? mx_len_resolve(s->margin[0], c->width) : 0;
    int mb = s->margin[2].kind == MX_LEN_SET ? mx_len_resolve(s->margin[2], c->width) : 0;
    int forced = -1;
    if (s->width.kind != MX_LEN_SET || is_replaced(e)) {
        if (is_replaced(e)) {
            int w, h;
            replaced_size(e, c->width, &w, &h);
            forced = w;
        } else {
            int mn, mxw;
            measure(e, &mn, &mxw);
            int avail = c->width - ml - mr;
            int shrink = mxw - ml - mr;
            if (shrink > avail) shrink = avail;
            if (shrink < mn - ml - mr) shrink = mn - ml - mr;
            forced = shrink;
        }
    }
    unsigned item_start = item_count, laid_start = laid_count;
    int h = layout_block(e, 0, mt, c->width, forced);
    int outer_w = e->box_w + ml + mr;
    if (c->cur_x + space + outer_w > c->width && c->count &&
        s->white_space != MX_WS_NOWRAP) {
        ifc_finish_line(c, 0);
        space = 0;
    }
    if (c->count >= MAX_FRAGS) ifc_finish_line(c, 0);
    frag_t* f = &c->frags[c->count++];
    memset(f, 0, sizeof(*f));
    f->kind = F_BOX;
    f->float_right = s->float_side == MX_FLOAT_RIGHT;
    f->x = c->cur_x + space;
    f->w = outer_w;
    f->item_start = item_start;
    f->item_end = item_count;
    f->laid_start = laid_start;
    f->laid_end = laid_count;
    f->box_h = h + mt + mb;
    f->middle = s->vertical_middle;
    /* the box's own text baseline sits a little above its bottom */
    int d = is_replaced(e) && !is_text_input(e) && !mx_dom_is(e, "select")
                ? 0 : (mx_text_height_px(font_px(s), 0) - mx_text_ascent_px(font_px(s), 0)) +
                      mx_len_resolve(s->padding[2], c->width) + s->border_width[2] + mb;
    if (d > f->box_h) d = f->box_h;
    f->ascent = f->box_h - d;
    f->descent = d;
    f->node = e;
    /* boxes are laid out with their margin box at x = 0 */
    shift_range(item_start, item_count, laid_start, laid_count, ml, 0);
    shift_range(item_start, item_count, laid_start, laid_count, 0, -mt);
    if (!f->float_right) c->cur_x = f->x + outer_w;
    c->pending_space = 0;
}

static void ifc_walk(ifc_t* c, mx_node_t* parent, mx_node_t* first,
                     mx_node_t* stop);

static void ifc_block_in_inline(ifc_t* c, mx_node_t* e) {
    /* a block inside an inline: it takes its own line(s) */
    ifc_finish_line(c, 0);
    const mx_style_t* s = e->style;
    int mt = mx_len_resolve(s->margin[0], c->width);
    int mb = mx_len_resolve(s->margin[2], c->width);
    int h = layout_block(e, c->x0, c->y + mt, c->width, -1);
    c->y += mt + h + mb;
}

static void ifc_element(ifc_t* c, mx_node_t* e) {
    if (is_hidden(e)) return;
    const mx_style_t* s = e->style;
    if (mx_dom_is(e, "br")) {
        ifc_finish_line(c, 1);
        return;
    }
    if (mx_dom_is(e, "wbr")) return;
    if (is_out_of_flow(e)) {
        /* positioned out of flow: laid out where it would start */
        int dummy_mt = 0;
        (void)dummy_mt;
        extern void layout_absolute(mx_node_t * child, int static_x, int static_y,
                                    int container_w);
        layout_absolute(e, c->x0 + c->cur_x, c->y, c->width);
        return;
    }
    if (is_atomic_inline(e)) {
        ifc_box(c, e);
        return;
    }
    if (s->display != MX_DISPLAY_INLINE && s->display != MX_DISPLAY_CONTENTS) {
        ifc_block_in_inline(c, e);
        return;
    }
    unsigned saved_opacity = current_opacity;
    if (s->opacity < 255) current_opacity = current_opacity * s->opacity / 255;
    int left = mx_len_resolve(s->margin[3], c->width) +
               mx_len_resolve(s->padding[3], c->width) + s->border_width[3];
    int right = mx_len_resolve(s->margin[1], c->width) +
                mx_len_resolve(s->padding[1], c->width) + s->border_width[1];
    int decorated = ((s->background >> 24) || s->border_width[0] ||
                     s->border_width[2]) && c->deco_depth < 16;
    if (left > 0) {
        if (c->pending_space && c->count)
            c->cur_x += mx_text_width_px(" ", 1, font_px(s), text_flags(s));
        c->pending_space = 0;
        c->cur_x += left;
    }
    if (decorated) c->deco_stack[c->deco_depth++] = e;
    ifc_walk(c, e, e->first, NULL);
    if (decorated) c->deco_depth--;
    if (right > 0) c->cur_x += right;
    /* empty inline elements (anchors with only an icon) still get a box */
    if (e->layout_gen != layout_generation) {
        e->layout_gen = layout_generation;
        e->box_x = c->x0 + c->cur_x;
        e->box_y = c->y;
        e->box_w = 0;
        e->box_h = line_height_of(s);
        record_laid(e);
    }
    current_opacity = saved_opacity;
}

static void ifc_walk(ifc_t* c, mx_node_t* parent, mx_node_t* first,
                     mx_node_t* stop) {
    for (mx_node_t* n = first; n && n != stop; n = n->next) {
        if (n->type == MX_NODE_TEXT) {
            ifc_text(c, n->text, n->text_length, parent->style, parent);
        } else if (n->type == MX_NODE_ELEMENT) {
            ifc_element(c, n);
        }
    }
}

/* Lays out the inline children [first, stop) of `block`. Returns height. */
static int layout_inline_run(mx_node_t* block, mx_node_t* first,
                             mx_node_t* stop, int x, int y, int w) {
    /* whitespace-only runs produce nothing */
    int any = 0;
    for (mx_node_t* n = first; n && n != stop && !any; n = n->next) {
        if (n->type == MX_NODE_ELEMENT) any = !is_hidden(n);
        else if (n->type == MX_NODE_TEXT)
            for (size_t i = 0; i < n->text_length && !any; i++)
                any = !mx_is_space(n->text[i]);
    }
    if (!any) return 0;
    if (ifc_depth >= 8) return 0;
    if (!ifc_pool[ifc_depth]) {
        ifc_pool[ifc_depth] = (ifc_t*)malloc(sizeof(ifc_t));
        if (!ifc_pool[ifc_depth]) return 0;
    }
    ifc_t* c = ifc_pool[ifc_depth++];
    c->block = block;
    c->block_style = block->style;
    c->x0 = x;
    c->width = w > 1 ? w : 1;
    c->y = y;
    c->cur_x = 0;
    c->lines = 0;
    c->count = 0;
    c->pending_space = 0;
    c->deco_depth = 0;
    c->indent = block->style->text_indent;
    ifc_walk(c, block, first, stop);
    ifc_finish_line(c, 0);
    int height = c->y - y;
    ifc_depth--;
    return height;
}

/* ====================================================================== */
/* Absolute positioning                                                   */
/* ====================================================================== */

void layout_absolute(mx_node_t* child, int static_x, int static_y,
                     int container_w);

void layout_absolute(mx_node_t* child, int static_x, int static_y,
                     int container_w) {
    const mx_style_t* s = child->style;
    cb_t cb = {NULL, 0, 0, viewport_w, viewport_h};
    if (s->position == MX_POS_ABSOLUTE && cb_depth) cb = cb_stack[cb_depth - 1];
    (void)container_w;
    int ml = mx_len_resolve(s->margin[3], cb.w), mr = mx_len_resolve(s->margin[1], cb.w);
    int mt = mx_len_resolve(s->margin[0], cb.w);
    int has_l = s->offset[3].kind == MX_LEN_SET, has_r = s->offset[1].kind == MX_LEN_SET;
    int has_t = s->offset[0].kind == MX_LEN_SET, has_b = s->offset[2].kind == MX_LEN_SET;
    int l = mx_len_resolve(s->offset[3], cb.w), r = mx_len_resolve(s->offset[1], cb.w);
    int t = mx_len_resolve(s->offset[0], cb.h > 0 ? cb.h : viewport_h);
    int b = mx_len_resolve(s->offset[2], cb.h > 0 ? cb.h : viewport_h);
    int forced = -1;
    if (s->width.kind != MX_LEN_SET) {
        if (has_l && has_r) {
            forced = cb.w - l - r - ml - mr;
        } else {
            int mn, mxw;
            measure(child, &mn, &mxw);
            forced = mxw - ml - mr;
            if (forced > cb.w) forced = cb.w;
            if (forced < 0) forced = 0;
        }
    }
    int x = has_l ? cb.x + l : static_x;
    int y = has_t ? cb.y + t : static_y;
    unsigned item_start = item_count, laid_start = laid_count;
    int saved_clip = current_clip;
    if (s->position == MX_POS_FIXED) current_clip = 0;
    int h = layout_block(child, x, y + mt, cb.w, forced);
    current_clip = saved_clip;
    int dx = 0, dy = 0;
    if (!has_l && has_r) dx = cb.x + cb.w - r - mr - child->box_w - child->box_x;
    if (!has_t && has_b && cb.h > 0) dy = cb.y + cb.h - b - h - (child->box_y);
    shift_range(item_start, item_count, laid_start, laid_count, dx, dy);
}

/* ====================================================================== */
/* Flex, grid and table                                                   */
/* ====================================================================== */

typedef struct {
    mx_node_t* node;
    int basis;          /* border-box main size */
    int min;
    int grow, shrink;
    int ml, mr, mt, mb;
    int h;
    unsigned item_start, item_end, laid_start, laid_end;
} flex_item_t;

static void stretch_box(mx_node_t* n, int new_h) {
    if (new_h <= n->box_h) return;
    resize_decorations(n->layout_item, n, n->box_h, new_h);
    n->box_h = new_h;
}

static int collect_flex_items(mx_node_t* n, flex_item_t* out, int capacity) {
    int count = 0;
    for (mx_node_t* c = n->first; c && count < capacity; c = c->next) {
        if (c->type != MX_NODE_ELEMENT || is_hidden(c)) continue;
        if (is_out_of_flow(c)) continue;
        out[count++].node = c;
    }
    /* stable sort by `order` */
    for (int i = 1; i < count; i++) {
        flex_item_t it = out[i];
        int j = i - 1;
        while (j >= 0 && out[j].node->style->order > it.node->style->order) {
            out[j + 1] = out[j];
            j--;
        }
        out[j + 1] = it;
    }
    return count;
}

static void layout_out_of_flow_children(mx_node_t* n, int x, int y, int w) {
    for (mx_node_t* c = n->first; c; c = c->next)
        if (c->type == MX_NODE_ELEMENT && !is_hidden(c) && is_out_of_flow(c))
            layout_absolute(c, x, y, w);
}

static int has_loose_text(mx_node_t* n) {
    for (mx_node_t* c = n->first; c; c = c->next)
        if (c->type == MX_NODE_TEXT)
            for (size_t i = 0; i < c->text_length; i++)
                if (!mx_is_space(c->text[i])) return 1;
    return 0;
}

static int layout_flex(mx_node_t* n, int x, int y, int w) {
    const mx_style_t* s = n->style;
    static flex_item_t pool[4][128];
    static int pool_depth;
    if (pool_depth >= 4 || has_loose_text(n)) return -1;   /* fall back to flow */
    flex_item_t* list = pool[pool_depth++];
    int count = collect_flex_items(n, list, 128);
    int column = s->flex_direction == MX_FLEX_COLUMN ||
                 s->flex_direction == MX_FLEX_COLUMN_REVERSE;
    int gap_main = column ? s->row_gap : s->column_gap;
    int total_h = 0;
    if (column) {
        int cy = y;
        int container_h = s->height.kind == MX_LEN_SET && !s->height.pct ? s->height.px : -1;
        unsigned first_item = item_count, first_laid = laid_count;
        for (int i = 0; i < count; i++) {
            mx_node_t* c = list[i].node;
            const mx_style_t* cs = c->style;
            int mt = mx_len_resolve(cs->margin[0], w), mb = mx_len_resolve(cs->margin[2], w);
            int forced = -1;
            int ml = mx_len_resolve(cs->margin[3], w), mr = mx_len_resolve(cs->margin[1], w);
            if (s->align_items != MX_ITEMS_STRETCH && cs->width.kind != MX_LEN_SET) {
                int mn, mxw;
                measure(c, &mn, &mxw);
                forced = mxw - ml - mr;
                if (forced > w - ml - mr) forced = w - ml - mr;
            }
            unsigned is = item_count, ls = laid_count;
            int h = layout_block(c, x, cy + mt, w, forced);
            if (forced >= 0 && (s->align_items == MX_ITEMS_CENTER ||
                                s->align_items == MX_ITEMS_END)) {
                int free_w = w - c->box_w - ml - mr;
                shift_range(is, item_count, ls, laid_count,
                            s->align_items == MX_ITEMS_CENTER ? free_w / 2 : free_w, 0);
            }
            cy += mt + h + mb + (i + 1 < count ? gap_main : 0);
        }
        total_h = cy - y;
        if (container_h > total_h && s->justify != MX_JUSTIFY_START) {
            int free_h = container_h - total_h;
            int dy = s->justify == MX_JUSTIFY_CENTER ? free_h / 2 :
                     s->justify == MX_JUSTIFY_END ? free_h : 0;
            shift_range(first_item, item_count, first_laid, laid_count, 0, dy);
        }
        pool_depth--;
        layout_out_of_flow_children(n, x, y, w);
        return total_h;
    }

    for (int i = 0; i < count; i++) {
        flex_item_t* it = &list[i];
        const mx_style_t* cs = it->node->style;
        it->ml = cs->margin[3].kind == MX_LEN_SET ? mx_len_resolve(cs->margin[3], w) : 0;
        it->mr = cs->margin[1].kind == MX_LEN_SET ? mx_len_resolve(cs->margin[1], w) : 0;
        it->mt = mx_len_resolve(cs->margin[0], w);
        it->mb = mx_len_resolve(cs->margin[2], w);
        int pad = mx_len_resolve(cs->padding[1], w) + mx_len_resolve(cs->padding[3], w) +
                  cs->border_width[1] + cs->border_width[3];
        int mn, mxw;
        measure(it->node, &mn, &mxw);
        mn -= it->ml + it->mr;
        mxw -= it->ml + it->mr;
        if (cs->flex_basis.kind == MX_LEN_SET) {
            it->basis = mx_len_resolve(cs->flex_basis, w) + (cs->border_box ? 0 : pad);
            if (cs->flex_basis.px == 0 && cs->flex_basis.pct == 0) it->basis = pad;
        } else if (cs->width.kind == MX_LEN_SET) {
            it->basis = mx_len_resolve(cs->width, w) + (cs->border_box ? 0 : pad);
        } else {
            it->basis = mxw;
        }
        if (cs->max_width.kind == MX_LEN_SET) {
            int max = mx_len_resolve(cs->max_width, w) + (cs->border_box ? 0 : pad);
            if (it->basis > max) it->basis = max;
        }
        it->min = cs->min_width.kind == MX_LEN_SET
                      ? mx_len_resolve(cs->min_width, w) + (cs->border_box ? 0 : pad)
                      : (cs->width.kind == MX_LEN_SET ? it->basis : mn);
        if (it->min > it->basis && cs->flex_basis.kind != MX_LEN_SET) it->basis = it->min;
        it->grow = cs->flex_grow;
        it->shrink = cs->flex_shrink;
    }
    int cy = y;
    int start = 0;
    while (start < count) {
        int end = start, used = 0;
        while (end < count) {
            int outer = list[end].basis + list[end].ml + list[end].mr;
            int with_gap = used + (end > start ? gap_main : 0) + outer;
            if (s->flex_wrap && end > start && with_gap > w) break;
            used = with_gap;
            end++;
        }
        int free_space = w - used;
        int grow_total = 0;
        long shrink_total = 0;
        for (int i = start; i < end; i++) {
            grow_total += list[i].grow;
            shrink_total += (long)list[i].shrink * list[i].basis;
        }
        int sizes[128];
        for (int i = start; i < end; i++) {
            int size = list[i].basis;
            if (free_space > 0 && grow_total > 0)
                size += (int)((long)free_space * list[i].grow / grow_total);
            else if (free_space < 0 && shrink_total > 0)
                size += (int)((long)free_space * list[i].shrink * list[i].basis / shrink_total);
            if (size < list[i].min) size = list[i].min;
            if (size < 0) size = 0;
            sizes[i - start] = size;
        }
        int consumed = 0;
        for (int i = start; i < end; i++)
            consumed += sizes[i - start] + list[i].ml + list[i].mr;
        consumed += gap_main * (end - start - 1);
        int remaining = w - consumed;
        int lead = 0, between = gap_main;
        int n_items = end - start;
        /* auto margins absorb free space first */
        int auto_margins = 0;
        for (int i = start; i < end; i++) {
            auto_margins += list[i].node->style->margin[3].kind == MX_LEN_AUTO;
            auto_margins += list[i].node->style->margin[1].kind == MX_LEN_AUTO;
        }
        if (remaining > 0 && !auto_margins) {
            switch (s->justify) {
            case MX_JUSTIFY_CENTER: lead = remaining / 2; break;
            case MX_JUSTIFY_END: lead = remaining; break;
            case MX_JUSTIFY_BETWEEN:
                if (n_items > 1) between += remaining / (n_items - 1);
                break;
            case MX_JUSTIFY_AROUND:
                lead = remaining / (2 * n_items);
                between += remaining / n_items;
                break;
            case MX_JUSTIFY_EVENLY:
                lead = remaining / (n_items + 1);
                between += remaining / (n_items + 1);
                break;
            default: break;
            }
        }
        if (s->flex_direction == MX_FLEX_ROW_REVERSE && remaining > 0 && !auto_margins &&
            s->justify == MX_JUSTIFY_START)
            lead = remaining;
        int cx = x + lead;
        int line_h = 0;
        for (int k = 0; k < n_items; k++) {
            int i = s->flex_direction == MX_FLEX_ROW_REVERSE ? end - 1 - k : start + k;
            flex_item_t* it = &list[i];
            const mx_style_t* cs = it->node->style;
            if (remaining > 0 && auto_margins && cs->margin[3].kind == MX_LEN_AUTO)
                cx += remaining / auto_margins;
            it->item_start = item_count;
            it->laid_start = laid_count;
            it->h = layout_block(it->node, cx, cy + it->mt, sizes[i - start] + it->ml + it->mr,
                                 sizes[i - start]);
            it->item_end = item_count;
            it->laid_end = laid_count;
            /* layout_block placed the margin box at cx */
            cx += sizes[i - start] + it->ml + it->mr + between;
            if (remaining > 0 && auto_margins && cs->margin[1].kind == MX_LEN_AUTO)
                cx += remaining / auto_margins;
            int outer_h = it->h + it->mt + it->mb;
            if (outer_h > line_h) line_h = outer_h;
        }
        int container_h = (s->height.kind == MX_LEN_SET && !s->height.pct && !s->flex_wrap)
                              ? s->height.px - (s->border_box
                                    ? mx_len_resolve(s->padding[0], w) + mx_len_resolve(s->padding[2], w) +
                                      s->border_width[0] + s->border_width[2] : 0)
                              : -1;
        if (container_h > line_h) line_h = container_h;
        for (int i = start; i < end; i++) {
            flex_item_t* it = &list[i];
            int outer_h = it->h + it->mt + it->mb;
            int align = s->align_items;
            if (align == MX_ITEMS_STRETCH) {
                if (it->node->style->height.kind != MX_LEN_SET)
                    stretch_box(it->node, line_h - it->mt - it->mb);
            } else if (align == MX_ITEMS_CENTER || align == MX_ITEMS_END) {
                int dy = align == MX_ITEMS_CENTER ? (line_h - outer_h) / 2 : line_h - outer_h;
                shift_range(it->item_start, it->item_end, it->laid_start, it->laid_end, 0, dy);
            }
        }
        cy += line_h;
        start = end;
        if (start < count) cy += s->row_gap;
    }
    total_h = cy - y;
    pool_depth--;
    layout_out_of_flow_children(n, x, y, w);
    return total_h;
}

static int layout_grid(mx_node_t* n, int x, int y, int w) {
    const mx_style_t* s = n->style;
    if (has_loose_text(n)) return -1;
    int cols = s->grid_columns;
    if (cols < 0) {
        int min = -cols;
        cols = (w + s->column_gap) / (min + s->column_gap);
    }
    if (cols < 1) cols = 1;
    if (cols > 24) cols = 24;
    int col_w = (w - s->column_gap * (cols - 1)) / cols;
    int cy = y, col = 0, row_h = 0;
    mx_node_t* row_items[24];
    int row_count = 0;
    for (mx_node_t* c = n->first; c; c = c->next) {
        if (c->type != MX_NODE_ELEMENT || is_hidden(c) || is_out_of_flow(c)) continue;
        const mx_style_t* cs = c->style;
        int mt = mx_len_resolve(cs->margin[0], w), mb = mx_len_resolve(cs->margin[2], w);
        int cx = x + col * (col_w + s->column_gap);
        int forced = col_w - mx_len_resolve(cs->margin[3], w) - mx_len_resolve(cs->margin[1], w);
        if (cs->width.kind == MX_LEN_SET) forced = -1;
        int h = layout_block(c, cx, cy + mt, col_w, forced);
        if (h + mt + mb > row_h) row_h = h + mt + mb;
        row_items[row_count++] = c;
        if (++col == cols) {
            for (int i = 0; i < row_count; i++)
                if (row_items[i]->style->height.kind != MX_LEN_SET)
                    stretch_box(row_items[i], row_h - mx_len_resolve(row_items[i]->style->margin[0], w) -
                                                  mx_len_resolve(row_items[i]->style->margin[2], w));
            cy += row_h + s->row_gap;
            col = 0;
            row_h = 0;
            row_count = 0;
        }
    }
    if (col) {
        for (int i = 0; i < row_count; i++)
            if (row_items[i]->style->height.kind != MX_LEN_SET)
                stretch_box(row_items[i], row_h);
        cy += row_h;
    } else if (cy > y) {
        cy -= s->row_gap;
    }
    layout_out_of_flow_children(n, x, y, w);
    return cy - y;
}

#define TABLE_MAX_COLS 48
#define TABLE_MAX_ROWS 512

typedef struct {
    mx_node_t* rows[TABLE_MAX_ROWS];
    int col_min[TABLE_MAX_COLS], col_max[TABLE_MAX_COLS], widths[TABLE_MAX_COLS];
    mx_node_t* cells[TABLE_MAX_COLS];
    int cell_h[TABLE_MAX_COLS];
    unsigned cell_items[TABLE_MAX_COLS][2], cell_laid[TABLE_MAX_COLS][2];
    int col_pct[TABLE_MAX_COLS];   /* width="25%" on a cell: percent * 100 */
} table_work_t;

#define TABLE_SPACING 2

/* Finds the rows (captions skipped) and sizes the columns: smallest and
 * widest content, and the percentage width cells ask for. Returns the
 * column count. */
static int table_columns(mx_node_t* table, table_work_t* t, int* row_count_out) {
    int row_count = 0, cols = 0;
    for (mx_node_t* c = table->first; c; c = c->next) {
        if (c->type != MX_NODE_ELEMENT || is_hidden(c)) continue;
        if (c->style->display == MX_DISPLAY_TABLE_ROW) {
            if (row_count < TABLE_MAX_ROWS) t->rows[row_count++] = c;
        } else if (c->style->display == MX_DISPLAY_TABLE_GROUP) {
            for (mx_node_t* r = c->first; r; r = r->next)
                if (r->type == MX_NODE_ELEMENT && !is_hidden(r) &&
                    r->style->display == MX_DISPLAY_TABLE_ROW && row_count < TABLE_MAX_ROWS)
                    t->rows[row_count++] = r;
        }
    }
    for (int r = 0; r < row_count; r++) {
        int col = 0;
        for (mx_node_t* cell = t->rows[r]->first; cell; cell = cell->next) {
            if (cell->type != MX_NODE_ELEMENT || is_hidden(cell)) continue;
            int span = attr_int(cell, "colspan", 1);
            if (span < 1) span = 1;
            if (col + span > TABLE_MAX_COLS) break;
            int mn, mxw;
            measure(cell, &mn, &mxw);
            for (int k = 0; k < span; k++) {
                int share_min = mn / span, share_max = mxw / span;
                if (share_min > t->col_min[col + k]) t->col_min[col + k] = share_min;
                if (share_max > t->col_max[col + k]) t->col_max[col + k] = share_max;
            }
            const mx_len_t* cw = &cell->style->width;
            if (span == 1 && cw->kind == MX_LEN_SET && cw->pct > 0 && !cw->px &&
                cw->pct > t->col_pct[col])
                t->col_pct[col] = cw->pct;
            col += span;
        }
        if (col > cols) cols = col;
    }
    int pct_total = 0;
    for (int i = 0; i < cols; i++) {
        if (t->col_max[i] < t->col_min[i]) t->col_max[i] = t->col_min[i];
        /* percentages past 100% are dropped, as browsers do */
        if (pct_total + t->col_pct[i] > 10000) t->col_pct[i] = 0;
        pct_total += t->col_pct[i];
    }
    *row_count_out = row_count;
    return cols;
}

/* The width the columns want, without spacing. A 25% column holding 100px
 * needs a 400px table; the other columns need what the percentages leave. */
static int table_wanted(const table_work_t* t, int cols) {
    int sum_max = 0, rest_max = 0, pct_total = 0, need = 0;
    for (int i = 0; i < cols; i++) {
        sum_max += t->col_max[i];
        if (t->col_pct[i]) {
            pct_total += t->col_pct[i];
            int w = (int)((long)t->col_max[i] * 10000 / t->col_pct[i]);
            if (w > need) need = w;
        } else {
            rest_max += t->col_max[i];
        }
    }
    if (pct_total && pct_total < 10000) {
        int w = (int)((long)rest_max * 10000 / (10000 - pct_total));
        if (w > need) need = w;
    }
    return need > sum_max ? need : sum_max;
}

static void table_measure(mx_node_t* table, int* min_out, int* max_out) {
    table_work_t* t = (table_work_t*)mx_arena_zalloc(&layout_arena, sizeof(table_work_t));
    *min_out = *max_out = 0;
    if (!t) return;
    int row_count;
    int cols = table_columns(table, t, &row_count);
    if (!cols) return;
    int sum_min = 0;
    for (int i = 0; i < cols; i++) sum_min += t->col_min[i];
    *min_out = sum_min + TABLE_SPACING * (cols + 1);
    *max_out = table_wanted(t, cols) + TABLE_SPACING * (cols + 1);
}

/* Shares `amount` between the columns in `pick` (1 = take part): first up
 * to each one's widest content, then the rest by that width. */
static void table_share(int* widths, const int* col_min, const int* col_max,
                        const int* pick, int cols, int amount) {
    int sum_min = 0, sum_max = 0, count = 0;
    for (int i = 0; i < cols; i++)
        if (pick[i]) { sum_min += col_min[i]; sum_max += col_max[i]; count++; }
    if (!count) return;
    for (int i = 0; i < cols; i++) {
        if (!pick[i]) continue;
        if (amount <= sum_min) {
            widths[i] = col_min[i];
        } else if (amount <= sum_max) {
            int range = sum_max - sum_min;
            widths[i] = col_min[i] + (range ? (int)((long)(amount - sum_min) *
                                                    (col_max[i] - col_min[i]) / range) : 0);
        } else {
            int extra = amount - sum_max;
            widths[i] = col_max[i] + (sum_max ? (int)((long)extra * col_max[i] / sum_max)
                                              : extra / count);
        }
    }
}

static int layout_table(mx_node_t* table, int x, int y, int w) {
    /* the work area lives in the layout arena: tables nest */
    table_work_t* t = (table_work_t*)mx_arena_zalloc(&layout_arena, sizeof(table_work_t));
    if (!t) return 0;
    int caption_h = 0;
    for (mx_node_t* c = table->first; c; c = c->next) {
        if (c->type != MX_NODE_ELEMENT || is_hidden(c)) continue;
        if (c->style->display == MX_DISPLAY_TABLE_CAPTION)
            caption_h += layout_block(c, x, y + caption_h, w, -1);
    }
    y += caption_h;
    int spacing = TABLE_SPACING;
    int row_count;
    int cols = table_columns(table, t, &row_count);
    if (!cols) return caption_h;
    mx_node_t** rows = t->rows;
    int* col_min = t->col_min;
    int* col_max = t->col_max;
    int* col_pct = t->col_pct;
    int sum_min = 0, sum_max = 0, pct_total = 0;
    for (int i = 0; i < cols; i++) {
        sum_min += col_min[i];
        sum_max += col_max[i];
        pct_total += col_pct[i];
    }
    int avail = w - spacing * (cols + 1);
    int fixed_width = table->style->width.kind == MX_LEN_SET;
    int* widths = t->widths;
    if (pct_total && sum_min < avail) {
        /* percentage columns take their share of the table, the others
         * the rest (the table was sized for this in layout_block) */
        int pick[TABLE_MAX_COLS];
        int used = 0, auto_cols = 0;
        for (int i = 0; i < cols; i++) {
            pick[i] = !col_pct[i];
            auto_cols += pick[i];
            if (col_pct[i]) {
                widths[i] = (int)((long)avail * col_pct[i] / 10000);
                if (widths[i] < col_min[i]) widths[i] = col_min[i];
                used += widths[i];
            }
        }
        if (auto_cols) {
            table_share(widths, col_min, col_max, pick, cols, avail - used);
        } else if (avail > used) {
            for (int i = 0; i < cols; i++)
                widths[i] += (int)((long)(avail - used) * col_pct[i] / pct_total);
        }
    } else if (sum_max <= avail) {
        int extra = fixed_width ? avail - sum_max : 0;
        for (int i = 0; i < cols; i++)
            widths[i] = col_max[i] + (sum_max ? (int)((long)extra * col_max[i] / sum_max)
                                              : extra / cols);
    } else if (sum_min >= avail) {
        for (int i = 0; i < cols; i++) widths[i] = col_min[i];
    } else {
        int range = sum_max - sum_min;
        int extra = avail - sum_min;
        for (int i = 0; i < cols; i++)
            widths[i] = col_min[i] + (range ? (int)((long)extra * (col_max[i] - col_min[i]) / range)
                                            : 0);
    }
    int cy = y + spacing;
    for (int r = 0; r < row_count; r++) {
        mx_node_t* row = rows[r];
        int row_item;
        emit_box_decorations(row, x, cy, w, 1, &row_item);
        mx_node_t** cells = t->cells;
        int* cell_h = t->cell_h;
        unsigned (*cell_items)[2] = t->cell_items;
        unsigned (*cell_laid)[2] = t->cell_laid;
        int cell_count = 0, row_h = 0, col = 0;
        int cx = x + spacing;
        for (mx_node_t* cell = row->first; cell; cell = cell->next) {
            if (cell->type != MX_NODE_ELEMENT || is_hidden(cell)) continue;
            int span = attr_int(cell, "colspan", 1);
            if (span < 1) span = 1;
            if (col + span > cols) span = cols - col;
            if (span <= 0) break;
            int cw = 0;
            for (int k = 0; k < span; k++) cw += widths[col + k];
            cw += spacing * (span - 1);
            unsigned is = item_count, ls = laid_count;
            int h = layout_block(cell, cx, cy, cw, cw);
            cells[cell_count] = cell;
            cell_h[cell_count] = h;
            cell_items[cell_count][0] = is;
            cell_items[cell_count][1] = item_count;
            cell_laid[cell_count][0] = ls;
            cell_laid[cell_count][1] = laid_count;
            cell_count++;
            if (h > row_h) row_h = h;
            cx += cw + spacing;
            col += span;
            if (cell_count >= TABLE_MAX_COLS) break;
        }
        for (int i = 0; i < cell_count; i++) {
            mx_node_t* cell = cells[i];
            if (cell->style->vertical_middle && cell_h[i] < row_h) {
                /* move the content, not the background */
                unsigned first_content = cell_items[i][0];
                while (first_content < cell_items[i][1] && items[first_content].node == cell &&
                       items[first_content].part)
                    first_content++;
                shift_range(first_content, cell_items[i][1], cell_laid[i][0] + 1, cell_laid[i][1],
                            0, (row_h - cell_h[i]) / 2);
            }
            stretch_box(cell, row_h);
        }
        resize_decorations(row_item, row, 1, row_h);
        row->box_x = x;
        row->box_y = cy;
        row->box_w = w;
        row->box_h = row_h;
        row->layout_gen = layout_generation;
        record_laid(row);
        cy += row_h + spacing;
    }
    return cy - y + caption_h;
}

/* ====================================================================== */
/* Block layout                                                           */
/* ====================================================================== */

static int layout_flow(mx_node_t* n, int x, int y, int w) {
    const mx_style_t* s = n->style;
    if (s->display == MX_DISPLAY_FLEX || s->display == MX_DISPLAY_INLINE_FLEX) {
        int h = layout_flex(n, x, y, w);
        if (h >= 0) return h;
    } else if (s->display == MX_DISPLAY_GRID) {
        int h = layout_grid(n, x, y, w);
        if (h >= 0) return h;
    } else if (s->display == MX_DISPLAY_TABLE) {
        return layout_table(n, x, y, w);
    }
    int cy = y;
    int prev_mb = 0;
    mx_node_t* run = NULL;
    for (mx_node_t* c = n->first;; c = c->next) {
        int inline_child = c && (is_inline_level(c) || c->type == MX_NODE_COMMENT ||
                                 (c->type == MX_NODE_ELEMENT && is_hidden(c)));
        if (c && inline_child) {
            if (!run) run = c;
            continue;
        }
        if (run) {
            int h = layout_inline_run(n, run, c, x, cy + (prev_mb > 0 ? prev_mb : 0), w);
            if (h > 0) {
                cy += (prev_mb > 0 ? prev_mb : 0) + h;
                prev_mb = 0;
            }
            run = NULL;
        }
        if (!c) break;
        if (c->type != MX_NODE_ELEMENT) continue;
        if (is_out_of_flow(c)) {
            layout_absolute(c, x, cy + prev_mb, w);
            continue;
        }
        const mx_style_t* cs = c->style;
        int mt = mx_len_resolve(cs->margin[0], w);
        int mb = mx_len_resolve(cs->margin[2], w);
        int gap = (prev_mb >= 0 && mt >= 0) ? (prev_mb > mt ? prev_mb : mt) : prev_mb + mt;
        if (cs->display == MX_DISPLAY_LIST_ITEM) {
            marker.pending = 1;
            marker.style = cs;
            marker.node = c;
            int value = attr_int(c, "value", -1);
            if (value >= 0) marker.number = value;
            else marker.number++;
        }
        int h = layout_block(c, x, cy + gap, w, -1);
        marker.pending = 0;
        cy += gap + h;
        prev_mb = mb;
    }
    if (prev_mb > 0) cy += prev_mb;
    return cy - y;
}

static int layout_block(mx_node_t* n, int x, int y, int container_w,
                        int forced_w) {
    const mx_style_t* s = n->style;
    if (!s || s->display == MX_DISPLAY_NONE) return 0;
    if (s->display == MX_DISPLAY_CONTENTS) return layout_flow(n, x, y, container_w);
    int cw = container_w;
    int ml = s->margin[3].kind == MX_LEN_SET ? mx_len_resolve(s->margin[3], cw) : 0;
    int mr = s->margin[1].kind == MX_LEN_SET ? mx_len_resolve(s->margin[1], cw) : 0;
    int bl = s->border_width[3], br = s->border_width[1];
    int bt = s->border_width[0], bb = s->border_width[2];
    int pl = mx_len_resolve(s->padding[3], cw), pr = mx_len_resolve(s->padding[1], cw);
    int pt = mx_len_resolve(s->padding[0], cw), pb = mx_len_resolve(s->padding[2], cw);
    int frame_h = bl + br + pl + pr;
    int content_w;
    int explicit_width = 0;
    if (forced_w >= 0) {
        content_w = forced_w - frame_h;
        explicit_width = 1;
    } else if (s->width.kind == MX_LEN_SET) {
        content_w = mx_len_resolve(s->width, cw);
        if (s->border_box) content_w -= frame_h;
        explicit_width = 1;
    } else {
        content_w = cw - ml - mr - frame_h;
    }
    if (s->max_width.kind == MX_LEN_SET) {
        int max = mx_len_resolve(s->max_width, cw) - (s->border_box ? frame_h : 0);
        if (content_w > max) {
            content_w = max;
            explicit_width = 1;
        }
    }
    if (s->min_width.kind == MX_LEN_SET) {
        int min = mx_len_resolve(s->min_width, cw) - (s->border_box ? frame_h : 0);
        if (content_w < min) content_w = min;
    }
    if (forced_w < 0 && s->display == MX_DISPLAY_TABLE && s->width.kind != MX_LEN_SET) {
        /* an auto-width table is as wide as its columns want (so <center>
         * and margin:auto can centre it), never wider than the space */
        int tmin, tmax;
        measure(n, &tmin, &tmax);
        int want = tmax - frame_h - ml - mr;
        if (want < content_w) {
            content_w = want;
            explicit_width = 1;
        }
    }
    if (content_w < 0) content_w = 0;
    if (explicit_width && forced_w < 0) {
        int free_w = cw - content_w - frame_h - ml - mr;
        int auto_l = s->margin[3].kind == MX_LEN_AUTO, auto_r = s->margin[1].kind == MX_LEN_AUTO;
        const mx_style_t* ps = n->parent && n->parent->style ? n->parent->style : NULL;
        if (ps && ps->text_align == MX_ALIGN_LEGACY_CENTER && !auto_l && !auto_r &&
            s->margin[3].kind != MX_LEN_SET && s->margin[1].kind != MX_LEN_SET)
            auto_l = auto_r = 1;
        if (free_w > 0 && auto_l && auto_r) ml += free_w / 2;
        else if (free_w > 0 && auto_l) ml += free_w;
    }
    int bx = x + ml;
    int border_w = content_w + frame_h;

    unsigned saved_opacity = current_opacity;
    if (s->opacity < 255) current_opacity = current_opacity * s->opacity / 255;
    int saved_clip = current_clip;

    n->box_x = bx;
    n->box_y = y;
    n->box_w = border_w;
    n->box_h = 0;
    n->layout_gen = layout_generation;
    record_laid(n);
    unsigned first_item = item_count;
    int bg_index = -1;
    (void)first_item;
    int invisible = current_opacity < 12;
    if (!invisible) emit_box_decorations(n, bx, y, border_w, 1, &bg_index);
    n->layout_item = bg_index;

    int fixed_h = -1;
    if (s->height.kind == MX_LEN_SET) {
        int reference = (n == doc.html || n == doc.body || s->height.pct == 0)
                            ? viewport_h : -1;
        if (s->height.pct && cb_depth && cb_stack[cb_depth - 1].h > 0)
            reference = cb_stack[cb_depth - 1].h;
        if (reference >= 0 || !s->height.pct) {
            fixed_h = mx_len_resolve(s->height, reference > 0 ? reference : 0);
            if (s->border_box) fixed_h -= bt + bb + pt + pb;
            if (fixed_h < 0) fixed_h = 0;
        }
    }
    if (s->max_height.kind == MX_LEN_SET && !s->max_height.pct && s->overflow_hidden &&
        fixed_h < 0) {
        /* clip at max-height */
    }
    int clip_h = fixed_h;
    if (clip_h < 0 && s->max_height.kind == MX_LEN_SET && !s->max_height.pct)
        clip_h = s->max_height.px;
    if (s->overflow_hidden && clip_h >= 0)
        current_clip = push_clip(bx, y, border_w, clip_h + bt + bb + pt + pb);
    else if (s->overflow_hidden && s->width.kind == MX_LEN_SET)
        current_clip = push_clip(bx, y - 4096, border_w, 1 << 20);

    int positioned = s->position != MX_POS_STATIC && cb_depth < 32;
    if (positioned) {
        cb_stack[cb_depth].node = n;
        cb_stack[cb_depth].x = bx;
        cb_stack[cb_depth].y = y;
        cb_stack[cb_depth].w = border_w;
        cb_stack[cb_depth].h = fixed_h >= 0 ? fixed_h + bt + bb + pt + pb : -1;
        cb_depth++;
    }
    int content_x = bx + bl + pl, content_y = y + bt + pt;
    n->content_w = content_w;
    marker_t saved_marker = marker;
    if (mx_dom_is(n, "ol") || mx_dom_is(n, "ul") || mx_dom_is(n, "menu")) {
        marker.number = attr_int(n, "start", 1) - 1;
        marker.pending = 0;
    }
    int h;
    if (is_replaced(n)) {
        int rw, rh;
        replaced_size(n, cw, &rw, &rh);
        h = rh - bt - bb - pt - pb;
        if (h < 0) h = 0;
        if (!invisible) emit_control(n, bx, y, border_w, rh);
    } else {
        h = layout_flow(n, content_x, content_y, content_w);
        if (mx_dom_is(n, "button") && n->first == NULL) h = font_px(s);
    }
    if (mx_dom_is(n, "ol") || mx_dom_is(n, "ul") || mx_dom_is(n, "menu")) {
        int number = marker.number;
        marker = saved_marker;
        (void)number;
    }
    if (marker.pending && marker.node == n) {
        /* a list item without lines */
        emit_marker(content_x, content_y + mx_text_ascent_px(font_px(s), 0),
                    content_y, line_height_of(s));
    }
    n->content_h = h;
    if (fixed_h >= 0) h = fixed_h;
    if (s->min_height.kind == MX_LEN_SET) {
        int min = mx_len_resolve(s->min_height, viewport_h);
        if (s->min_height.pct && n != doc.html && n != doc.body) min = 0;
        if (s->border_box) min -= bt + bb + pt + pb;
        if (h < min) h = min;
    }
    if (s->max_height.kind == MX_LEN_SET && !s->max_height.pct && s->overflow_hidden &&
        h > s->max_height.px)
        h = s->max_height.px;
    int border_h = bt + pt + h + pb + bb;
    n->box_h = border_h;
    resize_decorations(bg_index, n, 1, border_h);
    if (positioned) {
        cb_depth--;
        if (s->position == MX_POS_RELATIVE &&
            (s->offset[0].kind == MX_LEN_SET || s->offset[3].kind == MX_LEN_SET)) {
            int dx = mx_len_resolve(s->offset[3], cw), dy = mx_len_resolve(s->offset[0], cw);
            /* find this node in the laid list */
            unsigned laid_start = laid_count;
            while (laid_start > 0 && laid[laid_start - 1] != n) laid_start--;
            if (laid_start) laid_start--;
            shift_range(first_item, item_count, laid_start, laid_count, dx, dy);
        }
    }
    current_clip = saved_clip;
    current_opacity = saved_opacity;
    return border_h;
}

/* ====================================================================== */
/* Document layout                                                        */
/* ====================================================================== */

static void layout_document(void) {
    long start = mx_now_ms();
    mx_arena_free_all(&layout_arena);
    layout_arena.limit = 64u * 1024u * 1024u;
    items = NULL;
    item_count = item_capacity = 0;
    laid = NULL;
    laid_count = laid_capacity = 0;
    clips = NULL;
    clip_count = clip_capacity = 0;
    layout_generation++;
    cb_depth = 0;
    marker.pending = 0;
    marker.number = 0;
    ifc_depth = 0;
    current_opacity = 255;
    mx_css_env_t env = {viewport_w, viewport_h, prefers_dark};
    mx_css_set_env(&env);
    clip_t whole = {-100000, -100000, 1000000, 10000000, 0};
    unsigned capacity = 0;
    clips = (clip_t*)grow(NULL, 0, &capacity, sizeof(clip_t));
    if (!clips) return;
    clip_capacity = (int)capacity;
    clips[0] = whole;
    clip_count = 1;
    current_clip = 0;

    mx_css_initial(&root_parent_style);
    mx_style_t* doc_style = (mx_style_t*)mx_arena_alloc(&layout_arena, sizeof(mx_style_t));
    if (!doc_style) return;
    *doc_style = root_parent_style;
    doc_style->display = MX_DISPLAY_BLOCK;
    doc.root->style = doc_style;
    mx_css_bloom(1);
    compute_styles(doc.root, &root_parent_style);
    mx_css_bloom(0);
    long styled = mx_now_ms();
    stats.style_ms = styled - start;

    page_background = mx_css_dark_page() ? 0xFF1F1F1Fu : 0xFFFFFFFFu;
    if (doc.html && doc.html->style && (doc.html->style->background >> 24))
        page_background = doc.html->style->background;
    else if (doc.body && doc.body->style && (doc.body->style->background >> 24))
        page_background = doc.body->style->background;
    if ((page_background >> 24) < 255)
        page_background = mx_mix(mx_css_dark_page() ? 0xFF1F1F1Fu : 0xFFFFFFFFu,
                                 page_background | 0xFF000000u,
                                 page_background >> 24);

    int h = 0;
    if (doc.html && doc.html->style && doc.html->style->display != MX_DISPLAY_NONE)
        h = layout_block(doc.html, 0, 0, viewport_w, -1);
    content_height = h;
    stats.layout_ms = mx_now_ms() - styled;
    stats.items = item_count;
    stats.rules = mx_css_rule_count();
    stats.elements = doc.element_count;
    stats.dom_bytes = doc.arena.total;
    stats.layouts++;
    doc.mutated = 0;
    int maximum = content_height > viewport_h ? content_height - viewport_h : 0;
    if (scroll_y > maximum) scroll_y = maximum;
}

/* ====================================================================== */
/* Loading                                                                */
/* ====================================================================== */

static mx_fetch_fn fetch_callback;
static void* fetch_user;
static int fetch_enabled;

static void add_stylesheets(void) {
    unsigned sheets = 0;
    for (mx_node_t* n = doc.root; n; n = mx_dom_next(doc.root, n)) {
        if (n->type != MX_NODE_ELEMENT) continue;
        if (mx_dom_is(n, "style")) {
            const char* media = mx_dom_attr(n, "media");
            if (media && *media && !mx_css_media_matches(media)) continue;
            char* text = mx_dom_text_content(&doc, n);
            mx_css_add_stylesheet(text, strlen(text), 1);
            sheets++;
        } else if (mx_dom_is(n, "link")) {
            const char* rel = mx_dom_attr(n, "rel");
            const char* href = mx_dom_attr(n, "href");
            if (!rel || !href || !strstr(rel, "stylesheet") || strstr(rel, "alternate"))
                continue;
            const char* media = mx_dom_attr(n, "media");
            if (media && *media && !mx_css_media_matches(media)) continue;
            if (mx_dom_attr(n, "disabled")) continue;
            if (!fetch_enabled || !fetch_callback) continue;
            if (sheets >= 32) {
                mx_console_add(MX_LOG_WARN, "CSS", 0, "Too many stylesheets; skipping the rest.");
                break;
            }
            char url[2048];
            mx_html_resolve(href, url, sizeof(url));
            char* body = NULL;
            size_t length = 0;
            long t0 = mx_now_ms();
            if (fetch_callback(fetch_user, url, "stylesheet", &body, &length)) {
                mx_css_add_stylesheet(body, length, 1);
                free(body);
                sheets++;
            }
            stats.fetch_ms += mx_now_ms() - t0;
        }
    }
    stats.stylesheets = sheets;
}

static int script_is_javascript(mx_node_t* script) {
    const char* type = mx_dom_attr(script, "type");
    if (!type || !*type) return 1;
    return mx_streq_nocase(type, "text/javascript") ||
           mx_streq_nocase(type, "application/javascript") ||
           mx_streq_nocase(type, "text/ecmascript") ||
           mx_streq_nocase(type, "application/x-javascript") ||
           mx_streq_nocase(type, "module");
}

static void html_navigate(const char* url, void* user);
static void (*host_navigate)(const char* url, void* user);
static void (*host_alert)(const char* text, void* user);
static void* host_user;
static char pending_navigation[2048];

static void html_navigate(const char* url, void* user) {
    (void)user;
    mx_html_resolve(url, pending_navigation, sizeof(pending_navigation));
    if (host_navigate) host_navigate(pending_navigation, host_user);
}

static void html_alert(const char* text, void* user) {
    (void)user;
    if (host_alert) host_alert(text, host_user);
}

static int html_fetch(void* user, const char* url, const char* kind, char** body,
                      size_t* length) {
    (void)user;
    if (!fetch_callback) return 0;
    char resolved[2048];
    mx_html_resolve(url, resolved, sizeof(resolved));
    return fetch_callback(fetch_user, resolved, kind, body, length);
}

static void run_scripts(void) {
    mx_node_t* list[256];
    int count = 0;
    for (mx_node_t* n = doc.root; n && count < 256; n = mx_dom_next(doc.root, n))
        if (mx_dom_is(n, "script")) list[count++] = n;
    unsigned ran = 0;
    int inline_index = 0;
    for (int i = 0; i < count; i++) {
        mx_node_t* script = list[i];
        script->checked = 1;   /* marks the script as started */
        const char* script_type = mx_dom_attr(script, "type");
        if (script_type && mx_streq_nocase(script_type, "importmap")) {
            char* text = mx_dom_text_content(&doc, script);
            mx_js_set_import_map(text, strlen(text));
            continue;
        }
        if (!script_is_javascript(script)) continue;
        if (mx_dom_attr(script, "nomodule")) continue;   /* we run modules */
        const char* type = mx_dom_attr(script, "type");
        int module = type && mx_streq_nocase(type, "module");
        const char* src = mx_dom_attr(script, "src");
        char name[48];
        if (src && *src) {
            if (!fetch_enabled || !fetch_callback) continue;
            char url[2048];
            mx_html_resolve(src, url, sizeof(url));
            const char* base = strrchr(url, '/');
            mx_copy(name, sizeof(name), base && base[1] ? base + 1 : url);
            char* q = strchr(name, '?');
            if (q) *q = '\0';
            char* body = NULL;
            size_t length = 0;
            long t0 = mx_now_ms();
            int ok = fetch_callback(fetch_user, url, "script", &body, &length);
            stats.fetch_ms += mx_now_ms() - t0;
            if (!ok) continue;
            long s0 = mx_now_ms();
            if (module) mx_js_run_module(body, length, url, name);
            else mx_js_run(body, length, name);
            stats.script_ms += mx_now_ms() - s0;
            free(body);
            ran++;
        } else {
            inline_index++;
            mx_copy(name, sizeof(name), module ? "inline module " : "inline script ");
            mx_append_int(name, sizeof(name), inline_index);
            char* text = mx_dom_text_content(&doc, script);
            long s0 = mx_now_ms();
            if (module) {
                char url[2048];
                mx_copy(url, sizeof(url), doc.base_url);
                mx_append(url, sizeof(url), "#module-");
                mx_append_int(url, sizeof(url), inline_index);
                mx_js_run_module(text, strlen(text), url, name);
            } else {
                mx_js_run(text, strlen(text), name);
            }
            stats.script_ms += mx_now_ms() - s0;
            ran++;
        }
    }
    stats.scripts = ran;
}

static void attach_inline_handlers(void);

mx_html_result_t mx_html_load(const char* source, size_t length,
                              const mx_html_options_t* options) {
    mx_html_unload();
    if (!source || !length) return MX_HTML_EMPTY;
    if (length > MX_HTML_SOURCE_MAX) return MX_HTML_TOO_LARGE;
    memset(&stats, 0, sizeof(stats));
    long t0 = mx_now_ms();
    viewport_w = options->viewport_width > 80 ? options->viewport_width : 80;
    viewport_h = options->viewport_height > 80 ? options->viewport_height : 80;
    prefers_dark = options->dark;
    scripts_enabled = options->scripts;
    fetch_callback = options->fetch;
    fetch_user = options->fetch_user;
    fetch_enabled = options->subresources;
    host_navigate = options->navigate;
    host_alert = options->alert;
    host_user = options->host_user;
    scroll_y = 0;
    focused = highlighted = NULL;
    mx_dom_init(&doc);
    mx_copy(doc.base_url, sizeof(doc.base_url), options->url ? options->url : "");
    mx_dom_parse(&doc, source, length);
    if (doc.arena.failed) {
        mx_dom_free(&doc);
        return MX_HTML_TOO_COMPLEX;
    }
    for (mx_node_t* n = doc.head; n; n = mx_dom_next(doc.head, n)) {
        if (mx_dom_is(n, "base") && mx_dom_attr(n, "href")) {
            char resolved[512];
            mx_url_resolve(doc.base_url, mx_dom_attr(n, "href"), resolved, sizeof(resolved));
            mx_copy(doc.base_url, sizeof(doc.base_url), resolved);
            break;
        }
    }
    /* initial form control state */
    for (mx_node_t* n = doc.root; n; n = mx_dom_next(doc.root, n)) {
        if (n->type != MX_NODE_ELEMENT) continue;
        if ((mx_dom_is(n, "input") && mx_dom_attr(n, "checked")) ||
            (mx_dom_is(n, "option") && mx_dom_attr(n, "selected")))
            n->checked = 1;
    }
    doc_loaded = 1;
    stats.parse_ms = mx_now_ms() - t0;

    mx_css_reset();
    {
        /* media queries are evaluated while parsing */
        mx_css_env_t env = {viewport_w, viewport_h, prefers_dark};
        mx_css_set_env(&env);
    }
    mx_css_add_stylesheet(mx_css_user_agent, mx_css_user_agent_length, 0);
    if (!scripts_enabled) {
        static const char noscript[] = "noscript{display:block}";
        mx_css_add_stylesheet(noscript, sizeof(noscript) - 1, 0);
    }
    add_stylesheets();
    {
        /* local pages may show local pictures and videos; network pages may not */
        char local[8];
        mx_media_begin(fetch_enabled ? fetch_callback : NULL, fetch_user,
                       mx_url_local_path(doc.base_url, local, sizeof(local)));
    }

    if (scripts_enabled) {
        mx_js_host_t host = {html_navigate, html_alert,
                             fetch_enabled ? html_fetch : NULL, NULL};
        mx_js_begin(&doc, doc.base_url, &host);
        attach_inline_handlers();
        run_scripts();
        long s0 = mx_now_ms();
        mx_js_dispatch_window("DOMContentLoaded");
        mx_js_dispatch_window("load");
        stats.script_ms += mx_now_ms() - s0;
        stats.js_bytes = mx_js_heap_bytes();
    }
    layout_document();
    return MX_HTML_OK;
}

static void attach_inline_handlers(void) {
    /* onclick="..." attributes are compiled lazily by the JS engine when
     * events are dispatched; nothing to do here. */
}

void mx_html_unload(void) {
    mx_media_end();
    if (scripts_enabled) mx_js_end();
    if (doc_loaded) mx_dom_free(&doc);
    doc_loaded = 0;
    mx_arena_free_all(&layout_arena);
    items = NULL;
    item_count = item_capacity = 0;
    laid = NULL;
    laid_count = laid_capacity = 0;
    clips = NULL;
    clip_count = clip_capacity = 0;
    content_height = 0;
    focused = highlighted = NULL;
    page_background = 0xFFFFFFFFu;
}

int mx_html_loaded(void) {
    return doc_loaded;
}

void mx_html_relayout(int width, int height, int dark) {
    if (!doc_loaded) return;
    if (dark != prefers_dark) {
        /* @media (prefers-color-scheme) is evaluated while parsing */
        prefers_dark = dark;
        mx_css_env_t env = {width, height, dark};
        mx_css_set_env(&env);
        mx_css_reset();
        mx_css_add_stylesheet(mx_css_user_agent, mx_css_user_agent_length, 0);
        int saved = fetch_enabled;
        fetch_enabled = 0;   /* reuse only inline styles on theme switches */
        add_stylesheets();
        fetch_enabled = saved;
    } else if (width != viewport_w || height != viewport_h) {
        /* media queries depend on the viewport */
        mx_css_env_t env = {width, height, dark};
        mx_css_set_env(&env);
        mx_css_reset();
        mx_css_add_stylesheet(mx_css_user_agent, mx_css_user_agent_length, 0);
        int saved = fetch_enabled;
        fetch_enabled = 0;
        add_stylesheets();
        fetch_enabled = saved;
    }
    viewport_w = width > 80 ? width : 80;
    viewport_h = height > 80 ? height : 80;
    layout_document();
}

int mx_html_update(void) {
    if (!doc_loaded || !doc.mutated) return 0;
    layout_document();
    return 1;
}

void mx_html_viewport(int* width, int* height) {
    *width = viewport_w;
    *height = viewport_h;
}

void mx_html_ensure_layout(void) {
    if (doc_loaded && doc.mutated) layout_document();
}

int mx_html_media_tick(long now_ms) {
    if (!doc_loaded) return 0;
    return mx_media_tick(now_ms);
}

int mx_html_tick(long now_ms) {
    if (!doc_loaded || !scripts_enabled) return 0;
    if (mx_js_next_timer(now_ms) != 0) return 0;
    mx_js_run_timers(now_ms);
    return mx_html_update();
}

/* ====================================================================== */
/* Drawing                                                                */
/* ====================================================================== */

static void draw_item(const mx_canvas_t* base, const item_t* it, int ox, int oy) {
    const clip_t* cl = &clips[it->clip];
    mx_canvas_t c = mx_canvas_clip(base, cl->x0 + ox, cl->y0 + oy,
                                   cl->x1 - cl->x0, cl->y1 - cl->y0);
    int x = it->x + ox, y = it->y + oy;
    switch (it->kind) {
    case IT_RECT:
        if (it->radius > 0) mx_round_rect(&c, x, y, it->w, it->h, it->radius, it->color);
        else mx_fill(&c, x, y, it->w, it->h, it->color);
        break;
    case IT_BORDER:
        mx_round_rect_stroke(&c, x, y, it->w, it->h, it->radius, it->px, it->color);
        break;
    case IT_TEXT: {
        mx_text(&c, x, y, it->text, it->length, it->color, it->px, it->flags);
        if (it->deco) {
            int thickness = it->px >= 24 ? 2 : 1;
            int ascent = mx_text_ascent_px(it->px, it->flags);
            if (it->deco & MX_DECO_UNDERLINE)
                mx_fill(&c, x, y + ascent + 2, it->w, thickness, it->color);
            if (it->deco & MX_DECO_LINE_THROUGH)
                mx_fill(&c, x, y + ascent * 6 / 10, it->w, thickness, it->color);
            if (it->deco & MX_DECO_OVERLINE) mx_fill(&c, x, y, it->w, thickness, it->color);
        }
        break;
    }
    case IT_ICON:
        mx_icon(&c, (mx_icon_t)it->px, it->w, x, y, it->color);
        break;
    case IT_CHECKBOX:
        if (it->px) {
            mx_round_rect(&c, x, y, it->w, it->h, 3, it->color);
            mx_icon(&c, MX_ICON_CHECK, 16, x + (it->w - 16) / 2, y + (it->h - 16) / 2,
                    0xFFFFFFFFu);
        } else {
            mx_round_rect(&c, x, y, it->w, it->h, 3, 0xFFFFFFFFu);
            mx_round_rect_stroke(&c, x, y, it->w, it->h, 3, 1, 0xFF767C84u);
        }
        break;
    case IT_RADIO:
        mx_circle(&c, x + it->w / 2, y + it->h / 2, it->w / 2, 0xFFFFFFFFu);
        mx_ring(&c, x + it->w / 2, y + it->h / 2, it->w / 2, it->px ? 2 : 1,
                it->px ? it->color : 0xFF767C84u);
        if (it->px) mx_circle(&c, x + it->w / 2, y + it->h / 2, it->w / 4, it->color);
        break;
    case IT_CARET:
        mx_fill(&c, x, y, 1, it->h, it->color);
        break;
    case IT_MEDIA:
        mx_media_paint(&c, it->node, x, y, it->w, it->h, it->color >> 24, accent);
        break;
    }
}

void mx_html_draw(const mx_canvas_t* canvas, int top, int height, int scroll) {
    mx_canvas_t view = mx_canvas_clip(canvas, 0, top, canvas->width, height);
    mx_fill(&view, 0, top, canvas->width, height, page_background);
    if (!doc_loaded) return;
    int oy = top - scroll;
    int view_top = scroll - 64, view_bottom = scroll + height + 64;
    for (unsigned i = 0; i < item_count; i++) {
        const item_t* it = &items[i];
        if (it->y + it->h < view_top || it->y > view_bottom) {
            /* text items may have larger glyphs than h; keep a margin */
            continue;
        }
        draw_item(&view, it, 0, oy);
    }
    if (highlighted && highlighted->layout_gen == layout_generation) {
        mx_node_t* n = highlighted;
        const mx_style_t* s = n->style;
        int x = n->box_x, y = n->box_y + oy, w = n->box_w, h = n->box_h;
        if (s && n->type == MX_NODE_ELEMENT) {
            int mt = mx_len_resolve(s->margin[0], w), mb = mx_len_resolve(s->margin[2], w);
            int ml = mx_len_resolve(s->margin[3], w), mr = mx_len_resolve(s->margin[1], w);
            mx_fill(&view, x - ml, y - mt, w + ml + mr, mt, 0x66F6B26Bu);
            mx_fill(&view, x - ml, y + h, w + ml + mr, mb, 0x66F6B26Bu);
            mx_fill(&view, x - ml, y, ml, h, 0x66F6B26Bu);
            mx_fill(&view, x + w, y, mr, h, 0x66F6B26Bu);
            int pl = mx_len_resolve(s->padding[3], w) + s->border_width[3];
            int pt = mx_len_resolve(s->padding[0], w) + s->border_width[0];
            int pr = mx_len_resolve(s->padding[1], w) + s->border_width[1];
            int pb = mx_len_resolve(s->padding[2], w) + s->border_width[2];
            mx_fill(&view, x, y, w, h, 0x5593C47Du);
            mx_fill(&view, x + pl, y + pt, w - pl - pr, h - pt - pb, 0x666FA8DCu);
        }
        mx_stroke(&view, x, y, w, h, 0xFF2D7FF9u);
        /* label */
        char label[96];
        mx_copy(label, sizeof(label), n->tag ? n->tag : "#text");
        const char* id = mx_dom_attr(n, "id");
        if (id) {
            mx_append(label, sizeof(label), "#");
            mx_append(label, sizeof(label), id);
        }
        mx_append(label, sizeof(label), "  ");
        mx_append_int(label, sizeof(label), w);
        mx_append(label, sizeof(label), " x ");
        mx_append_int(label, sizeof(label), h);
        int lw = mx_text_width_px(label, (size_t)-1, 12, MX_TEXT_MEDIUM) + 14;
        int ly = y - 26 < top ? y + h + 4 : y - 26;
        mx_round_rect(&view, x, ly, lw, 22, 4, 0xF0202124u);
        mx_text(&view, x + 7, ly + 4, label, (size_t)-1, 0xFFFFFFFFu, 12, MX_TEXT_MEDIUM);
    }
}

void mx_html_scroll(int delta, uint32_t viewport_height) {
    mx_html_set_scroll(scroll_y + delta, viewport_height);
}

void mx_html_set_scroll(int y, uint32_t viewport_height) {
    int maximum = content_height > (int)viewport_height
                      ? content_height - (int)viewport_height : 0;
    scroll_y = y < 0 ? 0 : y > maximum ? maximum : y;
}

int mx_html_scroll_y(void) {
    return scroll_y;
}

int mx_html_content_height(void) {
    return content_height;
}

uint32_t mx_html_background(void) {
    return page_background;
}

const char* mx_html_title(void) {
    return doc_loaded ? doc.title : "";
}

mx_document_t* mx_html_document(void) {
    return doc_loaded ? &doc : NULL;
}

const mx_html_stats_t* mx_html_stats(void) {
    stats.js_bytes = scripts_enabled ? mx_js_heap_bytes() : 0;
    stats.dom_bytes = doc.arena.total;
    return &stats;
}

const char* mx_html_error(mx_html_result_t result) {
    switch (result) {
    case MX_HTML_EMPTY: return "The response contained no HTML.";
    case MX_HTML_TOO_LARGE: return "This page is larger than 2 MiB.";
    case MX_HTML_TOO_COMPLEX: return "This page needs more memory than Mxplorer allows.";
    default: return "HTML parsed successfully.";
    }
}

void mx_html_set_highlight(mx_node_t* node) {
    highlighted = node;
}

void mx_html_resolve(const char* reference, char* out, size_t capacity) {
    if (!mx_url_resolve(doc.base_url, reference ? reference : "", out, capacity))
        mx_copy(out, capacity, reference ? reference : "");
}

/* ====================================================================== */
/* Interaction                                                            */
/* ====================================================================== */

mx_node_t* mx_html_hit(int x, int y) {
    if (!doc_loaded) return NULL;
    for (unsigned i = item_count; i-- > 0;) {
        const item_t* it = &items[i];
        if (!it->node) continue;
        const clip_t* cl = &clips[it->clip];
        if (x < cl->x0 || x >= cl->x1 || y < cl->y0 || y >= cl->y1) continue;
        if (x >= it->x && x < it->x + it->w && y >= it->y && y < it->y + it->h)
            return it->node;
    }
    /* boxes without paint (transparent containers) */
    mx_node_t* best = NULL;
    for (unsigned i = 0; i < laid_count; i++) {
        mx_node_t* n = laid[i];
        if (n->layout_gen != layout_generation) continue;
        if (x >= n->box_x && x < n->box_x + n->box_w && y >= n->box_y &&
            y < n->box_y + n->box_h)
            best = n;
    }
    return best;
}

static mx_node_t* closest(mx_node_t* n, const char* tag) {
    for (; n; n = n->parent)
        if (mx_dom_is(n, tag)) return n;
    return NULL;
}

const char* mx_html_link_at(int x, int y) {
    mx_node_t* n = mx_html_hit(x, y);
    mx_node_t* a = closest(n, "a");
    if (!a) a = closest(n, "area");
    const char* href = a ? mx_dom_attr(a, "href") : NULL;
    if (!href || !*href) return NULL;
    if (mx_starts_with_nocase(href, "javascript:")) return href;
    mx_html_resolve(href, resolved_link, sizeof(resolved_link));
    return resolved_link;
}

int mx_html_pointer_at(int x, int y) {
    mx_node_t* n = mx_html_hit(x, y);
    if (!n) return 0;
    if (mx_html_link_at(x, y)) return 1;
    for (; n && n->type == MX_NODE_ELEMENT; n = n->parent) {
        if (n->style && n->style->pointer) return 1;
        if (mx_media_pointer(n)) return 1;
        if (mx_dom_is(n, "button") || mx_dom_is(n, "select") || mx_dom_is(n, "summary") ||
            (mx_dom_is(n, "input") && !is_text_input(n)) || mx_dom_attr(n, "onclick"))
            return 1;
    }
    return 0;
}

static void append_encoded(char* out, size_t capacity, const char* text) {
    static const char hex[] = "0123456789ABCDEF";
    size_t used = strlen(out);
    for (const unsigned char* p = (const unsigned char*)text; *p && used + 4 < capacity; p++) {
        unsigned char c = *p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out[used++] = (char)c;
        } else if (c == ' ') {
            out[used++] = '+';
        } else {
            out[used++] = '%';
            out[used++] = hex[c >> 4];
            out[used++] = hex[c & 15];
        }
    }
    out[used] = '\0';
}

static int submit_form(mx_node_t* form, mx_node_t* submitter, char* navigate,
                       size_t capacity) {
    if (!form) return 0;
    if (scripts_enabled && mx_js_dispatch(form, "submit", NULL)) return 0;
    const char* action = mx_dom_attr(form, "action");
    const char* method = mx_dom_attr(form, "method");
    if (method && mx_streq_nocase(method, "post"))
        mx_console_add(MX_LOG_WARN, "Forms", 0,
                       "POST forms are sent as GET requests by Mxplorer.");
    char url[2048];
    mx_html_resolve(action && *action ? action : doc.base_url, url, sizeof(url));
    char* q = strchr(url, '?');
    if (q) *q = '\0';
    char* hash = strchr(url, '#');
    if (hash) *hash = '\0';
    mx_append(url, sizeof(url), "?");
    int first = 1;
    for (mx_node_t* n = form; n; n = mx_dom_next(form, n)) {
        if (n->type != MX_NODE_ELEMENT) continue;
        const char* name = mx_dom_attr(n, "name");
        if (!name || !*name || mx_dom_attr(n, "disabled")) continue;
        const char* value = NULL;
        if (mx_dom_is(n, "input")) {
            const char* t = input_type(n);
            if (mx_streq_nocase(t, "checkbox") || mx_streq_nocase(t, "radio")) {
                if (!n->checked) continue;
                value = mx_dom_attr(n, "value");
                if (!value) value = "on";
            } else if (is_button_input(n)) {
                if (n != submitter) continue;
                value = mx_dom_attr(n, "value");
                if (!value) value = "";
            } else {
                value = control_value(n);
            }
        } else if (mx_dom_is(n, "button")) {
            if (n != submitter) continue;
            value = mx_dom_attr(n, "value");
            if (!value) value = "";
        } else if (mx_dom_is(n, "textarea")) {
            value = control_value(n);
        } else if (mx_dom_is(n, "select")) {
            mx_node_t* option = selected_option(n);
            if (!option) continue;
            value = mx_dom_attr(option, "value");
            if (!value) value = mx_dom_text_content(&doc, option);
        } else {
            continue;
        }
        if (!first) mx_append(url, sizeof(url), "&");
        first = 0;
        append_encoded(url, sizeof(url), name);
        mx_append(url, sizeof(url), "=");
        append_encoded(url, sizeof(url), value);
    }
    mx_copy(navigate, capacity, url);
    return 1;
}

static void set_value(mx_node_t* n, const char* value) {
    n->value = mx_arena_strdup(&doc.arena, value);
    doc.mutated = 1;
}

static int activate(mx_node_t* target, char* navigate, size_t capacity) {
    for (mx_node_t* n = target; n && n->type == MX_NODE_ELEMENT; n = n->parent) {
        if ((mx_dom_is(n, "a") || mx_dom_is(n, "area")) && mx_dom_attr(n, "href")) {
            const char* href = mx_dom_attr(n, "href");
            if (mx_starts_with_nocase(href, "javascript:")) {
                if (scripts_enabled) {
                    const char* code = href + 11;
                    char decoded[2048];
                    size_t used = 0;
                    for (const char* p = code; *p && used + 1 < sizeof(decoded); p++) {
                        if (*p == '%' && p[1] && p[2]) {
                            int hi = p[1] >= 'a' ? p[1] - 'a' + 10 : p[1] >= 'A' ? p[1] - 'A' + 10 : p[1] - '0';
                            int lo = p[2] >= 'a' ? p[2] - 'a' + 10 : p[2] >= 'A' ? p[2] - 'A' + 10 : p[2] - '0';
                            decoded[used++] = (char)(hi * 16 + lo);
                            p += 2;
                        } else {
                            decoded[used++] = *p;
                        }
                    }
                    decoded[used] = '\0';
                    mx_js_run(decoded, used, "javascript: URL");
                }
                return 0;
            }
            if (href[0] == '#') {
                /* same-page anchor: scroll to it */
                mx_node_t* anchor = href[1] ? mx_dom_by_id(doc.root, href + 1) : NULL;
                if (!anchor && href[1])
                    for (mx_node_t* a = doc.root; a; a = mx_dom_next(doc.root, a))
                        if (mx_dom_is(a, "a") && mx_dom_attr(a, "name") &&
                            !strcmp(mx_dom_attr(a, "name"), href + 1)) {
                            anchor = a;
                            break;
                        }
                scroll_y = anchor && anchor->layout_gen == layout_generation ? anchor->box_y : 0;
                mx_html_set_scroll(scroll_y, (uint32_t)viewport_h);
                return 2;
            }
            mx_html_resolve(href, navigate, capacity);
            return 1;
        }
        if (mx_dom_is(n, "label")) {
            const char* for_id = mx_dom_attr(n, "for");
            mx_node_t* control = for_id ? mx_dom_by_id(doc.root, for_id) : NULL;
            if (!control)
                for (mx_node_t* c = n; c; c = mx_dom_next(n, c))
                    if (mx_dom_is(c, "input") || mx_dom_is(c, "select") || mx_dom_is(c, "textarea")) {
                        control = c;
                        break;
                    }
            if (control && control != target) return activate(control, navigate, capacity);
            return 0;
        }
        if (mx_dom_is(n, "summary")) {
            mx_node_t* details = n->parent;
            if (mx_dom_is(details, "details")) {
                if (mx_dom_attr(details, "open")) mx_dom_remove_attr(details, "open");
                else mx_dom_set_attr(&doc, details, "open", "");
                doc.mutated = 1;
                if (scripts_enabled) mx_js_dispatch(details, "toggle", NULL);
            }
            return 2;
        }
        if (mx_dom_is(n, "input")) {
            const char* t = input_type(n);
            if (mx_dom_attr(n, "disabled")) return 0;
            if (mx_streq_nocase(t, "checkbox")) {
                n->checked = !n->checked;
                doc.mutated = 1;
                if (scripts_enabled) {
                    mx_js_dispatch(n, "input", NULL);
                    mx_js_dispatch(n, "change", NULL);
                }
                return 2;
            }
            if (mx_streq_nocase(t, "radio")) {
                const char* name = mx_dom_attr(n, "name");
                mx_node_t* scope = closest(n, "form");
                if (!scope) scope = doc.root;
                for (mx_node_t* o = scope; o; o = mx_dom_next(scope, o))
                    if (mx_dom_is(o, "input") && mx_streq_nocase(input_type(o), "radio") &&
                        name && mx_dom_attr(o, "name") && !strcmp(mx_dom_attr(o, "name"), name))
                        o->checked = 0;
                n->checked = 1;
                doc.mutated = 1;
                if (scripts_enabled) mx_js_dispatch(n, "change", NULL);
                return 2;
            }
            if (mx_streq_nocase(t, "submit") || mx_streq_nocase(t, "image"))
                return submit_form(closest(n, "form"), n, navigate, capacity);
            if (mx_streq_nocase(t, "reset")) {
                mx_node_t* form = closest(n, "form");
                for (mx_node_t* o = form; o; o = mx_dom_next(form, o)) {
                    o->value = NULL;
                    o->checked = mx_dom_attr(o, "checked") != NULL;
                }
                doc.mutated = 1;
                return 2;
            }
            if (is_text_input(n)) {
                focused = n;
                doc.mutated = 1;
                if (scripts_enabled) mx_js_dispatch(n, "focus", NULL);
                return 2;
            }
            return 0;
        }
        if (mx_dom_is(n, "textarea")) {
            focused = n;
            doc.mutated = 1;
            return 2;
        }
        if (mx_dom_is(n, "select")) {
            mx_node_t* current = selected_option(n);
            mx_node_t* next = NULL;
            int seen = 0;
            for (mx_node_t* o = n; o; o = mx_dom_next(n, o)) {
                if (!mx_dom_is(o, "option")) continue;
                if (seen) {
                    next = o;
                    break;
                }
                if (o == current) seen = 1;
            }
            if (!next)
                for (mx_node_t* o = n; o; o = mx_dom_next(n, o))
                    if (mx_dom_is(o, "option")) {
                        next = o;
                        break;
                    }
            for (mx_node_t* o = n; o; o = mx_dom_next(n, o))
                if (mx_dom_is(o, "option")) {
                    o->checked = o == next;
                    o->value = (char*)"";   /* marks the selection as user-made */
                    if (o != next) mx_dom_remove_attr(o, "selected");
                }
            if (next) mx_dom_set_attr(&doc, next, "selected", "");
            doc.mutated = 1;
            if (scripts_enabled) mx_js_dispatch(n, "change", NULL);
            return 2;
        }
        if (mx_dom_is(n, "button")) {
            const char* t = mx_dom_attr(n, "type");
            if (mx_dom_attr(n, "disabled")) return 0;
            if (!t || mx_streq_nocase(t, "submit")) {
                mx_node_t* form = closest(n, "form");
                if (form) return submit_form(form, n, navigate, capacity);
            }
            return 0;
        }
    }
    return 0;
}

int mx_html_click(int x, int y, char* navigate, size_t capacity) {
    if (!doc_loaded) return 0;
    mx_node_t* target = mx_html_hit(x, y);
    while (target && target->type != MX_NODE_ELEMENT) target = target->parent;
    mx_node_t* previous_focus = focused;
    focused = NULL;
    if (!target) {
        if (previous_focus) doc.mutated = 1;
        mx_html_update();
        return 0;
    }
    int prevented = 0;
    if (scripts_enabled) {
        mx_js_event_info_t info;
        memset(&info, 0, sizeof(info));
        info.client_x = x;
        info.client_y = y - scroll_y;
        prevented = mx_js_dispatch(target, "click", &info);
    }
    int result = 0;
    if (!prevented && mx_dom_is(target, "video") && target->style) {
        /* the video's controls */
        const mx_style_t* s = target->style;
        int pl = mx_len_resolve(s->padding[3], target->box_w) + s->border_width[3];
        int pt = mx_len_resolve(s->padding[0], target->box_w) + s->border_width[0];
        int pr = mx_len_resolve(s->padding[1], target->box_w) + s->border_width[1];
        int pb = mx_len_resolve(s->padding[2], target->box_w) + s->border_width[2];
        if (mx_media_click(target, x, y, target->box_x + pl, target->box_y + pt,
                           target->box_w - pl - pr, target->box_h - pt - pb))
            prevented = 1;
    }
    if (!prevented) result = activate(target, navigate, capacity);
    if (previous_focus != focused) doc.mutated = 1;
    mx_html_update();
    return result == 1;
}

int mx_html_has_focus(void) {
    return focused != NULL;
}

void mx_html_blur(void) {
    if (focused) {
        focused = NULL;
        doc.mutated = 1;
        mx_html_update();
    }
}

int mx_html_key(int usb_key, char ascii, int mods, char* navigate,
                size_t capacity) {
    if (!doc_loaded) return 0;
    mx_node_t* target = focused ? focused : doc.body;
    int prevented = 0;
    if (scripts_enabled && target) {
        mx_js_event_info_t info;
        memset(&info, 0, sizeof(info));
        info.key_code = usb_key;
        info.shift = (mods & 1) != 0;
        info.ctrl = (mods & 2) != 0;
        info.alt = (mods & 4) != 0;
        static const struct { int usb; const char* name; int code; } named[] = {
            {40, "Enter", 13}, {41, "Escape", 27}, {42, "Backspace", 8}, {43, "Tab", 9},
            {44, " ", 32}, {79, "ArrowRight", 39}, {80, "ArrowLeft", 37},
            {81, "ArrowDown", 40}, {82, "ArrowUp", 38}, {76, "Delete", 46},
            {74, "Home", 36}, {77, "End", 35},
        };
        info.key[0] = '\0';
        for (size_t i = 0; i < sizeof(named) / sizeof(named[0]); i++)
            if (named[i].usb == usb_key) {
                mx_copy(info.key, sizeof(info.key), named[i].name);
                info.key_code = named[i].code;
            }
        if (!info.key[0] && ascii >= 32 && ascii < 127) {
            info.key[0] = ascii;
            info.key[1] = '\0';
            info.key_code = ascii >= 'a' && ascii <= 'z' ? ascii - 32 : ascii;
        }
        prevented = mx_js_dispatch(target, "keydown", &info);
        mx_js_dispatch(target, "keyup", &info);
    }
    if (!focused) {
        int changed = mx_html_update();
        return prevented || changed;
    }
    if (prevented) {
        mx_html_update();
        return 1;
    }
    const char* value = control_value(focused);
    size_t length = strlen(value);
    int textarea = mx_dom_is(focused, "textarea");
    if (usb_key == 40) {   /* Enter */
        if (textarea) {
            char* next = (char*)mx_arena_alloc(&doc.arena, length + 2);
            if (next) {
                memcpy(next, value, length);
                next[length] = '\n';
                next[length + 1] = '\0';
                focused->value = next;
                doc.mutated = 1;
            }
            mx_html_update();
            return 1;
        }
        mx_node_t* form = closest(focused, "form");
        int submitted = form ? submit_form(form, NULL, navigate, capacity) : 0;
        if (scripts_enabled) mx_js_dispatch(focused, "change", NULL);
        mx_html_update();
        return submitted ? 2 : 1;
    }
    if (usb_key == 41) {   /* Escape */
        mx_html_blur();
        return 1;
    }
    if (usb_key == 42) {   /* Backspace */
        if (length) {
            size_t cut = length - 1;
            while (cut > 0 && ((unsigned char)value[cut] & 0xC0) == 0x80) cut--;
            char* next = mx_arena_strndup(&doc.arena, value, cut);
            if (next) set_value(focused, next);
        }
    } else if (ascii >= 32 && ascii < 127) {
        const char* max_attr = mx_dom_attr(focused, "maxlength");
        int max = max_attr ? atoi(max_attr) : 0;
        if (!max || (int)length < max) {
            char* next = (char*)mx_arena_alloc(&doc.arena, length + 2);
            if (next) {
                memcpy(next, value, length);
                next[length] = ascii;
                next[length + 1] = '\0';
                focused->value = next;
                doc.mutated = 1;
            }
        }
    } else {
        mx_html_update();
        return 1;
    }
    if (scripts_enabled) mx_js_dispatch(focused, "input", NULL);
    mx_html_update();
    return 1;
}
