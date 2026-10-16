#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "window.h"
#include "mosdesk/settings.h"
#include "mosdesk/password_hash.h"

extern uint32_t harness_w, harness_h; extern uint64_t harness_uptime_ms;
extern syscall_window_info_t harness_windows[8]; extern int harness_nwin; extern uint32_t* harness_frames[8];
extern void (*hook_exec)(const char*); extern void (*hook_event)(uint32_t, const syscall_window_event_t*);
extern void (*hook_bounds)(uint32_t, uint32_t, uint32_t);
extern void a_init(uint32_t); extern void a_set_auth(uint32_t); extern uint32_t a_get_auth(void);
extern void a_cursor(int32_t*, int32_t*); extern void a_set_cursor(int32_t, int32_t);
extern void a_input(const syscall_input_event_t*); extern uint32_t* a_render(void);
extern const char* a_auth_message(void);
extern uint32_t* settings_draw(uint32_t, uint32_t, uint32_t, uint32_t);
extern uint32_t* settings_click(int32_t, int32_t, uint32_t, uint32_t);

#define W 1280
#define H 720
static FILE* vid; static const char* outdir; static int nframes;
static uint32_t* sfb;           /* backing store for the Settings window surface */
static const uint32_t ACCENT = 0xFF3DAEE9u;

static void save_ppm(const char* name, const uint32_t* px) {
    char p[512]; snprintf(p, sizeof p, "%s/%s.ppm", outdir, name);
    FILE* o = fopen(p, "wb"); fprintf(o, "P6 %d %d 255\n", W, H);
    for (int i = 0; i < W * H; i++) { fputc((px[i] >> 16) & 255, o); fputc((px[i] >> 8) & 255, o); fputc(px[i] & 255, o); }
    fclose(o);
}
static void snap(const char* name) { save_ppm(name, a_render()); }
static void frame(void) { harness_uptime_ms += 33; uint32_t* fb = a_render(); if (vid) fwrite(fb, 4, W * H, vid); nframes++; }
static void hold(int n) { while (n-- > 0) frame(); }

/* ---- fake window server hooks: one Settings window ---- */
static void push_frame(uint32_t* surf) {
    syscall_window_info_t* w = &harness_windows[0];
    memcpy(sfb, surf, (size_t)w->width * w->height * 4);
    w->frame_version++;
}
static void on_exec(const char* path) {
    if (!strstr(path, "mosdesk.run")) return;
    syscall_window_style_t st = { .title_bar = 0xFF263945u, .title_text = 0xFFF4F7F8u, .close_button = 0xFF263945u,
        .minimize_button = 0xFF263945u, .maximize_button = 0xFF263945u, .close_icon = 0xFFF4F7F8u,
        .minimize_icon = 0xFFF4F7F8u, .maximize_icon = 0xFFF4F7F8u, .border = 0xFF536773u, .content_background = 0xFFF3F5F6u };
    syscall_window_info_t w = { .id = 1, .frame_version = 1, .x = 300, .y = 130, .width = 520, .height = 300, .style = st,
        .flags = SYSCALL_WINDOW_VISIBLE | SYSCALL_WINDOW_FOCUSED | SYSCALL_WINDOW_RESIZABLE };
    strcpy(w.title, "MOSdesk Settings");
    harness_windows[0] = w; harness_nwin = 1; harness_frames[0] = sfb;
    push_frame(settings_draw(520, 300, 0, ACCENT));
}
static void on_event(uint32_t id, const syscall_window_event_t* e) {
    if (id != 1 || !harness_nwin) return;
    syscall_window_info_t* w = &harness_windows[0];
    if (e->type == SYSCALL_WINDOW_EVENT_MOUSE) { if (e->buttons & 1) push_frame(settings_click(e->x, e->y, w->width, w->height)); }
    else if (e->type == SYSCALL_WINDOW_EVENT_CLOSE_REQUEST) harness_nwin = 0;
}
static void on_bounds(uint32_t id, uint32_t w, uint32_t h) {
    (void)id; harness_windows[0].width = w; harness_windows[0].height = h;
    push_frame(settings_click(-1000, -1000, w, h));
}

