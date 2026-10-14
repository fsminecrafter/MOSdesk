/* Checks mosWM's partial repaints against full ones.
 *
 * Drives the real compositor through a few hundred random changes (windows
 * moving, resizing and redrawing, hovering over the panel, menus and desktop
 * icons, the clock and the debug overlay updating, the pointer moving).
 * After every frame the screen it produced is compared with a full
 * repaint of the same state: any difference means a damage rectangle or a
 * clipped drawing routine missed something. Built and run by damage_test.sh. */
#include <stdio.h>
#include <stdlib.h>
#define main wm_main
#include "mosWM/main.c"
#undef main

extern uint32_t* harness_fb;
extern syscall_window_info_t harness_windows[8];
extern int harness_nwin;
extern uint32_t* harness_frames[8];
extern uint32_t harness_w, harness_h;

static uint32_t rng_state = 0x1234567u;
static uint32_t rng(uint32_t bound) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return bound ? rng_state % bound : rng_state;
}

static void paint_window(int index, uint32_t color) {
    syscall_window_info_t* info = &harness_windows[index];
    free(harness_frames[index]);
    harness_frames[index] = malloc((size_t)info->width * info->height * 4u);
    for (uint32_t i = 0; i < info->width * info->height; i++)
        harness_frames[index][i] = color;
    info->frame_version++;
}

static void frame(void) {
    syscall_window_info_t windows[MAX_WINDOWS];
    uint64_t count = mos_window_list(windows, MAX_WINDOWS);
    remove_missing_cache(windows, count);
    focused_id = 0;
    for (uint64_t i = 0; i < count; i++) {
        cached_window_t* entry = cache_for(windows[i].id);
        if (windows[i].flags & SYSCALL_WINDOW_FOCUSED)
            focused_id = windows[i].id;
        if (entry) refresh_frame(entry, &windows[i]);
    }
    render(windows, count);
}

