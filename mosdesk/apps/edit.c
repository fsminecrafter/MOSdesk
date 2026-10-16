/*
 * edit.c - Simple text editor for Minimal-OS (single file, easyui)
 *
 *     ./build.sh --output mos_edit.run edit.c
 *     run 0:/programs/mos_edit.run [file]    (inside MOSdesk)
 *
 * Bundled with MOSdesk: mosdesk/build.sh installs it as
 * 0:/programs/mos_edit.run, and mosWM opens text files with it.
 *
 * Features
 *   Open / Save / Save As            Ctrl+O, Ctrl+S, Ctrl+Shift+S
 *   Undo / Redo                      Ctrl+Z, Ctrl+Y, Ctrl+Shift+Z
 *   Find / Find + Replace            Ctrl+F, Ctrl+H, F3 = next, Esc closes
 *   Selection, mouse, Ctrl+A, in-app Cut / Copy / Paste (Ctrl+X / C / V)
 *   Smooth anti-aliased text (easyui's Inter renderer) + smooth scrolling
 *   MOSdesk theme colours (follows Settings > Appearance live)
 *
 * Notes
 *   - Text is ASCII from the keyboard; other bytes in a file are kept but
 *     drawn as '?'. Tabs in a file are drawn as one space. CR is dropped on load.
 *   - The mouse wheel is not delivered to canvases by easyui, so scrolling
 *     uses the scrollbar (wheel works over it), PgUp/PgDn and the arrow keys.
 *   - easyui does not pass mouse coordinates to canvas callbacks, so this file
 *     reads eui_mx / eui_my / eui_buttons, which easyui.h keeps in the same
 *     translation unit (see mouse_x() below).
 */
#include "easyui.h"
#include "stdlib.h"
#include "string.h"
#if defined(__has_include)
#if __has_include("../settings.h")
#include "../settings.h"         /* MOSdesk: the user's Documents folder */
#define EDIT_HAS_MOSDESK 1
#endif
#endif

#define DOC_MAX    262144      /* max document bytes */
#define ARENA_MAX  524288      /* undo/redo data pool */
#define MAX_EDITS  4096        /* undo steps; history resets if either pool fills */
#define MAX_LINES  32768
#define CLIP_MAX   65536
#define SEG_MAX    250         /* easyui draws/measures at most 259 bytes per line */

#define LH      18             /* line height in px */
#define GUTTER  50
#define PAD     8
#define SB_W    16
#define TS      1              /* easyui text size (12 px Inter) */

/* USB HID usage codes (what easyui's ONKEY delivers) */
enum { K_A = 4, K_C = 6, K_F = 9, K_H = 11, K_O = 18, K_S = 22, K_V = 25, K_X = 27, K_Y = 28, K_Z = 29,
       K_ENTER = 40, K_ESC = 41, K_BACKSPACE = 42, K_TAB = 43, K_F3 = 60, K_HOME = 74, K_PGUP = 75,
       K_DELETE = 76, K_END = 77, K_PGDN = 78, K_RIGHT = 79, K_LEFT = 80, K_DOWN = 81, K_UP = 82, K_KPENTER = 88 };

static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }

/* ===========================================================================
 *  Editor core (no UI calls)
 * =========================================================================== */
typedef struct { int pos, remOff, remLen, insOff, insLen, cb, ca, arenaEnd; } Edit;

static char *text, *tmp, *arena, *clipbuf;
static int  *ls;                         /* line start offsets */
static Edit *edits;
static int len = 0, nl = 1;              /* bytes, lines */
static int caret = 0, anchor_ = 0, wantCol = 0;
static int modified = 0, canMerge = 0, clipLen = 0;
static int ecount = 0, ecur = 0, atop = 0;   /* [0,ecur) undo, [ecur,ecount) redo */
static char path[128];
static char msg[80] = "Ready";

static void set_msg(const char* s) { strlcpy_safe(msg, s, sizeof msg); }
static int  u2s(int v, char* out) {
    char t[12]; int n = 0, i;
    if (v <= 0) { out[0] = '0'; out[1] = 0; return 1; }
    while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    for (i = 0; i < n; i++) out[i] = t[n - 1 - i];
    out[n] = 0; return n;
}
static void cat(char* dst, int cap, const char* s) { int n = (int)strlen(dst); if (n < cap - 1) strlcpy_safe(dst + n, s, (size_t)(cap - n)); }
static void set_msg_num(int v, const char* suffix) { char b[80]; u2s(v, b); cat(b, sizeof b, suffix); set_msg(b); }
static void set_msg_path(const char* pre, const char* p) { char b[80]; strlcpy_safe(b, pre, 24); cat(b, sizeof b, p); set_msg(b); }

static int alloc_all(void) {
    text    = (char*)malloc(DOC_MAX);
    tmp     = (char*)malloc(DOC_MAX);
    arena   = (char*)malloc(ARENA_MAX);
    clipbuf = (char*)malloc(CLIP_MAX);
    ls      = (int*)malloc(sizeof(int) * MAX_LINES);
    edits   = (Edit*)malloc(sizeof(Edit) * MAX_EDITS);
    return text && tmp && arena && clipbuf && ls && edits;
}

