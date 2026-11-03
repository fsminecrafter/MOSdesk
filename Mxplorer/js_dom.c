/* Mxplorer JavaScript browser objects: window, document, elements, events,
 * timers, location, storage and network access. */

#include "mxplorer/js_internal.h"
#include "mxplorer/css.h"
#include "mxplorer/devlog.h"
#include "mxplorer/html.h"
#include "string.h"
#include "stdlib.h"

#define ARG(i) ((i) < argc ? args[i] : js_undef())

mx_document_t* js_doc;
mx_js_host_t js_host;
static int js_running;
static char page_url[512];

static js_obj* node_proto;
static js_obj* element_proto;
static js_obj* text_proto;
static js_obj* document_proto;
static js_obj* style_proto;
static js_obj* classlist_proto;
static js_obj* event_proto;
static js_obj* location_obj;
static js_val document_val;

void js_core_init(void);
void js_core_reset_scope_stack(void);

static js_val noop(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)self; (void)args; (void)argc; (void)callee;
    return js_undef();
}

static js_val false_fn(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)self; (void)args; (void)argc; (void)callee;
    return js_bool(0);
}

static js_val null_fn(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)self; (void)args; (void)argc; (void)callee;
    return js_null();
}

/* ====================================================================== */
/* Event listeners                                                        */
/* ====================================================================== */

typedef struct mx_listener {
    js_str* type;
    js_val fn;
    uint8_t capture;
    uint8_t once;
    uint8_t property;          /* el.onclick = ... */
    const char* attr_source;   /* compiled from onclick="..." */
    struct mx_listener* next;
} mx_listener;

static mx_listener* window_listeners;

static mx_listener** listeners_of(mx_node_t* node) {
    return node ? (mx_listener**)&node->listeners : &window_listeners;
}

static void add_listener(mx_node_t* node, js_str* type, js_val fn, int capture,
                         int once, int property) {
    mx_listener** list = listeners_of(node);
    if (!property) {
        for (mx_listener* l = *list; l; l = l->next)
            if (!l->property && js_str_eq(l->type, type) && js_strict_equals(l->fn, fn) &&
                l->capture == capture)
                return;
    } else {
        for (mx_listener* l = *list; l; l = l->next)
            if (l->property && js_str_eq(l->type, type)) {
                l->fn = fn;
                l->attr_source = NULL;
                return;
            }
    }
    mx_listener* l = (mx_listener*)js_alloc(sizeof(mx_listener));
    memset(l, 0, sizeof(*l));
    l->type = type;
    l->fn = fn;
    l->capture = (uint8_t)capture;
    l->once = (uint8_t)once;
    l->property = (uint8_t)property;
    /* keep registration order */
    mx_listener** tail = list;
    while (*tail) tail = &(*tail)->next;
    *tail = l;
}

static void remove_listener(mx_node_t* node, js_str* type, js_val fn, int capture) {
    mx_listener** link = listeners_of(node);
    while (*link) {
        mx_listener* l = *link;
        if (!l->property && js_str_eq(l->type, type) && js_strict_equals(l->fn, fn) &&
            l->capture == capture) {
            *link = l->next;
            return;
        }
        link = &l->next;
    }
}

static js_val property_handler(mx_node_t* node, js_str* type) {
    for (mx_listener* l = *listeners_of(node); l; l = l->next)
        if (l->property && js_str_eq(l->type, type)) return l->fn;
    return js_null();
}

/* ====================================================================== */
/* Entry and exit                                                         */
/* ====================================================================== */

/* The interpreter recurses deeply; programs only get a 64 KiB stack, so
 * scripts run on a separate 1 MiB stack. */
#define JS_STACK_SIZE (1024u * 1024u)
static char* js_stack_memory;
static int on_js_stack;

static void run_on_js_stack(void (*fn)(void*), void* arg) {
    if (on_js_stack) {
        fn(arg);
        return;
    }
    if (!js_stack_memory) js_stack_memory = (char*)malloc(JS_STACK_SIZE);
    if (!js_stack_memory) {
        fn(arg);
        return;
    }
#if defined(__x86_64__)
    char* top = (char*)(((uintptr_t)(js_stack_memory + JS_STACK_SIZE)) & ~(uintptr_t)15u);
    on_js_stack = 1;
    __asm__ volatile(
        "mov %%rsp, %%rbx\n\t"
        "mov %0, %%rsp\n\t"
        "call *%1\n\t"
        "mov %%rbx, %%rsp\n\t"
        :
        : "r"(top), "r"(fn), "D"(arg)
        : "rax", "rbx", "rcx", "rdx", "rsi", "r8", "r9", "r10", "r11", "memory", "cc",
          "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8",
          "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15");
    on_js_stack = 0;
#else
    fn(arg);
#endif
}

static void enter(char* stack_marker) {
    if (!js_running) {
        js_rt.stack_limit = on_js_stack ? (long)JS_STACK_SIZE - 96 * 1024 : 40 * 1024;
        js_rt.stack_base = stack_marker;
        js_rt.steps = 0;
        js_rt.step_limit = 6000000;
        js_rt.timed_out = 0;
        js_rt.depth = 0;
    }
    js_running++;
}

static void leave(void) {
    js_running--;
    if (js_running) return;
    if (js_rt.ctl == CTL_THROW) js_report_exception();
    js_rt.ctl = CTL_NONE;
    js_run_microtasks();
    if (js_rt.ctl == CTL_THROW) js_report_exception();
    js_rt.ctl = CTL_NONE;
    js_core_reset_scope_stack();
    if (js_rt.out_of_memory) {
        static int reported;
        if (!reported) {
            mx_console_add(MX_LOG_ERROR, "JavaScript", 0,
                           "The page ran out of script memory; scripts are stopped.");
            reported = 1;
        }
    }
}

void js_dom_mutated(void) {
    if (js_doc) js_doc->mutated = 1;
}

/* ====================================================================== */
/* Wrappers                                                               */
/* ====================================================================== */

js_val js_wrap_node(mx_node_t* node) {
    if (!node) return js_null();
    if (node->js_wrapper) return js_obj_val((js_obj*)node->js_wrapper);
    js_obj* proto = node->type == MX_NODE_DOCUMENT ? document_proto :
                    node->type == MX_NODE_ELEMENT ? element_proto : text_proto;
    js_obj* o = js_new_object(proto);
    o->cls = CLS_NODE;
    o->node = node;
    node->js_wrapper = o;
    return js_obj_val(o);
}

static mx_node_t* node_of(js_val v) {
    if (v.type == JS_OBJ && v.u.obj->cls == CLS_NODE) return v.u.obj->node;
    return NULL;
}

static mx_node_t* this_node(js_val self) {
    mx_node_t* n = node_of(self);
    if (!n) js_throw_error("TypeError", "Illegal invocation");
    return n;
}

static js_obj* node_list(void) {
    return js_new_array(0);
}

static js_val wrap_list_children(mx_node_t* n, int elements_only) {
    js_obj* list = node_list();
    for (mx_node_t* c = n->first; c; c = c->next) {
        if (elements_only && c->type != MX_NODE_ELEMENT) continue;
        if (c->type == MX_NODE_COMMENT && elements_only) continue;
        js_array_push(list, js_wrap_node(c));
    }
    return js_obj_val(list);
}

static js_str* cstr_or_empty(const char* s) {
    return s ? js_cstr(s) : js_intern_c("");
}

/* camelCase -> kebab-case */
static void kebab(const js_str* key, char* out, size_t capacity) {
    size_t used = 0;
    for (uint32_t i = 0; i < key->length && used + 2 < capacity; i++) {
        char c = key->data[i];
        if (c >= 'A' && c <= 'Z') {
            out[used++] = '-';
            out[used++] = (char)(c + 32);
        } else {
            out[used++] = c;
        }
    }
    out[used] = '\0';
}

/* ---- style ---- */

/* Finds "prop:" in an inline style string; returns value bounds. */
static int style_lookup(const char* style, const char* prop, const char** value,
                        size_t* length, const char** decl_start, const char** decl_end) {
    size_t n = strlen(prop);
    const char* p = style;
    while (p && *p) {
        while (*p == ' ' || *p == ';' || *p == '\n' || *p == '\t') p++;
        const char* start = p;
        const char* colon = strchr(p, ':');
        const char* semi = strchr(p, ';');
        if (!colon || (semi && semi < colon)) {
            p = semi ? semi + 1 : NULL;
            continue;
        }
        const char* name_end = colon;
        while (name_end > start && name_end[-1] == ' ') name_end--;
        const char* end = semi ? semi : p + strlen(p);
        if ((size_t)(name_end - start) == n && mx_strneq_nocase(start, prop, n)) {
            const char* v = colon + 1;
            while (*v == ' ') v++;
            const char* ve = end;
            while (ve > v && ve[-1] == ' ') ve--;
            *value = v;
            *length = (size_t)(ve - v);
            if (decl_start) *decl_start = start;
            if (decl_end) *decl_end = semi ? semi + 1 : end;
            return 1;
        }
        p = semi ? semi + 1 : NULL;
    }
    return 0;
}

static void style_set(mx_node_t* n, const char* prop, const char* value) {
    const char* current = mx_dom_attr(n, "style");
    char buffer[2048];
    buffer[0] = '\0';
    if (current) {
        const char *v, *ds, *de;
        size_t len;
        if (style_lookup(current, prop, &v, &len, &ds, &de)) {
            mx_copy_n(buffer, sizeof(buffer), current, (size_t)(ds - current));
            mx_append(buffer, sizeof(buffer), de);
        } else {
            mx_copy(buffer, sizeof(buffer), current);
        }
    }
    if (value && *value) {
        size_t used = strlen(buffer);
        while (used && (buffer[used - 1] == ' ')) buffer[--used] = '\0';
        if (used && buffer[used - 1] != ';') mx_append(buffer, sizeof(buffer), ";");
        if (strlen(buffer)) mx_append(buffer, sizeof(buffer), " ");
        mx_append(buffer, sizeof(buffer), prop);
        mx_append(buffer, sizeof(buffer), ": ");
        mx_append(buffer, sizeof(buffer), value);
        mx_append(buffer, sizeof(buffer), ";");
    }
    mx_dom_set_attr(js_doc, n, "style", buffer);
    js_dom_mutated();
}

static js_val style_get_value(mx_node_t* n, const char* prop) {
    const char* current = mx_dom_attr(n, "style");
    const char* v;
    size_t len;
    if (current && style_lookup(current, prop, &v, &len, NULL, NULL))
        return js_str_val(js_new_str(v, len));
    return js_str_val(js_intern_c(""));
}

static js_val style_method(js_val self, js_val* args, int argc, js_obj* callee) {
    if (self.type != JS_OBJ || self.u.obj->cls != CLS_STYLE) return js_undef();
    mx_node_t* n = self.u.obj->node;
    char prop[96];
    mx_copy(prop, sizeof(prop), js_to_string(ARG(0))->data);
    switch (callee->magic) {
    case 0:   /* setProperty */
        style_set(n, prop, js_to_string(ARG(1))->data);
        return js_undef();
    case 1:   /* getPropertyValue */
        return style_get_value(n, prop);
    case 2: {   /* removeProperty */
        js_val old = style_get_value(n, prop);
        style_set(n, prop, NULL);
        return old;
    }
    }
    return js_undef();
}

static js_val make_style(mx_node_t* n) {
    js_obj* o = js_new_object(style_proto);
    o->cls = CLS_STYLE;
    o->node = n;
    return js_obj_val(o);
}

/* ---- classList ---- */

static int class_list_contains(mx_node_t* n, const char* name) {
    return mx_dom_has_class(n, name);
}

static void class_list_set(mx_node_t* n, const char* name, int on) {
    const char* current = mx_dom_attr(n, "class");
    char buffer[1024];
    buffer[0] = '\0';
    size_t want = strlen(name);
    const char* p = current ? current : "";
    int present = 0;
    while (*p) {
        while (mx_is_space(*p)) p++;
        const char* end = p;
        while (*end && !mx_is_space(*end)) end++;
        if (end > p) {
            int same = (size_t)(end - p) == want && !strncmp(p, name, want);
            if (same) present = 1;
            if (!same || on) {
                if (buffer[0]) mx_append(buffer, sizeof(buffer), " ");
                size_t used = strlen(buffer);
                mx_copy_n(buffer + used, sizeof(buffer) - used, p, (size_t)(end - p));
            }
        }
        p = end;
    }
    if (on && !present) {
        if (buffer[0]) mx_append(buffer, sizeof(buffer), " ");
        mx_append(buffer, sizeof(buffer), name);
    }
    mx_dom_set_attr(js_doc, n, "class", buffer);
    js_dom_mutated();
}

static js_val classlist_method(js_val self, js_val* args, int argc, js_obj* callee) {
    if (self.type != JS_OBJ || self.u.obj->cls != CLS_CLASSLIST) return js_undef();
    mx_node_t* n = self.u.obj->node;
    switch (callee->magic) {
    case 0:   /* add */
        for (int i = 0; i < argc; i++) class_list_set(n, js_to_string(args[i])->data, 1);
        return js_undef();
    case 1:   /* remove */
        for (int i = 0; i < argc; i++) class_list_set(n, js_to_string(args[i])->data, 0);
        return js_undef();
    case 2: {   /* toggle */
        const char* name = js_to_string(ARG(0))->data;
        int on = argc > 1 ? js_truthy(args[1]) : !class_list_contains(n, name);
        class_list_set(n, name, on);
        return js_bool(on);
    }
    case 3:   /* contains */
        return js_bool(class_list_contains(n, js_to_string(ARG(0))->data));
    case 4: {   /* replace */
        const char* old = js_to_string(ARG(0))->data;
        if (!class_list_contains(n, old)) return js_bool(0);
        class_list_set(n, old, 0);
        class_list_set(n, js_to_string(ARG(1))->data, 1);
        return js_bool(1);
    }
    case 5: {   /* item */
        const char* classes = mx_dom_attr(n, "class");
        int want = (int)js_to_number(ARG(0)), index = 0;
        for (const char* p = classes ? classes : ""; *p;) {
            while (mx_is_space(*p)) p++;
            const char* end = p;
            while (*end && !mx_is_space(*end)) end++;
            if (end > p && index++ == want) return js_str_val(js_new_str(p, (size_t)(end - p)));
            p = end;
        }
        return js_null();
    }
    case 6: {   /* forEach */
        const char* classes = mx_dom_attr(n, "class");
        int index = 0;
        for (const char* p = classes ? classes : ""; *p && !js_rt.ctl;) {
            while (mx_is_space(*p)) p++;
            const char* end = p;
            while (*end && !mx_is_space(*end)) end++;
            if (end > p) {
                js_val call_args[2] = {js_str_val(js_new_str(p, (size_t)(end - p))), js_num(index++)};
                js_call(ARG(0), js_undef(), call_args, 2);
            }
            p = end;
        }
        return js_undef();
    }
    }
    return js_undef();
}

/* ====================================================================== */
/* Node properties                                                        */
/* ====================================================================== */

enum {
    G_NODE_TYPE, G_NODE_NAME, G_TAG_NAME, G_LOCAL_NAME, G_ID, G_CLASS_NAME,
    G_CLASS_LIST, G_TEXT_CONTENT, G_INNER_TEXT, G_INNER_HTML, G_OUTER_HTML,
    G_CHILDREN, G_CHILD_NODES, G_FIRST_CHILD, G_LAST_CHILD, G_FIRST_ELEMENT,
    G_LAST_ELEMENT, G_PARENT_NODE, G_PARENT_ELEMENT, G_NEXT_SIBLING,
    G_PREV_SIBLING, G_NEXT_ELEMENT, G_PREV_ELEMENT, G_CHILD_COUNT, G_STYLE,
    G_DATASET, G_VALUE, G_CHECKED, G_DISABLED, G_HREF, G_SRC, G_TYPE, G_NAME,
    G_PLACEHOLDER, G_TITLE, G_HIDDEN, G_SELECTED_INDEX, G_OPTIONS,
    G_OFFSET_WIDTH, G_OFFSET_HEIGHT, G_OFFSET_TOP, G_OFFSET_LEFT,
    G_CLIENT_WIDTH, G_CLIENT_HEIGHT, G_SCROLL_WIDTH, G_SCROLL_HEIGHT,
    G_SCROLL_TOP, G_SCROLL_LEFT, G_NODE_VALUE, G_DATA, G_OWNER_DOCUMENT,
    G_IS_CONNECTED, G_LENGTH, G_SELECTED, G_DEFAULT_VALUE, G_FORM, G_ALT,
    G_ACTION, G_METHOD, G_TARGET, G_REL, G_HTML_FOR, G_READONLY, G_REQUIRED,
    G_TAB_INDEX, G_ATTRIBUTES, G_OFFSET_PARENT, G_NATURAL_WIDTH,
    G_NATURAL_HEIGHT, G_COMPLETE, G_DOCUMENT_ELEMENT, G_BODY, G_HEAD,
    G_DOC_TITLE, G_COOKIE, G_READY_STATE, G_URL, G_LOCATION, G_DEFAULT_VIEW,
    G_ACTIVE_ELEMENT, G_FORMS, G_IMAGES, G_LINKS, G_SCRIPTS,
    G_VISIBILITY_STATE, G_DOC_HIDDEN, G_REFERRER, G_DOMAIN, G_CHARSET,
    G_COMPAT_MODE, G_CONTENT_EDITABLE, G_IS_CONTENT_EDITABLE, G_LABEL,
    G_TEXT, G_COUNT
};

