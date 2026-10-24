/*
 * terminal.c - MOSdesk Terminal: a command line that runs in user mode
 *
 *     ./build.sh --output mos_terminal.run terminal.c
 *
 * Bundled with MOSdesk as 0:/programs/mos_terminal.run.
 *
 * The kernel's own terminal runs its commands inside the kernel. This one
 * is an ordinary user program: every command below is carried out with the
 * same system calls any .run program has, so it can do exactly what a user
 * program may do and nothing more. Commands that only the kernel can carry
 * out (format, mount, reboot, dhcp, ...) are named and refused, and any
 * other word is looked up as a program and started as a user process.
 *
 * `root <command>` runs a kernel terminal command after the kernel has
 * checked your account password (SYS_ROOT); the password is then not asked
 * for again for five minutes. An auto-login account has no password, so it
 * answers a y/N question instead. `root -k` forgets it, `root -v` renews it.
 *
 * Keys: Enter runs, Up/Down walk the history, Left/Right/Home/End/Delete
 * edit the line, Tab completes a file name, PgUp/PgDn scroll, Ctrl+L
 * clears, Ctrl+C drops the line.
 */
#include "easyui.h"
#include "stdlib.h"
#include "string.h"
#include "net.h"
#include "user_sysinfo.h"
#if defined(__has_include)
#if __has_include("../settings.h")
#include "../settings.h"         /* MOSdesk: username, home, clock format */
#define TERMINAL_HAS_MOSDESK 1
#endif
#endif

#define MAX_LINES   1200
#define LINE_MAX    200
#define INPUT_MAX   240
#define PATH_MAX_   192
#define HISTORY     32
#define MAX_ARGS    16
#define LH          17
#define PAD         8
#define TS          1
#define FILE_SHOW_MAX 65536

enum { K_A = 4, K_C = 6, K_L = 15, K_ENTER = 40, K_ESC = 41, K_BACKSPACE = 42,
       K_TAB = 43, K_HOME = 74, K_PGUP = 75, K_DELETE = 76, K_END = 77,
       K_PGDN = 78, K_RIGHT = 79, K_LEFT = 80, K_DOWN = 81, K_UP = 82,
       K_KPENTER = 88 };

/* SYS_ROOT, as in the kernel's x86_64/syscall.h (the SDK copy may predate
 * it). */
#ifndef SYS_ROOT
#define SYS_ROOT 39
#define SYS_ROOT_AUTH   1u
#define SYS_ROOT_RUN    2u
#define SYS_ROOT_READ   3u
#define SYS_ROOT_STATUS 4u
#define SYS_ROOT_DROP   5u
#define SYS_ROOT_OUTPUT_SIZE 2048u
typedef struct {
    uint32_t op;
    uint32_t running;
    char username[32];
    char password[64];
    char command[256];
    uint64_t ticket_ms_left;
    uint32_t output_length;
    char output[SYS_ROOT_OUTPUT_SIZE];
} syscall_root_t;
#endif

/* ------------------------------------------------------------ scrollback */

typedef struct { char text[LINE_MAX]; uint32_t color; } line_t;

static line_t* lines;
static int line_first, line_count;   /* ring buffer */
static int scroll_back;              /* lines scrolled up from the bottom */
static int canvas;
static uint32_t color_text = 0xFFE3E9EEu, color_dim = 0xFF8A9AA6u,
                color_error = 0xFFFF8A80u, color_ok = 0xFF8BD8A0u,
                color_prompt = 0xFF6CC3F0u;
static uint32_t out_color;

static char input[INPUT_MAX];
static int input_len, input_caret;
static char history[HISTORY][INPUT_MAX];
static int history_count, history_pos;
static char cwd[PATH_MAX_] = "0:/";
static char username[32] = "user";
static int caret_on = 1;

/* root: waiting for a password (1) or, on an account without one, a y/N
 * confirmation (2); and a kernel command whose output is being read back. */
#define ROOT_CONFIRM_PROMPT \
    "Are you sure to grant this and following commands ROOT " \
    "(kernel level access)? y/N "
static int password_mode;
static int password_tries;
static char root_pending[256];
static int root_running;
static int root_timer;
static syscall_root_t root_request;

static line_t* line_at(int index) {
    return &lines[(line_first + index) % MAX_LINES];
}

static line_t* new_line(void) {
    line_t* line;
    if (line_count < MAX_LINES) line = line_at(line_count++);
    else { line = line_at(0); line_first = (line_first + 1) % MAX_LINES; }
    line->text[0] = '\0';
    line->color = out_color;
    return line;
}

static int text_limit(void) {
    int width = easyui.getwidth(canvas) - 2 * PAD - 10;
    return width > 40 ? width : 40;
}

/* Appends text, starting new lines at '\n' and where the window is full. */
static void out(const char* text) {
    static int at_line_start = 1;
    line_t* line = at_line_start || !line_count ? NULL : line_at(line_count - 1);
    for (const char* p = text; *p; p++) {
        if (!line) { line = new_line(); at_line_start = 0; }
        if (*p == '\n') { line = NULL; at_line_start = 1; continue; }
        char c = (*p == '\t') ? ' ' : (*p >= 32 && *p < 127) ? *p : '?';
        size_t n = strlen(line->text);
        line->text[n] = c;
        line->text[n + 1] = '\0';
        if (n + 2 >= LINE_MAX || easyui.textwidth(line->text, TS) > text_limit()) {
            line->text[n] = '\0';
            line = new_line();
            line->text[0] = c;
            line->text[1] = '\0';
        }
    }
    scroll_back = 0;
    easyui.redraw();
}

static void outc(const char* text, uint32_t color) {
    uint32_t saved = out_color;
    out_color = color;
    out(text);
    out_color = saved;
}

static void out_u64(uint64_t value) {
    char digits[24];
    int n = 0;
    do { digits[n++] = (char)('0' + value % 10u); value /= 10u; } while (value);
    char text[24];
    for (int i = 0; i < n; i++) text[i] = digits[n - 1 - i];
    text[n] = '\0';
    out(text);
}

static void out_size(uint64_t bytes) {
    if (bytes >= 10u * 1024u * 1024u) { out_u64(bytes / (1024u * 1024u)); out(" MB"); }
    else if (bytes >= 10u * 1024u) { out_u64(bytes / 1024u); out(" KB"); }
    else { out_u64(bytes); out(" B"); }
}

static void out_ip(uint32_t ip) {
    for (int i = 3; i >= 0; i--) {
        out_u64((ip >> (i * 8)) & 0xFFu);
        if (i) out(".");
    }
}

static void error_line(const char* command, const char* message) {
    outc(command, color_error);
    outc(": ", color_error);
    outc(message, color_error);
    outc("\n", color_error);
}

/* ------------------------------------------------------------------ paths */