static void rebuild(void) {
    int i; ls[0] = 0; nl = 1;
    for (i = 0; i < len; i++) if (text[i] == '\n' && nl < MAX_LINES) ls[nl++] = i + 1;
}
static int line_of(int off) {
    int lo = 0, hi = nl - 1;
    while (lo < hi) { int mid = (lo + hi + 1) / 2; if (ls[mid] <= off) lo = mid; else hi = mid - 1; }
    return lo;
}
static int line_end(int l) { return l + 1 < nl ? ls[l + 1] - 1 : len; }
static int col_of(int off) { return off - ls[line_of(off)]; }
static int has_sel(void)   { return caret != anchor_; }
static int sel_a(void)     { return imin(caret, anchor_); }
static int sel_b(void)     { return imax(caret, anchor_); }

static void raw_replace(int pos, int rem, const char* ins, int il) {
    memmove(text + pos + il, text + pos + rem, (size_t)(len - pos - rem));
    memcpy(text + pos, ins, (size_t)il);
    len += il - rem;
    rebuild();
}

static void edit(int pos, int rem, const char* ins, int il) {
    int need;
    if (len - rem + il >= DOC_MAX) { set_msg("Document is full"); return; }
    /* merge consecutive typed characters into one undo step */
    if (canMerge && ecur > 0 && ecur == ecount && rem == 0 && il == 1 && ins[0] != '\n') {
        Edit* l = &edits[ecur - 1];
        if (l->remLen == 0 && l->pos + l->insLen == pos && atop < ARENA_MAX) {
            arena[atop++] = ins[0]; l->insLen++; l->ca = pos + 1; l->arenaEnd = atop;
            raw_replace(pos, 0, ins, 1);
            caret = anchor_ = pos + 1; modified = 1; return;
        }
    }
    if (ecur != ecount) { ecount = ecur; atop = ecur ? edits[ecur - 1].arenaEnd : 0; }    /* drop redo */
    need = rem + il;
    if (ecount >= MAX_EDITS || atop + need > ARENA_MAX) { ecount = ecur = 0; atop = 0; }   /* history full: reset */
    if (need <= ARENA_MAX) {
        Edit* e = &edits[ecount];
        e->pos = pos;
        e->remOff = atop; e->remLen = rem; memcpy(arena + atop, text + pos, (size_t)rem); atop += rem;
        e->insOff = atop; e->insLen = il;  memcpy(arena + atop, ins, (size_t)il);        atop += il;
        e->cb = caret; e->ca = pos + il; e->arenaEnd = atop;
        ecount++; ecur = ecount;
    }
    raw_replace(pos, rem, ins, il);
    caret = anchor_ = pos + il; modified = 1;
    canMerge = (rem == 0 && il == 1 && ins[0] != '\n');
}
static void insert_text(const char* s, int n) { edit(sel_a(), sel_b() - sel_a(), s, n); }
static void do_backspace(void) { if (has_sel()) insert_text("", 0); else if (caret > 0) edit(caret - 1, 1, "", 0); canMerge = 0; }
static void do_delete(void)    { if (has_sel()) insert_text("", 0); else if (caret < len) edit(caret, 1, "", 0); canMerge = 0; }

static void undo(void) {
    Edit* e; if (ecur == 0) { set_msg("Nothing to undo"); return; }
    e = &edits[--ecur];
    raw_replace(e->pos, e->insLen, arena + e->remOff, e->remLen);
    caret = anchor_ = e->cb; modified = 1; canMerge = 0; set_msg("Undo");
}
static void redo(void) {
    Edit* e; if (ecur == ecount) { set_msg("Nothing to redo"); return; }
    e = &edits[ecur++];
    raw_replace(e->pos, e->remLen, arena + e->insOff, e->insLen);
    caret = anchor_ = e->ca; modified = 1; canMerge = 0; set_msg("Redo");
}

