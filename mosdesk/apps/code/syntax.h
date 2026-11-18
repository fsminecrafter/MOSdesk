/*
 * syntax.h - syntax highlighting for MOSdesk Code (apps/code/main.c)
 *
 * One small scanner serves every language. A Lang says which keywords,
 * comment markers and string forms a language has; scan_line() colours one
 * line of text given the state the previous line ended in (block comment,
 * triple-quoted string, ...), so a document only has to remember one byte
 * per line and rescan from the first line that was edited.
 *
 * Header only: it has no dependencies except <string.h>-style helpers that
 * main.c already pulls in.
 */
#ifndef CODE_SYNTAX_H
#define CODE_SYNTAX_H

/* colour classes written into the per-byte colour array */
enum {
    HC_TEXT = 0, HC_KEYWORD, HC_CONTROL, HC_TYPE, HC_FUNC, HC_NUMBER, HC_STRING,
    HC_ESCAPE, HC_COMMENT, HC_PREPROC, HC_CONST, HC_VAR, HC_HEADING, HC_CODE,
    HC_LINK, HC_BOLD, HC_TARGET, HC_DECOR, HC_PROP, HC_COUNT
};

/* what a line ends in, carried to the next line */
enum { ST_NORMAL = 0, ST_BLOCK = 1, ST_TRIPLE_DQ = 2, ST_TRIPLE_SQ = 3, ST_FENCE = 4,
       ST_LONGSTR = 5, ST_TEMPLATE = 6 };

enum { LANG_TEXT = 0, LANG_C, LANG_PY, LANG_SH, LANG_LUA, LANG_JS, LANG_JSON, LANG_MD,
       LANG_MAKE, LANG_COUNT };

/* flags */
#define LF_PREPROC   0x001u   /* '#' directives at the start of a line */
#define LF_CHAR      0x002u   /* 'x' is a character literal */
#define LF_SQSTR     0x004u   /* 'x' is a string */
#define LF_DOLLAR    0x008u   /* $VAR, ${VAR}, $(VAR) */
#define LF_BACKTICK  0x010u   /* `...` template literal / command substitution */
#define LF_TRIPLE    0x020u   /* """ and ''' strings */
#define LF_LUALONG   0x040u   /* --[[ ]] and [[ ]] */
#define LF_CAPSCONST 0x080u   /* ALL_CAPS identifiers are constants */
#define LF_DECOR     0x100u   /* @decorator at the start of a line */
#define LF_JSONKEY   0x200u   /* "key": gets the property colour */
#define LF_STRPREFIX 0x400u   /* r"..", f"..", b"..", u".." */
#define LF_MARKDOWN  0x800u
#define LF_MAKEFILE  0x1000u
#define LF_NESTED_NO 0x2000u

typedef struct {
    const char* name;
    const char* kw;        /* declaration / storage keywords */
    const char* ctl;       /* control-flow keywords */
    const char* types;
    const char* consts;
    const char* lc;        /* line comment, or NULL */
    const char* bo;        /* block comment open, or NULL */
    const char* bc;        /* block comment close */
    unsigned flags;
} Lang;

