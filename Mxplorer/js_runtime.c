/* Mxplorer JavaScript built-ins: Object, Function, Array, String, Number,
 * Boolean, Math, JSON, RegExp, Date, Promise, Map, Set, Symbol, errors and
 * console. */

#include "mxplorer/js_internal.h"
#include "mxplorer/devlog.h"
#include "mxplorer/gfx.h"
#include "string.h"
#include "stdlib.h"

#ifndef MX_HOST
#include "minimalos.h"
#include "syscall.h"
#else
#include <time.h>
#endif

#define ARG(i) ((i) < argc ? args[i] : js_undef())
#define THIS_OBJ (self.type == JS_OBJ ? self.u.obj : NULL)

static double nan_value(void) {
    double zero = 0;
    return zero / zero;
}

static double inf_value(void) {
    double zero = 0;
    return 1 / zero;
}

static int hexval_re(char c);
js_str* S_then_name(void);

static int is_nan(double x) { return x != x; }
static int is_inf(double x) { return x == inf_value() || x == -inf_value(); }

/* ====================================================================== */
/* Math                                                                   */
/* ====================================================================== */

static double m_floor(double x) {
    if (is_nan(x) || is_inf(x) || x > 4.5e15 || x < -4.5e15) return x;
    double t = (double)(long long)x;
    return t > x ? t - 1 : t;
}

static double m_ceil(double x) {
    double f = m_floor(x);
    return f == x ? x : f + 1;
}

static double m_trunc(double x) {
    return x < 0 ? -m_floor(-x) : m_floor(x);
}

static double m_fabs(double x) { return x < 0 ? -x : x; }

#define M_PI_D 3.14159265358979323846
#define M_LN2_D 0.69314718055994530942

static double m_exp(double x) {
    if (is_nan(x)) return x;
    if (x > 709) return inf_value();
    if (x < -745) return 0;
    /* x = k ln2 + r */
    double k = m_floor(x / M_LN2_D + 0.5);
    double r = x - k * M_LN2_D;
    double term = 1, sum = 1;
    for (int i = 1; i < 30; i++) {
        term *= r / i;
        sum += term;
        if (m_fabs(term) < 1e-17 * m_fabs(sum)) break;
    }
    long long ki = (long long)k;
    while (ki > 0) { sum *= 2; ki--; }
    while (ki < 0) { sum /= 2; ki++; }
    return sum;
}

static double m_log(double x) {
    if (is_nan(x) || x < 0) return nan_value();
    if (x == 0) return -inf_value();
    if (is_inf(x)) return x;
    int k = 0;
    while (x > 2) { x /= 2; k++; }
    while (x < 1) { x *= 2; k--; }
    /* ln(x) = 2 atanh((x-1)/(x+1)) */
    double y = (x - 1) / (x + 1), y2 = y * y, term = y, sum = 0;
    for (int i = 1; i < 60; i += 2) {
        sum += term / i;
        term *= y2;
        if (term < 1e-18) break;
    }
    return 2 * sum + k * M_LN2_D;
}

double js_math_pow(double base, double exponent);
double js_math_pow(double base, double exponent) {
    if (exponent == 0) return 1;
    if (is_nan(base) || is_nan(exponent)) return nan_value();
    if (exponent == (long long)exponent && m_fabs(exponent) < 1e6) {
        long long e = (long long)exponent;
        int negative = e < 0;
        if (negative) e = -e;
        double result = 1, b = base;
        while (e) {
            if (e & 1) result *= b;
            b *= b;
            e >>= 1;
        }
        return negative ? 1 / result : result;
    }
    if (base < 0) return nan_value();
    if (base == 0) return exponent > 0 ? 0 : inf_value();
    return m_exp(exponent * m_log(base));
}

/* sin/cos on [-pi/4, pi/4] after reduction by quadrant, in long double */
static long double series_sin(long double x) {
    long double x2 = x * x, term = x, sum = x;
    for (int i = 1; i < 14; i++) {
        term *= -x2 / ((2 * i) * (2 * i + 1));
        sum += term;
    }
    return sum;
}

static long double series_cos(long double x) {
    long double x2 = x * x, term = 1, sum = 1;
    for (int i = 1; i < 14; i++) {
        term *= -x2 / ((2 * i - 1) * (2 * i));
        sum += term;
    }
    return sum;
}

static double trig(double x, int cosine) {
    if (is_nan(x) || is_inf(x)) return nan_value();
    const long double half_pi = 1.570796326794896619231321691639751442L;
    long double q = (long double)x / half_pi;
    long double k = q < 0 ? -(long double)(long long)(-q + 0.5L) : (long double)(long long)(q + 0.5L);
    long double r = (long double)x - k * half_pi;
    long long quadrant = ((long long)k + (cosine ? 1 : 0)) & 3;
    long double result;
    switch (quadrant) {
    case 0: result = series_sin(r); break;
    case 1: result = series_cos(r); break;
    case 2: result = -series_sin(r); break;
    default: result = -series_cos(r); break;
    }
    return (double)result;
}

static double m_sin(double x) { return trig(x, 0); }
static double m_cos(double x) { return trig(x, 1); }

static double m_atan(double x) {
    if (is_nan(x)) return x;
    int negative = x < 0;
    if (negative) x = -x;
    int invert = x > 1;
    if (invert) x = 1 / x;
    /* reduce further: atan(x) = 2 atan(x / (1 + sqrt(1 + x^2))) */
    double y = x / (1 + mx_sqrt(1 + x * x));
    double y2 = y * y, term = y, sum = 0;
    for (int i = 1; i < 80; i += 2) {
        sum += term / i;
        term *= -y2;
        if (m_fabs(term) < 1e-18) break;
    }
    sum *= 2;
    if (invert) sum = M_PI_D / 2 - sum;
    return negative ? -sum : sum;
}

static double m_atan2(double y, double x) {
    if (x > 0) return m_atan(y / x);
    if (x < 0) return y >= 0 ? m_atan(y / x) + M_PI_D : m_atan(y / x) - M_PI_D;
    if (y > 0) return M_PI_D / 2;
    if (y < 0) return -M_PI_D / 2;
    return 0;
}

static uint64_t rng_state = 0x9E3779B97F4A7C15ull;

static double m_random(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return (double)(rng_state >> 11) / 9007199254740992.0;
}

#define MATH1(name, expr) \
    static js_val math_##name(js_val self, js_val* args, int argc, js_obj* callee) { \
        (void)self; (void)callee; double x = js_to_number(ARG(0)); return js_num(expr); }

MATH1(floor, m_floor(x))
MATH1(ceil, m_ceil(x))
MATH1(trunc, m_trunc(x))
MATH1(abs, m_fabs(x))
MATH1(sqrt, x < 0 ? nan_value() : mx_sqrt(x))
MATH1(cbrt, x < 0 ? -js_math_pow(-x, 1.0 / 3) : js_math_pow(x, 1.0 / 3))
MATH1(exp, m_exp(x))
MATH1(log, m_log(x))
MATH1(log2, m_log(x) / M_LN2_D)
MATH1(log10, m_log(x) / 2.302585092994046)
MATH1(log1p, m_log(1 + x))
MATH1(expm1, m_exp(x) - 1)
MATH1(sin, m_sin(x))
MATH1(cos, m_cos(x))
MATH1(tan, m_sin(x) / m_cos(x))
MATH1(atan, m_atan(x))
MATH1(asin, m_atan2(x, mx_sqrt(1 - x * x)))
MATH1(acos, m_atan2(mx_sqrt(1 - x * x), x))
MATH1(sinh, (m_exp(x) - m_exp(-x)) / 2)
MATH1(cosh, (m_exp(x) + m_exp(-x)) / 2)
MATH1(tanh, (m_exp(2 * x) - 1) / (m_exp(2 * x) + 1))
MATH1(sign, is_nan(x) ? x : x > 0 ? 1 : x < 0 ? -1 : x)
MATH1(fround, x)
MATH1(clz32, (double)({ uint32_t v = (uint32_t)js_to_int32(js_num(x)); int n = 0; if (!v) n = 32; else while (!(v & 0x80000000u)) { v <<= 1; n++; } n; }))

static js_val math_round(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)self; (void)callee;
    double x = js_to_number(ARG(0));
    if (is_nan(x) || is_inf(x)) return js_num(x);
    return js_num(m_floor(x + 0.5));
}

static js_val math_atan2(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)self; (void)callee;
    return js_num(m_atan2(js_to_number(ARG(0)), js_to_number(ARG(1))));
}

static js_val math_pow(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)self; (void)callee;
    return js_num(js_math_pow(js_to_number(ARG(0)), js_to_number(ARG(1))));
}

static js_val math_minmax(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)self;
    int is_max = callee->name->data[1] == 'a';
    double best = is_max ? -inf_value() : inf_value();
    for (int i = 0; i < argc; i++) {
        double v = js_to_number(args[i]);
        if (is_nan(v)) return js_num(v);
        if (is_max ? v > best : v < best) best = v;
    }
    return js_num(best);
}

static js_val math_hypot(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)self; (void)callee;
    double sum = 0;
    for (int i = 0; i < argc; i++) {
        double v = js_to_number(args[i]);
        sum += v * v;
    }
    return js_num(mx_sqrt(sum));
}

static js_val math_random(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)self; (void)args; (void)argc; (void)callee;
    return js_num(m_random());
}

/* ====================================================================== */
/* Numbers                                                                */
/* ====================================================================== */

static double pow10_int(int e) {
    double result = 1, base = 10;
    int negative = e < 0;
    if (negative) e = -e;
    while (e) {
        if (e & 1) result *= base;
        base *= base;
        e >>= 1;
    }
    return negative ? 1 / result : result;
}

double js_string_to_number(const js_str* s) {
    const char* p = s->data;
    const char* end = s->data + s->length;
    while (p < end && (mx_is_space(*p) || *p == '\v')) p++;
    while (end > p && (mx_is_space(end[-1]) || end[-1] == '\v')) end--;
    if (p == end) return 0;
    int negative = 0;
    if (*p == '+' || *p == '-') negative = *p++ == '-';
    if (end - p == 8 && !strncmp(p, "Infinity", 8)) return negative ? -inf_value() : inf_value();
    if (end - p > 2 && p[0] == '0' && (p[1] | 0x20) >= 'b' && !negative) {
        int base = (p[1] | 0x20) == 'x' ? 16 : (p[1] | 0x20) == 'o' ? 8 : (p[1] | 0x20) == 'b' ? 2 : 0;
        if (base) {
            double v = 0;
            for (p += 2; p < end; p++) {
                int d = *p >= '0' && *p <= '9' ? *p - '0' : (*p | 0x20) >= 'a' && (*p | 0x20) <= 'z'
                                                                 ? (*p | 0x20) - 'a' + 10 : 99;
                if (d >= base) return nan_value();
                v = v * base + d;
            }
            return v;
        }
    }
    uint64_t mantissa = 0;
    int digits = 0, exponent = 0, any = 0;
    while (p < end && *p >= '0' && *p <= '9') {
        any = 1;
        if (digits < 19) {
            mantissa = mantissa * 10 + (uint64_t)(*p - '0');
            if (mantissa) digits++;
        } else {
            exponent++;
        }
        p++;
    }
    if (p < end && *p == '.') {
        p++;
        while (p < end && *p >= '0' && *p <= '9') {
            any = 1;
            if (digits < 19) {
                mantissa = mantissa * 10 + (uint64_t)(*p - '0');
                if (mantissa) digits++;
                exponent--;
            }
            p++;
        }
    }
    if (!any) return nan_value();
    if (p < end && (*p == 'e' || *p == 'E')) {
        p++;
        int exp_negative = 0, e = 0, exp_any = 0;
        if (p < end && (*p == '+' || *p == '-')) exp_negative = *p++ == '-';
        while (p < end && *p >= '0' && *p <= '9') {
            if (e < 10000) e = e * 10 + (*p - '0');
            p++;
            exp_any = 1;
        }
        if (!exp_any) return nan_value();
        exponent += exp_negative ? -e : e;
    }
    if (p != end) return nan_value();
    double value = (double)mantissa;
    if (exponent < -300) {
        value *= pow10_int(-300);
        exponent += 300;
    }
    value = exponent < 0 ? value / pow10_int(-exponent) : value * pow10_int(exponent);
    return negative ? -value : value;
}

/* Formats `value` with `precision` significant digits into digits/exponent.
 * Uses long double (x87, 64-bit mantissa) so 17-digit output rounds right. */
static int format_digits(double value, int precision, char* digits, int* exponent) {
    if (value == 0) {
        digits[0] = '0';
        digits[1] = '\0';
        *exponent = 0;
        return 1;
    }
    int e = 0;
    long double v = value;
    while (v >= 10) { v /= 10; e++; }
    while (v < 1) { v *= 10; e--; }
    long double scale = 1;
    int shift = precision - 1 - e;
    int magnitude = shift < 0 ? -shift : shift;
    long double base = 10;
    while (magnitude) {
        if (magnitude & 1) scale *= base;
        base *= base;
        magnitude >>= 1;
    }
    long double scaled = shift >= 0 ? (long double)value * scale : (long double)value / scale;
    uint64_t n = (uint64_t)(scaled + 0.5L);
    uint64_t limit = 1;
    for (int i = 0; i < precision; i++) limit *= 10;
    if (n >= limit) {
        n /= 10;
        e++;
    }
    char tmp[24];
    int count = 0;
    do {
        tmp[count++] = (char)('0' + n % 10);
        n /= 10;
    } while (n && count < 22);
    while (count < precision) tmp[count++] = '0';
    for (int i = 0; i < count; i++) digits[i] = tmp[count - 1 - i];
    digits[count] = '\0';
    while (count > 1 && digits[count - 1] == '0') digits[--count] = '\0';
    *exponent = e;
    return count;
}

js_str* js_number_to_string(double value, int radix) {
    char out[64];
    if (is_nan(value)) return js_intern_c("NaN");
    if (value == inf_value()) return js_intern_c("Infinity");
    if (value == -inf_value()) return js_intern_c("-Infinity");
    if (value == 0) return js_intern_c("0");
    int negative = value < 0;
    double a = negative ? -value : value;
    size_t used = 0;
    if (negative) out[used++] = '-';
    if (radix != 10) {
        double ip = m_floor(a);
        char tmp[80];
        int count = 0;
        do {
            double q = m_floor(ip / radix);
            int d = (int)(ip - q * radix);
            tmp[count++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
            ip = q;
        } while (ip > 0 && count < 70);
        while (count) out[used++] = tmp[--count];
        double frac = a - m_floor(a);
        if (frac > 0) {
            out[used++] = '.';
            for (int i = 0; i < 20 && frac > 0 && used + 2 < sizeof(out); i++) {
                frac *= radix;
                int d = (int)frac;
                out[used++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
                frac -= d;
            }
        }
        out[used] = '\0';
        return js_new_str(out, used);
    }
    if (a < 1e21 && a == m_floor(a) && a < 9.0e18) {
        uint64_t n = (uint64_t)a;
        char tmp[24];
        int count = 0;
        do {
            tmp[count++] = (char)('0' + n % 10);
            n /= 10;
        } while (n);
        while (count) out[used++] = tmp[--count];
        out[used] = '\0';
        return js_new_str(out, used);
    }
    char digits[24];
    int exponent = 0, count = 0;
    for (int precision = 1; precision <= 17; precision++) {
        count = format_digits(a, precision, digits, &exponent);
        /* check the round trip */
        char probe[48];
        size_t pn = 0;
        for (int i = 0; i < count; i++) {
            probe[pn++] = digits[i];
            if (i == 0 && count > 1) probe[pn++] = '.';
        }
        probe[pn++] = 'e';
        int e = exponent;
        if (e < 0) {
            probe[pn++] = '-';
            e = -e;
        }
        char et[8];
        int ec = 0;
        do {
            et[ec++] = (char)('0' + e % 10);
            e /= 10;
        } while (e);
        while (ec) probe[pn++] = et[--ec];
        probe[pn] = '\0';
        js_str probe_str_header;
        (void)probe_str_header;
        js_str* ps = js_new_str(probe, pn);
        if (js_string_to_number(ps) == a) break;
    }
    if (exponent >= 21 || exponent <= -7) {
        out[used++] = digits[0];
        if (count > 1) {
            out[used++] = '.';
            for (int i = 1; i < count; i++) out[used++] = digits[i];
        }
        out[used++] = 'e';
        out[used++] = exponent < 0 ? '-' : '+';
        int e = exponent < 0 ? -exponent : exponent;
        char et[8];
        int ec = 0;
        do {
            et[ec++] = (char)('0' + e % 10);
            e /= 10;
        } while (e);
        while (ec) out[used++] = et[--ec];
    } else if (exponent < 0) {
        out[used++] = '0';
        out[used++] = '.';
        for (int i = 0; i < -exponent - 1; i++) out[used++] = '0';
        for (int i = 0; i < count; i++) out[used++] = digits[i];
    } else {
        for (int i = 0; i <= exponent || i < count; i++) {
            if (i == exponent + 1) out[used++] = '.';
            out[used++] = i < count ? digits[i] : '0';
        }
    }
    out[used] = '\0';
    return js_new_str(out, used);
}

static js_str* format_fixed(double value, int decimals) {
    char out[400];
    size_t used = 0;
    if (is_nan(value)) return js_intern_c("NaN");
    if (value < 0) {
        out[used++] = '-';
        value = -value;
    }
    if (value >= 1e21) return js_number_to_string(value, 10);
    double scale = pow10_int(decimals);
    double rounded = m_floor(value * scale + 0.5);
    double ip = m_floor(rounded / scale);
    double frac = rounded - ip * scale;
    js_str* integer = js_number_to_string(ip, 10);
    memcpy(out + used, integer->data, integer->length);
    used += integer->length;
    if (decimals > 0) {
        out[used++] = '.';
        char tmp[120];
        for (int i = decimals - 1; i >= 0; i--) {
            double q = m_floor(frac / 10);
            tmp[i] = (char)('0' + (int)(frac - q * 10));
            frac = q;
        }
        memcpy(out + used, tmp, (size_t)decimals);
        used += (size_t)decimals;
    }
    if (out[0] == '-' && used == 2 && out[1] == '0') return js_intern_c("0");
    return js_new_str(out, used);
}

/* ====================================================================== */
/* String helpers                                                         */
/* ====================================================================== */

static js_str* this_string(js_val self) {
    if (self.type == JS_OBJ && self.u.obj->cls == CLS_BOXED && self.u.obj->primitive.type == JS_STR)
        return self.u.obj->primitive.u.str;
    return js_to_string(self);
}

static int str_is_ascii(const js_str* s) {
    for (uint32_t i = 0; i < s->length; i++)
        if ((unsigned char)s->data[i] >= 0x80) return 0;
    return 1;
}

static uint32_t cp_length(const js_str* s) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < s->length; i++)
        if (((unsigned char)s->data[i] & 0xC0) != 0x80) n++;
    return n;
}

/* code-point index -> byte offset (clamped) */
static uint32_t cp_to_byte(const js_str* s, uint32_t index, int ascii) {
    if (ascii) return index > s->length ? s->length : index;
    uint32_t cp = 0, i = 0;
    while (i < s->length && cp < index) {
        i++;
        while (i < s->length && ((unsigned char)s->data[i] & 0xC0) == 0x80) i++;
        cp++;
    }
    return i;
}

static uint32_t byte_to_cp(const js_str* s, uint32_t offset, int ascii) {
    if (ascii) return offset;
    uint32_t cp = 0;
    for (uint32_t i = 0; i < offset && i < s->length; i++)
        if (((unsigned char)s->data[i] & 0xC0) != 0x80) cp++;
    return cp;
}

static long relative_index(js_val v, long length, long fallback) {
    if (v.type == JS_UNDEF) return fallback;
    double n = js_to_number(v);
    if (is_nan(n)) n = 0;
    if (n < 0) n += length;
    if (n < 0) n = 0;
    if (n > length) n = length;
    return (long)m_trunc(n);
}

static long find_bytes(const js_str* hay, const js_str* needle, uint32_t from) {
    if (needle->length == 0) return from <= hay->length ? (long)from : (long)hay->length;
    if (needle->length > hay->length) return -1;
    for (uint32_t i = from; i + needle->length <= hay->length; i++)
        if (hay->data[i] == needle->data[0] && !memcmp(hay->data + i, needle->data, needle->length))
            return (long)i;
    return -1;
}

static uint32_t decode_utf8(const char* s, uint32_t length, uint32_t* i) {
    unsigned char c = (unsigned char)s[*i];
    uint32_t cp = c;
    int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    if (extra) cp = c & (0x3F >> extra);
    (*i)++;
    while (extra-- && *i < length) cp = (cp << 6) | ((unsigned char)s[(*i)++] & 0x3F);
    return cp;
}

/* ====================================================================== */
/* Regular expressions                                                    */
/* ====================================================================== */

enum {
    RE_CHAR, RE_ANY, RE_CLASS, RE_SPLIT, RE_JMP, RE_SAVE, RE_MATCH, RE_BOL,
    RE_EOL, RE_WORDB, RE_NWORDB, RE_LOOK, RE_LOOKEND, RE_BACKREF, RE_REPEAT
};

typedef struct {
    uint32_t lo, hi;
} re_range;

typedef struct {
    re_range* ranges;
    int count;
    uint8_t negated;
} re_class;

typedef struct {
    uint8_t op;
    uint8_t negated;     /* LOOK: negative; REPEAT: greedy */
    int x, y;            /* jump targets; REPEAT: min, max */
    uint32_t ch;         /* CHAR / CLASS index / SAVE slot / REPEAT atom pc */
} re_inst;

typedef struct {
    re_inst* code;
    int count, capacity;
    re_class* classes;
    int class_count, class_capacity;
    int groups;
    int icase, multiline, dotall, global, sticky;
    js_str* names[10];
} re_prog;

typedef enum { A_CHAR, A_ANY, A_CLASS, A_CAT, A_ALT, A_REPEAT, A_GROUP, A_BOL,
               A_EOL, A_WORDB, A_NWORDB, A_LOOK, A_BACKREF, A_EMPTY } re_ast_kind;

typedef struct re_ast {
    uint8_t kind;
    uint8_t flag;        /* greedy / negated / capture */
    int min, max;
    uint32_t value;
    struct re_ast** children;
    int count;
} re_ast;

typedef struct {
    const char* p;
    const char* end;
    re_prog* prog;
    int error;
} re_parser;

