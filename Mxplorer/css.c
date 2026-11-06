#include "mxplorer/css.h"
#include "string.h"

/* ====================================================================== */
/* Data                                                                   */
/* ====================================================================== */

enum {
    PSEUDO_FIRST_CHILD = 1u << 0,
    PSEUDO_LAST_CHILD = 1u << 1,
    PSEUDO_ONLY_CHILD = 1u << 2,
    PSEUDO_FIRST_OF_TYPE = 1u << 3,
    PSEUDO_LAST_OF_TYPE = 1u << 4,
    PSEUDO_ROOT = 1u << 5,
    PSEUDO_LINK = 1u << 6,
    PSEUDO_EMPTY = 1u << 7,
    PSEUDO_CHECKED = 1u << 8,
    PSEUDO_DISABLED = 1u << 9,
    PSEUDO_ENABLED = 1u << 10,
    PSEUDO_NEVER = 1u << 11,
    PSEUDO_NTH = 1u << 12,
    PSEUDO_NOT = 1u << 13,
    PSEUDO_IS = 1u << 14
};

enum { NTH_CHILD, NTH_LAST_CHILD, NTH_OF_TYPE, NTH_LAST_OF_TYPE };
enum { COMB_NONE, COMB_DESCENDANT, COMB_CHILD, COMB_ADJACENT, COMB_SIBLING };
enum { ATTR_EXISTS, ATTR_EQUALS, ATTR_INCLUDES, ATTR_DASH, ATTR_PREFIX,
       ATTR_SUFFIX, ATTR_SUBSTRING };

struct css_selector_list;

typedef struct {
    uint8_t op;
    uint8_t nocase;
    const char* name;
    const char* value;
} css_attr_t;

typedef struct {
    const char* tag;
    const char* id;
    const char* classes[8];
    uint32_t class_hash[8];
    uint8_t class_length[8];
    css_attr_t attrs[4];
    uint8_t class_count;
    uint8_t attr_count;
    uint8_t combinator;          /* relation to the compound on the left */
    uint8_t nth_kind;
    uint32_t pseudo;
    int nth_a, nth_b;
    struct css_selector_list* not_list;
    struct css_selector_list* is_list;
} css_compound_t;

typedef struct {
    css_compound_t* parts;
    int count;
    int specificity;
    uint16_t ancestor_hash[4];   /* Bloom keys that ancestors must provide */
    uint8_t ancestor_count;
} css_selector_t;

typedef struct css_selector_list {
    css_selector_t* items;
    int count;
} css_selector_list_t;

typedef struct {
    const char* name;            /* custom property name, or NULL */
    const char* value;
    uint16_t property;
    uint8_t important;
} css_decl_t;

typedef struct {
    css_selector_t selector;
    css_decl_t* decls;
    uint16_t decl_count;
    uint8_t origin;
    uint32_t order;
    const char* selector_text;
} css_rule_t;

typedef struct css_bucket {
    uint32_t rule;
    struct css_bucket* next;
} css_bucket_t;

#define CSS_TABLE 4096u

static mx_arena_t css_arena;
/* Bumped by mx_css_reset(): parsed inline styles cached on nodes live in
 * css_arena and are stale after a reset, even though the style attribute
 * (which the cache is keyed on) has not changed. Starts at 1 so a fresh
 * node (0) never matches. */
static uint32_t css_generation = 1;
static css_rule_t* rules;
static unsigned rule_count;
static unsigned rule_capacity;
static css_bucket_t* id_table[CSS_TABLE];
static css_bucket_t* class_table[CSS_TABLE];
static css_bucket_t* tag_table[CSS_TABLE];
static css_bucket_t* attr_table[CSS_TABLE];
static css_bucket_t* universal_rules;
/* tails, so appending keeps source order without walking the lists */
static css_bucket_t* id_tail[CSS_TABLE];
static css_bucket_t* class_tail[CSS_TABLE];
static css_bucket_t* tag_tail[CSS_TABLE];
static css_bucket_t* attr_tail[CSS_TABLE];
static css_bucket_t* universal_tail;
static mx_css_env_t env = {1024, 768, 0};
/* counting Bloom filter of the tags, ids and classes of the elements above
 * the one being styled (active only during mx_css_compute from layout) */
static uint8_t bloom[65536];
static int bloom_active;
static int dark_page;

/* ====================================================================== */
/* Properties                                                             */
/* ====================================================================== */

enum {
    P_UNKNOWN, P_CUSTOM, P_COLOR, P_BACKGROUND, P_BACKGROUND_COLOR,
    P_BACKGROUND_IMAGE, P_FONT, P_FONT_SIZE, P_FONT_WEIGHT, P_FONT_STYLE,
    P_FONT_FAMILY, P_LINE_HEIGHT, P_TEXT_ALIGN, P_TEXT_DECORATION,
    P_TEXT_DECORATION_LINE, P_TEXT_TRANSFORM, P_WHITE_SPACE, P_LETTER_SPACING,
    P_TEXT_INDENT, P_DISPLAY, P_VISIBILITY, P_OPACITY, P_LIST_STYLE,
    P_LIST_STYLE_TYPE, P_POSITION, P_FLOAT, P_CLEAR, P_BOX_SIZING, P_OVERFLOW,
    P_OVERFLOW_X, P_OVERFLOW_Y, P_VERTICAL_ALIGN, P_CURSOR, P_WIDTH, P_HEIGHT,
    P_MIN_WIDTH, P_MAX_WIDTH, P_MIN_HEIGHT, P_MAX_HEIGHT, P_MARGIN,
    P_MARGIN_TOP, P_MARGIN_RIGHT, P_MARGIN_BOTTOM, P_MARGIN_LEFT,
    P_MARGIN_INLINE, P_MARGIN_BLOCK, P_PADDING, P_PADDING_TOP,
    P_PADDING_RIGHT, P_PADDING_BOTTOM, P_PADDING_LEFT, P_PADDING_INLINE,
    P_PADDING_BLOCK, P_BORDER, P_BORDER_TOP, P_BORDER_RIGHT, P_BORDER_BOTTOM,
    P_BORDER_LEFT, P_BORDER_WIDTH, P_BORDER_COLOR, P_BORDER_STYLE,
    P_BORDER_TOP_WIDTH, P_BORDER_RIGHT_WIDTH, P_BORDER_BOTTOM_WIDTH,
    P_BORDER_LEFT_WIDTH, P_BORDER_TOP_COLOR, P_BORDER_RIGHT_COLOR,
    P_BORDER_BOTTOM_COLOR, P_BORDER_LEFT_COLOR, P_BORDER_RADIUS, P_GAP,
    P_ROW_GAP, P_COLUMN_GAP, P_FLEX, P_FLEX_DIRECTION, P_FLEX_WRAP,
    P_FLEX_FLOW, P_FLEX_GROW, P_FLEX_SHRINK, P_FLEX_BASIS, P_JUSTIFY_CONTENT,
    P_ALIGN_ITEMS, P_GRID_TEMPLATE_COLUMNS, P_TOP, P_RIGHT, P_BOTTOM, P_LEFT,
    P_INSET, P_Z_INDEX, P_ORDER, P_PLACE_ITEMS, P_BORDER_INLINE_START,
    P_BORDER_BLOCK_END, P_BORDER_BLOCK_START, P_BORDER_INLINE_END,
    P_COLOR_SCHEME,
    P_COUNT
};

static const char* const property_names[P_COUNT] = {
    "", "", "color", "background", "background-color", "background-image",
    "font", "font-size", "font-weight", "font-style", "font-family",
    "line-height", "text-align", "text-decoration", "text-decoration-line",
    "text-transform", "white-space", "letter-spacing", "text-indent",
    "display", "visibility", "opacity", "list-style", "list-style-type",
    "position", "float", "clear", "box-sizing", "overflow", "overflow-x",
    "overflow-y", "vertical-align", "cursor", "width", "height", "min-width",
    "max-width", "min-height", "max-height", "margin", "margin-top",
    "margin-right", "margin-bottom", "margin-left", "margin-inline",
    "margin-block", "padding", "padding-top", "padding-right",
    "padding-bottom", "padding-left", "padding-inline", "padding-block",
    "border", "border-top", "border-right", "border-bottom", "border-left",
    "border-width", "border-color", "border-style", "border-top-width",
    "border-right-width", "border-bottom-width", "border-left-width",
    "border-top-color", "border-right-color", "border-bottom-color",
    "border-left-color", "border-radius", "gap", "row-gap", "column-gap",
    "flex", "flex-direction", "flex-wrap", "flex-flow", "flex-grow",
    "flex-shrink", "flex-basis", "justify-content", "align-items",
    "grid-template-columns", "top", "right", "bottom", "left", "inset",
    "z-index", "order", "place-items", "border-inline-start",
    "border-block-end", "border-block-start", "border-inline-end",
    "color-scheme",
};

static uint16_t property_id(const char* name, size_t length) {
    for (uint16_t i = 2; i < P_COUNT; i++) {
        const char* p = property_names[i];
        if (strlen(p) == length && mx_strneq_nocase(p, name, length)) return i;
    }
    /* a few aliases and vendor prefixes */
    if (length > 8 && !strncmp(name, "-webkit-", 8))
        return property_id(name + 8, length - 8);
    if (length == 19 && mx_strneq_nocase(name, "margin-inline-start", 19))
        return P_MARGIN_LEFT;
    if (length == 17 && mx_strneq_nocase(name, "margin-inline-end", 17))
        return P_MARGIN_RIGHT;
    if (length == 18 && mx_strneq_nocase(name, "margin-block-start", 18))
        return P_MARGIN_TOP;
    if (length == 16 && mx_strneq_nocase(name, "margin-block-end", 16))
        return P_MARGIN_BOTTOM;
    if (length == 20 && mx_strneq_nocase(name, "padding-inline-start", 20))
        return P_PADDING_LEFT;
    if (length == 18 && mx_strneq_nocase(name, "padding-inline-end", 18))
        return P_PADDING_RIGHT;
    if (length == 19 && mx_strneq_nocase(name, "padding-block-start", 19))
        return P_PADDING_TOP;
    if (length == 17 && mx_strneq_nocase(name, "padding-block-end", 17))
        return P_PADDING_BOTTOM;
    if (length == 8 && mx_strneq_nocase(name, "grid-gap", 8)) return P_GAP;
    if (length == 11 && mx_strneq_nocase(name, "inline-size", 11)) return P_WIDTH;
    if (length == 15 && mx_strneq_nocase(name, "max-inline-size", 15))
        return P_MAX_WIDTH;
    if (length == 10 && mx_strneq_nocase(name, "block-size", 10)) return P_HEIGHT;
    return P_UNKNOWN;
}

/* ====================================================================== */
/* Colors                                                                 */
/* ====================================================================== */

typedef struct {
    const char* name;
    uint32_t rgb;
} named_color_t;

static const named_color_t named_colors[] = {
    {"aliceblue", 0xF0F8FF}, {"antiquewhite", 0xFAEBD7}, {"aqua", 0x00FFFF},
    {"aquamarine", 0x7FFFD4}, {"azure", 0xF0FFFF}, {"beige", 0xF5F5DC},
    {"bisque", 0xFFE4C4}, {"black", 0x000000}, {"blanchedalmond", 0xFFEBCD},
    {"blue", 0x0000FF}, {"blueviolet", 0x8A2BE2}, {"brown", 0xA52A2A},
    {"burlywood", 0xDEB887}, {"cadetblue", 0x5F9EA0},
    {"chartreuse", 0x7FFF00}, {"chocolate", 0xD2691E}, {"coral", 0xFF7F50},
    {"cornflowerblue", 0x6495ED}, {"cornsilk", 0xFFF8DC},
    {"crimson", 0xDC143C}, {"cyan", 0x00FFFF}, {"darkblue", 0x00008B},
    {"darkcyan", 0x008B8B}, {"darkgoldenrod", 0xB8860B},
    {"darkgray", 0xA9A9A9}, {"darkgreen", 0x006400}, {"darkgrey", 0xA9A9A9},
    {"darkkhaki", 0xBDB76B}, {"darkmagenta", 0x8B008B},
    {"darkolivegreen", 0x556B2F}, {"darkorange", 0xFF8C00},
    {"darkorchid", 0x9932CC}, {"darkred", 0x8B0000},
    {"darksalmon", 0xE9967A}, {"darkseagreen", 0x8FBC8F},
    {"darkslateblue", 0x483D8B}, {"darkslategray", 0x2F4F4F},
    {"darkslategrey", 0x2F4F4F}, {"darkturquoise", 0x00CED1},
    {"darkviolet", 0x9400D3}, {"deeppink", 0xFF1493},
    {"deepskyblue", 0x00BFFF}, {"dimgray", 0x696969}, {"dimgrey", 0x696969},
    {"dodgerblue", 0x1E90FF}, {"firebrick", 0xB22222},
    {"floralwhite", 0xFFFAF0}, {"forestgreen", 0x228B22},
    {"fuchsia", 0xFF00FF}, {"gainsboro", 0xDCDCDC}, {"ghostwhite", 0xF8F8FF},
    {"gold", 0xFFD700}, {"goldenrod", 0xDAA520}, {"gray", 0x808080},
    {"green", 0x008000}, {"greenyellow", 0xADFF2F}, {"grey", 0x808080},
    {"honeydew", 0xF0FFF0}, {"hotpink", 0xFF69B4}, {"indianred", 0xCD5C5C},
    {"indigo", 0x4B0082}, {"ivory", 0xFFFFF0}, {"khaki", 0xF0E68C},
    {"lavender", 0xE6E6FA}, {"lavenderblush", 0xFFF0F5},
    {"lawngreen", 0x7CFC00}, {"lemonchiffon", 0xFFFACD},
    {"lightblue", 0xADD8E6}, {"lightcoral", 0xF08080},
    {"lightcyan", 0xE0FFFF}, {"lightgoldenrodyellow", 0xFAFAD2},
    {"lightgray", 0xD3D3D3}, {"lightgreen", 0x90EE90},
    {"lightgrey", 0xD3D3D3}, {"lightpink", 0xFFB6C1},
    {"lightsalmon", 0xFFA07A}, {"lightseagreen", 0x20B2AA},
    {"lightskyblue", 0x87CEFA}, {"lightslategray", 0x778899},
    {"lightslategrey", 0x778899}, {"lightsteelblue", 0xB0C4DE},
    {"lightyellow", 0xFFFFE0}, {"lime", 0x00FF00}, {"limegreen", 0x32CD32},
    {"linen", 0xFAF0E6}, {"magenta", 0xFF00FF}, {"maroon", 0x800000},
    {"mediumaquamarine", 0x66CDAA}, {"mediumblue", 0x0000CD},
    {"mediumorchid", 0xBA55D3}, {"mediumpurple", 0x9370DB},
    {"mediumseagreen", 0x3CB371}, {"mediumslateblue", 0x7B68EE},
    {"mediumspringgreen", 0x00FA9A}, {"mediumturquoise", 0x48D1CC},
    {"mediumvioletred", 0xC71585}, {"midnightblue", 0x191970},
    {"mintcream", 0xF5FFFA}, {"mistyrose", 0xFFE4E1},
    {"moccasin", 0xFFE4B5}, {"navajowhite", 0xFFDEAD}, {"navy", 0x000080},
    {"oldlace", 0xFDF5E6}, {"olive", 0x808000}, {"olivedrab", 0x6B8E23},
    {"orange", 0xFFA500}, {"orangered", 0xFF4500}, {"orchid", 0xDA70D6},
    {"palegoldenrod", 0xEEE8AA}, {"palegreen", 0x98FB98},
    {"paleturquoise", 0xAFEEEE}, {"palevioletred", 0xDB7093},
    {"papayawhip", 0xFFEFD5}, {"peachpuff", 0xFFDAB9}, {"peru", 0xCD853F},
    {"pink", 0xFFC0CB}, {"plum", 0xDDA0DD}, {"powderblue", 0xB0E0E6},
    {"purple", 0x800080}, {"rebeccapurple", 0x663399}, {"red", 0xFF0000},
    {"rosybrown", 0xBC8F8F}, {"royalblue", 0x4169E1},
    {"saddlebrown", 0x8B4513}, {"salmon", 0xFA8072},
    {"sandybrown", 0xF4A460}, {"seagreen", 0x2E8B57},
    {"seashell", 0xFFF5EE}, {"sienna", 0xA0522D}, {"silver", 0xC0C0C0},
    {"skyblue", 0x87CEEB}, {"slateblue", 0x6A5ACD}, {"slategray", 0x708090},
    {"slategrey", 0x708090}, {"snow", 0xFFFAFA}, {"springgreen", 0x00FF7F},
    {"steelblue", 0x4682B4}, {"tan", 0xD2B48C}, {"teal", 0x008080},
    {"thistle", 0xD8BFD8}, {"tomato", 0xFF6347}, {"turquoise", 0x40E0D0},
    {"violet", 0xEE82EE}, {"wheat", 0xF5DEB3}, {"white", 0xFFFFFF},
    {"whitesmoke", 0xF5F5F5}, {"yellow", 0xFFFF00},
    {"yellowgreen", 0x9ACD32},
    /* system colors */
    {"canvas", 0xFFFFFF}, {"canvastext", 0x000000}, {"linktext", 0x1A0DAB},
    {"buttonface", 0xEFEFEF}, {"buttontext", 0x000000},
    {"field", 0xFFFFFF}, {"fieldtext", 0x000000}, {"graytext", 0x808080},
    {"highlight", 0x3478D4}, {"highlighttext", 0xFFFFFF},
};

