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
extern uint64_t harness_uptime_ms;
extern uint32_t harness_add_window(const syscall_window_info_t*, uint32_t*);
extern void harness_remove_window(uint32_t);
static uint32_t next_id = 10u;

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
    if (getenv("HOVER")) update_hover(windows, count);
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
    const char* recent[6] = {"", "", "", "", "", ""};
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
            /* Sometimes see-through, as easyui's Liquid look is. */
            paint_window(window, (rng(3u) || getenv("OPAQUE") ? 0xFF000000u : 0x60000000u) |
                                 rng(0x1000000u));
        } else if (action == 5u &&
                   !(info->flags & SYSCALL_WINDOW_MINIMIZED)) {
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
            /* Window styles too: Liquid Glass puts glass behind
             * see-through windows. */
            if (rng(3u) == 0u) ui_style = getenv("STYLE") ? (uint32_t)atoi(getenv("STYLE")) : rng(3u);
        } else if (action == 8u) {
            what = "pointer";
            cursor_x = (int32_t)rng(harness_w);
            cursor_y = rng(2u) ? (int32_t)(harness_h - 1u - rng(48u))
                               : (int32_t)rng(harness_h);
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
        } else if (action == 14u && rng(2u)) {
            /* Windows opening and closing, with every animation. */
            desktop_settings.anim_open = rng(4u);
            desktop_settings.anim_close = rng(4u);
            if (harness_nwin == 3 || (harness_nwin > 1 && rng(2u))) {
                what = "close";
                harness_remove_window(harness_windows[window % harness_nwin].id);
            } else {
                what = "open";
                syscall_window_info_t added = harness_windows[0];
                added.id = next_id++;
                added.x = (int32_t)rng(harness_w - 200u);
                added.y = (int32_t)rng(harness_h - 200u);
                added.flags = SYSCALL_WINDOW_VISIBLE;
                harness_add_window(&added, NULL);
                paint_window(harness_nwin - 1, 0xFF000000u | rng(0x1000000u));
            }
        } else if (action == 15u && rng(2u)) {
            what = "minimize/restore";
            desktop_settings.anim_interface = rng(4u) != 0u;
            cached_window_t* entry = cache_for(info->id);
            if (entry) {
                set_window_mode(info, entry, entry->last_mode == MODE_MINIMIZED
                                                 ? MODE_NORMAL
                                                 : MODE_MINIMIZED);
                /* A restore can put back the old size: the program redraws. */
                paint_window(window, 0xFF000000u | rng(0x1000000u));
            }
        } else if (rng(4u) == 0u) {
            what = "focus";
            for (int i = 0; i < harness_nwin; i++)
                harness_windows[i].flags &= ~SYSCALL_WINDOW_FOCUSED;
            info->flags |= SYSCALL_WINDOW_FOCUSED;
        }

        if (getenv("NOANIM")) {
            desktop_settings.anim_open = desktop_settings.anim_close = 0u;
            desktop_settings.anim_interface = 0u;
        }
        recent[step % 6] = what;
        harness_uptime_ms += 16u + rng(40u);
        frame();
        bool was_partial = !damage_everything && damage_count;
        partial_frames += was_partial;
        memcpy(partial, harness_fb, bytes);
        scene_valid = false;
        frame();
        /* TOLERANCE=n ignores differences of up to n per colour channel:
         * Liquid Glass blurs what is on screen inside the repainted area,
         * so its glass can come out a shade different near a repaint's
         * edge. */
        int tolerance = getenv("TOLERANCE") ? atoi(getenv("TOLERANCE")) : 0;
        bool differs = false;
        for (size_t i = 0; i < bytes / 4u && !differs; i++) {
            uint32_t a = partial[i], b = harness_fb[i];
            if (a == b) continue;
            for (int shift = 0; shift < 24; shift += 8) {
                int d = (int)((a >> shift) & 255u) - (int)((b >> shift) & 255u);
                if (d > tolerance || d < -tolerance) differs = true;
            }
        }
        if (differs) {
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
            if (getenv("PIXELS")) {
                int shown = 0;
                for (uint32_t y = 0; y < harness_h && shown < 12; y++)
                    for (uint32_t x = 0; x < harness_w && shown < 12; x++)
                        if (partial[y * harness_w + x] != harness_fb[y * harness_w + x]) {
                            printf("  (%u,%u) partial %08x full %08x\n", x, y,
                                   partial[y * harness_w + x], harness_fb[y * harness_w + x]);
                            shown++;
                        }
                for (int i = 0; i < harness_nwin; i++)
                    printf("  win %u at %d,%d %ux%u focused %d\n", harness_windows[i].id,
                           harness_windows[i].x, harness_windows[i].y, harness_windows[i].width,
                           harness_windows[i].height, (harness_windows[i].flags & SYSCALL_WINDOW_FOCUSED) != 0);
            }
            printf("step %d (%s, %s repaint): %zu pixels differ in "
                   "(%d,%d)-(%d,%d)\n", step, what,
                   was_partial ? "partial" : "full", wrong, left, top, right,
                   bottom);
            failures++;
            if (getenv("ANIMDEBUG") && failures == 1) {
                printf("  last steps:");
                for (int k = 0; k < 6; k++) printf(" %s", recent[(step + 1 + k) % 6]);
                printf("\n");
                for (int i = 0; i < harness_nwin; i++) {
                    cached_window_t* e = cache_for(harness_windows[i].id);
                    printf("  win %u at %d,%d %ux%u flags %x anim %u eff %u start %lu dur %u now %lu\n",
                        harness_windows[i].id, harness_windows[i].x, harness_windows[i].y,
                        harness_windows[i].width, harness_windows[i].height, harness_windows[i].flags,
                        e->anim_kind, e->anim_effect, (unsigned long)e->anim_start, e->anim_duration, (unsigned long)anim_now);
                }
                for (uint32_t g = 0; g < MAX_GHOSTS; g++) if (ghosts[g].active)
                    printf("  ghost kind %u eff %u at %d,%d %ux%u\n", ghosts[g].kind, ghosts[g].effect,
                        ghosts[g].window.last_info.x, ghosts[g].window.last_info.y,
                        ghosts[g].window.last_info.width, ghosts[g].window.last_info.height);
                printf("  menu_open %d at %d,%d hover %u start %d\n", menu_open, menu_x, menu_y, menu_hover, start_menu_open);
                for (uint32_t h = 0; h < HOVER_SLOTS; h++) if (hover_slots[h].key)
                    printf("  slot %llx level %u target %d area %d,%d-%d,%d\n", (unsigned long long)hover_slots[h].key, hover_slots[h].level, hover_slots[h].target,
                        hover_slots[h].area.left, hover_slots[h].area.top, hover_slots[h].area.right, hover_slots[h].area.bottom);
                for (uint32_t d = 0; d < damage_count; d++) printf("  damage %d,%d-%d,%d\n", damage[d].left, damage[d].top, damage[d].right, damage[d].bottom);
            }
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
