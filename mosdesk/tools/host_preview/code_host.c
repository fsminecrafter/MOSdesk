/* code_host.c - a host (Linux) stand-in for the Minimal-OS kernel, just enough
 * for easyui apps: files below a sandbox folder, a heap, one window whose
 * pixels are kept for screenshots, and a scripted event queue.
 * Built by code_shot.sh together with code_shot.c; the SDK headers it uses
 * have their `int $0x80` patched into calls to host_syscall() below. */
#define _GNU_SOURCE
#include <dirent.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "syscall.h"          /* the real SDK header (types and numbers) */
#include "code_host.h"

long host_ms;
static char root[512] = ".";
static void (*tick_cb)(int);
static int frame_no;
static uint32_t* pix; static int fw, fh;
static char title[128];

#define QMAX 256
static syscall_window_event_t q[QMAX]; static int qh, qt;
static void push(const syscall_window_event_t* e) { if ((qt + 1) % QMAX != qh) { q[qt] = *e; qt = (qt + 1) % QMAX; } }

void host_set_root(const char* dir) { strncpy(root, dir, sizeof root - 1); }
void host_set_tick(void (*cb)(int)) { tick_cb = cb; }
const char* host_title(void) { return title; }
void host_get_size(int* w, int* h) { *w = fw; *h = fh; }

void host_push_key(int key, int mods, int down) {
    syscall_window_event_t e; memset(&e, 0, sizeof e);
    e.type = SYSCALL_WINDOW_EVENT_KEY; e.key = (uint32_t)key; e.buttons = down ? 1u : 0u; e.mods = (uint8_t)(down ? mods : 0);
    push(&e);
}
void host_push_mouse(int x, int y, int buttons) {
    syscall_window_event_t e; memset(&e, 0, sizeof e);
    e.type = SYSCALL_WINDOW_EVENT_MOUSE; e.x = x; e.y = y; e.buttons = (uint32_t)buttons;
    push(&e);
}
void host_push_wheel(int x, int y, int dir) {
    syscall_window_event_t e; memset(&e, 0, sizeof e);
    e.type = SYSCALL_WINDOW_EVENT_MOUSE_WHEEL; e.x = x; e.y = y; e.wheel = dir;
    push(&e);
}
void host_push_close(void) { syscall_window_event_t e; memset(&e, 0, sizeof e); e.type = SYSCALL_WINDOW_EVENT_CLOSE_REQUEST; push(&e); }
void host_push_resize(int w, int h) {
    syscall_window_event_t e; memset(&e, 0, sizeof e); e.type = SYSCALL_WINDOW_EVENT_RESIZED; e.width = w; e.height = h; push(&e);
}
void host_save_ppm(const char* path) {
    FILE* f = fopen(path, "wb"); int i;
    if (!f || !pix) return;
    fprintf(f, "P6\n%d %d\n255\n", fw, fh);
    for (i = 0; i < fw * fh; i++) { unsigned char c[3] = { (pix[i] >> 16) & 255, (pix[i] >> 8) & 255, pix[i] & 255 }; fwrite(c, 1, 3, f); }
    fclose(f);
}

/* "0:/x/y" -> "<root>/x/y" */
static const char* hostpath(const char* p, char* out, size_t cap) {
    if (p[0] >= '0' && p[0] <= '9' && p[1] == ':') { snprintf(out, cap, "%s%s", root, p + 2); return out; }
    snprintf(out, cap, "%s/%s", root, p); return out;
}