#define COLOR_CURRENT 0x01000000u  /* "currentColor" marker */

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Parses a decimal number; returns chars consumed (0 if none). */
static int parse_number(const char* s, double* out) {
    const char* p = s;
    int negative = 0;
    if (*p == '+' || *p == '-') negative = *p++ == '-';
    double value = 0;
    int digits = 0;
    while (*p >= '0' && *p <= '9') {
        value = value * 10 + (*p++ - '0');
        digits++;
    }
    if (*p == '.' && p[1] >= '0' && p[1] <= '9') {
        p++;
        double scale = 0.1;
        while (*p >= '0' && *p <= '9') {
            value += (*p++ - '0') * scale;
            scale *= 0.1;
            digits++;
        }
    }
    if (!digits) return 0;
    if ((*p == 'e' || *p == 'E') &&
        ((p[1] >= '0' && p[1] <= '9') ||
         ((p[1] == '-' || p[1] == '+') && p[2] >= '0' && p[2] <= '9'))) {
        p++;
        int exp_negative = 0;
        if (*p == '+' || *p == '-') exp_negative = *p++ == '-';
        int exponent = 0;
        while (*p >= '0' && *p <= '9') exponent = exponent * 10 + (*p++ - '0');
        while (exponent-- > 0) value = exp_negative ? value / 10 : value * 10;
    }
    *out = negative ? -value : value;
    return (int)(p - s);
}

static double clamp01(double v) {
    return v < 0 ? 0 : v > 1 ? 1 : v;
}

/* Reads up to 4 numeric components of rgb()/hsl(); percentages scaled. */
static int color_args(const char* p, double* values, int* percent) {
    int count = 0;
    while (*p && *p != ')' && count < 4) {
        while (*p == ' ' || *p == ',' || *p == '/' || *p == '\t') p++;
        double v;
        int n = parse_number(p, &v);
        if (!n) {
            if (!strncmp(p, "none", 4)) {
                v = 0;
                n = 4;
            } else {
                return count;
            }
        }
        p += n;
        percent[count] = *p == '%';
        if (*p == '%') p++;
        while (*p && *p != ' ' && *p != ',' && *p != '/' && *p != ')') p++;
        values[count++] = v;
    }
    return count;
}

static uint32_t hsl_to_rgb(double h, double s, double l) {
    while (h < 0) h += 360;
    while (h >= 360) h -= 360;
    double c = (1 - (2 * l - 1 < 0 ? -(2 * l - 1) : 2 * l - 1)) * s;
    double hp = h / 60.0;
    double x_mod = hp - 2 * (int)(hp / 2);
    double x = c * (1 - (x_mod - 1 < 0 ? -(x_mod - 1) : x_mod - 1));
    double r = 0, g = 0, b = 0;
    if (hp < 1) { r = c; g = x; }
    else if (hp < 2) { r = x; g = c; }
    else if (hp < 3) { g = c; b = x; }
    else if (hp < 4) { g = x; b = c; }
    else if (hp < 5) { r = x; b = c; }
    else { r = c; b = x; }
    double m = l - c / 2;
    unsigned rr = (unsigned)(clamp01(r + m) * 255 + 0.5);
    unsigned gg = (unsigned)(clamp01(g + m) * 255 + 0.5);
    unsigned bb = (unsigned)(clamp01(b + m) * 255 + 0.5);
    return (rr << 16) | (gg << 8) | bb;
}

int mx_css_parse_color(const char* value, uint32_t* color) {
    while (mx_is_space(*value)) value++;
    if (mx_starts_with_nocase(value, "light-dark(")) {
        const char* p = value + 11;
        char first[96], second[96];
        size_t n = 0;
        int depth = 0;
        while (*p && !(*p == ',' && !depth) && n + 1 < sizeof(first)) {
            if (*p == '(') depth++;
            else if (*p == ')') depth--;
            first[n++] = *p++;
        }
        first[n] = '\0';
        if (*p == ',') p++;
        n = 0;
        depth = 0;
        while (*p && !(*p == ')' && !depth) && n + 1 < sizeof(second)) {
            if (*p == '(') depth++;
            else if (*p == ')') depth--;
            second[n++] = *p++;
        }
        second[n] = '\0';
        return mx_css_parse_color(env.dark ? second : first, color);
    }
    if (*value == '#') {
        const char* h = value + 1;
        size_t n = 0;
        while (hex_digit(h[n]) >= 0) n++;
        unsigned r, g, b, a = 255;
        if (n == 3 || n == 4) {
            r = (unsigned)hex_digit(h[0]) * 17u;
            g = (unsigned)hex_digit(h[1]) * 17u;
            b = (unsigned)hex_digit(h[2]) * 17u;
            if (n == 4) a = (unsigned)hex_digit(h[3]) * 17u;
        } else if (n == 6 || n == 8) {
            r = (unsigned)(hex_digit(h[0]) * 16 + hex_digit(h[1]));
            g = (unsigned)(hex_digit(h[2]) * 16 + hex_digit(h[3]));
            b = (unsigned)(hex_digit(h[4]) * 16 + hex_digit(h[5]));
            if (n == 8) a = (unsigned)(hex_digit(h[6]) * 16 + hex_digit(h[7]));
        } else {
            return 0;
        }
        *color = (a << 24) | (r << 16) | (g << 8) | b;
        return 1;
    }
    if (mx_starts_with_nocase(value, "rgb")) {
        const char* p = strchr(value, '(');
        if (!p) return 0;
        double v[4] = {0, 0, 0, 1};
        int pct[4] = {0, 0, 0, 0};
        int n = color_args(p + 1, v, pct);
        if (n < 3) return 0;
        unsigned c[3];
        for (int i = 0; i < 3; i++) {
            double x = pct[i] ? v[i] * 2.55 : v[i];
            c[i] = x < 0 ? 0 : x > 255 ? 255 : (unsigned)(x + 0.5);
        }
        double alpha = n >= 4 ? (pct[3] ? v[3] / 100 : v[3]) : 1;
        unsigned a = (unsigned)(clamp01(alpha) * 255 + 0.5);
        *color = (a << 24) | (c[0] << 16) | (c[1] << 8) | c[2];
        return 1;
    }
    if (mx_starts_with_nocase(value, "hsl")) {
        const char* p = strchr(value, '(');
        if (!p) return 0;
        double v[4] = {0, 0, 0, 1};
        int pct[4] = {0, 0, 0, 0};
        int n = color_args(p + 1, v, pct);
        if (n < 3) return 0;
        double alpha = n >= 4 ? (pct[3] ? v[3] / 100 : v[3]) : 1;
        uint32_t rgb = hsl_to_rgb(v[0], clamp01(v[1] / 100), clamp01(v[2] / 100));
        *color = ((uint32_t)(clamp01(alpha) * 255 + 0.5) << 24) | rgb;
        return 1;
    }
    char name[32];
    size_t n = 0;
    while (value[n] && !mx_is_space(value[n]) && value[n] != ';' &&
           value[n] != ',' && value[n] != ')' && n + 1 < sizeof(name)) {
        name[n] = mx_lower(value[n]);
        n++;
    }
    name[n] = '\0';
    if (!strcmp(name, "transparent")) {
        *color = 0;
        return 1;
    }
    if (!strcmp(name, "currentcolor")) {
        *color = COLOR_CURRENT;
        return 1;
    }
    for (size_t i = 0; i < sizeof(named_colors) / sizeof(named_colors[0]); i++) {
        if (!strcmp(named_colors[i].name, name)) {
            *color = 0xFF000000u | named_colors[i].rgb;
            return 1;
        }
    }
    return 0;
}

/* ====================================================================== */
/* Lengths                                                                */
/* ====================================================================== */

int mx_len_resolve(mx_len_t length, int reference) {
    if (length.kind != MX_LEN_SET) return 0;
    long long value = (long long)length.pct * reference / 10000;
    return length.px + (int)value;
}

typedef struct {
    double px;
    double pct;
} css_sum_t;

static const char* calc_expr(const char* p, int font_size, css_sum_t* out,
                             int* ok);

static const char* skip_ws(const char* p) {
    while (mx_is_space(*p)) p++;
    return p;
}