static const Lang g_langs[LANG_COUNT] = {
    [LANG_TEXT] = { "Plain Text", "", "", "", "", 0, 0, 0, 0 },
    [LANG_C] = { "C/C++",
        "auto const constexpr extern inline register restrict static volatile typedef struct union "
        "enum class namespace template typename using public private protected virtual override final "
        "explicit friend mutable operator sizeof alignof new delete this noexcept thread_local "
        "_Atomic _Alignas _Noreturn __attribute__ __asm__ asm decltype static_assert export module "
        "import concept requires co_await co_return co_yield",
        "if else for while do switch case default break continue return goto try catch throw",
        "void char short int long float double signed unsigned bool _Bool size_t ssize_t ptrdiff_t "
        "intptr_t uintptr_t int8_t int16_t int32_t int64_t uint8_t uint16_t uint32_t uint64_t FILE "
        "wchar_t char8_t char16_t char32_t string vector map set array unique_ptr shared_ptr "
        "std nullptr_t va_list off_t time_t",
        "NULL true false nullptr EOF TRUE FALSE __LINE__ __FILE__ __func__ __cplusplus",
        "//", "/*", "*/", LF_PREPROC | LF_CHAR | LF_CAPSCONST },
    [LANG_PY] = { "Python",
        "def class lambda global nonlocal import from as with async await yield del pass assert "
        "print exec",
        "if elif else for while break continue return try except finally raise in is not and or "
        "match case",
        "int str float bool list dict set tuple bytes bytearray object type range frozenset complex "
        "self cls super len open",
        "None True False Ellipsis NotImplemented __name__ __main__ __init__",
        "#", 0, 0, LF_SQSTR | LF_TRIPLE | LF_DECOR | LF_STRPREFIX | LF_CAPSCONST },
    [LANG_SH] = { "Shell",
        "function local export readonly declare typeset unset alias source set shift trap eval exec "
        "cd echo printf read test exit return",
        "if then elif else fi for while until do done case esac in select break continue",
        "", "true false",
        "#", 0, 0, LF_SQSTR | LF_DOLLAR | LF_BACKTICK },
    [LANG_LUA] = { "Lua",
        "local function end and or not require",
        "if then elseif else for while do repeat until break return goto in",
        "string table math io os coroutine debug utf8",
        "nil true false self _G _ENV",
        "--", 0, 0, LF_SQSTR | LF_LUALONG },
    [LANG_JS] = { "JavaScript",
        "var let const function class extends async await yield static get set new delete typeof "
        "instanceof void import export from as of this super interface type enum implements "
        "public private protected readonly declare namespace abstract",
        "if else for while do switch case default break continue return throw try catch finally with "
        "in",
        "number string boolean any unknown never object Array Object Promise Map Set Date RegExp "
        "Error JSON Math Symbol",
        "null undefined true false NaN Infinity console document window",
        "//", "/*", "*/", LF_SQSTR | LF_BACKTICK },
    [LANG_JSON] = { "JSON", "", "", "", "true false null",
        0, 0, 0, LF_JSONKEY },
    [LANG_MD] = { "Markdown", "", "", "", "", 0, 0, 0, LF_MARKDOWN },
    [LANG_MAKE] = { "Makefile",
        "include -include sinclude define endef export unexport override vpath private",
        "ifeq ifneq ifdef ifndef else endif",
        "", ".PHONY .DEFAULT .SUFFIXES .PRECIOUS .INTERMEDIATE .SECONDARY .DELETE_ON_ERROR",
        "#", 0, 0, LF_DOLLAR | LF_MAKEFILE },
};

/* ---- word tables: a small open-addressing hash set per language ---- */
#define WT_SIZE 512
typedef struct { const char* p; unsigned char len, cls; } WordEnt;
static WordEnt g_wt[LANG_COUNT][WT_SIZE];
static unsigned char g_wt_ready[LANG_COUNT];

static unsigned wt_hash(const char* s, int n) {
    unsigned h = 2166136261u; int i;
    for (i = 0; i < n; i++) h = (h ^ (unsigned char)s[i]) * 16777619u;
    return h;
}
static void wt_add(int lang, const char* list, int cls) {
    const char* p = list;
    while (*p) {
        const char* q; int n; unsigned h, k;
        while (*p == ' ') p++;
        if (!*p) break;
        q = p; while (*q && *q != ' ') q++;
        n = (int)(q - p);
        if (n > 0 && n < 64) {
            h = wt_hash(p, n);
            for (k = 0; k < WT_SIZE; k++) {
                WordEnt* e = &g_wt[lang][(h + k) & (WT_SIZE - 1)];
                if (!e->p) { e->p = p; e->len = (unsigned char)n; e->cls = (unsigned char)cls; break; }
            }
        }
        p = q;
    }
}
static void wt_build(int lang) {
    if (g_wt_ready[lang]) return;
    g_wt_ready[lang] = 1;
    wt_add(lang, g_langs[lang].kw, HC_KEYWORD);
    wt_add(lang, g_langs[lang].ctl, HC_CONTROL);
    wt_add(lang, g_langs[lang].types, HC_TYPE);
    wt_add(lang, g_langs[lang].consts, HC_CONST);
}
static int wt_find(int lang, const char* s, int n) {
    unsigned h, k;
    if (n <= 0 || n >= 64) return 0;
    h = wt_hash(s, n);
    for (k = 0; k < WT_SIZE; k++) {
        const WordEnt* e = &g_wt[lang][(h + k) & (WT_SIZE - 1)];
        if (!e->p) return 0;
        if (e->len == n && memcmp(e->p, s, (size_t)n) == 0) return e->cls;
    }
    return 0;
}

