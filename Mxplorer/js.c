/* Mxplorer JavaScript: lexer, parser, interpreter and object model.
 *
 * A tree-walking interpreter for a large practical subset of ES2020:
 * var/let/const, functions, closures, arrow functions, classes (extends,
 * super, static, getters/setters, fields), destructuring, spread/rest,
 * default parameters, template literals, optional chaining, nullish
 * coalescing, exceptions, labels, for-in/of and switch. Generators are not
 * supported; `await` unwraps settled promises synchronously.
 *
 * Memory comes from one arena per page. It has no fixed size: it grows
 * while the machine keeps a reserve of free memory (js_heap_may_grow). Calls to functions without nested
 * closures take their scopes from a LIFO stack so loops and handlers do not
 * grow the heap. */

#include "mxplorer/js_internal.h"
#include "mxplorer/devlog.h"
#include "string.h"
#include "stdlib.h"

js_rt_state js_rt;
js_obj* js_global;
js_obj* js_object_proto;
js_obj* js_function_proto;
js_obj* js_array_proto;
js_obj* js_string_proto;
js_obj* js_number_proto;
js_obj* js_boolean_proto;
js_obj* js_error_proto;
js_obj* js_regexp_proto;
js_obj* js_date_proto;
js_obj* js_promise_proto;
js_obj* js_map_proto;
js_obj* js_set_proto;
js_env* js_global_env;

/* ====================================================================== */
/* Memory                                                                 */
/* ====================================================================== */

#define JS_SCOPE_STACK (1024u * 1024u)
static char* scope_stack;
static size_t scope_top;

/* js_alloc() and the garbage collector: js_gc.c */

static void* scope_alloc(size_t size) {
    size = (size + 15u) & ~(size_t)15u;
    if (!scope_stack || scope_top + size > JS_SCOPE_STACK) return NULL;
    void* p = scope_stack + scope_top;
    scope_top += size;
    memset(p, 0, size);
    return p;
}

/* ====================================================================== */
/* Strings                                                                */
/* ====================================================================== */

uint32_t js_hash_bytes(const char* text, size_t length) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < length; i++) {
        h ^= (uint8_t)text[i];
        h *= 16777619u;
    }
    return h;
}

js_str* js_new_str(const char* text, size_t length) {
    js_str* s = (js_str*)js_alloc(sizeof(js_str) + length + 1);
    s->length = (uint32_t)length;
    s->capacity = (uint32_t)length + 1;
    s->owned = 0;
    s->ascii = 0;
    if (length) memcpy(s->data, text, length);
    s->data[length] = '\0';
    s->hash = js_hash_bytes(s->data, length);
    s->hashed = 1;
    return s;
}

js_str* js_cstr(const char* text) {
    return js_new_str(text, strlen(text));
}

#define INTERN_SIZE 4096u
typedef struct intern_entry {
    js_str* str;
    struct intern_entry* next;
} intern_entry;
static intern_entry* intern_table[INTERN_SIZE];

js_str* js_intern(const char* text, size_t length) {
    uint32_t h = js_hash_bytes(text, length);
    intern_entry** slot = &intern_table[h % INTERN_SIZE];
    for (intern_entry* e = *slot; e; e = e->next)
        if (e->str->hash == h && e->str->length == length &&
            !memcmp(e->str->data, text, length))
            return e->str;
    js_str* s = js_new_str(text, length);
    intern_entry* e = (intern_entry*)js_alloc(sizeof(*e));
    e->str = s;
    e->next = *slot;
    *slot = e;
    return s;
}

js_str* js_intern_c(const char* text) {
    return js_intern(text, strlen(text));
}

js_str* js_concat(js_str* a, js_str* b) {
    if (!a->length) return b;
    if (!b->length) return a;
    js_str* s = (js_str*)js_alloc(sizeof(js_str) + a->length + b->length + 1);
    s->length = a->length + b->length;
    s->capacity = s->length + 1;
    s->owned = 0;
    s->ascii = a->ascii == 1 && b->ascii == 1 ? 1 : 0;
    memcpy(s->data, a->data, a->length);
    memcpy(s->data + a->length, b->data, b->length);
    s->data[s->length] = '\0';
    s->hashed = 0;   /* hashed when first used as a key */
    return s;
}

/* `name += value` as a statement on a string variable. The result becomes a
 * string builder with spare room that only this variable holds (reading the
 * variable anywhere else clears `owned`), so a loop that builds a string
 * appends in place instead of copying the whole string every time. */
static inline js_val share_val(js_val v) {
    if (v.type == JS_STR) v.u.str->owned = 0;
    return v;
}

static js_str* append_owned(js_str* a, js_str* b) {
    uint32_t total = a->length + b->length;
    if (a->owned && a->capacity > total) {
        memcpy(a->data + a->length, b->data, b->length);
        a->length = total;
        a->data[total] = '\0';
        a->hashed = 0;
        if (b->ascii != 1) a->ascii = 0;
        return a;
    }
    uint32_t capacity = total < 64 ? total + 1 : total * 2;
    js_str* s = (js_str*)js_alloc(sizeof(js_str) + capacity);
    s->length = total;
    s->capacity = capacity;
    s->owned = 1;
    s->hashed = 0;
    s->ascii = a->ascii == 1 && b->ascii == 1 ? 1 : 0;
    memcpy(s->data, a->data, a->length);
    memcpy(s->data + a->length, b->data, b->length);
    s->data[total] = '\0';
    return s;
}

int js_str_eq(const js_str* a, const js_str* b) {
    return a == b || (a->length == b->length && js_str_hash((js_str*)a) == js_str_hash((js_str*)b) &&
                      !memcmp(a->data, b->data, a->length));
}

int js_str_eq_c(const js_str* a, const char* text) {
    size_t n = strlen(text);
    return a->length == n && !memcmp(a->data, text, n);
}

/* common names */
static js_str *S_length, *S_prototype, *S_constructor, *S_name, *S_message,
    *S_proto, *S_toString, *S_valueOf, *S_callee, *S_default, *S_then,
    *S_stack;
static js_str *K_var, *K_let, *K_const, *K_function, *K_return, *K_if,
    *K_else, *K_for, *K_while, *K_do, *K_break, *K_continue, *K_new,
    *K_this, *K_typeof, *K_instanceof, *K_in, *K_of, *K_delete, *K_void,
    *K_throw, *K_try, *K_catch, *K_finally, *K_switch, *K_case, *K_true,
    *K_false, *K_null, *K_undefined, *K_class, *K_extends, *K_super,
    *K_static, *K_get, *K_set, *K_async, *K_await, *K_yield, *K_import,
    *K_export, *K_debugger, *K_with, *K_arguments, *K_to_primitive;

static void init_names(void) {
#define N(var, text) var = js_intern_c(text)
    N(S_length, "length"); N(S_prototype, "prototype");
    N(S_constructor, "constructor"); N(S_name, "name");
    N(S_message, "message"); N(S_proto, "__proto__");
    N(S_toString, "toString"); N(S_valueOf, "valueOf");
    N(S_callee, "callee"); N(S_default, "default"); N(S_then, "then");
    N(S_stack, "stack");
    N(K_var, "var"); N(K_let, "let"); N(K_const, "const");
    N(K_function, "function"); N(K_return, "return"); N(K_if, "if");
    N(K_else, "else"); N(K_for, "for"); N(K_while, "while"); N(K_do, "do");
    N(K_break, "break"); N(K_continue, "continue"); N(K_new, "new");
    N(K_this, "this"); N(K_typeof, "typeof"); N(K_instanceof, "instanceof");
    N(K_in, "in"); N(K_of, "of"); N(K_delete, "delete"); N(K_void, "void");
    N(K_throw, "throw"); N(K_try, "try"); N(K_catch, "catch");
    N(K_finally, "finally"); N(K_switch, "switch"); N(K_case, "case");
    N(K_true, "true"); N(K_false, "false"); N(K_null, "null");
    N(K_undefined, "undefined"); N(K_class, "class"); N(K_extends, "extends");
    N(K_super, "super"); N(K_static, "static"); N(K_get, "get");
    N(K_set, "set"); N(K_async, "async"); N(K_await, "await");
    N(K_yield, "yield"); N(K_import, "import"); N(K_export, "export");
    N(K_debugger, "debugger"); N(K_with, "with"); N(K_arguments, "arguments"); N(K_to_primitive, "@@toPrimitive");
#undef N
}

/* ====================================================================== */
/* Objects                                                                */
/* ====================================================================== */

js_obj* js_new_object(js_obj* proto) {
    js_obj* o = (js_obj*)js_alloc(sizeof(js_obj));
    memset(o, 0, sizeof(*o));
    o->cls = CLS_OBJECT;
    o->proto = proto;
    return o;
}

js_obj* js_new_array(uint32_t capacity) {
    js_obj* a = js_new_object(js_array_proto);
    a->cls = CLS_ARRAY;
    if (capacity) {
        a->items = (js_val*)js_alloc(sizeof(js_val) * capacity);
        a->item_capacity = capacity;
    }
    return a;
}

js_obj* js_new_native(const char* name, js_native fn, int arity) {
    js_obj* f = js_new_object(js_function_proto);
    f->cls = CLS_NATIVE;
    f->native = fn;
    f->name = js_intern_c(name);
    f->magic = (int16_t)arity;
    return f;
}

static void array_reserve(js_obj* a, uint32_t needed) {
    if (needed <= a->item_capacity) return;
    uint32_t capacity = a->item_capacity ? a->item_capacity * 2 : 8;
    while (capacity < needed) capacity *= 2;
    js_val* items = (js_val*)js_alloc(sizeof(js_val) * capacity);
    if (a->length) memcpy(items, a->items, sizeof(js_val) * a->length);
    a->items = items;
    a->item_capacity = capacity;
}

void js_array_push(js_obj* a, js_val value) {
    array_reserve(a, a->length + 1);
    a->items[a->length++] = value;
}

void js_array_set_length(js_obj* a, uint32_t length) {
    if (length > a->length) {
        array_reserve(a, length);
        for (uint32_t i = a->length; i < length; i++) a->items[i] = js_undef();
    }
    a->length = length;
}

/* Objects with many properties (the global object, prototypes, big
 * dictionaries) get a hash index so lookups stop scanning every property. */
#define PROP_INDEX_MIN 10u

static void prop_index_insert(js_obj* o, uint32_t i) {
    uint32_t mask = o->prop_index_cap - 1;
    uint32_t h = js_str_hash(o->props[i].key) & mask;
    while (o->prop_index[h]) h = (h + 1) & mask;
    o->prop_index[h] = i + 1;
}

static void prop_index_build(js_obj* o) {
    uint32_t cap = 16;
    while (cap < o->prop_count * 2 + 2) cap <<= 1;
    uint32_t* slots = (uint32_t*)js_alloc(sizeof(uint32_t) * cap);
    if (!slots) { o->prop_index = NULL; return; }
    memset(slots, 0, sizeof(uint32_t) * cap);
    o->prop_index = slots;
    o->prop_index_cap = cap;
    for (uint32_t i = 0; i < o->prop_count; i++) prop_index_insert(o, i);
}

js_prop* js_find_own(js_obj* o, js_str* key) {
    if (o->prop_count >= PROP_INDEX_MIN) {
        if (!o->prop_index) prop_index_build(o);
        if (o->prop_index) {
            uint32_t mask = o->prop_index_cap - 1;
            for (uint32_t h = js_str_hash(key) & mask;; h = (h + 1) & mask) {
                uint32_t slot = o->prop_index[h];
                if (!slot) return NULL;
                js_prop* p = &o->props[slot - 1];
                if (p->key == key || (p->key->length == key->length && js_str_hash(p->key) == js_str_hash(key) &&
                                      !memcmp(p->key->data, key->data, key->length)))
                    return p;
            }
        }
    }
    for (uint32_t i = 0; i < o->prop_count; i++) {
        js_prop* p = &o->props[i];
        if (p->key == key || (p->key->length == key->length && js_str_hash(p->key) == js_str_hash(key) &&
                              !memcmp(p->key->data, key->data, key->length)))
            return p;
    }
    return NULL;
}

static js_prop* add_prop(js_obj* o, js_str* key) {
    if (o->prop_count == o->prop_capacity) {
        uint32_t capacity = o->prop_capacity ? o->prop_capacity * 2 : 4;
        js_prop* props = (js_prop*)js_alloc(sizeof(js_prop) * capacity);
        if (o->prop_count) memcpy(props, o->props, sizeof(js_prop) * o->prop_count);
        o->props = props;
        o->prop_capacity = capacity;
    }
    js_prop* p = &o->props[o->prop_count++];
    memset(p, 0, sizeof(*p));
    p->key = key;
    p->value = js_undef();
    if (o->prop_index) {
        if (o->prop_count * 2 + 2 > o->prop_index_cap) prop_index_build(o);
        else prop_index_insert(o, o->prop_count - 1);
    }
    return p;
}

void js_define(js_obj* o, const char* key, js_val value, uint8_t flags) {
    js_str* k = js_intern_c(key);
    js_prop* p = js_find_own(o, k);
    if (!p) p = add_prop(o, k);
    p->value = value;
    p->flags = flags;
}

void js_define_native(js_obj* o, const char* key, js_native fn, int arity) {
    js_define(o, key, js_obj_val(js_new_native(key, fn, arity)), PROP_HIDDEN);
}

void js_define_accessor(js_obj* o, const char* key, js_native getter,
                        js_native setter) {
    js_str* k = js_intern_c(key);
    js_prop* p = js_find_own(o, k);
    if (!p) p = add_prop(o, k);
    p->flags = PROP_ACCESSOR | PROP_HIDDEN;
    p->value = getter ? js_obj_val(js_new_native(key, getter, 0)) : js_undef();
    p->setter = setter ? js_new_native(key, setter, 1) : NULL;
}

int js_array_index(js_str* key, uint32_t* index) {
    if (!key->length || key->length > 10) return 0;
    if (key->data[0] == '0' && key->length > 1) return 0;
    uint64_t value = 0;
    for (uint32_t i = 0; i < key->length; i++) {
        char c = key->data[i];
        if (c < '0' || c > '9') return 0;
        value = value * 10 + (uint64_t)(c - '0');
    }
    if (value >= 0xFFFFFFFFu) return 0;
    *index = (uint32_t)value;
    return 1;
}

static js_val get_from(js_obj* o, js_str* key, js_val receiver);

/* ---- Proxy ----
 * A proxy keeps its target in bound_target and its handler in data
 * (NULL once revoked). Proxies of functions are natives that call
 * proxy_call, so they stay callable. */
static js_val proxy_call(js_val self, js_val* args, int argc, js_obj* callee);

static int is_proxy(const js_obj* o) {
    return o->cls == CLS_PROXY || (o->cls == CLS_NATIVE && o->native == proxy_call);
}

js_obj* js_proxy_target(js_obj* o) {
    return o && is_proxy(o) ? o->bound_target : NULL;
}

void js_proxy_revoke(js_obj* o) {
    if (o && is_proxy(o)) o->data = NULL;
}

/* the handler's trap, or undefined; throws when revoked */
static js_val proxy_trap(js_obj* p, const char* name) {
    if (!p->data) {
        js_throw_error("TypeError", "Cannot perform operation on a revoked proxy");
        return js_undef();
    }
    return js_get(js_obj_val((js_obj*)p->data), js_intern_c(name));
}

static js_val proxy_key(js_str* key) { return js_str_val(key); }

static js_val proxy_get(js_obj* p, js_str* key, js_val receiver) {
    js_val trap = proxy_trap(p, "get");
    if (js_rt.ctl) return js_undef();
    if (!js_is_callable(trap)) return get_from(p->bound_target, key, receiver);
    js_val args[3] = {js_obj_val(p->bound_target), proxy_key(key), receiver};
    return js_call(trap, js_obj_val((js_obj*)p->data), args, 3);
}

static int proxy_set(js_obj* p, js_str* key, js_val value, js_val receiver) {
    js_val trap = proxy_trap(p, "set");
    if (js_rt.ctl) return 1;
    if (!js_is_callable(trap)) {
        js_set(js_obj_val(p->bound_target), key, value);
        return 1;
    }
    js_val args[4] = {js_obj_val(p->bound_target), proxy_key(key), value, receiver};
    js_call(trap, js_obj_val((js_obj*)p->data), args, 4);
    return 1;
}

static int proxy_has(js_obj* p, js_str* key) {
    js_val trap = proxy_trap(p, "has");
    if (js_rt.ctl) return 0;
    if (!js_is_callable(trap)) return js_has_property(js_obj_val(p->bound_target), key);
    js_val args[2] = {js_obj_val(p->bound_target), proxy_key(key)};
    return js_truthy(js_call(trap, js_obj_val((js_obj*)p->data), args, 2));
}

static int proxy_delete(js_obj* p, js_str* key) {
    js_val trap = proxy_trap(p, "deleteProperty");
    if (js_rt.ctl) return 0;
    if (!js_is_callable(trap)) return js_delete(js_obj_val(p->bound_target), key);
    js_val args[2] = {js_obj_val(p->bound_target), proxy_key(key)};
    return js_truthy(js_call(trap, js_obj_val((js_obj*)p->data), args, 2));
}

static js_obj* proxy_own_keys(js_obj* p, int include_hidden) {
    js_val trap = proxy_trap(p, "ownKeys");
    if (js_rt.ctl) return js_new_array(0);
    if (!js_is_callable(trap)) return js_own_keys(p->bound_target, include_hidden);
    js_val target = js_obj_val(p->bound_target);
    js_val list = js_call(trap, js_obj_val((js_obj*)p->data), &target, 1);
    js_obj* keys = js_new_array(0);
    if (js_rt.ctl || list.type != JS_OBJ) return keys;
    for (uint32_t i = 0; i < list.u.obj->length; i++) {
        js_val k = list.u.obj->items[i];
        if (k.type != JS_STR) continue;
        if (!include_hidden && k.u.str->length > 2 && k.u.str->data[0] == '@' && k.u.str->data[1] == '@')
            continue;   /* symbols */
        js_array_push(keys, k);
    }
    return keys;
}

static js_val proxy_call(js_val self, js_val* args, int argc, js_obj* callee) {
    js_val trap = proxy_trap(callee, "apply");
    if (js_rt.ctl) return js_undef();
    js_val target = js_obj_val(callee->bound_target);
    if (!js_is_callable(trap)) return js_call(target, self, args, argc);
    js_obj* list = js_new_array((uint32_t)argc);
    for (int i = 0; i < argc; i++) js_array_push(list, args[i]);
    js_val trap_args[3] = {target, self, js_obj_val(list)};
    return js_call(trap, js_obj_val((js_obj*)callee->data), trap_args, 3);
}

static js_val proxy_construct(js_obj* p, js_val* args, int argc) {
    js_val trap = proxy_trap(p, "construct");
    if (js_rt.ctl) return js_undef();
    js_val target = js_obj_val(p->bound_target);
    if (!js_is_callable(trap)) return js_construct(target, args, argc);
    js_obj* list = js_new_array((uint32_t)argc);
    for (int i = 0; i < argc; i++) js_array_push(list, args[i]);
    js_val trap_args[3] = {target, js_obj_val(list), js_obj_val(p)};
    js_val r = js_call(trap, js_obj_val((js_obj*)p->data), trap_args, 3);
    if (!js_rt.ctl && r.type != JS_OBJ) return js_throw_error("TypeError", "proxy [[Construct]] must return an object");
    return r;
}

js_val js_new_proxy(js_val target, js_val handler) {
    if (target.type != JS_OBJ || handler.type != JS_OBJ)
        return js_throw_error("TypeError", "Cannot create proxy with a non-object as target or handler");
    js_obj* t = target.u.obj;
    js_obj* p;
    if (js_is_callable(target)) {
        p = js_new_native(t->name ? t->name->data : "", proxy_call, 0);
        p->magic = 0;
    } else {
        p = js_new_object(t->proto);
        p->cls = CLS_PROXY;
    }
    p->proto = t->proto;   /* instanceof and getPrototypeOf see the target's prototype */
    p->bound_target = t;
    p->data = handler.u.obj;
    return js_obj_val(p);
}

static js_val get_from(js_obj* o, js_str* key, js_val receiver) {
    int depth = 0;
    for (js_obj* cur = o; cur && depth < 64; cur = cur->proto, depth++) {
        if (cur->cls >= CLS_NODE && cur->cls <= CLS_DATASET) {
            int found = 0;
            js_val v = js_dom_get(cur, key, &found);
            if (found) return v;
        }
        if (cur->cls == CLS_PROXY || (cur->cls == CLS_NATIVE && cur->native == proxy_call))
            return proxy_get(cur, key, receiver);
        if ((cur->cls == CLS_ARRAY || cur->cls == CLS_ARGUMENTS)) {
            if (key == S_length) return js_num(cur->length);
            uint32_t index;
            if (js_array_index(key, &index)) {
                if (index < cur->length) return cur->items[index];
                continue;
            }
        }
        js_prop* p = js_find_own(cur, key);
        if (p) {
            if (p->flags & PROP_ACCESSOR) {
                if (!js_is_callable(p->value)) return js_undef();
                return js_call(p->value, receiver, NULL, 0);
            }
            return p->value;
        }
        if (cur->cls == CLS_FUNCTION && key == S_prototype && cur->func &&
            !cur->func->arrow) {
            js_obj* proto = js_new_object(js_object_proto);
            js_prop* cp = add_prop(proto, S_constructor);
            cp->value = js_obj_val(cur);
            cp->flags = PROP_HIDDEN;
            js_prop* pp = add_prop(cur, S_prototype);
            pp->value = js_obj_val(proto);
            pp->flags = PROP_HIDDEN;
            return pp->value;
        }
        if ((cur->cls == CLS_FUNCTION || cur->cls == CLS_NATIVE ||
             cur->cls == CLS_BOUND) && cur == o) {
            if (key == S_name) return js_str_val(cur->name ? cur->name : js_intern_c(""));
            if (key == S_length)
                return js_num(cur->func ? cur->func->param_count : cur->magic > 0 ? cur->magic : 0);
        }
    }
    return js_undef();
}

__attribute__((noinline, cold)) static js_val throw_nullish_access(js_val target, js_str* key,
                                                                  const char* verb,
                                                                  const char* gerund) {
    char message[200];
    mx_copy(message, sizeof(message), "Cannot ");
    mx_append(message, sizeof(message), verb);
    mx_append(message, sizeof(message), " properties of ");
    mx_append(message, sizeof(message), target.type == JS_NULL ? "null" : "undefined");
    mx_append(message, sizeof(message), " (");
    mx_append(message, sizeof(message), gerund);
    mx_append(message, sizeof(message), " '");
    mx_append(message, sizeof(message), key->data);
    mx_append(message, sizeof(message), "')");
    return js_throw_error("TypeError", message);
}

/* one-character strings are shared instead of allocated per s[i] */
static js_str* single_char(char c) {
    static js_str* table[256];
    static uint32_t generation_seen;
    extern uint32_t js_core_generation;
    if (generation_seen != js_core_generation) {
        memset(table, 0, sizeof(table));
        generation_seen = js_core_generation;
    }
    unsigned char k = (unsigned char)c;
    if (!table[k]) table[k] = js_intern(&c, 1);
    return table[k];
}

js_val js_get(js_val target, js_str* key) {
    switch (target.type) {
    case JS_OBJ:
        return get_from(target.u.obj, key, target);
    case JS_STR: {
        js_str* s = target.u.str;
        if (key == S_length) return js_num(js_str_cp_length(s));   /* code points */
        uint32_t index;
        if (js_array_index(key, &index)) {
            uint32_t start, end;
            if (!js_str_char_at(s, index, &start, &end)) return js_undef();
            return js_str_val(end - start == 1 ? single_char(s->data[start])
                                               : js_new_str(s->data + start, end - start));
        }
        return get_from(js_string_proto, key, target);
    }
    case JS_NUM:
        return get_from(js_number_proto, key, target);
    case JS_BOOL:
        return get_from(js_boolean_proto, key, target);
    default:
        return throw_nullish_access(target, key, "read", "reading");
    }
}

js_val js_get_c(js_val target, const char* key) {
    return js_get(target, js_intern_c(key));
}

static js_str* key_of(js_val key) {
    if (key.type == JS_STR) return key.u.str;
    if (key.type == JS_NUM) {
        double n = key.u.num;
        if (n >= 0 && n < 1e9 && n == (double)(uint32_t)n) {
            char digits[16];
            int count = 0;
            uint32_t v = (uint32_t)n;
            char tmp[16];
            do {
                tmp[count++] = (char)('0' + v % 10);
                v /= 10;
            } while (v);
            for (int i = 0; i < count; i++) digits[i] = tmp[count - 1 - i];
            return js_intern(digits, (size_t)count);
        }
    }
    return js_to_string(key);
}

js_val js_get_index(js_val target, js_val key) {
    if (target.type == JS_STR && key.type == JS_NUM) {
        double n = key.u.num;
        if (n >= 0 && n == (double)(uint32_t)n) {
            uint32_t start, end;
            js_str* s = target.u.str;
            if (!js_str_char_at(s, (uint32_t)n, &start, &end)) return js_undef();
            return js_str_val(end - start == 1 ? single_char(s->data[start])
                                               : js_new_str(s->data + start, end - start));
        }
    }
    if (target.type == JS_OBJ && key.type == JS_NUM &&
        (target.u.obj->cls == CLS_ARRAY || target.u.obj->cls == CLS_ARGUMENTS)) {
        double n = key.u.num;
        if (n >= 0 && n == (double)(uint32_t)n) {
            uint32_t i = (uint32_t)n;
            if (i < target.u.obj->length) return target.u.obj->items[i];
        }
    }
    return js_get(target, key_of(key));
}

static int set_on(js_obj* o, js_str* key, js_val value, js_val receiver) {
    if (is_proxy(o)) return proxy_set(o, key, value, receiver);
    if (o->cls >= CLS_NODE && o->cls <= CLS_DATASET && js_dom_set(o, key, value)) return 1;
    if (o->cls == CLS_ARRAY || o->cls == CLS_ARGUMENTS) {
        if (key == S_length) {
            if (o->typed) return 1;
            double n = js_to_number(value);
            if (n >= 0 && n < 1e8) js_array_set_length(o, (uint32_t)n);
            return 1;
        }
        uint32_t index;
        if (o->typed) {
            /* fixed length; writes are converted to the element type */
            if (js_array_index(key, &index)) {
                if (index < o->length)
                    o->items[index] = js_num(js_typed_coerce(o->typed, js_to_number(value)));
                return 1;
            }
        } else if (js_array_index(key, &index) && index < o->length + 100000u) {
            if (index >= o->length) js_array_set_length(o, index + 1);
            o->items[index] = value;
            return 1;
        }
    }
    js_prop* p = js_find_own(o, key);
    if (!p) {
        /* setters up the prototype chain */
        for (js_obj* cur = o->proto; cur; cur = cur->proto) {
            js_prop* inherited = js_find_own(cur, key);
            if (inherited && (inherited->flags & PROP_ACCESSOR)) {
                if (inherited->setter) js_call(js_obj_val(inherited->setter), receiver, &value, 1);
                return 1;
            }
            if (inherited) break;
        }
        if (o->frozen) return 1;
        p = add_prop(o, key);
    }
    if (p->flags & PROP_ACCESSOR) {
        if (p->setter) js_call(js_obj_val(p->setter), receiver, &value, 1);
        return 1;
    }
    if ((p->flags & PROP_READONLY) || o->frozen) return 1;
    p->value = value;
    return 1;
}

void js_set(js_val target, js_str* key, js_val value) {
    if (target.type == JS_OBJ) {
        set_on(target.u.obj, key, value, target);
        return;
    }
    if (target.type == JS_UNDEF || target.type == JS_NULL)
        throw_nullish_access(target, key, "set", "setting");
    /* writes to primitives are ignored */
}