/* One length term: number+unit, percentage, nested calc or parenthesis. */
static const char* calc_term(const char* p, int font_size, css_sum_t* out,
                             int* ok) {
    p = skip_ws(p);
    out->px = out->pct = 0;
    if (*p == '(') {
        p = calc_expr(p + 1, font_size, out, ok);
        p = skip_ws(p);
        if (*p == ')') p++;
        return p;
    }
    if (mx_starts_with_nocase(p, "calc(")) {
        p = calc_expr(p + 5, font_size, out, ok);
        p = skip_ws(p);
        if (*p == ')') p++;
        return p;
    }
    if (mx_starts_with_nocase(p, "min(") || mx_starts_with_nocase(p, "max(") ||
        mx_starts_with_nocase(p, "clamp(")) {
        int is_clamp = mx_lower(*p) == 'c';
        int is_min = mx_lower(p[1]) == 'i';
        p = strchr(p, '(') + 1;
        css_sum_t values[3];
        int count = 0;
        while (*p && *p != ')' && count < 3) {
            p = calc_expr(p, font_size, &values[count], ok);
            count++;
            p = skip_ws(p);
            if (*p == ',') p++;
        }
        if (*p == ')') p++;
        if (!count) {
            *ok = 0;
            return p;
        }
        int pick = 0;
        if (is_clamp && count == 3) {
            /* clamp(min, preferred, max): with percentages unknown here,
             * resolve against an assumed 1000 px container. */
            double v[3];
            for (int i = 0; i < 3; i++) v[i] = values[i].px + values[i].pct * 10;
            pick = v[1] < v[0] ? 0 : v[1] > v[2] ? 2 : 1;
        } else {
            double best = values[0].px + values[0].pct * 10;
            for (int i = 1; i < count; i++) {
                double v = values[i].px + values[i].pct * 10;
                if (is_min ? v < best : v > best) {
                    best = v;
                    pick = i;
                }
            }
        }
        *out = values[pick];
        return p;
    }
    double number;
    int n = parse_number(p, &number);
    if (!n) {
        *ok = 0;
        return p;
    }
    p += n;
    char unit[8];
    int u = 0;
    while (((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || *p == '%') &&
           u < 7) {
        unit[u++] = mx_lower(*p++);
        if (unit[u - 1] == '%') break;
    }
    unit[u] = '\0';
    if (!u || !strcmp(unit, "px")) out->px = number;
    else if (!strcmp(unit, "%")) out->pct = number;
    else if (!strcmp(unit, "em")) out->px = number * font_size;
    else if (!strcmp(unit, "rem")) out->px = number * 16;
    else if (!strcmp(unit, "pt")) out->px = number * 4 / 3;
    else if (!strcmp(unit, "pc")) out->px = number * 16;
    else if (!strcmp(unit, "vw") || !strcmp(unit, "dvw") || !strcmp(unit, "svw"))
        out->px = number * env.viewport_width / 100;
    else if (!strcmp(unit, "vh") || !strcmp(unit, "dvh") ||
             !strcmp(unit, "svh") || !strcmp(unit, "lvh"))
        out->px = number * env.viewport_height / 100;
    else if (!strcmp(unit, "vmin"))
        out->px = number * (env.viewport_width < env.viewport_height
                                ? env.viewport_width : env.viewport_height) / 100;
    else if (!strcmp(unit, "vmax"))
        out->px = number * (env.viewport_width > env.viewport_height
                                ? env.viewport_width : env.viewport_height) / 100;
    else if (!strcmp(unit, "ex") || !strcmp(unit, "ch"))
        out->px = number * font_size / 2;
    else if (!strcmp(unit, "in")) out->px = number * 96;
    else if (!strcmp(unit, "cm")) out->px = number * 96 / 2.54;
    else if (!strcmp(unit, "mm")) out->px = number * 96 / 25.4;
    else if (!strcmp(unit, "fr")) out->pct = number * 100;
    else *ok = 0;
    return p;
}

static const char* calc_product(const char* p, int font_size, css_sum_t* out,
                                int* ok) {
    p = calc_term(p, font_size, out, ok);
    for (;;) {
        p = skip_ws(p);
        if (*p != '*' && *p != '/') return p;
        char op = *p++;
        css_sum_t rhs;
        p = calc_term(p, font_size, &rhs, ok);
        /* the unitless side is the factor */
        double factor = rhs.px;
        if (op == '*' && out->pct == 0 && rhs.pct != 0) {
            factor = out->px;
            *out = rhs;
        }
        if (op == '*') {
            out->px *= factor;
            out->pct *= factor;
        } else if (factor != 0) {
            out->px /= factor;
            out->pct /= factor;
        }
    }
}

static const char* calc_expr(const char* p, int font_size, css_sum_t* out,
                             int* ok) {
    p = calc_product(p, font_size, out, ok);
    for (;;) {
        p = skip_ws(p);
        if (*p != '+' && *p != '-') return p;
        char op = *p++;
        css_sum_t rhs;
        p = calc_product(p, font_size, &rhs, ok);
        if (op == '+') {
            out->px += rhs.px;
            out->pct += rhs.pct;
        } else {
            out->px -= rhs.px;
            out->pct -= rhs.pct;
        }
    }
}

static int parse_length(const char* value, int font_size, mx_len_t* out) {
    value = skip_ws(value);
    if (mx_starts_with_nocase(value, "auto")) {
        out->kind = MX_LEN_AUTO;
        out->px = out->pct = 0;
        return 1;
    }
    if (mx_starts_with_nocase(value, "none") ||
        mx_starts_with_nocase(value, "max-content") ||
        mx_starts_with_nocase(value, "min-content") ||
        mx_starts_with_nocase(value, "fit-content") ||
        mx_starts_with_nocase(value, "initial") ||
        mx_starts_with_nocase(value, "unset")) {
        out->kind = MX_LEN_NONE;
        out->px = out->pct = 0;
        return 1;
    }
    css_sum_t sum;
    int ok = 1;
    calc_term(value, font_size, &sum, &ok);
    if (!ok) return 0;
    out->kind = MX_LEN_SET;
    out->px = (int)(sum.px + (sum.px < 0 ? -0.5 : 0.5));
    out->pct = (int)(sum.pct * 100);
    return 1;
}

static int parse_px(const char* value, int font_size, int* out) {
    mx_len_t length;
    if (!parse_length(value, font_size, &length) || length.kind != MX_LEN_SET)
        return 0;
    *out = length.px + length.pct * font_size / 10000;
    return 1;
}

/* ====================================================================== */
/* Value tokens                                                           */
/* ====================================================================== */

/* Copies the next whitespace-separated token (parentheses kept whole). */
static const char* next_token(const char* p, char* out, size_t capacity) {
    p = skip_ws(p);
    size_t used = 0;
    int depth = 0;
    char quote = 0;
    while (*p) {
        char c = *p;
        if (quote) {
            if (c == quote) quote = 0;
        } else if (c == '"' || c == '\'') {
            quote = c;
        } else if (c == '(') {
            depth++;
        } else if (c == ')') {
            if (depth) depth--;
        } else if ((mx_is_space(c) || c == ',') && !depth) {
            if (used) break;
            if (c == ',') {
                if (used + 1 < capacity) out[used++] = c;
                p++;
                break;
            }
        }
        if (used + 1 < capacity) out[used++] = c;
        p++;
    }
    out[used] = '\0';
    return p;
}

static int is_word(const char* value, const char* word) {
    value = skip_ws(value);
    size_t n = strlen(word);
    if (!mx_strneq_nocase(value, word, n)) return 0;
    char after = value[n];
    return !after || mx_is_space(after) || after == ';' || after == '!';
}

/* ====================================================================== */
/* Selectors                                                              */
/* ====================================================================== */

static int ident_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_' ||
           (unsigned char)c >= 0x80 || c == '\\';
}

static const char* parse_ident(const char* p, char* out, size_t capacity,
                               int lower) {
    size_t used = 0;
    while (*p && ident_char(*p)) {
        char c = *p++;
        if (c == '\\' && *p) c = *p++;  /* escaped character */
        if (lower) c = mx_lower(c);
        if (used + 1 < capacity) out[used++] = c;
    }
    out[used] = '\0';
    return p;
}

static css_selector_list_t* parse_selector_list(const char* text,
                                                size_t length, int* error);

/* Parses "an+b", "odd", "even" into a, b. */
static void parse_nth(const char* p, int* a, int* b) {
    p = skip_ws(p);
    *a = 0;
    *b = 0;
    if (mx_starts_with_nocase(p, "odd")) {
        *a = 2;
        *b = 1;
        return;
    }
    if (mx_starts_with_nocase(p, "even")) {
        *a = 2;
        return;
    }
    int sign = 1;
    if (*p == '-') {
        sign = -1;
        p++;
    } else if (*p == '+') {
        p++;
    }
    int number = 0, digits = 0;
    while (*p >= '0' && *p <= '9') {
        number = number * 10 + (*p++ - '0');
        digits++;
    }
    if (*p == 'n' || *p == 'N') {
        *a = sign * (digits ? number : 1);
        p++;
        p = skip_ws(p);
        int bsign = 1;
        if (*p == '+' || *p == '-') {
            bsign = *p == '-' ? -1 : 1;
            p++;
        }
        p = skip_ws(p);
        int bn = 0;
        while (*p >= '0' && *p <= '9') bn = bn * 10 + (*p++ - '0');
        *b = bsign * bn;
    } else {
        *b = sign * number;
    }
}

/* Parses one complex selector from [p, end). Returns 0 on error. */
static int parse_selector(const char* p, const char* end,
                          css_selector_t* out) {
    css_compound_t parts[16];
    int count = 0;
    int specificity = 0;
    uint8_t pending = COMB_NONE;
    memset(parts, 0, sizeof(parts));
    while (p < end) {
        while (p < end && mx_is_space(*p)) {
            if (count && pending == COMB_NONE) pending = COMB_DESCENDANT;
            p++;
        }
        if (p >= end) break;
        if (*p == '>' || *p == '+' || *p == '~') {
            if (!count) return 0;
            pending = *p == '>' ? COMB_CHILD : *p == '+' ? COMB_ADJACENT
                                                         : COMB_SIBLING;
            p++;
            continue;
        }
        if (count >= 16) return 0;
        css_compound_t* c = &parts[count];
        c->combinator = count ? (pending ? pending : COMB_DESCENDANT) : COMB_NONE;
        pending = COMB_NONE;
        int any = 0;
        while (p < end && !mx_is_space(*p) && *p != '>' && *p != '+' &&
               *p != '~') {
            char name[96];
            if (*p == '*') {
                p++;
                any = 1;
            } else if (ident_char(*p)) {
                p = parse_ident(p, name, sizeof(name), 1);
                c->tag = mx_arena_strdup(&css_arena, name);
                specificity += 1;
                any = 1;
            } else if (*p == '#') {
                p = parse_ident(p + 1, name, sizeof(name), 0);
                c->id = mx_arena_strdup(&css_arena, name);
                specificity += 10000;
                any = 1;
            } else if (*p == '.') {
                p = parse_ident(p + 1, name, sizeof(name), 0);
                if (c->class_count < 8) {
                    c->class_hash[c->class_count] = mx_hash(name, strlen(name));
                    c->class_length[c->class_count] = (uint8_t)(strlen(name) > 255 ? 255 : strlen(name));
                    c->classes[c->class_count++] = mx_arena_strdup(&css_arena, name);
                }
                specificity += 100;
                any = 1;
            } else if (*p == '[') {
                p++;
                while (p < end && mx_is_space(*p)) p++;
                p = parse_ident(p, name, sizeof(name), 1);
                css_attr_t attr = {ATTR_EXISTS, 0, NULL, NULL};
                attr.name = mx_arena_strdup(&css_arena, name);
                while (p < end && mx_is_space(*p)) p++;
                if (p < end && *p != ']') {
                    char op = *p;
                    if (op == '=') {
                        attr.op = ATTR_EQUALS;
                        p++;
                    } else if (p + 1 < end && p[1] == '=') {
                        attr.op = op == '~' ? ATTR_INCLUDES : op == '|' ? ATTR_DASH :
                                  op == '^' ? ATTR_PREFIX : op == '$' ? ATTR_SUFFIX :
                                  ATTR_SUBSTRING;
                        p += 2;
                    } else {
                        return 0;
                    }
                    while (p < end && mx_is_space(*p)) p++;
                    char value[128];
                    size_t used = 0;
                    if (*p == '"' || *p == '\'') {
                        char quote = *p++;
                        while (p < end && *p != quote) {
                            if (used + 1 < sizeof(value)) value[used++] = *p;
                            p++;
                        }
                        if (p < end) p++;
                    } else {
                        while (p < end && *p != ']' && !mx_is_space(*p)) {
                            if (used + 1 < sizeof(value)) value[used++] = *p;
                            p++;
                        }
                    }
                    value[used] = '\0';
                    attr.value = mx_arena_strdup(&css_arena, value);
                    while (p < end && mx_is_space(*p)) p++;
                    if (p < end && (*p == 'i' || *p == 'I')) {
                        attr.nocase = 1;
                        p++;
                    } else if (p < end && (*p == 's' || *p == 'S')) {
                        p++;
                    }
                    while (p < end && mx_is_space(*p)) p++;
                }
                if (p >= end || *p != ']') return 0;
                p++;
                if (c->attr_count < 4) c->attrs[c->attr_count++] = attr;
                specificity += 100;
                any = 1;
            } else if (*p == ':') {
                int element = p + 1 < end && p[1] == ':';
                p += element ? 2 : 1;
                p = parse_ident(p, name, sizeof(name), 1);
                const char* args = NULL;
                size_t args_length = 0;
                if (p < end && *p == '(') {
                    int depth = 1;
                    args = ++p;
                    while (p < end && depth) {
                        if (*p == '(') depth++;
                        else if (*p == ')') depth--;
                        if (depth) p++;
                    }
                    args_length = (size_t)(p - args);
                    if (p < end) p++;
                }
                any = 1;
                if (element || !strcmp(name, "before") || !strcmp(name, "after") ||
                    !strcmp(name, "first-line") || !strcmp(name, "first-letter")) {
                    c->pseudo |= PSEUDO_NEVER;
                    specificity += 1;
                    continue;
                }
                specificity += 100;
                if (!strcmp(name, "first-child")) c->pseudo |= PSEUDO_FIRST_CHILD;
                else if (!strcmp(name, "last-child")) c->pseudo |= PSEUDO_LAST_CHILD;
                else if (!strcmp(name, "only-child")) c->pseudo |= PSEUDO_ONLY_CHILD;
                else if (!strcmp(name, "first-of-type")) c->pseudo |= PSEUDO_FIRST_OF_TYPE;
                else if (!strcmp(name, "last-of-type")) c->pseudo |= PSEUDO_LAST_OF_TYPE;
                else if (!strcmp(name, "root")) c->pseudo |= PSEUDO_ROOT;
                else if (!strcmp(name, "link") || !strcmp(name, "any-link"))
                    c->pseudo |= PSEUDO_LINK;
                else if (!strcmp(name, "empty")) c->pseudo |= PSEUDO_EMPTY;
                else if (!strcmp(name, "checked")) c->pseudo |= PSEUDO_CHECKED;
                else if (!strcmp(name, "disabled")) c->pseudo |= PSEUDO_DISABLED;
                else if (!strcmp(name, "enabled")) c->pseudo |= PSEUDO_ENABLED;
                else if ((!strcmp(name, "nth-child") || !strcmp(name, "nth-last-child") ||
                          !strcmp(name, "nth-of-type") ||
                          !strcmp(name, "nth-last-of-type")) && args) {
                    char buffer[32];
                    mx_copy_n(buffer, sizeof(buffer), args, args_length);
                    parse_nth(buffer, &c->nth_a, &c->nth_b);
                    c->nth_kind = !strcmp(name, "nth-child") ? NTH_CHILD :
                                  !strcmp(name, "nth-last-child") ? NTH_LAST_CHILD :
                                  !strcmp(name, "nth-of-type") ? NTH_OF_TYPE :
                                  NTH_LAST_OF_TYPE;
                    c->pseudo |= PSEUDO_NTH;
                } else if (!strcmp(name, "not") && args) {
                    int error = 0;
                    c->not_list = parse_selector_list(args, args_length, &error);
                    if (error || !c->not_list) c->pseudo |= PSEUDO_NEVER;
                    else c->pseudo |= PSEUDO_NOT;
                } else if ((!strcmp(name, "is") || !strcmp(name, "where") ||
                            !strcmp(name, "matches") || !strcmp(name, "-webkit-any")) &&
                           args) {
                    int error = 0;
                    c->is_list = parse_selector_list(args, args_length, &error);
                    if (error || !c->is_list) c->pseudo |= PSEUDO_NEVER;
                    else c->pseudo |= PSEUDO_IS;
                    if (!strcmp(name, "where")) specificity -= 100;
                } else {
                    /* :hover, :focus, :visited, :has(), ... never match */
                    c->pseudo |= PSEUDO_NEVER;
                }
            } else {
                return 0;
            }
        }
        if (!any) return 0;
        count++;
    }
    if (!count || pending != COMB_NONE) return 0;
    out->parts = (css_compound_t*)mx_arena_alloc(&css_arena,
                                                 sizeof(css_compound_t) * (size_t)count);
    if (!out->parts) return 0;
    memcpy(out->parts, parts, sizeof(css_compound_t) * (size_t)count);
    out->count = count;
    out->specificity = specificity;
    out->ancestor_count = 0;
    /* a compound is an ancestor of the key when a descendant or child
     * combinator lies between them */
    int ancestor = 0;
    for (int i = count - 2; i >= 0 && out->ancestor_count < 4; i--) {
        uint8_t comb = parts[i + 1].combinator;
        if (comb == COMB_DESCENDANT || comb == COMB_CHILD) ancestor = 1;
        if (!ancestor) continue;
        const css_compound_t* c = &parts[i];
        if (c->pseudo & (PSEUDO_NOT | PSEUDO_IS)) continue;
        const char* key = c->id ? c->id : c->class_count ? c->classes[0] : c->tag;
        if (!key) continue;
        out->ancestor_hash[out->ancestor_count++] =
            (uint16_t)(mx_hash(key, strlen(key)) & 0xFFFFu);
    }
    return 1;
}

