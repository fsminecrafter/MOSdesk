/* Renders the real mosWM Files window on the host: a folder with sorting,
 * the places sidebar, a selection and the type-to-filter state.
 *   ./files_shot.sh [outdir] */
#define main wm_main
#include "mosWM/main.c"
#undef main
#include <stdio.h>
extern uint32_t* harness_fb;
extern uint32_t harness_w, harness_h;
extern syscall_window_info_t harness_windows[8];
extern int harness_nwin;
extern uint64_t harness_uptime_ms;

static void save(const char* path) {
    FILE* o = fopen(path, "wb");
    fprintf(o, "P6 %u %u 255\n", harness_w, harness_h);
    for (uint32_t i = 0; i < harness_w * harness_h; i++) {
        uint32_t p = harness_fb[i];
        fputc((p >> 16) & 255, o); fputc((p >> 8) & 255, o); fputc(p & 255, o);
    }
    fclose(o);
}

static void frame(const char* out) {
    syscall_window_info_t windows[MAX_WINDOWS];
    for (int i = 0; i < harness_nwin; i++)
        if (harness_windows[i].id == browser_window_id)
            harness_windows[i].flags |= SYSCALL_WINDOW_FOCUSED;
    uint64_t count = mos_window_list(windows, MAX_WINDOWS);
    focused_id = browser_window_id;
    browser_drawn_signature = 0;
    browser_render();
    count = mos_window_list(windows, MAX_WINDOWS);
    for (uint64_t i = 0; i < count; i++) {
        cached_window_t* e = cache_for(windows[i].id);
        if (e) { e->announced = true; refresh_frame(e, &windows[i]); }
    }
    /* Let the opening animation finish. */
    for (int step = 0; step < 40; step++) {
        harness_uptime_ms += 50;
        update_hover(windows, count);
        render(windows, count);
    }
    save(out);
}

int main(int argc, char** argv) {
    (void)argc;
    mos_graphics_info(&display);
    canvas = wm_malloc((size_t)display.width * display.height * 4);
    wallpaper = wm_malloc((size_t)display.width * display.height * 4);
    paint_wallpaper(wallpaper);
    load_desktop_settings();
    load_fonts();
    strcpy(user_home, "0:/users/demo");
    strcpy(user_desktop, "0:/users/demo/Desktop");
    cursor_x = 1200; cursor_y = 700;
    browser_open("0:/users/demo/Projects/farcade");
    browser_open("0:/users/demo/Projects/farcade/src");   /* history */
    browser_history_go(false);                             /* back: forward on */
    browser_key(81u, 0); browser_key(81u, 0); browser_key(81u, 0);
    frame(argv[1]);
    /* Sorted by size, then a filter typed. */
    browser_set_sort(BROWSER_SORT_SIZE);
    browser_key(22u, 0);   /* s */
    browser_key(6u, 0);    /* c */
    frame(argv[2]);
    return 0;
}
