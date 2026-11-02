#include "mxplorer/css.h"
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