static const char* const getter_names[G_COUNT] = {
    "nodeType", "nodeName", "tagName", "localName", "id", "className",
    "classList", "textContent", "innerText", "innerHTML", "outerHTML",
    "children", "childNodes", "firstChild", "lastChild", "firstElementChild",
    "lastElementChild", "parentNode", "parentElement", "nextSibling",
    "previousSibling", "nextElementSibling", "previousElementSibling",
    "childElementCount", "style", "dataset", "value", "checked", "disabled",
    "href", "src", "type", "name", "placeholder", "title", "hidden",
    "selectedIndex", "options", "offsetWidth", "offsetHeight", "offsetTop",
    "offsetLeft", "clientWidth", "clientHeight", "scrollWidth",
    "scrollHeight", "scrollTop", "scrollLeft", "nodeValue", "data",
    "ownerDocument", "isConnected", "length", "selected", "defaultValue",
    "form", "alt", "action", "method", "target", "rel", "htmlFor", "readOnly",
    "required", "tabIndex", "attributes", "offsetParent", "naturalWidth",
    "naturalHeight", "complete", "documentElement", "body", "head", "title",
    "cookie", "readyState", "URL", "location", "defaultView", "activeElement",
    "forms", "images", "links", "scripts", "visibilityState", "hidden",
    "referrer", "domain", "characterSet", "compatMode", "contentEditable",
    "isContentEditable", "label", "text",
};

static js_str* getter_keys[G_COUNT];
static int16_t getter_table[512];   /* open addressing on the key hash */

static void build_getter_table(void) {
    for (int i = 0; i < 512; i++) getter_table[i] = -1;
    for (int i = 0; i < G_COUNT; i++) {
        uint32_t slot = getter_keys[i]->hash & 511u;
        int duplicate = 0;
        while (getter_table[slot] >= 0) {
            if (js_str_eq(getter_keys[getter_table[slot]], getter_keys[i])) duplicate = 1;
            slot = (slot + 1) & 511u;
        }
        if (!duplicate) getter_table[slot] = (int16_t)i;
    }
}

static int getter_id(js_str* key) {
    uint32_t slot = key->hash & 511u;
    while (getter_table[slot] >= 0) {
        js_str* candidate = getter_keys[getter_table[slot]];
        if (candidate == key || js_str_eq(candidate, key)) return getter_table[slot];
        slot = (slot + 1) & 511u;
    }
    return -1;
}

static int attr_reflect(int id) {
    switch (id) {
    case G_SRC: case G_TYPE: case G_NAME: case G_PLACEHOLDER: case G_TITLE:
    case G_ALT: case G_ACTION: case G_METHOD: case G_TARGET: case G_REL:
        return 1;
    }
    return 0;
}

static const char* reflected_attr(int id) {
    switch (id) {
    case G_SRC: return "src";
    case G_TYPE: return "type";
    case G_NAME: return "name";
    case G_PLACEHOLDER: return "placeholder";
    case G_TITLE: return "title";
    case G_ALT: return "alt";
    case G_ACTION: return "action";
    case G_METHOD: return "method";
    case G_TARGET: return "target";
    case G_REL: return "rel";
    case G_HTML_FOR: return "for";
    case G_ID: return "id";
    case G_CLASS_NAME: return "class";
    }
    return NULL;
}

static js_str* upper_tag(const char* tag) {
    char buffer[64];
    size_t n = 0;
    for (; tag[n] && n + 1 < sizeof(buffer); n++)
        buffer[n] = tag[n] >= 'a' && tag[n] <= 'z' ? (char)(tag[n] - 32) : tag[n];
    buffer[n] = '\0';
    return js_intern(buffer, n);
}

static mx_node_t* closest_tag(mx_node_t* n, const char* tag) {
    for (; n; n = n->parent)
        if (mx_dom_is(n, tag)) return n;
    return NULL;
}

static js_val collect_tags(const char* tag) {
    js_obj* list = node_list();
    for (mx_node_t* n = js_doc->root; n; n = mx_dom_next(js_doc->root, n))
        if (mx_dom_is(n, tag)) js_array_push(list, js_wrap_node(n));
    return js_obj_val(list);
}

static const char* control_value(mx_node_t* n) {
    if (n->value) return n->value;
    if (mx_dom_is(n, "textarea")) return mx_dom_text_content(js_doc, n);
    if (mx_dom_is(n, "select")) {
        mx_node_t* first = NULL;
        for (mx_node_t* o = n; o; o = mx_dom_next(n, o)) {
            if (!mx_dom_is(o, "option")) continue;
            if (!first) first = o;
            if (o->checked) {
                first = o;
                break;
            }
        }
        if (!first) return "";
        const char* v = mx_dom_attr(first, "value");
        return v ? v : mx_dom_text_content(js_doc, first);
    }
    if (mx_dom_is(n, "option")) {
        const char* v = mx_dom_attr(n, "value");
        return v ? v : mx_dom_text_content(js_doc, n);
    }
    const char* v = mx_dom_attr(n, "value");
    if (!v && mx_dom_is(n, "input")) {
        const char* t = mx_dom_attr(n, "type");
        if (t && (mx_streq_nocase(t, "checkbox") || mx_streq_nocase(t, "radio"))) return "on";
    }
    return v ? v : "";
}

typedef struct {
    char* data;
    size_t used, capacity;
    int pending_space;
} text_buffer;

static void tb_put(text_buffer* b, char c) {
    if (b->used + 2 > b->capacity) {
        size_t capacity = b->capacity ? b->capacity * 2 : 256;
        char* grown = (char*)realloc(b->data, capacity);
        if (!grown) return;
        b->data = grown;
        b->capacity = capacity;
    }
    b->data[b->used++] = c;
}

static void inner_text_walk(mx_node_t* n, text_buffer* b, int depth) {
    if (depth > 200) return;
    for (mx_node_t* c = n->first; c; c = c->next) {
        if (c->type == MX_NODE_TEXT) {
            for (size_t i = 0; i < c->text_length; i++) {
                char ch = c->text[i];
                if (mx_is_space(ch)) {
                    b->pending_space = b->used && b->data[b->used - 1] != '\n';
                    continue;
                }
                if (b->pending_space) tb_put(b, ' ');
                b->pending_space = 0;
                tb_put(b, ch);
            }
            continue;
        }
        if (c->type != MX_NODE_ELEMENT) continue;
        if (mx_dom_is(c, "script") || mx_dom_is(c, "style") || mx_dom_is(c, "template") ||
            mx_dom_is(c, "noscript") || (c->style && c->style->display == MX_DISPLAY_NONE))
            continue;
        if (mx_dom_is(c, "br")) {
            tb_put(b, '\n');
            b->pending_space = 0;
            continue;
        }
        int block = c->style && c->style->display != MX_DISPLAY_INLINE &&
                    c->style->display != MX_DISPLAY_INLINE_BLOCK;
        if (block && b->used && b->data[b->used - 1] != '\n') tb_put(b, '\n');
        if (block) b->pending_space = 0;
        inner_text_walk(c, b, depth + 1);
        if (block && b->used && b->data[b->used - 1] != '\n') tb_put(b, '\n');
        if (block) b->pending_space = 0;
    }
}

static js_str* inner_text(mx_node_t* n) {
    text_buffer b = {NULL, 0, 0, 0};
    inner_text_walk(n, &b, 0);
    while (b.used && b.data[b.used - 1] == '\n') b.used--;
    js_str* s = js_new_str(b.data ? b.data : "", b.used);
    free(b.data);
    return s;
}

static js_val node_getter(mx_node_t* n, int id) {
    char buffer[1024];
    switch (id) {
    case G_NODE_TYPE:
        return js_num(n->type == MX_NODE_ELEMENT ? 1 : n->type == MX_NODE_TEXT ? 3 :
                      n->type == MX_NODE_COMMENT ? 8 : 9);
    case G_NODE_NAME:
        if (n->type == MX_NODE_TEXT) return js_str_val(js_intern_c("#text"));
        if (n->type == MX_NODE_COMMENT) return js_str_val(js_intern_c("#comment"));
        if (n->type == MX_NODE_DOCUMENT) return js_str_val(js_intern_c("#document"));
        return js_str_val(upper_tag(n->tag));
    case G_TAG_NAME:
        return n->type == MX_NODE_ELEMENT ? js_str_val(upper_tag(n->tag)) : js_undef();
    case G_LOCAL_NAME:
        return n->type == MX_NODE_ELEMENT ? js_str_val(js_cstr(n->tag)) : js_null();
    case G_ID: case G_CLASS_NAME: case G_HTML_FOR:
        return js_str_val(cstr_or_empty(mx_dom_attr(n, reflected_attr(id))));
    case G_CLASS_LIST: {
        js_obj* o = js_new_object(classlist_proto);
        o->cls = CLS_CLASSLIST;
        o->node = n;
        return js_obj_val(o);
    }
    case G_TEXT_CONTENT:
        if (n->type == MX_NODE_DOCUMENT) return js_null();
        return js_str_val(js_cstr(mx_dom_text_content(js_doc, n)));
    case G_INNER_TEXT:
        mx_html_ensure_layout();
        return js_str_val(inner_text(n));
    case G_INNER_HTML:
        return js_str_val(js_cstr(mx_dom_serialize(js_doc, n, 0)));
    case G_OUTER_HTML:
        return js_str_val(js_cstr(mx_dom_serialize(js_doc, n, 1)));
    case G_CHILDREN: return wrap_list_children(n, 1);
    case G_CHILD_NODES: return wrap_list_children(n, 0);
    case G_FIRST_CHILD: return js_wrap_node(n->first);
    case G_LAST_CHILD: return js_wrap_node(n->last);
    case G_FIRST_ELEMENT: return js_wrap_node(mx_dom_first_element_child(n));
    case G_LAST_ELEMENT: {
        mx_node_t* c = n->last;
        while (c && c->type != MX_NODE_ELEMENT) c = c->prev;
        return js_wrap_node(c);
    }
    case G_PARENT_NODE: return js_wrap_node(n->parent);
    case G_PARENT_ELEMENT:
        return n->parent && n->parent->type == MX_NODE_ELEMENT ? js_wrap_node(n->parent) : js_null();
    case G_NEXT_SIBLING: return js_wrap_node(n->next);
    case G_PREV_SIBLING: return js_wrap_node(n->prev);
    case G_NEXT_ELEMENT: return js_wrap_node(mx_dom_next_element(n));
    case G_PREV_ELEMENT: return js_wrap_node(mx_dom_prev_element(n));
    case G_CHILD_COUNT: {
        int count = 0;
        for (mx_node_t* c = n->first; c; c = c->next) count += c->type == MX_NODE_ELEMENT;
        return js_num(count);
    }
    case G_STYLE: return make_style(n);
    case G_DATASET: {
        js_obj* o = js_new_object(js_object_proto);
        o->cls = CLS_DATASET;
        o->node = n;
        return js_obj_val(o);
    }
    case G_VALUE: return js_str_val(js_cstr(control_value(n)));
    case G_DEFAULT_VALUE: return js_str_val(cstr_or_empty(mx_dom_attr(n, "value")));
    case G_CHECKED: return js_bool(n->checked);
    case G_SELECTED: return js_bool(n->checked || (mx_dom_attr(n, "selected") != NULL));
    case G_DISABLED: return js_bool(mx_dom_attr(n, "disabled") != NULL);
    case G_READONLY: return js_bool(mx_dom_attr(n, "readonly") != NULL);
    case G_REQUIRED: return js_bool(mx_dom_attr(n, "required") != NULL);
    case G_HIDDEN:
        if (n->type == MX_NODE_DOCUMENT) return js_bool(0);
        return js_bool(mx_dom_attr(n, "hidden") != NULL);
    case G_HREF: {
        const char* href = mx_dom_attr(n, "href");
        if (!href) return js_str_val(js_intern_c(""));
        mx_html_resolve(href, buffer, sizeof(buffer));
        return js_str_val(js_cstr(buffer));
    }
    case G_SRC: {
        const char* src = mx_dom_attr(n, "src");
        if (!src) return js_str_val(js_intern_c(""));
        mx_html_resolve(src, buffer, sizeof(buffer));
        return js_str_val(js_cstr(buffer));
    }
    case G_TYPE:
        if (mx_dom_is(n, "input") && !mx_dom_attr(n, "type")) return js_str_val(js_intern_c("text"));
        if (mx_dom_is(n, "button") && !mx_dom_attr(n, "type")) return js_str_val(js_intern_c("submit"));
        return js_str_val(cstr_or_empty(mx_dom_attr(n, "type")));
    case G_TITLE:
        if (n->type == MX_NODE_DOCUMENT) return js_str_val(js_cstr(js_doc->title));
        return js_str_val(cstr_or_empty(mx_dom_attr(n, "title")));
    case G_SELECTED_INDEX: {
        int index = 0, selected = -1, first = -1;
        for (mx_node_t* o = n; o; o = mx_dom_next(n, o)) {
            if (!mx_dom_is(o, "option")) continue;
            if (first < 0) first = index;
            if (o->checked && selected < 0) selected = index;
            index++;
        }
        return js_num(selected >= 0 ? selected : first);
    }
    case G_OPTIONS: return js_obj_val(({ js_obj* list = node_list();
        for (mx_node_t* o = n; o; o = mx_dom_next(n, o))
            if (mx_dom_is(o, "option")) js_array_push(list, js_wrap_node(o));
        list; }));
    case G_LENGTH:
        if (mx_dom_is(n, "select") || mx_dom_is(n, "form")) {
            int count = 0;
            for (mx_node_t* o = n; o; o = mx_dom_next(n, o))
                count += mx_dom_is(n, "select") ? mx_dom_is(o, "option")
                                                : (mx_dom_is(o, "input") || mx_dom_is(o, "select") ||
                                                   mx_dom_is(o, "textarea") || mx_dom_is(o, "button"));
            return js_num(count);
        }
        if (n->type == MX_NODE_TEXT) return js_num(n->text_length);
        return js_undef();
    case G_OFFSET_WIDTH: case G_CLIENT_WIDTH: case G_SCROLL_WIDTH:
        mx_html_ensure_layout();
        return js_num(n->box_w);
    case G_OFFSET_HEIGHT: case G_CLIENT_HEIGHT: case G_SCROLL_HEIGHT:
        mx_html_ensure_layout();
        if (n == js_doc->html || n == js_doc->body) return js_num(mx_html_content_height());
        return js_num(n->box_h);
    case G_OFFSET_TOP: mx_html_ensure_layout(); return js_num(n->box_y);
    case G_OFFSET_LEFT: mx_html_ensure_layout(); return js_num(n->box_x);
    case G_SCROLL_TOP:
        if (n == js_doc->html || n == js_doc->body) return js_num(mx_html_scroll_y());
        return js_num(0);
    case G_SCROLL_LEFT: return js_num(0);
    case G_NODE_VALUE: case G_DATA:
        if (n->type == MX_NODE_TEXT || n->type == MX_NODE_COMMENT)
            return js_str_val(js_new_str(n->text, n->text_length));
        return js_null();
    case G_OWNER_DOCUMENT: return document_val;
    case G_IS_CONNECTED: {
        for (mx_node_t* p = n; p; p = p->parent)
            if (p == js_doc->root) return js_bool(1);
        return js_bool(0);
    }
    case G_FORM: return js_wrap_node(closest_tag(n, "form"));
    case G_ALT: case G_ACTION: case G_METHOD: case G_TARGET: case G_REL:
    case G_NAME: case G_PLACEHOLDER:
        return js_str_val(cstr_or_empty(mx_dom_attr(n, reflected_attr(id))));
    case G_TAB_INDEX: {
        const char* t = mx_dom_attr(n, "tabindex");
        return js_num(t ? atoi(t) : -1);
    }
    case G_ATTRIBUTES: {
        js_obj* list = node_list();
        for (mx_attr_t* a = n->attrs; a; a = a->next) {
            js_obj* attr = js_new_object(js_object_proto);
            js_set_c(js_obj_val(attr), "name", js_str_val(js_cstr(a->name)));
            js_set_c(js_obj_val(attr), "value", js_str_val(js_cstr(a->value)));
            js_array_push(list, js_obj_val(attr));
        }
        return js_obj_val(list);
    }
    case G_OFFSET_PARENT: return js_wrap_node(js_doc->body);
    case G_NATURAL_WIDTH: case G_NATURAL_HEIGHT: return js_num(0);
    case G_COMPLETE: return js_bool(1);
    case G_LABEL:
        return js_str_val(js_cstr(mx_dom_attr(n, "label") ? mx_dom_attr(n, "label")
                                                          : mx_dom_text_content(js_doc, n)));
    case G_TEXT:
        return js_str_val(js_cstr(mx_dom_text_content(js_doc, n)));
    case G_CONTENT_EDITABLE:
        return js_str_val(cstr_or_empty(mx_dom_attr(n, "contenteditable")));
    case G_IS_CONTENT_EDITABLE: return js_bool(mx_dom_attr(n, "contenteditable") != NULL);
    }
    return js_undef();
}