static void str_append(char* dst, size_t cap, const char* s) {
    size_t n = strlen(dst);
    while (*s && n + 1 < cap) dst[n++] = *s++;
    dst[n] = '\0';
}

/* "0:/a/b" from `arg` relative to the current directory, with "." and ".."
 * resolved. Returns 0 when it does not fit. */
static int resolve(const char* arg, char* outp, size_t cap) {
    char joined[PATH_MAX_ * 2];
    if (!arg || !arg[0]) arg = ".";
    if (arg[0] >= '0' && arg[0] <= '9' && arg[1] == ':') {
        strlcpy_safe(joined, arg, sizeof joined);
    } else if (arg[0] == '/') {
        strlcpy_safe(joined, "0:", sizeof joined);
        str_append(joined, sizeof joined, arg);
    } else if (arg[0] == '~') {
        strlcpy_safe(joined, cwd, sizeof joined);
#ifdef TERMINAL_HAS_MOSDESK
        char home[PATH_MAX_];
        if (mosdesk_user_path(home, sizeof home, username, NULL))
            strlcpy_safe(joined, home, sizeof joined);
#endif
        str_append(joined, sizeof joined, arg + 1);
    } else {
        strlcpy_safe(joined, cwd, sizeof joined);
        str_append(joined, sizeof joined, "/");
        str_append(joined, sizeof joined, arg);
    }
    /* Normalise: drive prefix, then components. */
    char drive[3] = {joined[0], ':', '\0'};
    const char* p = joined + 2;
    char parts[32][64];
    int count = 0;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        char part[64];
        int n = 0;
        while (*p && *p != '/') { if (n < 63) part[n++] = *p; p++; }
        part[n] = '\0';
        if (!strcmp(part, ".")) continue;
        if (!strcmp(part, "..")) { if (count) count--; continue; }
        if (count < 32) strlcpy_safe(parts[count++], part, 64);
    }
    char result[PATH_MAX_ * 2];
    strlcpy_safe(result, drive, sizeof result);
    str_append(result, sizeof result, "/");
    for (int i = 0; i < count; i++) {
        if (i) str_append(result, sizeof result, "/");
        str_append(result, sizeof result, parts[i]);
    }
    if (strlen(result) + 1 > cap) return 0;
    strlcpy_safe(outp, result, cap);
    return 1;
}

static const char* base_name(const char* path) {
    const char* slash = path;
    for (const char* p = path; *p; p++) if (*p == '/') slash = p + 1;
    return slash;
}

static long list_dir(const char* path, syscall_dirent_t* entries, long max) {
    long count = mos_syscall(SYS_LISTDIR, (long)(uintptr_t)path,
                             (long)(uintptr_t)entries, max);
    if (count > max) count = max;
    return count;
}

static long file_size(const char* path) {
    long fd = mos_open(path, SYS_O_RDONLY);
    if (fd <= 0) return -1;
    long size = mos_size(fd);
    mos_close(fd);
    return size;
}

/* ---------------------------------------------------------------- commands */

typedef void (*command_fn)(int argc, char** argv);
typedef struct { const char* name; command_fn fn; const char* usage; } command_t;

static void cmd_help(int argc, char** argv);

static void cmd_clear(int argc, char** argv) {
    (void)argc; (void)argv;
    line_first = line_count = 0;
    scroll_back = 0;
}

static void cmd_echo(int argc, char** argv) {
    for (int i = 1; i < argc; i++) { if (i > 1) out(" "); out(argv[i]); }
    out("\n");
}

static void cmd_pwd(int argc, char** argv) { (void)argc; (void)argv; out(cwd); out("\n"); }

static void cmd_cd(int argc, char** argv) {
    char path[PATH_MAX_];
    if (!resolve(argc > 1 ? argv[1] : "~", path, sizeof path)) {
        error_line("cd", "path too long");
        return;
    }
    if (mos_is_dir(path) != 1) { error_line("cd", "no such folder"); return; }
    strlcpy_safe(cwd, path, sizeof cwd);
}

static void cmd_ls(int argc, char** argv) {
    int detailed = 0;
    const char* target = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-l")) detailed = 1;
        else target = argv[i];
    }
    char path[PATH_MAX_];
    if (!resolve(target, path, sizeof path)) { error_line("ls", "path too long"); return; }
    if (mos_is_dir(path) != 1) {
        if (mos_exists(path) == 1) { out(base_name(path)); out("\n"); }
        else error_line("ls", "no such file or folder");
        return;
    }
    static syscall_dirent_t entries[128];
    long count = list_dir(path, entries, 128);
    if (count < 0) { error_line("ls", "cannot read this folder"); return; }
    if (!count) { outc("(empty)\n", color_dim); return; }
    for (long i = 0; i < count; i++) {
        const syscall_dirent_t* e = &entries[i];
        int is_dir = e->type == SYSCALL_DIRENT_TYPE_DIR;
        uint32_t color = is_dir ? color_prompt
            : e->type == SYSCALL_DIRENT_TYPE_EXECUTABLE ? color_ok : color_text;
        if (detailed) {
            char full[PATH_MAX_];
            strlcpy_safe(full, path, sizeof full);
            if (full[strlen(full) - 1] != '/') str_append(full, sizeof full, "/");
            str_append(full, sizeof full, e->name);
            outc(is_dir ? "dir   " : e->type == SYSCALL_DIRENT_TYPE_EXECUTABLE
                 ? "exec  " : "file  ", color_dim);
            if (!is_dir) {
                long size = file_size(full);
                char pad[16] = "";
                if (size >= 0) out_size((uint64_t)size);
                (void)pad;
                out("  ");
            }
        }
        outc(e->name, color);
        if (is_dir) outc("/", color);
        out("\n");
    }
}

static void cmd_cat(int argc, char** argv) {
    if (argc < 2) { error_line("cat", "usage: cat <file>"); return; }
    for (int a = 1; a < argc; a++) {
        char path[PATH_MAX_];
        if (!resolve(argv[a], path, sizeof path) || mos_is_dir(path) == 1) {
            error_line("cat", "not a file");
            continue;
        }
        long fd = mos_open(path, SYS_O_RDONLY);
        if (fd <= 0) { error_line("cat", "no such file"); continue; }
        long size = mos_size(fd);
        long shown = size > FILE_SHOW_MAX ? FILE_SHOW_MAX : size;
        char* data = (char*)malloc((size_t)shown + 1u);
        long got = 0, n;
        while (data && got < shown &&
               (n = mos_read(fd, data + got, (size_t)(shown - got))) > 0)
            got += n;
        mos_close(fd);
        if (!data) { error_line("cat", "out of memory"); return; }
        data[got] = '\0';
        for (long i = 0; i < got; i++) if (data[i] == '\r' || !data[i]) data[i] = ' ';
        out(data);
        if (got && data[got - 1] != '\n') out("\n");
        free(data);
        if (size > shown) outc("(only the first 64 KB are shown)\n", color_dim);
    }
}

