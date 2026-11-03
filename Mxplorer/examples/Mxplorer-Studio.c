/*
 * Mxplorer single-file Run Studio / .run preview build.
 * Generated from the modular sources; keep this snapshot synchronized.
 */
#include "stdlib.h"
#include "stdio.h"
#include "string.h"
#include "window.h"
#include "font.h"
#include "net.h"
#include "x86_64/user_usb.h"

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

#ifndef MXPLORER_CSS_H
#define MXPLORER_CSS_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t color;
    uint32_t background;
    int font_size;
    int bold;
    int text_align;
    int has_font_size;
    int has_font_weight;
    int margin_top;
    int margin_bottom;
    int padding;
    int display_none;
    int has_background;
} mx_css_style_t;

void mx_css_reset(void);
void mx_css_add_stylesheet(const char* source, size_t length);
void mx_css_compute(mx_css_style_t* result,
                    const mx_css_style_t* inherited,
                    const char* tag, const char* class_name,
                    const char* id, const char* inline_style);

#endif

#ifndef MXPLORER_HTML_H
#define MXPLORER_HTML_H

#include <stddef.h>
#include <stdint.h>

#define MX_HTML_SOURCE_MAX (96u * 1024u)
#define MX_HTML_VIEW_TOP 106u

typedef enum {
    MX_HTML_OK = 0,
    MX_HTML_EMPTY,
    MX_HTML_TOO_LARGE,
    MX_HTML_TOO_COMPLEX
} mx_html_result_t;

mx_html_result_t mx_html_parse(const char* source, size_t length,
                               uint32_t viewport_width);
void mx_html_draw(uint32_t* pixels, uint32_t surface_width,
                  uint32_t surface_height, int scroll_y);
void mx_html_scroll(int delta, uint32_t viewport_height);
int mx_html_scroll_y(void);
int mx_html_content_height(void);
const char* mx_html_error(mx_html_result_t result);

#endif

#ifndef MXPLORER_HTTP_H
#define MXPLORER_HTTP_H

#include <stddef.h>

#define MX_HTTP_BODY_MAX (96u * 1024u)

int mx_http_get(const char* url, char* body, size_t body_capacity,
                size_t* body_length, int allow_insecure_tls,
                char* error, size_t error_capacity);

#endif

#ifndef MXPLORER_UI_H
#define MXPLORER_UI_H


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

/* ---- css.c ---- */
#include "string.h"

#define MX_CSS_RULE_MAX 64
#define MX_CSS_SELECTOR_MAX 48

typedef struct {
    char selector[MX_CSS_SELECTOR_MAX];
    mx_css_style_t style;
    uint16_t properties;
} mx_css_rule_t;

enum {
    CSS_COLOR = 1u << 0,
    CSS_BACKGROUND = 1u << 1,
    CSS_FONT_SIZE = 1u << 2,
    CSS_FONT_WEIGHT = 1u << 3,
    CSS_TEXT_ALIGN = 1u << 4,
    CSS_MARGIN_TOP = 1u << 5,
    CSS_MARGIN_BOTTOM = 1u << 6,
    CSS_PADDING = 1u << 7,
    CSS_DISPLAY = 1u << 8
};

static mx_css_rule_t rules[MX_CSS_RULE_MAX];
static unsigned rule_count;

static int css_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static char css_lower(char c) {
    return c >= 'A' && c <= 'Z' ? (char)(c + ('a' - 'A')) : c;
}

static int css_equal(const char* a, const char* b) {
    while (*a && *b && css_lower(*a) == css_lower(*b)) {
        a++;
        b++;
    }
    return !*a && !*b;
}

static uint32_t css_named_color(const char* value) {
    if (css_equal(value, "black")) return 0xFF000000u;
    if (css_equal(value, "white")) return 0xFFFFFFFFu;
    if (css_equal(value, "red")) return 0xFFCC3333u;
    if (css_equal(value, "green")) return 0xFF228833u;
    if (css_equal(value, "blue")) return 0xFF3366CCu;
    if (css_equal(value, "gray") || css_equal(value, "grey"))
        return 0xFF808080u;
    if (css_equal(value, "yellow")) return 0xFFFFFF00u;
    if (css_equal(value, "transparent")) return 0x00000000u;
    return 0;
}

static int css_color(const char* value, uint32_t* color) {
    if (*value == '#') {
        unsigned channels[3] = {0, 0, 0};
        value++;
        if (strlen(value) == 3) {
            for (int i = 0; i < 3; i++) {
                char c = value[i];
                unsigned digit = c >= '0' && c <= '9' ? (unsigned)(c - '0') :
                                 c >= 'a' && c <= 'f' ? (unsigned)(c - 'a' + 10) :
                                 c >= 'A' && c <= 'F' ? (unsigned)(c - 'A' + 10) : 99u;
                if (digit > 15u) return 0;
                channels[i] = digit * 17u;
            }
            *color = 0xFF000000u | (channels[0] << 16) |
                     (channels[1] << 8) | channels[2];
            return 1;
        }
        if (strlen(value) == 6) {
            for (int i = 0; i < 3; i++) {
                unsigned parsed = 0;
                for (int j = 0; j < 2; j++) {
                    char c = value[i * 2 + j];
                    unsigned digit = c >= '0' && c <= '9' ? (unsigned)(c - '0') :
                                     c >= 'a' && c <= 'f' ? (unsigned)(c - 'a' + 10) :
                                     c >= 'A' && c <= 'F' ? (unsigned)(c - 'A' + 10) : 99u;
                    if (digit > 15u) return 0;
                    parsed = (parsed << 4) | digit;
                }
                channels[i] = parsed;
            }
            *color = 0xFF000000u | (channels[0] << 16) |
                     (channels[1] << 8) | channels[2];
            return 1;
        }
        return 0;
    }
    *color = css_named_color(value);
    return *color != 0 || css_equal(value, "transparent");
}

static int css_number(const char* value, int fallback) {
    int number = 0;
    int digits = 0;
    while (*value >= '0' && *value <= '9') {
        digits = 1;
        number = number * 10 + (*value++ - '0');
        if (number > 256) return 256;
    }
    return digits ? number : fallback;
}

static void css_declarations(mx_css_rule_t* rule,
                             const char* source, size_t length) {
    size_t pos = 0;
    while (pos < length) {
        while (pos < length && (css_space(source[pos]) || source[pos] == ';'))
            pos++;
        size_t property_start = pos;
        while (pos < length && source[pos] != ':' && source[pos] != ';')
            pos++;
        if (pos >= length || source[pos] != ':') {
            while (pos < length && source[pos] != ';') pos++;
            continue;
        }
        size_t property_end = pos++;
        size_t value_start = pos;
        while (pos < length && source[pos] != ';') pos++;
        size_t value_end = pos;
        while (property_start < property_end &&
               css_space(source[property_start])) property_start++;
        while (property_end > property_start &&
               css_space(source[property_end - 1])) property_end--;
        while (value_start < value_end && css_space(source[value_start]))
            value_start++;
        while (value_end > value_start && css_space(source[value_end - 1]))
            value_end--;

        char property[32];
        char value[48];
        size_t pn = property_end - property_start;
        size_t vn = value_end - value_start;
        if (pn >= sizeof(property)) pn = sizeof(property) - 1;
        if (vn >= sizeof(value)) vn = sizeof(value) - 1;
        for (size_t i = 0; i < pn; i++)
            property[i] = css_lower(source[property_start + i]);
        property[pn] = '\0';
        for (size_t i = 0; i < vn; i++)
            value[i] = css_lower(source[value_start + i]);
        value[vn] = '\0';

        uint32_t color;
        if (css_equal(property, "color") && css_color(value, &color)) {
            rule->style.color = color;
            rule->properties |= CSS_COLOR;
        } else if ((css_equal(property, "background") ||
                    css_equal(property, "background-color")) &&
                   css_color(value, &color)) {
            rule->style.background = color;
            rule->style.has_background = 1;
            rule->properties |= CSS_BACKGROUND;
        } else if (css_equal(property, "font-size")) {
            int px = css_number(value, 16);
            rule->style.font_size = px <= 12 ? 1 : px <= 18 ? 2 :
                                    px <= 26 ? 3 : px <= 34 ? 4 : 5;
            rule->style.has_font_size = 1;
            rule->properties |= CSS_FONT_SIZE;
        } else if (css_equal(property, "font-weight")) {
            rule->style.bold = css_equal(value, "bold") ||
                               css_number(value, 400) >= 600;
            rule->style.has_font_weight = 1;
            rule->properties |= CSS_FONT_WEIGHT;
        } else if (css_equal(property, "text-align")) {
            rule->style.text_align = css_equal(value, "center") ? 1 :
                                     css_equal(value, "right") ? 2 : 0;
            rule->properties |= CSS_TEXT_ALIGN;
        } else if (css_equal(property, "margin") ||
                   css_equal(property, "margin-top")) {
            rule->style.margin_top = css_number(value, 0);
            rule->properties |= CSS_MARGIN_TOP;
        } else if (css_equal(property, "margin-bottom")) {
            rule->style.margin_bottom = css_number(value, 0);
            rule->properties |= CSS_MARGIN_BOTTOM;
        } else if (css_equal(property, "padding")) {
            rule->style.padding = css_number(value, 0);
            rule->properties |= CSS_PADDING;
        } else if (css_equal(property, "display")) {
            rule->style.display_none = css_equal(value, "none");
            rule->properties |= CSS_DISPLAY;
        }
        if (pos < length) pos++;
    }
}

static void css_parse_rule(const char* selector, size_t selector_length,
                           const char* declarations, size_t declaration_length) {
    while (selector_length && css_space(*selector)) {
        selector++;
        selector_length--;
    }
    while (selector_length && css_space(selector[selector_length - 1]))
        selector_length--;
    if (!selector_length || selector_length >= MX_CSS_SELECTOR_MAX ||
        rule_count >= MX_CSS_RULE_MAX)
        return;
    mx_css_rule_t* rule = &rules[rule_count];
    memset(rule, 0, sizeof(*rule));
    for (size_t i = 0; i < selector_length; i++)
        rule->selector[i] = css_lower(selector[i]);
    rule->selector[selector_length] = '\0';
    css_declarations(rule, declarations, declaration_length);
    rule_count++;
}

void mx_css_reset(void) {
    rule_count = 0;
}

void mx_css_add_stylesheet(const char* source, size_t length) {
    size_t pos = 0;
    while (pos < length && rule_count < MX_CSS_RULE_MAX) {
        while (pos < length && (css_space(source[pos]) || source[pos] == '}'))
            pos++;
        if (pos + 1 < length && source[pos] == '/' && source[pos + 1] == '*') {
            pos += 2;
            while (pos + 1 < length &&
                   !(source[pos] == '*' && source[pos + 1] == '/'))
                pos++;
            pos = pos + 1 < length ? pos + 2 : length;
            continue;
        }
        size_t selector_start = pos;
        while (pos < length && source[pos] != '{') pos++;
        if (pos >= length) break;
        size_t selector_end = pos++;
        size_t declaration_start = pos;
        while (pos < length && source[pos] != '}') pos++;
        size_t declaration_end = pos;
        if (pos < length) pos++;

        size_t start = selector_start;
        for (size_t i = selector_start; i <= selector_end; i++) {
            if (i == selector_end || source[i] == ',') {
                css_parse_rule(source + start, i - start,
                               source + declaration_start,
                               declaration_end - declaration_start);
                start = i + 1;
            }
        }
    }
}

static int css_selector_matches(const char* selector, const char* tag,
                                const char* class_name, const char* id) {
    if (*selector == '#') return id && css_equal(selector + 1, id);
    if (*selector == '.') {
        const char* wanted = selector + 1;
        size_t length = strlen(wanted);
        const char* current = class_name;
        while (current && *current) {
            while (css_space(*current)) current++;
            const char* end = current;
            while (*end && !css_space(*end)) end++;
            if ((size_t)(end - current) == length &&
                strncmp(current, wanted, length) == 0)
                return 1;
            current = *end ? end + 1 : NULL;
        }
        return 0;
    }
    if (strchr(selector, ' ') || strchr(selector, '>') ||
        strchr(selector, ':') || strchr(selector, '['))
        return 0;
    return css_equal(selector, "*") || css_equal(selector, tag);
}

static int css_specificity(const char* selector) {
    if (*selector == '#') return 2;
    if (*selector == '.') return 1;
    return 0;
}

