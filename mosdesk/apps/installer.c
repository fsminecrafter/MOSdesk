/*
 * Installer - puts Minimal-OS (with MOSdesk) on a disk so it boots by
 * itself, on BIOS and UEFI PCs.
 *
 * Lists the disks (SYS_STORAGE), lets you pick one and a partition
 * scheme, asks for your account password and then runs the kernel's
 * 'install' command as root (SYS_ROOT), showing its progress.
 *
 * Disks holding the running system are listed but cannot be picked.
 */
#include "easyui.h"
#include "storage.h"
#include "stdlib.h"
#include "string.h"
#include "../settings.h"

#define MAX_DEVICES 48
#define LOG_SIZE 4096

static syscall_storage_device_t g_devices[MAX_DEVICES];
static int g_disk_index[MAX_DEVICES];      // list row -> g_devices index
static int g_disk_rows;
static int g_payload;
static char g_username[MOSDESK_USERNAME_SIZE];

static int list, info, gpt, mbr, confirm, password, install_button, rescan;
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
}

static void describe(void) {
    const syscall_storage_device_t* d = selected_disk();
    char text[256] = "";
    if (!d) {
        easyui.settext(info, g_disk_rows ? "Pick a disk." : "No disks found.");
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

    char command[96] = "install ";
    append(command, sizeof command, d->name);
    if (easyui.getvalue(mbr)) append(command, sizeof command, " --mbr");
    append(command, sizeof command, " --yes");

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

int main(void) {
    mosdesk_settings_t settings = mosdesk_settings_load();
    memcpy(g_username, settings.username, sizeof g_username);
    g_username[sizeof g_username - 1] = '\0';

    easyui.init("Install Minimal-OS", 560, 470);
    easyui.setsmoothtext(1);

    int title = easyui.createlabel(20, 10, 520, 32, "Install Minimal-OS");
    easyui.settextsize(title, 3);
    easyui.createlabel(20, 44, 520, 20,
                       "Copies the system and MOSdesk to a disk that then boots by itself.");

    easyui.createlabel(20, 76, 200, 20, "Disk");
    list = easyui.createlistbox(20, 98, 430, 120);
    rescan = easyui.createbutton(460, 98, 80, 30, "Rescan");
    easyui.setstyle(rescan, EASYUI_SECONDARY);
    info = easyui.createlabel(20, 222, 520, 36, "");

    easyui.createlabel(20, 262, 160, 20, "Partition scheme");
    gpt = easyui.createradio(180, 260, 170, 24, "GPT (recommended)", 1);
    mbr = easyui.createradio(360, 260, 180, 24, "MBR (older PCs)", 1);
    easyui.setvalue(gpt, 1);

    confirm = easyui.createcheckbox(20, 292, 520, 24, "Erase this disk and install Minimal-OS on it", 0);

    easyui.createlabel(20, 324, 260, 20, "Password for your account");
    password = easyui.createtextinput(20, 346, 300, 32, settings.username[0] ? "password" : "");
    easyui.setpassword(password, 1);
    install_button = easyui.createbutton(400, 346, 140, 32, "Install");
    easyui.setstyle(install_button, EASYUI_DANGER);

    progress = easyui.createprogress(20, 392, 520, 14, 100);
    status = easyui.createlabel(20, 410, 520, 20, "");
    log_label = easyui.createlabel(20, 432, 400, 30, "");
    done_button = easyui.createbutton(440, 428, 100, 30, "Close");
    easyui.setvisible(done_button, 0);

    easyui.recallfunc(list, ONCHANGE, on_select);
    easyui.recallfunc(confirm, ONCHANGE, on_confirm);
    easyui.recallfunc(rescan, ONCLICK, on_rescan);
    easyui.recallfunc(install_button, ONCLICK, on_install);
    easyui.recallfunc(done_button, ONCLICK, on_done);

    scan();
    return easyui.run();
}