static re_ast* re_node(int kind) {
    re_ast* n = (re_ast*)js_alloc(sizeof(re_ast));
    memset(n, 0, sizeof(*n));
    n->kind = (uint8_t)kind;
    return n;
}

static void re_add_child(re_ast* parent, re_ast* child) {
    re_ast** list = (re_ast**)js_alloc(sizeof(re_ast*) * (size_t)(parent->count + 1));
    if (parent->count) memcpy(list, parent->children, sizeof(re_ast*) * (size_t)parent->count);
    list[parent->count++] = child;
    parent->children = list;
}

static int re_add_class(re_prog* prog, re_range* ranges, int count, int negated) {
    if (prog->class_count == prog->class_capacity) {
        int capacity = prog->class_capacity ? prog->class_capacity * 2 : 8;
        re_class* classes = (re_class*)js_alloc(sizeof(re_class) * (size_t)capacity);
        if (prog->class_count) memcpy(classes, prog->classes, sizeof(re_class) * (size_t)prog->class_count);
        prog->classes = classes;
        prog->class_capacity = capacity;
    }
    re_class* c = &prog->classes[prog->class_count];
    c->ranges = (re_range*)js_alloc(sizeof(re_range) * (size_t)(count ? count : 1));
    if (count) memcpy(c->ranges, ranges, sizeof(re_range) * (size_t)count);
    c->count = count;
    c->negated = (uint8_t)negated;
    return prog->class_count++;
}

static int class_escape(char c, re_range* out, int* count, int capacity) {
    static const re_range digits[] = {{'0', '9'}};
    static const re_range word[] = {{'0', '9'}, {'A', 'Z'}, {'_', '_'}, {'a', 'z'}};
    static const re_range space[] = {{' ', ' '}, {'\t', '\r'}, {0xA0, 0xA0}, {0xFEFF, 0xFEFF},
                                     {0x2000, 0x200A}, {0x2028, 0x2029}, {0x3000, 0x3000}};
    static const re_range not_digits[] = {{0, '0' - 1}, {'9' + 1, 0x10FFFF}};
    static const re_range not_word[] = {{0, '0' - 1}, {'9' + 1, 'A' - 1}, {'Z' + 1, '_' - 1},
                                        {'_' + 1, 'a' - 1}, {'z' + 1, 0x10FFFF}};
    static const re_range not_space[] = {{0, 8}, {14, 31}, {33, 0x9F}, {0xA1, 0x1FFF},
                                         {0x200B, 0x2027}, {0x202A, 0x2FFF}, {0x3001, 0xFEFE},
                                         {0xFF00, 0x10FFFF}};
    const re_range* src = NULL;
    int n = 0;
    switch (c) {
    case 'd': src = digits; n = 1; break;
    case 'w': src = word; n = 4; break;
    case 's': src = space; n = 7; break;
    case 'D': src = not_digits; n = 2; break;
    case 'W': src = not_word; n = 5; break;
    case 'S': src = not_space; n = 8; break;
    default: return 0;
    }
    for (int i = 0; i < n && *count < capacity; i++) out[(*count)++] = src[i];
    return 1;
}

static uint32_t re_escape_char(re_parser* rp) {
    char c = *rp->p++;
    switch (c) {
    case 'n': return '\n';
    case 't': return '\t';
    case 'r': return '\r';
    case 'f': return '\f';
    case 'v': return '\v';
    case '0': return 0;
    case 'x':
        if (rp->end - rp->p >= 2 && hexval_re(rp->p[0]) >= 0) {
            uint32_t v = (uint32_t)(hexval_re(rp->p[0]) * 16 + hexval_re(rp->p[1]));
            rp->p += 2;
            return v;
        }
        return 'x';
    case 'u': {
        uint32_t v = 0;
        if (rp->p < rp->end && *rp->p == '{') {
            rp->p++;
            while (rp->p < rp->end && *rp->p != '}') v = v * 16 + (uint32_t)hexval_re(*rp->p++);
            if (rp->p < rp->end) rp->p++;
            return v;
        }
        for (int i = 0; i < 4 && rp->p < rp->end && hexval_re(*rp->p) >= 0; i++)
            v = v * 16 + (uint32_t)hexval_re(*rp->p++);
        return v;
    }
    default: {
        /* other escapes are the literal character (UTF-8 aware) */
        rp->p--;
        uint32_t i = 0;
        uint32_t cp = decode_utf8(rp->p, (uint32_t)(rp->end - rp->p), &i);
        rp->p += i;
        return cp;
    }
    }
}

static re_ast* re_parse_alt(re_parser* rp);

static re_ast* re_parse_class(re_parser* rp) {
    re_range ranges[128];
    int count = 0, negated = 0;
    if (rp->p < rp->end && *rp->p == '^') {
        negated = 1;
        rp->p++;
    }
    int first = 1;
    while (rp->p < rp->end && (*rp->p != ']' || first)) {
        first = 0;
        uint32_t lo;
        if (*rp->p == '\\') {
            rp->p++;
            if (rp->p >= rp->end) break;
            if (class_escape(*rp->p, ranges, &count, 128)) {
                rp->p++;
                continue;
            }
            if (*rp->p == 'b') {
                rp->p++;
                lo = '\b';
            } else {
                lo = re_escape_char(rp);
            }
        } else {
            uint32_t i = 0;
            lo = decode_utf8(rp->p, (uint32_t)(rp->end - rp->p), &i);
            rp->p += i;
        }
        uint32_t hi = lo;
        if (rp->p + 1 < rp->end && *rp->p == '-' && rp->p[1] != ']') {
            rp->p++;
            if (*rp->p == '\\') {
                rp->p++;
                hi = re_escape_char(rp);
            } else {
                uint32_t i = 0;
                hi = decode_utf8(rp->p, (uint32_t)(rp->end - rp->p), &i);
                rp->p += i;
            }
        }
        if (count < 128) {
            ranges[count].lo = lo;
            ranges[count].hi = hi;
            count++;
        }
    }
    if (rp->p < rp->end) rp->p++;   /* ] */
    if (rp->prog->icase) {
        int original = count;
        for (int i = 0; i < original && count < 128; i++) {
            re_range r = ranges[i];
            if (r.hi >= 'a' && r.lo <= 'z') {
                uint32_t lo = r.lo < 'a' ? 'a' : r.lo, hi = r.hi > 'z' ? 'z' : r.hi;
                ranges[count].lo = lo - 32;
                ranges[count++].hi = hi - 32;
            }
            if (r.hi >= 'A' && r.lo <= 'Z' && count < 128) {
                uint32_t lo = r.lo < 'A' ? 'A' : r.lo, hi = r.hi > 'Z' ? 'Z' : r.hi;
                ranges[count].lo = lo + 32;
                ranges[count++].hi = hi + 32;
            }
        }
    }
    re_ast* n = re_node(A_CLASS);
    n->value = (uint32_t)re_add_class(rp->prog, ranges, count, negated);
    return n;
}

static re_ast* re_parse_atom(re_parser* rp) {
    char c = *rp->p;
    if (c == '(') {
        rp->p++;
        re_ast* group = re_node(A_GROUP);
        if (rp->p + 1 < rp->end && rp->p[0] == '?') {
            char kind = rp->p[1];
            if (kind == ':') {
                rp->p += 2;
                group->flag = 0;
            } else if (kind == '=' || kind == '!') {
                rp->p += 2;
                re_ast* look = re_node(A_LOOK);
                look->flag = kind == '!';
                re_add_child(look, re_parse_alt(rp));
                if (rp->p < rp->end && *rp->p == ')') rp->p++;
                return look;
            } else if (kind == '<' && rp->p + 2 < rp->end && (rp->p[2] == '=' || rp->p[2] == '!')) {
                /* lookbehind: unsupported, treated as always true */
                int depth = 1;
                rp->p += 3;
                while (rp->p < rp->end && depth) {
                    if (*rp->p == '\\') rp->p++;
                    else if (*rp->p == '(') depth++;
                    else if (*rp->p == ')') depth--;
                    rp->p++;
                }
                return re_node(A_EMPTY);
            } else if (kind == '<') {
                /* named group */
                rp->p += 2;
                const char* name = rp->p;
                while (rp->p < rp->end && *rp->p != '>') rp->p++;
                group->flag = 1;
                group->value = (uint32_t)++rp->prog->groups;
                if (group->value < 10) rp->prog->names[group->value] = js_intern(name, (size_t)(rp->p - name));
                if (rp->p < rp->end) rp->p++;
            } else {
                rp->p++;
            }
        } else {
            group->flag = 1;
            group->value = (uint32_t)++rp->prog->groups;
        }
        re_add_child(group, re_parse_alt(rp));
        if (rp->p < rp->end && *rp->p == ')') rp->p++;
        else rp->error = 1;
        return group;
    }
    if (c == '[') {
        rp->p++;
        return re_parse_class(rp);
    }
    if (c == '.') {
        rp->p++;
        return re_node(A_ANY);
    }
    if (c == '^') {
        rp->p++;
        return re_node(A_BOL);
    }
    if (c == '$') {
        rp->p++;
        return re_node(A_EOL);
    }
    if (c == '\\') {
        rp->p++;
        if (rp->p >= rp->end) return re_node(A_EMPTY);
        char e = *rp->p;
        if (e == 'b') { rp->p++; return re_node(A_WORDB); }
        if (e == 'B') { rp->p++; return re_node(A_NWORDB); }
        if (e >= '1' && e <= '9') {
            rp->p++;
            re_ast* n = re_node(A_BACKREF);
            n->value = (uint32_t)(e - '0');
            return n;
        }
        re_range ranges[16];
        int count = 0;
        if (class_escape(e, ranges, &count, 16)) {
            rp->p++;
            re_ast* n = re_node(A_CLASS);
            n->value = (uint32_t)re_add_class(rp->prog, ranges, count, 0);
            return n;
        }
        re_ast* n = re_node(A_CHAR);
        n->value = re_escape_char(rp);
        return n;
    }
    re_ast* n = re_node(A_CHAR);
    uint32_t i = 0;
    n->value = decode_utf8(rp->p, (uint32_t)(rp->end - rp->p), &i);
    rp->p += i;
    return n;
}

static re_ast* re_parse_repeat(re_parser* rp) {
    re_ast* atom = re_parse_atom(rp);
    while (rp->p < rp->end) {
        char c = *rp->p;
        int min, max;
        if (c == '*') { min = 0; max = -1; rp->p++; }
        else if (c == '+') { min = 1; max = -1; rp->p++; }
        else if (c == '?') { min = 0; max = 1; rp->p++; }
        else if (c == '{' && rp->p + 1 < rp->end && rp->p[1] >= '0' && rp->p[1] <= '9') {
            const char* q = rp->p + 1;
            min = 0;
            while (q < rp->end && *q >= '0' && *q <= '9') min = min * 10 + (*q++ - '0');
            max = min;
            if (q < rp->end && *q == ',') {
                q++;
                if (q < rp->end && *q >= '0' && *q <= '9') {
                    max = 0;
                    while (q < rp->end && *q >= '0' && *q <= '9') max = max * 10 + (*q++ - '0');
                } else {
                    max = -1;
                }
            }
            if (q >= rp->end || *q != '}') return atom;   /* literal '{' */
            rp->p = q + 1;
            if (min > 1000) min = 1000;
            if (max > 1000) max = 1000;
        } else {
            break;
        }
        re_ast* r = re_node(A_REPEAT);
        r->min = min;
        r->max = max;
        r->flag = 1;   /* greedy */
        if (rp->p < rp->end && *rp->p == '?') {
            r->flag = 0;
            rp->p++;
        }
        re_add_child(r, atom);
        atom = r;
    }
    return atom;
}

static re_ast* re_parse_cat(re_parser* rp) {
    re_ast* cat = re_node(A_CAT);
    while (rp->p < rp->end && *rp->p != '|' && *rp->p != ')') re_add_child(cat, re_parse_repeat(rp));
    return cat;
}

static re_ast* re_parse_alt(re_parser* rp) {
    re_ast* first = re_parse_cat(rp);
    if (rp->p >= rp->end || *rp->p != '|') return first;
    re_ast* alt = re_node(A_ALT);
    re_add_child(alt, first);
    while (rp->p < rp->end && *rp->p == '|') {
        rp->p++;
        re_add_child(alt, re_parse_cat(rp));
    }
    return alt;
}

static int re_emit(re_prog* prog, int op) {
    if (prog->count == prog->capacity) {
        int capacity = prog->capacity ? prog->capacity * 2 : 32;
        re_inst* code = (re_inst*)js_alloc(sizeof(re_inst) * (size_t)capacity);
        if (prog->count) memcpy(code, prog->code, sizeof(re_inst) * (size_t)prog->count);
        prog->code = code;
        prog->capacity = capacity;
    }
    re_inst* in = &prog->code[prog->count];
    memset(in, 0, sizeof(*in));
    in->op = (uint8_t)op;
    return prog->count++;
}

static int re_simple(re_ast* n) {
    return n->kind == A_CHAR || n->kind == A_ANY || n->kind == A_CLASS;
}

static void re_compile(re_prog* prog, re_ast* n) {
    switch (n->kind) {
    case A_CHAR: {
        int i = re_emit(prog, RE_CHAR);
        prog->code[i].ch = n->value;
        break;
    }
    case A_ANY: re_emit(prog, RE_ANY); break;
    case A_CLASS: {
        int i = re_emit(prog, RE_CLASS);
        prog->code[i].ch = n->value;
        break;
    }
    case A_BOL: re_emit(prog, RE_BOL); break;
    case A_EOL: re_emit(prog, RE_EOL); break;
    case A_WORDB: re_emit(prog, RE_WORDB); break;
    case A_NWORDB: re_emit(prog, RE_NWORDB); break;
    case A_EMPTY: break;
    case A_BACKREF: {
        int i = re_emit(prog, RE_BACKREF);
        prog->code[i].ch = n->value;
        break;
    }
    case A_CAT:
        for (int i = 0; i < n->count; i++) re_compile(prog, n->children[i]);
        break;
    case A_ALT: {
        int jumps[64], jump_count = 0;
        for (int i = 0; i < n->count; i++) {
            if (i + 1 < n->count) {
                int split = re_emit(prog, RE_SPLIT);
                prog->code[split].x = split + 1;
                re_compile(prog, n->children[i]);
                if (jump_count < 64) jumps[jump_count++] = re_emit(prog, RE_JMP);
                prog->code[split].y = prog->count;
            } else {
                re_compile(prog, n->children[i]);
            }
        }
        for (int i = 0; i < jump_count; i++) prog->code[jumps[i]].x = prog->count;
        break;
    }
    case A_GROUP:
        if (n->flag && n->value < 10) {
            int s = re_emit(prog, RE_SAVE);
            prog->code[s].ch = n->value * 2;
        }
        re_compile(prog, n->children[0]);
        if (n->flag && n->value < 10) {
            int s = re_emit(prog, RE_SAVE);
            prog->code[s].ch = n->value * 2 + 1;
        }
        break;
    case A_LOOK: {
        int look = re_emit(prog, RE_LOOK);
        prog->code[look].negated = n->flag;
        re_compile(prog, n->children[0]);
        re_emit(prog, RE_LOOKEND);
        prog->code[look].x = prog->count;
        break;
    }
    case A_REPEAT: {
        re_ast* child = n->children[0];
        if (re_simple(child)) {
            int rep = re_emit(prog, RE_REPEAT);
            prog->code[rep].x = n->min;
            prog->code[rep].y = n->max;
            prog->code[rep].negated = n->flag;
            re_compile(prog, child);   /* the atom follows */
            prog->code[rep].ch = (uint32_t)(rep + 1);
            break;
        }
        for (int i = 0; i < n->min; i++) re_compile(prog, child);
        if (n->max < 0) {
            int split = re_emit(prog, RE_SPLIT);
            re_compile(prog, child);
            int jmp = re_emit(prog, RE_JMP);
            prog->code[jmp].x = split;
            if (n->flag) {
                prog->code[split].x = split + 1;
                prog->code[split].y = prog->count;
            } else {
                prog->code[split].x = prog->count;
                prog->code[split].y = split + 1;
            }
        } else {
            int splits[1001], count = 0;
            for (int i = n->min; i < n->max && count < 1000; i++) {
                int split = re_emit(prog, RE_SPLIT);
                splits[count++] = split;
                re_compile(prog, child);
            }
            for (int i = 0; i < count; i++) {
                if (n->flag) {
                    prog->code[splits[i]].x = splits[i] + 1;
                    prog->code[splits[i]].y = prog->count;
                } else {
                    prog->code[splits[i]].x = prog->count;
                    prog->code[splits[i]].y = splits[i] + 1;
                }
            }
        }
        break;
    }
    }
}

typedef struct {
    re_prog* prog;
    const char* s;
    uint32_t length;
    int caps[20];
    long steps;
    int depth;
} re_vm;