static void cmd_mkdir(int argc, char** argv) {
    if (argc < 2) { error_line("mkdir", "usage: mkdir <folder>"); return; }
    for (int i = 1; i < argc; i++) {
        char path[PATH_MAX_];
        if (!resolve(argv[i], path, sizeof path)) { error_line("mkdir", "path too long"); continue; }
        mos_mkdir(path);
        if (mos_is_dir(path) != 1) error_line("mkdir", "could not create it");
    }
}

static void cmd_rm(int argc, char** argv) {
    if (argc < 2) { error_line("rm", "usage: rm <file>"); return; }
    for (int i = 1; i < argc; i++) {
        char path[PATH_MAX_];
        if (!resolve(argv[i], path, sizeof path) || mos_exists(path) != 1) {
            error_line("rm", "no such file");
            continue;
        }
        if (mos_is_dir(path) == 1) { error_line("rm", "that is a folder (use rmdir)"); continue; }
        if ((long)mos_syscall(SYS_DELETE, (long)(uintptr_t)path, 0, 0) < 0)
            error_line("rm", "not allowed or failed");
    }
}

static void cmd_rmdir(int argc, char** argv) {
    if (argc < 2) { error_line("rmdir", "usage: rmdir <folder>"); return; }
    char path[PATH_MAX_];
    if (!resolve(argv[1], path, sizeof path) || mos_is_dir(path) != 1) {
        error_line("rmdir", "no such folder");
        return;
    }
    if ((long)mos_syscall(SYS_RMDIR, (long)(uintptr_t)path, 0, 0) < 0)
        error_line("rmdir", "the folder is not empty, or not allowed");
}

/* Writes `size` bytes as `path`, replacing what is there. */
static int write_file(const char* path, const char* data, long size) {
    if (mos_exists(path) == 1) {
        if (mos_is_dir(path) == 1) return 0;
        if ((long)mos_syscall(SYS_DELETE, (long)(uintptr_t)path, 0, 0) < 0) return 0;
    }
    if (mos_create(path, "text", "txt") < 0) return 0;
    long fd = mos_open(path, SYS_O_RDWR);
    if (fd <= 0) return 0;
    long total = 0, n;
    while (total < size &&
           (n = (long)mos_syscall(SYS_FWRITE, fd, (long)(uintptr_t)(data + total),
                                  size - total)) > 0)
        total += n;
    mos_close(fd);
    return total == size;
}

static void cmd_touch(int argc, char** argv) {
    if (argc < 2) { error_line("touch", "usage: touch <file>"); return; }
    char path[PATH_MAX_];
    if (!resolve(argv[1], path, sizeof path)) { error_line("touch", "path too long"); return; }
    if (mos_exists(path) == 1) return;
    if (!write_file(path, "", 0)) error_line("touch", "could not create it");
}

static void cmd_write(int argc, char** argv) {
    if (argc < 3) { error_line("write", "usage: write <file> <text...>"); return; }
    char path[PATH_MAX_];
    if (!resolve(argv[1], path, sizeof path)) { error_line("write", "path too long"); return; }
    char text[INPUT_MAX + 2] = "";
    for (int i = 2; i < argc; i++) {
        if (i > 2) str_append(text, sizeof text, " ");
        str_append(text, sizeof text, argv[i]);
    }
    str_append(text, sizeof text, "\n");
    if (!write_file(path, text, (long)strlen(text))) error_line("write", "could not write it");
}

static int copy_file(const char* from, const char* to) {
    long fd = mos_open(from, SYS_O_RDONLY);
    if (fd <= 0) return 0;
    long size = mos_size(fd);
    char* data = size >= 0 ? (char*)malloc((size_t)size + 1u) : NULL;
    long got = 0, n;
    while (data && got < size && (n = mos_read(fd, data + got, (size_t)(size - got))) > 0)
        got += n;
    mos_close(fd);
    int ok = data && got == size && write_file(to, data, size);
    free(data);
    return ok;
}

/* Where `cp a b` / `mv a b` puts a: inside b when b is a folder. */
static int target_path(const char* from, const char* arg, char* outp, size_t cap) {
    if (!resolve(arg, outp, cap)) return 0;
    if (mos_is_dir(outp) == 1) {
        if (outp[strlen(outp) - 1] != '/') str_append(outp, cap, "/");
        str_append(outp, cap, base_name(from));
    }
    return 1;
}

static void cmd_cp(int argc, char** argv) {
    if (argc < 3) { error_line("cp", "usage: cp <file> <destination>"); return; }
    char from[PATH_MAX_], to[PATH_MAX_];
    if (!resolve(argv[1], from, sizeof from) || mos_exists(from) != 1 ||
        mos_is_dir(from) == 1) { error_line("cp", "no such file"); return; }
    if (!target_path(from, argv[2], to, sizeof to) || !copy_file(from, to))
        error_line("cp", "copy failed");
}

static void cmd_mv(int argc, char** argv) {
    if (argc < 3) { error_line("mv", "usage: mv <file> <destination>"); return; }
    char from[PATH_MAX_], to[PATH_MAX_];
    if (!resolve(argv[1], from, sizeof from) || mos_exists(from) != 1 ||
        mos_is_dir(from) == 1) { error_line("mv", "no such file"); return; }
    if (!target_path(from, argv[2], to, sizeof to) || !copy_file(from, to)) {
        error_line("mv", "move failed");
        return;
    }
    if ((long)mos_syscall(SYS_DELETE, (long)(uintptr_t)from, 0, 0) < 0)
        error_line("mv", "copied, but the original could not be removed");
}

typedef struct { uint16_t year; uint8_t month, day, hour, minute, second; } datetime_t_;

static void cmd_date(int argc, char** argv) {
    (void)argc; (void)argv;
    datetime_t_ now = {0};
    if (mos_syscall(SYS_GETTIME, (long)(uintptr_t)&now, 0, 0) != SYS_SUCCESS) {
        error_line("date", "the clock is not available");
        return;
    }
    char text[64];
    /* Clock and date format settings, when this MOSdesk has them. */
#if defined(TERMINAL_HAS_MOSDESK) && defined(MOSDESK_DATE_ISO)
    mosdesk_settings_t s = mosdesk_settings_load();
    uint8_t second = now.second;
    mosdesk_apply_utc_offset(&now.year, &now.month, &now.day, &now.hour,
                             &now.minute, s.utc_offset_minutes);
    char date[12], time_text[12], offset[10];
    mosdesk_format_date(date, now.year, now.month, now.day, s.date_format);
    mosdesk_format_time(time_text, now.hour, now.minute, s.clock_24h != 0u);
    mosdesk_format_utc_offset(offset, s.utc_offset_minutes);
    strlcpy_safe(text, date, sizeof text);
    str_append(text, sizeof text, "  ");
    str_append(text, sizeof text, time_text);
    char sec[5] = {' ', '(', (char)('0' + second / 10u), (char)('0' + second % 10u), 0};
    str_append(text, sizeof text, sec);
    str_append(text, sizeof text, " s)  ");
    str_append(text, sizeof text, offset);
#else
    /* 2026-10-24  18:02:05 */
    {
        const uint32_t v[6] = {now.year, now.month, now.day,
                               now.hour, now.minute, now.second};
        const char sep[6] = {'-', '-', ' ', ':', ':', 0};
        int n = 0;
        for (int i = 0; i < 6; i++) {
            uint32_t value = v[i];
            int digits = i == 0 ? 4 : 2;
            for (int d = digits - 1; d >= 0; d--) {
                uint32_t div = 1;
                for (int k = 0; k < d; k++) div *= 10u;
                text[n++] = (char)('0' + value / div % 10u);
            }
            if (sep[i]) text[n++] = sep[i];
            if (i == 2) text[n++] = ' ';
        }
        text[n] = '\0';
    }
#endif
    out(text);
    out("\n");
}

