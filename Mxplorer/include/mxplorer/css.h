#ifndef MXPLORER_CSS_H
#define MXPLORER_CSS_H

#include <stddef.h>
#include <stdint.h>
#include "mxplorer/dom.h"

/* A length: px + pct/100 percent of the reference size. */
typedef struct {
    int px;
    int pct;          /* percent * 100 */
    uint8_t kind;     /* MX_LEN_NONE / MX_LEN_SET / MX_LEN_AUTO */
} mx_len_t;

enum { MX_LEN_NONE = 0, MX_LEN_SET = 1, MX_LEN_AUTO = 2 };

enum {
    MX_DISPLAY_INLINE, MX_DISPLAY_BLOCK, MX_DISPLAY_INLINE_BLOCK,
    MX_DISPLAY_LIST_ITEM, MX_DISPLAY_FLEX, MX_DISPLAY_INLINE_FLEX,
    MX_DISPLAY_GRID, MX_DISPLAY_TABLE, MX_DISPLAY_TABLE_ROW,
    MX_DISPLAY_TABLE_CELL, MX_DISPLAY_TABLE_GROUP, MX_DISPLAY_TABLE_CAPTION,
    MX_DISPLAY_NONE, MX_DISPLAY_CONTENTS
};
/* MX_ALIGN_LEGACY_CENTER is <center>/align=center: it also centres child
 * blocks and does not reach into tables. */
enum { MX_ALIGN_LEFT, MX_ALIGN_CENTER, MX_ALIGN_RIGHT, MX_ALIGN_JUSTIFY,
       MX_ALIGN_LEGACY_CENTER };
enum { MX_WS_NORMAL, MX_WS_PRE, MX_WS_NOWRAP, MX_WS_PRE_WRAP };
enum { MX_DECO_UNDERLINE = 1, MX_DECO_LINE_THROUGH = 2, MX_DECO_OVERLINE = 4 };
enum { MX_TT_NONE, MX_TT_UPPER, MX_TT_LOWER, MX_TT_CAPITALIZE };
enum {
    MX_LIST_DISC, MX_LIST_CIRCLE, MX_LIST_SQUARE, MX_LIST_DECIMAL,
    MX_LIST_LOWER_ALPHA, MX_LIST_UPPER_ALPHA, MX_LIST_LOWER_ROMAN,
    MX_LIST_UPPER_ROMAN, MX_LIST_NONE
};
enum { MX_POS_STATIC, MX_POS_RELATIVE, MX_POS_ABSOLUTE, MX_POS_FIXED,
       MX_POS_STICKY };
enum { MX_FLEX_ROW, MX_FLEX_COLUMN, MX_FLEX_ROW_REVERSE,
       MX_FLEX_COLUMN_REVERSE };
enum { MX_JUSTIFY_START, MX_JUSTIFY_CENTER, MX_JUSTIFY_END,
       MX_JUSTIFY_BETWEEN, MX_JUSTIFY_AROUND, MX_JUSTIFY_EVENLY };
enum { MX_ITEMS_STRETCH, MX_ITEMS_START, MX_ITEMS_CENTER, MX_ITEMS_END,
       MX_ITEMS_BASELINE };
enum { MX_FLOAT_NONE, MX_FLOAT_LEFT, MX_FLOAT_RIGHT };

typedef struct mx_var {
    const char* name;
    const char* value;
    struct mx_var* next;
    uint32_t hash;
    uint32_t length;
} mx_var_t;

typedef struct mx_style {
    uint32_t color;
    uint32_t background;          /* alpha 0: transparent */
    int font_size;                /* px */
    int font_weight;              /* 100..900 */
    int line_height;              /* px; 0: normal */
    int letter_spacing;
    int text_indent;
    uint8_t italic;
    uint8_t mono;
    uint8_t display;
    uint8_t text_align;
    uint8_t white_space;
    uint8_t decoration;
    uint8_t transform;
    uint8_t visibility_hidden;
    uint8_t list_style;
    uint8_t position;
    uint8_t float_side;
    uint8_t border_box;
    uint8_t flex_direction;
    uint8_t flex_wrap;
    uint8_t justify;
    uint8_t align_items;
    uint8_t overflow_hidden;
    uint8_t vertical_middle;
    uint8_t opacity;              /* 0..255 */
    uint8_t pointer;
    uint8_t clear;
    uint8_t scheme_dark;          /* color-scheme allows dark */
    mx_len_t width, height, min_width, max_width, min_height, max_height;
    mx_len_t margin[4];           /* top right bottom left */
    mx_len_t padding[4];
    mx_len_t offset[4];           /* top right bottom left (positioning) */
    int border_width[4];
    uint32_t border_color[4];
    int radius;
    int row_gap;
    int column_gap;
    int grid_columns;             /* repeat count of grid-template-columns */
    int flex_grow;                /* * 100 */
    int flex_shrink;              /* * 100 */
    mx_len_t flex_basis;
    int z_index;
    int order;
    mx_var_t* vars;               /* custom properties (inherited) */
} mx_style_t;

typedef struct {
    int viewport_width;
    int viewport_height;
    int dark;                     /* prefers-color-scheme: dark */
} mx_css_env_t;

void mx_css_reset(void);
void mx_css_set_env(const mx_css_env_t* env);
/* origin 0: browser defaults, 1: page styles. */
void mx_css_add_stylesheet(const char* source, size_t length, int origin);
unsigned mx_css_rule_count(void);
void mx_css_initial(mx_style_t* style);
/* Custom-property lists are allocated from `arena`. */
void mx_css_compute(mx_arena_t* arena, mx_node_t* element,
                    const mx_style_t* parent, mx_style_t* out);
/* Ancestor filter for fast descendant-selector rejection while styling a
 * tree top-down: bloom(1), push each element before styling its children,
 * pop after, bloom(0) when done. */
void mx_css_bloom(int active);
void mx_css_push_ancestor(mx_node_t* element);
void mx_css_pop_ancestor(mx_node_t* element);
/* 1 when the page's root opted into dark colors and dark mode is on. */
int mx_css_dark_page(void);
/* 1 when the element matches the selector list; -1 on a syntax error. */
int mx_css_matches(mx_node_t* element, const char* selectors);
int mx_css_parse_color(const char* value, uint32_t* color);
int mx_css_media_matches(const char* query);
int mx_len_resolve(mx_len_t length, int reference);
/* Describes the declarations that matched `element` (devtools). */
size_t mx_css_describe_rules(mx_node_t* element, char* out, size_t capacity);

extern const char mx_css_user_agent[];
extern const size_t mx_css_user_agent_length;

#endif