static void css_apply(mx_css_style_t* style, const mx_css_rule_t* rule) {
    uint16_t p = rule->properties;
    if (p & CSS_COLOR) style->color = rule->style.color;
    if (p & CSS_BACKGROUND) {
        style->background = rule->style.background;
        style->has_background = (rule->style.background >> 24) != 0;
    }
    if (p & CSS_FONT_SIZE) style->font_size = rule->style.font_size;
    if (p & CSS_FONT_SIZE) style->has_font_size = 1;
    if (p & CSS_FONT_WEIGHT) {
        style->bold = rule->style.bold;
        style->has_font_weight = 1;
    }
    if (p & CSS_TEXT_ALIGN) style->text_align = rule->style.text_align;
    if (p & CSS_MARGIN_TOP) style->margin_top = rule->style.margin_top;
    if (p & CSS_MARGIN_BOTTOM) style->margin_bottom = rule->style.margin_bottom;
    if (p & CSS_PADDING) style->padding = rule->style.padding;
    if (p & CSS_DISPLAY) style->display_none = rule->style.display_none;
}

void mx_css_compute(mx_css_style_t* result,
                    const mx_css_style_t* inherited,
                    const char* tag, const char* class_name,
                    const char* id, const char* inline_style) {
    *result = *inherited;
    result->has_background = 0;
    result->background = 0xFFFFFFFFu;
    result->margin_top = 0;
    result->margin_bottom = 0;
    result->padding = 0;
    result->display_none = 0;
    for (int specificity = 0; specificity <= 2; specificity++) {
        for (unsigned i = 0; i < rule_count; i++) {
            if (css_specificity(rules[i].selector) == specificity &&
                css_selector_matches(rules[i].selector, tag, class_name, id))
                css_apply(result, &rules[i]);
        }
    }
    if (inline_style) {
        mx_css_rule_t inline_rule = {0};
        css_declarations(&inline_rule, inline_style, strlen(inline_style));
        css_apply(result, &inline_rule);
    }
}

/* ---- html.c ---- */
#include "font.h"
#include "string.h"

#define MX_HTML_ITEM_MAX 1024
#define MX_HTML_TEXT_MAX 192
#define MX_HTML_STACK_MAX 32

typedef enum {
    HTML_ITEM_TEXT,
    HTML_ITEM_BACKGROUND,
    HTML_ITEM_RULE
} html_item_kind_t;

typedef struct {
    int x;
    int y;
    int width;
    int height;
    uint32_t color;
    uint32_t background;
    uint8_t kind;
    uint8_t font_size;
    uint8_t align;
    uint8_t bold;
    char text[MX_HTML_TEXT_MAX];
} html_item_t;

typedef struct {
    html_item_t items[MX_HTML_ITEM_MAX];
    unsigned item_count;
    int viewport_width;
    int content_height;
    int scroll_y;
    uint32_t background;
    int overflow;
} html_document_t;

typedef struct {
    char tag[24];
    mx_css_style_t style;
    int hidden;
    int background_item;
    int block_start_y;
} html_stack_entry_t;

static html_document_t document;
static const char* parser_source;
static size_t parser_length;
static size_t parser_pos;
static int layout_x;
static int layout_y;
static int layout_left;
static int layout_right;
static int pending_space;
static int in_head;
static int skip_script;
static int stack_depth;
static html_stack_entry_t style_stack[MX_HTML_STACK_MAX];
static char line_text[MX_HTML_TEXT_MAX];
static int line_width;
static int line_height;
static mx_css_style_t line_style;
static int line_has_style;

static char html_lower(char c) {
    return c >= 'A' && c <= 'Z' ? (char)(c + ('a' - 'A')) : c;
}

static int html_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f';
}

static int html_equal(const char* a, const char* b) {
    while (*a && *b && html_lower(*a) == html_lower(*b)) {
        a++;
        b++;
    }
    return !*a && !*b;
}

static void html_copy(char* out, size_t capacity, const char* source,
                      size_t length) {
    if (!capacity) return;
    if (length >= capacity) length = capacity - 1;
    for (size_t i = 0; i < length; i++) out[i] = source[i];
    out[length] = '\0';
}

static int html_is_block(const char* tag) {
    static const char* const blocks[] = {
        "address", "article", "aside", "blockquote", "body", "dd", "div",
        "dl", "dt", "fieldset", "figcaption", "figure", "footer", "form",
        "h1", "h2", "h3", "h4", "h5", "h6", "header", "hr", "li", "main",
        "nav", "ol", "p", "pre", "section", "table", "ul"
    };
    for (size_t i = 0; i < sizeof(blocks) / sizeof(blocks[0]); i++)
        if (html_equal(tag, blocks[i])) return 1;
    return 0;
}

static int html_is_void(const char* tag) {
    static const char* const voids[] = {
        "area", "base", "br", "col", "embed", "hr", "img", "input",
        "link", "meta", "param", "source", "track", "wbr"
    };
    for (size_t i = 0; i < sizeof(voids) / sizeof(voids[0]); i++)
        if (html_equal(tag, voids[i])) return 1;
    return 0;
}

static int html_line_step(int size) {
    return size == 1 ? 15 : size * 10 + 3;
}

static void html_add_item(uint8_t kind, int x, int y, int width, int height,
                          uint32_t color, uint32_t background, int font_size,
                          int align, int bold, const char* text) {
    if (document.item_count >= MX_HTML_ITEM_MAX) {
        document.overflow = 1;
        return;
    }
    html_item_t* item = &document.items[document.item_count++];
    item->kind = kind;
    item->x = x;
    item->y = y;
    item->width = width;
    item->height = height;
    item->color = color;
    item->background = background;
    item->font_size = (uint8_t)font_size;
    item->align = (uint8_t)align;
    item->bold = (uint8_t)bold;
    html_copy(item->text, sizeof(item->text), text ? text : "",
              text ? strlen(text) : 0);
}

static void html_flush_line(void) {
    if (!line_text[0] || !line_has_style) {
        line_text[0] = '\0';
        line_width = 0;
        line_has_style = 0;
        return;
    }
    int x = layout_x;
    if (line_style.text_align == 1 && layout_x == layout_left)
        x += (layout_right - layout_left - line_width) / 2;
    else if (line_style.text_align == 2 && layout_x == layout_left)
        x += layout_right - layout_left - line_width;
    if (line_style.has_background)
        html_add_item(HTML_ITEM_BACKGROUND, x, layout_y, line_width,
                      line_height, 0, line_style.background, 1, 0, 0, NULL);
    html_add_item(HTML_ITEM_TEXT, x, layout_y, line_width, line_height,
                  line_style.color, 0, line_style.font_size,
                  line_style.text_align, line_style.bold, line_text);
    layout_x += line_width;
    line_text[0] = '\0';
    line_width = 0;
    line_has_style = 0;
    if (layout_x > layout_right) layout_x = layout_right;
}

static void html_new_line(void) {
    html_flush_line();
    layout_y += line_height > 0 ? line_height : 16;
    layout_x = layout_left;
    line_height = 0;
    pending_space = 0;
}

static void html_emit_word(const char* word, size_t length,
                           const mx_css_style_t* style) {
    if (!length || document.overflow) return;
    char part[MX_HTML_TEXT_MAX];
    size_t offset = 0;
    int font_size = style->font_size < 1 ? 1 : style->font_size;
    int height = html_line_step(font_size);
    while (offset < length) {
        size_t count = length - offset;
        if (count >= sizeof(part)) count = sizeof(part) - 1;
        html_copy(part, sizeof(part), word + offset, count);
        int width = text_width(part, font_size);
        int space_width = pending_space && layout_x > layout_left
                              ? text_width(" ", font_size) : 0;
        int current_x = layout_x + (line_has_style ? line_width : 0);
        if (current_x > layout_left &&
            current_x + space_width + width > layout_right) {
            html_new_line();
            space_width = 0;
        }
        if (pending_space && line_has_style && layout_x == layout_left &&
            line_text[0])
            space_width = text_width(" ", font_size);
        if (line_has_style &&
            (line_style.color != style->color ||
             line_style.background != style->background ||
             line_style.has_background != style->has_background ||
             line_style.font_size != style->font_size ||
             line_style.text_align != style->text_align ||
             line_style.bold != style->bold))
            html_flush_line();
        if (space_width && line_has_style &&
            strlen(line_text) + 2 < sizeof(line_text)) {
            size_t line_length = strlen(line_text);
            line_text[line_length] = ' ';
            line_text[line_length + 1] = '\0';
            line_width += space_width;
        } else if (space_width && !line_has_style) {
            layout_x += space_width;
        }
        size_t current = strlen(line_text);
        if (current + count >= sizeof(line_text)) {
            html_flush_line();
            current = 0;
        }
        if (!line_has_style) {
            line_style = *style;
            line_has_style = 1;
        }
        for (size_t i = 0; i < count; i++) line_text[current + i] = part[i];
        line_text[current + count] = '\0';
        line_width += width;
        if (height > line_height) line_height = height;
        pending_space = 0;
        offset += count;
        if (offset < length) html_new_line();
    }
}

static void html_text(const char* text, size_t length,
                      const mx_css_style_t* style) {
    if (document.overflow || (stack_depth && style_stack[stack_depth - 1].hidden))
        return;
    char word[MX_HTML_TEXT_MAX];
    size_t word_length = 0;
    for (size_t i = 0; i <= length; i++) {
        char c = i < length ? text[i] : ' ';
        if (html_space(c)) {
            if (word_length) {
                html_emit_word(word, word_length, style);
                word_length = 0;
            }
            if (line_has_style || layout_x > layout_left) pending_space = 1;
        } else {
            word[word_length++] = c;
            if (word_length + 1 >= sizeof(word)) {
                html_emit_word(word, word_length, style);
                word_length = 0;
            }
        }
    }
}

static void html_entity(char* out, size_t* used, size_t capacity,
                        const char* name, size_t length) {
    char value = 0;
    if (length == 3 && !strncmp(name, "amp", 3)) value = '&';
    else if (length == 2 && !strncmp(name, "lt", 2)) value = '<';
    else if (length == 2 && !strncmp(name, "gt", 2)) value = '>';
    else if (length == 4 && !strncmp(name, "quot", 4)) value = '"';
    else if (length == 4 && !strncmp(name, "apos", 4)) value = '\'';
    else if (length == 4 && !strncmp(name, "nbsp", 4)) value = ' ';
    else if (length > 1 && name[0] == '#') {
        unsigned code = 0;
        size_t pos = 1;
        int base = 10;
        if (pos < length && (name[pos] == 'x' || name[pos] == 'X')) {
            base = 16;
            pos++;
        }
        for (; pos < length; pos++) {
            char c = name[pos];
            unsigned digit = c >= '0' && c <= '9' ? (unsigned)(c - '0') :
                             c >= 'a' && c <= 'f' ? (unsigned)(c - 'a' + 10) :
                             c >= 'A' && c <= 'F' ? (unsigned)(c - 'A' + 10) : 99u;
            if (digit >= (unsigned)base) return;
            code = code * (unsigned)base + digit;
        }
        if (code >= 32 && code < 127) value = (char)code;
    }
    if (value && *used + 1 < capacity) out[(*used)++] = value;
}

static size_t html_decode_text(char* out, size_t capacity,
                               const char* source, size_t length) {
    size_t used = 0;
    for (size_t i = 0; i < length && used + 1 < capacity; i++) {
        if (source[i] == '&') {
            size_t end = i + 1;
            while (end < length && end - i <= 12 && source[end] != ';' &&
                   !html_space(source[end]))
                end++;
            if (end < length && source[end] == ';') {
                size_t before = used;
                html_entity(out, &used, capacity, source + i + 1, end - i - 1);
                if (used != before) {
                    i = end;
                    continue;
                }
            }
        }
        out[used++] = source[i];
    }
    out[used] = '\0';
    return used;
}

static int html_attr_value(const char* tag, size_t tag_length,
                           const char* wanted, char* output, size_t capacity) {
    size_t pos = 0;
    while (pos < tag_length && !html_space(tag[pos])) pos++;
    while (pos < tag_length) {
        while (pos < tag_length &&
               (html_space(tag[pos]) || tag[pos] == '/' || tag[pos] == '>'))
            pos++;
        size_t key_start = pos;
        while (pos < tag_length && !html_space(tag[pos]) &&
               tag[pos] != '=' && tag[pos] != '>' && tag[pos] != '/')
            pos++;
        if (pos == key_start) break;
        char key[24];
        html_copy(key, sizeof(key), tag + key_start, pos - key_start);
        while (pos < tag_length && html_space(tag[pos])) pos++;
        if (pos >= tag_length || tag[pos] != '=') continue;
        pos++;
        while (pos < tag_length && html_space(tag[pos])) pos++;
        char quote = 0;
        if (pos < tag_length && (tag[pos] == '"' || tag[pos] == '\'')) {
            quote = tag[pos++];
        }
        size_t value_start = pos;
        if (quote) {
            while (pos < tag_length && tag[pos] != quote) pos++;
        } else {
            while (pos < tag_length && !html_space(tag[pos]) && tag[pos] != '>')
                pos++;
        }
        if (html_equal(key, wanted)) {
            html_copy(output, capacity, tag + value_start, pos - value_start);
            return 1;
        }
        if (quote && pos < tag_length) pos++;
    }
    if (capacity) output[0] = '\0';
    return 0;
}

