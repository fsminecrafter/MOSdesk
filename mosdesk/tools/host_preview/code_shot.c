/* code_shot.c - host harness for the Code app (mosdesk/apps/code/main.c).
 *
 * The app is compiled into this file (its main() renamed), against the real
 * SDK headers whose system calls are redirected to code_host.c. A script
 * (one function per frame, below) feeds key and mouse events, checks what
 * the editor did, and saves screenshots. See code_shot.sh.
 *
 *     ./mosdesk/tools/host_preview/code_shot.sh [outdir]
 */
#define main code_app_main
#include "main.c"                  /* mosdesk/apps/code/main.c */
#undef main
#include "code_host.h"

extern int code_app_main(int argc, char** argv);

static int failures, checks;
static const char* shotDir = ".";
static void check(const char* name, int ok) {
    checks++;
    if (!ok) { char m[200]; failures++; strlcpy_safe(m, "FAIL: ", sizeof m); cat(m, sizeof m, name); cat(m, sizeof m, "\n"); host_log(m); }
}
static int text_is(const char* s) { return D->len == (int)strlen(s) && memcmp(D->text, s, (size_t)D->len) == 0; }
static void dump(const char* label) {
    char m[600]; strlcpy_safe(m, "  ", sizeof m); cat(m, sizeof m, label); cat(m, sizeof m, ": [");
    { int n = (int)strlen(m); int k; for (k = 0; k < D->len && n < 560; k++) m[n++] = D->text[k] == '\n' ? '|' : D->text[k]; m[n] = 0; }
    cat(m, sizeof m, "]\n"); host_log(m);
}

