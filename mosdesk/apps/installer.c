/*
 * Installer - puts Minimal-OS (with MOSdesk) on a disk so it boots by
 * itself, on BIOS and UEFI PCs.
 *
 * A wizard of four pages:
 *   1. Disk          the disks (SYS_STORAGE) and the partition scheme
 *   2. Apps          extra apps on the install media (Doom, the Farcade
 *                    editor, ...) to copy along; all are picked at first
 *   3. Repositories  Deliver servers the installed system knows; the main
 *                    one (DEFAULT_REPO) is picked unless you opt out, and
 *                    you can add your own
 *   4. Install       summary, confirmation and your account password; runs
 *                    the kernel's 'install' command as root (SYS_ROOT)
 *                    with --apps and --repo and shows its progress
 *
 * Disks holding the running system are listed but cannot be picked.
 * Repositories go to 0:/etc/dlr/repos on the new disk; 'dlr' and Deliver
 * Manager add each one the first time the installed system reaches it.
 */
#include "easyui.h"
#include "storage.h"
#include "stdlib.h"
#include "string.h"
#include "../settings.h"

#define MAX_DEVICES 48
#define LOG_SIZE 4096
#define MAX_APPS 8
#define MAX_REPOS 5             // the main one plus four of your own
#define DEFAULT_REPO "daserver.taildf3e90.ts.net:7870"

// Kernels before SYS_STORAGE_APPS: the call fails and the page says so.
#ifndef SYS_STORAGE_APPS
#define SYS_STORAGE_APPS 3u
#endif

enum { PAGE_DISK, PAGE_APPS, PAGE_REPOS, PAGE_INSTALL, PAGE_COUNT };
static const char* const g_page_names[PAGE_COUNT] = {
    "1  Disk", "2  Apps", "3  Repositories", "4  Install"
};
static int g_page;

// Widgets of each page, shown and hidden together.
#define PAGE_MAX_WIDGETS 32
static int g_page_widgets[PAGE_COUNT][PAGE_MAX_WIDGETS];
static int g_page_widget_count[PAGE_COUNT];
static int g_building = -1;     // page the next widgets belong to

static int page_add(int widget) {
    if (g_building >= 0 && g_page_widget_count[g_building] < PAGE_MAX_WIDGETS)
        g_page_widgets[g_building][g_page_widget_count[g_building]++] = widget;
    return widget;
}

// Apps on the install media.
static char g_app_names[MAX_APPS][48];
static uint64_t g_app_bytes[MAX_APPS];
static int g_app_count;
static int g_app_checks[MAX_APPS];
static int g_apps_note;

// Repositories: [0] is DEFAULT_REPO.
static char g_repo_specs[MAX_REPOS][64];
static int g_repo_checks[MAX_REPOS];
static int g_repo_count;
static int repo_input, repo_add;

static int step_labels[PAGE_COUNT];
static int back_button, next_button, summary;

static syscall_storage_device_t g_devices[MAX_DEVICES];
static int g_disk_index[MAX_DEVICES];      // list row -> g_devices index
static int g_disk_rows;
static int g_payload;
static char g_username[MOSDESK_USERNAME_SIZE];

static int list, info, gpt, mbr, confirm, password, install_button, rescan;
static void show_page(int page);
static int status, progress, log_label, done_button;
static int g_timer = -1;
static char g_log[LOG_SIZE];
static int g_running;

static void number(char* out, uint64_t v) {
    char t[24];
    int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) *out++ = t[--n];
    *out = '\0';
}

static void append(char* dst, size_t size, const char* src) {
    size_t n = strlen(dst);
    while (*src && n + 1 < size) dst[n++] = *src++;
    dst[n] = '\0';
}

static void size_text(uint64_t bytes, char* out, size_t size) {
    static const char* units[] = { "B", "KiB", "MiB", "GiB", "TiB" };
    int u = 0;
    uint64_t whole = bytes, tenth = 0;
    while (whole >= 1024 && u < 4) { tenth = (whole % 1024) * 10 / 1024; whole /= 1024; u++; }
    char n[24];
    out[0] = '\0';
    number(n, whole);
    append(out, size, n);
    if (u) {
        append(out, size, ".");
        number(n, tenth);
        append(out, size, n);
    }
    append(out, size, " ");
    append(out, size, units[u]);
}

