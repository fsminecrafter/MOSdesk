/* Typed arrays, ArrayBuffer, TextEncoder and TextDecoder.
 *
 * A typed array is an ordinary array object (CLS_ARRAY, so every Array
 * method works on it) with a fixed length and an element type in
 * `typed`. Every element write goes through js_typed_coerce() (js.c), so
 * a Uint8Array really wraps 256 to 0 and a Float32Array really rounds.
 *
 * Simplification: elements are stored as JS numbers, not bytes. Views of
 * the same element type share storage (new Uint8Array(buf), subarray()),
 * which covers how pages use them. A view of a different type over the
 * same ArrayBuffer gets a copy of the bytes at the time it is made, so
 * later writes through one are not seen by the other. */
#include "mxplorer/js_internal.h"
#include "string.h"

enum { TA_NONE, TA_I8, TA_U8, TA_U8C, TA_I16, TA_U16, TA_I32, TA_U32, TA_F32, TA_F64, TA_COUNT };

static const char* const ta_names[TA_COUNT] = {
    NULL, "Int8Array", "Uint8Array", "Uint8ClampedArray", "Int16Array", "Uint16Array",
    "Int32Array", "Uint32Array", "Float32Array", "Float64Array",
};
static const uint8_t ta_size[TA_COUNT] = {0, 1, 1, 1, 2, 2, 4, 4, 4, 8};

static js_obj* ta_protos[TA_COUNT];
static js_obj* ta_base_proto;
static js_obj* buffer_proto;

#define BUFFER_TAG 0x4142   /* 'AB' in js_obj.magic marks an ArrayBuffer */

#define ARG(i) ((i) < argc ? args[i] : js_undef())

double js_typed_coerce(int kind, double n) {
    switch (kind) {
    case TA_I8:  return (double)(int8_t)js_to_int32(js_num(n));
    case TA_U8:  return (double)(uint8_t)js_to_uint32(js_num(n));
    case TA_U8C: {
        if (!(n > 0)) return 0;             /* also NaN */
        if (n >= 255) return 255;
        double f = (double)(int)n, d = n - f;
        if (d > 0.5 || (d == 0.5 && ((int)f & 1))) f += 1;  /* round half to even */
        return f;
    }
    case TA_I16: return (double)(int16_t)js_to_int32(js_num(n));
    case TA_U16: return (double)(uint16_t)js_to_uint32(js_num(n));
    case TA_I32: return (double)js_to_int32(js_num(n));
    case TA_U32: return (double)js_to_uint32(js_num(n));
    case TA_F32: return (double)(float)n;
    default:     return n;
    }
}

static int is_buffer(js_val v) {
    return v.type == JS_OBJ && v.u.obj->magic == BUFFER_TAG && v.u.obj->bound_target;
}

static int is_typed(js_val v) {
    return v.type == JS_OBJ && v.u.obj->cls == CLS_ARRAY && v.u.obj->typed;
}

static js_obj* new_typed(int kind, uint32_t length) {
    js_obj* a = js_new_object(ta_protos[kind]);
    a->cls = CLS_ARRAY;
    a->typed = (uint8_t)kind;
    a->items = (js_val*)js_alloc(sizeof(js_val) * (length ? length : 1));
    for (uint32_t i = 0; i < length; i++) a->items[i] = js_num(0);
    a->length = a->item_capacity = length;
    return a;
}

/* An ArrayBuffer wraps the view whose storage it is (`bound_target`);
 * `number` is its size in bytes. */
static js_obj* new_buffer(js_obj* backing) {
    js_obj* b = js_new_object(buffer_proto);
    b->magic = BUFFER_TAG;
    b->bound_target = backing;
    b->number = (double)backing->length * ta_size[backing->typed];
    return b;
}

static js_obj* buffer_of(js_obj* view) {
    if (!view->home_object) view->home_object = new_buffer(view);
    return view->home_object;
}