static js_val document_getter(int id, int* found) {
    *found = 1;
    switch (id) {
    case G_DOCUMENT_ELEMENT: return js_wrap_node(js_doc->html);
    case G_BODY: return js_wrap_node(js_doc->body);
    case G_HEAD: return js_wrap_node(js_doc->head);
    case G_TITLE: return js_str_val(js_cstr(js_doc->title));
    case G_COOKIE: {
        extern js_val js_cookie_get(void);
        return js_cookie_get();
    }
    case G_READY_STATE: return js_str_val(js_intern_c("complete"));
    case G_URL: return js_str_val(js_cstr(page_url));
    case G_LOCATION: return js_obj_val(location_obj);
    case G_DEFAULT_VIEW: return js_obj_val(js_global);
    case G_ACTIVE_ELEMENT: return js_wrap_node(js_doc->body);
    case G_FORMS: return collect_tags("form");
    case G_IMAGES: return collect_tags("img");
    case G_SCRIPTS: return collect_tags("script");
    case G_LINKS: return collect_tags("a");
    case G_VISIBILITY_STATE: return js_str_val(js_intern_c("visible"));
    case G_DOC_HIDDEN: case G_HIDDEN: return js_bool(0);
    case G_REFERRER: return js_str_val(js_intern_c(""));
    case G_DOMAIN: {
        const char* host = strstr(page_url, "://");
        host = host ? host + 3 : page_url;
        size_t n = 0;
        while (host[n] && host[n] != '/' && host[n] != ':' && host[n] != '?') n++;
        return js_str_val(js_new_str(host, n));
    }
    case G_CHARSET: return js_str_val(js_intern_c("UTF-8"));
    case G_COMPAT_MODE: return js_str_val(js_intern_c("CSS1Compat"));
    case G_NODE_TYPE: case G_NODE_NAME: case G_FIRST_CHILD: case G_LAST_CHILD:
    case G_CHILDREN: case G_CHILD_NODES: case G_FIRST_ELEMENT: case G_PARENT_NODE:
    case G_TEXT_CONTENT: case G_OWNER_DOCUMENT: case G_IS_CONNECTED:
    case G_CHILD_COUNT: case G_LAST_ELEMENT:
        return node_getter(js_doc->root, id);
    case G_SCROLL_TOP: return js_num(mx_html_scroll_y());
    }
    *found = 0;
    return js_undef();
}

js_val js_dom_get(js_obj* obj, js_str* key, int* found) {
    *found = 0;
    if (!js_doc) return js_undef();
    if (obj->cls == CLS_STYLE) {
        if (js_str_eq_c(key, "setProperty") || js_str_eq_c(key, "getPropertyValue") ||
            js_str_eq_c(key, "removeProperty") || key->data[0] == '@' || key->data[0] == '\x01')
            return js_undef();
        *found = 1;
        if (js_str_eq_c(key, "cssText"))
            return js_str_val(cstr_or_empty(mx_dom_attr(obj->node, "style")));
        if (js_str_eq_c(key, "length")) return js_num(0);
        char prop[96];
        kebab(key, prop, sizeof(prop));
        if (!strncmp(prop, "webkit-", 7)) {
            memmove(prop + 1, prop, strlen(prop) + 1);
            prop[0] = '-';
        }
        if (!strcmp(prop, "css-float")) mx_copy(prop, sizeof(prop), "float");
        return style_get_value(obj->node, prop);
    }
    if (obj->cls == CLS_CLASSLIST) {
        if (js_str_eq_c(key, "length")) {
            *found = 1;
            const char* classes = mx_dom_attr(obj->node, "class");
            int count = 0;
            for (const char* p = classes ? classes : ""; *p;) {
                while (mx_is_space(*p)) p++;
                if (!*p) break;
                count++;
                while (*p && !mx_is_space(*p)) p++;
            }
            return js_num(count);
        }
        if (js_str_eq_c(key, "value")) {
            *found = 1;
            return js_str_val(cstr_or_empty(mx_dom_attr(obj->node, "class")));
        }
        return js_undef();
    }
    if (obj->cls == CLS_DATASET) {
        if (key->data[0] == '\x01') {
            *found = 1;
            js_obj* keys = js_new_array(0);
            for (mx_attr_t* a = obj->node->attrs; a; a = a->next) {
                if (strncmp(a->name, "data-", 5)) continue;
                char camel[96];
                size_t used = 0;
                for (const char* p = a->name + 5; *p && used + 1 < sizeof(camel); p++) {
                    if (*p == '-' && p[1]) {
                        p++;
                        camel[used++] = *p >= 'a' && *p <= 'z' ? (char)(*p - 32) : *p;
                    } else {
                        camel[used++] = *p;
                    }
                }
                js_array_push(keys, js_str_val(js_new_str(camel, used)));
            }
            return js_obj_val(keys);
        }
        char name[120] = "data-";
        kebab(key, name + 5, sizeof(name) - 5);
        const char* v = mx_dom_attr(obj->node, name);
        if (!v) return js_undef();
        *found = 1;
        return js_str_val(js_cstr(v));
    }
    if (obj->cls != CLS_NODE || !obj->node) return js_undef();
    mx_node_t* n = obj->node;
    /* event handler properties */
    if (key->length > 2 && key->data[0] == 'o' && key->data[1] == 'n' &&
        key->data[2] >= 'a' && key->data[2] <= 'z') {
        js_str* type = js_intern(key->data + 2, key->length - 2);
        js_val h = property_handler(n, type);
        *found = 1;
        return h;
    }
    int id = getter_id(key);
    if (id < 0) return js_undef();
    if (n->type == MX_NODE_DOCUMENT) return document_getter(id, found);
    if (n->type != MX_NODE_ELEMENT) {
        switch (id) {
        case G_NODE_VALUE: case G_DATA: case G_TEXT_CONTENT: case G_LENGTH:
        case G_NODE_TYPE: case G_NODE_NAME: case G_PARENT_NODE: case G_PARENT_ELEMENT:
        case G_NEXT_SIBLING: case G_PREV_SIBLING: case G_OWNER_DOCUMENT:
        case G_IS_CONNECTED: case G_NEXT_ELEMENT: case G_PREV_ELEMENT:
            *found = 1;
            if (id == G_TEXT_CONTENT) return js_str_val(js_new_str(n->text, n->text_length));
            return node_getter(n, id);
        }
        return js_undef();
    }
    if (id >= G_DOCUMENT_ELEMENT && id != G_TITLE && id != G_HIDDEN && id != G_LABEL &&
        id != G_TEXT && id != G_CONTENT_EDITABLE && id != G_IS_CONTENT_EDITABLE)
        return js_undef();
    *found = 1;
    return node_getter(n, id);
}

static void set_inner_html(mx_node_t* n, const char* html) {
    mx_dom_remove_children(n);
    mx_dom_parse_fragment(js_doc, n, html, strlen(html));
    js_dom_mutated();
}

int js_dom_set(js_obj* obj, js_str* key, js_val value) {
    if (!js_doc) return 0;
    if (obj->cls == CLS_STYLE) {
        if (key->data[0] == '@' || key->data[0] == '\x01') return 0;
        if (js_str_eq_c(key, "cssText")) {
            mx_dom_set_attr(js_doc, obj->node, "style", js_to_string(value)->data);
            js_dom_mutated();
            return 1;
        }
        char prop[96];
        kebab(key, prop, sizeof(prop));
        if (!strncmp(prop, "webkit-", 7)) {
            memmove(prop + 1, prop, strlen(prop) + 1);
            prop[0] = '-';
        }
        if (!strcmp(prop, "css-float")) mx_copy(prop, sizeof(prop), "float");
        js_str* text = value.type == JS_NULL || value.type == JS_UNDEF ? js_intern_c("") : js_to_string(value);
        if (value.type == JS_NUM && value.u.num != 0 && strcmp(prop, "opacity") && strcmp(prop, "z-index") &&
            strcmp(prop, "font-weight") && strcmp(prop, "flex-grow") && strcmp(prop, "flex-shrink") &&
            strcmp(prop, "line-height") && strcmp(prop, "order"))
            text = js_concat(text, js_intern_c("px"));
        style_set(obj->node, prop, text->data);
        return 1;
    }
    if (obj->cls == CLS_DATASET) {
        char name[120] = "data-";
        kebab(key, name + 5, sizeof(name) - 5);
        mx_dom_set_attr(js_doc, obj->node, name, js_to_string(value)->data);
        js_dom_mutated();
        return 1;
    }
    if (obj->cls == CLS_CLASSLIST) {
        if (js_str_eq_c(key, "value")) {
            mx_dom_set_attr(js_doc, obj->node, "class", js_to_string(value)->data);
            js_dom_mutated();
            return 1;
        }
        return 0;
    }
    if (obj->cls != CLS_NODE || !obj->node) return 0;
    mx_node_t* n = obj->node;
    if (key->length > 2 && key->data[0] == 'o' && key->data[1] == 'n' &&
        key->data[2] >= 'a' && key->data[2] <= 'z') {
        js_str* type = js_intern(key->data + 2, key->length - 2);
        add_listener(n, type, js_is_callable(value) ? value : js_null(), 0, 0, 1);
        return 1;
    }
    int id = getter_id(key);
    if (id < 0) return 0;
    if (n->type == MX_NODE_DOCUMENT) {
        if (id == G_TITLE) {
            mx_copy(js_doc->title, sizeof(js_doc->title), js_to_string(value)->data);
            return 1;
        }
        if (id == G_COOKIE) {
            extern void js_cookie_set(js_str * text);
            js_cookie_set(js_to_string(value));
            return 1;
        }
        if (id == G_LOCATION) {
            if (js_host.navigate) js_host.navigate(js_to_string(value)->data, js_host.user);
            return 1;
        }
        return 0;
    }
    js_str* text = js_to_string(value);
    switch (id) {
    case G_ID: case G_CLASS_NAME: case G_HTML_FOR:
        mx_dom_set_attr(js_doc, n, reflected_attr(id), text->data);
        js_dom_mutated();
        return 1;
    case G_TEXT_CONTENT: case G_INNER_TEXT: case G_NODE_VALUE: case G_DATA: case G_TEXT:
        if (n->type == MX_NODE_ELEMENT) {
            mx_dom_set_text_content(js_doc, n, value.type == JS_NULL ? "" : text->data);
        } else {
            mx_dom_set_text_content(js_doc, n, text->data);
        }
        js_dom_mutated();
        return 1;
    case G_INNER_HTML:
        set_inner_html(n, value.type == JS_NULL ? "" : text->data);
        return 1;
    case G_OUTER_HTML: {
        mx_node_t* parent = n->parent;
        if (!parent) return 1;
        mx_node_t* holder = mx_dom_create_element(js_doc, "div");
        mx_dom_parse_fragment(js_doc, holder, text->data, text->length);
        while (holder->first) mx_dom_insert_before(parent, holder->first, n);
        mx_dom_remove(n);
        js_dom_mutated();
        return 1;
    }
    case G_VALUE:
        if (mx_dom_is(n, "select")) {
            for (mx_node_t* o = n; o; o = mx_dom_next(n, o))
                if (mx_dom_is(o, "option")) o->checked = !strcmp(control_value(o), text->data);
        } else {
            n->value = mx_arena_strdup(&js_doc->arena, text->data);
        }
        js_dom_mutated();
        return 1;
    case G_DEFAULT_VALUE:
        mx_dom_set_attr(js_doc, n, "value", text->data);
        js_dom_mutated();
        return 1;
    case G_CHECKED: case G_SELECTED:
        n->checked = (uint8_t)js_truthy(value);
        js_dom_mutated();
        return 1;
    case G_DISABLED: case G_HIDDEN: case G_READONLY: case G_REQUIRED: {
        const char* attr = id == G_DISABLED ? "disabled" : id == G_HIDDEN ? "hidden" :
                           id == G_READONLY ? "readonly" : "required";
        if (js_truthy(value)) mx_dom_set_attr(js_doc, n, attr, "");
        else mx_dom_remove_attr(n, attr);
        js_dom_mutated();
        return 1;
    }
    case G_HREF:
        mx_dom_set_attr(js_doc, n, "href", text->data);
        js_dom_mutated();
        return 1;
    case G_SELECTED_INDEX: {
        int want = (int)js_to_number(value), index = 0;
        for (mx_node_t* o = n; o; o = mx_dom_next(n, o))
            if (mx_dom_is(o, "option")) o->checked = index++ == want;
        js_dom_mutated();
        return 1;
    }
    case G_TAB_INDEX:
        mx_dom_set_attr(js_doc, n, "tabindex", text->data);
        return 1;
    case G_SCROLL_TOP:
        if (n == js_doc->html || n == js_doc->body) {
            int w, h;
            mx_html_viewport(&w, &h);
            mx_html_set_scroll((int)js_to_number(value), (uint32_t)h);
        }
        return 1;
    case G_STYLE:
        mx_dom_set_attr(js_doc, n, "style", text->data);
        js_dom_mutated();
        return 1;
    case G_CONTENT_EDITABLE:
        mx_dom_set_attr(js_doc, n, "contenteditable", text->data);
        return 1;
    default:
        if (attr_reflect(id)) {
            mx_dom_set_attr(js_doc, n, reflected_attr(id), text->data);
            js_dom_mutated();
            return 1;
        }
        /* read-only properties ignore writes */
        return id != G_ATTRIBUTES ? 1 : 0;
    }
}

/* ====================================================================== */
/* Tree mutation                                                          */
/* ====================================================================== */

static void run_inserted_scripts(mx_node_t* root);

static mx_node_t* as_insertable(js_val v) {
    mx_node_t* n = node_of(v);
    if (n) return n;
    if (v.type == JS_UNDEF || v.type == JS_NULL) return NULL;
    js_str* text = js_to_string(v);
    return mx_dom_create_text(js_doc, text->data, text->length);
}

static int is_fragment(mx_node_t* n) {
    return n && n->type == MX_NODE_ELEMENT && !strcmp(n->tag, "#document-fragment");
}

static void insert(mx_node_t* parent, mx_node_t* child, mx_node_t* before) {
    if (!parent || !child) return;
    if (is_fragment(child)) {
        mx_node_t* c = child->first;
        while (c) {
            mx_node_t* next = c->next;
            mx_dom_insert_before(parent, c, before);
            run_inserted_scripts(c);
            c = next;
        }
    } else {
        mx_dom_insert_before(parent, child, before);
        run_inserted_scripts(child);
    }
    js_dom_mutated();
}

static js_val el_append_child(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)callee;
    mx_node_t* n = this_node(self);
    if (!n) return js_undef();
    mx_node_t* child = node_of(ARG(0));
    if (!child) return js_throw_error("TypeError", "Failed to execute 'appendChild': parameter 1 is not of type 'Node'.");
    insert(n, child, NULL);
    return ARG(0);
}

static js_val el_append(js_val self, js_val* args, int argc, js_obj* callee) {
    mx_node_t* n = this_node(self);
    if (!n) return js_undef();
    mx_node_t* first = n->first;
    for (int i = 0; i < argc; i++) {
        mx_node_t* child = as_insertable(args[i]);
        if (!child) continue;
        if (callee->magic == 1) insert(n, child, first);    /* prepend */
        else if (callee->magic == 2) insert(n->parent, child, n);   /* before */
        else if (callee->magic == 3) insert(n->parent, child, n->next);   /* after */
        else insert(n, child, NULL);
        if (callee->magic == 3) {
            /* keep order for several nodes */
            n = child;
        }
    }
    return js_undef();
}

static js_val el_remove_child(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)callee;
    mx_node_t* n = this_node(self);
    mx_node_t* child = node_of(ARG(0));
    if (!n || !child || child->parent != n)
        return js_throw_error("NotFoundError", "The node to be removed is not a child of this node.");
    mx_dom_remove(child);
    js_dom_mutated();
    return ARG(0);
}

static js_val el_remove(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)args; (void)argc; (void)callee;
    mx_node_t* n = this_node(self);
    if (n && n->parent) {
        mx_dom_remove(n);
        js_dom_mutated();
    }
    return js_undef();
}

static js_val el_insert_before(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)callee;
    mx_node_t* n = this_node(self);
    mx_node_t* child = node_of(ARG(0));
    if (!n || !child) return js_throw_error("TypeError", "parameter 1 is not of type 'Node'.");
    insert(n, child, node_of(ARG(1)));
    return ARG(0);
}

static js_val el_replace_child(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)callee;
    mx_node_t* n = this_node(self);
    mx_node_t* fresh = node_of(ARG(0));
    mx_node_t* old = node_of(ARG(1));
    if (!n || !fresh || !old || old->parent != n) return js_throw_error("NotFoundError", "The node to be replaced is not a child of this node.");
    insert(n, fresh, old);
    mx_dom_remove(old);
    return ARG(1);
}

static js_val el_replace_with(js_val self, js_val* args, int argc, js_obj* callee) {
    mx_node_t* n = this_node(self);
    if (!n || !n->parent) return js_undef();
    mx_node_t* parent = n->parent;
    if (callee->magic) {   /* replaceChildren */
        mx_dom_remove_children(n);
        for (int i = 0; i < argc; i++) insert(n, as_insertable(args[i]), NULL);
        return js_undef();
    }
    for (int i = 0; i < argc; i++) insert(parent, as_insertable(args[i]), n);
    mx_dom_remove(n);
    js_dom_mutated();
    return js_undef();
}

static js_val el_insert_adjacent(js_val self, js_val* args, int argc, js_obj* callee) {
    mx_node_t* n = this_node(self);
    if (!n) return js_undef();
    js_str* where = js_to_string(ARG(0));
    mx_node_t* holder = NULL;
    mx_node_t* single = NULL;
    if (callee->magic == 0) {   /* HTML */
        holder = mx_dom_create_element(js_doc, "#document-fragment");
        js_str* html = js_to_string(ARG(1));
        mx_dom_parse_fragment(js_doc, holder, html->data, html->length);
    } else if (callee->magic == 1) {
        single = node_of(ARG(1));
    } else {
        js_str* text = js_to_string(ARG(1));
        single = mx_dom_create_text(js_doc, text->data, text->length);
    }
    mx_node_t* node = holder ? holder : single;
    if (!node) return js_null();
    if (mx_streq_nocase(where->data, "beforebegin")) insert(n->parent, node, n);
    else if (mx_streq_nocase(where->data, "afterbegin")) insert(n, node, n->first);
    else if (mx_streq_nocase(where->data, "beforeend")) insert(n, node, NULL);
    else if (mx_streq_nocase(where->data, "afterend")) insert(n->parent, node, n->next);
    return single ? js_wrap_node(single) : js_undef();
}

static js_val el_clone(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)callee;
    mx_node_t* n = this_node(self);
    if (!n) return js_undef();
    return js_wrap_node(mx_dom_clone(js_doc, n, js_truthy(ARG(0))));
}

