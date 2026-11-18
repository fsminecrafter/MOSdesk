/*
 * Code - a small code editor for MOSdesk (easyui)
 *
 *     ./build.sh --output mos_code.run apps/code
 *     run 0:/programs/mos_code.run [file-or-folder ...]
 *
 * Bundled with MOSdesk: mosdesk/build.sh installs it as
 * 0:/programs/mos_code.run.
 *
 * Features
 *   Tabs (dirty dot, close button), Ctrl+Tab / Ctrl+W / Ctrl+N, folder
 *   sidebar (Ctrl+B), line numbers, current-line and matching-bracket
 *   highlight, syntax colours (C/C++, Python, shell, Lua, JavaScript/JSON,
 *   Markdown, Makefile), auto-indent, auto-close brackets and quotes,
 *   Ctrl+/ comment, Ctrl+D duplicate, Alt+Up/Down move line, undo/redo with
 *   typing groups, Find / Replace (Ctrl+F / Ctrl+H, F3), Go to line (Ctrl+G),
 *   Open / Save / Save As (Ctrl+O / Ctrl+S / Ctrl+Shift+S), mouse selection,
 *   double-click word, scrollbars, wheel and Shift+wheel.
 *   Ctrl+= / Ctrl+- change the font size.
 *   Word completion from the open files (Ctrl+Space, or by itself after
 *   three letters; Tab/Enter take one), the word under the caret marked
 *   where else it appears, Go to file (Ctrl+P, fuzzy over the folder),
 *   the command palette (Ctrl+Shift+P: save all, sort lines, upper/lower
 *   case, trim trailing whitespace, line endings ...), and the last
 *   session's files and folder reopened when Code starts without files.
 *
 * Notes
 *   - Code is drawn in JetBrains Mono (0:/programs/mosdesk/fonts/); if the
 *     font cannot be loaded the 8x8 bitmap font is used instead. Every
 *     character takes one fixed cell, tabs are drawn as 4 cells.
 *   - The window is one easyui canvas; the tab bar, sidebar, find bar,
 *     dialogs and scrollbars are all drawn and hit-tested here.
 *   - easyui swallows the Tab key (it moves focus between widgets) and
 *     passes no modifier state with mouse events, so the window events are
 *     looked at first, in code_next_event() below.
 *   - Text is bytes: UTF-8 text is kept as it is but drawn as '?' per byte.
 */
#include "window.h"
#include "minimalos.h"

/* ---------------------------------------------------------------------
 * Window events, seen before easyui sees them:
 *   - the Tab key (easyui uses it to move focus) is handed to the editor;
 *   - the modifier state is kept, so mouse clicks and the wheel know
 *     whether Shift / Ctrl are down;
 *   - mouse moves are handed to the editor for hover and dragging.
 * --------------------------------------------------------------------- */
static int g_mods;                                 /* SYSCALL_INPUT_MOD_* of the last key / mouse event */
static int g_repeat;                               /* the key being handled is an auto-repeat */
static int (*g_tab_hook)(int value);               /* returns 1 if it took the key */
static void (*g_move_hook)(int x, int y, int buttons);
static int g_prev_buttons;
static uint64_t code_next_event(uint32_t id, syscall_window_event_t* ev) {
    uint64_t r = mos_window_next_event(id, ev);
    if (r != SYS_SUCCESS) return r;
    if (ev->type == SYSCALL_WINDOW_EVENT_KEY) {
        g_mods = ev->mods;
        g_repeat = SYSCALL_WINDOW_KEY_IS_REPEAT(ev);
        if (ev->buttons && ev->key == 43 && g_tab_hook && g_tab_hook((int)ev->key | ((int)ev->mods << 8)))
            ev->type = 0;                          /* handled: easyui ignores unknown types */
    } else if (ev->type == SYSCALL_WINDOW_EVENT_MOUSE || ev->type == SYSCALL_WINDOW_EVENT_MOUSE_WHEEL) {
        g_mods = ev->mods;                         /* mosWM sends the modifier state with every mouse event */
        if (ev->type == SYSCALL_WINDOW_EVENT_MOUSE) {
            if (g_move_hook && (int)(ev->buttons & 1u) == (g_prev_buttons & 1))
                g_move_hook(ev->x, ev->y, (int)ev->buttons);
            g_prev_buttons = (int)ev->buttons;
        }
    }
    return r;
}
#define mos_window_next_event code_next_event
#include "easyui.h"
#undef mos_window_next_event
#include "stdlib.h"
#include "string.h"
#include "x86_64/user_usb.h"
#include "syntax.h"
#if defined(__has_include)
#if __has_include("../../settings.h")
#include "../../settings.h"      /* MOSdesk: the user's home folder */
#define CODE_HAS_MOSDESK 1
#endif
#endif

void* memcpy(void* dst, const void* src, unsigned long n);
void* memmove(void* dst, const void* src, unsigned long n);
void* memset(void* dst, int value, unsigned long n);
int   memcmp(const void* a, const void* b, unsigned long n);

#define DOC_MAX     (1 << 20)          /* bytes per document */
#define MAX_LINES   200000
#define ARENA_MAX   (6 << 20)          /* undo data per document */
#define MAX_EDITS   8192
#define MAX_TABS    24
#define HL_CAP      2048               /* bytes of a line that get syntax colours */
#define TABW        4
#define MAX_MATCHES 50000
#define TREE_MAX    4000
#define DIR_MAX     400
#define PATH_MAX_   192

#define TAB_H       34
#define STATUS_H    24
#define ROW_H       22
#define SB_HDR      34
#define SB_W_MIN    140
#define SB_W_MAX    420
#define SCROLL_W    12
#define GUT_PAD     14

/* USB HID usage codes (what easyui's ONKEY delivers) */
enum { K_A = 4, K_B = 5, K_C = 6, K_D = 7, K_F = 9, K_G = 10, K_H = 11, K_K = 14, K_L = 15, K_N = 17,
       K_O = 18, K_P = 19, K_S = 22, K_V = 25, K_W = 26, K_X = 27, K_Y = 28, K_Z = 29, K_1 = 30, K_0 = 39,
       K_ENTER = 40, K_ESC = 41, K_BACKSPACE = 42, K_TAB = 43, K_SPACE = 44, K_MINUS = 45, K_EQUAL = 46,
       K_LBRACKET = 47, K_RBRACKET = 48, K_SLASH = 56, K_F3 = 60, K_F5 = 62, K_HOME = 74, K_PGUP = 75,
       K_DELETE = 76, K_END = 77, K_PGDN = 78, K_RIGHT = 79, K_LEFT = 80, K_DOWN = 81, K_UP = 82,
       K_KPMINUS = 86, K_KPPLUS = 87, K_KPENTER = 88 };
enum { M_SHIFT = 1, M_CTRL = 2, M_ALT = 4 };

static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }
static inline int iclamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* ===========================================================================
 *  Theme
 * =========================================================================== */
static uint32_t C_BG = 0xFF1E1F22, C_SIDE = 0xFF27282C, C_TABBAR = 0xFF191A1D, C_TAB = 0xFF222326,
                C_TAB_HOT = 0xFF2B2C30, C_LINE = 0xFF26282D, C_BORDER = 0xFF323438,
                C_TEXT = 0xFFD4D4D4, C_MUTED = 0xFF858B96, C_GUTTER = 0xFF5D6370, C_GUTTER_HI = 0xFFC0C4CC,
                C_FIELD = 0xFF15161A, C_PANEL = 0xFF2B2D31, C_DANGER = 0xFFF0706A;
static uint32_t g_accent = 0xFF3DAEE9;
static uint32_t PAL[HC_COUNT] = {
    [HC_TEXT] = 0xFFD4D4D4, [HC_KEYWORD] = 0xFF569CD6, [HC_CONTROL] = 0xFFC586C0, [HC_TYPE] = 0xFF4EC9B0,
    [HC_FUNC] = 0xFFDCDCAA, [HC_NUMBER] = 0xFFB5CEA8, [HC_STRING] = 0xFFCE9178, [HC_ESCAPE] = 0xFFD7BA7D,
    [HC_COMMENT] = 0xFF6A9955, [HC_PREPROC] = 0xFFC586C0, [HC_CONST] = 0xFF4FC1FF, [HC_VAR] = 0xFF9CDCFE,
    [HC_HEADING] = 0xFF569CD6, [HC_CODE] = 0xFFCE9178, [HC_LINK] = 0xFF3794FF, [HC_BOLD] = 0xFFDCDCAA,
    [HC_TARGET] = 0xFFDCDCAA, [HC_DECOR] = 0xFFDCDCAA, [HC_PROP] = 0xFF9CDCFE,
};

static uint32_t with_alpha(uint32_t c, uint32_t a) { return (c & 0x00FFFFFFu) | (a << 24); }
static uint32_t mix(uint32_t a, uint32_t b, uint32_t t) {          /* t/255 of b */
    uint32_t r = (((a >> 16) & 255u) * (255u - t) + ((b >> 16) & 255u) * t) / 255u;
    uint32_t g = (((a >> 8) & 255u) * (255u - t) + ((b >> 8) & 255u) * t) / 255u;
    uint32_t bl = ((a & 255u) * (255u - t) + (b & 255u) * t) / 255u;
    return 0xFF000000u | (r << 16) | (g << 8) | bl;
}

/* ===========================================================================
 *  Small string helpers (no libc printf here)
 * =========================================================================== */
