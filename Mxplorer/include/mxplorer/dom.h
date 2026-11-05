#ifndef MXPLORER_DOM_H
#define MXPLORER_DOM_H

#include <stddef.h>
#include <stdint.h>
#include "mxplorer/util.h"

typedef enum {
    MX_NODE_DOCUMENT,
    MX_NODE_ELEMENT,
    MX_NODE_TEXT,
    MX_NODE_COMMENT
} mx_node_type_t;

typedef struct mx_attr {
    char* name;
    char* value;
    struct mx_attr* next;
} mx_attr_t;

struct mx_style;
struct mx_listener;

typedef struct mx_class_token {
    uint32_t hash;
    uint16_t offset;
    uint16_t length;
} mx_class_token_t;

typedef struct mx_node {
    uint8_t type;
    uint8_t dirty_style;
    uint8_t checked;        /* checkbox / radio state, <details open> */
    uint8_t devtools_open;  /* expanded in the Elements tree */
    uint32_t serial;        /* creation order, for devtools */
    char* tag;              /* lower-case, elements only */
    char* text;             /* text and comment nodes */
    size_t text_length;
    char* value;            /* form control value (NULL: use attribute) */
    mx_attr_t* attrs;
    struct mx_node* parent;
    struct mx_node* first;
    struct mx_node* last;
    struct mx_node* prev;
    struct mx_node* next;
    struct mx_style* style;           /* computed style (layout pass) */
    struct mx_listener* listeners;    /* JavaScript event listeners */
    void* js_wrapper;                 /* cached JavaScript object */
    void* css_inline;                 /* parsed style="" declarations */
    const char* css_inline_source;    /* attribute value they came from */
    uint16_t css_inline_count;
    uint32_t css_inline_generation;   /* mx_css_reset() count they belong to */
    uint16_t class_count;             /* cached tokens of class="" */
    const char* class_source;
    struct mx_class_token* classes;
    int box_x, box_y, box_w, box_h;   /* border box from the last layout */
    int content_w, content_h;
    uint32_t layout_gen;              /* layout pass that set the box */
    uint32_t measure_gen;             /* layout pass of the cached widths */
    int measure_min, measure_max;     /* min/max-content widths */
    int layout_item;                  /* background item, -1 if none */
} mx_node_t;

typedef struct {
    mx_arena_t arena;
    mx_node_t* root;          /* document node */
    mx_node_t* html;
    mx_node_t* head;
    mx_node_t* body;
    char title[128];
    char base_url[512];
    uint32_t next_serial;
    unsigned element_count;
    int mutated;              /* set by scripts; layout must rerun */
} mx_document_t;

void mx_dom_init(mx_document_t* doc);
void mx_dom_free(mx_document_t* doc);
/* Parses a whole document. */
void mx_dom_parse(mx_document_t* doc, const char* source, size_t length);
/* Parses `source` as children of `parent` (innerHTML). */
void mx_dom_parse_fragment(mx_document_t* doc, mx_node_t* parent,
                           const char* source, size_t length);

mx_node_t* mx_dom_create_element(mx_document_t* doc, const char* tag);
mx_node_t* mx_dom_create_text(mx_document_t* doc, const char* text,
                              size_t length);
void mx_dom_append(mx_node_t* parent, mx_node_t* child);
void mx_dom_insert_before(mx_node_t* parent, mx_node_t* child,
                          mx_node_t* reference);
void mx_dom_remove(mx_node_t* child);
void mx_dom_remove_children(mx_node_t* node);
mx_node_t* mx_dom_clone(mx_document_t* doc, mx_node_t* node, int deep);

const char* mx_dom_attr(const mx_node_t* node, const char* name);
void mx_dom_set_attr(mx_document_t* doc, mx_node_t* node, const char* name,
                     const char* value);
void mx_dom_remove_attr(mx_node_t* node, const char* name);
int mx_dom_has_class(const mx_node_t* node, const char* name);
/* Same with a precomputed mx_hash(name). */
int mx_dom_has_class_hashed(mx_node_t* node, const char* name, size_t length,
                            uint32_t hash);
/* Needs the document to cache the class tokens. */
void mx_dom_set_class_arena(mx_arena_t* arena);
int mx_dom_is(const mx_node_t* node, const char* tag);

/* Text content of a subtree, written to an arena string. */
char* mx_dom_text_content(mx_document_t* doc, const mx_node_t* node);
void mx_dom_set_text_content(mx_document_t* doc, mx_node_t* node,
                             const char* text);
/* Serialises children (inner) or the node itself (outer) as HTML. */
char* mx_dom_serialize(mx_document_t* doc, const mx_node_t* node, int outer);

mx_node_t* mx_dom_by_id(mx_node_t* root, const char* id);
/* Next node in document order inside `root` (NULL at the end). */
mx_node_t* mx_dom_next(const mx_node_t* root, const mx_node_t* node);
mx_node_t* mx_dom_first_element_child(const mx_node_t* node);
mx_node_t* mx_dom_next_element(const mx_node_t* node);
mx_node_t* mx_dom_prev_element(const mx_node_t* node);

int mx_dom_is_void(const char* tag);
/* Decodes HTML character references into `out` (UTF-8). */
size_t mx_html_decode(char* out, size_t capacity, const char* source,
                      size_t length);

#endif