/* Splits on top-level commas. */
static css_selector_list_t* parse_selector_list(const char* text,
                                                size_t length, int* error) {
    css_selector_t items[128];
    int count = 0;
    size_t start = 0;
    int depth = 0;
    char quote = 0;
    for (size_t i = 0; i <= length; i++) {
        char c = i < length ? text[i] : ',';
        if (quote) {
            if (c == quote) quote = 0;
            continue;
        }
        if (c == '"' || c == '\'') quote = c;
        else if (c == '(' || c == '[') depth++;
        else if ((c == ')' || c == ']') && depth) depth--;
        else if (c == ',' && !depth) {
            if (count < 128) {
                if (parse_selector(text + start, text + i, &items[count])) count++;
                else *error = 1;
            }
            start = i + 1;
        }
    }
    if (!count) return NULL;
    css_selector_list_t* list =
        (css_selector_list_t*)mx_arena_alloc(&css_arena, sizeof(*list));
    if (!list) return NULL;
    list->items = (css_selector_t*)mx_arena_alloc(&css_arena,
                                                  sizeof(css_selector_t) * (size_t)count);
    if (!list->items) return NULL;
    memcpy(list->items, items, sizeof(css_selector_t) * (size_t)count);
    list->count = count;
    return list;
}

/* ====================================================================== */
/* Matching                                                               */
/* ====================================================================== */

static mx_node_t* parent_element(const mx_node_t* node) {
    mx_node_t* p = node->parent;
    return p && p->type == MX_NODE_ELEMENT ? p : NULL;
}

static int element_index(const mx_node_t* e, int from_end, int same_type) {
    int index = 1;
    for (const mx_node_t* s = from_end ? e->next : e->prev; s;
         s = from_end ? s->next : s->prev) {
        if (s->type != MX_NODE_ELEMENT) continue;
        if (same_type && strcmp(s->tag, e->tag)) continue;
        index++;
    }
    return index;
}

static int nth_matches(int a, int b, int index) {
    if (!a) return index == b;
    int diff = index - b;
    if (diff % a) return 0;
    return diff / a >= 0;
}

static int attr_matches(const css_attr_t* attr, const mx_node_t* e) {
    const char* value = mx_dom_attr(e, attr->name);
    if (!value) return 0;
    const char* want = attr->value;
    size_t vn = strlen(value), wn = want ? strlen(want) : 0;
    switch (attr->op) {
    case ATTR_EXISTS: return 1;
    case ATTR_EQUALS:
        return attr->nocase ? mx_streq_nocase(value, want) : !strcmp(value, want);
    case ATTR_PREFIX: return wn && vn >= wn && !strncmp(value, want, wn);
    case ATTR_SUFFIX: return wn && vn >= wn && !strcmp(value + vn - wn, want);
    case ATTR_SUBSTRING: return wn && strstr(value, want) != NULL;
    case ATTR_DASH:
        return !strcmp(value, want) ||
               (vn > wn && !strncmp(value, want, wn) && value[wn] == '-');
    case ATTR_INCLUDES: {
        const char* p = value;
        while (*p) {
            while (mx_is_space(*p)) p++;
            const char* end = p;
            while (*end && !mx_is_space(*end)) end++;
            if ((size_t)(end - p) == wn && !strncmp(p, want, wn)) return 1;
            p = end;
        }
        return 0;
    }
    }
    return 0;
}

static int list_matches(const css_selector_list_t* list, mx_node_t* e);

static int compound_matches(const css_compound_t* c, mx_node_t* e) {
    if (e->type != MX_NODE_ELEMENT) return 0;
    if (c->pseudo & PSEUDO_NEVER) return 0;
    if (c->tag && strcmp(c->tag, e->tag)) return 0;
    if (c->id) {
        const char* id = mx_dom_attr(e, "id");
        if (!id || strcmp(id, c->id)) return 0;
    }
    for (int i = 0; i < c->class_count; i++)
        if (!mx_dom_has_class_hashed(e, c->classes[i], c->class_length[i],
                                     c->class_hash[i]))
            return 0;
    for (int i = 0; i < c->attr_count; i++)
        if (!attr_matches(&c->attrs[i], e)) return 0;
    uint32_t p = c->pseudo;
    if (!p) return 1;
    if ((p & PSEUDO_FIRST_CHILD) && mx_dom_prev_element(e)) return 0;
    if ((p & PSEUDO_LAST_CHILD) && mx_dom_next_element(e)) return 0;
    if ((p & PSEUDO_ONLY_CHILD) && (mx_dom_prev_element(e) || mx_dom_next_element(e)))
        return 0;
    if ((p & PSEUDO_FIRST_OF_TYPE) && element_index(e, 0, 1) != 1) return 0;
    if ((p & PSEUDO_LAST_OF_TYPE) && element_index(e, 1, 1) != 1) return 0;
    if ((p & PSEUDO_ROOT) && strcmp(e->tag, "html")) return 0;
    if ((p & PSEUDO_LINK) &&
        !((!strcmp(e->tag, "a") || !strcmp(e->tag, "area")) && mx_dom_attr(e, "href")))
        return 0;
    if (p & PSEUDO_EMPTY) {
        for (mx_node_t* child = e->first; child; child = child->next)
            if (child->type == MX_NODE_ELEMENT ||
                (child->type == MX_NODE_TEXT && child->text_length))
                return 0;
    }
    if ((p & PSEUDO_CHECKED) && !e->checked) return 0;
    if ((p & PSEUDO_DISABLED) && !mx_dom_attr(e, "disabled")) return 0;
    if ((p & PSEUDO_ENABLED) && mx_dom_attr(e, "disabled")) return 0;
    if (p & PSEUDO_NTH) {
        int index = element_index(e, c->nth_kind == NTH_LAST_CHILD ||
                                         c->nth_kind == NTH_LAST_OF_TYPE,
                                  c->nth_kind >= NTH_OF_TYPE);
        if (!nth_matches(c->nth_a, c->nth_b, index)) return 0;
    }
    if ((p & PSEUDO_NOT) && list_matches(c->not_list, e)) return 0;
    if ((p & PSEUDO_IS) && !list_matches(c->is_list, e)) return 0;
    return 1;
}

static int selector_matches_at(const css_selector_t* s, int index,
                               mx_node_t* e, int depth) {
    if (!compound_matches(&s->parts[index], e)) return 0;
    if (index == 0) return 1;
    if (depth > 64) return 0;
    switch (s->parts[index].combinator) {
    case COMB_CHILD: {
        mx_node_t* parent = parent_element(e);
        return parent && selector_matches_at(s, index - 1, parent, depth + 1);
    }
    case COMB_ADJACENT: {
        mx_node_t* prev = mx_dom_prev_element(e);
        return prev && selector_matches_at(s, index - 1, prev, depth + 1);
    }
    case COMB_SIBLING:
        for (mx_node_t* prev = mx_dom_prev_element(e); prev;
             prev = mx_dom_prev_element(prev))
            if (selector_matches_at(s, index - 1, prev, depth + 1)) return 1;
        return 0;
    default:
        for (mx_node_t* parent = parent_element(e); parent;
             parent = parent_element(parent))
            if (selector_matches_at(s, index - 1, parent, depth + 1)) return 1;
        return 0;
    }
}

static int list_matches(const css_selector_list_t* list, mx_node_t* e) {
    if (!list) return 0;
    for (int i = 0; i < list->count; i++)
        if (selector_matches_at(&list->items[i], list->items[i].count - 1, e, 0))
            return 1;
    return 0;
}

int mx_css_matches(mx_node_t* element, const char* selectors) {
    if (!element || element->type != MX_NODE_ELEMENT) return 0;
    int error = 0;
    css_selector_list_t* list =
        parse_selector_list(selectors, strlen(selectors), &error);
    if (!list) return -1;
    return list_matches(list, element);
}

/* ====================================================================== */
/* Stylesheet parsing                                                     */
/* ====================================================================== */

static void bucket_append(css_bucket_t** head, css_bucket_t** tail, uint32_t rule) {
    css_bucket_t* entry = (css_bucket_t*)mx_arena_alloc(&css_arena, sizeof(*entry));
    if (!entry) return;
    entry->rule = rule;
    entry->next = NULL;
    if (*tail) (*tail)->next = entry;
    else *head = entry;
    *tail = entry;
}

static void bucket_add(css_bucket_t** table, css_bucket_t** tails, const char* key,
                       uint32_t rule) {
    uint32_t h = mx_hash(key, strlen(key)) % CSS_TABLE;
    bucket_append(&table[h], &tails[h], rule);
}

static void index_rule(uint32_t index) {
    const css_selector_t* s = &rules[index].selector;
    const css_compound_t* key = &s->parts[s->count - 1];
    if (key->pseudo & PSEUDO_NEVER) return;   /* ::before, :hover ... */
    if (key->id) bucket_add(id_table, id_tail, key->id, index);
    else if (key->class_count) bucket_add(class_table, class_tail, key->classes[0], index);
    else if (key->tag) bucket_add(tag_table, tag_tail, key->tag, index);
    else if (key->pseudo & PSEUDO_ROOT) bucket_add(tag_table, tag_tail, "html", index);
    else if (key->attr_count) bucket_add(attr_table, attr_tail, key->attrs[0].name, index);
    else bucket_append(&universal_rules, &universal_tail, index);
}

/* Parses "a: b; c: d" into an arena array. */
static css_decl_t* parse_declarations(const char* source, size_t length,
                                      uint16_t* count_out) {
    static css_decl_t decls[160];   /* scratch; copied out below */
    uint16_t count = 0;
    size_t pos = 0;
    while (pos < length && count < 160) {
        while (pos < length && (mx_is_space(source[pos]) || source[pos] == ';'))
            pos++;
        size_t name_start = pos;
        while (pos < length && source[pos] != ':' && source[pos] != ';' &&
               source[pos] != '{' && source[pos] != '}')
            pos++;
        if (pos >= length || source[pos] != ':') {
            /* skip to the next declaration (nested blocks are skipped whole) */
            int depth = 0;
            while (pos < length) {
                if (source[pos] == '{') depth++;
                else if (source[pos] == '}') {
                    if (!depth) break;
                    depth--;
                    if (!depth) {
                        pos++;
                        break;
                    }
                } else if (source[pos] == ';' && !depth) break;
                pos++;
            }
            if (pos < length && source[pos] == '}') pos++;
            continue;
        }
        size_t name_end = pos++;
        size_t value_start = pos;
        int depth = 0;
        char quote = 0;
        while (pos < length) {
            char c = source[pos];
            if (quote) {
                if (c == '\\') pos++;
                else if (c == quote) quote = 0;
            } else if (c == '"' || c == '\'') {
                quote = c;
            } else if (c == '(') {
                depth++;
            } else if (c == ')') {
                if (depth) depth--;
            } else if ((c == ';' || c == '}') && !depth) {
                break;
            }
            pos++;
        }
        size_t value_end = pos;
        while (name_start < name_end && mx_is_space(source[name_start])) name_start++;
        while (name_end > name_start && mx_is_space(source[name_end - 1])) name_end--;
        while (value_start < value_end && mx_is_space(source[value_start]))
            value_start++;
        while (value_end > value_start && mx_is_space(source[value_end - 1]))
            value_end--;
        css_decl_t d = {NULL, NULL, P_UNKNOWN, 0};
        /* !important */
        if (value_end - value_start >= 10) {
            size_t bang = value_end;
            while (bang > value_start && source[bang - 1] != '!') bang--;
            if (bang > value_start &&
                mx_strneq_nocase(source + bang, "important", 9)) {
                d.important = 1;
                value_end = bang - 1;
                while (value_end > value_start && mx_is_space(source[value_end - 1]))
                    value_end--;
            }
        }
        size_t name_length = name_end - name_start;
        if (name_length > 2 && source[name_start] == '-' &&
            source[name_start + 1] == '-') {
            d.property = P_CUSTOM;
            d.name = mx_arena_strndup(&css_arena, source + name_start, name_length);
        } else {
            d.property = property_id(source + name_start, name_length);
        }
        if (d.property != P_UNKNOWN) {
            d.value = mx_arena_strndup(&css_arena, source + value_start,
                                       value_end - value_start);
            if (d.value) decls[count++] = d;
        }
        if (pos < length && source[pos] == ';') pos++;
    }
    *count_out = count;
    if (!count) return NULL;
    css_decl_t* out = (css_decl_t*)mx_arena_alloc(&css_arena,
                                                  sizeof(css_decl_t) * count);
    if (out) memcpy(out, decls, sizeof(css_decl_t) * count);
    else *count_out = 0;
    return out;
}

static void add_rule(const css_selector_t* selector, css_decl_t* decls,
                     uint16_t decl_count, int origin, const char* text) {
    if (rule_count == rule_capacity) {
        unsigned capacity = rule_capacity ? rule_capacity * 2 : 256;
        css_rule_t* grown =
            (css_rule_t*)mx_arena_alloc(&css_arena, sizeof(css_rule_t) * capacity);
        if (!grown) return;
        if (rule_count) memcpy(grown, rules, sizeof(css_rule_t) * rule_count);
        rules = grown;
        rule_capacity = capacity;
    }
    css_rule_t* rule = &rules[rule_count];
    rule->selector = *selector;
    rule->decls = decls;
    rule->decl_count = decl_count;
    rule->origin = (uint8_t)origin;
    rule->order = rule_count;
    rule->selector_text = text;
    index_rule(rule_count);
    rule_count++;
}