static uint32_t re_lower(uint32_t c) {
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

static int class_has(const re_class* c, uint32_t ch) {
    int in = 0;
    for (int i = 0; i < c->count && !in; i++) in = ch >= c->ranges[i].lo && ch <= c->ranges[i].hi;
    return c->negated ? !in : in;
}

static int is_word_char(re_vm* vm, uint32_t pos) {
    if (pos >= vm->length) return 0;
    char c = vm->s[pos];
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

/* Matches one simple atom at *sp; advances on success. */
static int re_atom(re_vm* vm, const re_inst* in, uint32_t* sp) {
    if (*sp >= vm->length) return 0;
    uint32_t i = *sp;
    uint32_t ch = decode_utf8(vm->s, vm->length, &i);
    switch (in->op) {
    case RE_CHAR:
        if (vm->prog->icase ? re_lower(ch) != re_lower(in->ch) : ch != in->ch) return 0;
        break;
    case RE_ANY:
        if (!vm->prog->dotall && (ch == '\n' || ch == '\r' || ch == 0x2028 || ch == 0x2029)) return 0;
        break;
    case RE_CLASS:
        if (!class_has(&vm->prog->classes[in->ch], ch)) return 0;
        break;
    default:
        return 0;
    }
    *sp = i;
    return 1;
}

static int re_run(re_vm* vm, int pc, uint32_t sp, uint32_t* end_out) {
    if (++vm->depth > 1500) {
        vm->depth--;
        return 0;
    }
    for (;;) {
        if (++vm->steps > 2000000) {
            vm->depth--;
            return 0;
        }
        const re_inst* in = &vm->prog->code[pc];
        switch (in->op) {
        case RE_CHAR: case RE_ANY: case RE_CLASS:
            if (!re_atom(vm, in, &sp)) {
                vm->depth--;
                return 0;
            }
            pc++;
            break;
        case RE_MATCH:
        case RE_LOOKEND:
            *end_out = sp;
            vm->depth--;
            return 1;
        case RE_JMP:
            pc = in->x;
            break;
        case RE_SPLIT:
            if (re_run(vm, in->x, sp, end_out)) {
                vm->depth--;
                return 1;
            }
            pc = in->y;
            break;
        case RE_SAVE: {
            int slot = (int)in->ch;
            int old = vm->caps[slot];
            vm->caps[slot] = (int)sp;
            if (re_run(vm, pc + 1, sp, end_out)) {
                vm->depth--;
                return 1;
            }
            vm->caps[slot] = old;
            vm->depth--;
            return 0;
        }
        case RE_BOL:
            if (!(sp == 0 || (vm->prog->multiline && vm->s[sp - 1] == '\n'))) {
                vm->depth--;
                return 0;
            }
            pc++;
            break;
        case RE_EOL:
            if (!(sp == vm->length || (vm->prog->multiline && vm->s[sp] == '\n'))) {
                vm->depth--;
                return 0;
            }
            pc++;
            break;
        case RE_WORDB:
        case RE_NWORDB: {
            int a = sp > 0 && is_word_char(vm, sp - 1), b = is_word_char(vm, sp);
            int boundary = a != b;
            if (boundary != (in->op == RE_WORDB)) {
                vm->depth--;
                return 0;
            }
            pc++;
            break;
        }
        case RE_LOOK: {
            uint32_t ignored;
            int saved[20];
            memcpy(saved, vm->caps, sizeof(saved));
            int matched = re_run(vm, pc + 1, sp, &ignored);
            if (in->negated) memcpy(vm->caps, saved, sizeof(saved));
            if (matched == in->negated) {
                vm->depth--;
                return 0;
            }
            pc = in->x;
            break;
        }
        case RE_BACKREF: {
            int g = (int)in->ch;
            int start = vm->caps[g * 2], end = vm->caps[g * 2 + 1];
            if (start >= 0 && end >= start) {
                uint32_t n = (uint32_t)(end - start);
                if (sp + n > vm->length || memcmp(vm->s + start, vm->s + sp, n)) {
                    vm->depth--;
                    return 0;
                }
                sp += n;
            }
            pc++;
            break;
        }
        case RE_REPEAT: {
            const re_inst* atom = &vm->prog->code[in->ch];
            int min = in->x, max = in->y;
            int next_pc = (int)in->ch + 1;
            uint32_t positions[2048];
            int count = 0;
            uint32_t pos = sp;
            positions[0] = pos;
            while ((max < 0 || count < max) && count < 2047) {
                uint32_t p2 = pos;
                if (!re_atom(vm, atom, &p2)) break;
                pos = p2;
                positions[++count] = pos;
            }
            if (count < min) {
                vm->depth--;
                return 0;
            }
            if (in->negated) {
                for (int k = count; k >= min; k--)
                    if (re_run(vm, next_pc, positions[k], end_out)) {
                        vm->depth--;
                        return 1;
                    }
            } else {
                for (int k = min; k <= count; k++)
                    if (re_run(vm, next_pc, positions[k], end_out)) {
                        vm->depth--;
                        return 1;
                    }
            }
            vm->depth--;
            return 0;
        }
        default:
            vm->depth--;
            return 0;
        }
    }
}

/* Finds a match at or after `start` (bytes). Fills caps[0..]. */
static int re_search(re_prog* prog, const js_str* s, uint32_t start, int* caps) {
    re_vm* vm = (re_vm*)malloc(sizeof(re_vm));
    if (!vm) return 0;
    vm->prog = prog;
    vm->s = s->data;
    vm->length = s->length;
    vm->steps = 0;
    int found = 0;
    for (uint32_t pos = start; pos <= s->length; pos++) {
        if (pos > start && ((unsigned char)s->data[pos] & 0xC0) == 0x80 && pos < s->length) continue;
        for (int i = 0; i < 20; i++) vm->caps[i] = -1;
        vm->depth = 0;
        uint32_t end;
        if (re_run(vm, 0, pos, &end)) {
            vm->caps[0] = (int)pos;
            vm->caps[1] = (int)end;
            memcpy(caps, vm->caps, sizeof(vm->caps));
            found = 1;
            break;
        }
        if (prog->sticky) break;
        if (prog->code[0].op == RE_BOL && !prog->multiline) break;
        if (vm->steps > 2000000) break;
    }
    free(vm);
    return found;
}

static int hexval_re(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static re_prog* re_compile_pattern(const js_str* pattern, const js_str* flags,
                                   int* error) {
    re_prog* prog = (re_prog*)js_alloc(sizeof(re_prog));
    memset(prog, 0, sizeof(*prog));
    for (uint32_t i = 0; flags && i < flags->length; i++) {
        char f = flags->data[i];
        if (f == 'i') prog->icase = 1;
        else if (f == 'm') prog->multiline = 1;
        else if (f == 's') prog->dotall = 1;
        else if (f == 'g') prog->global = 1;
        else if (f == 'y') prog->sticky = 1;
    }
    re_parser rp = {pattern->data, pattern->data + pattern->length, prog, 0};
    re_ast* ast = re_parse_alt(&rp);
    if (rp.p < rp.end) rp.error = 1;   /* unbalanced ')' */
    re_compile(prog, ast);
    re_emit(prog, RE_MATCH);
    *error = rp.error;
    return prog;
}

js_val js_regexp_create(js_str* pattern, js_str* flags) {
    int error = 0;
    re_prog* prog = re_compile_pattern(pattern, flags, &error);
    if (error) {
        char message[200];
        mx_copy(message, sizeof(message), "Invalid regular expression: /");
        mx_append(message, sizeof(message), pattern->data);
        mx_append(message, sizeof(message), "/");
        return js_throw_error("SyntaxError", message);
    }
    js_obj* re = js_new_object(js_regexp_proto);
    re->cls = CLS_REGEXP;
    re->data = prog;
    js_define(re, "source", js_str_val(pattern), PROP_HIDDEN);
    js_define(re, "flags", js_str_val(flags ? flags : js_intern_c("")), PROP_HIDDEN);
    js_define(re, "global", js_bool(prog->global), PROP_HIDDEN);
    js_define(re, "ignoreCase", js_bool(prog->icase), PROP_HIDDEN);
    js_define(re, "multiline", js_bool(prog->multiline), PROP_HIDDEN);
    js_define(re, "lastIndex", js_num(0), PROP_HIDDEN);
    return js_obj_val(re);
}

static js_val regexp_exec_at(js_obj* re, js_str* s, uint32_t start_byte, int* caps_out) {
    re_prog* prog = (re_prog*)re->data;
    int caps[20];
    if (!re_search(prog, s, start_byte, caps)) return js_null();
    js_obj* result = js_new_array((uint32_t)prog->groups + 1);
    for (int g = 0; g <= prog->groups && g < 10; g++) {
        if (caps[g * 2] >= 0 && caps[g * 2 + 1] >= caps[g * 2])
            js_array_push(result, js_str_val(js_new_str(s->data + caps[g * 2],
                                                        (size_t)(caps[g * 2 + 1] - caps[g * 2]))));
        else
            js_array_push(result, js_undef());
    }
    int ascii = str_is_ascii(s);
    js_define(result, "index", js_num(byte_to_cp(s, (uint32_t)caps[0], ascii)), 0);
    js_define(result, "input", js_str_val(s), PROP_HIDDEN);
    int has_names = 0;
    for (int g = 1; g < 10; g++) has_names |= prog->names[g] != NULL;
    if (has_names) {
        js_obj* groups = js_new_object(js_object_proto);
        for (int g = 1; g < 10 && g <= prog->groups; g++)
            if (prog->names[g]) js_set(js_obj_val(groups), prog->names[g], result->items[g]);
        js_define(result, "groups", js_obj_val(groups), 0);
    }
    if (caps_out) memcpy(caps_out, caps, sizeof(caps));
    return js_obj_val(result);
}

static js_obj* this_regexp(js_val self) {
    if (self.type == JS_OBJ && self.u.obj->cls == CLS_REGEXP) return self.u.obj;
    js_throw_error("TypeError", "Receiver is not a RegExp");
    return NULL;
}

static js_val regexp_exec(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)callee;
    js_obj* re = this_regexp(self);
    if (!re) return js_undef();
    js_str* s = js_to_string(ARG(0));
    re_prog* prog = (re_prog*)re->data;
    uint32_t start = 0;
    int ascii = str_is_ascii(s);
    if (prog->global || prog->sticky) {
        double li = js_to_number(js_get_c(self, "lastIndex"));
        if (li > cp_length(s)) {
            js_set_c(self, "lastIndex", js_num(0));
            return js_null();
        }
        start = cp_to_byte(s, (uint32_t)li, ascii);
    }
    int caps[20];
    js_val result = regexp_exec_at(re, s, start, caps);
    if (prog->global || prog->sticky) {
        if (result.type == JS_NULL) {
            js_set_c(self, "lastIndex", js_num(0));
        } else {
            uint32_t end = (uint32_t)caps[1];
            if (caps[1] == caps[0]) end++;   /* avoid empty-match loops */
            js_set_c(self, "lastIndex", js_num(byte_to_cp(s, end, ascii)));
        }
    }
    return result;
}

static js_val regexp_test(js_val self, js_val* args, int argc, js_obj* callee) {
    js_val r = regexp_exec(self, args, argc, callee);
    return js_bool(r.type == JS_OBJ);
}

static js_val regexp_to_string(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)args; (void)argc; (void)callee;
    js_str* src = js_to_string(js_get_c(self, "source"));
    js_str* flags = js_to_string(js_get_c(self, "flags"));
    return js_str_val(js_concat(js_concat(js_concat(js_intern_c("/"), src), js_intern_c("/")), flags));
}

static js_val regexp_ctor(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)self; (void)callee;
    js_val p = ARG(0);
    if (p.type == JS_OBJ && p.u.obj->cls == CLS_REGEXP) {
        js_str* flags = ARG(1).type == JS_UNDEF ? js_to_string(js_get_c(p, "flags")) : js_to_string(ARG(1));
        return js_regexp_create(js_to_string(js_get_c(p, "source")), flags);
    }
    return js_regexp_create(p.type == JS_UNDEF ? js_intern_c("(?:)") : js_to_string(p),
                            ARG(1).type == JS_UNDEF ? js_intern_c("") : js_to_string(ARG(1)));
}

/* ====================================================================== */
/* Object / Function                                                      */
/* ====================================================================== */

static js_val object_ctor(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)self; (void)callee;
    js_val v = ARG(0);
    if (v.type == JS_UNDEF || v.type == JS_NULL) return js_obj_val(js_new_object(js_object_proto));
    return js_obj_val(js_to_object(v));
}

static js_obj* require_object(js_val v) {
    if (v.type == JS_OBJ) return v.u.obj;
    if (v.type == JS_UNDEF || v.type == JS_NULL) {
        js_throw_error("TypeError", "Cannot convert undefined or null to object");
        return NULL;
    }
    return js_to_object(v);
}

static js_val object_keys(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)self;
    js_obj* o = require_object(ARG(0));
    if (!o) return js_undef();
    js_obj* keys = js_own_keys(o, 0);
    int mode = callee->magic;   /* 0 keys, 1 values, 2 entries */
    if (ARG(0).type == JS_STR) {
        keys = js_new_array(0);
        uint32_t n = cp_length(ARG(0).u.str);
        for (uint32_t i = 0; i < n; i++) js_array_push(keys, js_str_val(js_number_to_string(i, 10)));
    }
    if (mode == 0) return js_obj_val(keys);
    js_obj* out = js_new_array(keys->length);
    for (uint32_t i = 0; i < keys->length; i++) {
        js_val value = js_get(ARG(0), keys->items[i].u.str);
        if (mode == 1) {
            js_array_push(out, value);
        } else {
            js_obj* pair = js_new_array(2);
            js_array_push(pair, keys->items[i]);
            js_array_push(pair, value);
            js_array_push(out, js_obj_val(pair));
        }
    }
    return js_obj_val(out);
}

static js_val object_assign(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)self; (void)callee;
    js_obj* target = require_object(ARG(0));
    if (!target) return js_undef();
    for (int i = 1; i < argc; i++) {
        if (args[i].type != JS_OBJ) continue;
        js_obj* keys = js_own_keys(args[i].u.obj, 0);
        for (uint32_t k = 0; k < keys->length; k++) {
            js_str* key = keys->items[k].u.str;
            js_set(js_obj_val(target), key, js_get(args[i], key));
        }
    }
    return js_obj_val(target);
}

static void define_from_descriptor(js_obj* o, js_str* key, js_val desc) {
    if (desc.type != JS_OBJ) return;
    js_val get = js_get_c(desc, "get"), set = js_get_c(desc, "set");
    js_prop* p = js_find_own(o, key);
    if (js_is_callable(get) || js_is_callable(set)) {
        if (!p) {
            js_set(js_obj_val(o), key, js_undef());
            p = js_find_own(o, key);
            if (!p) return;
        }
        p->flags = PROP_ACCESSOR | (js_truthy(js_get_c(desc, "enumerable")) ? 0 : PROP_HIDDEN);
        p->value = js_is_callable(get) ? get : js_undef();
        p->setter = js_is_callable(set) ? set.u.obj : NULL;
        return;
    }
    js_val value = js_get_c(desc, "value");
    if (o->cls == CLS_ARRAY) {
        js_set(js_obj_val(o), key, value);
        return;
    }
    if (!p) {
        js_set(js_obj_val(o), key, value);
        p = js_find_own(o, key);
        if (!p) return;
        p->flags = (js_truthy(js_get_c(desc, "enumerable")) ? 0 : PROP_HIDDEN) |
                   (js_truthy(js_get_c(desc, "writable")) ? 0 : PROP_READONLY);
    } else {
        p->flags &= (uint8_t)~(PROP_ACCESSOR | PROP_READONLY);
        p->value = value;
    }
}

static js_val object_define_property(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)self; (void)callee;
    if (ARG(0).type != JS_OBJ) return js_throw_error("TypeError", "Object.defineProperty called on non-object");
    define_from_descriptor(ARG(0).u.obj, js_to_string(ARG(1)), ARG(2));
    return ARG(0);
}

static js_val object_define_properties(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)self; (void)callee;
    if (ARG(0).type != JS_OBJ || ARG(1).type != JS_OBJ) return ARG(0);
    js_obj* keys = js_own_keys(ARG(1).u.obj, 0);
    for (uint32_t i = 0; i < keys->length; i++)
        define_from_descriptor(ARG(0).u.obj, keys->items[i].u.str, js_get(ARG(1), keys->items[i].u.str));
    return ARG(0);
}

static js_val object_get_own_property_descriptor(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)self; (void)callee;
    if (ARG(0).type != JS_OBJ) return js_undef();
    js_str* key = js_to_string(ARG(1));
    js_prop* p = js_find_own(ARG(0).u.obj, key);
    if (!p && !js_has_property(ARG(0), key)) return js_undef();
    js_obj* d = js_new_object(js_object_proto);
    if (p && (p->flags & PROP_ACCESSOR)) {
        js_set_c(js_obj_val(d), "get", p->value);
        js_set_c(js_obj_val(d), "set", p->setter ? js_obj_val(p->setter) : js_undef());
    } else {
        js_set_c(js_obj_val(d), "value", js_get(ARG(0), key));
        js_set_c(js_obj_val(d), "writable", js_bool(!p || !(p->flags & PROP_READONLY)));
    }
    js_set_c(js_obj_val(d), "enumerable", js_bool(!p || !(p->flags & PROP_HIDDEN)));
    js_set_c(js_obj_val(d), "configurable", js_bool(1));
    return js_obj_val(d);
}

static js_val object_create(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)self; (void)callee;
    js_obj* o = js_new_object(ARG(0).type == JS_OBJ ? ARG(0).u.obj : NULL);
    if (ARG(1).type == JS_OBJ) {
        js_val pair[2] = {js_obj_val(o), ARG(1)};
        object_define_properties(js_undef(), pair, 2, callee);
    }
    return js_obj_val(o);
}

static js_val object_freeze(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)self;
    if (ARG(0).type == JS_OBJ && callee->magic == 0) ARG(0).u.obj->frozen = 1;
    return ARG(0);
}

static js_val object_is_frozen(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)self; (void)callee;
    return js_bool(ARG(0).type != JS_OBJ || ARG(0).u.obj->frozen);
}

static js_val object_get_prototype_of(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)self; (void)callee;
    js_val v = ARG(0);
    if (v.type == JS_STR) return js_obj_val(js_string_proto);
    if (v.type == JS_NUM) return js_obj_val(js_number_proto);
    if (v.type != JS_OBJ) return js_null();
    return v.u.obj->proto ? js_obj_val(v.u.obj->proto) : js_null();
}

static js_val object_set_prototype_of(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)self; (void)callee;
    if (ARG(0).type == JS_OBJ)
        ARG(0).u.obj->proto = ARG(1).type == JS_OBJ ? ARG(1).u.obj : NULL;
    return ARG(0);
}

static js_val object_get_own_property_names(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)self; (void)callee;
    js_obj* o = require_object(ARG(0));
    if (!o) return js_undef();
    return js_obj_val(js_own_keys(o, 1));
}

static js_val object_from_entries(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)self; (void)callee;
    js_obj* o = js_new_object(js_object_proto);
    js_val from = js_get_c(js_get_c(js_obj_val(js_global), "Array"), "from");
    js_val list = js_call(from, js_undef(), args, argc > 0 ? 1 : 0);
    if (list.type != JS_OBJ) return js_obj_val(o);
    for (uint32_t i = 0; i < list.u.obj->length; i++) {
        js_val pair = list.u.obj->items[i];
        js_set(js_obj_val(o), js_to_string(js_get_index(pair, js_num(0))), js_get_index(pair, js_num(1)));
    }
    return js_obj_val(o);
}

static js_val object_is(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)self; (void)callee;
    js_val a = ARG(0), b = ARG(1);
    if (a.type == JS_NUM && b.type == JS_NUM && is_nan(a.u.num) && is_nan(b.u.num)) return js_bool(1);
    return js_bool(js_strict_equals(a, b));
}

static js_val object_has_own(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)callee;
    js_val target = callee->magic ? ARG(0) : self;
    js_val key = callee->magic ? ARG(1) : ARG(0);
    if (target.type != JS_OBJ) return js_bool(target.type == JS_STR && js_get_index(target, key).type != JS_UNDEF);
    js_obj* o = target.u.obj;
    js_str* k = js_to_string(key);
    if (o->cls == CLS_ARRAY || o->cls == CLS_ARGUMENTS) {
        uint32_t index;
        if (js_array_index(k, &index)) return js_bool(index < o->length);
        if (js_str_eq_c(k, "length")) return js_bool(1);
    }
    if (o->cls >= CLS_NODE && o->cls <= CLS_DATASET) {
        int found = 0;
        js_dom_get(o, k, &found);
        if (found) return js_bool(1);
    }
    return js_bool(js_find_own(o, k) != NULL);
}

static js_val object_is_prototype_of(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)callee;
    if (self.type != JS_OBJ || ARG(0).type != JS_OBJ) return js_bool(0);
    for (js_obj* p = ARG(0).u.obj->proto; p; p = p->proto)
        if (p == self.u.obj) return js_bool(1);
    return js_bool(0);
}

static js_val object_to_string(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)args; (void)argc; (void)callee;
    if (self.type == JS_UNDEF) return js_str_val(js_intern_c("[object Undefined]"));
    if (self.type == JS_NULL) return js_str_val(js_intern_c("[object Null]"));
    const char* tag = "Object";
    if (self.type == JS_OBJ) {
        switch (self.u.obj->cls) {
        case CLS_ARRAY: tag = "Array"; break;
        case CLS_FUNCTION: case CLS_NATIVE: case CLS_BOUND: tag = "Function"; break;
        case CLS_ERROR: tag = "Error"; break;
        case CLS_DATE: tag = "Date"; break;
        case CLS_REGEXP: tag = "RegExp"; break;
        case CLS_ARGUMENTS: tag = "Arguments"; break;
        case CLS_NODE: tag = "HTMLElement"; break;
        case CLS_PROMISE: tag = "Promise"; break;
        case CLS_MAP: tag = "Map"; break;
        case CLS_SET: tag = "Set"; break;
        default: break;
        }
        js_val custom = js_get_c(self, "@@toStringTag");
        if (custom.type == JS_STR) tag = custom.u.str->data;
    } else if (self.type == JS_STR) tag = "String";
    else if (self.type == JS_NUM) tag = "Number";
    else if (self.type == JS_BOOL) tag = "Boolean";
    char out[64] = "[object ";
    mx_append(out, sizeof(out), tag);
    mx_append(out, sizeof(out), "]");
    return js_str_val(js_cstr(out));
}

static js_val object_value_of(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)args; (void)argc; (void)callee;
    if (self.type == JS_OBJ && self.u.obj->cls == CLS_BOXED) return self.u.obj->primitive;
    if (self.type == JS_OBJ && self.u.obj->cls == CLS_DATE) return js_num(self.u.obj->number);
    return self;
}

static js_val function_ctor(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)self; (void)callee;
    /* new Function(a, b, body) */
    js_str* source = js_intern_c("(function anonymous(");
    for (int i = 0; i + 1 < argc; i++) {
        if (i) source = js_concat(source, js_intern_c(","));
        source = js_concat(source, js_to_string(args[i]));
    }
    source = js_concat(source, js_intern_c(") {\n"));
    if (argc) source = js_concat(source, js_to_string(args[argc - 1]));
    source = js_concat(source, js_intern_c("\n})"));
    char error[160];
    int line = 0;
    js_node* program = js_parse(source->data, source->length, "Function", error, sizeof(error), &line);
    if (!program) return js_throw_error("SyntaxError", error);
    return js_exec_program(program);
}

static js_val function_call(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)callee;
    return js_call(self, ARG(0), argc > 1 ? args + 1 : NULL, argc > 1 ? argc - 1 : 0);
}

static js_val function_apply(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)callee;
    js_val list = ARG(1);
    if (list.type == JS_OBJ && (list.u.obj->cls == CLS_ARRAY || list.u.obj->cls == CLS_ARGUMENTS))
        return js_call(self, ARG(0), list.u.obj->items, (int)list.u.obj->length);
    if (list.type == JS_OBJ) {
        js_val from = js_get_c(js_get_c(js_obj_val(js_global), "Array"), "from");
        js_val arr = js_call(from, js_undef(), &list, 1);
        if (arr.type == JS_OBJ) return js_call(self, ARG(0), arr.u.obj->items, (int)arr.u.obj->length);
    }
    return js_call(self, ARG(0), NULL, 0);
}

static js_val function_bind(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)callee;
    if (!js_is_callable(self)) return js_throw_error("TypeError", "Bind must be called on a function");
    js_obj* b = js_new_object(js_function_proto);
    b->cls = CLS_BOUND;
    b->bound_target = self.u.obj;
    b->bound_this = ARG(0);
    b->bound_count = argc > 1 ? argc - 1 : 0;
    if (b->bound_count) {
        b->bound_args = (js_val*)js_alloc(sizeof(js_val) * (size_t)b->bound_count);
        memcpy(b->bound_args, args + 1, sizeof(js_val) * (size_t)b->bound_count);
    }
    js_str* name = self.u.obj->name ? self.u.obj->name : js_intern_c("");
    b->name = js_concat(js_intern_c("bound "), name);
    return js_obj_val(b);
}

static js_val function_to_string(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)args; (void)argc; (void)callee;
    js_str* name = self.type == JS_OBJ && self.u.obj->name ? self.u.obj->name : js_intern_c("");
    return js_str_val(js_concat(js_concat(js_intern_c("function "), name),
                                js_intern_c("() { [code] }")));
}

/* ====================================================================== */
/* Arrays                                                                 */
/* ====================================================================== */

static js_obj* this_array(js_val self) {
    if (self.type == JS_OBJ && (self.u.obj->cls == CLS_ARRAY || self.u.obj->cls == CLS_ARGUMENTS))
        return self.u.obj;
    /* array-likes (NodeList-ish objects with length) */
    if (self.type == JS_OBJ || self.type == JS_STR) {
        js_val from = js_get_c(js_get_c(js_obj_val(js_global), "Array"), "from");
        js_val arr = js_call(from, js_undef(), &self, 1);
        if (arr.type == JS_OBJ) return arr.u.obj;
    }
    js_throw_error("TypeError", "Array method called on incompatible receiver");
    return NULL;
}

static js_val iterate_to_array(js_val v) {
    js_obj* out = js_new_array(0);
    if (v.type == JS_STR) {
        js_str* s = v.u.str;
        uint32_t i = 0;
        while (i < s->length) {
            uint32_t start = i++;
            while (i < s->length && ((unsigned char)s->data[i] & 0xC0) == 0x80) i++;
            js_array_push(out, js_str_val(js_new_str(s->data + start, i - start)));
        }
        return js_obj_val(out);
    }
    if (v.type != JS_OBJ) return js_obj_val(out);
    js_obj* o = v.u.obj;
    if (o->cls == CLS_ARRAY || o->cls == CLS_ARGUMENTS) {
        for (uint32_t i = 0; i < o->length; i++) js_array_push(out, o->items[i]);
        return js_obj_val(out);
    }
    if (o->cls == CLS_SET) {
        for (uint32_t i = 0; i < o->length; i++) js_array_push(out, o->items[i]);
        return js_obj_val(out);
    }
    if (o->cls == CLS_MAP) {
        js_obj* values = (js_obj*)o->data;
        for (uint32_t i = 0; i < o->length; i++) {
            js_obj* pair = js_new_array(2);
            js_array_push(pair, o->items[i]);
            js_array_push(pair, values->items[i]);
            js_array_push(out, js_obj_val(pair));
        }
        return js_obj_val(out);
    }
    js_val iterator_fn = js_get_c(v, "@@iterator");
    if (js_is_callable(iterator_fn)) {
        js_val it = js_call(iterator_fn, v, NULL, 0);
        if (js_rt.ctl) return js_undef();
        js_val next = js_get_c(it, "next");
        for (int guard = 0; guard < 1000000 && js_is_callable(next); guard++) {
            js_val r = js_call(next, it, NULL, 0);
            if (js_rt.ctl || r.type != JS_OBJ) break;
            if (js_truthy(js_get_c(r, "done"))) break;
            js_array_push(out, js_get_c(r, "value"));
        }
        return js_obj_val(out);
    }
    js_val length = js_get(v, js_intern_c("length"));
    double n = js_to_number(length);
    if (n > 0 && n < 1e7)
        for (uint32_t i = 0; i < (uint32_t)n; i++) js_array_push(out, js_get_index(v, js_num(i)));
    return js_obj_val(out);
}