void js_set_c(js_val target, const char* key, js_val value) {
    js_set(target, js_intern_c(key), value);
}

void js_set_index(js_val target, js_val key, js_val value) {
    if (target.type == JS_OBJ && key.type == JS_NUM && target.u.obj->cls == CLS_ARRAY) {
        double n = key.u.num;
        js_obj* a = target.u.obj;
        if (a->typed) {
            if (n >= 0 && n == (double)(uint32_t)n) {
                if ((uint32_t)n < a->length)
                    a->items[(uint32_t)n] = js_num(js_typed_coerce(a->typed, js_to_number(value)));
                return;
            }
        } else if (n >= 0 && n == (double)(uint32_t)n && (uint32_t)n < a->length + 100000u) {
            uint32_t i = (uint32_t)n;
            if (i >= a->length) js_array_set_length(a, i + 1);
            a->items[i] = value;
            return;
        }
    }
    js_set(target, key_of(key), value);
}

int js_delete(js_val target, js_str* key) {
    if (target.type != JS_OBJ) return 1;
    js_obj* o = target.u.obj;
    if (is_proxy(o)) return proxy_delete(o, key);
    if (o->frozen) return 0;
    if (o->cls == CLS_ARRAY) {
        uint32_t index;
        if (js_array_index(key, &index) && index < o->length) {
            o->items[index] = js_undef();
            return 1;
        }
    }
    if (o->cls >= CLS_NODE && o->cls <= CLS_DATASET) {
        js_dom_set(o, key, js_undef());
    }
    for (uint32_t i = 0; i < o->prop_count; i++) {
        if (js_str_eq(o->props[i].key, key)) {
            memmove(&o->props[i], &o->props[i + 1],
                    sizeof(js_prop) * (o->prop_count - i - 1));
            o->prop_count--;
            o->prop_index = NULL;   /* rebuilt on the next lookup */
            return 1;
        }
    }
    return 1;
}

int js_has_property(js_val target, js_str* key) {
    if (target.type != JS_OBJ) return 0;
    for (js_obj* o = target.u.obj; o; o = o->proto) {
        if (is_proxy(o)) return proxy_has(o, key);
        if (o->cls == CLS_ARRAY) {
            uint32_t index;
            if (key == S_length) return 1;
            if (js_array_index(key, &index)) return index < o->length;
        }
        if (o->cls >= CLS_NODE && o->cls <= CLS_DATASET) {
            int found = 0;
            js_dom_get(o, key, &found);
            if (found) return 1;
        }
        if (js_find_own(o, key)) return 1;
    }
    return 0;
}

js_obj* js_own_keys(js_obj* o, int include_hidden) {
    if (is_proxy(o)) return proxy_own_keys(o, include_hidden);
    js_obj* keys = js_new_array(o->prop_count + o->length);
    if (o->cls == CLS_ARRAY || o->cls == CLS_ARGUMENTS) {
        for (uint32_t i = 0; i < o->length; i++)
            js_array_push(keys, js_str_val(key_of(js_num(i))));
    }
    if (o->cls == CLS_DATASET || o->cls == CLS_STYLE) {
        int found = 0;
        js_val list = js_dom_get(o, js_intern_c("\x01keys"), &found);
        if (found && list.type == JS_OBJ)
            for (uint32_t i = 0; i < list.u.obj->length; i++)
                js_array_push(keys, list.u.obj->items[i]);
    }
    /* integer keys first in ascending order, then the others in the order
     * they were added; symbols (stored as "@@..." keys) only when hidden
     * keys are asked for */
    uint32_t first_index = keys->length, index_count = 0;
    for (uint32_t i = 0; i < o->prop_count; i++) {
        js_prop* p = &o->props[i];
        uint32_t index;
        if (!include_hidden && (p->flags & PROP_HIDDEN)) continue;
        if (js_array_index(p->key, &index)) {
            js_array_push(keys, js_str_val(p->key));
            index_count++;
        }
    }
    if (index_count > 1) {
        for (uint32_t i = first_index + 1; i < keys->length; i++) {
            js_val v = keys->items[i];
            uint32_t key;
            js_array_index(v.u.str, &key);
            uint32_t j = i;
            while (j > first_index) {
                uint32_t prev;
                js_array_index(keys->items[j - 1].u.str, &prev);
                if (prev <= key) break;
                keys->items[j] = keys->items[j - 1];
                j--;
            }
            keys->items[j] = v;
        }
    }
    for (uint32_t i = 0; i < o->prop_count; i++) {
        js_prop* p = &o->props[i];
        uint32_t index;
        if (!include_hidden && (p->flags & PROP_HIDDEN)) continue;
        if (index_count && js_array_index(p->key, &index)) continue;
        if (!include_hidden && p->key->length > 2 && p->key->data[0] == '@' && p->key->data[1] == '@')
            continue;
        js_array_push(keys, js_str_val(p->key));
    }
    return keys;
}

/* ====================================================================== */
/* Conversions                                                            */
/* ====================================================================== */

int js_truthy(js_val v) {
    switch (v.type) {
    case JS_BOOL: return v.u.boolean;
    case JS_NUM: return v.u.num != 0 && v.u.num == v.u.num;
    case JS_STR: return v.u.str->length != 0;
    case JS_OBJ: return 1;
    default: return 0;
    }
}

/* hint: 0 string, 1 number, 2 default (+, ==): valueOf first except Dates */
js_val js_to_primitive(js_val v, int hint) {
    if (v.type != JS_OBJ) return v;
    js_obj* o = v.u.obj;
    if (o->cls == CLS_BOXED) return o->primitive;
    js_val exotic = js_get(v, K_to_primitive);
    if (js_rt.ctl) return js_undef();
    if (js_is_callable(exotic)) {
        js_val name = js_str_val(js_intern_c(hint == 0 ? "string" : hint == 1 ? "number" : "default"));
        js_val r = js_call(exotic, v, &name, 1);
        if (js_rt.ctl) return js_undef();
        if (r.type == JS_OBJ) return js_throw_error("TypeError", "Cannot convert object to primitive value");
        return r;
    }
    if (hint == 2) hint = o->cls == CLS_DATE ? 0 : 1;
    int hint_number = hint == 1;
    if (o->cls == CLS_DATE && !hint_number) {
        js_val fn = js_get(v, S_toString);
        if (js_is_callable(fn)) return js_call(fn, v, NULL, 0);
    }
    js_str* order[2] = {hint_number ? S_valueOf : S_toString,
                        hint_number ? S_toString : S_valueOf};
    for (int i = 0; i < 2; i++) {
        js_val fn = js_get(v, order[i]);
        if (js_rt.ctl) return js_undef();
        if (js_is_callable(fn)) {
            js_val r = js_call(fn, v, NULL, 0);
            if (js_rt.ctl) return js_undef();
            if (r.type != JS_OBJ) return r;
        }
    }
    return js_str_val(js_intern_c("[object Object]"));
}

double js_to_number(js_val v) {
    switch (v.type) {
    case JS_NUM: return v.u.num;
    case JS_BOOL: return v.u.boolean;
    case JS_NULL: return 0;
    case JS_STR: return js_string_to_number(v.u.str);
    case JS_OBJ: {
        js_val p = js_to_primitive(v, 1);
        if (p.type == JS_OBJ) return 0.0 / 0.0;
        return js_to_number(p);
    }
    default: {
        double zero = 0;
        return zero / zero;
    }
    }
}

js_str* js_to_string(js_val v) {
    switch (v.type) {
    case JS_STR: return v.u.str;
    case JS_NUM: return js_number_to_string(v.u.num, 10);
    case JS_BOOL: return v.u.boolean ? K_true : K_false;
    case JS_NULL: return K_null;
    case JS_UNDEF: return K_undefined;
    case JS_OBJ: {
        js_val p = js_to_primitive(v, 0);
        if (js_rt.ctl || p.type == JS_OBJ) return js_intern_c("[object Object]");
        return js_to_string(p);
    }
    }
    return K_undefined;
}

js_obj* js_to_object(js_val v) {
    if (v.type == JS_OBJ) return v.u.obj;
    js_obj* proto = v.type == JS_STR ? js_string_proto : v.type == JS_NUM ? js_number_proto :
                    v.type == JS_BOOL ? js_boolean_proto : js_object_proto;
    js_obj* o = js_new_object(proto);
    o->cls = CLS_BOXED;
    o->primitive = v;
    return o;
}

int32_t js_to_int32(js_val v) {
    double n = v.type == JS_NUM ? v.u.num : js_to_number(v);
    if (n != n || n > 1e300 || n < -1e300) return 0;
    double t = n < 0 ? -(double)(long long)(-n) : (double)(long long)n;
    if (t >= -9.2e18 && t <= 9.2e18) return (int32_t)(uint32_t)(uint64_t)(int64_t)t;
    return 0;
}

uint32_t js_to_uint32(js_val v) {
    return (uint32_t)js_to_int32(v);
}

int js_strict_equals(js_val a, js_val b) {
    if (a.type != b.type) return 0;
    switch (a.type) {
    case JS_UNDEF: case JS_NULL: return 1;
    case JS_BOOL: return a.u.boolean == b.u.boolean;
    case JS_NUM: return a.u.num == b.u.num;
    case JS_STR: return js_str_eq(a.u.str, b.u.str);
    case JS_OBJ: return a.u.obj == b.u.obj;
    }
    return 0;
}

int js_loose_equals(js_val a, js_val b) {
    if (a.type == b.type) return js_strict_equals(a, b);
    int a_nullish = a.type == JS_NULL || a.type == JS_UNDEF;
    int b_nullish = b.type == JS_NULL || b.type == JS_UNDEF;
    if (a_nullish || b_nullish) return a_nullish && b_nullish;
    if (a.type == JS_OBJ) a = js_to_primitive(a, 2);
    if (b.type == JS_OBJ) b = js_to_primitive(b, 2);
    if (a.type == JS_STR && b.type == JS_STR) return js_str_eq(a.u.str, b.u.str);
    return js_to_number(a) == js_to_number(b);
}

const char* js_typeof(js_val v) {
    switch (v.type) {
    case JS_UNDEF: return "undefined";
    case JS_NULL: return "object";
    case JS_BOOL: return "boolean";
    case JS_NUM: return "number";
    case JS_STR: return "string";
    case JS_OBJ: return js_is_callable(v) ? "function" : "object";
    }
    return "undefined";
}

/* ====================================================================== */
/* Errors                                                                 */
/* ====================================================================== */

js_val js_throw(js_val value) {
    if (js_rt.ctl == CTL_THROW) return js_undef();
    js_rt.ctl = CTL_THROW;
    js_rt.value = value;
    return js_undef();
}

js_obj* js_make_error(const char* type, const char* message) {
    js_val ctor = js_get(js_obj_val(js_global), js_intern_c(type));
    js_obj* proto = js_error_proto;
    if (ctor.type == JS_OBJ) {
        js_prop* p = js_find_own(ctor.u.obj, S_prototype);
        if (p && p->value.type == JS_OBJ) proto = p->value.u.obj;
    }
    js_obj* e = js_new_object(proto);
    e->cls = CLS_ERROR;
    js_define(e, "message", js_str_val(js_cstr(message)), PROP_HIDDEN);
    char stack[200];
    mx_copy(stack, sizeof(stack), type);
    mx_append(stack, sizeof(stack), ": ");
    mx_append(stack, sizeof(stack), message);
    mx_append(stack, sizeof(stack), "\n    at ");
    mx_append(stack, sizeof(stack), js_rt.source ? js_rt.source : "script");
    mx_append(stack, sizeof(stack), ":");
    mx_append_int(stack, sizeof(stack), js_rt.line);
    js_define(e, "stack", js_str_val(js_cstr(stack)), PROP_HIDDEN);
    js_define(e, "\x01line", js_num(js_rt.line), PROP_HIDDEN);
    js_define(e, "\x01source", js_str_val(js_cstr(js_rt.source ? js_rt.source : "")), PROP_HIDDEN);
    return e;
}

js_val js_throw_error(const char* type, const char* message) {
    if (js_rt.ctl == CTL_THROW) return js_undef();
    return js_throw(js_obj_val(js_make_error(type, message)));
}

js_val js_throw_type(const char* fmt, const char* detail) {
    char message[200];
    mx_copy(message, sizeof(message), detail ? detail : "");
    mx_append(message, sizeof(message), fmt);
    return js_throw_error("TypeError", message);
}

/* ====================================================================== */
/* Lexer                                                                  */
/* ====================================================================== */

enum { T_EOF, T_NUM, T_STR, T_TEMPLATE, T_IDENT, T_PUNCT, T_REGEX, T_PRIVATE };

typedef struct {
    uint8_t type;
    uint8_t newline_before;
    uint8_t regex_after;  /* closing ) or }: a following '/' starts a regex */
    uint32_t punct;
    int line;
    js_str* str;          /* identifier / string / regex body / template raw */
    js_str* flags;        /* regex flags */
    double num;
    uint32_t offset;      /* in source */
} js_token;

typedef struct {
    const char* src;
    size_t length;
    size_t pos;
    int line;
    js_token* tokens;
    uint32_t count, capacity;
    char error[160];
    int error_line;
    /* open brackets: 1 = ( of if/for/while/with, 2 = block {, 0 = other */
    uint8_t open_kind[1024];
    int open_depth;
} lexer_t;

/* PACK("op") for string literals folds to a constant even at -Os, where the
 * pack() call would run on every operator the interpreter dispatches. */
#define PACK(s) ((uint32_t)(uint8_t)(s)[0] | \
                 (sizeof(s) > 2 ? (uint32_t)(uint8_t)(s)[1 % sizeof(s)] << 8 : 0u) | \
                 (sizeof(s) > 3 ? (uint32_t)(uint8_t)(s)[2 % sizeof(s)] << 16 : 0u) | \
                 (sizeof(s) > 4 ? (uint32_t)(uint8_t)(s)[3 % sizeof(s)] << 24 : 0u))

static uint32_t pack(const char* p) {
    uint32_t v = 0;
    for (int i = 0; i < 4 && p[i]; i++) v |= (uint32_t)(uint8_t)p[i] << (8 * i);
    return v;
}

static const char* const puncts[] = {
    ">>>=", "...", "===", "!==", "**=", "<<=", ">>=", ">>>", "&&=", "||=",
    "?\?=", "=>", "==", "!=", "<=", ">=", "&&", "||", "??", "?.", "++", "--",
    "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "**", "<<", ">>", "{",
    "}", "(", ")", "[", "]", ";", ",", "<", ">", "+", "-", "*", "/", "%",
    "&", "|", "^", "!", "~", "?", ":", "=", ".", "@", "#"
};

static int ident_start(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' ||
           c == '$' || c >= 0x80;
}

static int ident_part(unsigned char c) {
    return ident_start(c) || (c >= '0' && c <= '9');
}

static js_token* lex_push(lexer_t* lx) {
    if (lx->count == lx->capacity) {
        uint32_t capacity = lx->capacity ? lx->capacity * 2 : 1024;
        js_token* tokens = (js_token*)malloc(sizeof(js_token) * capacity);
        if (!tokens) return NULL;
        if (lx->count) memcpy(tokens, lx->tokens, sizeof(js_token) * lx->count);
        free(lx->tokens);
        lx->tokens = tokens;
        lx->capacity = capacity;
    }
    js_token* t = &lx->tokens[lx->count++];
    memset(t, 0, sizeof(*t));
    return t;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Decodes escapes of a string literal body into UTF-8. */
static js_str* decode_string(const char* s, size_t n) {
    char* out = (char*)malloc(n * 3 + 4);
    if (!out) return js_new_str("", 0);
    size_t used = 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c != '\\' || i + 1 >= n) {
            out[used++] = c;
            continue;
        }
        c = s[++i];
        switch (c) {
        case 'n': out[used++] = '\n'; break;
        case 't': out[used++] = '\t'; break;
        case 'r': out[used++] = '\r'; break;
        case 'b': out[used++] = '\b'; break;
        case 'f': out[used++] = '\f'; break;
        case 'v': out[used++] = '\v'; break;
        case '0':
            if (i + 1 < n && s[i + 1] >= '0' && s[i + 1] <= '7') {
                int v = 0;
                while (i < n && s[i] >= '0' && s[i] <= '7') v = v * 8 + (s[i++] - '0');
                i--;
                used += mx_utf8_encode((uint32_t)v, out + used);
            } else {
                out[used++] = '\0';
            }
            break;
        case '\r':
            if (i + 1 < n && s[i + 1] == '\n') i++;
            break;
        case '\n': break;
        case 'x':
            if (i + 2 < n && hexval(s[i + 1]) >= 0 && hexval(s[i + 2]) >= 0) {
                used += mx_utf8_encode((uint32_t)(hexval(s[i + 1]) * 16 + hexval(s[i + 2])),
                                       out + used);
                i += 2;
            }
            break;
        case 'u': {
            uint32_t cp = 0;
            if (i + 1 < n && s[i + 1] == '{') {
                i += 2;
                while (i < n && s[i] != '}') {
                    int h = hexval(s[i++]);
                    if (h < 0) break;
                    cp = cp * 16 + (uint32_t)h;
                }
            } else {
                int k;
                for (k = 0; k < 4 && i + 1 < n && hexval(s[i + 1]) >= 0; k++)
                    cp = cp * 16 + (uint32_t)hexval(s[++i]);
                /* surrogate pair */
                if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 < n && s[i + 1] == '\\' &&
                    s[i + 2] == 'u') {
                    uint32_t low = 0;
                    int ok = 1;
                    for (int j = 0; j < 4; j++) {
                        int h = hexval(s[i + 3 + j]);
                        if (h < 0) ok = 0;
                        else low = low * 16 + (uint32_t)h;
                    }
                    if (ok && low >= 0xDC00 && low <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                        i += 6;
                    }
                }
            }
            used += mx_utf8_encode(cp ? cp : 0xFFFD, out + used);
            break;
        }
        default:
            out[used++] = c;
        }
    }
    js_str* result = js_new_str(out, used);
    free(out);
    return result;
}

js_str* js_decode_escapes(const char* text, size_t length);
js_str* js_decode_escapes(const char* text, size_t length) {
    return decode_string(text, length);
}

/* Template literal scanning. scan_template() starts after the opening
 * backtick and returns the index of the closing one; scan_code() starts
 * after "${" and returns the index of the matching "}". Both recurse into
 * strings, comments and nested templates. */
static size_t scan_template(const char* s, size_t n, size_t i, int* lines);

/* Inside ${...}: a '/' starts a regular expression when the code before it
 * cannot end an operand (same idea as regex_allowed, on characters). */
static int scan_regex_start(const char* s, size_t from, size_t i) {
    size_t k = i;
    while (k > from && (s[k - 1] == ' ' || s[k - 1] == '\t' || s[k - 1] == '\n' || s[k - 1] == '\r')) k--;
    if (k == from) return 1;
    char p = s[k - 1];
    if (strchr("(,=:[!&|?{};+-*%<>~^", p)) return 1;
    /* keywords such as return/typeof that precede an expression */
    static const char* const words[] = {"return", "typeof", "case", "in", "of", "void", "delete",
                                        "throw", "new", "else", "do", "await", "yield"};
    for (size_t w = 0; w < sizeof(words) / sizeof(words[0]); w++) {
        size_t len = strlen(words[w]);
        if (k - from >= len && !memcmp(s + k - len, words[w], len) &&
            (k - from == len || !ident_part((unsigned char)s[k - len - 1])))
            return 1;
    }
    return 0;
}

static size_t scan_code(const char* s, size_t n, size_t i, int* lines) {
    int depth = 0;
    size_t from = i;
    while (i < n) {
        char c = s[i];
        if (c == '\n') (*lines)++;
        if (c == '/' && i + 1 < n && s[i + 1] != '/' && s[i + 1] != '*' && scan_regex_start(s, from, i)) {
            int in_class = 0;
            i++;
            while (i < n && s[i] != '\n') {
                if (s[i] == '\\') i++;
                else if (s[i] == '[') in_class = 1;
                else if (s[i] == ']') in_class = 0;
                else if (s[i] == '/' && !in_class) break;
                i++;
            }
            i++;
            continue;
        }
        if (c == '"' || c == '\'') {
            i++;
            while (i < n && s[i] != c) {
                if (s[i] == '\\') i++;
                i++;
            }
            i++;
            continue;
        }
        if (c == '`') {
            i = scan_template(s, n, i + 1, lines) + 1;
            continue;
        }
        if (c == '/' && i + 1 < n && s[i + 1] == '/') {
            while (i < n && s[i] != '\n') i++;
            continue;
        }
        if (c == '/' && i + 1 < n && s[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(s[i] == '*' && s[i + 1] == '/')) {
                if (s[i] == '\n') (*lines)++;
                i++;
            }
            i += 2;
            continue;
        }
        if (c == '{') depth++;
        else if (c == '}') {
            if (!depth) return i;
            depth--;
        }
        i++;
    }
    return n;
}

static size_t scan_template(const char* s, size_t n, size_t i, int* lines) {
    while (i < n) {
        char c = s[i];
        if (c == '\\') {
            i += 2;
            continue;
        }
        if (c == '\n') (*lines)++;
        if (c == '`') return i;
        if (c == '$' && i + 1 < n && s[i + 1] == '{') {
            i = scan_code(s, n, i + 2, lines) + 1;
            continue;
        }
        i++;
    }
    return n;
}

static int regex_allowed(lexer_t* lx);
static void track_bracket(lexer_t* lx, js_token* t);

/* Called after the new token was pushed: look at the one before it. */
static int regex_allowed(lexer_t* lx) {
    if (lx->count < 2) return 1;
    js_token* t = &lx->tokens[lx->count - 2];
    if (t->type == T_NUM || t->type == T_STR || t->type == T_TEMPLATE ||
        t->type == T_REGEX)
        return 0;
    if (t->type == T_IDENT) {
        js_str* s = t->str;
        return s == K_return || s == K_typeof || s == K_instanceof || s == K_in ||
               s == K_of || s == K_new || s == K_delete || s == K_void ||
               s == K_throw || s == K_case || s == K_do || s == K_else ||
               s == K_await || s == K_yield;
    }
    if (t->type == T_PUNCT) {
        if (t->punct == PACK(")") || t->punct == PACK("}")) return t->regex_after;
        return t->punct != PACK("]") && t->punct != PACK("++") && t->punct != PACK("--");
    }
    return 1;
}

/* Keeps the bracket stack for regex_allowed: `if (x) /re/.test(y)` and
 * `function f() {} /re/` start regular expressions, `(a) / b` and
 * `{a: 1} / 2`-like expressions divide. */
static void track_bracket(lexer_t* lx, js_token* t) {
    uint32_t p = t->punct;
    js_token* prev = lx->count >= 2 ? &lx->tokens[lx->count - 2] : NULL;
    if (p == PACK("(") || p == PACK("[") || p == PACK("{")) {
        uint8_t kind = 0;
        if (p == PACK("(") && prev && prev->type == T_IDENT &&
            (prev->str == K_if || prev->str == K_for || prev->str == K_while || prev->str == K_with))
            kind = 1;
        if (p == PACK("{")) {
            /* a block unless it follows something that makes it an object literal */
            kind = 2;
            if (prev && prev->type == T_PUNCT) {
                uint32_t q = prev->punct;
                if (q == PACK("(") || q == PACK(",") || q == PACK(":") || q == PACK("=") ||
                    q == PACK("[") || q == PACK("?") || q == PACK("||") || q == PACK("&&") ||
                    q == PACK("??") || q == PACK("+") || q == PACK("-") || q == PACK("!") ||
                    q == PACK("...") || q == PACK("==") || q == PACK("===") || q == PACK("!=") ||
                    q == PACK("!=="))
                    kind = 0;
            } else if (prev && prev->type == T_IDENT && (prev->str == K_return || prev->str == K_typeof ||
                                                         prev->str == K_in || prev->str == K_of ||
                                                         prev->str == K_case || prev->str == K_yield ||
                                                         prev->str == K_await)) {
                kind = 0;
            }
        }
        if (lx->open_depth < (int)sizeof(lx->open_kind)) lx->open_kind[lx->open_depth] = kind;
        lx->open_depth++;
    } else if (p == PACK(")") || p == PACK("]") || p == PACK("}")) {
        uint8_t kind = 0;
        if (lx->open_depth > 0) {
            lx->open_depth--;
            if (lx->open_depth < (int)sizeof(lx->open_kind)) kind = lx->open_kind[lx->open_depth];
        }
        t->regex_after = (p == PACK(")") && kind == 1) || (p == PACK("}") && kind == 2);
    }
}

