#include "mxplorer/dom.h"
#include "string.h"

/* ---------------------------------------------------------------------- */
/* Nodes                                                                  */
/* ---------------------------------------------------------------------- */

static mx_node_t* new_node(mx_document_t* doc, mx_node_type_t type) {
    mx_node_t* node = (mx_node_t*)mx_arena_zalloc(&doc->arena, sizeof(*node));
    if (!node) return NULL;
    node->type = (uint8_t)type;
    node->serial = doc->next_serial++;
    node->dirty_style = 1;
    return node;
}

static char* intern_tag(mx_document_t* doc, const char* tag, size_t length) {
    char* copy = mx_arena_strndup(&doc->arena, tag, length);
    if (!copy) return NULL;
    for (char* p = copy; *p; p++) *p = mx_lower(*p);
    return copy;
}

mx_node_t* mx_dom_create_element(mx_document_t* doc, const char* tag) {
    mx_node_t* node = new_node(doc, MX_NODE_ELEMENT);
    if (!node) return NULL;
    node->tag = intern_tag(doc, tag, strlen(tag));
    if (!node->tag) node->tag = (char*)"span";
    doc->element_count++;
    return node;
}

mx_node_t* mx_dom_create_text(mx_document_t* doc, const char* text,
                              size_t length) {
    mx_node_t* node = new_node(doc, MX_NODE_TEXT);
    if (!node) return NULL;
    node->text = mx_arena_strndup(&doc->arena, text ? text : "",
                                  text ? length : 0);
    node->text_length = node->text ? length : 0;
    if (!node->text) node->text = (char*)"";
    return node;
}

void mx_dom_remove(mx_node_t* child) {
    mx_node_t* parent = child->parent;
    if (!parent) return;
    if (child->prev) child->prev->next = child->next;
    else parent->first = child->next;
    if (child->next) child->next->prev = child->prev;
    else parent->last = child->prev;
    child->parent = child->prev = child->next = NULL;
}

void mx_dom_insert_before(mx_node_t* parent, mx_node_t* child,
                          mx_node_t* reference) {
    if (!parent || !child || child == parent) return;
    for (mx_node_t* p = parent; p; p = p->parent)
        if (p == child) return;  /* would create a cycle */
    if (child->parent) mx_dom_remove(child);
    if (reference && reference->parent != parent) reference = NULL;
    child->parent = parent;
    if (!reference) {
        child->prev = parent->last;
        child->next = NULL;
        if (parent->last) parent->last->next = child;
        else parent->first = child;
        parent->last = child;
    } else {
        child->next = reference;
        child->prev = reference->prev;
        if (reference->prev) reference->prev->next = child;
        else parent->first = child;
        reference->prev = child;
    }
}

void mx_dom_append(mx_node_t* parent, mx_node_t* child) {
    mx_dom_insert_before(parent, child, NULL);
}

void mx_dom_remove_children(mx_node_t* node) {
    while (node->first) mx_dom_remove(node->first);
}

/* Attribute names are stored in lower case; callers pass lower case. */
const char* mx_dom_attr(const mx_node_t* node, const char* name) {
    if (!node || node->type != MX_NODE_ELEMENT) return NULL;
    for (mx_attr_t* a = node->attrs; a; a = a->next)
        if (a->name[0] == name[0] && !strcmp(a->name, name)) return a->value;
    return NULL;
}

void mx_dom_set_attr(mx_document_t* doc, mx_node_t* node, const char* name,
                     const char* value) {
    if (!node || node->type != MX_NODE_ELEMENT) return;
    for (mx_attr_t* a = node->attrs; a; a = a->next) {
        if (mx_streq_nocase(a->name, name)) {
            if (a->value && value && !strcmp(a->value, value)) return;
            a->value = mx_arena_strdup(&doc->arena, value);
            return;
        }
    }
    mx_attr_t* attr = (mx_attr_t*)mx_arena_zalloc(&doc->arena, sizeof(*attr));
    if (!attr) return;
    attr->name = intern_tag(doc, name, strlen(name));
    attr->value = mx_arena_strdup(&doc->arena, value);
    if (!attr->name || !attr->value) return;
    /* keep source order */
    mx_attr_t** tail = &node->attrs;
    while (*tail) tail = &(*tail)->next;
    *tail = attr;
}

void mx_dom_remove_attr(mx_node_t* node, const char* name) {
    mx_attr_t** link = &node->attrs;
    while (*link) {
        if (mx_streq_nocase((*link)->name, name)) {
            *link = (*link)->next;
            return;
        }
        link = &(*link)->next;
    }
}

static mx_arena_t* class_arena;

void mx_dom_set_class_arena(mx_arena_t* arena) {
    class_arena = arena;
}

