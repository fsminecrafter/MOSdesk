/*
 * Tasks - the MOSdesk task manager.
 *
 * Processes: every program with its CPU use, memory, threads and state
 * (SYS_TASK_LIST), sortable by any column; End task ends the selected one
 * (SYS_TASK_END, after a confirmation). Kernel tasks can be shown too, but
 * not ended.
 * Performance: CPU use over the last minute, per core, and memory.
 *
 * Keys: Up/Down select, Delete ends the selected program, Tab switches
 * page, Ctrl+K shows or hides kernel tasks, F5 refreshes now.
 */
#include "window.h"
#include "stdlib.h"
#include "string.h"
#include "font.h"
#include "user_sysinfo.h"

/* The SDK's string.h has no strcat family. */
static char* tm_strcat(char* d, const char* s) { strcpy(d + strlen(d), s); return d; }
static char* tm_strncat(char* d, const char* s, size_t n) {
    char* e = d + strlen(d);
    while (n-- && *s) *e++ = *s++;
    *e = 0;
    return d;
}
static char* tm_strncpy(char* d, const char* s, size_t n) {
    size_t i = 0;
    for (; i < n && s[i]; i++) d[i] = s[i];
    for (; i < n; i++) d[i] = 0;
    return d;
}
#define strcat tm_strcat
#define strncat tm_strncat
#define strncpy tm_strncpy

#define MAX_TASKS 256
#define HISTORY   60
#define TAB_H     40
#define HEAD_H    28
#define ROW_H     26
#define FOOT_H    48

/* Colours, as the other MOSdesk apps. */
#define C_BG       0xFF141C22u
#define C_PANEL    0xFF1B262Eu
#define C_ROW_ALT  0xFF18222Au
#define C_SEL      0xFF24425Au
#define C_HOVER    0xFF1F2E38u
#define C_LINE     0xFF2A3A44u
#define C_TEXT     0xFFE6EEF3u
#define C_DIM      0xFF8FA3AFu
#define C_ACCENT   0xFF4FA3E0u
#define C_ACCENT2  0xFF9B7BE8u
#define C_DANGER   0xFFE0566Au
#define C_OK       0xFF5CC48Au

enum { COL_NAME, COL_PID, COL_CPU, COL_MEM, COL_THREADS, COL_STATE, COLS };
static const char* const col_title[COLS] = { "Name", "PID", "CPU", "Memory", "Threads", "Status" };

typedef struct {
    syscall_task_info_t info;
    uint32_t cpu_tenths;            // CPU use over the last refresh, 0.1 % steps
    char label[64];                 // what the list shows (see pretty_name)
} task_t;

/* "0:/programs/mos_terminal.run@1000000005B [pid 8]" -> "Terminal": the
 * kernel names a program after its file and load address. Built-in apps
 * (mos_<name>.run) show as the launcher shows them; kernel tasks keep
 * their names. */
static void pretty_name(const syscall_task_info_t* t, char* out, size_t cap) {
    const char* s = t->name;
    size_t end = 0;
    while (s[end] && s[end] != '@' && s[end] != '[') end++;
    while (end && s[end - 1] == ' ') end--;
    size_t start = end;
    while (start && s[start - 1] != '/') start--;
    if (start == end) { start = 0; }
    size_t n = end - start;
    if (n > 4 && !memcmp(s + end - 4, ".run", 4)) n -= 4;
    const char* b = s + start;
    int builtin = n > 4 && !memcmp(b, "mos_", 4);
    if (builtin) { b += 4; n -= 4; }
    if (n >= cap) n = cap - 1;
    memcpy(out, b, n);
    out[n] = 0;
    if (builtin && out[0] >= 'a' && out[0] <= 'z') out[0] = (char)(out[0] - 'a' + 'A');
    if (t->flags & SYS_TASK_FLAG_WINDOW_MANAGER) {
        const char* d = "Desktop (mosWM)";
        size_t k = strlen(d);
        if (k < cap) memcpy(out, d, k + 1);
    }
    if (!out[0]) {
        size_t k = strlen(t->name);
        if (k >= cap) k = cap - 1;
        memcpy(out, t->name, k);
        out[k] = 0;
    }
}

