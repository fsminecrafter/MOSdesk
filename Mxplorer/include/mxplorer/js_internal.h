#ifndef MXPLORER_JS_INTERNAL_H
#define MXPLORER_JS_INTERNAL_H

/* Shared between js.c (language), js_runtime.c (built-ins) and js_dom.c
 * (browser objects). Not part of the public API. */

#include <stddef.h>
#include <stdint.h>
#include "mxplorer/dom.h"
#include "mxplorer/js.h"
#include "mxplorer/util.h"

typedef struct js_str {
    uint32_t length;
    uint32_t hash;
    char data[];
} js_str;

typedef enum { JS_UNDEF, JS_NULL, JS_BOOL, JS_NUM, JS_STR, JS_OBJ } js_type;

struct js_obj;

typedef struct {
    uint8_t type;
    union {
        double num;
        int boolean;
        js_str* str;
        struct js_obj* obj;
    } u;
} js_val;

enum {
    PROP_HIDDEN = 1,       /* not enumerable */
    PROP_ACCESSOR = 2,     /* getter/setter pair */
    PROP_READONLY = 4
};

typedef struct {
    js_str* key;
    js_val value;            /* accessor: getter object */
    struct js_obj* setter;
    uint8_t flags;
} js_prop;

typedef enum {
    CLS_OBJECT, CLS_ARRAY, CLS_FUNCTION, CLS_NATIVE, CLS_BOUND, CLS_ERROR,
    CLS_DATE, CLS_REGEXP, CLS_NODE, CLS_STYLE, CLS_CLASSLIST, CLS_DATASET,
    CLS_PROMISE, CLS_BOXED, CLS_ARGUMENTS, CLS_MAP, CLS_SET
} js_class;

struct js_func;
struct js_env;
struct js_obj;

typedef js_val (*js_native)(js_val self, js_val* args, int argc,
                            struct js_obj* callee);

typedef struct js_obj {
    uint8_t cls;
    uint8_t frozen;
    int16_t magic;              /* native function variant */
    struct js_obj* proto;
    js_prop* props;
    uint32_t prop_count, prop_capacity;
    /* arrays */
    js_val* items;
    uint32_t length, item_capacity;
    /* functions */
    struct js_func* func;
    struct js_env* env;
    js_native native;
    js_str* name;
    js_val bound_this;
    struct js_obj* bound_target;
    js_val* bound_args;
    int bound_count;
    js_val home_this;           /* arrow functions: captured this */
    struct js_obj* home_object; /* methods: for super */
    /* host data */
    mx_node_t* node;
    double number;              /* Date value, boxed primitive */
    void* data;                 /* regexp program, promise state, ... */
    js_val primitive;           /* boxed primitive value */
} js_obj;

/* ---- AST ---- */

typedef enum {
    N_PROGRAM, N_BLOCK, N_VAR, N_DECL, N_IF, N_FOR, N_FOR_IN, N_FOR_OF,
    N_WHILE, N_DO, N_RETURN, N_BREAK, N_CONTINUE, N_THROW, N_TRY, N_SWITCH,
    N_CASE, N_EXPR_STMT, N_EMPTY, N_FUNC_DECL, N_CLASS_DECL, N_LABEL,
    N_NUM, N_STR, N_TEMPLATE, N_IDENT, N_THIS, N_TRUE, N_FALSE, N_NULL,
    N_UNDEF, N_ARRAY, N_OBJECT, N_PROP, N_FUNC, N_CLASS, N_REGEX, N_UNARY,
    N_UPDATE, N_BINARY, N_LOGICAL, N_ASSIGN, N_COND, N_CALL, N_NEW,
    N_MEMBER, N_SEQ, N_SPREAD, N_ARRAY_PAT, N_OBJECT_PAT, N_ASSIGN_PAT,
    N_REST, N_SUPER, N_TAGGED, N_HOLE, N_AWAIT, N_YIELD, N_IMPORT, N_EXPORT,
    N_IMPORT_CALL, N_IMPORT_META
} js_node_type;