/* ---- find / replace ---- */
static int fold(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static int eq_at(const char* a, const char* b, int n, int cs) {
    int i; for (i = 0; i < n; i++) { int x = (unsigned char)a[i], y = (unsigned char)b[i]; if (!cs) { x = fold(x); y = fold(y); } if (x != y) return 0; }
    return 1;
}
static int find_in(const char* nd, int n, int from, int cs) {
    int i; if (n <= 0) return -1;
    for (i = from; i + n <= len; i++) if (eq_at(text + i, nd, n, cs)) return i;
    return -1;
}
static int find_next(const char* nd, int n, int cs) {
    int p = find_in(nd, n, sel_b(), cs);
    if (p < 0) p = find_in(nd, n, 0, cs);                       /* wrap around */
    if (p < 0) return 0;
    anchor_ = p; caret = p + n; canMerge = 0; return 1;
}
static int replace_current(const char* nd, int n, const char* rp, int rn, int cs) {
    if (n <= 0) return 0;
    if (has_sel() && sel_b() - sel_a() == n && eq_at(text + sel_a(), nd, n, cs)) { insert_text(rp, rn); canMerge = 0; }
    return find_next(nd, n, cs);
}
static int replace_all(const char* nd, int n, const char* rp, int rn, int cs) {   /* one undo step; -1 = too big */
    int count = 0, out = 0, last = 0, p;
    if (n <= 0) return 0;
    p = find_in(nd, n, 0, cs);
    while (p >= 0) {
        if (out + (p - last) + rn >= DOC_MAX) return -1;
        memcpy(tmp + out, text + last, (size_t)(p - last)); out += p - last;
        memcpy(tmp + out, rp, (size_t)rn); out += rn;
        last = p + n; count++;
        p = find_in(nd, n, last, cs);
    }
    if (!count) return 0;
    if (out + (len - last) >= DOC_MAX) return -1;
    memcpy(tmp + out, text + last, (size_t)(len - last)); out += len - last;
    edit(0, len, tmp, out);
    caret = anchor_ = 0; canMerge = 0;
    return count;
}

/* ---- files (MinimaFS) ---- */
static int read_all(const char* p) {                  /* 1 ok, 0 failed, -1 too large */
    long fd = mos_open(p, SYS_O_RDONLY), size, got = 0, n; int i, o = 0;
    if (fd <= 0) return 0;
    size = mos_size(fd);
    if (size < 0 || size >= DOC_MAX) { mos_close(fd); return -1; }
    while (got < size && (n = mos_read(fd, tmp + got, (size_t)(size - got))) > 0) got += n;
    mos_close(fd);
    for (i = 0; i < got; i++) if (tmp[i] != '\r') text[o++] = tmp[i];
    len = o; rebuild();
    strlcpy_safe(path, p, sizeof path);
    caret = anchor_ = wantCol = 0; ecount = ecur = atop = 0; modified = 0; canMerge = 0;
    return 1;
}
static int write_all(const char* p) {
    long fd, total = 0, n;
    if (mos_exists(p) == 1) { if (mos_is_dir(p) == 1 || mos_delete(p) < 0) return 0; }   /* writes never shorten a file */
    if (mos_create(p, "text", "txt") < 0) return 0;
    fd = mos_open(p, SYS_O_RDWR);
    if (fd <= 0) return 0;
    while (total < len && (n = mos_fwrite(fd, text + total, (size_t)(len - total))) > 0) total += n;
    mos_close(fd);
    return total == len;
}

/* ===========================================================================
 *  UI (easyui)
 * =========================================================================== */
static int bOpen, bSave, bSaveAs, bUndo, bRedo, bFind, bRepl, tgDark;
static int findIn, replIn, chkCase, bNext, bRepl2, bAll, bFindX;
static int lblPath, pathIn, bPathOk, bPathCancel;
static int canvas, slider, lblLeft, lblRight;

static int findOpen = 0, replaceMode = 0, pathMode = 0;      /* pathMode: 0 none, 1 save as, 2 open */
static int matchCase = 0, caretOn = 1, dragging = 0, discardArmed = 0, closeArmed = 0;
static char armedPath[128];
static int scroll_fp = 0, scrollT_fp = 0;                    /* 8.8 fixed-point pixels */
static int colOff = 0;                                       /* horizontal scroll, in characters */
static int tAnim = 0, sliderLock = 0, sRange = -1, sVal = -1;
static int lastUndoEn = -1, lastRedoEn = -1;
static char lastLeft[160], lastRight[48], lastTitle[160];

/* easyui hands no mouse position to canvas callbacks; use the ones it tracks. */
static int mouse_x(void)       { return (int)eui_mx; }
static int mouse_y(void)       { return (int)eui_my; }
static int mouse_down(void)    { return (eui_buttons & 1u) != 0; }

static int view_h(void)  { return easyui.getheight(canvas); }
static int max_scroll_px(void) { return imax(0, nl * LH - view_h() + LH); }

/* ---- text segments (clamped, printable, NUL-terminated copy of part of a line) ---- */
static int build_seg(int start, int n, char* out) {
    int i; if (n > SEG_MAX) n = SEG_MAX; if (n < 0) n = 0;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)text[start + i];
        out[i] = (c >= 32 && c < 127) ? (char)c : (c == '\t' ? ' ' : '?');
    }
    out[n] = 0; return n;
}
static int seg_w(int start, int n) { char b[SEG_MAX + 1]; if (n <= 0) return 0; build_seg(start, n, b); return easyui.textwidth(b, TS); }