static int u2s(int v, char* out) {
    char t[12]; int n = 0, i;
    if (v <= 0) { out[0] = '0'; out[1] = 0; return 1; }
    while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    for (i = 0; i < n; i++) out[i] = t[n - 1 - i];
    out[n] = 0; return n;
}
static void cat(char* dst, int cap, const char* s) {
    int n = (int)strlen(dst);
    if (n < cap - 1) strlcpy_safe(dst + n, s, (size_t)(cap - n));
}
static void cat_num(char* dst, int cap, int v) { char t[12]; u2s(v, t); cat(dst, cap, t); }
static int is_word(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c >= 128; }
static int fold(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

/* ===========================================================================
 *  Documents
 * =========================================================================== */
typedef struct { int pos, remOff, remLen, insOff, insLen, cb, ab, ca, aa, arenaEnd; } Edit;

typedef struct Doc {
    char* text; int len, cap;
    int* ls; int nl, lscap;                       /* line start offsets */
    unsigned char* st; int stcap, stValid;        /* syntax state at the start of each line */
    int caret, anchor, wantCol;
    Edit* ed; int ecount, ecur, ecap;             /* [0,ecur) undo, [ecur,ecount) redo */
    char* arena; int atop, acap;
    int saveMark, merge, modified, ver, maxLine;
    char path[PATH_MAX_]; char name[64];
    int lang, crlf, discard, untitled;
    int scrollY, scrollX, targetY;
} Doc;

static Doc* tabs[MAX_TABS];
static int ntabs, cur;
static Doc* D;                                    /* the document being edited: tabs[cur] */
static int untitledCount;

static char* clipbuf; static int clipLen, clipLine, clipCap;
/* keeps a copy of `n` bytes for Ctrl+V (the buffer grows as needed) */
static int clip_store(const char* data, int n) {
    if (n + 2 > clipCap) {
        int nc = imax(4096, n + 2); char* p = clipbuf ? (char*)realloc(clipbuf, (size_t)nc) : (char*)malloc((size_t)nc);
        if (!p) return 0;
        clipbuf = p; clipCap = nc;
    }
    memcpy(clipbuf, data, (size_t)n); clipLen = n; return 1;
}
static char g_msg[120]; static int g_msg_err; static long g_msg_until;

static void set_msg(const char* s, int err) {
    strlcpy_safe(g_msg, s, sizeof g_msg); g_msg_err = err; g_msg_until = mos_uptime() + 5000;
}
static void set_msg2(const char* a, const char* b, int err) {
    char t[sizeof g_msg]; strlcpy_safe(t, a, sizeof t); cat(t, sizeof t, b); set_msg(t, err);
}

/* ---- line index ---- */
static inline int line_start(const Doc* d, int l) { return d->ls[l]; }
static inline int line_end_(const Doc* d, int l) { return l + 1 < d->nl ? d->ls[l + 1] - 1 : d->len; }
static int line_of(const Doc* d, int off) {
    int lo = 0, hi = d->nl - 1;
    while (lo < hi) { int mid = (lo + hi + 1) / 2; if (d->ls[mid] <= off) lo = mid; else hi = mid - 1; }
    return lo;
}
static int col_of(const Doc* d, int off) { return off - d->ls[line_of(d, off)]; }
static int has_sel(const Doc* d) { return d->caret != d->anchor; }
static int sel_a(const Doc* d) { return imin(d->caret, d->anchor); }
static int sel_b(const Doc* d) { return imax(d->caret, d->anchor); }

static void rebuild_lines(Doc* d) {
    int i, n = 1, longest = 0, last = 0;
    if (d->lscap < 1024) { int* p = (int*)realloc(d->ls, sizeof(int) * 1024); if (!p) return; d->ls = p; d->lscap = 1024; }
    d->ls[0] = 0;
    for (i = 0; i < d->len; i++) {
        if (d->text[i] == '\n') {
            if (n >= d->lscap) {
                int nc = d->lscap * 2; int* p;
                if (nc > MAX_LINES + 2) nc = MAX_LINES + 2;
                if (n >= nc) break;
                p = (int*)realloc(d->ls, sizeof(int) * (size_t)nc);
                if (!p) break;
                d->ls = p; d->lscap = nc;
            }
            if (i - last > longest) longest = i - last;
            d->ls[n++] = i + 1; last = i + 1;
        }
    }
    if (d->len - last > longest) longest = d->len - last;
    d->nl = n; d->maxLine = longest; d->stValid = 0;
}

static int reserve_text(Doc* d, int need) {
    int nc; char* p;
    if (need <= d->cap) return 1;
    nc = d->cap ? d->cap : 4096;
    while (nc < need) nc *= 2;
    if (nc > DOC_MAX + 16) nc = DOC_MAX + 16;
    if (nc < need) return 0;
    p = d->text ? (char*)realloc(d->text, (size_t)nc) : (char*)malloc((size_t)nc);
    if (!p) return 0;
    d->text = p; d->cap = nc; return 1;
}

static Doc* doc_create(void) {
    Doc* d = (Doc*)calloc(1, sizeof(Doc));
    if (!d) return 0;
    if (!reserve_text(d, 4096)) { free(d); return 0; }
    d->ls = (int*)malloc(sizeof(int) * 1024); d->lscap = 1024;
    d->st = (unsigned char*)malloc(1024); d->stcap = 1024;
    if (!d->ls || !d->st) { free(d->text); free(d->ls); free(d->st); free(d); return 0; }
    d->nl = 1; d->ls[0] = 0; d->st[0] = 0;
    return d;
}
static void doc_free(Doc* d) {
    if (!d) return;
    free(d->text); free(d->ls); free(d->st); free(d->ed); free(d->arena); free(d);
}

/* Applies a replacement to the text and the line index. */
static void doc_apply(Doc* d, int pos, int rem, const char* ins, int il) {
    int l0 = line_of(d, pos), r = 0, k = 0, i, tail, delta = il - rem, j;
    for (i = pos; i < pos + rem; i++) if (d->text[i] == '\n') r++;
    for (i = 0; i < il; i++) if (ins[i] == '\n') k++;
    memmove(d->text + pos + il, d->text + pos + rem, (size_t)(d->len - pos - rem));
    memcpy(d->text + pos, ins, (size_t)il);
    d->len += delta;
    d->text[d->len] = 0;
    tail = d->nl - (l0 + 1 + r);
    if (d->nl - r + k + 1 > d->lscap) {
        int nc = d->lscap; int* p;
        while (nc < d->nl - r + k + 2) nc *= 2;
        p = (int*)realloc(d->ls, sizeof(int) * (size_t)nc);
        if (!p) { rebuild_lines(d); d->ver++; return; }        /* cannot happen (edit() checks), but stay safe */
        d->ls = p; d->lscap = nc;
    }
    memmove(d->ls + l0 + 1 + k, d->ls + l0 + 1 + r, sizeof(int) * (size_t)tail);
    for (j = 0, i = 0; i < il; i++) if (ins[i] == '\n') d->ls[l0 + 1 + j++] = pos + i + 1;
    for (i = 0; i < tail; i++) d->ls[l0 + 1 + k + i] += delta;
    d->nl += k - r;
    for (i = l0; i <= l0 + k && i < d->nl; i++) { int ln = line_end_(d, i) - d->ls[i]; if (ln > d->maxLine) d->maxLine = ln; }
    if (d->stValid > l0) d->stValid = l0;
    d->ver++;
}

static int arena_reserve(Doc* d, int need) {
    int nc; char* p;
    if (need > ARENA_MAX) return 0;
    if (need <= d->acap) return 1;
    nc = d->acap ? d->acap : 16384;
    while (nc < need) nc *= 2;
    if (nc > ARENA_MAX) nc = ARENA_MAX;
    p = d->arena ? (char*)realloc(d->arena, (size_t)nc) : (char*)malloc((size_t)nc);
    if (!p) return 0;
    d->arena = p; d->acap = nc; return 1;
}
static int edits_reserve(Doc* d) {
    Edit* p; int nc;
    if (d->ecount < d->ecap) return 1;
    if (d->ecap >= MAX_EDITS) return 0;
    nc = d->ecap ? d->ecap * 2 : 256;
    p = d->ed ? (Edit*)realloc(d->ed, sizeof(Edit) * (size_t)nc) : (Edit*)malloc(sizeof(Edit) * (size_t)nc);
    if (!p) return 0;
    d->ed = p; d->ecap = nc; return 1;
}

enum { MG_NONE = 0, MG_TYPE = 1, MG_BACK = 2, MG_DEL = 3 };

/* Every change goes through here: undo record, text, line index, caret.
 * merge says how consecutive edits of the same kind are grouped. */
static int doc_edit(Doc* d, int pos, int rem, const char* ins, int il, int newCaret, int newAnchor, int merge) {
    if (pos < 0 || rem < 0 || pos + rem > d->len) return 0;
    if (d->len - rem + il > DOC_MAX) { set_msg("Document is full (1 MB limit)", 1); return 0; }
    {   /* the line count is bounded too */
        int r = 0, k = 0, i;
        for (i = pos; i < pos + rem; i++) if (d->text[i] == '\n') r++;
        for (i = 0; i < il; i++) if (ins[i] == '\n') k++;
        if (d->nl - r + k > MAX_LINES) { set_msg("Too many lines", 1); return 0; }
    }
    if (!reserve_text(d, d->len - rem + il + 1)) { set_msg("Out of memory", 1); return 0; }

    /* grouping: consecutive typing / backspace / delete become one undo step */
    if (merge && d->merge == merge && d->ecur > 0 && d->ecur == d->ecount) {
        Edit* e = &d->ed[d->ecur - 1];
        if (merge == MG_TYPE && rem == 0 && il > 0 && e->remLen == 0 && e->pos + e->insLen == pos &&
            e->insLen < 4096 && arena_reserve(d, d->atop + il)) {
            memcpy(d->arena + d->atop, ins, (size_t)il); d->atop += il;
            e->insLen += il; e->ca = newCaret; e->aa = newAnchor; e->arenaEnd = d->atop;
            if (d->saveMark == d->ecur) d->saveMark = -1;
            doc_apply(d, pos, rem, ins, il);
            d->caret = newCaret; d->anchor = newAnchor; d->modified = 1;
            return 1;
        }
        if (merge == MG_BACK && il == 0 && rem > 0 && e->insLen == 0 && pos + rem == e->pos &&
            arena_reserve(d, d->atop + rem)) {
            memmove(d->arena + e->remOff + rem, d->arena + e->remOff, (size_t)e->remLen);
            memcpy(d->arena + e->remOff, d->text + pos, (size_t)rem);
            e->remLen += rem; e->insOff += rem; e->pos = pos; e->ca = newCaret; e->aa = newAnchor;
            d->atop += rem; e->arenaEnd = d->atop;
            if (d->saveMark == d->ecur) d->saveMark = -1;
            doc_apply(d, pos, rem, ins, il);
            d->caret = newCaret; d->anchor = newAnchor; d->modified = 1;
            return 1;
        }
        if (merge == MG_DEL && il == 0 && rem > 0 && e->insLen == 0 && pos == e->pos &&
            arena_reserve(d, d->atop + rem)) {
            memcpy(d->arena + d->atop, d->text + pos, (size_t)rem);
            e->remLen += rem; e->ca = newCaret; e->aa = newAnchor; d->atop += rem; e->arenaEnd = d->atop;
            if (d->saveMark == d->ecur) d->saveMark = -1;
            doc_apply(d, pos, rem, ins, il);
            d->caret = newCaret; d->anchor = newAnchor; d->modified = 1;
            return 1;
        }
    }
    if (d->ecur != d->ecount) {                            /* a new edit drops the redo branch */
        d->ecount = d->ecur;
        d->atop = d->ecur ? d->ed[d->ecur - 1].arenaEnd : 0;
        if (d->saveMark > d->ecur) d->saveMark = -1;
    }
    if (!edits_reserve(d) || !arena_reserve(d, d->atop + rem + il)) {
        /* history full: forget it (the document is then "modified" for good) */
        d->ecount = d->ecur = 0; d->atop = 0; d->saveMark = -1;
        if (!edits_reserve(d) || !arena_reserve(d, rem + il)) {
            doc_apply(d, pos, rem, ins, il);
            d->caret = newCaret; d->anchor = newAnchor; d->modified = 1; d->merge = 0;
            return 1;
        }
    }
    {
        Edit* e = &d->ed[d->ecount];
        e->pos = pos;
        e->remOff = d->atop; e->remLen = rem; memcpy(d->arena + d->atop, d->text + pos, (size_t)rem); d->atop += rem;
        e->insOff = d->atop; e->insLen = il;  memcpy(d->arena + d->atop, ins, (size_t)il);             d->atop += il;
        e->cb = d->caret; e->ab = d->anchor; e->ca = newCaret; e->aa = newAnchor; e->arenaEnd = d->atop;
        d->ecount++; d->ecur = d->ecount;
    }
    doc_apply(d, pos, rem, ins, il);
    d->caret = newCaret; d->anchor = newAnchor; d->modified = 1;
    d->merge = merge;
    return 1;
}

static void doc_undo(Doc* d) {
    Edit* e;
    if (d->ecur == 0) { set_msg("Nothing to undo", 0); return; }
    e = &d->ed[--d->ecur];
    doc_apply(d, e->pos, e->insLen, d->arena + e->remOff, e->remLen);
    d->caret = e->cb; d->anchor = e->ab; d->merge = 0;
    d->modified = d->saveMark != d->ecur;
}
static void doc_redo(Doc* d) {
    Edit* e;
    if (d->ecur == d->ecount) { set_msg("Nothing to redo", 0); return; }
    e = &d->ed[d->ecur++];
    doc_apply(d, e->pos, e->remLen, d->arena + e->insOff, e->insLen);
    d->caret = e->ca; d->anchor = e->aa; d->merge = 0;
    d->modified = d->saveMark != d->ecur;
}

/* ---- syntax state per line ---- */
static unsigned char hl_cols[HL_CAP + 8];
static void st_ensure(Doc* d, int line) {
    if (line + 2 > d->stcap) {
        int nc = d->stcap; unsigned char* p;
        while (nc < line + 2) nc *= 2;
        p = (unsigned char*)realloc(d->st, (size_t)nc);
        if (!p) return;
        d->st = p; d->stcap = nc;
    }
}
/* the state line `line` starts in */
static int line_state(Doc* d, int line) {
    if (d->lang == LANG_TEXT) return 0;
    if (line >= d->nl) line = d->nl - 1;
    st_ensure(d, d->nl);
    if (d->stcap < d->nl + 1) return 0;
    while (d->stValid < line) {
        int l = d->stValid;
        d->st[l + 1] = (unsigned char)scan_line(d->lang, d->text + d->ls[l], line_end_(d, l) - d->ls[l], d->st[l], hl_cols, HL_CAP);
        d->stValid = l + 1;
    }
    return d->st[line];
}

/* ---- text queries ---- */
static int indent_of(const Doc* d, int l) {           /* leading whitespace width in columns */
    int i = d->ls[l], e = line_end_(d, l), w = 0;
    while (i < e && (d->text[i] == ' ' || d->text[i] == '\t')) { w += d->text[i] == '\t' ? TABW - w % TABW : 1; i++; }
    return w;
}
static int first_nonspace(const Doc* d, int l) {      /* offset of the first non-blank byte, or line end */
    int i = d->ls[l], e = line_end_(d, l);
    while (i < e && (d->text[i] == ' ' || d->text[i] == '\t')) i++;
    return i;
}
/* display cell of byte `col` of a line (tabs expand to the next multiple of TABW) */
static int cell_of(const Doc* d, int l, int col) {
    int i = d->ls[l], e = imin(d->ls[l] + col, line_end_(d, l)), c = 0;
    for (; i < e; i++) c += d->text[i] == '\t' ? TABW - c % TABW : 1;
    return c;
}
/* byte column nearest to display edge `cell` (the boundary between cells) */
static int col_at_cell(const Doc* d, int l, int cell) {
    int i = d->ls[l], e = line_end_(d, l), c = 0;
    for (; i < e; i++) {
        int w = d->text[i] == '\t' ? TABW - c % TABW : 1;
        if (cell < c + w) return (cell - c >= (w + 1) / 2 && w > 1) ? i + 1 - d->ls[l] : i - d->ls[l];
        c += w;
    }
    return e - d->ls[l];
}
static int word_start(const Doc* d, int p) { while (p > 0 && is_word((unsigned char)d->text[p - 1])) p--; return p; }
static int word_end(const Doc* d, int p) { while (p < d->len && is_word((unsigned char)d->text[p])) p++; return p; }

/* ---- viewport (needs the layout below) ---- */
static void calc_layout(void);
static void reveal(void);

/* ===========================================================================
 *  Loading, saving, paths
 * =========================================================================== */
static int path_is_abs(const char* p) { return p[0] >= '0' && p[0] <= '9' && p[1] == ':'; }

/* Makes a path absolute (relative to the working directory), without "." and "..". */
static void path_resolve(const char* in, char* out, int cap) {
    char tmp[PATH_MAX_ * 2]; int n = 0, i; char* parts[40]; int np = 0;
    if (path_is_abs(in)) strlcpy_safe(tmp, in, sizeof tmp);
    else if (in[0] == '/') { strlcpy_safe(tmp, "0:", sizeof tmp); cat(tmp, sizeof tmp, in); }
    else {
        char cwd[PATH_MAX_]; cwd[0] = 0;
        if (mos_getcwd(cwd, sizeof cwd) < 0 || !cwd[0]) strlcpy_safe(cwd, "0:/", sizeof cwd);
        if (!path_is_abs(cwd)) { strlcpy_safe(tmp, "0:", sizeof tmp); cat(tmp, sizeof tmp, cwd); }
        else strlcpy_safe(tmp, cwd, sizeof tmp);
        cat(tmp, sizeof tmp, "/"); cat(tmp, sizeof tmp, in);
    }
    /* split after "0:" on '/' */
    out[0] = 0;
    {
        char* p = tmp + 2; char drive[3]; drive[0] = tmp[0]; drive[1] = ':'; drive[2] = 0;
        while (*p) {
            char* s; while (*p == '/') p++;
            if (!*p) break;
            s = p; while (*p && *p != '/') p++;
            if (*p) *p++ = 0;
            if (s[0] == '.' && s[1] == 0) continue;
            if (s[0] == '.' && s[1] == '.' && s[2] == 0) { if (np) np--; continue; }
            if (np < 40) parts[np++] = s;
        }
        strlcpy_safe(out, drive, (size_t)cap);
        if (!np) cat(out, cap, "/");
        for (i = 0; i < np; i++) { cat(out, cap, "/"); cat(out, cap, parts[i]); }
    }
    (void)n;
}
static const char* base_name(const char* p) {
    const char* b = p; for (; *p; p++) if (*p == '/' || *p == ':') b = p + 1;
    return b;
}
static void dir_of(const char* p, char* out, int cap) {
    int n = (int)strlen(p), i = n;
    while (i > 0 && p[i - 1] != '/') i--;
    if (i <= 3) { strlcpy_safe(out, "0:/", (size_t)cap); if (p[0] != '0') out[0] = p[0]; return; }
    if (i - 1 >= cap) i = cap;
    memcpy(out, p, (size_t)(i - 1)); out[i - 1] = 0;
}
static void doc_set_path(Doc* d, const char* path) {
    strlcpy_safe(d->path, path, sizeof d->path);
    strlcpy_safe(d->name, base_name(path), sizeof d->name);
    d->lang = lang_for_name(d->path);
    d->stValid = 0; d->untitled = 0;
}

/* Replaces the document's text with a file's bytes. */
static int doc_load(Doc* d, const char* path) {          /* 1 ok, 0 failed, -1 too big */
    long fd = mos_open(path, SYS_O_RDONLY), size, got = 0, n; int i, crlf = 0, lf = 0, o;
    if (fd <= 0) return 0;
    size = mos_size(fd);
    if (size < 0) { mos_close(fd); return 0; }
    if (size > DOC_MAX) { mos_close(fd); return -1; }
    if (!reserve_text(d, (int)size + 1)) { mos_close(fd); return 0; }
    while (got < size && (n = mos_read(fd, d->text + got, (size_t)(size - got))) > 0) got += n;
    mos_close(fd);
    d->len = (int)got; d->text[d->len] = 0;
    for (i = 0; i < d->len; i++) if (d->text[i] == '\n') { if (i > 0 && d->text[i - 1] == '\r') crlf++; else lf++; }
    d->crlf = crlf > 0 && crlf >= lf;
    if (d->crlf) {                                        /* edit as LF, write back as CRLF */
        for (o = 0, i = 0; i < d->len; i++) {
            if (d->text[i] == '\r' && i + 1 < d->len && d->text[i + 1] == '\n') continue;
            d->text[o++] = d->text[i];
        }
        d->len = o; d->text[o] = 0;
    }
    rebuild_lines(d);
    d->caret = d->anchor = d->wantCol = 0;
    d->ecount = d->ecur = d->atop = 0; d->saveMark = 0; d->modified = 0; d->merge = 0;
    d->scrollX = d->scrollY = d->targetY = 0; d->ver++;
    doc_set_path(d, path);
    return 1;
}

static int doc_write(Doc* d, const char* path) {
    long fd, n; int total = 0, i;
    if (mos_exists(path) == 1) { if (mos_is_dir(path) == 1 || mos_delete(path) < 0) return 0; }   /* writes never shorten a file */
    if (mos_create(path, "text", "txt") < 0) return 0;
    fd = mos_open(path, SYS_O_RDWR);
    if (fd <= 0) return 0;
    if (!d->crlf) {
        while (total < d->len && (n = mos_fwrite(fd, d->text + total, (size_t)(d->len - total))) > 0) total += (int)n;
        mos_close(fd);
        return total == d->len;
    }
    {
        char buf[4096]; int o = 0, ok = 1;
        for (i = 0; i < d->len && ok; i++) {
            if (d->text[i] == '\n') buf[o++] = '\r';
            buf[o++] = d->text[i];
            if (o >= (int)sizeof buf - 2 || i == d->len - 1) {
                int w = 0;
                while (w < o && (n = mos_fwrite(fd, buf + w, (size_t)(o - w))) > 0) w += (int)n;
                if (w != o) ok = 0;
                o = 0;
            }
        }
        mos_close(fd);
        return ok;
    }
}

/* ===========================================================================
 *  Tabs
 * =========================================================================== */
static void tree_dirty(void);
static void update_title(void);

static Doc* tab_new_doc(const char* path) {
    Doc* d;
    if (ntabs >= MAX_TABS) { set_msg("Too many open tabs", 1); return 0; }
    d = doc_create();
    if (!d) { set_msg("Out of memory", 1); return 0; }
    if (path) doc_set_path(d, path);
    else {
        strlcpy_safe(d->name, "Untitled-", sizeof d->name); cat_num(d->name, sizeof d->name, ++untitledCount);
        d->untitled = 1; d->lang = LANG_TEXT;
    }
    tabs[ntabs++] = d;
    return d;
}
static void tab_activate(int i) {
    if (i < 0 || i >= ntabs) return;
    cur = i; D = tabs[i];
    tree_dirty(); update_title();
}
static int tab_find(const char* path) {
    int i; for (i = 0; i < ntabs; i++) if (!tabs[i]->untitled && strcmp(tabs[i]->path, path) == 0) return i;
    return -1;
}
static int tab_index(const Doc* d) { int i; for (i = 0; i < ntabs; i++) if (tabs[i] == d) return i; return -1; }

/* Opens a file in a tab (or switches to it). Returns 1 on success. */
static int open_file(const char* in) {
    char path[PATH_MAX_]; int i, r, reuse = 0; Doc* d;
    path_resolve(in, path, sizeof path);
    if ((i = tab_find(path)) >= 0) { tab_activate(i); return 1; }
    if (mos_is_dir(path) == 1) { set_msg("That is a folder", 1); return 0; }
    /* a pristine untitled tab is reused */
    if (ntabs == 1 && tabs[0]->untitled && !tabs[0]->modified && tabs[0]->len == 0) { d = tabs[0]; reuse = 1; }
    else d = tab_new_doc(path);
    if (!d) return 0;
    if (mos_exists(path) == 1) {
        r = doc_load(d, path);
        if (r != 1) {
            if (!reuse) { ntabs--; doc_free(d); }
            set_msg2(r < 0 ? "File too large (1 MB max): " : "Could not open ", base_name(path), 1);
            return 0;
        }
        set_msg2("Opened ", d->name, 0);
    } else {                                                 /* a new file that does not exist yet */
        doc_set_path(d, path);
        set_msg2("New file ", d->name, 0);
    }
    tab_activate(reuse ? 0 : ntabs - 1);
    return 1;
}
static void tab_remove(int i) {
    Doc* d = tabs[i]; int k;
    for (k = i; k + 1 < ntabs; k++) tabs[k] = tabs[k + 1];
    ntabs--;
    doc_free(d);
    if (ntabs == 0) { tab_new_doc(0); cur = 0; }
    else if (i < cur) cur--;
    else if (i == cur && cur >= ntabs) cur = ntabs - 1;
    D = tabs[cur];
    tree_dirty(); update_title();
}

/* ===========================================================================
 *  Sidebar tree
 * =========================================================================== */
typedef struct { char name[64]; unsigned char isdir, depth, open; } TNode;
static TNode* tn; static int tcount;
static char treeRoot[PATH_MAX_] = "0:/";
static int treeScroll, treeActive = -2, sbOn = 1, sbW = 230;
static syscall_dirent_t* dirbuf;

static void tree_dirty(void) { treeActive = -2; }

static void tree_path(int idx, char* out, int cap) {
    int stack[20], sp = 0, dep = tn[idx].depth, j, s;
    stack[sp++] = idx;
    for (j = idx - 1; j >= 0 && dep > 0 && sp < 20; j--) if (tn[j].depth == dep - 1) { stack[sp++] = j; dep--; }
    strlcpy_safe(out, treeRoot, (size_t)cap);
    for (s = sp - 1; s >= 0; s--) {
        if (out[strlen(out) - 1] != '/') cat(out, cap, "/");
        cat(out, cap, tn[stack[s]].name);
    }
}
static int dirent_less(const syscall_dirent_t* a, const syscall_dirent_t* b) {
    int ad = a->type == SYSCALL_DIRENT_TYPE_DIR, bd = b->type == SYSCALL_DIRENT_TYPE_DIR, i;
    if (ad != bd) return ad > bd;
    for (i = 0; i < SYSCALL_DIRENT_NAME_SIZE; i++) {
        int x = fold((unsigned char)a->name[i]), y = fold((unsigned char)b->name[i]);
        if (x != y) return x < y;
        if (!x) return 0;
    }
    return 0;
}
/* Lists a folder and returns the sorted entries in dirbuf (count), 0 on failure. */
static int list_dir(const char* path) {
    long n; int cnt = 0, i;
    if (!dirbuf) dirbuf = (syscall_dirent_t*)malloc(sizeof(syscall_dirent_t) * DIR_MAX);
    if (!dirbuf) return 0;
    n = mos_listdir(path, dirbuf, DIR_MAX);
    if (n < 0) return 0;
    if (n > DIR_MAX) n = DIR_MAX;
    for (i = 0; i < n; i++) {                                 /* drop hidden entries, insertion-sort the rest */
        syscall_dirent_t e = dirbuf[i]; int at;
        if (e.hidden || e.name[0] == 0) continue;
        e.name[SYSCALL_DIRENT_NAME_SIZE - 1] = 0;
        at = cnt++;
        while (at > 0 && dirent_less(&e, &dirbuf[at - 1])) { dirbuf[at] = dirbuf[at - 1]; at--; }
        dirbuf[at] = e;
    }
    return cnt;
}
/* Inserts the entries of folder `idx` after it. Returns how many were added. */
static int tree_expand(int idx) {
    char path[PATH_MAX_]; int n, i, depth;
    if (idx >= 0) tree_path(idx, path, sizeof path); else strlcpy_safe(path, treeRoot, sizeof path);
    depth = idx >= 0 ? tn[idx].depth + 1 : 0;
    if (depth > 12) return 0;
    n = list_dir(path);
    if (n > TREE_MAX - tcount - 1) n = TREE_MAX - tcount - 1;
    if (n <= 0) return 0;
    memmove(&tn[idx + 1 + n], &tn[idx + 1], sizeof(TNode) * (size_t)(tcount - idx - 1));
    for (i = 0; i < n; i++) {
        TNode* t = &tn[idx + 1 + i];
        strlcpy_safe(t->name, dirbuf[i].name, sizeof t->name);
        t->isdir = dirbuf[i].type == SYSCALL_DIRENT_TYPE_DIR; t->depth = (unsigned char)depth; t->open = 0;
    }
    tcount += n;
    return n;
}
static void tree_collapse(int idx) {
    int j = idx + 1;
    while (j < tcount && tn[j].depth > tn[idx].depth) j++;
    memmove(&tn[idx + 1], &tn[j], sizeof(TNode) * (size_t)(tcount - j));
    tcount -= j - idx - 1;
    tn[idx].open = 0;
}
static void tree_toggle(int idx) {
    if (!tn[idx].isdir) return;
    if (tn[idx].open) tree_collapse(idx);
    else { tn[idx].open = 1; tree_expand(idx); }
}

/* Rebuilds the tree from disk, keeping the folders that were open. */
static void tree_refresh(void) {
    static char openPaths[48][PATH_MAX_];
    int nopen = 0, i;
    if (!tn) return;
    for (i = 0; i < tcount && nopen < 48; i++) if (tn[i].isdir && tn[i].open) tree_path(i, openPaths[nopen++], PATH_MAX_);
    tcount = 0;
    tree_expand(-1);
    for (i = 0; i < tcount; i++) {
        if (tn[i].isdir) {
            char p[PATH_MAX_]; int k;
            tree_path(i, p, sizeof p);
            for (k = 0; k < nopen; k++) if (strcmp(openPaths[k], p) == 0) { tn[i].open = 1; tree_expand(i); break; }
        }
    }
    tree_dirty();
}
static void tree_set_root(const char* path) {
    strlcpy_safe(treeRoot, path, sizeof treeRoot);
    tcount = 0; treeScroll = 0;
    tree_expand(-1);
    tree_dirty();
}
static int tree_find_active(void) {
    int i;
    if (D->untitled || !D->path[0]) return -1;
    for (i = 0; i < tcount; i++) if (!tn[i].isdir) {
        char p[PATH_MAX_]; tree_path(i, p, sizeof p);
        if (strcmp(p, D->path) == 0) return i;
    }
    return -1;
}

/* ===========================================================================
 *  Editing commands
 * =========================================================================== */
static int wantCell;                 /* column (in cells) vertical movement aims for */
static int caretOn = 1;
static void set_want(void) { wantCell = cell_of(D, line_of(D, D->caret), col_of(D, D->caret)); }

static void move_to(int to, int keepWant, int extend) {
    D->caret = iclamp(to, 0, D->len);
    if (!extend) D->anchor = D->caret;
    D->merge = 0;
    if (!keepWant) set_want();
    reveal();
}
static void move_vert(int delta, int extend) {
    int l = line_of(D, D->caret), t = iclamp(l + delta, 0, D->nl - 1);
    if (t == l && delta < 0) { move_to(0, 0, extend); return; }          /* Up on the first line goes home */
    if (t == l && delta > 0) { move_to(D->len, 0, extend); return; }
    move_to(D->ls[t] + col_at_cell(D, t, wantCell), 1, extend);
}
static int char_class(int c) { return (c == ' ' || c == '\t' || c == '\n') ? 0 : is_word(c) ? 1 : 2; }
static int word_left(int p) {
    int cl;
    if (p <= 0) return 0;
    p--;
    while (p > 0 && D->text[p] == ' ') p--;
    if (D->text[p] == '\n' || D->text[p] == '\t') return p;
    cl = char_class((unsigned char)D->text[p]);
    while (p > 0 && char_class((unsigned char)D->text[p - 1]) == cl) p--;
    return p;
}
static int word_right(int p) {
    int cl;
    if (p >= D->len) return D->len;
    if (D->text[p] == '\n') return p + 1;
    while (p < D->len && (D->text[p] == ' ' || D->text[p] == '\t')) p++;
    if (p >= D->len || D->text[p] == '\n') return p;
    cl = char_class((unsigned char)D->text[p]);
    while (p < D->len && char_class((unsigned char)D->text[p]) == cl) p++;
    return p;
}

static void after_edit(void) { set_want(); reveal(); caretOn = 1; }

/* Replaces the selection (or inserts at the caret). */
static void insert_text(const char* s, int n, int merge) {
    int a = sel_a(D), b = sel_b(D);
    if (doc_edit(D, a, b - a, s, n, a + n, a + n, a == b ? merge : MG_NONE)) after_edit();
}

static void do_backspace(void) {
    Doc* d = D;
    if (has_sel(d)) { insert_text("", 0, MG_NONE); return; }
    if (d->caret == 0) return;
    {
        int p = d->caret, l = line_of(d, p), ls0 = d->ls[l], k = 1, sp = 1, i;
        char prev = d->text[p - 1], next = p < d->len ? d->text[p] : 0;
        for (i = ls0; i < p; i++) if (d->text[i] != ' ') sp = 0;
        if (sp && p - ls0 > 0) { k = (p - ls0) % TABW; if (!k) k = TABW; }                 /* back to the previous tab stop */
        else if ((prev == '(' && next == ')') || (prev == '[' && next == ']') || (prev == '{' && next == '}') ||
                 (prev == '"' && next == '"') || (prev == '\'' && next == '\'') || (prev == '`' && next == '`')) {
            if (doc_edit(d, p - 1, 2, "", 0, p - 1, p - 1, MG_NONE)) after_edit();
            return;
        }
        if (doc_edit(d, p - k, k, "", 0, p - k, p - k, MG_BACK)) after_edit();
    }
}
static void do_delete(void) {
    Doc* d = D;
    if (has_sel(d)) { insert_text("", 0, MG_NONE); return; }
    if (d->caret >= d->len) return;
    if (doc_edit(d, d->caret, 1, "", 0, d->caret, d->caret, MG_DEL)) after_edit();
}
static void delete_word_left(void) {
    int a;
    if (has_sel(D)) { insert_text("", 0, MG_NONE); return; }
    a = word_left(D->caret);
    if (a < D->caret && doc_edit(D, a, D->caret - a, "", 0, a, a, MG_NONE)) after_edit();
}
static void delete_word_right(void) {
    int b;
    if (has_sel(D)) { insert_text("", 0, MG_NONE); return; }
    b = word_right(D->caret);
    if (b > D->caret && doc_edit(D, D->caret, b - D->caret, "", 0, D->caret, D->caret, MG_NONE)) after_edit();
}

static const char* indent_unit(void) { return D->lang == LANG_MAKE ? "\t" : "    "; }

static void do_enter(int below, int above) {
    Doc* d = D;
    char* buf; int a, b, l, i, il = 0, bl, extra = 0, before = 0, after = 0, n, usetab = 0;
    char ind[PATH_MAX_];
    if (below || above) {                                /* Ctrl+Enter: a new line below / above, wherever the caret is */
        l = line_of(d, d->caret);
        a = b = below ? line_end_(d, l) : d->ls[l];
    } else { a = sel_a(d); b = sel_b(d); l = line_of(d, a); }
    {
        int k = d->ls[l], e = (below || above) ? line_end_(d, l) : a;      /* the indentation of the line we are on */
        for (; k < e && il < (int)sizeof ind - 2 && (d->text[k] == ' ' || d->text[k] == '\t'); k++) { ind[il++] = d->text[k]; if (d->text[k] == '\t') usetab = 1; }
    }
    ind[il] = 0;
    for (i = a - 1; i >= d->ls[l]; i--) if (d->text[i] != ' ' && d->text[i] != '\t') { before = (unsigned char)d->text[i]; break; }
    after = b < d->len ? (unsigned char)d->text[b] : 0;
    if (!above && (before == '{' || before == '(' || before == '[' || (d->lang == LANG_PY && before == ':'))) extra = 1;
    if (d->lang == LANG_MAKE) usetab = 1;
    bl = (int)sizeof(ind) * 2 + 16;
    buf = (char*)malloc((size_t)bl);
    if (!buf) return;
    n = 0;
    if (above) {
        memcpy(buf, ind, (size_t)il); n = il; buf[n++] = '\n';
        if (doc_edit(d, a, 0, buf, n, a + il, a + il, MG_NONE)) after_edit();
        free(buf); return;
    }
    buf[n++] = '\n'; memcpy(buf + n, ind, (size_t)il); n += il;
    if (extra) { if (usetab) buf[n++] = '\t'; else { memcpy(buf + n, "    ", 4); n += 4; } }
    {
        int caretAt = a + n;
        if (extra && ((before == '{' && after == '}') || (before == '(' && after == ')') || (before == '[' && after == ']')) && !below) {
            buf[n++] = '\n'; memcpy(buf + n, ind, (size_t)il); n += il;     /* split "{|}" into three lines */
        }
        if (doc_edit(d, a, b - a, buf, n, caretAt, caretAt, MG_NONE)) after_edit();
    }
    free(buf);
}

/* ---- block edits: indent, unindent, comment (one undo step) ---- */
enum { BLK_INDENT, BLK_UNINDENT, BLK_COMMENT };
static void block_lines(int* l0, int* l1) {
    *l0 = line_of(D, sel_a(D)); *l1 = line_of(D, sel_b(D));
    if (*l1 > *l0 && sel_b(D) == D->ls[*l1]) (*l1)--;      /* a selection that ends at a line start leaves that line out */
}
static int map_col(int col, int editCol, int rem, int ins) {
    if (col <= editCol) return col;
    if (col <= editCol + rem) return editCol + ins;
    return col + ins - rem;
}
static void block_edit(int mode) {
    Doc* d = D;
    int l0, l1, l, start, end, n = 0, cap, k;
    char* out; int newCaret = -1, newAnchor = -1, allCommented = 1, comment, minInd = 1 << 30;
    const char* cm = lang_comment(d->lang); int cml = cm ? (int)strlen(cm) : 0;
    int ca = d->caret, an = d->anchor, unitLen = d->lang == LANG_MAKE ? 1 : TABW;
    block_lines(&l0, &l1);
    if (mode == BLK_COMMENT) {
        if (!cml) { set_msg("No line comment syntax for this language", 0); return; }
        for (l = l0; l <= l1; l++) {                                      /* all commented already? */
            int f = first_nonspace(d, l), e = line_end_(d, l), c;
            if (f >= e) continue;
            if (!(e - f >= cml && memcmp(d->text + f, cm, (size_t)cml) == 0)) allCommented = 0;
            c = f - d->ls[l]; if (c < minInd) minInd = c;
        }
        if (minInd == (1 << 30)) minInd = 0;
    }
    comment = !allCommented;                                              /* all commented: take the markers off */
    start = d->ls[l0]; end = line_end_(d, l1);
    cap = (end - start) + (l1 - l0 + 1) * (unitLen + cml + 2) + 16;
    out = (char*)malloc((size_t)cap);
    if (!out) return;
    for (l = l0; l <= l1; l++) {
        int ls0 = d->ls[l], le = line_end_(d, l), editCol = 0, rem = 0, insl = 0; const char* ins = "";
        if (mode == BLK_INDENT) {
            if (le > ls0 || l0 == l1) { ins = indent_unit(); insl = unitLen; }
        } else if (mode == BLK_UNINDENT) {
            if (le > ls0 && d->text[ls0] == '\t') rem = 1;
            else { while (rem < TABW && ls0 + rem < le && d->text[ls0 + rem] == ' ') rem++; }
        } else {
            int f = first_nonspace(d, l);
            if (f < le) {
                if (comment) { ins = cm; insl = cml; editCol = minInd; }
                else {
                    editCol = f - ls0; rem = cml;
                    if (f + cml < le && d->text[f + cml] == ' ') rem++;
                }
            }
        }
        memcpy(out + n, d->text + ls0, (size_t)editCol);
        k = n + editCol;
        memcpy(out + k, ins, (size_t)insl); k += insl;
        if (mode == BLK_COMMENT && comment && insl) { out[k++] = ' '; insl++; }
        memcpy(out + k, d->text + ls0 + editCol + rem, (size_t)(le - ls0 - editCol - rem)); k += le - ls0 - editCol - rem;
        /* carry the selection ends over to their place in the new text */
        if (ca >= ls0 && ca <= le) newCaret = start + n + map_col(ca - ls0, editCol, rem, insl);
        if (an >= ls0 && an <= le) newAnchor = start + n + map_col(an - ls0, editCol, rem, insl);
        n = k;
        if (l < l1) out[n++] = '\n';
    }
    if (newCaret < 0) newCaret = start + n;
    if (newAnchor < 0) newAnchor = start + n;
    if (n == end - start && memcmp(out, d->text + start, (size_t)n) == 0) { free(out); return; }
    if (doc_edit(d, start, end - start, out, n, newCaret, newAnchor, MG_NONE)) after_edit();
    free(out);
}

/* Tab: insert spaces up to the next tab stop, or indent a multi-line selection. */
static void do_tab(int shift) {
    Doc* d = D;
    if (shift) { block_edit(BLK_UNINDENT); return; }
    if (has_sel(d) && line_of(d, sel_a(d)) != line_of(d, sel_b(d))) { block_edit(BLK_INDENT); return; }
    if (d->lang == LANG_MAKE) { insert_text("\t", 1, MG_NONE); return; }
    {
        int cell = cell_of(d, line_of(d, sel_a(d)), col_of(d, sel_a(d))), k = TABW - cell % TABW;
        insert_text("    ", k, MG_NONE);
    }
}

static void move_lines(int dir) {
    Doc* d = D; int l0, l1, bs, be, ps, pe, n, blk, other;
    char* out;
    block_lines(&l0, &l1);
    if (dir < 0 && l0 == 0) return;
    if (dir > 0 && l1 >= d->nl - 1) return;
    bs = d->ls[l0]; be = line_end_(d, l1); blk = be - bs;
    if (dir < 0) { ps = d->ls[l0 - 1]; pe = line_end_(d, l0 - 1); }
    else { ps = d->ls[l1 + 1]; pe = line_end_(d, l1 + 1); }
    other = pe - ps;
    out = (char*)malloc((size_t)(blk + other + 4));
    if (!out) return;
    n = 0;
    if (dir < 0) {
        memcpy(out, d->text + bs, (size_t)blk); n = blk; out[n++] = '\n';
        memcpy(out + n, d->text + ps, (size_t)other); n += other;
        if (doc_edit(d, ps, be - ps, out, n, d->caret - other - 1, d->anchor - other - 1, MG_NONE)) after_edit();
    } else {
        memcpy(out, d->text + ps, (size_t)other); n = other; out[n++] = '\n';
        memcpy(out + n, d->text + bs, (size_t)blk); n += blk;
        if (doc_edit(d, bs, pe - bs, out, n, d->caret + other + 1, d->anchor + other + 1, MG_NONE)) after_edit();
    }
    free(out);
}
static void duplicate_lines(void) {
    Doc* d = D; int l0, l1, bs, be, blk;
    char* out;
    block_lines(&l0, &l1);
    bs = d->ls[l0]; be = line_end_(d, l1); blk = be - bs;
    out = (char*)malloc((size_t)blk + 2);
    if (!out) return;
    out[0] = '\n'; memcpy(out + 1, d->text + bs, (size_t)blk);
    if (doc_edit(d, be, 0, out, blk + 1, d->caret + blk + 1, d->anchor + blk + 1, MG_NONE)) after_edit();
    free(out);
}
static void delete_lines(void) {
    Doc* d = D; int l0, l1, bs, be;
    block_lines(&l0, &l1);
    bs = d->ls[l0]; be = line_end_(d, l1);
    if (be < d->len) be++;                               /* the newline that ends the last line */
    else if (bs > 0) bs--;                               /* the last line: take the newline before it */
    if (doc_edit(d, bs, be - bs, "", 0, bs, bs, MG_NONE)) {
        int l = line_of(D, D->caret);
        D->caret = D->anchor = D->ls[l] + col_at_cell(D, l, wantCell);
        after_edit();
    }
}
static void select_line(void) {
    int l = line_of(D, D->caret);
    D->anchor = D->ls[l]; D->caret = l + 1 < D->nl ? D->ls[l + 1] : D->len;
    D->merge = 0; set_want(); reveal();
}

/* ---- clipboard (in-app only: the SDK has no system clipboard) ---- */
static void do_copy(void) {
    Doc* d = D; int a, b;
    if (has_sel(d)) { a = sel_a(d); b = sel_b(d); clipLine = 0; }
    else { int l = line_of(d, d->caret); a = d->ls[l]; b = line_end_(d, l); clipLine = 1; }
    if (!clip_store(d->text + a, b - a)) { set_msg("Out of memory", 1); return; }
    if (clipLine) clipbuf[clipLen++] = '\n';                 /* clip_store left room for it */
    set_msg("Copied", 0);
}
static void do_cut(void) {
    Doc* d = D;
    do_copy();
    if (has_sel(d)) insert_text("", 0, MG_NONE);
    else delete_lines();
}
static void do_paste(void) {
    Doc* d = D;
    if (!clipLen) { set_msg("Clipboard is empty", 0); return; }
    if (clipLine && !has_sel(d)) {                        /* a whole line goes above the current one */
        int l = line_of(d, d->caret), p = d->ls[l], off = d->caret - p;
        if (doc_edit(d, p, 0, clipbuf, clipLen, p + clipLen + off, p + clipLen + off, MG_NONE)) after_edit();
    } else insert_text(clipbuf, clipLen, MG_NONE);
}

/* ---- typing with auto-close ---- */
static int closer_of(int c) { return c == '(' ? ')' : c == '[' ? ']' : c == '{' ? '}' : c; }
static void type_char(int c) {
    Doc* d = D; char ch = (char)c;
    int p = d->caret, next = p < d->len ? (unsigned char)d->text[p] : 0, prev = p > 0 ? (unsigned char)d->text[p - 1] : 0;
    int quote = c == '"' || c == '\'' || c == '`';
    int opener = c == '(' || c == '[' || c == '{';
    int closer = c == ')' || c == ']' || c == '}';
    int prose = d->lang == LANG_TEXT || d->lang == LANG_MD;
    if (!has_sel(d) && (closer || quote) && next == c) { move_to(p + 1, 0, 0); return; }          /* type over the closer */
    if (has_sel(d) && (opener || (quote && !prose))) {                                           /* wrap the selection */
        int a = sel_a(d), b = sel_b(d), n = b - a; char* t = (char*)malloc((size_t)n + 3);
        if (!t) return;
        t[0] = ch; memcpy(t + 1, d->text + a, (size_t)n); t[n + 1] = (char)closer_of(c);
        if (doc_edit(d, a, n, t, n + 2, a + 1 + n, a + 1, MG_NONE)) after_edit();
        free(t); return;
    }
    if (!has_sel(d) && (opener || (quote && !prose))) {
        int ok = next == 0 || next == ' ' || next == '\t' || next == '\n' || next == ')' || next == ']' || next == '}' || next == ',' || next == ';';
        if (quote && (is_word(prev) || prev == c || prev == '\\')) ok = 0;                       /* an apostrophe or a word */
        if (ok) {
            char t[2]; t[0] = ch; t[1] = (char)closer_of(c);
            if (doc_edit(d, p, 0, t, 2, p + 1, p + 1, MG_NONE)) after_edit();
            return;
        }
    }
    if (c == '}' && !has_sel(d)) {                                                                /* electric: dedent a lone } */
        int l = line_of(d, p), i, only = 1;
        for (i = d->ls[l]; i < p; i++) if (d->text[i] != ' ') only = 0;
        if (only && p - d->ls[l] >= TABW) {
            int k = (p - d->ls[l]) % TABW; if (!k) k = TABW;
            if (doc_edit(d, p - k, k, "}", 1, p - k + 1, p - k + 1, MG_NONE)) after_edit();
            return;
        }
    }
    insert_text(&ch, 1, MG_TYPE);
}

/* ---- font metrics ---- */
static const int zoomPx[3] = { 10, 15, 20 };
static int zoomIdx = 1, fontId, FPX = 15, CW = 9, LH = 22, fontIsMono;
static void apply_font(void) {
    int w;
    FPX = zoomPx[zoomIdx];
    w = easyui.textwidthfont("MMMMMMMMMM", fontId, FPX);
    CW = w / 10;
    if (CW < 4) CW = FPX * 6 / 10 + 1;
    LH = imax(14, FPX + FPX / 2 - (FPX >= 20 ? 1 : 0));
    easyui.redraw();
}

/* ===========================================================================
 *  UI state: text fields, find, layout
 * =========================================================================== */
enum { F_EDITOR = 0, F_FIND, F_REPL, F_GOTO, F_PAL };
typedef struct { char buf[160]; int len, caret, all; } Field;
static Field fFind, fRepl, fGoto;
static int focus = F_EDITOR, findOpen, replOpen, gotoOpen, matchCase;
static int canvas;

static void field_set(Field* f, const char* s) {
    strlcpy_safe(f->buf, s, sizeof f->buf);
    f->len = (int)strlen(f->buf); f->caret = f->len; f->all = 0;
}
static void field_insert(Field* f, const char* s, int n) {
    int i;
    if (f->all) { f->len = 0; f->caret = 0; f->all = 0; }
    for (i = 0; i < n && f->len < (int)sizeof f->buf - 2; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 32 || c > 126) continue;
        memmove(f->buf + f->caret + 1, f->buf + f->caret, (size_t)(f->len - f->caret + 1));
        f->buf[f->caret++] = (char)c; f->len++;
    }
}

