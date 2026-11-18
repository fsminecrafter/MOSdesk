#pragma once
#include "syscall.h"
#define SYSCALL_WINDOW_MAX_TITLE 48
#define SYSCALL_WINDOW_MAX_WINDOWS 16
#define SYSCALL_WINDOW_VISIBLE 1u
#define SYSCALL_WINDOW_FOCUSED 2u
#define SYSCALL_WINDOW_MINIMIZED 4u
#define SYSCALL_WINDOW_MAXIMIZED 8u
#define SYSCALL_WINDOW_FULLSCREEN 16u
#define SYSCALL_WINDOW_RESIZABLE 32u
#define SYSCALL_WINDOW_EVENT_CLOSE_REQUEST 1u
#define SYSCALL_WINDOW_EVENT_KEY 2u
#define SYSCALL_WINDOW_EVENT_MOUSE 3u
#define SYSCALL_WINDOW_EVENT_MOUSE_WHEEL 4u
#define SYSCALL_WINDOW_EVENT_MAXIMIZED 5u
#define SYSCALL_WINDOW_EVENT_MINIMIZED 6u
#define SYSCALL_WINDOW_EVENT_RESTORED 7u
#define SYSCALL_WINDOW_EVENT_RESIZED 8u
#define SYSCALL_CURSOR_ARROW 0u
#define SYSCALL_CURSOR_BUSY 1u
#define SYSCALL_CURSOR_WORKING 2u
#define SYSCALL_CURSOR_TEXT 3u
#define SYSCALL_CURSOR_HAND 4u
#define SYSCALL_CURSOR_HELP 5u
#define SYSCALL_CURSOR_FORBIDDEN 6u
#define SYSCALL_CURSOR_MOVE 7u
#define SYSCALL_CURSOR_RESIZE_EW 8u
#define SYSCALL_CURSOR_RESIZE_NESW 9u
#define SYSCALL_CURSOR_RESIZE_NS 10u
#define SYSCALL_CURSOR_RESIZE_NWSE 11u
#define SYSCALL_CURSOR_PRECISE 12u
#define SYSCALL_CURSOR_PEN 13u
#define SYSCALL_CURSOR_COUNT 14u
#define SYSCALL_CURSOR_PACK_PATH_SIZE 128u
#define SYSCALL_MOUSE_BTN_LEFT 1u
#define SYSCALL_INPUT_BTN_LEFT 1u
#define SYSCALL_INPUT_KEY_DOWN 1u
#define SYSCALL_INPUT_KEY_UP 2u
#define SYSCALL_INPUT_MOUSE_BUTTON 3u
#define SYSCALL_INPUT_MOUSE_MOVE 4u
#define SYSCALL_INPUT_MOUSE_WHEEL 5u
#define SYSCALL_INPUT_MOD_SHIFT (1u << 0)
#define SYSCALL_INPUT_MOD_CTRL (1u << 1)
#define SYSCALL_INPUT_MOD_ALT (1u << 2)
typedef struct { uint32_t title_bar, title_text, close_button, minimize_button, maximize_button,
    close_icon, minimize_icon, maximize_icon, border, content_background; } syscall_window_style_t;
typedef struct { uint32_t id, frame_version, flags; int32_t x, y; uint32_t width, height;
    char title[SYSCALL_WINDOW_MAX_TITLE]; syscall_window_style_t style; } syscall_window_info_t;
typedef struct { uint32_t status; char pack[SYSCALL_CURSOR_PACK_PATH_SIZE]; } syscall_cursor_state_t;
typedef struct { uint32_t width, height, pitch; uint32_t* pixels; } syscall_window_frame_t;
typedef struct { uint32_t type; int32_t x, y, width, height; uint32_t buttons, key, mods, time_ms; int32_t wheel; uint8_t reserved[3]; } syscall_window_event_t;
typedef struct { uint32_t type, buttons, key, mods, time_ms; int32_t dx, dy, wheel; } syscall_input_event_t;
#define SYSCALL_WINDOW_SHARED_BUFFERS 2u
typedef struct { uint64_t address, buffer_stride; uint32_t width, height, pitch, buffer_count,
    generation, front, frame_version, damage_x, damage_y, damage_width, damage_height; } syscall_window_shared_t;
/* The host window server has no shared frames: mosWM takes the copy path. */
static inline uint64_t mos_window_take_damage(uint32_t id, syscall_window_shared_t* s) { (void)id; (void)s; return (uint64_t)-1; }
static inline uint64_t mos_window_map_frame(uint32_t id, syscall_window_shared_t* s) { (void)id; (void)s; return (uint64_t)-1; }
static inline long mos_vm_unmap(void* a, size_t n) { (void)a; (void)n; return 0; }
uint64_t mos_window_register_manager(void);
uint64_t mos_window_list(syscall_window_info_t* w, uint64_t max);
uint64_t mos_window_get_frame(uint32_t id, syscall_window_frame_t* f);
uint64_t mos_window_set_flags(uint32_t id, uint32_t flags);
uint64_t mos_window_set_bounds(uint32_t id, uint32_t x, uint32_t y, uint32_t w, uint32_t h);
uint64_t mos_window_post_event(uint32_t id, const syscall_window_event_t* e);
uint64_t mos_input_read(syscall_input_event_t* e, uint64_t max, uint64_t a, void* b);
void mos_register_cleanup(void (*fn)(void));
void mos_exit();
void mos_sleep(uint32_t ms);
uint64_t mos_write(const char* s, uint32_t n);
uint32_t mos_window_create(uint32_t a, int32_t x, int32_t y, uint32_t w, uint32_t h, const char* title);
uint64_t mos_window_close(uint32_t id);
uint64_t mos_window_set_resizable(uint32_t id, bool b);
uint64_t mos_window_set_style(uint32_t id, syscall_window_style_t s);
uint64_t mos_window_next_event(uint32_t id, syscall_window_event_t* e);
typedef syscall_window_info_t syscall_window_info_dummy_t;
uint64_t mos_window_get_info(uint32_t id, syscall_window_info_t* i);
uint64_t mos_window_get_cursor_state(syscall_cursor_state_t* state);
uint64_t mos_window_present(uint32_t id, const uint32_t* px, uint32_t pitch, int32_t x, int32_t y, int32_t w, int32_t h);
#define SYSCALL_WINDOW_ICON_SIZE 64u
#define SYSCALL_WINDOW_ICON_PIXELS (SYSCALL_WINDOW_ICON_SIZE * SYSCALL_WINDOW_ICON_SIZE)
uint64_t mos_window_get_icon(uint32_t id, uint32_t* pixels);