static void html_parse_tag_name(const char* source, size_t length,
                                char* name, size_t name_capacity) {
    size_t pos = 0;
    while (pos < length && (html_space(source[pos]) || source[pos] == '/'))
        pos++;
    size_t start = pos;
    while (pos < length && !html_space(source[pos]) && source[pos] != '/' &&
           source[pos] != '>')
        pos++;
    html_copy(name, name_capacity, source + start, pos - start);
    for (size_t i = 0; name[i]; i++) name[i] = html_lower(name[i]);
}

static void html_add_css_from_source(const char* source, size_t length) {
    size_t pos = 0;
    while (pos + 6 < length) {
        if (source[pos] != '<' || html_lower(source[pos + 1]) != 's' ||
            html_lower(source[pos + 2]) != 't' ||
            html_lower(source[pos + 3]) != 'y' ||
            html_lower(source[pos + 4]) != 'l' ||
            html_lower(source[pos + 5]) != 'e') {
            pos++;
            continue;
        }
        size_t start = pos + 6;
        while (start < length && source[start] != '>') start++;
        if (start >= length) break;
        start++;
        size_t end = start;
        while (end + 7 < length && !(source[end] == '<' &&
               source[end + 1] == '/' &&
               html_lower(source[end + 2]) == 's' &&
               html_lower(source[end + 3]) == 't' &&
               html_lower(source[end + 4]) == 'y' &&
               html_lower(source[end + 5]) == 'l' &&
               html_lower(source[end + 6]) == 'e'))
            end++;
        mx_css_add_stylesheet(source + start, end - start);
        pos = end + 7;
    }
}

static int html_tag_has_class(const char* tag, size_t length,
                              const char* wanted) {
    char classes[96];
    if (!html_attr_value(tag, length, "class", classes, sizeof(classes)))
        return 0;
    const char* pos = classes;
    size_t wanted_length = strlen(wanted);
    while (*pos) {
        while (html_space(*pos)) pos++;
        const char* end = pos;
        while (*end && !html_space(*end)) end++;
        if ((size_t)(end - pos) == wanted_length &&
            !strncmp(pos, wanted, wanted_length))
            return 1;
        pos = end;
    }
    return 0;
}

static int html_start_block(const char* tag, const mx_css_style_t* style) {
    html_flush_line();
    if (layout_x != layout_left) html_new_line();
    pending_space = 0;
    int margin = style->margin_top;
    if (!margin) {
        if (tag[0] == 'h') margin = 14;
        else if (html_equal(tag, "p") || html_equal(tag, "blockquote") ||
                 html_equal(tag, "li")) margin = 8;
        else margin = 4;
    }
    layout_y += margin;
    layout_left = 20 + style->padding;
    layout_right = document.viewport_width - 20 - style->padding;
    if (layout_right <= layout_left) layout_right = layout_left + 1;
    layout_x = layout_left;
    if (style->has_background) {
        int index = (int)document.item_count;
        html_add_item(HTML_ITEM_BACKGROUND, 12, layout_y - margin,
                      (int)document.viewport_width - 24, 1,
                      0, style->background, 1, 0, 0, NULL);
        return index;
    }
    return -1;
}

static void html_end_block(const mx_css_style_t* style) {
    html_flush_line();
    if (layout_x != layout_left) html_new_line();
    layout_y += style->margin_bottom ? style->margin_bottom : 8;
    layout_left = 20;
    layout_right = (int)document.viewport_width - 20;
    layout_x = layout_left;
    pending_space = 0;
}

static void html_handle_open_tag(const char* raw_tag, size_t raw_length,
                                 const char* tag, int self_closing) {
    if (html_equal(tag, "head")) {
        in_head = 1;
        return;
    }
    if ((in_head && !html_equal(tag, "body")) ||
        html_equal(tag, "script") || html_equal(tag, "style") ||
        html_equal(tag, "title") || html_equal(tag, "meta") ||
        html_equal(tag, "link") || html_equal(tag, "base"))
        return;

    char class_name[96], id[64], inline_style[256], alt[96];
    html_attr_value(raw_tag, raw_length, "class", class_name, sizeof(class_name));
    html_attr_value(raw_tag, raw_length, "id", id, sizeof(id));
    html_attr_value(raw_tag, raw_length, "style", inline_style, sizeof(inline_style));
    html_attr_value(raw_tag, raw_length, "alt", alt, sizeof(alt));

    mx_css_style_t inherited = {
        .color = 0xFF273640u, .font_size = 1
    };
    int parent_hidden = 0;
    if (stack_depth) {
        inherited = style_stack[stack_depth - 1].style;
        parent_hidden = style_stack[stack_depth - 1].hidden;
    }
    mx_css_style_t style;
    mx_css_compute(&style, &inherited, tag, class_name, id, inline_style);
    if (html_equal(tag, "body")) {
        in_head = 0;
        if (style.has_background) document.background = style.background;
        if (stack_depth < MX_HTML_STACK_MAX) {
            html_stack_entry_t* entry = &style_stack[stack_depth++];
            html_copy(entry->tag, sizeof(entry->tag), tag, strlen(tag));
            entry->style = style;
            entry->hidden = style.display_none;
            entry->background_item = -1;
            entry->block_start_y = layout_y;
        }
        return;
    }
    if (!style.has_font_weight &&
        (html_equal(tag, "strong") || html_equal(tag, "b") ||
         (tag[0] == 'h' && tag[1] >= '1' && tag[1] <= '6')))
        style.bold = 1;
    if (!style.has_font_size && tag[0] == 'h' &&
        tag[1] >= '1' && tag[1] <= '6')
        style.font_size = 7 - (tag[1] - '0');
    int hidden = parent_hidden || style.display_none;

    int background_item = -1;
    if (!hidden && html_is_block(tag))
        background_item = html_start_block(tag, &style);
    if (!hidden && html_equal(tag, "br")) html_new_line();
    if (!hidden && html_equal(tag, "hr")) {
        html_flush_line();
        html_add_item(HTML_ITEM_RULE, layout_left, layout_y + 5,
                      layout_right - layout_left, 1, 0xFFBCC6CDu, 0, 1, 0, 0,
                      NULL);
        layout_y += 16;
    }
    if (!hidden && html_equal(tag, "li")) {
        const char* bullet = html_tag_has_class(raw_tag, raw_length, "ordered")
                                 ? "1." : "-";
        html_emit_word(bullet, strlen(bullet), &style);
    }
    if (!hidden && html_equal(tag, "img")) {
        char image_label[128];
        if (alt[0]) {
            html_copy(image_label, sizeof(image_label), "[image: ", 8);
            size_t used = strlen(image_label);
            html_copy(image_label + used, sizeof(image_label) - used, alt,
                      strlen(alt));
            used = strlen(image_label);
            if (used + 1 < sizeof(image_label)) {
                image_label[used] = ']';
                image_label[used + 1] = '\0';
            }
        } else {
            html_copy(image_label, sizeof(image_label), "[image]", 7);
        }
        html_text(image_label, strlen(image_label), &style);
    }

    if (!self_closing && !html_is_void(tag) && stack_depth < MX_HTML_STACK_MAX) {
        html_stack_entry_t* entry = &style_stack[stack_depth++];
        html_copy(entry->tag, sizeof(entry->tag), tag, strlen(tag));
        entry->style = style;
        entry->hidden = hidden;
        entry->background_item = background_item;
        entry->block_start_y = layout_y;
    }
}

static void html_handle_close_tag(const char* tag) {
    if (html_equal(tag, "head")) {
        in_head = 0;
        return;
    }
    if (html_equal(tag, "script") || html_equal(tag, "style")) {
        skip_script = 0;
        return;
    }
    if (in_head) return;
    int index = stack_depth - 1;
    while (index >= 0 && !html_equal(style_stack[index].tag, tag)) index--;
    if (index < 0) return;
    for (int i = stack_depth - 1; i >= index; i--) {
        if (!style_stack[i].hidden && html_is_block(style_stack[i].tag)) {
            html_end_block(&style_stack[i].style);
            if (style_stack[i].background_item >= 0 &&
                (unsigned)style_stack[i].background_item < document.item_count) {
                html_item_t* background =
                    &document.items[style_stack[i].background_item];
                background->height = layout_y - background->y;
                if (background->height < 1) background->height = 1;
            }
        }
    }
    stack_depth = index;
}

static void html_parse_document(const char* source, size_t length) {
    parser_source = source;
    parser_length = length;
    parser_pos = 0;
    layout_x = layout_left = 20;
    layout_right = (int)document.viewport_width - 20;
    layout_y = 20;
    pending_space = 0;
    in_head = 0;
    skip_script = 0;
    stack_depth = 0;
    line_text[0] = '\0';
    line_width = line_height = line_has_style = 0;
    mx_css_reset();
    html_add_css_from_source(source, length);

    while (parser_pos < parser_length && !document.overflow) {
        if (parser_source[parser_pos] != '<') {
            size_t end = parser_pos;
            while (end < parser_length && parser_source[end] != '<') end++;
            if (!skip_script && !in_head) {
                char text[2048];
                size_t decoded = html_decode_text(text, sizeof(text),
                    parser_source + parser_pos, end - parser_pos);
                mx_css_style_t style = stack_depth
                    ? style_stack[stack_depth - 1].style
                    : (mx_css_style_t){.color = 0xFF273640u, .font_size = 1};
                html_text(text, decoded, &style);
            }
            parser_pos = end;
            continue;
        }
        if (parser_pos + 3 < parser_length &&
            !strncmp(parser_source + parser_pos, "<!--", 4)) {
            parser_pos += 4;
            while (parser_pos + 2 < parser_length &&
                   strncmp(parser_source + parser_pos, "-->", 3))
                parser_pos++;
            parser_pos = parser_pos + 2 < parser_length ? parser_pos + 3
                                                        : parser_length;
            continue;
        }
        size_t start = ++parser_pos;
        while (parser_pos < parser_length && parser_source[parser_pos] != '>')
            parser_pos++;
        size_t end = parser_pos;
        if (parser_pos < parser_length) parser_pos++;
        if (start >= end) continue;
        int closing = parser_source[start] == '/';
        char tag[24];
        html_parse_tag_name(parser_source + start, end - start, tag, sizeof(tag));
        if (!tag[0]) continue;
        if (html_equal(tag, "script")) {
            skip_script = !closing;
            continue;
        }
        if (skip_script) continue;
        if (closing) {
            html_handle_close_tag(tag);
        } else {
            int self_closing = end > start && parser_source[end - 1] == '/';
            html_handle_open_tag(parser_source + start, end - start, tag,
                                 self_closing);
        }
    }
    html_flush_line();
    document.content_height = layout_y + (line_height ? line_height : 16) + 24;
}

mx_html_result_t mx_html_parse(const char* source, size_t length,
                               uint32_t viewport_width) {
    memset(&document, 0, sizeof(document));
    if (!source || !length) return MX_HTML_EMPTY;
    if (length > MX_HTML_SOURCE_MAX) return MX_HTML_TOO_LARGE;
    document.viewport_width = viewport_width > 80 ? (int)viewport_width : 80;
    document.background = 0xFFFFFFFFu;
    html_parse_document(source, length);
    if (document.overflow) return MX_HTML_TOO_COMPLEX;
    return MX_HTML_OK;
}

static void html_fill_rect(uint32_t* pixels, uint32_t surface_width,
                           uint32_t surface_height, int x, int y, int width,
                           int height, uint32_t color, int clip_x, int clip_y,
                           int clip_width, int clip_height) {
    int x0 = x < clip_x ? clip_x : x;
    int y0 = y < clip_y ? clip_y : y;
    int x1 = x + width > clip_x + clip_width ? clip_x + clip_width : x + width;
    int y1 = y + height > clip_y + clip_height ? clip_y + clip_height : y + height;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int)surface_width) x1 = (int)surface_width;
    if (y1 > (int)surface_height) y1 = (int)surface_height;
    for (int row = y0; row < y1; row++) {
        uint32_t* line = pixels + (size_t)row * surface_width;
        for (int col = x0; col < x1; col++) line[col] = color;
    }
}