static const syscall_storage_device_t* selected_disk(void) {
    int row = easyui.getvalue(list);
    if (row < 0 || row >= g_disk_rows) return NULL;
    return &g_devices[g_disk_index[row]];
}

static void update_buttons(void) {
    const syscall_storage_device_t* d = selected_disk();
    int ok = !g_running && g_payload && d && !(d->flags & SYSCALL_STORAGE_FLAG_IN_USE) &&
             easyui.getvalue(confirm);
    easyui.setenabled(install_button, ok);
    easyui.setenabled(mbr, d && d->size_bytes <= (2ull << 40));
    int disk_ok = g_payload && d && !(d->flags & SYSCALL_STORAGE_FLAG_IN_USE);
    easyui.setenabled(next_button, !g_running && (g_page != PAGE_DISK || disk_ok));
    easyui.setenabled(back_button, !g_running && g_page > 0);
}

static void describe(void) {
    const syscall_storage_device_t* d = selected_disk();
    char text[256] = "";
    if (!d) {
        int usable = 0;
        for (int r = 0; r < g_disk_rows; r++)
            if (!(g_devices[g_disk_index[r]].flags & SYSCALL_STORAGE_FLAG_IN_USE)) usable++;
        easyui.settext(info, !g_disk_rows ? "No disks found." :
                       usable ? "Pick a disk." :
                       "Every disk is in use by the running system. Attach another disk and press Rescan.");
        update_buttons();
        return;
    }
    char size[24];
    size_text(d->size_bytes, size, sizeof size);
    append(text, sizeof text, d->name);
    append(text, sizeof text, ": ");
    append(text, sizeof text, size);
    if (d->model[0]) { append(text, sizeof text, ", "); append(text, sizeof text, d->model); }
    if (d->flags & SYSCALL_STORAGE_FLAG_IN_USE)
        append(text, sizeof text, ". In use by the running system; pick another disk.");
    else
        append(text, sizeof text, ". Everything on it will be erased.");
    easyui.settext(info, text);
    easyui.settext(confirm, "Erase this disk and install Minimal-OS on it");
    update_buttons();
}

static void scan(void) {
    long n = mos_storage_list(g_devices, MAX_DEVICES);
    if (n < 0) n = 0;
    if (n > MAX_DEVICES) n = MAX_DEVICES;
    char boot[16];
    mos_storage_status(&g_payload, boot);
    easyui.clearitems(list);
    g_disk_rows = 0;
    for (long i = 0; i < n; i++) {
        syscall_storage_device_t* d = &g_devices[i];
        if (d->kind != SYSCALL_STORAGE_KIND_DISK) continue;
        char row[128] = "", size[24];
        size_text(d->size_bytes, size, sizeof size);
        append(row, sizeof row, d->name);
        append(row, sizeof row, "   ");
        append(row, sizeof row, size);
        append(row, sizeof row, "   ");
        append(row, sizeof row, d->model[0] ? d->model : "disk");
        if (d->flags & SYSCALL_STORAGE_FLAG_BOOT) append(row, sizeof row, "   (system disk)");
        else if (d->flags & SYSCALL_STORAGE_FLAG_IN_USE) append(row, sizeof row, "   (in use)");
        easyui.additem(list, row);
        g_disk_index[g_disk_rows++] = (int)i;
    }
    // Preselect the first disk that can be used.
    for (int r = 0; r < g_disk_rows; r++) {
        if (!(g_devices[g_disk_index[r]].flags & SYSCALL_STORAGE_FLAG_IN_USE)) {
            easyui.setvalue(list, r);
            break;
        }
    }
    if (!g_payload) {
        easyui.settext(status, "No install payload: start Minimal-OS from the ISO or an "
                               "installed disk to install.");
    }
    describe();
}

static void on_select(int value) { (void)value; describe(); }
static void on_confirm(int value) { (void)value; update_buttons(); }
static void on_rescan(int value) { (void)value; scan(); }