static int usb_ascii(int key, int shift) {
    static const char lower[] = "abcdefghijklmnopqrstuvwxyz1234567890";
    static const char upper[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ!@#$%^&*()";
    static const char sym_lo[] = "-=[]\\#;'`,./";
    static const char sym_up[] = "_+{}|~:\"~<>?";
    if (key >= 4 && key <= 39) return shift ? upper[key - 4] : lower[key - 4];
    if (key == 44) return ' ';
    if (key >= 45 && key <= 56) return shift ? sym_up[key - 45] : sym_lo[key - 45];
    return 0;
}

static long window_call(syscall_window_request_t* r) {
    switch (r->op) {
    case SYS_WINDOW_CREATE:
        fw = (int)r->width; fh = (int)r->height;
        free(pix); pix = calloc((size_t)fw * (size_t)fh, 4);
        strncpy(title, r->title, sizeof title - 1);
        return 1;
    case SYS_WINDOW_SET_TITLE: strncpy(title, r->title, sizeof title - 1); return 0;
    case SYS_WINDOW_PRESENT: {
        int y, w = (int)r->width, h = (int)r->height, x0 = (int)r->x, y0 = (int)r->y;
        if (!pix) return 0;
        for (y = 0; y < h && y0 + y < fh; y++) memcpy(pix + (size_t)(y0 + y) * (size_t)fw + (size_t)x0,
            (const uint8_t*)r->pixels + (size_t)(y0 + y) * r->pitch + (size_t)x0 * 4u, (size_t)(w < fw - x0 ? w : fw - x0) * 4u);
        return 0;
    }
    case SYS_WINDOW_NEXT_EVENT:
        if (qh == qt) return (long)SYS_ERR_AGAIN;
        *r->event = q[qh]; qh = (qh + 1) % QMAX;
        if (r->event->type == SYSCALL_WINDOW_EVENT_RESIZED) {
            fw = r->event->width; fh = r->event->height; free(pix); pix = calloc((size_t)fw * (size_t)fh, 4);
        }
        return 0;
    default: return 0;
    }
}

void host_log(const char* s) { fputs(s, stderr); fflush(stderr); }
const char* host_getenv(const char* name) { return getenv(name); }
void host_exit(int code) { exit(code); }
void host_write_file(const char* path, const char* data, int n) {
    char hp[600]; FILE* f;
    snprintf(hp, sizeof hp, "%s/%s", root, path);
    f = fopen(hp, "wb"); if (f) { fwrite(data, 1, (size_t)n, f); fclose(f); }
}
int host_read_file(const char* path, char* out, int cap) {
    char hp[600]; FILE* f; int n;
    snprintf(hp, sizeof hp, "%s/%s", root, path);
    f = fopen(hp, "rb"); if (!f) return -1;
    n = (int)fread(out, 1, (size_t)cap, f); fclose(f); return n;
}

long host_syscall(long n, long a, long b, long c) {
    char hp[1024];
    switch (n) {
    case SYS_UPTIME: return host_ms;
    case SYS_SLEEP:
        host_ms += 16; frame_no++;
        if (tick_cb) tick_cb(frame_no);
        return 0;
    case SYS_HEAP:
        if (a == SYS_HEAP_ALLOC) return (long)malloc((size_t)c);
        if (a == SYS_HEAP_ALLOC_ZEROED) return (long)calloc(1, (size_t)c);
        if (a == SYS_HEAP_RESIZE) return (long)realloc((void*)b, (size_t)c);
        if (a == SYS_HEAP_FREE) { free((void*)b); return 0; }
        return -1;
    case SYS_WRITE: return (long)write((int)a == 3 ? 2 : (int)a, (const void*)b, (size_t)c);
    case SYS_OPEN: {
        int fd = open(hostpath((const char*)a, hp, sizeof hp), b == SYS_O_RDONLY ? O_RDONLY : O_RDWR);
        return fd < 0 ? -1 : fd;
    }
    case SYS_READ: return (long)read((int)a, (void*)b, (size_t)c);
    case SYS_CLOSE: close((int)a); return 0;
    case SYS_SIZE: { struct stat st; return fstat((int)a, &st) == 0 ? (long)st.st_size : -1; }
    case SYS_SEEK: return lseek((int)a, (off_t)b, SEEK_SET) < 0 ? -1 : 0;
    case SYS_FWRITE: return (long)write((int)a, (const void*)b, (size_t)c);
    case SYS_CREATE: { int fd = creat(hostpath((const char*)a, hp, sizeof hp), 0644); if (fd >= 0) close(fd); return fd < 0 ? -1 : 0; }
    case SYS_DELETE: return unlink(hostpath((const char*)a, hp, sizeof hp)) == 0 ? 0 : -1;
    case SYS_EXISTS: { struct stat st; return stat(hostpath((const char*)a, hp, sizeof hp), &st) == 0 ? 1 : 0; }
    case SYS_IS_DIR: { struct stat st; return stat(hostpath((const char*)a, hp, sizeof hp), &st) == 0 && S_ISDIR(st.st_mode) ? 1 : 0; }
    case SYS_GETCWD: strncpy((char*)a, "0:/", (size_t)b); return 0;
    case SYS_USB:
        if (a == SYS_USB_KEY_TO_ASCII) return usb_ascii((int)b, (c & 0x22) != 0);
        return 0;
    case SYS_LISTDIR: {
        DIR* d = opendir(hostpath((const char*)a, hp, sizeof hp)); struct dirent* de; long cnt = 0;
        syscall_dirent_t* e = (syscall_dirent_t*)b;
        if (!d) return -1;
        while ((de = readdir(d)) && cnt < c) {
            char full[1536]; struct stat st;
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..") || strlen(de->d_name) >= SYSCALL_DIRENT_NAME_SIZE) continue;
            memset(&e[cnt], 0, sizeof e[cnt]);
            strcpy(e[cnt].name, de->d_name);
            snprintf(full, sizeof full, "%s/%s", hp, de->d_name);
            e[cnt].type = (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) ? SYSCALL_DIRENT_TYPE_DIR : SYSCALL_DIRENT_TYPE_FILE;
            e[cnt].hidden = de->d_name[0] == '.';
            cnt++;
        }
        closedir(d);
        return cnt;
    }
    case SYS_WINDOW: return window_call((syscall_window_request_t*)a);
    default: return -1;
    }
}
long host_syscall4(long n, long a, long b, long c, long d) { (void)d; return host_syscall(n, a, b, c); }
