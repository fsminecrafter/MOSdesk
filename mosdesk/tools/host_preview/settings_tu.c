/* Compiles the real MOSdesk Settings client and exposes its drawing + clicks. */
#define main settings_main
#include "mosdesk/main.c"
#undef main
uint32_t* settings_draw(uint32_t w, uint32_t h, uint32_t cat, uint32_t accent) {
    preferences = mosdesk_settings_load();
    preferences.accent_color = accent;
    load_fonts();
    category = cat;
    resize_surface(w, h);
    draw_window();
    return surface.pixels;
}
uint32_t* settings_click(int32_t x, int32_t y, uint32_t w, uint32_t h) {
    syscall_window_event_t e = {.type = SYSCALL_WINDOW_EVENT_MOUSE, .x = x, .y = y, .buttons = SYSCALL_MOUSE_BTN_LEFT};
    handle_mouse(&e);
    resize_surface(w, h);
    draw_window();
    return surface.pixels;
}
/* Any window event for the Settings client (hover, click, wheel). Returns the
 * redrawn surface, or NULL when the event changed nothing visible. */
uint32_t* settings_event(const syscall_window_event_t* e, uint32_t w, uint32_t h) {
    bool changed = false;
    if (e->type == SYSCALL_WINDOW_EVENT_MOUSE) {
        changed = mouse_changed_window(e);
    } else if (e->type == SYSCALL_WINDOW_EVENT_MOUSE_WHEEL) {
        int32_t before_page = appearance_scroll, before_nav = nav_scroll;
        handle_wheel(e);
        changed = before_page != appearance_scroll || before_nav != nav_scroll;
    }
    if (!changed) return NULL;
    resize_surface(w, h);
    draw_window();
    return surface.pixels;
}