static js_val el_contains(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)callee;
    mx_node_t* n = this_node(self);
    mx_node_t* other = node_of(ARG(0));
    for (mx_node_t* p = other; p; p = p->parent)
        if (p == n) return js_bool(1);
    return js_bool(0);
}

static js_val el_has_child_nodes(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)args; (void)argc;
    mx_node_t* n = this_node(self);
    if (!n) return js_undef();
    return js_bool(callee->magic ? n->attrs != NULL : n->first != NULL);
}

/* ---- attributes ---- */

static js_val el_attribute(js_val self, js_val* args, int argc, js_obj* callee) {
    mx_node_t* n = this_node(self);
    if (!n) return js_undef();
    char name[96];
    js_str* raw = js_to_string(ARG(0));
    size_t k = 0;
    for (; k < raw->length && k + 1 < sizeof(name); k++) name[k] = mx_lower(raw->data[k]);
    name[k] = '\0';
    switch (callee->magic) {
    case 0: {   /* getAttribute */
        const char* v = mx_dom_attr(n, name);
        return v ? js_str_val(js_cstr(v)) : js_null();
    }
    case 1:   /* setAttribute */
        mx_dom_set_attr(js_doc, n, name, js_to_string(ARG(1))->data);
        if (!strcmp(name, "checked")) n->checked = 1;
        if (!strcmp(name, "value") && mx_dom_is(n, "input")) n->value = NULL;
        js_dom_mutated();
        return js_undef();
    case 2:   /* removeAttribute */
        mx_dom_remove_attr(n, name);
        if (!strcmp(name, "checked")) n->checked = 0;
        js_dom_mutated();
        return js_undef();
    case 3:   /* hasAttribute */
        return js_bool(mx_dom_attr(n, name) != NULL);
    case 4: {   /* toggleAttribute */
        int on = argc > 1 ? js_truthy(args[1]) : !mx_dom_attr(n, name);
        if (on) mx_dom_set_attr(js_doc, n, name, "");
        else mx_dom_remove_attr(n, name);
        js_dom_mutated();
        return js_bool(on);
    }
    case 5: {   /* getAttributeNames */
        js_obj* list = js_new_array(0);
        for (mx_attr_t* a = n->attrs; a; a = a->next) js_array_push(list, js_str_val(js_cstr(a->name)));
        return js_obj_val(list);
    }
    }
    return js_undef();
}

/* ---- queries ---- */

static js_val query(mx_node_t* root, js_val selector, int all) {
    js_str* s = js_to_string(selector);
    js_obj* list = all ? node_list() : NULL;
    for (mx_node_t* n = root->first ? mx_dom_next(root, root) : NULL; n; n = mx_dom_next(root, n)) {
        if (n->type != MX_NODE_ELEMENT) continue;
        int m = mx_css_matches(n, s->data);
        if (m < 0) {
            char message[200];
            mx_copy(message, sizeof(message), "Failed to execute 'querySelector': '");
            mx_append(message, sizeof(message), s->data);
            mx_append(message, sizeof(message), "' is not a valid selector.");
            return js_throw_error("SyntaxError", message);
        }
        if (!m) continue;
        if (!all) return js_wrap_node(n);
        js_array_push(list, js_wrap_node(n));
    }
    return all ? js_obj_val(list) : js_null();
}

static js_val el_query(js_val self, js_val* args, int argc, js_obj* callee) {
    mx_node_t* n = node_of(self);
    if (!n) n = js_doc->root;
    switch (callee->magic) {
    case 0: return query(n, ARG(0), 0);
    case 1: return query(n, ARG(0), 1);
    case 2: {   /* getElementsByClassName */
        js_obj* list = node_list();
        js_str* names = js_to_string(ARG(0));
        for (mx_node_t* e = mx_dom_next(n, n); e; e = mx_dom_next(n, e)) {
            if (e->type != MX_NODE_ELEMENT) continue;
            int all = 1;
            const char* p = names->data;
            int any = 0;
            while (*p && all) {
                while (mx_is_space(*p)) p++;
                const char* end = p;
                while (*end && !mx_is_space(*end)) end++;
                if (end > p) {
                    char one[96];
                    mx_copy_n(one, sizeof(one), p, (size_t)(end - p));
                    all = mx_dom_has_class(e, one);
                    any = 1;
                }
                p = end;
            }
            if (all && any) js_array_push(list, js_wrap_node(e));
        }
        return js_obj_val(list);
    }
    case 3: {   /* getElementsByTagName */
        js_obj* list = node_list();
        js_str* tag = js_to_string(ARG(0));
        char lower[64];
        size_t k = 0;
        for (; k < tag->length && k + 1 < sizeof(lower); k++) lower[k] = mx_lower(tag->data[k]);
        lower[k] = '\0';
        for (mx_node_t* e = mx_dom_next(n, n); e; e = mx_dom_next(n, e))
            if (e->type == MX_NODE_ELEMENT && (lower[0] == '*' || !strcmp(e->tag, lower)))
                js_array_push(list, js_wrap_node(e));
        return js_obj_val(list);
    }
    case 4:   /* getElementById */
        return js_wrap_node(mx_dom_by_id(js_doc->root, js_to_string(ARG(0))->data));
    case 5: {   /* getElementsByName */
        js_obj* list = node_list();
        js_str* name = js_to_string(ARG(0));
        for (mx_node_t* e = js_doc->root; e; e = mx_dom_next(js_doc->root, e)) {
            const char* v = mx_dom_attr(e, "name");
            if (v && !strcmp(v, name->data)) js_array_push(list, js_wrap_node(e));
        }
        return js_obj_val(list);
    }
    case 6: {   /* matches */
        int m = mx_css_matches(n, js_to_string(ARG(0))->data);
        if (m < 0) return js_throw_error("SyntaxError", "is not a valid selector");
        return js_bool(m);
    }
    case 7: {   /* closest */
        js_str* s = js_to_string(ARG(0));
        for (mx_node_t* e = n; e && e->type == MX_NODE_ELEMENT; e = e->parent) {
            int m = mx_css_matches(e, s->data);
            if (m < 0) return js_throw_error("SyntaxError", "is not a valid selector");
            if (m) return js_wrap_node(e);
        }
        return js_null();
    }
    }
    return js_undef();
}

/* ---- creation ---- */

static js_val doc_create(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)self;
    switch (callee->magic) {
    case 0: {   /* createElement */
        js_str* tag = js_to_string(ARG(0));
        return js_wrap_node(mx_dom_create_element(js_doc, tag->data));
    }
    case 1: {   /* createTextNode */
        js_str* text = js_to_string(ARG(0));
        return js_wrap_node(mx_dom_create_text(js_doc, text->data, text->length));
    }
    case 2: {   /* createComment */
        js_str* text = js_to_string(ARG(0));
        mx_node_t* c = mx_dom_create_text(js_doc, text->data, text->length);
        c->type = MX_NODE_COMMENT;
        return js_wrap_node(c);
    }
    case 3:   /* createDocumentFragment */
        return js_wrap_node(mx_dom_create_element(js_doc, "#document-fragment"));
    case 4: {   /* createElementNS */
        js_str* tag = js_to_string(ARG(1));
        return js_wrap_node(mx_dom_create_element(js_doc, tag->data));
    }
    }
    return js_undef();
}

static js_val doc_write(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)self; (void)callee;
    for (int i = 0; i < argc; i++) {
        js_str* html = js_to_string(args[i]);
        mx_dom_parse_fragment(js_doc, js_doc->body, html->data, html->length);
    }
    js_dom_mutated();
    return js_undef();
}

/* ====================================================================== */
/* Geometry                                                               */
/* ====================================================================== */

static js_val make_rect(int x, int y, int w, int h) {
    js_obj* r = js_new_object(js_object_proto);
    js_val rv = js_obj_val(r);
    js_set_c(rv, "x", js_num(x));
    js_set_c(rv, "y", js_num(y));
    js_set_c(rv, "left", js_num(x));
    js_set_c(rv, "top", js_num(y));
    js_set_c(rv, "width", js_num(w));
    js_set_c(rv, "height", js_num(h));
    js_set_c(rv, "right", js_num(x + w));
    js_set_c(rv, "bottom", js_num(y + h));
    return rv;
}

static js_val el_geometry(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)args; (void)argc;
    mx_node_t* n = this_node(self);
    if (!n) return js_undef();
    mx_html_ensure_layout();
    int scroll = mx_html_scroll_y();
    switch (callee->magic) {
    case 0:   /* getBoundingClientRect */
        return make_rect(n->box_x, n->box_y - scroll, n->box_w, n->box_h);
    case 1: {   /* getClientRects */
        js_obj* list = js_new_array(1);
        js_array_push(list, make_rect(n->box_x, n->box_y - scroll, n->box_w, n->box_h));
        return js_obj_val(list);
    }
    case 2: {   /* scrollIntoView */
        int w, h;
        mx_html_viewport(&w, &h);
        mx_html_set_scroll(n->box_y, (uint32_t)h);
        return js_undef();
    }
    }
    return js_undef();
}

static js_val el_misc(js_val self, js_val* args, int argc, js_obj* callee) {
    mx_node_t* n = node_of(self);
    switch (callee->magic) {
    case 0:   /* click */
        if (n) mx_js_dispatch(n, "click", NULL);
        return js_undef();
    case 1:   /* focus, blur, select, noop methods */
        return js_undef();
    case 2:   /* checkValidity, reportValidity */
        return js_bool(1);
    case 3: {   /* animate: a finished animation */
        js_obj* anim = js_new_object(js_object_proto);
        js_set_c(js_obj_val(anim), "finished", js_promise_resolved(js_obj_val(anim), 0));
        js_define_native(anim, "cancel", (js_native)el_misc, 0);
        js_define_native(anim, "play", (js_native)el_misc, 0);
        js_define_native(anim, "pause", (js_native)el_misc, 0);
        js_set_c(js_obj_val(anim), "onfinish", js_null());
        (void)args;
        (void)argc;
        return js_obj_val(anim);
    }
    case 4:   /* attachShadow: the element doubles as its shadow root */
        return self;
    case 5:   /* dialog show / showModal */
        if (n) mx_dom_set_attr(js_doc, n, "open", "");
        js_dom_mutated();
        return js_undef();
    case 6:   /* dialog close */
        if (n) mx_dom_remove_attr(n, "open");
        js_dom_mutated();
        return js_undef();
    case 7:   /* media play() */
        return js_promise_resolved(js_undef(), 0);
    case 8:   /* form submit / requestSubmit */
        if (n) {
            int prevented = mx_js_dispatch(n, "submit", NULL);
            if (!prevented && js_host.navigate) {
                const char* action = mx_dom_attr(n, "action");
                js_host.navigate(action && *action ? action : page_url, js_host.user);
            }
        }
        return js_undef();
    case 9:   /* getContext: no canvas support */
        return js_null();
    }
    return js_undef();
}

/* ====================================================================== */
/* Events                                                                 */
/* ====================================================================== */

static js_val event_method(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)args; (void)argc;
    if (self.type != JS_OBJ) return js_undef();
    switch (callee->magic) {
    case 0:   /* preventDefault */
        if (js_truthy(js_get_c(self, "cancelable")))
            js_set_c(self, "defaultPrevented", js_bool(1));
        break;
    case 1:   /* stopPropagation */
        js_define(self.u.obj, "\x01stop", js_bool(1), PROP_HIDDEN);
        break;
    case 2:   /* stopImmediatePropagation */
        js_define(self.u.obj, "\x01stop", js_bool(1), PROP_HIDDEN);
        js_define(self.u.obj, "\x01stopnow", js_bool(1), PROP_HIDDEN);
        break;
    case 3:   /* composedPath */
        return js_get_c(self, "\x01path");
    }
    return js_undef();
}

static int event_bubbles(const char* type) {
    static const char* const no_bubble[] = {"focus", "blur", "load", "unload", "scroll",
                                            "mouseenter", "mouseleave", "DOMContentLoaded",
                                            "resize", "toggle"};
    for (size_t i = 0; i < sizeof(no_bubble) / sizeof(no_bubble[0]); i++)
        if (!strcmp(type, no_bubble[i])) return 0;
    return 1;
}

static js_obj* make_event(const char* type, int bubbles, int cancelable) {
    js_obj* e = js_new_object(event_proto);
    js_val ev = js_obj_val(e);
    js_set_c(ev, "type", js_str_val(js_cstr(type)));
    js_set_c(ev, "bubbles", js_bool(bubbles));
    js_set_c(ev, "cancelable", js_bool(cancelable));
    js_set_c(ev, "defaultPrevented", js_bool(0));
    js_set_c(ev, "timeStamp", js_num((double)mx_now_ms()));
    js_set_c(ev, "isTrusted", js_bool(1));
    js_set_c(ev, "target", js_null());
    js_set_c(ev, "currentTarget", js_null());
    js_set_c(ev, "eventPhase", js_num(0));
    return e;
}

/* Compiles onclick="..." attributes lazily. */
static void sync_attribute_handler(mx_node_t* node, js_str* type) {
    if (!node || node->type != MX_NODE_ELEMENT) return;
    char attr[48] = "on";
    mx_append(attr, sizeof(attr), type->data);
    const char* source = mx_dom_attr(node, attr);
    mx_listener* existing = NULL;
    for (mx_listener* l = *listeners_of(node); l; l = l->next)
        if (l->property && js_str_eq(l->type, type)) existing = l;
    if (!source) return;
    if (existing && (existing->attr_source == source || !existing->attr_source)) return;
    js_str* code = js_concat(js_concat(js_intern_c("(function(event){\n"), js_cstr(source)),
                             js_intern_c("\n})"));
    char error[160];
    int line = 0;
    char name[64];
    mx_copy(name, sizeof(name), node->tag);
    mx_append(name, sizeof(name), " ");
    mx_append(name, sizeof(name), attr);
    js_node* program = js_parse(code->data, code->length, js_intern_c(name)->data, error,
                                sizeof(error), &line);
    if (!program) {
        char message[220];
        mx_copy(message, sizeof(message), "SyntaxError: ");
        mx_append(message, sizeof(message), error);
        mx_console_add(MX_LOG_ERROR, name, line, message);
        return;
    }
    js_val fn = js_exec_program(program);
    if (!js_is_callable(fn)) return;
    add_listener(node, type, fn, 0, 0, 1);
    for (mx_listener* l = *listeners_of(node); l; l = l->next)
        if (l->property && js_str_eq(l->type, type)) l->attr_source = source;
}

static void invoke_listeners(mx_node_t* node, js_val target_val, js_obj* event,
                             js_str* type, int phase) {
    js_val ev = js_obj_val(event);
    sync_attribute_handler(node, type);
    js_set_c(ev, "currentTarget", node ? js_wrap_node(node) : js_obj_val(js_global));
    js_set_c(ev, "eventPhase", js_num(phase));
    mx_listener* snapshot[64];
    int count = 0;
    for (mx_listener* l = *listeners_of(node); l && count < 64; l = l->next) {
        if (!js_str_eq(l->type, type) || !js_is_callable(l->fn)) continue;
        if (phase == 1 && !l->capture) continue;
        if (phase == 3 && l->capture) continue;
        snapshot[count++] = l;
    }
    for (int i = 0; i < count; i++) {
        mx_listener* l = snapshot[i];
        if (l->once) remove_listener(node, type, l->fn, l->capture);
        js_val this_val = node ? js_wrap_node(node) : js_obj_val(js_global);
        js_val r = js_call(l->fn, this_val, &ev, 1);
        if (js_rt.ctl == CTL_THROW) {
            js_report_exception();
            js_rt.ctl = CTL_NONE;
            if (js_rt.timed_out) return;
        }
        if (l->property && r.type == JS_BOOL && !r.u.boolean)
            js_set_c(ev, "defaultPrevented", js_bool(1));
        if (js_truthy(js_get_c(ev, "\x01stopnow"))) break;
    }
    (void)target_val;
}

static int dispatch_event(mx_node_t* target, js_obj* event) {
    js_val ev = js_obj_val(event);
    js_str* type = js_to_string(js_get_c(ev, "type"));
    int bubbles = js_truthy(js_get_c(ev, "bubbles"));
    mx_node_t* path[256];
    int depth = 0;
    for (mx_node_t* n = target; n && depth < 255; n = n->parent) path[depth++] = n;
    int include_window = target && (depth && path[depth - 1] == js_doc->root);
    js_val target_val = js_wrap_node(target);
    js_set_c(ev, "target", target ? target_val : js_obj_val(js_global));
    js_set_c(ev, "srcElement", target ? target_val : js_obj_val(js_global));
    js_obj* composed = js_new_array((uint32_t)depth + 1);
    for (int i = 0; i < depth; i++) js_array_push(composed, js_wrap_node(path[i]));
    if (include_window) js_array_push(composed, js_obj_val(js_global));
    js_define(event, "\x01path", js_obj_val(composed), PROP_HIDDEN);
    /* capture */
    if (include_window) invoke_listeners(NULL, target_val, event, type, 1);
    for (int i = depth - 1; i >= 1 && !js_truthy(js_get_c(ev, "\x01stop")); i--)
        invoke_listeners(path[i], target_val, event, type, 1);
    /* target */
    if (!js_truthy(js_get_c(ev, "\x01stop")) && target) {
        invoke_listeners(target, target_val, event, type, 1);
        invoke_listeners(target, target_val, event, type, 3);
    }
    /* bubble */
    if (bubbles) {
        for (int i = 1; i < depth && !js_truthy(js_get_c(ev, "\x01stop")); i++)
            invoke_listeners(path[i], target_val, event, type, 3);
        if (include_window && !js_truthy(js_get_c(ev, "\x01stop")))
            invoke_listeners(NULL, target_val, event, type, 3);
    }
    js_set_c(ev, "eventPhase", js_num(0));
    js_set_c(ev, "currentTarget", js_null());
    return js_truthy(js_get_c(ev, "defaultPrevented"));
}