enum { VAR_VAR, VAR_LET, VAR_CONST };
enum { PROP_INIT, PROP_GET, PROP_SET, PROP_SPREAD, PROP_SHORTHAND };

typedef struct js_node {
    uint8_t type;
    uint8_t op;            /* operator token / var kind / prop kind */
    uint8_t flag;          /* computed, prefix, optional, static ... */
    uint8_t flag2;
    int line;
    struct js_node* a;
    struct js_node* b;
    struct js_node* c;
    struct js_node* d;
    struct js_node** list;
    uint32_t count;
    js_str* str;
    double num;
    struct js_func* func;
} js_node;

typedef struct js_func {
    js_str* name;
    js_node** params;      /* patterns */
    int param_count;
    js_node* body;
    uint8_t arrow;
    uint8_t expression_body;
    uint8_t uses_arguments;
    uint8_t has_closures;  /* contains nested functions: keep its scope */
    uint8_t is_async;
    uint8_t generator;
    uint8_t is_class_constructor;
    uint8_t derived;       /* class extends: needs super() */
    uint8_t strict;        /* "use strict" or class code */
    js_str** vars;         /* hoisted var names */
    int var_count;
    js_node** functions;   /* hoisted function declarations */
    int function_count;
    const char* source_name;
    js_node* class_fields; /* instance fields (class constructor only) */
} js_func;

/* ---- scopes ---- */

typedef struct {
    js_str* name;
    js_val value;
    uint8_t kind;
    uint8_t initialized;
} js_var;

typedef struct js_env {
    struct js_env* parent;
    js_var* vars;
    uint32_t count, capacity;
    js_val this_val;
    js_obj* function;      /* the function object running here */
    js_obj* arguments;
    js_obj* new_target;
    uint8_t is_function;
    uint8_t on_stack;
} js_env;

/* ---- runtime ---- */

typedef enum { CTL_NONE, CTL_RETURN, CTL_BREAK, CTL_CONTINUE, CTL_THROW } js_ctl;

typedef struct {
    js_ctl ctl;
    js_val value;            /* return value or exception */
    js_str* label;
    long steps;
    long step_limit;
    int depth;
    int line;
    const char* source;
    char* stack_base;
    long stack_limit;        /* bytes the interpreter may use from stack_base */
    int out_of_memory;
    int timed_out;
} js_rt_state;

extern js_rt_state js_rt;
extern mx_arena_t js_heap;
extern js_obj* js_global;
extern js_obj* js_object_proto;
extern js_obj* js_function_proto;
extern js_obj* js_array_proto;
extern js_obj* js_string_proto;
extern js_obj* js_number_proto;
extern js_obj* js_boolean_proto;
extern js_obj* js_error_proto;
extern js_obj* js_regexp_proto;
extern js_obj* js_date_proto;
extern js_obj* js_promise_proto;
extern js_obj* js_map_proto;
extern js_obj* js_set_proto;
extern js_env* js_global_env;
extern mx_document_t* js_doc;
extern mx_js_host_t js_host;

/* values */
static inline js_val js_undef(void) { js_val v; v.type = JS_UNDEF; v.u.num = 0; return v; }
static inline js_val js_null(void) { js_val v; v.type = JS_NULL; v.u.num = 0; return v; }
static inline js_val js_bool(int b) { js_val v; v.type = JS_BOOL; v.u.boolean = b != 0; return v; }
static inline js_val js_num(double n) { js_val v; v.type = JS_NUM; v.u.num = n; return v; }
static inline js_val js_str_val(js_str* s) { js_val v; v.type = JS_STR; v.u.str = s; return v; }
static inline js_val js_obj_val(js_obj* o) { js_val v; v.type = JS_OBJ; v.u.obj = o; return v; }
static inline int js_is_obj(js_val v) { return v.type == JS_OBJ; }
static inline int js_is_callable(js_val v) {
    return v.type == JS_OBJ && (v.u.obj->cls == CLS_FUNCTION ||
                                v.u.obj->cls == CLS_NATIVE ||
                                v.u.obj->cls == CLS_BOUND);
}