static void cache_classes(mx_node_t* node, const char* classes) {
    node->class_source = classes;
    node->class_count = 0;
    node->classes = NULL;
    unsigned count = 0;
    for (const char* p = classes; *p;) {
        while (mx_is_space(*p)) p++;
        if (!*p) break;
        count++;
        while (*p && !mx_is_space(*p)) p++;
    }
    if (!count || !class_arena) return;
    if (count > 64) count = 64;
    mx_class_token_t* tokens = (mx_class_token_t*)mx_arena_alloc(
        class_arena, sizeof(mx_class_token_t) * count);
    if (!tokens) return;
    unsigned used = 0;
    for (const char* p = classes; *p && used < count;) {
        while (mx_is_space(*p)) p++;
        if (!*p) break;
        const char* end = p;
        while (*end && !mx_is_space(*end)) end++;
        tokens[used].offset = (uint16_t)(p - classes);
        tokens[used].length = (uint16_t)(end - p);
        tokens[used].hash = mx_hash(p, (size_t)(end - p));
        used++;
        p = end;
    }
    node->classes = tokens;
    node->class_count = (uint16_t)used;
}

int mx_dom_has_class_hashed(mx_node_t* node, const char* name, size_t length,
                            uint32_t hash) {
    const char* classes = mx_dom_attr(node, "class");
    if (!classes) return 0;
    if (node->class_source != classes) cache_classes(node, classes);
    if (!node->classes) return mx_dom_has_class(node, name);
    for (unsigned i = 0; i < node->class_count; i++) {
        const mx_class_token_t* t = &node->classes[i];
        if (t->hash == hash && t->length == length &&
            !strncmp(classes + t->offset, name, length))
            return 1;
    }
    return 0;
}

int mx_dom_has_class(const mx_node_t* node, const char* name) {
    const char* classes = mx_dom_attr(node, "class");
    if (!classes) return 0;
    size_t length = strlen(name);
    const char* p = classes;
    while (*p) {
        while (mx_is_space(*p)) p++;
        const char* end = p;
        while (*end && !mx_is_space(*end)) end++;
        if ((size_t)(end - p) == length && !strncmp(p, name, length)) return 1;
        p = end;
    }
    return 0;
}

int mx_dom_is(const mx_node_t* node, const char* tag) {
    return node && node->type == MX_NODE_ELEMENT && !strcmp(node->tag, tag);
}

mx_node_t* mx_dom_next(const mx_node_t* root, const mx_node_t* node) {
    if (node->first) return node->first;
    while (node && node != root) {
        if (node->next) return node->next;
        node = node->parent;
    }
    return NULL;
}

mx_node_t* mx_dom_by_id(mx_node_t* root, const char* id) {
    for (mx_node_t* n = root; n; n = mx_dom_next(root, n)) {
        if (n->type != MX_NODE_ELEMENT) continue;
        const char* value = mx_dom_attr(n, "id");
        if (value && !strcmp(value, id)) return n;
    }
    return NULL;
}

mx_node_t* mx_dom_first_element_child(const mx_node_t* node) {
    for (mx_node_t* c = node->first; c; c = c->next)
        if (c->type == MX_NODE_ELEMENT) return c;
    return NULL;
}

mx_node_t* mx_dom_next_element(const mx_node_t* node) {
    for (mx_node_t* c = node->next; c; c = c->next)
        if (c->type == MX_NODE_ELEMENT) return c;
    return NULL;
}

mx_node_t* mx_dom_prev_element(const mx_node_t* node) {
    for (mx_node_t* c = node->prev; c; c = c->prev)
        if (c->type == MX_NODE_ELEMENT) return c;
    return NULL;
}

mx_node_t* mx_dom_clone(mx_document_t* doc, mx_node_t* node, int deep) {
    mx_node_t* copy;
    if (node->type == MX_NODE_ELEMENT) {
        copy = mx_dom_create_element(doc, node->tag);
        if (!copy) return NULL;
        for (mx_attr_t* a = node->attrs; a; a = a->next)
            mx_dom_set_attr(doc, copy, a->name, a->value);
        copy->checked = node->checked;
        if (node->value) copy->value = mx_arena_strdup(&doc->arena, node->value);
    } else {
        copy = mx_dom_create_text(doc, node->text, node->text_length);
        if (!copy) return NULL;
        copy->type = node->type;
    }
    if (deep)
        for (mx_node_t* c = node->first; c; c = c->next) {
            mx_node_t* child = mx_dom_clone(doc, c, 1);
            if (child) mx_dom_append(copy, child);
        }
    return copy;
}

/* ---------------------------------------------------------------------- */
/* Text content and serialisation                                         */
/* ---------------------------------------------------------------------- */

static size_t text_length_of(const mx_node_t* node) {
    if (node->type == MX_NODE_TEXT) return node->text_length;
    size_t total = 0;
    for (const mx_node_t* c = node->first; c; c = c->next)
        if (c->type == MX_NODE_TEXT || c->type == MX_NODE_ELEMENT)
            total += text_length_of(c);
    return total;
}