/* Element i of a view as little-endian bytes. */
static void put_bytes(int kind, double v, uint8_t* out) {
    union { int8_t i8; uint8_t u8; int16_t i16; uint16_t u16; int32_t i32; uint32_t u32;
            float f32; double f64; uint8_t b[8]; } u;
    memset(&u, 0, sizeof(u));
    switch (kind) {
    case TA_I8:  u.i8 = (int8_t)v; break;
    case TA_U8: case TA_U8C: u.u8 = (uint8_t)v; break;
    case TA_I16: u.i16 = (int16_t)v; break;
    case TA_U16: u.u16 = (uint16_t)v; break;
    case TA_I32: u.i32 = (int32_t)v; break;
    case TA_U32: u.u32 = (uint32_t)v; break;
    case TA_F32: u.f32 = (float)v; break;
    default:     u.f64 = v; break;
    }
    memcpy(out, u.b, ta_size[kind]);
}

static double get_bytes(int kind, const uint8_t* in) {
    union { int8_t i8; uint8_t u8; int16_t i16; uint16_t u16; int32_t i32; uint32_t u32;
            float f32; double f64; uint8_t b[8]; } u;
    memset(&u, 0, sizeof(u));
    memcpy(u.b, in, ta_size[kind]);
    switch (kind) {
    case TA_I8:  return u.i8;
    case TA_U8: case TA_U8C: return u.u8;
    case TA_I16: return u.i16;
    case TA_U16: return u.u16;
    case TA_I32: return u.i32;
    case TA_U32: return u.u32;
    case TA_F32: return u.f32;
    default:     return u.f64;
    }
}

static uint8_t* view_bytes(js_obj* view, uint32_t* size) {
    int kind = view->typed;
    uint32_t n = view->length * ta_size[kind];
    uint8_t* bytes = (uint8_t*)js_alloc(n ? n : 1);
    for (uint32_t i = 0; i < view->length; i++)
        put_bytes(kind, js_to_number(view->items[i]), bytes + (size_t)i * ta_size[kind]);
    *size = n;
    return bytes;
}

static uint32_t to_count(js_val v) {
    double n = js_to_number(v);
    if (!(n > 0)) return 0;
    if (n > 256.0 * 1024 * 1024) n = 256.0 * 1024 * 1024;
    return (uint32_t)n;
}

static js_val array_from(js_val source) {
    js_val from = js_get_c(js_get_c(js_obj_val(js_global), "Array"), "from");
    return js_call(from, js_undef(), &source, 1);
}

static js_obj* typed_from_values(int kind, js_val source) {
    js_val list = is_typed(source) || (source.type == JS_OBJ && source.u.obj->cls == CLS_ARRAY)
                      ? source : array_from(source);
    if (list.type != JS_OBJ) return new_typed(kind, 0);
    js_obj* l = list.u.obj;
    js_obj* a = new_typed(kind, l->length);
    for (uint32_t i = 0; i < l->length; i++)
        a->items[i] = js_num(js_typed_coerce(kind, js_to_number(l->items[i])));
    return a;
}

static js_val typed_ctor(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)self;
    int kind = callee->magic;
    js_val a0 = ARG(0);
    if (is_buffer(a0)) {
        js_obj* buf = a0.u.obj;
        js_obj* backing = buf->bound_target;
        uint32_t bytes = (uint32_t)buf->number;
        uint32_t offset = to_count(ARG(1));
        uint32_t size = ta_size[kind];
        if (offset > bytes || offset % size)
            return js_throw_error("RangeError", "Start offset is outside the buffer or misaligned");
        uint32_t length = ARG(2).type == JS_UNDEF ? (bytes - offset) / size : to_count(ARG(2));
        if ((uint64_t)offset + (uint64_t)length * size > bytes)
            return js_throw_error("RangeError", "Invalid typed array length");
        js_obj* view;
        if (backing->typed == kind) {
            /* same element type: share the storage */
            view = js_new_object(ta_protos[kind]);
            view->cls = CLS_ARRAY;
            view->typed = (uint8_t)kind;
            view->items = backing->items + offset / size;
            view->length = view->item_capacity = length;
        } else {
            uint32_t have;
            uint8_t* raw = view_bytes(backing, &have);
            view = new_typed(kind, length);
            for (uint32_t i = 0; i < length; i++)
                view->items[i] = js_num(get_bytes(kind, raw + offset + (size_t)i * size));
        }
        view->home_object = buf;
        view->number = offset;
        return js_obj_val(view);
    }
    if (a0.type != JS_OBJ) return js_obj_val(new_typed(kind, to_count(a0)));
    return js_obj_val(typed_from_values(kind, a0));
}

