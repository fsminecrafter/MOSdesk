/* Real mosWM compositor, with hooks to drive input and render frames. */
#define main wm_main
#include "mosWM/main.c"
#undef main
extern uint32_t* harness_fb;
void a_init(uint32_t auth) {
    mos_graphics_info(&display);
    canvas = wm_malloc((size_t)display.width * display.height * 4);
    wallpaper = wm_malloc((size_t)display.width * display.height * 4);
    cursor_x = (int32_t)display.width / 2; cursor_y = (int32_t)display.height / 2;
    load_desktop_settings();
    load_desktop_apps();
    auth_state = auth;
    painted_wallpaper_style = UINT32_MAX;
    load_fonts();
}
void a_set_auth(uint32_t s) { auth_state = s; }
uint32_t a_get_auth(void) { return auth_state; }
void a_set_menu(int open) { start_menu_open = open != 0; }
int a_menu(void) { return start_menu_open; }
const char* a_auth_message(void) { return auth_message; }
void a_cursor(int32_t* x, int32_t* y) { *x = cursor_x; *y = cursor_y; }
void a_set_cursor(int32_t x, int32_t y) { cursor_x = x; cursor_y = y; }
void a_input(const syscall_input_event_t* in) {
    syscall_window_info_t windows[MAX_WINDOWS];
    uint64_t count = mos_window_list(windows, MAX_WINDOWS);
    route_input(in, windows, count);
}
/* One trip round the real mosWM main loop (minus sleeping and the debug
 * stopwatch), so Files, hover highlights and new-window focus all behave.
 * The real loop polls every 30 iterations at ~60 fps (about 0.5 s); the video
 * runs at 30 fps, so the same wall-clock cadence is every 15 frames. */
uint32_t* a_render(void) {
    static unsigned poll, icon_poll;
    if (++icon_poll >= 15u) { icon_poll = 0; if (desktop_icon_drag_index == UINT32_MAX) reload_desktop_files(); }
    if (++poll >= 15u && desktop_icon_drag_index == UINT32_MAX) { poll = 0; load_desktop_settings(); }
    browser_render();
    syscall_window_info_t windows[MAX_WINDOWS];
    uint64_t count = mos_window_list(windows, MAX_WINDOWS);
    remove_missing_cache(windows, count);
    sync_window_modes(windows, count);
    focused_id = 0;
    for (uint64_t i = 0; i < count; i++) {
        cached_window_t* e = cache_for(windows[i].id);
        if (windows[i].flags & SYSCALL_WINDOW_FOCUSED) focused_id = windows[i].id;
        if (e && !e->announced) {                 /* a new window takes the keyboard focus */
            e->announced = true;
            if (windows[i].flags & SYSCALL_WINDOW_VISIBLE) focus_window(&windows[i]);
        }
        refresh_window_icon(e, false);
        if (e) refresh_frame(e, &windows[i]);
    }
    update_cursor_appearance(windows, count);
    update_hover(windows, count);
    update_clock();
    render(windows, count);
    return harness_fb;
}

/* ---- geometry taken from the real compositor, so scripted clicks never drift ---- */
void a_start_row(uint32_t row, int32_t* x, int32_t* y) {
    int32_t bx, by, bw, bh; start_menu_bounds(&bx, &by, &bw, &bh);
    *x = bx + 70; *y = start_menu_row_y(row) + 18;
}
uint32_t a_start_rows(void) { return 3u + start_menu_rows(NULL); }
void a_menu_item(uint32_t index, int32_t* x, int32_t* y) {
    int32_t py = menu_y + 6;
    for (uint32_t i = 0; i < menu_item_count; i++) {
        if (menu_items[i].separator_before) py += 9;
        if (i == index) { *x = menu_x + 60; *y = py + MENU_ITEM_HEIGHT / 2; return; }
        py += MENU_ITEM_HEIGHT;
    }
    *x = menu_x; *y = menu_y;
}
int a_menu_open(void) { return menu_open; }
uint32_t a_icon_count(void) { return desktop_icon_count(); }
void a_icon_center(uint32_t i, int32_t* x, int32_t* y) { int32_t px, py; desktop_icon_position(i, &px, &py); *x = px + 32; *y = py + 32; }
uint32_t a_browser_id(void) { return browser_window_id; }
uint32_t a_browser_count(void) { return browser_entry_count; }
const char* a_browser_name(uint32_t i) { return browser_entries[i].name; }
const char* a_browser_path(void) { return browser_path; }
/* Content-relative centre of Files row `index` / toolbar button `tool`. */
void a_browser_row(uint32_t index, int32_t* x, int32_t* y) { *x = 120; *y = (int32_t)BROWSER_HEADER_HEIGHT + 4 + (int32_t)(index - browser_scroll) * (int32_t)BROWSER_ROW_HEIGHT + (int32_t)BROWSER_ROW_HEIGHT / 2; }
void a_browser_tool(uint32_t tool, int32_t* x, int32_t* y) { *x = browser_tool_x(tool) + 16; *y = 25; }
/* A fresh start of mosWM on the same disk, e.g. after a restart. */
void a_restart(uint32_t auth) {
    if (browser_window_id) browser_close();
    menu_open = false; start_menu_open = false; focused_id = 0;
    load_desktop_settings();
    reload_desktop_apps();
    update_user_paths();
    auth_state = auth; auth_password_length = 0; auth_message[0] = '\0';
    memset(auth_password, 0, sizeof auth_password);
    painted_wallpaper_style = UINT32_MAX;
    cursor_x = (int32_t)display.width / 2; cursor_y = (int32_t)display.height / 2;
}