static int lex(lexer_t* lx) {
    const char* s = lx->src;
    size_t n = lx->length;
    int newline = 1;
    while (lx->pos < n) {
        char c = s[lx->pos];
        if (c == '\n') {
            lx->line++;
            newline = 1;
            lx->pos++;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') {
            lx->pos++;
            continue;
        }
        if ((unsigned char)c == 0xC2 && lx->pos + 1 < n && (unsigned char)s[lx->pos + 1] == 0xA0) {
            lx->pos += 2;
            continue;
        }
        if ((unsigned char)c == 0xEF && lx->pos + 2 < n && (unsigned char)s[lx->pos + 1] == 0xBB &&
            (unsigned char)s[lx->pos + 2] == 0xBF) {
            lx->pos += 3;
            continue;
        }
        if ((unsigned char)c == 0xE2 && lx->pos + 2 < n && (unsigned char)s[lx->pos + 1] == 0x80 &&
            ((unsigned char)s[lx->pos + 2] == 0xA8 || (unsigned char)s[lx->pos + 2] == 0xA9)) {
            lx->pos += 3;
            lx->line++;
            newline = 1;
            continue;
        }
        if (c == '/' && lx->pos + 1 < n && s[lx->pos + 1] == '/') {
            while (lx->pos < n && s[lx->pos] != '\n') lx->pos++;
            continue;
        }
        if (c == '/' && lx->pos + 1 < n && s[lx->pos + 1] == '*') {
            lx->pos += 2;
            while (lx->pos + 1 < n && !(s[lx->pos] == '*' && s[lx->pos + 1] == '/')) {
                if (s[lx->pos] == '\n') {
                    lx->line++;
                    newline = 1;
                }
                lx->pos++;
            }
            lx->pos += 2;
            continue;
        }
        if (c == '<' && lx->pos + 3 < n && !strncmp(s + lx->pos, "<!--", 4)) {
            while (lx->pos < n && s[lx->pos] != '\n') lx->pos++;
            continue;
        }
        if (c == '-' && newline && lx->pos + 2 < n && !strncmp(s + lx->pos, "-->", 3)) {
            while (lx->pos < n && s[lx->pos] != '\n') lx->pos++;
            continue;
        }
        js_token* t = lex_push(lx);
        if (!t) return 0;
        t->line = lx->line;
        t->newline_before = (uint8_t)newline;
        t->offset = (uint32_t)lx->pos;
        newline = 0;
        if (ident_start((unsigned char)c) || (c == '\\' && lx->pos + 1 < n && s[lx->pos + 1] == 'u')) {
            size_t start = lx->pos;
            while (lx->pos < n && (ident_part((unsigned char)s[lx->pos]) || s[lx->pos] == '\\')) {
                if ((unsigned char)s[lx->pos] == 0xE2 && lx->pos + 2 < n &&
                    (unsigned char)s[lx->pos + 1] == 0x80 &&
                    ((unsigned char)s[lx->pos + 2] == 0xA8 || (unsigned char)s[lx->pos + 2] == 0xA9))
                    break;
                lx->pos++;
            }
            t->type = T_IDENT;
            if (mx_memchr(s + start, '\\', lx->pos - start)) {
                js_str* decoded = decode_string(s + start, lx->pos - start);
                t->str = js_intern(decoded->data, decoded->length);
            } else {
                t->str = js_intern(s + start, lx->pos - start);
            }
            continue;
        }
        if (c == '#' && lx->pos + 1 < n && ident_start((unsigned char)s[lx->pos + 1])) {
            size_t start = lx->pos;
            lx->pos++;
            while (lx->pos < n && ident_part((unsigned char)s[lx->pos])) lx->pos++;
            t->type = T_IDENT;   /* private names behave as ordinary keys */
            t->str = js_intern(s + start, lx->pos - start);
            continue;
        }
        if ((c >= '0' && c <= '9') || (c == '.' && lx->pos + 1 < n && s[lx->pos + 1] >= '0' &&
                                       s[lx->pos + 1] <= '9')) {
            t->type = T_NUM;
            double value = 0;
            if (c == '0' && lx->pos + 1 < n &&
                (s[lx->pos + 1] == 'x' || s[lx->pos + 1] == 'X' || s[lx->pos + 1] == 'o' ||
                 s[lx->pos + 1] == 'O' || s[lx->pos + 1] == 'b' || s[lx->pos + 1] == 'B')) {
                char kind = (char)(s[lx->pos + 1] | 0x20);
                int base = kind == 'x' ? 16 : kind == 'o' ? 8 : 2;
                lx->pos += 2;
                while (lx->pos < n) {
                    if (s[lx->pos] == '_') {
                        lx->pos++;
                        continue;
                    }
                    int d = hexval(s[lx->pos]);
                    if (d < 0 || d >= base) break;
                    value = value * base + d;
                    lx->pos++;
                }
            } else {
                char buffer[64];
                size_t used = 0;
                int seen_dot = 0, seen_exp = 0;
                while (lx->pos < n && used + 1 < sizeof(buffer)) {
                    char d = s[lx->pos];
                    if (d >= '0' && d <= '9') {
                        buffer[used++] = d;
                    } else if (d == '.' && !seen_dot && !seen_exp) {
                        seen_dot = 1;
                        buffer[used++] = d;
                    } else if ((d == 'e' || d == 'E') && !seen_exp) {
                        seen_exp = 1;
                        buffer[used++] = d;
                        if (lx->pos + 1 < n && (s[lx->pos + 1] == '+' || s[lx->pos + 1] == '-'))
                            buffer[used++] = s[++lx->pos];
                    } else if (d != '_') {
                        break;
                    }
                    lx->pos++;
                }
                buffer[used] = '\0';
                value = js_string_to_number(js_new_str(buffer, used));
            }
            if (lx->pos < n && s[lx->pos] == 'n') lx->pos++;   /* BigInt suffix */
            t->num = value;
            continue;
        }
        if (c == '"' || c == '\'') {
            size_t start = ++lx->pos;
            while (lx->pos < n && s[lx->pos] != c) {
                if (s[lx->pos] == '\\') {
                    lx->pos++;
                    if (lx->pos < n && s[lx->pos] == '\n') lx->line++;
                    if (lx->pos + 1 < n && s[lx->pos] == '\r' && s[lx->pos + 1] == '\n') {
                        lx->pos++;   /* line continuation with CRLF */
                        lx->line++;
                    }
                } else if (s[lx->pos] == '\n') {
                    break;
                }
                lx->pos++;
            }
            if (lx->pos >= n || s[lx->pos] != c) {
                mx_copy(lx->error, sizeof(lx->error), "Invalid or unexpected token (unterminated string)");
                lx->error_line = lx->line;
                return 0;
            }
            t->type = T_STR;
            t->str = decode_string(s + start, lx->pos - start);
            lx->pos++;
            continue;
        }
        if (c == '`') {
            size_t start = ++lx->pos;
            int lines = 0;
            lx->pos = scan_template(s, n, start, &lines);
            lx->line += lines;
            if (lx->pos >= n) {
                mx_copy(lx->error, sizeof(lx->error), "Unterminated template literal");
                lx->error_line = t->line;
                return 0;
            }
            t->type = T_TEMPLATE;
            t->str = js_new_str(s + start, lx->pos - start);
            lx->pos++;
            continue;
        }
        if (c == '/' && regex_allowed(lx)) {
            size_t start = ++lx->pos;
            int in_class = 0;
            while (lx->pos < n) {
                char d = s[lx->pos];
                if (d == '\\') {
                    lx->pos += 2;
                    continue;
                }
                if (d == '\n') break;
                if (d == '[') in_class = 1;
                else if (d == ']') in_class = 0;
                else if (d == '/' && !in_class) break;
                lx->pos++;
            }
            if (lx->pos >= n || s[lx->pos] != '/') {
                mx_copy(lx->error, sizeof(lx->error), "Invalid regular expression: missing /");
                lx->error_line = t->line;
                return 0;
            }
            t->type = T_REGEX;
            t->str = js_new_str(s + start, lx->pos - start);
            lx->pos++;
            size_t flags = lx->pos;
            while (lx->pos < n && ident_part((unsigned char)s[lx->pos])) lx->pos++;
            t->flags = js_new_str(s + flags, lx->pos - flags);
            continue;
        }
        t->type = T_PUNCT;
        int matched = 0;
        static uint8_t punct_length[sizeof(puncts) / sizeof(puncts[0])];
        if (!punct_length[0])
            for (size_t i = 0; i < sizeof(puncts) / sizeof(puncts[0]); i++) punct_length[i] = (uint8_t)strlen(puncts[i]);
        for (size_t i = 0; i < sizeof(puncts) / sizeof(puncts[0]); i++) {
            size_t len = punct_length[i];
            if (puncts[i][0] != c) continue;
            if (lx->pos + len <= n && !memcmp(s + lx->pos, puncts[i], len)) {
                /* "?." followed by a digit is "?" then a number */
                if (len == 2 && puncts[i][0] == '?' && puncts[i][1] == '.' &&
                    lx->pos + 2 < n && s[lx->pos + 2] >= '0' && s[lx->pos + 2] <= '9')
                    continue;
                t->punct = pack(puncts[i]);
                lx->pos += len;
                matched = 1;
                track_bracket(lx, t);
                break;
            }
        }
        if (!matched) {
            mx_copy(lx->error, sizeof(lx->error), "Invalid or unexpected token  near: ");
            size_t used = strlen(lx->error), k = 0;
            while (lx->pos + k < n && k < 24 && used + 1 < sizeof(lx->error) && s[lx->pos + k] != '\n')
                lx->error[used++] = s[lx->pos + k++];
            lx->error[used] = '\0';
            lx->error_line = lx->line;
            return 0;
        }
    }
    js_token* t = lex_push(lx);
    if (!t) return 0;
    t->type = T_EOF;
    t->line = lx->line;
    t->newline_before = 1;
    t->offset = (uint32_t)n;
    return 1;
}

/* ====================================================================== */
/* Parser                                                                 */
/* ====================================================================== */

typedef struct fn_ctx {
    struct fn_ctx* parent;
    js_str** vars;
    int var_count, var_capacity;
    js_node** functions;
    int function_count, function_capacity;
    int uses_arguments;
    int has_closures;
    int arrow;
    int is_async;
    int strict;
} fn_ctx;

typedef struct {
    js_token* tokens;
    uint32_t count;
    uint32_t pos;
    const char* name;
    const char* source;
    int failed;
    char error[160];
    int error_line;
    uint32_t error_offset;
    fn_ctx* fn;
    int no_in;
    int line_offset;
    int module;            /* parsing an ES module */
    js_str* module_url;    /* for import.meta / import() */
} parser_t;

static int parse_as_module;
static js_str* parse_module_url;

static js_node* parse_assignment(parser_t* p);
static js_node* parse_expression(parser_t* p);
static js_node* parse_statement(parser_t* p);
static js_node* parse_block_body(parser_t* p);
static js_func* parse_function_rest(parser_t* p, js_str* name, int arrow,
                                    int is_async, int method);
static js_node* parse_binding(parser_t* p);
static js_node* parse_class(parser_t* p, int declaration);

static js_token* peek(parser_t* p) {
    return &p->tokens[p->pos < p->count ? p->pos : p->count - 1];
}

static js_token* peek_at(parser_t* p, uint32_t ahead) {
    uint32_t i = p->pos + ahead;
    return &p->tokens[i < p->count ? i : p->count - 1];
}

static js_token* next(parser_t* p) {
    js_token* t = peek(p);
    if (p->pos < p->count - 1) p->pos++;
    return t;
}

static int is_punct_s(js_token* t, const char* text) {
    return t->type == T_PUNCT && t->punct == pack(text);
}

static inline int is_punct_p(const js_token* t, uint32_t packed) {
    return t->type == T_PUNCT && t->punct == packed;
}
#define is_punct(t, literal) is_punct_p((t), PACK(literal))

static int is_kw(js_token* t, js_str* kw) {
    return t->type == T_IDENT && t->str == kw;
}

static void fail(parser_t* p, const char* message, js_token* at) {
    if (p->failed) return;
    p->failed = 1;
    mx_copy(p->error, sizeof(p->error), message);
    if (at) {
        if (at->type == T_EOF) {
            mx_copy(p->error, sizeof(p->error), "Unexpected end of input");
        } else if (!strncmp(message, "Unexpected token", 16)) {
            mx_append(p->error, sizeof(p->error), " '");
            if (at->type == T_IDENT || at->type == T_STR) {
                mx_append(p->error, sizeof(p->error), at->str->data);
            } else if (at->type == T_PUNCT) {
                char text[5] = {0};
                for (int i = 0; i < 4; i++) text[i] = (char)(at->punct >> (8 * i));
                mx_append(p->error, sizeof(p->error), text);
            } else if (at->type == T_NUM) {
                mx_append(p->error, sizeof(p->error), "number");
            }
            mx_append(p->error, sizeof(p->error), "'");
        }
        p->error_line = at->line + p->line_offset;
        p->error_offset = at->offset;
    }
}

static int accept(parser_t* p, const char* text) {
    if (is_punct_s(peek(p), text)) {
        next(p);
        return 1;
    }
    return 0;
}

static int accept_kw(parser_t* p, js_str* kw) {
    if (is_kw(peek(p), kw)) {
        next(p);
        return 1;
    }
    return 0;
}

static void expect(parser_t* p, const char* text) {
    if (!accept(p, text)) fail(p, "Unexpected token", peek(p));
}

static void consume_semicolon(parser_t* p) {
    if (accept(p, ";")) return;
    js_token* t = peek(p);
    if (is_punct(t, "}") || t->type == T_EOF || t->newline_before) return;
    fail(p, "Unexpected token", t);
}

static js_node* node(parser_t* p, js_node_type type) {
    js_node* n = (js_node*)js_alloc(sizeof(js_node));
    memset(n, 0, sizeof(*n));
    n->type = (uint8_t)type;
    n->line = peek(p)->line + p->line_offset;
    return n;
}

typedef struct {
    js_node** items;
    uint32_t count, capacity;
} node_list;

static void list_push(node_list* l, js_node* n) {
    if (l->count == l->capacity) {
        uint32_t capacity = l->capacity ? l->capacity * 2 : 4;
        js_node** items = (js_node**)js_alloc(sizeof(js_node*) * capacity);
        if (l->count) memcpy(items, l->items, sizeof(js_node*) * l->count);
        l->items = items;
        l->capacity = capacity;
    }
    l->items[l->count++] = n;
}

static void declare_var(parser_t* p, js_str* name) {
    fn_ctx* f = p->fn;
    for (int i = 0; i < f->var_count; i++)
        if (f->vars[i] == name) return;
    if (f->var_count == f->var_capacity) {
        int capacity = f->var_capacity ? f->var_capacity * 2 : 8;
        js_str** vars = (js_str**)js_alloc(sizeof(js_str*) * (size_t)capacity);
        if (f->var_count) memcpy(vars, f->vars, sizeof(js_str*) * (size_t)f->var_count);
        f->vars = vars;
        f->var_capacity = capacity;
    }
    f->vars[f->var_count++] = name;
}

static void declare_pattern_vars(parser_t* p, js_node* pattern) {
    if (!pattern) return;
    switch (pattern->type) {
    case N_IDENT: declare_var(p, pattern->str); break;
    case N_ASSIGN_PAT: declare_pattern_vars(p, pattern->a); break;
    case N_REST: declare_pattern_vars(p, pattern->a); break;
    case N_ARRAY_PAT:
        for (uint32_t i = 0; i < pattern->count; i++) declare_pattern_vars(p, pattern->list[i]);
        break;
    case N_OBJECT_PAT:
        for (uint32_t i = 0; i < pattern->count; i++)
            declare_pattern_vars(p, pattern->list[i]->b ? pattern->list[i]->b : pattern->list[i]->a);
        break;
    default: break;
    }
}

static void add_function_decl(parser_t* p, js_node* decl) {
    fn_ctx* f = p->fn;
    if (f->function_count == f->function_capacity) {
        int capacity = f->function_capacity ? f->function_capacity * 2 : 4;
        js_node** fns = (js_node**)js_alloc(sizeof(js_node*) * (size_t)capacity);
        if (f->function_count) memcpy(fns, f->functions, sizeof(js_node*) * (size_t)f->function_count);
        f->functions = fns;
        f->function_capacity = capacity;
    }
    f->functions[f->function_count++] = decl;
}

static js_str* property_name(parser_t* p, js_node** computed) {
    js_token* t = peek(p);
    *computed = NULL;
    if (t->type == T_IDENT || t->type == T_STR) {
        next(p);
        return t->type == T_STR ? js_intern(t->str->data, t->str->length) : t->str;
    }
    if (t->type == T_NUM) {
        next(p);
        js_str* s = js_number_to_string(t->num, 10);
        return js_intern(s->data, s->length);
    }
    if (accept(p, "[")) {
        *computed = parse_assignment(p);
        expect(p, "]");
        return NULL;
    }
    fail(p, "Unexpected token", t);
    next(p);
    return js_intern_c("");
}

/* Template literal: raw text with ${...} parts parsed by a nested parser. */
static js_node* parse_template(parser_t* p, js_token* t) {
    js_node* n = node(p, N_TEMPLATE);
    n->line = t->line + p->line_offset;
    node_list parts = {0};
    const char* s = t->str->data;
    size_t length = t->str->length, i = 0, start = 0;
    int line = t->line;
    while (i <= length) {
        if (i == length || (s[i] == '$' && i + 1 < length && s[i + 1] == '{')) {
            js_node* text = node(p, N_STR);
            text->str = decode_string(s + start, i - start);
            js_node* raw = node(p, N_STR);   /* String.raw / tag functions */
            raw->str = js_new_str(s + start, i - start);
            text->b = raw;
            list_push(&parts, text);
            if (i == length) break;
            size_t expr_start = i + 2;
            int ignored_lines = 0;
            size_t j = scan_code(s, length, expr_start, &ignored_lines);
            lexer_t lx;
            memset(&lx, 0, sizeof(lx));
            lx.src = s + expr_start;
            lx.length = j - expr_start;
            lx.line = 1;
            int base_line = line;
            for (size_t k = 0; k < expr_start; k++) if (s[k] == '\n') base_line++;
            if (!lex(&lx)) {
                fail(p, lx.error, t);
                free(lx.tokens);
                return n;
            }
            parser_t inner = *p;
            inner.tokens = lx.tokens;
            inner.count = lx.count;
            inner.pos = 0;
            inner.line_offset = p->line_offset + base_line - 1;
            js_node* expr = parse_expression(&inner);
            if (inner.failed) {
                p->failed = 1;
                mx_copy(p->error, sizeof(p->error), inner.error);
                p->error_line = inner.error_line;
            }
            free(lx.tokens);
            list_push(&parts, expr);
            i = start = j + 1;
            continue;
        }
        i++;
    }
    n->list = parts.items;
    n->count = parts.count;
    return n;
}

static js_node* parse_array_literal(parser_t* p) {
    js_node* n = node(p, N_ARRAY);
    node_list items = {0};
    while (!is_punct(peek(p), "]") && !p->failed) {
        if (is_punct(peek(p), ",")) {
            next(p);
            list_push(&items, node(p, N_HOLE));
            continue;
        }
        if (accept(p, "...")) {
            js_node* spread = node(p, N_SPREAD);
            spread->a = parse_assignment(p);
            list_push(&items, spread);
        } else {
            list_push(&items, parse_assignment(p));
        }
        if (!accept(p, ",")) break;
    }
    expect(p, "]");
    n->list = items.items;
    n->count = items.count;
    return n;
}

static js_node* parse_object_literal(parser_t* p) {
    js_node* n = node(p, N_OBJECT);
    node_list props = {0};
    while (!is_punct(peek(p), "}") && !p->failed) {
        js_node* prop = node(p, N_PROP);
        if (accept(p, "...")) {
            prop->op = PROP_SPREAD;
            prop->b = parse_assignment(p);
            list_push(&props, prop);
            if (!accept(p, ",")) break;
            continue;
        }
        int is_async = 0, is_getter = 0, is_setter = 0;
        js_token* t = peek(p);
        js_token* after = peek_at(p, 1);
        int name_follows = !is_punct(after, ":") && !is_punct(after, "(") &&
                           !is_punct(after, ",") && !is_punct(after, "}") &&
                           !is_punct(after, "=");
        if (is_kw(t, K_async) && name_follows && !after->newline_before) {
            next(p);
            is_async = 1;
        } else if (is_kw(t, K_get) && name_follows) {
            next(p);
            is_getter = 1;
        } else if (is_kw(t, K_set) && name_follows) {
            next(p);
            is_setter = 1;
        }
        int generator = 0;
        if (is_punct(peek(p), "*")) {
            next(p);
            generator = 1;
        }
        js_token* key_token = peek(p);
        js_node* computed;
        prop->str = property_name(p, &computed);
        prop->a = computed;
        prop->flag = computed != NULL;
        if (is_getter || is_setter || is_punct(peek(p), "(")) {
            js_func* f = parse_function_rest(p, prop->str, 0, is_async, 1);
            f->generator = (uint8_t)generator;
            js_node* fn = node(p, N_FUNC);
            fn->func = f;
            prop->b = fn;
            prop->op = is_getter ? PROP_GET : is_setter ? PROP_SET : PROP_INIT;
        } else if (accept(p, ":")) {
            prop->b = parse_assignment(p);
            prop->op = PROP_INIT;
        } else {
            /* shorthand, possibly with a default (only valid in patterns) */
            if (key_token->type != T_IDENT) fail(p, "Unexpected token", key_token);
            js_node* ident = node(p, N_IDENT);
            ident->str = prop->str;
            if (ident->str == K_arguments) p->fn->uses_arguments = 1;
            prop->op = PROP_SHORTHAND;
            if (accept(p, "=")) {
                js_node* def = node(p, N_ASSIGN_PAT);
                def->a = ident;
                def->b = parse_assignment(p);
                prop->b = def;
            } else {
                prop->b = ident;
            }
        }
        list_push(&props, prop);
        if (!accept(p, ",")) break;
    }
    expect(p, "}");
    n->list = props.items;
    n->count = props.count;
    return n;
}

/* Turns an expression into an assignment target pattern. */
static js_node* to_pattern(parser_t* p, js_node* e) {
    if (!e) return e;
    switch (e->type) {
    case N_IDENT: case N_MEMBER: case N_ASSIGN_PAT: case N_ARRAY_PAT:
    case N_OBJECT_PAT: case N_REST:
        return e;
    case N_ASSIGN:
        if (e->op == 0) {   /* plain `=`: a default value */
            js_node* d = node(p, N_ASSIGN_PAT);
            d->line = e->line;
            d->a = to_pattern(p, e->a);
            d->b = e->b;
            return d;
        }
        break;
    case N_ARRAY: {
        e->type = N_ARRAY_PAT;
        for (uint32_t i = 0; i < e->count; i++) {
            js_node* item = e->list[i];
            if (item->type == N_SPREAD) {
                item->type = N_REST;
                item->a = to_pattern(p, item->a);
            } else if (item->type != N_HOLE) {
                e->list[i] = to_pattern(p, item);
            }
        }
        return e;
    }
    case N_OBJECT: {
        e->type = N_OBJECT_PAT;
        for (uint32_t i = 0; i < e->count; i++) {
            js_node* prop = e->list[i];
            if (prop->op == PROP_SPREAD) {
                js_node* rest = node(p, N_REST);
                rest->a = to_pattern(p, prop->b);
                prop->b = rest;
            } else {
                prop->b = to_pattern(p, prop->b);
            }
        }
        return e;
    }
    default:
        break;
    }
    fail(p, "Invalid destructuring assignment target", peek(p));
    return e;
}

static js_node* parse_binding(parser_t* p) {
    js_token* t = peek(p);
    if (is_punct(t, "[")) {
        next(p);
        js_node* n = node(p, N_ARRAY_PAT);
        node_list items = {0};
        while (!is_punct(peek(p), "]") && !p->failed) {
            if (is_punct(peek(p), ",")) {
                next(p);
                list_push(&items, node(p, N_HOLE));
                continue;
            }
            if (accept(p, "...")) {
                js_node* rest = node(p, N_REST);
                rest->a = parse_binding(p);
                list_push(&items, rest);
            } else {
                js_node* target = parse_binding(p);
                if (accept(p, "=")) {
                    js_node* d = node(p, N_ASSIGN_PAT);
                    d->a = target;
                    d->b = parse_assignment(p);
                    target = d;
                }
                list_push(&items, target);
            }
            if (!accept(p, ",")) break;
        }
        expect(p, "]");
        n->list = items.items;
        n->count = items.count;
        return n;
    }
    if (is_punct(t, "{")) {
        next(p);
        js_node* n = node(p, N_OBJECT_PAT);
        node_list props = {0};
        while (!is_punct(peek(p), "}") && !p->failed) {
            js_node* prop = node(p, N_PROP);
            if (accept(p, "...")) {
                prop->op = PROP_SPREAD;
                js_node* rest = node(p, N_REST);
                rest->a = parse_binding(p);
                prop->b = rest;
            } else {
                js_token* key_token = peek(p);
                js_node* computed;
                prop->str = property_name(p, &computed);
                prop->a = computed;
                prop->flag = computed != NULL;
                js_node* target;
                if (accept(p, ":")) {
                    target = parse_binding(p);
                } else {
                    if (key_token->type != T_IDENT) fail(p, "Unexpected token", key_token);
                    target = node(p, N_IDENT);
                    target->str = prop->str;
                }
                if (accept(p, "=")) {
                    js_node* d = node(p, N_ASSIGN_PAT);
                    d->a = target;
                    d->b = parse_assignment(p);
                    target = d;
                }
                prop->b = target;
            }
            list_push(&props, prop);
            if (!accept(p, ",")) break;
        }
        expect(p, "}");
        n->list = props.items;
        n->count = props.count;
        return n;
    }
    if (t->type == T_IDENT) {
        next(p);
        js_node* n = node(p, N_IDENT);
        n->str = t->str;
        n->line = t->line + p->line_offset;
        return n;
    }
    fail(p, "Unexpected token", t);
    next(p);
    return node(p, N_IDENT);
}

static void parse_params(parser_t* p, js_func* f) {
    node_list params = {0};
    expect(p, "(");
    while (!is_punct(peek(p), ")") && !p->failed) {
        if (accept(p, "...")) {
            js_node* rest = node(p, N_REST);
            rest->a = parse_binding(p);
            list_push(&params, rest);
        } else {
            js_node* target = parse_binding(p);
            if (accept(p, "=")) {
                js_node* d = node(p, N_ASSIGN_PAT);
                d->a = target;
                d->b = parse_assignment(p);
                target = d;
            }
            list_push(&params, target);
        }
        if (!accept(p, ",")) break;
    }
    expect(p, ")");
    f->params = params.items;
    f->param_count = (int)params.count;
}

static int has_use_strict(js_node* body) {
    if (!body || body->type != N_BLOCK) return 0;
    for (uint32_t i = 0; i < body->count; i++) {
        js_node* s = body->list[i];
        if (s->type != N_EXPR_STMT || s->a->type != N_STR) return 0;
        if (js_str_eq_c(s->a->str, "use strict")) return 1;
    }
    return 0;
}

static js_func* finish_function(parser_t* p, js_func* f, fn_ctx* ctx) {
    if (has_use_strict(f->body) || ctx->strict) f->strict = 1;
    if (ctx->parent && ctx->parent->strict) f->strict = 1;
    f->vars = ctx->vars;
    f->var_count = ctx->var_count;
    f->functions = ctx->functions;
    f->function_count = ctx->function_count;
    f->uses_arguments = (uint8_t)ctx->uses_arguments;
    f->has_closures = (uint8_t)ctx->has_closures;
    f->source_name = p->name;
    p->fn = ctx->parent;
    if (p->fn) {
        p->fn->has_closures = 1;
        if (f->arrow && ctx->uses_arguments) p->fn->uses_arguments = 1;
    }
    return f;
}

/* After the function name: (params) { body } */
static js_func* parse_function_rest(parser_t* p, js_str* name, int arrow,
                                    int is_async, int method) {
    (void)method;
    js_func* f = (js_func*)js_alloc(sizeof(js_func));
    memset(f, 0, sizeof(*f));
    f->name = name;
    f->arrow = (uint8_t)arrow;
    f->is_async = (uint8_t)is_async;
    fn_ctx* ctx = (fn_ctx*)js_alloc(sizeof(fn_ctx));
    memset(ctx, 0, sizeof(*ctx));
    ctx->parent = p->fn;
    ctx->arrow = arrow;
    ctx->is_async = is_async;
    p->fn = ctx;
    int saved_no_in = p->no_in;
    p->no_in = 0;
    parse_params(p, f);
    if (is_punct(peek(p), "{")) {
        next(p);
        f->body = parse_block_body(p);
    } else {
        fail(p, "Unexpected token", peek(p));
    }
    p->no_in = saved_no_in;
    return finish_function(p, f, ctx);
}

static js_node* parse_arrow(parser_t* p, int is_async) {
    js_func* f = (js_func*)js_alloc(sizeof(js_func));
    memset(f, 0, sizeof(*f));
    f->arrow = 1;
    f->is_async = (uint8_t)is_async;
    fn_ctx* ctx = (fn_ctx*)js_alloc(sizeof(fn_ctx));
    memset(ctx, 0, sizeof(*ctx));
    ctx->parent = p->fn;
    ctx->arrow = 1;
    ctx->is_async = is_async;
    p->fn = ctx;
    if (peek(p)->type == T_IDENT) {
        js_node* param = node(p, N_IDENT);
        param->str = next(p)->str;
        f->params = (js_node**)js_alloc(sizeof(js_node*));
        f->params[0] = param;
        f->param_count = 1;
    } else {
        parse_params(p, f);
    }
    expect(p, "=>");
    if (is_punct(peek(p), "{")) {
        next(p);
        f->body = parse_block_body(p);
    } else {
        f->expression_body = 1;
        f->body = parse_assignment(p);
    }
    finish_function(p, f, ctx);
    js_node* n = node(p, N_FUNC);
    n->func = f;
    return n;
}

/* After '(' at the current position, does a '=>' follow the matching ')'? */
static int arrow_ahead(parser_t* p) {
    uint32_t i = p->pos;
    if (!is_punct(&p->tokens[i], "(")) return 0;
    int depth = 0;
    for (; i < p->count; i++) {
        js_token* t = &p->tokens[i];
        if (t->type != T_PUNCT) continue;
        if (t->punct == PACK("(") || t->punct == PACK("[") || t->punct == PACK("{")) depth++;
        else if (t->punct == PACK(")") || t->punct == PACK("]") || t->punct == PACK("}")) {
            depth--;
            if (!depth) {
                return i + 1 < p->count && is_punct(&p->tokens[i + 1], "=>") &&
                       !p->tokens[i + 1].newline_before;
            }
        }
    }
    return 0;
}