static uint32_t window_id, *pixels, width = 720, height = 520;
static task_t tasks[MAX_TASKS], shown[MAX_TASKS];
static int task_count, shown_count;
static int page;                    // 0 processes, 1 performance
static int sort_col = COL_CPU, sort_desc = 1;
static int show_kernel;
static uint64_t selected_pid;
static int scroll;                  // rows scrolled
static int mouse_x = -1, mouse_y = -1;
static uint32_t buttons_down;
static int confirm;                 // asking whether to end selected_pid
static char message[96];
static uint64_t message_until;

/* Previous CPU times, for the per-refresh use. */
static struct { uint64_t pid, cpu_ms; } prev[MAX_TASKS];
static int prev_count;
static uint64_t prev_sample_ms;

static uint8_t cpu_hist[HISTORY], mem_hist[HISTORY];
static int hist_count;
static uint32_t cores;

/* ------------------------------------------------------------ drawing */

static void fill(int x, int y, int w, int h, uint32_t c) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)width) w = (int)width - x;
    if (y + h > (int)height) h = (int)height - y;
    if (w <= 0 || h <= 0) return;
    for (int j = 0; j < h; j++) {
        uint32_t* row = pixels + (size_t)(y + j) * width + x;
        for (int i = 0; i < w; i++) row[i] = c;
    }
}

static uint32_t blend(uint32_t a, uint32_t b, int t) {     // t of 256 toward b
    uint32_t r = (((a >> 16) & 255) * (256 - t) + ((b >> 16) & 255) * t) >> 8;
    uint32_t g = (((a >> 8) & 255) * (256 - t) + ((b >> 8) & 255) * t) >> 8;
    uint32_t bl = ((a & 255) * (256 - t) + (b & 255) * t) >> 8;
    return 0xFF000000u | (r << 16) | (g << 8) | bl;
}

static void round_rect(int x, int y, int w, int h, int r, uint32_t c) {
    for (int j = 0; j < h; j++) {
        int inset = 0;
        int dy = j < r ? r - j : (j >= h - r ? j - (h - r - 1) : 0);
        if (dy) {
            int dx = 0;
            while (dx * dx + dy * dy <= r * r) dx++;
            inset = r - dx + 1;
            if (inset < 0) inset = 0;
        }
        fill(x + inset, y + j, w - 2 * inset, 1, c);
    }
}

static void text(int x, int y, const char* s, uint32_t c) { draw_text(pixels, width, height, x, y, s, c, 1); }
static void text_big(int x, int y, const char* s, uint32_t c) { draw_text(pixels, width, height, x, y, s, c, 2); }

/* Text cut to fit `max` pixels, with "..." when it does not. */
static void text_fit(int x, int y, const char* s, int max, uint32_t c) {
    char buf[80];
    size_t n = strlen(s);
    if (n >= sizeof(buf)) n = sizeof(buf) - 1;
    memcpy(buf, s, n);
    buf[n] = 0;
    if (text_width(buf, 1) > max) {
        while (n > 0) {
            buf[--n] = 0;
            char t[84];
            memcpy(t, buf, n);
            memcpy(t + n, "...", 4);
            if (text_width(t, 1) <= max) { text(x, y, t, c); return; }
        }
        return;
    }
    text(x, y, buf, c);
}

static void fmt_u(char* out, uint64_t v) {
    char t[24];
    int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    for (int i = 0; i < n; i++) out[i] = t[n - 1 - i];
    out[n] = 0;
}

static void fmt_bytes(char* out, uint64_t b) {
    static const char* const unit[] = { "B", "KB", "MB", "GB", "TB" };
    int u = 0;
    uint64_t whole = b, frac = 0;
    while (whole >= 1024 && u < 4) { frac = (whole % 1024) * 10 / 1024; whole /= 1024; u++; }
    fmt_u(out, whole);
    size_t n = strlen(out);
    if (u > 0 && whole < 100) { out[n++] = '.'; out[n++] = (char)('0' + frac); }
    out[n++] = ' ';
    strcpy(out + n, unit[u]);
}

static void fmt_tenths(char* out, uint32_t t) {
    fmt_u(out, t / 10);
    size_t n = strlen(out);
    out[n++] = '.';
    out[n++] = (char)('0' + t % 10);
    out[n++] = ' ';
    out[n++] = '%';
    out[n] = 0;
}