static js_val el_add_listener(js_val self, js_val* args, int argc, js_obj* callee) {
    mx_node_t* n = node_of(self);
    js_str* type = js_to_string(ARG(0));
    js_val fn = ARG(1);
    if (fn.type == JS_OBJ && !js_is_callable(fn)) {
        /* { handleEvent() } objects */
        js_val handler = js_get_c(fn, "handleEvent");
        if (js_is_callable(handler)) {
            js_obj* bound = js_new_object(js_function_proto);
            bound->cls = CLS_BOUND;
            bound->bound_target = handler.u.obj;
            bound->bound_this = fn;
            fn = js_obj_val(bound);
        }
    }
    if (!js_is_callable(fn)) return js_undef();
    int capture = 0, once = 0;
    if (ARG(2).type == JS_BOOL) capture = ARG(2).u.boolean;
    else if (ARG(2).type == JS_OBJ) {
        capture = js_truthy(js_get_c(ARG(2), "capture"));
        once = js_truthy(js_get_c(ARG(2), "once"));
    }
    if (callee->magic) remove_listener(n, type, fn, capture);
    else add_listener(n, type, fn, capture, once, 0);
    return js_undef();
}

static js_val el_dispatch_event(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)callee;
    if (ARG(0).type != JS_OBJ) return js_throw_error("TypeError", "parameter 1 is not of type 'Event'.");
    int prevented = dispatch_event(node_of(self), ARG(0).u.obj);
    return js_bool(!prevented);
}

static js_val event_ctor(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)self; (void)callee;
    js_str* type = js_to_string(ARG(0));
    int bubbles = 0, cancelable = 0;
    if (ARG(1).type == JS_OBJ) {
        bubbles = js_truthy(js_get_c(ARG(1), "bubbles"));
        cancelable = js_truthy(js_get_c(ARG(1), "cancelable"));
    }
    js_obj* e = make_event(type->data, bubbles, cancelable);
    js_set_c(js_obj_val(e), "isTrusted", js_bool(0));
    if (ARG(1).type == JS_OBJ) {
        js_obj* keys = js_own_keys(ARG(1).u.obj, 0);
        for (uint32_t i = 0; i < keys->length; i++) {
            js_str* key = keys->items[i].u.str;
            if (js_str_eq_c(key, "bubbles") || js_str_eq_c(key, "cancelable")) continue;
            js_set(js_obj_val(e), key, js_get(ARG(1), key));
        }
    }
    if (!js_find_own(e, js_intern_c("detail"))) js_set_c(js_obj_val(e), "detail", js_null());
    return js_obj_val(e);
}

static js_val create_event(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)self; (void)args; (void)argc; (void)callee;
    js_obj* e = make_event("", 0, 0);
    return js_obj_val(e);
}

static js_val event_init(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)callee;
    js_set_c(self, "type", js_str_val(js_to_string(ARG(0))));
    js_set_c(self, "bubbles", js_bool(js_truthy(ARG(1))));
    js_set_c(self, "cancelable", js_bool(js_truthy(ARG(2))));
    if (argc > 3) js_set_c(self, "detail", ARG(3));
    return js_undef();
}

/* ====================================================================== */
/* Timers                                                                 */
/* ====================================================================== */

typedef struct {
    int id;
    js_val fn;
    js_val* args;
    int argc;
    long due;
    long interval;
    int active;
    int animation;
} js_timer;

#define MAX_TIMERS 256
static js_timer timers[MAX_TIMERS];
static int timer_next_id = 1;

static js_val set_timer(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)self;
    js_val fn = ARG(0);
    if (!js_is_callable(fn)) {
        if (fn.type == JS_STR) {
            /* setTimeout("code") */
            js_str* code = js_concat(js_concat(js_intern_c("(function(){"), fn.u.str), js_intern_c("\n})"));
            char error[160];
            int line;
            js_node* program = js_parse(code->data, code->length, "timer", error, sizeof(error), &line);
            if (!program) return js_throw_error("SyntaxError", error);
            fn = js_exec_program(program);
        }
        if (!js_is_callable(fn)) return js_num(0);
    }
    int interval = callee->magic == 1;
    int animation = callee->magic == 2;
    double requested = animation ? 16 : js_to_number(ARG(1));
    long delay = requested == requested && requested > 0 ? (long)requested : 0;
    if (interval && delay < 10) delay = 10;
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (timers[i].active) continue;
        js_timer* t = &timers[i];
        t->id = timer_next_id++;
        t->fn = fn;
        int extra = animation ? 0 : argc > 2 ? argc - 2 : 0;
        t->argc = extra;
        t->args = NULL;
        if (extra) {
            t->args = (js_val*)js_alloc(sizeof(js_val) * (size_t)extra);
            memcpy(t->args, args + 2, sizeof(js_val) * (size_t)extra);
        }
        t->due = mx_now_ms() + delay;
        t->interval = interval ? delay : 0;
        t->animation = animation;
        t->active = 1;
        return js_num(t->id);
    }
    mx_console_add(MX_LOG_WARN, "Timers", 0, "Too many active timers; setTimeout ignored.");
    return js_num(0);
}

static js_val clear_timer(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)self; (void)callee;
    int id = (int)js_to_number(ARG(0));
    for (int i = 0; i < MAX_TIMERS; i++)
        if (timers[i].active && timers[i].id == id) timers[i].active = 0;
    return js_undef();
}

long mx_js_next_timer(long now_ms) {
    long best = -1;
    if (!js_doc || js_rt.out_of_memory) return -1;
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (!timers[i].active) continue;
        long wait = timers[i].due - now_ms;
        if (wait < 0) wait = 0;
        if (best < 0 || wait < best) best = wait;
    }
    return best;
}

unsigned mx_js_timer_count(void) {
    unsigned count = 0;
    for (int i = 0; i < MAX_TIMERS; i++) count += timers[i].active != 0;
    return count;
}

static void run_timers_direct(long now_ms) {
    if (!js_doc) return;
    char marker;
    int due[MAX_TIMERS];
    int count = 0;
    for (int i = 0; i < MAX_TIMERS; i++)
        if (timers[i].active && timers[i].due <= now_ms) due[count++] = i;
    /* earliest first, then creation order */
    for (int i = 1; i < count; i++) {
        int v = due[i], j = i - 1;
        while (j >= 0 && (timers[due[j]].due > timers[v].due ||
                          (timers[due[j]].due == timers[v].due && timers[due[j]].id > timers[v].id))) {
            due[j + 1] = due[j];
            j--;
        }
        due[j + 1] = v;
    }
    for (int k = 0; k < count; k++) {
        js_timer* t = &timers[due[k]];
        if (!t->active) continue;
        js_val fn = t->fn;
        js_val* args = t->args;
        int argc = t->argc;
        js_val stamp = js_num((double)now_ms);
        if (t->interval) {
            t->due = now_ms + t->interval;
        } else {
            t->active = 0;
        }
        enter(&marker);
        js_rt.source = "timer";
        if (t->animation) js_call(fn, js_undef(), &stamp, 1);
        else js_call(fn, js_undef(), args, argc);
        if (js_rt.timed_out && t->interval) t->active = 0;   /* stop runaway intervals */
        leave();
    }
}

/* ====================================================================== */
/* Window, location, storage                                              */
/* ====================================================================== */

static js_val win_alert(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)self;
    js_str* text = js_to_string(ARG(0));
    if (callee->magic == 0) {
        mx_console_add(MX_LOG_INFO, "alert", 0, text->data);
        if (js_host.alert) js_host.alert(text->data, js_host.user);
        return js_undef();
    }
    if (callee->magic == 1) {   /* confirm */
        mx_console_add(MX_LOG_INFO, "confirm", 0, text->data);
        return js_bool(1);
    }
    mx_console_add(MX_LOG_INFO, "prompt", 0, text->data);   /* prompt */
    return argc > 1 ? js_str_val(js_to_string(args[1])) : js_null();
}

static js_val win_scroll(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)self;
    int w, h;
    mx_html_viewport(&w, &h);
    double y;
    if (ARG(0).type == JS_OBJ) y = js_to_number(js_get_c(ARG(0), "top"));
    else y = js_to_number(ARG(1));
    if (y != y) y = 0;
    if (callee->magic) y += mx_html_scroll_y();   /* scrollBy */
    mx_html_set_scroll((int)y, (uint32_t)h);
    return js_undef();
}

static js_val win_get(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)self; (void)args; (void)argc;
    int w, h;
    mx_html_viewport(&w, &h);
    switch (callee->magic) {
    case 0: return js_num(w);
    case 1: return js_num(h);
    case 2: return js_num(mx_html_scroll_y());
    case 3: return js_num(0);
    case 4: return document_val;
    }
    return js_undef();
}

static js_val computed_property(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)callee;
    js_str* name = js_to_string(ARG(0));
    char camel[96];
    size_t used = 0;
    for (uint32_t i = 0; i < name->length && used + 1 < sizeof(camel); i++) {
        if (name->data[i] == '-' && i + 1 < name->length) {
            char c = name->data[++i];
            camel[used++] = c >= 'a' && c <= 'z' ? (char)(c - 32) : c;
        } else {
            camel[used++] = name->data[i];
        }
    }
    js_val v = js_get(self, js_intern(camel, used));
    return v.type == JS_UNDEF ? js_str_val(js_intern_c("")) : v;
}

static js_val win_get_computed_style(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)self; (void)callee;
    mx_node_t* n = node_of(ARG(0));
    js_obj* o = js_new_object(js_object_proto);
    js_val ov = js_obj_val(o);
    if (!n) return ov;
    mx_html_ensure_layout();
    const mx_style_t* s = n->style;
    char buffer[64];
    if (s) {
        static const char* const displays[] = {"inline", "block", "inline-block", "list-item",
                                               "flex", "inline-flex", "grid", "table",
                                               "table-row", "table-cell", "table-row-group",
                                               "table-caption", "none", "contents"};
        js_set_c(ov, "display", js_str_val(js_intern_c(displays[s->display < 14 ? s->display : 1])));
        buffer[0] = '\0';
        mx_append(buffer, sizeof(buffer), "rgb(");
        mx_append_int(buffer, sizeof(buffer), (s->color >> 16) & 255);
        mx_append(buffer, sizeof(buffer), ", ");
        mx_append_int(buffer, sizeof(buffer), (s->color >> 8) & 255);
        mx_append(buffer, sizeof(buffer), ", ");
        mx_append_int(buffer, sizeof(buffer), s->color & 255);
        mx_append(buffer, sizeof(buffer), ")");
        js_set_c(ov, "color", js_str_val(js_cstr(buffer)));
        buffer[0] = '\0';
        mx_append(buffer, sizeof(buffer), (s->background >> 24) ? "rgb(" : "rgba(0, 0, 0, 0)");
        if (s->background >> 24) {
            mx_append_int(buffer, sizeof(buffer), (s->background >> 16) & 255);
            mx_append(buffer, sizeof(buffer), ", ");
            mx_append_int(buffer, sizeof(buffer), (s->background >> 8) & 255);
            mx_append(buffer, sizeof(buffer), ", ");
            mx_append_int(buffer, sizeof(buffer), s->background & 255);
            mx_append(buffer, sizeof(buffer), ")");
        }
        js_set_c(ov, "backgroundColor", js_str_val(js_cstr(buffer)));
        buffer[0] = '\0';
        mx_append_int(buffer, sizeof(buffer), s->font_size);
        mx_append(buffer, sizeof(buffer), "px");
        js_set_c(ov, "fontSize", js_str_val(js_cstr(buffer)));
        buffer[0] = '\0';
        mx_append_int(buffer, sizeof(buffer), s->font_weight);
        js_set_c(ov, "fontWeight", js_str_val(js_cstr(buffer)));
        js_set_c(ov, "visibility", js_str_val(js_intern_c(s->visibility_hidden ? "hidden" : "visible")));
        buffer[0] = '\0';
        mx_append(buffer, sizeof(buffer), js_number_to_string(s->opacity / 255.0, 10)->data);
        js_set_c(ov, "opacity", js_str_val(js_cstr(buffer)));
        static const char* const positions[] = {"static", "relative", "absolute", "fixed", "sticky"};
        js_set_c(ov, "position", js_str_val(js_intern_c(positions[s->position < 5 ? s->position : 0])));
    }
    buffer[0] = '\0';
    mx_append_int(buffer, sizeof(buffer), n->box_w);
    mx_append(buffer, sizeof(buffer), "px");
    js_set_c(ov, "width", js_str_val(js_cstr(buffer)));
    buffer[0] = '\0';
    mx_append_int(buffer, sizeof(buffer), n->box_h);
    mx_append(buffer, sizeof(buffer), "px");
    js_set_c(ov, "height", js_str_val(js_cstr(buffer)));
    js_define_native(o, "getPropertyValue", computed_property, 1);
    return ov;
}

static js_val win_match_media(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)self; (void)callee;
    js_str* q = js_to_string(ARG(0));
    js_obj* m = js_new_object(js_object_proto);
    js_set_c(js_obj_val(m), "matches", js_bool(mx_css_media_matches(q->data)));
    js_set_c(js_obj_val(m), "media", js_str_val(q));
    js_define_native(m, "addListener", noop, 0);
    js_define_native(m, "removeListener", noop, 0);
    js_define_native(m, "addEventListener", noop, 0);
    js_define_native(m, "removeEventListener", noop, 0);
    return js_obj_val(m);
}

static js_val performance_now(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)self; (void)args; (void)argc; (void)callee;
    return js_num((double)mx_now_ms());
}

/* location */

static void url_part(int part, char* out, size_t capacity) {
    const char* url = page_url;
    const char* scheme_end = strstr(url, "://");
    out[0] = '\0';
    if (!scheme_end) {
        if (part == 0) mx_copy(out, capacity, url);
        return;
    }
    const char* host = scheme_end + 3;
    const char* path = host;
    while (*path && *path != '/' && *path != '?' && *path != '#') path++;
    const char* query = path;
    while (*query && *query != '?' && *query != '#') query++;
    const char* hash = query;
    while (*hash && *hash != '#') hash++;
    switch (part) {
    case 0: mx_copy(out, capacity, url); break;                                   /* href */
    case 1: mx_copy_n(out, capacity, url, (size_t)(scheme_end - url) + 1); break; /* protocol */
    case 2: mx_copy_n(out, capacity, host, (size_t)(path - host)); break;          /* host */
    case 3: {                                                                      /* hostname */
        const char* colon = host;
        while (colon < path && *colon != ':') colon++;
        mx_copy_n(out, capacity, host, (size_t)(colon - host));
        break;
    }
    case 4: mx_copy_n(out, capacity, path, (size_t)(query - path)); if (!out[0]) mx_copy(out, capacity, "/"); break;
    case 5: mx_copy_n(out, capacity, query, (size_t)(hash - query)); break;        /* search */
    case 6: mx_copy(out, capacity, hash); break;                                   /* hash */
    case 7: mx_copy_n(out, capacity, url, (size_t)(path - url)); break;            /* origin */
    case 8: {                                                                      /* port */
        const char* colon = host;
        while (colon < path && *colon != ':') colon++;
        if (colon < path) mx_copy_n(out, capacity, colon + 1, (size_t)(path - colon - 1));
        break;
    }
    }
}

static js_val location_get(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)self; (void)args; (void)argc;
    char out[512];
    url_part(callee->magic, out, sizeof(out));
    return js_str_val(js_cstr(out));
}

static js_val location_set(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)self;
    char target[1024];
    js_str* value = js_to_string(ARG(0));
    if (callee->magic == 6) {   /* hash */
        mx_copy(target, sizeof(target), page_url);
        char* h = strchr(target, '#');
        if (h) *h = '\0';
        if (value->data[0] != '#') mx_append(target, sizeof(target), "#");
        mx_append(target, sizeof(target), value->data);
        mx_copy(page_url, sizeof(page_url), target);
        return js_undef();
    }
    if (callee->magic == 5) {   /* search */
        mx_copy(target, sizeof(target), page_url);
        char* q = strchr(target, '?');
        if (!q) q = strchr(target, '#');
        if (q) *q = '\0';
        if (value->data[0] != '?') mx_append(target, sizeof(target), "?");
        mx_append(target, sizeof(target), value->data);
    } else {
        mx_copy(target, sizeof(target), value->data);
    }
    if (js_host.navigate) js_host.navigate(target, js_host.user);
    return js_undef();
}

static js_val location_method(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)self;
    if (callee->magic == 2) {   /* reload */
        if (js_host.navigate) js_host.navigate(page_url, js_host.user);
        return js_undef();
    }
    if (callee->magic == 3) return location_get(self, args, argc, callee);   /* toString */
    if (js_host.navigate) js_host.navigate(js_to_string(ARG(0))->data, js_host.user);
    return js_undef();
}

static js_val history_method(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)self;
    if (callee->magic == 0 && argc > 2 && ARG(2).type != JS_UNDEF && ARG(2).type != JS_NULL) {
        /* pushState/replaceState: update the address without loading */
        char resolved[512];
        mx_html_resolve(js_to_string(ARG(2))->data, resolved, sizeof(resolved));
        mx_copy(page_url, sizeof(page_url), resolved);
    }
    return js_undef();
}

/* storage and cookies live outside the JS heap so they survive page loads */

typedef struct store_entry {
    char* scope;
    char* key;
    char* value;
    struct store_entry* next;
} store_entry;

static store_entry* storage_list;
static store_entry* cookie_list;