static void out_duration(uint64_t ms) {
    uint64_t s = ms / 1000u;
    if (s >= 86400u) { out_u64(s / 86400u); out("d "); }
    if (s >= 3600u) { out_u64(s / 3600u % 24u); out("h "); }
    out_u64(s / 60u % 60u); out("m ");
    out_u64(s % 60u); out("s");
}

static void cmd_uptime(int argc, char** argv) {
    (void)argc; (void)argv;
    out("up ");
    out_duration(mos_sysinfo_uptime_ms());
    out("\n");
}

static void cmd_whoami(int argc, char** argv) { (void)argc; (void)argv; out(username); out("\n"); }

static void cmd_ps(int argc, char** argv) {
    (void)argc; (void)argv;
    static syscall_process_info_t list[64];
    long count = mos_pslist(list, 64);
    if (count < 0) { error_line("ps", "cannot list processes"); return; }
    static const char* const states[] = {"ready", "running", "waiting",
                                         "paused", "zombie", "ended"};
    outc("  PID  MODE    STATE    NAME\n", color_dim);
    for (long i = 0; i < count && i < 64; i++) {
        char pid[8];
        int n = 0;
        uint64_t v = list[i].pid;
        char d[8]; int k = 0;
        do { d[k++] = (char)('0' + v % 10u); v /= 10u; } while (v && k < 7);
        while (n < 5 - k) pid[n++] = ' ';
        while (k) pid[n++] = d[--k];
        pid[n] = '\0';
        out(pid);
        out(list[i].privilege == SYSCALL_PROC_PRIV_USER ? "  user    " : "  kernel  ");
        const char* state = list[i].state < 6u ? states[list[i].state] : "?";
        out(state);
        for (int pad = (int)strlen(state); pad < 9; pad++) out(" ");
        out(list[i].name);
        out("\n");
    }
}

static void cmd_sysinfo(int argc, char** argv) {
    (void)argc; (void)argv;
    out("CPU cores   "); out_u64(mos_sysinfo_core_count()); out("\n");
    out("CPU usage   "); out_u64(mos_sysinfo_cpu_usage()); out(" %\n");
    out("Memory      "); out_size(mos_sysinfo_ram_used()); out(" used of ");
    out_size(mos_sysinfo_ram_total()); out("\n");
    out("Processes   "); out_u64(mos_sysinfo_process_count()); out("\n");
    out("Uptime      "); out_duration(mos_sysinfo_uptime_ms()); out("\n");
}

static void cmd_ifconfig(int argc, char** argv) {
    (void)argc; (void)argv;
    syscall_net_status_t status;
    memset(&status, 0, sizeof status);
    if (mos_net_status(&status) != (long)SYS_SUCCESS || !status.has_driver) {
        error_line("ifconfig", "no network interface");
        return;
    }
    static const char hex[] = "0123456789abcdef";
    char mac[18];
    for (int i = 0; i < 6; i++) {
        mac[i * 3] = hex[status.mac[i] >> 4];
        mac[i * 3 + 1] = hex[status.mac[i] & 15];
        mac[i * 3 + 2] = i < 5 ? ':' : '\0';
    }
    out("MAC      "); out(mac); out("\n");
    if (!status.configured) { outc("No address yet.\n", color_dim); return; }
    out("Address  "); out_ip(status.local_ip); out("\n");
    out("Netmask  "); out_ip(status.netmask); out("\n");
    out("Gateway  "); out_ip(status.gateway); out("\n");
    out("DNS      "); out_ip(status.dns); out("\n");
}

static void cmd_resolve(int argc, char** argv) {
    if (argc < 2) { error_line("resolve", "usage: resolve <host>"); return; }
    uint32_t ip = 0;
    if (mos_net_resolve(argv[1], &ip, 5000) != (long)SYS_SUCCESS || !ip) {
        error_line("resolve", "lookup failed");
        return;
    }
    out(argv[1]); out(" is "); out_ip(ip); out("\n");
}