static js_node* parse_primary(parser_t* p) {
    js_token* t = peek(p);
    js_node* n;
    switch (t->type) {
    case T_NUM:
        next(p);
        n = node(p, N_NUM);
        n->num = t->num;
        n->line = t->line + p->line_offset;
        return n;
    case T_STR:
        next(p);
        n = node(p, N_STR);
        n->str = t->str;
        return n;
    case T_TEMPLATE:
        next(p);
        return parse_template(p, t);
    case T_REGEX:
        next(p);
        n = node(p, N_REGEX);
        n->str = t->str;
        n->a = node(p, N_STR);
        n->a->str = t->flags;
        return n;
    case T_IDENT: {
        js_str* s = t->str;
        if (s->length > 1 && s->data[0] == '#' && is_kw(peek_at(p, 1), K_in)) {
            /* `#field in object`: private fields are keys named "#field" */
            next(p);
            n = node(p, N_STR);
            n->str = s;
            n->line = t->line + p->line_offset;
            return n;
        }
        if (s == K_function) {
            next(p);
            int generator = accept(p, "*");
            js_str* name = peek(p)->type == T_IDENT && !is_punct(peek(p), "(") ? next(p)->str : NULL;
            n = node(p, N_FUNC);
            n->func = parse_function_rest(p, name, 0, 0, 0);
            n->func->generator = (uint8_t)generator;
            return n;
        }
        if (s == K_async && is_kw(peek_at(p, 1), K_function) && !peek_at(p, 1)->newline_before) {
            next(p);
            next(p);
            int generator = accept(p, "*");
            js_str* name = peek(p)->type == T_IDENT ? next(p)->str : NULL;
            n = node(p, N_FUNC);
            n->func = parse_function_rest(p, name, 0, 1, 0);
            n->func->generator = (uint8_t)generator;
            return n;
        }
        if (s == K_async && !peek_at(p, 1)->newline_before) {
            js_token* after = peek_at(p, 1);
            if (after->type == T_IDENT && is_punct(peek_at(p, 2), "=>")) {
                next(p);
                return parse_arrow(p, 1);
            }
            if (is_punct(after, "(")) {
                uint32_t saved = p->pos;
                next(p);
                if (arrow_ahead(p)) return parse_arrow(p, 1);
                p->pos = saved;
            }
        }
        if (is_punct(peek_at(p, 1), "=>") && !peek_at(p, 1)->newline_before)
            return parse_arrow(p, 0);
        if (s == K_class) return parse_class(p, 0);
        next(p);
        if (s == K_this) return node(p, N_THIS);
        if (s == K_true) return node(p, N_TRUE);
        if (s == K_false) return node(p, N_FALSE);
        if (s == K_null) return node(p, N_NULL);
        if (s == K_super) return node(p, N_SUPER);
        if (s == K_new) {
            if (accept(p, ".")) {   /* new.target */
                next(p);
                return node(p, N_UNDEF);
            }
            n = node(p, N_NEW);
            n->line = t->line + p->line_offset;
            /* member expression without call */
            js_node* callee;
            if (is_kw(peek(p), K_new)) {
                callee = parse_primary(p);
            } else {
                callee = parse_primary(p);
                for (;;) {
                    if (accept(p, ".")) {
                        js_node* m = node(p, N_MEMBER);
                        m->a = callee;
                        m->str = next(p)->str;
                        callee = m;
                    } else if (is_punct(peek(p), "[")) {
                        next(p);
                        js_node* m = node(p, N_MEMBER);
                        m->a = callee;
                        m->b = parse_expression(p);
                        m->flag = 1;
                        expect(p, "]");
                        callee = m;
                    } else {
                        break;
                    }
                }
            }
            n->a = callee;
            node_list args = {0};
            if (accept(p, "(")) {
                while (!is_punct(peek(p), ")") && !p->failed) {
                    if (accept(p, "...")) {
                        js_node* spread = node(p, N_SPREAD);
                        spread->a = parse_assignment(p);
                        list_push(&args, spread);
                    } else {
                        list_push(&args, parse_assignment(p));
                    }
                    if (!accept(p, ",")) break;
                }
                expect(p, ")");
            }
            n->list = args.items;
            n->count = args.count;
            return n;
        }
        if (s == K_import) {
            if (accept(p, "(")) {
                n = node(p, N_IMPORT_CALL);
                n->a = parse_assignment(p);
                if (accept(p, ",")) parse_assignment(p);   /* import options */
                expect(p, ")");
                n->str = p->module_url;
                return n;
            }
            if (accept(p, ".")) {
                next(p);   /* meta */
                n = node(p, N_IMPORT_META);
                n->str = p->module_url;
                return n;
            }
            fail(p, "Cannot use import statement outside a module", t);
            return node(p, N_UNDEF);
        }
        if (s == K_arguments) p->fn->uses_arguments = 1;
        n = node(p, N_IDENT);
        n->str = s;
        n->line = t->line + p->line_offset;
        return n;
    }
    case T_PUNCT:
        if (is_punct(t, "(")) {
            if (arrow_ahead(p)) return parse_arrow(p, 0);
            next(p);
            int saved = p->no_in;
            p->no_in = 0;
            n = parse_expression(p);
            p->no_in = saved;
            expect(p, ")");
            return n;
        }
        if (is_punct(t, "[")) {
            next(p);
            return parse_array_literal(p);
        }
        if (is_punct(t, "{")) {
            next(p);
            return parse_object_literal(p);
        }
        break;
    default:
        break;
    }
    fail(p, "Unexpected token", t);
    next(p);
    return node(p, N_UNDEF);
}

static void parse_arguments(parser_t* p, js_node* call) {
    node_list args = {0};
    while (!is_punct(peek(p), ")") && !p->failed) {
        if (accept(p, "...")) {
            js_node* spread = node(p, N_SPREAD);
            spread->a = parse_assignment(p);
            list_push(&args, spread);
        } else {
            list_push(&args, parse_assignment(p));
        }
        if (!accept(p, ",")) break;
    }
    expect(p, ")");
    call->list = args.items;
    call->count = args.count;
}

static js_node* parse_call_member(parser_t* p) {
    js_node* e = parse_primary(p);
    int in_chain = 0;
    while (!p->failed) {
        js_token* t = peek(p);
        if (is_punct(t, ".")) {
            next(p);
            js_node* m = node(p, N_MEMBER);
            m->a = e;
            js_token* name = next(p);
            if (name->type != T_IDENT) fail(p, "Unexpected token", name);
            m->str = name->str;
            m->flag2 = (uint8_t)in_chain;
            e = m;
        } else if (is_punct(t, "?.")) {
            next(p);
            in_chain = 1;
            if (accept(p, "(")) {
                js_node* call = node(p, N_CALL);
                call->a = e;
                call->flag2 = 1;
                parse_arguments(p, call);
                e = call;
            } else if (accept(p, "[")) {
                js_node* m = node(p, N_MEMBER);
                m->a = e;
                m->b = parse_expression(p);
                m->flag = 1;
                m->flag2 = 1;
                expect(p, "]");
                e = m;
            } else {
                js_node* m = node(p, N_MEMBER);
                m->a = e;
                m->str = next(p)->str;
                m->flag2 = 1;
                e = m;
            }
        } else if (is_punct(t, "[")) {
            next(p);
            js_node* m = node(p, N_MEMBER);
            m->a = e;
            int saved = p->no_in;
            p->no_in = 0;
            m->b = parse_expression(p);
            p->no_in = saved;
            m->flag = 1;
            m->flag2 = (uint8_t)in_chain;
            expect(p, "]");
            e = m;
        } else if (is_punct(t, "(")) {
            next(p);
            js_node* call = node(p, N_CALL);
            call->line = t->line + p->line_offset;
            call->a = e;
            call->flag2 = (uint8_t)in_chain;
            parse_arguments(p, call);
            e = call;
        } else if (t->type == T_TEMPLATE) {
            next(p);
            js_node* tagged = node(p, N_TAGGED);
            tagged->a = e;
            tagged->b = parse_template(p, t);
            e = tagged;
        } else {
            break;
        }
    }
    return e;
}

static js_node* parse_unary(parser_t* p);

static js_node* parse_postfix(parser_t* p) {
    js_node* e = parse_call_member(p);
    js_token* t = peek(p);
    if ((is_punct(t, "++") || is_punct(t, "--")) && !t->newline_before) {
        next(p);
        js_node* u = node(p, N_UPDATE);
        u->op = t->punct == PACK("++") ? '+' : '-';
        u->flag = 0;   /* postfix */
        u->a = e;
        return u;
    }
    return e;
}

static js_node* parse_unary(parser_t* p) {
    js_token* t = peek(p);
    if (t->type == T_PUNCT) {
        const char* ops[] = {"!", "~", "+", "-"};
        for (int i = 0; i < 4; i++)
            if (is_punct_s(t, ops[i])) {
                next(p);
                js_node* u = node(p, N_UNARY);
                u->op = (uint8_t)ops[i][0];
                u->a = parse_unary(p);
                return u;
            }
        if (is_punct(t, "++") || is_punct(t, "--")) {
            next(p);
            js_node* u = node(p, N_UPDATE);
            u->op = t->punct == PACK("++") ? '+' : '-';
            u->flag = 1;
            u->a = parse_unary(p);
            return u;
        }
    }
    if (t->type == T_IDENT) {
        if (t->str == K_typeof || t->str == K_void || t->str == K_delete) {
            next(p);
            js_node* u = node(p, N_UNARY);
            u->op = t->str == K_typeof ? 't' : t->str == K_void ? 'v' : 'd';
            u->a = parse_unary(p);
            return u;
        }
        if (t->str == K_await && (p->fn->is_async || !p->fn->parent) &&
            !is_punct(peek_at(p, 1), "=>") && !is_punct(peek_at(p, 1), ")") &&
            !is_punct(peek_at(p, 1), ";") && !is_punct(peek_at(p, 1), ",")) {
            next(p);
            js_node* u = node(p, N_AWAIT);
            u->a = parse_unary(p);
            return u;
        }
    }
    return parse_postfix(p);
}

static int binary_precedence(parser_t* p, js_token* t) {
    if (t->type == T_IDENT) {
        if (t->str == K_instanceof) return 8;
        if (t->str == K_in && !p->no_in) return 8;
        return 0;
    }
    if (t->type != T_PUNCT) return 0;
    static const struct { const char* op; int prec; } table[] = {
        {"??", 1}, {"||", 2}, {"&&", 3}, {"|", 4}, {"^", 5}, {"&", 6},
        {"==", 7}, {"!=", 7}, {"===", 7}, {"!==", 7}, {"<", 8}, {">", 8},
        {"<=", 8}, {">=", 8}, {"<<", 9}, {">>", 9}, {">>>", 9}, {"+", 10},
        {"-", 10}, {"*", 11}, {"/", 11}, {"%", 11}, {"**", 12},
    };
    static uint32_t packed[sizeof(table) / sizeof(table[0])];
    if (!packed[0])
        for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) packed[i] = pack(table[i].op);
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++)
        if (t->punct == packed[i]) return table[i].prec;
    return 0;
}

static js_node* parse_binary(parser_t* p, int min_prec) {
    js_node* left = parse_unary(p);
    for (;;) {
        js_token* t = peek(p);
        int prec = binary_precedence(p, t);
        if (!prec || prec < min_prec) return left;
        next(p);
        int right_assoc = prec == 12;
        js_node* right = parse_binary(p, right_assoc ? prec : prec + 1);
        js_node* b;
        if (t->type == T_PUNCT &&
            (t->punct == PACK("&&") || t->punct == PACK("||") || t->punct == PACK("??"))) {
            b = node(p, N_LOGICAL);
            b->op = t->punct == PACK("&&") ? '&' : t->punct == PACK("||") ? '|' : '?';
        } else {
            b = node(p, N_BINARY);
            if (t->type == T_IDENT) b->op = t->str == K_in ? 'i' : 'I';
        }
        b->line = t->line + p->line_offset;
        b->num = (double)t->punct;   /* operator, packed */
        b->a = left;
        b->b = right;
        left = b;
    }
}

static js_node* parse_conditional(parser_t* p) {
    js_node* test = parse_binary(p, 1);
    if (!accept(p, "?")) return test;
    js_node* n = node(p, N_COND);
    n->a = test;
    int saved = p->no_in;
    p->no_in = 0;
    n->b = parse_assignment(p);
    p->no_in = saved;
    expect(p, ":");
    n->c = parse_assignment(p);
    return n;
}

static const char* const assign_ops[] = {
    "=", "+=", "-=", "*=", "/=", "%=", "**=", "<<=", ">>=", ">>>=", "&=",
    "|=", "^=", "&&=", "||=", "?\?="
};

static js_node* parse_assignment(parser_t* p) {
    if (is_kw(peek(p), K_yield) && p->fn->parent) {
        next(p);
        js_node* y = node(p, N_YIELD);
        if (accept(p, "*")) y->flag = 1;
        if (!peek(p)->newline_before && !is_punct(peek(p), ")") && !is_punct(peek(p), ";") &&
            !is_punct(peek(p), "]") && !is_punct(peek(p), "}") && !is_punct(peek(p), ","))
            y->a = parse_assignment(p);
        return y;
    }
    js_node* left = parse_conditional(p);
    js_token* t = peek(p);
    if (t->type != T_PUNCT) return left;
    static uint32_t assign_packed[sizeof(assign_ops) / sizeof(assign_ops[0])];
    if (!assign_packed[0])
        for (size_t i = 0; i < sizeof(assign_ops) / sizeof(assign_ops[0]); i++) assign_packed[i] = pack(assign_ops[i]);
    for (size_t i = 0; i < sizeof(assign_ops) / sizeof(assign_ops[0]); i++) {
        if (t->punct != assign_packed[i]) continue;
        next(p);
        js_node* a = node(p, N_ASSIGN);
        a->line = t->line + p->line_offset;
        a->op = (uint8_t)i;
        if (i == 0 && (left->type == N_ARRAY || left->type == N_OBJECT))
            left = to_pattern(p, left);
        else if (left->type != N_IDENT && left->type != N_MEMBER)
            fail(p, "Invalid left-hand side in assignment", t);
        a->a = left;
        a->b = parse_assignment(p);
        return a;
    }
    return left;
}

static js_node* parse_expression(parser_t* p) {
    js_node* e = parse_assignment(p);
    if (!is_punct(peek(p), ",")) return e;
    js_node* seq = node(p, N_SEQ);
    node_list items = {0};
    list_push(&items, e);
    while (accept(p, ",")) list_push(&items, parse_assignment(p));
    seq->list = items.items;
    seq->count = items.count;
    return seq;
}

/* ---- statements ---- */

static js_node* parse_var(parser_t* p, int kind, int in_for) {
    js_node* n = node(p, N_VAR);
    n->op = (uint8_t)kind;
    node_list decls = {0};
    do {
        js_node* d = node(p, N_DECL);
        d->a = parse_binding(p);
        if (kind == VAR_VAR) declare_pattern_vars(p, d->a);
        if (accept(p, "=")) d->b = parse_assignment(p);
        list_push(&decls, d);
    } while (accept(p, ","));
    n->list = decls.items;
    n->count = decls.count;
    if (!in_for) consume_semicolon(p);
    return n;
}

static js_node* parse_block_body(parser_t* p) {
    js_node* n = node(p, N_BLOCK);
    node_list items = {0};
    while (!is_punct(peek(p), "}") && peek(p)->type != T_EOF && !p->failed) {
        js_node* s = parse_statement(p);
        if (s) {
            list_push(&items, s);
            if ((s->type == N_VAR && s->op != VAR_VAR) || s->type == N_CLASS_DECL)
                n->flag = 1;   /* has lexical declarations */
        }
    }
    expect(p, "}");
    n->list = items.items;
    n->count = items.count;
    return n;
}

static js_node* parse_class(parser_t* p, int declaration) {
    js_token* start = next(p);   /* class */
    js_node* n = node(p, declaration ? N_CLASS_DECL : N_CLASS);
    n->line = start->line + p->line_offset;
    if (peek(p)->type == T_IDENT && !is_kw(peek(p), K_extends)) n->str = next(p)->str;
    if (accept_kw(p, K_extends)) n->a = parse_call_member(p);
    expect(p, "{");
    node_list members = {0};
    while (!is_punct(peek(p), "}") && !p->failed) {
        if (accept(p, ";")) continue;
        js_node* m = node(p, N_PROP);
        js_token* t = peek(p);
        if (is_kw(t, K_static) && !is_punct(peek_at(p, 1), "(") && !is_punct(peek_at(p, 1), "=")) {
            next(p);
            m->flag2 = 1;
            if (is_punct(peek(p), "{")) {   /* static block */
                next(p);
                js_func* f = (js_func*)js_alloc(sizeof(js_func));
                memset(f, 0, sizeof(*f));
                fn_ctx* ctx = (fn_ctx*)js_alloc(sizeof(fn_ctx));
                memset(ctx, 0, sizeof(*ctx));
                ctx->parent = p->fn;
                p->fn = ctx;
                f->body = parse_block_body(p);
                finish_function(p, f, ctx);
                m->op = 9;   /* static block */
                m->func = f;
                list_push(&members, m);
                continue;
            }
        }
        int is_async = 0, is_getter = 0, is_setter = 0;
        t = peek(p);
        js_token* after = peek_at(p, 1);
        int name_follows = !is_punct(after, "(") && !is_punct(after, "=") &&
                           !is_punct(after, ";") && !is_punct(after, "}");
        if (is_kw(t, K_async) && name_follows) { next(p); is_async = 1; }
        else if (is_kw(t, K_get) && name_follows) { next(p); is_getter = 1; }
        else if (is_kw(t, K_set) && name_follows) { next(p); is_setter = 1; }
        int generator = accept(p, "*");
        js_node* computed;
        m->str = property_name(p, &computed);
        m->a = computed;
        m->flag = computed != NULL;
        if (is_punct(peek(p), "(")) {
            js_func* f = parse_function_rest(p, m->str, 0, is_async, 1);
            f->strict = 1;
            f->generator = (uint8_t)generator;
            m->op = is_getter ? PROP_GET : is_setter ? PROP_SET : PROP_INIT;
            m->func = f;
            if (m->str == S_constructor && !m->flag2) m->op = 8;   /* constructor */
        } else {
            /* field */
            m->op = 7;
            if (accept(p, "=")) {
                js_func* f = (js_func*)js_alloc(sizeof(js_func));
                memset(f, 0, sizeof(*f));
                f->arrow = 1;
                f->expression_body = 1;
                fn_ctx* ctx = (fn_ctx*)js_alloc(sizeof(fn_ctx));
                memset(ctx, 0, sizeof(*ctx));
                ctx->parent = p->fn;
                ctx->arrow = 1;
                p->fn = ctx;
                f->body = parse_assignment(p);
                finish_function(p, f, ctx);
                m->func = f;
            }
            consume_semicolon(p);
        }
        list_push(&members, m);
    }
    expect(p, "}");
    n->list = members.items;
    n->count = members.count;
    p->fn->has_closures = 1;
    return n;
}

static js_node* parse_statement(parser_t* p) {
    js_token* t = peek(p);
    if (p->failed) return NULL;
    if (t->type == T_PUNCT) {
        if (is_punct(t, "{")) {
            next(p);
            return parse_block_body(p);
        }
        if (is_punct(t, ";")) {
            next(p);
            return node(p, N_EMPTY);
        }
    }
    if (t->type == T_IDENT) {
        js_str* s = t->str;
        if (s == K_var) {
            next(p);
            return parse_var(p, VAR_VAR, 0);
        }
        if ((s == K_let && (peek_at(p, 1)->type == T_IDENT || is_punct(peek_at(p, 1), "[") ||
                            is_punct(peek_at(p, 1), "{"))) ||
            s == K_const) {
            next(p);
            return parse_var(p, s == K_let ? VAR_LET : VAR_CONST, 0);
        }
        if (s == K_function || (s == K_async && is_kw(peek_at(p, 1), K_function) &&
                                !peek_at(p, 1)->newline_before)) {
            int is_async = s == K_async;
            next(p);
            if (is_async) next(p);
            int generator = accept(p, "*");
            js_token* name = next(p);
            if (name->type != T_IDENT) {
                fail(p, "Function statements require a function name", name);
                return NULL;
            }
            js_node* n = node(p, N_FUNC_DECL);
            n->str = name->str;
            n->func = parse_function_rest(p, name->str, 0, is_async, 0);
            n->func->generator = (uint8_t)generator;
            declare_var(p, name->str);
            add_function_decl(p, n);
            return node(p, N_EMPTY);
        }
        if (s == K_class) return parse_class(p, 1);
        if (s == K_if) {
            next(p);
            js_node* n = node(p, N_IF);
            n->line = t->line + p->line_offset;
            expect(p, "(");
            n->a = parse_expression(p);
            expect(p, ")");
            n->b = parse_statement(p);
            if (accept_kw(p, K_else)) n->c = parse_statement(p);
            return n;
        }
        if (s == K_for) {
            next(p);
            int line = t->line + p->line_offset;
            int for_await = 0;
            if (is_kw(peek(p), K_await)) {
                next(p);
                for_await = 1;
            }
            expect(p, "(");
            js_node* init = NULL;
            int kind = -1;
            p->no_in = 1;
            if (is_kw(peek(p), K_var) || is_kw(peek(p), K_const) ||
                (is_kw(peek(p), K_let) && !is_punct(peek_at(p, 1), "in") &&
                 !is_kw(peek_at(p, 1), K_of))) {
                js_str* kw = next(p)->str;
                kind = kw == K_var ? VAR_VAR : kw == K_let ? VAR_LET : VAR_CONST;
                init = parse_var(p, kind, 1);
            } else if (!is_punct(peek(p), ";")) {
                init = parse_expression(p);
            }
            p->no_in = 0;
            if (init && (is_kw(peek(p), K_in) || is_kw(peek(p), K_of))) {
                int of = is_kw(peek(p), K_of);
                next(p);
                js_node* n = node(p, of ? N_FOR_OF : N_FOR_IN);
                n->line = line;
                n->flag = (uint8_t)for_await;
                if (init->type != N_VAR) init = to_pattern(p, init);
                n->a = init;
                n->b = of ? parse_assignment(p) : parse_expression(p);
                expect(p, ")");
                n->c = parse_statement(p);
                return n;
            }
            js_node* n = node(p, N_FOR);
            n->line = line;
            n->a = init;
            expect(p, ";");
            if (!is_punct(peek(p), ";")) n->b = parse_expression(p);
            expect(p, ";");
            if (!is_punct(peek(p), ")")) n->c = parse_expression(p);
            expect(p, ")");
            n->d = parse_statement(p);
            return n;
        }
        if (s == K_while) {
            next(p);
            js_node* n = node(p, N_WHILE);
            n->line = t->line + p->line_offset;
            expect(p, "(");
            n->a = parse_expression(p);
            expect(p, ")");
            n->b = parse_statement(p);
            return n;
        }
        if (s == K_do) {
            next(p);
            js_node* n = node(p, N_DO);
            n->a = parse_statement(p);
            if (!accept_kw(p, K_while)) fail(p, "Unexpected token", peek(p));
            expect(p, "(");
            n->b = parse_expression(p);
            expect(p, ")");
            accept(p, ";");
            return n;
        }
        if (s == K_return) {
            next(p);
            js_node* n = node(p, N_RETURN);
            n->line = t->line + p->line_offset;
            if (!is_punct(peek(p), ";") && !is_punct(peek(p), "}") &&
                peek(p)->type != T_EOF && !peek(p)->newline_before)
                n->a = parse_expression(p);
            consume_semicolon(p);
            return n;
        }
        if (s == K_break || s == K_continue) {
            next(p);
            js_node* n = node(p, s == K_break ? N_BREAK : N_CONTINUE);
            if (peek(p)->type == T_IDENT && !peek(p)->newline_before) n->str = next(p)->str;
            consume_semicolon(p);
            return n;
        }
        if (s == K_throw) {
            next(p);
            js_node* n = node(p, N_THROW);
            n->line = t->line + p->line_offset;
            n->a = parse_expression(p);
            consume_semicolon(p);
            return n;
        }
        if (s == K_try) {
            next(p);
            js_node* n = node(p, N_TRY);
            expect(p, "{");
            n->a = parse_block_body(p);
            if (accept_kw(p, K_catch)) {
                if (accept(p, "(")) {
                    n->b = parse_binding(p);
                    expect(p, ")");
                }
                expect(p, "{");
                n->c = parse_block_body(p);
            }
            if (accept_kw(p, K_finally)) {
                expect(p, "{");
                n->d = parse_block_body(p);
            }
            if (!n->c && !n->d) fail(p, "Missing catch or finally after try", peek(p));
            return n;
        }
        if (s == K_switch) {
            next(p);
            js_node* n = node(p, N_SWITCH);
            expect(p, "(");
            n->a = parse_expression(p);
            expect(p, ")");
            expect(p, "{");
            node_list cases = {0};
            while (!is_punct(peek(p), "}") && !p->failed) {
                js_node* c = node(p, N_CASE);
                if (accept_kw(p, K_case)) {
                    c->a = parse_expression(p);
                } else if (!accept_kw(p, S_default)) {
                    fail(p, "Unexpected token", peek(p));
                    break;
                }
                expect(p, ":");
                node_list body = {0};
                while (!is_kw(peek(p), K_case) && !is_kw(peek(p), S_default) &&
                       !is_punct(peek(p), "}") && !p->failed && peek(p)->type != T_EOF)
                    list_push(&body, parse_statement(p));
                c->list = body.items;
                c->count = body.count;
                list_push(&cases, c);
            }
            expect(p, "}");
            n->list = cases.items;
            n->count = cases.count;
            return n;
        }
        if (s == K_import && !is_punct(peek_at(p, 1), "(") && !is_punct(peek_at(p, 1), ".")) {
            if (!p->module) {
                fail(p, "Cannot use import statement outside a module", t);
                return NULL;
            }
            next(p);
            js_node* n = node(p, N_IMPORT);
            node_list specs = {0};
            if (peek(p)->type != T_STR) {
                if (peek(p)->type == T_IDENT && !is_punct(peek(p), "{")) {
                    js_node* spec = node(p, N_PROP);
                    spec->str = S_default;
                    spec->b = node(p, N_IDENT);
                    spec->b->str = next(p)->str;
                    list_push(&specs, spec);
                    accept(p, ",");
                }
                if (accept(p, "*")) {
                    next(p);   /* as */
                    js_node* spec = node(p, N_PROP);
                    spec->str = js_intern_c("*");
                    spec->b = node(p, N_IDENT);
                    spec->b->str = next(p)->str;
                    list_push(&specs, spec);
                } else if (accept(p, "{")) {
                    while (!is_punct(peek(p), "}") && !p->failed) {
                        js_token* imported = next(p);
                        js_node* spec = node(p, N_PROP);
                        spec->str = imported->type == T_STR ? js_intern(imported->str->data, imported->str->length)
                                                            : imported->str;
                        spec->b = node(p, N_IDENT);
                        spec->b->str = spec->str;
                        if (is_kw(peek(p), js_intern_c("as"))) {
                            next(p);
                            spec->b->str = next(p)->str;
                        }
                        list_push(&specs, spec);
                        if (!accept(p, ",")) break;
                    }
                    expect(p, "}");
                }
                if (!accept_kw(p, js_intern_c("from"))) fail(p, "Unexpected token", peek(p));
            }
            js_token* source = next(p);
            if (source->type != T_STR) fail(p, "Unexpected token", source);
            n->c = node(p, N_STR);
            n->c->str = source->str;
            if (is_kw(peek(p), K_with) || is_kw(peek(p), js_intern_c("assert"))) {
                next(p);
                if (accept(p, "{")) while (!accept(p, "}") && peek(p)->type != T_EOF) next(p);
            }
            consume_semicolon(p);
            n->list = specs.items;
            n->count = specs.count;
            return n;
        }
        if (s == K_debugger) {
            next(p);
            consume_semicolon(p);
            return node(p, N_EMPTY);
        }
        if (s == K_export) {
            next(p);
            js_node* n = node(p, N_EXPORT);
            if (accept_kw(p, S_default)) {
                n->op = 1;
                js_token* d = peek(p);
                if ((is_kw(d, K_function) || (is_kw(d, K_async) && is_kw(peek_at(p, 1), K_function))) &&
                    (peek_at(p, is_kw(d, K_async) ? 2 : 1)->type == T_IDENT ||
                     (is_punct(peek_at(p, is_kw(d, K_async) ? 2 : 1), "*") &&
                      peek_at(p, is_kw(d, K_async) ? 3 : 2)->type == T_IDENT))) {
                    int before = p->fn->function_count;
                    parse_statement(p);
                    if (p->fn->function_count > before) {
                        js_node* ident = node(p, N_IDENT);
                        ident->str = p->fn->functions[p->fn->function_count - 1]->str;
                        n->a = ident;
                    }
                } else if (is_kw(d, K_class) && peek_at(p, 1)->type == T_IDENT &&
                           !is_kw(peek_at(p, 1), K_extends)) {
                    n->b = parse_class(p, 1);
                    js_node* ident = node(p, N_IDENT);
                    ident->str = n->b->str;
                    n->a = ident;
                } else {
                    n->a = parse_assignment(p);
                    consume_semicolon(p);
                }
                return n;
            }
            if (accept(p, "*")) {
                n->op = 4;
                if (is_kw(peek(p), js_intern_c("as"))) {
                    next(p);
                    n->str = next(p)->str;
                }
                if (!accept_kw(p, js_intern_c("from"))) fail(p, "Unexpected token", peek(p));
                n->c = node(p, N_STR);
                n->c->str = next(p)->str;
                consume_semicolon(p);
                return n;
            }
            if (accept(p, "{")) {
                n->op = 3;
                node_list specs = {0};
                while (!is_punct(peek(p), "}") && !p->failed) {
                    js_token* local = next(p);
                    js_node* spec = node(p, N_PROP);
                    spec->str = local->type == T_STR ? js_intern(local->str->data, local->str->length) : local->str;
                    spec->b = node(p, N_IDENT);
                    spec->b->str = spec->str;
                    if (is_kw(peek(p), js_intern_c("as"))) {
                        next(p);
                        js_token* exported = next(p);
                        spec->b->str = exported->type == T_STR
                                           ? js_intern(exported->str->data, exported->str->length)
                                           : exported->str;
                    }
                    list_push(&specs, spec);
                    if (!accept(p, ",")) break;
                }
                expect(p, "}");
                if (accept_kw(p, js_intern_c("from"))) {
                    n->c = node(p, N_STR);
                    n->c->str = next(p)->str;
                }
                consume_semicolon(p);
                n->list = specs.items;
                n->count = specs.count;
                return n;
            }
            /* export <declaration> */
            n->op = 2;
            int before = p->fn->function_count;
            n->a = parse_statement(p);
            node_list names = {0};
            for (int i = before; i < p->fn->function_count; i++) {
                js_node* ident = node(p, N_IDENT);
                ident->str = p->fn->functions[i]->str;
                list_push(&names, ident);
            }
            if (n->a && n->a->type == N_CLASS_DECL) {
                js_node* ident = node(p, N_IDENT);
                ident->str = n->a->str;
                list_push(&names, ident);
            }
            n->list = names.items;
            n->count = names.count;
            return n;
        }
        if (s == K_with) {
            fail(p, "'with' statements are not supported", t);
            return NULL;
        }
        if (is_punct(peek_at(p, 1), ":") && s != K_this) {
            next(p);
            next(p);
            js_node* n = node(p, N_LABEL);
            n->str = s;
            n->a = parse_statement(p);
            return n;
        }
    }
    js_node* n = node(p, N_EXPR_STMT);
    n->line = t->line + p->line_offset;
    n->a = parse_expression(p);
    consume_semicolon(p);
    return n;
}

