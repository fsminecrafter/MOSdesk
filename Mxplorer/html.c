#include "mxplorer/html.h"
#include "mxplorer/css.h"
#include "mxplorer/text.h"
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
        int width = mx_text_width(part, font_size);
        int space_width = pending_space && layout_x > layout_left
                              ? mx_text_width(" ", font_size) : 0;
        int current_x = layout_x + (line_has_style ? line_width : 0);
        if (current_x > layout_left &&
            current_x + space_width + width > layout_right) {
            html_new_line();
            space_width = 0;
        }
        if (pending_space && line_has_style && layout_x == layout_left &&
            line_text[0])
            space_width = mx_text_width(" ", font_size);
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
            mx_text_draw(pixels, surface_width, surface_height, item->x, y,
                         item->text, item->color, item->font_size);
            if (item->bold)
                mx_text_draw(pixels, surface_width, surface_height,
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