static void text_write(const mx_node_t* node, char* out, size_t* used) {
    if (node->type == MX_NODE_TEXT) {
        memcpy(out + *used, node->text, node->text_length);
        *used += node->text_length;
        return;
    }
    for (const mx_node_t* c = node->first; c; c = c->next)
        if (c->type == MX_NODE_TEXT || c->type == MX_NODE_ELEMENT)
            text_write(c, out, used);
}

char* mx_dom_text_content(mx_document_t* doc, const mx_node_t* node) {
    size_t length = text_length_of(node);
    char* out = (char*)mx_arena_alloc(&doc->arena, length + 1);
    if (!out) return (char*)"";
    size_t used = 0;
    text_write(node, out, &used);
    out[used] = '\0';
    return out;
}

void mx_dom_set_text_content(mx_document_t* doc, mx_node_t* node,
                             const char* text) {
    if (node->type == MX_NODE_TEXT) {
        node->text = mx_arena_strdup(&doc->arena, text);
        node->text_length = node->text ? strlen(node->text) : 0;
        if (!node->text) node->text = (char*)"";
        return;
    }
    mx_dom_remove_children(node);
    if (text && *text) {
        mx_node_t* child = mx_dom_create_text(doc, text, strlen(text));
        if (child) mx_dom_append(node, child);
    }
}

typedef struct {
    char* data;
    size_t used;
    size_t capacity;
    mx_document_t* doc;
} sbuf_t;

static void sb_put(sbuf_t* b, const char* text, size_t length) {
    if (b->used + length + 1 > b->capacity) {
        size_t capacity = b->capacity ? b->capacity * 2 : 256;
        while (capacity < b->used + length + 1) capacity *= 2;
        char* grown = (char*)mx_arena_alloc(&b->doc->arena, capacity);
        if (!grown) return;
        if (b->used) memcpy(grown, b->data, b->used);
        b->data = grown;
        b->capacity = capacity;
    }
    memcpy(b->data + b->used, text, length);
    b->used += length;
    b->data[b->used] = '\0';
}

static void sb_escaped(sbuf_t* b, const char* text, size_t length, int attr) {
    size_t start = 0;
    for (size_t i = 0; i < length; i++) {
        const char* entity = NULL;
        if (text[i] == '&') entity = "&amp;";
        else if (text[i] == '<' && !attr) entity = "&lt;";
        else if (text[i] == '>' && !attr) entity = "&gt;";
        else if (text[i] == '"' && attr) entity = "&quot;";
        if (!entity) continue;
        sb_put(b, text + start, i - start);
        sb_put(b, entity, strlen(entity));
        start = i + 1;
    }
    sb_put(b, text + start, length - start);
}

static int is_raw_text(const char* tag) {
    return !strcmp(tag, "script") || !strcmp(tag, "style");
}

static void serialize_node(sbuf_t* b, const mx_node_t* node) {
    if (node->type == MX_NODE_TEXT) {
        if (node->parent && node->parent->tag && is_raw_text(node->parent->tag))
            sb_put(b, node->text, node->text_length);
        else
            sb_escaped(b, node->text, node->text_length, 0);
        return;
    }
    if (node->type == MX_NODE_COMMENT) {
        sb_put(b, "<!--", 4);
        sb_put(b, node->text, node->text_length);
        sb_put(b, "-->", 3);
        return;
    }
    if (node->type == MX_NODE_DOCUMENT) {
        for (const mx_node_t* c = node->first; c; c = c->next)
            serialize_node(b, c);
        return;
    }
    sb_put(b, "<", 1);
    sb_put(b, node->tag, strlen(node->tag));
    for (mx_attr_t* a = node->attrs; a; a = a->next) {
        sb_put(b, " ", 1);
        sb_put(b, a->name, strlen(a->name));
        sb_put(b, "=\"", 2);
        sb_escaped(b, a->value, strlen(a->value), 1);
        sb_put(b, "\"", 1);
    }
    sb_put(b, ">", 1);
    if (mx_dom_is_void(node->tag)) return;
    for (const mx_node_t* c = node->first; c; c = c->next) serialize_node(b, c);
    sb_put(b, "</", 2);
    sb_put(b, node->tag, strlen(node->tag));
    sb_put(b, ">", 1);
}

char* mx_dom_serialize(mx_document_t* doc, const mx_node_t* node, int outer) {
    sbuf_t b = {NULL, 0, 0, doc};
    sb_put(&b, "", 0);
    if (outer) serialize_node(&b, node);
    else
        for (const mx_node_t* c = node->first; c; c = c->next)
            serialize_node(&b, c);
    return b.data ? b.data : (char*)"";
}

/* ---------------------------------------------------------------------- */
/* Character references                                                   */
/* ---------------------------------------------------------------------- */

typedef struct {
    const char* name;
    uint32_t codepoint;
} entity_t;

