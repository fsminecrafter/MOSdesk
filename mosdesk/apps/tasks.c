/*
 * Tasks - the MOSdesk task manager.
 *
 * Processes: every program with its CPU use, memory, threads and state
 * (SYS_TASK_LIST), sortable by any column; End task ends the selected one
 * (SYS_TASK_END, after a confirmation). Kernel tasks can be shown too, but
 * not ended.
 * Performance: a list on the left (CPU, Memory, GPU 0, each with a small
 * graph) and the one picked on the right: CPU use over the last minute
 * or per core (right-click the graph: Overall utilization / Logical processors,
 * each core with the two programs that used it most), the processor's details
 * (SYS_TASK_CPU); memory; the graphics card's engines (3D, Copy, Video
 * Decode, Video Encode) and its dedicated memory (SYS_TASK_GPU).
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

/* SYS_TASK_GPU, for SDKs from before it (the kernel refuses it then, and the GPU page says so). */
#ifndef SYS_TASK_GPU
#define SYS_TASK_GPU 3u
#define SYS_GPU_ENGINES 4u
#define SYS_GPU_STATS_ENGINES 1u
#define SYS_GPU_STATS_HW_3D 2u
typedef struct {
    uint32_t size, flags;
    char name[64];
    char renderer[32];
    uint64_t vram_total, vram_used;
    uint64_t now_ns;
    uint64_t engine_busy_ns[SYS_GPU_ENGINES];
    uint64_t engine_jobs[SYS_GPU_ENGINES];
    uint64_t cpu_render_ns;
} syscall_gpu_stats_t;
#endif

/* SYS_TASK_CPU, for SDKs from before it (the kernel refuses it then: usage per core only, no names). */
#ifndef SYS_TASK_CPU
#define SYS_TASK_CPU 4u
#define SYS_CPU_TOP 4u
#define SYS_CPU_MAX_CORES 64u
#define SYS_CPU_FEAT_SSE42   (1u << 0)
#define SYS_CPU_FEAT_AVX     (1u << 1)
#define SYS_CPU_FEAT_AVX2    (1u << 2)
#define SYS_CPU_FEAT_AVX512  (1u << 3)
#define SYS_CPU_FEAT_AES     (1u << 4)
#define SYS_CPU_FEAT_FMA     (1u << 5)
#define SYS_CPU_FEAT_VM      (1u << 6)
typedef struct { uint64_t pid; uint32_t percent; uint32_t reserved; char name[32]; } syscall_cpu_top_t;
typedef struct { uint32_t usage; uint32_t top_count; syscall_cpu_top_t top[SYS_CPU_TOP]; } syscall_core_stats_t;
typedef struct {
    uint32_t size, cores;
    char brand[52], vendor[16], hypervisor[16];
    uint32_t family, model, stepping, base_mhz;
    uint32_t l1d_kb, l1i_kb, l2_kb, l3_kb;
    uint32_t features;
    uint64_t uptime_ms;
    syscall_core_stats_t core[SYS_CPU_MAX_CORES];
} syscall_cpu_stats_t;
#endif

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

/* The graphics card: engine use (3D, Copy, Video Decode, Video Encode, then FarcadeGL drawn on the CPU) and
 * dedicated memory, per refresh. */