/* key -> character, with the layout the kernel knows; a plain US table is the fallback */
#define M_ALTGR 8   /* SYSCALL_INPUT_MOD_ALTGR: right Alt */
static int key_char(int key, int mods) {
    int shift = (mods & 1) != 0;
    static const char lower[] = "abcdefghijklmnopqrstuvwxyz1234567890";
    static const char upper[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ!@#$%^&*()";
    static const char sym_lo[] = "-=[]\\#;'`,./";
    static const char sym_up[] = "_+{}|~:\"~<>?";
    int c;
    if ((key >= 4 && key <= 39) || (key >= 45 && key <= 56) || key == 100) {
        c = (unsigned char)mos_usb_key_to_ascii((uint8_t)key,
                (uint8_t)((shift ? 0x02 : 0) | ((mods & M_ALTGR) ? 0x40 : 0)));
        if (c >= 32 && c < 127) return c;
        if (mods & M_ALTGR) return 0;    /* nothing on this key with AltGr */
    }
    if (key >= 4 && key <= 39) return shift ? upper[key - 4] : lower[key - 4];
    if (key == 44) return ' ';
    if (key >= 45 && key <= 56) return shift ? sym_up[key - 45] : sym_lo[key - 45];
    if (key >= 89 && key <= 97) return '1' + (key - 89);
    if (key == 98) return '0';
    if (key == 99) return '.';
    if (key == 84) return '/';
    if (key == 85) return '*';
    if (key == 86) return '-';
    if (key == 87) return '+';
    return 0;
}

/* returns 1 when the text changed */
static int field_key(Field* f, int key, int mods) {
    int shift = mods & M_SHIFT, ctrl = mods & M_CTRL, c;
    (void)shift;
    if (ctrl) {
        if (key == K_A) { f->all = f->len > 0; f->caret = f->len; return 0; }
        if (key == K_V && clipLen) {
            char t[160]; int n = 0;
            while (n < clipLen && n < (int)sizeof t - 1 && clipbuf[n] != '\n' && clipbuf[n] != '\r') { t[n] = clipbuf[n]; n++; }
            t[n] = 0; field_insert(f, t, n); return 1;
        }
        if (key == K_C || key == K_X) {
            if (f->len && clip_store(f->buf, f->len)) clipLine = 0;
            if (key == K_X) { f->len = 0; f->caret = 0; f->buf[0] = 0; f->all = 0; return 1; }
            return 0;
        }
        if (key == K_BACKSPACE && f->caret > 0) {
            int a = f->caret; while (a > 0 && f->buf[a - 1] == ' ') a--; while (a > 0 && f->buf[a - 1] != ' ') a--;
            memmove(f->buf + a, f->buf + f->caret, (size_t)(f->len - f->caret + 1)); f->len -= f->caret - a; f->caret = a; return 1;
        }
        return 0;
    }
    if (key == K_BACKSPACE) {
        if (f->all) { f->len = 0; f->caret = 0; f->buf[0] = 0; f->all = 0; return 1; }
        if (f->caret > 0) { memmove(f->buf + f->caret - 1, f->buf + f->caret, (size_t)(f->len - f->caret + 1)); f->caret--; f->len--; return 1; }
        return 0;
    }
    if (key == K_DELETE) {
        if (f->all) { f->len = 0; f->caret = 0; f->buf[0] = 0; f->all = 0; return 1; }
        if (f->caret < f->len) { memmove(f->buf + f->caret, f->buf + f->caret + 1, (size_t)(f->len - f->caret)); f->len--; return 1; }
        return 0;
    }
    if (key == K_LEFT) { f->all = 0; if (f->caret > 0) f->caret--; return 0; }
    if (key == K_RIGHT) { f->all = 0; if (f->caret < f->len) f->caret++; return 0; }
    if (key == K_HOME) { f->all = 0; f->caret = 0; return 0; }
    if (key == K_END) { f->all = 0; f->caret = f->len; return 0; }
    c = key_char(key, mods);
    if (c) { char ch = (char)c; field_insert(f, &ch, 1); return 1; }
    return 0;
}

/* ---- find ---- */
static int* matches; static int nmatches, mver = -1, mcase, mlen; static Doc* mdoc; static char mneedle[160];
static int eq_at(const char* a, const char* b, int n, int cs) {
    int i;
    for (i = 0; i < n; i++) { int x = (unsigned char)a[i], y = (unsigned char)b[i]; if (!cs) { x = fold(x); y = fold(y); } if (x != y) return 0; }
    return 1;
}
static void find_refresh(void) {
    Doc* d = D; int i, n = fFind.len, c0;
    if (!matches) return;
    if (!findOpen || n == 0) { nmatches = 0; mdoc = 0; return; }
    if (mdoc == d && mver == d->ver && mcase == matchCase && strcmp(mneedle, fFind.buf) == 0) return;
    mdoc = d; mver = d->ver; mcase = matchCase; mlen = n; strlcpy_safe(mneedle, fFind.buf, sizeof mneedle);
    nmatches = 0;
    c0 = matchCase ? (unsigned char)fFind.buf[0] : fold((unsigned char)fFind.buf[0]);
    for (i = 0; i + n <= d->len && nmatches < MAX_MATCHES; i++) {
        int c = matchCase ? (unsigned char)d->text[i] : fold((unsigned char)d->text[i]);
        if (c == c0 && eq_at(d->text + i, fFind.buf, n, matchCase)) { matches[nmatches++] = i; i += n - 1; }
    }
}
/* index of the first match starting at or after pos (nmatches if none) */
static int match_lower(int pos) {
    int lo = 0, hi = nmatches;
    while (lo < hi) { int mid = (lo + hi) / 2; if (matches[mid] < pos) lo = mid + 1; else hi = mid; }
    return lo;
}
static int match_index_of_sel(void) {                   /* which match the selection is, or -1 */
    int k;
    if (!findOpen || !fFind.len || !has_sel(D) || sel_b(D) - sel_a(D) != mlen) return -1;
    k = match_lower(sel_a(D));
    return (k < nmatches && matches[k] == sel_a(D)) ? k : -1;
}
static void select_match(int k) {
    D->anchor = matches[k]; D->caret = matches[k] + mlen; D->merge = 0; set_want(); reveal();
}
static int find_step(int dir, int fromSelStart) {        /* 1 if a match was selected */
    int k;
    find_refresh();
    if (nmatches == 0) { set_msg(fFind.len ? "No results" : "Type something to find", 0); return 0; }
    if (dir > 0) {
        k = match_lower(fromSelStart ? sel_a(D) : sel_b(D));
        if (k >= nmatches) k = 0;
    } else {
        k = match_lower(sel_a(D)) - 1;
        if (k < 0) k = nmatches - 1;
    }
    select_match(k);
    return 1;
}
static void replace_one(void) {
    int k;
    find_refresh();
    k = match_index_of_sel();
    if (k >= 0) {
        int a = sel_a(D);
        if (doc_edit(D, a, mlen, fRepl.buf, fRepl.len, a + fRepl.len, a + fRepl.len, MG_NONE)) set_want();
        find_refresh();
        if (nmatches) { int j = match_lower(D->caret); select_match(j >= nmatches ? 0 : j); }
        else set_msg("Replaced the last match", 0);
    } else find_step(1, 1);
}
static void replace_all(void) {
    Doc* d = D; int i, n, out = 0, last, start, end;
    char* buf;
    find_refresh();
    if (!nmatches) { set_msg("No results", 0); return; }
    start = matches[0]; end = matches[nmatches - 1] + mlen;
    if ((long)(end - start) + (long)nmatches * (fRepl.len - mlen) + d->len - (end - start) > DOC_MAX) { set_msg("Result too large", 1); return; }
    buf = (char*)malloc((size_t)((end - start) + nmatches * imax(0, fRepl.len - mlen) + 16));
    if (!buf) { set_msg("Out of memory", 1); return; }
    last = start;
    for (i = 0; i < nmatches; i++) {
        n = matches[i] - last; memcpy(buf + out, d->text + last, (size_t)n); out += n;
        memcpy(buf + out, fRepl.buf, (size_t)fRepl.len); out += fRepl.len;
        last = matches[i] + mlen;
    }
    n = nmatches;
    if (doc_edit(d, start, end - start, buf, out, start + out, start + out, MG_NONE)) {
        char m[40]; u2s(n, m); cat(m, sizeof m, n == 1 ? " replacement" : " replacements"); set_msg(m, 0);
        set_want(); reveal();
    }
    free(buf);
}

/* ---- matching bracket ---- */
static int bm_a = -1, bm_b = -1, bm_caret = -1, bm_ver = -1; static Doc* bm_doc;
static int bracket_pair(int c) { return c == '(' ? ')' : c == ')' ? '(' : c == '[' ? ']' : c == ']' ? '[' : c == '{' ? '}' : c == '}' ? '{' : 0; }
static void bracket_update(void) {
    Doc* d = D; int p, i, depth, open, dir, want, found = -1, tries;
    if (bm_doc == d && bm_caret == d->caret && bm_ver == d->ver) return;
    bm_doc = d; bm_caret = d->caret; bm_ver = d->ver; bm_a = bm_b = -1;
    if (has_sel(d)) return;
    for (tries = 0; tries < 2; tries++) {                 /* the character before the caret, then the one after */
        p = tries == 0 ? d->caret - 1 : d->caret;
        if (p < 0 || p >= d->len) continue;
        want = bracket_pair((unsigned char)d->text[p]);
        if (!want) continue;
        open = d->text[p]; dir = (open == '(' || open == '[' || open == '{') ? 1 : -1; depth = 0;
        for (i = p; i >= 0 && i < d->len && i - p < 60000 && p - i < 60000; i += dir) {
            int c = (unsigned char)d->text[i];
            if (c == open) depth++;
            else if (c == want && --depth == 0) { found = i; break; }
        }
        if (found >= 0) { bm_a = p; bm_b = found; return; }
    }
}

/* ===========================================================================
 *  Layout
 * =========================================================================== */
typedef struct { int W, H, sbw, ex, ey, ew, eh, gw, tx, tvw, tvh, needH; } Lay;
static Lay L;

static int digits_of(int v) { int n = 1; while (v >= 10) { v /= 10; n++; } return n; }
static void calc_layout(void) {
    Doc* d = D;
    L.W = easyui.width(); L.H = easyui.height();
    L.sbw = sbOn ? iclamp(sbW, SB_W_MIN, imax(SB_W_MIN, imin(SB_W_MAX, L.W - 260))) : 0;
    L.ex = L.sbw; L.ey = TAB_H; L.ew = L.W - L.sbw; L.eh = imax(40, L.H - TAB_H - STATUS_H);
    L.gw = imax(3, digits_of(d->nl)) * CW + 2 * GUT_PAD;
    L.tx = L.ex + L.gw;
    L.tvw = imax(40, L.ew - L.gw - SCROLL_W);
    L.needH = d->maxLine * CW + 8 * CW > L.tvw;
    L.tvh = L.eh - (L.needH ? SCROLL_W : 0);
}
static int max_scroll_y(void) { return imax(0, D->nl * LH - L.tvh + L.tvh / 3); }
static int max_scroll_x(void) { return imax(0, D->maxLine * CW + 8 * CW - L.tvw + 24); }
static void clamp_scroll(void) {
    D->scrollY = iclamp(D->scrollY, 0, max_scroll_y());
    D->scrollX = iclamp(D->scrollX, 0, imax(max_scroll_x(), 0));
    D->targetY = iclamp(D->targetY, 0, max_scroll_y());
}
static void reveal(void) {
    Doc* d = D; int l, y, cx;
    calc_layout();
    l = line_of(d, d->caret); y = l * LH;
    if (y < d->scrollY) d->scrollY = y;
    else if (y + LH > d->scrollY + L.tvh) d->scrollY = y + LH - L.tvh;
    d->scrollY = iclamp(d->scrollY, 0, max_scroll_y());
    d->targetY = d->scrollY;
    cx = cell_of(d, l, col_of(d, d->caret)) * CW;
    if (cx < d->scrollX) d->scrollX = imax(0, cx - 8 * CW);
    else if (cx + CW + 8 > d->scrollX + L.tvw - 8) d->scrollX = cx + CW + 8 - (L.tvw - 8) + 8 * CW;
    if (d->scrollX < 0) d->scrollX = 0;
    caretOn = 1;
}

/* ---- tab bar ---- */
static int tabX[MAX_TABS], tabWd[MAX_TABS], tabScroll;
static int UI_H = 15;                                    /* height of one line of UI text */
static int TW(const char* s) { return easyui.textwidth(s, 1); }
static void tab_geom(void) {
    int i, sum = 0, avail = L.W - 38, x = 0;
    for (i = 0; i < ntabs; i++) { tabWd[i] = iclamp(TW(tabs[i]->name) + 16 + 28, 96, 210); sum += tabWd[i]; }
    if (sum > avail) {
        for (i = 0; i < ntabs; i++) tabWd[i] = imax(64, tabWd[i] * avail / sum);
    }
    for (i = 0; i < ntabs; i++) { tabX[i] = x; x += tabWd[i]; }
    if (x <= avail) tabScroll = 0;
    else {
        if (tabX[cur] < tabScroll) tabScroll = tabX[cur];
        if (tabX[cur] + tabWd[cur] > tabScroll + avail) tabScroll = tabX[cur] + tabWd[cur] - avail;
        tabScroll = iclamp(tabScroll, 0, x - avail);
    }
}

/* ---- find / goto overlay rectangles ---- */
enum { FG_BAR, FG_FIND, FG_CASE, FG_PREV, FG_NEXT, FG_CLOSE, FG_REPL, FG_ONE, FG_ALL, FG_GOTOBAR, FG_GOTO, FG_N };
static int fr[FG_N][4];
static void overlay_geom(void) {
    int bw = imin(520, L.tvw - 24), x, y = L.ey + 6, fh = 24, rowGap = 4, countW = 74, btn = 26, i;
    if (bw < 300) bw = imax(220, L.tvw - 8);
    x = L.ex + L.ew - SCROLL_W - bw - 8;
    for (i = 0; i < FG_N; i++) fr[i][0] = fr[i][1] = fr[i][2] = fr[i][3] = 0;
    fr[FG_BAR][0] = x; fr[FG_BAR][1] = y; fr[FG_BAR][2] = bw; fr[FG_BAR][3] = (replOpen ? 2 : 1) * fh + (replOpen ? rowGap : 0) + 12;
    {
        int ix = x + 6, iy = y + 6, right = x + bw - 6;
        int closeX = right - btn, nextX = closeX - btn, prevX = nextX - btn, caseX = prevX - btn, cntX = caseX - countW;
        fr[FG_FIND][0] = ix; fr[FG_FIND][1] = iy; fr[FG_FIND][2] = cntX - ix - 4; fr[FG_FIND][3] = fh;
        fr[FG_CASE][0] = caseX; fr[FG_CASE][1] = iy; fr[FG_CASE][2] = btn; fr[FG_CASE][3] = fh;
        fr[FG_PREV][0] = prevX; fr[FG_PREV][1] = iy; fr[FG_PREV][2] = btn; fr[FG_PREV][3] = fh;
        fr[FG_NEXT][0] = nextX; fr[FG_NEXT][1] = iy; fr[FG_NEXT][2] = btn; fr[FG_NEXT][3] = fh;
        fr[FG_CLOSE][0] = closeX; fr[FG_CLOSE][1] = iy; fr[FG_CLOSE][2] = btn; fr[FG_CLOSE][3] = fh;
        if (replOpen) {
            int ry = iy + fh + rowGap, allW = 40, oneW = 66;
            fr[FG_ALL][0] = right - allW; fr[FG_ALL][1] = ry; fr[FG_ALL][2] = allW; fr[FG_ALL][3] = fh;
            fr[FG_ONE][0] = fr[FG_ALL][0] - oneW - 4; fr[FG_ONE][1] = ry; fr[FG_ONE][2] = oneW; fr[FG_ONE][3] = fh;
            fr[FG_REPL][0] = ix; fr[FG_REPL][1] = ry; fr[FG_REPL][2] = fr[FG_ONE][0] - ix - 4; fr[FG_REPL][3] = fh;
        }
    }
    {
        int gw = 250, gx = L.ex + L.ew - SCROLL_W - gw - 8;
        fr[FG_GOTOBAR][0] = gx; fr[FG_GOTOBAR][1] = y; fr[FG_GOTOBAR][2] = gw; fr[FG_GOTOBAR][3] = fh + 12;
        fr[FG_GOTO][0] = gx + 82; fr[FG_GOTO][1] = y + 6; fr[FG_GOTO][2] = gw - 82 - 8; fr[FG_GOTO][3] = fh;
    }
}
static int in_rect(int x, int y, const int* r) { return r[2] > 0 && x >= r[0] && y >= r[1] && x < r[0] + r[2] && y < r[1] + r[3]; }

/* ===========================================================================
 *  Saving, opening, dialogs
 * =========================================================================== */
enum { DLG_NONE = 0, DLG_CLOSE, DLG_QUIT };
static int dlgKind, dlgSel; static Doc* dlgDoc;
static int pendKind; static Doc* pendDoc; static Doc* saveAsDoc;
static int quitting;

static void update_title(void) {
    static char last[96];
    char t[96];
    strlcpy_safe(t, D->name, sizeof t);
    if (D->modified) cat(t, sizeof t, " *");
    cat(t, sizeof t, " - Code");
    if (strcmp(t, last) != 0) { easyui.settitle(t); strlcpy_safe(last, t, sizeof last); }
}

static void dlg_open(int kind, Doc* d) { dlgKind = kind; dlgDoc = d; dlgSel = 0; easyui.redraw(); }
static void session_save(void);
static void continue_quit(void) {
    int i;
    for (i = 0; i < ntabs; i++)
        if (tabs[i]->modified && !tabs[i]->discard) { tab_activate(i); quitting = 1; dlg_open(DLG_QUIT, tabs[i]); return; }
    session_save();
    easyui.quit();
}
static void close_tab_request(int i) {
    if (i < 0 || i >= ntabs) return;
    if (tabs[i]->modified && !tabs[i]->discard) { dlg_open(DLG_CLOSE, tabs[i]); return; }
    tab_remove(i);
}

static int trimOnSave;
static int trim_trailing(Doc* d);
static int do_save_to(Doc* d, const char* path) {
    if (trimOnSave) trim_trailing(d);
    if (doc_write(d, path)) {
        if (d->untitled || strcmp(d->path, path) != 0) { doc_set_path(d, path); tree_refresh(); }
        d->modified = 0; d->saveMark = d->ecur; d->merge = 0;
        set_msg2("Saved ", d->name, 0);
        return 1;
    }
    set_msg2("Save FAILED: ", path, 1);
    return 0;
}
static const char* dialog_folder(void) {
    static char f[PATH_MAX_];
    if (!D->untitled && D->path[0]) dir_of(D->path, f, sizeof f); else strlcpy_safe(f, treeRoot, sizeof f);
    return f;
}
static void on_saveas_done(int chosen) {
    Doc* d = saveAsDoc; int kind = pendKind; Doc* pd = pendDoc;
    saveAsDoc = 0; pendKind = DLG_NONE; pendDoc = 0;
    if (!d || tab_index(d) < 0) return;
    if (!chosen) { quitting = 0; return; }
    {
        char path[PATH_MAX_];
        path_resolve(easyui.pickedpath(), path, sizeof path);
        if (!do_save_to(d, path)) { quitting = 0; return; }
    }
    if (kind == DLG_CLOSE && pd == d) { int i = tab_index(d); if (i >= 0) tab_remove(i); }
    else if (kind == DLG_QUIT) continue_quit();
}
static void start_save_as(Doc* d) {
    saveAsDoc = d;
    easyui.savefile("Save As", dialog_folder(), d->untitled ? "untitled.txt" : d->name, on_saveas_done);
}
/* 1 saved, 0 failed, 2 a Save As dialog was opened */
static int save_doc(Doc* d) {
    if (d->untitled || !d->path[0]) { start_save_as(d); return 2; }
    return do_save_to(d, d->path);
}
static void dlg_choose(int which) {                      /* 0 save, 1 don't save, 2 cancel */
    Doc* d = dlgDoc; int kind = dlgKind, i;
    dlgKind = DLG_NONE; dlgDoc = 0;
    if (!d || (i = tab_index(d)) < 0) return;
    if (which == 2) { quitting = 0; return; }
    if (which == 1) {
        d->discard = 1;
        if (kind == DLG_CLOSE) tab_remove(i); else continue_quit();
        return;
    }
    pendKind = kind; pendDoc = d;
    {
        int r = save_doc(d);
        if (r == 2) return;                               /* the Save As dialog continues the job */
        pendKind = DLG_NONE; pendDoc = 0;
        if (r == 1) { if (kind == DLG_CLOSE) tab_remove(i); else continue_quit(); }
        else quitting = 0;
    }
}
static void on_open_done(int chosen) { if (chosen) open_file(easyui.pickedpath()); }
static void on_folder_done(int chosen) {
    if (chosen) { char p[PATH_MAX_]; path_resolve(easyui.pickedpath(), p, sizeof p); tree_set_root(p); sbOn = 1; }
}
static void new_tab(void) { Doc* d = tab_new_doc(0); if (d) tab_activate(ntabs - 1); }
static void next_tab(int dir) { if (ntabs > 1) tab_activate((cur + dir + ntabs) % ntabs); }

static void open_find(int withReplace) {
    findOpen = 1; replOpen = withReplace; gotoOpen = 0; focus = withReplace && fFind.len ? F_REPL : F_FIND;
    if (has_sel(D) && sel_b(D) - sel_a(D) < 120 && line_of(D, sel_a(D)) == line_of(D, sel_b(D))) {
        char b[130]; int n = sel_b(D) - sel_a(D); memcpy(b, D->text + sel_a(D), (size_t)n); b[n] = 0; field_set(&fFind, b);
        focus = withReplace ? F_REPL : F_FIND;
    }
    fFind.all = fFind.len > 0 && focus == F_FIND; fFind.caret = fFind.len;
    find_refresh();
}
static void close_overlays(void) { findOpen = gotoOpen = 0; focus = F_EDITOR; }
static void open_goto(void) {
    gotoOpen = 1; findOpen = 0; focus = F_GOTO; field_set(&fGoto, ""); 
}
static void do_goto(void) {
    int line = 0, col = 0, i = 0;
    const char* s = fGoto.buf;
    while (s[i] >= '0' && s[i] <= '9') line = line * 10 + (s[i++] - '0');
    if (s[i] == ':') { i++; while (s[i] >= '0' && s[i] <= '9') col = col * 10 + (s[i++] - '0'); }
    if (i == 0) { set_msg("Type a line number", 0); return; }
    line = iclamp(line - 1, 0, D->nl - 1);
    {
        int l = line, e = line_end_(D, l) - D->ls[l];
        move_to(D->ls[l] + iclamp(col > 0 ? col - 1 : 0, 0, e), 0, 0);
        if (!col) D->caret = D->anchor = D->ls[l] + (first_nonspace(D, l) - D->ls[l]);
    }
    set_want(); close_overlays();
    /* put the line near the middle of the view */
    calc_layout(); D->scrollY = iclamp(line * LH - L.tvh / 2 + LH, 0, max_scroll_y()); D->targetY = D->scrollY;
}

/* ===========================================================================
 *  Drawing
 * =========================================================================== */
static int dragVsb, dragHsb;                             /* scrollbar thumbs being dragged */
static int hoverId;                                      /* what the pointer is over, see hover_at() */
enum { H_NONE = 0, H_PLUS = 1, H_VSB, H_HSB, H_SPLIT, H_TAB = 1000, H_CLOSE = 2000, H_ROW = 3000, H_FG = 4000, H_DLG = 5000 };

static void R(int x, int y, int w, int h, uint32_t c) { if (w > 0 && h > 0) easyui.fillrect(x, y, w, h, c); }
static void T(int x, int y, const char* s, uint32_t c) { easyui.drawtext(x, y, s, c, 1); }
static void clipr(int x, int y, int w, int h) { eui_clip(x, y, imax(w, 0), imax(h, 0)); }
static void unclip(void) { eui_clip(0, 0, L.W, L.H); }
static void code_text(int x, int y, const char* s, uint32_t c) { easyui.drawtextfont(x, y, s, c, fontId, FPX); }
static int text_off(void) { return (LH - (FPX * 13 / 10)) / 2; }

/* shortens `s` to fit maxw pixels; left = keep the end ("...name") */
static void fit(char* out, int cap, const char* s, int maxw, int left) {
    int n = (int)strlen(s), k;
    strlcpy_safe(out, s, (size_t)cap);
    if (TW(out) <= maxw) return;
    for (k = n - 1; k > 0; k--) {
        char t[PATH_MAX_ + 8];
        if (left) { strlcpy_safe(t, "...", sizeof t); cat(t, sizeof t, s + (n - k)); }
        else { int m = imin(k, (int)sizeof t - 5); memcpy(t, s, (size_t)m); t[m] = 0; cat(t, sizeof t, "..."); }
        if (TW(t) <= maxw || k == 1) { strlcpy_safe(out, t, (size_t)cap); return; }
    }
}
static void chevron(int x, int y, int open, uint32_t c) {
    int i;
    if (open) for (i = 0; i < 4; i++) R(x + i, y + 2 + i, 7 - 2 * i, 1, c);
    else for (i = 0; i < 4; i++) R(x + 1 + i, y + i - 1, 1, 7 - 2 * i, c);
}
static void draw_x(int cx, int cy, uint32_t c) {         /* a small cross centred on cx, cy */
    easyui.drawline(cx - 4, cy - 4, cx + 4, cy + 4, c); easyui.drawline(cx - 4, cy - 3, cx + 3, cy + 4, c);
    easyui.drawline(cx + 4, cy - 4, cx - 4, cy + 4, c); easyui.drawline(cx + 4, cy - 3, cx - 3, cy + 4, c);
}
static uint32_t lang_color(int lang) {
    switch (lang) {
        case LANG_C: return 0xFF5C9BD1; case LANG_PY: return 0xFFE5C453; case LANG_SH: return 0xFF8FC46F;
        case LANG_LUA: return 0xFF5B7FE0; case LANG_JS: return 0xFFE2C95A; case LANG_JSON: return 0xFFD9A441;
        case LANG_MD: return 0xFF6CB4E4; case LANG_MAKE: return 0xFFD0773F; default: return 0xFF8A909B;
    }
}

static int vsb_thumb(int* ty, int* th) {                 /* the vertical scroll thumb, 0 when there is nothing to scroll */
    int maxy = max_scroll_y(), content = maxy + L.tvh;
    if (maxy <= 0) return 0;
    *th = imax(28, L.tvh * L.tvh / content);
    *ty = L.ey + (int)((long)D->scrollY * (L.tvh - *th) / maxy);
    return 1;
}
static int hsb_thumb(int* tx, int* tw) {
    int maxx = max_scroll_x(), content = maxx + L.tvw;
    if (!L.needH || maxx <= 0) return 0;
    *tw = imax(28, L.tvw * L.tvw / content);
    *tx = L.tx + (int)((long)D->scrollX * (L.tvw - *tw) / maxx);
    return 1;
}

static void draw_occurrences(int line, int y, int x0);
static void draw_completion(void);
static void draw_palette(void);
static void draw_editor(void) {
    Doc* d = D;
    const easyui_theme_t* th = easyui.theme();
    int sy = d->scrollY, sx = d->scrollX, first = sy / LH, last = imin(d->nl, (sy + L.tvh) / LH + 2);
    int x0 = L.tx + 6 - sx, c0 = sx / CW, c1 = c0 + L.tvw / CW + 3, i, st, cl = line_of(d, d->caret), toff = text_off();
    uint32_t selc = with_alpha(g_accent, 0x58), matchc = 0x48E8A33D, curc = 0x70E8A33D;
    static char cells[1100]; static unsigned char ccol[1100]; char run[260];
    (void)th;
    if (c1 - c0 > 1000) c1 = c0 + 1000;
    find_refresh(); bracket_update();

    R(L.ex, L.ey, L.ew, L.eh, C_BG);
    clipr(L.tx, L.ey, L.tvw, L.tvh);
    st = line_state(d, first);
    for (i = first; i < last; i++) {
        int a = d->ls[i], e = line_end_(d, i), y = L.ey + i * LH - sy, n = e - a, b, k, cell = 0, ncell = 0, rs = -1, rc = 0;
        if (i == cl) R(L.tx, y, L.tvw, LH, C_LINE);
        /* indent guides */
        {
            int ind = indent_of(d, i), g;
            for (g = TABW; g < ind; g += TABW) R(x0 + g * CW - 1, y, 1, LH, 0xFF33353A);
        }
        /* find matches */
        if (findOpen && nmatches) {
            int m = match_lower(a - mlen + 1);
            for (; m < nmatches && matches[m] < e; m++) {
                int ms = matches[m] - a, x1, x2;
                if (matches[m] + mlen <= a) continue;
                x1 = cell_of(d, i, ms); x2 = cell_of(d, i, ms + mlen);
                R(x0 + x1 * CW, y + 1, (x2 - x1) * CW, LH - 2, matches[m] == sel_a(d) && mlen == sel_b(d) - sel_a(d) ? curc : matchc);
            }
        }
        draw_occurrences(i, y, x0);
        /* selection */
        if (has_sel(d) && sel_a(d) <= e && sel_b(d) > a) {
            int sa = imax(sel_a(d), a) - a, sb = imin(sel_b(d), e) - a;
            int x1 = cell_of(d, i, sa), x2 = cell_of(d, i, sb);
            if (sel_b(d) > e) x2 += 1;                     /* the selected line break */
            R(x0 + x1 * CW, y, (x2 - x1) * CW, LH, selc);
        }
        /* matching brackets */
        if (bm_a >= 0 && ((bm_a >= a && bm_a <= e) || (bm_b >= a && bm_b <= e))) {
            int p;
            for (p = 0; p < 2; p++) {
                int o = p ? bm_b : bm_a, x1;
                if (o < a || o >= e) continue;
                x1 = x0 + cell_of(d, i, o - a) * CW;
                R(x1, y + 1, CW, LH - 2, 0x40FFFFFF);
                easyui.drawrect(x1, y + 1, CW, LH - 2, 0xFF8A8F98);
            }
        }
        /* text */
        if (d->lang != LANG_TEXT) st = scan_line(d->lang, d->text + a, n, st, hl_cols, HL_CAP);
        else memset(hl_cols, 0, HL_CAP);
        for (b = a; b < e && cell < c1; b++) {
            unsigned char ch = (unsigned char)d->text[b];
            int w = ch == '\t' ? TABW - cell % TABW : 1, color = b - a < HL_CAP ? hl_cols[b - a] : 0;
            for (k = 0; k < w; k++, cell++) {
                if (cell >= c0 && cell < c1 && ncell < 1000) {
                    cells[ncell] = (ch == '\t') ? ' ' : (ch >= 32 && ch < 127) ? (char)ch : '?';
                    ccol[ncell] = (unsigned char)((ch == '\t') ? 0 : color);
                    ncell++;
                }
            }
        }
        for (k = 0; k <= ncell; k++) {                     /* draw in runs of one colour; spaces join the run */
            int flush = 0;
            if (k == ncell) flush = rs >= 0;
            else if (cells[k] != ' ') {
                if (rs < 0) { rs = k; rc = ccol[k]; }
                else if (ccol[k] != rc) flush = 1;
            }
            if (flush) {
                int re = k, len, pos = rs;
                if (k < ncell) { re = k; } 
                while (re > rs && cells[re - 1] == ' ') re--;
                while (pos < re) {
                    len = imin(re - pos, 240);
                    memcpy(run, cells + pos, (size_t)len); run[len] = 0;
                    code_text(x0 + (c0 + pos) * CW, y + toff, run, PAL[rc]);
                    pos += len;
                }
                rs = -1;
                if (k < ncell && cells[k] != ' ') { rs = k; rc = ccol[k]; }
            }
        }
        /* caret */
        if (caretOn && i == cl) {
            int x = x0 + cell_of(d, i, d->caret - a) * CW;
            R(x, y + 2, 2, LH - 4, 0xFFE6E6E6);
        }
    }
    /* a hint on an empty new document */
    if (d->len == 0 && d->untitled) {
        static const char* hints[] = { "Ctrl+O  Open file", "Ctrl+S  Save", "Ctrl+B  Toggle sidebar", "Ctrl+F  Find   Ctrl+H  Replace", "Ctrl+G  Go to line", "Ctrl+W  Close tab" };
        int k, y = L.ey + L.tvh / 2 - 70;
        for (k = 0; k < 6; k++) { int w = TW(hints[k]); T(L.tx + (L.tvw - w) / 2, y + k * 22, hints[k], 0xFF4F5560); }
    }
    unclip();

    /* gutter */
    R(L.ex, L.ey, L.gw, L.eh, C_BG);
    clipr(L.ex, L.ey, L.gw, L.tvh);
    for (i = first; i < last; i++) {
        char num[12]; int y = L.ey + i * LH - sy;
        u2s(i + 1, num);
        code_text(L.tx - GUT_PAD - (int)strlen(num) * CW, y + toff, num, i == cl ? C_GUTTER_HI : C_GUTTER);
    }
    unclip();

    /* scrollbars */
    {
        int ty, tl; uint32_t tc;
        if (vsb_thumb(&ty, &tl)) {
            tc = hoverId == H_VSB || dragVsb ? 0x99808690 : 0x55808690;
            R(L.ex + L.ew - SCROLL_W + 2, ty, SCROLL_W - 4, tl, tc);
        }
        if (hsb_thumb(&ty, &tl)) {
            tc = hoverId == H_HSB ? 0x99808690 : 0x55808690;
            R(ty, L.ey + L.eh - SCROLL_W + 2, tl, SCROLL_W - 4, tc);
        }
    }
}

static void draw_sidebar(void) {
    int i, y, bottom = L.H - STATUS_H, rowsTop = TAB_H + SB_HDR;
    char t[PATH_MAX_];
    if (!sbOn) return;
    R(0, TAB_H, L.sbw, L.H - TAB_H - STATUS_H, C_SIDE);
    R(L.sbw - 1, TAB_H, 1, L.H - TAB_H - STATUS_H, C_BORDER);
    T(14, TAB_H + 9, "EXPLORER", C_MUTED);
    if (treeActive == -2) treeActive = tree_find_active();
    clipr(0, rowsTop, L.sbw - 1, bottom - rowsTop);
    {
        int maxScroll = imax(0, tcount * ROW_H - (bottom - rowsTop) + 8);
        treeScroll = iclamp(treeScroll, 0, maxScroll);
    }
    for (i = treeScroll / ROW_H; i < tcount; i++) {
        TNode* nd = &tn[i];
        int x = 10 + nd->depth * 14;
        y = rowsTop + i * ROW_H - treeScroll;
        if (y >= bottom) break;
        if (y + ROW_H <= rowsTop) continue;
        if (i == treeActive) R(0, y, L.sbw - 1, ROW_H, with_alpha(g_accent, 0x38));
        else if (hoverId == H_ROW + i) R(0, y, L.sbw - 1, ROW_H, C_TAB_HOT);
        if (nd->isdir) chevron(x, y + (ROW_H - 8) / 2, nd->open, C_MUTED);
        else R(x + 4, y + ROW_H / 2 - 3, 6, 6, lang_color(lang_for_name(nd->name)));
        fit(t, sizeof t, nd->name, L.sbw - 1 - (x + 18) - 6, 0);
        T(x + 18, y + (ROW_H - UI_H) / 2, t, i == treeActive ? 0xFFFFFFFF : nd->isdir ? C_TEXT : 0xFFC4C8D0);
    }
    if (tcount == 0) T(14, rowsTop + 8, "Folder is empty", C_MUTED);
    unclip();
    /* header: folder name */
    R(0, TAB_H, L.sbw - 1, SB_HDR, C_SIDE);
    T(14, TAB_H + 9, "EXPLORER", C_MUTED);
    fit(t, sizeof t, base_name(treeRoot)[0] ? base_name(treeRoot) : treeRoot, L.sbw - 100, 0);
    {
        int w = TW(t);
        T(L.sbw - 14 - w, TAB_H + 9, t, 0xFFA9AEB8);
    }
    /* scroll thumb */
    {
        int view = bottom - rowsTop, content = tcount * ROW_H + 8;
        if (content > view) {
            int th2 = imax(24, view * view / content), ty = rowsTop + (int)((long)treeScroll * (view - th2) / imax(1, content - view));
            R(L.sbw - 7, ty, 4, th2, 0x44808690);
        }
    }
}

static void draw_tabs(void) {
    int i;
    R(0, 0, L.W, TAB_H, C_TABBAR);
    R(0, TAB_H - 1, L.W, 1, C_BORDER);
    tab_geom();
    clipr(0, 0, L.W - 36, TAB_H);
    for (i = 0; i < ntabs; i++) {
        Doc* d = tabs[i]; int x = tabX[i] - tabScroll, w = tabWd[i], act = i == cur, hot = hoverId == H_TAB + i || hoverId == H_CLOSE + i;
        char t[80];
        R(x, 0, w, TAB_H, act ? C_BG : hot ? C_TAB_HOT : C_TAB);
        R(x + w - 1, 0, 1, TAB_H - 1, C_BORDER);
        if (act) { R(x, 0, w, 2, g_accent); R(x, TAB_H - 1, w, 1, C_BG); }
        else R(x, TAB_H - 1, w, 1, C_BORDER);
        R(x + 10, TAB_H / 2 - 3, 6, 6, lang_color(d->lang));
        fit(t, sizeof t, d->name, w - 24 - 28, 0);
        T(x + 22, (TAB_H - UI_H) / 2 + 1, t, act ? 0xFFFFFFFF : 0xFFA9AEB8);
        {   /* close button / dirty dot */
            int cx = x + w - 16, cy = TAB_H / 2;
            int overClose = hoverId == H_CLOSE + i;
            if (overClose) R(cx - 9, cy - 9, 18, 18, 0xFF3A3C42);
            if (d->modified && !overClose) easyui.fillcircle(cx, cy, 4, act ? 0xFFE6E6E6 : 0xFFA9AEB8);
            else if (act || hot) draw_x(cx, cy, overClose ? 0xFFFFFFFF : 0xFFA9AEB8);
        }
    }
    unclip();
    {   /* the + button */
        int x = L.W - 34;
        R(x, 0, 34, TAB_H - 1, C_TABBAR);
        if (hoverId == H_PLUS) R(x + 5, 6, 24, 22, 0xFF33353A);
        R(x + 11, TAB_H / 2 - 1, 12, 2, 0xFFA9AEB8); R(x + 16, TAB_H / 2 - 6, 2, 12, 0xFFA9AEB8);
    }
}

static void draw_status(void) {
    Doc* d = D; uint32_t bg = mix(g_accent, 0xFF000000u, 100), fg = 0xFFF2F4F7, dim = mix(fg, bg, 90);
    char left[PATH_MAX_ + 8], right[96], t[PATH_MAX_ + 8], num[12];
    int y = L.H - STATUS_H, ty = y + (STATUS_H - UI_H) / 2, rw = 0, l = line_of(d, d->caret), lw;
    R(0, y, L.W, STATUS_H, bg);
    /* right side: Ln, Col | Spaces | EOL | language */
    strlcpy_safe(right, "Ln ", sizeof right); cat_num(right, sizeof right, l + 1);
    cat(right, sizeof right, ", Col "); cat_num(right, sizeof right, cell_of(d, l, col_of(d, d->caret)) + 1);
    if (has_sel(d)) { u2s(sel_b(d) - sel_a(d), num); cat(right, sizeof right, " ("); cat(right, sizeof right, num); cat(right, sizeof right, " selected)"); }
    {
        const char* items[4]; int w[4], k, x = L.W - 14;
        items[0] = right; items[1] = d->lang == LANG_MAKE ? "Tabs" : "Spaces: 4"; items[2] = d->crlf ? "CRLF" : "LF"; items[3] = g_langs[d->lang].name;
        for (k = 0; k < 4; k++) w[k] = TW(items[k]);
        for (k = 0; k < 4; k++) { x -= w[k]; T(x, ty, items[k], fg); x -= 18; rw += w[k] + 18; }
    }
    /* left side: path, dirty mark, message */
    strlcpy_safe(left, d->untitled ? d->name : d->path, sizeof left);
    if (d->modified) cat(left, sizeof left, "  (unsaved)");
    lw = L.W - rw - 40;
    if (g_msg[0] && mos_uptime() < g_msg_until) {
        int mw = TW(g_msg);
        fit(t, sizeof t, left, imax(60, lw - mw - 30), 1);
        T(12, ty, t, dim);
        T(12 + TW(t) + 22, ty, g_msg, g_msg_err ? 0xFFFFC9C4 : fg);
    } else {
        fit(t, sizeof t, left, lw, 1);
        T(12, ty, t, dim);
    }
}

static void draw_field(const int* r, Field* f, int id, const char* placeholder) {
    int focused = focus == id, tx = r[0] + 7, ty = r[1] + (r[3] - UI_H) / 2, w;
    char t[170];
    R(r[0], r[1], r[2], r[3], C_FIELD);
    easyui.drawrect(r[0], r[1], r[2], r[3], focused ? g_accent : 0xFF3B3D43);
    clipr(r[0] + 2, r[1], r[2] - 4, r[3]);
    if (f->len == 0) T(tx, ty, placeholder, 0xFF6B717C);
    else {
        if (f->all) R(tx - 1, r[1] + 3, TW(f->buf) + 2, r[3] - 6, with_alpha(g_accent, 0x70));
        T(tx, ty, f->buf, C_TEXT);
    }
    if (focused && caretOn) {
        strlcpy_safe(t, f->buf, sizeof t); t[imin(f->caret, (int)sizeof t - 1)] = 0;
        w = f->caret ? TW(t) : 0;
        R(tx + w, r[1] + 4, 1, r[3] - 8, 0xFFE6E6E6);
    }
    unclip();
}
static void small_button(const int* r, const char* label, int hot, int on) {
    int w = TW(label);
    if (on) R(r[0], r[1], r[2], r[3], with_alpha(g_accent, 0x70));
    else if (hot) R(r[0], r[1], r[2], r[3], 0xFF3A3C42);
    T(r[0] + (r[2] - w) / 2, r[1] + (r[3] - UI_H) / 2, label, on ? 0xFFFFFFFF : 0xFFC4C8D0);
}
static void draw_overlays(void) {
    overlay_geom();
    if (findOpen) {
        char cnt[40]; int k;
        R(fr[FG_BAR][0] - 1, fr[FG_BAR][1] + 2, fr[FG_BAR][2] + 2, fr[FG_BAR][3] + 2, 0x40000000);
        R(fr[FG_BAR][0], fr[FG_BAR][1], fr[FG_BAR][2], fr[FG_BAR][3], C_PANEL);
        R(fr[FG_BAR][0], fr[FG_BAR][1], 2, fr[FG_BAR][3], g_accent);
        draw_field(fr[FG_FIND], &fFind, F_FIND, "Find");
        find_refresh();
        if (!fFind.len) cnt[0] = 0;
        else if (!nmatches) strlcpy_safe(cnt, "No results", sizeof cnt);
        else {
            k = match_index_of_sel();
            if (k >= 0) { cnt[0] = 0; cat_num(cnt, sizeof cnt, k + 1); cat(cnt, sizeof cnt, " of "); }
            else cnt[0] = 0;
            cat_num(cnt, sizeof cnt, nmatches); if (nmatches >= MAX_MATCHES) cat(cnt, sizeof cnt, "+");
        }
        T(fr[FG_FIND][0] + fr[FG_FIND][2] + 8, fr[FG_FIND][1] + (fr[FG_FIND][3] - UI_H) / 2, cnt, !fFind.len || nmatches ? C_MUTED : C_DANGER);
        small_button(fr[FG_CASE], "Aa", hoverId == H_FG + FG_CASE, matchCase);
        small_button(fr[FG_PREV], "", hoverId == H_FG + FG_PREV, 0);
        small_button(fr[FG_NEXT], "", hoverId == H_FG + FG_NEXT, 0);
        small_button(fr[FG_CLOSE], "", hoverId == H_FG + FG_CLOSE, 0);
        {   /* arrows and the cross, drawn */
            int px = fr[FG_PREV][0] + fr[FG_PREV][2] / 2, py = fr[FG_PREV][1] + fr[FG_PREV][3] / 2;
            int nx = fr[FG_NEXT][0] + fr[FG_NEXT][2] / 2, ny = fr[FG_NEXT][1] + fr[FG_NEXT][3] / 2;
            for (k = 0; k < 4; k++) R(px - k, py - 2 + k, 2 * k + 1, 1, 0xFFC4C8D0);
            for (k = 0; k < 4; k++) R(nx - (3 - k), ny - 2 + k, 2 * (3 - k) + 1, 1, 0xFFC4C8D0);
            draw_x(fr[FG_CLOSE][0] + fr[FG_CLOSE][2] / 2, fr[FG_CLOSE][1] + fr[FG_CLOSE][3] / 2, 0xFFC4C8D0);
        }
        if (replOpen) {
            draw_field(fr[FG_REPL], &fRepl, F_REPL, "Replace");
            small_button(fr[FG_ONE], "Replace", hoverId == H_FG + FG_ONE, 0);
            small_button(fr[FG_ALL], "All", hoverId == H_FG + FG_ALL, 0);
        }
    } else if (gotoOpen) {
        R(fr[FG_GOTOBAR][0] - 1, fr[FG_GOTOBAR][1] + 2, fr[FG_GOTOBAR][2] + 2, fr[FG_GOTOBAR][3] + 2, 0x40000000);
        R(fr[FG_GOTOBAR][0], fr[FG_GOTOBAR][1], fr[FG_GOTOBAR][2], fr[FG_GOTOBAR][3], C_PANEL);
        R(fr[FG_GOTOBAR][0], fr[FG_GOTOBAR][1], 2, fr[FG_GOTOBAR][3], g_accent);
        T(fr[FG_GOTOBAR][0] + 12, fr[FG_GOTO][1] + (fr[FG_GOTO][3] - UI_H) / 2, "Go to line", C_MUTED);
        draw_field(fr[FG_GOTO], &fGoto, F_GOTO, "line[:col]");
    }
}

/* ---- the save-changes dialog ---- */
static void dlg_rects(int r[3][4], int box[4]) {
    int bw = imin(460, L.W - 30), bh = 150, x = (L.W - bw) / 2, y = (L.H - bh) / 2, k, bwid[3] = { 86, 120, 80 }, gx = x + bw - 16;
    box[0] = x; box[1] = y; box[2] = bw; box[3] = bh;
    for (k = 2; k >= 0; k--) { gx -= bwid[k]; r[k][0] = gx; r[k][1] = y + bh - 46; r[k][2] = bwid[k]; r[k][3] = 30; gx -= 10; }
}
static void draw_dialog(void) {
    int r[3][4], box[4], k, w; char t[160], t2[100];
    static const char* labels[3] = { "Save", "Don't Save", "Cancel" };
    if (!dlgKind) return;
    dlg_rects(r, box);
    R(0, 0, L.W, L.H, 0x99000000);
    R(box[0] - 1, box[1] - 1, box[2] + 2, box[3] + 2, 0xFF45474D);
    R(box[0], box[1], box[2], box[3], C_PANEL);
    R(box[0], box[1], box[2], 3, g_accent);
    strlcpy_safe(t2, dlgDoc ? dlgDoc->name : "", sizeof t2);
    easyui.drawtext(box[0] + 20, box[1] + 20, "Unsaved changes", 0xFFFFFFFF, 2);
    strlcpy_safe(t, "Do you want to save the changes you made to ", sizeof t);
    fit(t2, sizeof t2, dlgDoc ? dlgDoc->name : "", imax(60, box[2] - 40 - TW("Do you want to save the changes you made to ?") ), 0);
    cat(t, sizeof t, t2); cat(t, sizeof t, "?");
    T(box[0] + 20, box[1] + 56, t, C_TEXT);
    T(box[0] + 20, box[1] + 76, "Your changes will be lost if you don't save them.", C_MUTED);
    for (k = 0; k < 3; k++) {
        int hot = hoverId == H_DLG + k, sel = dlgSel == k;
        uint32_t bg = k == 0 ? (hot ? mix(g_accent, 0xFFFFFFFFu, 36) : g_accent) : (hot ? 0xFF474A52 : 0xFF3A3D44);
        R(r[k][0], r[k][1], r[k][2], r[k][3], bg);
        if (sel) easyui.drawrect(r[k][0] - 1, r[k][1] - 1, r[k][2] + 2, r[k][3] + 2, 0xFFE6E6E6);
        w = TW(labels[k]);
        T(r[k][0] + (r[k][2] - w) / 2, r[k][1] + (r[k][3] - UI_H) / 2, labels[k], 0xFFFFFFFF);
    }
}

static void on_canvas_draw(int v) {
    (void)v;
    L.W = easyui.width(); L.H = easyui.height();
    g_accent = easyui.accent();
    calc_layout();
    clamp_scroll();
    R(0, 0, L.W, L.H, C_BG);
    draw_editor();
    draw_sidebar();
    draw_tabs();
    draw_status();
    draw_overlays();
    draw_completion();
    draw_palette();
    draw_dialog();
    update_title();
}

/* ===========================================================================
 *  Input
 * =========================================================================== */
static int dragSel, dragLo, dragHi, dragSplit, dragOff;
static int clickCount, lastClickX, lastClickY; static long lastClickT;
static int blinkTimer, scrollTimer, autoTimer;

static int offset_at(int px, int py) {
    Doc* d = D; int l, edge;
    l = iclamp((py - L.ey + d->scrollY) / LH, 0, d->nl - 1);
    if (py - L.ey + d->scrollY < 0) l = 0;
    edge = px - (L.tx + 6) + d->scrollX + CW / 2;
    edge = edge < 0 ? 0 : edge / CW;
    return d->ls[l] + col_at_cell(d, l, edge);
}

static void start_scroll_anim(void);
static void on_scroll_tick(int t) {
    Doc* d = D; int diff = d->targetY - d->scrollY, step;
    (void)t;
    if (diff == 0) { easyui.stoptimer(scrollTimer); scrollTimer = 0; return; }
    step = diff / 3; if (step == 0) step = diff > 0 ? 1 : -1;
    d->scrollY += step;
    if (diff > -2 && diff < 2) d->scrollY = d->targetY;
}
static void start_scroll_anim(void) { if (!scrollTimer) scrollTimer = easyui.settimer(16, on_scroll_tick); }

static void drag_update(int x, int y) {
    int q = offset_at(x, y);
    if (dragSel == 2) {
        if (q < dragLo) { D->anchor = dragHi; D->caret = word_start(D, q); }
        else { D->anchor = dragLo; D->caret = word_end(D, q); }
    } else if (dragSel == 3) {
        int l = line_of(D, q);
        if (q < dragLo) { D->anchor = dragHi; D->caret = D->ls[l]; }
        else { D->anchor = dragLo; D->caret = l + 1 < D->nl ? D->ls[l + 1] : D->len; }
    } else D->caret = q;
    D->merge = 0; set_want(); caretOn = 1;
}
static void on_auto_tick(int t) {
    int x, y, b, top, bot;
    (void)t;
    easyui.mouse(&x, &y, &b);
    if (!dragSel || !(b & 1)) { easyui.stoptimer(autoTimer); autoTimer = 0; dragSel = 0; return; }
    calc_layout();
    top = L.ey; bot = L.ey + L.tvh;
    if (y < top) D->scrollY -= imax(4, (top - y) / 2); else if (y > bot) D->scrollY += imax(4, (y - bot) / 2);
    if (x < L.tx) D->scrollX -= imax(4, (L.tx - x) / 2); else if (x > L.tx + L.tvw) D->scrollX += imax(4, (x - L.tx - L.tvw) / 2);
    clamp_scroll(); D->targetY = D->scrollY;
    drag_update(x, y);
}

static int hover_at(int x, int y) {
    int i, bottom = L.H - STATUS_H;
    if (dlgKind) { int r[3][4], box[4]; dlg_rects(r, box); for (i = 0; i < 3; i++) if (in_rect(x, y, r[i])) return H_DLG + i; return H_NONE; }
    overlay_geom();
    if (findOpen && in_rect(x, y, fr[FG_BAR])) {
        for (i = FG_CASE; i <= FG_CLOSE; i++) if (in_rect(x, y, fr[i])) return H_FG + i;
        if (replOpen) for (i = FG_ONE; i <= FG_ALL; i++) if (in_rect(x, y, fr[i])) return H_FG + i;
        return H_NONE;
    }
    if (y < TAB_H) {
        if (x >= L.W - 34) return H_PLUS;
        tab_geom();
        for (i = 0; i < ntabs; i++) {
            int tx = tabX[i] - tabScroll;
            if (x >= tx && x < tx + tabWd[i]) {
                int cx = tx + tabWd[i] - 16, cy = TAB_H / 2;
                return (x >= cx - 9 && x <= cx + 9 && y >= cy - 9 && y <= cy + 9) ? H_CLOSE + i : H_TAB + i;
            }
        }
        return H_NONE;
    }
    if (y >= bottom) return H_NONE;
    if (sbOn && x < L.sbw) {
        i = (y - TAB_H - SB_HDR + treeScroll) / ROW_H;
        if (y >= TAB_H + SB_HDR && i >= 0 && i < tcount) return H_ROW + i;
        return H_NONE;
    }
    if (sbOn && x >= L.sbw - 2 && x <= L.sbw + 3) return H_SPLIT;
    if (x >= L.ex + L.ew - SCROLL_W && y < L.ey + L.tvh) return H_VSB;
    if (L.needH && y >= L.ey + L.tvh) return H_HSB;
    return H_NONE;
}

static void field_click(Field* f, const int* r, int x) {
    int i, best = 0, bd = 1 << 30; char t[170];
    focus = f == &fFind ? F_FIND : f == &fRepl ? F_REPL : F_GOTO;
    f->all = 0;
    for (i = 0; i <= f->len; i++) {
        int w, dd; strlcpy_safe(t, f->buf, sizeof t); t[imin(i, (int)sizeof t - 1)] = 0;
        w = i ? TW(t) : 0; dd = x - (r[0] + 7 + w); if (dd < 0) dd = -dd;
        if (dd < bd) { bd = dd; best = i; }
    }
    f->caret = best; caretOn = 1;
}
static void find_changed(void) {
    find_refresh();
    if (nmatches && fFind.len) { int k = match_lower(sel_a(D)); select_match(k >= nmatches ? 0 : k); }
}

static int pal_click(int x, int y);
static int comp_click(int x, int y);
static void on_press(int v) {
    int x, y, b, i, bottom;
    (void)v;
    easyui.mouse(&x, &y, &b);
    calc_layout(); caretOn = 1; bottom = L.H - STATUS_H;
    if (dlgKind) {
        int r[3][4], box[4]; dlg_rects(r, box);
        for (i = 0; i < 3; i++) if (in_rect(x, y, r[i])) { dlg_choose(i); return; }
        return;
    }
    if (pal_click(x, y) || comp_click(x, y)) return;
    overlay_geom();
    if (findOpen && in_rect(x, y, fr[FG_BAR])) {
        if (in_rect(x, y, fr[FG_FIND])) field_click(&fFind, fr[FG_FIND], x);
        else if (replOpen && in_rect(x, y, fr[FG_REPL])) field_click(&fRepl, fr[FG_REPL], x);
        else if (in_rect(x, y, fr[FG_CASE])) { matchCase = !matchCase; find_changed(); }
        else if (in_rect(x, y, fr[FG_PREV])) find_step(-1, 0);
        else if (in_rect(x, y, fr[FG_NEXT])) find_step(1, 0);
        else if (in_rect(x, y, fr[FG_CLOSE])) close_overlays();
        else if (replOpen && in_rect(x, y, fr[FG_ONE])) replace_one();
        else if (replOpen && in_rect(x, y, fr[FG_ALL])) replace_all();
        return;
    }
    if (gotoOpen && in_rect(x, y, fr[FG_GOTOBAR])) { if (in_rect(x, y, fr[FG_GOTO])) field_click(&fGoto, fr[FG_GOTO], x); return; }

    if (y < TAB_H) {
        int h = hover_at(x, y);
        if (h == H_PLUS) new_tab();
        else if (h >= H_CLOSE && h < H_CLOSE + MAX_TABS) close_tab_request(h - H_CLOSE);
        else if (h >= H_TAB && h < H_TAB + MAX_TABS) tab_activate(h - H_TAB);
        focus = F_EDITOR; return;
    }
    if (y >= bottom) return;
    if (sbOn && x >= L.sbw - 3 && x <= L.sbw + 3) { dragSplit = 1; return; }
    if (sbOn && x < L.sbw) {
        i = (y - TAB_H - SB_HDR + treeScroll) / ROW_H;
        if (y >= TAB_H + SB_HDR && i >= 0 && i < tcount) {
            if (tn[i].isdir) { tree_toggle(i); tree_dirty(); }
            else { char p[PATH_MAX_]; tree_path(i, p, sizeof p); open_file(p); focus = F_EDITOR; }
        }
        return;
    }
    focus = F_EDITOR;
    if (x >= L.ex + L.ew - SCROLL_W && y < L.ey + L.tvh) {                     /* vertical scrollbar */
        int ty, th;
        if (vsb_thumb(&ty, &th)) {
            if (y >= ty && y < ty + th) { dragVsb = 1; dragOff = y - ty; }
            else { D->scrollY += (y < ty ? -1 : 1) * (L.tvh - LH); clamp_scroll(); D->targetY = D->scrollY; }
        }
        return;
    }
    if (L.needH && y >= L.ey + L.tvh) {                                         /* horizontal scrollbar */
        int tx, tw;
        if (hsb_thumb(&tx, &tw)) {
            if (x >= tx && x < tx + tw) { dragHsb = 1; dragOff = x - tx; }
            else { D->scrollX += (x < tx ? -1 : 1) * (L.tvw - CW * 4); clamp_scroll(); }
        }
        return;
    }
    if (x < L.tx) {                                                              /* gutter: select a line */
        int l = iclamp((y - L.ey + D->scrollY) / LH, 0, D->nl - 1);
        D->anchor = D->ls[l]; D->caret = l + 1 < D->nl ? D->ls[l + 1] : D->len;
        dragSel = 3; dragLo = D->anchor; dragHi = D->caret; D->merge = 0; set_want();
        if (!autoTimer) autoTimer = easyui.settimer(40, on_auto_tick);
        return;
    }
    {   /* the text */
        long now = mos_uptime(); int q = offset_at(x, y);
        if (now - lastClickT < 450 && imax(x - lastClickX, lastClickX - x) < 5 && imax(y - lastClickY, lastClickY - y) < 5) clickCount = clickCount >= 3 ? 1 : clickCount + 1;
        else clickCount = 1;
        lastClickT = now; lastClickX = x; lastClickY = y;
        D->merge = 0;
        if (clickCount == 1) {
            if (g_mods & M_SHIFT) D->caret = q; else D->caret = D->anchor = q;
            dragSel = 1;
        } else if (clickCount == 2) {
            int a = q, e = q;
            if (q < D->len && is_word((unsigned char)D->text[q])) { a = word_start(D, q); e = word_end(D, q); }
            else if (q > 0 && is_word((unsigned char)D->text[q - 1])) { a = word_start(D, q); e = word_end(D, q); }
            else { a = q; e = imin(q + 1, D->len); while (a > 0 && D->text[a - 1] == D->text[q > 0 && q == D->len ? q - 1 : q] && D->text[a - 1] == ' ') a--; }
            D->anchor = a; D->caret = e; dragSel = 2; dragLo = a; dragHi = e;
        } else {
            int l = line_of(D, q);
            D->anchor = D->ls[l]; D->caret = l + 1 < D->nl ? D->ls[l + 1] : D->len;
            dragSel = 3; dragLo = D->anchor; dragHi = D->caret;
        }
        set_want();
        if (!autoTimer) autoTimer = easyui.settimer(40, on_auto_tick);
    }
}
static void on_release(int v) { (void)v; dragSel = dragVsb = dragHsb = dragSplit = 0; }

static void on_move(int x, int y, int b) {
    int h;
    if (x < 0 || y < 0) return;
    calc_layout();
    if (dragVsb && (b & 1)) {
        int ty, th, maxy = max_scroll_y();
        if (vsb_thumb(&ty, &th) && L.tvh > th) {
            D->scrollY = iclamp((int)((long)(y - dragOff - L.ey) * maxy / (L.tvh - th)), 0, maxy); D->targetY = D->scrollY;
        }
        easyui.redraw(); return;
    }
    if (dragHsb && (b & 1)) {
        int tx, tw, maxx = max_scroll_x();
        if (hsb_thumb(&tx, &tw) && L.tvw > tw) D->scrollX = iclamp((int)((long)(x - dragOff - L.tx) * maxx / (L.tvw - tw)), 0, maxx);
        easyui.redraw(); return;
    }
    if (dragSplit && (b & 1)) { sbW = iclamp(x, SB_W_MIN, SB_W_MAX); easyui.redraw(); return; }
    if (dragSel && (b & 1)) { drag_update(x, y); easyui.redraw(); return; }
    h = hover_at(x, y);
    if (h != hoverId) { hoverId = h; easyui.redraw(); }
}

static int pal_wheel(int v);
static int compOpen;
static void on_wheel(int v) {
    int x, y, b;
    easyui.mouse(&x, &y, &b);
    calc_layout();
    if (dlgKind) return;
    if (pal_wheel(v)) return;
    compOpen = 0;
    if (g_mods & M_CTRL) {                                    /* Ctrl+wheel: font size */
        int z = iclamp(zoomIdx + (v > 0 ? 1 : -1), 0, 2);
        if (z != zoomIdx) { zoomIdx = z; apply_font(); reveal(); }
        return;
    }
    if (sbOn && x < L.sbw) { treeScroll -= v * ROW_H * 3; if (treeScroll < 0) treeScroll = 0; return; }
    if (g_mods & M_SHIFT) { D->scrollX -= v * CW * 8; clamp_scroll(); return; }
    D->targetY = iclamp(D->targetY - v * LH * 3, 0, max_scroll_y());
    start_scroll_anim();
}

/* ===========================================================================
 *  Word completion, quick open, command palette, session
 * =========================================================================== */

/* ---- word completion (Ctrl+Space, or by itself after 3 letters) ----
 * Candidates are the words of the open documents that start with the word
 * before the caret; Up/Down choose, Tab or Enter take one, Esc closes. */
#define COMP_MAX 64
#define COMP_SHOW 8
static int compOpen, compSel, compScroll, compN, compStart;   /* compStart: offset where the prefix starts */
static char compWord[COMP_MAX][48];
static int trimOnSave;                                        /* palette: trim trailing spaces on save */

static int comp_have(const char* w, int n) {
    int i;
    for (i = 0; i < compN; i++) if ((int)strlen(compWord[i]) == n && !memcmp(compWord[i], w, (size_t)n)) return 1;
    return 0;
}
static void comp_collect(const Doc* d, const char* pre, int pl, int skipAt) {
    int p = 0;
    while (p < d->len && compN < COMP_MAX) {
        int s, e;
        while (p < d->len && !is_word((unsigned char)d->text[p])) p++;
        s = p;
        while (p < d->len && is_word((unsigned char)d->text[p])) p++;
        e = p;
        if (s == skipAt || e - s <= pl || e - s >= 48) continue;
        if (d->text[s] >= '0' && d->text[s] <= '9') continue;
        if (memcmp(d->text + s, pre, (size_t)pl) != 0) continue;
        if (comp_have(d->text + s, e - s)) continue;
        memcpy(compWord[compN], d->text + s, (size_t)(e - s));
        compWord[compN][e - s] = 0;
        compN++;
    }
}
static void comp_sort(void) {                                 /* shorter first, then alphabetical */
    int i, j;
    for (i = 1; i < compN; i++) {
        char t[48]; int lt;
        strlcpy_safe(t, compWord[i], sizeof t); lt = (int)strlen(t);
        for (j = i - 1; j >= 0; j--) {
            int lj = (int)strlen(compWord[j]);
            if (lj < lt || (lj == lt && strcmp(compWord[j], t) <= 0)) break;
            memcpy(compWord[j + 1], compWord[j], sizeof compWord[0]);
        }
        strlcpy_safe(compWord[j + 1], t, sizeof compWord[0]);
    }
}
/* Recomputes the list for the word before the caret; closes it when there is none. */
static void comp_update(int minLen) {
    Doc* d = D; char pre[48]; int s = word_start(d, d->caret), pl = d->caret - s, i;
    compN = 0;
    if (has_sel(d) || pl < minLen || pl >= 47 || (d->caret < d->len && is_word((unsigned char)d->text[d->caret]))) { compOpen = 0; return; }
    memcpy(pre, d->text + s, (size_t)pl); pre[pl] = 0;
    comp_collect(d, pre, pl, s);
    for (i = 0; i < ntabs && compN < COMP_MAX; i++) if (tabs[i] != d) comp_collect(tabs[i], pre, pl, -1);
    if (!compN) { compOpen = 0; return; }
    comp_sort();
    compStart = s; compOpen = 1; compSel = 0; compScroll = 0;
}
static void comp_accept(void) {
    Doc* d = D; const char* w = compWord[compSel]; int pl = d->caret - compStart, n = (int)strlen(w);
    compOpen = 0;
    if (pl < 0 || pl > n) return;
    d->merge = 0;
    insert_text(w + pl, n - pl, 0);
}
/* Keys while the list is open: 1 if the key was used. */
static int comp_key(int key) {
    if (!compOpen) return 0;
    if (key == K_DOWN) { compSel = (compSel + 1) % compN; }
    else if (key == K_UP) { compSel = (compSel + compN - 1) % compN; }
    else if (key == K_TAB || key == K_ENTER || key == K_KPENTER) { comp_accept(); return 1; }
    else if (key == K_ESC) { compOpen = 0; return 1; }
    else return 0;
    if (compSel < compScroll) compScroll = compSel;
    if (compSel >= compScroll + COMP_SHOW) compScroll = compSel - COMP_SHOW + 1;
    return 1;
}
static void comp_rect(int r[4]) {
    Doc* d = D; int l = line_of(d, compStart), w = 0, i, rows = imin(compN, COMP_SHOW);
    for (i = 0; i < compN; i++) w = imax(w, (int)strlen(compWord[i]));
    r[2] = imax(160, w * CW + 24); r[3] = rows * LH + 6;
    r[0] = L.tx + 6 - d->scrollX + cell_of(d, l, compStart - d->ls[l]) * CW - 6;
    r[1] = L.ey + (l + 1) * LH - d->scrollY + 2;
    if (r[1] + r[3] > L.ey + L.tvh) r[1] = L.ey + l * LH - d->scrollY - r[3] - 2;   /* above the line when there is no room */
    if (r[0] + r[2] > L.ex + L.ew - SCROLL_W) r[0] = L.ex + L.ew - SCROLL_W - r[2];
    if (r[0] < L.tx) r[0] = L.tx;
}
static void draw_completion(void) {
    int r[4], i, rows, pl;
    if (!compOpen || focus != F_EDITOR) return;
    comp_rect(r); rows = imin(compN, COMP_SHOW); pl = D->caret - compStart;
    R(r[0] + 2, r[1] + 3, r[2], r[3], 0x50000000);
    R(r[0], r[1], r[2], r[3], C_PANEL);
    easyui.drawrect(r[0], r[1], r[2], r[3], 0xFF3B3D43);
    for (i = 0; i < rows; i++) {
        int k = compScroll + i, y = r[1] + 3 + i * LH;
        char head[48];
        if (k == compSel) R(r[0] + 2, y, r[2] - 4, LH, with_alpha(g_accent, 0x60));
        strlcpy_safe(head, compWord[k], sizeof head); head[imin(pl, 47)] = 0;
        code_text(r[0] + 10, y + text_off(), compWord[k], C_TEXT);
        code_text(r[0] + 10, y + text_off(), head, g_accent);             /* the typed part in the accent colour */
    }
    if (compN > COMP_SHOW) {
        int th = imax(10, r[3] * COMP_SHOW / compN), ty = r[1] + (r[3] - th) * compScroll / (compN - COMP_SHOW);
        R(r[0] + r[2] - 4, ty, 2, th, 0xFF5A5E66);
    }
}
static int comp_click(int x, int y) {
    int r[4];
    if (!compOpen) return 0;
    comp_rect(r);
    if (!in_rect(x, y, r)) { compOpen = 0; return 0; }
    compSel = iclamp(compScroll + (y - r[1] - 3) / LH, 0, compN - 1);
    comp_accept();
    return 1;
}

/* ---- the word under the caret, highlighted where else it appears ---- */
static int occ_word(const Doc* d, int* s, int* n) {
    int a, b;
    if (has_sel(d)) {                                            /* a selected whole word */
        a = sel_a(d); b = sel_b(d);
        if (b - a < 2 || b - a > 64 || word_start(d, a) != a || word_end(d, a) != b) return 0;
    } else {
        a = word_start(d, d->caret); b = word_end(d, d->caret);
        if (b - a < 2 || b - a > 64) return 0;
    }
    if (d->text[a] >= '0' && d->text[a] <= '9') return 0;
    *s = a; *n = b - a;
    return 1;
}
static void draw_occurrences(int line, int y, int x0) {
    static int ws = -1, wn, wver = -1; static Doc* wdoc; static int wcaret = -1;
    Doc* d = D; int a = d->ls[line], e = line_end_(d, line), p;
    if (findOpen) return;
    if (wdoc != d || wver != d->ver || wcaret != d->caret) {
        wdoc = d; wver = d->ver; wcaret = d->caret;
        if (!occ_word(d, &ws, &wn)) ws = -1;
    }
    if (ws < 0) return;
    for (p = a; p + wn <= e; p++) {
        int x1, x2;
        if (memcmp(d->text + p, d->text + ws, (size_t)wn) != 0) continue;
        if ((p > a && is_word((unsigned char)d->text[p - 1])) || (p + wn < d->len && is_word((unsigned char)d->text[p + wn]))) continue;
        x1 = cell_of(d, line, p - a); x2 = cell_of(d, line, p - a + wn);
        R(x0 + x1 * CW, y + 1, (x2 - x1) * CW, LH - 2, p == ws ? 0x28FFFFFF : 0x30FFFFFF);
        easyui.drawrect(x0 + x1 * CW, y + 1, (x2 - x1) * CW, LH - 2, 0x40FFFFFF);
        p += wn - 1;
    }
}

/* ---- quick open (Ctrl+P) and the command palette (Ctrl+Shift+P) ----
 * One list overlay: a filter field and the items that match it (letters in
 * order, closer together and at word starts first). */
#define QO_FILES 3000
#define PAL_SHOW 12
enum { PAL_NONE, PAL_FILES, PAL_COMMANDS };
static int palMode, palSel, palScroll;
static Field fPal;
static char (*qoPath)[PATH_MAX_]; static int qoCount;           /* files under the sidebar folder */
static int palItems[QO_FILES], palScores[QO_FILES], palN;

typedef struct { const char* name; const char* keys; int id; } Cmd;
enum { CMD_NEW = 1, CMD_OPEN, CMD_OPEN_FOLDER, CMD_SAVE, CMD_SAVE_AS, CMD_SAVE_ALL, CMD_CLOSE, CMD_FIND, CMD_REPLACE,
       CMD_GOTO, CMD_SIDEBAR, CMD_ZOOM_IN, CMD_ZOOM_OUT, CMD_ZOOM_RESET, CMD_COMMENT, CMD_DUPLICATE, CMD_DELETE_LINE,
       CMD_UPPER, CMD_LOWER, CMD_TRIM, CMD_TRIM_TOGGLE, CMD_SORT, CMD_CRLF, CMD_REFRESH, CMD_QUICK_OPEN, CMD_COMPLETE };
static const Cmd k_cmds[] = {
    { "New file", "Ctrl+N", CMD_NEW }, { "Open file...", "Ctrl+O", CMD_OPEN },
    { "Open folder...", "Ctrl+Shift+O", CMD_OPEN_FOLDER }, { "Go to file...", "Ctrl+P", CMD_QUICK_OPEN },
    { "Save", "Ctrl+S", CMD_SAVE }, { "Save as...", "Ctrl+Shift+S", CMD_SAVE_AS }, { "Save all", "", CMD_SAVE_ALL },
    { "Close tab", "Ctrl+W", CMD_CLOSE }, { "Find", "Ctrl+F", CMD_FIND }, { "Replace", "Ctrl+H", CMD_REPLACE },
    { "Go to line...", "Ctrl+G", CMD_GOTO }, { "Toggle sidebar", "Ctrl+B", CMD_SIDEBAR },
    { "Complete word", "Ctrl+Space", CMD_COMPLETE },
    { "Zoom in", "Ctrl+=", CMD_ZOOM_IN }, { "Zoom out", "Ctrl+-", CMD_ZOOM_OUT }, { "Reset zoom", "Ctrl+0", CMD_ZOOM_RESET },
    { "Toggle comment", "Ctrl+/", CMD_COMMENT }, { "Duplicate line", "Ctrl+D", CMD_DUPLICATE },
    { "Delete line", "Ctrl+Shift+K", CMD_DELETE_LINE }, { "Selection to upper case", "", CMD_UPPER },
    { "Selection to lower case", "", CMD_LOWER }, { "Sort selected lines", "", CMD_SORT },
    { "Trim trailing whitespace", "", CMD_TRIM }, { "Trim trailing whitespace on save: on/off", "", CMD_TRIM_TOGGLE },
    { "Line endings: LF / CRLF", "", CMD_CRLF }, { "Refresh folder", "F5", CMD_REFRESH },
};
#define NCMDS ((int)(sizeof k_cmds / sizeof k_cmds[0]))

/* Fuzzy score of `s` for the filter, -1 when the letters are not all there in order. */
static int fuzzy(const char* s, const char* q) {
    int score = 0, last = -2, i = 0, j;
    if (!*q) return 0;
    for (j = 0; q[j]; j++) {
        int c = fold((unsigned char)q[j]);
        if (c == ' ') continue;
        while (s[i] && fold((unsigned char)s[i]) != c) i++;
        if (!s[i]) return -1;
        score += 10;
        if (i == last + 1) score += 15;                                       /* next to the previous letter */
        if (i == 0 || s[i - 1] == '/' || s[i - 1] == '_' || s[i - 1] == ' ' || s[i - 1] == '.' || s[i - 1] == '-') score += 20;
        last = i++;
    }
    return score - (int)strlen(s) / 4;
}
static void qo_walk(const char* dir, int depth) {
    syscall_dirent_t* ents; long n, i;
    if (depth > 8 || qoCount >= QO_FILES) return;
    ents = (syscall_dirent_t*)malloc(sizeof(syscall_dirent_t) * DIR_MAX);
    if (!ents) return;
    n = mos_listdir(dir, ents, DIR_MAX);
    if (n > DIR_MAX) n = DIR_MAX;
    for (i = 0; i < n && qoCount < QO_FILES; i++) {
        char p[PATH_MAX_];
        if (ents[i].hidden || !ents[i].name[0]) continue;
        ents[i].name[SYSCALL_DIRENT_NAME_SIZE - 1] = 0;
        strlcpy_safe(p, dir, sizeof p);
        if (p[strlen(p) - 1] != '/') cat(p, sizeof p, "/");
        cat(p, sizeof p, ents[i].name);
        if (ents[i].type == SYSCALL_DIRENT_TYPE_DIR) qo_walk(p, depth + 1);
        else strlcpy_safe(qoPath[qoCount++], p, PATH_MAX_);
    }
    free(ents);
}
static const char* pal_text(int item) {                                    /* what an item is matched and shown by */
    if (palMode == PAL_COMMANDS) return k_cmds[item].name;
    {
        const char* p = qoPath[item]; size_t n = strlen(treeRoot);
        if (!strncmp(p, treeRoot, n)) { p += n; if (*p == '/') p++; }
        return p;
    }
}
static void pal_filter(void) {
    int total = palMode == PAL_COMMANDS ? NCMDS : qoCount, i, j;
    palN = 0;
    for (i = 0; i < total && palN < QO_FILES; i++) {
        int s = fuzzy(pal_text(i), fPal.buf);
        if (s < 0) continue;
        if (palMode == PAL_FILES && fPal.len) s += fuzzy(base_name(qoPath[i]), fPal.buf) > 0 ? 40 : 0;   /* the file name itself */
        j = palN++;
        while (j > 0 && (fPal.len ? palScores[j - 1] < s : 0)) { palItems[j] = palItems[j - 1]; palScores[j] = palScores[j - 1]; j--; }
        palItems[j] = i; palScores[j] = s;
    }
    palSel = 0; palScroll = 0;
}
static void pal_open(int mode) {
    palMode = mode; field_set(&fPal, ""); compOpen = 0; focus = F_PAL;
    if (mode == PAL_FILES) {
        if (!qoPath) qoPath = (char(*)[PATH_MAX_])malloc((size_t)QO_FILES * PATH_MAX_);
        if (!qoPath) { palMode = PAL_NONE; set_msg("Out of memory", 1); return; }
        qoCount = 0;
        qo_walk(treeRoot, 0);
    }
    pal_filter();
}
static void pal_rect(int r[4]) {
    r[2] = imin(620, L.W - 40); r[0] = (L.W - r[2]) / 2; r[1] = TAB_H + 8;
    r[3] = 44 + imax(1, imin(palN, PAL_SHOW)) * ROW_H + 8;
}

static void change_case(int upper) {
    Doc* d = D; int a = sel_a(d), b = sel_b(d), i; char* t;
    if (a == b) { set_msg("Select some text first", 1); return; }
    t = (char*)malloc((size_t)(b - a));
    if (!t) return;
    for (i = 0; i < b - a; i++) {
        char c = d->text[a + i];
        if (upper && c >= 'a' && c <= 'z') c = (char)(c - 32);
        if (!upper && c >= 'A' && c <= 'Z') c = (char)(c + 32);
        t[i] = c;
    }
    doc_edit(d, a, b - a, t, b - a, d->caret, d->anchor, 0);
    free(t);
}
/* Removes spaces and tabs at line ends; returns how many bytes went. */
static int trim_trailing(Doc* d) {
    int l, removed = 0, caret = d->caret, anchor = d->anchor;
    for (l = d->nl - 1; l >= 0; l--) {
        int a = d->ls[l], e = line_end_(d, l), s = e;
        while (s > a && (d->text[s - 1] == ' ' || d->text[s - 1] == '\t')) s--;
        if (s == e) continue;
        if (caret > s) caret = caret >= e ? caret - (e - s) : s;
        if (anchor > s) anchor = anchor >= e ? anchor - (e - s) : s;
        doc_edit(d, s, e - s, "", 0, caret, anchor, 0);
        removed += e - s;
    }
    return removed;
}
static int cmp_lines(const char* a, int an, const char* b, int bn) {
    int i, n = imin(an, bn);
    for (i = 0; i < n; i++) if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i];
    return an - bn;
}
static void sort_lines(void) {
    Doc* d = D; int l0 = line_of(d, sel_a(d)), l1 = line_of(d, sel_b(d)), n, i, j, a, e, o = 0;
    int* st; int* ln; char* out;
    if (has_sel(d) && sel_b(d) == d->ls[l1] && l1 > l0) l1--;
    n = l1 - l0 + 1;
    if (n < 2) { set_msg("Select the lines to sort", 1); return; }
    a = d->ls[l0]; e = line_end_(d, l1);
    st = (int*)malloc(sizeof(int) * (size_t)n); ln = (int*)malloc(sizeof(int) * (size_t)n); out = (char*)malloc((size_t)(e - a + 1));
    if (!st || !ln || !out) { free(st); free(ln); free(out); return; }
    for (i = 0; i < n; i++) { st[i] = d->ls[l0 + i]; ln[i] = line_end_(d, l0 + i) - st[i]; }
    for (i = 1; i < n; i++) {
        int s = st[i], k = ln[i];
        for (j = i - 1; j >= 0 && cmp_lines(d->text + st[j], ln[j], d->text + s, k) > 0; j--) { st[j + 1] = st[j]; ln[j + 1] = ln[j]; }
        st[j + 1] = s; ln[j + 1] = k;
    }
    for (i = 0; i < n; i++) { memcpy(out + o, d->text + st[i], (size_t)ln[i]); o += ln[i]; if (i + 1 < n) out[o++] = '\n'; }
    doc_edit(d, a, e - a, out, o, a + o, a, 0);
    free(st); free(ln); free(out);
}

