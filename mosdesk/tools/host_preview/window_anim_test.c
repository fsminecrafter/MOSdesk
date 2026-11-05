/* Renders the window animations on the REAL mosWM compositor (host build,
 * syscalls stubbed by sdk_impl.c) as filmstrips: one row per effect, a
 * frame every few hundredths of the animation, for each window style.
 * Built and run by window_anim.sh.
 *   window_anim_test <outdir> [video.rgba]
 * With a second argument it also writes every frame of a short scripted
 * session (raw RGBA, 1024x640, 60 fps) for turning into a video. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

#define FRAMES 7
#define SCALE 2

static uint32_t* content;
static FILE* video;

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
    update_hover(windows, count);
    render(windows, count);
    if (video) fwrite(harness_fb, 4, (size_t)harness_w * harness_h, video);
}

/* A window with something to look at: a gradient, a grid and some bars. */
static uint32_t* paint_content(uint32_t width, uint32_t height) {
    uint32_t* pixels = malloc((size_t)width * height * 4u);
    for (uint32_t y = 0; y < height; y++)
        for (uint32_t x = 0; x < width; x++) {
            uint32_t r = 40 + x * 160 / width, g = 70 + y * 120 / height;
            uint32_t color = 0xFF000000u | (r << 16) | (g << 8) | 200u;
            if (x % 40 == 0 || y % 40 == 0) color = 0xFFF0F4F8u;
            if (y > 30 && y < 50 && x > 20 && x < 20 + (y * 7 % 200))
                color = 0xFF1B2A36u;
            pixels[(size_t)y * width + x] = color;
        }
    return pixels;
}

static void open_window(uint32_t id) {
    syscall_window_style_t style = {
        .title_bar = 0xFF263945u, .title_text = 0xFFF4F7F8u,
        .close_button = 0xFF263945u, .minimize_button = 0xFF263945u,
        .maximize_button = 0xFF263945u, .close_icon = 0xFFF4F7F8u,
        .minimize_icon = 0xFFF4F7F8u, .maximize_icon = 0xFFF4F7F8u,
        .border = 0xFF536773u, .content_background = 0xFFF3F5F6u};
    syscall_window_info_t info = {
        .id = id, .flags = SYSCALL_WINDOW_VISIBLE | SYSCALL_WINDOW_FOCUSED,
        .x = 300, .y = 120, .width = 420u, .height = 300u, .style = style,
        .frame_version = 1};
    strcpy(info.title, "Animation test");
    for (int i = 0; i < harness_nwin; i++)
        harness_windows[i].flags &= ~SYSCALL_WINDOW_FOCUSED;
    harness_add_window(&info, content);
}

static void step(uint32_t ms) {
    harness_uptime_ms += ms;
    frame();
}

/* Copies the part of the screen around the window, halved, into the sheet. */
static void grab(uint32_t* sheet, uint32_t sheet_width, int column, int row,
                 int crop_x, int crop_y, int crop_w, int crop_h) {
    int tile_w = crop_w / SCALE, tile_h = crop_h / SCALE;
    for (int y = 0; y < tile_h; y++)
        for (int x = 0; x < tile_w; x++) {
            uint32_t r = 0, g = 0, b = 0;
            for (int sy = 0; sy < SCALE; sy++)
                for (int sx = 0; sx < SCALE; sx++) {
                    int px = crop_x + x * SCALE + sx, py = crop_y + y * SCALE + sy;
                    uint32_t p = 0;
                    if (px >= 0 && py >= 0 && px < (int)harness_w && py < (int)harness_h)
                        p = harness_fb[(size_t)py * harness_w + (size_t)px];
                    r += (p >> 16) & 255; g += (p >> 8) & 255; b += p & 255;
                }
            uint32_t n = SCALE * SCALE;
            sheet[(size_t)(row * tile_h + y) * sheet_width + (size_t)(column * tile_w + x)] =
                0xFF000000u | (r / n) << 16 | (g / n) << 8 | (b / n);
        }
}

static void save_ppm(const char* path, const uint32_t* pixels, uint32_t w,
                     uint32_t h) {
    FILE* out = fopen(path, "wb");
    fprintf(out, "P6 %u %u 255\n", w, h);
    for (size_t i = 0; i < (size_t)w * h; i++) {
        fputc((pixels[i] >> 16) & 255, out);
        fputc((pixels[i] >> 8) & 255, out);
        fputc(pixels[i] & 255, out);
    }
    fclose(out);
}

/* One row: the animation sampled at FRAMES points. `close` animates a
 * window away, otherwise one in. */