void mx_html_draw(uint32_t* pixels, uint32_t surface_width,
                  uint32_t surface_height, int scroll_y) {
    if (!pixels || !surface_width || surface_height <= MX_HTML_VIEW_TOP) return;
    int top = (int)MX_HTML_VIEW_TOP;
    int height = (int)surface_height - top;
    html_fill_rect(pixels, surface_width, surface_height, 0, top,
                   (int)surface_width, height, document.background,
                   0, top, (int)surface_width, height);
    for (unsigned i = 0; i < document.item_count; i++) {
        const html_item_t* item = &document.items[i];
        int y = top + item->y - scroll_y;
        if (y + item->height <= top || y >= (int)surface_height) continue;
        if (item->kind == HTML_ITEM_BACKGROUND) {
            html_fill_rect(pixels, surface_width, surface_height, item->x, y,
                           item->width, item->height, item->background,
                           0, top, (int)surface_width, height);
        } else if (item->kind == HTML_ITEM_RULE) {
            html_fill_rect(pixels, surface_width, surface_height, item->x, y,
                           item->width, item->height, item->color,
                           0, top, (int)surface_width, height);
        } else {
            draw_text(pixels, surface_width, surface_height,
                      item->x, y, item->text, item->color, item->font_size);
            if (item->bold)
                draw_text(pixels, surface_width, surface_height,
                          item->x + 1, y, item->text, item->color,
                          item->font_size);
        }
    }
}

void mx_html_scroll(int delta, uint32_t viewport_height) {
    int maximum = document.content_height > (int)viewport_height
                      ? document.content_height - (int)viewport_height : 0;
    document.scroll_y += delta;
    if (document.scroll_y < 0) document.scroll_y = 0;
    if (document.scroll_y > maximum) document.scroll_y = maximum;
}

int mx_html_scroll_y(void) {
    return document.scroll_y;
}

int mx_html_content_height(void) {
    return document.content_height;
}

const char* mx_html_error(mx_html_result_t result) {
    switch (result) {
        case MX_HTML_EMPTY: return "The response contained no HTML.";
        case MX_HTML_TOO_LARGE: return "HTML document exceeds the 96 KiB limit.";
        case MX_HTML_TOO_COMPLEX: return "HTML document exceeds renderer limits.";
        default: return "HTML parsed successfully.";
    }
}

/* ---- http.c ---- */
#include "net.h"
#include "stdlib.h"
#include "string.h"

#define HTTP_HEADER_MAX 8192u
#define HTTP_RESPONSE_MAX (MX_HTTP_BODY_MAX + HTTP_HEADER_MAX)
#define HTTP_IDLE_TIMEOUT_MS 5000
#define HTTP_TOTAL_TIMEOUT_MS 20000

static char response[HTTP_RESPONSE_MAX];

static void set_error(char* error, size_t capacity, const char* message) {
    if (capacity) strlcpy_safe(error, message, capacity);
}

static char lower_ascii(char c) {
    return c >= 'A' && c <= 'Z' ? (char)(c + ('a' - 'A')) : c;
}

static int equal_nocase(const char* a, const char* b, size_t length) {
    for (size_t i = 0; i < length; i++) {
        if (lower_ascii(a[i]) != lower_ascii(b[i])) return 0;
        if (!a[i] || !b[i]) return a[i] == b[i];
    }
    return 1;
}

static int append_text(char* destination, size_t capacity, size_t* used,
                       const char* text) {
    size_t length = strlen(text);
    if (length >= capacity - *used) return 0;
    memcpy(destination + *used, text, length);
    *used += length;
    destination[*used] = '\0';
    return 1;
}

static int append_port(char* destination, size_t capacity, size_t* used,
                       uint16_t port) {
    char digits[5];
    size_t count = 0;
    do {
        digits[count++] = (char)('0' + port % 10u);
        port = (uint16_t)(port / 10u);
    } while (port && count < sizeof(digits));
    if (count + 1 >= capacity - *used) return 0;
    destination[(*used)++] = ':';
    while (count) destination[(*used)++] = digits[--count];
    destination[*used] = '\0';
    return 1;
}

static int parse_url(const char* url, char* host, size_t host_capacity,
                     char* path, size_t path_capacity, uint16_t* port,
                     int* use_tls, char* error, size_t error_capacity) {
    static const char prefix[] = "http://";
    static const char tls_prefix[] = "https://";
    if (!strncmp(url, prefix, sizeof(prefix) - 1)) {
        *use_tls = 0;
        *port = 80;
        url += sizeof(prefix) - 1;
    } else if (!strncmp(url, tls_prefix, sizeof(tls_prefix) - 1)) {
        *use_tls = 1;
        *port = 443;
        url += sizeof(tls_prefix) - 1;
    } else {
        set_error(error, error_capacity,
                  "Only HTTP and explicitly enabled unsafe HTTPS are supported.");
        return 0;
    }
    const char* authority = url;
    const char* path_start = strchr(authority, '/');
    const char* query_start = strchr(authority, '?');
    const char* authority_end = path_start;
    if (!authority_end || (query_start && query_start < authority_end))
        authority_end = query_start;
    if (!authority_end) authority_end = authority + strlen(authority);
    if (authority_end == authority || strchr(authority, '@') ||
        strchr(authority, '[')) {
        set_error(error, error_capacity, "Unsupported or invalid HTTP address.");
        return 0;
    }

    const char* colon = NULL;
    for (const char* c = authority; c < authority_end; c++)
        if (*c == ':') colon = c;
    const char* host_end = colon ? colon : authority_end;
    size_t host_length = (size_t)(host_end - authority);
    if (!host_length || host_length >= host_capacity) {
        set_error(error, error_capacity, "HTTP host is empty or too long.");
        return 0;
    }
    memcpy(host, authority, host_length);
    host[host_length] = '\0';
    if (colon) {
        unsigned value = 0;
        if (colon + 1 == authority_end) {
            set_error(error, error_capacity, "HTTP port is missing.");
            return 0;
        }
        for (const char* c = colon + 1; c < authority_end; c++) {
            if (*c < '0' || *c > '9') {
                set_error(error, error_capacity, "HTTP port is invalid.");
                return 0;
            }
            value = value * 10u + (unsigned)(*c - '0');
            if (value > 65535u) break;
        }
        if (value == 0 || value > 65535u) {
            set_error(error, error_capacity, "HTTP port is outside 1-65535.");
            return 0;
        }
        *port = (uint16_t)value;
    }

    size_t path_length = strlen(authority_end);
    if (!path_length || *authority_end == '?') {
        if (path_capacity < path_length + 2) {
            set_error(error, error_capacity, "HTTP path is too long.");
            return 0;
        }
        path[0] = '/';
        memcpy(path + 1, authority_end, path_length);
        path[path_length + 1] = '\0';
    } else {
        if (path_length >= path_capacity) {
            set_error(error, error_capacity, "HTTP path is too long.");
            return 0;
        }
        memcpy(path, authority_end, path_length);
        path[path_length] = '\0';
    }
    return 1;
}

static long transport_send(int use_tls, long connection, const void* data,
                           uint32_t length) {
    return use_tls ? mos_tls_send(connection, data, length)
                   : mos_tcp_send(connection, data, length);
}

static long transport_recv(int use_tls, long connection, void* data,
                           uint32_t length) {
    return use_tls ? mos_tls_recv(connection, data, length)
                   : mos_tcp_recv(connection, data, length);
}

static void transport_close(int use_tls, long connection) {
    if (use_tls)
        mos_tls_close(connection);
    else
        mos_tcp_close(connection);
}

static int transport_send_all(int use_tls, long connection, const char* data,
                              size_t length) {
    size_t sent = 0;
    while (sent < length) {
        size_t remaining = length - sent;
        uint32_t chunk = remaining > 4096u ? 4096u : (uint32_t)remaining;
        long count = transport_send(use_tls, connection, data + sent, chunk);
        if (count <= 0 || (size_t)count > remaining) return 0;
        sent += (size_t)count;
    }
    return 1;
}

static char* find_header_end(char* data, size_t length) {
    for (size_t i = 0; i + 3 < length; i++)
        if (data[i] == '\r' && data[i + 1] == '\n' &&
            data[i + 2] == '\r' && data[i + 3] == '\n')
            return data + i;
    return NULL;
}

static int parse_status(const char* response_data) {
    const char* space = strchr(response_data, ' ');
    if (!space || space[1] < '0' || space[1] > '9' ||
        space[2] < '0' || space[2] > '9' ||
        space[3] < '0' || space[3] > '9')
        return 0;
    return (space[1] - '0') * 100 + (space[2] - '0') * 10 +
           space[3] - '0';
}

static void parse_headers(char* headers, size_t length, size_t* content_length,
                          int* has_content_length, int* is_chunked) {
    size_t pos = 0;
    while (pos < length && !(headers[pos] == '\r' && pos + 1 < length &&
                             headers[pos + 1] == '\n'))
        pos++;
    pos += 2;
    while (pos + 1 < length) {
        size_t end = pos;
        while (end + 1 < length &&
               !(headers[end] == '\r' && headers[end + 1] == '\n'))
            end++;
        if (end == pos) break;
        const char* line = headers + pos;
        size_t line_length = end - pos;
        const char* colon = NULL;
        for (size_t i = 0; i < line_length; i++) {
            if (line[i] == ':') {
                colon = line + i;
                break;
            }
        }
        if (colon) {
            size_t key_length = (size_t)(colon - line);
            const char* value = colon + 1;
            size_t value_length = line_length - key_length - 1;
            while (value_length && (*value == ' ' || *value == '\t')) {
                value++;
                value_length--;
            }
            if (key_length == 14 &&
                equal_nocase(line, "content-length", key_length)) {
                size_t length_value = 0;
                int valid = value_length != 0;
                for (size_t i = 0; i < value_length; i++) {
                    if (value[i] < '0' || value[i] > '9') {
                        valid = 0;
                        break;
                    }
                    length_value = length_value * 10u +
                                   (size_t)(value[i] - '0');
                    if (length_value > MX_HTTP_BODY_MAX) {
                        valid = 0;
                        break;
                    }
                }
                if (valid) {
                    *content_length = length_value;
                    *has_content_length = 1;
                }
            } else if (key_length == 17 &&
                       equal_nocase(line, "transfer-encoding", key_length) &&
                       value_length >= 7 &&
                       equal_nocase(value + value_length - 7, "chunked", 7)) {
                *is_chunked = 1;
            }
        }
        pos = end + 2;
    }
}

static int decode_chunked(const char* source, size_t length,
                          char* body, size_t capacity, size_t* out_length) {
    size_t pos = 0;
    size_t written = 0;
    while (pos < length) {
        size_t chunk_length = 0;
        int digits = 0;
        while (pos < length && source[pos] != '\r') {
            char c = lower_ascii(source[pos++]);
            if (c == ';') {
                while (pos < length && source[pos] != '\r') pos++;
                break;
            }
            unsigned digit = c >= '0' && c <= '9' ? (unsigned)(c - '0') :
                             c >= 'a' && c <= 'f' ? (unsigned)(c - 'a' + 10) : 99u;
            if (digit > 15u || chunk_length > (capacity - digit) / 16u)
                return 0;
            chunk_length = chunk_length * 16u + digit;
            digits++;
        }
        if (!digits || pos + 1 >= length || source[pos] != '\r' ||
            source[pos + 1] != '\n')
            return 0;
        pos += 2;
        if (!chunk_length) {
            body[written] = '\0';
            *out_length = written;
            return 1;
        }
        if (chunk_length > capacity - written ||
            chunk_length + 2 > length - pos)
            return 0;
        memcpy(body + written, source + pos, chunk_length);
        written += chunk_length;
        pos += chunk_length;
        if (source[pos] != '\r' || source[pos + 1] != '\n') return 0;
        pos += 2;
    }
    return 0;
}