enum { GE_3D, GE_COPY, GE_DECODE, GE_ENCODE, GE_CPU, GE_COUNT };
static const char* const ge_name[GE_COUNT] = { "3D", "Copy", "Video Decode", "Video Encode", "FarcadeGL on the CPU" };
static uint8_t gpu_hist[GE_COUNT][HISTORY], vram_hist[HISTORY], gpu_top_hist[HISTORY];
static syscall_gpu_stats_t gpu_now, gpu_prev;
static int gpu_ok, gpu_have_prev;
static uint32_t gpu_jobs_per_s;
static int perf_view;               // 0 CPU, 1 memory, 2 GPU
// The CPU page: SYS_TASK_CPU (the processor, and per core its usage and busiest programs), a history per core,
// and which graph shows (right-click menu, like Windows: one overall graph or one per logical processor).
static syscall_cpu_stats_t cpu_stats;
static int cpu_ok;
static uint8_t core_hist[SYS_CPU_MAX_CORES][HISTORY];
static int cpu_graph_mode;          // 0 overall utilization, 1 logical processors
static int menu_open, menu_x, menu_y;
static int cg_x, cg_y, cg_w, cg_h;   // where the CPU graph is (right-click opens the menu there)

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

    // History for the graphs: when full, every graph moves one sample left.
    uint64_t cpu = mos_sysinfo_cpu_usage();
    uint64_t total = mos_sysinfo_ram_total(), used = mos_sysinfo_ram_used();
    if (hist_count == HISTORY) {
        memmove(cpu_hist, cpu_hist + 1, HISTORY - 1);
        memmove(mem_hist, mem_hist + 1, HISTORY - 1);
        for (int e = 0; e < GE_COUNT; e++) memmove(gpu_hist[e], gpu_hist[e] + 1, HISTORY - 1);
        memmove(vram_hist, vram_hist + 1, HISTORY - 1);
        memmove(gpu_top_hist, gpu_top_hist + 1, HISTORY - 1);
        for (uint32_t c = 0; c < SYS_CPU_MAX_CORES; c++) memmove(core_hist[c], core_hist[c] + 1, HISTORY - 1);
        hist_count--;
    }
    cpu_hist[hist_count] = (uint8_t)(cpu > 100 ? 100 : cpu);
    mem_hist[hist_count] = (uint8_t)(total ? used * 100 / total : 0);
    // The cores: usage and their busiest programs (an older kernel: usage only, from SYS_SYSINFO).
    memset(&cpu_stats.size, 0, sizeof cpu_stats.size);
    cpu_stats.size = sizeof cpu_stats;
    cpu_ok = mos_syscall(SYS_TASK, SYS_TASK_CPU, (uint64_t)(uintptr_t)&cpu_stats, 0) == SYS_SUCCESS;
    if (!cpu_ok) {
        cpu_stats.cores = cores > SYS_CPU_MAX_CORES ? SYS_CPU_MAX_CORES : cores;
        for (uint32_t c = 0; c < cpu_stats.cores; c++) {
            uint64_t u = mos_sysinfo_core_usage(c);
            cpu_stats.core[c].usage = (uint32_t)(u > 100 ? 100 : u);
            cpu_stats.core[c].top_count = 0;
        }
    }
    for (uint32_t c = 0; c < cpu_stats.cores && c < SYS_CPU_MAX_CORES; c++)
        core_hist[c][hist_count] = (uint8_t)(cpu_stats.core[c].usage > 100 ? 100 : cpu_stats.core[c].usage);

    // The graphics card: usage = busy time since the last refresh / the time between.
    uint8_t ge[GE_COUNT] = { 0 };
    syscall_gpu_stats_t g;
    memset(&g, 0, sizeof g);
    g.size = sizeof g;
    gpu_ok = mos_syscall(SYS_TASK, SYS_TASK_GPU, (uint64_t)(uintptr_t)&g, 0) == SYS_SUCCESS;
    if (gpu_ok) {
        if (gpu_have_prev && g.now_ns > gpu_prev.now_ns) {
            uint64_t span = g.now_ns - gpu_prev.now_ns, jobs = 0;
            for (int e = 0; e < GE_COUNT; e++) {
                uint64_t a = e == GE_CPU ? g.cpu_render_ns : g.engine_busy_ns[e];
                uint64_t b = e == GE_CPU ? gpu_prev.cpu_render_ns : gpu_prev.engine_busy_ns[e];
                uint64_t pct = a > b ? (a - b) * 100 / span : 0;
                ge[e] = (uint8_t)(pct > 100 ? 100 : pct);
                if (e < GE_CPU && g.engine_jobs[e] > gpu_prev.engine_jobs[e]) jobs += g.engine_jobs[e] - gpu_prev.engine_jobs[e];
            }
            gpu_jobs_per_s = (uint32_t)(jobs * 1000000000ull / span);
        }
        gpu_prev = g;
        gpu_have_prev = 1;
        gpu_now = g;
    }
    uint8_t top = 0;
    for (int e = 0; e < GE_COUNT; e++) {
        gpu_hist[e][hist_count] = ge[e];
        // Like Windows: the GPU's figure is its busiest engine (the CPU renderer counts when there is no card's).
        if ((e < GE_CPU || !(gpu_now.flags & SYS_GPU_STATS_ENGINES)) && ge[e] > top) top = ge[e];
    }
    gpu_top_hist[hist_count] = top;
    vram_hist[hist_count] = (uint8_t)(gpu_now.vram_total ? gpu_now.vram_used * 100 / gpu_now.vram_total : 0);
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