/* ---- helpers ---- */
static int hs_alpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static int hs_digit(int c) { return c >= '0' && c <= '9'; }
static int hs_word(int c)  { return hs_alpha(c) || hs_digit(c); }
static int hs_starts(const char* s, int n, int i, const char* t) {
    int k = 0;
    if (!t || !t[0]) return 0;
    while (t[k]) { if (i + k >= n || s[i + k] != t[k]) return 0; k++; }
    return 1;
}
static void hs_fill(unsigned char* col, int cap, int a, int b, int c) {
    if (a < 0) a = 0;
    if (b > cap) b = cap;
    while (a < b) col[a++] = (unsigned char)c;
}
/* index just past the closing text `t` at or after i, or -1 */
static int hs_find(const char* s, int n, int i, const char* t) {
    for (; i < n; i++) if (hs_starts(s, n, i, t)) { int k = 0; while (t[k]) k++; return i + k; }
    return -1;
}
/* end of a quoted string that starts at i (s[i] is the quote); returns the
 * index past the closing quote, or n when the line ends first */
static int hs_quoted(const char* s, int n, int i, unsigned char* col, int cap, int cls, int escapes) {
    char q = s[i]; int j = i + 1;
    while (j < n) {
        if (s[j] == '\\' && j + 1 < n) {
            if (escapes) hs_fill(col, cap, j, j + 2, HC_ESCAPE);
            j += 2; continue;
        }
        if (s[j] == q) { j++; break; }
        j++;
    }
    /* colour the body (escapes are painted over the string colour) */
    {
        int k = i;
        while (k < j) {
            if (k >= cap) break;
            if (col[k] != HC_ESCAPE || k == i) col[k] = (unsigned char)cls;
            k++;
        }
    }
    return j;
}

/* Colours one line. `col` holds one class per byte (only the first `cap`
 * bytes are coloured, the rest of a very long line stays plain). Returns the
 * state the next line starts in. */