/* ---- event helpers ---- */
static void key(int k, int mods) { host_push_key(k, mods, 1); host_push_key(k, 0, 0); }
static int hid_of(char c, int* shift) {
    static const char lower[] = "abcdefghijklmnopqrstuvwxyz1234567890";
    static const char upper[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ!@#$%^&*()";
    static const char sym_lo[] = "-=[]\\#;'`,./";
    static const char sym_up[] = "_+{}|~:\"~<>?";
    const char* p;
    *shift = 0;
    if (c == '\n') return K_ENTER;
    if (c == '\t') return K_TAB;
    if (c == ' ') return K_SPACE;
    if ((p = strchr(lower, c)) && c) return 4 + (int)(p - lower);
    if ((p = strchr(upper, c)) && c) { *shift = 1; return 4 + (int)(p - upper); }
    if ((p = strchr(sym_lo, c)) && c) return 45 + (int)(p - sym_lo);
    if ((p = strchr(sym_up, c)) && c) { *shift = 1; return 45 + (int)(p - sym_up); }
    return 0;
}
static void typ(const char* s) {
    for (; *s; s++) { int sh, k = hid_of(*s, &sh); if (k) key(k, sh ? M_SHIFT : 0); }
}
static void click(int x, int y) { host_push_mouse(x, y, 0); host_push_mouse(x, y, 1); host_push_mouse(x, y, 0); }
static void drag(int x1, int y1, int x2, int y2) {
    int i;
    host_push_mouse(x1, y1, 0); host_push_mouse(x1, y1, 1);
    for (i = 1; i <= 4; i++) host_push_mouse(x1 + (x2 - x1) * i / 4, y1 + (y2 - y1) * i / 4, 1);
    host_push_mouse(x2, y2, 0);
}
static void shot(const char* name) { char p[512]; strlcpy_safe(p, shotDir, sizeof p); cat(p, sizeof p, "/"); cat(p, sizeof p, name); host_save_ppm(p); }
/* the pixel position of a byte offset in the current document, for clicks */
static int px_x(int off) { calc_layout(); return L.tx + 6 - D->scrollX + cell_of(D, line_of(D, off), col_of(D, off)) * CW + CW / 2; }
static int px_y(int off) { calc_layout(); return L.ey + line_of(D, off) * LH - D->scrollY + LH / 2; }

static void write_file(const char* path, const char* data, int n) { host_write_file(path, data, n); }
static int read_file(const char* path, char* out, int cap) { return host_read_file(path, out, cap); }

/* ===========================================================================
 *  The script: one function per frame
 * =========================================================================== */
static void s_nop(void) {}
static void s_shot_setup(void) {
    calc_layout();
    check("startup: two tabs", ntabs == 2);
    check("startup: tree root is the folder argument", strcmp(treeRoot, "0:/proj") == 0);
    check("startup: C language", tabs[0]->lang == LANG_C);
    check("startup: font loaded", fontIsMono && CW == 9);
    key(K_END, M_CTRL); typ(" "); key(K_BACKSPACE, 0);   /* util.h (the active tab) gets the dirty dot */
}
static void s_shot_setup1(void) {
    check("typing marks the tab dirty", tabs[1]->modified);
    click(60, TAB_H + SB_HDR + 0 * ROW_H + 11);          /* the src folder, to expand it */
}
static void s_shot_setup2(void) { check("tree: folder expanded", tcount > 5); click(tabX[0] + 40, 15); }
static void s_shot_setup3(void) {
    check("tab click: first tab active", cur == 0);
    key(K_F, M_CTRL); typ("count");
}
static void s_shot_final(void) {
    int l = 18;
    check("find bar open with matches", findOpen && nmatches >= 2);
    click(px_x(line_end_(D, l)) + 4 * CW, px_y(D->ls[l]));   /* just after the { of the for loop */
}
static void s_shot_final3(void) {
    check("click after { puts the caret there", D->text[D->caret - 1] == '{' && line_of(D, D->caret) == 18);
    check("matching bracket found", bm_a >= 0 && bm_b > bm_a && D->text[bm_b] == '}');
    shot("code.png");
}

/* ---- behaviour tests (each in a fresh tab) ---- */
static int tabBefore;
static void t_tree_click(void) {
    int i, want = -1;
    for (i = 0; i < tcount; i++) if (!tn[i].isdir && strcmp(tn[i].name, "Makefile") == 0) want = i;
    check("tree lists the Makefile", want >= 0);
    key(K_ESC, 0);
    click(60, TAB_H + SB_HDR + want * ROW_H - treeScroll + 11);
}
static void t_tree_check(void) {
    check("clicking a file in the tree opens it in a tab", strcmp(D->name, "Makefile") == 0 && D->lang == LANG_MAKE);
    tabBefore = cur; key(K_TAB, M_CTRL);
}
static void t_ctrl_tab(void) {
    check("Ctrl+Tab goes to the next tab", cur == (tabBefore + 1) % ntabs);
    key(K_TAB, M_CTRL | M_SHIFT);
}
static void t_ctrl_tab2(void) { check("Ctrl+Shift+Tab goes back", cur == tabBefore); }

static void t_new(void) { key(K_ESC, 0); key(K_N, M_CTRL); }
static void t_type(void) {
    check("new tab created", ntabs >= 3 && D->untitled);
    typ("int main() {\nreturn 0;");
}
static void t_type_check(void) {
    check("auto-close + auto-indent", text_is("int main() {\n    return 0;\n}"));
    if (!text_is("int main() {\n    return 0;\n}")) dump("typed");
    key(K_Z, M_CTRL);
}
static void t_undo_check(void) {
    check("undo removes the typing group", text_is("int main() {\n    \n}"));
    key(K_Y, M_CTRL);
}
static void t_redo_check(void) {
    check("redo restores it", text_is("int main() {\n    return 0;\n}"));
    /* Tab key comes through the event hook, not easyui */
    key(K_HOME, M_CTRL); key(K_TAB, 0);
}
static void t_tab_check(void) {
    check("Tab key reaches the editor", text_is("    int main() {\n    return 0;\n}"));
    key(K_TAB, M_SHIFT);
}
static void t_shifttab_check(void) {
    check("Shift+Tab unindents", text_is("int main() {\n    return 0;\n}"));
    key(K_A, M_CTRL); key(K_TAB, 0);
}
static void t_indent_sel_check(void) {
    check("Tab indents a multi-line selection", text_is("    int main() {\n        return 0;\n    }"));
    key(K_TAB, M_SHIFT);
}
static void t_unindent_sel_check(void) {
    check("Shift+Tab unindents the selection", text_is("int main() {\n    return 0;\n}"));
    check("selection kept", has_sel(D));
    key(K_Z, M_CTRL); key(K_Z, M_CTRL);
}
static void t_undo_blocks_check(void) {
    check("undo of two block edits", text_is("int main() {\n    return 0;\n}"));
    key(K_HOME, M_CTRL); key(K_DOWN, M_ALT);
}
static void t_move_check(void) {
    check("Alt+Down moves the line", text_is("    return 0;\nint main() {\n}"));
    key(K_UP, M_ALT); key(K_D, M_CTRL);
}
static void t_dup_check(void) {
    check("Ctrl+D duplicates the line", text_is("int main() {\nint main() {\n    return 0;\n}"));
    key(K_K, M_CTRL | M_SHIFT);
}
static void t_delline_check(void) {
    check("Ctrl+Shift+K deletes the line", text_is("int main() {\n    return 0;\n}"));
    key(K_A, M_CTRL); key(K_C, M_CTRL); key(K_END, M_CTRL); key(K_ENTER, 0); key(K_V, M_CTRL);
}
static void t_paste_check(void) {
    check("copy/paste", text_is("int main() {\n    return 0;\n}\nint main() {\n    return 0;\n}"));
    key(K_A, M_CTRL); typ("x"); key(K_Z, M_CTRL);
}
static void t_open_c(void) {
    check("typing over a selection, then undo", D->len > 20);
    open_file("0:/proj/util.h");
}
static void t_comment(void) {
    check("util.h is a C document", D->lang == LANG_C && !D->untitled);
    key(K_A, M_CTRL); key(K_SLASH, M_CTRL);
}
static void t_comment_check(void) {
    int l, ok = 1;
    for (l = 0; l < D->nl; l++) { int f = first_nonspace(D, l); if (f < line_end_(D, l) && !(D->text[f] == '/' && D->text[f + 1] == '/')) ok = 0; }
    check("Ctrl+/ comments every non-blank line", ok);
    check("document modified", D->modified);
    key(K_SLASH, M_CTRL);
}
static void t_uncomment_check(void) {
    int l, ok = 1;
    for (l = 0; l < D->nl; l++) { int f = first_nonspace(D, l); if (f < line_end_(D, l) && D->text[f] == '/' && D->text[f + 1] == '/') ok = 0; }
    check("Ctrl+/ again uncomments", ok);
    check("still modified", D->modified);
    key(K_Z, M_CTRL); key(K_Z, M_CTRL); key(K_Z, M_CTRL); key(K_Z, M_CTRL);   /* the comment toggles and the dirty-dot edits */
}
static void t_undo_to_clean(void) {
    check("undo to the saved state clears the dirty flag", !D->modified);
}
static void t_find(void) {
    key(K_HOME, M_CTRL); key(K_F, M_CTRL); typ("int");
}
static void t_find_check(void) {
    check("find: matches counted", nmatches >= 1);
    check("find: first match selected", has_sel(D) && sel_b(D) - sel_a(D) == 3);
    key(K_F3, 0);
}
static void t_find_next_check(void) {
    check("F3 moves to another match", has_sel(D));
    key(K_F3, M_SHIFT);
}
static void t_replace(void) {
    key(K_ESC, 0); key(K_H, M_CTRL);
}
static void t_replace2(void) {
    check("Ctrl+H opens replace", findOpen && replOpen);
    field_set(&fFind, "int"); find_refresh();
    focus = F_REPL; typ("long");
    key(K_ENTER, M_CTRL);
}
static void t_replace_check(void) {
    int i, ok = 1;
    for (i = 0; i + 3 <= D->len; i++) if (memcmp(D->text + i, "int", 3) == 0) ok = 0;
    check("Replace All replaced every match", ok);
    check("Replace All is one undo step", 1);
    key(K_Z, M_CTRL);
}
static void t_goto(void) { key(K_ESC, 0); key(K_G, M_CTRL); typ("3"); key(K_ENTER, 0); }
static void t_goto_check(void) {
    check("Go to line 3", line_of(D, D->caret) == 2 && !gotoOpen);
}
static void t_newfile(void) { open_file("0:/tmp/new.c"); }
static void t_newfile2(void) {
    check("a path that does not exist yet opens as a new file", D->lang == LANG_C && !D->untitled && D->len == 0);
    typ("int x = 1;");
    key(K_S, M_CTRL);
}
static void t_newfile_check(void) {
    char buf[64]; int n = read_file("tmp/new.c", buf, sizeof buf - 1);
    check("Ctrl+S wrote the file", n == 10 && memcmp(buf, "int x = 1;", 10) == 0);
    check("not modified after saving", !D->modified);
}
static void t_crlf(void) {
    write_file("tmp/crlf.txt", "one\r\ntwo\r\nthree\r\n", 17);
    open_file("0:/tmp/crlf.txt");
}
static void t_crlf2(void) {
    check("CRLF detected and stripped", D->crlf && text_is("one\ntwo\nthree\n"));
    key(K_END, M_CTRL); typ("x"); key(K_S, M_CTRL);
}
static void t_crlf_check(void) {
    char buf[64]; int n = read_file("tmp/crlf.txt", buf, sizeof buf - 1);
    check("CRLF written back", n == 18 && memcmp(buf, "one\r\ntwo\r\nthree\r\nx", 18) == 0);
}
static void t_binary(void) {
    static char bin[70000]; int i;
    for (i = 0; i < (int)sizeof bin; i++) bin[i] = (char)(i * 131 + 7);       /* NULs, high bytes, no newlines mostly */
    write_file("tmp/blob.bin", bin, (int)sizeof bin);
    open_file("0:/tmp/blob.bin");
}
static void t_binary2(void) {
    check("binary file opens", D->len == 70000);
    key(K_END, M_CTRL); key(K_PGUP, 0); key(K_HOME, M_CTRL); key(K_PGDN, 0); key(K_END, 0);
}
static void t_longline(void) {
    static char big[600000]; int i;
    for (i = 0; i < (int)sizeof big; i++) big[i] = (i % 97 == 96) ? ' ' : (char)('a' + i % 26);
    big[sizeof big - 1] = '\n';
    write_file("tmp/long.js", big, (int)sizeof big);
    open_file("0:/tmp/long.js");
}
static void t_longline2(void) {
    check("a 600 KB single line opens", D->len == 600000 && D->nl == 2);
    key(K_END, 0);
}
static void t_longline3(void) {
    check("End on a huge line scrolls horizontally", D->scrollX > 0);
    key(K_HOME, 0);
}
static void t_toolarge(void) {
    static char huge[(1 << 20) + 10];
    memset(huge, 'a', sizeof huge);
    write_file("tmp/huge.txt", huge, (int)sizeof huge);
    check("too large file is refused", !open_file("0:/tmp/huge.txt"));
}
static void t_mouse_open(void) { open_file("0:/proj/main.c"); }
static void t_mouse(void) {
    int off = D->ls[10] + 6;
    click(px_x(off), px_y(off));
}
static void t_mouse_check(void) {
    check("click puts the caret at the character", D->caret == D->ls[10] + 6 || D->caret == D->ls[10] + 5 || D->caret == D->ls[10] + 7);
    {   int off = D->ls[11] + 8; click(px_x(off), px_y(off)); click(px_x(off), px_y(off)); }
}
static void t_dbl_check(void) {
    check("double-click selects a word", has_sel(D) && sel_b(D) - sel_a(D) >= 2 && is_word((unsigned char)D->text[sel_a(D)]));
    {   int a = D->ls[14] + 4, b = D->ls[16] + 10; drag(px_x(a), px_y(a), px_x(b), px_y(b)); }
}
static void t_drag_check(void) {
    check("mouse drag selects across lines", has_sel(D) && line_of(D, sel_b(D)) - line_of(D, sel_a(D)) == 2);
    host_push_wheel(600, 300, -1);
}
static void t_wheel_check(void) {
    int y0 = D->scrollY;
    check("wheel down scrolls (animated)", D->targetY > y0 || D->scrollY > y0);
    host_push_wheel(600, 300, 1);
}
static void t_close_dirty(void) {
    key(K_END, M_CTRL); typ("zz");
}
static void t_close_dirty2(void) {
    check("typed into the file", D->modified);
    key(K_W, M_CTRL);
}
static void t_close_dirty3(void) {
    check("closing a dirty tab asks first", dlgKind == DLG_CLOSE);
    key(K_N, 0);
}
static void t_close_dirty4(void) {
    check("Don't Save closes the tab", dlgKind == DLG_NONE);
}
static void t_syntax(void) {
    /* syntax states: a block comment spanning lines, Python triple quotes */
    Doc* d = tab_new_doc(0); tab_activate(ntabs - 1);
    doc_set_path(d, "0:/tmp/s.c");
    { const char* src = "int a; /* start\nstill comment\nend */ int b;\n"; insert_text(src, (int)strlen(src), MG_NONE); }
    check("syntax: block comment state carries", line_state(d, 1) == ST_BLOCK && line_state(d, 2) == ST_BLOCK && line_state(d, 3) == ST_NORMAL);
    {
        int n = d->nl; (void)n;
        doc_edit(d, 0, 0, "/*", 2, 2, 2, MG_NONE);          /* editing line 0 invalidates the cached states */
        check("syntax: states recomputed after an edit", line_state(d, 1) == ST_BLOCK && line_state(d, 3) == ST_NORMAL);
    }
    {
        unsigned char col[64]; int st = scan_line(LANG_C, "#include <x.h>", 14, 0, col, 64);
        check("syntax: #include is preprocessor, <x.h> is a string", col[0] == HC_PREPROC && col[10] == HC_STRING && st == 0);
        st = scan_line(LANG_PY, "x = \"\"\"abc", 10, 0, col, 64);
        check("syntax: python triple-quote opens", st == ST_TRIPLE_DQ);
        st = scan_line(LANG_C, "if (x == 'a') return 0x1F;", 26, 0, col, 64);
        check("syntax: keyword, char, number", col[0] == HC_CONTROL && col[11] == HC_STRING && col[21] == HC_NUMBER);
    }
}
/* ---- extra screenshots (kept only with CODE_SHOT_KEEP) ---- */
#define OPEN_STEP(name, path) static void name(void) { open_file(path); }
#define SHOT_STEP(name, file) static void name(void) { shot(file); }
OPEN_STEP(x_open_py, "0:/proj/src/tool.py")   SHOT_STEP(x_shot_py, "lang_py.png")
OPEN_STEP(x_open_sh, "0:/proj/src/build.sh")  SHOT_STEP(x_shot_sh, "lang_sh.png")
OPEN_STEP(x_open_lua, "0:/proj/src/init.lua") SHOT_STEP(x_shot_lua, "lang_lua.png")
OPEN_STEP(x_open_json, "0:/proj/src/data.json") SHOT_STEP(x_shot_json, "lang_json.png")
OPEN_STEP(x_open_js, "0:/proj/src/app.js")    SHOT_STEP(x_shot_js, "lang_js.png")
OPEN_STEP(x_open_mk, "0:/proj/Makefile")      SHOT_STEP(x_shot_mk, "lang_make.png")
OPEN_STEP(x_open_md, "0:/proj/README.md")     SHOT_STEP(x_shot_md, "lang_md.png")
static void x_replace(void) { key(K_H, M_CTRL); }
static void x_replace2(void) { field_set(&fFind, "words"); focus = F_REPL; typ("W"); find_changed(); }
SHOT_STEP(x_shot_replace, "ui_replace.png")
static void x_goto(void) { key(K_ESC, 0); key(K_G, M_CTRL); typ("12"); }
SHOT_STEP(x_shot_goto, "ui_goto.png")
static void x_dialog(void) { key(K_ESC, 0); key(K_END, M_CTRL); typ("zz"); key(K_W, M_CTRL); }
SHOT_STEP(x_shot_dialog, "ui_dialog.png")
static void x_dialog2(void) { key(K_ESC, 0); open_file("0:/tmp/long.js"); }
SHOT_STEP(x_shot_hscroll, "ui_hscroll.png")
static void x_zoom(void) { key(K_EQUAL, M_CTRL); key(K_HOME, M_CTRL); }
SHOT_STEP(x_shot_zoom, "ui_zoom.png")
static void x_zoom2(void) { key(K_0, M_CTRL); key(K_B, M_CTRL); }
SHOT_STEP(x_shot_nosidebar, "ui_nosidebar.png")

/* ---- fuzz: random keys and mouse, then structural checks ---- */
static char evlog[800];
static void ev_add(const char* a, int v) { cat(evlog, sizeof evlog, a); cat_num(evlog, sizeof evlog, v); cat(evlog, sizeof evlog, " "); }
static unsigned rng = 12345u;
static int rnd(int n) { rng = rng * 1664525u + 1013904223u; return (int)((rng >> 8) % (unsigned)n); }
static char* fuzzBase; static int fuzzBaseLen, fuzzLeft = 150, fuzzBad;
#define SNAP_MAX 9000
static char* snapText[SNAP_MAX]; static int snapLen[SNAP_MAX];
static void snapshot(void) {                              /* the text after D->ecur edits, as it is now */
    int k = D->ecur;
    if (k < 0 || k >= SNAP_MAX || strcmp(D->name, "main.c") != 0) return;
    free(snapText[k]); snapText[k] = (char*)malloc((size_t)D->len + 1); memcpy(snapText[k], D->text, (size_t)D->len); snapLen[k] = D->len;
}
static void verify_docs(void) {
    int t, i, n;
    for (t = 0; t < ntabs; t++) {
        Doc* d = tabs[t];
        n = 1;
        for (i = 0; i < d->len; i++) if (d->text[i] == '\n') { if (d->ls[n] != i + 1 && !fuzzBad) { fuzzBad = 1; check("fuzz: line index matches the text", 0); } n++; }
        if (n != d->nl && !fuzzBad) { fuzzBad = 1; check("fuzz: line count matches the text", 0); }
        if ((d->caret < 0 || d->caret > d->len || d->anchor < 0 || d->anchor > d->len) && !fuzzBad) { fuzzBad = 1; check("fuzz: caret inside the text", 0); }
        if (d->text[d->len] != 0 && !fuzzBad) { fuzzBad = 1; check("fuzz: text is NUL terminated", 0); }
    }
}
static void fuzz_step(void) {
    static const char chars[] = "abcxyzQ019 (){}[]\"'`;:,./+-=<>!@#$%^&*_|~?\n    ";
    static const int nav[] = { K_ENTER, K_BACKSPACE, K_DELETE, K_TAB, K_LEFT, K_RIGHT, K_UP, K_DOWN, K_HOME, K_END, K_PGUP, K_PGDN, K_ESC };
    static const int ctl[] = { K_Z, K_Y, K_A, K_C, K_X, K_V, K_D, K_K, K_L, K_SLASH, K_RBRACKET, K_LBRACKET, K_F, K_H, K_G, K_B,
                               K_EQUAL, K_MINUS, K_HOME, K_END, K_LEFT, K_RIGHT, K_BACKSPACE, K_DELETE, K_ENTER, K_UP, K_DOWN };
    int e, W, H;
    host_get_size(&W, &H);
    evlog[0] = 0;
    for (e = 0; e < 14; e++) {
        int r = rnd(100);
        if (e && !host_getenv("CODE_SHOT_BATCH")) { easyui.update(); snapshot(); }
        if (r < 50) { char s[2]; s[0] = chars[rnd((int)sizeof chars - 1)]; s[1] = 0; typ(s); ev_add("c", s[0]); }
        else if (r < 70) { int k = nav[rnd((int)(sizeof nav / sizeof nav[0]))], m = rnd(4) == 0 ? M_ALT : rnd(3) == 0 ? M_SHIFT : 0; key(k, m); ev_add("K", k); ev_add("m", m); }
        else if (r < 82) { int k = ctl[rnd((int)(sizeof ctl / sizeof ctl[0]))], m = rnd(3) == 0 ? (M_CTRL | M_SHIFT) : M_CTRL; key(k, m); ev_add("K", k); ev_add("m", m); }
        else if (r < 90) {
            /* only inside the editor: the sidebar is at most SB_W_MAX wide, and clicking the tabs or the tree would change tab */
            int x0 = SB_W_MAX + 12, x = x0 + rnd(imax(10, W - x0 - 12)), y = TAB_H + 4 + rnd(imax(10, H - TAB_H - STATUS_H - 8));
            if (rnd(3) == 0) drag(x, y, x0 + rnd(imax(10, W - x0 - 12)), TAB_H + 4 + rnd(imax(10, H - TAB_H - STATUS_H - 8)));
            else click(x, y);
            ev_add("M", x); ev_add("y", y);
        }
        else if (r < 96) host_push_wheel(SB_W_MAX + 40, 200, rnd(2) ? 1 : -1);
        else if (r < 97 && rnd(8) == 0) host_push_resize(560 + rnd(600), 260 + rnd(400));
        else key(K_ESC, 0);
    }
    easyui.update(); snapshot();
    verify_docs();
    if (host_getenv("CODE_SHOT_ROUNDTRIP") && D->saveMark != -1) {   /* debugging aid: undo and redo everything after every tick */
        char* now = (char*)malloc((size_t)D->len + 1); int nowLen = D->len, ec = D->ecur, ok;
        memcpy(now, D->text, (size_t)D->len);
        while (D->ecur > 0) doc_undo(D);
        ok = D->len == fuzzBaseLen && memcmp(D->text, fuzzBase, (size_t)fuzzBaseLen) == 0;
        while (D->ecur < ec) doc_redo(D);
        if (ok) ok = D->len == nowLen && memcmp(D->text, now, (size_t)nowLen) == 0;
        if (!ok) { char m[100]; strlcpy_safe(m, "  roundtrip broke in tick ", sizeof m); cat_num(m, sizeof m, fuzzLeft); cat(m, sizeof m, ": "); host_log(m); host_log(evlog); host_log("\n"); fuzzLeft = 1; }
        free(now);
    }
}
static void fuzz_finish(void) {
    char* after; int afterLen, i;
    key(K_ESC, 0);
    /* the whole history backwards must give back the starting text, forwards the final text */
    close_overlays(); dlgKind = DLG_NONE;
    after = (char*)malloc((size_t)D->len + 1); afterLen = D->len; memcpy(after, D->text, (size_t)D->len);
    if (D->saveMark == -1) {                  /* a huge edit overflowed the undo memory: the history was dropped, as designed */
        host_log("  (fuzz: undo history was reset by an oversized edit; undo-all check skipped)\n");
        verify_docs(); free(after); return;
    }
    while (D->ecur > 0) {
        doc_undo(D);
        if (!host_getenv("CODE_SHOT_BATCH") && snapText[D->ecur] && (D->len != snapLen[D->ecur] || memcmp(D->text, snapText[D->ecur], (size_t)D->len))) {
            Edit* e = &D->ed[D->ecur]; char m[240]; int k = 0;
            while (k < D->len && k < snapLen[D->ecur] && D->text[k] == snapText[D->ecur][k]) k++;
            strlcpy_safe(m, "  undo mismatch at edit ", sizeof m); cat_num(m, sizeof m, D->ecur); cat(m, sizeof m, " (pos "); cat_num(m, sizeof m, e->pos);
            cat(m, sizeof m, " rem "); cat_num(m, sizeof m, e->remLen); cat(m, sizeof m, " ins "); cat_num(m, sizeof m, e->insLen);
            cat(m, sizeof m, "), first diff at "); cat_num(m, sizeof m, k); cat(m, sizeof m, ", len "); cat_num(m, sizeof m, D->len); cat(m, sizeof m, " vs "); cat_num(m, sizeof m, snapLen[D->ecur]); cat(m, sizeof m, "\n");
            host_log(m); break;
        }
    }
    while (D->ecur > 0) doc_undo(D);
    check("fuzz: undoing everything restores the original text", D->len == fuzzBaseLen && memcmp(D->text, fuzzBase, (size_t)fuzzBaseLen) == 0);
    if (D->len != fuzzBaseLen || memcmp(D->text, fuzzBase, (size_t)fuzzBaseLen)) {
        int k = 0; char m[200];
        while (k < D->len && k < fuzzBaseLen && D->text[k] == fuzzBase[k]) k++;
        strlcpy_safe(m, "  first difference at ", sizeof m); cat_num(m, sizeof m, k); cat(m, sizeof m, ", len "); cat_num(m, sizeof m, D->len);
        cat(m, sizeof m, " vs "); cat_num(m, sizeof m, fuzzBaseLen); cat(m, sizeof m, ", ecount "); cat_num(m, sizeof m, D->ecount); cat(m, sizeof m, "\n"); host_log(m);
    }
    while (D->ecur < D->ecount) doc_redo(D);
    check("fuzz: redoing everything gives the final text", D->len == afterLen && memcmp(D->text, after, (size_t)afterLen) == 0);
    for (i = 0; i < ntabs; i++) (void)i;
    verify_docs();
    free(after);
}
static void fuzz_begin(void) {
    key(K_ESC, 0);
    open_file("0:/proj/main.c");
    fuzzBaseLen = D->len; fuzzBase = (char*)malloc((size_t)D->len + 1); memcpy(fuzzBase, D->text, (size_t)D->len);
    D->ecount = D->ecur = 0; D->atop = 0; D->saveMark = 0;     /* history starts here */
    check("fuzz: starting on main.c", strcmp(D->name, "main.c") == 0);
}
static int quitTicks;
static void q_close(void) {
    int i, dirty = 0;
    for (i = 0; i < ntabs; i++) if (tabs[i]->modified) dirty++;
    close_overlays(); dlgKind = DLG_NONE;
    check("some tabs are modified before quitting", dirty > 0);
    host_push_close();
}
static void q_wait(void) {
    /* every modified tab is asked about; N = Don't Save; the app must exit by itself */
    check("closing the window asks about unsaved changes", dlgKind == DLG_QUIT || quitTicks > 0);
    if (dlgKind == DLG_QUIT) key(K_N, 0);
    quitTicks++;
}
static void t_quit(void) {
    { char m[80]; u2s(checks, m); cat(m, sizeof m, " checks, "); cat_num(m, sizeof m, failures); cat(m, sizeof m, " failures\n"); host_log(m); }
    host_exit(failures ? 1 : 0);
}

/* the screenshot script runs first so its tabs are the ones in the picture */
static void (*script[])(void) = {
    s_nop, s_nop, s_nop, s_shot_setup, s_shot_setup1, s_shot_setup2, s_nop, s_shot_setup3, s_nop, s_shot_final, s_nop,
    s_shot_final3, t_tree_click, t_tree_check, t_ctrl_tab, t_ctrl_tab2,
    t_new, t_type, t_type_check, t_undo_check, t_redo_check, t_tab_check, t_shifttab_check, t_indent_sel_check,
    t_unindent_sel_check, t_undo_blocks_check, t_move_check, t_dup_check, t_delline_check, t_paste_check,
    t_open_c, t_comment, t_comment_check, t_uncomment_check, t_undo_to_clean, t_find, t_find_check,
    t_find_next_check, t_replace, t_replace2, t_replace_check, t_goto, t_goto_check, t_newfile, t_newfile2,
    t_newfile_check, t_crlf, t_crlf2, t_crlf_check, t_binary, t_binary2, t_longline, t_longline2, t_longline3,
    t_toolarge, t_mouse_open, t_mouse, t_mouse_check, t_dbl_check, t_drag_check, t_wheel_check,
    t_close_dirty, t_close_dirty2, t_close_dirty3, t_close_dirty4, t_syntax,
    x_open_py, x_shot_py, x_open_sh, x_shot_sh, x_open_lua, x_shot_lua, x_open_json, x_shot_json, x_open_js, x_shot_js,
    x_open_mk, x_shot_mk, x_open_md, x_shot_md, x_replace, x_replace2, x_shot_replace, x_goto, x_shot_goto, x_dialog,
    s_nop, x_shot_dialog, x_dialog2, s_nop, x_shot_hscroll, x_zoom, s_nop, x_shot_zoom, x_zoom2, s_nop, x_shot_nosidebar,
    fuzz_begin
};
#define FUZZ_END_TICKS 0

static void on_tick(int frame) {
    int idx = frame - 1;
    if (idx >= 0 && idx < (int)(sizeof script / sizeof script[0])) script[idx]();
    else if (fuzzLeft > 0) { fuzz_step(); fuzzLeft--; }
    else if (fuzzLeft == 0) { fuzz_finish(); fuzzLeft = -1; }
    else if (fuzzLeft == -1) { q_close(); fuzzLeft = -2; }
    else if (fuzzLeft == -2) { q_wait(); if (quitTicks > 300) { check("the app quits after the dialogs", 0); t_quit(); } }
    else t_quit();
}

int main(int argc, char** argv) {
    char* args[8] = { (char*)"mos_code.run", (char*)"0:/proj", (char*)"0:/proj/main.c", (char*)"0:/proj/util.h", 0 };
    const char* root = host_getenv("CODE_HOST_ROOT");
    (void)argc; (void)argv;
    if (!root) { host_log("CODE_HOST_ROOT not set (use code_shot.sh)\n"); return 2; }
    if (host_getenv("CODE_SHOT_DIR")) shotDir = host_getenv("CODE_SHOT_DIR");
    if (host_getenv("CODE_SHOT_SEED")) { const char* sd = host_getenv("CODE_SHOT_SEED"); rng = 0; while (*sd >= '0' && *sd <= '9') rng = rng * 10u + (unsigned)(*sd++ - '0'); }
    if (host_getenv("CODE_SHOT_FUZZ")) { const char* sd = host_getenv("CODE_SHOT_FUZZ"); fuzzLeft = 0; while (*sd >= '0' && *sd <= '9') fuzzLeft = fuzzLeft * 10 + (*sd++ - '0'); }
    host_set_root(root);
    host_set_tick(on_tick);
    code_app_main(4, args);                               /* returns when the window is closed */
    check("the app quit by itself after Don't Save", quitTicks > 0);
    { char m[80]; u2s(checks, m); cat(m, sizeof m, " checks, "); cat_num(m, sizeof m, failures); cat(m, sizeof m, " failures\n"); host_log(m); }
    return failures ? 1 : 0;
}