#define SIDE_W 172

static rect_t side_rect(int i) {
    rect_t r = { 8, TAB_H + 8 + i * 64, SIDE_W - 16, 58 };
    return r;
}

// A small graph without frame (the sidebar's).
static void spark(int x, int y, int w, int h, const uint8_t* hist, int n, uint32_t color) {
    fill(x, y, w, h, C_BG);
    if (n < 1 || w < 4) return;
    for (int px = 0; px < w; px++) {
        int i = (HISTORY - 1) * px / (w - 1) - (HISTORY - n);
        if (i < 0) continue;
        int bar = hist[i] * h / 100;
        if (bar > 0) fill(x + px, y + h - bar, 1, bar, blend(C_BG, color, 110));
    }
}

static void draw_side(void) {
    fill(0, TAB_H, SIDE_W, (int)height - TAB_H, C_PANEL);
    fill(SIDE_W - 1, TAB_H, 1, (int)height - TAB_H, C_LINE);
    char v[48], t[24];
    for (int i = 0; i < 3; i++) {
        rect_t r = side_rect(i);
        int on = perf_view == i, hover = inside(r, mouse_x, mouse_y);
        if (on || hover) round_rect(r.x, r.y, r.w, r.h, 6, on ? C_SEL : C_HOVER);
        const uint8_t* h = i == 0 ? cpu_hist : i == 1 ? mem_hist : gpu_top_hist;
        uint32_t col = i == 0 ? C_ACCENT : i == 1 ? C_ACCENT2 : C_OK;
        spark(r.x + 8, r.y + 8, 48, r.h - 16, h, hist_count, col);
        const char* name = i == 0 ? "CPU" : i == 1 ? "Memory" : "GPU 0";
        text(r.x + 64, r.y + 10, name, C_TEXT);
        if (i == 0) { fmt_u(v, hist_count ? cpu_hist[hist_count - 1] : 0); strcat(v, " %"); }
        else if (i == 1) {
            fmt_bytes(v, mos_sysinfo_ram_used()); strcat(v, " / "); fmt_bytes(t, mos_sysinfo_ram_total()); strcat(v, t);
        } else if (!gpu_ok || !gpu_now.name[0]) strcpy(v, "No card");
        else { fmt_u(v, hist_count ? gpu_top_hist[hist_count - 1] : 0); strcat(v, " %"); }
        text_fit(r.x + 64, r.y + 30, v, r.w - 72, C_DIM);
    }
}

/* What a core ran, as people read it: a program as the Processes list names it, a kernel task as
 * "kernel <what>" ("usb_keyboard_update" -> "kernel usb keyboard"). */
static void core_task_name(const syscall_cpu_top_t* t, char* out, size_t cap) {
    for (int i = 0; i < task_count; i++) {
        if (tasks[i].info.pid != t->pid) continue;
        if (is_user(&tasks[i].info)) { tm_strncpy(out, tasks[i].label, cap - 1); out[cap - 1] = 0; return; }
        break;
    }
    char raw[40];
    size_t n = 0;
    for (const char* s = t->name; *s && *s != '@' && *s != '[' && n < sizeof raw - 1; s++) raw[n++] = *s == '_' ? ' ' : *s;
    while (n && raw[n - 1] == ' ') n--;
    raw[n] = 0;
    static const char* const tails[] = { " updater", " update", " worker", "updater", "update" };
    for (unsigned k = 0; k < sizeof tails / sizeof tails[0]; k++) {
        size_t l = strlen(tails[k]);
        if (n > l && !strcmp(raw + n - l, tails[k])) { n -= l; raw[n] = 0; break; }
    }
    const char* what = raw;
    if (!strncmp(what, "kernel", 6)) { what += 6; while (*what == ' ') what++; }
    if (!strcmp(raw, "idle")) { tm_strncpy(out, "idle", cap - 1); out[cap - 1] = 0; return; }
    out[0] = 0;
    tm_strncat(out, "kernel ", cap - 1);
    tm_strncat(out, *what ? what : raw, cap - 1 - strlen(out));
}