static int scan_line(int lang, const char* s, int n, int st, unsigned char* col, int cap) {
    const Lang* L = &g_langs[lang];
    unsigned fl = L->flags;
    int i = 0, first = 1, k;
    if (n > cap) n = cap;                 /* very long lines: only the head is coloured */
    hs_fill(col, cap, 0, n, HC_TEXT);
    wt_build(lang);

    /* ---- continue a multi-line construct ---- */
    if (st == ST_BLOCK) {
        k = hs_find(s, n, 0, L->bc ? L->bc : "*/");
        if (lang == LANG_LUA) k = hs_find(s, n, 0, "]]");
        if (k < 0) { hs_fill(col, cap, 0, n, HC_COMMENT); return ST_BLOCK; }
        hs_fill(col, cap, 0, k, HC_COMMENT); i = k; st = ST_NORMAL;
    } else if (st == ST_TRIPLE_DQ || st == ST_TRIPLE_SQ) {
        k = hs_find(s, n, 0, st == ST_TRIPLE_DQ ? "\"\"\"" : "'''");
        if (k < 0) { hs_fill(col, cap, 0, n, HC_STRING); return st; }
        hs_fill(col, cap, 0, k, HC_STRING); i = k; st = ST_NORMAL;
    } else if (st == ST_LONGSTR) {
        k = hs_find(s, n, 0, "]]");
        if (k < 0) { hs_fill(col, cap, 0, n, HC_STRING); return st; }
        hs_fill(col, cap, 0, k, HC_STRING); i = k; st = ST_NORMAL;
    } else if (st == ST_TEMPLATE) {
        int j = 0;
        while (j < n && s[j] != '`') { if (s[j] == '\\') j++; j++; }
        if (j >= n) { hs_fill(col, cap, 0, n, HC_STRING); return st; }
        hs_fill(col, cap, 0, j + 1, HC_STRING); i = j + 1; st = ST_NORMAL;
    } else if (st == ST_FENCE) {
        hs_fill(col, cap, 0, n, HC_CODE);
        k = 0; while (k < n && s[k] == ' ') k++;
        if (hs_starts(s, n, k, "```") || hs_starts(s, n, k, "~~~")) return ST_NORMAL;
        return ST_FENCE;
    }

    /* ---- Markdown: block structure first ---- */
    if (fl & LF_MARKDOWN) {
        k = 0; while (k < n && s[k] == ' ') k++;
        if (hs_starts(s, n, k, "```") || hs_starts(s, n, k, "~~~")) { hs_fill(col, cap, 0, n, HC_CODE); return ST_FENCE; }
        if (k < n && s[k] == '#') {
            int h = k; while (h < n && s[h] == '#') h++;
            if (h - k <= 6 && (h >= n || s[h] == ' ')) { hs_fill(col, cap, 0, n, HC_HEADING); return ST_NORMAL; }
        }
        if (k < n && s[k] == '>') { hs_fill(col, cap, 0, n, HC_COMMENT); return ST_NORMAL; }
        if (hs_starts(s, n, k, "---") || hs_starts(s, n, k, "***") || hs_starts(s, n, k, "___")) {
            int all = 1, j; for (j = k; j < n; j++) if (s[j] != s[k] && s[j] != ' ') all = 0;
            if (all) { hs_fill(col, cap, 0, n, HC_COMMENT); return ST_NORMAL; }
        }
        if (k + 1 < n && (s[k] == '-' || s[k] == '*' || s[k] == '+') && s[k + 1] == ' ') { col[k] = HC_KEYWORD; i = k + 1; }
        else if (hs_digit(k < n ? s[k] : 0)) {
            int j = k; while (j < n && hs_digit(s[j])) j++;
            if (j + 1 < n && (s[j] == '.' || s[j] == ')') && s[j + 1] == ' ') { hs_fill(col, cap, k, j + 1, HC_KEYWORD); i = j + 1; }
        }
        while (i < n) {
            int c = s[i];
            if (c == '`') {
                k = i + 1; while (k < n && s[k] != '`') k++;
                if (k < n) { hs_fill(col, cap, i, k + 1, HC_CODE); i = k + 1; continue; }
            } else if ((c == '*' || c == '_') && i + 1 < n && s[i + 1] == c) {
                k = hs_find(s, n, i + 2, c == '*' ? "**" : "__");
                if (k > 0) { hs_fill(col, cap, i, k, HC_BOLD); i = k; continue; }
            } else if (c == '[') {
                k = i + 1; while (k < n && s[k] != ']') k++;
                if (k + 1 < n && s[k + 1] == '(') {
                    int e = k + 2; while (e < n && s[e] != ')') e++;
                    if (e < n) { hs_fill(col, cap, i, e + 1, HC_LINK); i = e + 1; continue; }
                }
            } else if (c == '<' ) {
                k = i + 1; while (k < n && s[k] != '>') k++;
                if (k < n && k > i + 1 && (hs_alpha(s[i + 1]) || s[i + 1] == '/' || s[i + 1] == '!')) { hs_fill(col, cap, i, k + 1, HC_PREPROC); i = k + 1; continue; }
            }
            i++;
        }
        return ST_NORMAL;
    }

    /* ---- Makefile: targets and recipe lines ---- */
    if (fl & LF_MAKEFILE) {
        if (n > 0 && s[0] == '\t') {
            /* recipe line: shell, so only variables and comments stand out */
            first = 0;
        } else {
            int j = 0, ok = 0;
            while (j < n && s[j] != ':' && s[j] != '=' && s[j] != '#' && s[j] != ' ' && s[j] != '\t') {
                if (s[j] == '$' && j + 1 < n && s[j + 1] == '(') { while (j < n && s[j] != ')') j++; }
                j++;
            }
            if (j < n && j > 0) {
                int e = j; while (e < n && (s[e] == ' ' || s[e] == '\t')) e++;
                if (e < n && s[e] == ':' && !(e + 1 < n && s[e + 1] == '=') && !(e + 1 < n && s[e + 1] == ':' && e + 2 < n && s[e + 2] == '=')) ok = 1;
                if (ok && wt_find(lang, s, j) == 0) { hs_fill(col, cap, 0, j, HC_TARGET); i = j; }
                else if (e < n && (s[e] == '=' || ((s[e] == ':' || s[e] == '?' || s[e] == '+') && e + 1 < n && (s[e + 1] == '=' || (s[e + 1] == ':' && e + 2 < n && s[e + 2] == '='))))) {
                    hs_fill(col, cap, 0, j, HC_VAR); i = j;
                }
            }
        }
    }

    /* ---- Python decorator ---- */
    if ((fl & LF_DECOR) && i == 0) {
        k = 0; while (k < n && (s[k] == ' ' || s[k] == '\t')) k++;
        if (k < n && s[k] == '@') { int e = k + 1; while (e < n && (hs_word(s[e]) || s[e] == '.')) e++; hs_fill(col, cap, k, e, HC_DECOR); i = e; }
    }

    while (i < n) {
        int c = (unsigned char)s[i];
        if (c == ' ' || c == '\t') { i++; continue; }

        /* preprocessor */
        if (first && (fl & LF_PREPROC) && c == '#') {
            int e = i + 1, d;
            while (e < n && (s[e] == ' ' || s[e] == '\t')) e++;
            d = e; while (d < n && hs_alpha(s[d])) d++;
            hs_fill(col, cap, i, d, HC_PREPROC);
            if (d - e == 7 && memcmp(s + e, "include", 7) == 0) {
                int a = d; while (a < n && (s[a] == ' ' || s[a] == '\t')) a++;
                if (a < n && s[a] == '<') {
                    int z = a; while (z < n && s[z] != '>') z++;
                    if (z < n) z++;
                    hs_fill(col, cap, a, z, HC_STRING); d = z;
                }
            }
            i = d; first = 0; continue;
        }
        first = 0;

        /* line comment */
        if (L->lc && hs_starts(s, n, i, L->lc)) {
            if ((fl & LF_LUALONG) && hs_starts(s, n, i, "--[[")) {
                k = hs_find(s, n, i + 4, "]]");
                if (k < 0) { hs_fill(col, cap, i, n, HC_COMMENT); return ST_BLOCK; }
                hs_fill(col, cap, i, k, HC_COMMENT); i = k; continue;
            }
            /* shell: $# and ${#x} are not comments */
            if (!((fl & LF_DOLLAR) && L->lc[0] == '#' && i > 0 && (s[i - 1] == '$' || s[i - 1] == '{'))) {
                hs_fill(col, cap, i, n, HC_COMMENT); return ST_NORMAL;
            }
        }
        /* block comment */
        if (L->bo && hs_starts(s, n, i, L->bo)) {
            k = hs_find(s, n, i + 2, L->bc);
            if (k < 0) { hs_fill(col, cap, i, n, HC_COMMENT); return ST_BLOCK; }
            hs_fill(col, cap, i, k, HC_COMMENT); i = k; continue;
        }
        /* Python: triple-quoted strings */
        if ((fl & LF_TRIPLE) && (hs_starts(s, n, i, "\"\"\"") || hs_starts(s, n, i, "'''"))) {
            int dq = s[i] == '"';
            k = hs_find(s, n, i + 3, dq ? "\"\"\"" : "'''");
            if (k < 0) { hs_fill(col, cap, i, n, HC_STRING); return dq ? ST_TRIPLE_DQ : ST_TRIPLE_SQ; }
            hs_fill(col, cap, i, k, HC_STRING); i = k; continue;
        }
        /* Lua long string */
        if ((fl & LF_LUALONG) && hs_starts(s, n, i, "[[")) {
            k = hs_find(s, n, i + 2, "]]");
            if (k < 0) { hs_fill(col, cap, i, n, HC_STRING); return ST_LONGSTR; }
            hs_fill(col, cap, i, k, HC_STRING); i = k; continue;
        }
        /* strings */
        if (c == '"') {
            int e = hs_quoted(s, n, i, col, cap, HC_STRING, 1);
            if (fl & LF_JSONKEY) {
                int a = e; while (a < n && (s[a] == ' ' || s[a] == '\t')) a++;
                if (a < n && s[a] == ':') hs_fill(col, cap, i, e, HC_PROP);
            }
            i = e; continue;
        }
        if (c == '\'') {
            if (fl & LF_CHAR) {
                /* 'x', '\n', '\x41' - but not the digit separator in 1'000 */
                int e = i + 1, ok = 0;
                if (e < n && s[e] == '\\') e += 2; else e++;
                while (e < n && e < i + 10 && s[e] != '\'') e++;
                if (e < n && s[e] == '\'') ok = 1;
                if (ok) { hs_quoted(s, n, i, col, cap, HC_STRING, 1); i = e + 1; continue; }
                i++; continue;
            }
            if (fl & LF_SQSTR) { i = hs_quoted(s, n, i, col, cap, HC_STRING, 1); continue; }
        }
        if (c == '`' && (fl & LF_BACKTICK)) {
            if (lang == LANG_SH) {
                k = i + 1; while (k < n && s[k] != '`') k++;
                if (k < n) k++;
                hs_fill(col, cap, i, k, HC_STRING); i = k; continue;
            } else {
                int j = i + 1;
                while (j < n && s[j] != '`') { if (s[j] == '\\') j++; j++; }
                if (j >= n) { hs_fill(col, cap, i, n, HC_STRING); return ST_TEMPLATE; }
                hs_fill(col, cap, i, j + 1, HC_STRING); i = j + 1; continue;
            }
        }
        /* $VAR */
        if (c == '$' && (fl & LF_DOLLAR)) {
            int e = i + 1;
            if (e < n && (s[e] == '(' || s[e] == '{')) {
                char close = s[e] == '(' ? ')' : '}';
                int depth = 0;
                for (; e < n; e++) { if (s[e] == s[i + 1]) depth++; else if (s[e] == close && --depth == 0) { e++; break; } }
            } else if (e < n && hs_word(s[e])) { while (e < n && hs_word(s[e])) e++; }
            else if (e < n) e++;
            hs_fill(col, cap, i, e, HC_VAR); i = e; continue;
        }
        /* numbers */
        if (hs_digit(c) || (c == '.' && i + 1 < n && hs_digit(s[i + 1]) && (i == 0 || !hs_word(s[i - 1])))) {
            int e = i;
            if (i > 0 && hs_word(s[i - 1])) { i++; continue; }
            if (c == '0' && e + 1 < n && (s[e + 1] == 'x' || s[e + 1] == 'X')) e += 2;
            while (e < n && (hs_word(s[e]) || s[e] == '.' || ((s[e] == '+' || s[e] == '-') && (s[e - 1] == 'e' || s[e - 1] == 'E') && !(e > i + 1 && s[i + 1] == 'x')))) e++;
            hs_fill(col, cap, i, e, HC_NUMBER); i = e; continue;
        }
        /* identifiers and keywords */
        if (hs_alpha(c)) {
            int e = i, cls, nx;
            while (e < n && hs_word(s[e])) e++;
            /* Python string prefixes: f"..", rb'..' */
            if ((fl & LF_STRPREFIX) && e - i <= 2 && e < n && (s[e] == '"' || s[e] == '\'')) {
                int p, okp = 1;
                for (p = i; p < e; p++) { int ch = s[p] | 32; if (ch != 'r' && ch != 'b' && ch != 'f' && ch != 'u') okp = 0; }
                if (okp) {
                    int z = hs_quoted(s, n, e, col, cap, HC_STRING, 1);
                    hs_fill(col, cap, i, e, HC_STRING); i = z; continue;
                }
            }
            cls = wt_find(lang, s + i, e - i);
            /* Makefile / shell words after a '.' or '-' are plain; ignore */
            if (!cls) {
                nx = e; while (nx < n && (s[nx] == ' ' || s[nx] == '\t') && nx < e + 1) nx++;
                if (nx < n && s[nx] == '(' && lang != LANG_MAKE && lang != LANG_SH) cls = HC_FUNC;
                else if ((fl & LF_CAPSCONST) && e - i >= 2) {
                    int p, caps = 1; for (p = i; p < e; p++) if (!((s[p] >= 'A' && s[p] <= 'Z') || hs_digit(s[p]) || s[p] == '_')) caps = 0;
                    if (caps) cls = HC_CONST;
                }
                if (!cls && lang == LANG_C && e - i > 2 && s[e - 2] == '_' && s[e - 1] == 't') cls = HC_TYPE;
            }
            if (cls) hs_fill(col, cap, i, e, cls);
            i = e; continue;
        }
        i++;
    }
    return ST_NORMAL;
}

