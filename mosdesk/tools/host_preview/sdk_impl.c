/* Host implementation of the Minimal-OS syscalls: files via open(), heap via
 * malloc, plus a tiny window server so the real mosWM can be driven. */
#define _GNU_SOURCE
#include "syscall.h"
#include "graphics.h"
#include "window.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
uint32_t harness_cursor_status; uint64_t harness_uptime_ms;
uint32_t* harness_fb; uint32_t harness_w = 1280, harness_h = 720;
syscall_window_info_t harness_windows[8]; int harness_nwin;
uint32_t* harness_frames[8];
struct { uint16_t year; uint8_t month, day, hour, minute, second; } harness_time = {2026, 10, 12, 13, 37, 0};
/* hooks supplied by the driver */
void (*hook_exec)(const char* path);
void (*hook_event)(uint32_t id, const syscall_window_event_t* e);
void (*hook_bounds)(uint32_t id, uint32_t w, uint32_t h);

static char hid_to_ascii(uint32_t key, uint32_t mods) {
    int shift = (mods & (1u << 1)) != 0;
    if (key >= 4 && key <= 29) return (char)((shift ? 'A' : 'a') + (key - 4));
    if (key >= 30 && key <= 38) return (char)('1' + (key - 30));
    if (key == 39) return '0';
    if (key == 44) return ' ';
    return 0;
}
uint64_t mos_syscall3(uint64_t n, uint64_t a, uint64_t b, uint64_t c) {
    switch (n) {
    case SYS_HEAP: if (a == 1) return (uint64_t)(uintptr_t)calloc(1, c); if (a == 2) { free((void*)(uintptr_t)b); return 0; } return (uint64_t)-1;
    case SYS_OPEN: { int fd = open((const char*)(uintptr_t)a, b == 1 ? O_RDWR : O_RDONLY); return fd < 0 ? (uint64_t)-1 : (uint64_t)fd; }
    case SYS_FWRITE: return (uint64_t)write((int)a, (const void*)(uintptr_t)b, (size_t)c);
    case 34u /* SYS_CREATE */: { int fd = creat((const char*)(uintptr_t)a, 0644); if (fd >= 0) close(fd); return fd < 0 ? (uint64_t)-1 : 0; }
    case 20u /* SYS_DELETE */: return unlink((const char*)(uintptr_t)a) == 0 ? 0 : (uint64_t)-1;
    case SYS_SIZE: { struct stat st; return fstat((int)a, &st) == 0 ? (uint64_t)st.st_size : (uint64_t)-1; }
    case SYS_READ: return (uint64_t)read((int)a, (void*)(uintptr_t)b, (size_t)c);
    case SYS_CLOSE: close((int)a); return 0;
    case SYS_SEEK: return (uint64_t)lseek((int)a, (off_t)b, SEEK_SET) < 0 ? (uint64_t)-1 : 0;
    case SYS_USB: return (uint64_t)(unsigned char)hid_to_ascii((uint32_t)b, (uint32_t)c);
    case SYS_RANDOM: { uint8_t* p = (uint8_t*)(uintptr_t)a; for (uint64_t i = 0; i < b; i++) p[i] = (uint8_t)(i * 37 + 11); return b; }
    case SYS_LISTDIR: {
        const char* p = (const char*)(uintptr_t)a; syscall_dirent_t* e = (syscall_dirent_t*)(uintptr_t)b;
        memset(e, 0, sizeof(*e) * 4);
        if (!strcmp(p, "0:/programs")) {
            strcpy(e[0].name, "mosdesk"); e[0].type = SYSCALL_DIRENT_TYPE_DIR;
            strcpy(e[1].name, "mosWM");   e[1].type = SYSCALL_DIRENT_TYPE_DIR;
            return 2;
        }
        if (!strcmp(p, "0:/programs/mosdesk")) { strcpy(e[0].name, "mosdesk.run"); e[0].type = SYSCALL_DIRENT_TYPE_EXECUTABLE; return 1; }
        if (!strcmp(p, "0:/programs/mosWM"))   { strcpy(e[0].name, "mosWM.run");   e[0].type = SYSCALL_DIRENT_TYPE_EXECUTABLE; return 1; }
        return (uint64_t)-1;
    }
    case SYS_EXEC: if (hook_exec) hook_exec((const char*)(uintptr_t)a); return 1;
    default: return (uint64_t)-1;
    }
}
uint64_t mos_syscall(uint64_t n, uint64_t a, uint64_t b, uint64_t c) { return mos_syscall3(n, a, b, c); }
uint64_t mos_syscall1(uint64_t n, uint64_t a) {
    if (n == SYS_GETTIME) { memcpy((void*)(uintptr_t)a, &harness_time, sizeof harness_time); return 0; }
    if (n == SYS_UPTIME) return harness_uptime_ms;
    return (uint64_t)-1;
}
uint64_t mos_graphics_acquire(void) { return 0; }
uint64_t mos_graphics_release(void) { return 0; }
uint64_t mos_graphics_info(syscall_graphics_info_t* i) { i->width = harness_w; i->height = harness_h; i->pixel_format = 1; i->pitch = harness_w * 4; return 0; }
uint64_t mos_graphics_blit_surface(const uint32_t* px, uint32_t w, uint32_t h, uint32_t pitch, int32_t sx, int32_t sy, int32_t sw, int32_t sh, int32_t dx, int32_t dy, uint32_t flags) {
    (void)w; (void)h; (void)flags;
    /* The compositor blits the whole scene, then small pointer patches. */
    if (!harness_fb) harness_fb = calloc((size_t)harness_w * harness_h, 4);
    for (int32_t row = 0; row < sh; row++) {
        if (dy + row < 0 || (uint32_t)(dy + row) >= harness_h) continue;
        for (int32_t col = 0; col < sw; col++) {
            if (dx + col < 0 || (uint32_t)(dx + col) >= harness_w) continue;
            harness_fb[(size_t)(dy + row) * harness_w + (uint32_t)(dx + col)] =
                *(const uint32_t*)((const uint8_t*)px + (size_t)(sy + row) * pitch + (size_t)(sx + col) * 4u);
        }
    }
    return 0;
}
uint64_t mos_graphics_present(int32_t x, int32_t y, int32_t w, int32_t h) { (void)x; (void)y; (void)w; (void)h; return 0; }
uint64_t mos_window_register_manager(void) { return 0; }
uint64_t mos_window_list(syscall_window_info_t* w, uint64_t max) { (void)max; memcpy(w, harness_windows, sizeof(harness_windows[0]) * harness_nwin); return (uint64_t)harness_nwin; }
uint64_t mos_window_get_frame(uint32_t id, syscall_window_frame_t* f) {
    for (int i = 0; i < harness_nwin; i++) if (harness_windows[i].id == id) {
        memcpy(f->pixels, harness_frames[i], (size_t)f->width * f->height * 4); return 0; }
    return (uint64_t)-1;
}
uint64_t mos_window_get_icon(uint32_t id, uint32_t* p) { (void)id; (void)p; return SYS_ERR_NOTFOUND; }
uint64_t mos_window_set_flags(uint32_t id, uint32_t f) {
    for (int i = 0; i < harness_nwin; i++) if (harness_windows[i].id == id) {
        if (f & SYSCALL_WINDOW_FOCUSED) for (int j = 0; j < harness_nwin; j++) harness_windows[j].flags &= ~SYSCALL_WINDOW_FOCUSED;
        harness_windows[i].flags = f; harness_windows[i].frame_version++; return 0; }
    return (uint64_t)-1;
}
uint64_t mos_window_set_bounds(uint32_t id, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    for (int i = 0; i < harness_nwin; i++) if (harness_windows[i].id == id) {
        int resized = harness_windows[i].width != w || harness_windows[i].height != h;
        harness_windows[i].x = (int32_t)x; harness_windows[i].y = (int32_t)y;
        harness_windows[i].width = w; harness_windows[i].height = h;
        if (resized && hook_bounds) hook_bounds(id, w, h);
        harness_windows[i].frame_version++; return 0; }
    return (uint64_t)-1;
}
uint64_t mos_window_post_event(uint32_t id, const syscall_window_event_t* e) { if (hook_event) hook_event(id, e); return 0; }
uint64_t mos_input_read(syscall_input_event_t* e, uint64_t max, uint64_t a, void* b) { (void)e; (void)max; (void)a; (void)b; return 0; }
void mos_register_cleanup(void (*fn)(void)) { (void)fn; }
void mos_exit() { exit(0); }
void mos_sleep(uint32_t ms) { (void)ms; }
uint64_t mos_write(const char* s, uint32_t n) { return (uint64_t)write(2, s, n); }
uint32_t mos_window_create(uint32_t a, int32_t x, int32_t y, uint32_t w, uint32_t h, const char* t) { (void)a;(void)x;(void)y;(void)w;(void)h;(void)t; return 1; }
uint64_t mos_window_close(uint32_t id) { (void)id; return 0; }
uint64_t mos_window_set_resizable(uint32_t id, bool b) { (void)id; (void)b; return 0; }
uint64_t mos_window_set_style(uint32_t id, syscall_window_style_t s) { (void)id; (void)s; return 0; }
uint64_t mos_window_next_event(uint32_t id, syscall_window_event_t* e) { (void)id; (void)e; return 1; }
uint64_t mos_window_get_info(uint32_t id, syscall_window_info_t* i) {
    for (int k = 0; k < harness_nwin; k++) if (harness_windows[k].id == id) { *i = harness_windows[k]; return 0; }
    return (uint64_t)-1;
}
uint64_t mos_window_get_cursor_state(syscall_cursor_state_t* state) { if (state) { memset(state, 0, sizeof(*state)); state->status = harness_cursor_status; } return 0; }
uint64_t mos_window_present(uint32_t id, const uint32_t* px, uint32_t pitch, int32_t x, int32_t y, int32_t w, int32_t h) { (void)id;(void)px;(void)pitch;(void)x;(void)y;(void)w;(void)h; return 0; }