static js_node* js_parse_impl(const char* source, size_t length, const char* name,
                              char* error, size_t error_capacity, int* error_line);

/* The lexer keeps its tokens (and their strings) in malloc'd memory the
 * collector does not see, so no collection runs while parsing. */
js_node* js_parse(const char* source, size_t length, const char* name,
                  char* error, size_t error_capacity, int* error_line) {
    js_gc_inhibit(1);
    js_node* program = js_parse_impl(source, length, name, error, error_capacity, error_line);
    js_gc_inhibit(-1);
    return program;
}

static js_node* js_parse_impl(const char* source, size_t length, const char* name,
                  char* error, size_t error_capacity, int* error_line) {
    lexer_t lx;
    memset(&lx, 0, sizeof(lx));
    lx.src = source;
    lx.length = length;
    lx.line = 1;
    if (!lex(&lx)) {
        mx_copy(error, error_capacity, lx.error);
        *error_line = lx.error_line;
        free(lx.tokens);
        return NULL;
    }
    parser_t p;
    memset(&p, 0, sizeof(p));
    p.tokens = lx.tokens;
    p.count = lx.count;
    p.name = name;
    p.source = source;
    p.module = parse_as_module;
    p.module_url = parse_module_url;
    fn_ctx top;
    memset(&top, 0, sizeof(top));
    top.strict = parse_as_module;
    p.fn = &top;
    js_node* program = node(&p, N_PROGRAM);
    node_list items = {0};
    while (peek(&p)->type != T_EOF && !p.failed) {
        js_node* s = parse_statement(&p);
        if (s) list_push(&items, s);
    }
    free(lx.tokens);
    if (p.failed) {
        mx_copy(error, error_capacity, p.error);
        *error_line = p.error_line;
        /* show a little of the source around the error */
        if (p.error_offset < length && !p.line_offset) {
            size_t start = p.error_offset > 24 ? p.error_offset - 24 : 0;
            size_t end = p.error_offset + 24 < length ? p.error_offset + 24 : length;
            char snippet[64];
            size_t used = 0;
            for (size_t i = start; i < end && used + 1 < sizeof(snippet); i++)
                snippet[used++] = source[i] == '\n' || source[i] == '\t' ? ' ' : source[i];
            snippet[used] = '\0';
            mx_append(error, error_capacity, "  near: ");
            mx_append(error, error_capacity, snippet);
        }
        return NULL;
    }
    program->list = items.items;
    program->count = items.count;
    js_func* f = (js_func*)js_alloc(sizeof(js_func));
    memset(f, 0, sizeof(*f));
    f->vars = top.vars;
    f->var_count = top.var_count;
    f->functions = top.functions;
    f->function_count = top.function_count;
    f->has_closures = 1;
    f->source_name = name;
    program->func = f;
    return program;
}

/* ====================================================================== */
/* Scopes                                                                 */
/* ====================================================================== */

static int scope_allowed;   /* current function may use the scope stack */

/* The heap has no garbage collector, so every call used to leave its
 * scope behind: Google Search made close to a million of them (over
 * 200 MiB) and the browser ran out of memory. A scope whose function
 * (or block) has finished and that no closure captured cannot be reached
 * any more, so it goes on a free list by size and the next scope reuses
 * it. Closures are the only thing that keeps a scope: js_make_function()
 * marks the whole chain as captured. Generators and await run eagerly,
 * so nothing else outlives the call. */
#define ENV_POOL_CLASSES 7          /* capacities 4, 8, ..., 256 vars */
static js_env* env_pool[ENV_POOL_CLASSES + 1];

static int env_pool_class(uint32_t capacity) {
    uint32_t c = 4;
    for (int k = 1; k <= ENV_POOL_CLASSES; k++, c <<= 1)
        if (capacity <= c) return k;
    return 0;
}

static void env_release(js_env* e) {
    if (!e || e->on_stack || e->captured || !e->pool_class || e == js_global_env) return;
    int k = e->pool_class;
    e->parent = env_pool[k];
    env_pool[k] = e;
}

static void env_pool_reset(void) {
    memset(env_pool, 0, sizeof(env_pool));
}

static js_env* new_env(js_env* parent, uint32_t capacity, int use_stack) {
    js_env* e = NULL;
    if (use_stack) e = (js_env*)scope_alloc(sizeof(js_env) + sizeof(js_var) * capacity);
    if (e) {
        e->on_stack = 1;
    } else {
        int k = env_pool_class(capacity);
        if (k && env_pool[k]) {
            e = env_pool[k];
            env_pool[k] = e->parent;
        } else {
            if (k) capacity = 4u << (k - 1);
            e = (js_env*)js_alloc(sizeof(js_env) + sizeof(js_var) * capacity);
        }
        memset(e, 0, sizeof(js_env));
        e->pool_class = (uint8_t)k;
        if (k) capacity = 4u << (k - 1);
    }
    e->parent = parent;
    e->vars = (js_var*)(e + 1);
    e->capacity = capacity;
    e->count = 0;
    if (parent) {
        e->this_val = parent->this_val;
        e->function = parent->function;
        e->new_target = parent->new_target;
    }
    return e;
}

static js_var* env_declare(js_env* e, js_str* name, uint8_t kind) {
    for (uint32_t i = 0; i < e->count; i++)
        if (e->vars[i].name == name) return &e->vars[i];
    if (e->count == e->capacity) {
        uint32_t capacity = e->capacity ? e->capacity * 2 : 8;
        js_var* vars = (js_var*)js_alloc(sizeof(js_var) * capacity);
        if (e->count) memcpy(vars, e->vars, sizeof(js_var) * e->count);
        e->vars = vars;
        e->capacity = capacity;
    }
    js_var* v = &e->vars[e->count++];
    v->name = name;
    v->value = js_undef();
    v->kind = kind;
    v->initialized = kind == VAR_VAR;
    return v;
}

static js_var* env_lookup(js_env* e, js_str* name) {
    for (; e; e = e->parent)
        for (uint32_t i = 0; i < e->count; i++)
            if (e->vars[i].name == name) return &e->vars[i];
    return NULL;
}

__attribute__((noinline, cold)) static js_val throw_reference(js_str* name, int tdz) {
    char message[160];
    if (tdz) {
        mx_copy(message, sizeof(message), "Cannot access '");
        mx_append(message, sizeof(message), name->data);
        mx_append(message, sizeof(message), "' before initialization");
    } else {
        mx_copy(message, sizeof(message), name->data);
        mx_append(message, sizeof(message), " is not defined");
    }
    return js_throw_error("ReferenceError", message);
}

static js_val lookup_identifier(js_env* env, js_str* name, int typeof_check) {
    js_var* v = env_lookup(env, name);
    if (v) {
        if (!v->initialized) return throw_reference(name, 1);
        if (v->value.type == JS_STR) v->value.u.str->owned = 0;   /* now shared */
        return v->value;
    }
    if (name == K_arguments) {
        for (js_env* e = env; e; e = e->parent)
            if (e->arguments) return js_obj_val(e->arguments);
    }
    if (name == K_undefined) return js_undef();
    js_obj* g = js_global;
    js_prop* p = js_find_own(g, name);
    if (p) {
        if (p->flags & PROP_ACCESSOR)
            return js_is_callable(p->value) ? js_call(p->value, js_obj_val(g), NULL, 0) : js_undef();
        return p->value;
    }
    if (g->proto) {
        int found = js_has_property(js_obj_val(g), name);
        if (found) return js_get(js_obj_val(g), name);
    }
    /* named access: elements with an id are window properties */
    if (js_doc && js_doc->root) {
        mx_node_t* element = mx_dom_by_id(js_doc->root, name->data);
        if (element) return js_wrap_node(element);
    }
    if (typeof_check) return js_undef();
    return throw_reference(name, 0);
}

static void assign_identifier(js_env* env, js_str* name, js_val value) {
    js_var* v = env_lookup(env, name);
    if (v) {
        if (v->kind == VAR_CONST && v->initialized) {
            js_throw_error("TypeError", "Assignment to constant variable.");
            return;
        }
        v->value = value;
        return;
    }
    js_set(js_obj_val(js_global), name, value);
}

/* ====================================================================== */
/* Interpreter                                                            */
/* ====================================================================== */

static js_val eval(js_node* n, js_env* env);
static void exec(js_node* n, js_env* env);
static void bind_pattern(js_node* pattern, js_val value, js_env* env, int kind,
                         int declare);

static int stack_exhausted(void) {
    char probe;
    if (!js_rt.stack_base) return 0;
    long used = js_rt.stack_base - &probe;
    if (used < 0) used = -used;
    return used > (js_rt.stack_limit ? js_rt.stack_limit : 40 * 1024);
}

static int step(void) {
    if (++js_rt.steps > js_rt.step_limit) {
        if (!js_rt.timed_out) {
            js_rt.timed_out = 1;
            js_throw_error("RangeError", "Script took too long and was stopped");
        }
        return 0;
    }
    if (js_rt.out_of_memory) {
        js_throw_error("RangeError", "Out of memory");
        return 0;
    }
    return 1;
}

js_val js_make_function(js_func* func, js_env* env, js_val this_val) {
    js_obj* f = js_new_object(js_function_proto);
    f->cls = CLS_FUNCTION;
    f->func = func;
    f->env = env;
    for (js_env* e = env; e && !e->captured; e = e->parent) e->captured = 1;
    f->name = func->name ? func->name : js_intern_c("");
    f->home_this = this_val;
    return js_obj_val(f);
}

static js_val make_function_node(js_func* func, js_env* env) {
    /* named function expressions can refer to themselves */
    js_val fn = js_make_function(func, env, env->this_val);
    return fn;
}

static void hoist(js_func* f, js_env* env) {
    for (int i = 0; i < f->var_count; i++) env_declare(env, f->vars[i], VAR_VAR);
    for (int i = 0; i < f->function_count; i++) {
        js_node* decl = f->functions[i];
        js_var* v = env_declare(env, decl->str, VAR_VAR);
        v->value = make_function_node(decl->func, env);
    }
}

static js_val call_function(js_obj* fo, js_val self, js_val* args, int argc,
                            js_obj* new_target);

js_val js_call(js_val fn, js_val self, js_val* args, int argc) {
    if (!js_is_callable(fn)) {
        return js_throw_error("TypeError", "value is not a function");
    }
    return call_function(fn.u.obj, self, args, argc, NULL);
}

static js_val await_value(js_val v) {
    if (v.type != JS_OBJ || v.u.obj->cls != CLS_PROMISE) return v;
    js_val result;
    int state = js_promise_state(v, &result);
    for (int i = 0; i < 64 && state == 0; i++) {
        js_run_microtasks();
        state = js_promise_state(v, &result);
    }
    if (state == 2) return js_throw(result);
    return state == 1 ? result : js_undef();
}

static void init_class_fields(js_obj* ctor, js_val self);

/* Generators run to completion when called; their values are collected and
 * replayed by the iterator. */
typedef struct gen_collector {
    js_obj* values;
    struct gen_collector* prev;
} gen_collector;
static gen_collector* current_gen;
static js_str* current_label;
#define GEN_MAX_YIELDS 20000

static js_obj* generator_proto;

static js_val gen_next(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)args; (void)argc;
    if (self.type != JS_OBJ || !self.u.obj->data) return js_throw_error("TypeError", "next method called on incompatible receiver");
    js_obj* it = self.u.obj;
    js_obj* values = (js_obj*)it->data;
    js_obj* result = js_new_object(js_object_proto);
    js_val rv = js_obj_val(result);
    uint32_t index = (uint32_t)it->number;
    if (callee->magic == 1) it->number = values->length;   /* return() */
    if (index < values->length && callee->magic == 0) {
        js_set(rv, js_intern_c("value"), values->items[index]);
        js_set(rv, js_intern_c("done"), js_bool(0));
        it->number = index + 1;
    } else {
        js_set(rv, js_intern_c("value"), callee->magic == 1 ? (argc ? args[0] : js_undef()) : it->primitive);
        js_set(rv, js_intern_c("done"), js_bool(1));
        it->number = values->length;
    }
    if (it->magic) return js_promise_resolved(rv, 0);   /* async generator */
    return rv;
}

static js_val gen_iterator(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)args; (void)argc; (void)callee;
    if (self.type != JS_OBJ || !self.u.obj->data || !self.u.obj->magic) return self;
    /* for-of over an async generator: a synchronous view */
    js_obj* view = js_new_object(generator_proto);
    view->data = self.u.obj->data;
    view->number = self.u.obj->number;
    view->primitive = self.u.obj->primitive;
    return js_obj_val(view);
}

static js_val make_generator(js_obj* values, js_val return_value, int is_async) {
    if (!generator_proto) {
        generator_proto = js_new_object(js_object_proto);
        js_obj* next = js_new_native("next", gen_next, 1);
        next->magic = 0;
        js_define(generator_proto, "next", js_obj_val(next), PROP_HIDDEN);
        js_obj* ret = js_new_native("return", gen_next, 1);
        ret->magic = 1;
        js_define(generator_proto, "return", js_obj_val(ret), PROP_HIDDEN);
        js_obj* thr = js_new_native("throw", gen_next, 1);
        thr->magic = 1;
        js_define(generator_proto, "throw", js_obj_val(thr), PROP_HIDDEN);
        js_define_native(generator_proto, "@@iterator", gen_iterator, 0);
        js_define_native(generator_proto, "@@asyncIterator", gen_iterator, 0);
    }
    js_obj* it = js_new_object(generator_proto);
    it->data = values;
    it->number = 0;
    it->primitive = return_value;
    it->magic = (int16_t)is_async;
    return js_obj_val(it);
}

/* ---- coroutines: lazy generators and suspending async functions ----
 *
 * A generator or async function body runs on its own small stack, so it can
 * stop at a `yield` or at an `await` of a pending promise and continue later
 * exactly where it was. Without this (other CPUs, or when no stack can be
 * allocated) generators fall back to running eagerly (gen_collector above)
 * and `await` to draining microtasks. */
#if defined(__x86_64__)
#define JS_HAVE_CORO 1
__attribute__((visibility("hidden"))) void js_coro_switch(void** save_sp, void* to_sp);
__asm__(
    ".text\n"
    ".p2align 4\n"
    "js_coro_switch:\n"
    "    pushq %rbp\n"
    "    pushq %rbx\n"
    "    pushq %r12\n"
    "    pushq %r13\n"
    "    pushq %r14\n"
    "    pushq %r15\n"
    "    movq %rsp, (%rdi)\n"
    "    movq %rsi, %rsp\n"
    "    popq %r15\n"
    "    popq %r14\n"
    "    popq %r13\n"
    "    popq %r12\n"
    "    popq %rbx\n"
    "    popq %rbp\n"
    "    ret\n");
/* Calls fn(arg) with the stack pointer at `top`. */
__attribute__((visibility("hidden"))) void js_call_on_stack(void* arg, void (*fn)(void*), void* top);
__asm__(
    ".text\n"
    ".p2align 4\n"
    "js_call_on_stack:\n"
    "    pushq %rbp\n"
    "    movq %rsp, %rbp\n"
    "    movq %rdx, %rsp\n"
    "    call *%rsi\n"
    "    movq %rbp, %rsp\n"
    "    popq %rbp\n"
    "    ret\n");
#else
#define JS_HAVE_CORO 0
#endif

/* Only the generator or async function's own body runs on its stack: every
 * function it calls runs on the resumer's (big) stack, just below where the
 * resumer is waiting, so a small stack is enough however deep that code
 * goes. */
#define CORO_STACK_SIZE (64u * 1024u)
#define CORO_STACK_MARGIN (16u * 1024u)
#define CORO_MAX_LIVE 1024
#define CORO_POOL 16

enum { SUSPEND_NONE, SUSPEND_YIELD, SUSPEND_AWAIT };

typedef struct agen_request {
    int mode;                   /* 0 next, 1 throw, 2 return */
    js_val value;
    js_obj* promise;
    struct agen_request* next;
} agen_request;

typedef struct js_coro {
    char* stack;
    void* sp;                   /* coroutine's stack pointer while suspended */
    void* caller_sp;            /* resumer's stack pointer while running */
    js_obj* fo;
    js_env* env;
    js_obj* new_target;
    uint8_t is_async, is_generator, started, running, done, suspend;
    int resume_mode;            /* how it was resumed: 0 value, 1 throw, 2 return */
    js_val transfer;            /* value passed in (resume) or out (yield) */
    js_val result;              /* return value once done */
    int depth_delta;
    const char* source;
    char* big_base;             /* the resumer's stack, for calls made from the body */
    char* body_sp;              /* where the body's stack ends while it calls out */
    long big_limit;
    js_obj* promise;            /* async function: its result */
    agen_request* queue;        /* async generator: pending next() calls */
    struct js_coro* next_live;
} js_coro;

static js_coro* current_coro;
static js_coro* live_coros;     /* malloc'd; freed when the page goes */
static int live_coro_count;
static char* stack_pool[CORO_POOL];
static int stack_pool_count;
static js_obj* coro_gen_proto;
static js_obj* coro_agen_proto;

static js_val run_body(js_obj* fo, js_env* env, js_obj* new_target, js_val self);

static void coro_release_stack(js_coro* c) {
    if (!c->stack) return;
    if (stack_pool_count < CORO_POOL) stack_pool[stack_pool_count++] = c->stack;
    else free(c->stack);
    c->stack = NULL;
}

/* Frees every coroutine of the previous page (their stacks point into the
 * script heap, which is gone). */
static void coro_reset(void) {
    for (js_coro* c = live_coros; c;) {
        js_coro* next = c->next_live;
        coro_release_stack(c);
        free(c);
        c = next;
    }
    live_coros = NULL;
    live_coro_count = 0;
    current_coro = NULL;
    coro_gen_proto = coro_agen_proto = NULL;
}

static void coro_entry(void) {
    js_coro* c = current_coro;
    c->result = run_body(c->fo, c->env, c->new_target, c->env->this_val);
    c->done = 1;
    c->suspend = SUSPEND_NONE;
#if JS_HAVE_CORO
    js_coro_switch(&c->sp, c->caller_sp);
#endif
    for (;;) {}
}

static js_coro* coro_create(js_obj* fo, js_env* env, js_obj* new_target) {
#if JS_HAVE_CORO
    if (live_coro_count >= CORO_MAX_LIVE) {
        /* drop finished ones from the list first */
        js_coro** link = &live_coros;
        while (*link) {
            js_coro* c = *link;
            if (c->done && !c->running) {
                *link = c->next_live;
                coro_release_stack(c);
                free(c);
                live_coro_count--;
            } else {
                link = &c->next_live;
            }
        }
        if (live_coro_count >= CORO_MAX_LIVE) return NULL;
    }
    char* stack = stack_pool_count ? stack_pool[--stack_pool_count] : (char*)malloc(CORO_STACK_SIZE);
    if (!stack) return NULL;
    js_coro* c = (js_coro*)calloc(1, sizeof(js_coro));
    if (!c) {
        free(stack);
        return NULL;
    }
    c->stack = stack;
    c->fo = fo;
    c->env = env;
    c->new_target = new_target;
    c->transfer = c->result = js_undef();
    /* first switch: pop six registers, then `ret` into coro_entry with the
     * stack aligned as for a call */
    uintptr_t top = ((uintptr_t)(stack + CORO_STACK_SIZE)) & ~(uintptr_t)15u;
    void** sp = (void**)top;
    *--sp = NULL;                       /* coro_entry's return address */
    *--sp = (void*)coro_entry;
    for (int i = 0; i < 6; i++) *--sp = NULL;
    c->sp = sp;
    c->next_live = live_coros;
    live_coros = c;
    live_coro_count++;
    env->captured = 1;                  /* never pooled: the body keeps using it */
    return c;
#else
    (void)fo; (void)env; (void)new_target;
    return NULL;
#endif
}

/* Runs `c` until it yields, awaits or finishes. */
static void coro_run(js_coro* c) {
#if JS_HAVE_CORO
    char* saved_base = js_rt.stack_base;
    long saved_limit = js_rt.stack_limit;
    int depth = js_rt.depth;
    const char* saved_source = js_rt.source;
    int saved_line = js_rt.line;
    int saved_allowed = scope_allowed;
    js_str* saved_label = current_label;
    gen_collector* saved_gen = current_gen;
    js_coro* prev = current_coro;
    current_coro = c;
    current_gen = NULL;
    current_label = NULL;
    c->running = 1;
    c->started = 1;
    c->suspend = SUSPEND_NONE;
    c->big_base = saved_base;
    c->big_limit = saved_limit;
    js_rt.stack_base = (char*)(((uintptr_t)(c->stack + CORO_STACK_SIZE)) & ~(uintptr_t)15u);
    js_rt.stack_limit = (long)(CORO_STACK_SIZE - CORO_STACK_MARGIN);
    js_rt.depth = depth + c->depth_delta;
    if (c->source) js_rt.source = c->source;
    scope_allowed = 0;
    js_coro_switch(&c->caller_sp, c->sp);
    c->running = 0;
    c->depth_delta = js_rt.depth - depth;
    c->source = js_rt.source;
    js_rt.stack_base = saved_base;
    js_rt.stack_limit = saved_limit;
    js_rt.depth = depth;
    js_rt.source = saved_source;
    js_rt.line = saved_line;
    scope_allowed = saved_allowed;
    current_label = saved_label;
    current_gen = saved_gen;
    current_coro = prev;
    if (c->done) coro_release_stack(c);
#else
    (void)c;
#endif
}

static void coro_suspend(js_coro* c, int kind) {
#if JS_HAVE_CORO
    c->suspend = (uint8_t)kind;
    js_coro_switch(&c->sp, c->caller_sp);
#else
    (void)c; (void)kind;
#endif
}

static js_val iter_result(js_val value, int done) {
    js_obj* r = js_new_object(js_object_proto);
    js_set(js_obj_val(r), js_intern_c("value"), value);
    js_set(js_obj_val(r), js_intern_c("done"), js_bool(done));
    return js_obj_val(r);
}

/* async functions: settle the result promise once the body is done */
static void async_finish(js_coro* c) {
    if (!c->done || !c->promise) return;
    js_obj* p = c->promise;
    c->promise = NULL;
    if (js_rt.ctl == CTL_THROW) {
        js_val error = js_rt.value;
        js_rt.ctl = CTL_NONE;
        js_promise_settle(p, error, 1);
    } else {
        js_rt.ctl = CTL_NONE;
        js_promise_settle(p, c->result, 0);
    }
}

/* async generators: answer the oldest next() after a yield or the end */
static void agen_after(js_coro* c) {
    if (!c->done && c->suspend == SUSPEND_AWAIT) return;
    agen_request* req = c->queue;
    if (!req) return;
    c->queue = req->next;
    if (c->done) {
        if (js_rt.ctl == CTL_THROW) {
            js_val error = js_rt.value;
            js_rt.ctl = CTL_NONE;
            js_promise_settle(req->promise, error, 1);
        } else {
            js_rt.ctl = CTL_NONE;
            js_promise_settle(req->promise, iter_result(c->result, 1), 0);
            c->result = js_undef();
        }
    } else {
        js_promise_settle(req->promise, iter_result(c->transfer, 0), 0);
    }
}

static void agen_pump(js_coro* c) {
    while (c->queue && !c->running) {
        if (!c->done && c->suspend == SUSPEND_AWAIT) return;
        agen_request* req = c->queue;
        if (!c->done && !c->started && req->mode) {
            c->done = 1;
            coro_release_stack(c);
        }
        if (c->done) {
            c->queue = req->next;
            if (req->mode == 1) js_promise_settle(req->promise, req->value, 1);
            else js_promise_settle(req->promise, iter_result(req->mode == 2 ? req->value : js_undef(), 1), 0);
            continue;
        }
        c->resume_mode = req->mode;
        c->transfer = req->value;
        coro_run(c);
        agen_after(c);
    }
}