void* js_alloc(size_t size);
js_str* js_new_str(const char* text, size_t length);
js_str* js_cstr(const char* text);
js_str* js_intern(const char* text, size_t length);
js_str* js_intern_c(const char* text);
js_str* js_concat(js_str* a, js_str* b);
int js_str_eq(const js_str* a, const js_str* b);
int js_str_eq_c(const js_str* a, const char* text);

js_obj* js_new_object(js_obj* proto);
js_obj* js_new_array(uint32_t capacity);
js_obj* js_new_native(const char* name, js_native fn, int arity);
void js_array_push(js_obj* array, js_val value);
void js_array_set_length(js_obj* array, uint32_t length);

/* properties */
js_prop* js_find_own(js_obj* obj, js_str* key);
js_val js_get(js_val target, js_str* key);
js_val js_get_c(js_val target, const char* key);
js_val js_get_index(js_val target, js_val key);
void js_set(js_val target, js_str* key, js_val value);
void js_set_c(js_val target, const char* key, js_val value);
void js_set_index(js_val target, js_val key, js_val value);
void js_define(js_obj* obj, const char* key, js_val value, uint8_t flags);
void js_define_native(js_obj* obj, const char* key, js_native fn, int arity);
void js_define_accessor(js_obj* obj, const char* key, js_native getter,
                        js_native setter);
int js_delete(js_val target, js_str* key);
int js_has_property(js_val target, js_str* key);
js_obj* js_own_keys(js_obj* obj, int include_hidden);

/* conversions */
int js_truthy(js_val v);
double js_to_number(js_val v);
js_str* js_to_string(js_val v);
js_val js_to_primitive(js_val v, int hint_number);
js_obj* js_to_object(js_val v);
int32_t js_to_int32(js_val v);
uint32_t js_to_uint32(js_val v);
int js_strict_equals(js_val a, js_val b);
int js_loose_equals(js_val a, js_val b);
const char* js_typeof(js_val v);
js_str* js_number_to_string(double value, int radix);
double js_string_to_number(const js_str* s);
int js_array_index(js_str* key, uint32_t* index);

/* calls and errors */
js_val js_call(js_val fn, js_val self, js_val* args, int argc);
js_val js_construct(js_val fn, js_val* args, int argc);
js_val js_throw(js_val value);
js_val js_throw_error(const char* type, const char* message);
js_val js_throw_type(const char* fmt, const char* detail);
js_obj* js_make_error(const char* type, const char* message);
int js_instanceof(js_val v, js_val ctor);

/* execution */
js_node* js_parse(const char* source, size_t length, const char* name,
                  char* error, size_t error_capacity, int* error_line);
js_val js_exec_program(js_node* program);
/* Modules: parse/run `source` as an ES module at `url`; returns its
 * namespace object (or undefined after an error). */
js_val js_run_module_source(const char* source, size_t length, const char* url,
                            const char* name);
js_val js_load_module(const char* url);
/* Implemented by js_dom.c: GET a script through the host. */
int js_fetch_source(const char* url, char** body, size_t* length);
js_val js_make_function(js_func* func, js_env* env, js_val this_val);
void js_run_microtasks(void);
void js_enqueue_microtask(js_val fn, js_val arg);
/* Formats a value for the console. */
void js_inspect(js_val v, char* out, size_t capacity, int depth, int quote);
void js_report_exception(void);

/* runtime.c */
void js_runtime_init(void);
js_val js_regexp_create(js_str* pattern, js_str* flags);
js_val js_promise_resolved(js_val value, int rejected);
js_val js_promise_then(js_val promise, js_val on_ok, js_val on_err);
int js_promise_state(js_val promise, js_val* value);   /* 0 pending, 1 ok, 2 err */

/* dom.c */
void js_dom_init(void);
js_val js_wrap_node(mx_node_t* node);
js_val js_dom_get(js_obj* obj, js_str* key, int* found);
int js_dom_set(js_obj* obj, js_str* key, js_val value);
void js_dom_mutated(void);

#define JS_CHECK(v) do { if (js_rt.ctl) return js_undef(); } while (0)

#endif