/* "Terminal (60%)  kernel keyboard (40%)": the two busiest of a core's last second. */
static void core_top_line(uint32_t c, char* out, size_t cap) {
    out[0] = 0;
    if (c >= cpu_stats.cores) return;
    const syscall_core_stats_t* cs = &cpu_stats.core[c];
    if (!cpu_ok) { tm_strncat(out, "(a newer kernel names the programs)", cap - 1); return; }
    if (!cs->top_count) { tm_strncat(out, "idle", cap - 1); return; }
    for (uint32_t i = 0; i < cs->top_count && i < 2; i++) {
        char name[40], pct[12];
        core_task_name(&cs->top[i], name, sizeof name);
        fmt_u(pct, cs->top[i].percent);
        if (i) tm_strncat(out, "  ", cap - 1 - strlen(out));
        tm_strncat(out, name, cap - 1 - strlen(out));
        tm_strncat(out, " (", cap - 1 - strlen(out));
        tm_strncat(out, pct, cap - 1 - strlen(out));
        tm_strncat(out, "%)", cap - 1 - strlen(out));
    }
}

/* A small graph for one core, with its number, usage and busiest programs. */
static void core_tile(int x, int y, int w, int h, uint32_t c) {
    round_rect(x, y, w, h, 6, C_BG);
    char label[24], pct[12], line[96];
    tm_strncpy(label, "CPU ", sizeof label - 1);
    label[sizeof label - 1] = 0;
    fmt_u(pct, c);
    strcat(label, pct);
    text(x + 8, y + 6, label, C_DIM);
    fmt_u(pct, c < cpu_stats.cores ? cpu_stats.core[c].usage : 0);
    strcat(pct, "%");
    text(x + w - 8 - text_width(pct, 1), y + 6, pct, C_TEXT);
    int th = text_height(1);
    // The two busiest programs, a line each (name, then its share), under the graph.
    const syscall_core_stats_t* cs = c < cpu_stats.cores ? &cpu_stats.core[c] : 0;
    int lines = cs && cpu_ok && cs->top_count > 1 ? 2 : 1;
    int gy = y + 10 + th, gh = h - 16 - th - lines * (th + 3);
    if (gh >= 8) {
        fill(x + 8, gy, w - 16, gh, C_PANEL);
        int gw = w - 16;
        for (int px = 0; px < gw && hist_count > 0; px++) {
            int i = (HISTORY - 1) * px / (gw > 1 ? gw - 1 : 1) - (HISTORY - hist_count);
            if (i < 0) continue;
            int bar = core_hist[c][i] * gh / 100;
            if (bar > 0) {
                fill(x + 8 + px, gy + gh - bar, 1, bar, blend(C_PANEL, C_ACCENT, 80));
                fill(x + 8 + px, gy + gh - bar, 1, 1, C_ACCENT);
            }
        }
    }
    int ly = y + h - lines * (th + 3) - 3;
    if (!cs || !cpu_ok || !cs->top_count) {
        core_top_line(c, line, sizeof line);
        text_fit(x + 8, ly, line, w - 16, C_DIM);
        return;
    }
    for (int i = 0; i < lines; i++) {
        char name[40], share[12];
        core_task_name(&cs->top[i], name, sizeof name);
        fmt_u(share, cs->top[i].percent);
        strcat(share, "%");
        int sw = text_width(share, 1);
        text_fit(x + 8, ly + i * (th + 3), name, w - 24 - sw, i ? C_DIM : C_TEXT);
        text(x + w - 8 - sw, ly + i * (th + 3), share, C_DIM);
    }
}

static void cpu_feature_text(char* out, size_t cap) {
    static const struct { uint32_t bit; const char* name; } f[] = {
        { SYS_CPU_FEAT_SSE42, "SSE4.2" }, { SYS_CPU_FEAT_AVX, "AVX" }, { SYS_CPU_FEAT_AVX2, "AVX2" },
        { SYS_CPU_FEAT_AVX512, "AVX-512" }, { SYS_CPU_FEAT_FMA, "FMA" }, { SYS_CPU_FEAT_AES, "AES" },
    };
    out[0] = 0;
    for (unsigned i = 0; i < sizeof f / sizeof f[0]; i++) {
        if (!(cpu_stats.features & f[i].bit)) continue;
        if (out[0]) tm_strncat(out, " ", cap - 1 - strlen(out));
        tm_strncat(out, f[i].name, cap - 1 - strlen(out));
    }
    if (!out[0]) tm_strncat(out, "-", cap - 1);
}