static js_val array_ctor(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)self; (void)callee;
    if (argc == 1 && args[0].type == JS_NUM) {
        double n = args[0].u.num;
        if (n < 0 || n > 1e8 || n != m_floor(n)) return js_throw_error("RangeError", "Invalid array length");
        js_obj* a = js_new_array((uint32_t)n);
        js_array_set_length(a, (uint32_t)n);
        return js_obj_val(a);
    }
    js_obj* a = js_new_array((uint32_t)argc);
    for (int i = 0; i < argc; i++) js_array_push(a, args[i]);
    return js_obj_val(a);
}

static js_val array_is_array(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)self; (void)callee;
    return js_bool(ARG(0).type == JS_OBJ && ARG(0).u.obj->cls == CLS_ARRAY);
}

static js_val array_from(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)self; (void)callee;
    js_val list = iterate_to_array(ARG(0));
    if (js_rt.ctl || list.type != JS_OBJ) return js_obj_val(js_new_array(0));
    if (js_is_callable(ARG(1))) {
        js_obj* a = list.u.obj;
        for (uint32_t i = 0; i < a->length && !js_rt.ctl; i++) {
            js_val call_args[2] = {a->items[i], js_num(i)};
            a->items[i] = js_call(ARG(1), ARG(2), call_args, 2);
        }
    }
    return list;
}

static js_val array_of(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)self; (void)callee;
    js_obj* a = js_new_array((uint32_t)argc);
    for (int i = 0; i < argc; i++) js_array_push(a, args[i]);
    return js_obj_val(a);
}

static js_val array_push(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)callee;
    js_obj* a = this_array(self);
    if (!a) return js_undef();
    for (int i = 0; i < argc; i++) js_array_push(a, args[i]);
    return js_num(a->length);
}

static js_val array_pop(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)args; (void)argc; (void)callee;
    js_obj* a = this_array(self);
    if (!a || !a->length) return js_undef();
    return a->items[--a->length];
}

static js_val array_shift(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)args; (void)argc; (void)callee;
    js_obj* a = this_array(self);
    if (!a || !a->length) return js_undef();
    js_val first = a->items[0];
    memmove(a->items, a->items + 1, sizeof(js_val) * (a->length - 1));
    a->length--;
    return first;
}

static js_val array_unshift(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)callee;
    js_obj* a = this_array(self);
    if (!a) return js_undef();
    uint32_t old = a->length;
    js_array_set_length(a, old + (uint32_t)argc);
    memmove(a->items + argc, a->items, sizeof(js_val) * old);
    for (int i = 0; i < argc; i++) a->items[i] = args[i];
    return js_num(a->length);
}

static js_val array_slice(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)callee;
    js_obj* a = this_array(self);
    if (!a) return js_undef();
    long len = (long)a->length;
    long start = relative_index(ARG(0), len, 0), end = relative_index(ARG(1), len, len);
    js_obj* out = js_new_array(end > start ? (uint32_t)(end - start) : 0);
    for (long i = start; i < end; i++) js_array_push(out, a->items[i]);
    return js_obj_val(out);
}

static js_val array_splice(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)callee;
    js_obj* a = this_array(self);
    if (!a) return js_undef();
    long len = (long)a->length;
    long start = relative_index(ARG(0), len, 0);
    long remove = argc < 2 ? len - start : (long)js_to_number(ARG(1));
    if (remove < 0) remove = 0;
    if (start + remove > len) remove = len - start;
    js_obj* removed = js_new_array((uint32_t)remove);
    for (long i = 0; i < remove; i++) js_array_push(removed, a->items[start + i]);
    int insert = argc > 2 ? argc - 2 : 0;
    long new_len = len - remove + insert;
    js_val* items = (js_val*)js_alloc(sizeof(js_val) * (size_t)(new_len ? new_len : 1));
    for (long i = 0; i < start; i++) items[i] = a->items[i];
    for (int i = 0; i < insert; i++) items[start + i] = args[2 + i];
    for (long i = start + remove; i < len; i++) items[i - remove + insert] = a->items[i];
    a->items = items;
    a->length = (uint32_t)new_len;
    a->item_capacity = (uint32_t)(new_len ? new_len : 1);
    return js_obj_val(removed);
}

static js_val array_concat(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)callee;
    js_obj* a = this_array(self);
    if (!a) return js_undef();
    js_obj* out = js_new_array(a->length);
    for (uint32_t i = 0; i < a->length; i++) js_array_push(out, a->items[i]);
    for (int k = 0; k < argc; k++) {
        js_val v = args[k];
        if (v.type == JS_OBJ && v.u.obj->cls == CLS_ARRAY) {
            for (uint32_t i = 0; i < v.u.obj->length; i++) js_array_push(out, v.u.obj->items[i]);
        } else {
            js_array_push(out, v);
        }
    }
    return js_obj_val(out);
}

static js_val array_join(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)callee;
    js_obj* a = this_array(self);
    if (!a) return js_undef();
    js_str* sep = ARG(0).type == JS_UNDEF ? js_intern_c(",") : js_to_string(ARG(0));
    size_t total = 0;
    js_str** parts = (js_str**)js_alloc(sizeof(js_str*) * (a->length ? a->length : 1));
    for (uint32_t i = 0; i < a->length; i++) {
        js_val v = a->items[i];
        parts[i] = v.type == JS_UNDEF || v.type == JS_NULL ? js_intern_c("") : js_to_string(v);
        if (js_rt.ctl) return js_undef();
        total += parts[i]->length + (i ? sep->length : 0);
    }
    char* buffer = (char*)malloc(total + 1);
    if (!buffer) return js_str_val(js_intern_c(""));
    size_t used = 0;
    for (uint32_t i = 0; i < a->length; i++) {
        if (i) {
            memcpy(buffer + used, sep->data, sep->length);
            used += sep->length;
        }
        memcpy(buffer + used, parts[i]->data, parts[i]->length);
        used += parts[i]->length;
    }
    js_str* result = js_new_str(buffer, used);
    free(buffer);
    return js_str_val(result);
}

static js_val array_reverse(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)args; (void)argc; (void)callee;
    js_obj* a = this_array(self);
    if (!a) return js_undef();
    for (uint32_t i = 0; i < a->length / 2; i++) {
        js_val t = a->items[i];
        a->items[i] = a->items[a->length - 1 - i];
        a->items[a->length - 1 - i] = t;
    }
    return self;
}

static int same_value_zero(js_val a, js_val b) {
    if (a.type == JS_NUM && b.type == JS_NUM && is_nan(a.u.num) && is_nan(b.u.num)) return 1;
    return js_strict_equals(a, b);
}

static js_val array_index_of(js_val self, js_val* args, int argc, js_obj* callee) {
    js_obj* a = this_array(self);
    if (!a) return js_undef();
    int mode = callee->magic;   /* 0 indexOf, 1 lastIndexOf, 2 includes */
    long len = (long)a->length;
    if (mode == 1) {
        long from = argc > 1 ? relative_index(ARG(1), len, len - 1) : len - 1;
        if (from >= len) from = len - 1;
        for (long i = from; i >= 0; i--)
            if (js_strict_equals(a->items[i], ARG(0))) return js_num((double)i);
        return js_num(-1);
    }
    long from = relative_index(ARG(1), len, 0);
    for (long i = from; i < len; i++) {
        if (mode == 2 ? same_value_zero(a->items[i], ARG(0)) : js_strict_equals(a->items[i], ARG(0)))
            return mode == 2 ? js_bool(1) : js_num((double)i);
    }
    return mode == 2 ? js_bool(0) : js_num(-1);
}

/* forEach/map/filter/find/findIndex/some/every/findLast/findLastIndex */
static js_val array_iterate(js_val self, js_val* args, int argc, js_obj* callee) {
    js_obj* a = this_array(self);
    if (!a) return js_undef();
    js_val fn = ARG(0);
    if (!js_is_callable(fn)) return js_throw_error("TypeError", "callback is not a function");
    int mode = callee->magic;
    js_obj* out = mode == 1 || mode == 2 ? js_new_array(mode == 1 ? a->length : 0) : NULL;
    uint32_t length = a->length;
    int reverse = mode == 7 || mode == 8;
    for (uint32_t k = 0; k < length && !js_rt.ctl; k++) {
        uint32_t i = reverse ? length - 1 - k : k;
        if (i >= a->length) break;
        js_val item = a->items[i];
        js_val call_args[3] = {item, js_num(i), self};
        js_val r = js_call(fn, ARG(1), call_args, 3);
        if (js_rt.ctl) return js_undef();
        switch (mode) {
        case 1: js_array_push(out, r); break;
        case 2: if (js_truthy(r)) js_array_push(out, item); break;
        case 3: case 7: if (js_truthy(r)) return item; break;
        case 4: case 8: if (js_truthy(r)) return js_num(i); break;
        case 5: if (js_truthy(r)) return js_bool(1); break;
        case 6: if (!js_truthy(r)) return js_bool(0); break;
        default: break;
        }
    }
    switch (mode) {
    case 1: case 2: return js_obj_val(out);
    case 4: case 8: return js_num(-1);
    case 5: return js_bool(0);
    case 6: return js_bool(1);
    default: return js_undef();
    }
}

static js_val array_reduce(js_val self, js_val* args, int argc, js_obj* callee) {
    js_obj* a = this_array(self);
    if (!a) return js_undef();
    js_val fn = ARG(0);
    if (!js_is_callable(fn)) return js_throw_error("TypeError", "callback is not a function");
    int right = callee->magic;
    long len = (long)a->length;
    long i = right ? len - 1 : 0;
    js_val acc;
    if (argc >= 2) {
        acc = args[1];
    } else {
        if (!len) return js_throw_error("TypeError", "Reduce of empty array with no initial value");
        acc = a->items[i];
        i += right ? -1 : 1;
    }
    for (; right ? i >= 0 : i < len; i += right ? -1 : 1) {
        if (i >= (long)a->length) break;
        js_val call_args[4] = {acc, a->items[i], js_num((double)i), self};
        acc = js_call(fn, js_undef(), call_args, 4);
        if (js_rt.ctl) return js_undef();
    }
    return acc;
}

static int sort_compare(js_val fn, js_val a, js_val b) {
    if (a.type == JS_UNDEF) return b.type == JS_UNDEF ? 0 : 1;
    if (b.type == JS_UNDEF) return -1;
    if (js_is_callable(fn)) {
        js_val call_args[2] = {a, b};
        double r = js_to_number(js_call(fn, js_undef(), call_args, 2));
        return r < 0 ? -1 : r > 0 ? 1 : 0;
    }
    js_str* x = js_to_string(a);
    js_str* y = js_to_string(b);
    int c = strcmp(x->data, y->data);
    return c < 0 ? -1 : c > 0;
}

static js_val array_sort(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)callee;
    js_obj* a = this_array(self);
    if (!a) return js_undef();
    js_val fn = ARG(0);
    uint32_t n = a->length;
    /* merge sort (stable) */
    js_val* tmp = (js_val*)malloc(sizeof(js_val) * (n ? n : 1));
    if (!tmp) return self;
    for (uint32_t width = 1; width < n && !js_rt.ctl; width *= 2) {
        for (uint32_t lo = 0; lo < n && !js_rt.ctl; lo += 2 * width) {
            uint32_t mid = lo + width < n ? lo + width : n;
            uint32_t hi = lo + 2 * width < n ? lo + 2 * width : n;
            uint32_t i = lo, j = mid, k = lo;
            while (i < mid && j < hi) {
                if (sort_compare(fn, a->items[j], a->items[i]) < 0) tmp[k++] = a->items[j++];
                else tmp[k++] = a->items[i++];
                if (js_rt.ctl) break;
            }
            while (i < mid) tmp[k++] = a->items[i++];
            while (j < hi) tmp[k++] = a->items[j++];
            for (k = lo; k < hi; k++) a->items[k] = tmp[k];
        }
    }
    free(tmp);
    return self;
}

static js_val array_fill(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)callee;
    js_obj* a = this_array(self);
    if (!a) return js_undef();
    long len = (long)a->length;
    long start = relative_index(ARG(1), len, 0), end = relative_index(ARG(2), len, len);
    for (long i = start; i < end; i++) a->items[i] = ARG(0);
    return self;
}

static void flatten_into(js_obj* out, js_obj* a, int depth) {
    for (uint32_t i = 0; i < a->length; i++) {
        js_val v = a->items[i];
        if (depth > 0 && v.type == JS_OBJ && v.u.obj->cls == CLS_ARRAY) flatten_into(out, v.u.obj, depth - 1);
        else js_array_push(out, v);
    }
}

static js_val array_flat(js_val self, js_val* args, int argc, js_obj* callee) {
    js_obj* a = this_array(self);
    if (!a) return js_undef();
    if (callee->magic) {   /* flatMap */
        js_val mapped = array_iterate(self, args, argc, js_get_c(js_obj_val(js_array_proto), "map").u.obj);
        if (mapped.type != JS_OBJ) return mapped;
        a = mapped.u.obj;
    }
    int depth = callee->magic ? 1 : ARG(0).type == JS_UNDEF ? 1 : (int)js_to_number(ARG(0));
    js_obj* out = js_new_array(a->length);
    flatten_into(out, a, depth);
    return js_obj_val(out);
}

static js_val array_at(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)callee;
    if (self.type == JS_STR) {
        long len = (long)cp_length(self.u.str);
        long i = (long)js_to_number(ARG(0));
        if (i < 0) i += len;
        if (i < 0 || i >= len) return js_undef();
        return js_get_index(self, js_num((double)i));
    }
    js_obj* a = this_array(self);
    if (!a) return js_undef();
    long i = (long)js_to_number(ARG(0));
    if (i < 0) i += (long)a->length;
    if (i < 0 || i >= (long)a->length) return js_undef();
    return a->items[i];
}

static js_val array_keys(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)args; (void)argc;
    js_obj* a = this_array(self);
    if (!a) return js_undef();
    js_obj* out = js_new_array(a->length);
    for (uint32_t i = 0; i < a->length; i++) {
        if (callee->magic == 0) {
            js_array_push(out, js_num(i));
        } else if (callee->magic == 1) {
            js_array_push(out, a->items[i]);
        } else {
            js_obj* pair = js_new_array(2);
            js_array_push(pair, js_num(i));
            js_array_push(pair, a->items[i]);
            js_array_push(out, js_obj_val(pair));
        }
    }
    return js_obj_val(out);
}

static js_val array_to_string(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)args; (void)argc; (void)callee;
    if (self.type != JS_OBJ || self.u.obj->cls != CLS_ARRAY) return object_to_string(self, NULL, 0, callee);
    return array_join(self, NULL, 0, callee);
}

static js_val return_this(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)args; (void)argc; (void)callee;
    return self;
}

/* ====================================================================== */
/* Strings                                                                */
/* ====================================================================== */

static js_val string_ctor(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)self; (void)callee;
    if (!argc) return js_str_val(js_intern_c(""));
    if (args[0].type == JS_OBJ && args[0].u.obj->cls == CLS_ERROR) {
        js_val fn = js_get_c(args[0], "toString");
        if (js_is_callable(fn)) return js_call(fn, args[0], NULL, 0);
    }
    return js_str_val(js_to_string(args[0]));
}

static js_val string_from_char_code(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)self; (void)callee;
    char buffer[1024];
    size_t used = 0;
    for (int i = 0; i < argc && used + 4 < sizeof(buffer); i++) {
        uint32_t cp = (uint32_t)js_to_number(args[i]) & (callee->magic ? 0x1FFFFF : 0xFFFF);
        /* combine surrogate pairs */
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < argc) {
            uint32_t low = (uint32_t)js_to_number(args[i + 1]) & 0xFFFF;
            if (low >= 0xDC00 && low <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                i++;
            }
        }
        used += mx_utf8_encode(cp, buffer + used);
    }
    return js_str_val(js_new_str(buffer, used));
}

static js_val string_char_at(js_val self, js_val* args, int argc, js_obj* callee) {
    js_str* s = this_string(self);
    double n = js_to_number(ARG(0));
    if (is_nan(n)) n = 0;
    int ascii = str_is_ascii(s);
    long len = ascii ? (long)s->length : (long)cp_length(s);
    if (n < 0 || n >= len) {
        if (callee->magic == 0) return js_str_val(js_intern_c(""));
        return callee->magic == 1 ? js_num(nan_value()) : js_undef();
    }
    uint32_t start = cp_to_byte(s, (uint32_t)n, ascii);
    uint32_t i = start;
    uint32_t cp = decode_utf8(s->data, s->length, &i);
    if (callee->magic == 0) return js_str_val(js_new_str(s->data + start, i - start));
    if (callee->magic == 1 && cp > 0xFFFF) return js_num(0xD800 + ((cp - 0x10000) >> 10));
    return js_num(cp);
}

static js_val string_index_of(js_val self, js_val* args, int argc, js_obj* callee) {
    js_str* s = this_string(self);
    js_str* needle = js_to_string(ARG(0));
    int ascii = str_is_ascii(s);
    int mode = callee->magic;   /* 0 indexOf 1 lastIndexOf 2 includes 3 startsWith 4 endsWith */
    if (mode == 1) {
        long best = -1;
        for (uint32_t i = 0; i + needle->length <= s->length; i++)
            if (!memcmp(s->data + i, needle->data, needle->length)) best = (long)i;
        return js_num(best < 0 ? -1 : byte_to_cp(s, (uint32_t)best, ascii));
    }
    if (mode == 3) {
        uint32_t from = cp_to_byte(s, (uint32_t)(argc > 1 ? js_to_number(ARG(1)) : 0), ascii);
        return js_bool(from + needle->length <= s->length &&
                       !memcmp(s->data + from, needle->data, needle->length));
    }
    if (mode == 4) {
        uint32_t end = argc > 1 ? cp_to_byte(s, (uint32_t)js_to_number(ARG(1)), ascii) : s->length;
        return js_bool(needle->length <= end &&
                       !memcmp(s->data + end - needle->length, needle->data, needle->length));
    }
    uint32_t from = cp_to_byte(s, (uint32_t)(argc > 1 && js_to_number(ARG(1)) > 0 ? js_to_number(ARG(1)) : 0), ascii);
    long found = find_bytes(s, needle, from);
    if (mode == 2) return js_bool(found >= 0);
    return js_num(found < 0 ? -1 : byte_to_cp(s, (uint32_t)found, ascii));
}

static js_val string_slice(js_val self, js_val* args, int argc, js_obj* callee) {
    js_str* s = this_string(self);
    int ascii = str_is_ascii(s);
    long len = ascii ? (long)s->length : (long)cp_length(s);
    long start, end;
    if (callee->magic == 0) {   /* slice */
        start = relative_index(ARG(0), len, 0);
        end = relative_index(ARG(1), len, len);
    } else if (callee->magic == 1) {   /* substring */
        double a = js_to_number(ARG(0)), b = ARG(1).type == JS_UNDEF ? len : js_to_number(ARG(1));
        if (is_nan(a) || a < 0) a = 0;
        if (is_nan(b) || b < 0) b = 0;
        if (a > len) a = len;
        if (b > len) b = len;
        start = (long)(a < b ? a : b);
        end = (long)(a < b ? b : a);
    } else {   /* substr */
        start = relative_index(ARG(0), len, 0);
        double count = ARG(1).type == JS_UNDEF ? len - start : js_to_number(ARG(1));
        if (count < 0) count = 0;
        end = start + (long)count > len ? len : start + (long)count;
    }
    if (end <= start) return js_str_val(js_intern_c(""));
    uint32_t b0 = cp_to_byte(s, (uint32_t)start, ascii), b1 = cp_to_byte(s, (uint32_t)end, ascii);
    return js_str_val(js_new_str(s->data + b0, b1 - b0));
}

static js_val string_case(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)args; (void)argc;
    js_str* s = this_string(self);
    js_str* out = js_new_str(s->data, s->length);
    for (uint32_t i = 0; i < out->length; i++) {
        char c = out->data[i];
        if (callee->magic && c >= 'a' && c <= 'z') out->data[i] = (char)(c - 32);
        else if (!callee->magic && c >= 'A' && c <= 'Z') out->data[i] = (char)(c + 32);
    }
    out->hash = 0;
    js_str* fixed = js_new_str(out->data, out->length);
    return js_str_val(fixed);
}

static int js_space_byte(const char* p, const char* end, int* width) {
    unsigned char c = (unsigned char)*p;
    *width = 1;
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f') return 1;
    if (c == 0xC2 && p + 1 < end && (unsigned char)p[1] == 0xA0) { *width = 2; return 1; }
    if (c == 0xEF && p + 2 < end && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) { *width = 3; return 1; }
    if (c == 0xE2 && p + 2 < end && (unsigned char)p[1] == 0x80 &&
        ((unsigned char)p[2] <= 0x8A || (unsigned char)p[2] == 0xA8 || (unsigned char)p[2] == 0xA9 ||
         (unsigned char)p[2] == 0xAF)) { *width = 3; return 1; }
    return 0;
}

static js_val string_trim(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)args; (void)argc;
    js_str* s = this_string(self);
    const char* start = s->data;
    const char* end = s->data + s->length;
    int w;
    if (callee->magic != 2)
        while (start < end && js_space_byte(start, end, &w)) start += w;
    if (callee->magic != 1)
        while (end > start) {
            const char* q = end - 1;
            while (q > start && ((unsigned char)*q & 0xC0) == 0x80) q--;
            if (!js_space_byte(q, end, &w)) break;
            end = q;
        }
    return js_str_val(js_new_str(start, (size_t)(end - start)));
}

static js_val string_pad(js_val self, js_val* args, int argc, js_obj* callee) {
    js_str* s = this_string(self);
    long target = (long)js_to_number(ARG(0));
    js_str* fill = ARG(1).type == JS_UNDEF ? js_intern_c(" ") : js_to_string(ARG(1));
    long len = (long)cp_length(s);
    if (target <= len || !fill->length || target > 100000) return js_str_val(s);
    long need = target - len;
    char* buffer = (char*)malloc((size_t)need * fill->length + s->length + 1);
    if (!buffer) return js_str_val(s);
    size_t used = 0;
    if (callee->magic) {
        memcpy(buffer, s->data, s->length);
        used = s->length;
    }
    for (long i = 0; i < need; i++) buffer[used++] = fill->data[i % fill->length];
    if (!callee->magic) {
        memcpy(buffer + used, s->data, s->length);
        used += s->length;
    }
    js_str* out = js_new_str(buffer, used);
    free(buffer);
    return js_str_val(out);
}