static js_val coro_await_cb(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)self;
    js_coro* c = (js_coro*)callee->data;
    if (!c || c->done || c->running || c->suspend != SUSPEND_AWAIT) return js_undef();
    c->resume_mode = callee->magic;
    c->transfer = argc > 0 ? args[0] : js_undef();
    coro_run(c);
    if (c->is_generator) {
        agen_after(c);
        agen_pump(c);
    } else {
        async_finish(c);
    }
    return js_undef();
}

/* `await v` */
static js_val coro_await(js_val v) {
    js_coro* c = current_coro;
    if (!c || !c->is_async || current_gen) return await_value(v);
    js_val p = v;
    if (v.type != JS_OBJ || v.u.obj->cls != CLS_PROMISE) {
        p = js_promise_resolved(v, 0);   /* adopts thenables */
        if (js_rt.ctl) return js_undef();
    }
    js_obj* ok = js_new_native("", coro_await_cb, 1);
    ok->data = c;
    ok->magic = 0;
    js_obj* err = js_new_native("", coro_await_cb, 1);
    err->data = c;
    err->magic = 1;
    js_promise_then(p, js_obj_val(ok), js_obj_val(err));
    coro_suspend(c, SUSPEND_AWAIT);
    if (c->resume_mode == 1) return js_throw(c->transfer);
    return c->transfer;
}

/* `yield v` inside a coroutine generator */
static js_val coro_yield(js_coro* c, js_val v) {
    if (c->is_async) {
        v = coro_await(v);
        if (js_rt.ctl) return js_undef();
    }
    c->transfer = v;
    coro_suspend(c, SUSPEND_YIELD);
    if (c->resume_mode == 1) return js_throw(c->transfer);
    if (c->resume_mode == 2) {
        js_val r = c->transfer;
        if (c->is_async) {
            r = coro_await(r);
            if (js_rt.ctl) return js_undef();
        }
        js_rt.ctl = CTL_RETURN;   /* runs finally blocks on the way out */
        js_rt.value = r;
        return js_undef();
    }
    return c->transfer;
}

/* `yield* iterable` inside a coroutine generator */
static js_val coro_yield_star(js_coro* c, js_val iterable) {
    js_val method = js_undef();
    if (c->is_async) method = js_get_c(iterable, "@@asyncIterator");
    if (!js_is_callable(method)) method = js_get_c(iterable, "@@iterator");
    if (js_rt.ctl) return js_undef();
    if (!js_is_callable(method)) return js_throw_error("TypeError", "yield* operand is not iterable");
    js_val it = js_call(method, iterable, NULL, 0);
    if (js_rt.ctl) return js_undef();
    js_val next = js_get_c(it, "next");
    if (js_rt.ctl) return js_undef();
    js_val sent = js_undef();
    int mode = 0;
    for (;;) {
        js_val fn = next;
        if (mode == 1) fn = js_get_c(it, "throw");
        else if (mode == 2) fn = js_get_c(it, "return");
        if (js_rt.ctl) return js_undef();
        if (!js_is_callable(fn)) {
            if (mode == 1) return js_throw(sent);
            if (mode == 2) {
                js_rt.ctl = CTL_RETURN;
                js_rt.value = sent;
                return js_undef();
            }
            return js_throw_error("TypeError", "iterator.next is not a function");
        }
        js_val r = js_call(fn, it, &sent, 1);
        if (js_rt.ctl) return js_undef();
        if (c->is_async) {
            r = coro_await(r);
            if (js_rt.ctl) return js_undef();
        }
        if (r.type != JS_OBJ) return js_throw_error("TypeError", "Iterator result is not an object");
        int done = js_truthy(js_get_c(r, "done"));
        js_val value = js_get_c(r, "value");
        if (js_rt.ctl) return js_undef();
        if (done) {
            if (mode == 2) {
                js_rt.ctl = CTL_RETURN;
                js_rt.value = value;
                return js_undef();
            }
            return value;
        }
        c->transfer = value;
        coro_suspend(c, SUSPEND_YIELD);
        mode = c->resume_mode;
        sent = c->transfer;
    }
}

static js_coro* this_coro(js_val self) {
    if (self.type == JS_OBJ && self.u.obj->magic == 0x7c0 && self.u.obj->data) return (js_coro*)self.u.obj->data;
    js_throw_error("TypeError", "next method called on incompatible receiver");
    return NULL;
}

static js_val coro_gen_next(js_val self, js_val* args, int argc, js_obj* callee) {
    js_coro* c = this_coro(self);
    if (!c) return js_undef();
    int mode = callee->magic;
    js_val arg = argc > 0 ? args[0] : js_undef();
    if (c->running) return js_throw_error("TypeError", "Generator is already running");
    if (!c->done && !c->started && mode) {
        c->done = 1;
        coro_release_stack(c);
    }
    if (c->done) {
        if (mode == 1) return js_throw(arg);
        return iter_result(mode == 2 ? arg : js_undef(), 1);
    }
    c->resume_mode = mode;
    c->transfer = arg;
    coro_run(c);
    if (js_rt.ctl) return js_undef();
    if (c->done) {
        js_val r = c->result;
        c->result = js_undef();
        return iter_result(r, 1);
    }
    return iter_result(c->transfer, 0);
}

static js_val coro_agen_next(js_val self, js_val* args, int argc, js_obj* callee) {
    js_coro* c = this_coro(self);
    if (!c) return js_undef();
    agen_request* req = (agen_request*)js_alloc(sizeof(agen_request));
    req->mode = callee->magic;
    req->value = argc > 0 ? args[0] : js_undef();
    req->promise = js_promise_new();
    req->next = NULL;
    agen_request** tail = &c->queue;
    while (*tail) tail = &(*tail)->next;
    *tail = req;
    js_obj* promise = req->promise;
    agen_pump(c);
    return js_obj_val(promise);
}

static js_val return_self(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)args; (void)argc; (void)callee;
    return self;
}

static js_val make_coro_generator(js_coro* c, js_obj* fo) {
    js_obj** protop = c->is_async ? &coro_agen_proto : &coro_gen_proto;
    if (!*protop) {
        js_obj* proto = js_new_object(js_object_proto);
        js_native fn = c->is_async ? coro_agen_next : coro_gen_next;
        static const char* const names[] = {"next", "throw", "return"};
        for (int i = 0; i < 3; i++) {
            js_obj* m = js_new_native(names[i], fn, 1);
            m->magic = (int16_t)i;
            js_define(proto, names[i], js_obj_val(m), PROP_HIDDEN);
        }
        js_define_native(proto, c->is_async ? "@@asyncIterator" : "@@iterator", return_self, 0);
        js_define(proto, "@@toStringTag", js_str_val(js_intern_c(c->is_async ? "AsyncGenerator" : "Generator")), PROP_HIDDEN);
        *protop = proto;
    }
    (void)fo;
    js_obj* it = js_new_object(*protop);
    it->magic = 0x7c0;
    it->data = c;
    return js_obj_val(it);
}

#if JS_HAVE_CORO
typedef struct {
    js_obj* fo;
    js_val self;
    js_val* args;
    int argc;
    js_obj* new_target;
    js_val result;
} big_call;

static void big_call_run(void* p) {
    big_call* b = (big_call*)p;
    b->result = call_function(b->fo, b->self, b->args, b->argc, b->new_target);
}

static int on_coro_stack(js_coro* c) {
    char probe;
    return c->stack && &probe >= c->stack && &probe < c->stack + CORO_STACK_SIZE;
}

static js_val call_on_big_stack(js_coro* c, js_obj* fo, js_val self, js_val* args, int argc,
                                js_obj* new_target) {
    big_call b = {fo, self, args, argc, new_target, js_undef()};
    char* coro_base = js_rt.stack_base;
    long coro_limit = js_rt.stack_limit;
    /* the resumer waits inside js_coro_switch at caller_sp; below it is free */
    char* top = (char*)(((uintptr_t)c->caller_sp - 512u) & ~(uintptr_t)15u);
    long used_above = c->big_base > top ? (long)(c->big_base - top) : 0;
    c->body_sp = (char*)&b;
    js_rt.stack_base = c->big_base;
    js_rt.stack_limit = c->big_limit;
    if (used_above >= c->big_limit) {
        js_rt.stack_base = coro_base;
        js_rt.stack_limit = coro_limit;
        return js_throw_error("RangeError", "Maximum call stack size exceeded");
    }
    js_call_on_stack(&b, big_call_run, top);
    js_rt.stack_base = coro_base;
    js_rt.stack_limit = coro_limit;
    return b.result;
}
#endif

static js_val call_function(js_obj* fo, js_val self, js_val* args, int argc,
                            js_obj* new_target) {
    if (js_rt.ctl) return js_undef();
#if JS_HAVE_CORO
    if (current_coro && current_coro->running && on_coro_stack(current_coro))
        return call_on_big_stack(current_coro, fo, self, args, argc, new_target);
#endif
    if (stack_exhausted() || js_rt.depth > 1000) {
        return js_throw_error("RangeError", "Maximum call stack size exceeded");
    }
    if (fo->cls == CLS_NATIVE) {
        js_rt.depth++;
        js_val r = fo->native(self, args, argc, fo);
        js_rt.depth--;
        return r;
    }
    if (fo->cls == CLS_BOUND) {
        int total = fo->bound_count + argc;
        js_val* all = (js_val*)js_alloc(sizeof(js_val) * (size_t)(total ? total : 1));
        for (int i = 0; i < fo->bound_count; i++) all[i] = fo->bound_args[i];
        for (int i = 0; i < argc; i++) all[fo->bound_count + i] = args[i];
        return call_function(fo->bound_target, new_target ? self : fo->bound_this, all, total,
                             new_target);
    }
    js_func* f = fo->func;
    if (f->is_class_constructor && !new_target)
        return js_throw_error("TypeError", "Class constructor cannot be invoked without 'new'");
    size_t saved_top = scope_top;
    int saved_allowed = scope_allowed;
    int use_stack = !f->has_closures && !f->uses_arguments && !f->generator && !f->is_async;
    scope_allowed = use_stack;
    uint32_t capacity = (uint32_t)(f->param_count + f->var_count + f->function_count + 2);
    js_env* env = new_env(fo->env, capacity, use_stack);
    env->is_function = 1;
    env->function = fo;
    if (f->arrow) {
        env->this_val = fo->home_this;
    } else if (!f->strict && !new_target) {
        /* sloppy mode: undefined this is the window, primitives are boxed */
        if (self.type == JS_UNDEF || self.type == JS_NULL) env->this_val = js_obj_val(js_global);
        else if (self.type != JS_OBJ) env->this_val = js_obj_val(js_to_object(self));
        else env->this_val = self;
    } else {
        env->this_val = self;
    }
    if (!f->arrow) env->new_target = new_target;
    if (f->arrow && fo->env) env->new_target = fo->env->new_target;
    const char* saved_source = js_rt.source;
    if (f->source_name) js_rt.source = f->source_name;
    js_rt.depth++;
    if (f->uses_arguments && !f->arrow) {
        js_obj* arguments = js_new_array((uint32_t)argc);
        arguments->cls = CLS_ARGUMENTS;
        for (int i = 0; i < argc; i++) js_array_push(arguments, args[i]);
        env->arguments = arguments;
    }
    /* named function expressions see their own name */
    if (f->name && !f->arrow) {
        js_var* self_ref = env_declare(env, f->name, VAR_VAR);
        self_ref->value = js_obj_val(fo);
    }
    for (int i = 0; i < f->param_count && !js_rt.ctl; i++) {
        js_node* param = f->params[i];
        if (param->type == N_REST) {
            js_obj* rest = js_new_array(argc > i ? (uint32_t)(argc - i) : 0);
            for (int k = i; k < argc; k++) js_array_push(rest, args[k]);
            bind_pattern(param->a, js_obj_val(rest), env, VAR_LET, 1);
            break;
        }
        bind_pattern(param, i < argc ? args[i] : js_undef(), env, VAR_LET, 1);
    }
    js_val result = js_undef();
    if ((f->generator || f->is_async) && !js_rt.ctl) {
        js_coro* c = coro_create(fo, env, new_target);
        if (c) {
            c->is_async = f->is_async;
            c->is_generator = f->generator;
            js_rt.depth--;
            js_rt.source = saved_source;
            scope_top = saved_top;
            scope_allowed = saved_allowed;
            if (f->generator) return make_coro_generator(c, fo);
            /* async function: runs now until its first await */
            js_obj* promise = js_promise_new();
            c->promise = promise;
            coro_run(c);
            async_finish(c);
            return js_obj_val(promise);
        }
    }
    gen_collector collector;
    js_coro* outer_coro = current_coro;
    if (f->generator) {
        collector.values = js_new_array(0);
        collector.prev = current_gen;
        current_gen = &collector;
    }
    if (f->generator || f->is_async) current_coro = NULL;   /* eager fallback */
    if (!js_rt.ctl) result = run_body(fo, env, new_target, self);
    current_coro = outer_coro;
    if (new_target && f->derived && !js_rt.ctl) {
        /* derived constructors return the object super() made */
        js_val this_now = env->this_val;
        if (result.type != JS_OBJ) result = this_now;
    }
    js_rt.depth--;
    js_rt.source = saved_source;
    scope_top = saved_top;
    scope_allowed = saved_allowed;
    env_release(env);
    if (f->generator) {
        current_gen = collector.prev;
        if (js_rt.ctl == CTL_THROW) return js_undef();
        return make_generator(collector.values, result, f->is_async);
    }
    if (f->is_async) {
        if (js_rt.ctl == CTL_THROW) {
            js_val error = js_rt.value;
            js_rt.ctl = CTL_NONE;
            return js_promise_resolved(error, 1);
        }
        return js_promise_resolved(result, 0);
    }
    return result;
}

/* Hoists and runs a function body in its prepared scope. */
static js_val run_body(js_obj* fo, js_env* env, js_obj* new_target, js_val self) {
    js_func* f = fo->func;
    js_val result = js_undef();
    hoist(f, env);
    if (new_target && f->is_class_constructor && !f->derived)
        init_class_fields(fo, self);
    if (js_rt.ctl) return result;
    if (f->expression_body) {
        result = eval(f->body, env);
    } else if (f->body) {
        js_node* body = f->body;
        for (uint32_t i = 0; i < body->count && !js_rt.ctl; i++) exec(body->list[i], env);
        if (js_rt.ctl == CTL_RETURN) {
            result = js_rt.value;
            js_rt.ctl = CTL_NONE;
        } else if (js_rt.ctl == CTL_BREAK || js_rt.ctl == CTL_CONTINUE) {
            js_rt.ctl = CTL_NONE;
        }
    }
    return result;
}

/* An iterator ({next, return, throw, [Symbol.iterator]}) over a snapshot
 * of values: what Array.prototype.values(), Map.prototype.entries() and
 * the like return. It is a finished generator's object, which already
 * implements the protocol. */
js_val js_list_iterator(js_obj* values) {
    return make_generator(values, js_undef(), 0);
}

js_obj* js_construct_this_override;
js_obj* js_super_new_target;

js_val js_construct(js_val fn, js_val* args, int argc) {
    if (!js_is_callable(fn)) return js_throw_error("TypeError", "value is not a constructor");
    js_obj* ctor = fn.u.obj;
    while (ctor->cls == CLS_BOUND) ctor = ctor->bound_target;
    if (ctor->cls == CLS_NATIVE && ctor->native == proxy_call) return proxy_construct(ctor, args, argc);
    if (ctor->cls == CLS_NATIVE) {
        js_rt.depth++;
        js_val r = ctor->native(js_undef(), args, argc, ctor);
        js_rt.depth--;
        /* natives that are not constructors still return an object */
        if (r.type != JS_OBJ && !js_rt.ctl) {
            js_obj* o = js_to_object(r);
            return js_obj_val(o);
        }
        return r;
    }
    if (ctor->func && ctor->func->arrow)
        return js_throw_error("TypeError", "value is not a constructor");
    js_val proto = js_get(js_obj_val(ctor), S_prototype);
    js_obj* obj;
    if (js_construct_this_override) {   /* upgrading an existing element */
        obj = js_construct_this_override;
        js_construct_this_override = NULL;
        if (proto.type == JS_OBJ) obj->proto = proto.u.obj;
    } else {
        obj = js_new_object(proto.type == JS_OBJ ? proto.u.obj : js_object_proto);
        /* objects of classes extending built-ins keep the built-in's behaviour */
        for (js_obj* p = obj->proto; p; p = p->proto) {
            if (p == js_error_proto) { obj->cls = CLS_ERROR; break; }
            if (p == js_array_proto) { obj->cls = CLS_ARRAY; break; }
        }
    }
    js_val self = js_obj_val(obj);
    js_val r = call_function(fn.u.obj, self, args, argc, ctor);
    if (js_rt.ctl) return js_undef();
    return r.type == JS_OBJ ? r : self;
}

int js_instanceof(js_val v, js_val ctor) {
    if (!js_is_callable(ctor)) {
        js_throw_error("TypeError", "Right-hand side of 'instanceof' is not callable");
        return 0;
    }
    if (v.type != JS_OBJ) return 0;
    js_obj* c = ctor.u.obj;
    while (c->cls == CLS_BOUND) c = c->bound_target;
    js_val proto = js_get(js_obj_val(c), S_prototype);
    if (proto.type != JS_OBJ) return 0;
    for (js_obj* o = v.u.obj->proto; o; o = o->proto)
        if (o == proto.u.obj) return 1;
    return 0;
}

static js_str* member_key(js_node* n, js_env* env) {
    if (!n->flag) return n->str;
    js_val k = eval(n->b, env);
    if (js_rt.ctl) return js_intern_c("");
    return key_of(k);
}

/* Binds a destructuring pattern. declare: create bindings (else assign). */
static void bind_pattern(js_node* pattern, js_val value, js_env* env, int kind,
                         int declare) {
    if (js_rt.ctl) return;
    switch (pattern->type) {
    case N_IDENT:
        if (declare) {
            js_var* v = env_declare(env, pattern->str, (uint8_t)kind);
            v->value = value;
            v->initialized = 1;
            if (kind == VAR_VAR) {
                /* var inside blocks lands in the function scope */
                js_var* outer = env_lookup(env, pattern->str);
                if (outer) outer->value = share_val(value);
            }
        } else {
            assign_identifier(env, pattern->str, value);
        }
        return;
    case N_MEMBER: {
        js_val target = eval(pattern->a, env);
        if (js_rt.ctl) return;
        if (pattern->flag) {
            js_val k = eval(pattern->b, env);
            if (js_rt.ctl) return;
            js_set_index(target, k, value);
        } else {
            js_set(target, pattern->str, value);
        }
        return;
    }
    case N_ASSIGN_PAT:
        if (value.type == JS_UNDEF) {
            value = eval(pattern->b, env);
            if (js_rt.ctl) return;
            if (pattern->a->type == N_IDENT && value.type == JS_OBJ &&
                js_is_callable(value) && value.u.obj->name && !value.u.obj->name->length)
                value.u.obj->name = pattern->a->str;
        }
        bind_pattern(pattern->a, value, env, kind, declare);
        return;
    case N_ARRAY_PAT: {
        if (value.type == JS_UNDEF || value.type == JS_NULL) {
            js_throw_error("TypeError", "value is not iterable");
            return;
        }
        js_obj* source = NULL;
        if (value.type == JS_OBJ && (value.u.obj->cls == CLS_ARRAY || value.u.obj->cls == CLS_ARGUMENTS)) {
            source = value.u.obj;
        } else {
            /* strings, sets, maps and other iterables */
            js_val from = js_get(js_get_c(js_obj_val(js_global), "Array"), js_intern_c("from"));
            js_val arr = js_call(from, js_undef(), &value, 1);
            if (js_rt.ctl) return;
            source = arr.type == JS_OBJ ? arr.u.obj : js_new_array(0);
        }
        for (uint32_t i = 0; i < pattern->count && !js_rt.ctl; i++) {
            js_node* item = pattern->list[i];
            if (item->type == N_HOLE) continue;
            if (item->type == N_REST) {
                js_obj* rest = js_new_array(0);
                for (uint32_t k = i; k < source->length; k++) js_array_push(rest, source->items[k]);
                bind_pattern(item->a, js_obj_val(rest), env, kind, declare);
                break;
            }
            bind_pattern(item, i < source->length ? source->items[i] : js_undef(), env, kind,
                         declare);
        }
        return;
    }
    case N_OBJECT_PAT: {
        if (value.type == JS_UNDEF || value.type == JS_NULL) {
            js_throw_error("TypeError", "Cannot destructure undefined or null");
            return;
        }
        js_str* used[64];
        int used_count = 0;
        for (uint32_t i = 0; i < pattern->count && !js_rt.ctl; i++) {
            js_node* prop = pattern->list[i];
            if (prop->op == PROP_SPREAD) {
                js_obj* rest = js_new_object(js_object_proto);
                if (value.type == JS_OBJ) {
                    js_obj* keys = js_own_keys(value.u.obj, 0);
                    for (uint32_t k = 0; k < keys->length; k++) {
                        js_str* key = keys->items[k].u.str;
                        int skip = 0;
                        for (int u = 0; u < used_count; u++) skip |= js_str_eq(used[u], key);
                        if (!skip) js_set(js_obj_val(rest), key, js_get(value, key));
                    }
                }
                bind_pattern(prop->b->type == N_REST ? prop->b->a : prop->b, js_obj_val(rest), env,
                             kind, declare);
                continue;
            }
            js_str* key = prop->flag ? key_of(eval(prop->a, env)) : prop->str;
            if (js_rt.ctl) return;
            if (used_count < 64) used[used_count++] = key;
            bind_pattern(prop->b, js_get(value, key), env, kind, declare);
        }
        return;
    }
    default:
        js_throw_error("SyntaxError", "Invalid destructuring target");
    }
}

static double js_pow(double base, double exponent);

static js_val arithmetic(uint32_t op, js_val a, js_val b) {
    if (op == PACK("+")) {
        if (a.type == JS_NUM && b.type == JS_NUM) return js_num(a.u.num + b.u.num);
        js_val pa = js_to_primitive(a, 2);
        JS_CHECK(pa);
        js_val pb = js_to_primitive(b, 2);
        JS_CHECK(pb);
        if (pa.type == JS_STR || pb.type == JS_STR)
            return js_str_val(js_concat(js_to_string(pa), js_to_string(pb)));
        return js_num(js_to_number(pa) + js_to_number(pb));
    }
    if (op == PACK("==")) return js_bool(js_loose_equals(a, b));
    if (op == PACK("!=")) return js_bool(!js_loose_equals(a, b));
    if (op == PACK("===")) return js_bool(js_strict_equals(a, b));
    if (op == PACK("!==")) return js_bool(!js_strict_equals(a, b));
    if (op == PACK("<") || op == PACK(">") || op == PACK("<=") || op == PACK(">=")) {
        js_val pa = js_to_primitive(a, 1), pb = js_to_primitive(b, 1);
        if (pa.type == JS_STR && pb.type == JS_STR) {
            int c = strcmp(pa.u.str->data, pb.u.str->data);
            if (op == PACK("<")) return js_bool(c < 0);
            if (op == PACK(">")) return js_bool(c > 0);
            if (op == PACK("<=")) return js_bool(c <= 0);
            return js_bool(c >= 0);
        }
        double x = js_to_number(pa), y = js_to_number(pb);
        if (op == PACK("<")) return js_bool(x < y);
        if (op == PACK(">")) return js_bool(x > y);
        if (op == PACK("<=")) return js_bool(x <= y);
        return js_bool(x >= y);
    }
    double x = js_to_number(a), y = js_to_number(b);
    JS_CHECK(x);
    if (op == PACK("-")) return js_num(x - y);
    if (op == PACK("*")) return js_num(x * y);
    if (op == PACK("/")) return js_num(x / y);
    if (op == PACK("%")) {
        if (y == 0 || x != x || y != y) return js_num(0.0 / 0.0);
        if (x == (long long)x && y == (long long)y && x >= -9e15 && x <= 9e15 && y >= -9e15 && y <= 9e15) {
            long long r = (long long)x % (long long)y;
            if (r == 0 && x < 0) return js_num(-0.0);
            return js_num((double)r);
        }
        double q = x / y;
        double t = q < 0 ? -(double)(long long)(-q) : (double)(long long)q;
        return js_num(x - t * y);
    }
    if (op == PACK("**")) return js_num(js_pow(x, y));
    int32_t ix = js_to_int32(a), iy = js_to_int32(b);
    if (op == PACK("&")) return js_num(ix & iy);
    if (op == PACK("|")) return js_num(ix | iy);
    if (op == PACK("^")) return js_num(ix ^ iy);
    if (op == PACK("<<")) return js_num((int32_t)((uint32_t)ix << (iy & 31)));
    if (op == PACK(">>")) return js_num(ix >> (iy & 31));
    if (op == PACK(">>>")) return js_num((double)((uint32_t)ix >> (iy & 31)));
    return js_undef();
}

static double js_pow(double base, double exponent) {
    extern double js_math_pow(double, double);
    return js_math_pow(base, exponent);
}

static uint32_t assign_binary_op(int index) {
    static const char* const ops[] = {
        "", "+", "-", "*", "/", "%", "**", "<<", ">>", ">>>", "&", "|", "^"
    };
    return index > 0 && index < 13 ? pack(ops[index]) : 0;
}

static js_val get_reference(js_node* target, js_env* env, js_val* object,
                            js_str** key) {
    if (target->type == N_IDENT) {
        *key = target->str;
        return lookup_identifier(env, target->str, 0);
    }
    *object = eval(target->a, env);
    JS_CHECK(*object);
    *key = member_key(target, env);
    JS_CHECK(*key);
    return js_get(*object, *key);
}

static void put_reference(js_node* target, js_env* env, js_val object,
                          js_str* key, js_val value) {
    if (target->type == N_IDENT) assign_identifier(env, key, value);
    else js_set(object, key, value);
}

__attribute__((noinline)) static js_val eval_args(js_node* call, js_env* env, js_val** out, int* argc) {
    int count = 0;
    int spread = 0;
    for (uint32_t i = 0; i < call->count; i++) spread |= call->list[i]->type == N_SPREAD;
    if (!spread) {
        js_val* args = call->count ? (js_val*)(scope_allowed ? scope_alloc(sizeof(js_val) * call->count) : NULL) : NULL;
        if (call->count && !args) args = (js_val*)js_alloc(sizeof(js_val) * call->count);
        for (uint32_t i = 0; i < call->count; i++) {
            args[i] = eval(call->list[i], env);
            JS_CHECK(args[i]);
        }
        *out = args;
        *argc = (int)call->count;
        return js_undef();
    }
    js_obj* list = js_new_array(call->count);
    for (uint32_t i = 0; i < call->count; i++) {
        js_node* item = call->list[i];
        if (item->type == N_SPREAD) {
            js_val v = eval(item->a, env);
            JS_CHECK(v);
            if (v.type == JS_OBJ && (v.u.obj->cls == CLS_ARRAY || v.u.obj->cls == CLS_ARGUMENTS)) {
                for (uint32_t k = 0; k < v.u.obj->length; k++) js_array_push(list, v.u.obj->items[k]);
            } else {
                js_val from = js_get(js_get_c(js_obj_val(js_global), "Array"), js_intern_c("from"));
                js_val arr = js_call(from, js_undef(), &v, 1);
                JS_CHECK(arr);
                if (arr.type == JS_OBJ)
                    for (uint32_t k = 0; k < arr.u.obj->length; k++) js_array_push(list, arr.u.obj->items[k]);
            }
        } else {
            js_val v = eval(item, env);
            JS_CHECK(v);
            js_array_push(list, v);
        }
        count = (int)list->length;
    }
    *out = list->items;
    *argc = (int)list->length;
    (void)count;
    return js_undef();
}