static void run_command(int id) {
    int i, n;
    switch (id) {
        case CMD_NEW: new_tab(); break;
        case CMD_OPEN: easyui.pickfile("Open file", dialog_folder(), 0, on_open_done); break;
        case CMD_OPEN_FOLDER: easyui.pickfolder("Open folder", dialog_folder(), on_folder_done); break;
        case CMD_QUICK_OPEN: pal_open(PAL_FILES); return;
        case CMD_SAVE: save_doc(D); break;
        case CMD_SAVE_AS: start_save_as(D); break;
        case CMD_SAVE_ALL:
            for (i = n = 0; i < ntabs; i++) if (tabs[i]->modified && !tabs[i]->untitled && do_save_to(tabs[i], tabs[i]->path)) n++;
            { char m[48] = "Saved "; cat_num(m, sizeof m, n); cat(m, sizeof m, n == 1 ? " file" : " files"); set_msg(m, 0); }
            break;
        case CMD_CLOSE: close_tab_request(cur); break;
        case CMD_FIND: open_find(0); break;
        case CMD_REPLACE: open_find(1); break;
        case CMD_GOTO: open_goto(); break;
        case CMD_SIDEBAR: sbOn = !sbOn; break;
        case CMD_COMPLETE: comp_update(1); if (!compOpen) set_msg("No completions", 0); break;
        case CMD_ZOOM_IN: if (zoomIdx < 2) { zoomIdx++; apply_font(); reveal(); } break;
        case CMD_ZOOM_OUT: if (zoomIdx > 0) { zoomIdx--; apply_font(); reveal(); } break;
        case CMD_ZOOM_RESET: zoomIdx = 1; apply_font(); reveal(); break;
        case CMD_COMMENT: block_edit(BLK_COMMENT); break;
        case CMD_DUPLICATE: duplicate_lines(); break;
        case CMD_DELETE_LINE: delete_lines(); break;
        case CMD_UPPER: change_case(1); break;
        case CMD_LOWER: change_case(0); break;
        case CMD_SORT: sort_lines(); break;
        case CMD_TRIM: {
            char m[48] = "Removed "; cat_num(m, sizeof m, trim_trailing(D)); cat(m, sizeof m, " trailing spaces"); set_msg(m, 0);
            break;
        }
        case CMD_TRIM_TOGGLE: trimOnSave = !trimOnSave; set_msg(trimOnSave ? "Trailing whitespace is trimmed on save" : "Trailing whitespace is kept on save", 0); break;
        case CMD_CRLF: D->crlf = !D->crlf; D->modified = 1; set_msg(D->crlf ? "Line endings: CRLF (saved on the next save)" : "Line endings: LF (saved on the next save)", 0); break;
        case CMD_REFRESH: tree_refresh(); set_msg("Folder refreshed", 0); break;
        default: break;
    }
    after_edit();
}
static void pal_choose(void) {
    int mode = palMode, item;
    if (palSel < 0 || palSel >= palN) { palMode = PAL_NONE; return; }
    item = palItems[palSel];
    palMode = PAL_NONE;
    focus = F_EDITOR;
    if (mode == PAL_FILES) open_file(qoPath[item]);
    else run_command(k_cmds[item].id);
}
static void pal_key(int key, int mods) {
    if (key == K_ESC) { palMode = PAL_NONE; return; }
    if (key == K_ENTER || key == K_KPENTER) { pal_choose(); return; }
    if (key == K_DOWN || key == K_UP || key == K_PGDN || key == K_PGUP) {
        int step = key == K_DOWN ? 1 : key == K_UP ? -1 : key == K_PGDN ? PAL_SHOW : -PAL_SHOW;
        if (palN) palSel = iclamp(palSel + step, 0, palN - 1);
        if (palSel < palScroll) palScroll = palSel;
        if (palSel >= palScroll + PAL_SHOW) palScroll = palSel - PAL_SHOW + 1;
        return;
    }
    if (field_key(&fPal, key, mods)) pal_filter();
}
static void draw_palette(void) {
    int r[4], i, fr2[4], rows;
    if (palMode == PAL_NONE) return;
    pal_rect(r); rows = imin(palN, PAL_SHOW);
    R(0, 0, L.W, L.H, 0x30000000);
    R(r[0] + 3, r[1] + 4, r[2], r[3], 0x60000000);
    R(r[0], r[1], r[2], r[3], C_PANEL);
    easyui.drawrect(r[0], r[1], r[2], r[3], 0xFF3B3D43);
    fr2[0] = r[0] + 8; fr2[1] = r[1] + 8; fr2[2] = r[2] - 16; fr2[3] = 28;
    draw_field(fr2, &fPal, F_PAL, palMode == PAL_FILES ? "Go to file (type part of its name)" : "Run a command");
    if (!palN) T(r[0] + 16, r[1] + 48, palMode == PAL_FILES && !qoCount ? "No files in this folder" : "Nothing matches", C_MUTED);
    for (i = 0; i < rows; i++) {
        int k = palScroll + i, item = palItems[k], y = r[1] + 44 + i * ROW_H;
        char t[200];
        if (k == palSel) R(r[0] + 4, y, r[2] - 8, ROW_H, with_alpha(g_accent, 0x60));
        if (palMode == PAL_FILES) {
            const char* rel = pal_text(item); const char* bn = base_name(qoPath[item]);
            int bw;
            fit(t, sizeof t, bn, r[2] / 2 - 24, 0);
            T(r[0] + 14, y + (ROW_H - UI_H) / 2, t, C_TEXT);
            bw = TW(t);
            fit(t, sizeof t, rel, r[2] - bw - 48, 1);
            T(r[0] + 26 + bw, y + (ROW_H - UI_H) / 2, t, C_MUTED);
        } else {
            T(r[0] + 14, y + (ROW_H - UI_H) / 2, k_cmds[item].name, C_TEXT);
            if (k_cmds[item].keys[0]) T(r[0] + r[2] - 14 - TW(k_cmds[item].keys), y + (ROW_H - UI_H) / 2, k_cmds[item].keys, C_MUTED);
        }
    }
    if (palN > PAL_SHOW) {
        int track = PAL_SHOW * ROW_H, th = imax(16, track * PAL_SHOW / palN), ty = r[1] + 44 + (track - th) * palScroll / (palN - PAL_SHOW);
        R(r[0] + r[2] - 6, ty, 3, th, 0xFF5A5E66);
    }
}
static int pal_click(int x, int y) {
    int r[4];
    if (palMode == PAL_NONE) return 0;
    pal_rect(r);
    if (!in_rect(x, y, r)) { palMode = PAL_NONE; focus = F_EDITOR; return 1; }
    if (y >= r[1] + 44) {
        int k = palScroll + (y - r[1] - 44) / ROW_H;
        if (k >= 0 && k < palN && k < palScroll + PAL_SHOW) { palSel = k; pal_choose(); }
    }
    return 1;
}
static int pal_wheel(int v) {
    if (palMode == PAL_NONE) return 0;
    palScroll = iclamp(palScroll - v * 3, 0, imax(0, palN - PAL_SHOW));
    return 1;
}