static const entity_t entities[] = {
    {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''},
    {"nbsp", 0xA0}, {"copy", 0xA9}, {"reg", 0xAE}, {"trade", 0x2122},
    {"mdash", 0x2014}, {"ndash", 0x2013}, {"hellip", 0x2026},
    {"laquo", 0xAB}, {"raquo", 0xBB}, {"lsquo", 0x2018}, {"rsquo", 0x2019},
    {"ldquo", 0x201C}, {"rdquo", 0x201D}, {"sbquo", 0x201A},
    {"bdquo", 0x201E}, {"bull", 0x2022}, {"middot", 0xB7},
    {"times", 0xD7}, {"divide", 0xF7}, {"deg", 0xB0}, {"plusmn", 0xB1},
    {"euro", 0x20AC}, {"pound", 0xA3}, {"yen", 0xA5}, {"cent", 0xA2},
    {"sect", 0xA7}, {"para", 0xB6}, {"larr", 0x2190}, {"rarr", 0x2192},
    {"uarr", 0x2191}, {"darr", 0x2193}, {"harr", 0x2194},
    {"hearts", 0x2665}, {"check", 0x2713}, {"iexcl", 0xA1},
    {"iquest", 0xBF}, {"shy", 0xAD}, {"ordf", 0xAA}, {"ordm", 0xBA},
    {"frac12", 0xBD}, {"frac14", 0xBC}, {"frac34", 0xBE}, {"sup2", 0xB2},
    {"sup3", 0xB3}, {"micro", 0xB5}, {"aacute", 0xE1}, {"eacute", 0xE9},
    {"iacute", 0xED}, {"oacute", 0xF3}, {"uacute", 0xFA}, {"auml", 0xE4},
    {"ouml", 0xF6}, {"uuml", 0xFC}, {"Auml", 0xC4}, {"Ouml", 0xD6},
    {"Uuml", 0xDC}, {"aring", 0xE5}, {"Aring", 0xC5}, {"szlig", 0xDF},
    {"ccedil", 0xE7}, {"ntilde", 0xF1}, {"egrave", 0xE8},
    {"agrave", 0xE0}, {"ecirc", 0xEA}, {"oslash", 0xF8}, {"aelig", 0xE6},
    {"Eacute", 0xC9}, {"thinsp", 0x2009}, {"ensp", 0x2002},
    {"emsp", 0x2003}, {"zwj", 0x200D}, {"zwnj", 0x200C}, {"lrm", 0x200E},
    {"rlm", 0x200F}, {"prime", 0x2032}, {"minus", 0x2212},
    {"le", 0x2264}, {"ge", 0x2265}, {"ne", 0x2260}, {"infin", 0x221E},
    {"star", 0x2606}, {"dagger", 0x2020}, {"permil", 0x2030},
    {"num", '#'}, {"excl", '!'}, {"colon", ':'}, {"semi", ';'},
    {"comma", ','}, {"period", '.'}, {"sol", '/'}, {"lpar", '('},
    {"rpar", ')'}, {"lbrack", '['}, {"rbrack", ']'}, {"lcub", '{'},
    {"rcub", '}'}, {"equals", '='}, {"plus", '+'}, {"dollar", '$'},
    {"percnt", '%'}, {"ast", '*'}, {"vert", '|'}, {"Hat", '^'},
    {"grave", '`'}, {"commat", '@'}, {"lowbar", '_'}, {"tilde", 0x2DC},
    {"NewLine", '\n'}, {"Tab", '\t'},
};

size_t mx_html_decode(char* out, size_t capacity, const char* source,
                      size_t length) {
    size_t used = 0;
    if (!capacity) return 0;
    for (size_t i = 0; i < length && used + 5 < capacity; i++) {
        if (source[i] != '&') {
            out[used++] = source[i];
            continue;
        }
        size_t end = i + 1;
        while (end < length && end - i <= 32 &&
               ((source[end] >= 'a' && source[end] <= 'z') ||
                (source[end] >= 'A' && source[end] <= 'Z') ||
                (source[end] >= '0' && source[end] <= '9') ||
                source[end] == '#'))
            end++;
        size_t name_length = end - i - 1;
        const char* name = source + i + 1;
        uint32_t cp = 0;
        if (name_length > 1 && name[0] == '#') {
            size_t p = 1;
            int base = 10;
            if (name[1] == 'x' || name[1] == 'X') {
                base = 16;
                p = 2;
                /* hex digits include a-f which the scan above accepted */
            }
            uint32_t value = 0;
            int digits = 0;
            for (; p < name_length; p++) {
                char c = name[p];
                unsigned d = c >= '0' && c <= '9' ? (unsigned)(c - '0') :
                             c >= 'a' && c <= 'f' ? (unsigned)(c - 'a' + 10) :
                             c >= 'A' && c <= 'F' ? (unsigned)(c - 'A' + 10) : 99u;
                if (d >= (unsigned)base) break;
                value = value * (uint32_t)base + d;
                digits++;
                if (value > 0x10FFFF) value = 0xFFFD;
            }
            if (digits) {
                cp = value ? value : 0xFFFD;
                end = i + 1 + p;
            }
        } else if (name_length) {
            for (size_t e = 0; e < sizeof(entities) / sizeof(entities[0]); e++) {
                size_t n = strlen(entities[e].name);
                if (n == name_length && !strncmp(entities[e].name, name, n)) {
                    cp = entities[e].codepoint;
                    break;
                }
            }
        }
        if (!cp) {
            out[used++] = '&';
            continue;
        }
        if (end < length && source[end] == ';') end++;
        used += mx_utf8_encode(cp, out + used);
        i = end - 1;
    }
    out[used] = '\0';
    return used;
}