static char* heap_strdup(const char* s) {
    size_t n = strlen(s);
    char* copy = (char*)malloc(n + 1);
    if (copy) memcpy(copy, s, n + 1);
    return copy;
}

static void origin_of_page(char* out, size_t capacity) {
    url_part(7, out, capacity);
}

static store_entry* store_find(store_entry* list, const char* scope, const char* key) {
    for (store_entry* e = list; e; e = e->next)
        if (!strcmp(e->scope, scope) && !strcmp(e->key, key)) return e;
    return NULL;
}

static void store_put(store_entry** list, const char* scope, const char* key, const char* value) {
    store_entry* e = store_find(*list, scope, key);
    if (e) {
        char* v = heap_strdup(value);
        if (!v) return;
        free(e->value);
        e->value = v;
        return;
    }
    e = (store_entry*)malloc(sizeof(store_entry));
    if (!e) return;
    e->scope = heap_strdup(scope);
    e->key = heap_strdup(key);
    e->value = heap_strdup(value);
    if (!e->scope || !e->key || !e->value) {
        free(e->scope);
        free(e->key);
        free(e->value);
        free(e);
        return;
    }
    e->next = *list;
    *list = e;
}

static void store_remove(store_entry** list, const char* scope, const char* key) {
    for (store_entry** link = list; *link; link = &(*link)->next) {
        store_entry* e = *link;
        if (!strcmp(e->scope, scope) && (!key || !strcmp(e->key, key))) {
            *link = e->next;
            free(e->scope);
            free(e->key);
            free(e->value);
            free(e);
            if (key) return;
            if (!*link) return;
        }
    }
}

static js_val storage_method(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)self;
    char origin[256];
    origin_of_page(origin, sizeof(origin));
    switch (callee->magic) {
    case 0: {   /* getItem */
        store_entry* e = store_find(storage_list, origin, js_to_string(ARG(0))->data);
        return e ? js_str_val(js_cstr(e->value)) : js_null();
    }
    case 1:   /* setItem */
        store_put(&storage_list, origin, js_to_string(ARG(0))->data, js_to_string(ARG(1))->data);
        return js_undef();
    case 2:   /* removeItem */
        store_remove(&storage_list, origin, js_to_string(ARG(0))->data);
        return js_undef();
    case 3:   /* clear */
        store_remove(&storage_list, origin, NULL);
        return js_undef();
    case 4: {   /* key */
        int want = (int)js_to_number(ARG(0)), index = 0;
        for (store_entry* e = storage_list; e; e = e->next)
            if (!strcmp(e->scope, origin) && index++ == want) return js_str_val(js_cstr(e->key));
        return js_null();
    }
    case 5: {   /* length */
        int count = 0;
        for (store_entry* e = storage_list; e; e = e->next) count += !strcmp(e->scope, origin);
        return js_num(count);
    }
    }
    return js_undef();
}

void mx_js_clear_storage(void) {
    while (storage_list) {
        store_entry* e = storage_list;
        storage_list = e->next;
        free(e->scope);
        free(e->key);
        free(e->value);
        free(e);
    }
    while (cookie_list) {
        store_entry* e = cookie_list;
        cookie_list = e->next;
        free(e->scope);
        free(e->key);
        free(e->value);
        free(e);
    }
}

js_val js_cookie_get(void);
js_val js_cookie_get(void) {
    char host[256];
    url_part(3, host, sizeof(host));
    char out[2048] = "";
    for (store_entry* e = cookie_list; e; e = e->next) {
        if (strcmp(e->scope, host)) continue;
        if (out[0]) mx_append(out, sizeof(out), "; ");
        mx_append(out, sizeof(out), e->key);
        mx_append(out, sizeof(out), "=");
        mx_append(out, sizeof(out), e->value);
    }
    return js_str_val(js_cstr(out));
}

void js_cookie_set(js_str* text);
void js_cookie_set(js_str* text) {
    char host[256];
    url_part(3, host, sizeof(host));
    const char* eq = strchr(text->data, '=');
    if (!eq) return;
    const char* end = strchr(text->data, ';');
    if (!end) end = text->data + text->length;
    char key[128], value[512];
    mx_copy_n(key, sizeof(key), text->data, (size_t)(eq - text->data));
    mx_copy_n(value, sizeof(value), eq + 1, (size_t)(end - eq - 1));
    char* k = key;
    while (*k == ' ') k++;
    if (strstr(text->data, "max-age=0") || strstr(text->data, "Max-Age=0") ||
        strstr(text->data, "1970"))
        store_remove(&cookie_list, host, k);
    else
        store_put(&cookie_list, host, k, value);
}

/* ---- network ---- */

static js_val make_response(const char* url, char* body, size_t length, int ok) {
    js_obj* r = js_new_object(js_object_proto);
    js_val rv = js_obj_val(r);
    js_str* text = js_new_str(body ? body : "", body ? length : 0);
    js_set_c(rv, "ok", js_bool(ok));
    js_set_c(rv, "status", js_num(ok ? 200 : 0));
    js_set_c(rv, "statusText", js_str_val(js_intern_c(ok ? "OK" : "")));
    js_set_c(rv, "url", js_str_val(js_cstr(url)));
    js_define(r, "\x01" "body", js_str_val(text), PROP_HIDDEN);
    js_obj* headers = js_new_object(js_object_proto);
    js_define_native(headers, "get", null_fn, 0);
    js_define_native(headers, "has", false_fn, 0);
    js_set_c(rv, "headers", js_obj_val(headers));
    return rv;
}

static js_val response_body(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)args; (void)argc;
    js_val body = js_get_c(self, "\x01" "body");
    if (callee->magic == 1) {
        js_val parse = js_get_c(js_get_c(js_obj_val(js_global), "JSON"), "parse");
        js_val parsed = js_call(parse, js_undef(), &body, 1);
        if (js_rt.ctl == CTL_THROW) {
            js_val error = js_rt.value;
            js_rt.ctl = CTL_NONE;
            return js_promise_resolved(error, 1);
        }
        return js_promise_resolved(parsed, 0);
    }
    return js_promise_resolved(body, 0);
}

static int network_get(const char* url, char** body, size_t* length) {
    if (!js_host.fetch) return 0;
    return js_host.fetch(js_host.user, url, "fetch", body, length);
}

static js_val win_fetch(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)self; (void)callee;
    js_str* url = ARG(0).type == JS_OBJ ? js_to_string(js_get_c(ARG(0), "url")) : js_to_string(ARG(0));
    char resolved[1024];
    mx_html_resolve(url->data, resolved, sizeof(resolved));
    if (ARG(1).type == JS_OBJ) {
        js_val method = js_get_c(ARG(1), "method");
        if (method.type == JS_STR && !mx_streq_nocase(method.u.str->data, "GET"))
            mx_console_add(MX_LOG_WARN, "fetch", js_rt.line, "Only GET requests are supported; sending GET.");
    }
    char* body = NULL;
    size_t length = 0;
    if (!network_get(resolved, &body, &length))
        return js_promise_resolved(js_obj_val(js_make_error("TypeError", "Failed to fetch")), 1);
    js_val response = make_response(resolved, body, length, 1);
    free(body);
    js_obj* r = response.u.obj;
    js_obj* text_fn = js_new_native("text", response_body, 0);
    text_fn->magic = 0;
    js_define(r, "text", js_obj_val(text_fn), PROP_HIDDEN);
    js_obj* json_fn = js_new_native("json", response_body, 0);
    json_fn->magic = 1;
    js_define(r, "json", js_obj_val(json_fn), PROP_HIDDEN);
    return js_promise_resolved(response, 0);
}

static js_val xhr_method(js_val self, js_val* args, int argc, js_obj* callee) {
    switch (callee->magic) {
    case 0: {   /* constructor */
        js_obj* x = js_new_object(js_object_proto);
        js_val xv = js_obj_val(x);
        js_set_c(xv, "readyState", js_num(0));
        js_set_c(xv, "status", js_num(0));
        js_set_c(xv, "responseText", js_str_val(js_intern_c("")));
        js_set_c(xv, "response", js_str_val(js_intern_c("")));
        static const char* const names[] = {"open", "send", "setRequestHeader", "abort",
                                            "getResponseHeader", "getAllResponseHeaders",
                                            "addEventListener", "overrideMimeType"};
        for (int i = 0; i < 8; i++) {
            js_obj* f = js_new_native(names[i], xhr_method, 1);
            f->magic = (int16_t)(i + 1);
            js_define(x, names[i], js_obj_val(f), PROP_HIDDEN);
        }
        return xv;
    }
    case 1:   /* open */
        js_set_c(self, "\x01url", js_str_val(js_to_string(ARG(1))));
        js_set_c(self, "readyState", js_num(1));
        return js_undef();
    case 2: {   /* send */
        char resolved[1024];
        mx_html_resolve(js_to_string(js_get_c(self, "\x01url"))->data, resolved, sizeof(resolved));
        char* body = NULL;
        size_t length = 0;
        int ok = network_get(resolved, &body, &length);
        js_str* text = js_new_str(body ? body : "", body ? length : 0);
        free(body);
        js_set_c(self, "readyState", js_num(4));
        js_set_c(self, "status", js_num(ok ? 200 : 0));
        js_set_c(self, "statusText", js_str_val(js_intern_c(ok ? "OK" : "")));
        js_set_c(self, "responseText", js_str_val(text));
        js_val type = js_get_c(self, "responseType");
        if (type.type == JS_STR && js_str_eq_c(type.u.str, "json")) {
            js_val parse = js_get_c(js_get_c(js_obj_val(js_global), "JSON"), "parse");
            js_val tv = js_str_val(text);
            js_val parsed = js_call(parse, js_undef(), &tv, 1);
            js_rt.ctl = CTL_NONE;
            js_set_c(self, "response", parsed);
        } else {
            js_set_c(self, "response", js_str_val(text));
        }
        static const char* const handlers[] = {"onreadystatechange", "onload", "onloadend"};
        for (int i = 0; i < 3; i++) {
            js_val h = js_get_c(self, handlers[i]);
            if (!ok && i == 1) h = js_get_c(self, "onerror");
            if (js_is_callable(h)) js_call(h, self, NULL, 0);
            if (js_rt.ctl) return js_undef();
        }
        js_val listeners = js_get_c(self, "\x01listeners");
        if (listeners.type == JS_OBJ)
            for (uint32_t i = 0; i < listeners.u.obj->length; i++) {
                js_val pair = listeners.u.obj->items[i];
                js_str* t = js_to_string(js_get_index(pair, js_num(0)));
                if (js_str_eq_c(t, ok ? "load" : "error") || js_str_eq_c(t, "loadend") ||
                    js_str_eq_c(t, "readystatechange"))
                    js_call(js_get_index(pair, js_num(1)), self, NULL, 0);
            }
        return js_undef();
    }
    case 7: {   /* addEventListener */
        js_val listeners = js_get_c(self, "\x01listeners");
        if (listeners.type != JS_OBJ) {
            listeners = js_obj_val(js_new_array(0));
            js_define(self.u.obj, "\x01listeners", listeners, PROP_HIDDEN);
        }
        js_obj* pair = js_new_array(2);
        js_array_push(pair, ARG(0));
        js_array_push(pair, ARG(1));
        js_array_push(listeners.u.obj, js_obj_val(pair));
        return js_undef();
    }
    case 5: return js_null();
    case 6: return js_str_val(js_intern_c(""));
    default: return js_undef();
    }
}

/* ---- observers ---- */

static js_val observer_method(js_val self, js_val* args, int argc, js_obj* callee) {
    switch (callee->magic) {
    case 0: {   /* constructor */
        js_obj* o = js_new_object(js_object_proto);
        js_define(o, "\x01" "callback", ARG(0), PROP_HIDDEN);
        static const char* const names[] = {"observe", "unobserve", "disconnect", "takeRecords"};
        for (int i = 0; i < 4; i++) {
            js_obj* f = js_new_native(names[i], observer_method, 1);
            f->magic = (int16_t)(i == 0 ? (callee->data ? 5 : 1) : 2);
            js_define(o, names[i], js_obj_val(f), PROP_HIDDEN);
        }
        return js_obj_val(o);
    }
    case 5: {   /* IntersectionObserver.observe: report the element as visible */
        js_val cb = js_get_c(self, "\x01" "callback");
        if (!js_is_callable(cb)) return js_undef();
        js_obj* entry = js_new_object(js_object_proto);
        js_set_c(js_obj_val(entry), "target", ARG(0));
        js_set_c(js_obj_val(entry), "isIntersecting", js_bool(1));
        js_set_c(js_obj_val(entry), "intersectionRatio", js_num(1));
        js_obj* list = js_new_array(1);
        js_array_push(list, js_obj_val(entry));
        js_obj* call = js_new_native("observe-callback", observer_method, 2);
        call->magic = 6;
        js_define(call, "\x01" "cb", cb, PROP_HIDDEN);
        js_define(call, "\x01self", self, PROP_HIDDEN);
        js_enqueue_microtask(js_obj_val(call), js_obj_val(list));
        return js_undef();
    }
    case 6: {   /* deferred callback */
        js_val cb = js_get_c(js_obj_val(callee), "\x01" "cb");
        js_val observer = js_get_c(js_obj_val(callee), "\x01self");
        js_val pair[2] = {ARG(0), observer};
        return js_call(cb, observer, pair, 2);
    }
    case 2: return js_obj_val(js_new_array(0));
    default: return js_undef();
    }
}

/* URL and URLSearchParams (simplified) */

static js_val url_to_string(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)args; (void)argc; (void)callee;
    return js_get_c(self, "href");
}

static js_val url_ctor(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)self; (void)callee;
    char resolved[1024];
    js_str* input = js_to_string(ARG(0));
    if (argc > 1) {
        js_str* base = js_to_string(ARG(1));
        if (!mx_url_resolve(base->data, input->data, resolved, sizeof(resolved)))
            mx_copy(resolved, sizeof(resolved), input->data);
    } else if (!strstr(input->data, ":")) {
        return js_throw_error("TypeError", "Failed to construct 'URL': Invalid URL");
    } else {
        mx_copy(resolved, sizeof(resolved), input->data);
    }
    char saved[512];
    mx_copy(saved, sizeof(saved), page_url);
    mx_copy(page_url, sizeof(page_url), resolved);
    js_obj* u = js_new_object(js_object_proto);
    static const char* const names[] = {"href", "protocol", "host", "hostname", "pathname",
                                        "search", "hash", "origin", "port"};
    for (int i = 0; i < 9; i++) {
        char part[512];
        url_part(i, part, sizeof(part));
        js_set_c(js_obj_val(u), names[i], js_str_val(js_cstr(part)));
    }
    mx_copy(page_url, sizeof(page_url), saved);
    js_val params_ctor = js_get_c(js_obj_val(js_global), "URLSearchParams");
    js_val search = js_get_c(js_obj_val(u), "search");
    js_set_c(js_obj_val(u), "searchParams", js_construct(params_ctor, &search, 1));
    js_define_native(u, "toString", url_to_string, 0);
    js_define_native(u, "toJSON", url_to_string, 0);
    return js_obj_val(u);
}

static js_val params_method(js_val self, js_val* args, int argc, js_obj* callee) {
    switch (callee->magic) {
    case 0: {   /* constructor */
        js_obj* p = js_new_object(js_object_proto);
        js_obj* keys = js_new_array(0);
        js_obj* values = js_new_array(0);
        js_define(p, "\x01k", js_obj_val(keys), PROP_HIDDEN);
        js_define(p, "\x01v", js_obj_val(values), PROP_HIDDEN);
        js_str* q = ARG(0).type == JS_UNDEF ? js_intern_c("") : js_to_string(ARG(0));
        const char* s = q->data;
        if (*s == '?') s++;
        while (*s) {
            const char* amp = strchr(s, '&');
            const char* end = amp ? amp : s + strlen(s);
            const char* eq = mx_memchr(s, '=', (size_t)(end - s));
            js_val decode = js_get_c(js_obj_val(js_global), "decodeURIComponent");
            js_val k = js_str_val(js_new_str(s, (size_t)((eq ? eq : end) - s)));
            js_val v = js_str_val(eq ? js_new_str(eq + 1, (size_t)(end - eq - 1)) : js_intern_c(""));
            if (end > s) {
                js_array_push(keys, js_call(decode, js_undef(), &k, 1));
                js_array_push(values, js_call(decode, js_undef(), &v, 1));
            }
            s = amp ? amp + 1 : end;
        }
        static const char* const names[] = {"get", "set", "has", "append", "delete", "toString",
                                            "forEach", "getAll"};
        for (int i = 0; i < 8; i++) {
            js_obj* f = js_new_native(names[i], params_method, 1);
            f->magic = (int16_t)(i + 1);
            js_define(p, names[i], js_obj_val(f), PROP_HIDDEN);
        }
        return js_obj_val(p);
    }
    default: break;
    }
    js_obj* keys = js_get_c(self, "\x01k").u.obj;
    js_obj* values = js_get_c(self, "\x01v").u.obj;
    if (!keys || !values) return js_undef();
    js_str* name = js_to_string(ARG(0));
    switch (callee->magic) {
    case 1:   /* get */
        for (uint32_t i = 0; i < keys->length; i++)
            if (js_str_eq(keys->items[i].u.str, name)) return values->items[i];
        return js_null();
    case 2:   /* set */
        for (uint32_t i = 0; i < keys->length; i++)
            if (js_str_eq(keys->items[i].u.str, name)) {
                values->items[i] = js_str_val(js_to_string(ARG(1)));
                return js_undef();
            }
        js_array_push(keys, js_str_val(name));
        js_array_push(values, js_str_val(js_to_string(ARG(1))));
        return js_undef();
    case 3:
        for (uint32_t i = 0; i < keys->length; i++)
            if (js_str_eq(keys->items[i].u.str, name)) return js_bool(1);
        return js_bool(0);
    case 4:   /* append */
        js_array_push(keys, js_str_val(name));
        js_array_push(values, js_str_val(js_to_string(ARG(1))));
        return js_undef();
    case 5:   /* delete */
        for (uint32_t i = 0; i < keys->length; i++)
            if (js_str_eq(keys->items[i].u.str, name)) {
                memmove(keys->items + i, keys->items + i + 1, sizeof(js_val) * (keys->length - i - 1));
                memmove(values->items + i, values->items + i + 1, sizeof(js_val) * (values->length - i - 1));
                keys->length--;
                values->length--;
                i--;
            }
        return js_undef();
    case 6: {   /* toString */
        js_val encode = js_get_c(js_obj_val(js_global), "encodeURIComponent");
        js_str* out = js_intern_c("");
        for (uint32_t i = 0; i < keys->length; i++) {
            if (i) out = js_concat(out, js_intern_c("&"));
            out = js_concat(out, js_to_string(js_call(encode, js_undef(), &keys->items[i], 1)));
            out = js_concat(out, js_intern_c("="));
            out = js_concat(out, js_to_string(js_call(encode, js_undef(), &values->items[i], 1)));
        }
        return js_str_val(out);
    }
    case 7:   /* forEach */
        for (uint32_t i = 0; i < keys->length && !js_rt.ctl; i++) {
            js_val call_args[2] = {values->items[i], keys->items[i]};
            js_call(ARG(0), js_undef(), call_args, 2);
        }
        return js_undef();
    case 8: {   /* getAll */
        js_obj* out = js_new_array(0);
        for (uint32_t i = 0; i < keys->length; i++)
            if (js_str_eq(keys->items[i].u.str, name)) js_array_push(out, values->items[i]);
        return js_obj_val(out);
    }
    }
    return js_undef();
}