/* ---- view ---- */
static void reveal_y(void) {
    int ah = view_h(), y = line_of(caret) * LH, t = scrollT_fp >> 8;
    if (y < t) t = y; else if (y + LH > t + ah) t = y + LH - ah;
    scrollT_fp = imax(0, imin(t, max_scroll_px())) << 8;
}
static void reveal_x(void) {
    int l = line_of(caret), c = caret - ls[l], avail = easyui.getwidth(canvas) - GUTTER - 2 * PAD - 12;
    if (c < colOff) colOff = c > 8 ? c - 8 : 0;
    while (colOff < c && seg_w(ls[l] + colOff, c - colOff) > avail) colOff += 6;
    if (colOff > c) colOff = c;
}
static void reveal(void) { reveal_y(); reveal_x(); caretOn = 1; }

static int animate(void) {                                   /* smooth scroll easing, no floats */
    int d, m = max_scroll_px() << 8;
    if (scrollT_fp > m) scrollT_fp = m;
    if (scrollT_fp < 0) scrollT_fp = 0;
    d = scrollT_fp - scroll_fp;
    if (d > -77 && d < 77) { int moved = scroll_fp != scrollT_fp; scroll_fp = scrollT_fp; return moved; }
    scroll_fp += d * 9 / 32;
    return 1;
}

static int offset_at(int px, int py) {
    int cx = easyui.getx(canvas), cy = easyui.gety(canvas), l, start, n, lo = 0, hi, rel, k;
    l = imax(0, imin((py - cy + (scroll_fp >> 8)) / LH, nl - 1));
    start = ls[l]; n = imax(0, imin(line_end(l) - start - colOff, SEG_MAX));
    rel = px - (cx + GUTTER + PAD);
    hi = n;
    while (lo < hi) { int mid = (lo + hi) / 2; if (seg_w(start + colOff, mid + 1) <= rel) lo = mid + 1; else hi = mid; }
    k = lo;
    if (k < n && rel - seg_w(start + colOff, k) > seg_w(start + colOff, k + 1) - rel) k++;   /* nearest edge */
    return start + colOff + k;
}

/* ---- slider used as scrollbar ---- */
static void sync_slider(void) {
    int maxl = (max_scroll_px() + LH - 1) / LH, range = imax(maxl, 1), cur = ((scrollT_fp >> 8) + LH / 2) / LH, val;
    cur = imin(imax(cur, 0), maxl);
    val = range - cur;                                       /* vertical slider: top = max */
    sliderLock = 1;
    if (range != sRange) { easyui.setrange(slider, 0, range); sRange = range; sVal = -1; easyui.setenabled(slider, maxl > 0); }
    if (val != sVal) { easyui.setvalue(slider, val); sVal = val; }
    sliderLock = 0;
}

/* ---- timers ---- */
static void on_anim(int timer) {
    int moving = animate();
    if (dragging) {
        if (!mouse_down()) dragging = 0;
        else {
            int cy = easyui.gety(canvas), my = mouse_y();
            if (my < cy) scrollT_fp -= (LH / 2) << 8; else if (my > cy + view_h()) scrollT_fp += (LH / 2) << 8;
            caret = offset_at(mouse_x(), imax(cy, imin(my, cy + view_h() - 1)));
            wantCol = col_of(caret); reveal_x(); moving = 1;
        }
    }
    sync_slider();
    if (!moving) { easyui.stoptimer(timer); tAnim = 0; }
}
static void start_anim(void) { if (!tAnim) tAnim = easyui.settimer(16, on_anim); }
static void on_blink(int timer) { (void)timer; if (easyui.focused() == 0) caretOn = !caretOn; else caretOn = 0; }

/* ---- status / title / enabled state ---- */
static void update_status(void) {
    char left[160], right[48], title[160], t[12];
    int l = line_of(caret), nn;
    strlcpy_safe(left, path[0] ? path : "untitled", sizeof left);
    if (modified) cat(left, sizeof left, " *");
    cat(left, sizeof left, "    "); cat(left, sizeof left, msg);
    strlcpy_safe(right, "Ln ", sizeof right); u2s(l + 1, t); cat(right, sizeof right, t);
    cat(right, sizeof right, ", Col "); u2s(caret - ls[l] + 1, t); cat(right, sizeof right, t);
    if (strcmp(left, lastLeft))   { easyui.settext(lblLeft, left);   strlcpy_safe(lastLeft, left, sizeof lastLeft); }
    if (strcmp(right, lastRight)) { easyui.settext(lblRight, right); strlcpy_safe(lastRight, right, sizeof lastRight); }
    strlcpy_safe(title, "MOS Edit - ", sizeof title); cat(title, sizeof title, path[0] ? path : "untitled");
    if (modified) cat(title, sizeof title, " *");
    if (strcmp(title, lastTitle)) { easyui.settitle(title); strlcpy_safe(lastTitle, title, sizeof lastTitle); }
    nn = ecur > 0; if (nn != lastUndoEn) { easyui.setenabled(bUndo, nn); lastUndoEn = nn; }
    nn = ecur < ecount; if (nn != lastRedoEn) { easyui.setenabled(bRedo, nn); lastRedoEn = nn; }
}
static void refresh(void) { sync_slider(); update_status(); if (scrollT_fp != scroll_fp) start_anim(); }