/* ---- session: the open files and the folder, reopened next time ---- */
static void session_path(char* out, int cap) {
#ifdef CODE_HAS_MOSDESK
    mosdesk_settings_t s = mosdesk_settings_load();
    char home[PATH_MAX_];
    if (mosdesk_user_path(home, sizeof home, s.username, NULL) && mos_is_dir(home) == 1) {
        strlcpy_safe(out, home, (size_t)cap);
        if (out[strlen(out) - 1] != '/') cat(out, cap, "/");
        cat(out, cap, ".code_session");
        return;
    }
#endif
    strlcpy_safe(out, "0:/.code_session", (size_t)cap);
}
static void session_save(void) {
    char p[PATH_MAX_], buf[MAX_TABS * (PATH_MAX_ + 2) + PATH_MAX_ + 64]; int i, o = 0; long fd;
    buf[0] = 0;
    cat(buf, sizeof buf, "root "); cat(buf, sizeof buf, treeRoot); cat(buf, sizeof buf, "\n");
    cat(buf, sizeof buf, "current "); cat_num(buf, sizeof buf, cur); cat(buf, sizeof buf, "\n");
    if (trimOnSave) cat(buf, sizeof buf, "trim 1\n");
    for (i = 0; i < ntabs; i++)
        if (!tabs[i]->untitled && tabs[i]->path[0]) { cat(buf, sizeof buf, "file "); cat(buf, sizeof buf, tabs[i]->path); cat(buf, sizeof buf, "\n"); }
    o = (int)strlen(buf);
    session_path(p, sizeof p);
    if (mos_exists(p) == 1) mos_delete(p);
    if (mos_create(p, "text", "txt") < 0) return;
    fd = mos_open(p, SYS_O_RDWR);
    if (fd <= 0) return;
    mos_fwrite(fd, buf, (size_t)o);
    mos_close(fd);
}
/* Reopens the files of the last session; returns 1 if it did. */
static int session_restore(char* root, int rootCap) {
    char p[PATH_MAX_], buf[MAX_TABS * (PATH_MAX_ + 2) + PATH_MAX_ + 64]; long fd, n = 0, got; int want = 0, opened = 0;
    char* line;
    session_path(p, sizeof p);
    if (mos_exists(p) != 1) return 0;
    fd = mos_open(p, SYS_O_RDONLY);
    if (fd <= 0) return 0;
    while (n < (long)sizeof buf - 1 && (got = mos_read(fd, buf + n, sizeof buf - 1 - (size_t)n)) > 0) n += got;
    mos_close(fd);
    buf[n] = 0;
    line = buf;
    while (*line) {
        char* e = line; while (*e && *e != '\n') e++;
        if (*e) *e++ = 0;
        if (!strncmp(line, "root ", 5) && mos_is_dir(line + 5) == 1) strlcpy_safe(root, line + 5, (size_t)rootCap);
        else if (!strncmp(line, "current ", 8)) want = atoi(line + 8);
        else if (!strncmp(line, "trim 1", 6)) trimOnSave = 1;
        else if (!strncmp(line, "file ", 5) && mos_exists(line + 5) == 1 && open_file(line + 5)) opened++;
        line = e;
    }
    if (opened) cur = iclamp(want, 0, ntabs - 1);
    return opened > 0;
}