static void fmt_duration(char* out, uint64_t ms) {
    uint64_t s = ms / 1000, h = s / 3600, m = s / 60 % 60;
    char t[24];
    out[0] = 0;
    if (h) { fmt_u(t, h); strcat(out, t); strcat(out, " h "); }
    fmt_u(t, m); strcat(out, t); strcat(out, " min ");
    fmt_u(t, s % 60); strcat(out, t); strcat(out, " s");
}

static const char* state_name(const syscall_task_info_t* t) {
    if (t->flags & SYS_TASK_FLAG_CRASHED) return "Crashed";
    switch (t->state) {
        case SYSCALL_PROC_STATE_READY: return "Ready";
        case SYSCALL_PROC_STATE_RUNNING: return "Running";
        case SYSCALL_PROC_STATE_WAITING: return "Waiting";
        case SYSCALL_PROC_STATE_PAUSED: return "Paused";
        case SYSCALL_PROC_STATE_ZOMBIE: return "Ending";
        default: return "Ended";
    }
}

static int is_user(const syscall_task_info_t* t) { return t->privilege == SYSCALL_PROC_PRIV_USER; }
static uint64_t own_pid;
static int can_end(const syscall_task_info_t* t) {
    return is_user(t) && !(t->flags & SYS_TASK_FLAG_WINDOW_MANAGER) && t->state != SYSCALL_PROC_STATE_ZOMBIE &&
           t->pid != own_pid;
}

static void say(const char* s, uint64_t now) {
    strncpy(message, s, sizeof(message) - 1);
    message[sizeof(message) - 1] = 0;
    message_until = now + 4000;
}

/* ------------------------------------------------------------- data */

static int compare(const task_t* a, const task_t* b) {
    int r = 0;
    switch (sort_col) {
        case COL_NAME: r = strcmp(a->label, b->label); break;
        case COL_PID: r = a->info.pid < b->info.pid ? -1 : a->info.pid > b->info.pid; break;
        case COL_CPU: r = (int)a->cpu_tenths - (int)b->cpu_tenths; break;
        case COL_MEM: r = a->info.memory_bytes < b->info.memory_bytes ? -1 : a->info.memory_bytes > b->info.memory_bytes; break;
        case COL_THREADS: r = (int)a->info.threads - (int)b->info.threads; break;
        case COL_STATE: r = strcmp(state_name(&a->info), state_name(&b->info)); break;
    }
    if (!r) r = a->info.pid < b->info.pid ? -1 : a->info.pid > b->info.pid;
    return sort_desc ? -r : r;
}

static void rebuild_view(void) {
    shown_count = 0;
    for (int i = 0; i < task_count; i++)
        if (show_kernel || is_user(&tasks[i].info)) shown[shown_count++] = tasks[i];
    for (int i = 1; i < shown_count; i++) {             // insertion sort: a few dozen rows
        task_t t = shown[i];
        int j = i - 1;
        while (j >= 0 && compare(&shown[j], &t) > 0) { shown[j + 1] = shown[j]; j--; }
        shown[j + 1] = t;
    }
}

static void refresh(uint64_t now) {
    static syscall_task_info_t list[MAX_TASKS];
    long n = mos_task_list(list, MAX_TASKS);
    if (n < 0) n = 0;
    uint64_t dt = prev_sample_ms ? now - prev_sample_ms : 0;
    cores = (uint32_t)mos_sysinfo_core_count();
    if (!cores) cores = 1;
    task_count = 0;
    for (long i = 0; i < n; i++) {
        task_t* t = &tasks[task_count++];
        t->info = list[i];
        t->cpu_tenths = 0;
        pretty_name(&t->info, t->label, sizeof(t->label));
        for (int k = 0; k < prev_count && dt; k++) {
            if (prev[k].pid != list[i].pid) continue;
            uint64_t used = list[i].cpu_time_ms > prev[k].cpu_ms ? list[i].cpu_time_ms - prev[k].cpu_ms : 0;
            uint64_t tenths = used * 1000 / (dt * cores);
            t->cpu_tenths = (uint32_t)(tenths > 1000 ? 1000 : tenths);
            break;
        }
    }
    prev_count = 0;
    for (int i = 0; i < task_count; i++) {
        prev[prev_count].pid = tasks[i].info.pid;
        prev[prev_count].cpu_ms = tasks[i].info.cpu_time_ms;
        prev_count++;
    }
    prev_sample_ms = now;

    // History for the graphs.
    uint64_t cpu = mos_sysinfo_cpu_usage();
    uint64_t total = mos_sysinfo_ram_total(), used = mos_sysinfo_ram_used();
    if (hist_count == HISTORY) {
        memmove(cpu_hist, cpu_hist + 1, HISTORY - 1);
        memmove(mem_hist, mem_hist + 1, HISTORY - 1);
        hist_count--;
    }
    cpu_hist[hist_count] = (uint8_t)(cpu > 100 ? 100 : cpu);
    mem_hist[hist_count] = (uint8_t)(total ? used * 100 / total : 0);
    hist_count++;
    rebuild_view();
}