/* Plain HTTP over the user-mode TCP calls. */
static void cmd_wget(int argc, char** argv) {
    if (argc < 2) { error_line("wget", "usage: wget http://host[:port]/path [file]"); return; }
    const char* url = argv[1];
    if (!strncmp(url, "https://", 8)) {
        error_line("wget", "HTTPS needs the kernel's TLS client, which user "
                           "programs cannot use yet. Try http://");
        return;
    }
    if (strncmp(url, "http://", 7)) { error_line("wget", "the address must start with http://"); return; }
    char host[128];
    const char* p = url + 7;
    int n = 0;
    while (*p && *p != '/' && *p != ':' && n < 127) host[n++] = *p++;
    host[n] = '\0';
    uint16_t port = 80;
    if (*p == ':') {
        uint32_t v = 0;
        for (p++; *p >= '0' && *p <= '9'; p++) v = v * 10u + (uint32_t)(*p - '0');
        if (v && v < 65536u) port = (uint16_t)v;
    }
    const char* path = *p ? p : "/";
    uint32_t ip = mos_ip_parse(host);
    if (!ip && (mos_net_resolve(host, &ip, 5000) != (long)SYS_SUCCESS || !ip)) {
        error_line("wget", "could not resolve the host");
        return;
    }
    long handle = mos_tcp_connect(ip, port, 5000);
    if (handle <= 0) { error_line("wget", "connection failed"); return; }
    char request[INPUT_MAX + 160];
    strlcpy_safe(request, "GET ", sizeof request);
    str_append(request, sizeof request, path);
    str_append(request, sizeof request, " HTTP/1.0\r\nHost: ");
    str_append(request, sizeof request, host);
    str_append(request, sizeof request,
               "\r\nUser-Agent: MOSdesk-Terminal\r\nConnection: close\r\n\r\n");
    long length = (long)strlen(request), sent = 0;
    while (sent < length) {
        long k = mos_tcp_send(handle, request + sent, (uint32_t)(length - sent));
        if (k <= 0) break;
        sent += k;
    }
    char* body = (char*)malloc(FILE_SHOW_MAX + 1u);
    long got = 0;
    long idle_since = (long)mos_syscall(SYS_UPTIME, 0, 0, 0);
    while (body && got < FILE_SHOW_MAX) {
        long k = mos_tcp_recv(handle, body + got, (uint32_t)(FILE_SHOW_MAX - got));
        if (k > 0) { got += k; idle_since = (long)mos_syscall(SYS_UPTIME, 0, 0, 0); continue; }
        if (k < 0) break;               /* closed */
        if ((long)mos_syscall(SYS_UPTIME, 0, 0, 0) - idle_since > 8000) break;
        mos_sleep(10);
    }
    mos_tcp_close(handle);
    if (!body) { error_line("wget", "out of memory"); return; }
    body[got] = '\0';
    char* content = strstr(body, "\r\n\r\n");
    if (!content) { error_line("wget", "no HTTP answer"); free(body); return; }
    *content = '\0';
    content += 4;
    char* line_end = strstr(body, "\r\n");
    if (line_end) *line_end = '\0';
    outc(body, color_dim);
    out("\n");
    long content_length = got - (long)(content - body);
    if (argc > 2) {
        char target[PATH_MAX_];
        if (resolve(argv[2], target, sizeof target) &&
            write_file(target, content, content_length)) {
            out("Saved "); out_size((uint64_t)content_length); out(" to ");
            out(target); out("\n");
        } else {
            error_line("wget", "could not save the file");
        }
    } else {
        for (long i = 0; i < content_length; i++)
            if (content[i] == '\r' || !content[i]) content[i] = ' ';
        out(content);
        if (content_length && content[content_length - 1] != '\n') out("\n");
    }
    free(body);
}

static int find_program(const char* name, char* outp, size_t cap) {
    char path[PATH_MAX_];
    if (strchr(name, '/') || (name[0] && name[1] == ':')) {
        if (resolve(name, path, sizeof path) && mos_exists(path) == 1 &&
            mos_is_dir(path) != 1) { strlcpy_safe(outp, path, cap); return 1; }
        str_append(path, sizeof path, ".run");
        if (mos_exists(path) == 1) { strlcpy_safe(outp, path, cap); return 1; }
        return 0;
    }
    const char* forms[5][3] = {
        {"0:/programs/", name, ".run"},
        {"0:/programs/mos_", name, ".run"},
        {"0:/programs/", name, ""},
        {NULL, name, ".run"},           /* the current folder */
        {NULL, name, ""},
    };
    for (int i = 0; i < 5; i++) {
        if (forms[i][0]) strlcpy_safe(path, forms[i][0], sizeof path);
        else {
            strlcpy_safe(path, cwd, sizeof path);
            if (path[strlen(path) - 1] != '/') str_append(path, sizeof path, "/");
        }
        str_append(path, sizeof path, forms[i][1]);
        str_append(path, sizeof path, forms[i][2]);
        if (mos_exists(path) != 1) continue;
        if (mos_is_dir(path) == 1) {
            /* 0:/programs/<name>/<name>.run */
            char inner[PATH_MAX_];
            strlcpy_safe(inner, path, sizeof inner);
            str_append(inner, sizeof inner, "/");
            str_append(inner, sizeof inner, name);
            str_append(inner, sizeof inner, ".run");
            if (mos_exists(inner) != 1) continue;
            strlcpy_safe(path, inner, sizeof path);
        }
        strlcpy_safe(outp, path, cap);
        return 1;
    }
    return 0;
}

static void start_program(const char* path, int argc, char** argv) {
    const char* args[MAX_ARGS];
    for (int i = 0; i < argc && i < MAX_ARGS; i++) {
        /* Relative file arguments mean files in this folder. */
        static char resolved[MAX_ARGS][PATH_MAX_];
        args[i] = argv[i];
        if (argv[i][0] != '-' && resolve(argv[i], resolved[i], PATH_MAX_) &&
            mos_exists(resolved[i]) == 1)
            args[i] = resolved[i];
    }
    long result = mos_exec(path, argc ? args : NULL, argc);
    if (result < 0) error_line(base_name(path), "could not start it");
}

static void cmd_run(int argc, char** argv) {
    if (argc < 2) { error_line("run", "usage: run <program> [arguments]"); return; }
    char path[PATH_MAX_];
    if (!find_program(argv[1], path, sizeof path)) { error_line("run", "no such program"); return; }
    start_program(path, argc - 2, argv + 2);
}

static void cmd_programs(int argc, char** argv) {
    (void)argc; (void)argv;
    static syscall_dirent_t entries[64];
    long count = list_dir("0:/programs", entries, 64);
    for (long i = 0; i < count; i++) {
        const char* name = entries[i].name;
        size_t length = strlen(name);
        if (entries[i].type == SYSCALL_DIRENT_TYPE_DIR) {
            outc(name, color_ok); outc("  (folder)\n", color_dim);
        } else if (length > 4 && !strcmp(name + length - 4, ".run")) {
            char shown[64];
            strlcpy_safe(shown, name, sizeof shown);
            shown[length - 4] = '\0';
            outc(!strncmp(shown, "mos_", 4) ? shown + 4 : shown, color_ok);
            out("\n");
        }
    }
}

static long root_call(uint32_t op) {
    root_request.op = op;
    return (long)mos_syscall(SYS_ROOT, (long)(uintptr_t)&root_request, 0, 0);
}

static void root_poll(int timer) {
    (void)timer;
    if (!root_running) return;
    char text[SYS_ROOT_OUTPUT_SIZE + 1];
    for (int round = 0; round < 8; round++) {
        if (root_call(SYS_ROOT_READ) != SYS_SUCCESS) {
            root_request.running = 0;
            break;
        }
        uint32_t length = root_request.output_length;
        memcpy(text, root_request.output, length);
        text[length] = '\0';
        if (length) out(text);
        if (length < SYS_ROOT_OUTPUT_SIZE) break;
    }
    if (!root_request.running) {
        root_running = 0;
        easyui.stoptimer(root_timer);
        root_timer = 0;
        outc("\n", color_dim);
    }
    easyui.redraw();
}

static void root_start(void) {
    memset(&root_request, 0, sizeof root_request);
    strlcpy_safe(root_request.command, root_pending, sizeof root_request.command);
    long result = root_call(SYS_ROOT_RUN);
    if (result == (long)SYS_ERR_BUSY) {
        error_line("root", "another kernel command is still running");
        return;
    }
    if (result != SYS_SUCCESS) {
        error_line("root", "the kernel would not run it");
        return;
    }
    root_running = 1;
    root_timer = easyui.settimer(50, root_poll);
}