static int media_feature(const char* feature, size_t length) {
    char name[40], op[3] = "", value[40];
    size_t i = 0, n = 0;
    while (i < length && mx_is_space(feature[i])) i++;
    while (i < length && (ident_char(feature[i])) && n + 1 < sizeof(name))
        name[n++] = mx_lower(feature[i++]);
    name[n] = '\0';
    while (i < length && mx_is_space(feature[i])) i++;
    size_t o = 0;
    while (i < length && (feature[i] == ':' || feature[i] == '<' ||
                          feature[i] == '>' || feature[i] == '=') && o < 2)
        op[o++] = feature[i++];
    op[o] = '\0';
    while (i < length && mx_is_space(feature[i])) i++;
    n = 0;
    while (i < length && feature[i] != ')' && n + 1 < sizeof(value))
        value[n++] = mx_lower(feature[i++]);
    while (n && mx_is_space(value[n - 1])) n--;
    value[n] = '\0';

    if (!strcmp(name, "prefers-color-scheme"))
        return !strcmp(value, env.dark ? "dark" : "light");
    if (!strcmp(name, "prefers-reduced-motion")) return !strcmp(value, "no-preference") || !o;
    if (!strcmp(name, "prefers-contrast")) return !strcmp(value, "no-preference");
    if (!strcmp(name, "hover") || !strcmp(name, "any-hover"))
        return !o || !strcmp(value, "hover");
    if (!strcmp(name, "pointer") || !strcmp(name, "any-pointer"))
        return !o || !strcmp(value, "fine");
    if (!strcmp(name, "orientation"))
        return !strcmp(value, env.viewport_width >= env.viewport_height
                                  ? "landscape" : "portrait");
    if (!strcmp(name, "color")) return 1;
    int px = 0;
    int is_width = !strcmp(name, "width") || !strcmp(name, "min-width") ||
                   !strcmp(name, "max-width");
    int is_height = !strcmp(name, "height") || !strcmp(name, "min-height") ||
                    !strcmp(name, "max-height");
    if (is_width || is_height) {
        if (!parse_px(value, 16, &px)) return 0;
        int actual = is_width ? env.viewport_width : env.viewport_height;
        if (!strncmp(name, "min-", 4)) return actual >= px;
        if (!strncmp(name, "max-", 4)) return actual <= px;
        if (!strcmp(op, ">=")) return actual >= px;
        if (!strcmp(op, "<=")) return actual <= px;
        if (!strcmp(op, ">")) return actual > px;
        if (!strcmp(op, "<")) return actual < px;
        return actual == px;
    }
    return 0;
}

/* Evaluates a media query list (comma = or). */
static int media_matches(const char* query, size_t length) {
    size_t start = 0;
    for (size_t i = 0; i <= length; i++) {
        if (i < length && query[i] != ',') continue;
        const char* q = query + start;
        size_t n = i - start;
        start = i + 1;
        int result = 1, negate = 0;
        size_t p = 0;
        while (p < n) {
            while (p < n && mx_is_space(q[p])) p++;
            if (p >= n) break;
            if (q[p] == '(') {
                size_t close = p + 1;
                int depth = 1;
                while (close < n && depth) {
                    if (q[close] == '(') depth++;
                    else if (q[close] == ')') depth--;
                    if (depth) close++;
                }
                if (!media_feature(q + p + 1, close - p - 1)) result = 0;
                p = close + 1;
                continue;
            }
            char word[24];
            size_t w = 0;
            while (p < n && !mx_is_space(q[p]) && q[p] != '(' && w + 1 < sizeof(word))
                word[w++] = mx_lower(q[p++]);
            word[w] = '\0';
            if (!strcmp(word, "not")) negate = 1;
            else if (!strcmp(word, "print") || !strcmp(word, "speech") ||
                     !strcmp(word, "tv") || !strcmp(word, "handheld"))
                result = 0;
        }
        if (negate) result = !result;
        if (result) return 1;
    }
    return 0;
}

int mx_css_media_matches(const char* query) {
    return media_matches(query, strlen(query));
}

/* Finds the matching '}' for the '{' at `open`. */
static size_t block_end(const char* s, size_t length, size_t open) {
    int depth = 0;
    char quote = 0;
    for (size_t i = open; i < length; i++) {
        char c = s[i];
        if (quote) {
            if (c == '\\') i++;
            else if (c == quote) quote = 0;
            continue;
        }
        if (c == '"' || c == '\'') quote = c;
        else if (c == '/' && i + 1 < length && s[i + 1] == '*') {
            i += 2;
            while (i + 1 < length && !(s[i] == '*' && s[i + 1] == '/')) i++;
            i++;
        } else if (c == '{') depth++;
        else if (c == '}') {
            depth--;
            if (!depth) return i;
        }
    }
    return length;
}

static void parse_sheet(const char* s, size_t length, int origin) {
    size_t pos = 0;
    while (pos < length) {
        if (css_arena.failed) return;
        while (pos < length && (mx_is_space(s[pos]) || s[pos] == '}' ||
                                s[pos] == ';'))
            pos++;
        if (pos >= length) break;
        if (pos + 1 < length && s[pos] == '/' && s[pos + 1] == '*') {
            pos += 2;
            while (pos + 1 < length && !(s[pos] == '*' && s[pos + 1] == '/')) pos++;
            pos += 2;
            continue;
        }
        if (pos + 3 < length && !strncmp(s + pos, "<!--", 4)) {
            pos += 4;
            continue;
        }
        if (pos + 2 < length && !strncmp(s + pos, "-->", 3)) {
            pos += 3;
            continue;
        }
        /* prelude up to '{' or ';' */
        size_t prelude = pos;
        char quote = 0;
        while (pos < length) {
            char c = s[pos];
            if (quote) {
                if (c == quote) quote = 0;
            } else if (c == '"' || c == '\'') {
                quote = c;
            } else if (c == '{' || (c == ';' && s[prelude] == '@')) {
                break;
            } else if (c == '/' && pos + 1 < length && s[pos + 1] == '*') {
                break;
            }
            pos++;
        }
        if (pos < length && s[pos] == '/' && s[prelude] != '@') {
            /* comment inside a selector: skip it, keep scanning */
            size_t comment = pos + 2;
            while (comment + 1 < length && !(s[comment] == '*' && s[comment + 1] == '/'))
                comment++;
            /* simplest: treat text before the comment as junk */
            pos = comment + 2;
            continue;
        }
        if (pos >= length) break;
        if (s[pos] == ';') {  /* @import, @charset, @namespace */
            pos++;
            continue;
        }
        size_t open = pos;
        size_t close = block_end(s, length, open);
        size_t prelude_end = open;
        while (prelude_end > prelude && mx_is_space(s[prelude_end - 1])) prelude_end--;
        if (s[prelude] == '@') {
            char name[24];
            size_t n = 0, i = prelude + 1;
            while (i < prelude_end && ident_char(s[i]) && n + 1 < sizeof(name))
                name[n++] = mx_lower(s[i++]);
            name[n] = '\0';
            int include = 0;
            if (!strcmp(name, "media")) {
                include = media_matches(s + i, prelude_end - i);
            } else if (!strcmp(name, "supports")) {
                const char* cond = s + i;
                size_t cn = prelude_end - i;
                include = !(cn > 4 && mx_starts_with_nocase(skip_ws(cond), "not"));
            } else if (!strcmp(name, "layer") || !strcmp(name, "document") ||
                       !strcmp(name, "-moz-document") || !strcmp(name, "container") ||
                       !strcmp(name, "scope")) {
                include = 1;
            }
            if (include && close > open) parse_sheet(s + open + 1, close - open - 1, origin);
            pos = close + 1;
            continue;
        }
        uint16_t decl_count = 0;
        css_decl_t* decls =
            close > open ? parse_declarations(s + open + 1, close - open - 1,
                                              &decl_count)
                         : NULL;
        if (decl_count) {
            int error = 0;
            css_selector_list_t* list =
                parse_selector_list(s + prelude, prelude_end - prelude, &error);
            if (list) {
                const char* text = mx_arena_strndup(&css_arena, s + prelude,
                                                    prelude_end - prelude > 120
                                                        ? 120
                                                        : prelude_end - prelude);
                for (int k = 0; k < list->count; k++)
                    add_rule(&list->items[k], decls, decl_count, origin, text);
            }
        }
        pos = close + 1;
    }
}

void mx_css_reset(void) {
    mx_arena_free_all(&css_arena);
    css_generation++;
    css_arena.limit = 24u * 1024u * 1024u;
    rules = NULL;
    rule_count = rule_capacity = 0;
    memset(id_table, 0, sizeof(id_table));
    memset(class_table, 0, sizeof(class_table));
    memset(tag_table, 0, sizeof(tag_table));
    memset(attr_table, 0, sizeof(attr_table));
    memset(id_tail, 0, sizeof(id_tail));
    memset(class_tail, 0, sizeof(class_tail));
    memset(tag_tail, 0, sizeof(tag_tail));
    memset(attr_tail, 0, sizeof(attr_tail));
    universal_rules = universal_tail = NULL;
}

void mx_css_set_env(const mx_css_env_t* e) {
    env = *e;
}

void mx_css_add_stylesheet(const char* source, size_t length, int origin) {
    if (source && length) parse_sheet(source, length, origin);
}

unsigned mx_css_rule_count(void) {
    return rule_count;
}

/* ====================================================================== */
/* Cascade                                                                */
/* ====================================================================== */

typedef struct {
    mx_style_t* style;
    const mx_style_t* parent;
    mx_arena_t* arena;
    mx_node_t* element;
} apply_ctx_t;

static const char* lookup_var(const mx_var_t* vars, const char* name,
                              size_t length) {
    uint32_t hash = mx_hash(name, length);
    for (const mx_var_t* v = vars; v; v = v->next)
        if (v->hash == hash && v->length == length && !strncmp(v->name, name, length))
            return v->value;
    return NULL;
}

/* Replaces var(--x, fallback) references. Returns 0 when unresolvable. */
static int substitute_vars(const char* value, const mx_var_t* vars, char* out,
                           size_t capacity, int depth) {
    size_t used = 0;
    const char* p = value;
    while (*p) {
        if (!strncmp(p, "var(", 4)) {
            const char* q = skip_ws(p + 4);
            const char* name = q;
            while (*q && *q != ',' && *q != ')' && !mx_is_space(*q)) q++;
            size_t name_length = (size_t)(q - name);
            q = skip_ws(q);
            const char* fallback = NULL;
            size_t fallback_length = 0;
            int nest = 0;
            if (*q == ',') {
                fallback = skip_ws(q + 1);
                q = fallback;
            }
            while (*q && !(*q == ')' && !nest)) {
                if (*q == '(') nest++;
                else if (*q == ')') nest--;
                q++;
            }
            if (fallback) fallback_length = (size_t)(q - fallback);
            const char* replacement = lookup_var(vars, name, name_length);
            char temp[512];
            if (!replacement && fallback) {
                mx_copy_n(temp, sizeof(temp), fallback, fallback_length);
                replacement = temp;
            }
            if (!replacement) return 0;
            char expanded[512];
            if (depth < 8 && strstr(replacement, "var(")) {
                if (!substitute_vars(replacement, vars, expanded, sizeof(expanded),
                                     depth + 1))
                    return 0;
                replacement = expanded;
            }
            size_t n = strlen(replacement);
            if (used + n + 1 >= capacity) return 0;
            memcpy(out + used, replacement, n);
            used += n;
            p = *q ? q + 1 : q;
            continue;
        }
        if (used + 2 >= capacity) return 0;
        out[used++] = *p++;
    }
    out[used] = '\0';
    return 1;
}

static void set_box(mx_len_t* sides, const char* value, int font_size,
                    int allow_auto) {
    mx_len_t v[4];
    int count = 0;
    char token[96];
    const char* p = value;
    while (count < 4) {
        p = next_token(p, token, sizeof(token));
        if (!token[0]) break;
        if (!parse_length(token, font_size, &v[count])) return;
        if (!allow_auto && v[count].kind == MX_LEN_AUTO) v[count].kind = MX_LEN_SET;
        count++;
    }
    if (!count) return;
    if (count == 1) v[1] = v[2] = v[3] = v[0];
    else if (count == 2) { v[2] = v[0]; v[3] = v[1]; }
    else if (count == 3) v[3] = v[1];
    for (int i = 0; i < 4; i++) sides[i] = v[i];
}

static int border_width_value(const char* token, int font_size, int* out) {
    if (mx_streq_nocase(token, "thin")) { *out = 1; return 1; }
    if (mx_streq_nocase(token, "medium")) { *out = 3; return 1; }
    if (mx_streq_nocase(token, "thick")) { *out = 5; return 1; }
    return parse_px(token, font_size, out);
}

static int border_style_value(const char* token, int* visible) {
    static const char* const visible_styles[] = {
        "solid", "dashed", "dotted", "double", "groove", "ridge", "inset",
        "outset"
    };
    if (mx_streq_nocase(token, "none") || mx_streq_nocase(token, "hidden")) {
        *visible = 0;
        return 1;
    }
    for (size_t i = 0; i < 8; i++)
        if (mx_streq_nocase(token, visible_styles[i])) {
            *visible = 1;
            return 1;
        }
    return 0;
}

/* border / border-<side> shorthand for the sides in `mask` (bit per side). */
static void set_border(mx_style_t* s, const char* value, int mask) {
    int width = 3, visible = -1;
    uint32_t color = COLOR_CURRENT;
    char token[96];
    const char* p = value;
    for (;;) {
        p = next_token(p, token, sizeof(token));
        if (!token[0]) break;
        int w;
        uint32_t c;
        int vis;
        if (border_style_value(token, &vis)) visible = vis;
        else if (border_width_value(token, s->font_size, &w)) width = w;
        else if (mx_css_parse_color(token, &c)) color = c;
    }
    if (visible == -1) visible = 0;  /* border-style defaults to none */
    for (int i = 0; i < 4; i++) {
        if (!(mask & (1 << i))) continue;
        s->border_width[i] = visible ? width : 0;
        s->border_color[i] = color;
    }
}

static uint32_t first_color_in(const char* value, int* found) {
    char token[160];
    const char* p = value;
    *found = 0;
    for (;;) {
        p = next_token(p, token, sizeof(token));
        if (!token[0]) return 0;
        uint32_t c;
        if (mx_starts_with_nocase(token, "linear-gradient(") ||
            mx_starts_with_nocase(token, "radial-gradient(") ||
            mx_starts_with_nocase(token, "repeating-linear-gradient(")) {
            /* use the first color stop */
            const char* inner = strchr(token, '(') + 1;
            char part[96];
            const char* q = inner;
            for (;;) {
                q = next_token(q, part, sizeof(part));
                if (!part[0]) break;
                size_t n = strlen(part);
                if (n && part[n - 1] == ')' && !strchr(part, '(')) part[n - 1] = '\0';
                if (mx_css_parse_color(part, &c)) {
                    *found = 1;
                    return c;
                }
            }
            continue;
        }
        if (mx_css_parse_color(token, &c)) {
            *found = 1;
            return c;
        }
    }
}