/* ---------------------------------------------------------------------- */
/* Tree construction                                                      */
/* ---------------------------------------------------------------------- */

static int in_list(const char* tag, const char* const* list, size_t count) {
    for (size_t i = 0; i < count; i++)
        if (!strcmp(tag, list[i])) return 1;
    return 0;
}
#define IN(tag, list) in_list(tag, list, sizeof(list) / sizeof(list[0]))

int mx_dom_is_void(const char* tag) {
    static const char* const voids[] = {
        "area", "base", "br", "col", "embed", "hr", "img", "input", "link",
        "meta", "param", "source", "track", "wbr", "keygen"
    };
    return IN(tag, voids);
}

static const char* const closes_p[] = {
    "address", "article", "aside", "blockquote", "center", "details",
    "dialog", "dir", "div", "dl", "fieldset", "figcaption", "figure",
    "footer", "form", "h1", "h2", "h3", "h4", "h5", "h6", "header",
    "hgroup", "hr", "main", "menu", "nav", "ol", "p", "pre", "section",
    "summary", "table", "ul", "li", "dd", "dt", "search"
};

static const char* const head_tags[] = {
    "meta", "link", "style", "script", "title", "base", "noscript",
    "template"
};

typedef struct {
    mx_document_t* doc;
    mx_node_t* current;
    int in_body;
    int fragment;
    mx_node_t* boundary;   /* fragment parent: never pop above it */
} builder_t;

static mx_node_t* find_open(builder_t* b, const char* tag,
                            const char* const* stops, size_t stop_count) {
    for (mx_node_t* n = b->current; n && n->type == MX_NODE_ELEMENT;
         n = n->parent) {
        if (n == b->boundary) return NULL;
        if (!strcmp(n->tag, tag)) return n;
        if (stops && in_list(n->tag, stops, stop_count)) return NULL;
        if (!strcmp(n->tag, "html") || !strcmp(n->tag, "body")) return NULL;
    }
    return NULL;
}

static void pop_to(builder_t* b, mx_node_t* node) {
    if (node && node != b->boundary && node->parent) b->current = node->parent;
}

static void ensure_body(builder_t* b) {
    if (b->in_body || b->fragment) return;
    b->in_body = 1;
    b->current = b->doc->body;
}

static void add_text(builder_t* b, const char* text, size_t length) {
    if (!length) return;
    if (!b->in_body && !b->fragment) {
        size_t i = 0;
        while (i < length && mx_is_space(text[i])) i++;
        if (i == length) return;  /* whitespace between head elements */
        if (b->current == b->doc->head || b->current == b->doc->html ||
            b->current == b->doc->root)
            ensure_body(b);
    }
    mx_node_t* last = b->current->last;
    if (last && last->type == MX_NODE_TEXT) {
        char* joined = (char*)mx_arena_alloc(&b->doc->arena,
                                             last->text_length + length + 1);
        if (!joined) return;
        memcpy(joined, last->text, last->text_length);
        memcpy(joined + last->text_length, text, length);
        joined[last->text_length + length] = '\0';
        last->text = joined;
        last->text_length += length;
        return;
    }
    mx_node_t* node = mx_dom_create_text(b->doc, text, length);
    if (node) mx_dom_append(b->current, node);
}

static void add_decoded_text(builder_t* b, const char* text, size_t length) {
    static char buffer[4096];   /* not reentrant; keeps the 64 KiB stack free */
    size_t pos = 0;
    while (pos < length) {
        size_t chunk = length - pos;
        if (chunk > 3000) {
            chunk = 3000;
            /* do not split an entity */
            size_t back = 0;
            while (back < 32 && pos + chunk - back > pos &&
                   text[pos + chunk - back] != '&')
                back++;
            if (back < 32 && text[pos + chunk - back] == '&') chunk -= back;
            if (!chunk) chunk = 3000;
        }
        size_t n = mx_html_decode(buffer, sizeof(buffer), text + pos, chunk);
        add_text(b, buffer, n);
        pos += chunk;
    }
}