static int account_has_password(void) {
#ifdef TERMINAL_HAS_MOSDESK
    /* MOSdesk keeps one account in settings.dat; the kernel's root check
     * reads the same password_enabled flag (login_enabled) from it. */
    mosdesk_settings_t s = mosdesk_settings_load();
    return s.login_enabled != 0u;
#else
    return 1;
#endif
}

static void root_ask_password(void) {
    password_mode = 1;
    input_len = input_caret = 0;
    input[0] = '\0';
}

static void root_ask_confirm(void) {
    password_mode = 2;
    input_len = input_caret = 0;
    input[0] = '\0';
}

/* Enter in confirm mode. An account without a password (auto login) has
 * nothing to check, so the kernel grants the ticket on an empty password;
 * the y/N is what stands between a typo and kernel access. */
static void root_submit_confirm(void) {
    int yes = !strcmp(input, "y") || !strcmp(input, "Y") ||
              !strcmp(input, "yes") || !strcmp(input, "YES");
    outc(ROOT_CONFIRM_PROMPT, color_dim);
    outc(input, color_dim);
    outc("\n", color_dim);
    memset(input, 0, sizeof input);
    input_len = input_caret = 0;
    password_mode = 0;
    if (!yes) {
        outc("root: cancelled\n", color_dim);
        return;
    }
    memset(&root_request, 0, sizeof root_request);
    strlcpy_safe(root_request.username, username, sizeof root_request.username);
    if (root_call(SYS_ROOT_AUTH) != SYS_SUCCESS) {
        error_line("root", "the kernel refused (does your account have a "
                           "password now? run root again)");
        return;
    }
    if (root_pending[0]) root_start();
    else outc("Root granted for 5 minutes.\n", color_ok);
}

/* Enter in password mode. */
static void root_submit_password(void) {
    memset(&root_request, 0, sizeof root_request);
    strlcpy_safe(root_request.username, username, sizeof root_request.username);
    strlcpy_safe(root_request.password, input, sizeof root_request.password);
    memset(input, 0, sizeof input);
    input_len = input_caret = 0;
    outc("[root] password for ", color_dim);
    outc(username, color_dim);
    outc(":\n", color_dim);
    long result = root_call(SYS_ROOT_AUTH);
    memset(root_request.password, 0, sizeof root_request.password);
    if (result != SYS_SUCCESS) {
        if (++password_tries < 3) {
            outc("Sorry, try again.\n", color_error);
            return;                     /* still in password mode */
        }
        error_line("root", "3 incorrect password attempts");
        password_mode = 0;
        return;
    }
    password_mode = 0;
    if (root_pending[0]) root_start();
    else outc("Password accepted for 5 minutes.\n", color_ok);
}

static void cmd_root(int argc, char** argv) {
    memset(&root_request, 0, sizeof root_request);
    long status = root_call(SYS_ROOT_STATUS);
    if (status != SYS_SUCCESS) {
        error_line("root", "this kernel does not support root");
        return;
    }
    if (argc < 2) {
        error_line("root", "usage: root <kernel command>   root -k   root -v");
        return;
    }
    if (!strcmp(argv[1], "-k")) {
        root_call(SYS_ROOT_DROP);
        outc("root will ask for your password again.\n", color_dim);
        return;
    }
    root_pending[0] = '\0';
    if (strcmp(argv[1], "-v")) {
        for (int i = 1; i < argc; i++) {
            if (i > 1) str_append(root_pending, sizeof root_pending, " ");
            /* Keep arguments with spaces together for the kernel. */
            int spaced = strchr(argv[i], ' ') != NULL;
            if (spaced) str_append(root_pending, sizeof root_pending, "\"");
            str_append(root_pending, sizeof root_pending, argv[i]);
            if (spaced) str_append(root_pending, sizeof root_pending, "\"");
        }
    }
    if (root_request.ticket_ms_left && root_pending[0]) {
        root_start();
        return;
    }
    if (!account_has_password()) {
        root_ask_confirm();
        return;
    }
    password_tries = 0;
    root_ask_password();
}

static void cmd_history(int argc, char** argv) {
    (void)argc; (void)argv;
    for (int i = 0; i < history_count; i++) {
        out_u64((uint64_t)i + 1u); out("  "); out(history[i]); out("\n");
    }
}

static void cmd_exit(int argc, char** argv) { (void)argc; (void)argv; easyui.quit(); }

static const command_t commands[] = {
    {"help", cmd_help, "this list"},
    {"ls", cmd_ls, "[-l] [folder]  list a folder"},
    {"cd", cmd_cd, "[folder]  change folder (~ is your home)"},
    {"pwd", cmd_pwd, "show the current folder"},
    {"cat", cmd_cat, "<file>  show a file"},
    {"mkdir", cmd_mkdir, "<folder>  make a folder"},
    {"rmdir", cmd_rmdir, "<folder>  remove an empty folder"},
    {"rm", cmd_rm, "<file>  delete a file"},
    {"touch", cmd_touch, "<file>  make an empty file"},
    {"write", cmd_write, "<file> <text>  write text to a file"},
    {"cp", cmd_cp, "<file> <to>  copy a file"},
    {"mv", cmd_mv, "<file> <to>  move a file"},
    {"echo", cmd_echo, "<text>  print text"},
    {"date", cmd_date, "the date and time"},
    {"uptime", cmd_uptime, "time since start"},
    {"whoami", cmd_whoami, "the signed-in account"},
    {"ps", cmd_ps, "running processes"},
    {"sysinfo", cmd_sysinfo, "CPU, memory and uptime"},
    {"ifconfig", cmd_ifconfig, "network address"},
    {"resolve", cmd_resolve, "<host>  look up an address"},
    {"wget", cmd_wget, "<http://...> [file]  fetch a page"},
    {"run", cmd_run, "<program> [args]  start a program"},
    {"root", cmd_root, "<command>  run a kernel command (asks for your password, or y/N)"},
    {"programs", cmd_programs, "installed programs"},
    {"history", cmd_history, "commands typed so far"},
    {"clear", cmd_clear, "clear the screen"},
    {"exit", cmd_exit, "close the terminal"},
};
#define COMMAND_COUNT (sizeof(commands) / sizeof(commands[0]))

/* Other names for the commands above, as the kernel terminal spells them. */
static const char* const aliases[][2] = {
    {"list", "ls"}, {"dir", "ls"}, {"read", "cat"}, {"md", "mkdir"},
    {"rd", "rmdir"}, {"del", "rm"}, {"remove", "rm"}, {"info", "sysinfo"},
    {"usage", "sysinfo"}, {"cls", "clear"}, {"systime", "date"},
    {"time", "date"}, {"quit", "exit"},
};