/* ---- layout ---- */
static void place(int w, int x, int y, int ww, int hh) { easyui.setposition(w, x, y); easyui.setsize(w, imax(ww, 24), hh); }
static void layout(void) {
    int W = easyui.width(), H = easyui.height(), top = 46, x, bw = 70;
    easyui.setposition(tgDark, W - 90, 8);
    easyui.setvisible(findIn, findOpen); easyui.setvisible(chkCase, findOpen); easyui.setvisible(bNext, findOpen); easyui.setvisible(bFindX, findOpen);
    easyui.setvisible(replIn, findOpen && replaceMode); easyui.setvisible(bRepl2, findOpen && replaceMode); easyui.setvisible(bAll, findOpen && replaceMode);
    if (findOpen) {
        x = 8; place(findIn, x, top, 200, 30); x += 208;
        if (replaceMode) { place(replIn, x, top, 200, 30); x += 208; }
        place(chkCase, x, top, 104, 30); x += 112;
        place(bNext, x, top, 56, 30); x += 62;
        if (replaceMode) { place(bRepl2, x, top, 74, 30); x += 80; place(bAll, x, top, 48, 30); x += 54; }
        place(bFindX, x, top, 34, 30);
        top += 38;
    }
    easyui.setvisible(lblPath, pathMode != 0); easyui.setvisible(pathIn, pathMode != 0);
    easyui.setvisible(bPathOk, pathMode != 0); easyui.setvisible(bPathCancel, pathMode != 0);
    if (pathMode) {
        place(lblPath, 8, top, 70, 30);
        place(pathIn, 80, top, W - 80 - 8 - 2 * (bw + 6), 30);
        place(bPathOk, W - 8 - 2 * bw - 6, top, bw, 30);
        place(bPathCancel, W - 8 - bw, top, bw, 30);
        top += 38;
    }
    place(canvas, 8, top, W - 16 - SB_W, H - top - 34);
    place(slider, W - 8 - SB_W, top, SB_W, H - top - 34);
    place(lblLeft, 8, H - 28, W - 16 - 190, 22);
    place(lblRight, W - 8 - 180, H - 28, 180, 22);
}

/* ---- actions ---- */
/* Where a new document is suggested: 0:/users/<username>/Documents. */
static const char* new_file_path(void) {
#ifdef EDIT_HAS_MOSDESK
    static char p[128];
    mosdesk_settings_t s = mosdesk_settings_load();
    if (mosdesk_user_path(p, sizeof p, s.username, MOSDESK_USER_DOCUMENTS)) {
        cat(p, sizeof p, "/untitled.txt");
        return p;
    }
#endif
    return "0:/untitled.txt";
}

static void begin_path(int mode) {
    pathMode = mode; armedPath[0] = 0; discardArmed = 0;
    easyui.settext(lblPath, mode == 1 ? "Save as:" : "Open:");
    easyui.settext(pathIn, path[0] ? path : new_file_path());
    layout(); easyui.setfocus(pathIn);
}
static void end_path(void) { pathMode = 0; layout(); easyui.setfocus(0); }

static void do_save_to(const char* p) {
    if (write_all(p)) { strlcpy_safe(path, p, sizeof path); modified = 0; set_msg_path("Saved ", p); }
    else set_msg_path("Save FAILED: ", p);
}
static void do_save(void) { if (path[0]) do_save_to(path); else begin_path(1); }

static void commit_path(void) {
    const char* p = easyui.gettext(pathIn);
    if (!p[0]) return;
    if (pathMode == 1) {
        if (mos_exists(p) == 1 && strcmp(p, path) != 0 && strcmp(p, armedPath) != 0) {    /* ask before overwriting another file */
            strlcpy_safe(armedPath, p, sizeof armedPath); set_msg("File exists - press Enter again to overwrite"); return;
        }
        do_save_to(p); end_path();
    } else {
        int r;
        if (modified && !discardArmed) { discardArmed = 1; set_msg("Unsaved changes - press Enter again to discard them"); return; }
        r = read_all(p); discardArmed = 0;
        if (r == 1) { scroll_fp = scrollT_fp = 0; colOff = 0; set_msg_path("Opened ", p); end_path(); }
        else set_msg(r < 0 ? "File too large (max 256 KB)" : "Could not open file");
    }
}

static void open_find(int withReplace) {
    findOpen = 1; replaceMode = withReplace;
    layout();
    if (has_sel() && sel_b() - sel_a() < 100 && line_of(sel_a()) == line_of(sel_b())) {
        char b[104]; int n = sel_b() - sel_a(); memcpy(b, text + sel_a(), (size_t)n); b[n] = 0; easyui.settext(findIn, b);
    }
    easyui.setfocus(findIn);
}
static void close_find(void) { findOpen = 0; layout(); easyui.setfocus(0); }