static js_val abort_method(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)args; (void)argc;
    switch (callee->magic) {
    case 0: {   /* new AbortController() */
        js_obj* c = js_new_object(js_object_proto);
        js_obj* signal = js_new_object(node_proto);
        js_set_c(js_obj_val(signal), "aborted", js_bool(0));
        js_set_c(js_obj_val(signal), "reason", js_undef());
        js_set_c(js_obj_val(signal), "onabort", js_null());
        js_set_c(js_obj_val(c), "signal", js_obj_val(signal));
        js_obj* abort = js_new_native("abort", abort_method, 0);
        abort->magic = 1;
        js_define(c, "abort", js_obj_val(abort), PROP_HIDDEN);
        return js_obj_val(c);
    }
    case 1: {   /* controller.abort() */
        js_val signal = js_get_c(self, "signal");
        if (signal.type == JS_OBJ) {
            js_set_c(signal, "aborted", js_bool(1));
            js_val h = js_get_c(signal, "onabort");
            if (js_is_callable(h)) js_call(h, signal, NULL, 0);
        }
        return js_undef();
    }
    case 2: {   /* AbortSignal.timeout() / abort(): a signal object */
        js_obj* signal = js_new_object(node_proto);
        js_set_c(js_obj_val(signal), "aborted", js_bool(0));
        return js_obj_val(signal);
    }
    }
    return js_undef();
}

static js_val crypto_random(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)self;
    if (callee->magic == 1) {   /* randomUUID */
        char uuid[37];
        static const char hex[] = "0123456789abcdef";
        js_val rnd = js_call(js_get_c(js_get_c(js_obj_val(js_global), "Math"), "random"), js_undef(), NULL, 0);
        (void)rnd;
        for (int i = 0; i < 36; i++) {
            js_val r = js_call(js_get_c(js_get_c(js_obj_val(js_global), "Math"), "random"), js_undef(), NULL, 0);
            uuid[i] = (i == 8 || i == 13 || i == 18 || i == 23) ? '-' : i == 14 ? '4' : hex[(int)(r.u.num * 16) & 15];
        }
        uuid[36] = '\0';
        return js_str_val(js_cstr(uuid));
    }
    js_val arr = ARG(0);
    if (arr.type == JS_OBJ && arr.u.obj->cls == CLS_ARRAY)
        for (uint32_t i = 0; i < arr.u.obj->length; i++) {
            js_val r = js_call(js_get_c(js_get_c(js_obj_val(js_global), "Math"), "random"), js_undef(), NULL, 0);
            arr.u.obj->items[i] = js_num((double)(uint32_t)(r.u.num * 4294967296.0));
        }
    return arr;
}

static js_val structured_clone(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)self; (void)callee;
    js_val json = js_get_c(js_obj_val(js_global), "JSON");
    js_val text = js_call(js_get_c(json, "stringify"), js_undef(), args, argc > 0 ? 1 : 0);
    if (text.type != JS_STR) return js_undef();
    return js_call(js_get_c(json, "parse"), js_undef(), &text, 1);
}

static js_val image_ctor(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)self; (void)args; (void)argc;
    return js_wrap_node(mx_dom_create_element(js_doc, callee->magic ? "option" : "img"));
}


/* ---- dynamic scripts ---- */

static void run_script_node(mx_node_t* script) {
    if (script->checked) return;
    script->checked = 1;
    const char* type = mx_dom_attr(script, "type");
    if (type && *type && !mx_streq_nocase(type, "text/javascript") &&
        !mx_streq_nocase(type, "application/javascript") && !mx_streq_nocase(type, "module"))
        return;
    const char* src = mx_dom_attr(script, "src");
    if (src && *src) {
        char url[1024];
        mx_html_resolve(src, url, sizeof(url));
        char* body = NULL;
        size_t length = 0;
        if (!js_host.fetch || !js_host.fetch(js_host.user, url, "script", &body, &length)) {
            mx_js_dispatch(script, "error", NULL);
            return;
        }
        const char* base = strrchr(url, '/');
        char name[48];
        mx_copy(name, sizeof(name), base && base[1] ? base + 1 : url);
        const char* saved = js_rt.source;
        mx_js_run(body, length, js_intern_c(name)->data);
        js_rt.source = saved;
        free(body);
        mx_js_dispatch(script, "load", NULL);
        return;
    }
    char* text = mx_dom_text_content(js_doc, script);
    if (*text) {
        const char* saved = js_rt.source;
        mx_js_run(text, strlen(text), "inserted script");
        js_rt.source = saved;
    }
}

static void run_inserted_scripts(mx_node_t* root) {
    if (!root || root->type != MX_NODE_ELEMENT) return;
    int connected = 0;
    for (mx_node_t* p = root; p; p = p->parent)
        if (p == js_doc->root) connected = 1;
    if (!connected) return;
    for (mx_node_t* n = root; n; n = mx_dom_next(root, n))
        if (mx_dom_is(n, "script")) run_script_node(n);
}

/* ====================================================================== */
/* Setup                                                                  */
/* ====================================================================== */

static void method(js_obj* o, const char* name, js_native fn, int magic) {
    js_obj* f = js_new_native(name, fn, 1);
    f->magic = (int16_t)magic;
    js_define(o, name, js_obj_val(f), PROP_HIDDEN);
}

static js_obj* global_ctor(const char* name, js_native fn, int magic, js_obj* proto) {
    js_obj* f = js_new_native(name, fn, 1);
    f->magic = (int16_t)magic;
    if (proto) {
        js_define(f, "prototype", js_obj_val(proto), PROP_HIDDEN);
        if (!js_find_own(proto, js_intern_c("constructor")))
            js_define(proto, "constructor", js_obj_val(f), PROP_HIDDEN);
    }
    js_define(js_global, name, js_obj_val(f), PROP_HIDDEN);
    return f;
}

static js_val illegal_ctor(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)self; (void)args; (void)argc; (void)callee;
    return js_throw_error("TypeError", "Illegal constructor");
}

void js_dom_init(void) {
    for (int i = 0; i < G_COUNT; i++) getter_keys[i] = js_intern_c(getter_names[i]);
    build_getter_table();
    window_listeners = NULL;
    memset(timers, 0, sizeof(timers));

    node_proto = js_new_object(js_object_proto);
    element_proto = js_new_object(node_proto);
    text_proto = js_new_object(node_proto);
    document_proto = js_new_object(node_proto);
    style_proto = js_new_object(js_object_proto);
    classlist_proto = js_new_object(js_object_proto);
    event_proto = js_new_object(js_object_proto);

    /* Node and Element methods */
    js_obj* np = node_proto;
    method(np, "appendChild", el_append_child, 0);
    method(np, "removeChild", el_remove_child, 0);
    method(np, "insertBefore", el_insert_before, 0);
    method(np, "replaceChild", el_replace_child, 0);
    method(np, "cloneNode", el_clone, 0);
    method(np, "contains", el_contains, 0);
    method(np, "hasChildNodes", el_has_child_nodes, 0);
    method(np, "addEventListener", el_add_listener, 0);
    method(np, "removeEventListener", el_add_listener, 1);
    method(np, "dispatchEvent", el_dispatch_event, 0);
    method(np, "remove", el_remove, 0);
    method(np, "append", el_append, 0);
    method(np, "prepend", el_append, 1);
    method(np, "before", el_append, 2);
    method(np, "after", el_append, 3);
    method(np, "replaceWith", el_replace_with, 0);
    method(np, "replaceChildren", el_replace_with, 1);
    method(np, "querySelector", el_query, 0);
    method(np, "querySelectorAll", el_query, 1);
    method(np, "getElementsByClassName", el_query, 2);
    method(np, "getElementsByTagName", el_query, 3);
    method(np, "normalize", noop, 0);
    js_obj* ep = element_proto;
    method(ep, "getAttribute", el_attribute, 0);
    method(ep, "setAttribute", el_attribute, 1);
    method(ep, "removeAttribute", el_attribute, 2);
    method(ep, "hasAttribute", el_attribute, 3);
    method(ep, "toggleAttribute", el_attribute, 4);
    method(ep, "getAttributeNames", el_attribute, 5);
    method(ep, "setAttributeNS", el_attribute, 1);
    method(ep, "getAttributeNS", el_attribute, 0);
    method(ep, "hasAttributes", el_has_child_nodes, 1);
    method(ep, "matches", el_query, 6);
    method(ep, "webkitMatchesSelector", el_query, 6);
    method(ep, "closest", el_query, 7);
    method(ep, "insertAdjacentHTML", el_insert_adjacent, 0);
    method(ep, "insertAdjacentElement", el_insert_adjacent, 1);
    method(ep, "insertAdjacentText", el_insert_adjacent, 2);
    method(ep, "getBoundingClientRect", el_geometry, 0);
    method(ep, "getClientRects", el_geometry, 1);
    method(ep, "scrollIntoView", el_geometry, 2);
    method(ep, "click", el_misc, 0);
    method(ep, "focus", el_misc, 1);
    method(ep, "blur", el_misc, 1);
    method(ep, "select", el_misc, 1);
    method(ep, "setSelectionRange", el_misc, 1);
    method(ep, "scrollTo", el_misc, 1);
    method(ep, "scrollBy", el_misc, 1);
    method(ep, "setPointerCapture", el_misc, 1);
    method(ep, "releasePointerCapture", el_misc, 1);
    method(ep, "requestFullscreen", el_misc, 7);
    method(ep, "checkValidity", el_misc, 2);
    method(ep, "reportValidity", el_misc, 2);
    method(ep, "animate", el_misc, 3);
    method(ep, "attachShadow", el_misc, 4);
    method(ep, "show", el_misc, 5);
    method(ep, "showModal", el_misc, 5);
    method(ep, "close", el_misc, 6);
    method(ep, "play", el_misc, 7);
    method(ep, "pause", el_misc, 1);
    method(ep, "load", el_misc, 1);
    method(ep, "submit", el_misc, 8);
    method(ep, "requestSubmit", el_misc, 8);
    method(ep, "reset", el_misc, 1);
    method(ep, "getContext", el_misc, 9);
    js_obj* dp = document_proto;
    method(dp, "getElementById", el_query, 4);
    method(dp, "getElementsByName", el_query, 5);
    method(dp, "createElement", doc_create, 0);
    method(dp, "createTextNode", doc_create, 1);
    method(dp, "createComment", doc_create, 2);
    method(dp, "createDocumentFragment", doc_create, 3);
    method(dp, "createElementNS", doc_create, 4);
    method(dp, "createEvent", create_event, 0);
    method(dp, "write", doc_write, 0);
    method(dp, "writeln", doc_write, 0);
    method(dp, "open", noop, 0);
    method(dp, "close", noop, 0);
    method(dp, "hasFocus", false_fn, 0);
    method(dp, "execCommand", false_fn, 0);
    method(dp, "getSelection", noop, 0);

    method(style_proto, "setProperty", style_method, 0);
    method(style_proto, "getPropertyValue", style_method, 1);
    method(style_proto, "removeProperty", style_method, 2);
    static const char* const classlist_names[] = {"add", "remove", "toggle", "contains",
                                                  "replace", "item", "forEach"};
    for (int i = 0; i < 7; i++) method(classlist_proto, classlist_names[i], classlist_method, i);
    method(event_proto, "preventDefault", event_method, 0);
    method(event_proto, "stopPropagation", event_method, 1);
    method(event_proto, "stopImmediatePropagation", event_method, 2);
    method(event_proto, "composedPath", event_method, 3);
    method(event_proto, "initEvent", event_init, 0);
    method(event_proto, "initCustomEvent", event_init, 0);
    js_val at = js_get_c(js_obj_val(js_array_proto), "at");
    if (at.type == JS_OBJ) method(js_array_proto, "item", at.u.obj->native, 0);

    /* constructors for instanceof checks */
    static const char* const element_types[] = {
        "Element", "HTMLElement", "HTMLDivElement", "HTMLInputElement", "HTMLAnchorElement",
        "HTMLButtonElement", "HTMLFormElement", "HTMLImageElement", "HTMLSelectElement",
        "HTMLTextAreaElement", "HTMLScriptElement", "HTMLCanvasElement", "HTMLSpanElement",
        "HTMLBodyElement", "HTMLIFrameElement", "HTMLTemplateElement", "SVGElement",
        "HTMLMediaElement", "HTMLVideoElement", "HTMLLinkElement", "HTMLStyleElement"};
    for (size_t i = 0; i < sizeof(element_types) / sizeof(element_types[0]); i++)
        global_ctor(element_types[i], illegal_ctor, 0, element_proto);
    global_ctor("Node", illegal_ctor, 0, node_proto);
    global_ctor("Text", illegal_ctor, 0, text_proto);
    global_ctor("CharacterData", illegal_ctor, 0, text_proto);
    global_ctor("Document", illegal_ctor, 0, document_proto);
    global_ctor("HTMLDocument", illegal_ctor, 0, document_proto);
    global_ctor("DocumentFragment", illegal_ctor, 0, element_proto);
    global_ctor("EventTarget", illegal_ctor, 0, node_proto);
    global_ctor("CSSStyleDeclaration", illegal_ctor, 0, style_proto);
    global_ctor("DOMTokenList", illegal_ctor, 0, classlist_proto);
    static const char* const event_types[] = {"Event", "CustomEvent", "KeyboardEvent",
                                              "MouseEvent", "UIEvent", "FocusEvent",
                                              "InputEvent", "PointerEvent", "TouchEvent",
                                              "WheelEvent", "SubmitEvent", "ErrorEvent",
                                              "PopStateEvent", "HashChangeEvent", "MessageEvent"};
    for (size_t i = 0; i < sizeof(event_types) / sizeof(event_types[0]); i++)
        global_ctor(event_types[i], event_ctor, 0, event_proto);
    global_ctor("NodeList", illegal_ctor, 0, js_array_proto);
    global_ctor("HTMLCollection", illegal_ctor, 0, js_array_proto);

    /* window */
    js_obj* w = js_global;
    js_val wv = js_obj_val(w);
    js_define(w, "window", wv, PROP_HIDDEN);
    js_define(w, "self", wv, PROP_HIDDEN);
    js_define(w, "globalThis", wv, PROP_HIDDEN);
    js_define(w, "top", wv, PROP_HIDDEN);
    js_define(w, "parent", wv, PROP_HIDDEN);
    js_define(w, "frames", wv, PROP_HIDDEN);
    js_define(w, "opener", js_null(), PROP_HIDDEN);
    js_define(w, "frameElement", js_null(), PROP_HIDDEN);
    js_define(w, "devicePixelRatio", js_num(1), PROP_HIDDEN);
    js_define(w, "isSecureContext", js_bool(0), PROP_HIDDEN);
    js_define(w, "name", js_str_val(js_intern_c("")), 0);
    method(w, "addEventListener", el_add_listener, 0);
    method(w, "removeEventListener", el_add_listener, 1);
    method(w, "dispatchEvent", el_dispatch_event, 0);
    method(w, "setTimeout", set_timer, 0);
    method(w, "setInterval", set_timer, 1);
    method(w, "requestAnimationFrame", set_timer, 2);
    method(w, "requestIdleCallback", set_timer, 0);
    method(w, "clearTimeout", clear_timer, 0);
    method(w, "clearInterval", clear_timer, 0);
    method(w, "cancelAnimationFrame", clear_timer, 0);
    method(w, "cancelIdleCallback", clear_timer, 0);
    method(w, "alert", win_alert, 0);
    method(w, "confirm", win_alert, 1);
    method(w, "prompt", win_alert, 2);
    method(w, "scrollTo", win_scroll, 0);
    method(w, "scroll", win_scroll, 0);
    method(w, "scrollBy", win_scroll, 1);
    method(w, "getComputedStyle", win_get_computed_style, 0);
    method(w, "matchMedia", win_match_media, 0);
    method(w, "fetch", win_fetch, 0);
    method(w, "structuredClone", structured_clone, 0);
    method(w, "open", noop, 0);
    method(w, "close", noop, 0);
    method(w, "focus", noop, 0);
    method(w, "blur", noop, 0);
    method(w, "print", noop, 0);
    method(w, "postMessage", noop, 0);
    method(w, "getSelection", noop, 0);
    js_define_accessor(w, "innerWidth", win_get, NULL);
    js_find_own(w, js_intern_c("innerWidth"))->value.u.obj->magic = 0;
    js_define_accessor(w, "outerWidth", win_get, NULL);
    js_find_own(w, js_intern_c("outerWidth"))->value.u.obj->magic = 0;
    js_define_accessor(w, "innerHeight", win_get, NULL);
    js_find_own(w, js_intern_c("innerHeight"))->value.u.obj->magic = 1;
    js_define_accessor(w, "outerHeight", win_get, NULL);
    js_find_own(w, js_intern_c("outerHeight"))->value.u.obj->magic = 1;
    static const char* const scroll_y_names[] = {"scrollY", "pageYOffset"};
    for (int i = 0; i < 2; i++) {
        js_define_accessor(w, scroll_y_names[i], win_get, NULL);
        js_find_own(w, js_intern_c(scroll_y_names[i]))->value.u.obj->magic = 2;
    }
    static const char* const scroll_x_names[] = {"scrollX", "pageXOffset"};
    for (int i = 0; i < 2; i++) {
        js_define_accessor(w, scroll_x_names[i], win_get, NULL);
        js_find_own(w, js_intern_c(scroll_x_names[i]))->value.u.obj->magic = 3;
    }

    document_val = js_wrap_node(js_doc->root);
    js_define(w, "document", document_val, PROP_HIDDEN);

    /* location */
    location_obj = js_new_object(js_object_proto);
    static const char* const location_parts[] = {"href", "protocol", "host", "hostname",
                                                 "pathname", "search", "hash", "origin", "port"};
    for (int i = 0; i < 9; i++) {
        js_define_accessor(location_obj, location_parts[i], location_get, location_set);
        js_prop* p = js_find_own(location_obj, js_intern_c(location_parts[i]));
        p->value.u.obj->magic = (int16_t)i;
        p->setter->magic = (int16_t)i;
        p->flags = PROP_ACCESSOR;
    }
    method(location_obj, "assign", location_method, 0);
    method(location_obj, "replace", location_method, 1);
    method(location_obj, "reload", location_method, 2);
    method(location_obj, "toString", location_method, 3);
    js_define(w, "location", js_obj_val(location_obj), PROP_HIDDEN);

    js_obj* history = js_new_object(js_object_proto);
    method(history, "pushState", history_method, 0);
    method(history, "replaceState", history_method, 0);
    method(history, "back", history_method, 1);
    method(history, "forward", history_method, 1);
    method(history, "go", history_method, 1);
    js_define(history, "length", js_num(1), 0);
    js_define(history, "state", js_null(), 0);
    js_define(w, "history", js_obj_val(history), PROP_HIDDEN);

    js_obj* navigator = js_new_object(js_object_proto);
    js_val nv = js_obj_val(navigator);
    js_set_c(nv, "userAgent", js_str_val(js_intern_c("Mozilla/5.0 (Minimal-OS; x86_64) Mxplorer/0.3")));
    js_set_c(nv, "appName", js_str_val(js_intern_c("Netscape")));
    js_set_c(nv, "appVersion", js_str_val(js_intern_c("5.0 (Minimal-OS)")));
    js_set_c(nv, "platform", js_str_val(js_intern_c("Minimal-OS")));
    js_set_c(nv, "vendor", js_str_val(js_intern_c("Minimal-OS")));
    js_set_c(nv, "language", js_str_val(js_intern_c("en-US")));
    js_obj* langs = js_new_array(1);
    js_array_push(langs, js_str_val(js_intern_c("en-US")));
    js_set_c(nv, "languages", js_obj_val(langs));
    js_set_c(nv, "onLine", js_bool(1));
    js_set_c(nv, "cookieEnabled", js_bool(1));
    js_set_c(nv, "hardwareConcurrency", js_num(1));
    js_set_c(nv, "maxTouchPoints", js_num(0));
    js_set_c(nv, "webdriver", js_bool(0));
    method(navigator, "sendBeacon", false_fn, 0);
    js_define(w, "navigator", nv, PROP_HIDDEN);

    js_obj* screen = js_new_object(js_object_proto);
    js_define_accessor(screen, "width", win_get, NULL);
    js_find_own(screen, js_intern_c("width"))->value.u.obj->magic = 0;
    js_define_accessor(screen, "height", win_get, NULL);
    js_find_own(screen, js_intern_c("height"))->value.u.obj->magic = 1;
    js_set_c(js_obj_val(screen), "colorDepth", js_num(24));
    js_define(w, "screen", js_obj_val(screen), PROP_HIDDEN);

    js_obj* performance = js_new_object(js_object_proto);
    method(performance, "now", performance_now, 0);
    method(performance, "mark", noop, 0);
    method(performance, "measure", noop, 0);
    js_define(performance, "timeOrigin", js_num(0), 0);
    js_define(w, "performance", js_obj_val(performance), PROP_HIDDEN);

    js_obj* storage = js_new_object(js_object_proto);
    static const char* const storage_names[] = {"getItem", "setItem", "removeItem", "clear", "key"};
    for (int i = 0; i < 5; i++) method(storage, storage_names[i], storage_method, i);
    js_define_accessor(storage, "length", storage_method, NULL);
    js_find_own(storage, js_intern_c("length"))->value.u.obj->magic = 5;
    js_define(w, "localStorage", js_obj_val(storage), PROP_HIDDEN);
    js_define(w, "sessionStorage", js_obj_val(storage), PROP_HIDDEN);

    js_obj* xhr = global_ctor("XMLHttpRequest", xhr_method, 0, NULL);
    (void)xhr;
    js_obj* mo = global_ctor("MutationObserver", observer_method, 0, NULL);
    (void)mo;
    js_obj* io = global_ctor("IntersectionObserver", observer_method, 0, NULL);
    io->data = io;   /* marks the intersection variant */
    global_ctor("ResizeObserver", observer_method, 0, NULL);
    global_ctor("PerformanceObserver", observer_method, 0, NULL);
    global_ctor("Image", image_ctor, 0, element_proto);
    global_ctor("Option", image_ctor, 1, element_proto);
    global_ctor("URL", url_ctor, 0, NULL);
    global_ctor("URLSearchParams", params_method, 0, NULL);

    global_ctor("AbortController", abort_method, 0, NULL);
    js_obj* abort_signal = global_ctor("AbortSignal", abort_method, 2, NULL);
    method(abort_signal, "timeout", abort_method, 2);
    method(abort_signal, "abort", abort_method, 2);
    method(abort_signal, "any", abort_method, 2);
    js_obj* crypto = js_new_object(js_object_proto);
    method(crypto, "getRandomValues", crypto_random, 0);
    method(crypto, "randomUUID", crypto_random, 1);
    js_define(w, "crypto", js_obj_val(crypto), PROP_HIDDEN);

    js_obj* custom_elements = js_new_object(js_object_proto);
    method(custom_elements, "define", noop, 0);
    method(custom_elements, "get", noop, 0);
    method(custom_elements, "whenDefined", noop, 0);
    js_define(w, "customElements", js_obj_val(custom_elements), PROP_HIDDEN);
    js_obj* css = js_new_object(js_object_proto);
    method(css, "supports", false_fn, 0);
    js_define(w, "CSS", js_obj_val(css), PROP_HIDDEN);
}