static int font_size_value(const char* value, int parent_size, int* out) {
    static const struct { const char* name; int px; } keywords[] = {
        {"xx-small", 9}, {"x-small", 10}, {"small", 13}, {"medium", 16},
        {"large", 18}, {"x-large", 24}, {"xx-large", 32}, {"xxx-large", 48},
    };
    for (size_t i = 0; i < sizeof(keywords) / sizeof(keywords[0]); i++)
        if (is_word(value, keywords[i].name)) {
            *out = keywords[i].px;
            return 1;
        }
    if (is_word(value, "smaller")) { *out = parent_size * 5 / 6; return 1; }
    if (is_word(value, "larger")) { *out = parent_size * 6 / 5; return 1; }
    mx_len_t length;
    if (!parse_length(value, parent_size, &length) || length.kind != MX_LEN_SET)
        return 0;
    int px = length.px + length.pct * parent_size / 10000;
    if (px < 1) px = 1;
    if (px > 200) px = 200;
    *out = px;
    return 1;
}

static int font_weight_value(const char* value, int parent_weight, int* out) {
    if (is_word(value, "bold") || is_word(value, "bolder")) {
        *out = is_word(value, "bolder") ? (parent_weight >= 600 ? 900 : 700) : 700;
        return 1;
    }
    if (is_word(value, "normal")) { *out = 400; return 1; }
    if (is_word(value, "lighter")) { *out = 300; return 1; }
    double n;
    if (parse_number(skip_ws(value), &n) && n >= 1 && n <= 1000) {
        *out = (int)n;
        return 1;
    }
    return 0;
}

static int is_mono_family(const char* value) {
    char lower[160];
    size_t n = 0;
    for (; value[n] && n + 1 < sizeof(lower); n++) lower[n] = mx_lower(value[n]);
    lower[n] = '\0';
    /* the first family decides */
    char* comma = strchr(lower, ',');
    if (comma) *comma = '\0';
    return strstr(lower, "mono") || strstr(lower, "courier") ||
           strstr(lower, "consol") || strstr(lower, "menlo") ||
           strstr(lower, "code") || strstr(lower, "fixed");
}

static int line_height_value(const char* value, int font_size, int* out) {
    if (is_word(value, "normal")) {
        *out = 0;
        return 1;
    }
    double n;
    const char* p = skip_ws(value);
    int used = parse_number(p, &n);
    if (used && (!p[used] || mx_is_space(p[used]))) {
        /* unitless: inherited as a factor, stored as -(factor * 100) */
        *out = n > 0 ? -(int)(n * 100 + 0.5) : 0;
        return 1;
    }
    mx_len_t length;
    if (!parse_length(value, font_size, &length) || length.kind != MX_LEN_SET)
        return 0;
    *out = length.px + length.pct * font_size / 10000;
    return 1;
}

static int keyword_index(const char* value, const char* const* words,
                         int count) {
    for (int i = 0; i < count; i++)
        if (is_word(value, words[i])) return i;
    return -1;
}

