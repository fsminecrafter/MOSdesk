/* MOS Calculator.
 *
 * Type or click a whole calculation, like "1+3+5/4" or "2(3+4)^2", and
 * press = (or Enter): it is worked out with the usual order of operations
 * (brackets, powers, then * and /, then + and -).
 *
 * Make the window bigger and it becomes a scientific calculator, with
 * trigonometry (degrees or radians), logarithms, powers, roots,
 * factorials, pi, e and the previous answer (ans). Make it small again
 * and the extra keys go away.
 *
 * There is no maths library on Minimal-OS, so the functions it needs
 * (sqrt, exp, ln, sin, atan ...) are written out below. */
#include <stdint.h>
#include <stddef.h>

#ifndef CALC_NO_UI
#include "window.h"
#include "graphics.h"
#include "string.h"
#include "easyui.h"
#endif

/* ------------------------------------------------------------- maths */

#define CALC_PI 3.14159265358979323846
#define CALC_E 2.71828182845904523536
#define CALC_LN2 0.69314718055994530942
#define CALC_LN10 2.30258509299404568402

typedef union {
    double d;
    uint64_t u;
} calc_bits_t;

/* Small string helpers, so the maths needs no C library. */
static int strcmp_calc(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static void strcpy_calc(char* to, const char* from) {
    while ((*to++ = *from++)) {}
}

static double c_abs(double x) { return x < 0 ? -x : x; }

static int c_finite(double x) {
    calc_bits_t b = {x};
    return ((b.u >> 52) & 0x7FFu) != 0x7FFu;
}

/* 2^k, for -1022 <= k <= 1023. */
static double c_pow2(int k) {
    calc_bits_t b;
    b.u = (uint64_t)(k + 1023) << 52;
    return b.d;
}

static double c_scale2(double x, int k) {      /* x * 2^k */
    while (k > 1000) { x *= c_pow2(1000); k -= 1000; }
    while (k < -1000) { x *= c_pow2(-1000); k += 1000; }
    return x * c_pow2(k);
}

static double c_round(double x) {             /* to the nearest whole */
    if (c_abs(x) >= 4503599627370496.0) return x;   /* 2^52: already whole */
    double r = (double)(int64_t)(x < 0 ? x - 0.5 : x + 0.5);
    return r;
}

static int c_is_whole(double x) {
    return c_finite(x) && (c_abs(x) >= 4503599627370496.0 ||
                           (double)(int64_t)x == x);
}

static double c_sqrt(double x) {
    if (x <= 0) return 0;
    calc_bits_t b = {x};
    int e = (int)((b.u >> 52) & 0x7FFu) - 1023;
    double r = c_pow2(e / 2);               /* within a factor of 2 */
    for (int i = 0; i < 8; i++) r = 0.5 * (r + x / r);
    return r;
}

static double c_exp(double x) {
    if (x > 709.78) return c_pow2(1023) * 2.0;     /* overflow: infinity */
    if (x < -745.2) return 0;
    double k = c_round(x / CALC_LN2);
    /* ln 2 split in two so x - k ln 2 keeps its precision */
    double r = (x - k * 6.93147180369123816490e-01) -
               k * 1.90821492927058770002e-10;
    double term = 1, sum = 1;
    for (int n = 1; n < 22; n++) {
        term *= r / n;
        sum += term;
    }
    return c_scale2(sum, (int)k);
}

/* Natural logarithm, x > 0. */
static double c_ln(double x) {
    calc_bits_t b = {x};
    int e = (int)((b.u >> 52) & 0x7FFu) - 1023;
    if (e == -1023) {                        /* subnormal: make it normal */
        b.d = x * c_pow2(54);
        e = (int)((b.u >> 52) & 0x7FFu) - 1023 - 54;
    }
    b.u = (b.u & 0x000FFFFFFFFFFFFFull) | 0x3FF0000000000000ull;  /* [1, 2) */
    double m = b.d;
    if (m > 1.41421356237309504880) { m *= 0.5; e++; }
    /* ln m = 2 atanh(s), s = (m - 1) / (m + 1), |s| < 0.18 */
    double s = (m - 1) / (m + 1), s2 = s * s, term = s, sum = 0;
    for (int n = 1; n < 40; n += 2) {
        sum += term / n;
        term *= s2;
    }
    return e * CALC_LN2 + 2 * sum;
}

/* sin and cos of r, |r| <= pi/4. */
static double c_sin_small(double r) {
    double r2 = r * r, term = r, sum = r;
    for (int n = 2; n < 22; n += 2) {
        term *= -r2 / (n * (n + 1));
        sum += term;
    }
    return sum;
}

static double c_cos_small(double r) {
    double r2 = r * r, term = 1, sum = 1;
    for (int n = 1; n < 22; n += 2) {
        term *= -r2 / (n * (n + 1));
        sum += term;
    }
    return sum;
}

/* sin (want_cos = 0) or cos of x in radians. */
static double c_trig(double x, int want_cos) {
    double k = c_round(x / (CALC_PI / 2));
    double r = (x - k * 1.57079632679489655800e+00) -
               k * 6.12323399573676603587e-17;
    int quadrant = (int)((int64_t)k & 3);
    if (want_cos) quadrant = (quadrant + 1) & 3;
    switch (quadrant) {
    case 0: return c_sin_small(r);
    case 1: return c_cos_small(r);
    case 2: return -c_sin_small(r);
    default: return -c_cos_small(r);
    }
}

static double c_atan(double x) {
    if (x < 0) return -c_atan(-x);
    if (x > 1) return CALC_PI / 2 - c_atan(1 / x);
    double offset = 0;
    if (x > 0.41421356237309504880) {         /* tan(pi/8) */
        offset = CALC_PI / 4;
        x = (x - 1) / (x + 1);
    }
    double x2 = x * x, term = x, sum = 0;
    for (int n = 1; n < 90; n += 2) {
        sum += term / n;
        term *= -x2;
    }
    return offset + sum;
}

/* ------------------------------------------------------- the evaluator
 * expression := term (('+' | '-') term)*
 * term       := unary (('*' | 'x' | '/') unary | implicit power)*
 * unary      := ('-' | '+') unary | power
 * power      := postfix ('^' unary)?          right to left: 2^3^2 = 2^9
 * postfix    := primary ('!' | '%')*
 * primary    := number | '(' expression ')' | constant | function arg
 * "2pi", "3(4+1)" and "2sin(30)" multiply. A bracket left open at the end
 * is closed for you. */

typedef struct {
    const char* s;
    int pos;
    int depth;
    int degrees;
    double ans;
    const char* error;
} calc_parser_t;

static double parse_expression(calc_parser_t* p);
static double parse_unary(calc_parser_t* p);
static double parse_power(calc_parser_t* p);

static void calc_fail(calc_parser_t* p, const char* message) {
    if (!p->error) p->error = message;
}

static char peek(calc_parser_t* p) {
    while (p->s[p->pos] == ' ') p->pos++;
    return p->s[p->pos];
}

static int is_letter(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static int is_digit(char c) { return c >= '0' && c <= '9'; }

/* Could this character start a number, bracket, name or function? */
static int starts_operand(char c) {
    return is_digit(c) || c == '.' || c == '(' ||
           (is_letter(c) && c != 'x' && c != 'X');
}

static double parse_number(calc_parser_t* p) {
    double value = 0, scale = 1;
    int digits = 0, dot = 0;
    for (;; p->pos++) {
        char c = p->s[p->pos];
        if (is_digit(c)) {
            digits++;
            if (dot) { scale /= 10; value += (c - '0') * scale; }
            else value = value * 10 + (c - '0');
        } else if (c == '.' && !dot) {
            dot = 1;
        } else {
            break;
        }
    }
    if (!digits) { calc_fail(p, "Error"); return 0; }
    /* 1.5e+12, as answers are shown: only when a digit follows. */
    char c = p->s[p->pos];
    if (c == 'e' || c == 'E') {
        int at = p->pos + 1, negative = 0;
        if (p->s[at] == '+' || p->s[at] == '-') negative = p->s[at++] == '-';
        if (is_digit(p->s[at])) {
            int exponent = 0;
            while (is_digit(p->s[at]) && exponent < 10000)
                exponent = exponent * 10 + (p->s[at++] - '0');
            while (is_digit(p->s[at])) at++;
            p->pos = at;
            for (int i = 0; i < exponent && value != 0 && c_finite(value); i++)
                value = negative ? value / 10 : value * 10;
        }
    }
    return value;
}

static double to_radians(calc_parser_t* p, double x) {
    return p->degrees ? x * (CALC_PI / 180) : x;
}

static double from_radians(calc_parser_t* p, double x) {
    return p->degrees ? x * (180 / CALC_PI) : x;
}

/* sin, cos or tan. In degrees, whole multiples of 90 are exact, so
 * sin(180) is 0 and tan(90) is an error rather than a huge number. */
static double trig(calc_parser_t* p, double x, int which) {
    if (!c_finite(x) || c_abs(x) > 1e15) { calc_fail(p, "Error"); return 0; }
    if (p->degrees && c_is_whole(x / 90)) {
        int64_t quarter = (int64_t)(x / 90) % 4;
        if (quarter < 0) quarter += 4;
        static const double sin_q[4] = {0, 1, 0, -1};
        static const double cos_q[4] = {1, 0, -1, 0};
        if (which == 0) return sin_q[quarter];
        if (which == 1) return cos_q[quarter];
        if (cos_q[quarter] == 0) { calc_fail(p, "Undefined"); return 0; }
        return 0;
    }
    double r = to_radians(p, x);
    double s = c_trig(r, 0), c = c_trig(r, 1);
    if (which == 0) return c_abs(s) < 1e-15 ? 0 : s;
    if (which == 1) return c_abs(c) < 1e-15 ? 0 : c;
    if (c_abs(c) < 1e-15) { calc_fail(p, "Undefined"); return 0; }
    return s / c;
}

static double power(calc_parser_t* p, double base, double exponent) {
    if (c_is_whole(exponent) && c_abs(exponent) < 2147483648.0) {
        int64_t n = (int64_t)exponent;
        int negative = n < 0;
        if (negative) n = -n;
        if (base == 0 && negative) { calc_fail(p, "Can't divide by 0"); return 0; }
        double result = 1, square = base;
        while (n) {
            if (n & 1) result *= square;
            square *= square;
            n >>= 1;
        }
        return negative ? 1 / result : result;
    }
    if (base == 0) return 0;
    if (base < 0) { calc_fail(p, "Error"); return 0; }
    return c_exp(exponent * c_ln(base));
}

static double apply_function(calc_parser_t* p, const char* name, double x) {
    if (!strcmp_calc(name, "sin")) return trig(p, x, 0);
    if (!strcmp_calc(name, "cos")) return trig(p, x, 1);
    if (!strcmp_calc(name, "tan")) return trig(p, x, 2);
    if (!strcmp_calc(name, "asin") || !strcmp_calc(name, "acos")) {
        if (x < -1 || x > 1) { calc_fail(p, "Error"); return 0; }
        double a = c_abs(x) == 1 ? (x > 0 ? CALC_PI / 2 : -CALC_PI / 2)
                                 : c_atan(x / c_sqrt(1 - x * x));
        if (name[1] == 'c') a = CALC_PI / 2 - a;
        return from_radians(p, a);
    }
    if (!strcmp_calc(name, "atan")) return from_radians(p, c_atan(x));
    if (!strcmp_calc(name, "sqrt")) {
        if (x < 0) { calc_fail(p, "Error"); return 0; }
        return c_sqrt(x);
    }
    if (!strcmp_calc(name, "ln") || !strcmp_calc(name, "log")) {
        if (x <= 0) { calc_fail(p, "Error"); return 0; }
        double l = c_ln(x);
        return name[1] == 'n' ? l : l / CALC_LN10;
    }
    if (!strcmp_calc(name, "abs")) return c_abs(x);
    calc_fail(p, "Unknown name");
    return 0;
}

static int is_function(const char* name) {
    static const char* const names[] = {"sin", "cos", "tan", "asin", "acos",
                                        "atan", "sqrt", "ln", "log", "abs"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (!strcmp_calc(name, names[i])) return 1;
    return 0;
}

static double parse_primary(calc_parser_t* p) {
    char c = peek(p);
    if (p->error) return 0;
    if (++p->depth > 100) { calc_fail(p, "Too long"); return 0; }
    double value = 0;
    if (is_digit(c) || c == '.') {
        value = parse_number(p);
    } else if (c == '(') {
        p->pos++;
        value = parse_expression(p);
        if (peek(p) == ')') p->pos++;
        else if (peek(p)) calc_fail(p, "Error");
    } else if (is_letter(c)) {
        char name[8];
        int length = 0;
        while (is_letter(p->s[p->pos])) {
            if (length < 7) {
                char l = p->s[p->pos];
                name[length++] = l >= 'A' && l <= 'Z' ? (char)(l + 32) : l;
            }
            p->pos++;
        }
        name[length] = '\0';
        if (!strcmp_calc(name, "pi")) value = CALC_PI;
        else if (!strcmp_calc(name, "e")) value = CALC_E;
        else if (!strcmp_calc(name, "ans")) value = p->ans;
        else if (is_function(name)) {
            /* sin(30), or sin 30 */
            double arg = peek(p) == '(' ? parse_primary(p) : parse_power(p);
            value = apply_function(p, name, arg);
        } else calc_fail(p, "Unknown name");
    } else if (!c) {
        calc_fail(p, "Incomplete");
    } else {
        calc_fail(p, "Error");
    }
    p->depth--;
    return value;
}

static double parse_postfix(calc_parser_t* p) {
    double value = parse_primary(p);
    for (;;) {
        char c = peek(p);
        if (c == '!') {
            p->pos++;
            if (!c_is_whole(value) || value < 0) { calc_fail(p, "Error"); return 0; }
            if (value > 170) { calc_fail(p, "Too big"); return 0; }
            double f = 1;
            for (int i = 2; i <= (int)value; i++) f *= i;
            value = f;
        } else if (c == '%') {
            p->pos++;
            value /= 100;
        } else {
            return value;
        }
    }
}

static double parse_power(calc_parser_t* p) {
    double base = parse_postfix(p);
    if (peek(p) != '^') return base;
    p->pos++;
    if (++p->depth > 100) { calc_fail(p, "Too long"); return 0; }
    double exponent = parse_unary(p);
    p->depth--;
    return power(p, base, exponent);
}

static double parse_unary(calc_parser_t* p) {
    char c = peek(p);
    if (c == '-' || c == '+') {
        p->pos++;
        if (++p->depth > 100) { calc_fail(p, "Too long"); return 0; }
        double value = parse_unary(p);
        p->depth--;
        return c == '-' ? -value : value;
    }
    return parse_power(p);
}

static double parse_term(calc_parser_t* p) {
    double value = parse_unary(p);
    while (!p->error) {
        char c = peek(p);
        if (c == '*' || c == 'x' || c == 'X') {
            p->pos++;
            value *= parse_unary(p);
        } else if (c == '/') {
            p->pos++;
            double divisor = parse_unary(p);
            if (divisor == 0 && !p->error) {
                calc_fail(p, "Can't divide by 0");
                return 0;
            }
            value /= divisor;
        } else if (starts_operand(c)) {
            value *= parse_power(p);       /* 2pi, 3(4), 2sin(30) */
        } else {
            break;
        }
    }
    return value;
}

static double parse_expression(calc_parser_t* p) {
    double value = parse_term(p);
    while (!p->error) {
        char c = peek(p);
        if (c == '+') { p->pos++; value += parse_term(p); }
        else if (c == '-') { p->pos++; value -= parse_term(p); }
        else break;
    }
    return value;
}

/* Works out `text`. Returns 1 and the value, or 0 and why not. */
static int calc_evaluate(const char* text, int degrees, double ans,
                         double* value, const char** error) {
    calc_parser_t p = {text, 0, 0, degrees, ans, NULL};
    if (!peek(&p)) { *error = ""; return 0; }
    double v = parse_expression(&p);
    if (!p.error && peek(&p))
        calc_fail(&p, peek(&p) == ')' ? "Too many )" : "Error");
    if (!p.error && !c_finite(v)) calc_fail(&p, "Too big");
    if (p.error) { *error = p.error; return 0; }
    *value = v == 0 ? 0 : v;               /* no "-0" */
    return 1;
}

/* --------------------------------------------------------- formatting */

static char* put_digits(char* out, uint64_t n, int min_digits) {
    char tmp[24];
    int count = 0;
    do { tmp[count++] = (char)('0' + n % 10); n /= 10; } while (n);
    while (count < min_digits) tmp[count++] = '0';
    while (count) *out++ = tmp[--count];
    *out = '\0';
    return out;
}

/* Up to 12 significant digits: "8", "2.5", "0.333333333333", and
 * "1.23456789e+20" for very big or very small numbers. */
static void calc_format(double v, char* out) {
    if (!c_finite(v)) { strcpy_calc(out, "Too big"); return; }
    if (v == 0) { strcpy_calc(out, "0"); return; }
    if (v < 0) { *out++ = '-'; v = -v; }
    int magnitude = 0;                       /* floor(log10(v)) */
    double scaled = v;
    while (scaled >= 10) { scaled /= 10; magnitude++; }
    while (scaled < 1) { scaled *= 10; magnitude--; }
    if (magnitude >= 15 || magnitude < -9) {
        uint64_t digits = (uint64_t)(scaled * 1e9 + 0.5);   /* 10 digits */
        if (digits >= 10000000000ull) { digits /= 10; magnitude++; }
        out = put_digits(out, digits / 1000000000ull, 1);
        uint64_t rest = digits % 1000000000ull;
        if (rest) {
            *out++ = '.';
            char* end = put_digits(out, rest, 9);
            while (end > out && end[-1] == '0') *--end = '\0';
            out = end;
        }
        *out++ = 'e';
        *out++ = magnitude < 0 ? '-' : '+';
        put_digits(out, (uint64_t)(magnitude < 0 ? -magnitude : magnitude), 1);
        return;
    }
    int decimals = 11 - magnitude;
    if (decimals < 0) decimals = 0;
    if (decimals > 15) decimals = 15;
    double factor = 1;
    for (int i = 0; i < decimals; i++) factor *= 10;
    uint64_t total = (uint64_t)(v * factor + 0.5);
    uint64_t f = (uint64_t)factor;
    out = put_digits(out, total / f, 1);
    uint64_t fraction = total % f;
    if (fraction) {
        *out++ = '.';
        char* end = put_digits(out, fraction, decimals);
        while (end > out && end[-1] == '0') *--end = '\0';
    }
}

#ifndef CALC_NO_UI
/* ------------------------------------------------------------- the app */

#define KEY_ROWS 5
#define BASIC_COLUMNS 4
#define SCI_COLUMNS 4
/* Bigger than this and the scientific keys appear. */
#define SCIENTIFIC_WIDTH 540
#define SCIENTIFIC_HEIGHT 360

typedef enum {
    KEY_INSERT,     /* adds its text */
    KEY_EQUALS,
    KEY_CLEAR,
    KEY_BACK,
    KEY_NEGATE,
    KEY_INVERSE,    /* 1/x of the whole entry */
    KEY_ANGLE       /* degrees <-> radians */
} key_kind_t;

typedef struct {
    const char* label;
    const char* insert;
    key_kind_t kind;
    int widget;
} calc_key_t;

static calc_key_t basic_keys[KEY_ROWS][BASIC_COLUMNS] = {
    {{"C", 0, KEY_CLEAR, 0}, {"<-", 0, KEY_BACK, 0},
     {"%", "%", KEY_INSERT, 0}, {"/", "/", KEY_INSERT, 0}},
    {{"7", "7", KEY_INSERT, 0}, {"8", "8", KEY_INSERT, 0},
     {"9", "9", KEY_INSERT, 0}, {"*", "*", KEY_INSERT, 0}},
    {{"4", "4", KEY_INSERT, 0}, {"5", "5", KEY_INSERT, 0},
     {"6", "6", KEY_INSERT, 0}, {"-", "-", KEY_INSERT, 0}},
    {{"1", "1", KEY_INSERT, 0}, {"2", "2", KEY_INSERT, 0},
     {"3", "3", KEY_INSERT, 0}, {"+", "+", KEY_INSERT, 0}},
    {{"+/-", 0, KEY_NEGATE, 0}, {"0", "0", KEY_INSERT, 0},
     {".", ".", KEY_INSERT, 0}, {"=", 0, KEY_EQUALS, 0}},
};

static calc_key_t sci_keys[KEY_ROWS][SCI_COLUMNS] = {
    {{"Deg", 0, KEY_ANGLE, 0}, {"(", "(", KEY_INSERT, 0},
     {")", ")", KEY_INSERT, 0}, {"x^y", "^", KEY_INSERT, 0}},
    {{"sin", "sin(", KEY_INSERT, 0}, {"cos", "cos(", KEY_INSERT, 0},
     {"tan", "tan(", KEY_INSERT, 0}, {"sqrt", "sqrt(", KEY_INSERT, 0}},
    {{"asin", "asin(", KEY_INSERT, 0}, {"acos", "acos(", KEY_INSERT, 0},
     {"atan", "atan(", KEY_INSERT, 0}, {"x^2", "^2", KEY_INSERT, 0}},
    {{"ln", "ln(", KEY_INSERT, 0}, {"log", "log(", KEY_INSERT, 0},
     {"e", "e", KEY_INSERT, 0}, {"pi", "pi", KEY_INSERT, 0}},
    {{"n!", "!", KEY_INSERT, 0}, {"1/x", 0, KEY_INVERSE, 0},
     {"abs", "abs(", KEY_INSERT, 0}, {"ans", "ans", KEY_INSERT, 0}},
};

static int entry;           /* the text input with the calculation */
static int history;         /* the last calculation, above it */
static int degrees = 1;
static double last_answer;
/* Just showing an answer: a digit starts afresh, an operator goes on
 * from the answer. */
static int showing_answer;

static calc_key_t* key_for(int widget) {
    for (int r = 0; r < KEY_ROWS; r++) {
        for (int c = 0; c < BASIC_COLUMNS; c++)
            if (basic_keys[r][c].widget == widget) return &basic_keys[r][c];
        for (int c = 0; c < SCI_COLUMNS; c++)
            if (sci_keys[r][c].widget == widget) return &sci_keys[r][c];
    }
    return 0;
}

/* a + b + c into `out` (cut short if it does not fit). */
static void join3(char* out, size_t capacity, const char* a, const char* b,
                  const char* c) {
    const char* parts[3] = {a, b, c};
    size_t at = 0;
    for (int i = 0; i < 3; i++)
        for (const char* p = parts[i]; *p && at + 1 < capacity; p++)
            out[at++] = *p;
    out[at] = '\0';
}

static void set_entry(const char* text) {
    easyui.settext(entry, text);
}

static void calculate(void) {
    const char* text = easyui.gettext(entry);
    double value;
    const char* error;
    char line[EASYUI_TEXT_MAX + 4];
    if (!calc_evaluate(text, degrees, last_answer, &value, &error)) {
        if (error[0]) easyui.settext(history, error);
        return;
    }
    join3(line, sizeof line, text, " =", "");
    easyui.settext(history, line);
    char result[40];
    calc_format(value, result);
    last_answer = value;
    set_entry(result);
    showing_answer = 1;
}

static int is_operator_text(const char* s) {
    return s[0] == '+' || s[0] == '-' || s[0] == '*' || s[0] == '/' ||
           s[0] == '^' || s[0] == '%' || s[0] == '!';
}

static void insert_text(const char* add) {
    char text[EASYUI_TEXT_MAX];
    const char* old = easyui.gettext(entry);
    /* After an answer, a number or function starts a new calculation;
     * an operator carries on from the answer. */
    if (showing_answer && !is_operator_text(add)) old = "";
    showing_answer = 0;
    if (strlen(old) + strlen(add) >= sizeof text) return;
    strcpy(text, old);
    strcpy(text + strlen(text), add);
    set_entry(text);
}

static void on_key(int value) {
    (void)value;
    calc_key_t* key = key_for(easyui.current());
    if (!key) return;
    char text[EASYUI_TEXT_MAX];
    const char* old = easyui.gettext(entry);
    switch (key->kind) {
    case KEY_INSERT:
        insert_text(key->insert);
        break;
    case KEY_EQUALS:
        calculate();
        break;
    case KEY_CLEAR:
        set_entry("");
        easyui.settext(history, "");
        showing_answer = 0;
        break;
    case KEY_BACK: {
        size_t length = strlen(old);
        if (!length) break;
        strlcpy_safe(text, old, sizeof text);
        /* A whole function name goes at once: "sin(" */
        size_t cut = length - 1;
        if (text[cut] == '(')
            while (cut > 0 && text[cut - 1] >= 'a' && text[cut - 1] <= 'z')
                cut--;
        text[cut] = '\0';
        set_entry(text);
        showing_answer = 0;
        break;
    }
    case KEY_NEGATE:
    case KEY_INVERSE:
        if (!old[0]) break;
        if (key->kind == KEY_NEGATE && old[0] == '-' && old[1] == '(' &&
            old[strlen(old) - 1] == ')') {
            strlcpy_safe(text, old + 2, sizeof text);   /* -(x) -> x */
            text[strlen(text) - 1] = '\0';
        } else {
            char wrapped[EASYUI_TEXT_MAX];
            join3(wrapped, sizeof wrapped,
                  key->kind == KEY_NEGATE ? "-(" : "1/(", old, ")");
            strlcpy_safe(text, wrapped, sizeof text);
        }
        set_entry(text);
        showing_answer = 0;
        break;
    case KEY_ANGLE:
        degrees = !degrees;
        easyui.settext(key->widget, degrees ? "Deg" : "Rad");
        break;
    }
    easyui.setfocus(entry);
}

static void on_submit(int value) {
    (void)value;
    calculate();
}

/* Typing into the entry after an answer edits it, as you would expect. */
static void on_typed(int value) {
    (void)value;
    showing_answer = 0;
}

/* Places every key for the window size; big windows get the scientific
 * keys on the left. */
static void layout(int width, int height) {
    const int margin = 12, gap = 6;
    int scientific = width >= SCIENTIFIC_WIDTH && height >= SCIENTIFIC_HEIGHT;
    int history_h = 20, entry_h = scientific ? 54 : 48;
    easyui.setposition(history, margin, margin);
    easyui.setsize(history, width - 2 * margin, history_h);
    easyui.setposition(entry, margin, margin + history_h + 2);
    easyui.setsize(entry, width - 2 * margin, entry_h);
    int top = margin + history_h + 2 + entry_h + 12;
    int columns = BASIC_COLUMNS + (scientific ? SCI_COLUMNS : 0);
    int section_gap = scientific ? 14 : 0;
    int key_w = (width - 2 * margin - gap * (columns - 1) - section_gap) /
                columns;
    int key_h = (height - top - margin - gap * (KEY_ROWS - 1)) / KEY_ROWS;
    if (key_w < 30) key_w = 30;
    if (key_h < 26) key_h = 26;
    for (int r = 0; r < KEY_ROWS; r++) {
        int y = top + r * (key_h + gap);
        for (int c = 0; c < SCI_COLUMNS; c++) {
            calc_key_t* key = &sci_keys[r][c];
            easyui.setvisible(key->widget, scientific);
            easyui.setposition(key->widget, margin + c * (key_w + gap), y);
            easyui.setsize(key->widget, key_w, key_h);
        }
        int left = margin + (scientific ? SCI_COLUMNS * (key_w + gap) +
                                          section_gap : 0);
        for (int c = 0; c < BASIC_COLUMNS; c++) {
            calc_key_t* key = &basic_keys[r][c];
            easyui.setposition(key->widget, left + c * (key_w + gap), y);
            easyui.setsize(key->widget, key_w, key_h);
        }
    }
    easyui.redraw();
}

static void on_resize(int value) {
    (void)value;
    layout(easyui.width(), easyui.height());
}

int main(void) {
    if (!easyui.init("Calculator", 340, 420)) return 1;
    easyui.setsmoothtext(1);
    mos_window_set_resizable(easyui.window(), true);

    history = easyui.createlabel(0, 0, 10, 10, "");
    easyui.setalign(history, EASYUI_ALIGN_RIGHT);
    entry = easyui.createtextinput(0, 0, 10, 10, "Type a calculation...");
    easyui.settextsize(entry, 2);
    easyui.recallfunc(entry, ONSUBMIT, on_submit);
    easyui.recallfunc(entry, ONCHANGE, on_typed);

    for (int r = 0; r < KEY_ROWS; r++) {
        for (int c = 0; c < BASIC_COLUMNS; c++) {
            calc_key_t* key = &basic_keys[r][c];
            key->widget = easyui.createbutton(0, 0, 10, 10, key->label);
            easyui.setstyle(key->widget, key->kind == KEY_EQUALS
                                             ? EASYUI_PRIMARY
                                             : EASYUI_SECONDARY);
            easyui.recallfunc(key->widget, ONPRESS, on_key);
        }
        for (int c = 0; c < SCI_COLUMNS; c++) {
            calc_key_t* key = &sci_keys[r][c];
            key->widget = easyui.createbutton(0, 0, 10, 10, key->label);
            easyui.setstyle(key->widget, EASYUI_SECONDARY);
            easyui.recallfunc(key->widget, ONPRESS, on_key);
        }
    }
    easyui.recallfunc(EASYUI_WINDOW, ONRESIZE, on_resize);
    layout(easyui.width(), easyui.height());
    easyui.setfocus(entry);

    while (true) {
        easyui.run();
        mos_sleep(16);
    }
}
#endif