int mx_http_get(const char* url, char* body, size_t body_capacity,
                size_t* body_length, int allow_insecure_tls,
                char* error, size_t error_capacity) {
    char host[128];
    char path[512];
    char request[1024];
    uint16_t port;
    int use_tls = 0;
    size_t response_length = 0;
    if (body_length) *body_length = 0;
    if (!body || body_capacity < 2 || !body_length || !url) {
        set_error(error, error_capacity, "Invalid HTTP request buffer.");
        return 0;
    }
    if (!parse_url(url, host, sizeof(host), path, sizeof(path), &port, &use_tls,
                   error, error_capacity))
        return 0;
    if (use_tls && !allow_insecure_tls) {
        set_error(error, error_capacity,
                  "Unsafe HTTPS is disabled. Enable it in Settings to continue.");
        return 0;
    }
    if (!mos_net_is_up()) {
        set_error(error, error_capacity,
                  "Network is unavailable; connect a network adapter and run dhcp.");
        return 0;
    }

    uint32_t ip = mos_net_lookup(host, 5000);
    if (!ip) {
        set_error(error, error_capacity, "DNS lookup failed for this host.");
        return 0;
    }
    long connection = use_tls ? mos_tls_connect(ip, port, 5000)
                              : mos_tcp_connect(ip, port, 5000);
    if (connection <= 0) {
        set_error(error, error_capacity,
                  use_tls ? "Could not start the unsafe TLS transport."
                          : "Could not connect to the HTTP server.");
        return 0;
    }
    if (use_tls && !mos_tls_wait_established(connection, 10000)) {
        transport_close(use_tls, connection);
        set_error(error, error_capacity, "Unsafe TLS handshake failed.");
        return 0;
    }

    size_t request_length = 0;
    int request_ok =
        append_text(request, sizeof(request), &request_length, "GET ") &&
        append_text(request, sizeof(request), &request_length, path) &&
        append_text(request, sizeof(request), &request_length, " HTTP/1.0\r\nHost: ") &&
        append_text(request, sizeof(request), &request_length, host) &&
        ((use_tls && port == 443) || (!use_tls && port == 80) ||
         append_port(request, sizeof(request), &request_length, port)) &&
        append_text(request, sizeof(request), &request_length,
            "\r\nUser-Agent: Mxplorer/0.1\r\n"
            "Accept: text/html,text/plain;q=0.8\r\n"
            "Connection: close\r\n\r\n");
    if (!request_ok ||
        !transport_send_all(use_tls, connection, request, request_length)) {
        transport_close(use_tls, connection);
        set_error(error, error_capacity, "Could not send the HTTP request.");
        return 0;
    }

    long start = mos_uptime();
    long last_data = start;
    int receive_failed = 0;
    while (response_length < sizeof(response)) {
        long count = transport_recv(use_tls, connection,
            response + response_length,
            (uint32_t)(sizeof(response) - response_length));
        if (count < 0) {
            if (count == (long)SYS_ERR_NOTFOUND) break;
            receive_failed = 1;
            break;
        }
        if (count == 0) {
            long now = mos_uptime();
            if (now - last_data > HTTP_IDLE_TIMEOUT_MS ||
                now - start > HTTP_TOTAL_TIMEOUT_MS)
                break;
            mos_sleep(5);
            continue;
        }
        response_length += (size_t)count;
        last_data = mos_uptime();
        if (response_length >= HTTP_HEADER_MAX &&
            !find_header_end(response, response_length)) {
            receive_failed = 1;
            break;
        }
        char* header_end = find_header_end(response, response_length);
        if (header_end) {
            size_t header_length = (size_t)(header_end - response);
            size_t content_length = 0;
            int has_content_length = 0;
            int chunked = 0;
            parse_headers(response, header_length, &content_length,
                          &has_content_length, &chunked);
            size_t received_body = response_length - header_length - 4;
            if (has_content_length && !chunked &&
                received_body >= content_length)
                break;
        }
    }
    transport_close(use_tls, connection);
    if (receive_failed) {
        set_error(error, error_capacity, "HTTP response could not be read.");
        return 0;
    }

    char* header_end = find_header_end(response, response_length);
    if (!header_end) {
        set_error(error, error_capacity, "HTTP response headers are incomplete.");
        return 0;
    }
    size_t header_length = (size_t)(header_end - response);
    int status = parse_status(response);
    if (status < 200 || status >= 300) {
        set_error(error, error_capacity,
                  status >= 300 && status < 400
                      ? "Redirects are not supported yet."
                      : "HTTP server returned an error status.");
        return 0;
    }
    size_t declared_length = 0;
    int has_content_length = 0;
    int chunked = 0;
    parse_headers(response, header_length, &declared_length,
                  &has_content_length, &chunked);
    size_t available = response_length - header_length - 4;
    const char* response_body = header_end + 4;
    if (has_content_length && !chunked) {
        if (declared_length > body_capacity - 1) {
            set_error(error, error_capacity, "HTTP page exceeds the 96 KiB limit.");
            return 0;
        }
        if (available < declared_length) {
            set_error(error, error_capacity, "HTTP response ended before the page was complete.");
            return 0;
        }
        available = declared_length;
    }
    if (chunked) {
        if (!decode_chunked(response_body, available, body, body_capacity,
                            body_length)) {
            set_error(error, error_capacity, "Invalid or oversized chunked HTTP response.");
            return 0;
        }
    } else {
        if (available >= body_capacity) {
            set_error(error, error_capacity, "HTTP page exceeds the 96 KiB limit.");
            return 0;
        }
        memcpy(body, response_body, available);
        body[available] = '\0';
        *body_length = available;
    }
    if (error_capacity) error[0] = '\0';
    return 1;
}

/* ---- ui.c ---- */
#include "font.h"
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
    draw_text(state->pixels, state->width, state->height, x, y, text, color, size);
}

static void draw_centered_label(const mx_ui_state_t* state, int center_x, int y,
                                const char* text, uint32_t color, int size) {
    draw_label(state, center_x - text_width(text, size) / 2, y, text, color,
               size);
}