/* ---- keys ---- */
static void editor_key(int key, int mods) {
    int shift = (mods & M_SHIFT) != 0, ctrl = (mods & M_CTRL) != 0, alt = (mods & M_ALT) != 0;
    Doc* d = D; int l = line_of(d, d->caret), vis, c;
    calc_layout();
    vis = imax(1, L.tvh / LH);
    if (alt && !ctrl) {
        if (key == K_UP || key == K_DOWN) { if (shift) duplicate_lines(); else move_lines(key == K_UP ? -1 : 1); }
        return;
    }
    if (ctrl) {
        switch (key) {
            case K_Z: if (shift) doc_redo(d); else doc_undo(d); set_want(); reveal(); break;
            case K_Y: doc_redo(d); set_want(); reveal(); break;
            case K_A: d->anchor = 0; d->caret = d->len; d->merge = 0; break;
            case K_C: do_copy(); break;
            case K_X: do_cut(); break;
            case K_V: do_paste(); break;
            case K_D: duplicate_lines(); break;
            case K_K: if (shift) delete_lines(); break;
            case K_L: select_line(); break;
            case K_SLASH: case 84: block_edit(BLK_COMMENT); break;
            case K_RBRACKET: block_edit(BLK_INDENT); break;
            case K_LBRACKET: block_edit(BLK_UNINDENT); break;
            case K_HOME: move_to(0, 0, shift); break;
            case K_END: move_to(d->len, 0, shift); break;
            case K_LEFT: move_to(word_left(d->caret), 0, shift); break;
            case K_RIGHT: move_to(word_right(d->caret), 0, shift); break;
            case K_BACKSPACE: delete_word_left(); break;
            case K_DELETE: delete_word_right(); break;
            case K_ENTER: case K_KPENTER: if (shift) do_enter(0, 1); else do_enter(1, 0); break;
            case K_UP: d->scrollY = iclamp(d->scrollY - LH, 0, max_scroll_y()); d->targetY = d->scrollY; break;
            case K_DOWN: d->scrollY = iclamp(d->scrollY + LH, 0, max_scroll_y()); d->targetY = d->scrollY; break;
            default: break;
        }
        return;
    }
    switch (key) {
        case K_LEFT:  if (!shift && has_sel(d)) move_to(sel_a(d), 0, 0); else if (d->caret > 0) move_to(d->caret - 1, 0, shift); break;
        case K_RIGHT: if (!shift && has_sel(d)) move_to(sel_b(d), 0, 0); else if (d->caret < d->len) move_to(d->caret + 1, 0, shift); break;
        case K_UP:    move_vert(-1, shift); break;
        case K_DOWN:  move_vert(1, shift); break;
        case K_PGUP:  d->scrollY = iclamp(d->scrollY - (vis - 1) * LH, 0, max_scroll_y()); d->targetY = d->scrollY; move_vert(-(vis - 1), shift); break;
        case K_PGDN:  d->scrollY = iclamp(d->scrollY + (vis - 1) * LH, 0, max_scroll_y()); d->targetY = d->scrollY; move_vert(vis - 1, shift); break;
        case K_HOME: {
            int f = first_nonspace(d, l);
            move_to(d->caret == f ? d->ls[l] : f, 0, shift);
            break;
        }
        case K_END:   move_to(line_end_(d, l), 0, shift); break;
        case K_BACKSPACE: do_backspace(); break;
        case K_DELETE:    do_delete(); break;
        case K_ENTER: case K_KPENTER: do_enter(0, 0); break;
        case K_TAB:   do_tab(shift); break;
        case K_ESC:   if (has_sel(d)) { d->anchor = d->caret; } break;
        default:
            c = key_char(key, mods);
            if (c) type_char(c);
            break;
    }
}