static void do_find_next(void) {
    const char* nd = easyui.gettext(findIn);
    set_msg(find_next(nd, (int)strlen(nd), matchCase) ? "Found" : "Not found"); reveal();
}
static void do_replace(void) {
    const char* nd = easyui.gettext(findIn); const char* rp = easyui.gettext(replIn);
    set_msg(replace_current(nd, (int)strlen(nd), rp, (int)strlen(rp), matchCase) ? "Replaced" : "No more matches"); reveal();
}
static void do_replace_all(void) {
    const char* nd = easyui.gettext(findIn); const char* rp = easyui.gettext(replIn);
    int n = replace_all(nd, (int)strlen(nd), rp, (int)strlen(rp), matchCase);
    if (n < 0) set_msg("Result too large"); else set_msg_num(n, " replaced");
    reveal();
}

/* ---- callbacks ---- */
static void on_button(int v) {
    int w = easyui.current(); (void)v;
    if (w == bSave) do_save();
    else if (w == bSaveAs) begin_path(1);
    else if (w == bOpen) begin_path(2);
    else if (w == bUndo) { undo(); reveal(); }
    else if (w == bRedo) { redo(); reveal(); }
    else if (w == bFind) { if (findOpen) close_find(); else open_find(0); }
    else if (w == bRepl) open_find(1);
    else if (w == bNext) do_find_next();
    else if (w == bRepl2) do_replace();
    else if (w == bAll) do_replace_all();
    else if (w == bFindX) close_find();
    else if (w == bPathOk) commit_path();
    else if (w == bPathCancel) end_path();
    if (easyui.focused() == w) easyui.setfocus(pathMode ? pathIn : 0);   /* give the keyboard back to the editor */
    refresh();
}
static void on_submit(int v) {
    int w = easyui.current(); (void)v;
    if (w == findIn) do_find_next(); else if (w == replIn) do_replace(); else if (w == pathIn) commit_path();
    refresh();
}
static void on_case(int v) { matchCase = v; }
static void on_dark(int v) { easyui.setdarkmode(v); }

static void on_slider(int v) {
    if (sliderLock) return;
    scrollT_fp = imax(0, imin((sRange - v) * LH, max_scroll_px())) << 8; sVal = v;
    start_anim();
}