// Picks the last "[ NN%] text" line out of the command's output.
static void show_progress(void) {
    const char* last = NULL;
    for (const char* p = g_log; *p; p++) {
        if (*p == '[' && (p == g_log || p[-1] == '\n')) last = p;
    }
    if (!last) return;
    int pct = 0;
    const char* p = last + 1;
    while (*p == ' ') p++;
    while (*p >= '0' && *p <= '9') pct = pct * 10 + (*p++ - '0');
    if (*p == '%') p++;
    if (*p == ']') p++;
    while (*p == ' ') p++;
    char line[96];
    size_t n = 0;
    while (p[n] && p[n] != '\n' && n + 1 < sizeof line) { line[n] = p[n]; n++; }
    line[n] = '\0';
    easyui.setvalue(progress, pct);
    if (line[0]) easyui.settext(status, line);
}

static void finish(void) {
    g_running = 0;
    if (g_timer >= 0) easyui.stoptimer(g_timer);
    g_timer = -1;
    show_progress();
    if (strstr(g_log, "Minimal-OS is installed")) {
        easyui.setvalue(progress, 100);
        easyui.settext(status, "Done. Remove the install media and restart the computer.");
        const char* warn = strstr(g_log, "Warning: ");
        easyui.settext(log_label, warn ? warn : "The disk now boots on BIOS and UEFI PCs.");
    } else {
        const char* why = strstr(g_log, "Install failed: ");
        easyui.settext(status, "The install did not finish.");
        easyui.settext(log_label, why ? why : g_log);
    }
    easyui.setvisible(done_button, 1);
    easyui.setvalue(confirm, 0);       // ask again before another install
    easyui.setenabled(list, 1);
    scan();
}

static void poll(int value) {
    (void)value;
    long r = mos_root_read(g_log, sizeof g_log);
    show_progress();
    if (r <= 0) finish();
}

// ---------------------------------------------------------------- apps

static void load_apps(void) {
    static char text[1024];
    syscall_storage_request_t r;
    memset(&r, 0, sizeof(r));
    r.size = sizeof(r);
    r.version = SYS_STORAGE_ABI_VERSION;
    r.op = SYS_STORAGE_APPS;
    r.capacity = sizeof text;
    r.devices = (syscall_storage_device_t*)(void*)text;
    text[0] = '\0';
    long n = (long)mos_syscall1(SYS_STORAGE, (uint64_t)(uintptr_t)&r);
    g_app_count = 0;
    if (n <= 0) return;
    // "name bytes\n" per app.
    const char* p = text;
    while (*p && g_app_count < MAX_APPS) {
        size_t k = 0;
        while (*p && *p != ' ' && *p != '\n' && k + 1 < sizeof g_app_names[0])
            g_app_names[g_app_count][k++] = *p++;
        g_app_names[g_app_count][k] = '\0';
        uint64_t bytes = 0;
        while (*p == ' ') p++;
        while (*p >= '0' && *p <= '9') bytes = bytes * 10 + (uint64_t)(*p++ - '0');
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
        if (k) g_app_bytes[g_app_count++] = bytes;
    }
}

// A friendlier line for the apps the build puts on the media.
static void app_title(int i, char* out, size_t size) {
    const char* n = g_app_names[i];
    const char* what = NULL;
    if (strcmp(n, "Doom") == 0) what = "Doom (1993), with the Freedoom game data";
    else if (strcmp(n, "FarcadeEditor") == 0) what = "Farcade Engine editor";
    else if (strcmp(n, "feplayer") == 0) what = "Farcade game player (runs exported games)";
    char sz[24];
    size_text(g_app_bytes[i], sz, sizeof sz);
    out[0] = '\0';
    append(out, size, what ? what : n);
    append(out, size, "   (");
    append(out, size, sz);
    append(out, size, ")");
}

// ---------------------------------------------------------------- repositories

static int repo_y(int i) { return 136 + i * 30; }