static js_val string_repeat(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)callee;
    js_str* s = this_string(self);
    double n = js_to_number(ARG(0));
    if (n < 0 || is_inf(n)) return js_throw_error("RangeError", "Invalid count value");
    size_t count = (size_t)n;
    if (count * s->length > 8u * 1024u * 1024u) return js_throw_error("RangeError", "Invalid string length");
    char* buffer = (char*)malloc(count * s->length + 1);
    if (!buffer) return js_str_val(js_intern_c(""));
    for (size_t i = 0; i < count; i++) memcpy(buffer + i * s->length, s->data, s->length);
    js_str* out = js_new_str(buffer, count * s->length);
    free(buffer);
    return js_str_val(out);
}

static js_val string_split(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)callee;
    js_str* s = this_string(self);
    js_obj* out = js_new_array(0);
    uint32_t limit = ARG(1).type == JS_UNDEF ? 0xFFFFFFFFu : js_to_uint32(ARG(1));
    if (ARG(0).type == JS_UNDEF) {
        js_array_push(out, js_str_val(s));
        return js_obj_val(out);
    }
    if (ARG(0).type == JS_OBJ && ARG(0).u.obj->cls == CLS_REGEXP) {
        re_prog* prog = (re_prog*)ARG(0).u.obj->data;
        uint32_t pos = 0, last = 0;
        int caps[20];
        if (!s->length) {
            if (!re_search(prog, s, 0, caps)) js_array_push(out, js_str_val(s));
            return js_obj_val(out);
        }
        while (pos < s->length && out->length < limit) {
            if (!re_search(prog, s, pos, caps)) break;
            uint32_t ms = (uint32_t)caps[0], me = (uint32_t)caps[1];
            if (me == ms) {
                if (ms >= s->length) break;
                if (ms == last) {
                    pos = ms + 1;
                    while (pos < s->length && ((unsigned char)s->data[pos] & 0xC0) == 0x80) pos++;
                    if (ms == 0) continue;
                }
            }
            if (ms == s->length) break;
            js_array_push(out, js_str_val(js_new_str(s->data + last, ms - last)));
            for (int g = 1; g <= prog->groups && g < 10; g++)
                js_array_push(out, caps[g * 2] >= 0 ? js_str_val(js_new_str(s->data + caps[g * 2],
                                                      (size_t)(caps[g * 2 + 1] - caps[g * 2])))
                                                    : js_undef());
            last = me;
            pos = me > ms ? me : ms + 1;
        }
        if (out->length < limit) js_array_push(out, js_str_val(js_new_str(s->data + last, s->length - last)));
        return js_obj_val(out);
    }
    js_str* sep = js_to_string(ARG(0));
    if (!sep->length) {
        uint32_t i = 0;
        while (i < s->length && out->length < limit) {
            uint32_t start = i++;
            while (i < s->length && ((unsigned char)s->data[i] & 0xC0) == 0x80) i++;
            js_array_push(out, js_str_val(js_new_str(s->data + start, i - start)));
        }
        return js_obj_val(out);
    }
    uint32_t start = 0;
    for (;;) {
        if (out->length >= limit) break;
        long found = find_bytes(s, sep, start);
        if (found < 0) {
            js_array_push(out, js_str_val(js_new_str(s->data + start, s->length - start)));
            break;
        }
        js_array_push(out, js_str_val(js_new_str(s->data + start, (size_t)found - start)));
        start = (uint32_t)found + sep->length;
    }
    return js_obj_val(out);
}

/* Expands $&, $1.., $$, $<name> in a replacement string. */
static void append_replacement(char** buffer, size_t* used, size_t* capacity, js_str* rep,
                               const js_str* s, int* caps, int groups, re_prog* prog) {
    for (uint32_t i = 0; i < rep->length; i++) {
        const char* piece = rep->data + i;
        size_t piece_length = 1;
        if (rep->data[i] == '$' && i + 1 < rep->length) {
            char c = rep->data[i + 1];
            if (c == '$') {
                i++;
            } else if (c == '&') {
                piece = s->data + caps[0];
                piece_length = (size_t)(caps[1] - caps[0]);
                i++;
            } else if (c == '`') {
                piece = s->data;
                piece_length = (size_t)caps[0];
                i++;
            } else if (c == '\'') {
                piece = s->data + caps[1];
                piece_length = s->length - (uint32_t)caps[1];
                i++;
            } else if (c >= '1' && c <= '9' && c - '0' <= groups) {
                int g = c - '0';
                if (i + 2 < rep->length && rep->data[i + 2] >= '0' && rep->data[i + 2] <= '9' &&
                    g * 10 + (rep->data[i + 2] - '0') <= groups && g * 10 + (rep->data[i + 2] - '0') < 10) {
                    g = g * 10 + (rep->data[i + 2] - '0');
                    i++;
                }
                piece = caps[g * 2] >= 0 ? s->data + caps[g * 2] : "";
                piece_length = caps[g * 2] >= 0 ? (size_t)(caps[g * 2 + 1] - caps[g * 2]) : 0;
                i++;
            } else if (c == '<' && prog) {
                const char* close = mx_memchr(rep->data + i, '>', rep->length - i);
                if (close) {
                    js_str* name = js_intern(rep->data + i + 2, (size_t)(close - rep->data - i - 2));
                    piece_length = 0;
                    for (int g = 1; g < 10; g++)
                        if (prog->names[g] == name && caps[g * 2] >= 0) {
                            piece = s->data + caps[g * 2];
                            piece_length = (size_t)(caps[g * 2 + 1] - caps[g * 2]);
                        }
                    i = (uint32_t)(close - rep->data);
                }
            }
        }
        if (*used + piece_length + 1 > *capacity) {
            *capacity = (*used + piece_length + 1) * 2;
            *buffer = (char*)realloc(*buffer, *capacity);
            if (!*buffer) return;
        }
        memcpy(*buffer + *used, piece, piece_length);
        *used += piece_length;
    }
}

static js_val string_replace(js_val self, js_val* args, int argc, js_obj* callee) {
    js_str* s = this_string(self);
    js_val pattern = ARG(0), replacement = ARG(1);
    int all = callee->magic;
    int fn = js_is_callable(replacement);
    js_str* rep = fn ? NULL : js_to_string(replacement);
    size_t capacity = s->length + 64, used = 0;
    char* buffer = (char*)malloc(capacity);
    if (!buffer) return js_str_val(s);
    uint32_t pos = 0, last = 0;
    re_prog* prog = NULL;
    js_str* needle = NULL;
    if (pattern.type == JS_OBJ && pattern.u.obj->cls == CLS_REGEXP) {
        prog = (re_prog*)pattern.u.obj->data;
        if (prog->global) all = 1;
        else all = 0;
    } else {
        needle = js_to_string(pattern);
    }
    int ascii = str_is_ascii(s);
    for (;;) {
        int caps[20];
        for (int i = 0; i < 20; i++) caps[i] = -1;
        if (prog) {
            if (pos > s->length || !re_search(prog, s, pos, caps)) break;
        } else {
            long found = find_bytes(s, needle, pos);
            if (found < 0) break;
            caps[0] = (int)found;
            caps[1] = (int)(found + needle->length);
        }
        /* text before the match */
        size_t before = (size_t)caps[0] - last;
        if (used + before + 1 > capacity) {
            capacity = (used + before + 1) * 2;
            buffer = (char*)realloc(buffer, capacity);
            if (!buffer) return js_str_val(s);
        }
        memcpy(buffer + used, s->data + last, before);
        used += before;
        if (fn) {
            int groups = prog ? prog->groups : 0;
            js_val* call_args = (js_val*)js_alloc(sizeof(js_val) * (size_t)(groups + 3));
            int count = 0;
            call_args[count++] = js_str_val(js_new_str(s->data + caps[0], (size_t)(caps[1] - caps[0])));
            for (int g = 1; g <= groups && g < 10; g++)
                call_args[count++] = caps[g * 2] >= 0 ? js_str_val(js_new_str(s->data + caps[g * 2],
                                                                (size_t)(caps[g * 2 + 1] - caps[g * 2])))
                                                      : js_undef();
            call_args[count++] = js_num(byte_to_cp(s, (uint32_t)caps[0], ascii));
            call_args[count++] = js_str_val(s);
            js_val r = js_call(replacement, js_undef(), call_args, count);
            if (js_rt.ctl) {
                free(buffer);
                return js_undef();
            }
            js_str* rs = js_to_string(r);
            if (used + rs->length + 1 > capacity) {
                capacity = (used + rs->length + 1) * 2;
                buffer = (char*)realloc(buffer, capacity);
                if (!buffer) return js_str_val(s);
            }
            memcpy(buffer + used, rs->data, rs->length);
            used += rs->length;
        } else {
            append_replacement(&buffer, &used, &capacity, rep, s, caps, prog ? prog->groups : 0, prog);
            if (!buffer) return js_str_val(s);
        }
        last = (uint32_t)caps[1];
        pos = caps[1] > caps[0] ? (uint32_t)caps[1] : (uint32_t)caps[1] + 1;
        if (!all) break;
    }
    if (last < s->length) {
        size_t rest = s->length - last;
        if (used + rest + 1 > capacity) {
            capacity = used + rest + 1;
            buffer = (char*)realloc(buffer, capacity);
            if (!buffer) return js_str_val(s);
        }
        memcpy(buffer + used, s->data + last, rest);
        used += rest;
    }
    js_str* out = js_new_str(buffer, used);
    free(buffer);
    if (prog && prog->global) js_set_c(pattern, "lastIndex", js_num(0));
    return js_str_val(out);
}

static js_val to_regexp(js_val v) {
    if (v.type == JS_OBJ && v.u.obj->cls == CLS_REGEXP) return v;
    return js_regexp_create(v.type == JS_UNDEF ? js_intern_c("(?:)") : js_to_string(v), js_intern_c(""));
}

static js_val string_match(js_val self, js_val* args, int argc, js_obj* callee) {
    js_str* s = this_string(self);
    js_val re = to_regexp(ARG(0));
    if (js_rt.ctl) return js_undef();
    re_prog* prog = (re_prog*)re.u.obj->data;
    int mode = callee->magic;   /* 0 match, 1 matchAll, 2 search */
    if (mode == 2) {
        int caps[20];
        if (!re_search(prog, s, 0, caps)) return js_num(-1);
        return js_num(byte_to_cp(s, (uint32_t)caps[0], str_is_ascii(s)));
    }
    if (!prog->global && mode == 0) return regexp_exec_at(re.u.obj, s, 0, NULL);
    js_obj* out = js_new_array(0);
    uint32_t pos = 0;
    while (pos <= s->length) {
        int caps[20];
        js_val m = regexp_exec_at(re.u.obj, s, pos, caps);
        if (m.type != JS_OBJ) break;
        js_array_push(out, mode == 1 ? m : m.u.obj->items[0]);
        pos = caps[1] > caps[0] ? (uint32_t)caps[1] : (uint32_t)caps[1] + 1;
    }
    if (mode == 0 && !out->length) return js_null();
    return js_obj_val(out);
}

static js_val string_concat(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)callee;
    js_str* s = this_string(self);
    for (int i = 0; i < argc; i++) s = js_concat(s, js_to_string(args[i]));
    return js_str_val(s);
}

static js_val string_locale_compare(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)callee;
    js_str* a = this_string(self);
    js_str* b = js_to_string(ARG(0));
    /* case-insensitive first, like most locales */
    for (uint32_t i = 0; i < a->length && i < b->length; i++) {
        char x = mx_lower(a->data[i]), y = mx_lower(b->data[i]);
        if (x != y) return js_num(x < y ? -1 : 1);
    }
    if (a->length != b->length) return js_num(a->length < b->length ? -1 : 1);
    int c = strcmp(a->data, b->data);
    return js_num(c < 0 ? 1 : c > 0 ? -1 : 0);
}

static js_val string_to_string(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)args; (void)argc; (void)callee;
    return js_str_val(this_string(self));
}

/* ====================================================================== */
/* Numbers / Booleans                                                     */
/* ====================================================================== */

static double this_number(js_val self) {
    if (self.type == JS_OBJ && self.u.obj->cls == CLS_BOXED) return js_to_number(self.u.obj->primitive);
    return js_to_number(self);
}

static js_val number_ctor(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)self; (void)callee;
    return js_num(argc ? js_to_number(args[0]) : 0);
}

static js_val number_to_string(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)callee;
    int radix = ARG(0).type == JS_UNDEF ? 10 : (int)js_to_number(ARG(0));
    if (radix < 2 || radix > 36) return js_throw_error("RangeError", "toString() radix must be between 2 and 36");
    return js_str_val(js_number_to_string(this_number(self), radix));
}

static js_val number_to_fixed(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)callee;
    int digits = (int)js_to_number(ARG(0));
    if (digits < 0 || digits > 100) return js_throw_error("RangeError", "toFixed() digits argument must be between 0 and 100");
    return js_str_val(format_fixed(this_number(self), digits));
}

static js_val number_to_precision(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)callee;
    double v = this_number(self);
    if (ARG(0).type == JS_UNDEF || v == 0 || is_nan(v) || is_inf(v)) return js_str_val(js_number_to_string(v, 10));
    int p = (int)js_to_number(ARG(0));
    if (p < 1 || p > 21) return js_throw_error("RangeError", "toPrecision() argument must be between 1 and 21");
    double a = v < 0 ? -v : v;
    int e = 0;
    double t = a;
    while (t >= 10) { t /= 10; e++; }
    while (t < 1) { t *= 10; e--; }
    int decimals = p - 1 - e;
    if (decimals < 0) decimals = 0;
    return js_str_val(format_fixed(v, decimals));
}

static js_val number_to_locale(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)callee;
    double v = this_number(self);
    int max = 3;
    if (ARG(1).type == JS_OBJ) {
        js_val m = js_get_c(ARG(1), "maximumFractionDigits");
        if (m.type == JS_NUM) max = (int)m.u.num;
        js_val mn = js_get_c(ARG(1), "minimumFractionDigits");
        if (mn.type == JS_NUM && mn.u.num > max) max = (int)mn.u.num;
    }
    js_str* fixed = format_fixed(v, max);
    /* strip trailing zeros of the fraction */
    char text[400];
    mx_copy(text, sizeof(text), fixed->data);
    char* dot = strchr(text, '.');
    int min = 0;
    if (ARG(1).type == JS_OBJ) {
        js_val mn = js_get_c(ARG(1), "minimumFractionDigits");
        if (mn.type == JS_NUM) min = (int)mn.u.num;
    }
    if (dot) {
        char* end = text + strlen(text);
        while (end > dot + 1 + min && end[-1] == '0') *--end = '\0';
        if (end == dot + 1) *dot = '\0';
    }
    char out[440];
    size_t used = 0;
    char* p = text;
    if (*p == '-') out[used++] = *p++;
    char* int_end = strchr(p, '.');
    size_t int_len = int_end ? (size_t)(int_end - p) : strlen(p);
    for (size_t i = 0; i < int_len; i++) {
        if (i && (int_len - i) % 3 == 0) out[used++] = ',';
        out[used++] = p[i];
    }
    if (int_end) {
        mx_copy(out + used, sizeof(out) - used, int_end);
        used += strlen(int_end);
    }
    out[used] = '\0';
    return js_str_val(js_cstr(out));
}

static js_val number_value_of(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)args; (void)argc; (void)callee;
    return js_num(this_number(self));
}

static js_val number_is(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)self;
    js_val v = ARG(0);
    if (v.type != JS_NUM) return js_bool(0);
    double n = v.u.num;
    switch (callee->magic) {
    case 0: return js_bool(is_nan(n));
    case 1: return js_bool(!is_nan(n) && !is_inf(n));
    case 2: return js_bool(!is_nan(n) && !is_inf(n) && n == m_floor(n));
    default: return js_bool(!is_nan(n) && n == m_floor(n) && m_fabs(n) <= 9007199254740991.0);
    }
}

static js_val global_is_nan(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)self;
    double n = js_to_number(ARG(0));
    return js_bool(callee->magic ? (!is_nan(n) && !is_inf(n)) : is_nan(n));
}

static js_val global_parse_int(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)self; (void)callee;
    js_str* s = js_to_string(ARG(0));
    int radix = ARG(1).type == JS_UNDEF ? 0 : (int)js_to_number(ARG(1));
    const char* p = s->data;
    while (mx_is_space(*p)) p++;
    int negative = 0;
    if (*p == '+' || *p == '-') negative = *p++ == '-';
    if ((radix == 0 || radix == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
        radix = 16;
    }
    if (!radix) radix = 10;
    if (radix < 2 || radix > 36) return js_num(nan_value());
    double value = 0;
    int digits = 0;
    for (;; p++) {
        int d = *p >= '0' && *p <= '9' ? *p - '0' : (*p | 0x20) >= 'a' && (*p | 0x20) <= 'z' ? (*p | 0x20) - 'a' + 10 : 99;
        if (d >= radix) break;
        value = value * radix + d;
        digits++;
    }
    if (!digits) return js_num(nan_value());
    return js_num(negative ? -value : value);
}

static js_val global_parse_float(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)self; (void)callee;
    js_str* s = js_to_string(ARG(0));
    const char* p = s->data;
    while (mx_is_space(*p)) p++;
    const char* start = p;
    if (*p == '+' || *p == '-') p++;
    if (!strncmp(p, "Infinity", 8)) return js_num(*start == '-' ? -inf_value() : inf_value());
    while (*p >= '0' && *p <= '9') p++;
    if (*p == '.') {
        p++;
        while (*p >= '0' && *p <= '9') p++;
    }
    if ((*p == 'e' || *p == 'E') && (p[1] == '-' || p[1] == '+' || (p[1] >= '0' && p[1] <= '9'))) {
        const char* q = p + 1;
        if (*q == '-' || *q == '+') q++;
        if (*q >= '0' && *q <= '9') {
            p = q;
            while (*p >= '0' && *p <= '9') p++;
        }
    }
    if (p == start) return js_num(nan_value());
    return js_num(js_string_to_number(js_new_str(start, (size_t)(p - start))));
}

static js_val boolean_ctor(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)self; (void)callee;
    return js_bool(js_truthy(ARG(0)));
}

static js_val boolean_to_string(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)args; (void)argc; (void)callee;
    js_val v = self.type == JS_OBJ && self.u.obj->cls == CLS_BOXED ? self.u.obj->primitive : self;
    return js_str_val(js_intern_c(js_truthy(v) ? "true" : "false"));
}

/* ====================================================================== */
/* Errors                                                                 */
/* ====================================================================== */

static js_val error_ctor(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)self;
    const char* type = callee->name->data;
    js_obj* e = js_make_error(type, ARG(0).type == JS_UNDEF ? "" : js_to_string(ARG(0))->data);
    if (ARG(1).type == JS_OBJ) js_define(e, "cause", js_get_c(ARG(1), "cause"), PROP_HIDDEN);
    return js_obj_val(e);
}

static js_val error_to_string(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)args; (void)argc; (void)callee;
    js_str* name = js_to_string(js_get_c(self, "name"));
    js_val m = js_get_c(self, "message");
    js_str* message = m.type == JS_UNDEF ? js_intern_c("") : js_to_string(m);
    if (!message->length) return js_str_val(name);
    if (!name->length) return js_str_val(message);
    return js_str_val(js_concat(js_concat(name, js_intern_c(": ")), message));
}

/* ====================================================================== */
/* JSON                                                                   */
/* ====================================================================== */

typedef struct {
    const char* p;
    const char* end;
    int error;
} json_parser;

static void json_ws(json_parser* jp) {
    while (jp->p < jp->end && mx_is_space(*jp->p)) jp->p++;
}

static js_val json_value(json_parser* jp, int depth);

static js_str* json_string(json_parser* jp) {
    const char* start = ++jp->p;
    int escaped = 0;
    while (jp->p < jp->end && *jp->p != '"') {
        if (*jp->p == '\\') {
            escaped = 1;
            jp->p++;
        }
        jp->p++;
    }
    if (jp->p >= jp->end) {
        jp->error = 1;
        return js_intern_c("");
    }
    js_str* s;
    if (escaped) {
        extern js_str* js_decode_escapes(const char* text, size_t length);
        s = js_decode_escapes(start, (size_t)(jp->p - start));
    } else {
        s = js_new_str(start, (size_t)(jp->p - start));
    }
    jp->p++;
    return s;
}

static js_val json_value(json_parser* jp, int depth) {
    json_ws(jp);
    if (jp->p >= jp->end || depth > 256) {
        jp->error = 1;
        return js_undef();
    }
    char c = *jp->p;
    if (c == '{') {
        jp->p++;
        js_obj* o = js_new_object(js_object_proto);
        json_ws(jp);
        if (jp->p < jp->end && *jp->p == '}') {
            jp->p++;
            return js_obj_val(o);
        }
        for (;;) {
            json_ws(jp);
            if (jp->p >= jp->end || *jp->p != '"') {
                jp->error = 1;
                return js_undef();
            }
            js_str* key = json_string(jp);
            json_ws(jp);
            if (jp->p >= jp->end || *jp->p != ':') {
                jp->error = 1;
                return js_undef();
            }
            jp->p++;
            js_val v = json_value(jp, depth + 1);
            if (jp->error) return js_undef();
            js_set(js_obj_val(o), js_intern(key->data, key->length), v);
            json_ws(jp);
            if (jp->p < jp->end && *jp->p == ',') {
                jp->p++;
                continue;
            }
            if (jp->p < jp->end && *jp->p == '}') {
                jp->p++;
                return js_obj_val(o);
            }
            jp->error = 1;
            return js_undef();
        }
    }
    if (c == '[') {
        jp->p++;
        js_obj* a = js_new_array(0);
        json_ws(jp);
        if (jp->p < jp->end && *jp->p == ']') {
            jp->p++;
            return js_obj_val(a);
        }
        for (;;) {
            js_val v = json_value(jp, depth + 1);
            if (jp->error) return js_undef();
            js_array_push(a, v);
            json_ws(jp);
            if (jp->p < jp->end && *jp->p == ',') {
                jp->p++;
                continue;
            }
            if (jp->p < jp->end && *jp->p == ']') {
                jp->p++;
                return js_obj_val(a);
            }
            jp->error = 1;
            return js_undef();
        }
    }
    if (c == '"') return js_str_val(json_string(jp));
    if (!strncmp(jp->p, "true", 4)) { jp->p += 4; return js_bool(1); }
    if (!strncmp(jp->p, "false", 5)) { jp->p += 5; return js_bool(0); }
    if (!strncmp(jp->p, "null", 4)) { jp->p += 4; return js_null(); }
    const char* start = jp->p;
    if (*jp->p == '-') jp->p++;
    while (jp->p < jp->end && ((*jp->p >= '0' && *jp->p <= '9') || *jp->p == '.' || *jp->p == 'e' ||
                               *jp->p == 'E' || *jp->p == '+' || *jp->p == '-'))
        jp->p++;
    if (jp->p == start) {
        jp->error = 1;
        return js_undef();
    }
    return js_num(js_string_to_number(js_new_str(start, (size_t)(jp->p - start))));
}

