/* Scripted MOSdesk session on the REAL mosWM compositor and the REAL Settings
 * client (host build, Minimal-OS syscalls stubbed by sdk_impl.c).
 *
 * The story, in order:
 *   1. First run      username -> "Set password" -> desktop (default look)
 *   2. Start menu     System rows plus the six built-in apps
 *   3. Files          Home, Documents, Pictures; a photo opened as wallpaper
 *   4. Desktop menu   right-click -> Change wallpaper -> Settings
 *   5. Settings       theme, photo wallpapers + fit, cursor pack, Debug page
 *   6. Windows        drag, maximize, restore, close
 *   7. Restart        sign-in screen (wrong then right password); look persisted
 *
 * Every frame is rendered by mosWM itself; the only post-processing is a short
 * fade to black around the "restart" cut.  Stills for each scene are written
 * next to the video.
 */
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
extern const char* harness_exec_arg;
extern uint32_t harness_add_window(const syscall_window_info_t*, uint32_t*);
extern void harness_remove_window(uint32_t);
extern void a_init(uint32_t);
extern void a_cursor(int32_t*, int32_t*); extern void a_set_cursor(int32_t, int32_t);
extern void a_input(const syscall_input_event_t*); extern uint32_t* a_render(void);
extern void a_restart(uint32_t);
extern void a_start_row(uint32_t, int32_t*, int32_t*); extern uint32_t a_start_rows(void);
extern void a_menu_item(uint32_t, int32_t*, int32_t*); extern int a_menu_open(void);
extern uint32_t a_icon_count(void); extern void a_icon_center(uint32_t, int32_t*, int32_t*);
extern uint32_t a_browser_id(void); extern uint32_t a_browser_count(void);
extern const char* a_browser_name(uint32_t); extern const char* a_browser_path(void);
extern void a_browser_row(uint32_t, int32_t*, int32_t*); extern void a_browser_tool(uint32_t, int32_t*, int32_t*);
extern uint32_t* settings_draw(uint32_t, uint32_t, uint32_t, uint32_t);
extern uint32_t* settings_click(int32_t, int32_t, uint32_t, uint32_t);
extern uint32_t* settings_event(const syscall_window_event_t*, uint32_t, uint32_t);

#define W 1280
#define H 720
#define FPS 30
#define SETTINGS_ID 1u
#define SETTINGS_W 720u
#define SETTINGS_H 480u
#define NAV_WIDTH 148
#define AUTH_FIRST_CHOICE 1u
#define AUTH_LOGIN 3u
#define AUTH_USERNAME 4u
#define KEY_ENTER 40u

static FILE* vid; static const char* outdir; static int nframes;
static uint32_t* sfb;                 /* backing store for the Settings window surface */
static const uint32_t ACCENT = 0xFF3DAEE9u;
static int fade = 256;                /* 256 = normal, 0 = black */
static uint32_t buttons_down;

/* ------------------------------------------------------------- output */
static void save_ppm(const char* name, const uint32_t* px) {
    char p[512]; snprintf(p, sizeof p, "%s/%s.ppm", outdir, name);
    FILE* o = fopen(p, "wb"); fprintf(o, "P6 %d %d 255\n", W, H);
    for (int i = 0; i < W * H; i++) { fputc((px[i] >> 16) & 255, o); fputc((px[i] >> 8) & 255, o); fputc(px[i] & 255, o); }
    fclose(o);
}
static void snap(const char* name) { save_ppm(name, a_render()); fprintf(stderr, "%6.1fs  %s\n", nframes / (double)FPS, name); }
static void frame(void) {
    static uint32_t* tmp;
    harness_uptime_ms = (uint64_t)nframes * 1000u / FPS;
    uint32_t* fb = a_render();
    if (vid) {
        if (fade < 256) {
            if (!tmp) tmp = malloc((size_t)W * H * 4);
            for (int i = 0; i < W * H; i++) {
                uint32_t p = fb[i];
                tmp[i] = 0xFF000000u | ((((p >> 16) & 255) * (uint32_t)fade >> 8) << 16) |
                         ((((p >> 8) & 255) * (uint32_t)fade >> 8) << 8) | (((p & 255) * (uint32_t)fade) >> 8);
            }
            fb = tmp;
        }
        fwrite(fb, 4, (size_t)W * H, vid);
    }
    nframes++;
}
static void hold(int n) { while (n-- > 0) frame(); }
static void fade_to(int level, int frames) {
    int start = fade;
    for (int f = 1; f <= frames; f++) { fade = start + (level - start) * f / frames; frame(); }
}