/* Kernel terminal commands that need kernel privileges. */
static const char* const kernel_only[] = {
    "format", "initdisk", "mount", "insert", "reboot", "shutdown", "dhcp",
    "installpkg", "pkg", "pkginfo", "importmusic", "chgres", "kbrlayout",
    "trace_on", "trace_off", "trace_stack", "testwrite", "memsize", "gpu",
    "services", "cpu", "diskusage", "lsroot", "listroot", "verifyfile",
    "meta", "mdr", "sleep",
};

static void cmd_help(int argc, char** argv) {
    (void)argc; (void)argv;
    outc("Commands run in user mode, with the rights of any program.\n", color_dim);
    for (unsigned i = 0; i < COMMAND_COUNT; i++) {
        outc(commands[i].name, color_ok);
        for (int pad = (int)strlen(commands[i].name); pad < 10; pad++) out(" ");
        out(commands[i].usage);
        out("\n");
    }
    outc("Anything else is started as a program from 0:/programs or this folder.\n",
         color_dim);
}

/* --------------------------------------------------------------- the line */

static int split_args(char* line, char** argv) {
    int argc = 0;
    char* p = line;
    while (*p && argc < MAX_ARGS) {
        while (*p == ' ') p++;
        if (!*p) break;
        char quote = (*p == '"' || *p == '\'') ? *p++ : 0;
        argv[argc++] = p;
        while (*p && (quote ? *p != quote : *p != ' ')) p++;
        if (*p) *p++ = '\0';
    }
    return argc;
}

static void prompt_text(char* text, size_t cap) {
    strlcpy_safe(text, username, cap);
    str_append(text, cap, " ");
    str_append(text, cap, cwd);
    str_append(text, cap, " > ");
}

static void execute(const char* typed) {
    char prompt[PATH_MAX_ + 48];
    prompt_text(prompt, sizeof prompt);
    outc(prompt, color_prompt);
    out(typed);
    out("\n");
    char line[INPUT_MAX];
    strlcpy_safe(line, typed, sizeof line);
    char* argv[MAX_ARGS];
    int argc = split_args(line, argv);
    if (!argc) return;
    if (history_count == 0 || strcmp(history[history_count - 1], typed)) {
        if (history_count == HISTORY) {
            for (int i = 1; i < HISTORY; i++) strlcpy_safe(history[i - 1], history[i], INPUT_MAX);
            history_count--;
        }
        strlcpy_safe(history[history_count++], typed, INPUT_MAX);
    }
    history_pos = history_count;

    const char* name = argv[0];
    for (unsigned i = 0; i < sizeof(aliases) / sizeof(aliases[0]); i++)
        if (!strcmp(name, aliases[i][0])) name = aliases[i][1];
    for (unsigned i = 0; i < COMMAND_COUNT; i++) {
        if (!strcmp(name, commands[i].name)) {
            commands[i].fn(argc, argv);
            return;
        }
    }
    for (unsigned i = 0; i < sizeof(kernel_only) / sizeof(kernel_only[0]); i++) {
        if (!strcmp(name, kernel_only[i])) {
            outc(name, color_error);
            outc(": needs kernel privileges. Run it as: root ", color_error);
            outc(typed, color_error);
            outc("\n", color_error);
            return;
        }
    }
    char path[PATH_MAX_];
    if (find_program(argv[0], path, sizeof path)) {
        start_program(path, argc - 1, argv + 1);
        return;
    }
    error_line(argv[0], "unknown command (type help)");
}

/* Completes the last word as a file or folder name. */
static void complete(void) {
    int start = input_len;
    while (start > 0 && input[start - 1] != ' ') start--;
    char word[PATH_MAX_];
    int n = input_len - start;
    if (n >= (int)sizeof(word)) return;
    memcpy(word, input + start, (size_t)n);
    word[n] = '\0';
    const char* slash = NULL;
    for (const char* p = word; *p; p++) if (*p == '/') slash = p;
    char folder_arg[PATH_MAX_] = ".";
    const char* prefix = word;
    if (slash) {
        int k = (int)(slash - word) + 1;
        memcpy(folder_arg, word, (size_t)k);
        folder_arg[k] = '\0';
        prefix = slash + 1;
    }
    char folder[PATH_MAX_];
    if (!resolve(folder_arg, folder, sizeof folder)) return;
    static syscall_dirent_t entries[128];
    long count = list_dir(folder, entries, 128);
    int matches = 0;
    char best[64] = "";
    int best_dir = 0;
    size_t plen = strlen(prefix);
    for (long i = 0; i < count; i++) {
        if (strncmp(entries[i].name, prefix, plen)) continue;
        if (!matches) { strlcpy_safe(best, entries[i].name, sizeof best); best_dir = entries[i].type == SYSCALL_DIRENT_TYPE_DIR; }
        else {
            int k = 0;
            while (best[k] && best[k] == entries[i].name[k]) k++;
            best[k] = '\0';
            best_dir = 0;
        }
        matches++;
    }
    if (!matches) return;
    const char* add = best + plen;
    while (*add && input_len + 2 < INPUT_MAX) input[input_len++] = *add++;
    if (matches == 1 && best_dir && input_len + 2 < INPUT_MAX) input[input_len++] = '/';
    input[input_len] = '\0';
    input_caret = input_len;
}

static void set_input(const char* text) {
    strlcpy_safe(input, text, sizeof input);
    input_len = input_caret = (int)strlen(input);
}