static void film(uint32_t* sheet, uint32_t sheet_width, int row, bool close,
                 uint32_t effect, uint32_t kind_minimize) {
    static uint32_t next_id = 20;
    int crop_x = 180, crop_y = 60, crop_w = 660, crop_h = 580;
    uint32_t id = next_id++;
    if (close || kind_minimize) {
        desktop_settings.anim_open = MOSDESK_ANIM_NONE;
        open_window(id);
        step(16);
        step(16);
    }
    desktop_settings.anim_open = effect;
    desktop_settings.anim_close = effect;
    uint32_t duration;
    if (kind_minimize) {
        cached_window_t* entry = cache_for(id);
        set_window_mode(&harness_windows[harness_nwin - 1], entry,
                        MODE_MINIMIZED);
        duration = anim_duration(260u);
    } else if (close) {
        harness_remove_window(id);
        duration = window_close_duration(effect);
    } else {
        open_window(id);
        duration = window_open_duration(effect);
    }
    uint64_t start = harness_uptime_ms;
    for (int f = 0; f < FRAMES; f++) {
        uint64_t at = start + (uint64_t)duration * (uint64_t)(f + 1) / (FRAMES + 1);
        harness_uptime_ms = at - 1;
        step(1);
        grab(sheet, sheet_width, f, row, crop_x, crop_y, crop_w, crop_h);
    }
    harness_uptime_ms = start + duration + 50;
    frame();
    frame();
    if (!close && !kind_minimize) {
        desktop_settings.anim_close = MOSDESK_ANIM_NONE;
        harness_remove_window(id);
    } else if (kind_minimize) {
        desktop_settings.anim_close = MOSDESK_ANIM_NONE;
        harness_remove_window(id);
    }
    step(16);
    step(16);
}

static void sheet_for_style(const char* outdir, uint32_t style,
                            const char* name) {
    ui_style = style;
    desktop_settings.ui_style = style;
    scene_valid = false;
    int tile_w = 660 / SCALE, tile_h = 580 / SCALE, rows = 7;
    uint32_t sheet_width = (uint32_t)(tile_w * FRAMES);
    uint32_t* sheet = calloc((size_t)sheet_width * (size_t)(tile_h * rows), 4);
    film(sheet, sheet_width, 0, false, MOSDESK_ANIM_OPEN_FADE, 0);
    film(sheet, sheet_width, 1, false, MOSDESK_ANIM_OPEN_BLOCKS, 0);
    film(sheet, sheet_width, 2, false, MOSDESK_ANIM_OPEN_MATRIX, 0);
    film(sheet, sheet_width, 3, true, MOSDESK_ANIM_CLOSE_FADE, 0);
    film(sheet, sheet_width, 4, true, MOSDESK_ANIM_CLOSE_KATANA, 0);
    film(sheet, sheet_width, 5, true, MOSDESK_ANIM_CLOSE_MATRIX, 0);
    film(sheet, sheet_width, 6, false, MOSDESK_ANIM_OPEN_FADE, 1);
    char path[512];
    snprintf(path, sizeof path, "%s/window_anims_%s.ppm", outdir, name);
    save_ppm(path, sheet, sheet_width, (uint32_t)(tile_h * rows));
    free(sheet);
}

/* A short session at 60 fps: each effect in turn, plus the start menu. */
static void session(void) {
    static const uint32_t opens[3] = {MOSDESK_ANIM_OPEN_FADE,
        MOSDESK_ANIM_OPEN_BLOCKS, MOSDESK_ANIM_OPEN_MATRIX};
    static const uint32_t closes[3] = {MOSDESK_ANIM_CLOSE_FADE,
        MOSDESK_ANIM_CLOSE_KATANA, MOSDESK_ANIM_CLOSE_MATRIX};
    for (int i = 0; i < 3; i++) {
        desktop_settings.anim_open = opens[i];
        desktop_settings.anim_close = closes[i];
        open_window(40u + (uint32_t)i);
        for (int f = 0; f < 70; f++) step(16);
        harness_remove_window(40u + (uint32_t)i);
        for (int f = 0; f < 60; f++) step(16);
    }
    start_menu_open = true;
    for (int f = 0; f < 30; f++) step(16);
    start_menu_open = false;
    for (int f = 0; f < 10; f++) step(16);
}

int main(int argc, char** argv) {
    const char* outdir = argc > 1 ? argv[1] : ".";
    harness_w = 1024;
    harness_h = 640;
    mos_graphics_info(&display);
    size_t bytes = (size_t)display.width * display.height * 4u;
    canvas = wm_malloc(bytes);
    wallpaper = wm_malloc(bytes);
    desktop_layer = wm_malloc(bytes);
    load_desktop_settings();
    load_desktop_apps();
    painted_wallpaper_style = UINT32_MAX;
    load_fonts();
    cursor_x = 1000;
    cursor_y = 20;
    content = paint_content(420u, 300u);
    harness_uptime_ms = 1000;
    step(16);
    sheet_for_style(outdir, MOSDESK_UI_STYLE_MODERN, "modern");
    sheet_for_style(outdir, MOSDESK_UI_STYLE_AERO, "aero");
    sheet_for_style(outdir, MOSDESK_UI_STYLE_LIQUID, "liquid");
    if (argc > 2) {
        ui_style = MOSDESK_UI_STYLE_MODERN;
        scene_valid = false;
        video = fopen(argv[2], "wb");
        session();
        fclose(video);
    }
    printf("window animation sheets written to %s\n", outdir);
    return 0;
}