static js_val json_parse(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)self; (void)callee;
    js_str* s = js_to_string(ARG(0));
    json_parser jp = {s->data, s->data + s->length, 0};
    js_val v = json_value(&jp, 0);
    json_ws(&jp);
    if (jp.error || jp.p != jp.end) return js_throw_error("SyntaxError", "Unexpected token in JSON");
    return v;
}

typedef struct {
    char* data;
    size_t used, capacity;
} sbuf;

static void sb_add(sbuf* b, const char* text, size_t length) {
    if (b->used + length + 1 > b->capacity) {
        size_t capacity = b->capacity ? b->capacity * 2 : 256;
        while (capacity < b->used + length + 1) capacity *= 2;
        char* grown = (char*)realloc(b->data, capacity);
        if (!grown) return;
        b->data = grown;
        b->capacity = capacity;
    }
    memcpy(b->data + b->used, text, length);
    b->used += length;
    b->data[b->used] = '\0';
}

static void sb_cstr(sbuf* b, const char* text) {
    sb_add(b, text, strlen(text));
}

static void json_quote(sbuf* b, const js_str* s) {
    sb_add(b, "\"", 1);
    for (uint32_t i = 0; i < s->length; i++) {
        unsigned char c = (unsigned char)s->data[i];
        switch (c) {
        case '"': sb_add(b, "\\\"", 2); break;
        case '\\': sb_add(b, "\\\\", 2); break;
        case '\n': sb_add(b, "\\n", 2); break;
        case '\r': sb_add(b, "\\r", 2); break;
        case '\t': sb_add(b, "\\t", 2); break;
        case '\b': sb_add(b, "\\b", 2); break;
        case '\f': sb_add(b, "\\f", 2); break;
        default:
            if (c < 0x20) {
                char hex[8];
                static const char digits[] = "0123456789abcdef";
                hex[0] = '\\'; hex[1] = 'u'; hex[2] = '0'; hex[3] = '0';
                hex[4] = digits[c >> 4]; hex[5] = digits[c & 15];
                sb_add(b, hex, 6);
            } else {
                sb_add(b, (const char*)&s->data[i], 1);
            }
        }
    }
    sb_add(b, "\"", 1);
}

static void json_indent(sbuf* b, const js_str* gap, int depth) {
    if (!gap || !gap->length) return;
    sb_add(b, "\n", 1);
    for (int i = 0; i < depth; i++) sb_add(b, gap->data, gap->length);
}

static int json_write(sbuf* b, js_val v, const js_str* gap, int depth, js_val replacer) {
    if (depth > 64) {
        js_throw_error("TypeError", "Converting circular structure to JSON");
        return 0;
    }
    if (v.type == JS_OBJ) {
        js_val to_json = js_get_c(v, "toJSON");
        if (js_is_callable(to_json)) v = js_call(to_json, v, NULL, 0);
        if (js_rt.ctl) return 0;
    }
    if (v.type == JS_OBJ && v.u.obj->cls == CLS_BOXED) v = v.u.obj->primitive;
    switch (v.type) {
    case JS_NULL: sb_cstr(b, "null"); return 1;
    case JS_BOOL: sb_cstr(b, v.u.boolean ? "true" : "false"); return 1;
    case JS_NUM:
        if (is_nan(v.u.num) || is_inf(v.u.num)) sb_cstr(b, "null");
        else sb_cstr(b, js_number_to_string(v.u.num, 10)->data);
        return 1;
    case JS_STR: json_quote(b, v.u.str); return 1;
    case JS_UNDEF: return 0;
    case JS_OBJ: break;
    }
    js_obj* o = v.u.obj;
    if (js_is_callable(v)) return 0;
    if (o->cls == CLS_ARRAY) {
        sb_add(b, "[", 1);
        for (uint32_t i = 0; i < o->length; i++) {
            if (i) sb_add(b, ",", 1);
            json_indent(b, gap, depth + 1);
            if (!json_write(b, o->items[i], gap, depth + 1, replacer)) {
                if (js_rt.ctl) return 0;
                sb_cstr(b, "null");
            }
        }
        if (o->length) json_indent(b, gap, depth);
        sb_add(b, "]", 1);
        return 1;
    }
    sb_add(b, "{", 1);
    js_obj* keys = js_own_keys(o, 0);
    int first = 1;
    for (uint32_t i = 0; i < keys->length; i++) {
        js_str* key = keys->items[i].u.str;
        js_val value = js_get(v, key);
        if (js_is_callable(replacer)) {
            js_val call_args[2] = {js_str_val(key), value};
            value = js_call(replacer, v, call_args, 2);
        }
        if (value.type == JS_UNDEF || js_is_callable(value)) continue;
        size_t mark = b->used;
        if (!first) sb_add(b, ",", 1);
        json_indent(b, gap, depth + 1);
        json_quote(b, key);
        sb_add(b, gap && gap->length ? ": " : ":", gap && gap->length ? 2 : 1);
        if (!json_write(b, value, gap, depth + 1, replacer)) {
            if (js_rt.ctl) return 0;
            b->used = mark;
            b->data[mark] = '\0';
            continue;
        }
        first = 0;
    }
    if (!first) json_indent(b, gap, depth);
    sb_add(b, "}", 1);
    return 1;
}

static js_val json_stringify(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)self; (void)callee;
    js_str* gap = NULL;
    if (ARG(2).type == JS_NUM) {
        int n = (int)ARG(2).u.num;
        if (n > 10) n = 10;
        char spaces[11] = "          ";
        gap = js_new_str(spaces, n > 0 ? (size_t)n : 0);
    } else if (ARG(2).type == JS_STR) {
        gap = ARG(2).u.str;
    }
    sbuf b = {NULL, 0, 0};
    int ok = json_write(&b, ARG(0), gap, 0, ARG(1));
    if (!ok || js_rt.ctl) {
        free(b.data);
        return js_undef();
    }
    js_str* s = js_new_str(b.data ? b.data : "", b.used);
    free(b.data);
    return js_str_val(s);
}

/* ====================================================================== */
/* Date                                                                   */
/* ====================================================================== */

static double epoch_now_ms(void) {
#ifdef MX_HOST
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)(ts.tv_nsec / 1000000);
#else
    static double base;
    static long base_uptime;
    if (!base) {
        struct { uint16_t year; uint8_t month, day, hour, minute, second; } dt;
        memset(&dt, 0, sizeof(dt));
        if (mos_syscall(SYS_GETTIME, (long)(uintptr_t)&dt, 0, 0) == SYS_SUCCESS && dt.year > 1970) {
            /* days from civil (Howard Hinnant) */
            int y = dt.year - (dt.month <= 2);
            int era = (y >= 0 ? y : y - 399) / 400;
            int yoe = y - era * 400;
            int m = dt.month;
            int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + dt.day - 1;
            int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
            double days = (double)era * 146097 + doe - 719468;
            base = days * 86400000.0 + dt.hour * 3600000.0 + dt.minute * 60000.0 + dt.second * 1000.0;
        } else {
            base = 1.7e12;
        }
        base_uptime = mx_now_ms();
    }
    return base + (double)(mx_now_ms() - base_uptime);
#endif
}

typedef struct {
    int year, month, day, hour, minute, second, ms, weekday;
} civil_t;

static civil_t civil_from_ms(double t) {
    civil_t c;
    double days = m_floor(t / 86400000.0);
    double rem = t - days * 86400000.0;
    long long z = (long long)days + 719468;
    long long era = (z >= 0 ? z : z - 146096) / 146097;
    long long doe = z - era * 146097;
    long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long long y = yoe + era * 400;
    long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    long long mp = (5 * doy + 2) / 153;
    c.day = (int)(doy - (153 * mp + 2) / 5 + 1);
    c.month = (int)(mp < 10 ? mp + 3 : mp - 9);
    c.year = (int)(y + (c.month <= 2));
    long long ms = (long long)rem;
    c.hour = (int)(ms / 3600000);
    c.minute = (int)(ms / 60000 % 60);
    c.second = (int)(ms / 1000 % 60);
    c.ms = (int)(ms % 1000);
    long long wd = ((long long)days + 4) % 7;
    c.weekday = (int)(wd < 0 ? wd + 7 : wd);
    return c;
}

static double ms_from_civil(double year, double month, double day, double h, double mi,
                            double s, double ms) {
    /* month is 0-based here; normalise overflow */
    double ym = year + m_floor(month / 12);
    double mn = month - m_floor(month / 12) * 12;
    int y = (int)ym - (mn < 2);
    int era = (y >= 0 ? y : y - 399) / 400;
    int yoe = y - era * 400;
    int m = (int)mn + 1;
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    double days = (double)era * 146097 + doe - 719468 + (day - 1);
    return days * 86400000.0 + h * 3600000.0 + mi * 60000.0 + s * 1000.0 + ms;
}

static double parse_date(const js_str* s) {
    int parts[7] = {0, 1, 1, 0, 0, 0, 0};
    int count = 0;
    const char* p = s->data;
    while (*p && count < 7) {
        while (*p && !(*p >= '0' && *p <= '9')) {
            if (count >= 3 && (*p == 'Z' || *p == '+')) break;
            p++;
        }
        if (!*p || !(*p >= '0' && *p <= '9')) break;
        int v = 0;
        while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
        parts[count++] = v;
    }
    if (count < 1 || parts[0] < 100) return nan_value();
    return ms_from_civil(parts[0], parts[1] - 1, parts[2], parts[3], parts[4], parts[5], parts[6]);
}

static js_val date_ctor(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)self; (void)callee;
    js_obj* d = js_new_object(js_date_proto);
    d->cls = CLS_DATE;
    if (!argc) {
        d->number = epoch_now_ms();
    } else if (argc == 1) {
        js_val v = args[0];
        if (v.type == JS_OBJ && v.u.obj->cls == CLS_DATE) d->number = v.u.obj->number;
        else if (v.type == JS_STR) d->number = parse_date(v.u.str);
        else d->number = js_to_number(v);
    } else {
        double f[7] = {0, 0, 1, 0, 0, 0, 0};
        for (int i = 0; i < argc && i < 7; i++) f[i] = js_to_number(args[i]);
        if (f[0] >= 0 && f[0] < 100) f[0] += 1900;
        d->number = ms_from_civil(f[0], f[1], f[2], f[3], f[4], f[5], f[6]);
    }
    return js_obj_val(d);
}

static js_val date_now(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)self; (void)args; (void)argc; (void)callee;
    return js_num(m_floor(epoch_now_ms()));
}

static js_val date_parse(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)self; (void)callee;
    return js_num(parse_date(js_to_string(ARG(0))));
}

static js_val date_utc(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)self; (void)callee;
    double f[7] = {1970, 0, 1, 0, 0, 0, 0};
    for (int i = 0; i < argc && i < 7; i++) f[i] = js_to_number(args[i]);
    return js_num(ms_from_civil(f[0], f[1], f[2], f[3], f[4], f[5], f[6]));
}

static js_val date_get(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)args; (void)argc;
    if (self.type != JS_OBJ || self.u.obj->cls != CLS_DATE) return js_throw_error("TypeError", "this is not a Date object.");
    double t = self.u.obj->number;
    if (is_nan(t)) return js_num(t);
    civil_t c = civil_from_ms(t);
    switch (callee->magic) {
    case 0: return js_num(t);
    case 1: return js_num(c.year);
    case 2: return js_num(c.month - 1);
    case 3: return js_num(c.day);
    case 4: return js_num(c.weekday);
    case 5: return js_num(c.hour);
    case 6: return js_num(c.minute);
    case 7: return js_num(c.second);
    case 8: return js_num(c.ms);
    case 9: return js_num(0);   /* timezone offset */
    }
    return js_undef();
}

static js_val date_set(js_val self, js_val* args, int argc, js_obj* callee) {
    if (self.type != JS_OBJ || self.u.obj->cls != CLS_DATE) return js_undef();
    js_obj* d = self.u.obj;
    civil_t c = civil_from_ms(d->number);
    double f[7] = {c.year, c.month - 1, c.day, c.hour, c.minute, c.second, c.ms};
    int start = callee->magic;   /* index of the first field set */
    if (start < 0) {
        d->number = js_to_number(ARG(0));
        return js_num(d->number);
    }
    for (int i = 0; i < argc && start + i < 7; i++) f[start + i] = js_to_number(args[i]);
    d->number = ms_from_civil(f[0], f[1], f[2], f[3], f[4], f[5], f[6]);
    return js_num(d->number);
}

static const char* const day_names[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
static const char* const month_names[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                          "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

static void two(char* out, int v) {
    out[0] = (char)('0' + v / 10 % 10);
    out[1] = (char)('0' + v % 10);
    out[2] = '\0';
}

static js_val date_format(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)args; (void)argc;
    if (self.type != JS_OBJ || self.u.obj->cls != CLS_DATE) return js_throw_error("TypeError", "this is not a Date object.");
    double t = self.u.obj->number;
    if (is_nan(t)) {
        if (callee->magic == 1) return js_throw_error("RangeError", "Invalid time value");
        return js_str_val(js_intern_c("Invalid Date"));
    }
    civil_t c = civil_from_ms(t);
    char out[96] = "";
    char b[8];
    switch (callee->magic) {
    case 1: {   /* toISOString */
        mx_append_int(out, sizeof(out), c.year);
        mx_append(out, sizeof(out), "-");
        two(b, c.month); mx_append(out, sizeof(out), b);
        mx_append(out, sizeof(out), "-");
        two(b, c.day); mx_append(out, sizeof(out), b);
        mx_append(out, sizeof(out), "T");
        two(b, c.hour); mx_append(out, sizeof(out), b);
        mx_append(out, sizeof(out), ":");
        two(b, c.minute); mx_append(out, sizeof(out), b);
        mx_append(out, sizeof(out), ":");
        two(b, c.second); mx_append(out, sizeof(out), b);
        mx_append(out, sizeof(out), ".");
        b[0] = (char)('0' + c.ms / 100); b[1] = (char)('0' + c.ms / 10 % 10);
        b[2] = (char)('0' + c.ms % 10); b[3] = '\0';
        mx_append(out, sizeof(out), b);
        mx_append(out, sizeof(out), "Z");
        break;
    }
    case 2:   /* toDateString */
    case 4: {
        mx_append(out, sizeof(out), day_names[c.weekday]);
        mx_append(out, sizeof(out), " ");
        mx_append(out, sizeof(out), month_names[c.month - 1]);
        mx_append(out, sizeof(out), " ");
        two(b, c.day); mx_append(out, sizeof(out), b);
        mx_append(out, sizeof(out), " ");
        mx_append_int(out, sizeof(out), c.year);
        if (callee->magic == 2) break;
    }
    /* fall through for toString */
    /* fallthrough */
    case 3: {   /* toTimeString */
        if (callee->magic == 4) mx_append(out, sizeof(out), " ");
        two(b, c.hour); mx_append(out, sizeof(out), b);
        mx_append(out, sizeof(out), ":");
        two(b, c.minute); mx_append(out, sizeof(out), b);
        mx_append(out, sizeof(out), ":");
        two(b, c.second); mx_append(out, sizeof(out), b);
        mx_append(out, sizeof(out), " GMT+0000 (Coordinated Universal Time)");
        break;
    }
    case 5:   /* toLocaleDateString */
        mx_append_int(out, sizeof(out), c.month);
        mx_append(out, sizeof(out), "/");
        mx_append_int(out, sizeof(out), c.day);
        mx_append(out, sizeof(out), "/");
        mx_append_int(out, sizeof(out), c.year);
        break;
    case 6:   /* toLocaleTimeString */
    case 7: { /* toLocaleString */
        if (callee->magic == 7) {
            mx_append_int(out, sizeof(out), c.month);
            mx_append(out, sizeof(out), "/");
            mx_append_int(out, sizeof(out), c.day);
            mx_append(out, sizeof(out), "/");
            mx_append_int(out, sizeof(out), c.year);
            mx_append(out, sizeof(out), ", ");
        }
        int h = c.hour % 12;
        mx_append_int(out, sizeof(out), h ? h : 12);
        mx_append(out, sizeof(out), ":");
        two(b, c.minute); mx_append(out, sizeof(out), b);
        mx_append(out, sizeof(out), ":");
        two(b, c.second); mx_append(out, sizeof(out), b);
        mx_append(out, sizeof(out), c.hour < 12 ? " AM" : " PM");
        break;
    }
    }
    return js_str_val(js_cstr(out));
}

/* ====================================================================== */
/* Promises                                                               */
/* ====================================================================== */

typedef struct reaction {
    js_val on_ok, on_err;
    js_obj* derived;       /* promise returned by then() */
    struct reaction* next;
} reaction;

typedef struct {
    int state;             /* 0 pending, 1 fulfilled, 2 rejected */
    js_val value;
    reaction* reactions;
    int handled;
} promise_data;

typedef struct microtask {
    js_val fn;
    js_val arg;
    reaction* r;
    int state;
    struct microtask* next;
} microtask;

static microtask* micro_head;
static microtask* micro_tail;

static void push_micro(microtask* m) {
    m->next = NULL;
    if (micro_tail) micro_tail->next = m;
    else micro_head = m;
    micro_tail = m;
}

void js_enqueue_microtask(js_val fn, js_val arg) {
    microtask* m = (microtask*)js_alloc(sizeof(microtask));
    memset(m, 0, sizeof(*m));
    m->fn = fn;
    m->arg = arg;
    push_micro(m);
}

static js_obj* new_promise(void) {
    js_obj* p = js_new_object(js_promise_proto);
    p->cls = CLS_PROMISE;
    promise_data* d = (promise_data*)js_alloc(sizeof(promise_data));
    memset(d, 0, sizeof(*d));
    p->data = d;
    return p;
}

static void settle(js_obj* p, int state, js_val value);

static void schedule_reaction(reaction* r, int state, js_val value) {
    microtask* m = (microtask*)js_alloc(sizeof(microtask));
    memset(m, 0, sizeof(*m));
    m->r = r;
    m->state = state;
    m->arg = value;
    push_micro(m);
}

static js_val resolve_fn(js_val self, js_val* args, int argc, js_obj* callee);

static void resolve_promise(js_obj* p, js_val value) {
    promise_data* d = (promise_data*)p->data;
    if (d->state) return;
    if (value.type == JS_OBJ && value.u.obj == p) {
        settle(p, 2, js_obj_val(js_make_error("TypeError", "Chaining cycle detected for promise")));
        return;
    }
    if (value.type == JS_OBJ) {
        js_val then = js_get(value, S_then_name());
        if (js_rt.ctl) {
            js_val error = js_rt.value;
            js_rt.ctl = CTL_NONE;
            settle(p, 2, error);
            return;
        }
        if (js_is_callable(then)) {
            js_obj* ok = js_new_native("resolve", resolve_fn, 1);
            ok->data = p;
            ok->magic = 0;
            js_obj* err = js_new_native("reject", resolve_fn, 1);
            err->data = p;
            err->magic = 1;
            js_val pair[2] = {js_obj_val(ok), js_obj_val(err)};
            js_call(then, value, pair, 2);
            if (js_rt.ctl == CTL_THROW) {
                js_val error = js_rt.value;
                js_rt.ctl = CTL_NONE;
                settle(p, 2, error);
            }
            return;
        }
    }
    settle(p, 1, value);
}

static void settle(js_obj* p, int state, js_val value) {
    promise_data* d = (promise_data*)p->data;
    if (d->state) return;
    d->state = state;
    d->value = value;
    for (reaction* r = d->reactions; r; r = r->next) schedule_reaction(r, state, value);
    d->reactions = NULL;
}

static js_val resolve_fn(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)self;
    js_obj* p = (js_obj*)callee->data;
    if (!p) return js_undef();
    callee->data = NULL;
    if (callee->magic) settle(p, 2, ARG(0));
    else resolve_promise(p, ARG(0));
    return js_undef();
}

js_val js_promise_resolved(js_val value, int rejected) {
    js_obj* p = new_promise();
    if (rejected) settle(p, 2, value);
    else resolve_promise(p, value);
    return js_obj_val(p);
}

int js_promise_state(js_val promise, js_val* value) {
    if (promise.type != JS_OBJ || promise.u.obj->cls != CLS_PROMISE) return 1;
    promise_data* d = (promise_data*)promise.u.obj->data;
    *value = d->value;
    if (d->state == 2) d->handled = 1;
    return d->state;
}

js_val js_promise_then(js_val promise, js_val on_ok, js_val on_err) {
    js_obj* p = promise.u.obj;
    promise_data* d = (promise_data*)p->data;
    reaction* r = (reaction*)js_alloc(sizeof(reaction));
    memset(r, 0, sizeof(*r));
    r->on_ok = on_ok;
    r->on_err = on_err;
    r->derived = new_promise();
    d->handled = 1;
    if (d->state) {
        schedule_reaction(r, d->state, d->value);
    } else {
        r->next = d->reactions;
        d->reactions = r;
    }
    return js_obj_val(r->derived);
}

void js_run_microtasks(void) {
    int guard = 0;
    while (micro_head && guard++ < 100000) {
        microtask* m = micro_head;
        micro_head = m->next;
        if (!micro_head) micro_tail = NULL;
        if (m->r) {
            reaction* r = m->r;
            js_val handler = m->state == 1 ? r->on_ok : r->on_err;
            if (js_is_callable(handler)) {
                js_val result = js_call(handler, js_undef(), &m->arg, 1);
                if (js_rt.ctl == CTL_THROW) {
                    js_val error = js_rt.value;
                    js_rt.ctl = CTL_NONE;
                    js_rt.timed_out = 0;
                    if (r->derived) settle(r->derived, 2, error);
                } else if (r->derived) {
                    resolve_promise(r->derived, result);
                }
            } else if (r->derived) {
                if (m->state == 1) resolve_promise(r->derived, m->arg);
                else settle(r->derived, 2, m->arg);
            }
        } else if (js_is_callable(m->fn)) {
            js_call(m->fn, js_undef(), &m->arg, 1);
            if (js_rt.ctl == CTL_THROW) js_report_exception();
            js_rt.ctl = CTL_NONE;
        }
    }
}