static int selected_index(void) {
    for (int i = 0; i < shown_count; i++)
        if (shown[i].info.pid == selected_pid) return i;
    return -1;
}

/* ----------------------------------------------------------- layout */

static int col_x[COLS + 1];

static void layout_columns(void) {
    // Name takes what the others leave.
    static const int fixed[COLS] = { 0, 64, 72, 96, 72, 90 };
    int rest = 0;
    for (int c = 1; c < COLS; c++) rest += fixed[c];
    int name_w = (int)width - 24 - rest;
    if (name_w < 120) name_w = 120;
    col_x[0] = 12;
    col_x[1] = col_x[0] + name_w;
    for (int c = 1; c < COLS; c++) col_x[c + 1] = col_x[c] + fixed[c];
}

static int list_top(void) { return TAB_H + HEAD_H; }
static int list_rows(void) {
    int h = (int)height - list_top() - FOOT_H;
    return h > 0 ? h / ROW_H : 0;
}

static void clamp_scroll(void) {
    int max = shown_count - list_rows();
    if (max < 0) max = 0;
    if (scroll > max) scroll = max;
    if (scroll < 0) scroll = 0;
}

static void ensure_visible(void) {
    int i = selected_index();
    if (i < 0) return;
    if (i < scroll) scroll = i;
    if (i >= scroll + list_rows()) scroll = i - list_rows() + 1;
    clamp_scroll();
}

typedef struct { int x, y, w, h; } rect_t;
static int inside(rect_t r, int x, int y) { return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h; }

static rect_t tab_rect(int i) { return (rect_t){ 12 + i * 128, 6, 120, TAB_H - 10 }; }
static rect_t end_button(void) { return (rect_t){ (int)width - 132, (int)height - FOOT_H + 9, 120, 30 }; }
static rect_t kernel_toggle(void) { return (rect_t){ 12, (int)height - FOOT_H + 13, 180, 22 }; }
static rect_t confirm_yes(void) { return (rect_t){ (int)width - 252, (int)height - FOOT_H + 9, 120, 30 }; }
static rect_t confirm_no(void) { return (rect_t){ (int)width - 124, (int)height - FOOT_H + 9, 112, 30 }; }

/* ------------------------------------------------------------ pages */

static void draw_button(rect_t r, const char* label, uint32_t bg, uint32_t fg, int enabled) {
    int hover = enabled && inside(r, mouse_x, mouse_y);
    round_rect(r.x, r.y, r.w, r.h, 6, enabled ? (hover ? blend(bg, 0xFFFFFFFF, 30) : bg) : C_PANEL);
    text(r.x + (r.w - text_width(label, 1)) / 2, r.y + (r.h - text_height(1)) / 2, label, enabled ? fg : C_DIM);
}