/* ------------------------------------------------- fake window server hooks */
static syscall_window_info_t* wininfo(uint32_t id) {
    for (int i = 0; i < harness_nwin; i++) if (harness_windows[i].id == id) return &harness_windows[i];
    return NULL;
}
static void push_frame(uint32_t* surf) {
    syscall_window_info_t* w = wininfo(SETTINGS_ID); if (!w || !surf) return;
    memcpy(sfb, surf, (size_t)w->width * w->height * 4);
    w->frame_version++;
}
static void on_exec(const char* path) {
    if (!strstr(path, "mosdesk.run") || wininfo(SETTINGS_ID)) return;
    syscall_window_style_t st = { .title_bar = 0xFF263945u, .title_text = 0xFFF4F7F8u, .close_button = 0xFF263945u,
        .minimize_button = 0xFF263945u, .maximize_button = 0xFF263945u, .close_icon = 0xFFF4F7F8u,
        .minimize_icon = 0xFFF4F7F8u, .maximize_icon = 0xFFF4F7F8u, .border = 0xFF536773u, .content_background = 0xFFF3F5F6u };
    syscall_window_info_t w = { .id = SETTINGS_ID, .frame_version = 1, .x = 250, .y = 64, .width = SETTINGS_W, .height = SETTINGS_H,
        .style = st, .flags = SYSCALL_WINDOW_VISIBLE | SYSCALL_WINDOW_RESIZABLE };
    strcpy(w.title, "MOSdesk Settings");
    harness_add_window(&w, sfb);
    push_frame(settings_draw(SETTINGS_W, SETTINGS_H, 0, ACCENT));   /* "wallpaper" and no page both land on Appearance */
}
static void on_event(uint32_t id, const syscall_window_event_t* e) {
    if (id != SETTINGS_ID) return;
    syscall_window_info_t* w = wininfo(SETTINGS_ID); if (!w) return;
    if (e->type == SYSCALL_WINDOW_EVENT_CLOSE_REQUEST) { harness_remove_window(SETTINGS_ID); return; }
    push_frame(settings_event(e, w->width, w->height));
}
static void on_bounds(uint32_t id, uint32_t w, uint32_t h) {
    if (id != SETTINGS_ID) return;
    push_frame(settings_click(-1000, -1000, w, h));
}