static js_val promise_ctor(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)self; (void)callee;
    js_obj* p = new_promise();
    js_val executor = ARG(0);
    if (!js_is_callable(executor)) return js_throw_error("TypeError", "Promise resolver is not a function");
    js_obj* ok = js_new_native("resolve", resolve_fn, 1);
    ok->data = p;
    js_obj* err = js_new_native("reject", resolve_fn, 1);
    err->data = p;
    err->magic = 1;
    js_val pair[2] = {js_obj_val(ok), js_obj_val(err)};
    js_call(executor, js_undef(), pair, 2);
    if (js_rt.ctl == CTL_THROW) {
        js_val error = js_rt.value;
        js_rt.ctl = CTL_NONE;
        settle(p, 2, error);
    }
    return js_obj_val(p);
}

static js_val promise_then(js_val self, js_val* args, int argc, js_obj* callee) {
    if (self.type != JS_OBJ || self.u.obj->cls != CLS_PROMISE) return js_throw_error("TypeError", "not a promise");
    if (callee->magic == 1) return js_promise_then(self, js_undef(), ARG(0));   /* catch */
    if (callee->magic == 2) {   /* finally: run the callback, keep the value */
        return js_promise_then(self, ARG(0), ARG(0));
    }
    return js_promise_then(self, ARG(0), ARG(1));
}

static js_val promise_static_resolve(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)self;
    if (callee->magic == 0 && ARG(0).type == JS_OBJ && ARG(0).u.obj->cls == CLS_PROMISE) return ARG(0);
    return js_promise_resolved(ARG(0), callee->magic);
}

/* Promise.all / allSettled / race / any with synchronous settlement when
 * possible: run microtasks, then combine. */
static js_val promise_combine(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)self;
    js_val list = iterate_to_array(ARG(0));
    if (list.type != JS_OBJ) return js_promise_resolved(js_undef(), 0);
    js_obj* items = list.u.obj;
    js_run_microtasks();
    js_obj* results = js_new_array(items->length);
    int mode = callee->magic;   /* 0 all 1 allSettled 2 race 3 any */
    for (uint32_t i = 0; i < items->length; i++) {
        js_val v = items->items[i], value;
        int state = js_promise_state(v, &value);
        if (state == 0) continue;
        if (v.type != JS_OBJ || v.u.obj->cls != CLS_PROMISE) value = v;
        if (mode == 2) return js_promise_resolved(value, state == 2);
        if (mode == 3 && state == 1) return js_promise_resolved(value, 0);
        if (mode == 0 && state == 2) return js_promise_resolved(value, 1);
        if (mode == 1) {
            js_obj* entry = js_new_object(js_object_proto);
            js_set_c(js_obj_val(entry), "status", js_str_val(js_intern_c(state == 1 ? "fulfilled" : "rejected")));
            js_set_c(js_obj_val(entry), state == 1 ? "value" : "reason", value);
            js_array_set_length(results, i + 1);
            results->items[i] = js_obj_val(entry);
        } else {
            js_array_set_length(results, i + 1 > results->length ? i + 1 : results->length);
            results->items[i] = value;
        }
    }
    if (mode == 3) return js_promise_resolved(js_obj_val(js_make_error("AggregateError", "All promises were rejected")), 1);
    js_array_set_length(results, items->length);
    return js_promise_resolved(js_obj_val(results), 0);
}

static js_val queue_microtask(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)self; (void)callee;
    js_enqueue_microtask(ARG(0), js_undef());
    return js_undef();
}

/* ====================================================================== */
/* Map / Set                                                              */
/* ====================================================================== */

static long collection_find(js_obj* c, js_val key) {
    for (uint32_t i = 0; i < c->length; i++)
        if (same_value_zero(c->items[i], key)) return (long)i;
    return -1;
}

static js_val collection_ctor(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)self;
    int is_map = callee->magic;
    js_obj* c = js_new_object(is_map ? js_map_proto : js_set_proto);
    c->cls = is_map ? CLS_MAP : CLS_SET;
    if (is_map) c->data = js_new_array(0);
    if (ARG(0).type == JS_OBJ || ARG(0).type == JS_STR) {
        js_val list = iterate_to_array(ARG(0));
        for (uint32_t i = 0; list.type == JS_OBJ && i < list.u.obj->length; i++) {
            js_val item = list.u.obj->items[i];
            if (is_map) {
                js_val k = js_get_index(item, js_num(0)), v = js_get_index(item, js_num(1));
                long at = collection_find(c, k);
                if (at >= 0) {
                    ((js_obj*)c->data)->items[at] = v;
                } else {
                    js_array_push(c, k);
                    js_array_push((js_obj*)c->data, v);
                }
            } else if (collection_find(c, item) < 0) {
                js_array_push(c, item);
            }
        }
    }
    return js_obj_val(c);
}

static js_obj* this_collection(js_val self) {
    if (self.type == JS_OBJ && (self.u.obj->cls == CLS_MAP || self.u.obj->cls == CLS_SET)) return self.u.obj;
    js_throw_error("TypeError", "Method called on incompatible receiver");
    return NULL;
}

static js_val collection_method(js_val self, js_val* args, int argc, js_obj* callee) {
    js_obj* c = this_collection(self);
    if (!c) return js_undef();
    js_obj* values = (js_obj*)c->data;
    long at;
    switch (callee->magic) {
    case 0:   /* get */
        at = collection_find(c, ARG(0));
        return at >= 0 && values ? values->items[at] : js_undef();
    case 1:   /* set / add */
        at = collection_find(c, ARG(0));
        if (at >= 0) {
            if (values) values->items[at] = ARG(1);
        } else {
            js_array_push(c, ARG(0));
            if (values) js_array_push(values, ARG(1));
        }
        return self;
    case 2:   /* has */
        return js_bool(collection_find(c, ARG(0)) >= 0);
    case 3:   /* delete */
        at = collection_find(c, ARG(0));
        if (at < 0) return js_bool(0);
        memmove(c->items + at, c->items + at + 1, sizeof(js_val) * (c->length - (uint32_t)at - 1));
        c->length--;
        if (values) {
            memmove(values->items + at, values->items + at + 1, sizeof(js_val) * (values->length - (uint32_t)at - 1));
            values->length--;
        }
        return js_bool(1);
    case 4:   /* clear */
        c->length = 0;
        if (values) values->length = 0;
        return js_undef();
    case 5:   /* forEach */
        for (uint32_t i = 0; i < c->length && !js_rt.ctl; i++) {
            js_val call_args[3] = {values ? values->items[i] : c->items[i], c->items[i], self};
            js_call(ARG(0), ARG(1), call_args, 3);
        }
        return js_undef();
    case 6: {   /* keys */
        js_obj* out = js_new_array(c->length);
        for (uint32_t i = 0; i < c->length; i++) js_array_push(out, c->items[i]);
        return js_obj_val(out);
    }
    case 7: {   /* values */
        js_obj* out = js_new_array(c->length);
        for (uint32_t i = 0; i < c->length; i++) js_array_push(out, values ? values->items[i] : c->items[i]);
        return js_obj_val(out);
    }
    case 8:   /* entries */
        return iterate_to_array(self.u.obj->cls == CLS_SET
            ? ({ js_obj* out = js_new_array(c->length);
                 for (uint32_t i = 0; i < c->length; i++) {
                     js_obj* pair = js_new_array(2);
                     js_array_push(pair, c->items[i]);
                     js_array_push(pair, c->items[i]);
                     js_array_push(out, js_obj_val(pair));
                 }
                 js_obj_val(out); })
            : self);
    }
    return js_undef();
}

static js_val collection_size(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)args; (void)argc; (void)callee;
    js_obj* c = this_collection(self);
    return c ? js_num(c->length) : js_undef();
}

/* ====================================================================== */
/* Symbol, Reflect, URI, base64                                           */
/* ====================================================================== */

static unsigned symbol_counter;

static js_val symbol_fn(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)self; (void)callee;
    char name[96] = "@@sym:";
    if (ARG(0).type != JS_UNDEF) mx_append(name, sizeof(name), js_to_string(ARG(0))->data);
    mx_append(name, sizeof(name), ":");
    mx_append_int(name, sizeof(name), (long)++symbol_counter);
    return js_str_val(js_intern_c(name));
}

static js_val symbol_for(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)self; (void)callee;
    return js_str_val(js_concat(js_intern_c("@@for:"), js_to_string(ARG(0))));
}

static js_val reflect_method(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)self;
    switch (callee->magic) {
    case 0: return ARG(0).type == JS_OBJ ? js_obj_val(js_own_keys(ARG(0).u.obj, 1)) : js_undef();
    case 1: return js_bool(js_has_property(ARG(0), js_to_string(ARG(1))));
    case 2: return js_get(ARG(0), js_to_string(ARG(1)));
    case 3: js_set(ARG(0), js_to_string(ARG(1)), ARG(2)); return js_bool(1);
    case 4: return function_apply(ARG(0), args + 1, argc - 1, callee);
    case 5: {
        js_val list = iterate_to_array(ARG(1));
        return js_construct(ARG(0), list.type == JS_OBJ ? list.u.obj->items : NULL,
                            list.type == JS_OBJ ? (int)list.u.obj->length : 0);
    }
    case 6: return object_get_prototype_of(js_undef(), args, argc, callee);
    case 7: object_define_property(js_undef(), args, argc, callee); return js_bool(1);
    case 8: return js_bool(js_delete(ARG(0), js_to_string(ARG(1))));
    }
    return js_undef();
}

static js_val uri_encode(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)self;
    js_str* s = js_to_string(ARG(0));
    const char* keep = callee->magic == 1 ? ";,/?:@&=+$#-_.!~*'()" : callee->magic == 2 ? "@*_+-./" : "-_.!~*'()";
    static const char hex[] = "0123456789ABCDEF";
    char* out = (char*)malloc(s->length * 3 + 1);
    if (!out) return js_str_val(s);
    size_t used = 0;
    for (uint32_t i = 0; i < s->length; i++) {
        unsigned char c = (unsigned char)s->data[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            (c < 0x80 && strchr(keep, c) && c)) {
            out[used++] = (char)c;
        } else {
            out[used++] = '%';
            out[used++] = hex[c >> 4];
            out[used++] = hex[c & 15];
        }
    }
    js_str* r = js_new_str(out, used);
    free(out);
    return js_str_val(r);
}

static js_val uri_decode(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)self; (void)callee;
    js_str* s = js_to_string(ARG(0));
    char* out = (char*)malloc(s->length + 1);
    if (!out) return js_str_val(s);
    size_t used = 0;
    for (uint32_t i = 0; i < s->length; i++) {
        if (s->data[i] == '%' && i + 2 < s->length && hexval_re(s->data[i + 1]) >= 0 &&
            hexval_re(s->data[i + 2]) >= 0) {
            out[used++] = (char)(hexval_re(s->data[i + 1]) * 16 + hexval_re(s->data[i + 2]));
            i += 2;
        } else if (s->data[i] == '%' && callee->magic == 0) {
            free(out);
            return js_throw_error("URIError", "URI malformed");
        } else {
            out[used++] = s->data[i];
        }
    }
    js_str* r = js_new_str(out, used);
    free(out);
    return js_str_val(r);
}

static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static js_val base64(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)self;
    js_str* s = js_to_string(ARG(0));
    if (callee->magic == 0) {   /* btoa */
        size_t n = s->length;
        char* out = (char*)malloc(n * 4 / 3 + 8);
        if (!out) return js_str_val(s);
        size_t used = 0;
        for (size_t i = 0; i < n; i += 3) {
            uint32_t v = (uint32_t)(unsigned char)s->data[i] << 16;
            if (i + 1 < n) v |= (uint32_t)(unsigned char)s->data[i + 1] << 8;
            if (i + 2 < n) v |= (unsigned char)s->data[i + 2];
            out[used++] = b64[(v >> 18) & 63];
            out[used++] = b64[(v >> 12) & 63];
            out[used++] = i + 1 < n ? b64[(v >> 6) & 63] : '=';
            out[used++] = i + 2 < n ? b64[v & 63] : '=';
        }
        js_str* r = js_new_str(out, used);
        free(out);
        return js_str_val(r);
    }
    char* out = (char*)malloc(s->length + 1);
    if (!out) return js_str_val(s);
    size_t used = 0;
    uint32_t v = 0;
    int bits = 0;
    for (uint32_t i = 0; i < s->length; i++) {
        const char* at = strchr(b64, s->data[i]);
        if (!at || !s->data[i]) continue;
        v = (v << 6) | (uint32_t)(at - b64);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[used++] = (char)((v >> bits) & 0xFF);
        }
    }
    js_str* r = js_new_str(out, used);
    free(out);
    return js_str_val(r);
}

/* ====================================================================== */
/* Console                                                                */
/* ====================================================================== */

static void inspect_rec(sbuf* b, js_val v, int depth, int quote) {
    char tmp[64];
    switch (v.type) {
    case JS_UNDEF: sb_cstr(b, "undefined"); return;
    case JS_NULL: sb_cstr(b, "null"); return;
    case JS_BOOL: sb_cstr(b, v.u.boolean ? "true" : "false"); return;
    case JS_NUM: sb_cstr(b, js_number_to_string(v.u.num, 10)->data); return;
    case JS_STR:
        if (quote) {
            sb_add(b, "\"", 1);
            sb_add(b, v.u.str->data, v.u.str->length > 200 ? 200 : v.u.str->length);
            if (v.u.str->length > 200) sb_cstr(b, "...");
            sb_add(b, "\"", 1);
        } else {
            sb_add(b, v.u.str->data, v.u.str->length);
        }
        return;
    case JS_OBJ: break;
    }
    js_obj* o = v.u.obj;
    if (js_is_callable(v)) {
        sb_cstr(b, o->func && o->func->is_class_constructor ? "class " : "\xC6\x92 ");
        sb_cstr(b, o->name ? o->name->data : "");
        if (!(o->func && o->func->is_class_constructor)) sb_cstr(b, "()");
        return;
    }
    if (o->cls == CLS_ERROR) {
        js_val s = error_to_string(v, NULL, 0, NULL);
        if (s.type == JS_STR) sb_add(b, s.u.str->data, s.u.str->length);
        return;
    }
    if (o->cls == CLS_DATE) {
        js_obj fake;
        memset(&fake, 0, sizeof(fake));
        fake.magic = 1;
        js_val s = date_format(v, NULL, 0, &fake);
        if (s.type == JS_STR) sb_add(b, s.u.str->data, s.u.str->length);
        return;
    }
    if (o->cls == CLS_REGEXP) {
        js_val s = regexp_to_string(v, NULL, 0, NULL);
        if (s.type == JS_STR) sb_add(b, s.u.str->data, s.u.str->length);
        return;
    }
    if (o->cls == CLS_NODE && o->node) {
        mx_node_t* n = o->node;
        if (n->type == MX_NODE_TEXT) {
            sb_cstr(b, "#text \"");
            sb_add(b, n->text, n->text_length > 40 ? 40 : n->text_length);
            sb_cstr(b, "\"");
            return;
        }
        if (n->type == MX_NODE_DOCUMENT) {
            sb_cstr(b, "#document");
            return;
        }
        sb_cstr(b, "<");
        sb_cstr(b, n->tag);
        const char* id = mx_dom_attr(n, "id");
        if (id) {
            sb_cstr(b, " id=\"");
            sb_cstr(b, id);
            sb_cstr(b, "\"");
        }
        const char* cls = mx_dom_attr(n, "class");
        if (cls) {
            sb_cstr(b, " class=\"");
            sb_add(b, cls, strlen(cls) > 40 ? 40 : strlen(cls));
            sb_cstr(b, "\"");
        }
        sb_cstr(b, ">");
        return;
    }
    if (o->cls == CLS_PROMISE) {
        promise_data* d = (promise_data*)o->data;
        sb_cstr(b, d->state == 0 ? "Promise {<pending>}" : d->state == 1 ? "Promise {" : "Promise {<rejected> ");
        if (d->state) {
            inspect_rec(b, d->value, depth + 1, 1);
            sb_cstr(b, "}");
        }
        return;
    }
    if (depth > 2) {
        sb_cstr(b, o->cls == CLS_ARRAY ? "[...]" : "{...}");
        return;
    }
    if (o->cls == CLS_ARRAY || o->cls == CLS_ARGUMENTS || o->cls == CLS_SET) {
        if (o->cls == CLS_SET) {
            sb_cstr(b, "Set(");
            tmp[0] = '\0';
            mx_append_int(tmp, sizeof(tmp), o->length);
            sb_cstr(b, tmp);
            sb_cstr(b, ") ");
        } else if (o->length > 6) {
            sb_cstr(b, "(");
            tmp[0] = '\0';
            mx_append_int(tmp, sizeof(tmp), o->length);
            sb_cstr(b, tmp);
            sb_cstr(b, ") ");
        }
        sb_cstr(b, o->cls == CLS_SET ? "{" : "[");
        for (uint32_t i = 0; i < o->length && i < 50; i++) {
            if (i) sb_cstr(b, ", ");
            if (b->used > 400) {
                sb_cstr(b, "...");
                break;
            }
            inspect_rec(b, o->items[i], depth + 1, 1);
        }
        if (o->length > 50) sb_cstr(b, ", ...");
        sb_cstr(b, o->cls == CLS_SET ? "}" : "]");
        return;
    }
    if (o->cls == CLS_MAP) {
        js_obj* values = (js_obj*)o->data;
        sb_cstr(b, "Map(");
        tmp[0] = '\0';
        mx_append_int(tmp, sizeof(tmp), o->length);
        sb_cstr(b, tmp);
        sb_cstr(b, ") {");
        for (uint32_t i = 0; i < o->length && i < 20; i++) {
            if (i) sb_cstr(b, ", ");
            inspect_rec(b, o->items[i], depth + 1, 1);
            sb_cstr(b, " => ");
            inspect_rec(b, values->items[i], depth + 1, 1);
        }
        sb_cstr(b, "}");
        return;
    }
    if (o->cls == CLS_STYLE) {
        sb_cstr(b, "CSSStyleDeclaration");
        return;
    }
    js_val ctor = js_get(v, js_intern_c("constructor"));
    if (js_is_callable(ctor) && ctor.u.obj->name && ctor.u.obj->name->length &&
        !js_str_eq_c(ctor.u.obj->name, "Object")) {
        sb_cstr(b, ctor.u.obj->name->data);
        sb_cstr(b, " ");
    }
    sb_cstr(b, "{");
    js_obj* keys = js_own_keys(o, 0);
    for (uint32_t i = 0; i < keys->length && i < 30; i++) {
        if (i) sb_cstr(b, ", ");
        if (b->used > 400) {
            sb_cstr(b, "...");
            break;
        }
        js_str* key = keys->items[i].u.str;
        sb_add(b, key->data, key->length);
        sb_cstr(b, ": ");
        js_prop* p = js_find_own(o, key);
        if (p && (p->flags & PROP_ACCESSOR)) sb_cstr(b, "(getter)");
        else inspect_rec(b, js_get(v, key), depth + 1, 1);
    }
    sb_cstr(b, "}");
}

void js_inspect(js_val v, char* out, size_t capacity, int depth, int quote) {
    sbuf b = {NULL, 0, 0};
    int saved_ctl = js_rt.ctl;
    js_val saved_value = js_rt.value;
    js_rt.ctl = CTL_NONE;
    inspect_rec(&b, v, depth, quote);
    js_rt.ctl = (js_ctl)saved_ctl;
    js_rt.value = saved_value;
    mx_copy(out, capacity, b.data ? b.data : "");
    free(b.data);
}

static js_val console_log(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)self;
    char text[MX_CONSOLE_TEXT];
    text[0] = '\0';
    int start = 0;
    if (callee->magic == 9) {   /* assert */
        if (js_truthy(ARG(0))) return js_undef();
        mx_copy(text, sizeof(text), "Assertion failed: ");
        start = 1;
    }
    if (argc > start && args[start].type == JS_STR && strchr(args[start].u.str->data, '%')) {
        /* printf-style substitutions */
        const char* f = args[start].u.str->data;
        int next_arg = start + 1;
        char piece[MX_CONSOLE_TEXT];
        size_t used = strlen(text);
        for (; *f && used + 1 < sizeof(text); f++) {
            if (*f == '%' && f[1] && strchr("sdifoOc", f[1])) {
                char kind = *++f;
                if (kind == 'c') {
                    next_arg++;
                    continue;
                }
                js_val v = next_arg < argc ? args[next_arg++] : js_undef();
                if (kind == 'd' || kind == 'i') v = js_num(m_trunc(js_to_number(v)));
                else if (kind == 'f') v = js_num(js_to_number(v));
                js_inspect(v, piece, sizeof(piece), 0, kind == 'o' || kind == 'O');
                mx_copy(text + used, sizeof(text) - used, piece);
                used = strlen(text);
            } else if (*f == '%' && f[1] == '%') {
                text[used++] = '%';
                f++;
                text[used] = '\0';
            } else {
                text[used++] = *f;
                text[used] = '\0';
            }
        }
        start = next_arg;
    }
    for (int i = start; i < argc; i++) {
        char piece[MX_CONSOLE_TEXT];
        js_inspect(args[i], piece, sizeof(piece), 0, 0);
        if (text[0]) mx_append(text, sizeof(text), " ");
        mx_append(text, sizeof(text), piece);
    }
    static const mx_log_level_t levels[] = {MX_LOG_LOG, MX_LOG_INFO, MX_LOG_WARN, MX_LOG_ERROR,
                                            MX_LOG_DEBUG, MX_LOG_LOG, MX_LOG_LOG, MX_LOG_LOG,
                                            MX_LOG_LOG, MX_LOG_ERROR};
    int level = callee->magic >= 0 && callee->magic < 10 ? callee->magic : 0;
    if (callee->magic == 7) {   /* clear */
        mx_console_clear();
        mx_console_add(MX_LOG_SYSTEM, "", 0, "Console was cleared");
        return js_undef();
    }
    mx_console_add(levels[level], js_rt.source ? js_rt.source : "", js_rt.line, text);
    return js_undef();
}