static void draw_tabs(void) {
    fill(0, 0, (int)width, TAB_H, C_PANEL);
    static const char* const names[2] = { "Processes", "Performance" };
    for (int i = 0; i < 2; i++) {
        rect_t r = tab_rect(i);
        int on = page == i, hover = inside(r, mouse_x, mouse_y);
        if (on || hover) round_rect(r.x, r.y, r.w, r.h, 6, on ? C_BG : C_HOVER);
        text(r.x + (r.w - text_width(names[i], 1)) / 2, r.y + 8, names[i], on ? C_TEXT : C_DIM);
        if (on) fill(r.x + 16, r.y + r.h - 3, r.w - 32, 2, C_ACCENT);
    }
    char line[64], num[24];
    fmt_u(num, (uint64_t)task_count);
    strcpy(line, num);
    strcat(line, " tasks");
    text((int)width - 12 - text_width(line, 1), 14, line, C_DIM);
}

static void draw_processes(void) {
    layout_columns();
    int y0 = TAB_H;
    fill(0, y0, (int)width, HEAD_H, C_BG);
    for (int c = 0; c < COLS; c++) {
        char head[24];
        strcpy(head, col_title[c]);
        if (c == sort_col) strcat(head, sort_desc ? "  v" : "  ^");
        int tx = c == COL_NAME ? col_x[c] + 4 : col_x[c + 1] - 8 - text_width(head, 1);
        if (c == COL_STATE) tx = col_x[c] + 8;
        text(tx, y0 + 7, head, c == sort_col ? C_TEXT : C_DIM);
    }
    fill(0, y0 + HEAD_H - 1, (int)width, 1, C_LINE);

    int rows = list_rows(), top = list_top();
    clamp_scroll();
    for (int r = 0; r < rows; r++) {
        int i = scroll + r, y = top + r * ROW_H;
        uint32_t bg = (r & 1) ? C_ROW_ALT : C_BG;
        if (i < shown_count) {
            const task_t* t = &shown[i];
            int sel = t->info.pid == selected_pid;
            int hover = mouse_y >= y && mouse_y < y + ROW_H && mouse_x >= 0;
            if (sel) bg = C_SEL;
            else if (hover) bg = C_HOVER;
            fill(0, y, (int)width, ROW_H, bg);
            uint32_t fg = is_user(&t->info) ? C_TEXT : C_DIM;
            int ty = y + (ROW_H - text_height(1)) / 2;
            // A small marker: blue for programs, grey for kernel tasks.
            round_rect(col_x[0] + 4, y + 9, 8, 8, 4, is_user(&t->info) ? C_ACCENT : C_LINE);
            text_fit(col_x[0] + 18, ty, t->label, col_x[1] - col_x[0] - 26, fg);
            char v[32];
            fmt_u(v, t->info.pid);
            text(col_x[2] - 8 - text_width(v, 1), ty, v, C_DIM);
            fmt_tenths(v, t->cpu_tenths);
            // CPU cell tinted by how busy it is.
            if (t->cpu_tenths >= 50) {
                int heat = (int)(t->cpu_tenths > 500 ? 500 : t->cpu_tenths) * 120 / 500;
                fill(col_x[2], y + 1, col_x[3] - col_x[2], ROW_H - 2, blend(bg, C_ACCENT2, heat));
            }
            text(col_x[3] - 8 - text_width(v, 1), ty, v, fg);
            if (is_user(&t->info)) fmt_bytes(v, t->info.memory_bytes);
            else strcpy(v, "-");
            text(col_x[4] - 8 - text_width(v, 1), ty, v, fg);
            fmt_u(v, (uint64_t)t->info.threads + 1u);
            text(col_x[5] - 8 - text_width(v, 1), ty, v, C_DIM);
            const char* st = state_name(&t->info);
            uint32_t sc = (t->info.flags & SYS_TASK_FLAG_CRASHED) ? C_DANGER
                        : t->info.state == SYSCALL_PROC_STATE_RUNNING ? C_OK : C_DIM;
            text(col_x[5] + 8, ty, st, sc);
        } else {
            fill(0, y, (int)width, ROW_H, bg);
        }
    }
    int list_end = top + rows * ROW_H;
    fill(0, list_end, (int)width, (int)height - list_end, C_BG);
    // Scrollbar.
    if (shown_count > rows && rows > 0) {
        int track = rows * ROW_H;
        int th = track * rows / shown_count;
        if (th < 24) th = 24;
        int ty = top + (track - th) * scroll / (shown_count - rows);
        round_rect((int)width - 7, ty, 4, th, 2, C_LINE);
    }

    // Footer: kernel tasks toggle, the message or the confirmation, End task.
    int fy = (int)height - FOOT_H;
    fill(0, fy, (int)width, FOOT_H, C_PANEL);
    fill(0, fy, (int)width, 1, C_LINE);
    rect_t k = kernel_toggle();
    round_rect(k.x, k.y + 3, 16, 16, 4, show_kernel ? C_ACCENT : C_LINE);
    if (show_kernel) { fill(k.x + 4, k.y + 10, 3, 3, C_TEXT); fill(k.x + 7, k.y + 8, 5, 3, C_TEXT); }
    text(k.x + 24, k.y + 4, "Show kernel tasks", inside(k, mouse_x, mouse_y) ? C_TEXT : C_DIM);
    int sel = selected_index();
    if (confirm && sel >= 0) {
        char q[96];
        strcpy(q, "End ");
        strncat(q, shown[sel].label, 40);
        strcat(q, "? Unsaved work is lost.");
        text_fit(k.x + 200, fy + 16, q, confirm_yes().x - k.x - 212, C_TEXT);
        draw_button(confirm_yes(), "End task", C_DANGER, 0xFFFFFFFF, 1);
        draw_button(confirm_no(), "Cancel", C_LINE, C_TEXT, 1);
        return;
    }
    if (message[0]) text_fit(k.x + 200, fy + 16, message, end_button().x - k.x - 212, C_DIM);
    draw_button(end_button(), "End task", C_DANGER, 0xFFFFFFFF, sel >= 0 && can_end(&shown[sel].info));
}