/* ----------------------------------------------------------- scripted input */
static double ease(double t) { return t < 0.5 ? 2 * t * t : 1 - pow(-2 * t + 2, 2) / 2; }
static void send(syscall_input_event_t e) { e.time_ms = (uint32_t)harness_uptime_ms; a_input(&e); }
static void step_to(int tx, int ty, int frames) {       /* moves with whatever buttons are held */
    int32_t sx, sy; a_cursor(&sx, &sy);
    for (int f = 1; f <= frames; f++) {
        double t = ease((double)f / frames);
        int nx = (int)lround(sx + (tx - sx) * t), ny = (int)lround(sy + (ty - sy) * t);
        int32_t cx, cy; a_cursor(&cx, &cy);
        send((syscall_input_event_t){ .type = SYSCALL_INPUT_MOUSE_MOVE, .dx = nx - cx, .dy = ny - cy, .buttons = buttons_down });
        frame();
    }
}
static void move_to(int tx, int ty, int frames) { step_to(tx, ty, frames); }
static void button(uint32_t mask) { buttons_down = mask; send((syscall_input_event_t){ .type = SYSCALL_INPUT_MOUSE_BUTTON, .buttons = mask }); }
static void click(void) { button(1); hold(3); button(0); hold(2); }
static void right_click(void) { button(2); hold(3); button(0); hold(3); }
static void double_click(void) { click(); hold(4); click(); }
static void wheel(int notches_down, int gap) {
    for (int i = 0; i < notches_down; i++) {
        send((syscall_input_event_t){ .type = SYSCALL_INPUT_MOUSE_WHEEL, .wheel = -1 }); hold(gap);
    }
}
static void key(uint32_t usb, uint32_t mods) {
    send((syscall_input_event_t){ .type = SYSCALL_INPUT_KEY_DOWN, .key = usb, .mods = mods }); frame();
    send((syscall_input_event_t){ .type = SYSCALL_INPUT_KEY_UP, .key = usb, .mods = mods }); hold(4);
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

/* ------------------------------------------------------------ geometry helpers */
static void win_pt(uint32_t id, int cx, int cy, int* x, int* y) {      /* content coords -> screen */
    syscall_window_info_t* w = wininfo(id); *x = w->x + 1 + cx; *y = w->y + 28 + 1 + cy;
}
static void move_win(uint32_t id, int cx, int cy, int frames) { int x, y; win_pt(id, cx, cy, &x, &y); move_to(x, y, frames); }
static void click_win(uint32_t id, int cx, int cy, int frames) { move_win(id, cx, cy, frames); hold(3); click(); }
static void title_button(uint32_t id, int from_right, int frames) {    /* 12 = close, 36 = maximize, 60 = minimize */
    syscall_window_info_t* w = wininfo(id); move_to(w->x + (int)w->width + 1 - from_right, w->y + 14, frames); hold(5); click();
}
static void start_menu_row(uint32_t row, int frames) { int x, y; a_start_row(row, &x, &y); move_to(x, y, frames); }
static void open_start_menu(void) { move_to(24, H - 24, 26); hold(6); click(); hold(12); }
static int browser_index(const char* name) {
    for (uint32_t i = 0; i < a_browser_count(); i++) if (!strcmp(a_browser_name(i), name)) return (int)i;
    fprintf(stderr, "demo: '%s' not found in %s\n", name, a_browser_path()); exit(2);
}
static void browser_row_click(const char* name, int dbl) {
    int row = browser_index(name); int cx, cy; a_browser_row((uint32_t)row, &cx, &cy);
    move_win(a_browser_id(), cx, cy, 16); hold(5);
    if (dbl) double_click(); else click();
}
static void settings_nav(int row) { click_win(SETTINGS_ID, 70, 52 + row * 44 + 20, 18); hold(14); }

/* Settings > Appearance geometry for a SETTINGS_W-wide window (see mosdesk/main.c). */
#define LEFT (NAV_WIDTH + 22)
#define TILE_X(col) (LEFT + (col) * 108 + 48)
static int appearance_scroll_now;
static void appearance_wheel(int notches) { wheel(notches, 5); appearance_scroll_now += notches * 44; hold(8); }
static void click_theme(int index) {                 /* Minimal-OS wallpapers: 5 per row */
    int col = index % 5, row = index / 5;
    click_win(SETTINGS_ID, TILE_X(col), 198 + row * 86 + 30 - appearance_scroll_now, 20);
}
static void click_photo(int index) {                 /* photo wallpapers start at page y 404 */
    int col = index % 5, row = index / 5;
    click_win(SETTINGS_ID, TILE_X(col), 404 + row * 86 + 30 - appearance_scroll_now, 20);
}

/* ------------------------------------------------------------------- main */
int main(int argc, char** argv) {
    outdir = argc > 1 ? argv[1] : "."; const char* mp4 = argc > 2 ? argv[2] : NULL;
    harness_w = W; harness_h = H; sfb = calloc((size_t)W * H, 4);
    hook_exec = on_exec; hook_event = on_event; hook_bounds = on_bounds;
    if (mp4) {
        char cmd[1024];
        snprintf(cmd, sizeof cmd, "ffmpeg -loglevel error -y -f rawvideo -pix_fmt bgra -s %dx%d -r %d -i - "
                 "-c:v libx264 -preset slow -crf 22 -pix_fmt yuv420p -movflags +faststart %s", W, H, FPS, mp4);
        vid = popen(cmd, "w");
    }

    /* ============ 1. first run: no settings.dat yet, exactly like a fresh install ============ */
    a_init(AUTH_USERNAME);
    a_set_cursor(W / 2 + 120, H / 2 + 60);
    hold(18);
    snap("01_first_run_username");
    type_str("demo"); hold(14);
    key(KEY_ENTER, 0); hold(22);
    snap("02_first_run_choice");
    move_to(W / 2 - 120, H / 2 + 100, 24); hold(10); click(); hold(18);       /* Set password */
    type_str("mosdesk1"); hold(14);
    snap("03_first_run_password");
    key(KEY_ENTER, 0); hold(30);                                              /* enters the desktop */
    snap("04_desktop_default");

    /* ============ 2. Start menu with the built-in apps ============ */
    move_to(640, 330, 26); hold(14);
    open_start_menu();
    snap("05_start_menu");
    for (uint32_t row = 3; row < a_start_rows(); row++) { start_menu_row(row, row == 3 ? 20 : 8); hold(3); }
    hold(8);
    start_menu_row(0, 18); hold(12);                                           /* Files */
    snap("06_start_menu_files");
    click(); hold(30);

    /* ============ 3. Files: Home -> Documents -> Pictures -> photo as wallpaper ============ */
    snap("07_files_home");
    browser_row_click("Documents", 1); hold(26);
    snap("08_files_documents");
    { int x, y; a_browser_tool(0, &x, &y); click_win(a_browser_id(), x, y, 18); hold(20); }   /* Up */
    browser_row_click("Pictures", 1); hold(26);
    snap("09_files_pictures");
    browser_row_click("aurora.qoi", 1); hold(26);                              /* opens as wallpaper */
    snap("10_files_wallpaper_set");
    hold(12);
    title_button(a_browser_id(), 12, 22); hold(22);                            /* close Files */

    /* ============ 4. desktop menu -> Change wallpaper ============ */
    move_to(930, 250, 26); hold(8);
    right_click(); hold(12);
    snap("11_desktop_menu");
    { int x, y; a_menu_item(1, &x, &y); move_to(x, y, 16); hold(14); click(); hold(30); }
    snap("12_settings_appearance");

    /* ============ 5. Settings ============ */
    click_theme(2); hold(26);                                                  /* a Minimal-OS theme */
    snap("13_theme_changed");
    appearance_wheel(4);
    snap("14_settings_photos");
    click_photo(5); hold(26);
    snap("15_photo_wallpaper");
    click_photo(7); hold(26);
    /* photo fit buttons: Fill / Crop / Stretch / Rotate, at page y 610; end on Crop so it fills the screen */
    click_win(SETTINGS_ID, LEFT + 2 * 84 + 38, 610 + 14 - appearance_scroll_now, 18); hold(26);
    click_win(SETTINGS_ID, LEFT + 84 + 38, 610 + 14 - appearance_scroll_now, 14); hold(30);
    snap("16_photo_fit");
    settings_nav(2); hold(10);                                                 /* Cursor */
    snap("17_settings_cursor");
    click_win(SETTINGS_ID, 219, 117, 18); hold(26);                            /* yellow pointer fill */
    move_win(SETTINGS_ID, 400, 190, 16); hold(12);
    snap("17b_cursor_yellow");
    click_win(SETTINGS_ID, 183, 117, 18); hold(24);                            /* and back to the white fill */
    settings_nav(5); hold(10);                                                 /* Debug */
    snap("18_settings_debug");
    move_win(SETTINGS_ID, 360, 200, 22); hold(30);                             /* look around, change nothing */

    /* ============ 6. window management ============ */
    { syscall_window_info_t* w = wininfo(SETTINGS_ID);
      int x = w->x + 300, y = w->y + 12;
      move_to(x, y, 18); hold(4);
      button(1); hold(3); step_to(x - 150, y + 70, 30); hold(2); button(0); hold(18); }
    title_button(SETTINGS_ID, 36, 20); hold(22);                               /* maximize */
    snap("20_maximized"); hold(8);
    title_button(SETTINGS_ID, 36, 18); hold(18);                               /* restore */
    title_button(SETTINGS_ID, 12, 18); hold(18);                               /* close */
    move_to(980, 420, 22); hold(14);

    /* ============ 7. restart: sign-in screen, settings persisted ============ */
    fade_to(0, 12); hold(4);
    a_restart(AUTH_LOGIN);
    fade_to(256, 12);
    hold(14);
    snap("21_signin");
    type_str("wrong1"); hold(8);
    key(KEY_ENTER, 0); hold(28);
    snap("22_signin_error");
    type_str("mosdesk1"); hold(10);
    key(KEY_ENTER, 0); hold(20);
    move_to(700, 300, 24); hold(30);
    snap("23_desktop_final");
    hold(14);
    fade_to(0, 16);

    if (vid) pclose(vid);
    printf("frames: %d (%.1fs)\n", nframes, nframes / (double)FPS);
    return 0;
}