static js_val typed_static_from(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)self;
    js_obj* a = typed_from_values(callee->magic, ARG(0));
    js_val map = ARG(1);
    if (js_is_callable(map))
        for (uint32_t i = 0; i < a->length && !js_rt.ctl; i++) {
            js_val call_args[2] = {a->items[i], js_num(i)};
            js_val r = js_call(map, ARG(2), call_args, 2);
            a->items[i] = js_num(js_typed_coerce(a->typed, js_to_number(r)));
        }
    return js_obj_val(a);
}

static js_val typed_static_of(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)self;
    js_obj* a = new_typed(callee->magic, (uint32_t)argc);
    for (int i = 0; i < argc; i++)
        a->items[i] = js_num(js_typed_coerce(a->typed, js_to_number(args[i])));
    return js_obj_val(a);
}

static js_obj* this_typed(js_val self) {
    if (!is_typed(self)) {
        js_throw_error("TypeError", "this is not a typed array");
        return NULL;
    }
    return self.u.obj;
}

static void clamp_range(js_obj* a, js_val* args, int argc, int first, uint32_t* begin, uint32_t* end) {
    double len = a->length;
    double b = ARG(first).type == JS_UNDEF ? 0 : js_to_number(ARG(first));
    double e = ARG(first + 1).type == JS_UNDEF ? len : js_to_number(ARG(first + 1));
    if (b != b) b = 0;
    if (e != e) e = 0;
    if (b < 0) b = b + len < 0 ? 0 : b + len;
    if (e < 0) e = e + len < 0 ? 0 : e + len;
    if (b > len) b = len;
    if (e > len) e = len;
    if (e < b) e = b;
    *begin = (uint32_t)b;
    *end = (uint32_t)e;
}

/* magic: 0 set, 1 subarray, 2 slice, 3 fill */
static js_val typed_method(js_val self, js_val* args, int argc, js_obj* callee) {
    js_obj* a = this_typed(self);
    if (!a) return js_undef();
    int kind = a->typed;
    switch (callee->magic) {
    case 0: {
        uint32_t offset = to_count(ARG(1));
        js_val src = ARG(0);
        js_obj* list = NULL;
        if (src.type == JS_OBJ && src.u.obj->cls == CLS_ARRAY) list = src.u.obj;
        else {
            js_val l = array_from(src);
            if (l.type == JS_OBJ) list = l.u.obj;
        }
        if (!list) return js_undef();
        if ((uint64_t)offset + list->length > a->length)
            return js_throw_error("RangeError", "offset is out of bounds");
        /* copy through a temporary in case both share storage */
        js_val* tmp = (js_val*)js_alloc(sizeof(js_val) * (list->length ? list->length : 1));
        for (uint32_t i = 0; i < list->length; i++)
            tmp[i] = js_num(js_typed_coerce(kind, js_to_number(list->items[i])));
        for (uint32_t i = 0; i < list->length; i++) a->items[offset + i] = tmp[i];
        return js_undef();
    }
    case 1: {
        uint32_t b, e;
        clamp_range(a, args, argc, 0, &b, &e);
        js_obj* view = js_new_object(ta_protos[kind]);
        view->cls = CLS_ARRAY;
        view->typed = (uint8_t)kind;
        view->items = a->items + b;
        view->length = view->item_capacity = e - b;
        view->home_object = buffer_of(a);
        view->number = a->number + (double)b * ta_size[kind];
        return js_obj_val(view);
    }
    case 2: {
        uint32_t b, e;
        clamp_range(a, args, argc, 0, &b, &e);
        js_obj* copy = new_typed(kind, e - b);
        for (uint32_t i = b; i < e; i++) copy->items[i - b] = a->items[i];
        return js_obj_val(copy);
    }
    case 3: {
        uint32_t b, e;
        clamp_range(a, args, argc, 1, &b, &e);
        js_val v = js_num(js_typed_coerce(kind, js_to_number(ARG(0))));
        for (uint32_t i = b; i < e; i++) a->items[i] = v;
        return self;
    }
    }
    return js_undef();
}