static void move_to(int to, int keepWant, int extend) {
    caret = to; if (!extend) anchor_ = caret; canMerge = 0;
    if (!keepWant) wantCol = col_of(caret);
    reveal();
}
static void move_vert(int delta, int extend) {
    int t = imax(0, imin(line_of(caret) + delta, nl - 1));
    move_to(ls[t] + imin(wantCol, line_end(t) - ls[t]), 1, extend);
}
static int key_ascii(int key, int shift) {
    static const char lower[] = "abcdefghijklmnopqrstuvwxyz1234567890";
    static const char upper[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ!@#$%^&*()";
    static const char sym_lo[] = "-=[]\\#;'`,./";
    static const char sym_up[] = "_+{}|~:\"~<>?";
    if (key >= 4 && key <= 39) return shift ? upper[key - 4] : lower[key - 4];
    if (key == 44) return ' ';
    if (key >= 45 && key <= 56 && key != 50) return shift ? sym_up[key - 45] : sym_lo[key - 45];
    return 0;
}
static void copy_sel(void) {
    int n = imin(sel_b() - sel_a(), CLIP_MAX);
    if (n > 0) { memcpy(clipbuf, text + sel_a(), (size_t)n); clipLen = n; set_msg("Copied"); }
}

static void on_key(int v) {                                  /* window-level ONKEY: key | modifiers << 8 */
    int key = v & 0xFF, mods = (v >> 8) & 7;
    int shift = mods & 1, ctrl = (mods >> 1) & 1, alt = (mods >> 2) & 1;
    int inField = easyui.focused() != 0, l, vis, c;
    caretOn = 1; closeArmed = 0;

    if (ctrl) {                                              /* shortcuts that work everywhere */
        switch (key) {
            case K_S: if (shift) begin_path(1); else do_save(); refresh(); return;
            case K_O: begin_path(2); refresh(); return;
            case K_F: open_find(0); refresh(); return;
            case K_H: open_find(1); refresh(); return;
        }
    }
    if (key == K_F3) { if (easyui.gettext(findIn)[0]) { do_find_next(); refresh(); } return; }
    if (key == K_ESC && !inField) { if (pathMode) end_path(); else if (findOpen) close_find(); refresh(); return; }
    if (inField) return;                                     /* a text field has the keyboard */

    l = line_of(caret); vis = imax(1, view_h() / LH);
    if (ctrl) {
        switch (key) {
            case K_Z: if (shift) redo(); else undo(); reveal(); break;
            case K_Y: redo(); reveal(); break;
            case K_A: anchor_ = 0; caret = len; break;
            case K_C: copy_sel(); break;
            case K_X: if (has_sel()) { copy_sel(); insert_text("", 0); set_msg("Cut"); reveal(); } break;
            case K_V: if (clipLen) { insert_text(clipbuf, clipLen); canMerge = 0; wantCol = col_of(caret); reveal(); } break;
            case K_HOME: move_to(0, 0, shift); break;
            case K_END:  move_to(len, 0, shift); break;
        }
        refresh(); return;
    }
    if (alt) return;
    switch (key) {
        case K_LEFT:  if (!shift && has_sel()) move_to(sel_a(), 0, 0); else if (caret > 0) move_to(caret - 1, 0, shift); break;
        case K_RIGHT: if (!shift && has_sel()) move_to(sel_b(), 0, 0); else if (caret < len) move_to(caret + 1, 0, shift); break;
        case K_UP:    move_vert(-1, shift); break;
        case K_DOWN:  move_vert(1, shift); break;
        case K_PGUP:  move_vert(-vis, shift); break;
        case K_PGDN:  move_vert(vis, shift); break;
        case K_HOME:  move_to(ls[l], 0, shift); break;
        case K_END:   move_to(line_end(l), 0, shift); break;
        case K_BACKSPACE: do_backspace(); wantCol = col_of(caret); reveal(); break;
        case K_DELETE:    do_delete(); reveal(); break;
        case K_ENTER: case K_KPENTER: insert_text("\n", 1); canMerge = 0; wantCol = 0; reveal(); break;
        case K_TAB:   insert_text("    ", 4); canMerge = 0; wantCol = col_of(caret); reveal(); break;
        default:
            c = key_ascii(key, shift);
            if (c) { char ch = (char)c; insert_text(&ch, 1); wantCol = col_of(caret); reveal(); }
            break;
    }
    refresh();
}

static void on_canvas_press(int v) {
    (void)v;
    caretOn = 1; dragging = 1;
    caret = anchor_ = offset_at(mouse_x(), mouse_y()); wantCol = col_of(caret); canMerge = 0;
    reveal_x(); start_anim(); refresh();
}
static void on_canvas_release(int v) { (void)v; dragging = 0; }
static void on_resize(int w) { (void)w; layout(); reveal(); refresh(); }
static void on_close(int v) {
    (void)v;
    if (modified && !closeArmed) { closeArmed = 1; easyui.messagebox("Unsaved changes", "Press Ctrl+S to save, or close the window again to discard them."); return; }
    easyui.quit();
}

static void on_canvas_draw(int v) {
    const easyui_theme_t* th = easyui.theme();
    int cx = easyui.getx(canvas), cy = easyui.gety(canvas), cw = easyui.getwidth(canvas), ch = easyui.getheight(canvas);
    int sy = scroll_fp >> 8, first = sy / LH, last = imin(nl, first + ch / LH + 2), cl = line_of(caret), i, tx = cx + GUTTER + PAD;
    uint32_t selc = (th->accent & 0x00FFFFFFu) | 0x66000000u, curl = (th->accent & 0x00FFFFFFu) | 0x1Cu << 24;
    char seg[SEG_MAX + 1], num[12];
    (void)v;
    easyui.fillrect(cx, cy, cw, ch, th->surface);
    easyui.fillrect(cx, cy, GUTTER, ch, th->background);
    easyui.fillrect(cx + GUTTER - 1, cy, 1, ch, th->border);
    for (i = first; i < last; i++) {
        int y = cy + i * LH - sy, a = ls[i], e = line_end(i), n = imax(0, imin(e - a - colOff, SEG_MAX));
        if (i == cl) easyui.fillrect(cx + GUTTER, y, cw - GUTTER, LH, curl);
        if (has_sel() && sel_a() <= e && sel_b() > a) {
            int sa = imax(sel_a(), a + colOff) - a - colOff, sb = imin(sel_b(), e) - a - colOff;
            int x1 = tx + seg_w(a + colOff, imax(sa, 0)), x2 = tx + seg_w(a + colOff, imax(sb, 0));
            if (sel_b() > e) x2 += 6;                                   /* selected newline */
            if (x2 > x1) easyui.fillrect(x1, y, x2 - x1, LH, selc);
        }
        if (n > 0) { build_seg(a + colOff, n, seg); easyui.drawtext(tx, y + 2, seg, th->text, TS); }
        u2s(i + 1, num);
        easyui.drawtext(cx + GUTTER - 10 - easyui.textwidth(num, TS), y + 2, num, i == cl ? th->text : th->muted, TS);
    }
    if (caretOn && easyui.focused() == 0) {
        int y = cy + cl * LH - sy, x = tx + seg_w(ls[cl] + colOff, caret - ls[cl] - colOff);
        easyui.fillrect(x, y + 1, 2, LH - 2, th->accent);
    }
}

/* ===========================================================================
 *  main
 * =========================================================================== */
int main(int argc, char** argv) {
    if (!alloc_all()) { mos_puts("edit: out of memory\n"); return 1; }
    rebuild();
    if (!easyui.init("MOS Edit", 780, 520)) { mos_puts("edit: start MOSdesk first\n"); return 1; }

    bOpen   = easyui.createbutton(8,   8, 58, 30, "Open");
    bSave   = easyui.createbutton(70,  8, 58, 30, "Save");
    bSaveAs = easyui.createbutton(132, 8, 74, 30, "Save As");
    bUndo   = easyui.createbutton(224, 8, 58, 30, "Undo");
    bRedo   = easyui.createbutton(286, 8, 58, 30, "Redo");
    bFind   = easyui.createbutton(362, 8, 58, 30, "Find");
    bRepl   = easyui.createbutton(424, 8, 74, 30, "Replace");
    tgDark  = easyui.createtoggle(690, 8, 84, 30, "Dark", 0);

    findIn  = easyui.createtextinput(8, 46, 200, 30, "Find");
    replIn  = easyui.createtextinput(216, 46, 200, 30, "Replace with");
    chkCase = easyui.createcheckbox(424, 46, 104, 30, "Match case", 0);
    bNext   = easyui.createbutton(0, 46, 56, 30, "Next");
    bRepl2  = easyui.createbutton(0, 46, 74, 30, "Replace");
    bAll    = easyui.createbutton(0, 46, 48, 30, "All");
    bFindX  = easyui.createbutton(0, 46, 34, 30, "X");

    lblPath = easyui.createlabel(8, 46, 70, 30, "Save as:");
    pathIn  = easyui.createtextinput(80, 46, 400, 30, "0:/folder/file.txt");
    bPathOk = easyui.createbutton(0, 46, 70, 30, "OK");
    bPathCancel = easyui.createbutton(0, 46, 70, 30, "Cancel");

    canvas  = easyui.createcanvas(8, 46, 740, 430);
    slider  = easyui.createslider(756, 46, SB_W, 430, 0, 1, 1);
    lblLeft = easyui.createlabel(8, 492, 580, 22, "");
    lblRight = easyui.createlabel(592, 492, 180, 22, "");
    easyui.setalign(lblLeft, EASYUI_ALIGN_LEFT);
    easyui.setalign(lblRight, EASYUI_ALIGN_RIGHT);
    easyui.setstep(slider, 1);

    easyui.setstyle(bOpen, EASYUI_SECONDARY); easyui.setstyle(bSaveAs, EASYUI_SECONDARY);
    easyui.setstyle(bUndo, EASYUI_SECONDARY); easyui.setstyle(bRedo, EASYUI_SECONDARY);
    easyui.setstyle(bFind, EASYUI_SECONDARY); easyui.setstyle(bRepl, EASYUI_SECONDARY);
    easyui.setstyle(bNext, EASYUI_SECONDARY); easyui.setstyle(bRepl2, EASYUI_SECONDARY);
    easyui.setstyle(bAll, EASYUI_SECONDARY);  easyui.setstyle(bFindX, EASYUI_FLAT);
    easyui.setstyle(bPathCancel, EASYUI_SECONDARY);
    easyui.setmaxlength(findIn, 100); easyui.setmaxlength(replIn, 100);

    easyui.recallfunc(bOpen, ONCLICK, on_button);   easyui.recallfunc(bSave, ONCLICK, on_button);
    easyui.recallfunc(bSaveAs, ONCLICK, on_button); easyui.recallfunc(bUndo, ONCLICK, on_button);
    easyui.recallfunc(bRedo, ONCLICK, on_button);   easyui.recallfunc(bFind, ONCLICK, on_button);
    easyui.recallfunc(bRepl, ONCLICK, on_button);   easyui.recallfunc(bNext, ONCLICK, on_button);
    easyui.recallfunc(bRepl2, ONCLICK, on_button);  easyui.recallfunc(bAll, ONCLICK, on_button);
    easyui.recallfunc(bFindX, ONCLICK, on_button);  easyui.recallfunc(bPathOk, ONCLICK, on_button);
    easyui.recallfunc(bPathCancel, ONCLICK, on_button);
    easyui.recallfunc(findIn, ONSUBMIT, on_submit); easyui.recallfunc(replIn, ONSUBMIT, on_submit);
    easyui.recallfunc(pathIn, ONSUBMIT, on_submit);
    easyui.recallfunc(chkCase, ONCHANGE, on_case);
    easyui.recallfunc(tgDark, ONCHANGE, on_dark);
    easyui.recallfunc(slider, ONCHANGE, on_slider);
    easyui.recallfunc(canvas, ONDRAW, on_canvas_draw);
    easyui.recallfunc(canvas, ONPRESS, on_canvas_press);
    easyui.recallfunc(canvas, ONRELEASE, on_canvas_release);
    easyui.recallfunc(EASYUI_WINDOW, ONKEY, on_key);
    easyui.recallfunc(EASYUI_WINDOW, ONRESIZE, on_resize);
    easyui.recallfunc(EASYUI_WINDOW, ONCLOSE, on_close);
    easyui.settimer(530, on_blink);

    layout();
    if (argc > 1) {
        int r = read_all(argv[1]);
        if (r == 1) set_msg_path("Opened ", argv[1]);
        else { strlcpy_safe(path, argv[1], sizeof path); set_msg(r < 0 ? "File too large (max 256 KB)" : "New file"); }
    }
    refresh();
    return easyui.run();
}