static void graph(int x, int y, int w, int h, const uint8_t* hist, int n, uint32_t color, const char* title,
                  const char* value) {
    round_rect(x, y, w, h, 8, C_PANEL);
    text(x + 14, y + 10, title, C_DIM);
    text_big(x + 14, y + 28, value, C_TEXT);
    int gx = x + 14, gy = y + 64, gw = w - 28, gh = h - 78;
    if (gw < 20 || gh < 20) return;
    fill(gx, gy, gw, gh, C_BG);
    for (int i = 1; i < 4; i++) fill(gx, gy + gh * i / 4, gw, 1, C_ROW_ALT);
    if (n < 1) return;
    // Area chart: newest sample on the right.
    for (int px = 0; px < gw; px++) {
        float pos = (float)(HISTORY - 1) * (float)px / (float)(gw - 1) - (float)(HISTORY - n);
        if (pos < 0) continue;
        int i0 = (int)pos, i1 = i0 + 1 < n ? i0 + 1 : i0;
        float f = pos - (float)i0;
        float v = (float)hist[i0] * (1.0f - f) + (float)hist[i1] * f;
        int bar = (int)(v * (float)gh / 100.0f);
        if (bar <= 0) continue;
        fill(gx + px, gy + gh - bar, 1, bar, blend(C_BG, color, 70));
        fill(gx + px, gy + gh - bar, 1, 2, color);
    }
}