/* magic: 0 buffer, 1 byteLength, 2 byteOffset, 3 BYTES_PER_ELEMENT */
static js_val typed_getter(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)args; (void)argc;
    if (!is_typed(self)) return js_undef();
    js_obj* a = self.u.obj;
    switch (callee->magic) {
    case 0: return js_obj_val(buffer_of(a));
    case 1: return js_num((double)a->length * ta_size[a->typed]);
    case 2: return js_num(a->number);
    default: return js_num(ta_size[a->typed]);
    }
}

static js_val buffer_ctor(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)self; (void)callee;
    return js_obj_val(new_buffer(new_typed(TA_U8, to_count(ARG(0)))));
}

static js_val buffer_byte_length(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)args; (void)argc; (void)callee;
    return is_buffer(self) ? js_num(self.u.obj->number) : js_undef();
}

static js_val buffer_slice(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)callee;
    if (!is_buffer(self)) return js_throw_error("TypeError", "this is not an ArrayBuffer");
    uint32_t size;
    uint8_t* raw = view_bytes(self.u.obj->bound_target, &size);
    js_obj tmp;
    memset(&tmp, 0, sizeof(tmp));
    tmp.length = size;
    uint32_t b, e;
    clamp_range(&tmp, args, argc, 0, &b, &e);
    js_obj* copy = new_typed(TA_U8, e - b);
    for (uint32_t i = b; i < e; i++) copy->items[i - b] = js_num(raw[i]);
    return js_obj_val(new_buffer(copy));
}

static js_val buffer_is_view(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)self; (void)callee;
    return js_bool(is_typed(ARG(0)));
}

/* TextEncoder / TextDecoder: strings are stored as UTF-8. */
static js_val text_encoder_ctor(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)self; (void)args; (void)argc;
    js_val proto = js_get_c(js_obj_val(callee), "prototype");
    js_obj* o = js_new_object(proto.type == JS_OBJ ? proto.u.obj : js_object_proto);
    js_define(o, "encoding", js_str_val(js_intern_c("utf-8")), 0);
    return js_obj_val(o);
}

static js_val text_encode(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)self; (void)callee;
    js_str* s = ARG(0).type == JS_UNDEF ? js_intern_c("") : js_to_string(ARG(0));
    js_obj* a = new_typed(TA_U8, s->length);
    for (uint32_t i = 0; i < s->length; i++) a->items[i] = js_num((uint8_t)s->data[i]);
    return js_obj_val(a);
}

static js_val text_decode(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)self; (void)callee;
    js_val src = ARG(0);
    js_obj* view = NULL;
    if (is_buffer(src)) view = src.u.obj->bound_target;
    else if (is_typed(src)) view = src.u.obj;
    if (!view) return js_str_val(js_intern_c(""));
    uint32_t size;
    uint8_t* raw = view->typed == TA_U8 || view->typed == TA_I8 || view->typed == TA_U8C
                       ? NULL : view_bytes(view, &size);
    if (!raw) {
        size = view->length;
        raw = (uint8_t*)js_alloc(size ? size : 1);
        for (uint32_t i = 0; i < size; i++) raw[i] = (uint8_t)(int)js_to_number(view->items[i]);
    }
    uint32_t start = size >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF ? 3 : 0;
    return js_str_val(js_new_str((const char*)raw + start, size - start));
}

