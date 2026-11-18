/* code_host.h - interface between the host window server (code_host.c, built
 * against glibc) and the harness that includes the Code app (code_shot.c,
 * built against the Minimal-OS SDK headers). Plain C types only. */
#ifndef CODE_HOST_H
#define CODE_HOST_H

extern long host_ms;                              /* fake uptime in ms, advanced 16 ms per frame */
void host_set_root(const char* dir);              /* the folder that "0:/" maps to */
void host_set_tick(void (*cb)(int frame));        /* called from the sleep inside easyui.run() */
void host_push_key(int key, int mods, int down);  /* key = HID usage, mods = SHIFT 1 | CTRL 2 | ALT 4 */
void host_push_mouse(int x, int y, int buttons);
void host_push_wheel(int x, int y, int dir);      /* +1 up, -1 down */
void host_push_close(void);
void host_push_resize(int w, int h);
void host_save_ppm(const char* path);
void host_get_size(int* w, int* h);
const char* host_title(void);
/* libc side services, so the app's translation unit needs no <stdio.h> */
void host_log(const char* s);
void host_write_file(const char* dir_relative_path, const char* data, int n);
int  host_read_file(const char* dir_relative_path, char* out, int cap);
const char* host_getenv(const char* name);
void host_exit(int code);

#endif