static void draw_truncated(const mx_ui_state_t* state, int x, int y, int max_width,
                           const char* text, uint32_t color) {
    char clipped[80];
    size_t length = 0;
    while (text && text[length] && length + 1 < sizeof(clipped)) {
        clipped[length] = text[length];
        clipped[length + 1] = '\0';
        if (text_width(clipped, 1) > max_width) {
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
    int label_width = text_width(label, 1);
    draw_label(state, x + (width - label_width) / 2, y + (height - 13) / 2,
               label, theme->text, 1);
}

static void ui_ensure_active_visible(const mx_ui_state_t* state) {
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
        int selection_width = text_width(visible_address, 1);
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
        int caret_x = text_x + text_width(caret_prefix, 1);
        if (caret_x <= text_x + text_width_available)
            fill_rect(state, caret_x, MX_BROWSER_TOOLBAR_Y + 12, 1, 18,
                      theme->accent);
    }
    draw_button(state, settings_x, MX_BROWSER_TOOLBAR_Y, 74,
                MX_BROWSER_TOOLBAR_HEIGHT, "Settings");
    fill_rect(state, 0, 100, (int)state->width, 2, theme->border);
}

void mx_ui_draw_chrome(const mx_ui_state_t* state) {
    if (!state || !state->pixels || !state->width || !state->height) return;
    fill_rect(state, 0, 0, (int)state->width, (int)MX_HTML_VIEW_TOP,
              state->theme->background);
    ui_ensure_active_visible(state);
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

/* ---- main.c ---- */
#include "stdlib.h"
#include "stdio.h"
#include "string.h"
#include "window.h"
#include "font.h"
#include "x86_64/user_usb.h"

#define WIN_W 1080u
#define WIN_H 720u
#define MIN_WIN_W 640u
#define MIN_WIN_H 400u
#define MAX_TABS MX_BROWSER_MAX_TABS
#define MAX_HISTORY MX_BROWSER_MAX_HISTORY
#define URL_LENGTH MX_BROWSER_URL_LENGTH
#define TAB_WIDTH MX_BROWSER_TAB_WIDTH
#define TOOLBAR_Y MX_BROWSER_TOOLBAR_Y
#define TOOLBAR_H MX_BROWSER_TOOLBAR_HEIGHT
#define PAGE_Y MX_BROWSER_PAGE_Y
#define HOMEPAGE_GOOGLE 0
#define HOMEPAGE_MXPLORER 1
#define HOMEPAGE_CUSTOM 2
#define HOMEPAGE_SETTING_PATH "0:/data/mxplorer-homepage.txt"
#define HOMEPAGE_ASSET_PATH "0:/programs/Mxplorer/Resources/homepage.html"
#define HOMEPAGE_HTML_CAPACITY 8192u

typedef mx_browser_tab_t browser_tab_t;
typedef mx_browser_theme_t browser_theme_t;

static browser_tab_t tabs[MAX_TABS];
static int tab_count;
static int active_tab;
static int tab_scroll;
static uint32_t window_id;
static uint32_t* pixels;
static uint32_t surface_width;
static uint32_t surface_height;
static int running = 1;
static int address_focused;
static int address_select_all;
static int address_cursor;
static int address_scroll;
static char address_text[URL_LENGTH];
static int settings_category;
static int allow_insecure_tls;
static int homepage_mode = HOMEPAGE_GOOGLE;
static int homepage_previous_mode = HOMEPAGE_GOOGLE;
static char custom_homepage[URL_LENGTH];
static int homepage_editing;
static int previous_buttons;
static int drag_tab = -1;
static browser_theme_t theme;
static char status_message[96] = "Browser framework ready";
static long last_theme_check;
static char page_source[MX_HTTP_BODY_MAX];
static char home_page_html[HOMEPAGE_HTML_CAPACITY];
static size_t home_page_html_length;
static char debug_logs[8][80];
static int debug_log_count;
static int debug_open;
static int debug_tab;
static int page_loaded;
static int chrome_dirty = 1;
static int page_dirty = 1;

static const char home_page_fallback[] =
    "<!doctype html><html><head><style>"
    "body{background-color:#f5f8fc;color:#23364a}"
    ".eyebrow{color:#3478d4;font-size:12px;font-weight:bold;text-align:center;margin-top:24px;margin-bottom:8px}"
    "h1{color:#3478d4;font-size:44px;text-align:center;margin-bottom:8px}"
    ".lead{color:#60758b;font-size:16px;text-align:center;margin-bottom:18px}"
    ".search-prompt{background-color:#ffffff;color:#60758b;text-align:center;padding:14px;margin-bottom:20px}"
    "h2{color:#203d5c;font-size:20px;margin-top:12px;margin-bottom:10px}"
    ".card{background-color:#ffffff;color:#465d74;padding:12px;margin-bottom:8px}"
    ".tag{color:#3478d4;font-weight:bold;margin-bottom:4px}"
    ".note{color:#73869a;font-size:12px;margin-top:14px}"
    "</style></head><body>"
    "<p class='eyebrow'>A SIMPLE BROWSER FOR MINIMAL-OS</p>"
    "<h1>Mxplorer</h1>"
    "<p class='lead'>A little space to find what you need.</p>"
    "<p class='search-prompt'>Search or enter a website in the address bar above.</p>"
    "<h2>Quick guide</h2>"
    "<div class='card'><p class='tag'>Search and browse</p>"
    "<p>Type a search or web address above. Use Back, Forward, and + to move between pages and tabs.</p></div>"
    "<div class='card'><p class='tag'>Make it yours</p>"
    "<p>Choose Google, this start page, or a custom homepage in Settings.</p></div>"
    "<div class='card'><p class='tag'>Know what to expect</p>"
    "<p>Mxplorer renders basic HTML and CSS. Scripts are off, and HTTPS attempts are disabled by default.</p></div>"
    "<p class='note'>Unsafe HTTPS in Privacy is not secure. Never use it for passwords or private data.</p>"
    "</body></html>";

static void copy_text(char* destination, const char* source, size_t capacity) {
    size_t i = 0;
    if (!capacity) return;
    while (source && source[i] && i + 1 < capacity) {
        destination[i] = source[i];
        i++;
    }
    destination[i] = '\0';
}

static void add_debug_log(const char* prefix, const char* message) {
    if (debug_log_count == (int)(sizeof(debug_logs) / sizeof(debug_logs[0]))) {
        for (int i = 1; i < debug_log_count; i++)
            copy_text(debug_logs[i - 1], debug_logs[i], sizeof(debug_logs[i - 1]));
        debug_log_count--;
    }
    char* line = debug_logs[debug_log_count++];
    copy_text(line, prefix, sizeof(debug_logs[0]));
    size_t used = strlen(line);
    if (used + 1 < sizeof(debug_logs[0])) {
        line[used++] = ' ';
        line[used] = '\0';
    }
    copy_text(line + used, message, sizeof(debug_logs[0]) - used);
}

static int text_equal(const char* left, const char* right) {
    while (*left && *right && *left == *right) {
        left++;
        right++;
    }
    return *left == '\0' && *right == '\0';
}

static int starts_with(const char* text, const char* prefix) {
    while (*prefix) {
        if (*text++ != *prefix++) return 0;
    }
    return 1;
}

static const char* homepage_url(void) {
    if (homepage_mode == HOMEPAGE_MXPLORER) return "mos://home";
    if (homepage_mode == HOMEPAGE_CUSTOM && custom_homepage[0])
        return custom_homepage;
    return "https://google.com";
}

static void load_homepage_setting(void) {
    long fd = mos_open(HOMEPAGE_SETTING_PATH, SYS_O_RDONLY);
    if (fd < 0) return;
    char config[URL_LENGTH + 4];
    long size = mos_size(fd);
    if (size <= 0 || size >= (long)sizeof(config)) {
        mos_close(fd);
        return;
    }
    long count = mos_read(fd, config, (size_t)size);
    mos_close(fd);
    if (count < 2 || config[1] != '\n') return;
    config[count] = '\0';
    if (config[0] == '0') {
        homepage_mode = HOMEPAGE_GOOGLE;
    } else if (config[0] == '1') {
        homepage_mode = HOMEPAGE_MXPLORER;
    } else if (config[0] == '2' && count > 2) {
        size_t end = 2;
        while (end < (size_t)count && config[end] != '\n') end++;
        size_t length = end - 2;
        if (length < sizeof(custom_homepage)) {
            memcpy(custom_homepage, config + 2, length);
            custom_homepage[length] = '\0';
            if (length) homepage_mode = HOMEPAGE_CUSTOM;
        } else {
            homepage_mode = HOMEPAGE_GOOGLE;
        }
    }
}

static int save_homepage_setting(void) {
    if (!mos_exists("0:/data") && mos_mkdir("0:/data") != SYS_SUCCESS) {
        copy_text(status_message, "Could not create the browser settings folder.",
                  sizeof(status_message));
        return 0;
    }
    if (!mos_exists(HOMEPAGE_SETTING_PATH) &&
        mos_create(HOMEPAGE_SETTING_PATH, "text", "txt") != SYS_SUCCESS) {
        copy_text(status_message, "Could not create the homepage settings file.",
                  sizeof(status_message));
        return 0;
    }
    long fd = mos_open(HOMEPAGE_SETTING_PATH, SYS_O_RDWR);
    if (fd < 0) {
        copy_text(status_message, "Could not open the homepage settings file.",
                  sizeof(status_message));
        return 0;
    }
    char config[URL_LENGTH + 4];
    config[0] = (char)('0' + homepage_mode);
    config[1] = '\n';
    size_t length = 2;
    if (homepage_mode == HOMEPAGE_CUSTOM) {
        size_t custom_length = strlen(custom_homepage);
        memcpy(config + length, custom_homepage, custom_length);
        length += custom_length;
        config[length++] = '\n';
    }
    long written = mos_fwrite(fd, config, length);
    long close_result = mos_close(fd);
    if (written != (long)length || close_result != SYS_SUCCESS) {
        copy_text(status_message, "Could not save the homepage setting.",
                  sizeof(status_message));
        return 0;
    }
    return 1;
}

static void load_homepage_asset(void) {
    size_t fallback_length = sizeof(home_page_fallback) - 1;
    if (fallback_length >= sizeof(home_page_html))
        fallback_length = sizeof(home_page_html) - 1;
    memcpy(home_page_html, home_page_fallback, fallback_length);
    home_page_html[fallback_length] = '\0';
    home_page_html_length = fallback_length;

    long fd = mos_open(HOMEPAGE_ASSET_PATH, SYS_O_RDONLY);
    if (fd < 0) return;
    long size = mos_size(fd);
    if (size > 0 && size < (long)sizeof(home_page_html)) {
        long count = mos_read(fd, home_page_html, (size_t)size);
        if (count == size) {
            home_page_html[count] = '\0';
            home_page_html_length = (size_t)count;
        }
    }
    mos_close(fd);
}

static void set_default_theme(void) {
    theme.background = 0xFFF4F7FAu;
    theme.surface = 0xFFFFFFFFu;
    theme.border = 0xFFD7E0E9u;
    theme.text = 0xFF1F3042u;
    theme.muted = 0xFF687B8Fu;
    theme.button = 0xFFE9EFF5u;
    theme.button_hover = 0xFFDCE7F2u;
    theme.accent = 0xFF3478D4u;
    theme.accent_text = 0xFFFFFFFFu;
}

static int read_os_theme(void) {
    uint32_t header[3] = {0, 0, 0};
    uint8_t style_data[4] = {0, 0, 0, 0};
    int aero = 0;
    long fd = mos_open("0:/programs/mosdesk/settings.dat", SYS_O_RDONLY);
    if (fd <= 0) return 0;

    long header_bytes = mos_read(fd, header, sizeof(header));
    if (mos_size(fd) >= 864 && mos_seek(fd, 860) >= 0 &&
        mos_read(fd, style_data, sizeof(style_data)) == sizeof(style_data)) {
        aero = style_data[0] == 1 && !style_data[1] &&
               !style_data[2] && !style_data[3];
    }
    mos_close(fd);
    if (header_bytes != sizeof(header) || header[0] != 0x4D445331u)
        return 0;

    theme.accent = 0xFF000000u | (header[2] & 0x00FFFFFFu);
    if (aero) {
        theme.background = 0xFFF0F0F0u;
        theme.surface = 0xFFFFFFFFu;
        theme.border = 0xFFABADB3u;
        theme.text = 0xFF000000u;
        theme.muted = 0xFF6D6D6Du;
        theme.button = 0xFFE5E5E5u;
        theme.button_hover = 0xFFDCECFCu;
    }
    return 1;
}

static void init_tab(browser_tab_t* tab) {
    memset(tab, 0, sizeof(*tab));
    copy_text(tab->url, homepage_url(), sizeof(tab->url));
    if (homepage_mode == HOMEPAGE_MXPLORER)
        copy_text(tab->title, "Mxplorer Home", sizeof(tab->title));
    else if (homepage_mode == HOMEPAGE_CUSTOM)
        copy_text(tab->title, "Custom Home", sizeof(tab->title));
    else
        copy_text(tab->title, "Google", sizeof(tab->title));
    copy_text(tab->history[0], tab->url, sizeof(tab->history[0]));
    tab->history_count = 1;
}

static int is_settings_page(const browser_tab_t* tab) {
    return text_equal(tab->url, "mos://settings");
}

static int is_home_page(const browser_tab_t* tab) {
    return text_equal(tab->url, "mos://home");
}

static int resize_surface(uint32_t width, uint32_t height) {
    if (width < MIN_WIN_W || height < MIN_WIN_H ||
        (size_t)width > SIZE_MAX / height / sizeof(uint32_t))
        return 0;
    uint32_t* new_pixels =
        (uint32_t*)malloc((size_t)width * height * sizeof(uint32_t));
    if (!new_pixels) return 0;
    free(pixels);
    pixels = new_pixels;
    surface_width = width;
    surface_height = height;
    return 1;
}

static void update_title(void) {
    if (is_settings_page(&tabs[active_tab]))
        mos_window_set_title(window_id, "Mxplorer - Settings");
    else if (is_home_page(&tabs[active_tab]))
        mos_window_set_title(window_id, "Mxplorer Home");
    else
        mos_window_set_title(window_id, "Mxplorer");
}

static void ensure_active_visible(void) {
    int visible = ((int)surface_width - 64) / TAB_WIDTH;
    if (visible < 1) visible = 1;
    if (active_tab < tab_scroll) tab_scroll = active_tab;
    if (active_tab >= tab_scroll + visible)
        tab_scroll = active_tab - visible + 1;
    if (tab_scroll < 0) tab_scroll = 0;
}

static void set_tab_title(browser_tab_t* tab, int search_query) {
    if (text_equal(tab->url, "mos://settings")) {
        copy_text(tab->title, "Settings", sizeof(tab->title));
        return;
    }
    if (text_equal(tab->url, "mos://home")) {
        copy_text(tab->title, "Mxplorer Home", sizeof(tab->title));
        return;
    }
    if (search_query) {
        copy_text(tab->title, "Google Search", sizeof(tab->title));
        return;
    }
    const char* host = strstr(tab->url, "://");
    host = host ? host + 3 : tab->url;
    size_t length = 0;
    while (host[length] && host[length] != '/' && host[length] != '?' &&
           host[length] != ':' && length + 1 < sizeof(tab->title)) {
        tab->title[length] = host[length];
        length++;
    }
    tab->title[length] = '\0';
    if (!length) copy_text(tab->title, "New Tab", sizeof(tab->title));
}

static void load_page(int force_reload) {
    browser_tab_t* tab = &tabs[active_tab];
    page_loaded = 0;
    if (is_settings_page(tab)) {
        copy_text(status_message, "Mxplorer internal settings", sizeof(status_message));
        add_debug_log("PAGE", status_message);
        page_dirty = 1;
        return;
    }
    if (is_home_page(tab)) {
        mx_html_result_t result =
            mx_html_parse(home_page_html, home_page_html_length, surface_width);
        page_loaded = result == MX_HTML_OK;
        copy_text(status_message,
                  page_loaded ? "Mxplorer home rendered"
                              : mx_html_error(result),
                  sizeof(status_message));
        add_debug_log(page_loaded ? "HTML" : "ERR", status_message);
        page_dirty = 1;
        return;
    }

    char error[sizeof(status_message)];
    size_t body_length = 0;
    copy_text(status_message, "Loading page...", sizeof(status_message));
    add_debug_log("GET", tab->url);
    page_dirty = 1;
    if (!force_reload && tab->page_source &&
        text_equal(tab->source_url, tab->url)) {
        mx_html_result_t cached =
            mx_html_parse(tab->page_source, tab->page_source_length,
                          surface_width);
        if (cached == MX_HTML_OK) {
            page_loaded = 1;
            copy_text(status_message, "Page restored from this tab's cache",
                      sizeof(status_message));
            return;
        }
    }
    if (!mx_http_get(tab->url, page_source, sizeof(page_source), &body_length,
                     allow_insecure_tls, error, sizeof(error))) {
        copy_text(status_message, error, sizeof(status_message));
        add_debug_log("ERR", status_message);
        return;
    }
    mx_html_result_t result =
        mx_html_parse(page_source, body_length, surface_width);
    if (result != MX_HTML_OK) {
        copy_text(status_message, mx_html_error(result), sizeof(status_message));
        add_debug_log("ERR", status_message);
        return;
    }
    char* saved_source = (char*)malloc(body_length + 1);
    if (!saved_source) {
        copy_text(status_message, "Not enough memory to cache this page.",
                  sizeof(status_message));
        return;
    }
    memcpy(saved_source, page_source, body_length + 1);
    free(tab->page_source);
    tab->page_source = saved_source;
    tab->page_source_length = body_length;
    copy_text(tab->source_url, tab->url, sizeof(tab->source_url));
    page_loaded = 1;
    copy_text(status_message, "HTML page loaded", sizeof(status_message));
    add_debug_log("HTML", status_message);
    page_dirty = 1;
}

static void reflow_page(void) {
    if (!page_loaded) {
        page_dirty = 1;
        return;
    }
    mx_html_result_t result =
        mx_html_parse(tabs[active_tab].page_source,
                      tabs[active_tab].page_source_length, surface_width);
    if (result != MX_HTML_OK) {
        page_loaded = 0;
        copy_text(status_message, mx_html_error(result), sizeof(status_message));
    }
    page_dirty = 1;
}

static void push_history(browser_tab_t* tab, const char* url) {
    if (tab->history_count > 0 &&
        text_equal(tab->history[tab->history_index], url))
        return;
    if (tab->history_index + 1 < tab->history_count)
        tab->history_count = tab->history_index + 1;
    if (tab->history_count == MAX_HISTORY) {
        for (int i = 1; i < MAX_HISTORY; i++)
            copy_text(tab->history[i - 1], tab->history[i], URL_LENGTH);
        tab->history_count--;
        tab->history_index--;
    }
    tab->history_index = tab->history_count++;
    copy_text(tab->history[tab->history_index], url, URL_LENGTH);
}

static int encode_query(char* output, size_t capacity, const char* query) {
    static const char hex[] = "0123456789ABCDEF";
    size_t used = 0;
    for (size_t i = 0; query[i]; i++) {
        unsigned char c = (unsigned char)query[i];
        int safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                   c == '.' || c == '~';
        size_t needed = safe ? 1 : 3;
        if (used + needed >= capacity) return 0;
        if (safe) {
            output[used++] = (char)c;
        } else {
            output[used++] = '%';
            output[used++] = hex[c >> 4];
            output[used++] = hex[c & 15];
        }
    }
    output[used] = '\0';
    return 1;
}

static int starts_with_nocase(const char* text, const char* prefix) {
    while (*prefix) {
        char a = *text++;
        char b = *prefix++;
        if (a >= 'A' && a <= 'Z') a = (char)(a + ('a' - 'A'));
        if (b >= 'A' && b <= 'Z') b = (char)(b + ('a' - 'A'));
        if (a != b) return 0;
    }
    return 1;
}

static int normalize_address(const char* input, char* output, size_t capacity,
                             int* is_search) {
    char encoded[URL_LENGTH];
    int has_dot = 0;
    int has_colon = 0;
    int has_space = 0;
    size_t length = 0;
    while (input[length]) {
        if (input[length] == '.') has_dot = 1;
        if (input[length] == ':') has_colon = 1;
        if (input[length] == ' ' || input[length] == '\t') has_space = 1;
        length++;
    }
    *is_search = 0;
    if (!length) return 0;
    if (starts_with_nocase(input, "mos://") ||
        starts_with_nocase(input, "http://") ||
        starts_with_nocase(input, "https://")) {
        size_t scheme_length =
            starts_with_nocase(input, "mos://") ? 6 :
            starts_with_nocase(input, "http://") ? 7 : 8;
        if (length >= capacity) return 0;
        for (size_t i = 0; i < scheme_length; i++)
            output[i] = input[i] >= 'A' && input[i] <= 'Z'
                            ? (char)(input[i] + ('a' - 'A')) : input[i];
        copy_text(output + scheme_length, input + scheme_length,
                  capacity - scheme_length);
        return 1;
    }
    if (has_space || (!has_dot && !has_colon)) {
        static const char prefix[] = "https://google.com/search?q=";
        if (!encode_query(encoded, sizeof(encoded), input) ||
            sizeof(prefix) - 1 + strlen(encoded) >= capacity)
            return 0;
        copy_text(output, prefix, capacity);
        copy_text(output + sizeof(prefix) - 1, encoded,
                  capacity - (sizeof(prefix) - 1));
        *is_search = 1;
        return 1;
    }
    if (length + sizeof("https://") > capacity) return 0;
    copy_text(output, "https://", capacity);
    copy_text(output + sizeof("https://") - 1, input,
              capacity - (sizeof("https://") - 1));
    return 1;
}

static void navigate_to(const char* input) {
    char destination[URL_LENGTH];
    int is_search = 0;
    if (!normalize_address(input, destination, sizeof(destination), &is_search)) {
        copy_text(status_message, "Address is empty or too long", sizeof(status_message));
        return;
    }
    browser_tab_t* tab = &tabs[active_tab];
    homepage_editing = 0;
    copy_text(tab->url, destination, sizeof(tab->url));
    set_tab_title(tab, is_search);
    push_history(tab, destination);
    copy_text(address_text, destination, sizeof(address_text));
    address_focused = 0;
    address_select_all = 0;
    address_cursor = (int)strlen(address_text);
    address_scroll = 0;
    load_page(0);
    chrome_dirty = 1;
    page_dirty = 1;
    update_title();
}

static void go_back(void) {
    browser_tab_t* tab = &tabs[active_tab];
    if (tab->history_index <= 0) return;
    tab->history_index--;
    copy_text(tab->url, tab->history[tab->history_index], sizeof(tab->url));
    set_tab_title(tab, 0);
    copy_text(address_text, tab->url, sizeof(address_text));
    address_cursor = (int)strlen(address_text);
    address_scroll = 0;
    load_page(0);
    chrome_dirty = 1;
    page_dirty = 1;
    update_title();
}

static void go_forward(void) {
    browser_tab_t* tab = &tabs[active_tab];
    if (tab->history_index + 1 >= tab->history_count) return;
    tab->history_index++;
    copy_text(tab->url, tab->history[tab->history_index], sizeof(tab->url));
    set_tab_title(tab, 0);
    copy_text(address_text, tab->url, sizeof(address_text));
    address_cursor = (int)strlen(address_text);
    address_scroll = 0;
    load_page(0);
    chrome_dirty = 1;
    page_dirty = 1;
    update_title();
}

static void new_tab(void) {
    if (tab_count >= MAX_TABS) {
        copy_text(status_message, "Maximum number of tabs reached", sizeof(status_message));
        return;
    }
    init_tab(&tabs[tab_count]);
    active_tab = tab_count++;
    copy_text(address_text, tabs[active_tab].url, sizeof(address_text));
    address_focused = 0;
    address_cursor = (int)strlen(address_text);
    address_scroll = 0;
    load_page(0);
    chrome_dirty = 1;
    page_dirty = 1;
    ensure_active_visible();
    update_title();
}

static void close_tab(int index) {
    if (index < 0 || index >= tab_count) return;
    if (tab_count == 1) {
        free(tabs[0].page_source);
        init_tab(&tabs[0]);
        active_tab = 0;
    } else {
        free(tabs[index].page_source);
        for (int i = index + 1; i < tab_count; i++) tabs[i - 1] = tabs[i];
        tab_count--;
        if (active_tab > index) active_tab--;
        else if (active_tab == index && active_tab >= tab_count) active_tab = tab_count - 1;
    }
    drag_tab = -1;
    copy_text(address_text, tabs[active_tab].url, sizeof(address_text));
    address_focused = 0;
    address_cursor = (int)strlen(address_text);
    address_scroll = 0;
    load_page(0);
    chrome_dirty = 1;
    page_dirty = 1;
    ensure_active_visible();
    update_title();
}

static void reorder_tab(int from, int to) {
    if (from < 0 || to < 0 || from >= tab_count || to >= tab_count ||
        from == to)
        return;
    browser_tab_t moved = tabs[from];
    if (from < to) {
        for (int i = from; i < to; i++) tabs[i] = tabs[i + 1];
    } else {
        for (int i = from; i > to; i--) tabs[i] = tabs[i - 1];
    }
    tabs[to] = moved;
    if (active_tab == from) active_tab = to;
    else if (from < active_tab && active_tab <= to) active_tab--;
    else if (to <= active_tab && active_tab < from) active_tab++;
    drag_tab = to;
}

static int visible_tab_at(int x) {
    int first_x = 8;
    int index = tab_scroll + (x - first_x) / TAB_WIDTH;
    if (x < first_x || index < tab_scroll || index >= tab_count ||
        x >= (int)surface_width - 48)
        return -1;
    int tab_x = first_x + (index - tab_scroll) * TAB_WIDTH;
    if (x >= tab_x + TAB_WIDTH) return -1;
    return index;
}

static void activate_category_at(int x, int y) {
    int top = PAGE_Y + 78;
    if (x >= 12 && x < 194 && y >= top && y < top + 4 * 44) {
        int category = (y - top) / 44;
        if (category >= 0 && category < 4) settings_category = category;
    }
}

static int activate_privacy_setting_at(int x, int y) {
    int content_x = 238;
    int setting_y = PAGE_Y + 20 + 90;
    if (settings_category != 2 || x < content_x ||
        x >= (int)surface_width - 28 || y < setting_y ||
        y >= setting_y + 76)
        return 0;
    allow_insecure_tls = !allow_insecure_tls;
    copy_text(status_message,
              allow_insecure_tls
                  ? "Unsafe HTTPS enabled: the SDK TLS transport is not secure."
                  : "Unsafe HTTPS disabled.",
              sizeof(status_message));
    return 1;
}

static int activate_homepage_setting_at(int x, int y) {
    int content_x = 238;
    int content_y = PAGE_Y + 20;
    if (settings_category != 0 || x < content_x ||
        x >= (int)surface_width - 24)
        return 0;
    int selected_mode = -1;
    if (y >= content_y + 112 && y < content_y + 140)
        selected_mode = HOMEPAGE_GOOGLE;
    else if (y >= content_y + 140 && y < content_y + 168)
        selected_mode = HOMEPAGE_MXPLORER;
    else if (y >= content_y + 168 && y < content_y + 196)
        selected_mode = HOMEPAGE_CUSTOM;
    else if (y >= content_y + 201 && y < content_y + 244)
        selected_mode = HOMEPAGE_CUSTOM;
    if (selected_mode < 0) return 0;

    if (selected_mode == HOMEPAGE_CUSTOM) {
        homepage_previous_mode = homepage_mode;
        homepage_mode = HOMEPAGE_CUSTOM;
        homepage_editing = 1;
        address_focused = 1;
        copy_text(address_text, custom_homepage, sizeof(address_text));
        address_cursor = (int)strlen(address_text);
        address_scroll = 0;
        address_select_all = address_cursor != 0;
        copy_text(status_message,
                  "Edit the address above, then press Enter to save your homepage.",
                  sizeof(status_message));
    } else {
        homepage_mode = selected_mode;
        homepage_editing = 0;
        address_focused = 0;
        address_select_all = 0;
        copy_text(address_text, tabs[active_tab].url, sizeof(address_text));
        address_cursor = (int)strlen(address_text);
        address_scroll = 0;
        if (save_homepage_setting())
            copy_text(status_message,
                      selected_mode == HOMEPAGE_GOOGLE
                          ? "Homepage set to Google."
                          : "Homepage set to Mxplorer alternative.",
                      sizeof(status_message));
    }
    return 1;
}

static int address_cursor_at_x(int x) {
    int text_x = 200;
    if (x <= text_x) return address_scroll;
    int cursor = 0;
    int previous_width = 0;
    char prefix[URL_LENGTH];
    size_t length = strlen(address_text);
    if (length >= sizeof(prefix)) length = sizeof(prefix) - 1;
    for (size_t i = (size_t)address_scroll; i < length; i++) {
        size_t visible_length = i - (size_t)address_scroll + 1;
        memcpy(prefix, address_text + address_scroll, visible_length);
        prefix[visible_length] = '\0';
        int width = text_width(prefix, 1);
        if (x - text_x < previous_width + (width - previous_width) / 2)
            return (int)i;
        cursor = (int)i + 1;
        previous_width = width;
    }
    return cursor ? cursor : address_scroll;
}

static void update_address_scroll(void) {
    int available = (int)surface_width - 310;
    if (available < 20) available = 20;
    int length = (int)strlen(address_text);
    if (address_cursor < 0) address_cursor = 0;
    if (address_cursor > length) address_cursor = length;
    if (address_select_all) address_scroll = 0;
    if (address_scroll > address_cursor) address_scroll = address_cursor;
    char visible_prefix[URL_LENGTH];
    while (address_scroll < address_cursor) {
        int count = address_cursor - address_scroll;
        memcpy(visible_prefix, address_text + address_scroll, (size_t)count);
        visible_prefix[count] = '\0';
        if (text_width(visible_prefix, 1) <= available) break;
        address_scroll++;
    }
}

static void handle_mouse(const syscall_window_event_t* event) {
    int left_down = (event->buttons & SYSCALL_MOUSE_BTN_LEFT) != 0;
    int pressed = left_down && !(previous_buttons & SYSCALL_MOUSE_BTN_LEFT);
    int released = !left_down && (previous_buttons & SYSCALL_MOUSE_BTN_LEFT);
    if (event->type == SYSCALL_WINDOW_EVENT_MOUSE_WHEEL) {
        if (event->y < 44) {
            int visible = ((int)surface_width - 64) / TAB_WIDTH;
            if (visible < 1) visible = 1;
            tab_scroll += event->wheel < 0 ? 1 : -1;
            if (tab_scroll < 0) tab_scroll = 0;
            if (tab_scroll > tab_count - visible)
                tab_scroll = tab_count > visible ? tab_count - visible : 0;
            chrome_dirty = 1;
        } else if (event->y >= (int)MX_HTML_VIEW_TOP &&
                   (!debug_open ||
                    event->y < (int)surface_height -
                                   MX_BROWSER_DEBUG_PANEL_HEIGHT) &&
                   !is_settings_page(&tabs[active_tab]) && page_loaded) {
            mx_html_scroll(event->wheel < 0 ? 48 : -48,
                           surface_height - MX_HTML_VIEW_TOP -
                               (debug_open ? MX_BROWSER_DEBUG_PANEL_HEIGHT : 0));
            page_dirty = 1;
        }
        return;
    }
    if (pressed) {
        if (debug_open &&
            event->y >= (int)surface_height - MX_BROWSER_DEBUG_PANEL_HEIGHT) {
            int panel_top =
                (int)surface_height - MX_BROWSER_DEBUG_PANEL_HEIGHT;
            if (event->y < panel_top + 38) {
                if (event->x >= (int)surface_width - 44)
                    debug_open = 0;
                else {
                    debug_tab = (event->x - 170) / 112;
                    if (debug_tab < 0) debug_tab = 0;
                    if (debug_tab > 2) debug_tab = 2;
                }
            }
            page_dirty = 1;
            previous_buttons = event->buttons;
            return;
        }
        address_focused = 0;
        if (event->y < 44) {
            if (event->x >= (int)surface_width - 46) {
                new_tab();
            } else {
                int index = visible_tab_at(event->x);
                if (index >= 0) {
                    int tab_x = 8 + (index - tab_scroll) * TAB_WIDTH;
                    if (event->x >= tab_x + TAB_WIDTH - 38)
                        close_tab(index);
                    else {
                        active_tab = index;
                        drag_tab = index;
                        copy_text(address_text, tabs[active_tab].url,
                                  sizeof(address_text));
                        address_cursor = (int)strlen(address_text);
                        address_scroll = 0;
                        update_title();
                        load_page(0);
                        page_dirty = 1;
                    }
                }
            }
            chrome_dirty = 1;
        } else if (event->y >= TOOLBAR_Y &&
                   event->y < TOOLBAR_Y + TOOLBAR_H) {
            if (event->x < 40) go_back();
            else if (event->x < 82) go_forward();
            else if (event->x < 122) {
                tabs[active_tab].refresh_count++;
                load_page(1);
            } else if (event->x < 156) {
                navigate_to(homepage_url());
            } else if (event->x >= (int)surface_width - 92) {
                navigate_to("mos://settings");
            } else {
                address_focused = 1;
                address_select_all = 0;
                address_cursor = address_cursor_at_x(event->x);
                update_address_scroll();
            }
            chrome_dirty = 1;
            page_dirty = 1;
        } else if (is_settings_page(&tabs[active_tab])) {
            activate_category_at(event->x, event->y);
            if (!activate_homepage_setting_at(event->x, event->y))
                activate_privacy_setting_at(event->x, event->y);
            page_dirty = 1;
        }
    }
    if (left_down && drag_tab >= 0 && event->y < 44) {
        int target = visible_tab_at(event->x);
        if (target >= 0 && target != drag_tab) {
            reorder_tab(drag_tab, target);
            ensure_active_visible();
            chrome_dirty = 1;
        }
    }
    if (released) drag_tab = -1;
    previous_buttons = event->buttons;
}

static void replace_selected_address(void) {
    if (!address_select_all) return;
    address_text[0] = '\0';
    address_cursor = 0;
    address_scroll = 0;
    address_select_all = 0;
}

static void insert_address_character(char character) {
    if (character < 32 || character >= 127) return;
    replace_selected_address();
    size_t length = strlen(address_text);
    if (length + 1 < sizeof(address_text) &&
        address_cursor >= 0 && address_cursor <= (int)length) {
        memmove(address_text + address_cursor + 1,
                address_text + address_cursor, length - (size_t)address_cursor + 1);
        address_text[address_cursor++] = character;
    }
    update_address_scroll();
}

static void save_custom_homepage(void) {
    char normalized[URL_LENGTH];
    int is_search = 0;
    if (!address_text[0] ||
        !normalize_address(address_text, normalized, sizeof(normalized),
                           &is_search) ||
        is_search ||
        (!starts_with_nocase(normalized, "http://") &&
         !starts_with_nocase(normalized, "https://"))) {
        copy_text(status_message,
                  "Enter a website address for your custom homepage.",
                  sizeof(status_message));
        page_dirty = 1;
        return;
    }
    copy_text(custom_homepage, normalized, sizeof(custom_homepage));
    homepage_mode = HOMEPAGE_CUSTOM;
    homepage_editing = 0;
    address_focused = 0;
    address_select_all = 0;
    copy_text(address_text, custom_homepage, sizeof(address_text));
    address_cursor = (int)strlen(address_text);
    address_scroll = 0;
    if (save_homepage_setting())
        copy_text(status_message, "Custom homepage saved.", sizeof(status_message));
    chrome_dirty = 1;
    page_dirty = 1;
}

static void handle_key(const syscall_window_event_t* event) {
    if (!event->buttons) return;
    if ((event->mods & SYSCALL_INPUT_MOD_CTRL) && event->key == 20u) {
        debug_open = !debug_open;
        if (debug_open && page_loaded)
            mx_html_scroll(0, surface_height - MX_HTML_VIEW_TOP -
                                  MX_BROWSER_DEBUG_PANEL_HEIGHT);
        page_dirty = 1;
        return;
    }
    if ((event->mods & SYSCALL_INPUT_MOD_ALT) && event->key == 80u) {
        go_back();
        return;
    }
    if ((event->mods & SYSCALL_INPUT_MOD_ALT) && event->key == 79u) {
        go_forward();
        return;
    }
    if ((event->mods & SYSCALL_INPUT_MOD_CTRL) && event->key == 15u) {
        address_focused = 1;
        address_select_all = 1;
        copy_text(address_text, tabs[active_tab].url, sizeof(address_text));
        address_cursor = (int)strlen(address_text);
        address_scroll = 0;
        chrome_dirty = 1;
        return;
    }
    if (address_focused &&
        (event->mods & SYSCALL_INPUT_MOD_CTRL) && event->key == 4u) {
        address_select_all = 1;
        address_cursor = (int)strlen(address_text);
        address_scroll = 0;
        chrome_dirty = 1;
        return;
    }
    if (!address_focused) return;
    if (event->key == 40u) {
        if (homepage_editing)
            save_custom_homepage();
        else
            navigate_to(address_text);
    } else if (event->key == 41u) {
        if (homepage_editing) {
            homepage_mode = homepage_previous_mode;
            page_dirty = 1;
        }
        homepage_editing = 0;
        address_focused = 0;
        address_select_all = 0;
        address_scroll = 0;
        copy_text(address_text, tabs[active_tab].url, sizeof(address_text));
        address_cursor = (int)strlen(address_text);
        chrome_dirty = 1;
    } else if (event->key == 42u) {
        replace_selected_address();
        if (address_cursor > 0) {
            size_t length = strlen(address_text);
            memmove(address_text + address_cursor - 1,
                    address_text + address_cursor,
                    length - (size_t)address_cursor + 1);
            address_cursor--;
        }
        update_address_scroll();
        chrome_dirty = 1;
    } else if (event->key == 76u) {
        replace_selected_address();
        size_t length = strlen(address_text);
        if (address_cursor < (int)length)
            memmove(address_text + address_cursor,
                    address_text + address_cursor + 1,
                    length - (size_t)address_cursor);
        update_address_scroll();
        chrome_dirty = 1;
    } else if (event->key == 80u || event->key == 79u ||
               event->key == 74u || event->key == 77u) {
        int length = (int)strlen(address_text);
        if (address_select_all) {
            address_cursor = event->key == 80u || event->key == 74u ? 0 : length;
            address_select_all = 0;
        } else if (event->key == 80u && address_cursor > 0) address_cursor--;
        else if (event->key == 79u && address_cursor < length) address_cursor++;
        else if (event->key == 74u) address_cursor = 0;
        else if (event->key == 77u) address_cursor = length;
        update_address_scroll();
        chrome_dirty = 1;
    } else {
        char character = mos_usb_key_to_ascii((uint8_t)event->key,
                                              (uint8_t)event->mods);
        insert_address_character(character);
        chrome_dirty = 1;
    }
}

static mx_ui_state_t current_ui_state(void) {
    const char* debug_source = NULL;
    size_t debug_source_length = 0;
    if (is_home_page(&tabs[active_tab])) {
        debug_source = home_page_html;
        debug_source_length = home_page_html_length;
    } else {
        debug_source = tabs[active_tab].page_source;
        debug_source_length = tabs[active_tab].page_source_length;
    }
    mx_ui_state_t state = {
        .window_id = window_id,
        .pixels = pixels,
        .width = surface_width,
        .height = surface_height,
        .tabs = tabs,
        .tab_count = tab_count,
        .active_tab = active_tab,
        .tab_scroll = &tab_scroll,
        .address_focused = address_focused,
        .address_select_all = address_select_all,
        .address_cursor = address_cursor,
        .address_scroll = address_scroll,
        .settings_category = settings_category,
        .page_loaded = page_loaded,
        .allow_insecure_tls = allow_insecure_tls,
        .homepage_mode = homepage_mode,
        .homepage_editing = homepage_editing,
        .custom_homepage = custom_homepage,
        .debug_open = debug_open,
        .debug_tab = debug_tab,
        .debug_logs = debug_logs,
        .debug_log_count = debug_log_count,
        .debug_source = debug_source,
        .debug_source_length = debug_source_length,
        .address_text = address_text,
        .status_message = status_message,
        .theme = &theme
    };
    return state;
}

static int reload_theme(void) {
    browser_theme_t previous = theme;
    set_default_theme();
    read_os_theme();
    return memcmp(&previous, &theme, sizeof(theme)) != 0;
}

int main(void) {
    int regular_font_loaded =
        font_load("0:/programs/Mxplorer/Resources/fonts/Inter-Regular.otf", 0);
    int medium_font_loaded =
        font_load("0:/programs/Mxplorer/Resources/fonts/Inter-Medium.otf", 1);
    if (!regular_font_loaded || !medium_font_loaded)
        printf("Mxplorer: bundled Inter fonts unavailable; using fallback text\n");
    set_default_theme();
    read_os_theme();
    load_homepage_setting();
    load_homepage_asset();
    init_tab(&tabs[0]);
    tab_count = 1;
    copy_text(address_text, tabs[0].url, sizeof(address_text));

    window_id = mos_window_create(0, 72, 54, WIN_W, WIN_H, "Mxplorer");
    if (!window_id) {
        printf("Mxplorer: unable to create browser window\n");
        return 1;
    }
    mos_window_set_resizable(window_id, true);
    if (!resize_surface(WIN_W, WIN_H)) {
        printf("Mxplorer: unable to allocate browser surface\n");
        mos_window_close(window_id);
        return 1;
    }
    load_page(0);
    chrome_dirty = 1;
    page_dirty = 1;

    for (;;) {
        syscall_window_event_t event = {0};
        while (mos_window_next_event(window_id, &event) == SYS_SUCCESS) {
            if (event.type == SYSCALL_WINDOW_EVENT_CLOSE_REQUEST) {
                running = 0;
                break;
            }
            if (event.type == SYSCALL_WINDOW_EVENT_RESIZED &&
                event.width > 0 && event.height > 0 &&
                (surface_width != (uint32_t)event.width ||
                 surface_height != (uint32_t)event.height)) {
                if (!resize_surface((uint32_t)event.width,
                                    (uint32_t)event.height)) {
                    printf("Mxplorer: unable to resize browser surface\n");
                    free(pixels);
                    mos_window_close(window_id);
                    return 1;
                }
                reflow_page();
                chrome_dirty = 1;
            } else if (event.type == SYSCALL_WINDOW_EVENT_MOUSE ||
                       event.type == SYSCALL_WINDOW_EVENT_MOUSE_WHEEL) {
                handle_mouse(&event);
            } else if (event.type == SYSCALL_WINDOW_EVENT_KEY) {
                handle_key(&event);
            }
        }
        if (!running) break;

        syscall_window_info_t info = {0};
        if (mos_window_get_info(window_id, &info) != SYS_SUCCESS) {
            printf("Mxplorer: browser window is no longer available\n");
            free(pixels);
            return 1;
        }
        if (info.width && info.height &&
            (surface_width != info.width || surface_height != info.height)) {
            if (!resize_surface(info.width, info.height)) {
                printf("Mxplorer: unable to resize browser surface\n");
                free(pixels);
                mos_window_close(window_id);
                return 1;
            }
            reflow_page();
            chrome_dirty = 1;
        }
        long now = mos_uptime();
        if (now - last_theme_check >= 1000) {
            last_theme_check = now;
            if (reload_theme()) {
                chrome_dirty = 1;
                page_dirty = 1;
            }
        }
        mx_ui_state_t ui = current_ui_state();
        if (chrome_dirty) {
            mx_ui_draw_chrome(&ui);
            chrome_dirty = 0;
        }
        if (page_dirty) {
            mx_ui_draw_content(&ui);
            page_dirty = 0;
        }
        mos_sleep(16);
    }

    free(pixels);
    mos_window_close(window_id);
    return 0;
}