int main(int argc, char** argv) {
    int steps = argc > 1 ? atoi(argv[1]) : 400;
    harness_w = 1024;
    harness_h = 768;
    mos_graphics_info(&display);
    size_t bytes = (size_t)display.width * display.height * 4u;
    canvas = wm_malloc(bytes);
    wallpaper = wm_malloc(bytes);
    /* "nolayer" as the third argument runs without the cached desktop. */
    if (argc <= 3) desktop_layer = wm_malloc(bytes);
    load_desktop_settings();
    load_desktop_apps();
    painted_wallpaper_style = UINT32_MAX;
    load_fonts();

    syscall_window_style_t style = {
        .title_bar = 0xFF263945u, .title_text = 0xFFF4F7F8u,
        .close_button = 0xFF263945u, .minimize_button = 0xFF263945u,
        .maximize_button = 0xFF263945u, .close_icon = 0xFFF4F7F8u,
        .minimize_icon = 0xFFF4F7F8u, .maximize_icon = 0xFFF4F7F8u,
        .border = 0xFF536773u, .content_background = 0xFFF3F5F6u};
    harness_nwin = 3;
    for (int i = 0; i < harness_nwin; i++) {
        syscall_window_info_t info = {
            .id = (uint32_t)i + 1u, .flags = SYSCALL_WINDOW_VISIBLE,
            .x = 80 + i * 210, .y = 60 + i * 120,
            .width = 300u + (uint32_t)i * 60u, .height = 200u, .style = style};
        snprintf(info.title, sizeof(info.title), "Window %d", i + 1);
        harness_windows[i] = info;
        paint_window(i, 0xFF203040u + (uint32_t)i * 0x00303030u);
    }
    harness_windows[1].flags |= SYSCALL_WINDOW_FOCUSED;
    cursor_x = 500;
    cursor_y = 400;

    uint32_t* partial = malloc(bytes);
    int failures = 0, partial_frames = 0;
    for (int step = 0; step < steps && failures < 5; step++) {
        int window = (int)rng((uint32_t)harness_nwin);
        syscall_window_info_t* info = &harness_windows[window];
        uint32_t action = rng(16u);
        const char* what = "idle";
        if (action < 3u) {
            what = "move";
            info->x += (int32_t)rng(121u) - 60;
            info->y += (int32_t)rng(81u) - 40;
            if (info->x < -200) info->x = -200;
            if (info->y < 0) info->y = 0;
            if (info->x > (int32_t)harness_w - 40) info->x = (int32_t)harness_w - 40;
            if (info->y > (int32_t)harness_h - 40) info->y = (int32_t)harness_h - 40;
        } else if (action < 5u) {
            what = "redraw";
            paint_window(window, 0xFF000000u | rng(0x1000000u));
        } else if (action == 5u) {
            what = "resize";
            info->width = 160u + rng(400u);
            info->height = 100u + rng(300u);
            paint_window(window, 0xFF000000u | rng(0x1000000u));
        } else if (action == 6u) {
            what = "overlay figures";
            debug_report.valid = true;
            debug_report.fps_tenths = rng(700u);
            debug_report.frame_us = rng(90000u);
            for (uint32_t i = 0; i < PROFILE_STAGES; i++) {
                debug_report.average_us[i] = rng(20000u);
                debug_report.peak_us[i] = rng(200000u);
            }
            debug_generation++;
        } else if (action == 7u) {
            what = "overlay switch";
            debug_fps = rng(2u) != 0u;
            debug_profiler = rng(2u) != 0u;
        } else if (action == 8u) {
            what = "pointer";
            cursor_x = (int32_t)rng(harness_w);
            cursor_y = (int32_t)rng(harness_h);
        } else if (action == 9u) {
            what = "icon hover";
            hover_desktop_icon = rng(desktop_app_count + 3u);
            if (hover_desktop_icon == desktop_app_count + 2u)
                hover_desktop_icon = UINT32_MAX;
        } else if (action == 10u) {
            what = "panel hover";
            hover_taskbar = rng(5u) ? rng(5u) : UINT32_MAX;
        } else if (action == 11u) {
            what = "start menu";
            if (rng(3u) == 0u) start_menu_open = !start_menu_open;
            hover_start_row = rng(6u) ? rng(6u) : UINT32_MAX;
        } else if (action == 12u) {
            what = "context menu";
            if (rng(3u) == 0u) {
                menu_open = false;
            } else {
                cursor_x = (int32_t)rng(harness_w);
                cursor_y = (int32_t)rng(harness_h);
                menu_show_for_desktop();
                menu_hover = rng(5u) ? rng(4u) : UINT32_MAX;
            }
        } else if (action == 13u) {
            what = "clock";
            clock_valid = true;
            clock_time.month = 10;
            clock_time.day = 13;
            clock_time.hour = (uint8_t)rng(24u);
            clock_time.minute = (uint8_t)rng(60u);
        } else if (rng(4u) == 0u) {
            what = "focus";
            for (int i = 0; i < harness_nwin; i++)
                harness_windows[i].flags &= ~SYSCALL_WINDOW_FOCUSED;
            info->flags |= SYSCALL_WINDOW_FOCUSED;
        }

        frame();
        bool was_partial = !damage_everything && damage_count;
        partial_frames += was_partial;
        memcpy(partial, harness_fb, bytes);
        scene_valid = false;
        frame();
        if (memcmp(partial, harness_fb, bytes)) {
            int32_t left = (int32_t)harness_w, top = (int32_t)harness_h;
            int32_t right = 0, bottom = 0;
            size_t wrong = 0;
            for (uint32_t y = 0; y < harness_h; y++)
                for (uint32_t x = 0; x < harness_w; x++)
                    if (partial[y * harness_w + x] !=
                        harness_fb[y * harness_w + x]) {
                        wrong++;
                        if ((int32_t)x < left) left = (int32_t)x;
                        if ((int32_t)y < top) top = (int32_t)y;
                        if ((int32_t)x >= right) right = (int32_t)x + 1;
                        if ((int32_t)y >= bottom) bottom = (int32_t)y + 1;
                    }
            printf("step %d (%s, %s repaint): %zu pixels differ in "
                   "(%d,%d)-(%d,%d)\n", step, what,
                   was_partial ? "partial" : "full", wrong, left, top, right,
                   bottom);
            failures++;
        }
    }
    if (failures) return 1;
    if (argc > 2) {
        /* Final screen, for comparing two builds of the compositor. */
        FILE* dump = fopen(argv[2], "wb");
        if (dump) {
            fwrite(harness_fb, 1, bytes, dump);
            fclose(dump);
        }
    }
    printf("damage test: %d steps, %d partial repaints, all match a full "
           "repaint\n", steps, partial_frames);
    return 0;
}