/* ---- scripted input ---- */
static double ease(double t) { return t < 0.5 ? 2 * t * t : 1 - pow(-2 * t + 2, 2) / 2; }
static void move_to(int tx, int ty, int frames) {
    int32_t sx, sy; a_cursor(&sx, &sy);
    for (int f = 1; f <= frames; f++) {
        double t = ease((double)f / frames);
        int nx = (int)lround(sx + (tx - sx) * t), ny = (int)lround(sy + (ty - sy) * t);
        int32_t cx, cy; a_cursor(&cx, &cy);
        syscall_input_event_t e = { .type = SYSCALL_INPUT_MOUSE_MOVE, .dx = nx - cx, .dy = ny - cy, .buttons = 0 };
        a_input(&e); frame();
    }
}
static int btn_state;
static void drag_to(int tx, int ty, int frames) {
    int32_t sx, sy; a_cursor(&sx, &sy);
    for (int f = 1; f <= frames; f++) {
        double t = ease((double)f / frames);
        int nx = (int)lround(sx + (tx - sx) * t), ny = (int)lround(sy + (ty - sy) * t);
        int32_t cx, cy; a_cursor(&cx, &cy);
        syscall_input_event_t e = { .type = SYSCALL_INPUT_MOUSE_MOVE, .dx = nx - cx, .dy = ny - cy, .buttons = 1 };
        a_input(&e); frame();
    }
}
static void button(int down) { syscall_input_event_t e = { .type = SYSCALL_INPUT_MOUSE_BUTTON, .buttons = down ? 1u : 0u }; a_input(&e); btn_state = down; }
static void click(void) { button(1); hold(3); button(0); hold(2); }
static void key(uint32_t usb, uint32_t mods) {
    syscall_input_event_t d = { .type = SYSCALL_INPUT_KEY_DOWN, .key = usb, .mods = mods }; a_input(&d); frame();
    syscall_input_event_t u = { .type = SYSCALL_INPUT_KEY_UP, .key = usb, .mods = mods }; a_input(&u); hold(4);
}
static void type_str(const char* s) {
    for (; *s; s++) {
        char c = *s; uint32_t usb = 0, mods = 0;
        if (c >= 'a' && c <= 'z') usb = 4 + (c - 'a');
        else if (c >= 'A' && c <= 'Z') { usb = 4 + (c - 'A'); mods = 1; }
        else if (c >= '1' && c <= '9') usb = 30 + (c - '1');
        else if (c == '0') usb = 39;
        key(usb, mods);
    }
}
static void write_settings(const char* password) {
    mosdesk_settings_t s = mosdesk_settings_default();
    s.onboarding_complete = 1; s.login_enabled = 1;
    strcpy(s.username, "demo");
    for (int i = 0; i < 16; i++) s.password_salt[i] = (uint8_t)(0xA5 ^ (i * 29));
    mosdesk_password_hash(s.password_salt, password, s.password_hash);
    FILE* f = fopen("0:/programs/mosdesk/settings.dat", "wb"); fwrite(&s, sizeof s, 1, f); fclose(f);
}

int main(int argc, char** argv) {
    outdir = argc > 1 ? argv[1] : "."; const char* mp4 = argc > 2 ? argv[2] : NULL;
    harness_w = W; harness_h = H; sfb = calloc((size_t)W * H, 4);
    hook_exec = on_exec; hook_event = on_event; hook_bounds = on_bounds;
    write_settings("mosdesk1");
    a_init(4 /* AUTH_USERNAME */);
    a_set_cursor(W / 2, H / 2);
    snap("login_username");                        /* first run: choose a username */
    a_set_auth(1 /* AUTH_FIRST_CHOICE */);
    snap("login_first_run");                       /* welcome / choose sign-in mode */
    a_set_auth(3 /* AUTH_LOGIN */);
    if (mp4) {
        char cmd[1024]; snprintf(cmd, sizeof cmd, "ffmpeg -loglevel error -y -f rawvideo -pix_fmt bgra -s %dx%d -r 30 -i - -c:v libx264 -preset medium -crf 18 -pix_fmt yuv420p %s", W, H, mp4);
        vid = popen(cmd, "w");
    }
    /* ---------- login ---------- */
    move_to(W / 2 + 40, H / 2 + 120, 1); hold(1);
    snap("login_empty"); hold(30);
    type_str("wrong1"); hold(6); snap("login_typed");
    key(40, 0); hold(4); snap("login_error"); hold(36);
    type_str("mosdesk1"); hold(10);
    key(40, 0);
    hold(20);
    /* ---------- desktop ---------- */
    move_to(760, 300, 1); hold(10); snap("desktop"); hold(20);
    move_to(24, 696, 28); hold(8); click(); hold(14); snap("desktop_start_menu");
    move_to(150, 498, 22); hold(10); click(); hold(22);
    int wx = harness_windows[0].x, wy = harness_windows[0].y;
    int cx0 = wx + 1 + 70, cy0 = wy + 28 + 1;
    move_to(cx0, cy0 + 52 + 44 + 20, 22); hold(4); click(); hold(16);       /* Mouse */
    move_to(cx0, cy0 + 52 + 88 + 20, 12); hold(2); click(); hold(16);       /* Cursor */
    move_to(cx0, cy0 + 52 + 132 + 20, 12); hold(2); click(); hold(16);      /* Login */
    move_to(cx0, cy0 + 52 + 20, 12); hold(2); click(); hold(18);            /* Appearance */
    snap("desktop_settings");
    /* theme change: Teal accent + Teal wallpaper, written to settings.dat and picked up live by mosWM */
    move_to(cx0 + 165, cy0 + 110, 20); hold(4); click(); hold(34);
    move_to(cx0 + 165, cy0 + 226, 14); hold(2); click(); hold(40);
    snap("desktop_teal");
    move_to(wx + 260, wy + 12, 18); hold(4);                                 /* title bar */
    button(1); hold(3); drag_to(wx + 260 + 330, wy + 12 + 90, 30); hold(2); button(0); hold(18);
    wx = harness_windows[0].x; wy = harness_windows[0].y;
    move_to(wx + 485, wy + 14, 22); hold(6); click(); hold(26);              /* maximize */
    snap("desktop_settings_maximized"); hold(10);
    move_to((int)harness_windows[0].width + 1 - 36, 14, 22); hold(6);      /* maximized window sits at 0,0 */
    click(); hold(24);                                                       /* restore */
    wx = harness_windows[0].x; wy = harness_windows[0].y;
    move_to(wx + 1 + 148 + 22 + 15, wy + 29 + 110, 20); hold(4); click(); hold(30);   /* accent back to Blue */
    move_to(wx + 1 + 148 + 22 + 15, wy + 29 + 226, 12); hold(2); click(); hold(36);   /* wallpaper back to Blue */
    move_to(wx + 509, wy + 14, 20); hold(6); click(); hold(24);              /* close */
    move_to(980, 420, 30); hold(40);
    if (vid) pclose(vid);
    printf("frames: %d (%.1fs)\n", nframes, nframes / 30.0);
    return 0;
}