static void merge_attrs(mx_document_t* doc, mx_node_t* into, mx_node_t* from) {
    for (mx_attr_t* a = from->attrs; a; a = a->next)
        if (!mx_dom_attr(into, a->name)) mx_dom_set_attr(doc, into, a->name, a->value);
}

static void open_element(builder_t* b, mx_node_t* element, int self_closing) {
    const char* tag = element->tag;
    mx_document_t* doc = b->doc;
    if (!b->fragment) {
        if (!strcmp(tag, "html")) {
            merge_attrs(doc, doc->html, element);
            return;
        }
        if (!strcmp(tag, "head")) {
            if (!b->in_body) b->current = doc->head;
            return;
        }
        if (!strcmp(tag, "body")) {
            merge_attrs(doc, doc->body, element);
            b->in_body = 1;
            b->current = doc->body;
            return;
        }
        if (!b->in_body && !IN(tag, head_tags)) ensure_body(b);
        if (!b->in_body && b->current != doc->head &&
            !(b->current->type == MX_NODE_ELEMENT &&
              IN(b->current->tag, head_tags)))
            b->current = doc->head;
    }

    /* implicit end tags */
    if (IN(tag, closes_p)) {
        static const char* const p_stops[] = {"button", "table", "td", "th"};
        mx_node_t* p = find_open(b, "p", p_stops, 4);
        if (p) pop_to(b, p);
    }
    if (tag[0] == 'h' && tag[1] >= '1' && tag[1] <= '6' && !tag[2] &&
        b->current->type == MX_NODE_ELEMENT && b->current->tag[0] == 'h' &&
        b->current->tag[1] >= '1' && b->current->tag[1] <= '6' &&
        !b->current->tag[2])
        pop_to(b, b->current);
    if (!strcmp(tag, "li")) {
        static const char* const stops[] = {"ul", "ol", "menu"};
        mx_node_t* li = find_open(b, "li", stops, 3);
        if (li) pop_to(b, li);
    }
    if (!strcmp(tag, "dd") || !strcmp(tag, "dt")) {
        static const char* const stops[] = {"dl"};
        mx_node_t* open = find_open(b, "dd", stops, 1);
        if (!open) open = find_open(b, "dt", stops, 1);
        if (open) pop_to(b, open);
    }
    if (!strcmp(tag, "option") || !strcmp(tag, "optgroup")) {
        if (mx_dom_is(b->current, "option")) pop_to(b, b->current);
        if (!strcmp(tag, "optgroup") && mx_dom_is(b->current, "optgroup"))
            pop_to(b, b->current);
    }
    if (!strcmp(tag, "tr")) {
        static const char* const stops[] = {"table", "tbody", "thead", "tfoot"};
        mx_node_t* tr = find_open(b, "tr", stops, 4);
        if (tr) pop_to(b, tr);
    }
    if (!strcmp(tag, "td") || !strcmp(tag, "th")) {
        static const char* const stops[] = {"tr", "table"};
        mx_node_t* cell = find_open(b, "td", stops, 2);
        if (!cell) cell = find_open(b, "th", stops, 2);
        if (cell) pop_to(b, cell);
    }
    if (!strcmp(tag, "tbody") || !strcmp(tag, "thead") ||
        !strcmp(tag, "tfoot")) {
        static const char* const stops[] = {"table"};
        static const char* const sections[] = {"tbody", "thead", "tfoot"};
        for (int i = 0; i < 3; i++) {
            mx_node_t* open = find_open(b, sections[i], stops, 1);
            if (open) {
                pop_to(b, open);
                break;
            }
        }
    }
    if (!strcmp(tag, "a")) {
        mx_node_t* open = find_open(b, "a", NULL, 0);
        if (open) pop_to(b, open);
    }
    if (!strcmp(tag, "button")) {
        mx_node_t* open = find_open(b, "button", NULL, 0);
        if (open) pop_to(b, open);
    }

    mx_dom_append(b->current, element);
    if (!self_closing && !mx_dom_is_void(tag)) b->current = element;
}

static void close_element(builder_t* b, const char* tag) {
    if (!b->fragment) {
        if (!strcmp(tag, "head")) {
            if (!b->in_body) b->current = b->doc->html;
            return;
        }
        if (!strcmp(tag, "body") || !strcmp(tag, "html")) return;
    }
    if (!strcmp(tag, "br")) {
        mx_node_t* br = mx_dom_create_element(b->doc, "br");
        if (br) open_element(b, br, 1);
        return;
    }
    if (!strcmp(tag, "p") && !find_open(b, "p", NULL, 0)) {
        /* </p> with no open p: an empty paragraph */
        mx_node_t* p = mx_dom_create_element(b->doc, "p");
        if (p) open_element(b, p, 1);
        return;
    }
    static const char* const table_stops[] = {"table"};
    const char* const* stops = NULL;
    size_t stop_count = 0;
    if (!strcmp(tag, "td") || !strcmp(tag, "th") || !strcmp(tag, "tr")) {
        stops = table_stops;
        stop_count = 1;
    }
    mx_node_t* open = find_open(b, tag, stops, stop_count);
    if (open) pop_to(b, open);
}