static void add_repo_row(const char* spec, int checked) {
    if (g_repo_count >= MAX_REPOS) return;
    int i = g_repo_count++;
    size_t k = 0;
    while (spec[k] && k + 1 < sizeof g_repo_specs[i]) { g_repo_specs[i][k] = spec[k]; k++; }
    g_repo_specs[i][k] = '\0';
    char text[112] = "";
    append(text, sizeof text, g_repo_specs[i]);
    if (i == 0) append(text, sizeof text, "   (main repository)");
    int saved = g_building;
    g_building = PAGE_REPOS;
    g_repo_checks[i] = page_add(easyui.createcheckbox(20, repo_y(i), 520, 24, text, checked));
    g_building = saved;
    easyui.setvisible(g_repo_checks[i], g_page == PAGE_REPOS);
    easyui.setposition(repo_input, 20, repo_y(g_repo_count) + 6);
    easyui.setposition(repo_add, 400, repo_y(g_repo_count) + 6);
    easyui.setenabled(repo_add, g_repo_count < MAX_REPOS);
    easyui.setenabled(repo_input, g_repo_count < MAX_REPOS);
}

// host[:port] or tls://host[:port], letters, digits, '.', '-' only.
static int repo_spec_ok(const char* s) {
    if (strncmp(s, "tls://", 6) == 0) s += 6;
    int host = 0;
    while (*s && *s != ':') {
        char c = *s++;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '.' || c == '-')) return 0;
        host++;
    }
    if (!host) return 0;
    if (*s == ':') {
        s++;
        int port = 0, digits = 0;
        while (*s >= '0' && *s <= '9') { port = port * 10 + (*s++ - '0'); digits++; if (port > 65535) return 0; }
        if (!digits || !port) return 0;
    }
    return *s == '\0';
}

static void on_repo_add(int value) {
    (void)value;
    const char* spec = easyui.gettext(repo_input);
    while (*spec == ' ') spec++;
    if (!*spec) return;
    if (!repo_spec_ok(spec)) {
        easyui.messagebox("Installer", "Type a Deliver server as host or host:port, for "
                                       "example 192.168.1.20 or myserver.lan:4242.");
        return;
    }
    for (int i = 0; i < g_repo_count; i++) {
        if (strcmp(g_repo_specs[i], spec) == 0) {
            easyui.setvalue(g_repo_checks[i], 1);
            easyui.settext(repo_input, "");
            return;
        }
    }
    add_repo_row(spec, 1);
    easyui.settext(repo_input, "");
}

// ---------------------------------------------------------------- install

static int build_command(const syscall_storage_device_t* d, char* out, size_t size) {
    out[0] = '\0';
    append(out, size, "install ");
    append(out, size, d->name);
    if (easyui.getvalue(mbr)) append(out, size, " --mbr");
    if (g_app_count) {
        char apps[200] = "";
        for (int i = 0; i < g_app_count; i++) {
            if (!easyui.getvalue(g_app_checks[i])) continue;
            if (apps[0]) append(apps, sizeof apps, ",");
            append(apps, sizeof apps, g_app_names[i]);
        }
        append(out, size, " --apps ");
        append(out, size, apps[0] ? apps : "none");
    }
    for (int i = 0; i < g_repo_count; i++) {
        if (!easyui.getvalue(g_repo_checks[i])) continue;
        append(out, size, " --repo ");
        append(out, size, g_repo_specs[i]);
    }
    append(out, size, " --yes");
    return strlen(out) + 1 < size && strlen(out) < 255;
}

static void update_summary(void) {
    const syscall_storage_device_t* d = selected_disk();
    char text[512] = "";
    char sz[24];
    append(text, sizeof text, "Disk: ");
    if (d) {
        size_text(d->size_bytes, sz, sizeof sz);
        append(text, sizeof text, d->name);
        append(text, sizeof text, " (");
        append(text, sizeof text, sz);
        append(text, sizeof text, easyui.getvalue(mbr) ? ", MBR)" : ", GPT)");
    } else {
        append(text, sizeof text, "none picked");
    }
    append(text, sizeof text, "\nApps: MOSdesk and its built-in apps");
    for (int i = 0; i < g_app_count; i++) {
        if (!easyui.getvalue(g_app_checks[i])) continue;
        append(text, sizeof text, ", ");
        append(text, sizeof text, g_app_names[i]);
    }
    append(text, sizeof text, "\nRepositories: ");
    int any = 0;
    for (int i = 0; i < g_repo_count; i++) {
        if (!easyui.getvalue(g_repo_checks[i])) continue;
        if (any) append(text, sizeof text, ", ");
        append(text, sizeof text, g_repo_specs[i]);
        any = 1;
    }
    if (!any) append(text, sizeof text, "none (add some later with 'dlr add')");
    easyui.settext(summary, text);
}