static void draw_performance(void) {
    fill(0, TAB_H, (int)width, (int)height - TAB_H, C_BG);
    int pad = 12, y = TAB_H + pad;
    int w = (int)width - 2 * pad;
    int half = (w - pad) / 2;
    int gh = ((int)height - y - pad) * 55 / 100;
    if (gh < 120) gh = 120;
    char v[48], t[48];
    fmt_u(v, hist_count ? cpu_hist[hist_count - 1] : 0);
    strcat(v, " %");
    graph(pad, y, half, gh, cpu_hist, hist_count, C_ACCENT, "CPU", v);
    uint64_t total = mos_sysinfo_ram_total(), used = mos_sysinfo_ram_used();
    fmt_bytes(v, used);
    strcat(v, " / ");
    fmt_bytes(t, total);
    strcat(v, t);
    graph(pad * 2 + half, y, w - half - pad, gh, mem_hist, hist_count, C_ACCENT2, "Memory", v);

    // Cores and figures.
    y += gh + pad;
    int ph = (int)height - y - pad;
    if (ph < 40) return;
    round_rect(pad, y, half, ph, 8, C_PANEL);
    text(pad + 14, y + 10, "Cores", C_DIM);
    uint32_t n = cores ? cores : 1;
    int cols = n > 8 ? 2 : 1;
    int per_col = (int)((n + (uint32_t)cols - 1) / (uint32_t)cols);
    int bar_w = (half - 28 - (cols - 1) * 12) / cols;
    int row_h = per_col ? (ph - 36) / per_col : 0;
    if (row_h > 22) row_h = 22;
    for (uint32_t c = 0; c < n && row_h >= 8; c++) {
        int cx = pad + 14 + (int)(c / (uint32_t)per_col) * (bar_w + 12);
        int cy = y + 32 + (int)(c % (uint32_t)per_col) * row_h;
        uint64_t u = mos_sysinfo_core_usage(c);
        if (u > 100) u = 100;
        char label[16];
        fmt_u(label, c);
        text(cx, cy + (row_h - text_height(1)) / 2, label, C_DIM);
        int bx = cx + 26, bw = bar_w - 26 - 44;
        if (bw < 10) continue;
        round_rect(bx, cy + row_h / 2 - 4, bw, 8, 4, C_BG);
        int fw = (int)(u * (uint64_t)bw / 100);
        if (fw > 0) round_rect(bx, cy + row_h / 2 - 4, fw < 8 ? 8 : fw, 8, 4, C_ACCENT);
        fmt_u(label, u);
        strcat(label, "%");
        text(bx + bw + 8, cy + (row_h - text_height(1)) / 2, label, C_TEXT);
    }

    int fx = pad * 2 + half, fw = w - half - pad;
    round_rect(fx, y, fw, ph, 8, C_PANEL);
    struct { const char* k; char v[48]; } rows[6];
    int count = 0;
    rows[count].k = "Up time"; fmt_duration(rows[count].v, mos_sysinfo_uptime_ms()); count++;
    rows[count].k = "Tasks"; fmt_u(rows[count].v, (uint64_t)task_count); count++;
    rows[count].k = "Cores"; fmt_u(rows[count].v, n); count++;
    rows[count].k = "Free memory"; fmt_bytes(rows[count].v, mos_sysinfo_ram_free()); count++;
    rows[count].k = "Kernel heap"; fmt_bytes(rows[count].v, mos_sysinfo_heap_used()); count++;
    for (int i = 0; i < count; i++) {
        int ry = y + 12 + i * 22;
        if (ry + 18 > y + ph) break;
        text(fx + 14, ry, rows[i].k, C_DIM);
        text(fx + fw - 14 - text_width(rows[i].v, 1), ry, rows[i].v, C_TEXT);
    }
}

static void draw(void) {
    draw_tabs();
    if (page == 0) draw_processes();
    else draw_performance();
    mos_window_present(window_id, pixels, width * 4, 0, 0, width, height);
}

/* ------------------------------------------------------------ input */

static void end_selected(uint64_t now) {
    int i = selected_index();
    if (i < 0) return;
    long r = mos_task_end(shown[i].info.pid);
    char m[96];
    strcpy(m, r == 0 ? "Ended " : "Could not end ");
    strncat(m, shown[i].label, 60);
    if (r == (long)SYS_ERR_PERM) strcat(m, " (not allowed)");
    say(m, now);
    confirm = 0;
    refresh(now);
}

static void ask_end(uint64_t now) {
    int i = selected_index();
    if (i < 0) return;
    if (shown[i].info.pid == own_pid) { say("Close Tasks with its close button.", now); return; }
    if (!can_end(&shown[i].info)) { say("Kernel tasks and the desktop cannot be ended.", now); return; }
    confirm = 1;
}

static void move_selection(int delta) {
    if (!shown_count) return;
    int i = selected_index();
    i = i < 0 ? 0 : i + delta;
    if (i < 0) i = 0;
    if (i >= shown_count) i = shown_count - 1;
    selected_pid = shown[i].info.pid;
    confirm = 0;
    ensure_visible();
}