static void on_key(int v) {
    int key = v & 0xFF, mods = (v >> 8) & 7;
    int shift = mods & 1, ctrl = (mods >> 1) & 1;
    caret_on = 1;
    if (root_running) {
        /* The command keeps running in the kernel; we just stop waiting. */
        if (ctrl && key == K_C) {
            root_running = 0;
            if (root_timer) easyui.stoptimer(root_timer);
            root_timer = 0;
            outc("^C (the kernel command keeps running)\n", color_dim);
            easyui.redraw();
        }
        return;
    }
    if (ctrl) {
        if (key == K_L) cmd_clear(0, NULL);
        else if (key == K_C) {
            outc("^C\n", color_dim);
            set_input("");
            password_mode = 0;
        }
        easyui.redraw();
        return;
    }
    if (password_mode) {
        if (key == K_ENTER || key == K_KPENTER) {
            if (password_mode == 2) root_submit_confirm();
            else root_submit_password();
        }
        else if (key == K_ESC) {
            memset(input, 0, sizeof input);
            input_len = input_caret = 0;
            password_mode = 0;
            outc("root: cancelled\n", color_dim);
        } else if (key == K_BACKSPACE) {
            if (input_len) input[--input_len] = '\0';
            input_caret = input_len;
        } else {
            char c = (char)mos_syscall(SYS_USB, SYS_USB_KEY_TO_ASCII, key,
                                       shift ? 2 : 0);
            if (c >= 32 && c < 127 && input_len + 1 < 64) {
                input[input_len++] = c;
                input[input_len] = '\0';
                input_caret = input_len;
            }
        }
        easyui.redraw();
        return;
    }
    int visible = easyui.getheight(canvas) / LH - 1;
    switch (key) {
        case K_ENTER: case K_KPENTER: {
            char typed[INPUT_MAX];
            strlcpy_safe(typed, input, sizeof typed);
            set_input("");
            execute(typed);
            break;
        }
        case K_BACKSPACE:
            if (input_caret > 0) {
                memmove(input + input_caret - 1, input + input_caret,
                        (size_t)(input_len - input_caret + 1));
                input_caret--; input_len--;
            }
            break;
        case K_DELETE:
            if (input_caret < input_len) {
                memmove(input + input_caret, input + input_caret + 1,
                        (size_t)(input_len - input_caret));
                input_len--;
            }
            break;
        case K_LEFT: if (input_caret > 0) input_caret--; break;
        case K_RIGHT: if (input_caret < input_len) input_caret++; break;
        case K_HOME: input_caret = 0; break;
        case K_END: input_caret = input_len; break;
        case K_UP:
            if (history_pos > 0) set_input(history[--history_pos]);
            break;
        case K_DOWN:
            if (history_pos < history_count - 1) set_input(history[++history_pos]);
            else { history_pos = history_count; set_input(""); }
            break;
        case K_PGUP: scroll_back += visible > 1 ? visible - 1 : 1; break;
        case K_PGDN:
            scroll_back -= visible > 1 ? visible - 1 : 1;
            if (scroll_back < 0) scroll_back = 0;
            break;
        case K_TAB: complete(); break;
        case K_ESC: set_input(""); break;
        default: {
            char c = (char)mos_syscall(SYS_USB, SYS_USB_KEY_TO_ASCII, key,
                                       shift ? 2 /* left shift */ : 0);
            if (c >= 32 && c < 127 && input_len + 1 < INPUT_MAX) {
                memmove(input + input_caret + 1, input + input_caret,
                        (size_t)(input_len - input_caret + 1));
                input[input_caret++] = c;
                input_len++;
            }
            break;
        }
    }
    if (key != K_PGUP && key != K_PGDN) scroll_back = 0;
    easyui.redraw();
}

static void on_draw(int v) {
    (void)v;
    int cx = easyui.getx(canvas), cy = easyui.gety(canvas);
    int cw = easyui.getwidth(canvas), ch = easyui.getheight(canvas);
    easyui.fillrect(cx, cy, cw, ch, 0xFF16191Cu);

    /* The input line, wrapped the same way as output, sits below it. */
    char prompt[PATH_MAX_ + 48];
    prompt_text(prompt, sizeof prompt);
    if (password_mode == 2) {
        strlcpy_safe(prompt, ROOT_CONFIRM_PROMPT, sizeof prompt);
    } else if (password_mode) {
        strlcpy_safe(prompt, "[root] password for ", sizeof prompt);
        str_append(prompt, sizeof prompt, username);
        str_append(prompt, sizeof prompt, ": ");
    } else if (root_running) {
        strlcpy_safe(prompt, "(running in the kernel - Ctrl+C stops waiting)",
                     sizeof prompt);
    }
    int prompt_w = easyui.textwidth(prompt, TS);
    int rows = (ch - 2 * PAD) / LH;
    int max_scroll = line_count + 1 - rows;
    if (max_scroll < 0) max_scroll = 0;
    if (scroll_back > max_scroll) scroll_back = max_scroll;
    int first = line_count + 1 - rows - scroll_back;
    if (first < 0) first = 0;
    int y = cy + PAD;
    for (int i = first; i < line_count && y + LH <= cy + ch; i++, y += LH) {
        line_t* line = line_at(i);
        easyui.drawtext(cx + PAD, y, line->text, line->color, TS);
    }
    if (scroll_back == 0 && y + LH <= cy + ch) {
        easyui.drawtext(cx + PAD, y, prompt,
                        root_running ? color_dim : color_prompt, TS);
        /* Show the end of a line too long for the window; nothing of a
         * password. */
        const char* shown = password_mode == 1 || root_running ? "" : input;
        int room = cw - 2 * PAD - prompt_w - 6;
        while (*shown && easyui.textwidth(shown, TS) > room) shown++;
        easyui.drawtext(cx + PAD + prompt_w, y, shown, color_text, TS);
        if (caret_on) {
            char before[INPUT_MAX];
            int k = password_mode == 1 || root_running ? 0
                  : input_caret - (int)(shown - input);
            if (k < 0) k = 0;
            memcpy(before, shown, (size_t)k);
            before[k] = '\0';
            easyui.fillrect(cx + PAD + prompt_w + easyui.textwidth(before, TS),
                            y + 1, 2, LH - 3, color_prompt);
        }
    } else if (scroll_back) {
        easyui.drawtext(cx + cw - PAD - easyui.textwidth("PgDn for the newest lines", TS),
                        cy + ch - LH - 2, "PgDn for the newest lines", color_dim, TS);
    }
}

static void on_blink(int timer) { (void)timer; caret_on = !caret_on; easyui.redraw(); }
static void on_resize(int w) {
    (void)w;
    easyui.setposition(canvas, 0, 0);
    easyui.setsize(canvas, easyui.width(), easyui.height());
    easyui.redraw();
}
static void on_close(int v) { (void)v; easyui.quit(); }

int main(int argc, char** argv) {
    lines = (line_t*)malloc(sizeof(line_t) * MAX_LINES);
    if (!lines) { mos_puts("terminal: out of memory\n"); return 1; }
    if (!easyui.init("Terminal", 720, 440)) {
        mos_puts("terminal: start MOSdesk first\n");
        return 1;
    }
    canvas = easyui.createcanvas(0, 0, 720, 440);
    easyui.recallfunc(canvas, ONDRAW, on_draw);
    easyui.recallfunc(EASYUI_WINDOW, ONKEY, on_key);
    easyui.recallfunc(EASYUI_WINDOW, ONRESIZE, on_resize);
    easyui.recallfunc(EASYUI_WINDOW, ONCLOSE, on_close);
    easyui.settimer(530, on_blink);
    out_color = color_text;
#ifdef TERMINAL_HAS_MOSDESK
    mosdesk_settings_t s = mosdesk_settings_load();
    if (s.username[0]) strlcpy_safe(username, s.username, sizeof username);
    char home[PATH_MAX_];
    if (mosdesk_user_path(home, sizeof home, username, NULL) && mos_is_dir(home) == 1)
        strlcpy_safe(cwd, home, sizeof cwd);
#endif
    if (argc > 1 && mos_is_dir(argv[1]) == 1) strlcpy_safe(cwd, argv[1], sizeof cwd);
    on_resize(0);
    outc("MOSdesk Terminal - commands run in user mode. Type help.\n", color_dim);
    return easyui.run();
}