static void fmt_cache(char* out, uint32_t kb) {
    if (!kb) { strcpy(out, "-"); return; }
    if (kb >= 1024 && kb % 1024 == 0) { fmt_u(out, kb / 1024); strcat(out, " MB"); return; }
    fmt_u(out, kb); strcat(out, " KB");
}

/* The right-click menu over the CPU graph. */
static rect_t menu_item(int i) {
    rect_t r = { menu_x, menu_y + 6 + i * 26, 200, 26 };
    if (r.x + r.w > (int)width - 4) r.x = (int)width - 4 - r.w;
    if (menu_y + 64 > (int)height - 4) r.y -= menu_y + 64 - ((int)height - 4);
    return r;
}

static void draw_graph_menu(void) {
    static const char* const items[2] = { "Overall utilization", "Logical processors" };
    rect_t a = menu_item(0);
    round_rect(a.x - 2, a.y - 8, a.w + 4, 2 * 26 + 12, 6, C_LINE);
    round_rect(a.x, a.y - 6, a.w, 2 * 26 + 8, 6, C_PANEL);
    for (int i = 0; i < 2; i++) {
        rect_t r = menu_item(i);
        if (inside(r, mouse_x, mouse_y)) round_rect(r.x + 4, r.y + 1, r.w - 8, r.h - 2, 4, C_HOVER);
        if (cpu_graph_mode == i) round_rect(r.x + 12, r.y + r.h / 2 - 3, 6, 6, 3, C_ACCENT);
        text(r.x + 28, r.y + (r.h - text_height(1)) / 2, items[i], C_TEXT);
    }
}