static js_val console_count(js_val self, js_val* args, int argc, js_obj* callee) {
    (void)self;
    static js_obj* counters;
    if (!counters || counters->proto != js_object_proto) counters = js_new_object(js_object_proto);
    js_str* label = ARG(0).type == JS_UNDEF ? js_intern_c("default") : js_to_string(ARG(0));
    if (callee->magic == 0) {   /* count */
        double n = js_to_number(js_get(js_obj_val(counters), label));
        if (is_nan(n)) n = 0;
        js_set(js_obj_val(counters), label, js_num(n + 1));
        char text[128];
        mx_copy(text, sizeof(text), label->data);
        mx_append(text, sizeof(text), ": ");
        mx_append_int(text, sizeof(text), (long)(n + 1));
        mx_console_add(MX_LOG_LOG, js_rt.source, js_rt.line, text);
    } else if (callee->magic == 1) {   /* time */
        js_set(js_obj_val(counters), js_concat(js_intern_c("@time:"), label), js_num((double)mx_now_ms()));
    } else {   /* timeEnd */
        double start = js_to_number(js_get(js_obj_val(counters), js_concat(js_intern_c("@time:"), label)));
        char text[128];
        mx_copy(text, sizeof(text), label->data);
        mx_append(text, sizeof(text), ": ");
        mx_append_int(text, sizeof(text), is_nan(start) ? 0 : (long)((double)mx_now_ms() - start));
        mx_append(text, sizeof(text), " ms");
        mx_console_add(MX_LOG_LOG, js_rt.source, js_rt.line, text);
    }
    return js_undef();
}

void js_report_exception(void) {
    js_val e = js_rt.value;
    char text[MX_CONSOLE_TEXT];
    int line = js_rt.line;
    const char* source = js_rt.source ? js_rt.source : "";
    if (e.type == JS_OBJ && e.u.obj->cls == CLS_ERROR) {
        js_val l = js_get_c(e, "\x01line");
        if (l.type == JS_NUM) line = (int)l.u.num;
        js_val src = js_get_c(e, "\x01source");
        if (src.type == JS_STR && src.u.str->length) source = src.u.str->data;
    }
    char body[MX_CONSOLE_TEXT];
    js_inspect(e, body, sizeof(body), 0, 1);
    mx_copy(text, sizeof(text), "Uncaught ");
    mx_append(text, sizeof(text), body);
    mx_console_add(MX_LOG_ERROR, source, line, text);
}

/* ====================================================================== */
/* Setup                                                                  */
/* ====================================================================== */

js_str* js_decode_escapes(const char* text, size_t length);

static js_obj* make_ctor(const char* name, js_native fn, int arity, js_obj* proto) {
    js_obj* ctor = js_new_native(name, fn, arity);
    if (proto) {
        js_define(ctor, "prototype", js_obj_val(proto), PROP_HIDDEN);
        js_define(proto, "constructor", js_obj_val(ctor), PROP_HIDDEN);
    }
    js_define(js_global, name, js_obj_val(ctor), PROP_HIDDEN);
    return ctor;
}

static void method(js_obj* o, const char* name, js_native fn, int magic) {
    js_obj* f = js_new_native(name, fn, 1);
    f->magic = (int16_t)magic;
    js_define(o, name, js_obj_val(f), PROP_HIDDEN);
}

static js_str* then_name;
js_str* S_then_name(void);
js_str* S_then_name(void) {
    return then_name;
}

void js_runtime_init(void) {
    then_name = js_intern_c("then");
    rng_state ^= (uint64_t)mx_now_ms() * 0x2545F4914F6CDD1Dull + (uint64_t)(uintptr_t)&rng_state;
    if (!rng_state) rng_state = 1;
    micro_head = micro_tail = NULL;
    symbol_counter = 0;

    js_object_proto = js_new_object(NULL);
    js_function_proto = js_new_object(js_object_proto);
    js_function_proto->cls = CLS_NATIVE;
    js_function_proto->native = return_this;
    js_array_proto = js_new_object(js_object_proto);
    js_array_proto->cls = CLS_ARRAY;
    js_string_proto = js_new_object(js_object_proto);
    js_number_proto = js_new_object(js_object_proto);
    js_boolean_proto = js_new_object(js_object_proto);
    js_error_proto = js_new_object(js_object_proto);
    js_regexp_proto = js_new_object(js_object_proto);
    js_date_proto = js_new_object(js_object_proto);
    js_promise_proto = js_new_object(js_object_proto);
    js_map_proto = js_new_object(js_object_proto);
    js_set_proto = js_new_object(js_object_proto);
    js_global = js_new_object(js_object_proto);

    /* Object */
    js_obj* object = make_ctor("Object", object_ctor, 1, js_object_proto);
    method(object, "keys", object_keys, 0);
    method(object, "values", object_keys, 1);
    method(object, "entries", object_keys, 2);
    method(object, "assign", object_assign, 0);
    method(object, "create", object_create, 0);
    method(object, "freeze", object_freeze, 0);
    method(object, "seal", object_freeze, 1);
    method(object, "preventExtensions", object_freeze, 1);
    method(object, "isFrozen", object_is_frozen, 0);
    method(object, "isSealed", object_is_frozen, 0);
    method(object, "isExtensible", object_is_frozen, 0);
    method(object, "defineProperty", object_define_property, 0);
    method(object, "defineProperties", object_define_properties, 0);
    method(object, "getOwnPropertyDescriptor", object_get_own_property_descriptor, 0);
    method(object, "getOwnPropertyNames", object_get_own_property_names, 0);
    method(object, "getOwnPropertySymbols", array_of, 0);
    method(object, "getPrototypeOf", object_get_prototype_of, 0);
    method(object, "setPrototypeOf", object_set_prototype_of, 0);
    method(object, "fromEntries", object_from_entries, 0);
    method(object, "is", object_is, 0);
    method(object, "hasOwn", object_has_own, 1);
    method(js_object_proto, "hasOwnProperty", object_has_own, 0);
    method(js_object_proto, "isPrototypeOf", object_is_prototype_of, 0);
    method(js_object_proto, "propertyIsEnumerable", object_has_own, 0);
    method(js_object_proto, "toString", object_to_string, 0);
    method(js_object_proto, "toLocaleString", object_to_string, 0);
    method(js_object_proto, "valueOf", object_value_of, 0);

    /* Function */
    make_ctor("Function", function_ctor, 1, js_function_proto);
    method(js_function_proto, "call", function_call, 0);
    method(js_function_proto, "apply", function_apply, 0);
    method(js_function_proto, "bind", function_bind, 0);
    method(js_function_proto, "toString", function_to_string, 0);

    /* Array */
    js_obj* array = make_ctor("Array", array_ctor, 1, js_array_proto);
    method(array, "isArray", array_is_array, 0);
    method(array, "from", array_from, 0);
    method(array, "of", array_of, 0);
    js_obj* ap = js_array_proto;
    method(ap, "push", array_push, 0);
    method(ap, "pop", array_pop, 0);
    method(ap, "shift", array_shift, 0);
    method(ap, "unshift", array_unshift, 0);
    method(ap, "slice", array_slice, 0);
    method(ap, "splice", array_splice, 0);
    method(ap, "concat", array_concat, 0);
    method(ap, "join", array_join, 0);
    method(ap, "reverse", array_reverse, 0);
    method(ap, "indexOf", array_index_of, 0);
    method(ap, "lastIndexOf", array_index_of, 1);
    method(ap, "includes", array_index_of, 2);
    method(ap, "forEach", array_iterate, 0);
    method(ap, "map", array_iterate, 1);
    method(ap, "filter", array_iterate, 2);
    method(ap, "find", array_iterate, 3);
    method(ap, "findIndex", array_iterate, 4);
    method(ap, "some", array_iterate, 5);
    method(ap, "every", array_iterate, 6);
    method(ap, "findLast", array_iterate, 7);
    method(ap, "findLastIndex", array_iterate, 8);
    method(ap, "reduce", array_reduce, 0);
    method(ap, "reduceRight", array_reduce, 1);
    method(ap, "sort", array_sort, 0);
    method(ap, "fill", array_fill, 0);
    method(ap, "flat", array_flat, 0);
    method(ap, "flatMap", array_flat, 1);
    method(ap, "at", array_at, 0);
    method(ap, "keys", array_keys, 0);
    method(ap, "values", array_keys, 1);
    method(ap, "entries", array_keys, 2);
    method(ap, "toString", array_to_string, 0);
    method(ap, "@@iterator", array_keys, 1);

    /* String */
    js_obj* string = make_ctor("String", string_ctor, 1, js_string_proto);
    method(string, "fromCharCode", string_from_char_code, 0);
    method(string, "fromCodePoint", string_from_char_code, 1);
    js_obj* sp = js_string_proto;
    method(sp, "charAt", string_char_at, 0);
    method(sp, "charCodeAt", string_char_at, 1);
    method(sp, "codePointAt", string_char_at, 2);
    method(sp, "indexOf", string_index_of, 0);
    method(sp, "lastIndexOf", string_index_of, 1);
    method(sp, "includes", string_index_of, 2);
    method(sp, "startsWith", string_index_of, 3);
    method(sp, "endsWith", string_index_of, 4);
    method(sp, "slice", string_slice, 0);
    method(sp, "substring", string_slice, 1);
    method(sp, "substr", string_slice, 2);
    method(sp, "toLowerCase", string_case, 0);
    method(sp, "toUpperCase", string_case, 1);
    method(sp, "toLocaleLowerCase", string_case, 0);
    method(sp, "toLocaleUpperCase", string_case, 1);
    method(sp, "trim", string_trim, 0);
    method(sp, "trimStart", string_trim, 1);
    method(sp, "trimLeft", string_trim, 1);
    method(sp, "trimEnd", string_trim, 2);
    method(sp, "trimRight", string_trim, 2);
    method(sp, "padStart", string_pad, 0);
    method(sp, "padEnd", string_pad, 1);
    method(sp, "repeat", string_repeat, 0);
    method(sp, "split", string_split, 0);
    method(sp, "replace", string_replace, 0);
    method(sp, "replaceAll", string_replace, 1);
    method(sp, "match", string_match, 0);
    method(sp, "matchAll", string_match, 1);
    method(sp, "search", string_match, 2);
    method(sp, "concat", string_concat, 0);
    method(sp, "at", array_at, 0);
    method(sp, "localeCompare", string_locale_compare, 0);
    method(sp, "normalize", string_to_string, 0);
    method(sp, "toString", string_to_string, 0);
    method(sp, "valueOf", string_to_string, 0);

    /* Number, Boolean */
    js_obj* number = make_ctor("Number", number_ctor, 1, js_number_proto);
    method(number, "isNaN", number_is, 0);
    method(number, "isFinite", number_is, 1);
    method(number, "isInteger", number_is, 2);
    method(number, "isSafeInteger", number_is, 3);
    method(number, "parseFloat", global_parse_float, 0);
    method(number, "parseInt", global_parse_int, 0);
    js_define(number, "MAX_SAFE_INTEGER", js_num(9007199254740991.0), PROP_HIDDEN);
    js_define(number, "MIN_SAFE_INTEGER", js_num(-9007199254740991.0), PROP_HIDDEN);
    js_define(number, "EPSILON", js_num(2.220446049250313e-16), PROP_HIDDEN);
    js_define(number, "MAX_VALUE", js_num(1.7976931348623157e308), PROP_HIDDEN);
    js_define(number, "MIN_VALUE", js_num(5e-324), PROP_HIDDEN);
    js_define(number, "POSITIVE_INFINITY", js_num(inf_value()), PROP_HIDDEN);
    js_define(number, "NEGATIVE_INFINITY", js_num(-inf_value()), PROP_HIDDEN);
    js_define(number, "NaN", js_num(nan_value()), PROP_HIDDEN);
    method(js_number_proto, "toString", number_to_string, 0);
    method(js_number_proto, "toFixed", number_to_fixed, 0);
    method(js_number_proto, "toPrecision", number_to_precision, 0);
    method(js_number_proto, "toLocaleString", number_to_locale, 0);
    method(js_number_proto, "valueOf", number_value_of, 0);
    make_ctor("Boolean", boolean_ctor, 1, js_boolean_proto);
    method(js_boolean_proto, "toString", boolean_to_string, 0);
    method(js_boolean_proto, "valueOf", object_value_of, 0);

    /* Errors */
    js_obj* error = make_ctor("Error", error_ctor, 1, js_error_proto);
    (void)error;
    js_define(js_error_proto, "name", js_str_val(js_intern_c("Error")), PROP_HIDDEN);
    js_define(js_error_proto, "message", js_str_val(js_intern_c("")), PROP_HIDDEN);
    method(js_error_proto, "toString", error_to_string, 0);
    static const char* const error_types[] = {"TypeError", "RangeError", "SyntaxError",
                                              "ReferenceError", "EvalError", "URIError",
                                              "AggregateError"};
    for (size_t i = 0; i < sizeof(error_types) / sizeof(error_types[0]); i++) {
        js_obj* proto = js_new_object(js_error_proto);
        js_define(proto, "name", js_str_val(js_intern_c(error_types[i])), PROP_HIDDEN);
        js_obj* ctor = make_ctor(error_types[i], error_ctor, 1, proto);
        ctor->proto = error;
    }

    /* Math */
    js_obj* math = js_new_object(js_object_proto);
    js_define(js_global, "Math", js_obj_val(math), PROP_HIDDEN);
    js_define(math, "PI", js_num(M_PI_D), PROP_HIDDEN);
    js_define(math, "E", js_num(2.718281828459045), PROP_HIDDEN);
    js_define(math, "LN2", js_num(M_LN2_D), PROP_HIDDEN);
    js_define(math, "LN10", js_num(2.302585092994046), PROP_HIDDEN);
    js_define(math, "LOG2E", js_num(1.4426950408889634), PROP_HIDDEN);
    js_define(math, "LOG10E", js_num(0.4342944819032518), PROP_HIDDEN);
    js_define(math, "SQRT2", js_num(1.4142135623730951), PROP_HIDDEN);
    js_define(math, "SQRT1_2", js_num(0.7071067811865476), PROP_HIDDEN);
#define M(name) js_define_native(math, #name, math_##name, 1)
    M(floor); M(ceil); M(trunc); M(abs); M(sqrt); M(cbrt); M(exp); M(log); M(log2);
    M(log10); M(log1p); M(expm1); M(sin); M(cos); M(tan); M(atan); M(asin); M(acos);
    M(sinh); M(cosh); M(tanh); M(sign); M(fround); M(clz32); M(round); M(atan2); M(pow);
    M(hypot); M(random);
#undef M
    js_define_native(math, "max", math_minmax, 2);
    js_define_native(math, "min", math_minmax, 2);

    /* JSON */
    js_obj* json = js_new_object(js_object_proto);
    js_define(js_global, "JSON", js_obj_val(json), PROP_HIDDEN);
    js_define_native(json, "parse", json_parse, 2);
    js_define_native(json, "stringify", json_stringify, 3);

    /* RegExp */
    make_ctor("RegExp", regexp_ctor, 2, js_regexp_proto);
    method(js_regexp_proto, "exec", regexp_exec, 0);
    method(js_regexp_proto, "test", regexp_test, 0);
    method(js_regexp_proto, "toString", regexp_to_string, 0);

    /* Date */
    js_obj* date = make_ctor("Date", date_ctor, 7, js_date_proto);
    method(date, "now", date_now, 0);
    method(date, "parse", date_parse, 0);
    method(date, "UTC", date_utc, 0);
    static const char* const getters[] = {"getTime", "getFullYear", "getMonth", "getDate", "getDay",
                                          "getHours", "getMinutes", "getSeconds",
                                          "getMilliseconds", "getTimezoneOffset"};
    for (int i = 0; i < 10; i++) {
        method(js_date_proto, getters[i], date_get, i);
        if (i >= 1 && i <= 8) {
            char utc[32] = "getUTC";
            mx_append(utc, sizeof(utc), getters[i] + 3);
            method(js_date_proto, utc, date_get, i);
        }
    }
    method(js_date_proto, "valueOf", date_get, 0);
    method(js_date_proto, "getYear", date_get, 1);
    static const char* const setters[] = {"setFullYear", "setMonth", "setDate", "setHours",
                                          "setMinutes", "setSeconds", "setMilliseconds"};
    for (int i = 0; i < 7; i++) {
        method(js_date_proto, setters[i], date_set, i);
        char utc[32] = "setUTC";
        mx_append(utc, sizeof(utc), setters[i] + 3);
        method(js_date_proto, utc, date_set, i);
    }
    method(js_date_proto, "setTime", date_set, -1);
    method(js_date_proto, "toISOString", date_format, 1);
    method(js_date_proto, "toJSON", date_format, 1);
    method(js_date_proto, "toDateString", date_format, 2);
    method(js_date_proto, "toTimeString", date_format, 3);
    method(js_date_proto, "toString", date_format, 4);
    method(js_date_proto, "toUTCString", date_format, 4);
    method(js_date_proto, "toGMTString", date_format, 4);
    method(js_date_proto, "toLocaleDateString", date_format, 5);
    method(js_date_proto, "toLocaleTimeString", date_format, 6);
    method(js_date_proto, "toLocaleString", date_format, 7);

    /* Promise */
    js_obj* promise = make_ctor("Promise", promise_ctor, 1, js_promise_proto);
    method(js_promise_proto, "then", promise_then, 0);
    method(js_promise_proto, "catch", promise_then, 1);
    method(js_promise_proto, "finally", promise_then, 2);
    method(promise, "resolve", promise_static_resolve, 0);
    method(promise, "reject", promise_static_resolve, 1);
    method(promise, "all", promise_combine, 0);
    method(promise, "allSettled", promise_combine, 1);
    method(promise, "race", promise_combine, 2);
    method(promise, "any", promise_combine, 3);
    js_define_native(js_global, "queueMicrotask", queue_microtask, 1);

    /* Map, Set */
    js_obj* map = make_ctor("Map", collection_ctor, 0, js_map_proto);
    map->magic = 1;
    js_obj* set = make_ctor("Set", collection_ctor, 0, js_set_proto);
    set->magic = 0;
    js_define(js_global, "WeakMap", js_obj_val(map), PROP_HIDDEN);
    js_define(js_global, "WeakSet", js_obj_val(set), PROP_HIDDEN);
    method(js_map_proto, "get", collection_method, 0);
    method(js_map_proto, "set", collection_method, 1);
    method(js_map_proto, "has", collection_method, 2);
    method(js_map_proto, "delete", collection_method, 3);
    method(js_map_proto, "clear", collection_method, 4);
    method(js_map_proto, "forEach", collection_method, 5);
    method(js_map_proto, "keys", collection_method, 6);
    method(js_map_proto, "values", collection_method, 7);
    method(js_map_proto, "entries", collection_method, 8);
    method(js_map_proto, "@@iterator", collection_method, 8);
    js_define_accessor(js_map_proto, "size", collection_size, NULL);
    method(js_set_proto, "add", collection_method, 1);
    method(js_set_proto, "has", collection_method, 2);
    method(js_set_proto, "delete", collection_method, 3);
    method(js_set_proto, "clear", collection_method, 4);
    method(js_set_proto, "forEach", collection_method, 5);
    method(js_set_proto, "keys", collection_method, 7);
    method(js_set_proto, "values", collection_method, 7);
    method(js_set_proto, "entries", collection_method, 8);
    method(js_set_proto, "@@iterator", collection_method, 7);
    js_define_accessor(js_set_proto, "size", collection_size, NULL);

    /* Symbol */
    js_obj* symbol = js_new_native("Symbol", symbol_fn, 0);
    js_define(js_global, "Symbol", js_obj_val(symbol), PROP_HIDDEN);
    js_define(symbol, "iterator", js_str_val(js_intern_c("@@iterator")), PROP_HIDDEN);
    js_define(symbol, "asyncIterator", js_str_val(js_intern_c("@@asyncIterator")), PROP_HIDDEN);
    js_define(symbol, "toStringTag", js_str_val(js_intern_c("@@toStringTag")), PROP_HIDDEN);
    js_define(symbol, "hasInstance", js_str_val(js_intern_c("@@hasInstance")), PROP_HIDDEN);
    js_define(symbol, "toPrimitive", js_str_val(js_intern_c("@@toPrimitive")), PROP_HIDDEN);
    method(symbol, "for", symbol_for, 0);

    /* Reflect */
    js_obj* reflect = js_new_object(js_object_proto);
    js_define(js_global, "Reflect", js_obj_val(reflect), PROP_HIDDEN);
    static const char* const reflect_names[] = {"ownKeys", "has", "get", "set", "apply",
                                                "construct", "getPrototypeOf",
                                                "defineProperty", "deleteProperty"};
    for (int i = 0; i < 9; i++) method(reflect, reflect_names[i], reflect_method, i);

    /* globals */
    js_define(js_global, "NaN", js_num(nan_value()), PROP_HIDDEN | PROP_READONLY);
    js_define(js_global, "Infinity", js_num(inf_value()), PROP_HIDDEN | PROP_READONLY);
    js_define(js_global, "undefined", js_undef(), PROP_HIDDEN | PROP_READONLY);
    method(js_global, "parseInt", global_parse_int, 0);
    method(js_global, "parseFloat", global_parse_float, 0);
    method(js_global, "isNaN", global_is_nan, 0);
    method(js_global, "isFinite", global_is_nan, 1);
    method(js_global, "encodeURIComponent", uri_encode, 0);
    method(js_global, "encodeURI", uri_encode, 1);
    method(js_global, "escape", uri_encode, 2);
    method(js_global, "decodeURIComponent", uri_decode, 0);
    method(js_global, "decodeURI", uri_decode, 1);
    method(js_global, "unescape", uri_decode, 1);
    method(js_global, "btoa", base64, 0);
    method(js_global, "atob", base64, 1);

    /* console */
    js_obj* console = js_new_object(js_object_proto);
    js_define(js_global, "console", js_obj_val(console), PROP_HIDDEN);
    static const char* const console_names[] = {"log", "info", "warn", "error", "debug",
                                                "trace", "dir", "clear", "table", "assert"};
    for (int i = 0; i < 10; i++) method(console, console_names[i], console_log, i);
    method(console, "group", console_log, 0);
    method(console, "groupCollapsed", console_log, 0);
    method(console, "groupEnd", return_this, 0);
    method(console, "count", console_count, 0);
    method(console, "time", console_count, 1);
    method(console, "timeEnd", console_count, 2);
    method(console, "timeLog", console_count, 2);
}