static void dlg_key(int key, int mods) {
    (void)mods;
    if (key == K_ESC) dlg_choose(2);
    else if (key == K_ENTER || key == K_KPENTER) dlg_choose(dlgSel);
    else if (key == K_S) dlg_choose(0);
    else if (key == K_N || key == K_D) dlg_choose(1);
    else if (key == K_LEFT || (key == K_TAB && (mods & M_SHIFT))) dlgSel = (dlgSel + 2) % 3;
    else if (key == K_RIGHT || key == K_TAB) dlgSel = (dlgSel + 1) % 3;
}

static void field_focus_key(int key, int mods) {
    Field* f = focus == F_FIND ? &fFind : focus == F_REPL ? &fRepl : &fGoto;
    int shift = (mods & M_SHIFT) != 0, ctrl = (mods & M_CTRL) != 0;
    if (key == K_ESC) { close_overlays(); return; }
    if (key == K_TAB) {
        if (focus == F_FIND && replOpen) focus = F_REPL; else if (focus == F_REPL) focus = F_FIND;
        return;
    }
    if (key == K_ENTER || key == K_KPENTER) {
        if (focus == F_FIND) find_step(shift ? -1 : 1, 0);
        else if (focus == F_REPL) { if (ctrl) replace_all(); else replace_one(); }
        else do_goto();
        return;
    }
    if (field_key(f, key, mods) && focus == F_FIND) find_changed();
}