static void apply_declaration(apply_ctx_t* ctx, uint16_t property,
                              const char* name, const char* raw_value,
                              int pass) {
    mx_style_t* s = ctx->style;
    const mx_style_t* parent = ctx->parent;
    if (property == P_CUSTOM) {
        if (pass != 0 || !name) return;
        mx_var_t* var = (mx_var_t*)mx_arena_alloc(ctx->arena, sizeof(*var));
        if (!var) return;
        var->name = name;
        var->value = raw_value;
        var->length = (uint32_t)strlen(name);
        var->hash = mx_hash(name, var->length);
        var->next = s->vars;
        s->vars = var;
        return;
    }
    int is_font_pass = property == P_FONT_SIZE || property == P_FONT;
    if ((pass == 1) != is_font_pass || pass == 0) return;

    char expanded[512];
    const char* value = raw_value;
    if (strstr(raw_value, "var(")) {
        if (!substitute_vars(raw_value, s->vars, expanded, sizeof(expanded), 0))
            return;
        value = expanded;
    }
    value = skip_ws(value);
    int inherit = is_word(value, "inherit");
    int fs = s->font_size;
    uint32_t color;
    char token[160];

    switch (property) {
    case P_COLOR:
        if (inherit) s->color = parent->color;
        else if (mx_css_parse_color(value, &color))
            s->color = color == COLOR_CURRENT ? parent->color : color;
        break;
    case P_BACKGROUND_COLOR:
        if (mx_css_parse_color(value, &color))
            s->background = color == COLOR_CURRENT ? s->color : color;
        break;
    case P_BACKGROUND:
    case P_BACKGROUND_IMAGE: {
        if (is_word(value, "none")) {
            if (property == P_BACKGROUND) s->background = 0;
            break;
        }
        int found = 0;
        color = first_color_in(value, &found);
        if (found) s->background = color == COLOR_CURRENT ? s->color : color;
        else if (property == P_BACKGROUND)
            s->background = 0;   /* the shorthand resets the colour too */
        break;
    }
    case P_FONT_SIZE:
        if (inherit) s->font_size = parent->font_size;
        else font_size_value(value, parent->font_size, &s->font_size);
        break;
    case P_FONT: {
        /* [style] [weight] size[/line-height] family */
        const char* p = value;
        for (;;) {
            p = next_token(p, token, sizeof(token));
            if (!token[0]) break;
            int weight;
            if (mx_streq_nocase(token, "italic") || mx_streq_nocase(token, "oblique"))
                s->italic = 1;
            else if (font_weight_value(token, parent->font_weight, &weight) &&
                     !strchr(token, 'p') && !strchr(token, 'e') && !strchr(token, '%'))
                s->font_weight = weight;
            else if ((token[0] >= '0' && token[0] <= '9') || token[0] == '.' ||
                     mx_starts_with_nocase(token, "calc")) {
                char* slash = strchr(token, '/');
                if (slash) *slash = '\0';
                font_size_value(token, parent->font_size, &s->font_size);
                if (slash) line_height_value(slash + 1, s->font_size, &s->line_height);
                s->mono = (uint8_t)is_mono_family(p);
                break;
            } else if (keyword_index(token, (const char* const[]){"small", "medium",
                                     "large", "x-large", "x-small"}, 5) >= 0) {
                font_size_value(token, parent->font_size, &s->font_size);
            }
        }
        break;
    }
    case P_FONT_WEIGHT:
        if (inherit) s->font_weight = parent->font_weight;
        else font_weight_value(value, parent->font_weight, &s->font_weight);
        break;
    case P_FONT_STYLE:
        s->italic = is_word(value, "italic") || is_word(value, "oblique");
        break;
    case P_FONT_FAMILY:
        s->mono = (uint8_t)is_mono_family(value);
        break;
    case P_LINE_HEIGHT:
        if (inherit) s->line_height = parent->line_height;
        else line_height_value(value, fs, &s->line_height);
        break;
    case P_TEXT_ALIGN: {
        static const char* const words[] = {"left", "center", "right", "justify",
                                            "start", "end", "-webkit-center",
                                            "-moz-center", "-webkit-right"};
        int k = keyword_index(value, words, 9);
        if (k >= 0)
            s->text_align = (uint8_t)(k == 4 ? MX_ALIGN_LEFT : k == 5 ? MX_ALIGN_RIGHT :
                                      k == 6 || k == 7 ? MX_ALIGN_LEGACY_CENTER :
                                      k == 8 ? MX_ALIGN_RIGHT : k);
        else if (inherit) s->text_align = parent->text_align;
        break;
    }
    case P_TEXT_DECORATION:
    case P_TEXT_DECORATION_LINE: {
        uint8_t deco = 0;
        const char* p = value;
        int any = 0;
        for (;;) {
            p = next_token(p, token, sizeof(token));
            if (!token[0]) break;
            if (mx_streq_nocase(token, "underline")) deco |= MX_DECO_UNDERLINE, any = 1;
            else if (mx_streq_nocase(token, "line-through")) deco |= MX_DECO_LINE_THROUGH, any = 1;
            else if (mx_streq_nocase(token, "overline")) deco |= MX_DECO_OVERLINE, any = 1;
            else if (mx_streq_nocase(token, "none")) any = 1;
        }
        if (any) s->decoration = deco;
        break;
    }
    case P_TEXT_TRANSFORM: {
        static const char* const words[] = {"none", "uppercase", "lowercase",
                                            "capitalize"};
        int k = keyword_index(value, words, 4);
        if (k >= 0) s->transform = (uint8_t)k;
        break;
    }
    case P_WHITE_SPACE: {
        static const char* const words[] = {"normal", "pre", "nowrap", "pre-wrap",
                                            "pre-line", "break-spaces"};
        int k = keyword_index(value, words, 6);
        if (k >= 0) s->white_space = (uint8_t)(k == 4 ? MX_WS_NORMAL :
                                               k == 5 ? MX_WS_PRE_WRAP : k);
        break;
    }
    case P_LETTER_SPACING:
        if (is_word(value, "normal")) s->letter_spacing = 0;
        else parse_px(value, fs, &s->letter_spacing);
        break;
    case P_TEXT_INDENT:
        parse_px(value, fs, &s->text_indent);
        break;
    case P_DISPLAY: {
        static const char* const words[] = {
            "inline", "block", "inline-block", "list-item", "flex",
            "inline-flex", "grid", "table", "table-row", "table-cell",
            "table-row-group", "table-caption", "none", "contents",
            "flow-root", "inline-grid", "table-header-group",
            "table-footer-group", "inline-table", "-webkit-box", "-ms-flexbox",
            "-webkit-flex", "table-column", "table-column-group", "ruby",
            "inline flex", "block flex"
        };
        int k = keyword_index(value, words, 27);
        static const uint8_t map[] = {
            MX_DISPLAY_INLINE, MX_DISPLAY_BLOCK, MX_DISPLAY_INLINE_BLOCK,
            MX_DISPLAY_LIST_ITEM, MX_DISPLAY_FLEX, MX_DISPLAY_INLINE_FLEX,
            MX_DISPLAY_GRID, MX_DISPLAY_TABLE, MX_DISPLAY_TABLE_ROW,
            MX_DISPLAY_TABLE_CELL, MX_DISPLAY_TABLE_GROUP,
            MX_DISPLAY_TABLE_CAPTION, MX_DISPLAY_NONE, MX_DISPLAY_CONTENTS,
            MX_DISPLAY_BLOCK, MX_DISPLAY_INLINE_BLOCK, MX_DISPLAY_TABLE_GROUP,
            MX_DISPLAY_TABLE_GROUP, MX_DISPLAY_TABLE, MX_DISPLAY_FLEX,
            MX_DISPLAY_FLEX, MX_DISPLAY_FLEX, MX_DISPLAY_NONE, MX_DISPLAY_NONE,
            MX_DISPLAY_INLINE, MX_DISPLAY_INLINE_FLEX, MX_DISPLAY_FLEX
        };
        if (k >= 0) s->display = map[k];
        break;
    }
    case P_VISIBILITY:
        s->visibility_hidden = is_word(value, "hidden") || is_word(value, "collapse");
        break;
    case P_OPACITY: {
        double n;
        int used = parse_number(value, &n);
        if (used) {
            if (value[used] == '%') n /= 100;
            s->opacity = (uint8_t)(clamp01(n) * 255);
        }
        break;
    }
    case P_LIST_STYLE:
    case P_LIST_STYLE_TYPE: {
        static const char* const words[] = {
            "disc", "circle", "square", "decimal", "lower-alpha", "upper-alpha",
            "lower-roman", "upper-roman", "none", "lower-latin", "upper-latin",
            "decimal-leading-zero"
        };
        const char* p = value;
        for (;;) {
            p = next_token(p, token, sizeof(token));
            if (!token[0]) break;
            int k = keyword_index(token, words, 12);
            if (k >= 0) {
                s->list_style = (uint8_t)(k == 9 ? MX_LIST_LOWER_ALPHA :
                                          k == 10 ? MX_LIST_UPPER_ALPHA :
                                          k == 11 ? MX_LIST_DECIMAL : k);
                break;
            }
        }
        break;
    }
    case P_POSITION: {
        static const char* const words[] = {"static", "relative", "absolute",
                                            "fixed", "sticky", "-webkit-sticky"};
        int k = keyword_index(value, words, 6);
        if (k >= 0) s->position = (uint8_t)(k == 5 ? MX_POS_STICKY : k);
        break;
    }
    case P_FLOAT: {
        static const char* const words[] = {"none", "left", "right",
                                            "inline-start", "inline-end"};
        int k = keyword_index(value, words, 5);
        if (k >= 0) s->float_side = (uint8_t)(k == 3 ? 1 : k == 4 ? 2 : k);
        break;
    }
    case P_CLEAR:
        s->clear = !is_word(value, "none");
        break;
    case P_BOX_SIZING:
        s->border_box = is_word(value, "border-box");
        break;
    case P_OVERFLOW:
    case P_OVERFLOW_X:
    case P_OVERFLOW_Y:
        if (is_word(value, "hidden") || is_word(value, "clip") ||
            is_word(value, "auto") || is_word(value, "scroll"))
            s->overflow_hidden = 1;
        else if (is_word(value, "visible"))
            s->overflow_hidden = 0;
        break;
    case P_VERTICAL_ALIGN:
        s->vertical_middle = is_word(value, "middle");
        break;
    case P_CURSOR:
        s->pointer = is_word(value, "pointer");
        break;
    case P_WIDTH: parse_length(value, fs, &s->width); break;
    case P_HEIGHT: parse_length(value, fs, &s->height); break;
    case P_MIN_WIDTH: parse_length(value, fs, &s->min_width); break;
    case P_MAX_WIDTH: parse_length(value, fs, &s->max_width); break;
    case P_MIN_HEIGHT: parse_length(value, fs, &s->min_height); break;
    case P_MAX_HEIGHT: parse_length(value, fs, &s->max_height); break;
    case P_MARGIN: set_box(s->margin, value, fs, 1); break;
    case P_MARGIN_TOP: parse_length(value, fs, &s->margin[0]); break;
    case P_MARGIN_RIGHT: parse_length(value, fs, &s->margin[1]); break;
    case P_MARGIN_BOTTOM: parse_length(value, fs, &s->margin[2]); break;
    case P_MARGIN_LEFT: parse_length(value, fs, &s->margin[3]); break;
    case P_MARGIN_INLINE: {
        mx_len_t pair[4] = {{0}};
        set_box(pair, value, fs, 1);
        if (pair[0].kind) {
            s->margin[3] = pair[0];
            s->margin[1] = pair[1];
        }
        break;
    }
    case P_MARGIN_BLOCK: {
        mx_len_t pair[4] = {{0}};
        set_box(pair, value, fs, 1);
        if (pair[0].kind) {
            s->margin[0] = pair[0];
            s->margin[2] = pair[1];
        }
        break;
    }
    case P_PADDING: set_box(s->padding, value, fs, 0); break;
    case P_PADDING_TOP: parse_length(value, fs, &s->padding[0]); break;
    case P_PADDING_RIGHT: parse_length(value, fs, &s->padding[1]); break;
    case P_PADDING_BOTTOM: parse_length(value, fs, &s->padding[2]); break;
    case P_PADDING_LEFT: parse_length(value, fs, &s->padding[3]); break;
    case P_PADDING_INLINE: {
        mx_len_t pair[4] = {{0}};
        set_box(pair, value, fs, 0);
        if (pair[0].kind) {
            s->padding[3] = pair[0];
            s->padding[1] = pair[1];
        }
        break;
    }
    case P_PADDING_BLOCK: {
        mx_len_t pair[4] = {{0}};
        set_box(pair, value, fs, 0);
        if (pair[0].kind) {
            s->padding[0] = pair[0];
            s->padding[2] = pair[1];
        }
        break;
    }
    case P_BORDER: set_border(s, value, 15); break;
    case P_BORDER_TOP: case P_BORDER_BLOCK_START: set_border(s, value, 1); break;
    case P_BORDER_RIGHT: case P_BORDER_INLINE_END: set_border(s, value, 2); break;
    case P_BORDER_BOTTOM: case P_BORDER_BLOCK_END: set_border(s, value, 4); break;
    case P_BORDER_LEFT: case P_BORDER_INLINE_START: set_border(s, value, 8); break;
    case P_BORDER_WIDTH: {
        int w[4], count = 0;
        const char* p = value;
        while (count < 4) {
            p = next_token(p, token, sizeof(token));
            if (!token[0] || !border_width_value(token, fs, &w[count])) break;
            count++;
        }
        if (!count) break;
        if (count == 1) w[1] = w[2] = w[3] = w[0];
        else if (count == 2) { w[2] = w[0]; w[3] = w[1]; }
        else if (count == 3) w[3] = w[1];
        for (int i = 0; i < 4; i++) s->border_width[i] = w[i];
        break;
    }
    case P_BORDER_STYLE: {
        int visible;
        next_token(value, token, sizeof(token));
        if (border_style_value(token, &visible)) {
            for (int i = 0; i < 4; i++) {
                if (!visible) s->border_width[i] = 0;
                else if (!s->border_width[i]) s->border_width[i] = 3;
            }
        }
        break;
    }
    case P_BORDER_COLOR: {
        uint32_t c[4];
        int count = 0;
        const char* p = value;
        while (count < 4) {
            p = next_token(p, token, sizeof(token));
            if (!token[0] || !mx_css_parse_color(token, &c[count])) break;
            count++;
        }
        if (!count) break;
        if (count == 1) c[1] = c[2] = c[3] = c[0];
        else if (count == 2) { c[2] = c[0]; c[3] = c[1]; }
        else if (count == 3) c[3] = c[1];
        for (int i = 0; i < 4; i++) s->border_color[i] = c[i];
        break;
    }
    case P_BORDER_TOP_WIDTH: border_width_value(value, fs, &s->border_width[0]); break;
    case P_BORDER_RIGHT_WIDTH: border_width_value(value, fs, &s->border_width[1]); break;
    case P_BORDER_BOTTOM_WIDTH: border_width_value(value, fs, &s->border_width[2]); break;
    case P_BORDER_LEFT_WIDTH: border_width_value(value, fs, &s->border_width[3]); break;
    case P_BORDER_TOP_COLOR: mx_css_parse_color(value, &s->border_color[0]); break;
    case P_BORDER_RIGHT_COLOR: mx_css_parse_color(value, &s->border_color[1]); break;
    case P_BORDER_BOTTOM_COLOR: mx_css_parse_color(value, &s->border_color[2]); break;
    case P_BORDER_LEFT_COLOR: mx_css_parse_color(value, &s->border_color[3]); break;
    case P_BORDER_RADIUS: {
        next_token(value, token, sizeof(token));
        mx_len_t length;
        if (parse_length(token, fs, &length) && length.kind == MX_LEN_SET)
            s->radius = length.pct >= 5000 ? 9999 : length.px + length.pct / 100;
        break;
    }
    case P_GAP: {
        const char* p = next_token(value, token, sizeof(token));
        int row;
        if (parse_px(token, fs, &row)) {
            s->row_gap = s->column_gap = row;
            next_token(p, token, sizeof(token));
            int column;
            if (token[0] && parse_px(token, fs, &column)) s->column_gap = column;
        }
        break;
    }
    case P_ROW_GAP: parse_px(value, fs, &s->row_gap); break;
    case P_COLUMN_GAP: parse_px(value, fs, &s->column_gap); break;
    case P_FLEX: {
        if (is_word(value, "none")) {
            s->flex_grow = 0;
            s->flex_shrink = 0;
            break;
        }
        if (is_word(value, "auto")) {
            s->flex_grow = 100;
            s->flex_shrink = 100;
            s->flex_basis.kind = MX_LEN_AUTO;
            break;
        }
        const char* p = value;
        int numbers = 0;
        for (;;) {
            p = next_token(p, token, sizeof(token));
            if (!token[0]) break;
            double n;
            int used = parse_number(token, &n);
            if (used && !token[used] && numbers < 2) {
                if (numbers == 0) {
                    s->flex_grow = (int)(n * 100);
                    s->flex_basis.kind = MX_LEN_SET;
                    s->flex_basis.px = 0;
                    s->flex_basis.pct = 0;
                } else {
                    s->flex_shrink = (int)(n * 100);
                }
                numbers++;
            } else {
                parse_length(token, fs, &s->flex_basis);
            }
        }
        break;
    }
    case P_FLEX_DIRECTION:
    case P_FLEX_FLOW:
    case P_FLEX_WRAP: {
        const char* p = value;
        for (;;) {
            p = next_token(p, token, sizeof(token));
            if (!token[0]) break;
            if (mx_streq_nocase(token, "row")) s->flex_direction = MX_FLEX_ROW;
            else if (mx_streq_nocase(token, "column")) s->flex_direction = MX_FLEX_COLUMN;
            else if (mx_streq_nocase(token, "row-reverse"))
                s->flex_direction = MX_FLEX_ROW_REVERSE;
            else if (mx_streq_nocase(token, "column-reverse"))
                s->flex_direction = MX_FLEX_COLUMN_REVERSE;
            else if (mx_streq_nocase(token, "wrap") ||
                     mx_streq_nocase(token, "wrap-reverse"))
                s->flex_wrap = 1;
            else if (mx_streq_nocase(token, "nowrap")) s->flex_wrap = 0;
        }
        break;
    }
    case P_FLEX_GROW: {
        double n;
        if (parse_number(value, &n)) s->flex_grow = (int)(n * 100);
        break;
    }
    case P_FLEX_SHRINK: {
        double n;
        if (parse_number(value, &n)) s->flex_shrink = (int)(n * 100);
        break;
    }
    case P_FLEX_BASIS: parse_length(value, fs, &s->flex_basis); break;
    case P_JUSTIFY_CONTENT: {
        static const char* const words[] = {
            "flex-start", "center", "flex-end", "space-between", "space-around",
            "space-evenly", "start", "end", "left", "right", "normal", "stretch"
        };
        int k = keyword_index(value, words, 12);
        static const uint8_t map[] = {
            MX_JUSTIFY_START, MX_JUSTIFY_CENTER, MX_JUSTIFY_END,
            MX_JUSTIFY_BETWEEN, MX_JUSTIFY_AROUND, MX_JUSTIFY_EVENLY,
            MX_JUSTIFY_START, MX_JUSTIFY_END, MX_JUSTIFY_START, MX_JUSTIFY_END,
            MX_JUSTIFY_START, MX_JUSTIFY_START
        };
        if (k >= 0) s->justify = map[k];
        break;
    }
    case P_ALIGN_ITEMS:
    case P_PLACE_ITEMS: {
        static const char* const words[] = {
            "stretch", "flex-start", "center", "flex-end", "baseline", "start",
            "end", "normal", "self-start", "self-end"
        };
        int k = keyword_index(value, words, 10);
        static const uint8_t map[] = {
            MX_ITEMS_STRETCH, MX_ITEMS_START, MX_ITEMS_CENTER, MX_ITEMS_END,
            MX_ITEMS_BASELINE, MX_ITEMS_START, MX_ITEMS_END, MX_ITEMS_STRETCH,
            MX_ITEMS_START, MX_ITEMS_END
        };
        if (k >= 0) s->align_items = map[k];
        break;
    }
    case P_GRID_TEMPLATE_COLUMNS: {
        if (mx_starts_with_nocase(value, "repeat(")) {
            const char* p = value + 7;
            p = skip_ws(p);
            double n;
            int used = parse_number(p, &n);
            if (used) {
                s->grid_columns = (int)n;
            } else {
                /* repeat(auto-fill|auto-fit, minmax(Npx, 1fr)) */
                const char* mm = strstr(p, "minmax(");
                int min_px = 200;
                if (mm) {
                    char first[48];
                    next_token(mm + 7, first, sizeof(first));
                    size_t fn = strlen(first);
                    if (fn && first[fn - 1] == ',') first[fn - 1] = '\0';
                    parse_px(first, fs, &min_px);
                    if (min_px < 20) min_px = 20;
                } else {
                    const char* comma = strchr(p, ',');
                    if (comma) {
                        char width[48];
                        next_token(comma + 1, width, sizeof(width));
                        parse_px(width, fs, &min_px);
                    }
                }
                s->grid_columns = -min_px;
            }
        } else if (!is_word(value, "none")) {
            int count = 0;
            const char* p = value;
            for (;;) {
                p = next_token(p, token, sizeof(token));
                if (!token[0]) break;
                if (token[0] != '[') count++;
            }
            s->grid_columns = count;
        }
        break;
    }
    case P_TOP: parse_length(value, fs, &s->offset[0]); break;
    case P_RIGHT: parse_length(value, fs, &s->offset[1]); break;
    case P_BOTTOM: parse_length(value, fs, &s->offset[2]); break;
    case P_LEFT: parse_length(value, fs, &s->offset[3]); break;
    case P_INSET: set_box(s->offset, value, fs, 1); break;
    case P_Z_INDEX: {
        double n;
        if (parse_number(value, &n)) s->z_index = (int)n;
        break;
    }
    case P_COLOR_SCHEME:
        s->scheme_dark = strstr(value, "dark") != NULL;
        break;
    case P_ORDER: {
        double n;
        if (parse_number(value, &n)) s->order = (int)n;
        break;
    }
    default:
        break;
    }
}

void mx_css_initial(mx_style_t* s) {
    memset(s, 0, sizeof(*s));
    s->color = 0xFF000000u;
    s->font_size = 16;
    s->font_weight = 400;
    s->opacity = 255;
    s->flex_shrink = 100;
    for (int i = 0; i < 4; i++) s->border_color[i] = COLOR_CURRENT;
}

typedef struct {
    const css_rule_t* rule;
} matched_t;

static int matched_before(const css_rule_t* a, const css_rule_t* b) {
    if (a->origin != b->origin) return a->origin < b->origin;
    if (a->selector.specificity != b->selector.specificity)
        return a->selector.specificity < b->selector.specificity;
    return a->order < b->order;
}

static int bloom_rejects(const css_selector_t* s) {
    if (!bloom_active) return 0;
    for (int i = 0; i < s->ancestor_count; i++)
        if (!bloom[s->ancestor_hash[i]]) return 1;
    return 0;
}

static void bloom_update(mx_node_t* e, int delta) {
    const char* id = mx_dom_attr(e, "id");
    uint16_t h = (uint16_t)(mx_hash(e->tag, strlen(e->tag)) & 0xFFFFu);
    bloom[h] = (uint8_t)(bloom[h] + delta);
    if (id && *id) {
        h = (uint16_t)(mx_hash(id, strlen(id)) & 0xFFFFu);
        bloom[h] = (uint8_t)(bloom[h] + delta);
    }
    const char* classes = mx_dom_attr(e, "class");
    for (const char* p = classes ? classes : ""; *p;) {
        while (mx_is_space(*p)) p++;
        const char* end = p;
        while (*end && !mx_is_space(*end)) end++;
        if (end > p) {
            h = (uint16_t)(mx_hash(p, (size_t)(end - p)) & 0xFFFFu);
            bloom[h] = (uint8_t)(bloom[h] + delta);
        }
        p = end;
    }
}

void mx_css_push_ancestor(mx_node_t* element) {
    if (element->type == MX_NODE_ELEMENT) bloom_update(element, 1);
}

void mx_css_pop_ancestor(mx_node_t* element) {
    if (element->type == MX_NODE_ELEMENT) bloom_update(element, -1);
}

void mx_css_bloom(int active) {
    bloom_active = active;
    if (!active) memset(bloom, 0, sizeof(bloom));
}

static int collect_bucket(css_bucket_t* entry, mx_node_t* e, matched_t* out,
                          int count, int capacity) {
    for (; entry && count < capacity; entry = entry->next) {
        const css_rule_t* rule = &rules[entry->rule];
        if (bloom_rejects(&rule->selector)) continue;
        if (selector_matches_at(&rule->selector, rule->selector.count - 1, e, 0))
            out[count++].rule = rule;
    }
    return count;
}