/* ====================================================================== */
/* Public API                                                             */
/* ====================================================================== */

static int active;

void mx_js_begin(mx_document_t* doc, const char* url, const mx_js_host_t* host) {
    mx_js_end();
    memset(&js_rt, 0, sizeof(js_rt));
    js_heap.limit = 48u * 1024u * 1024u;
    js_doc = doc;
    js_host = *host;
    mx_copy(page_url, sizeof(page_url), url ? url : "");
    js_core_init();
    js_runtime_init();
    js_dom_init();
    active = 1;
}

void mx_js_end(void) {
    if (!active) return;
    for (mx_node_t* n = js_doc ? js_doc->root : NULL; n; n = mx_dom_next(js_doc->root, n)) {
        n->js_wrapper = NULL;
        n->listeners = NULL;
    }
    mx_arena_free_all(&js_heap);
    memset(&js_rt, 0, sizeof(js_rt));
    js_doc = NULL;
    js_global = NULL;
    window_listeners = NULL;
    memset(timers, 0, sizeof(timers));
    active = 0;
}

int mx_js_active(void) {
    return active;
}

size_t mx_js_heap_bytes(void) {
    return js_heap.total;
}

const char* js_page_url(void);
const char* js_page_url(void) {
    return page_url;
}

int js_fetch_source(const char* url, char** body, size_t* length) {
    if (!js_host.fetch) return 0;
    return js_host.fetch(js_host.user, url, "script", body, length);
}

static int run_module_direct(const char* source, size_t length, const char* url,
                             const char* name) {
    if (!active) return 0;
    char marker;
    enter(&marker);
    js_run_module_source(source, length, url, name ? name : "module");
    int ok = js_rt.ctl != CTL_THROW;
    leave();
    return ok;
}

static int run_direct(const char* source, size_t length, const char* name) {
    if (!active) return 0;
    char marker;
    char error[200];
    int line = 0;
    const char* source_name = js_intern_c(name ? name : "script")->data;
    js_node* program = js_parse(source, length, source_name, error, sizeof(error), &line);
    if (!program) {
        char message[260];
        mx_copy(message, sizeof(message), "Uncaught SyntaxError: ");
        mx_append(message, sizeof(message), error);
        mx_console_add(MX_LOG_ERROR, source_name, line, message);
        return 0;
    }
    enter(&marker);
    js_rt.source = source_name;
    js_exec_program(program);
    int ok = js_rt.ctl != CTL_THROW;
    leave();
    return ok;
}

static int eval_direct(const char* source, char* result, size_t capacity) {
    if (!active) {
        mx_copy(result, capacity, "JavaScript is not running on this page.");
        return 0;
    }
    char marker;
    char error[200];
    int line = 0;
    /* `{a: 1}` typed at the console means an object */
    js_node* program = NULL;
    size_t length = strlen(source);
    const char* p = source;
    while (mx_is_space(*p)) p++;
    if (*p == '{') {
        char* wrapped = (char*)malloc(length + 3);
        if (wrapped) {
            wrapped[0] = '(';
            memcpy(wrapped + 1, source, length);
            wrapped[length + 1] = ')';
            wrapped[length + 2] = '\0';
            program = js_parse(wrapped, length + 2, "console", error, sizeof(error), &line);
            free(wrapped);
        }
    }
    if (!program) program = js_parse(source, length, "console", error, sizeof(error), &line);
    if (!program) {
        mx_copy(result, capacity, "SyntaxError: ");
        mx_append(result, capacity, error);
        return 0;
    }
    enter(&marker);
    js_rt.source = "console";
    js_val v = js_exec_program(program);
    int ok = 1;
    if (js_rt.ctl == CTL_THROW) {
        char body[MX_CONSOLE_TEXT];
        js_rt.ctl = CTL_NONE;
        js_inspect(js_rt.value, body, sizeof(body), 0, 1);
        mx_copy(result, capacity, "Uncaught ");
        mx_append(result, capacity, body);
        ok = 0;
    } else {
        js_inspect(v, result, capacity, 0, 1);
        js_set_c(js_obj_val(js_global), "$_", v);
    }
    leave();
    return ok;
}

void mx_js_set_inspected(mx_node_t* node) {
    if (!active) return;
    js_define(js_global, "$0", node ? js_wrap_node(node) : js_undef(), PROP_HIDDEN);
}

static int dispatch_direct(mx_node_t* target, const char* type,
                           const mx_js_event_info_t* info) {
    if (!active || !target) return 0;
    char marker;
    enter(&marker);
    js_obj* e = make_event(type, event_bubbles(type), 1);
    js_val ev = js_obj_val(e);
    if (info) {
        js_set_c(ev, "clientX", js_num(info->client_x));
        js_set_c(ev, "clientY", js_num(info->client_y));
        js_set_c(ev, "pageX", js_num(info->client_x));
        js_set_c(ev, "pageY", js_num(info->client_y + mx_html_scroll_y()));
        js_set_c(ev, "offsetX", js_num(info->client_x - target->box_x));
        js_set_c(ev, "offsetY", js_num(info->client_y + mx_html_scroll_y() - target->box_y));
        js_set_c(ev, "button", js_num(info->button));
        js_set_c(ev, "buttons", js_num(1));
        js_set_c(ev, "detail", js_num(1));
        if (info->key[0] || info->key_code) {
            js_set_c(ev, "key", js_str_val(js_cstr(info->key)));
            char code[32];
            if (info->key[0] >= 'a' && info->key[0] <= 'z' && !info->key[1]) {
                mx_copy(code, sizeof(code), "Key");
                char up[2] = {(char)(info->key[0] - 32), 0};
                mx_append(code, sizeof(code), up);
            } else if (info->key[0] >= '0' && info->key[0] <= '9' && !info->key[1]) {
                mx_copy(code, sizeof(code), "Digit");
                mx_append(code, sizeof(code), info->key);
            } else if (info->key[0] == ' ') {
                mx_copy(code, sizeof(code), "Space");
            } else {
                mx_copy(code, sizeof(code), info->key);
            }
            js_set_c(ev, "code", js_str_val(js_cstr(code)));
            js_set_c(ev, "keyCode", js_num(info->key_code));
            js_set_c(ev, "which", js_num(info->key_code));
            js_set_c(ev, "charCode", js_num(info->key[0] && !info->key[1] ? info->key[0] : 0));
        }
        js_set_c(ev, "shiftKey", js_bool(info->shift));
        js_set_c(ev, "ctrlKey", js_bool(info->ctrl));
        js_set_c(ev, "altKey", js_bool(info->alt));
        js_set_c(ev, "metaKey", js_bool(0));
        js_set_c(ev, "repeat", js_bool(0));
    }
    int prevented = dispatch_event(target, e);
    leave();
    return prevented;
}

static void dispatch_window_direct(const char* type) {
    if (!active) return;
    char marker;
    enter(&marker);
    js_obj* e = make_event(type, 0, 0);
    js_val ev = js_obj_val(e);
    js_set_c(ev, "target", document_val);
    if (!strcmp(type, "DOMContentLoaded")) {
        invoke_listeners(js_doc->root, document_val, e, js_intern_c(type), 3);
        invoke_listeners(NULL, document_val, e, js_intern_c(type), 3);
    } else {
        invoke_listeners(NULL, document_val, e, js_intern_c(type), 3);
        /* body onload="..." */
        sync_attribute_handler(js_doc->body, js_intern_c(type));
        invoke_listeners(js_doc->body, document_val, e, js_intern_c(type), 3);
    }
    leave();
}

/* ---- entry points on the script stack ---- */

typedef struct {
    int kind;
    const char* source;
    size_t length;
    const char* url;
    const char* name;
    char* result;
    size_t capacity;
    mx_node_t* target;
    const mx_js_event_info_t* info;
    long now;
    int ret;
} js_call_box;

static void boxed_call(void* p) {
    js_call_box* b = (js_call_box*)p;
    switch (b->kind) {
    case 0: b->ret = run_direct(b->source, b->length, b->name); break;
    case 1: b->ret = run_module_direct(b->source, b->length, b->url, b->name); break;
    case 2: b->ret = eval_direct(b->source, b->result, b->capacity); break;
    case 3: b->ret = dispatch_direct(b->target, b->name, b->info); break;
    case 4: dispatch_window_direct(b->name); break;
    case 5: run_timers_direct(b->now); break;
    }
}

void js_set_import_map(js_val map);

void mx_js_set_import_map(const char* json, size_t length) {
    if (!active) return;
    char marker;
    enter(&marker);
    js_val text = js_str_val(js_new_str(json, length));
    js_val parse = js_get_c(js_get_c(js_obj_val(js_global), "JSON"), "parse");
    js_val map = js_call(parse, js_undef(), &text, 1);
    if (js_rt.ctl == CTL_THROW) {
        js_rt.ctl = CTL_NONE;
        mx_console_add(MX_LOG_ERROR, "importmap", 0, "The import map is not valid JSON.");
    } else {
        js_set_import_map(map);
    }
    leave();
}

int mx_js_run(const char* source, size_t length, const char* name) {
    js_call_box b = {0, source, length, NULL, name, NULL, 0, NULL, NULL, 0, 0};
    run_on_js_stack(boxed_call, &b);
    return b.ret;
}

int mx_js_run_module(const char* source, size_t length, const char* url,
                     const char* name) {
    js_call_box b = {1, source, length, url, name, NULL, 0, NULL, NULL, 0, 0};
    run_on_js_stack(boxed_call, &b);
    return b.ret;
}

int mx_js_eval(const char* source, char* result, size_t capacity) {
    js_call_box b = {2, source, 0, NULL, NULL, result, capacity, NULL, NULL, 0, 0};
    run_on_js_stack(boxed_call, &b);
    return b.ret;
}

int mx_js_dispatch(mx_node_t* target, const char* type,
                   const mx_js_event_info_t* info) {
    js_call_box b = {3, NULL, 0, NULL, type, NULL, 0, target, info, 0, 0};
    run_on_js_stack(boxed_call, &b);
    return b.ret;
}

void mx_js_dispatch_window(const char* type) {
    js_call_box b = {4, NULL, 0, NULL, type, NULL, 0, NULL, NULL, 0, 0};
    run_on_js_stack(boxed_call, &b);
}

void mx_js_run_timers(long now_ms) {
    js_call_box b = {5, NULL, 0, NULL, NULL, NULL, 0, NULL, NULL, now_ms, 0};
    run_on_js_stack(boxed_call, &b);
}