/* Language from a file name. */
static int lang_ext_eq(const char* a, const char* b) {
    while (*a && *b) { char x = *a, y = *b; if (x >= 'A' && x <= 'Z') x += 32; if (y >= 'A' && y <= 'Z') y += 32; if (x != y) return 0; a++; b++; }
    return *a == *b;
}
static int lang_for_name(const char* name) {
    const char* base = name; const char* dot = 0; const char* p;
    static const struct { const char* ext; int lang; } map[] = {
        {"c", LANG_C}, {"h", LANG_C}, {"cpp", LANG_C}, {"cc", LANG_C}, {"cxx", LANG_C}, {"hpp", LANG_C},
        {"hh", LANG_C}, {"hxx", LANG_C}, {"ino", LANG_C}, {"inl", LANG_C},
        {"py", LANG_PY}, {"pyw", LANG_PY},
        {"sh", LANG_SH}, {"bash", LANG_SH}, {"zsh", LANG_SH}, {"ksh", LANG_SH},
        {"lua", LANG_LUA},
        {"js", LANG_JS}, {"mjs", LANG_JS}, {"cjs", LANG_JS}, {"ts", LANG_JS}, {"jsx", LANG_JS}, {"tsx", LANG_JS},
        {"json", LANG_JSON}, {"jsonc", LANG_JSON},
        {"md", LANG_MD}, {"markdown", LANG_MD},
        {"mk", LANG_MAKE}, {"mak", LANG_MAKE},
    };
    unsigned i;
    for (p = name; *p; p++) { if (*p == '/' || *p == ':') { base = p + 1; dot = 0; } else if (*p == '.') dot = p; }
    if (lang_ext_eq(base, "Makefile") || lang_ext_eq(base, "GNUmakefile")) return LANG_MAKE;
    if (lang_ext_eq(base, ".bashrc") || lang_ext_eq(base, ".profile") || lang_ext_eq(base, ".zshrc")) return LANG_SH;
    if (dot && dot != base) {
        for (i = 0; i < sizeof map / sizeof map[0]; i++) if (lang_ext_eq(dot + 1, map[i].ext)) return map[i].lang;
    }
    return LANG_TEXT;
}
/* line-comment marker for Ctrl+/ */
static const char* lang_comment(int lang) { return g_langs[lang].lc; }

#endif