static void draw_system(int x0) {
    fill(x0, TAB_H, (int)width - x0, (int)height - TAB_H, C_BG);
    int pad = 12, y = TAB_H + pad;
    int w = (int)width - x0 - 2 * pad;
    char v[48], t[48];
    if (perf_view == 1) {
        uint64_t total = mos_sysinfo_ram_total(), used = mos_sysinfo_ram_used();
        fmt_bytes(v, used); strcat(v, " / "); fmt_bytes(t, total); strcat(v, t);
        int gh = ((int)height - y - pad) * 60 / 100;
        graph(x0 + pad, y, w, gh, mem_hist, hist_count, C_ACCENT2, "Memory", v);
        y += gh + pad;
        int ph = (int)height - y - pad;
        if (ph < 40) return;
        round_rect(x0 + pad, y, w, ph, 8, C_PANEL);
        struct { const char* k; char v[48]; } rows[4];
        int count = 0;
        rows[count].k = "In use"; fmt_bytes(rows[count].v, used); count++;
        rows[count].k = "Free"; fmt_bytes(rows[count].v, mos_sysinfo_ram_free()); count++;
        rows[count].k = "Total"; fmt_bytes(rows[count].v, total); count++;
        rows[count].k = "Kernel heap"; fmt_bytes(rows[count].v, mos_sysinfo_heap_used()); count++;
        for (int i = 0; i < count && y + 12 + i * 22 + 18 <= y + ph; i++) {
            text(x0 + pad + 14, y + 12 + i * 22, rows[i].k, C_DIM);
            text(x0 + pad + w - 14 - text_width(rows[i].v, 1), y + 12 + i * 22, rows[i].v, C_TEXT);
        }
        return;
    }
    // CPU: one graph for the whole processor, or one per logical processor (right-click to choose), then
    // the processor's details and the figures.
    int gh = ((int)height - y - pad) * 55 / 100;
    if (gh < 120) gh = 120;
    uint32_t n = cpu_stats.cores ? cpu_stats.cores : (cores ? cores : 1);
    if (n > SYS_CPU_MAX_CORES) n = SYS_CPU_MAX_CORES;
    cg_x = x0 + pad; cg_y = y; cg_w = w; cg_h = gh;
    if (cpu_graph_mode == 0) {
        fmt_u(v, hist_count ? cpu_hist[hist_count - 1] : 0);
        strcat(v, " %");
        graph(x0 + pad, y, w, gh, cpu_hist, hist_count, C_ACCENT, "CPU  -  overall utilization", v);
    } else {
        round_rect(x0 + pad, y, w, gh, 8, C_PANEL);
        text(x0 + pad + 14, y + 10, "CPU  -  logical processors", C_DIM);
        // Tiles wide enough for two program names (about 240 px), as tall as the rows allow (at least
        // about 70 px, else more columns).
        int ax = x0 + pad + 10, ay = y + 30, aw = w - 20, ah = gh - 40;
        int cols = aw / 240;
        if (cols < 1) cols = 1;
        if (cols > (int)n) cols = (int)n;
        int max_rows = ah / 70 > 0 ? ah / 70 : 1;
        if (((int)n + cols - 1) / cols > max_rows) cols = ((int)n + max_rows - 1) / max_rows;
        int rows = ((int)n + cols - 1) / cols;
        int tw = (aw - (cols - 1) * 8) / cols, tht = (ah - (rows - 1) * 8) / rows;
        for (uint32_t c = 0; c < n; c++)
            core_tile(ax + (int)(c % (uint32_t)cols) * (tw + 8), ay + (int)(c / (uint32_t)cols) * (tht + 8), tw, tht, c);
    }
    const char* hint = "Right-click the graph to change it";
    text(x0 + pad + w - 14 - text_width(hint, 1), y + 10, hint, C_DIM);
    y += gh + pad;
    int ph = (int)height - y - pad;
    if (ph < 40) return;
    // The processor (where the per-core bars were).
    int half = (w - pad) * 58 / 100;
    int px = x0 + pad;
    round_rect(px, y, half, ph, 8, C_PANEL);
    text(px + 14, y + 10, "Processor", C_DIM);
    struct { const char* k; char v[72]; } info[9];
    int ni = 0;
    char tmp[32];
    info[ni].k = "Name"; tm_strncpy(info[ni].v, cpu_ok && cpu_stats.brand[0] ? cpu_stats.brand : "unknown", 71); info[ni].v[71] = 0; ni++;
    if (cpu_ok) {
        info[ni].k = "Vendor"; tm_strncpy(info[ni].v, cpu_stats.vendor, 71); info[ni].v[71] = 0; ni++;
        info[ni].k = "Family"; strcpy(info[ni].v, "family "); fmt_u(tmp, cpu_stats.family); strcat(info[ni].v, tmp);
        strcat(info[ni].v, ", model "); fmt_u(tmp, cpu_stats.model); strcat(info[ni].v, tmp);
        strcat(info[ni].v, ", stepping "); fmt_u(tmp, cpu_stats.stepping); strcat(info[ni].v, tmp); ni++;
        info[ni].k = "Base speed";
        if (cpu_stats.base_mhz) {
            fmt_u(info[ni].v, cpu_stats.base_mhz / 1000); strcat(info[ni].v, ".");
            fmt_u(tmp, cpu_stats.base_mhz % 1000 / 10 + 100); strcat(info[ni].v, tmp + 1); strcat(info[ni].v, " GHz");
        } else strcpy(info[ni].v, "unknown");
        ni++;
    }
    info[ni].k = "Logical processors"; fmt_u(info[ni].v, n); ni++;
    if (cpu_ok) {
        info[ni].k = "Caches";
        info[ni].v[0] = 0;
        strcat(info[ni].v, "L1 "); fmt_cache(tmp, cpu_stats.l1d_kb + cpu_stats.l1i_kb); strcat(info[ni].v, tmp);
        strcat(info[ni].v, "  L2 "); fmt_cache(tmp, cpu_stats.l2_kb); strcat(info[ni].v, tmp);
        strcat(info[ni].v, "  L3 "); fmt_cache(tmp, cpu_stats.l3_kb); strcat(info[ni].v, tmp);
        ni++;
        info[ni].k = "Instructions"; cpu_feature_text(info[ni].v, sizeof info[ni].v); ni++;
        info[ni].k = "Virtual machine";
        if (cpu_stats.features & SYS_CPU_FEAT_VM) { strcpy(info[ni].v, "yes"); if (cpu_stats.hypervisor[0]) { strcat(info[ni].v, " ("); tm_strncat(info[ni].v, cpu_stats.hypervisor, 40); strcat(info[ni].v, ")"); } }
        else strcpy(info[ni].v, "no");
        ni++;
    }
    int kw = 0;
    for (int i = 0; i < ni; i++) { int tw2 = text_width(info[i].k, 1); if (tw2 > kw) kw = tw2; }
    for (int i = 0; i < ni; i++) {
        int ry = y + 32 + i * 20;
        if (ry + 18 > y + ph) break;
        text(px + 14, ry, info[i].k, C_DIM);
        text_fit(px + 14 + kw + 16, ry, info[i].v, half - 28 - kw - 16, C_TEXT);
    }
    // Figures.
    int fx = px + half + pad, fw = w - half - pad;
    round_rect(fx, y, fw, ph, 8, C_PANEL);
    struct { const char* k; char v[48]; } rows[5];
    int count = 0;
    rows[count].k = "Utilization"; fmt_u(rows[count].v, hist_count ? cpu_hist[hist_count - 1] : 0); strcat(rows[count].v, " %"); count++;
    rows[count].k = "Up time"; fmt_duration(rows[count].v, mos_sysinfo_uptime_ms()); count++;
    rows[count].k = "Tasks"; fmt_u(rows[count].v, (uint64_t)task_count); count++;
    // The busiest core and what kept it busy.
    uint32_t busiest = 0;
    for (uint32_t c = 1; c < n && c < cpu_stats.cores; c++)
        if (cpu_stats.core[c].usage > cpu_stats.core[busiest].usage) busiest = c;
    rows[count].k = "Busiest core"; strcpy(rows[count].v, "CPU "); fmt_u(tmp, busiest); strcat(rows[count].v, tmp);
    strcat(rows[count].v, "  "); fmt_u(tmp, busiest < cpu_stats.cores ? cpu_stats.core[busiest].usage : 0); strcat(rows[count].v, tmp); strcat(rows[count].v, "%");
    count++;
    for (int i = 0; i < count; i++) {
        int ry = y + 12 + i * 22;
        if (ry + 18 > y + ph) break;
        text(fx + 14, ry, rows[i].k, C_DIM);
        text(fx + fw - 14 - text_width(rows[i].v, 1), ry, rows[i].v, C_TEXT);
    }
    // What kept it busy: its two programs, a line each.
    if (busiest < cpu_stats.cores && cpu_ok && cpu_stats.core[busiest].top_count) {
        const syscall_core_stats_t* cs = &cpu_stats.core[busiest];
        for (uint32_t i = 0; i < cs->top_count && i < 2; i++) {
            int ry = y + 12 + (count + (int)i) * 22;
            if (ry + 18 > y + ph) break;
            char name[40];
            core_task_name(&cs->top[i], name, sizeof name);
            fmt_u(tmp, cs->top[i].percent);
            strcat(tmp, "%");
            int sw = text_width(tmp, 1);
            text_fit(fx + 26, ry, name, fw - 52 - sw, C_DIM);
            text(fx + fw - 14 - sw, ry, tmp, C_DIM);
        }
    }
}