static void click(int x, int y, uint64_t now) {
    for (int i = 0; i < 2; i++)
        if (inside(tab_rect(i), x, y)) { page = i; confirm = 0; return; }
    if (page != 0) return;
    if (confirm) {
        if (inside(confirm_yes(), x, y)) { end_selected(now); return; }
        if (inside(confirm_no(), x, y)) { confirm = 0; return; }
    }
    if (inside(kernel_toggle(), x, y)) { show_kernel = !show_kernel; rebuild_view(); return; }
    if (inside(end_button(), x, y)) { ask_end(now); return; }
    if (y >= TAB_H && y < TAB_H + HEAD_H) {
        for (int c = 0; c < COLS; c++) {
            if (x < col_x[c] || x >= col_x[c + 1]) continue;
            if (sort_col == c) sort_desc = !sort_desc;
            else { sort_col = c; sort_desc = c != COL_NAME; }
            rebuild_view();
        }
        return;
    }
    int top = list_top();
    if (y >= top && y < top + list_rows() * ROW_H) {
        int i = scroll + (y - top) / ROW_H;
        if (i < shown_count) { selected_pid = shown[i].info.pid; confirm = 0; }
    }
}

int main(void) {
    own_pid = (uint64_t)mos_syscall(SYS_GETPID, 0, 0, 0);
    window_id = mos_window_create(0, 180, 80, width, height, "Tasks");
    if (!window_id) return 1;
    pixels = malloc((size_t)width * height * 4);
    if (!pixels) return 1;
    uint64_t now = mos_sysinfo_uptime_ms();
    refresh(now);
    uint64_t next_refresh = now + 1000;
    int dirty = 1;
    for (;;) {
        syscall_window_event_t ev;
        now = mos_sysinfo_uptime_ms();
        while (mos_window_next_event(window_id, &ev) == SYS_SUCCESS) {
            dirty = 1;
            switch (ev.type) {
                case SYSCALL_WINDOW_EVENT_CLOSE_REQUEST:
                    mos_window_close(window_id);
                    return 0;
                case SYSCALL_WINDOW_EVENT_RESIZED: {
                    if (ev.width < 200 || ev.height < 160) break;
                    uint32_t* p = realloc(pixels, (size_t)ev.width * ev.height * 4);
                    if (!p) return 1;
                    pixels = p;
                    width = (uint32_t)ev.width;
                    height = (uint32_t)ev.height;
                    break;
                }
                case SYSCALL_WINDOW_EVENT_MOUSE: {
                    mouse_x = ev.x;
                    mouse_y = ev.y;
                    uint32_t pressed = ev.buttons & ~buttons_down;
                    buttons_down = ev.buttons;
                    if (pressed & SYSCALL_MOUSE_BTN_LEFT) click(ev.x, ev.y, now);
                    break;
                }
                case SYSCALL_WINDOW_EVENT_FOCUS_LOST:
                    mouse_x = mouse_y = -1;
                    buttons_down = 0;
                    break;
                case SYSCALL_WINDOW_EVENT_MOUSE_WHEEL:
                    scroll -= ev.wheel * 3;
                    clamp_scroll();
                    break;
                case SYSCALL_WINDOW_EVENT_KEY: {
                    if (!ev.buttons) break;                 // key up
                    uint32_t key = ev.key;
                    int ctrl = (ev.mods & SYSCALL_INPUT_MOD_CTRL) != 0;
                    if (key == 43) { page = !page; confirm = 0; }                       // Tab
                    else if (key == 62) refresh(now);                                   // F5
                    else if (ctrl && key == 14) { show_kernel = !show_kernel; rebuild_view(); }   // Ctrl+K
                    else if (page == 0 && key == 82) move_selection(-1);                // Up
                    else if (page == 0 && key == 81) move_selection(1);                 // Down
                    else if (page == 0 && key == 75) move_selection(-list_rows());      // Page Up
                    else if (page == 0 && key == 78) move_selection(list_rows());       // Page Down
                    else if (page == 0 && key == 76) {                                  // Delete
                        if (confirm) end_selected(now); else ask_end(now);
                    }
                    else if (confirm && key == 40) end_selected(now);                   // Enter
                    else if (key == 41) confirm = 0;                                    // Escape
                    break;
                }
                default:
                    break;
            }
        }
        if (now >= next_refresh) {
            refresh(now);
            next_refresh = now + 1000;
            dirty = 1;
        }
        if (message[0] && now >= message_until) { message[0] = 0; dirty = 1; }
        if (dirty) { draw(); dirty = 0; }
        mos_sleep(30);
    }
}