static void on_key(int value) {
    int key = value & 0xFF, mods = (value >> 8) & 15;
    int shift = (mods & M_SHIFT) != 0, ctrl = (mods & M_CTRL) != 0;
    if (key >= 0xE0) return;                                 /* a modifier key on its own */
    caretOn = 1;
    if (dlgKind) { dlg_key(key, mods); return; }
    if (palMode != PAL_NONE) { pal_key(key, mods); return; }
    if (ctrl && !(mods & M_ALT)) {
        if (key == K_P && !g_repeat) { pal_open(shift ? PAL_COMMANDS : PAL_FILES); return; }
        if (key == K_SPACE) { focus = F_EDITOR; comp_update(1); if (!compOpen) set_msg("No completions", 0); return; }
        if (g_repeat && (key == K_W || key == K_N || key == K_O || key == K_S || key == K_B || key == K_F || key == K_H || key == K_G)) return;
        switch (key) {
            case K_TAB: next_tab(shift ? -1 : 1); return;
            case K_PGDN: next_tab(1); return;
            case K_PGUP: next_tab(-1); return;
            case K_W: close_tab_request(cur); return;
            case K_N: new_tab(); return;
            case K_O:
                if (shift) easyui.pickfolder("Open folder", dialog_folder(), on_folder_done);
                else easyui.pickfile("Open file", dialog_folder(), 0, on_open_done);
                return;
            case K_S: if (shift) start_save_as(D); else save_doc(D); return;
            case K_B: sbOn = !sbOn; return;
            case K_F: open_find(0); return;
            case K_H: open_find(1); return;
            case K_G: open_goto(); return;
            case K_EQUAL: case K_KPPLUS: if (zoomIdx < 2) { zoomIdx++; apply_font(); reveal(); } return;
            case K_MINUS: case K_KPMINUS: if (zoomIdx > 0) { zoomIdx--; apply_font(); reveal(); } return;
            case K_0: zoomIdx = 1; apply_font(); reveal(); return;
            default: break;
        }
    }
    if (key == K_F3) { if (!findOpen) open_find(0); find_step(shift ? -1 : 1, 0); return; }
    if (key == K_F5) { tree_refresh(); set_msg("Folder refreshed", 0); return; }
    if (key == K_ESC && (findOpen || gotoOpen)) { close_overlays(); return; }
    if (focus != F_EDITOR && ((focus == F_GOTO && gotoOpen) || (findOpen && (focus == F_FIND || focus == F_REPL)))) { field_focus_key(key, mods); return; }
    focus = F_EDITOR;
    if (comp_key(key)) return;
    editor_key(key, mods);
    /* The completion list follows the word being typed. */
    if (!ctrl && !(mods & M_ALT)) {
        int c = key_char(key, mods);
        if (compOpen && (key == K_BACKSPACE || (c && is_word(c)))) comp_update(1);
        else if (c && is_word(c) && !g_repeat) comp_update(3);
        else compOpen = 0;
    } else {
        compOpen = 0;
    }
}

static int tab_hook(int value) {
    if (eui_msg_active || eui_pick_active) return 0;
    on_key(value);
    easyui.redraw();
    return 1;
}

static void on_blink(int t) { (void)t; caretOn = !caretOn; }
static void on_resize(int w) {
    (void)w;
    easyui.setsize(canvas, easyui.width(), easyui.height());
    calc_layout(); clamp_scroll();
}
static void on_close(int v) { (void)v; if (dlgKind) return; continue_quit(); }

/* ===========================================================================
 *  main
 * =========================================================================== */
int main(int argc, char** argv) {
    int i, haveRoot = 0; char root[PATH_MAX_];
    matches = (int*)malloc(sizeof(int) * MAX_MATCHES);
    tn = (TNode*)malloc(sizeof(TNode) * TREE_MAX);
    if (!matches || !tn) { mos_puts("code: out of memory\n"); return 1; }
    if (!easyui.init("Code", 1040, 660)) { mos_puts("code: start MOSdesk first\n"); return 1; }
    easyui.setdarkmode(1);
    g_accent = easyui.accent();

    canvas = easyui.createcanvas(0, 0, easyui.width(), easyui.height());
    easyui.recallfunc(canvas, ONDRAW, on_canvas_draw);
    easyui.recallfunc(canvas, ONPRESS, on_press);
    easyui.recallfunc(canvas, ONRELEASE, on_release);
    easyui.recallfunc(canvas, ONWHEEL, on_wheel);
    easyui.recallfunc(EASYUI_WINDOW, ONKEY, on_key);
    easyui.recallfunc(EASYUI_WINDOW, ONRESIZE, on_resize);
    easyui.recallfunc(EASYUI_WINDOW, ONCLOSE, on_close);
    blinkTimer = easyui.settimer(530, on_blink);
    g_tab_hook = tab_hook; g_move_hook = on_move;

    /* code font: JetBrains Mono next to the Inter fonts MOSdesk installs; the bitmap font if it is missing */
    fontId = easyui.addfont("0:/programs/mosdesk/fonts/JetBrainsMono-Regular.ttf");
    fontIsMono = fontId != 0;
    if (!fontId) fontId = EASYUI_FONT_BITMAP;
    UI_H = easyui.smoothtext() ? 15 : 9;
    apply_font();

    /* documents and the sidebar folder from the arguments */
    root[0] = 0;
    for (i = 1; i < argc; i++) {
        char p[PATH_MAX_];
        path_resolve(argv[i], p, sizeof p);
        if (mos_is_dir(p) == 1) { if (!haveRoot) { strlcpy_safe(root, p, sizeof root); haveRoot = 1; } continue; }
        open_file(p);
        if (!haveRoot) { dir_of(p, root, sizeof root); haveRoot = 1; }
    }
    if (argc <= 1 && session_restore(root, sizeof root)) haveRoot = root[0] != 0;
    if (!haveRoot) {
#ifdef CODE_HAS_MOSDESK
        mosdesk_settings_t s = mosdesk_settings_load();
        char home[PATH_MAX_];
        if (mosdesk_user_path(home, sizeof home, s.username, NULL) && mos_is_dir(home) == 1) { strlcpy_safe(root, home, sizeof root); haveRoot = 1; }
#endif
        if (!haveRoot) strlcpy_safe(root, "0:/", sizeof root);
    }
    if (ntabs == 0) new_tab();
    tab_activate(cur);
    tree_set_root(root);
    if (argc > 1 && mos_is_dir(root) != 1) set_msg("Ready", 0);
    return easyui.run();
}