static int collect_matches(mx_node_t* e, matched_t* out, int capacity) {
    int count = 0;
    const char* id = mx_dom_attr(e, "id");
    if (id && *id)
        count = collect_bucket(id_table[mx_hash(id, strlen(id)) % CSS_TABLE], e,
                               out, count, capacity);
    const char* classes = mx_dom_attr(e, "class");
    if (classes) {
        const char* p = classes;
        uint32_t seen[16];
        int seen_count = 0;
        while (*p) {
            while (mx_is_space(*p)) p++;
            const char* end = p;
            while (*end && !mx_is_space(*end)) end++;
            if (end > p) {
                uint32_t h = mx_hash(p, (size_t)(end - p)) % CSS_TABLE;
                int duplicate = 0;
                for (int i = 0; i < seen_count; i++) duplicate |= seen[i] == h;
                if (!duplicate) {
                    if (seen_count < 16) seen[seen_count++] = h;
                    count = collect_bucket(class_table[h], e, out, count, capacity);
                }
            }
            p = end;
        }
    }
    count = collect_bucket(tag_table[mx_hash(e->tag, strlen(e->tag)) % CSS_TABLE],
                           e, out, count, capacity);
    {
        uint32_t seen[16];
        int seen_count = 0;
        for (mx_attr_t* a = e->attrs; a; a = a->next) {
            uint32_t h = mx_hash(a->name, strlen(a->name)) % CSS_TABLE;
            int duplicate = 0;
            for (int i = 0; i < seen_count; i++) duplicate |= seen[i] == h;
            if (duplicate || !attr_table[h]) continue;
            if (seen_count < 16) seen[seen_count++] = h;
            count = collect_bucket(attr_table[h], e, out, count, capacity);
        }
    }
    count = collect_bucket(universal_rules, e, out, count, capacity);
    /* bucket collisions can produce duplicates: drop them */
    for (int i = 0; i < count; i++)
        for (int j = i + 1; j < count; j++)
            if (out[j].rule == out[i].rule) out[j--] = out[--count];
    /* insertion sort by cascade order */
    for (int i = 1; i < count; i++) {
        matched_t m = out[i];
        int j = i - 1;
        while (j >= 0 && matched_before(m.rule, out[j].rule)) {
            out[j + 1] = out[j];
            j--;
        }
        out[j + 1] = m;
    }
    return count;
}

static void apply_hints(apply_ctx_t* ctx, int pass) {
    mx_node_t* e = ctx->element;
    mx_style_t* s = ctx->style;
    const char* v;
    uint32_t color;
    if (pass == 1) {
        if (!strcmp(e->tag, "font") && (v = mx_dom_attr(e, "size"))) {
            static const int sizes[] = {10, 13, 16, 18, 24, 32, 48};
            int value = 0, sign = 0;
            const char* p = v;
            if (*p == '+' || *p == '-') sign = *p++ == '-' ? -1 : 1;
            while (*p >= '0' && *p <= '9') value = value * 10 + (*p++ - '0');
            if (sign) value = 3 + sign * value;
            if (value < 1) value = 1;
            if (value > 7) value = 7;
            s->font_size = sizes[value - 1];
        }
        return;
    }
    if (pass != 2) return;
    if ((v = mx_dom_attr(e, "bgcolor")) && mx_css_parse_color(v, &color))
        s->background = color;
    if (!strcmp(e->tag, "font") && (v = mx_dom_attr(e, "color")) &&
        mx_css_parse_color(v, &color))
        s->color = color;
    if (!strcmp(e->tag, "body") && (v = mx_dom_attr(e, "text")) &&
        mx_css_parse_color(v, &color))
        s->color = color;
    if ((v = mx_dom_attr(e, "align"))) {
        if (mx_streq_nocase(v, "center")) {
            if (!strcmp(e->tag, "table")) {
                s->margin[1].kind = s->margin[3].kind = MX_LEN_AUTO;
            } else {
                s->text_align = !strcmp(e->tag, "td") || !strcmp(e->tag, "th") ||
                                !strcmp(e->tag, "tr")
                                    ? MX_ALIGN_CENTER : MX_ALIGN_LEGACY_CENTER;
            }
        } else if (mx_streq_nocase(v, "right")) {
            s->text_align = MX_ALIGN_RIGHT;
        }
    }
    if (!strcmp(e->tag, "center")) s->text_align = MX_ALIGN_LEGACY_CENTER;
    if ((v = mx_dom_attr(e, "valign")))
        s->vertical_middle = mx_streq_nocase(v, "middle") || mx_streq_nocase(v, "center");
    if (strcmp(e->tag, "input") && (v = mx_dom_attr(e, "width")))
        parse_length(v, s->font_size, &s->width);
    if (strcmp(e->tag, "input") && (v = mx_dom_attr(e, "height")))
        parse_length(v, s->font_size, &s->height);
    if (!strcmp(e->tag, "table") && (v = mx_dom_attr(e, "border")) &&
        *v && *v != '0') {
        for (int i = 0; i < 4; i++) {
            s->border_width[i] = 1;
            s->border_color[i] = 0xFF808080u;
        }
    }
    if (mx_dom_attr(e, "hidden")) s->display = MX_DISPLAY_NONE;
}

void mx_css_compute(mx_arena_t* arena, mx_node_t* e, const mx_style_t* parent,
                    mx_style_t* s) {
    mx_css_initial(s);
    /* inherited properties */
    s->color = parent->color;
    s->font_size = parent->font_size;
    s->font_weight = parent->font_weight;
    s->italic = parent->italic;
    s->mono = parent->mono;
    s->line_height = parent->line_height;
    s->letter_spacing = parent->letter_spacing;
    s->text_indent = parent->text_indent;
    s->text_align = parent->text_align;
    s->white_space = parent->white_space;
    s->transform = parent->transform;
    s->visibility_hidden = parent->visibility_hidden;
    s->list_style = parent->list_style;
    s->pointer = parent->pointer;
    s->decoration = parent->decoration;
    s->vars = parent->vars;
    s->display = MX_DISPLAY_INLINE;
    /* tables do not inherit legacy centering */
    if (!strcmp(e->tag, "table") && s->text_align == MX_ALIGN_LEGACY_CENTER)
        s->text_align = MX_ALIGN_LEFT;

    matched_t matched[192];
    int count = collect_matches(e, matched, 192);

    uint16_t inline_count = 0;
    css_decl_t* inline_decls = NULL;
    const char* inline_style = mx_dom_attr(e, "style");
    if (inline_style && *inline_style) {
        if (e->css_inline_source != inline_style ||
            e->css_inline_generation != css_generation) {
            e->css_inline = parse_declarations(inline_style, strlen(inline_style),
                                               &e->css_inline_count);
            e->css_inline_source = inline_style;
            e->css_inline_generation = css_generation;
        }
        inline_decls = (css_decl_t*)e->css_inline;
        inline_count = e->css_inline_count;
    }

    apply_ctx_t ctx = {s, parent, arena, e};
    for (int pass = 0; pass < 3; pass++) {
        if (pass == 1 || pass == 2) apply_hints(&ctx, pass);
        for (int important = 0; important < 2; important++) {
            for (int i = 0; i < count; i++) {
                const css_rule_t* rule = matched[i].rule;
                for (uint16_t d = 0; d < rule->decl_count; d++) {
                    const css_decl_t* decl = &rule->decls[d];
                    if (decl->important != important) continue;
                    apply_declaration(&ctx, decl->property, decl->name,
                                      decl->value, pass);
                }
            }
            for (uint16_t d = 0; d < inline_count; d++) {
                const css_decl_t* decl = &inline_decls[d];
                if (decl->important != important) continue;
                apply_declaration(&ctx, decl->property, decl->name, decl->value,
                                  pass);
            }
        }
    }
    if (!strcmp(e->tag, "html")) {
        dark_page = s->scheme_dark && env.dark;
        if (dark_page && s->color == 0xFF000000u) s->color = 0xFFE3E3E3u;
    }
    for (int i = 0; i < 4; i++)
        if (s->border_color[i] == COLOR_CURRENT) s->border_color[i] = s->color;
    if (s->font_weight < 100) s->font_weight = 400;
}

int mx_css_dark_page(void) {
    return dark_page;
}

size_t mx_css_describe_rules(mx_node_t* e, char* out, size_t capacity) {
    if (!capacity) return 0;
    out[0] = '\0';
    if (!e || e->type != MX_NODE_ELEMENT) return 0;
    matched_t matched[64];
    int count = collect_matches(e, matched, 64);
    for (int i = count - 1; i >= 0; i--) {
        const css_rule_t* rule = matched[i].rule;
        if (rule->origin == 0) continue;
        mx_append(out, capacity, rule->selector_text ? rule->selector_text : "?");
        mx_append(out, capacity, " {\n");
        for (uint16_t d = 0; d < rule->decl_count; d++) {
            const css_decl_t* decl = &rule->decls[d];
            mx_append(out, capacity, "  ");
            mx_append(out, capacity, decl->name ? decl->name
                                                : property_names[decl->property]);
            mx_append(out, capacity, ": ");
            mx_append(out, capacity, decl->value);
            if (decl->important) mx_append(out, capacity, " !important");
            mx_append(out, capacity, ";\n");
        }
        mx_append(out, capacity, "}\n");
    }
    return strlen(out);
}

/* ====================================================================== */
/* Browser default styles                                                 */
/* ====================================================================== */

const char mx_css_user_agent[] =
    "html,address,blockquote,body,center,dialog,div,figure,figcaption,footer,"
    "form,header,hr,legend,listing,main,p,plaintext,pre,search,xmp,article,"
    "aside,h1,h2,h3,h4,h5,h6,hgroup,nav,section,dir,dd,dl,dt,menu,ol,ul,"
    "fieldset,details,summary,optgroup{display:block}"
    "li{display:list-item}"
    "head,script,style,title,meta,link,base,template,noscript,datalist,"
    "param,source,track,area,map,iframe,noembed,noframes,dialog:not([open]),"
    "[hidden],input[type=hidden],svg,math,canvas,audio,object,embed,"
    "select option,frame,frameset{display:none}"
    "body{margin:8px;line-height:1.25}"
    "p,dl,figure,blockquote{margin-top:1em;margin-bottom:1em}"
    "blockquote,figure{margin-left:40px;margin-right:40px}"
    "blockquote{padding-left:12px;border-left:3px solid #c8ced6}"
    "h1{font-size:2em;margin-top:.67em;margin-bottom:.67em;font-weight:bold}"
    "h2{font-size:1.5em;margin-top:.83em;margin-bottom:.83em;font-weight:bold}"
    "h3{font-size:1.17em;margin-top:1em;margin-bottom:1em;font-weight:bold}"
    "h4{margin-top:1.33em;margin-bottom:1.33em;font-weight:bold}"
    "h5{font-size:.83em;margin-top:1.67em;margin-bottom:1.67em;font-weight:bold}"
    "h6{font-size:.67em;margin-top:2.33em;margin-bottom:2.33em;font-weight:bold}"
    "ul,menu,dir,ol{margin-top:1em;margin-bottom:1em;padding-left:40px}"
    "ol{list-style-type:decimal}"
    "ul ul,ol ul{list-style-type:circle}"
    "ul ul ul,ol ul ul{list-style-type:square}"
    "li ul,li ol,ul ul,ul ol,ol ol,ol ul{margin-top:0;margin-bottom:0}"
    "dd{margin-left:40px}"
    "dt{font-weight:bold}"
    "hr{border:1px inset #c0c4cc;margin:8px 0;height:0}"
    "pre,xmp,listing,plaintext{white-space:pre;margin-top:1em;margin-bottom:1em}"
    "pre,code,kbd,samp,tt,var{font-family:monospace}"
    "pre,code,kbd,samp,tt{font-size:.9em}"
    "b,strong,th{font-weight:bold}"
    "i,em,cite,var,dfn,address{font-style:italic}"
    "u,ins{text-decoration:underline}"
    "s,strike,del{text-decoration:line-through}"
    "small{font-size:smaller}big{font-size:larger}"
    "sub,sup{font-size:smaller}"
    "mark{background-color:#ffe066;color:#000}"
    "a:link{color:#1a5fd6;text-decoration:underline;cursor:pointer}"
    "center{text-align:-webkit-center}"
    "table{display:table;border-spacing:2px}"
    "caption{display:table-caption;text-align:center}"
    "thead,tbody,tfoot{display:table-row-group}"
    "tr{display:table-row}"
    "td,th{display:table-cell;padding:2px 4px;vertical-align:middle}"
    "th{text-align:center}"
    "img,video{display:inline-block}"
    "input,button,select,textarea{display:inline-block;font-size:13px;"
    "line-height:normal}"
    "button,input[type=submit],input[type=button],input[type=reset]{"
    "padding:5px 14px;border:1px solid #b8c1cc;border-radius:6px;box-sizing:border-box;"
    "background-color:#eef1f5;color:#1f2933;cursor:pointer}"
    "input,select,textarea{border:1px solid #b8c1cc;border-radius:4px;"
    "background-color:#fff;color:#1f2933;padding:4px 6px}"
    "input[type=checkbox],input[type=radio]{border:0;padding:0;"
    "background-color:transparent;margin:3px 4px}"
    "textarea{white-space:pre-wrap}"
    "fieldset{margin:0 2px;padding:6px 10px 10px;border:2px groove #c0c4cc}"
    "legend{padding:0 2px;font-weight:bold}"
    "summary{display:list-item;cursor:pointer;font-weight:bold}"
    "details>*{display:none}details>summary{display:list-item}"
    "details[open]>*{display:block}details[open]>summary{display:list-item}"
    "abbr[title]{text-decoration:underline}"
    "q{font-style:italic}"
    "label{cursor:default}";

const size_t mx_css_user_agent_length = sizeof(mx_css_user_agent) - 1;

#ifdef MX_HOST
#include <stdio.h>
void css_debug_stats(void);
void css_debug_stats(void) {
    int u = 0;
    for (css_bucket_t* b = universal_rules; b; b = b->next) {
        if (u < 15) fprintf(stderr, "  universal: %s\n", rules[b->rule].selector_text);
        u++;
    }
    fprintf(stderr, "rules %u universal %d\n", rule_count, u);
    css_bucket_t** tables[3] = {class_table, tag_table, attr_table};
    const char* names[3] = {"class", "tag", "attr"};
    for (int t = 0; t < 3; t++) {
        int best = 0, best_i = 0;
        for (unsigned i = 0; i < CSS_TABLE; i++) {
            int c = 0;
            for (css_bucket_t* b = tables[t][i]; b; b = b->next) c++;
            if (c > best) { best = c; best_i = (int)i; }
        }
        fprintf(stderr, "largest %s bucket: %d (e.g. %s)\n", names[t], best,
                tables[t][best_i] ? rules[tables[t][best_i]->rule].selector_text : "-");
    }
}
#endif