static void method(js_obj* o, const char* name, js_native fn, int magic) {
    js_obj* f = js_new_native(name, fn, 1);
    f->magic = (int16_t)magic;
    js_define(o, name, js_obj_val(f), PROP_HIDDEN);
}

static void accessor(js_obj* o, const char* name, js_native fn, int magic) {
    js_define_accessor(o, name, fn, NULL);
    js_prop* p = js_find_own(o, js_intern_c(name));
    if (p && p->value.type == JS_OBJ) p->value.u.obj->magic = (int16_t)magic;
}

static js_obj* global_ctor(const char* name, js_native fn, int magic, js_obj* proto) {
    js_obj* ctor = js_new_native(name, fn, 1);
    ctor->magic = (int16_t)magic;
    js_define(ctor, "prototype", js_obj_val(proto), PROP_HIDDEN);
    js_define(proto, "constructor", js_obj_val(ctor), PROP_HIDDEN);
    js_define(js_global, name, js_obj_val(ctor), PROP_HIDDEN);
    return ctor;
}

void js_typed_init(void) {
    ta_base_proto = js_new_object(js_array_proto);
    method(ta_base_proto, "set", typed_method, 0);
    method(ta_base_proto, "subarray", typed_method, 1);
    method(ta_base_proto, "slice", typed_method, 2);
    method(ta_base_proto, "fill", typed_method, 3);
    accessor(ta_base_proto, "buffer", typed_getter, 0);
    accessor(ta_base_proto, "byteLength", typed_getter, 1);
    accessor(ta_base_proto, "byteOffset", typed_getter, 2);
    accessor(ta_base_proto, "BYTES_PER_ELEMENT", typed_getter, 3);
    for (int k = 1; k < TA_COUNT; k++) {
        ta_protos[k] = js_new_object(ta_base_proto);
        js_define(ta_protos[k], "@@toStringTag", js_str_val(js_intern_c(ta_names[k])), PROP_HIDDEN);
        js_obj* ctor = global_ctor(ta_names[k], typed_ctor, k, ta_protos[k]);
        js_define(ctor, "BYTES_PER_ELEMENT", js_num(ta_size[k]), PROP_HIDDEN);
        js_obj* from = js_new_native("from", typed_static_from, 1);
        from->magic = (int16_t)k;
        js_define(ctor, "from", js_obj_val(from), PROP_HIDDEN);
        js_obj* of = js_new_native("of", typed_static_of, 0);
        of->magic = (int16_t)k;
        js_define(ctor, "of", js_obj_val(of), PROP_HIDDEN);
    }

    buffer_proto = js_new_object(js_object_proto);
    js_obj* buffer = global_ctor("ArrayBuffer", buffer_ctor, 0, buffer_proto);
    method(buffer_proto, "slice", buffer_slice, 0);
    js_define_accessor(buffer_proto, "byteLength", buffer_byte_length, NULL);
    js_define(buffer_proto, "@@toStringTag", js_str_val(js_intern_c("ArrayBuffer")), PROP_HIDDEN);
    method(buffer, "isView", buffer_is_view, 0);

    js_obj* encoder_proto = js_new_object(js_object_proto);
    global_ctor("TextEncoder", text_encoder_ctor, 0, encoder_proto);
    method(encoder_proto, "encode", text_encode, 0);
    js_obj* decoder_proto = js_new_object(js_object_proto);
    global_ctor("TextDecoder", text_encoder_ctor, 0, decoder_proto);
    method(decoder_proto, "decode", text_decode, 0);
}