static void on_install(int value) {
    (void)value;
    const syscall_storage_device_t* d = selected_disk();
    if (!d || g_running) return;

    if (mos_root_auth(g_username, easyui.gettext(password)) != 0) {
        easyui.messagebox("Installer", "Wrong password. The installer needs the password "
                                       "of your MOSdesk account.");
        return;
    }
    easyui.settext(password, "");

    char command[256];
    if (!build_command(d, command, sizeof command)) {
        easyui.messagebox("Installer", "Too many apps and repositories for one install "
                                       "command. Untick a repository and try again.");
        return;
    }

    g_log[0] = '\0';
    if (mos_root_run(command) != 0) {
        easyui.messagebox("Installer", "Could not start the install (another root command "
                                       "may be running).");
        return;
    }
    g_running = 1;
    update_buttons();
    easyui.setenabled(list, 0);
    easyui.setvalue(progress, 0);
    easyui.settext(status, "Installing...");
    easyui.settext(log_label, "Do not turn the computer off.");
    g_timer = easyui.settimer(250, poll);
}

static void on_done(int value) { (void)value; easyui.quit(); }

static void show_page(int page) {
    if (page < 0 || page >= PAGE_COUNT) return;
    g_page = page;
    for (int p = 0; p < PAGE_COUNT; p++) {
        for (int k = 0; k < g_page_widget_count[p]; k++)
            easyui.setvisible(g_page_widgets[p][k], p == page);
        easyui.setstyle(step_labels[p], p == page ? EASYUI_PRIMARY : EASYUI_SECONDARY);
        easyui.setenabled(step_labels[p], p <= page);
    }
    easyui.setvisible(next_button, page != PAGE_INSTALL);
    if (page == PAGE_INSTALL) update_summary();
    if (page == PAGE_REPOS) easyui.setfocus(repo_input);
    if (page == PAGE_INSTALL) easyui.setfocus(password);
    update_buttons();
}

static void on_back(int value) { (void)value; if (!g_running) show_page(g_page - 1); }
static void on_next(int value) { (void)value; if (!g_running) show_page(g_page + 1); }

// The step strip at the top doubles as navigation to pages already seen.
static void on_step(int value) {
    (void)value;
    int w = easyui.current();
    for (int p = 0; p < PAGE_COUNT; p++)
        if (step_labels[p] == w && p <= g_page && !g_running) show_page(p);
}