static void draw_gpu(int x0) {
    fill(x0, TAB_H, (int)width - x0, (int)height - TAB_H, C_BG);
    int pad = 12, y = TAB_H + pad, w = (int)width - x0 - 2 * pad;
    char v[64], t[32];
    if (!gpu_ok) {
        text(x0 + pad, y + 8, "This system does not report its graphics card (a newer kernel does).", C_DIM);
        return;
    }
    text_big(x0 + pad, y, gpu_now.name[0] ? gpu_now.name : "No graphics card", C_TEXT);
    strcpy(v, "FarcadeGL: ");
    strcat(v, gpu_now.renderer[0] ? gpu_now.renderer : "CPU");
    text(x0 + pad + w - text_width(v, 1), y + 8, v, C_DIM);
    y += 40;
    // Engines, 2 x 2 (the CPU renderer instead of Video Encode when the card's engines are not measured).
    int engines = gpu_now.flags & SYS_GPU_STATS_ENGINES;
    int show[4] = { GE_3D, GE_COPY, GE_DECODE, engines ? GE_ENCODE : GE_CPU };
    int gw = (w - pad) / 2;
    int total_h = (int)height - y - pad;
    int gh = (total_h - 2 * pad) * 30 / 100;
    if (gh < 96) gh = 96;
    for (int k = 0; k < 4; k++) {
        int e = show[k];
        int gx = x0 + pad + (k % 2) * (gw + pad), gy = y + (k / 2) * (gh + pad);
        if (!engines && e != GE_CPU) {
            round_rect(gx, gy, gw, gh, 8, C_PANEL);
            text(gx + 14, gy + 10, ge_name[e], C_DIM);
            text(gx + 14, gy + 32, "not measured", C_DIM);
            continue;
        }
        fmt_u(v, hist_count ? gpu_hist[e][hist_count - 1] : 0);
        strcat(v, " %");
        if (e == GE_ENCODE) strcat(v, "  (not used yet)");
        graph(gx, gy, gw, gh, gpu_hist[e], hist_count, e == GE_3D ? C_OK : e == GE_COPY ? C_ACCENT : C_ACCENT2,
              ge_name[e], v);
    }
    y += 2 * (gh + pad);
    // Dedicated memory.
    int mh = (int)height - y - pad;
    if (mh < 60) return;
    int mw = w * 62 / 100;
    if (gpu_now.vram_total) {
        fmt_bytes(v, gpu_now.vram_used); strcat(v, " / "); fmt_bytes(t, gpu_now.vram_total); strcat(v, t);
    } else strcpy(v, "unknown");
    graph(x0 + pad, y, mw, mh, vram_hist, hist_count, C_ACCENT2, "Dedicated GPU memory", v);
    int fx = x0 + pad + mw + pad, fw = w - mw - pad;
    round_rect(fx, y, fw, mh, 8, C_PANEL);
    struct { const char* k; char v[32]; } rows[4];
    int count = 0;
    rows[count].k = "Utilization"; fmt_u(rows[count].v, hist_count ? gpu_top_hist[hist_count - 1] : 0); strcat(rows[count].v, " %"); count++;
    rows[count].k = "Jobs/s"; fmt_u(rows[count].v, gpu_jobs_per_s); count++;
    rows[count].k = "CPU drawing"; fmt_u(rows[count].v, hist_count ? gpu_hist[GE_CPU][hist_count - 1] : 0); strcat(rows[count].v, " %"); count++;
    rows[count].k = "Free"; if (gpu_now.vram_total > gpu_now.vram_used) fmt_bytes(rows[count].v, gpu_now.vram_total - gpu_now.vram_used); else strcpy(rows[count].v, "-"); count++;
    for (int i = 0; i < count; i++) {
        int ry = y + 12 + i * 22;
        if (ry + 18 > y + mh) break;
        text(fx + 14, ry, rows[i].k, C_DIM);
        text(fx + fw - 14 - text_width(rows[i].v, 1), ry, rows[i].v, C_TEXT);
    }
}