__attribute__((noinline, cold)) static js_val throw_named(const char* name, const char* what) {
    char message[200];
    mx_copy(message, sizeof(message), name);
    mx_append(message, sizeof(message), what);
    return js_throw_error("TypeError", message);
}

__attribute__((noinline, cold)) static js_val throw_not_method(js_node* callee, js_val self,
                                                              js_str* key) {
    char message[200];
    char what[80];
    js_inspect(self, what, sizeof(what), 0, 0);
    mx_copy(message, sizeof(message), callee->a->type == N_IDENT ? callee->a->str->data : what);
    mx_append(message, sizeof(message), ".");
    mx_append(message, sizeof(message), key->data);
    mx_append(message, sizeof(message), " is not a function");
    return js_throw_error("TypeError", message);
}

static js_val find_super_proto(js_env* env) {
    js_obj* fn = env->function;
    js_obj* home = fn ? fn->home_object : NULL;
    for (js_env* e = env; !home && e; e = e->parent)
        if (e->function && e->function->home_object) home = e->function->home_object;
    if (!home) return js_undef();
    return home->proto ? js_obj_val(home->proto) : js_undef();
}

static js_obj* build_class(js_node* n, js_env* env);

__attribute__((noinline)) static js_val eval_template(js_node* n, js_env* env) {
    js_str* result = js_intern_c("");
    for (uint32_t i = 0; i < n->count; i++) {
        js_val v = eval(n->list[i], env);
        JS_CHECK(v);
        result = js_concat(result, js_to_string(v));
        JS_CHECK(v);
    }
    return js_str_val(result);
}

__attribute__((noinline)) static js_val eval_object(js_node* n, js_env* env) {
    js_obj* o = js_new_object(js_object_proto);
    js_val ov = js_obj_val(o);
    for (uint32_t i = 0; i < n->count; i++) {
        js_node* prop = n->list[i];
        if (prop->op == PROP_SPREAD) {
            js_val src = eval(prop->b, env);
            JS_CHECK(src);
            if (src.type == JS_OBJ) {
                js_obj* keys = js_own_keys(src.u.obj, 0);
                for (uint32_t k = 0; k < keys->length; k++) {
                    js_str* key = keys->items[k].u.str;
                    js_set(ov, key, js_get(src, key));
                }
            } else if (src.type == JS_STR) {
                js_val s = src;
                double len = js_to_number(js_get(s, S_length));
                for (int k = 0; k < (int)len; k++)
                    js_set(ov, key_of(js_num(k)), js_get_index(s, js_num(k)));
            }
            continue;
        }
        js_str* key = prop->flag ? key_of(eval(prop->a, env)) : prop->str;
        JS_CHECK(key);
        if (prop->op == PROP_GET || prop->op == PROP_SET) {
            js_val fn = js_make_function(prop->b->func, env, env->this_val);
            fn.u.obj->home_object = o;
            js_prop* p = js_find_own(o, key);
            if (!p || !(p->flags & PROP_ACCESSOR)) {
                if (!p) p = add_prop(o, key);
                p->value = js_undef();
                p->setter = NULL;
                p->flags = PROP_ACCESSOR;
            }
            if (prop->op == PROP_GET) p->value = fn;
            else p->setter = fn.u.obj;
            continue;
        }
        js_val value;
        if (prop->b->type == N_FUNC && !prop->b->func->arrow && prop->b->func->name == prop->str) {
            value = js_make_function(prop->b->func, env, env->this_val);
            value.u.obj->home_object = o;
        } else {
            value = eval(prop->b, env);
        }
        JS_CHECK(value);
        if (value.type == JS_OBJ && js_is_callable(value) && value.u.obj->name &&
            !value.u.obj->name->length)
            value.u.obj->name = key;
        if (key == S_proto && !prop->flag && prop->op == PROP_INIT) {
            o->proto = value.type == JS_OBJ ? value.u.obj : value.type == JS_NULL ? NULL : o->proto;
            continue;
        }
        js_prop* p = js_find_own(o, key);
        if (!p) p = add_prop(o, key);
        p->value = value;
        p->flags = 0;
    }
    return ov;
}

__attribute__((noinline)) static js_val eval_call(js_node* n, js_env* env) {
    js_node* callee = n->a;
    js_val self = js_undef();
    js_val fn;
    if (callee->type == N_SUPER) {
        /* super(...) in a derived constructor */
        js_obj* ctor = env->function;
        for (js_env* e = env; e && (!ctor || !ctor->func || !ctor->func->is_class_constructor); e = e->parent)
            ctor = e->function;
        js_obj* parent = ctor ? ctor->proto : NULL;
        js_val* args;
        int argc;
        eval_args(n, env, &args, &argc);
        JS_CHECK(fn);
        js_val this_val = env->this_val;
        if (parent && parent != js_function_proto) {
            if (parent->cls == CLS_NATIVE) {
                js_obj* saved_new_target = js_super_new_target;
                js_super_new_target = env->new_target ? env->new_target : ctor;
                js_val made = parent->native(js_undef(), args, argc, parent);
                js_super_new_target = saved_new_target;
                JS_CHECK(made);
                if (made.type == JS_OBJ && this_val.type == JS_OBJ && made.u.obj != this_val.u.obj &&
                    made.u.obj->cls == CLS_NODE && made.u.obj->node) {
                    /* custom element: this object becomes the element's wrapper */
                    js_obj* target = this_val.u.obj;
                    target->cls = CLS_NODE;
                    target->node = made.u.obj->node;
                    target->node->js_wrapper = target;
                } else if (made.type == JS_OBJ && this_val.type == JS_OBJ && made.u.obj != this_val.u.obj) {
                    /* adopt the built-in's state */
                    js_obj* target = this_val.u.obj;
                    js_obj* src = made.u.obj;
                    target->cls = src->cls;
                    target->items = src->items;
                    target->length = src->length;
                    target->item_capacity = src->item_capacity;
                    target->data = src->data;
                    target->number = src->number;
                    for (uint32_t i = 0; i < src->prop_count; i++)
                        js_set(this_val, src->props[i].key, src->props[i].value);
                }
            } else {
                js_val r = call_function(parent, this_val, args, argc, env->new_target ? env->new_target : ctor);
                JS_CHECK(r);
                if (r.type == JS_OBJ && r.u.obj != this_val.u.obj) {
                    for (js_env* e = env; e; e = e->parent) {
                        e->this_val = r;
                        if (e->is_function && e->function == ctor) break;
                    }
                    this_val = r;
                }
            }
        }
        if (ctor) init_class_fields(ctor, this_val);
        return js_undef();
    }
    if (callee->type == N_MEMBER) {
        if (callee->a->type == N_SUPER) {
            js_val proto = find_super_proto(env);
            self = env->this_val;
            js_str* key = member_key(callee, env);
            JS_CHECK(key);
            fn = proto.type == JS_OBJ ? get_from(proto.u.obj, key, self) : js_undef();
        } else {
            self = eval(callee->a, env);
            JS_CHECK(self);
            if (callee->flag2 && (self.type == JS_UNDEF || self.type == JS_NULL)) return js_undef();
            js_str* key = member_key(callee, env);
            JS_CHECK(key);
            fn = js_get(self, key);
            JS_CHECK(fn);
            if (!js_is_callable(fn)) {
                if (n->flag2 && (fn.type == JS_UNDEF || fn.type == JS_NULL)) return js_undef();
                return throw_not_method(callee, self, key);
            }
        }
    } else {
        fn = eval(callee, env);
        JS_CHECK(fn);
        if (!js_is_callable(fn)) {
            if (n->flag2 && (fn.type == JS_UNDEF || fn.type == JS_NULL)) return js_undef();
            return throw_named(callee->type == N_IDENT ? callee->str->data : "expression",
                               " is not a function");
        }
    }
    js_val* args;
    int argc;
    eval_args(n, env, &args, &argc);
    JS_CHECK(fn);
    js_rt.line = n->line;
    return call_function(fn.u.obj, self, args, argc, NULL);
}

__attribute__((noinline)) static js_val eval_update(js_node* n, js_env* env) {
    js_val object = js_undef();
    js_str* key;
    js_val old = get_reference(n->a, env, &object, &key);
    JS_CHECK(old);
    double value = js_to_number(old);
    double updated = n->op == '+' ? value + 1 : value - 1;
    put_reference(n->a, env, object, key, js_num(updated));
    return js_num(n->flag ? updated : value);
}

/* `name += rhs;` where name is a string variable in scope: see append_owned.
 * Returns 0 to let the generic path handle it. */
__attribute__((noinline)) static int append_statement(js_node* n, js_env* env) {
    js_str* name = n->a->str;
    js_var* v = env_lookup(env, name);
    if (!v || !v->initialized || v->kind == VAR_CONST || v->value.type != JS_STR) return 0;
    js_str* old = v->value.u.str;
    js_val rhs = eval(n->b, env);
    if (js_rt.ctl) return 1;
    js_str* tail;
    if (rhs.type == JS_STR) tail = rhs.u.str;
    else if (rhs.type == JS_OBJ) {
        js_val p = js_to_primitive(rhs, 2);
        if (js_rt.ctl) return 1;
        tail = js_to_string(p);
    } else tail = js_to_string(rhs);
    if (js_rt.ctl) return 1;
    js_str* result = append_owned(old, tail);
    v = env_lookup(env, name);   /* the scope may have grown meanwhile */
    if (v) v->value = js_str_val(result);
    else { result->owned = 0; assign_identifier(env, name, js_str_val(result)); }
    return 1;
}

__attribute__((noinline)) static js_val eval_assign(js_node* n, js_env* env) {
    int op = n->op;
    js_node* target = n->a;
    if (op == 0) {
        if (target->type == N_ARRAY_PAT || target->type == N_OBJECT_PAT) {
            js_val v = eval(n->b, env);
            JS_CHECK(v);
            bind_pattern(target, v, env, VAR_VAR, 0);
            return v;
        }
        if (target->type == N_IDENT) {
            js_val v = eval(n->b, env);
            JS_CHECK(v);
            if (v.type == JS_OBJ && js_is_callable(v) && v.u.obj->name && !v.u.obj->name->length)
                v.u.obj->name = target->str;
            assign_identifier(env, target->str, v);
            return v;
        }
        js_val object = eval(target->a, env);
        JS_CHECK(object);
        if (target->flag) {
            js_val k = eval(target->b, env);
            JS_CHECK(k);
            js_val v = eval(n->b, env);
            JS_CHECK(v);
            js_set_index(object, k, v);
            return v;
        }
        js_val v = eval(n->b, env);
        JS_CHECK(v);
        js_set(object, target->str, v);
        return v;
    }
    js_val object = js_undef();
    js_str* key;
    js_val old = get_reference(target, env, &object, &key);
    JS_CHECK(old);
    js_val v;
    if (op >= 13) {   /* &&= ||= ??= */
        int assign = op == 13 ? js_truthy(old) : op == 14 ? !js_truthy(old)
                                                          : (old.type == JS_UNDEF || old.type == JS_NULL);
        if (!assign) return old;
        v = eval(n->b, env);
    } else {
        js_val rhs = eval(n->b, env);
        JS_CHECK(rhs);
        v = arithmetic(assign_binary_op(op), old, rhs);
    }
    JS_CHECK(v);
    put_reference(target, env, object, key, v);
    return v;
}

__attribute__((noinline)) static js_val eval_array(js_node* n, js_env* env) {
        js_obj* a = js_new_array(n->count);
        for (uint32_t i = 0; i < n->count; i++) {
            js_node* item = n->list[i];
            if (item->type == N_HOLE) {
                js_array_push(a, js_undef());
            } else if (item->type == N_SPREAD) {
                js_val v = eval(item->a, env);
                JS_CHECK(v);
                if (v.type == JS_OBJ && (v.u.obj->cls == CLS_ARRAY || v.u.obj->cls == CLS_ARGUMENTS)) {
                    for (uint32_t k = 0; k < v.u.obj->length; k++) js_array_push(a, v.u.obj->items[k]);
                } else {
                    js_val from = js_get(js_get_c(js_obj_val(js_global), "Array"), js_intern_c("from"));
                    js_val arr = js_call(from, js_undef(), &v, 1);
                    JS_CHECK(arr);
                    for (uint32_t k = 0; arr.type == JS_OBJ && k < arr.u.obj->length; k++)
                        js_array_push(a, arr.u.obj->items[k]);
                }
            } else {
                js_val v = eval(item, env);
                JS_CHECK(v);
                js_array_push(a, v);
            }
        }
        return js_obj_val(a);
    }

__attribute__((noinline)) static js_val eval_new(js_node* n, js_env* env) {
        js_val ctor = eval(n->a, env);
        JS_CHECK(ctor);
        js_val* args;
        int argc;
        eval_args(n, env, &args, &argc);
        JS_CHECK(ctor);
        js_rt.line = n->line;
        if (!js_is_callable(ctor)) {
            return throw_named(n->a->type == N_IDENT ? n->a->str->data : "expression",
                               " is not a constructor");
        }
        return js_construct(ctor, args, argc);
    }

__attribute__((noinline)) static js_val eval_unary(js_node* n, js_env* env) {
        if (n->op == 't') {
            if (n->a->type == N_IDENT) {
                js_val v = lookup_identifier(env, n->a->str, 1);
                JS_CHECK(v);
                return js_str_val(js_intern_c(js_typeof(v)));
            }
            js_val v = eval(n->a, env);
            JS_CHECK(v);
            return js_str_val(js_intern_c(js_typeof(v)));
        }
        if (n->op == 'd') {
            if (n->a->type == N_MEMBER) {
                js_val object = eval(n->a->a, env);
                JS_CHECK(object);
                js_str* key = member_key(n->a, env);
                JS_CHECK(key);
                return js_bool(js_delete(object, key));
            }
            return js_bool(1);
        }
        js_val v = eval(n->a, env);
        JS_CHECK(v);
        switch (n->op) {
        case '!': return js_bool(!js_truthy(v));
        case '-': return js_num(-js_to_number(v));
        case '+': return js_num(js_to_number(v));
        case '~': return js_num(~js_to_int32(v));
        case 'v': return js_undef();
        }
        return js_undef();
    }

#define TAGGED_CACHE 512
static struct { js_node* node; js_obj* strings; } tagged_cache[TAGGED_CACHE];

__attribute__((noinline)) static js_val eval_tagged(js_node* n, js_env* env) {
        js_val fn = eval(n->a, env);
        JS_CHECK(fn);
        /* the strings array is the same object every time this tag runs
         * (lit-html and others cache templates by it) */
        uint32_t slot = (uint32_t)(((uintptr_t)n >> 4) * 2654435761u) % TAGGED_CACHE;
        js_obj* strings = tagged_cache[slot].node == n ? tagged_cache[slot].strings : NULL;
        if (!strings) {
            strings = js_new_array(0);
            js_obj* raw = js_new_array(0);
            for (uint32_t i = 0; i < n->b->count; i += 2) {
                js_node* part = n->b->list[i];
                js_array_push(strings, js_str_val(part->str));
                js_array_push(raw, js_str_val(part->b ? part->b->str : part->str));
            }
            js_define(strings, "raw", js_obj_val(raw), PROP_HIDDEN);
            raw->frozen = strings->frozen = 1;
            tagged_cache[slot].node = n;
            tagged_cache[slot].strings = strings;
        }
        js_val* args = (js_val*)js_alloc(sizeof(js_val) * (n->b->count / 2 + 2));
        int argc = 1;
        for (uint32_t i = 1; i < n->b->count; i += 2) {
            args[argc++] = eval(n->b->list[i], env);
            JS_CHECK(fn);
        }
        args[0] = js_obj_val(strings);
        return js_call(fn, js_undef(), args, argc);
    }

__attribute__((noinline)) static js_val eval_yield(js_node* n, js_env* env) {
        if (!current_gen && current_coro && current_coro->is_generator) {
            js_val v = n->a ? eval(n->a, env) : js_undef();
            JS_CHECK(v);
            return n->flag ? coro_yield_star(current_coro, v) : coro_yield(current_coro, v);
        }
        if (!current_gen) return js_throw_error("SyntaxError", "yield is only valid in generator functions");
        js_val v = n->a ? eval(n->a, env) : js_undef();
        JS_CHECK(v);
        if (n->flag) {
            js_val from = js_get(js_get_c(js_obj_val(js_global), "Array"), js_intern_c("from"));
            js_val list = js_call(from, js_undef(), &v, 1);
            JS_CHECK(list);
            for (uint32_t i = 0; list.type == JS_OBJ && i < list.u.obj->length; i++)
                js_array_push(current_gen->values, list.u.obj->items[i]);
        } else {
            js_array_push(current_gen->values, v);
        }
        if (current_gen->values->length > GEN_MAX_YIELDS) {
            /* an endless generator: stop it here */
            js_rt.ctl = CTL_RETURN;
            js_rt.value = js_undef();
        }
        return js_undef();
    }

__attribute__((noinline)) static js_val eval_import_call(js_node* n, js_env* env) {
        js_val spec = eval(n->a, env);
        JS_CHECK(spec);
        extern const char* js_page_url(void);
        char resolved[1024];
        js_str* sp = js_to_string(spec);
        const char* base = n->str ? n->str->data : js_page_url();
        if (!mx_url_resolve(base, sp->data, resolved, sizeof(resolved)))
            mx_copy(resolved, sizeof(resolved), sp->data);
        js_val ns = js_load_module(resolved);
        if (js_rt.ctl == CTL_THROW) {
            js_val error = js_rt.value;
            js_rt.ctl = CTL_NONE;
            return js_promise_resolved(error, 1);
        }
        return js_promise_resolved(ns, 0);
    }

__attribute__((noinline)) static js_val eval_import_meta(js_node* n, js_env* env) {
        extern const char* js_page_url(void);
        js_obj* meta = js_new_object(js_object_proto);
        js_set(js_obj_val(meta), js_intern_c("url"),
               js_str_val(n->str ? n->str : js_cstr(js_page_url())));
        return js_obj_val(meta);
    }

static js_val eval(js_node* n, js_env* env) {
    if (js_rt.ctl) return js_undef();
    switch (n->type) {
    case N_NUM: return js_num(n->num);
    case N_STR: return js_str_val(n->str);
    case N_TRUE: return js_bool(1);
    case N_FALSE: return js_bool(0);
    case N_NULL: return js_null();
    case N_UNDEF: return js_undef();
    case N_THIS: return env->this_val;
    case N_TEMPLATE: return eval_template(n, env);
    case N_IDENT: return lookup_identifier(env, n->str, 0);
    case N_REGEX: return js_regexp_create(n->str, n->a->str);
    case N_FUNC: {
        js_val fn = js_make_function(n->func, env, env->this_val);
        return fn;
    }
    case N_CLASS: return js_obj_val(build_class(n, env));
    case N_ARRAY: return eval_array(n, env);
    case N_OBJECT: return eval_object(n, env);
    case N_MEMBER: {
        if (n->a->type == N_SUPER) {
            js_val proto = find_super_proto(env);
            js_str* key = member_key(n, env);
            JS_CHECK(key);
            return proto.type == JS_OBJ ? get_from(proto.u.obj, key, env->this_val) : js_undef();
        }
        js_val object = eval(n->a, env);
        JS_CHECK(object);
        if (n->flag2 && (object.type == JS_UNDEF || object.type == JS_NULL)) return js_undef();
        if (n->flag) {
            js_val k = eval(n->b, env);
            JS_CHECK(k);
            js_rt.line = n->line;
            return js_get_index(object, k);
        }
        js_rt.line = n->line;
        return js_get(object, n->str);
    }
    case N_CALL: return eval_call(n, env);
    case N_NEW: return eval_new(n, env);
    case N_UNARY: return eval_unary(n, env);
    case N_UPDATE: return eval_update(n, env);
    case N_BINARY: {
        js_val a = eval(n->a, env);
        JS_CHECK(a);
        js_val b = eval(n->b, env);
        JS_CHECK(b);
        js_rt.line = n->line;
        if (n->op == 'i') {
            if (b.type != JS_OBJ)
                return js_throw_error("TypeError", "Cannot use 'in' operator on a primitive");
            return js_bool(js_has_property(b, key_of(a)));
        }
        if (n->op == 'I') return js_bool(js_instanceof(a, b));
        return arithmetic((uint32_t)n->num, a, b);
    }
    case N_LOGICAL: {
        js_val a = eval(n->a, env);
        JS_CHECK(a);
        if (n->op == '&') return js_truthy(a) ? eval(n->b, env) : a;
        if (n->op == '|') return js_truthy(a) ? a : eval(n->b, env);
        return (a.type == JS_UNDEF || a.type == JS_NULL) ? eval(n->b, env) : a;
    }
    case N_ASSIGN: return eval_assign(n, env);
    case N_COND: {
        js_val t = eval(n->a, env);
        JS_CHECK(t);
        return js_truthy(t) ? eval(n->b, env) : eval(n->c, env);
    }
    case N_SEQ: {
        js_val v = js_undef();
        for (uint32_t i = 0; i < n->count; i++) {
            v = eval(n->list[i], env);
            JS_CHECK(v);
        }
        return v;
    }
    case N_TAGGED: return eval_tagged(n, env);
    case N_AWAIT: {
        js_val v = eval(n->a, env);
        JS_CHECK(v);
        return coro_await(v);
    }
    case N_YIELD: return eval_yield(n, env);
    case N_IMPORT_CALL: return eval_import_call(n, env);
    case N_IMPORT_META: return eval_import_meta(n, env);
    case N_SUPER:
        return find_super_proto(env);
    case N_SPREAD:
        return js_throw_error("SyntaxError", "Unexpected spread");
    default:
        return js_throw_error("SyntaxError", "Unsupported expression");
    }
}

/* ---- classes ---- */

static void init_class_fields(js_obj* ctor, js_val self) {
    if (!ctor || !ctor->func || !ctor->func->class_fields) return;
    js_node* fields = ctor->func->class_fields;
    js_env* env = ctor->env;
    for (uint32_t i = 0; i < fields->count && !js_rt.ctl; i++) {
        js_node* m = fields->list[i];
        js_str* key = m->str;
        if (m->flag && m->a) key = key_of(eval(m->a, env));
        js_val value = js_undef();
        if (m->func) {
            js_val fn = js_make_function(m->func, env, self);
            value = js_call(fn, self, NULL, 0);
        }
        if (js_rt.ctl) return;
        js_set(self, key, value);
    }
}

__attribute__((noinline)) static js_obj* build_class(js_node* n, js_env* env) {
    js_val parent = js_undef();
    if (n->a) {
        parent = eval(n->a, env);
        if (js_rt.ctl) return js_new_object(js_object_proto);
        if (parent.type != JS_NULL && !js_is_callable(parent)) {
            js_throw_error("TypeError", "Class extends value is not a constructor or null");
            return js_new_object(js_object_proto);
        }
    }
    js_env* class_env = new_env(env, 2, 0);
    js_func* ctor_func = NULL;
    for (uint32_t i = 0; i < n->count; i++)
        if (n->list[i]->op == 8) ctor_func = n->list[i]->func;
    js_func* f = (js_func*)js_alloc(sizeof(js_func));
    if (ctor_func) {
        *f = *ctor_func;
    } else {
        memset(f, 0, sizeof(*f));
        f->has_closures = 1;
        if (n->a) {
            /* default derived constructor: super(...arguments) */
            f->uses_arguments = 1;
            js_node* body = (js_node*)js_alloc(sizeof(js_node));
            memset(body, 0, sizeof(*body));
            body->type = N_BLOCK;
            js_node* stmt = (js_node*)js_alloc(sizeof(js_node));
            memset(stmt, 0, sizeof(*stmt));
            stmt->type = N_EXPR_STMT;
            js_node* call = (js_node*)js_alloc(sizeof(js_node));
            memset(call, 0, sizeof(*call));
            call->type = N_CALL;
            call->a = (js_node*)js_alloc(sizeof(js_node));
            memset(call->a, 0, sizeof(js_node));
            call->a->type = N_SUPER;
            js_node* spread = (js_node*)js_alloc(sizeof(js_node));
            memset(spread, 0, sizeof(*spread));
            spread->type = N_SPREAD;
            spread->a = (js_node*)js_alloc(sizeof(js_node));
            memset(spread->a, 0, sizeof(js_node));
            spread->a->type = N_IDENT;
            spread->a->str = K_arguments;
            call->list = (js_node**)js_alloc(sizeof(js_node*));
            call->list[0] = spread;
            call->count = 1;
            stmt->a = call;
            body->list = (js_node**)js_alloc(sizeof(js_node*));
            body->list[0] = stmt;
            body->count = 1;
            f->body = body;
        }
    }
    f->is_class_constructor = 1;
    f->derived = n->a != NULL;
    f->name = n->str ? n->str : js_intern_c("");
    f->has_closures = 1;
    /* instance fields */
    node_list fields = {0};
    for (uint32_t i = 0; i < n->count; i++)
        if (n->list[i]->op == 7 && !n->list[i]->flag2) list_push(&fields, n->list[i]);
    if (fields.count) {
        js_node* holder = (js_node*)js_alloc(sizeof(js_node));
        memset(holder, 0, sizeof(*holder));
        holder->list = fields.items;
        holder->count = fields.count;
        f->class_fields = holder;
    }
    js_obj* ctor = js_make_function(f, class_env, js_undef()).u.obj;
    js_obj* proto = js_new_object(js_object_proto);
    if (parent.type == JS_OBJ) {
        js_val parent_proto = js_get(parent, S_prototype);
        proto->proto = parent_proto.type == JS_OBJ ? parent_proto.u.obj : NULL;
        ctor->proto = parent.u.obj;
    } else if (parent.type == JS_NULL) {
        proto->proto = NULL;
    }
    js_define(proto, "constructor", js_obj_val(ctor), PROP_HIDDEN);
    js_define(ctor, "prototype", js_obj_val(proto), PROP_HIDDEN);
    ctor->home_object = proto;
    if (n->str) {
        js_var* self = env_declare(class_env, n->str, VAR_CONST);
        self->value = js_obj_val(ctor);
        self->initialized = 1;
    }
    for (uint32_t i = 0; i < n->count && !js_rt.ctl; i++) {
        js_node* m = n->list[i];
        if (m->op == 8) continue;
        js_obj* target = m->flag2 ? ctor : proto;
        if (m->op == 9) {   /* static block */
            js_val fn = js_make_function(m->func, class_env, js_obj_val(ctor));
            js_call(fn, js_obj_val(ctor), NULL, 0);
            continue;
        }
        js_str* key = m->flag ? key_of(eval(m->a, class_env)) : m->str;
        if (js_rt.ctl) break;
        if (m->op == 7) {
            if (m->flag2) {   /* static field */
                js_val value = js_undef();
                if (m->func) {
                    js_val fn = js_make_function(m->func, class_env, js_obj_val(ctor));
                    value = js_call(fn, js_obj_val(ctor), NULL, 0);
                }
                js_set(js_obj_val(ctor), key, value);
            }
            continue;
        }
        js_val fn = js_make_function(m->func, class_env, js_undef());
        fn.u.obj->home_object = target;
        js_prop* p = js_find_own(target, key);
        if (m->op == PROP_GET || m->op == PROP_SET) {
            if (!p || !(p->flags & PROP_ACCESSOR)) {
                if (!p) p = add_prop(target, key);
                p->value = js_undef();
                p->setter = NULL;
            }
            p->flags = PROP_ACCESSOR | PROP_HIDDEN;
            if (m->op == PROP_GET) p->value = fn;
            else p->setter = fn.u.obj;
        } else {
            if (!p) p = add_prop(target, key);
            p->value = fn;
            p->flags = PROP_HIDDEN;
        }
    }
    return ctor;
}

/* ---- statements ---- */

static int block_needs_scope(js_node* block) {
    return block->flag;
}