int main(void) {
    mosdesk_settings_t settings = mosdesk_settings_load();
    memcpy(g_username, settings.username, sizeof g_username);
    g_username[sizeof g_username - 1] = '\0';

    easyui.init("Install Minimal-OS", 560, 470);
    easyui.setsmoothtext(1);

    int title = easyui.createlabel(20, 10, 520, 32, "Install Minimal-OS");
    easyui.settextsize(title, 3);
    for (int p = 0; p < PAGE_COUNT; p++) {
        step_labels[p] = easyui.createbutton(20 + p * 130, 48, 122, 26, g_page_names[p]);
        easyui.recallfunc(step_labels[p], ONCLICK, on_step);
    }
    easyui.createseparator(20, 82, 520);

    // 1. Disk
    g_building = PAGE_DISK;
    page_add(easyui.createlabel(20, 92, 520, 20,
                                "Pick the disk to install on. Everything on it is erased."));
    list = page_add(easyui.createlistbox(20, 116, 430, 132));
    rescan = page_add(easyui.createbutton(460, 116, 80, 30, "Rescan"));
    easyui.setstyle(rescan, EASYUI_SECONDARY);
    info = page_add(easyui.createlabel(20, 254, 520, 36, ""));
    page_add(easyui.createlabel(20, 300, 160, 20, "Partition scheme"));
    gpt = page_add(easyui.createradio(180, 298, 170, 24, "GPT (recommended)", 1));
    mbr = page_add(easyui.createradio(360, 298, 180, 24, "MBR (older PCs)", 1));
    easyui.setvalue(gpt, 1);
    page_add(easyui.createlabel(20, 334, 520, 40,
                                "The disk then boots by itself on BIOS and UEFI PCs."));

    // 2. Apps
    g_building = PAGE_APPS;
    page_add(easyui.createlabel(20, 92, 520, 20, "Apps from the install media"));
    load_apps();
    for (int i = 0; i < g_app_count; i++) {
        char text[128];
        app_title(i, text, sizeof text);
        g_app_checks[i] = page_add(easyui.createcheckbox(20, 120 + i * 30, 520, 24, text, 1));
    }
    g_apps_note = page_add(easyui.createlabel(20, 128 + g_app_count * 30, 520, 60,
        g_app_count ? "MOSdesk and its built-in apps are always installed. Untick the "
                      "ones you do not want; Deliver can add them later."
                    : "This install media carries no extra apps. MOSdesk and its built-in "
                      "apps are always installed; more come from Deliver ('dlr install')."));

    // 3. Repositories
    g_building = PAGE_REPOS;
    page_add(easyui.createlabel(20, 92, 520, 36,
        "Deliver servers the new system installs packages from. Each is added the "
        "first time the installed system reaches it (like 'dlr add')."));
    repo_input = page_add(easyui.createtextinput(20, 0, 370, 30, "host:port"));
    easyui.setmaxlength(repo_input, 63);
    repo_add = page_add(easyui.createbutton(400, 0, 140, 30, "Add repository"));
    easyui.setstyle(repo_add, EASYUI_SECONDARY);
    add_repo_row(DEFAULT_REPO, 1);          // the main one: opt out by unticking

    // 4. Install
    g_building = PAGE_INSTALL;
    summary = page_add(easyui.createlabel(20, 92, 520, 92, ""));
    confirm = page_add(easyui.createcheckbox(20, 190, 520, 24,
                                             "Erase this disk and install Minimal-OS on it", 0));
    page_add(easyui.createlabel(20, 222, 260, 20, "Password for your account"));
    password = page_add(easyui.createtextinput(20, 244, 300, 32,
                                               settings.username[0] ? "password" : ""));
    easyui.setpassword(password, 1);
    install_button = page_add(easyui.createbutton(400, 244, 140, 32, "Install"));
    easyui.setstyle(install_button, EASYUI_DANGER);
    progress = page_add(easyui.createprogress(20, 296, 520, 14, 100));
    status = page_add(easyui.createlabel(20, 316, 520, 20, ""));
    log_label = page_add(easyui.createlabel(20, 340, 520, 70, ""));
    g_building = -1;

    easyui.createseparator(20, 420, 520);
    back_button = easyui.createbutton(20, 430, 100, 30, "Back");
    easyui.setstyle(back_button, EASYUI_SECONDARY);
    next_button = easyui.createbutton(440, 430, 100, 30, "Next");
    done_button = easyui.createbutton(440, 430, 100, 30, "Close");
    easyui.setvisible(done_button, 0);

    easyui.recallfunc(list, ONCHANGE, on_select);
    easyui.recallfunc(confirm, ONCHANGE, on_confirm);
    easyui.recallfunc(rescan, ONCLICK, on_rescan);
    easyui.recallfunc(install_button, ONCLICK, on_install);
    easyui.recallfunc(done_button, ONCLICK, on_done);
    easyui.recallfunc(back_button, ONCLICK, on_back);
    easyui.recallfunc(next_button, ONCLICK, on_next);
    easyui.recallfunc(repo_add, ONCLICK, on_repo_add);
    easyui.recallfunc(repo_input, ONSUBMIT, on_repo_add);

    scan();
    show_page(PAGE_DISK);
    return easyui.run();
}