static void draw_performance(void) {
    draw_side();
    if (perf_view == 2) draw_gpu(SIDE_W);
    else draw_system(SIDE_W);
}

static void draw(void) {
    draw_tabs();
    if (page == 0) draw_processes();
    else draw_performance();
    if (menu_open) draw_graph_menu();
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
    if (menu_open) {
        for (int i = 0; i < 2; i++)
            if (inside(menu_item(i), x, y)) cpu_graph_mode = i;
        menu_open = 0;
        return;
    }
    for (int i = 0; i < 2; i++)
        if (inside(tab_rect(i), x, y)) { page = i; confirm = 0; return; }
    if (page == 1) {
        for (int i = 0; i < 3; i++)
            if (inside(side_rect(i), x, y)) perf_view = i;
        return;
    }
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

static void right_click(int x, int y) {
    menu_open = 0;
    if (page == 1 && perf_view == 0 && x >= cg_x && y >= cg_y && x < cg_x + cg_w && y < cg_y + cg_h) {
        menu_open = 1;
        menu_x = x;
        menu_y = y;
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
                    if (pressed & SYSCALL_MOUSE_BTN_RIGHT) right_click(ev.x, ev.y);
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
                    else if (page == 1 && key == 82) perf_view = perf_view > 0 ? perf_view - 1 : 0;   // Up
                    else if (page == 1 && key == 81) perf_view = perf_view < 2 ? perf_view + 1 : 2;   // Down
                    else if (page == 0 && key == 82) move_selection(-1);                // Up
                    else if (page == 0 && key == 81) move_selection(1);                 // Down
                    else if (page == 0 && key == 75) move_selection(-list_rows());      // Page Up
                    else if (page == 0 && key == 78) move_selection(list_rows());       // Page Down
                    else if (page == 0 && key == 76) {                                  // Delete
                        if (confirm) end_selected(now); else ask_end(now);
                    }
                    else if (confirm && key == 40) end_selected(now);                   // Enter
                    else if (key == 41) { confirm = 0; menu_open = 0; }                 // Escape
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