static void exec_block(js_node* n, js_env* env) {
    js_env* scope = env;
    size_t saved = scope_top;
    if (block_needs_scope(n)) scope = new_env(env, 4, scope_allowed);
    for (uint32_t i = 0; i < n->count && !js_rt.ctl; i++) exec(n->list[i], scope);
    if (scope != env && scope->on_stack) scope_top = saved;
    if (scope != env) env_release(scope);
}

/* Copies loop variables into a fresh scope so closures see per-iteration
 * values of `let`. */
static js_env* iteration_scope(js_env* loop_env, js_env* parent) {
    js_env* fresh = new_env(parent, loop_env->count + 1, scope_allowed);
    for (uint32_t i = 0; i < loop_env->count; i++) { fresh->vars[i] = loop_env->vars[i]; share_val(fresh->vars[i].value); }
    fresh->count = loop_env->count;
    return fresh;
}

static int loop_continue(js_node* label_owner, js_str* own_label) {
    (void)label_owner;
    if (js_rt.ctl == CTL_BREAK) {
        if (!js_rt.label || js_rt.label == own_label) {
            js_rt.ctl = CTL_NONE;
            js_rt.label = NULL;
        }
        return 0;
    }
    if (js_rt.ctl == CTL_CONTINUE) {
        if (!js_rt.label || js_rt.label == own_label) {
            js_rt.ctl = CTL_NONE;
            js_rt.label = NULL;
            return 1;
        }
        return 0;
    }
    return js_rt.ctl == CTL_NONE;
}


/* for (x of iterable) / for await (x of iterable): asks the iterator for
 * one value at a time (generators can be endless) and closes it with
 * return() when the loop is left early. */
static void exec_for_of_iterator(js_node* n, js_env* env, js_str* label, js_val subject) {
    int is_await = n->flag;
    js_val method = js_undef();
    int sync_source = 0;
    if (is_await) method = js_get_c(subject, "@@asyncIterator");
    if (js_rt.ctl) return;
    if (!js_is_callable(method)) {
        method = js_get_c(subject, "@@iterator");
        sync_source = 1;
    }
    if (js_rt.ctl) return;
    js_val it;
    if (js_is_callable(method)) {
        it = js_call(method, subject, NULL, 0);
    } else if (subject.type == JS_OBJ && is_await) {
        /* array-likes */
        js_val from = js_get(js_get_c(js_obj_val(js_global), "Array"), js_intern_c("from"));
        js_val arr = js_call(from, js_undef(), &subject, 1);
        if (js_rt.ctl) return;
        it = js_list_iterator(arr.type == JS_OBJ ? arr.u.obj : js_new_array(0));
    } else {
        js_throw_error("TypeError", "object is not iterable");
        return;
    }
    if (js_rt.ctl) return;
    js_val next = js_get_c(it, "next");
    if (js_rt.ctl) return;
    if (!js_is_callable(next)) {
        js_throw_error("TypeError", "iterator.next is not a function");
        return;
    }
    js_node* left = n->a;
    int kind = left->type == N_VAR ? left->op : -1;
    js_node* pattern = left->type == N_VAR ? left->list[0]->a : left;
    for (;;) {
        if (!step()) return;
        js_val r = js_call(next, it, NULL, 0);
        if (js_rt.ctl) return;
        if (is_await && !sync_source) {
            r = coro_await(r);
            if (js_rt.ctl) return;
        }
        if (r.type != JS_OBJ) {
            js_throw_error("TypeError", "Iterator result is not an object");
            return;
        }
        if (js_truthy(js_get_c(r, "done"))) return;
        js_val item = js_get_c(r, "value");
        if (js_rt.ctl) return;
        if (is_await && sync_source) {
            item = coro_await(item);
            if (js_rt.ctl) return;
        }
        size_t saved = scope_top;
        js_env* scope = env;
        if (kind == VAR_LET || kind == VAR_CONST) scope = new_env(env, 2, scope_allowed);
        bind_pattern(pattern, item, scope, kind >= 0 ? kind : VAR_VAR,
                     kind == VAR_LET || kind == VAR_CONST);
        if (!js_rt.ctl) exec(n->c, scope);
        if (scope != env && scope->on_stack) scope_top = saved;
        if (scope != env) env_release(scope);
        int leaving = js_rt.ctl == CTL_BREAK || js_rt.ctl == CTL_RETURN || js_rt.ctl == CTL_THROW ||
                      (js_rt.ctl == CTL_CONTINUE && js_rt.label && js_rt.label != label);
        if (leaving) {
            js_ctl ctl = js_rt.ctl;
            js_val value = js_rt.value;
            js_str* saved_label = js_rt.label;
            js_rt.ctl = CTL_NONE;
            js_val ret = js_get_c(it, "return");
            if (!js_rt.ctl && js_is_callable(ret)) {
                js_val done = js_call(ret, it, NULL, 0);
                if (!js_rt.ctl && is_await && !sync_source) coro_await(done);
            }
            if (js_rt.ctl != CTL_THROW || ctl == CTL_THROW) {
                js_rt.ctl = ctl;
                js_rt.value = value;
                js_rt.label = saved_label;
            }
        }
        if (!loop_continue(n, label)) return;
    }
}

static void exec_for_in_of(js_node* n, js_env* env, js_str* label) {
    js_val subject = eval(n->b, env);
    if (js_rt.ctl) return;
    js_obj* items = NULL;
    if (n->type == N_FOR_IN) {
        if (subject.type != JS_OBJ) return;
        items = js_new_array(0);
        /* own keys then inherited enumerable keys */
        for (js_obj* o = subject.u.obj; o && o != js_object_proto && o != js_array_proto; o = o->proto) {
            js_obj* keys = js_own_keys(o, 0);
            for (uint32_t i = 0; i < keys->length; i++) js_array_push(items, keys->items[i]);
            if (o->cls == CLS_NODE) break;
        }
    } else {
        if (subject.type == JS_OBJ && (subject.u.obj->cls == CLS_ARRAY || subject.u.obj->cls == CLS_ARGUMENTS) &&
            !n->flag) {
            items = subject.u.obj;
        } else if (subject.type == JS_OBJ || n->flag) {
            exec_for_of_iterator(n, env, label, subject);
            return;
        } else {
            js_val from = js_get(js_get_c(js_obj_val(js_global), "Array"), js_intern_c("from"));
            js_val arr = js_call(from, js_undef(), &subject, 1);
            if (js_rt.ctl) return;
            items = arr.type == JS_OBJ ? arr.u.obj : js_new_array(0);
        }
    }
    js_node* left = n->a;
    int kind = left->type == N_VAR ? left->op : -1;
    js_node* pattern = left->type == N_VAR ? left->list[0]->a : left;
    for (uint32_t i = 0; i < items->length && !js_rt.ctl; i++) {
        if (!step()) return;
        size_t saved = scope_top;
        js_env* scope = env;
        if (kind == VAR_LET || kind == VAR_CONST) scope = new_env(env, 2, scope_allowed);
        js_val item = items->items[i];
        if (n->flag) {
            item = await_value(item);
            if (js_rt.ctl) return;
        }
        bind_pattern(pattern, item, scope, kind >= 0 ? kind : VAR_VAR,
                     kind == VAR_LET || kind == VAR_CONST);
        if (js_rt.ctl) return;
        exec(n->c, scope);
        if (scope != env && scope->on_stack) scope_top = saved;
        if (scope != env) env_release(scope);
        if (!loop_continue(n, label)) return;
    }
}

static void exec(js_node* n, js_env* env) {
    if (js_rt.ctl || !n) return;
    if (!step()) return;
    js_str* label = current_label;
    current_label = NULL;
    switch (n->type) {
    case N_EXPR_STMT:
        js_rt.line = n->line;
        if (n->a->type == N_ASSIGN && n->a->op == 1 && n->a->a->type == N_IDENT &&
            append_statement(n->a, env))
            return;
        eval(n->a, env);
        return;
    case N_VAR:
        for (uint32_t i = 0; i < n->count && !js_rt.ctl; i++) {
            js_node* d = n->list[i];
            js_rt.line = d->line;
            if (!d->b) {
                if (n->op == VAR_VAR) continue;
                bind_pattern(d->a, js_undef(), env, n->op, 1);
                continue;
            }
            js_val v = eval(d->b, env);
            if (js_rt.ctl) return;
            if (d->a->type == N_IDENT && v.type == JS_OBJ && js_is_callable(v) && v.u.obj->name &&
                !v.u.obj->name->length)
                v.u.obj->name = d->a->str;
            if (n->op == VAR_VAR) bind_pattern(d->a, v, env, VAR_VAR, 0);
            else bind_pattern(d->a, v, env, n->op, 1);
        }
        return;
    case N_FUNC_DECL:
    case N_EMPTY:
        return;
    case N_CLASS_DECL: {
        js_obj* c = build_class(n, env);
        if (js_rt.ctl) return;
        js_var* v = env_declare(env, n->str, VAR_LET);
        v->value = js_obj_val(c);
        v->initialized = 1;
        return;
    }
    case N_BLOCK:
        exec_block(n, env);
        return;
    case N_IF: {
        js_rt.line = n->line;
        js_val t = eval(n->a, env);
        if (js_rt.ctl) return;
        if (js_truthy(t)) exec(n->b, env);
        else if (n->c) exec(n->c, env);
        return;
    }
    case N_WHILE:
        for (;;) {
            js_rt.line = n->line;
            js_val t = eval(n->a, env);
            if (js_rt.ctl || !js_truthy(t)) return;
            exec(n->b, env);
            if (!loop_continue(n, label)) return;
            if (!step()) return;
        }
    case N_DO:
        for (;;) {
            exec(n->a, env);
            if (!loop_continue(n, label)) return;
            js_val t = eval(n->b, env);
            if (js_rt.ctl || !js_truthy(t)) return;
            if (!step()) return;
        }
    case N_FOR: {
        size_t saved = scope_top;
        js_env* loop_env = env;
        int lexical = n->a && n->a->type == N_VAR && n->a->op != VAR_VAR;
        if (lexical) loop_env = new_env(env, 4, scope_allowed);
        if (n->a) {
            if (n->a->type == N_VAR) exec(n->a, loop_env);
            else eval(n->a, loop_env);
        }
        /* per-iteration copies only matter when closures can capture them */
        int per_iteration = lexical && !scope_allowed;
        js_env* iter = per_iteration ? iteration_scope(loop_env, env) : loop_env;
        while (!js_rt.ctl) {
            if (n->b) {
                js_rt.line = n->line;
                js_val t = eval(n->b, iter);
                if (js_rt.ctl || !js_truthy(t)) break;
            }
            exec(n->d, iter);
            if (!loop_continue(n, label)) break;
            if (per_iteration) {
                js_env* done = iter;
                iter = iteration_scope(iter, env);
                env_release(done);
            }
            if (n->c) eval(n->c, iter);
            if (!step()) break;
        }
        if (loop_env->on_stack) scope_top = saved;
        if (iter != loop_env && iter != env) env_release(iter);
        if (loop_env != env) env_release(loop_env);
        return;
    }
    case N_FOR_IN:
    case N_FOR_OF:
        exec_for_in_of(n, env, label);
        return;
    case N_RETURN: {
        js_val v = n->a ? eval(n->a, env) : js_undef();
        if (js_rt.ctl) return;
        js_rt.ctl = CTL_RETURN;
        js_rt.value = v;
        return;
    }
    case N_BREAK:
        js_rt.ctl = CTL_BREAK;
        js_rt.label = n->str;
        return;
    case N_CONTINUE:
        js_rt.ctl = CTL_CONTINUE;
        js_rt.label = n->str;
        return;
    case N_THROW: {
        js_rt.line = n->line;
        js_val v = eval(n->a, env);
        if (js_rt.ctl) return;
        js_throw(v);
        return;
    }
    case N_TRY: {
        exec_block(n->a, env);
        if (js_rt.ctl == CTL_THROW && n->c) {
            if (js_rt.timed_out) {
                /* a runaway script cannot catch its own stop */
            } else {
                js_val error = js_rt.value;
                js_rt.ctl = CTL_NONE;
                js_env* scope = new_env(env, 2, 0);
                if (n->b) bind_pattern(n->b, error, scope, VAR_LET, 1);
                exec_block(n->c, scope);
            }
        }
        if (n->d) {
            js_ctl saved_ctl = js_rt.ctl;
            js_val saved_value = js_rt.value;
            js_str* saved_label = js_rt.label;
            js_rt.ctl = CTL_NONE;
            exec_block(n->d, env);
            if (js_rt.ctl == CTL_NONE) {
                js_rt.ctl = saved_ctl;
                js_rt.value = saved_value;
                js_rt.label = saved_label;
            }
        }
        return;
    }
    case N_SWITCH: {
        js_val d = eval(n->a, env);
        if (js_rt.ctl) return;
        int matched = -1;
        for (uint32_t i = 0; i < n->count && matched < 0; i++) {
            js_node* c = n->list[i];
            if (!c->a) continue;
            js_val v = eval(c->a, env);
            if (js_rt.ctl) return;
            if (js_strict_equals(d, v)) matched = (int)i;
        }
        if (matched < 0)
            for (uint32_t i = 0; i < n->count; i++)
                if (!n->list[i]->a) matched = (int)i;
        if (matched < 0) return;
        js_env* scope = new_env(env, 4, 0);
        for (uint32_t i = (uint32_t)matched; i < n->count && !js_rt.ctl; i++) {
            js_node* c = n->list[i];
            for (uint32_t k = 0; k < c->count && !js_rt.ctl; k++) exec(c->list[k], scope);
        }
        env_release(scope);
        if (js_rt.ctl == CTL_BREAK && (!js_rt.label || js_rt.label == label)) {
            js_rt.ctl = CTL_NONE;
            js_rt.label = NULL;
        }
        return;
    }
    case N_LABEL:
        current_label = n->str;
        exec(n->a, env);
        current_label = NULL;
        if (js_rt.ctl == CTL_BREAK && js_rt.label == n->str) {
            js_rt.ctl = CTL_NONE;
            js_rt.label = NULL;
        }
        return;
    default:
        eval(n, env);
        return;
    }
}

js_val js_exec_program(js_node* program) {
    js_func* f = program->func;
    /* In a classic script, top-level `this` is the global object (window).
     * Google's pages start with `this.gbar_ = ...`. */
    if (js_global) js_global_env->this_val = js_obj_val(js_global);
    hoist(f, js_global_env);
    /* top-level var and function declarations become window properties */
    for (int i = 0; i < f->var_count; i++) {
        js_var* v = env_lookup(js_global_env, f->vars[i]);
        if (v && !js_find_own(js_global, f->vars[i]))
            js_set(js_obj_val(js_global), f->vars[i], share_val(v->value));
    }
    /* drop them from the scope so the global object stays authoritative */
    uint32_t kept = 0;
    for (uint32_t i = 0; i < js_global_env->count; i++) {
        js_var* v = &js_global_env->vars[i];
        if (v->kind == VAR_VAR) {
            js_set(js_obj_val(js_global), v->name, share_val(v->value));
            continue;
        }
        js_global_env->vars[kept++] = *v;
    }
    js_global_env->count = kept;
    js_val last = js_undef();
    for (uint32_t i = 0; i < program->count && !js_rt.ctl; i++) {
        js_node* s = program->list[i];
        if (s->type == N_EXPR_STMT) {
            js_rt.line = s->line;
            last = eval(s->a, js_global_env);
        } else {
            exec(s, js_global_env);
        }
    }
    if (js_rt.ctl == CTL_RETURN || js_rt.ctl == CTL_BREAK || js_rt.ctl == CTL_CONTINUE)
        js_rt.ctl = CTL_NONE;
    return last;
}

/* ====================================================================== */
/* ES modules                                                             */
/* ====================================================================== */

typedef struct module_record {
    js_str* url;
    js_obj* ns;
    int state;      /* 1 evaluating, 2 done, 3 failed */
    struct module_record* next;
} module_record;

static module_record* modules;

static module_record* find_module(const char* url) {
    for (module_record* m = modules; m; m = m->next)
        if (js_str_eq_c(m->url, url)) return m;
    return NULL;
}

static js_val import_map;   /* {imports: {...}} from <script type=importmap> */

void js_set_import_map(js_val map);
void js_set_import_map(js_val map) {
    import_map = map;
}

static int resolve_specifier(const char* base, js_str* spec, char* out, size_t capacity) {
    const char* s = spec->data;
    js_val imports = import_map.type == JS_OBJ ? js_get_c(import_map, "imports") : js_undef();
    if (imports.type == JS_OBJ) {
        extern const char* js_page_url(void);
        js_val exact = js_get(imports, spec);
        if (exact.type == JS_STR) {
            if (!mx_url_resolve(js_page_url(), exact.u.str->data, out, capacity))
                mx_copy(out, capacity, exact.u.str->data);
            return 1;
        }
        /* longest matching "prefix/" key */
        js_obj* keys = js_own_keys(imports.u.obj, 0);
        js_str* best = NULL;
        for (uint32_t i = 0; i < keys->length; i++) {
            js_str* key = keys->items[i].u.str;
            if (key->length && key->data[key->length - 1] == '/' && !strncmp(s, key->data, key->length) &&
                (!best || key->length > best->length))
                best = key;
        }
        if (best) {
            js_val target = js_get(imports, best);
            if (target.type == JS_STR) {
                char joined[1024];
                mx_copy(joined, sizeof(joined), target.u.str->data);
                mx_append(joined, sizeof(joined), s + best->length);
                if (!mx_url_resolve(js_page_url(), joined, out, capacity)) mx_copy(out, capacity, joined);
                return 1;
            }
        }
    }
    if (!strncmp(s, "./", 2) || !strncmp(s, "../", 3) || s[0] == '/' || strstr(s, "://")) {
        if (!mx_url_resolve(base, s, out, capacity)) mx_copy(out, capacity, s);
        return 1;
    }
    char message[300];
    mx_copy(message, sizeof(message), "Failed to resolve module specifier \"");
    mx_append(message, sizeof(message), s);
    mx_append(message, sizeof(message), "\". Bare specifiers need an import map, which Mxplorer does not support.");
    js_throw_error("TypeError", message);
    return 0;
}

static void export_pattern_names(js_node* pattern, js_obj* ns, js_env* env) {
    if (!pattern) return;
    switch (pattern->type) {
    case N_IDENT: {
        js_var* v = env_lookup(env, pattern->str);
        js_set(js_obj_val(ns), pattern->str, v ? share_val(v->value) : js_undef());
        break;
    }
    case N_ASSIGN_PAT: case N_REST:
        export_pattern_names(pattern->a, ns, env);
        break;
    case N_ARRAY_PAT:
        for (uint32_t i = 0; i < pattern->count; i++) export_pattern_names(pattern->list[i], ns, env);
        break;
    case N_OBJECT_PAT:
        for (uint32_t i = 0; i < pattern->count; i++)
            export_pattern_names(pattern->list[i]->b, ns, env);
        break;
    default:
        break;
    }
}

static void exec_module(js_node* program, module_record* rec) {
    js_env* env = new_env(js_global_env, 64, 0);
    env->this_val = js_undef();
    env->is_function = 1;
    hoist(program->func, env);
    js_obj* ns = rec->ns;
    /* hoisted function exports are visible to cyclic importers early */
    for (uint32_t i = 0; i < program->count; i++) {
        js_node* s = program->list[i];
        if (s->type == N_EXPORT && s->op == 2)
            for (uint32_t k = 0; k < s->count; k++) export_pattern_names(s->list[k], ns, env);
    }
    for (uint32_t i = 0; i < program->count && !js_rt.ctl; i++) {
        js_node* s = program->list[i];
        if (s->type != N_IMPORT) continue;
        char url[1024];
        if (!resolve_specifier(rec->url->data, s->c->str, url, sizeof(url))) return;
        js_val dep = js_load_module(url);
        if (js_rt.ctl) return;
        for (uint32_t k = 0; k < s->count; k++) {
            js_node* spec = s->list[k];
            js_val value = js_str_eq_c(spec->str, "*") ? dep : js_get(dep, spec->str);
            js_var* v = env_declare(env, spec->b->str, VAR_CONST);
            v->value = value;
            v->initialized = 1;
        }
    }
    const char* saved_source = js_rt.source;
    js_rt.source = program->func->source_name;
    for (uint32_t i = 0; i < program->count && !js_rt.ctl; i++) {
        js_node* s = program->list[i];
        js_rt.line = s->line;
        if (s->type == N_IMPORT) continue;
        if (s->type != N_EXPORT) {
            exec(s, env);
            continue;
        }
        switch (s->op) {
        case 1: {
            if (s->b) exec(s->b, env);
            js_val value = s->a ? eval(s->a, env) : js_undef();
            if (js_rt.ctl) break;
            if (value.type == JS_OBJ && js_is_callable(value) && value.u.obj->name && !value.u.obj->name->length)
                value.u.obj->name = S_default;
            js_set(js_obj_val(ns), S_default, value);
            break;
        }
        case 2:
            exec(s->a, env);
            if (s->a && s->a->type == N_VAR)
                for (uint32_t k = 0; k < s->a->count; k++) export_pattern_names(s->a->list[k]->a, ns, env);
            break;
        case 3:
            if (s->c) {
                char url[1024];
                if (!resolve_specifier(rec->url->data, s->c->str, url, sizeof(url))) break;
                js_val dep = js_load_module(url);
                if (js_rt.ctl) break;
                for (uint32_t k = 0; k < s->count; k++)
                    js_set(js_obj_val(ns), s->list[k]->b->str, js_get(dep, s->list[k]->str));
            }
            break;
        case 4: {
            char url[1024];
            if (!resolve_specifier(rec->url->data, s->c->str, url, sizeof(url))) break;
            js_val dep = js_load_module(url);
            if (js_rt.ctl || dep.type != JS_OBJ) break;
            if (s->str) {
                js_set(js_obj_val(ns), s->str, dep);
            } else {
                js_obj* keys = js_own_keys(dep.u.obj, 0);
                for (uint32_t k = 0; k < keys->length; k++) {
                    js_str* key = keys->items[k].u.str;
                    if (key != S_default) js_set(js_obj_val(ns), key, js_get(dep, key));
                }
            }
            break;
        }
        }
    }
    /* local `export { a as b }` and final values of exported bindings */
    for (uint32_t i = 0; i < program->count; i++) {
        js_node* s = program->list[i];
        if (s->type != N_EXPORT) continue;
        if (s->op == 3 && !s->c)
            for (uint32_t k = 0; k < s->count; k++) {
                js_var* v = env_lookup(env, s->list[k]->str);
                js_set(js_obj_val(ns), s->list[k]->b->str, v ? share_val(v->value) : js_undef());
            }
        if (s->op == 2 && s->a && s->a->type == N_VAR)
            for (uint32_t k = 0; k < s->a->count; k++) export_pattern_names(s->a->list[k]->a, ns, env);
        if (s->op == 2)
            for (uint32_t k = 0; k < s->count; k++) export_pattern_names(s->list[k], ns, env);
    }
    js_rt.source = saved_source;
}

js_val js_run_module_source(const char* source, size_t length, const char* url,
                            const char* name) {
    module_record* rec = find_module(url);
    if (rec) return js_obj_val(rec->ns);
    rec = (module_record*)js_alloc(sizeof(module_record));
    rec->url = js_intern_c(url);
    rec->ns = js_new_object(NULL);
    rec->state = 1;
    rec->next = modules;
    modules = rec;
    char error[200];
    int line = 0;
    parse_as_module = 1;
    parse_module_url = rec->url;
    js_node* program = js_parse(source, length, js_intern_c(name)->data, error, sizeof(error), &line);
    parse_as_module = 0;
    parse_module_url = NULL;
    if (!program) {
        rec->state = 3;
        char message[260];
        mx_copy(message, sizeof(message), "Uncaught SyntaxError: ");
        mx_append(message, sizeof(message), error);
        mx_console_add(MX_LOG_ERROR, name, line, message);
        return js_obj_val(rec->ns);
    }
    exec_module(program, rec);
    rec->state = js_rt.ctl == CTL_THROW ? 3 : 2;
    return js_obj_val(rec->ns);
}

js_val js_load_module(const char* url) {
    module_record* rec = find_module(url);
    if (rec) return js_obj_val(rec->ns);
    char* body = NULL;
    size_t length = 0;
    if (!js_fetch_source(url, &body, &length)) {
        char message[300];
        mx_copy(message, sizeof(message), "Failed to fetch module: ");
        mx_append(message, sizeof(message), url);
        return js_throw_error("TypeError", message);
    }
    const char* slash = strrchr(url, '/');
    char name[64];
    mx_copy(name, sizeof(name), slash && slash[1] ? slash + 1 : url);
    char* q = strchr(name, '?');
    if (q) *q = '\0';
    js_val ns = js_run_module_source(body, length, url, name);
    free(body);
    return ns;
}

/* ====================================================================== */
/* Setup                                                                  */
/* ====================================================================== */

void js_core_init(void);
uint32_t js_core_generation = 1;

void js_core_init(void) {
    js_core_generation++;   /* caches of strings from the last page are stale */
    memset(intern_table, 0, sizeof(intern_table));
    init_names();
    if (!scope_stack) scope_stack = (char*)malloc(JS_SCOPE_STACK);
    scope_top = 0;
    scope_allowed = 0;
    current_label = NULL;
    current_gen = NULL;
    generator_proto = NULL;
    coro_reset();
    memset(tagged_cache, 0, sizeof(tagged_cache));
    modules = NULL;
    import_map = js_undef();
    parse_as_module = 0;
    parse_module_url = NULL;
    env_pool_reset();
    js_global_env = new_env(NULL, 64, 0);
    js_global_env->pool_class = 0;
}

void js_core_reset_scope_stack(void);
void js_core_reset_scope_stack(void) {
    scope_top = 0;
    scope_allowed = 0;
    current_label = NULL;
}

/* ---- garbage collector roots (see js_gc.c) ----
 * The live part of every stack the interpreter runs on, the coroutines
 * and the scope stack. */
void js_core_gc_roots(void) {
    char here;
    char* sp = &here;
    char* js_lo = NULL;
    char* js_hi = NULL;
    js_dom_stack_bounds(&js_lo, &js_hi);
    int on_js = js_lo && sp >= js_lo && sp < js_hi;
    char* js_low = on_js ? sp : NULL;   /* lowest live address on the script stack */
#if JS_HAVE_CORO
    js_coro* cur = current_coro && current_coro->running && on_coro_stack(current_coro) ? current_coro : NULL;
    char* top = cur ? (char*)(((uintptr_t)(cur->stack + CORO_STACK_SIZE)) & ~(uintptr_t)15u)
              : on_js ? js_hi : js_rt.stack_base;
    for (js_coro* c = live_coros; c; c = c->next_live) {
        js_gc_mark_range(c, (char*)c + sizeof(*c));
        if (!c->stack || c->done) continue;
        char* c_top = (char*)(((uintptr_t)(c->stack + CORO_STACK_SIZE)) & ~(uintptr_t)15u);
        if (c->running) {
            char* resumer = (char*)c->caller_sp;
            if (js_lo && resumer >= js_lo && resumer < js_hi && (!js_low || resumer < js_low)) js_low = resumer;
            if (c != cur && c->body_sp && c->body_sp >= c->stack && c->body_sp < c_top)
                js_gc_mark_range(c->body_sp, c_top);
        } else if (c->started && c->sp) {
            js_gc_mark_range(c->sp, c_top);
        }
    }
#else
    char* top = on_js ? js_hi : js_rt.stack_base;
#endif
    if (top && top > sp) js_gc_mark_range(sp, top);
    if (js_low && !on_js) js_gc_mark_range(js_low, js_hi);
    if (scope_stack) js_gc_mark_range(scope_stack, scope_stack + scope_top);
}