static int tag_name_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_' || c == ':' ||
           c == '.';
}

/* Parses "<name attr=value ...>" starting after '<'. Returns the index after
 * '>' and fills the element. */
static size_t parse_start_tag(builder_t* b, const char* s, size_t length,
                              size_t pos, mx_node_t** out, int* self_closing) {
    size_t name_start = pos;
    while (pos < length && tag_name_char(s[pos])) pos++;
    char tag[32];
    mx_copy_n(tag, sizeof(tag), s + name_start, pos - name_start);
    for (char* p = tag; *p; p++) *p = mx_lower(*p);
    mx_node_t* element = mx_dom_create_element(b->doc, tag);
    *self_closing = 0;
    while (pos < length) {
        while (pos < length && (mx_is_space(s[pos]) || s[pos] == '/')) {
            if (s[pos] == '/' && pos + 1 < length && s[pos + 1] == '>')
                *self_closing = 1;
            pos++;
        }
        if (pos >= length) break;
        if (s[pos] == '>') {
            pos++;
            break;
        }
        size_t key_start = pos;
        while (pos < length && !mx_is_space(s[pos]) && s[pos] != '=' &&
               s[pos] != '>' && !(s[pos] == '/' && pos + 1 < length &&
                                  s[pos + 1] == '>'))
            pos++;
        size_t key_end = pos;
        if (key_end == key_start) {
            pos++;
            continue;
        }
        while (pos < length && mx_is_space(s[pos])) pos++;
        const char* value = "";
        size_t value_length = 0;
        if (pos < length && s[pos] == '=') {
            pos++;
            while (pos < length && mx_is_space(s[pos])) pos++;
            if (pos < length && (s[pos] == '"' || s[pos] == '\'')) {
                char quote = s[pos++];
                value = s + pos;
                while (pos < length && s[pos] != quote) pos++;
                value_length = (size_t)(s + pos - value);
                if (pos < length) pos++;
            } else {
                value = s + pos;
                while (pos < length && !mx_is_space(s[pos]) && s[pos] != '>')
                    pos++;
                value_length = (size_t)(s + pos - value);
            }
        }
        if (element) {
            char key[48];
            mx_copy_n(key, sizeof(key), s + key_start, key_end - key_start);
            if (!mx_dom_attr(element, key)) {
                char* decoded = (char*)mx_arena_alloc(&b->doc->arena,
                                                      value_length * 2 + 8);
                if (decoded) {
                    mx_html_decode(decoded, value_length * 2 + 8, value,
                                   value_length);
                    mx_dom_set_attr(b->doc, element, key, decoded);
                }
            }
        }
    }
    if (element && ((!strcmp(element->tag, "input") && mx_dom_attr(element, "checked")) ||
                    (!strcmp(element->tag, "option") && mx_dom_attr(element, "selected"))))
        element->checked = 1;
    *out = element;
    return pos;
}

/* Finds "</tag" (case-insensitive) from pos; returns its index or length. */
static size_t find_end_tag(const char* s, size_t length, size_t pos,
                           const char* tag) {
    size_t n = strlen(tag);
    for (size_t i = pos; i + n + 2 <= length; i++) {
        if (s[i] == '<' && s[i + 1] == '/' &&
            mx_strneq_nocase(s + i + 2, tag, n) &&
            (i + 2 + n >= length || !tag_name_char(s[i + 2 + n])))
            return i;
    }
    return length;
}

static void build(builder_t* b, const char* s, size_t length) {
    size_t pos = 0;
    while (pos < length) {
        if (b->doc->arena.failed) return;
        if (s[pos] != '<') {
            size_t end = pos;
            while (end < length && s[end] != '<') end++;
            add_decoded_text(b, s + pos, end - pos);
            pos = end;
            continue;
        }
        if (pos + 3 < length && s[pos + 1] == '!' && s[pos + 2] == '-' &&
            s[pos + 3] == '-') {
            size_t end = pos + 4;
            while (end + 2 < length &&
                   !(s[end] == '-' && s[end + 1] == '-' && s[end + 2] == '>'))
                end++;
            if (b->in_body || b->fragment) {
                mx_node_t* comment = mx_dom_create_text(
                    b->doc, s + pos + 4,
                    end > pos + 4 && end <= length ? end - pos - 4 : 0);
                if (comment) {
                    comment->type = MX_NODE_COMMENT;
                    mx_dom_append(b->current, comment);
                }
            }
            pos = end + 3 <= length ? end + 3 : length;
            continue;
        }
        if (pos + 1 < length && (s[pos + 1] == '!' || s[pos + 1] == '?')) {
            while (pos < length && s[pos] != '>') pos++;
            pos++;
            continue;
        }
        if (pos + 2 < length && s[pos + 1] == '/' &&
            ((s[pos + 2] >= 'a' && s[pos + 2] <= 'z') ||
             (s[pos + 2] >= 'A' && s[pos + 2] <= 'Z'))) {
            size_t name_start = pos + 2, p = name_start;
            while (p < length && tag_name_char(s[p])) p++;
            char tag[32];
            mx_copy_n(tag, sizeof(tag), s + name_start, p - name_start);
            for (char* c = tag; *c; c++) *c = mx_lower(*c);
            while (p < length && s[p] != '>') p++;
            pos = p + 1;
            close_element(b, tag);
            continue;
        }
        if (pos + 1 < length &&
            ((s[pos + 1] >= 'a' && s[pos + 1] <= 'z') ||
             (s[pos + 1] >= 'A' && s[pos + 1] <= 'Z'))) {
            mx_node_t* element = NULL;
            int self_closing = 0;
            pos = parse_start_tag(b, s, length, pos + 1, &element,
                                  &self_closing);
            if (!element) return;
            const char* tag = element->tag;
            int raw = !strcmp(tag, "script") || !strcmp(tag, "style") ||
                      !strcmp(tag, "xmp") || !strcmp(tag, "noembed");
            int rcdata = !strcmp(tag, "textarea") || !strcmp(tag, "title");
            if (!strcmp(tag, "svg") || !strcmp(tag, "math")) {
                /* Foreign content is not rendered: keep the element but skip
                 * its markup, which may use XML-style self-closing tags. */
                open_element(b, element, 1);
                if (!self_closing) {
                    size_t end = find_end_tag(s, length, pos, tag);
                    pos = end;
                    while (pos < length && s[pos] != '>') pos++;
                    pos++;
                }
                continue;
            }
            open_element(b, element, self_closing);
            if ((raw || rcdata) && !self_closing) {
                size_t end = find_end_tag(s, length, pos, tag);
                mx_node_t* holder = element;
                if (end > pos) {
                    if (raw) {
                        mx_node_t* text = mx_dom_create_text(b->doc, s + pos,
                                                             end - pos);
                        if (text) mx_dom_append(holder, text);
                    } else {
                        mx_node_t* saved = b->current;
                        b->current = holder;
                        add_decoded_text(b, s + pos, end - pos);
                        b->current = saved;
                    }
                }
                if (b->current == holder) pop_to(b, holder);
                pos = end;
                while (pos < length && s[pos] != '>') pos++;
                pos++;
            }
            continue;
        }
        add_text(b, "<", 1);
        pos++;
    }
}

void mx_dom_init(mx_document_t* doc) {
    memset(doc, 0, sizeof(*doc));
    mx_dom_set_class_arena(&doc->arena);
    doc->arena.limit = 48u * 1024u * 1024u;
    doc->next_serial = 1;
    doc->root = new_node(doc, MX_NODE_DOCUMENT);
    doc->html = mx_dom_create_element(doc, "html");
    doc->head = mx_dom_create_element(doc, "head");
    doc->body = mx_dom_create_element(doc, "body");
    if (!doc->root || !doc->html || !doc->head || !doc->body) return;
    mx_dom_append(doc->root, doc->html);
    mx_dom_append(doc->html, doc->head);
    mx_dom_append(doc->html, doc->body);
}

void mx_dom_free(mx_document_t* doc) {
    mx_arena_free_all(&doc->arena);
    memset(doc, 0, sizeof(*doc));
}

void mx_dom_parse(mx_document_t* doc, const char* source, size_t length) {
    builder_t b = {doc, doc->head, 0, 0, NULL};
    if (!doc->root) return;
    /* skip a UTF-8 byte order mark */
    if (length >= 3 && (unsigned char)source[0] == 0xEF &&
        (unsigned char)source[1] == 0xBB && (unsigned char)source[2] == 0xBF) {
        source += 3;
        length -= 3;
    }
    build(&b, source, length);
    for (mx_node_t* n = doc->head; n; n = mx_dom_next(doc->head, n)) {
        if (mx_dom_is(n, "title") && !doc->title[0]) {
            char* text = mx_dom_text_content(doc, n);
            /* collapse whitespace */
            size_t used = 0;
            int space = 0;
            for (const char* p = text; *p && used + 1 < sizeof(doc->title); p++) {
                if (mx_is_space(*p)) {
                    space = used > 0;
                    continue;
                }
                if (space && used + 2 < sizeof(doc->title)) doc->title[used++] = ' ';
                space = 0;
                doc->title[used++] = *p;
            }
            doc->title[used] = '\0';
        }
    }
}

void mx_dom_parse_fragment(mx_document_t* doc, mx_node_t* parent,
                           const char* source, size_t length) {
    builder_t b = {doc, parent, 1, 1, parent};
    build(&b, source, length);
}
