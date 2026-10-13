#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "graphics.h"
#include "string.h"
#include "window.h"
#include "file_icons.h"
#include "wm_text.h"
#define MOSDESK_SYSCALL mos_syscall3
#include "../mosdesk/settings.h"
#include "../mosdesk/password_hash.h"
#include "../mosdesk/input.h"

#ifndef SYSCALL_WINDOW_ICON_PIXELS
#define SYSCALL_WINDOW_ICON_SIZE 32u
#define SYSCALL_WINDOW_ICON_PIXELS \
    (SYSCALL_WINDOW_ICON_SIZE * SYSCALL_WINDOW_ICON_SIZE)
static uint64_t mos_window_get_icon(uint32_t id, uint32_t* pixels) {
    (void)id;
    (void)pixels;
    return (uint64_t)-4;
}
#endif

#ifndef SYS_EXEC
#define SYS_EXEC 33u
#endif

#define SYS_HEAP_ALLOC 1u
#define SYS_HEAP_FREE 2u
#define MAX_WINDOWS SYSCALL_WINDOW_MAX_WINDOWS
#define TASKBAR_HEIGHT 48u
#define TITLE_HEIGHT 28u
#define CORNER_RADIUS 9u
#define SNAP_DISTANCE 24
#define CURSOR_MAX_SIZE 32u
#define DRAG_NONE 0u
#define DRAG_MOVE 1u
#define DRAG_RESIZE 2u
#define CURSOR_MAX_FRAMES 80u
#define CURSOR_MAX_FILE_SIZE (8u * 1024u * 1024u)
#define MAX_DESKTOP_APPS 32u
#define DESKTOP_APP_PATH_SIZE 192u
#define DESKTOP_APP_NAME_SIZE 48u
#define DESKTOP_DIR_ENTRIES 128u
#define BROWSER_MAX_ENTRIES 128u
#define BROWSER_PATH_SIZE 192u
#define BROWSER_ROW_HEIGHT 28u
#define DESKTOP_ICON_SIZE SYSCALL_WINDOW_ICON_SIZE
#define DESKTOP_ICON_PIXELS SYSCALL_WINDOW_ICON_PIXELS
#define RUN_ICON_MAX_SIZE (1024u * 1024u)
#define WALLPAPER_MAX_FILE_SIZE (20u * 1024u * 1024u)
#define WALLPAPER_MAX_PIXELS (1600u * 1600u)
#define MODE_NORMAL 0u
#define MODE_MAXIMIZED 1u
#define MODE_FULLSCREEN 2u
#define MODE_MINIMIZED 3u
#define AUTH_NONE 0u
#define AUTH_FIRST_CHOICE 1u
#define AUTH_FIRST_PASSWORD 2u
#define AUTH_LOGIN 3u
#define AUTH_PASSWORD_MAX 63u
#define BROWSER_HEADER_HEIGHT 50u
#define BROWSER_FOOTER_HEIGHT 26u
#define BROWSER_TOOL_COUNT 5u
#define CURSOR_PATCH_SIZE (CURSOR_MAX_SIZE * 2u)
#define MENU_MAX_ITEMS 8u
#define MENU_WIDTH 204
#define MENU_ITEM_HEIGHT 30
#define START_MENU_MAX_ROWS 9u
#define FILE_TREE_MAX_DEPTH 6u
#define FILE_COPY_CHUNK (64u * 1024u)

#ifndef SYSCALL_CURSOR_COUNT
/* This SDK does not expose per-window cursor state; keep the shell cursor local. */
#define SYSCALL_CURSOR_COUNT 14u
#define SYSCALL_CURSOR_ARROW 0u
#define SYSCALL_CURSOR_MOVE 7u
#define SYSCALL_CURSOR_HAND 4u
#define SYSCALL_CURSOR_RESIZE_NWSE 11u
#define SYSCALL_CURSOR_PACK_PATH_SIZE 128u
typedef struct {
    uint32_t status;
    char pack[SYSCALL_CURSOR_PACK_PATH_SIZE];
} moswm_cursor_state_t;
#else
typedef syscall_cursor_state_t moswm_cursor_state_t;
#endif

typedef struct {
    uint32_t id;
    uint32_t frame_version;
    uint32_t width;
    uint32_t height;
    uint32_t* pixels;
    uint32_t last_mode;
    uint32_t saved_x;
    uint32_t saved_y;
    uint32_t saved_width;
    uint32_t saved_height;
    bool icon_checked;
    bool has_icon;
    bool announced;
    uint32_t icon_sum;
    uint32_t icon_pixels[SYSCALL_WINDOW_ICON_PIXELS];
} cached_window_t;

typedef struct {
    char path[DESKTOP_APP_PATH_SIZE];
    char name[DESKTOP_APP_NAME_SIZE];
    bool has_icon;
    uint32_t icon_pixels[DESKTOP_ICON_PIXELS];
} desktop_app_t;

typedef enum {
    CLICK_ACTION_NONE,
    CLICK_ACTION_HOME,
    CLICK_ACTION_SYSTEM,
    CLICK_ACTION_APPLICATION,
    CLICK_ACTION_BROWSER_ENTRY
} click_action_t;

typedef enum {
    MENU_ACTION_NONE,
    MENU_ACTION_OPEN,
    MENU_ACTION_SHOW_IN_FILES,
    MENU_ACTION_SET_WALLPAPER,
    MENU_ACTION_COPY,
    MENU_ACTION_PASTE,
    MENU_ACTION_RENAME,
    MENU_ACTION_DELETE,
    MENU_ACTION_NEW_FOLDER,
    MENU_ACTION_REFRESH,
    MENU_ACTION_FILES,
    MENU_ACTION_WALLPAPERS,
    MENU_ACTION_SETTINGS
} menu_action_t;

typedef struct {
    menu_action_t action;
    const char* label;
    moswm_file_icon_t icon;
    bool separator_before;
    bool enabled;
} menu_item_t;

typedef enum {
    DIALOG_NONE,
    DIALOG_RENAME,
    DIALOG_NEW_FOLDER,
    DIALOG_DELETE
} dialog_kind_t;

typedef struct {
    bool valid;
    uint32_t width;
    uint32_t height;
    uint32_t hotspot_x;
    uint32_t hotspot_y;
    uint32_t pixels[CURSOR_MAX_SIZE * CURSOR_MAX_SIZE];
} cached_cursor_t;

static syscall_graphics_info_t display;
static uint32_t* canvas;
static cached_window_t cache[MAX_WINDOWS];
static desktop_app_t desktop_apps[MAX_DESKTOP_APPS];
static uint32_t desktop_app_count;
static syscall_dirent_t browser_entries[BROWSER_MAX_ENTRIES];
static uint32_t browser_entry_count;
static uint32_t browser_selected = UINT32_MAX;
static uint32_t browser_scroll;
static uint32_t browser_window_id;
static uint32_t* browser_pixels;
static uint32_t browser_width;
static uint32_t browser_height;
static char browser_path[BROWSER_PATH_SIZE];
static char browser_message[64];
static click_action_t pending_click_action;
static char pending_click_path[BROWSER_PATH_SIZE];
static int32_t pending_click_x;
static int32_t pending_click_y;
static click_action_t last_click_action;
static char last_click_path[BROWSER_PATH_SIZE];
static uint32_t last_click_time;
static bool last_click_valid;
static int32_t cursor_x;
static int32_t cursor_y;
static int32_t mouse_remainder_x;
static int32_t mouse_remainder_y;
static uint32_t focused_id;
static uint32_t cursor_pixels[CURSOR_MAX_SIZE * CURSOR_MAX_SIZE];
static uint32_t* cursor_animation_pixels;
static uint32_t cursor_animation_frame_count;
static uint32_t cursor_animation_frame_delay_ms;
static uint64_t cursor_animation_elapsed_ms;
static uint32_t cursor_width;
static uint32_t cursor_height;
static uint32_t cursor_hotspot_x;
static uint32_t cursor_hotspot_y;
static uint32_t cached_cursor_status = UINT32_MAX;
static char cached_cursor_pack[SYSCALL_CURSOR_PACK_PATH_SIZE];
static uint32_t cursor_fill_color = 0xFFFFFFFFu;
static uint32_t cursor_scale = 1u;
static uint32_t cursor_pack = MOSDESK_CURSOR_PACK_POSY;
static uint32_t mouse_speed = 100u;
static uint32_t accent_color = 0xFF3DAEE9u;
static uint32_t wallpaper_style;
static uint32_t wallpaper_mode;
static uint32_t wallpaper_rotation;
static uint32_t painted_wallpaper_style = UINT32_MAX;
static uint32_t auth_state;
static mosdesk_settings_t desktop_settings;
static char auth_password[AUTH_PASSWORD_MAX + 1u];
static uint32_t auth_password_length;
static char auth_message[64];
static uint32_t dragging_id;
static uint32_t drag_mode;
static uint32_t drag_start_x;
static uint32_t drag_start_y;
static uint32_t drag_start_width;
static uint32_t drag_start_height;
static uint32_t drag_start_window_x;
static uint32_t drag_start_window_y;
static uint32_t desktop_icon_drag_index = UINT32_MAX;
static int32_t desktop_icon_drag_start_x;
static int32_t desktop_icon_drag_start_y;
static int32_t desktop_icon_drag_origin_x;
static int32_t desktop_icon_drag_origin_y;
static uint32_t desktop_icon_saved_x;
static uint32_t desktop_icon_saved_y;
static bool desktop_icon_drag_moved;
static uint8_t previous_buttons;
static bool start_menu_open;
static char status_message[64];
static bool screen_owned;
static uint32_t settings_poll_frames;
static uint32_t icon_poll_frames;
static uint32_t* wallpaper_source;
static uint32_t wallpaper_source_width;
static uint32_t wallpaper_source_height;

/* Compositor caches: the desktop (wallpaper + icons) and the finished scene
 * are only repainted when something they show changes; the pointer is drawn
 * as a small patch on top, so moving the mouse costs almost nothing. */
static uint32_t* desktop_layer;
static uint64_t desktop_layer_signature;
static uint32_t wallpaper_generation;
static uint32_t desktop_apps_generation;
static uint64_t scene_signature;
static bool scene_valid;
static uint32_t cursor_patch[CURSOR_PATCH_SIZE * CURSOR_PATCH_SIZE];
static bool cursor_drawn;
static int32_t cursor_drawn_x;
static int32_t cursor_drawn_y;
static int32_t cursor_drawn_width;
static int32_t cursor_drawn_height;
static uint64_t cursor_drawn_signature;
static cached_cursor_t cursor_cache[SYSCALL_CURSOR_COUNT];
static char cursor_cache_pack[SYSCALL_CURSOR_PACK_PATH_SIZE];
static uint32_t hover_desktop_icon = UINT32_MAX;
static uint32_t hover_taskbar = UINT32_MAX;
static uint32_t hover_start_row = UINT32_MAX;

/* Debug overlay (Settings > Debug): a frame-rate meter and a per-stage
 * stopwatch for the main loop. Nothing is counted or timed while both are
 * off, and the figures on screen change once a second, so showing them costs
 * one scene repaint a second. */
typedef enum {
    PROFILE_SETTINGS,
    PROFILE_FILES,
    PROFILE_WINDOWS,
    PROFILE_INPUT,
    PROFILE_POINTER,
    PROFILE_DRAW,
    PROFILE_PRESENT,
    PROFILE_SLEEP,
    PROFILE_STAGES
} profile_stage_t;

typedef struct {
    bool valid;
    uint32_t fps_tenths;
    uint32_t frame_us;
    uint32_t pointer_per_second;
    uint32_t mouse_per_second;
    uint32_t repaints_per_second;
    uint32_t average_us[PROFILE_STAGES];
    uint32_t peak_us[PROFILE_STAGES];
} debug_report_t;

static bool debug_fps;
static bool debug_profiler;
static bool debug_window_open;
static uint64_t debug_window_start_ms;
static uint64_t debug_window_start_ticks;
static uint64_t debug_lap_ticks;
static uint64_t debug_stage_ticks[PROFILE_STAGES];
static uint64_t debug_stage_peak[PROFILE_STAGES];
static uint32_t debug_frames;
static uint32_t debug_pointer_draws;
static uint32_t debug_mouse_events;
static uint32_t debug_repaints;
static debug_report_t debug_report;
static uint32_t debug_generation;

/* Right-click menu, the small dialogs it opens, and the file clipboard. */
static menu_item_t menu_items[MENU_MAX_ITEMS];
static uint32_t menu_item_count;
static bool menu_open;
static int32_t menu_x;
static int32_t menu_y;
static uint32_t menu_hover = UINT32_MAX;
static click_action_t menu_target;
static char menu_path[BROWSER_PATH_SIZE];
static dialog_kind_t dialog_kind;
static char dialog_path[BROWSER_PATH_SIZE];
static char dialog_text[SYSCALL_DIRENT_NAME_SIZE];
static uint32_t dialog_length;
static char dialog_error[56];
static char clipboard_path[BROWSER_PATH_SIZE];
static uint32_t browser_generation;
static uint32_t browser_hover = UINT32_MAX;
static uint32_t browser_tool_hover = UINT32_MAX;
static uint64_t browser_drawn_signature;
static bool browser_message_is_error;

static uint8_t* wm_read_file(const char* path, size_t max_size, size_t* size);
static void paint_wallpaper(uint32_t* target);
static void load_wallpaper_image(const char* path);
static void log_message(const char* message);
static void browser_close(void);
static void browser_open(const char* path);
static void browser_render(void);
static void browser_mouse_down(int32_t x, int32_t y,
                               const syscall_window_info_t* info);
static void activate_pending_click(uint32_t time_ms);
static void focus_window(syscall_window_info_t* info);
static cached_window_t* cache_for(uint32_t id);
static bool set_window_mode(const syscall_window_info_t* info,
                            cached_window_t* entry, uint32_t mode);
static void post_simple_event(uint32_t id, uint32_t type);
static void fill_rounded_rect(int32_t x, int32_t y, int32_t width,
                              int32_t height, uint32_t radius,
                              uint32_t color);
static void fill_rect(int32_t x, int32_t y, int32_t width, int32_t height,
                      uint32_t color);
static void reload_desktop_apps(void);
static void open_settings(const char* page);

static void copy_pixels(uint32_t* destination, const uint32_t* source,
                        size_t count) {
    __asm__ volatile("rep movsl"
                     : "+D"(destination), "+S"(source), "+c"(count)
                     : : "memory");
}

static uint64_t hash_bytes(uint64_t hash, const void* data, size_t size) {
    const uint8_t* bytes = (const uint8_t*)data;
    for (size_t i = 0; i < size; i++)
        hash = (hash ^ bytes[i]) * 0x100000001B3ull;
    return hash;
}

static uint64_t hash_u64(uint64_t hash, uint64_t value) {
    return hash_bytes(hash, &value, sizeof(value));
}

static uint64_t hash_text(uint64_t hash, const char* text) {
    return hash_bytes(hash, text, strlen(text) + 1u);
}

static void* wm_malloc(size_t size) {
    uint64_t result = mos_syscall3(SYS_HEAP, SYS_HEAP_ALLOC, 0, size);
    int64_t signed_result = (int64_t)result;
    if (signed_result < 0 && signed_result >= -16) return NULL;
    return (void*)(uintptr_t)result;
}

static void wm_free(void* pointer) {
    if (pointer)
        mos_syscall3(SYS_HEAP, SYS_HEAP_FREE,
                     (uint64_t)(uintptr_t)pointer, 0);
}

static void load_desktop_settings(void) {
    mosdesk_settings_t settings = mosdesk_settings_load();
    bool wallpaper_changed = strcmp(settings.wallpaper_path,
                                     desktop_settings.wallpaper_path) != 0;
    if (wallpaper_changed)
        load_wallpaper_image(settings.wallpaper_path);
    if (wallpaper_changed || settings.wallpaper != desktop_settings.wallpaper ||
        settings.wallpaper_mode != desktop_settings.wallpaper_mode ||
        settings.wallpaper_rotation != desktop_settings.wallpaper_rotation)
        painted_wallpaper_style = UINT32_MAX;
    desktop_settings = settings;
    mouse_speed = settings.mouse_speed;
    accent_color = settings.accent_color;
    cursor_fill_color = settings.cursor_fill;
    cursor_scale = settings.cursor_scale;
    cursor_pack = settings.cursor_pack;
    wallpaper_style = settings.wallpaper;
    wallpaper_mode = settings.wallpaper_mode;
    wallpaper_rotation = settings.wallpaper_rotation;
    debug_fps = settings.debug_fps != 0u;
    debug_profiler = settings.debug_profiler != 0u;
}

static uint16_t read_u16le(const uint8_t* bytes) {
    return (uint16_t)bytes[0] | (uint16_t)((uint16_t)bytes[1] << 8u);
}

static uint32_t read_u32le(const uint8_t* bytes) {
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8u) |
           ((uint32_t)bytes[2] << 16u) | ((uint32_t)bytes[3] << 24u);
}

static bool decode_cursor(const uint8_t* data, size_t size,
                          uint32_t* pixels, uint32_t* width,
                          uint32_t* height, uint32_t* hotspot_x,
                          uint32_t* hotspot_y) {
    if (size < 22u || read_u16le(data) != 0u ||
        read_u16le(data + 2u) != 2u)
        return false;
    uint16_t count = read_u16le(data + 4u);
    if (!count || count > (size - 6u) / 16u) return false;

    uint32_t best_offset = 0;
    uint32_t best_size = 0;
    uint32_t best_width = 0;
    uint32_t best_height = 0;
    uint32_t best_hotspot_x = 0;
    uint32_t best_hotspot_y = 0;
    uint32_t best_distance = UINT32_MAX;
    for (uint32_t i = 0; i < count; i++) {
        size_t entry_offset = 6u + (size_t)i * 16u;
        uint32_t image_width = data[entry_offset]
            ? data[entry_offset] : 256u;
        uint32_t image_height = data[entry_offset + 1u]
            ? data[entry_offset + 1u] : 256u;
        uint32_t resource_size = read_u32le(data + entry_offset + 8u);
        uint32_t image_offset = read_u32le(data + entry_offset + 12u);
        if (!image_width || !image_height ||
            image_width > CURSOR_MAX_SIZE ||
            image_height > CURSOR_MAX_SIZE ||
            image_offset > size || resource_size > size - image_offset ||
            resource_size < 40u)
            continue;
        uint32_t distance = CURSOR_MAX_SIZE - image_width +
                            CURSOR_MAX_SIZE - image_height;
        if (distance < best_distance) {
            best_distance = distance;
            best_offset = image_offset;
            best_size = resource_size;
            best_width = image_width;
            best_height = image_height;
            best_hotspot_x = read_u16le(data + entry_offset + 4u);
            best_hotspot_y = read_u16le(data + entry_offset + 6u);
        }
    }
    if (!best_offset) return false;

    const uint8_t* dib = data + best_offset;
    uint32_t header_size = read_u32le(dib);
    int32_t dib_width = (int32_t)read_u32le(dib + 4u);
    int32_t dib_height = (int32_t)read_u32le(dib + 8u);
    uint16_t bits_per_pixel = read_u16le(dib + 14u);
    uint32_t compression = read_u32le(dib + 16u);
    size_t pixel_bytes = (size_t)best_width * best_height * 4u;
    if (header_size < 40u || dib_width != (int32_t)best_width ||
        dib_height != (int32_t)(best_height * 2u) ||
        bits_per_pixel != 32u || compression != 0u ||
        header_size > best_size || pixel_bytes > best_size - header_size)
        return false;

    memset(pixels, 0, CURSOR_MAX_SIZE * CURSOR_MAX_SIZE * sizeof(uint32_t));
    const uint8_t* source = dib + header_size;
    for (uint32_t y = 0; y < best_height; y++) {
        uint32_t source_y = best_height - 1u - y;
        for (uint32_t x = 0; x < best_width; x++) {
            size_t source_index =
                ((size_t)source_y * best_width + x) * 4u;
            uint8_t blue = source[source_index];
            uint8_t green = source[source_index + 1u];
            uint8_t red = source[source_index + 2u];
            uint8_t alpha = source[source_index + 3u];
            pixels[(size_t)y * CURSOR_MAX_SIZE + x] =
                ((uint32_t)alpha << 24u) | ((uint32_t)red << 16u) |
                ((uint32_t)green << 8u) | blue;
        }
    }
    *width = best_width;
    *height = best_height;
    *hotspot_x = best_hotspot_x < best_width ? best_hotspot_x : 0u;
    *hotspot_y = best_hotspot_y < best_height ? best_hotspot_y : 0u;
    return true;
}

static bool decode_animated_cursor(const uint8_t* data, size_t size) {
    if (size < 20u || memcmp(data, "RIFF", 4u) ||
        memcmp(data + 8u, "ACON", 4u))
        return false;
    uint32_t riff_size = read_u32le(data + 4u);
    if (riff_size > size - 8u) return false;
    size_t riff_end = 8u + riff_size;
    uint32_t declared_frames = 0;
    uint32_t frame_delay = 6u;
    size_t frames_start = 0;
    size_t frames_end = 0;
    for (size_t offset = 12u; offset + 8u <= riff_end;) {
        const uint8_t* chunk = data + offset;
        uint32_t chunk_size = read_u32le(chunk + 4u);
        size_t chunk_data = offset + 8u;
        if (chunk_size > riff_end - chunk_data) return false;
        if (!memcmp(chunk, "anih", 4u)) {
            if (chunk_size < 36u) return false;
            declared_frames = read_u32le(data + chunk_data + 4u);
            uint32_t rate = read_u32le(data + chunk_data + 28u);
            if (rate)
                frame_delay = rate > UINT32_MAX / 10u
                    ? UINT32_MAX : rate * 10u;
        } else if (!memcmp(chunk, "LIST", 4u) && chunk_size >= 4u &&
                   !memcmp(data + chunk_data, "fram", 4u)) {
            frames_start = chunk_data + 4u;
            frames_end = chunk_data + chunk_size;
        }
        offset = chunk_data + chunk_size + (chunk_size & 1u);
    }
    if (!declared_frames || declared_frames > CURSOR_MAX_FRAMES ||
        !frames_start || frames_end < frames_start)
        return false;
    uint32_t* frames = wm_malloc((size_t)declared_frames *
        CURSOR_MAX_SIZE * CURSOR_MAX_SIZE * sizeof(uint32_t));
    if (!frames) return false;
    memset(frames, 0, (size_t)declared_frames * CURSOR_MAX_SIZE *
           CURSOR_MAX_SIZE * sizeof(uint32_t));

    uint32_t loaded_frames = 0;
    uint32_t frame_width = 0;
    uint32_t frame_height = 0;
    uint32_t hotspot_x = 0;
    uint32_t hotspot_y = 0;
    for (size_t offset = frames_start; offset + 8u <= frames_end;) {
        const uint8_t* chunk = data + offset;
        uint32_t chunk_size = read_u32le(chunk + 4u);
        size_t chunk_data = offset + 8u;
        if (chunk_size > frames_end - chunk_data) {
            wm_free(frames);
            return false;
        }
        if (!memcmp(chunk, "icon", 4u)) {
            if (loaded_frames >= declared_frames) {
                wm_free(frames);
                return false;
            }
            uint32_t width, height, hot_x, hot_y;
            uint32_t* target = frames + (size_t)loaded_frames *
                CURSOR_MAX_SIZE * CURSOR_MAX_SIZE;
            if (!decode_cursor(data + chunk_data, chunk_size, target,
                    &width, &height, &hot_x, &hot_y) ||
                (loaded_frames && (width != frame_width ||
                                   height != frame_height ||
                                   hot_x != hotspot_x || hot_y != hotspot_y))) {
                wm_free(frames);
                return false;
            }
            frame_width = width;
            frame_height = height;
            hotspot_x = hot_x;
            hotspot_y = hot_y;
            loaded_frames++;
        }
        offset = chunk_data + chunk_size + (chunk_size & 1u);
    }
    if (loaded_frames != declared_frames) {
        wm_free(frames);
        return false;
    }
    cursor_animation_pixels = frames;
    cursor_animation_frame_count = loaded_frames;
    cursor_animation_frame_delay_ms = frame_delay ? frame_delay : 60u;
    cursor_width = frame_width;
    cursor_height = frame_height;
    cursor_hotspot_x = hotspot_x;
    cursor_hotspot_y = hotspot_y;
    cursor_animation_elapsed_ms = 0;
    return true;
}

static bool path_has_suffix(const char* path, const char* suffix) {
    size_t path_length = strlen(path);
    size_t suffix_length = strlen(suffix);
    return path_length >= suffix_length &&
        !strcmp(path + path_length - suffix_length, suffix);
}

static bool load_cursor_resource(const char* path) {
    wm_free(cursor_animation_pixels);
    cursor_animation_pixels = NULL;
    cursor_animation_frame_count = 0;
    cursor_width = 0;
    cursor_height = 0;
    cursor_hotspot_x = 0;
    cursor_hotspot_y = 0;
    size_t size = 0;
    uint8_t* data = wm_read_file(path, CURSOR_MAX_FILE_SIZE, &size);
    if (!data) return false;
    bool loaded;
    if (path_has_suffix(path, ".ani")) {
        loaded = decode_animated_cursor(data, size);
    } else {
        loaded = decode_cursor(data, size, cursor_pixels, &cursor_width,
            &cursor_height, &cursor_hotspot_x, &cursor_hotspot_y);
    }
    wm_free(data);
    return loaded;
}

static bool join_cursor_path(char* output, size_t capacity,
                             const char* pack_path, const char* filename) {
    size_t used = 0;
    while (pack_path[used] && used + 1u < capacity) {
        output[used] = pack_path[used];
        used++;
    }
    if (pack_path[used]) return false;
    if (!used || output[used - 1u] != '/') {
        if (used + 1u >= capacity) return false;
        output[used++] = '/';
    }
    for (size_t i = 0; filename[i]; i++) {
        if (used + 1u >= capacity) return false;
        output[used++] = filename[i];
    }
    output[used] = '\0';
    return true;
}

static const char* cursor_filename(uint32_t status) {
    static const char* const filenames[SYSCALL_CURSOR_COUNT] = {
        "arrow.cur", "busy.ani", "working.ani", "text.cur",
        "hand.cur", "help.cur", "forbidden.cur", "move.cur",
        "resize-ew.cur", "resize-nesw.cur", "resize-ns.cur",
        "resize-nwse.cur", "precise.cur", "pen.cur"
    };
    return status < SYSCALL_CURSOR_COUNT ? filenames[status] : filenames[0];
}

static bool try_cursor_from_pack(const char* pack_path, uint32_t status) {
    char path[SYSCALL_CURSOR_PACK_PATH_SIZE + 32u];
    if (join_cursor_path(path, sizeof(path), pack_path,
                         cursor_filename(status)) &&
        load_cursor_resource(path))
        return true;
    if (status && join_cursor_path(path, sizeof(path), pack_path,
                                   "arrow.cur") &&
        load_cursor_resource(path))
        return true;
    return false;
}

static const char* default_cursor_pack(void) {
    return cursor_pack == MOSDESK_CURSOR_PACK_CLASSIC
        ? "0:/programs/mosWM/assets/cursors/Classic"
        : "0:/programs/mosWM/assets/cursors/Posy";
}

static void ensure_cursor_loaded(const moswm_cursor_state_t* cursor_state,
                                 uint32_t status) {
    const char* system_pack = default_cursor_pack();
    const char* requested_pack = cursor_state && cursor_state->pack[0]
        ? cursor_state->pack : system_pack;
    if (status >= SYSCALL_CURSOR_COUNT) status = SYSCALL_CURSOR_ARROW;
    if (cached_cursor_status == status &&
        !strcmp(cached_cursor_pack, requested_pack))
        return;

    /* Shapes from the system pack are decoded once and kept, so crossing a
     * title bar does not read a cursor file from disk every time. */
    bool system_shape = !strcmp(requested_pack, system_pack);
    if (strcmp(cursor_cache_pack, system_pack)) {
        memset(cursor_cache, 0, sizeof(cursor_cache));
        strcpy(cursor_cache_pack, system_pack);
    }
    if (system_shape && cursor_cache[status].valid) {
        wm_free(cursor_animation_pixels);
        cursor_animation_pixels = NULL;
        cursor_animation_frame_count = 0;
        memcpy(cursor_pixels, cursor_cache[status].pixels,
               sizeof(cursor_pixels));
        cursor_width = cursor_cache[status].width;
        cursor_height = cursor_cache[status].height;
        cursor_hotspot_x = cursor_cache[status].hotspot_x;
        cursor_hotspot_y = cursor_cache[status].hotspot_y;
        cached_cursor_status = status;
        strcpy(cached_cursor_pack, requested_pack);
        return;
    }

    bool loaded = try_cursor_from_pack(requested_pack, status);
    if (!loaded && strcmp(requested_pack, system_pack)) {
        strcpy(status_message, "Cursor pack could not be loaded");
        loaded = try_cursor_from_pack(system_pack, status);
    }
    if (!loaded && status != SYSCALL_CURSOR_ARROW)
        loaded = try_cursor_from_pack(system_pack, SYSCALL_CURSOR_ARROW);
    if (!loaded) {
        cursor_width = 0;
        cursor_height = 0;
        strcpy(status_message, "Cursor resource could not be loaded");
    }
    if (loaded && system_shape && !cursor_animation_pixels) {
        cached_cursor_t* slot = &cursor_cache[status];
        memcpy(slot->pixels, cursor_pixels, sizeof(slot->pixels));
        slot->width = cursor_width;
        slot->height = cursor_height;
        slot->hotspot_x = cursor_hotspot_x;
        slot->hotspot_y = cursor_hotspot_y;
        slot->valid = true;
    }
    strcpy(cached_cursor_pack, requested_pack);
    cached_cursor_status = status;
}

/* Reads a whole file into a wm_malloc'd buffer (caller wm_free()s it). */
static uint8_t* wm_read_file(const char* path, size_t max_size, size_t* size) {
    long fd = (long)mos_syscall3(SYS_OPEN, (uint64_t)(uintptr_t)path,
                                 SYS_O_RDONLY, 0);
    if (fd < 0) return NULL;
    long file_size = (long)mos_syscall3(SYS_SIZE, (uint64_t)fd, 0, 0);
    if (file_size <= 0 || (size_t)file_size > max_size) {
        mos_syscall3(SYS_CLOSE, (uint64_t)fd, 0, 0);
        return NULL;
    }
    uint8_t* data = (uint8_t*)wm_malloc((size_t)file_size);
    if (!data) {
        mos_syscall3(SYS_CLOSE, (uint64_t)fd, 0, 0);
        return NULL;
    }
    size_t total = 0;
    while (total < (size_t)file_size) {
        long got = (long)mos_syscall3(SYS_READ, (uint64_t)fd,
            (uint64_t)(uintptr_t)(data + total),
            (uint64_t)((size_t)file_size - total));
        if (got <= 0) break;
        total += (size_t)got;
    }
    mos_syscall3(SYS_CLOSE, (uint64_t)fd, 0, 0);
    if (total != (size_t)file_size) {
        wm_free(data);
        return NULL;
    }
    *size = total;
    return data;
}

static uint32_t read_u32be(const uint8_t* bytes) {
    return ((uint32_t)bytes[0] << 24u) | ((uint32_t)bytes[1] << 16u) |
           ((uint32_t)bytes[2] << 8u) | (uint32_t)bytes[3];
}

static void load_wallpaper_image(const char* path) {
    wm_free(wallpaper_source);
    wallpaper_source = NULL;
    wallpaper_source_width = 0;
    wallpaper_source_height = 0;
    if (!path[0]) return;

    size_t file_size = 0;
    uint8_t* data = wm_read_file(path, WALLPAPER_MAX_FILE_SIZE, &file_size);
    if (!data || file_size < 22u || memcmp(data, "qoif", 4u) != 0) {
        wm_free(data);
        strcpy(status_message, "Wallpaper image could not be loaded");
        return;
    }
    uint32_t width = read_u32be(data + 4u);
    uint32_t height = read_u32be(data + 8u);
    if (!width || !height || width > 1600u || height > 1600u ||
        (uint64_t)width * height > WALLPAPER_MAX_PIXELS ||
        (data[12] != 3u && data[12] != 4u) || data[13] > 1u ||
        memcmp(data + file_size - 8u, "\0\0\0\0\0\0\0\1", 8u) != 0) {
        wm_free(data);
        strcpy(status_message, "Wallpaper image dimensions are invalid");
        return;
    }

    size_t pixel_count = (size_t)width * height;
    uint32_t* pixels = (uint32_t*)wm_malloc(pixel_count * sizeof(uint32_t));
    if (!pixels) {
        wm_free(data);
        strcpy(status_message, "Not enough memory for wallpaper image");
        return;
    }
    uint32_t index[64] = {0};
    uint8_t red = 0, green = 0, blue = 0, alpha = 255, run = 0;
    size_t offset = 14u;
    size_t data_end = file_size - 8u;
    size_t output = 0;
    bool valid = true;
    while (output < pixel_count) {
        if (run) {
            run--;
        } else {
            if (offset >= data_end) {
                valid = false;
                break;
            }
            uint8_t tag = data[offset++];
            if (tag == 0xFEu) {
                if (data_end - offset < 3u) {
                    valid = false;
                    break;
                }
                red = data[offset++];
                green = data[offset++];
                blue = data[offset++];
            } else if (tag == 0xFFu) {
                if (data_end - offset < 4u) {
                    valid = false;
                    break;
                }
                red = data[offset++];
                green = data[offset++];
                blue = data[offset++];
                alpha = data[offset++];
            } else if ((tag & 0xC0u) == 0x00u) {
                uint32_t pixel = index[tag & 0x3Fu];
                red = (uint8_t)(pixel >> 24u);
                green = (uint8_t)(pixel >> 16u);
                blue = (uint8_t)(pixel >> 8u);
                alpha = (uint8_t)pixel;
            } else if ((tag & 0xC0u) == 0x40u) {
                red = (uint8_t)(red + ((tag >> 4u) & 3u) - 2u);
                green = (uint8_t)(green + ((tag >> 2u) & 3u) - 2u);
                blue = (uint8_t)(blue + (tag & 3u) - 2u);
            } else if ((tag & 0xC0u) == 0x80u) {
                if (offset >= data_end) {
                    valid = false;
                    break;
                }
                int32_t dg = (tag & 0x3Fu) - 32;
                uint8_t second = data[offset++];
                int32_t dr = dg + ((second >> 4u) & 0x0Fu) - 8;
                int32_t db = dg + (second & 0x0Fu) - 8;
                red = (uint8_t)(red + dr);
                green = (uint8_t)(green + dg);
                blue = (uint8_t)(blue + db);
            } else {
                run = tag & 0x3Fu;
                if (run > pixel_count - output - 1u) {
                    valid = false;
                    break;
                }
            }
        }
        /* QOI's running index is keyed on RGBA; the canvas wants ARGB. */
        uint32_t pixel = (uint32_t)red << 24u |
                         (uint32_t)green << 16u |
                         (uint32_t)blue << 8u | alpha;
        pixels[output++] = 0xFF000000u | (uint32_t)red << 16u |
                           (uint32_t)green << 8u | blue;
        uint32_t slot = (red * 3u + green * 5u + blue * 7u +
                         alpha * 11u) % 64u;
        index[slot] = pixel;
    }
    wm_free(data);
    if (!valid || output != pixel_count) {
        wm_free(pixels);
        strcpy(status_message, "Wallpaper image data is invalid");
        return;
    }
    wallpaper_source = pixels;
    wallpaper_source_width = width;
    wallpaper_source_height = height;
    status_message[0] = '\0';
}

static bool wm_read_exact(uint64_t fd, void* output, size_t size) {
    size_t total = 0;
    while (total < size) {
        long got = (long)mos_syscall3(SYS_READ, fd,
            (uint64_t)(uintptr_t)((uint8_t*)output + total), size - total);
        if (got <= 0) return false;
        total += (size_t)got;
    }
    return true;
}

static bool decode_dib_icon(const uint8_t* dib, size_t size,
                            size_t pixel_offset, bool icon_file,
                            uint32_t* output) {
    if (!dib || !output || size < 40u) return false;
    uint32_t header_size = read_u32le(dib);
    int32_t width = (int32_t)read_u32le(dib + 4u);
    int32_t dib_height = (int32_t)read_u32le(dib + 8u);
    uint16_t bits = read_u16le(dib + 14u);
    uint32_t compression = read_u32le(dib + 16u);
    if (header_size < 40u || header_size > size || width <= 0 ||
        width > 256 || dib_height == 0 || dib_height == INT32_MIN ||
        (bits != 24u && bits != 32u) || compression != 0u ||
        read_u16le(dib + 12u) != 1u)
        return false;
    int32_t height = dib_height < 0 ? -dib_height : dib_height;
    if (icon_file) {
        if (dib_height <= 0 || (height & 1)) return false;
        height /= 2;
    }
    if (height <= 0 || height > 256) return false;
    uint32_t row_bytes = ((uint32_t)width * bits + 31u) / 32u * 4u;
    size_t pixel_bytes = (size_t)row_bytes * (uint32_t)height;
    if (pixel_offset < header_size || pixel_offset > size ||
        pixel_bytes > size - pixel_offset)
        return false;

    size_t mask_offset = pixel_offset + pixel_bytes;
    uint32_t mask_stride = ((uint32_t)width + 31u) / 32u * 4u;
    size_t mask_bytes = (size_t)mask_stride * (uint32_t)height;
    bool has_mask = icon_file && mask_offset <= size &&
                    mask_bytes <= size - mask_offset;
    bool has_alpha = false;
    if (bits == 32u) {
        for (int32_t y = 0; y < height && !has_alpha; y++) {
            const uint8_t* row = dib + pixel_offset +
                                 (size_t)y * row_bytes;
            for (int32_t x = 0; x < width; x++) {
                if (row[(size_t)x * 4u + 3u]) {
                    has_alpha = true;
                    break;
                }
            }
        }
    }

    memset(output, 0, DESKTOP_ICON_PIXELS * sizeof(uint32_t));
    uint32_t target_width = (uint32_t)width;
    uint32_t target_height = (uint32_t)height;
    if (target_width > target_height)
        target_height = target_height * DESKTOP_ICON_SIZE / target_width;
    else
        target_width = target_width * DESKTOP_ICON_SIZE / target_height;
    if (!target_width) target_width = 1u;
    if (!target_height) target_height = 1u;
    uint32_t offset_x = (DESKTOP_ICON_SIZE - target_width) / 2u;
    uint32_t offset_y = (DESKTOP_ICON_SIZE - target_height) / 2u;

    for (uint32_t y = 0; y < target_height; y++) {
        uint32_t source_y = y * (uint32_t)height / target_height;
        uint32_t source_row = dib_height < 0 && !icon_file
            ? source_y : (uint32_t)height - 1u - source_y;
        const uint8_t* row =
            dib + pixel_offset + (size_t)source_row * row_bytes;
        for (uint32_t x = 0; x < target_width; x++) {
            uint32_t source_x = x * (uint32_t)width / target_width;
            const uint8_t* pixel = row +
                (size_t)source_x * (bits / 8u);
            uint8_t alpha = bits == 32u && has_alpha ? pixel[3] : 255u;
            if (has_mask) {
                const uint8_t* mask_row = dib + mask_offset +
                    (size_t)((uint32_t)height - 1u - source_y) * mask_stride;
                if (mask_row[source_x / 8u] & (0x80u >> (source_x & 7u)))
                    alpha = 0u;
            }
            output[(size_t)(offset_y + y) * DESKTOP_ICON_SIZE +
                   offset_x + x] =
                (uint32_t)alpha << 24u |
                (uint32_t)pixel[2] << 16u |
                (uint32_t)pixel[1] << 8u | pixel[0];
        }
    }
    return true;
}

static bool decode_bmp_icon(const uint8_t* data, size_t size,
                            uint32_t* output) {
    if (!data || size < 54u || data[0] != 'B' || data[1] != 'M')
        return false;
    uint32_t pixel_offset = read_u32le(data + 10u);
    uint32_t header_size = read_u32le(data + 14u);
    if (header_size < 40u || pixel_offset < 14u + header_size ||
        pixel_offset > size)
        return false;
    return decode_dib_icon(data + 14u, size - 14u,
                           pixel_offset - 14u, false, output);
}

static bool decode_ico_icon(const uint8_t* data, size_t size,
                            uint32_t* output) {
    if (!data || size < 22u || read_u16le(data) != 0u ||
        read_u16le(data + 2u) != 1u)
        return false;
    uint16_t count = read_u16le(data + 4u);
    if (!count || count > (size - 6u) / 16u) return false;
    uint32_t best_index = UINT32_MAX;
    uint32_t best_score = UINT32_MAX;
    for (uint32_t i = 0; i < count; i++) {
        size_t entry = 6u + (size_t)i * 16u;
        uint32_t width = data[entry] ? data[entry] : 256u;
        uint32_t height = data[entry + 1u] ? data[entry + 1u] : 256u;
        uint32_t image_size = read_u32le(data + entry + 8u);
        uint32_t image_offset = read_u32le(data + entry + 12u);
        if (!image_size || image_offset > size ||
            image_size > size - image_offset)
            continue;
        uint32_t distance = width > DESKTOP_ICON_SIZE
            ? width - DESKTOP_ICON_SIZE : DESKTOP_ICON_SIZE - width;
        distance += height > DESKTOP_ICON_SIZE
            ? height - DESKTOP_ICON_SIZE : DESKTOP_ICON_SIZE - height;
        if (distance < best_score) {
            best_index = i;
            best_score = distance;
        }
    }
    if (best_index == UINT32_MAX) return false;
    size_t entry = 6u + (size_t)best_index * 16u;
    uint32_t image_size = read_u32le(data + entry + 8u);
    uint32_t image_offset = read_u32le(data + entry + 12u);
    const uint8_t* dib = data + image_offset;
    uint32_t header_size = read_u32le(dib);
    if (header_size < 40u || header_size > image_size) return false;
    return decode_dib_icon(dib, image_size, header_size, true, output);
}

static bool decode_icon(const uint8_t* data, size_t size, bool ico,
                        uint32_t* output) {
    return ico ? decode_ico_icon(data, size, output)
               : decode_bmp_icon(data, size, output);
}

static bool read_bundle_icon(const char* path, uint32_t* output) {
    long fd = (long)mos_syscall3(SYS_OPEN, (uint64_t)(uintptr_t)path,
                                 SYS_O_RDONLY, 0);
    if (fd < 0) return false;
    long file_size = (long)mos_syscall3(SYS_SIZE, (uint64_t)fd, 0, 0);
    uint8_t header[16];
    if (file_size < (long)sizeof(header) || file_size > UINT32_MAX ||
        !wm_read_exact((uint64_t)fd, header, sizeof(header)) ||
        memcmp(header, "MINIRUN1", 8u) ||
        read_u32le(header + 8u) != 1u) {
        mos_syscall3(SYS_CLOSE, (uint64_t)fd, 0, 0);
        return false;
    }
    uint32_t count = read_u32le(header + 12u);
    if (!count || count > 512u ||
        count > ((uint32_t)file_size - sizeof(header)) / 72u) {
        mos_syscall3(SYS_CLOSE, (uint64_t)fd, 0, 0);
        return false;
    }
    size_t table_size = (size_t)count * 72u;
    uint8_t* table = (uint8_t*)wm_malloc(table_size);
    if (!table ||
        mos_syscall3(SYS_SEEK, (uint64_t)fd, sizeof(header), 0) !=
            SYS_SUCCESS ||
        !wm_read_exact((uint64_t)fd, table, table_size)) {
        wm_free(table);
        mos_syscall3(SYS_CLOSE, (uint64_t)fd, 0, 0);
        return false;
    }

    static const char* const icon_names[] = {
        "icon.bmp", "icon.ico", "Resources/icon.bmp", "Resources/icon.ico"
    };
    bool loaded = false;
    for (uint32_t priority = 0; priority < 4u && !loaded; priority++) {
        for (uint32_t i = 0; i < count; i++) {
            const uint8_t* entry = table + (size_t)i * 72u;
            if (strncmp((const char*)entry, icon_names[priority], 64u))
                continue;
            uint32_t offset = read_u32le(entry + 64u);
            uint32_t size = read_u32le(entry + 68u);
            uint32_t table_end = 16u + count * 72u;
            if (offset < table_end || offset > (uint32_t)file_size ||
                size < 4u || size > RUN_ICON_MAX_SIZE ||
                size > (uint32_t)file_size - offset)
                continue;
            uint8_t* image = (uint8_t*)wm_malloc(size);
            if (!image) continue;
            if (mos_syscall3(SYS_SEEK, (uint64_t)fd, offset, 0) ==
                    SYS_SUCCESS &&
                wm_read_exact((uint64_t)fd, image, size))
                loaded = decode_icon(image, size, priority == 1u ||
                    priority == 3u, output);
            wm_free(image);
            if (loaded) break;
        }
    }
    wm_free(table);
    mos_syscall3(SYS_CLOSE, (uint64_t)fd, 0, 0);
    return loaded;
}

static bool path_join(char* output, size_t capacity,
                      const char* directory, const char* name) {
    size_t directory_length = strlen(directory);
    size_t name_length = strlen(name);
    if (!directory_length || !name_length) return false;
    bool needs_separator = directory[directory_length - 1u] != '/';
    if (directory_length + name_length + (needs_separator ? 2u : 1u) >
        capacity)
        return false;
    memcpy(output, directory, directory_length);
    size_t name_offset = directory_length;
    if (needs_separator) output[name_offset++] = '/';
    memcpy(output + name_offset, name, name_length + 1u);
    return true;
}

static bool is_run_name(const char* name) {
    size_t length = strlen(name);
    return length > 4u && !strcmp(name + length - 4u, ".run");
}

static void add_desktop_app(const char* path, const char* run_name) {
    if (desktop_app_count >= MAX_DESKTOP_APPS ||
        !is_run_name(run_name) || !strcmp(run_name, "mosWM.run"))
        return;
    desktop_app_t* app = &desktop_apps[desktop_app_count];
    size_t path_length = strlen(path);
    size_t name_length = strlen(run_name) - 4u;
    if (path_length >= sizeof(app->path) ||
        !name_length || name_length >= sizeof(app->name))
        return;
    memcpy(app->path, path, path_length + 1u);
    memcpy(app->name, run_name, name_length);
    app->name[name_length] = '\0';
    app->has_icon = read_bundle_icon(app->path, app->icon_pixels);
    desktop_app_count++;
}

static void load_desktop_apps(void) {
    syscall_dirent_t entries[DESKTOP_DIR_ENTRIES];
    const char* programs = "0:/programs";
    uint64_t count = mos_syscall3(SYS_LISTDIR,
        (uint64_t)(uintptr_t)programs, (uint64_t)(uintptr_t)entries,
        DESKTOP_DIR_ENTRIES);
    if ((int64_t)count < 0) {
        log_message("mosWM: unable to list 0:/programs\n");
        return;
    }
    if (count > DESKTOP_DIR_ENTRIES) count = DESKTOP_DIR_ENTRIES;
    for (uint64_t i = 0; i < count &&
         desktop_app_count < MAX_DESKTOP_APPS; i++) {
        if (entries[i].hidden) continue;
        char path[DESKTOP_APP_PATH_SIZE];
        if (entries[i].type == SYSCALL_DIRENT_TYPE_FILE ||
            entries[i].type == SYSCALL_DIRENT_TYPE_EXECUTABLE) {
            if (is_run_name(entries[i].name) &&
                path_join(path, sizeof(path), programs, entries[i].name))
                add_desktop_app(path, entries[i].name);
            continue;
        }
        if (entries[i].type != SYSCALL_DIRENT_TYPE_DIR ||
            !path_join(path, sizeof(path), programs, entries[i].name))
            continue;
        syscall_dirent_t children[DESKTOP_DIR_ENTRIES];
        uint64_t child_count = mos_syscall3(SYS_LISTDIR,
            (uint64_t)(uintptr_t)path,
            (uint64_t)(uintptr_t)children, DESKTOP_DIR_ENTRIES);
        if ((int64_t)child_count < 0) continue;
        if (child_count > DESKTOP_DIR_ENTRIES)
            child_count = DESKTOP_DIR_ENTRIES;
        for (uint64_t child = 0; child < child_count &&
             desktop_app_count < MAX_DESKTOP_APPS; child++) {
            if (children[child].hidden ||
                (children[child].type != SYSCALL_DIRENT_TYPE_FILE &&
                 children[child].type != SYSCALL_DIRENT_TYPE_EXECUTABLE) ||
                !is_run_name(children[child].name))
                continue;
            char run_path[DESKTOP_APP_PATH_SIZE];
            if (path_join(run_path, sizeof(run_path), path,
                          children[child].name))
                add_desktop_app(run_path, children[child].name);
        }
    }
}

static uint8_t* font_regular_data;
static uint8_t* font_medium_data;

/* Smooth OpenType text; silently stays on the bitmap font if the files are
 * missing or unusable. */
static void load_fonts(void) {
    size_t regular_size = 0, medium_size = 0;
    font_regular_data = wm_read_file(
        "0:/programs/mosWM/assets/fonts/Inter-Regular.otf",
        2u * 1024u * 1024u, &regular_size);
    font_medium_data = wm_read_file(
        "0:/programs/mosWM/assets/fonts/Inter-Medium.otf",
        2u * 1024u * 1024u, &medium_size);
    if (!moswm_text_load_fonts(font_regular_data, regular_size,
                               font_medium_data, medium_size))
        log_message("mosWM: no outline fonts, using the bitmap font\n");
}

static void log_message(const char* message) {
    uint32_t length = 0;
    while (message[length]) length++;
    mos_write(message, length);
}

static uint32_t clamp_u32(uint32_t value, uint32_t low, uint32_t high) {
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

static uint32_t alpha_blend(uint32_t source, uint32_t destination) {
    uint32_t alpha = source >> 24;
    if (alpha == 255u) return source;
    if (!alpha) return destination;
    uint32_t inverse = 255u - alpha;
    uint32_t red = (((source >> 16) & 255u) * alpha +
                    ((destination >> 16) & 255u) * inverse) / 255u;
    uint32_t green = (((source >> 8) & 255u) * alpha +
                      ((destination >> 8) & 255u) * inverse) / 255u;
    uint32_t blue = ((source & 255u) * alpha +
                     (destination & 255u) * inverse) / 255u;
    return 0xFF000000u | (red << 16) | (green << 8) | blue;
}

static void put_pixel(int32_t x, int32_t y, uint32_t color) {
    if (x < 0 || y < 0 || (uint32_t)x >= display.width ||
        (uint32_t)y >= display.height)
        return;
    uint32_t* pixel = canvas + (size_t)y * display.width + (uint32_t)x;
    *pixel = alpha_blend(color, *pixel);
}

static void fill_rect(int32_t x, int32_t y, int32_t width, int32_t height,
                      uint32_t color) {
    int64_t right = (int64_t)x + width;
    int64_t bottom = (int64_t)y + height;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (right > display.width) right = display.width;
    if (bottom > display.height) bottom = display.height;
    uint32_t alpha = color >> 24;
    if (!alpha || right <= x) return;
    for (int32_t py = y; py < bottom; py++) {
        uint32_t* row = canvas + (size_t)py * display.width;
        if (alpha == 255u) {
            for (int32_t px = x; px < right; px++) row[px] = color;
        } else {
            for (int32_t px = x; px < right; px++)
                row[px] = alpha_blend(color, row[px]);
        }
    }
}

static void draw_wallpaper_line(int32_t x0, int32_t y0, int32_t x1,
                                int32_t y1, uint32_t color) {
    int32_t dx = x1 >= x0 ? x1 - x0 : x0 - x1;
    int32_t sx = x0 < x1 ? 1 : -1;
    int32_t dy = y1 >= y0 ? y0 - y1 : y1 - y0;
    int32_t sy = y0 < y1 ? 1 : -1;
    int32_t error = dx + dy;
    for (;;) {
        put_pixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int32_t twice_error = error * 2;
        if (twice_error >= dy) {
            error += dy;
            x0 += sx;
        }
        if (twice_error <= dx) {
            error += dx;
            y0 += sy;
        }
    }
}

static void wm_text(int32_t x, int32_t y, const char* text, uint32_t color,
                    uint32_t shadow, uint32_t scale) {
    moswm_draw_text(canvas, display.width, display.height, x, y, text, color,
                    shadow, scale);
}

static void stroke_rect(int32_t x, int32_t y, int32_t w, int32_t h,
                        uint32_t color) {
    fill_rect(x, y, w, 1, color);
    fill_rect(x, y + h - 1, w, 1, color);
    fill_rect(x, y, 1, h, color);
    fill_rect(x + w - 1, y, 1, h, color);
}

/* Copies up to `max_chars` of `src` into `dst`, ending in ".." if cut. */
static void fit_text(char* dst, size_t dst_size, const char* src,
                     uint32_t max_chars) {
    uint32_t len = 0;
    while (src[len]) len++;
    if (max_chars + 1u > dst_size) max_chars = (uint32_t)dst_size - 1u;
    if (len <= max_chars) {
        for (uint32_t i = 0; i <= len; i++) dst[i] = src[i];
        return;
    }
    if (max_chars < 3u) {
        dst[0] = '\0';
        return;
    }
    for (uint32_t i = 0; i < max_chars - 2u; i++) dst[i] = src[i];
    dst[max_chars - 2u] = '.';
    dst[max_chars - 1u] = '.';
    dst[max_chars] = '\0';
}

/* Like fit_text, but cuts to `max_px` pixels of rendered width (proportional
 * smooth text is much narrower than the 9 px/char bitmap font). */
static void fit_text_px(char* dst, size_t dst_size, const char* src,
                        int32_t max_px, uint32_t scale) {
    size_t len = 0;
    while (src[len] && len + 1u < dst_size) {
        dst[len] = src[len];
        len++;
    }
    dst[len] = '\0';
    if (max_px <= 0) {
        dst[0] = '\0';
        return;
    }
    if ((int32_t)moswm_text_width(dst, scale) <= max_px && !src[len]) return;
    while (len > 0) {
        /* never cut inside a UTF-8 sequence */
        size_t cut = len - 1u;
        while (cut > 0 && ((uint8_t)dst[cut] & 0xC0u) == 0x80u) cut--;
        len = cut;
        dst[len] = '\0';
        if (len + 3u < dst_size) {
            dst[len] = '.';
            dst[len + 1u] = '.';
            dst[len + 2u] = '\0';
            if ((int32_t)moswm_text_width(dst, scale) <= max_px) return;
            dst[len] = '\0';
        }
    }
}

/* ------------------------------------------------------------ file helpers */

static const char* path_name(const char* path) {
    const char* name = path;
    for (const char* cursor = path; *cursor; cursor++)
        if (*cursor == '/' && cursor[1]) name = cursor + 1;
    return name;
}

static bool path_parent(char* output, size_t capacity, const char* path) {
    size_t length = (size_t)(path_name(path) - path);
    if (length < 3u || length >= capacity) return false;
    memcpy(output, path, length);
    if (length > 3u) length--;          /* keep the slash of "0:/" only */
    output[length] = '\0';
    return true;
}

static bool path_has_prefix(const char* path, const char* prefix) {
    size_t length = strlen(prefix);
    return !strncmp(path, prefix, length) &&
           (path[length] == '\0' || path[length] == '/');
}

static bool name_has_extension(const char* name, const char* extension) {
    size_t name_length = strlen(name);
    size_t extension_length = strlen(extension);
    return name_length > extension_length &&
           !strcmp(name + name_length - extension_length, extension);
}

/* The desktop itself, and the folders it is installed in, cannot be renamed
 * or deleted from the shell: that would leave the system unbootable. */
static bool path_is_protected(const char* path) {
    static const char* const exact[] = {
        "0:/", "0:/programs", "0:/services", MOSDESK_USERS_PATH
    };
    for (uint32_t i = 0; i < sizeof(exact) / sizeof(exact[0]); i++)
        if (!strcmp(path, exact[i])) return true;
    return path_has_prefix(path, "0:/programs/mosWM") ||
           path_has_prefix(path, "0:/programs/mosdesk") ||
           path_has_prefix(path, "0:/services/mosWM.service");
}

static bool fs_exists(const char* path) {
    return mos_syscall3(SYS_EXISTS, (uint64_t)(uintptr_t)path, 0, 0) == 1u;
}

static bool fs_is_directory(const char* path) {
    return mos_syscall3(SYS_IS_DIR, (uint64_t)(uintptr_t)path, 0, 0) == 1u;
}

static bool fs_copy_file(const char* source, const char* target) {
    syscall_file_metadata_t metadata;
    memset(&metadata, 0, sizeof(metadata));
    bool has_metadata = mos_syscall3(SYS_GET_METADATA,
        (uint64_t)(uintptr_t)source, (uint64_t)(uintptr_t)&metadata, 0) ==
        SYS_SUCCESS;
    long input = (long)mos_syscall3(SYS_OPEN, (uint64_t)(uintptr_t)source,
                                    SYS_O_RDONLY, 0);
    if (input < 0) return false;
    long remaining = (long)mos_syscall3(SYS_SIZE, (uint64_t)input, 0, 0);
    uint8_t* chunk = (uint8_t*)wm_malloc(FILE_COPY_CHUNK);
    long output = -1;
    bool ok = remaining >= 0 && chunk &&
        mos_syscall3(SYS_CREATE, (uint64_t)(uintptr_t)target,
            (uint64_t)(uintptr_t)(has_metadata && metadata.filetype[0]
                ? metadata.filetype : "binary"),
            (uint64_t)(uintptr_t)(has_metadata && metadata.fileformat[0]
                ? metadata.fileformat : "bin")) == SYS_SUCCESS;
    if (ok) {
        output = (long)mos_syscall3(SYS_OPEN, (uint64_t)(uintptr_t)target,
                                    SYS_O_RDWR, 0);
        ok = output >= 0;
    }
    while (ok && remaining > 0) {
        size_t want = (size_t)remaining > FILE_COPY_CHUNK
            ? FILE_COPY_CHUNK : (size_t)remaining;
        long got = (long)mos_syscall3(SYS_READ, (uint64_t)input,
            (uint64_t)(uintptr_t)chunk, want);
        if (got <= 0 ||
            (long)mos_syscall3(SYS_FWRITE, (uint64_t)output,
                (uint64_t)(uintptr_t)chunk, (uint64_t)got) != got) {
            ok = false;
            break;
        }
        remaining -= got;
    }
    if (output >= 0) mos_syscall3(SYS_CLOSE, (uint64_t)output, 0, 0);
    mos_syscall3(SYS_CLOSE, (uint64_t)input, 0, 0);
    wm_free(chunk);
    if (!ok && output >= 0)
        mos_syscall3(SYS_DELETE, (uint64_t)(uintptr_t)target, 0, 0);
    return ok;
}

/* Directory listings live on the heap: the user stack is only 64 KiB. */
static syscall_dirent_t* fs_list(const char* path, uint32_t* count) {
    syscall_dirent_t* entries = (syscall_dirent_t*)wm_malloc(
        sizeof(syscall_dirent_t) * DESKTOP_DIR_ENTRIES);
    *count = 0;
    if (!entries) return NULL;
    uint64_t result = mos_syscall3(SYS_LISTDIR, (uint64_t)(uintptr_t)path,
        (uint64_t)(uintptr_t)entries, DESKTOP_DIR_ENTRIES);
    if ((int64_t)result < 0) {
        wm_free(entries);
        return NULL;
    }
    *count = result > DESKTOP_DIR_ENTRIES
        ? DESKTOP_DIR_ENTRIES : (uint32_t)result;
    return entries;
}

static bool fs_delete_tree(const char* path, uint32_t depth) {
    if (!fs_is_directory(path))
        return mos_syscall3(SYS_DELETE, (uint64_t)(uintptr_t)path, 0, 0) ==
               SYS_SUCCESS;
    if (depth >= FILE_TREE_MAX_DEPTH) return false;
    uint32_t count;
    syscall_dirent_t* entries = fs_list(path, &count);
    if (!entries) return false;
    bool ok = true;
    for (uint32_t i = 0; i < count && ok; i++) {
        char child[BROWSER_PATH_SIZE];
        ok = path_join(child, sizeof(child), path, entries[i].name) &&
             fs_delete_tree(child, depth + 1u);
    }
    wm_free(entries);
    return ok && mos_syscall3(SYS_RMDIR, (uint64_t)(uintptr_t)path, 0, 0) ==
                 SYS_SUCCESS;
}

static bool fs_copy_tree(const char* source, const char* target,
                         uint32_t depth) {
    if (!fs_is_directory(source)) return fs_copy_file(source, target);
    if (depth >= FILE_TREE_MAX_DEPTH || path_has_prefix(target, source) ||
        mos_syscall3(SYS_MKDIR, (uint64_t)(uintptr_t)target, 0, 0) !=
            SYS_SUCCESS)
        return false;
    uint32_t count;
    syscall_dirent_t* entries = fs_list(source, &count);
    if (!entries) return false;
    bool ok = true;
    for (uint32_t i = 0; i < count && ok; i++) {
        char from[BROWSER_PATH_SIZE];
        char to[BROWSER_PATH_SIZE];
        ok = path_join(from, sizeof(from), source, entries[i].name) &&
             path_join(to, sizeof(to), target, entries[i].name) &&
             fs_copy_tree(from, to, depth + 1u);
    }
    wm_free(entries);
    return ok;
}

/* MinimaFS has no rename call, so a rename is a copy followed by a delete. */
static bool fs_rename(const char* source, const char* target) {
    if (!fs_copy_tree(source, target, 0)) {
        if (fs_exists(target)) fs_delete_tree(target, 0);
        return false;
    }
    return fs_delete_tree(source, 0);
}

/* "notes.txt" -> "notes copy.txt", then "notes copy 2.txt" ... */
static bool unique_copy_path(char* output, size_t capacity,
                             const char* directory, const char* name) {
    if (path_join(output, capacity, directory, name) && !fs_exists(output))
        return true;
    size_t length = strlen(name);
    size_t stem = length;
    for (size_t i = length; i > 1u; i--) {
        if (name[i - 1u] == '.') {
            stem = i - 1u;
            break;
        }
    }
    for (uint32_t attempt = 1u; attempt <= 9u; attempt++) {
        char candidate[SYSCALL_DIRENT_NAME_SIZE];
        size_t used = 0;
        if (stem + 8u + (length - stem) >= sizeof(candidate)) return false;
        memcpy(candidate, name, stem);
        used = stem;
        memcpy(candidate + used, " copy", 5u);
        used += 5u;
        if (attempt > 1u) {
            candidate[used++] = ' ';
            candidate[used++] = (char)('0' + attempt);
        }
        memcpy(candidate + used, name + stem, length - stem + 1u);
        if (path_join(output, capacity, directory, candidate) &&
            !fs_exists(output))
            return true;
    }
    return false;
}

static moswm_file_icon_t icon_for_entry(uint32_t type, const char* name) {
    static const struct {
        const char* extension;
        moswm_file_icon_t icon;
    } known[] = {
        {".run", MOSWM_ICON_APP}, {".qoi", MOSWM_ICON_IMAGE},
        {".bmp", MOSWM_ICON_IMAGE}, {".png", MOSWM_ICON_IMAGE},
        {".jpg", MOSWM_ICON_IMAGE}, {".ico", MOSWM_ICON_IMAGE},
        {".thm", MOSWM_ICON_IMAGE}, {".wav", MOSWM_ICON_AUDIO},
        {".mp3", MOSWM_ICON_AUDIO}, {".mp4", MOSWM_ICON_VIDEO},
        {".txt", MOSWM_ICON_TEXT}, {".md", MOSWM_ICON_TEXT},
        {".log", MOSWM_ICON_TEXT}, {".otf", MOSWM_ICON_TEXT},
        {".ttf", MOSWM_ICON_TEXT}, {".font", MOSWM_ICON_TEXT},
        {".pdf", MOSWM_ICON_PDF}, {".mpkg", MOSWM_ICON_ARCHIVE},
        {".pkg", MOSWM_ICON_ARCHIVE}, {".zip", MOSWM_ICON_ARCHIVE},
        {".slib", MOSWM_ICON_ARCHIVE}, {".c", MOSWM_ICON_CODE},
        {".h", MOSWM_ICON_CODE}, {".sh", MOSWM_ICON_TERMINAL},
        {".py", MOSWM_ICON_CODE}, {".service", MOSWM_ICON_SETTINGS},
        {".cfg", MOSWM_ICON_SETTINGS}, {".dat", MOSWM_ICON_SETTINGS},
        {".conf", MOSWM_ICON_SETTINGS},
        {".desc", MOSWM_ICON_SETTINGS}, {".cur", MOSWM_ICON_POINTER},
        {".ani", MOSWM_ICON_POINTER}
    };
    if (type == SYSCALL_DIRENT_TYPE_DIR) return MOSWM_ICON_FOLDER;
    for (uint32_t i = 0; i < sizeof(known) / sizeof(known[0]); i++)
        if (name_has_extension(name, known[i].extension))
            return known[i].icon;
    return type == SYSCALL_DIRENT_TYPE_EXECUTABLE
        ? MOSWM_ICON_APP : MOSWM_ICON_DOCUMENT;
}

static const char* kind_for_icon(moswm_file_icon_t icon) {
    switch (icon) {
        case MOSWM_ICON_FOLDER: return "Folder";
        case MOSWM_ICON_APP: return "Application";
        case MOSWM_ICON_IMAGE: return "Image";
        case MOSWM_ICON_AUDIO: return "Audio";
        case MOSWM_ICON_VIDEO: return "Video";
        case MOSWM_ICON_TEXT: return "Text";
        case MOSWM_ICON_PDF: return "PDF";
        case MOSWM_ICON_ARCHIVE: return "Package";
        case MOSWM_ICON_CODE: return "Source";
        case MOSWM_ICON_TERMINAL: return "Script";
        case MOSWM_ICON_SETTINGS: return "Settings";
        case MOSWM_ICON_POINTER: return "Cursor";
        default: return "File";
    }
}

/* ------------------------------------------------------------ File Browser */

static void browser_note(const char* message, bool is_error) {
    size_t length = strlen(message);
    if (length >= sizeof(browser_message))
        length = sizeof(browser_message) - 1u;
    memcpy(browser_message, message, length);
    browser_message[length] = '\0';
    browser_message_is_error = is_error;
    browser_generation++;
}

static uint32_t browser_visible_rows(uint32_t height) {
    return height > BROWSER_HEADER_HEIGHT + BROWSER_FOOTER_HEIGHT
        ? (height - BROWSER_HEADER_HEIGHT - BROWSER_FOOTER_HEIGHT) /
              BROWSER_ROW_HEIGHT
        : 0u;
}

static int compare_names(const char* a, const char* b) {
    for (;; a++, b++) {
        char ca = *a >= 'A' && *a <= 'Z' ? (char)(*a + 32) : *a;
        char cb = *b >= 'A' && *b <= 'Z' ? (char)(*b + 32) : *b;
        if (ca != cb) return ca < cb ? -1 : 1;
        if (!ca) return 0;
    }
}

static void browser_refresh_directory(void) {
    browser_entry_count = 0;
    browser_selected = UINT32_MAX;
    browser_scroll = 0;
    browser_generation++;
    uint64_t result = mos_syscall3(SYS_LISTDIR,
        (uint64_t)(uintptr_t)browser_path,
        (uint64_t)(uintptr_t)browser_entries, BROWSER_MAX_ENTRIES);
    if ((int64_t)result < 0) {
        browser_note("Unable to list this folder", true);
        return;
    }
    uint32_t count = result > BROWSER_MAX_ENTRIES
        ? BROWSER_MAX_ENTRIES : (uint32_t)result;
    for (uint32_t i = 0; i < count; i++) {
        if (browser_entries[i].hidden) continue;
        if (browser_entry_count != i)
            browser_entries[browser_entry_count] = browser_entries[i];
        browser_entry_count++;
    }
    /* Folders first, then by name. */
    for (uint32_t i = 1; i < browser_entry_count; i++) {
        syscall_dirent_t moving = browser_entries[i];
        bool moving_dir = moving.type == SYSCALL_DIRENT_TYPE_DIR;
        uint32_t slot = i;
        while (slot > 0) {
            const syscall_dirent_t* before = &browser_entries[slot - 1u];
            bool before_dir = before->type == SYSCALL_DIRENT_TYPE_DIR;
            if (before_dir != moving_dir ? before_dir
                    : compare_names(before->name, moving.name) <= 0)
                break;
            browser_entries[slot] = *before;
            slot--;
        }
        browser_entries[slot] = moving;
    }
    browser_message[0] = '\0';
}

static void browser_select_name(const char* name) {
    for (uint32_t i = 0; i < browser_entry_count; i++) {
        if (strcmp(browser_entries[i].name, name)) continue;
        browser_selected = i;
        uint32_t rows = browser_visible_rows(browser_height);
        if (rows && i >= rows) browser_scroll = i - rows + 1u;
        break;
    }
    browser_generation++;
}

static void browser_close(void) {
    if (browser_window_id) mos_window_close(browser_window_id);
    browser_window_id = 0;
    wm_free(browser_pixels);
    browser_pixels = NULL;
    browser_width = 0;
    browser_height = 0;
    browser_entry_count = 0;
    browser_selected = UINT32_MAX;
    browser_drawn_signature = 0;
    last_click_valid = false;
}

static void set_wallpaper_image(const char* path) {
    if (strlen(path) >= MOSDESK_WALLPAPER_PATH_SIZE) {
        browser_note("Wallpaper path is too long", true);
        return;
    }
    mosdesk_settings_t next = desktop_settings;
    memset(next.wallpaper_path, 0, sizeof(next.wallpaper_path));
    memcpy(next.wallpaper_path, path, strlen(path));
    if (!mosdesk_settings_save(&next)) {
        browser_note("Unable to save wallpaper settings", true);
        return;
    }
    desktop_settings = next;
    load_wallpaper_image(desktop_settings.wallpaper_path);
    painted_wallpaper_style = UINT32_MAX;
    browser_note(wallpaper_source ? "Wallpaper changed"
                                  : "Wallpaper image could not be loaded",
                 !wallpaper_source);
}

static void browser_open(const char* path) {
    if (!path || strlen(path) >= sizeof(browser_path) ||
        mos_syscall3(SYS_IS_DIR, (uint64_t)(uintptr_t)path, 0, 0) != 1u) {
        strcpy(status_message, "Folder is unavailable");
        return;
    }
    memcpy(browser_path, path, strlen(path) + 1u);
    browser_refresh_directory();
    if (!browser_window_id) {
        uint32_t width = display.width > 700u ? 660u : display.width - 36u;
        uint32_t height = display.height > TASKBAR_HEIGHT + 540u
            ? 500u : display.height - TASKBAR_HEIGHT - 36u;
        if (width < 240u) width = display.width - 36u;
        if (height < 96u) height = 96u;
        uint32_t x = (display.width - width) / 2u;
        uint32_t y = (display.height - TASKBAR_HEIGHT - height) / 2u;
        browser_window_id = mos_window_create(0, x, y, width, height,
                                               "Files");
        if (!browser_window_id) {
            strcpy(status_message, "Unable to open Files");
            return;
        }
        browser_width = width;
        browser_height = height;
        browser_drawn_signature = 0;
        browser_pixels = (uint32_t*)wm_malloc(
            (size_t)width * height * sizeof(uint32_t));
        if (!browser_pixels) {
            browser_close();
            strcpy(status_message, "Unable to allocate Files");
            return;
        }
        syscall_window_style_t style = {
            .title_bar = 0xFF263945u,
            .title_text = 0xFFF4F7F8u,
            .close_button = 0xFF263945u,
            .minimize_button = 0xFF263945u,
            .maximize_button = 0xFF263945u,
            .close_icon = 0xFFF4F7F8u,
            .minimize_icon = 0xFFF4F7F8u,
            .maximize_icon = 0xFFF4F7F8u,
            .border = 0xFF536773u,
            .content_background = 0xFFF3F5F6u
        };
        if (mos_window_set_resizable(browser_window_id, true) != SYS_SUCCESS ||
            mos_window_set_style(browser_window_id, style) != SYS_SUCCESS) {
            browser_close();
            strcpy(status_message, "Unable to configure Files");
            return;
        }
    }
    syscall_window_info_t info;
    if (mos_window_get_info(browser_window_id, &info) == SYS_SUCCESS) {
        if (info.flags & SYSCALL_WINDOW_MINIMIZED) {
            cached_window_t* entry = cache_for(browser_window_id);
            if (entry && set_window_mode(&info, entry, MODE_NORMAL))
                post_simple_event(browser_window_id,
                                  SYSCALL_WINDOW_EVENT_RESTORED);
        }
        focus_window(&info);
    } else {
        strcpy(status_message, "Unable to focus Files");
    }
}

static void browser_go_up(void) {
    if (!strcmp(browser_path, "0:/")) return;
    char previous[SYSCALL_DIRENT_NAME_SIZE];
    const char* name = path_name(browser_path);
    size_t name_length = strlen(name);
    if (name_length >= sizeof(previous)) name_length = sizeof(previous) - 1u;
    memcpy(previous, name, name_length);
    previous[name_length] = '\0';
    char parent[BROWSER_PATH_SIZE];
    if (!path_parent(parent, sizeof(parent), browser_path))
        strcpy(parent, "0:/");
    strcpy(browser_path, parent);
    browser_refresh_directory();
    browser_select_name(previous);
}

static void format_count(char* output, uint32_t count, const char* noun) {
    char digits[12];
    uint32_t length = 0;
    do {
        digits[length++] = (char)('0' + count % 10u);
        count /= 10u;
    } while (count);
    uint32_t used = 0;
    while (length) output[used++] = digits[--length];
    output[used++] = ' ';
    while (*noun) output[used++] = *noun++;
    output[used] = '\0';
}

static int32_t browser_tool_x(uint32_t index) {
    return 8 + (int32_t)index * 36;
}

/* Repaints the Files window only when something it shows has changed. */
static void browser_render(void) {
    if (!browser_window_id || !browser_pixels) return;
    syscall_window_info_t info;
    if (mos_window_get_info(browser_window_id, &info) != SYS_SUCCESS) {
        strcpy(status_message, "Files window is unavailable");
        browser_close();
        return;
    }
    if (info.width != browser_width || info.height != browser_height) {
        size_t bytes = (size_t)info.width * info.height * sizeof(uint32_t);
        uint32_t* resized = (uint32_t*)wm_malloc(bytes);
        if (!resized) {
            strcpy(status_message, "Unable to resize Files");
            return;
        }
        wm_free(browser_pixels);
        browser_pixels = resized;
        browser_width = info.width;
        browser_height = info.height;
    }
    uint32_t visible_rows = browser_visible_rows(browser_height);
    uint32_t max_scroll = browser_entry_count > visible_rows
        ? browser_entry_count - visible_rows : 0u;
    if (browser_scroll > max_scroll) browser_scroll = max_scroll;

    uint64_t signature = 0xCBF29CE484222325ull;
    signature = hash_u64(signature, browser_generation);
    signature = hash_u64(signature, ((uint64_t)browser_width << 32) |
                                    browser_height);
    signature = hash_u64(signature, ((uint64_t)browser_selected << 32) |
                                    browser_scroll);
    signature = hash_u64(signature, ((uint64_t)browser_hover << 32) |
                                    browser_tool_hover);
    signature = hash_u64(signature, accent_color);
    signature = hash_text(signature, clipboard_path);
    if (signature == browser_drawn_signature) return;
    browser_drawn_signature = signature;

    uint32_t* screen_canvas = canvas;
    uint32_t screen_width = display.width;
    uint32_t screen_height = display.height;
    canvas = browser_pixels;
    display.width = browser_width;
    display.height = browser_height;
    int32_t width = (int32_t)browser_width;
    int32_t height = (int32_t)browser_height;
    uint32_t accent = 0xFF000000u | (accent_color & 0x00FFFFFFu);
    fill_rect(0, 0, width, height, 0xFFF7F9FAu);

    /* Toolbar: icon buttons and the current location. */
    fill_rect(0, 0, width, (int32_t)BROWSER_HEADER_HEIGHT, 0xFFE9EDF0u);
    fill_rect(0, (int32_t)BROWSER_HEADER_HEIGHT - 1, width, 1, 0xFFD2D9DEu);
    static const moswm_file_icon_t tools[BROWSER_TOOL_COUNT] = {
        MOSWM_ICON_HOME, MOSWM_ICON_COMPUTER, MOSWM_ICON_UP,
        MOSWM_ICON_REFRESH, MOSWM_ICON_NEWFOLDER
    };
    for (uint32_t i = 0; i < BROWSER_TOOL_COUNT; i++) {
        int32_t x = browser_tool_x(i);
        if (browser_tool_hover == i)
            fill_rounded_rect(x, 9, 32, 32, 6, 0xFFD4E4EFu);
        moswm_draw_file_icon_sized(browser_pixels, browser_width,
            browser_height, x + 5, 14, tools[i], 22u);
    }
    int32_t field_x = browser_tool_x(BROWSER_TOOL_COUNT) + 6;
    int32_t field_width = width - field_x - 10;
    if (field_width > 60) {
        fill_rounded_rect(field_x, 10, field_width, 30, 6, 0xFFC9D3DAu);
        fill_rounded_rect(field_x + 1, 11, field_width - 2, 28, 5,
                          0xFFFFFFFFu);
        moswm_draw_file_icon_sized(browser_pixels, browser_width,
            browser_height, field_x + 8, 16, MOSWM_ICON_FOLDER, 18u);
        char shown[BROWSER_PATH_SIZE];
        fit_text_px(shown, sizeof(shown), browser_path, field_width - 46, 1u);
        wm_text(field_x + 34, 21, shown, 0xFF34434Cu, 0, 1u);
    }

    /* Entries. */
    bool show_kind = browser_width >= 430u;
    bool scrollable = browser_entry_count > visible_rows;
    int32_t list_right = width - (scrollable ? 18 : 8);
    for (uint32_t row = 0; row < visible_rows; row++) {
        uint32_t index = browser_scroll + row;
        if (index >= browser_entry_count) break;
        const syscall_dirent_t* entry = &browser_entries[index];
        int32_t y = (int32_t)BROWSER_HEADER_HEIGHT + 4 +
                    (int32_t)row * (int32_t)BROWSER_ROW_HEIGHT;
        if (browser_selected == index) {
            fill_rounded_rect(8, y, list_right - 8,
                              (int32_t)BROWSER_ROW_HEIGHT - 2, 5,
                              0x38000000u | (accent & 0x00FFFFFFu));
            fill_rect(8, y + 5, 3, (int32_t)BROWSER_ROW_HEIGHT - 12, accent);
        } else if (browser_hover == index) {
            fill_rounded_rect(8, y, list_right - 8,
                              (int32_t)BROWSER_ROW_HEIGHT - 2, 5,
                              0xFFE7EDF1u);
        }
        moswm_file_icon_t icon = icon_for_entry(entry->type, entry->name);
        moswm_draw_file_icon_sized(browser_pixels, browser_width,
            browser_height, 18, y + 3, icon, 20u);
        char label[SYSCALL_DIRENT_NAME_SIZE];
        fit_text_px(label, sizeof(label), entry->name,
                    list_right - 56 - (show_kind ? 110 : 0), 1u);
        wm_text(48, y + 9, label, 0xFF273640u, 0, 1u);
        if (show_kind)
            wm_text(list_right - 104, y + 9, kind_for_icon(icon),
                    0xFF7A8A95u, 0, 1u);
    }
    if (!browser_entry_count && visible_rows)
        wm_text(20, (int32_t)BROWSER_HEADER_HEIGHT + 18,
                "This folder is empty", 0xFF7A8A95u, 0, 1u);
    if (scrollable && visible_rows) {
        int32_t track_y = (int32_t)BROWSER_HEADER_HEIGHT + 6;
        int32_t track_height = (int32_t)(visible_rows * BROWSER_ROW_HEIGHT) - 8;
        int32_t thumb_height = track_height * (int32_t)visible_rows /
                               (int32_t)browser_entry_count;
        if (thumb_height < 24) thumb_height = 24;
        int32_t thumb_y = track_y + (track_height - thumb_height) *
            (int32_t)browser_scroll / (int32_t)(max_scroll ? max_scroll : 1u);
        fill_rounded_rect(width - 12, track_y, 6, track_height, 3,
                          0xFFE3E9EDu);
        fill_rounded_rect(width - 12, thumb_y, 6, thumb_height, 3,
                          0xFFA9B8C3u);
    }

    /* Status bar. */
    int32_t footer_y = height - (int32_t)BROWSER_FOOTER_HEIGHT;
    fill_rect(0, footer_y, width, (int32_t)BROWSER_FOOTER_HEIGHT,
              0xFFE9EDF0u);
    fill_rect(0, footer_y, width, 1, 0xFFD2D9DEu);
    if (browser_message[0]) {
        wm_text(12, footer_y + 9, browser_message,
                browser_message_is_error ? 0xFFB43A32u : 0xFF4D5E69u, 0, 1u);
    } else {
        char summary[32];
        format_count(summary, browser_entry_count,
                     browser_entry_count == 1u ? "item" : "items");
        wm_text(12, footer_y + 9, summary, 0xFF667680u, 0, 1u);
    }
    if (clipboard_path[0] && browser_width >= 430u) {
        const char* note = "Copied - right-click to paste";
        wm_text(width - 12 - (int32_t)moswm_text_width(note, 1u),
                footer_y + 9, note, 0xFF7A8A95u, 0, 1u);
    }

    canvas = screen_canvas;
    display.width = screen_width;
    display.height = screen_height;
    if (mos_window_present(browser_window_id, browser_pixels,
            browser_width * sizeof(uint32_t), 0, 0,
            browser_width, browser_height) != SYS_SUCCESS)
        strcpy(status_message, "Unable to update Files");
}

static void queue_open_click(click_action_t action, const char* path) {
    if (!path || strlen(path) >= sizeof(pending_click_path)) return;
    pending_click_action = action;
    memcpy(pending_click_path, path, strlen(path) + 1u);
    pending_click_x = cursor_x;
    pending_click_y = cursor_y;
}

static void open_dialog(dialog_kind_t kind, const char* path,
                        const char* initial_text) {
    if (strlen(path) >= sizeof(dialog_path)) return;
    dialog_kind = kind;
    strcpy(dialog_path, path);
    dialog_length = (uint32_t)strlen(initial_text);
    if (dialog_length >= sizeof(dialog_text))
        dialog_length = sizeof(dialog_text) - 1u;
    memcpy(dialog_text, initial_text, dialog_length);
    dialog_text[dialog_length] = '\0';
    dialog_error[0] = '\0';
}

static uint32_t browser_tool_at(int32_t x, int32_t y) {
    if (y < 9 || y >= 41 || x < 8) return UINT32_MAX;
    uint32_t index = (uint32_t)(x - 8) / 36u;
    if (index >= BROWSER_TOOL_COUNT || x >= browser_tool_x(index) + 32)
        return UINT32_MAX;
    return index;
}

/* Row under a point in Files content coordinates, or UINT32_MAX. */
static uint32_t browser_row_at(int32_t x, int32_t y) {
    (void)x;
    int32_t top = (int32_t)BROWSER_HEADER_HEIGHT + 4;
    if (y < top) return UINT32_MAX;
    uint32_t row = (uint32_t)(y - top) / BROWSER_ROW_HEIGHT;
    uint32_t index = browser_scroll + row;
    if (row >= browser_visible_rows(browser_height) ||
        index >= browser_entry_count)
        return UINT32_MAX;
    return index;
}

static void browser_mouse_down(int32_t x, int32_t y,
                               const syscall_window_info_t* info) {
    (void)info;
    if (y < (int32_t)BROWSER_HEADER_HEIGHT) {
        last_click_valid = false;
        uint32_t tool = browser_tool_at(x, y);
        if (tool == 0u) browser_open(MOSDESK_USERS_PATH);
        else if (tool == 1u) browser_open("0:/");
        else if (tool == 2u) browser_go_up();
        else if (tool == 3u) browser_refresh_directory();
        else if (tool == 4u)
            open_dialog(DIALOG_NEW_FOLDER, browser_path, "New folder");
        return;
    }
    uint32_t index = browser_row_at(x, y);
    if (index == UINT32_MAX) {
        if (browser_selected != UINT32_MAX) browser_generation++;
        browser_selected = UINT32_MAX;
        last_click_valid = false;
        return;
    }
    browser_selected = index;
    browser_generation++;
    char entry_path[BROWSER_PATH_SIZE];
    if (!path_join(entry_path, sizeof(entry_path), browser_path,
                   browser_entries[index].name)) {
        browser_note("Folder path is too long", true);
        return;
    }
    queue_open_click(CLICK_ACTION_BROWSER_ENTRY, entry_path);
}

static void open_path(click_action_t action, const char* path) {
    if (action == CLICK_ACTION_HOME) {
        browser_open(MOSDESK_USERS_PATH);
    } else if (action == CLICK_ACTION_SYSTEM) {
        browser_open("0:/");
    } else if (action == CLICK_ACTION_APPLICATION) {
        if ((int64_t)mos_syscall3(SYS_EXEC,
                (uint64_t)(uintptr_t)path, 0, 0) < 0)
            strcpy(status_message, "Unable to open desktop application");
    } else if (action == CLICK_ACTION_BROWSER_ENTRY) {
        if (fs_is_directory(path)) {
            browser_open(path);
            return;
        }
        const char* name = path_name(path);
        if (name_has_extension(name, ".qoi")) {
            set_wallpaper_image(path);
            return;
        }
        bool runnable = is_run_name(name);
        for (uint32_t i = 0; i < browser_entry_count && !runnable; i++) {
            char entry_path[BROWSER_PATH_SIZE];
            runnable = browser_entries[i].type ==
                           SYSCALL_DIRENT_TYPE_EXECUTABLE &&
                path_join(entry_path, sizeof(entry_path), browser_path,
                          browser_entries[i].name) &&
                !strcmp(entry_path, path);
        }
        if (!runnable)
            browser_note("Nothing is set up to open this file", false);
        else if ((int64_t)mos_syscall3(SYS_EXEC,
                     (uint64_t)(uintptr_t)path, 0, 0) < 0)
            browser_note("Unable to run this item", true);
    }
}

static void activate_pending_click(uint32_t time_ms) {
    if (pending_click_action == CLICK_ACTION_NONE) return;
    click_action_t action = pending_click_action;
    char path[BROWSER_PATH_SIZE];
    memcpy(path, pending_click_path, strlen(pending_click_path) + 1u);
    pending_click_action = CLICK_ACTION_NONE;
    bool activate = desktop_settings.click_once_open != 0u;
    if (!activate && last_click_valid &&
        last_click_action == action && !strcmp(last_click_path, path) &&
        (uint32_t)(time_ms - last_click_time) <= 500u) {
        activate = true;
        last_click_valid = false;
    } else {
        last_click_action = action;
        memcpy(last_click_path, path, strlen(path) + 1u);
        last_click_time = time_ms;
        last_click_valid = true;
    }
    if (activate) open_path(action, path);
}

static uint32_t isqrt_u64(uint64_t value) {
    uint64_t result = 0, bit = 1ull << 62;
    while (bit > value) bit >>= 2;
    while (bit) {
        if (value >= result + bit) {
            value -= result + bit;
            result = (result >> 1) + bit;
        } else {
            result >>= 1;
        }
        bit >>= 2;
    }
    return (uint32_t)result;
}

/* Each pixel is painted exactly once, so translucent colours stay even. */
static void fill_rounded_rect(int32_t x, int32_t y, int32_t width,
                              int32_t height, uint32_t radius,
                              uint32_t color) {
    if (width <= 0 || height <= 0) return;
    if (radius * 2u > (uint32_t)width) radius = (uint32_t)width / 2u;
    if (radius * 2u > (uint32_t)height) radius = (uint32_t)height / 2u;
    if (!radius) {
        fill_rect(x, y, width, height, color);
        return;
    }
    int32_t r = (int32_t)radius;
    for (int32_t row = 0; row < height; row++) {
        int32_t inset = 0;
        int32_t from_edge = row < r ? row : (row >= height - r ? height - 1 - row : r);
        if (from_edge < r) {
            int64_t dy2 = 2 * (int64_t)r - 2 * (int64_t)from_edge - 1;
            int64_t span2 = 4 * (int64_t)r * r - dy2 * dy2;
            inset = r - (int32_t)((isqrt_u64((uint64_t)span2) + 1u) / 2u);
            if (inset < 0) inset = 0;
        }
        fill_rect(x + inset, y + row, width - inset * 2, 1, color);
    }
}

static uint32_t* wallpaper;

static uint32_t lerp_channel(uint32_t from, uint32_t to, uint32_t t) {
    return (from * (255u - t) + to * t) / 255u;
}

static void paint_image_wallpaper(uint32_t* target) {
    uint32_t screen_width = display.width;
    uint32_t screen_height = display.height;
    uint32_t rotation = wallpaper_rotation % 4u;
    uint32_t rotated_width = rotation & 1u
        ? wallpaper_source_height : wallpaper_source_width;
    uint32_t rotated_height = rotation & 1u
        ? wallpaper_source_width : wallpaper_source_height;
    int32_t output_width = (int32_t)screen_width;
    int32_t output_height = (int32_t)screen_height;
    if (wallpaper_mode != MOSDESK_WALLPAPER_STRETCH) {
        bool screen_narrower = (uint64_t)screen_width * rotated_height <=
                               (uint64_t)screen_height * rotated_width;
        bool use_width = wallpaper_mode == MOSDESK_WALLPAPER_FILL
            ? screen_narrower : !screen_narrower;
        if (use_width) {
            output_width = (int32_t)screen_width;
            output_height = (int32_t)((uint64_t)screen_width *
                rotated_height / rotated_width);
        } else {
            output_height = (int32_t)screen_height;
            output_width = (int32_t)((uint64_t)screen_height *
                rotated_width / rotated_height);
        }
    }
    int32_t offset_x = ((int32_t)screen_width - output_width) / 2;
    int32_t offset_y = ((int32_t)screen_height - output_height) / 2;
    size_t screen_pixels = (size_t)screen_width * screen_height;
    for (size_t i = 0; i < screen_pixels; i++) target[i] = 0xFF11191Eu;
    for (int32_t y = 0; y < (int32_t)screen_height; y++) {
        int32_t mapped_y = y - offset_y;
        if (mapped_y < 0 || mapped_y >= output_height) continue;
        uint32_t rotated_y = (uint32_t)((uint64_t)mapped_y *
                                        rotated_height / output_height);
        for (int32_t x = 0; x < (int32_t)screen_width; x++) {
            int32_t mapped_x = x - offset_x;
            if (mapped_x < 0 || mapped_x >= output_width) continue;
            uint32_t rotated_x = (uint32_t)((uint64_t)mapped_x *
                                            rotated_width / output_width);
            uint32_t source_x, source_y;
            if (rotation == 1u) {
                source_x = rotated_y;
                source_y = wallpaper_source_height - 1u - rotated_x;
            } else if (rotation == 2u) {
                source_x = wallpaper_source_width - 1u - rotated_x;
                source_y = wallpaper_source_height - 1u - rotated_y;
            } else if (rotation == 3u) {
                source_x = wallpaper_source_width - 1u - rotated_y;
                source_y = rotated_x;
            } else {
                source_x = rotated_x;
                source_y = rotated_y;
            }
            target[(size_t)y * screen_width + (uint32_t)x] =
                wallpaper_source[(size_t)source_y * wallpaper_source_width +
                                 source_x];
        }
    }
}

/* Anti-aliased thick line with round caps, written as max-coverage into an
 * 8-bit mask so crossing strokes never double-darken where they meet.
 * Coordinates are in mask space; half16 is half the stroke width in 1/16 px. */
static void mask_thick_line(uint8_t* mask, int32_t mw, int32_t mh,
                            int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                            int32_t half16) {
    int64_t dx = x1 - x0, dy = y1 - y0;
    int64_t len2 = dx * dx + dy * dy;
    if (!len2) return;
    int32_t pad = half16 / 16 + 2;
    int32_t min_x = (x0 < x1 ? x0 : x1) - pad, max_x = (x0 > x1 ? x0 : x1) + pad;
    int32_t min_y = (y0 < y1 ? y0 : y1) - pad, max_y = (y0 > y1 ? y0 : y1) + pad;
    if (min_x < 0) min_x = 0;
    if (min_y < 0) min_y = 0;
    if (max_x >= mw) max_x = mw - 1;
    if (max_y >= mh) max_y = mh - 1;
    int64_t reach = (int64_t)(half16 / 16 + 2);
    for (int32_t y = min_y; y <= max_y; y++) {
        for (int32_t x = min_x; x <= max_x; x++) {
            int64_t px = x - x0, py = y - y0;
            int64_t t = px * dx + py * dy, d2;
            if (t <= 0) {
                d2 = px * px + py * py;
            } else if (t >= len2) {
                int64_t qx = x - x1, qy = y - y1;
                d2 = qx * qx + qy * qy;
            } else {
                int64_t cross = px * dy - py * dx;
                d2 = cross * cross / len2;
            }
            if (d2 > reach * reach) continue;
            int32_t distance16 = (int32_t)isqrt_u64((uint64_t)d2 * 256u);
            int32_t coverage = half16 + 8 - distance16;
            if (coverage <= 0) continue;
            if (coverage > 16) coverage = 16;
            uint8_t value = (uint8_t)(coverage * 255 / 16);
            uint8_t* cell = mask + (size_t)y * (uint32_t)mw + (uint32_t)x;
            if (value > *cell) *cell = value;
        }
    }
}

/* The Minimal-OS hourglass: top bar, bottom bar, one long diagonal, and two
 * short diagonals that stop just before the centre. Units follow the
 * 370x353 logo artwork so every stroke meets its neighbours exactly. */
static void draw_logo_mask(uint8_t* mask, int32_t mw, int32_t mh,
                           int32_t pad, int32_t scale_num, int32_t scale_den,
                           int32_t half16) {
#define LX(v) (pad + (v) * scale_num / scale_den)
    mask_thick_line(mask, mw, mh, LX(0), LX(0), LX(370), LX(0), half16);
    mask_thick_line(mask, mw, mh, LX(0), LX(353), LX(370), LX(353), half16);
    mask_thick_line(mask, mw, mh, LX(370), LX(0), LX(0), LX(353), half16);
    mask_thick_line(mask, mw, mh, LX(0), LX(0), LX(167), LX(155), half16);
    mask_thick_line(mask, mw, mh, LX(370), LX(353), LX(203), LX(197), half16);
#undef LX
}

static void paint_wallpaper(uint32_t* target) {
    uint32_t* saved = canvas;
    canvas = target;
    uint32_t w = display.width, h = display.height;
    if (wallpaper_source) {
        paint_image_wallpaper(target);
        canvas = saved;
        return;
    }
    static const uint8_t colors[6][6] = {
        {9u, 20u, 38u, 31u, 104u, 158u},
        {7u, 27u, 31u, 20u, 111u, 105u},
        {35u, 19u, 12u, 139u, 79u, 39u},
        {12u, 31u, 20u, 83u, 132u, 76u},
        {37u, 17u, 25u, 166u, 72u, 104u},
        {39u, 30u, 12u, 155u, 119u, 47u}
    };
    const uint8_t* palette = colors[wallpaper_style <= 5u
        ? wallpaper_style : 0u];

    /* Diagonal glass gradients with Plasma-style angled light bands. */
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            uint32_t t = (x * 255u / w + y * 255u / h) / 2u;
            uint32_t red, green, blue;
            if (t < 140u) {
                uint32_t k = t * 255u / 140u;
                red = lerp_channel(palette[0], palette[3] * 3u / 5u, k);
                green = lerp_channel(palette[1], palette[4] / 2u, k);
                blue = lerp_channel(palette[2], palette[5] / 2u, k);
            } else {
                uint32_t k = (t - 140u) * 255u / 115u;
                red = lerp_channel(palette[3] * 3u / 5u, palette[3], k);
                green = lerp_channel(palette[4] / 2u, palette[4], k);
                blue = lerp_channel(palette[5] / 2u, palette[5], k);
            }
            uint32_t nx = x * 1000u / w, ny = y * 1000u / h;
            uint32_t band = 2u * nx + ny;
            uint32_t lift = 0;
            if (band > 1350u && band < 2150u && ny > 140u) lift += 7;
            if (band > 1800u && band < 2600u) lift += 6;
            if (band > 2250u && band < 2750u && ny < 900u) lift += 5;
            red += lift; green += lift * 2u; blue += lift * 2u;
            if (red > 255u) red = 255u;
            if (green > 255u) green = 255u;
            if (blue > 255u) blue = 255u;
            target[(size_t)y * w + x] =
                0xFF000000u | red << 16 | green << 8 | blue;
        }
    }

    /* Hourglass logo, right of centre, built from one connected stroke set. */
    int32_t logo_h = (int32_t)(h * 46u / 100u);
    int32_t scale_num = logo_h, scale_den = 353;
    int32_t logo_w = 370 * scale_num / scale_den;
    int32_t pad = 12;
    int32_t mw = logo_w + pad * 2, mh = logo_h + pad * 2;
    int32_t half16 = (int32_t)(14 * scale_num * 8 / scale_den);
    if (half16 < 24) half16 = 24;
    uint8_t* mask = wm_malloc((size_t)mw * (size_t)mh);
    if (mask) {
        memset(mask, 0, (size_t)mw * (size_t)mh);
        draw_logo_mask(mask, mw, mh, pad, scale_num, scale_den, half16);
        int32_t ox = (int32_t)(w * 74u / 100u) - mw / 2;
        int32_t oy = (int32_t)(h * 47u / 100u) - mh / 2;
        for (int32_t y = 0; y < mh; y++) {
            for (int32_t x = 0; x < mw; x++) {
                uint32_t cover = mask[(size_t)y * (uint32_t)mw + (uint32_t)x];
                if (!cover) continue;
                put_pixel(ox + x, oy + y,
                          ((cover * 0x34u / 255u) << 24) | 0x00DCEBF7u);
            }
        }
        wm_free(mask);
    }
    canvas = saved;
}

static void draw_desktop_icons(void);

static void ensure_wallpaper(void) {
    if (!wallpaper || painted_wallpaper_style == wallpaper_style) return;
    paint_wallpaper(wallpaper);
    painted_wallpaper_style = wallpaper_style;
    wallpaper_generation++;
}

static void copy_wallpaper(uint32_t* target) {
    if (wallpaper)
        copy_pixels(target, wallpaper,
                    (size_t)display.width * display.height);
    else
        paint_wallpaper(target);
}

static uint64_t desktop_signature(void) {
    uint64_t hash = 0xCBF29CE484222325ull;
    hash = hash_u64(hash, wallpaper_generation);
    hash = hash_u64(hash, desktop_apps_generation);
    hash = hash_u64(hash, ((uint64_t)display.width << 32) | display.height);
    hash = hash_u64(hash, hover_desktop_icon);
    hash = hash_bytes(hash, desktop_settings.desktop_icon_x,
                      sizeof(desktop_settings.desktop_icon_x));
    hash = hash_bytes(hash, desktop_settings.desktop_icon_y,
                      sizeof(desktop_settings.desktop_icon_y));
    return hash;
}

static void draw_desktop(uint32_t* target) {
    uint32_t* saved = canvas;
    copy_wallpaper(target);
    canvas = target;
    draw_desktop_icons();
    canvas = saved;
}

/* Wallpaper and icons into the canvas, repainting them only on change. */
static void compose_desktop(void) {
    if (!desktop_layer) {
        draw_desktop(canvas);
        return;
    }
    uint64_t signature = desktop_signature();
    if (signature != desktop_layer_signature) {
        draw_desktop(desktop_layer);
        desktop_layer_signature = signature;
    }
    copy_pixels(canvas, desktop_layer,
                (size_t)display.width * display.height);
}

static void draw_custom_icon(const uint32_t* icon, int32_t x, int32_t y,
                             uint32_t size) {
    if (!icon || !size) return;
    for (uint32_t dy = 0; dy < size; dy++) {
        uint32_t source_y = dy * DESKTOP_ICON_SIZE / size;
        for (uint32_t dx = 0; dx < size; dx++) {
            uint32_t source_x = dx * DESKTOP_ICON_SIZE / size;
            uint32_t color = icon[source_y * DESKTOP_ICON_SIZE + source_x];
            if (color >> 24u)
                put_pixel(x + (int32_t)dx, y + (int32_t)dy, color);
        }
    }
}

static void desktop_icon_position(uint32_t index, int32_t* x, int32_t* y) {
    if (index >= MOSDESK_DESKTOP_ICON_COUNT) return;
    if (desktop_settings.desktop_icon_x[index] != UINT32_MAX &&
        desktop_settings.desktop_icon_y[index] != UINT32_MAX) {
        uint32_t max_x = display.width > 84u ? display.width - 84u : 0u;
        uint32_t work_height = display.height > TASKBAR_HEIGHT
            ? display.height - TASKBAR_HEIGHT : 0u;
        uint32_t max_y = work_height > 90u ? work_height - 90u : 0u;
        uint32_t saved_x = desktop_settings.desktop_icon_x[index];
        uint32_t saved_y = desktop_settings.desktop_icon_y[index];
        *x = (int32_t)(saved_x > max_x ? max_x : saved_x);
        *y = (int32_t)(saved_y > max_y ? max_y : saved_y);
        return;
    }
    if (index < 2u) {
        *x = 24;
        *y = 14 + (int32_t)index * 98;
        return;
    }
    index -= 2u;
    uint32_t columns = display.width > 112u
        ? (display.width - 112u) / 88u : 1u;
    uint32_t rows = display.height > TASKBAR_HEIGHT + 24u
        ? (display.height - TASKBAR_HEIGHT - 24u) / 98u : 1u;
    if (!columns) columns = 1u;
    if (!rows) rows = 1u;
    uint32_t column = index / rows;
    uint32_t row = index % rows;
    *x = 112 + (int32_t)column * 88;
    *y = 14 + (int32_t)row * 98;
}

/* Index of the desktop icon under a point (0 Home, 1 System, then apps). */
static uint32_t desktop_icon_at(int32_t px, int32_t py) {
    for (uint32_t i = 0; i < desktop_app_count + 2u; i++) {
        int32_t x, y;
        desktop_icon_position(i, &x, &y);
        if (px >= x - 10 && px < x + 74 && py >= y - 4 && py < y + 90)
            return i;
    }
    return UINT32_MAX;
}

static click_action_t desktop_shortcut_at(int32_t px, int32_t py,
                                          uint32_t* app_index) {
    uint32_t index = desktop_icon_at(px, py);
    if (index == UINT32_MAX) return CLICK_ACTION_NONE;
    if (index == 0u) return CLICK_ACTION_HOME;
    if (index == 1u) return CLICK_ACTION_SYSTEM;
    if (app_index) *app_index = index - 2u;
    return CLICK_ACTION_APPLICATION;
}

static void draw_desktop_icons(void) {
    for (uint32_t i = 0; i < desktop_app_count + 2u; i++) {
        int32_t x, y;
        desktop_icon_position(i, &x, &y);
        if (hover_desktop_icon == i) {
            fill_rounded_rect(x - 10, y - 4, 84, 94, 8, 0x30FFFFFFu);
            fill_rounded_rect(x - 9, y - 3, 82, 92, 7, 0x18000000u);
        }
        const char* name = i == 0u ? "Home" : i == 1u ? "System"
            : desktop_apps[i - 2u].name;
        if (i < 2u) {
            moswm_draw_file_icon_sized(canvas, display.width, display.height,
                x, y, i == 0u ? MOSWM_ICON_HOME : MOSWM_ICON_COMPUTER, 64u);
        } else if (desktop_apps[i - 2u].has_icon) {
            draw_custom_icon(desktop_apps[i - 2u].icon_pixels, x + 8, y + 6,
                             48u);
        } else {
            moswm_draw_file_icon_sized(canvas, display.width, display.height,
                x + 4, y + 2, MOSWM_ICON_APP, 56u);
        }
        char label[DESKTOP_APP_NAME_SIZE];
        fit_text_px(label, sizeof(label), name, 86, 2u);
        int32_t label_x = x + 32 - (int32_t)moswm_text_width(label, 2u) / 2;
        if (label_x < 2) label_x = 2;
        wm_text(label_x, y + 68, label, 0xFFF2F6F8u, 0x99000000u, 2u);
    }
}

static cached_window_t* cache_for(uint32_t id) {
    for (uint32_t i = 0; i < MAX_WINDOWS; i++) {
        if (cache[i].id == id) return &cache[i];
    }
    for (uint32_t i = 0; i < MAX_WINDOWS; i++) {
        if (!cache[i].id) {
            cache[i].id = id;
            return &cache[i];
        }
    }
    return NULL;
}

static void remove_missing_cache(const syscall_window_info_t* windows,
                                 uint64_t count) {
    for (uint32_t i = 0; i < MAX_WINDOWS; i++) {
        cached_window_t* entry = &cache[i];
        if (!entry->id) continue;
        bool found = false;
        for (uint64_t j = 0; j < count; j++) {
            if (windows[j].id == entry->id) {
                found = true;
                break;
            }
        }
        if (!found) {
            wm_free(entry->pixels);
            memset(entry, 0, sizeof(*entry));
        }
    }
}

static bool refresh_frame(cached_window_t* entry,
                          const syscall_window_info_t* info) {
    if (entry->width != info->width || entry->height != info->height) {
        if (info->width > SIZE_MAX / info->height / sizeof(uint32_t))
            return false;
        size_t bytes = (size_t)info->width * info->height * sizeof(uint32_t);
        uint32_t* pixels = wm_malloc(bytes);
        if (!pixels) return false;
        memset(pixels, 0, bytes);
        wm_free(entry->pixels);
        entry->pixels = pixels;
        entry->width = info->width;
        entry->height = info->height;
        entry->frame_version = UINT32_MAX;
    }
    if (entry->frame_version == info->frame_version) return true;
    syscall_window_frame_t frame = {
        .width = info->width,
        .height = info->height,
        .pitch = info->width * sizeof(uint32_t),
        .pixels = entry->pixels
    };
    if (mos_window_get_frame(info->id, &frame) != SYS_SUCCESS) return false;
    entry->frame_version = info->frame_version;
    return true;
}

static void refresh_window_icon(cached_window_t* entry, bool force) {
    if (!entry || (entry->icon_checked && !force)) return;
    uint64_t result = mos_window_get_icon(entry->id, entry->icon_pixels);
    if (result == SYS_SUCCESS) {
        entry->has_icon = true;
        entry->icon_sum = 0;
        for (uint32_t i = 0; i < SYSCALL_WINDOW_ICON_PIXELS; i++)
            entry->icon_sum = entry->icon_sum * 31u + entry->icon_pixels[i];
    } else {
        /* No icon set, or a kernel without window icons: use the default. */
        entry->has_icon = false;
    }
    entry->icon_checked = true;
}

static uint32_t window_mode(const syscall_window_info_t* info) {
    if (info->flags & SYSCALL_WINDOW_MINIMIZED) return MODE_MINIMIZED;
    if (info->flags & SYSCALL_WINDOW_FULLSCREEN) return MODE_FULLSCREEN;
    if (info->flags & SYSCALL_WINDOW_MAXIMIZED) return MODE_MAXIMIZED;
    return MODE_NORMAL;
}

static void save_bounds(cached_window_t* entry,
                        const syscall_window_info_t* info) {
    entry->saved_x = info->x;
    entry->saved_y = info->y;
    entry->saved_width = info->width;
    entry->saved_height = info->height;
}

static uint32_t mode_event(uint32_t mode) {
    if (mode == MODE_MINIMIZED) return SYSCALL_WINDOW_EVENT_MINIMIZED;
    if (mode == MODE_MAXIMIZED) return SYSCALL_WINDOW_EVENT_MAXIMIZED;
    if (mode == MODE_NORMAL) return SYSCALL_WINDOW_EVENT_RESTORED;
    return 0;
}

static void post_simple_event(uint32_t id, uint32_t type) {
    syscall_window_event_t event = {.type = type};
    uint64_t result = mos_window_post_event(id, &event);
    if (result == SYS_ERR_BUSY)
        strcpy(status_message, "Window event queue is full");
}

static bool set_window_mode(const syscall_window_info_t* info,
                            cached_window_t* entry, uint32_t mode) {
    if (!info || !entry) return false;
    uint32_t previous_mode = entry->last_mode;
    uint32_t flags = info->flags & ~(SYSCALL_WINDOW_MINIMIZED |
                                     SYSCALL_WINDOW_MAXIMIZED |
                                     SYSCALL_WINDOW_FULLSCREEN);
    if (mode == MODE_MINIMIZED) flags |= SYSCALL_WINDOW_MINIMIZED;
    else if (mode == MODE_MAXIMIZED) flags |= SYSCALL_WINDOW_MAXIMIZED;
    else if (mode == MODE_FULLSCREEN) flags |= SYSCALL_WINDOW_FULLSCREEN;

    if (mode != MODE_NORMAL && previous_mode == MODE_NORMAL)
        save_bounds(entry, info);
    if (mos_window_set_flags(info->id, flags) != SYS_SUCCESS) return false;
    entry->last_mode = mode;

    if (mode == MODE_MAXIMIZED || mode == MODE_FULLSCREEN) {
        uint32_t work_height = mode == MODE_FULLSCREEN
            ? display.height : display.height - TASKBAR_HEIGHT;
        if (display.width <= 2u || work_height <= TITLE_HEIGHT + 2u)
            return false;
        return mos_window_set_bounds(info->id, 0, 0, display.width - 2u,
                                     work_height - TITLE_HEIGHT - 2u) ==
               SYS_SUCCESS;
    }
    if (mode == MODE_NORMAL && previous_mode != MODE_NORMAL &&
        entry->saved_width && entry->saved_height) {
        bool restored = mos_window_set_bounds(info->id, entry->saved_x,
            entry->saved_y, entry->saved_width, entry->saved_height) ==
            SYS_SUCCESS;
        entry->saved_width = 0;
        entry->saved_height = 0;
        return restored;
    }
    return true;
}

static void sync_window_modes(syscall_window_info_t* windows, uint64_t count) {
    for (uint64_t i = 0; i < count; i++) {
        syscall_window_info_t* info = &windows[i];
        cached_window_t* entry = cache_for(info->id);
        if (!entry) continue;
        uint32_t requested = window_mode(info);
        if (requested == entry->last_mode) continue;
        uint32_t event = mode_event(requested);
        if (!set_window_mode(info, entry, requested)) {
            strcpy(status_message, "Window state update failed");
            continue;
        }
        if (event) post_simple_event(info->id, event);
    }
}

static void draw_chrome_text(const syscall_window_info_t* info,
                             const cached_window_t* entry);

static void composite_window(const syscall_window_info_t* info,
                             cached_window_t* entry) {
    int32_t x = (int32_t)info->x;
    int32_t y = (int32_t)info->y;
    int32_t outer_width = (int32_t)info->width + 2;
    int32_t outer_height = (int32_t)info->height + (int32_t)TITLE_HEIGHT + 2;
    if (x >= (int32_t)display.width || y >= (int32_t)display.height ||
        x + outer_width <= 0 || y + outer_height <= 0)
        return;

    bool window_focused = info->id == focused_id;
    fill_rounded_rect(x - 4, y - 1, outer_width + 8, outer_height + 10,
                      CORNER_RADIUS + 4u, 0x16000000u);
    fill_rounded_rect(x - 1, y + 2, outer_width + 2, outer_height + 4,
                      CORNER_RADIUS + 1u, window_focused ? 0x40000000u
                                                         : 0x24000000u);
    fill_rounded_rect(x, y, outer_width, outer_height,
                      CORNER_RADIUS, info->style.border);
    fill_rounded_rect(x + 1, y + 1, outer_width - 2, outer_height - 2,
                      CORNER_RADIUS - 1u, info->style.content_background);
    fill_rect(x + 1, y + (int32_t)TITLE_HEIGHT, outer_width - 2, 2,
              info->style.title_bar);
    fill_rounded_rect(x + 1, y + 1, outer_width - 2,
                      (int32_t)TITLE_HEIGHT, CORNER_RADIUS - 1u,
                      info->style.title_bar);
    fill_rect(x + 1,
              y + 1 + (int32_t)TITLE_HEIGHT -
                  (int32_t)(CORNER_RADIUS - 1u),
              outer_width - 2, (int32_t)(CORNER_RADIUS - 1u),
              info->style.title_bar);
    fill_rect(x + 1, y + (int32_t)TITLE_HEIGHT - 1, outer_width - 2, 1,
              0x30000000u);
    fill_rect(x + (int32_t)CORNER_RADIUS, y + 1,
              outer_width - (int32_t)CORNER_RADIUS * 2, 1, 0x1CFFFFFFu);

    if (entry && entry->pixels) {
        int32_t content_x = x + 1;
        int32_t content_y = y + (int32_t)TITLE_HEIGHT + 1;
        int32_t first = content_x < 0 ? -content_x : 0;
        int32_t last = (int32_t)info->width;
        if (content_x + last > (int32_t)display.width)
            last = (int32_t)display.width - content_x;
        for (uint32_t py = 0; py < info->height; py++) {
            int32_t dy = content_y + (int32_t)py;
            if (dy < 0 || (uint32_t)dy >= display.height) continue;
            const uint32_t* source =
                entry->pixels + (size_t)py * info->width;
            uint32_t* target = canvas + (size_t)dy * display.width +
                               content_x;
            if (py + CORNER_RADIUS < info->height) {
                /* Straight rows: no per-pixel bounds or corner tests. */
                for (int32_t px = first; px < last; px++) {
                    uint32_t color = source[px];
                    target[px] = color >> 24 == 255u
                        ? color : alpha_blend(color, target[px]);
                }
                continue;
            }
            for (int32_t px = first; px < last; px++) {
                if ((uint32_t)px < CORNER_RADIUS ||
                    (uint32_t)px + CORNER_RADIUS >= info->width) {
                    int32_t corner_x = (uint32_t)px < CORNER_RADIUS
                        ? (int32_t)CORNER_RADIUS - 1
                        : (int32_t)info->width - (int32_t)CORNER_RADIUS;
                    int32_t corner_y =
                        (int32_t)info->height - (int32_t)CORNER_RADIUS;
                    int32_t dxr = px - corner_x;
                    int32_t dyr = (int32_t)py - corner_y;
                    if (dxr * dxr + dyr * dyr >
                        (int32_t)(CORNER_RADIUS * CORNER_RADIUS))
                        continue;
                }
                target[px] = alpha_blend(source[px], target[px]);
            }
        }
    }
    draw_chrome_text(info, entry);
}

static uint32_t opaque(uint32_t color) {
    return 0xFF000000u | (color & 0x00FFFFFFu);
}

static void draw_chrome_text(const syscall_window_info_t* info,
                             const cached_window_t* entry) {
    int32_t x = (int32_t)info->x;
    int32_t y = (int32_t)info->y;
    int32_t outer_width = (int32_t)info->width + 2;
    int32_t right = x + (int32_t)info->width + 1;
    int32_t by = y + 5;
    int32_t close_x = right - 24;
    int32_t max_x = right - 48;
    int32_t min_x = right - 72;
    bool focused = info->id == focused_id;

    /* Breeze-style centred title, dimmed when the window is not focused. */
    char title[SYSCALL_WINDOW_MAX_TITLE];
    int32_t title_left = x + (entry && entry->has_icon ? 32 : 12);
    int32_t title_right = min_x - 10;
    int32_t room = title_right - title_left;
    fit_text_px(title, sizeof(title), info->title, room, 2);
    int32_t text_width = (int32_t)moswm_text_width(title, 2);
    int32_t text_x = title_left + (room - text_width) / 2;
    if (entry && entry->has_icon)
        draw_custom_icon(entry->icon_pixels, x + 7, y + 4, 20u);
    uint32_t title_color = (info->style.title_text & 0x00FFFFFFu) |
                           (focused ? 0xFF000000u : 0xA0000000u);
    wm_text(text_x, y + ((int32_t)TITLE_HEIGHT - 16) / 2, title,
            title_color, 0, 2);

    /* Round window buttons with crisp glyphs. */
    fill_rounded_rect(close_x, by, 18, 18, 9, opaque(info->style.close_button));
    draw_wallpaper_line(close_x + 6, by + 6, close_x + 11, by + 11,
                        opaque(info->style.close_icon));
    draw_wallpaper_line(close_x + 7, by + 6, close_x + 12, by + 11,
                        opaque(info->style.close_icon));
    draw_wallpaper_line(close_x + 11, by + 6, close_x + 6, by + 11,
                        opaque(info->style.close_icon));
    draw_wallpaper_line(close_x + 12, by + 6, close_x + 7, by + 11,
                        opaque(info->style.close_icon));
    fill_rounded_rect(max_x, by, 18, 18, 9,
                      opaque(info->style.maximize_button));
    stroke_rect(max_x + 5, by + 5, 8, 8, opaque(info->style.maximize_icon));
    fill_rect(max_x + 5, by + 5, 8, 2, opaque(info->style.maximize_icon));
    fill_rounded_rect(min_x, by, 18, 18, 9,
                      opaque(info->style.minimize_button));
    fill_rect(min_x + 5, by + 11, 8, 2, opaque(info->style.minimize_icon));
}

static void prepare_cursor_pixels(void) {
    if (cursor_width && cursor_height) return;
    static const char* fallback[] = {
        "#...........", "##..........", "#x#.........",
        "#xx#........", "#xxx#.......", "#xxxx#......",
        "#xxxxx#.....", "#xxxxxx#....", "#xxxxxxx#...",
        "#xxxxxxxx#..", "#xxxxx###...", "#xx#xx#......"
    };
    cursor_width = 12;
    cursor_height = 12;
    memset(cursor_pixels, 0, sizeof(cursor_pixels));
    for (uint32_t y = 0; y < cursor_height; y++) {
        for (uint32_t x = 0; x < cursor_width; x++) {
            char pixel = fallback[y][x];
            if (pixel == '#')
                cursor_pixels[y * CURSOR_MAX_SIZE + x] = 0xFF10191Du;
            else if (pixel == 'x')
                cursor_pixels[y * CURSOR_MAX_SIZE + x] = 0xFFFFFFFFu;
        }
    }
}

static bool blit_canvas_rect(int32_t x, int32_t y, int32_t width,
                             int32_t height) {
    return mos_graphics_blit_surface(canvas, display.width, display.height,
        display.width * sizeof(uint32_t), x, y, width, height, x, y,
        SYS_GRAPHICS_BLIT_OPAQUE) == SYS_SUCCESS;
}

/* Sends the finished scene to the screen with the pointer on top. `full`
 * pushes the whole canvas; otherwise only the pixels the pointer left and
 * the ones it now covers are updated, and nothing at all if it is still. */
static void present_frame(bool full) {
    prepare_cursor_pixels();
    uint32_t scale = cursor_scale ? cursor_scale : 1u;
    uint32_t frame = 0;
    if (cursor_animation_pixels) {
        cursor_animation_elapsed_ms = mos_syscall1(SYS_UPTIME, 0);
        frame = (uint32_t)((cursor_animation_elapsed_ms /
                            cursor_animation_frame_delay_ms) %
                           cursor_animation_frame_count);
    }
    const uint32_t* pixels = cursor_animation_pixels
        ? cursor_animation_pixels +
              (size_t)frame * CURSOR_MAX_SIZE * CURSOR_MAX_SIZE
        : cursor_pixels;
    int32_t origin_x = cursor_x - (int32_t)(cursor_hotspot_x * scale);
    int32_t origin_y = cursor_y - (int32_t)(cursor_hotspot_y * scale);

    uint64_t signature = 0xCBF29CE484222325ull;
    signature = hash_u64(signature, ((uint64_t)(uint32_t)origin_x << 32) |
                                    (uint32_t)origin_y);
    signature = hash_u64(signature, ((uint64_t)cursor_width << 32) |
                                    cursor_height);
    signature = hash_u64(signature, ((uint64_t)cached_cursor_status << 32) |
                                    frame);
    signature = hash_u64(signature, ((uint64_t)cursor_fill_color << 32) |
                                    scale);
    signature = hash_text(signature, cached_cursor_pack);
    if (!full && cursor_drawn && signature == cursor_drawn_signature) return;

    bool ok = true;
    if (full) {
        ok = blit_canvas_rect(0, 0, (int32_t)display.width,
                              (int32_t)display.height);
    } else if (cursor_drawn && cursor_drawn_width > 0 &&
               cursor_drawn_height > 0) {
        ok = blit_canvas_rect(cursor_drawn_x, cursor_drawn_y,
                              cursor_drawn_width, cursor_drawn_height);
    }

    int32_t left = origin_x < 0 ? 0 : origin_x;
    int32_t top = origin_y < 0 ? 0 : origin_y;
    int32_t right = origin_x + (int32_t)(cursor_width * scale);
    int32_t bottom = origin_y + (int32_t)(cursor_height * scale);
    if (right > (int32_t)display.width) right = (int32_t)display.width;
    if (bottom > (int32_t)display.height) bottom = (int32_t)display.height;
    int32_t width = right - left;
    int32_t height = bottom - top;
    if (width > (int32_t)CURSOR_PATCH_SIZE) width = (int32_t)CURSOR_PATCH_SIZE;
    if (height > (int32_t)CURSOR_PATCH_SIZE)
        height = (int32_t)CURSOR_PATCH_SIZE;
    if (width > 0 && height > 0) {
        for (int32_t row = 0; row < height; row++)
            copy_pixels(cursor_patch + (size_t)row * CURSOR_PATCH_SIZE,
                canvas + (size_t)(top + row) * display.width + left,
                (size_t)width);
        for (uint32_t y = 0; y < cursor_height; y++) {
            for (uint32_t x = 0; x < cursor_width; x++) {
                uint32_t color = pixels[y * CURSOR_MAX_SIZE + x];
                if (!(color >> 24u)) continue;
                if (((color >> 16u) & 255u) > 210u &&
                    ((color >> 8u) & 255u) > 210u && (color & 255u) > 210u)
                    color = (color & 0xFF000000u) |
                            (cursor_fill_color & 0x00FFFFFFu);
                for (uint32_t sy = 0; sy < scale; sy++) {
                    int32_t py = origin_y + (int32_t)(y * scale + sy) - top;
                    if (py < 0 || py >= height) continue;
                    for (uint32_t sx = 0; sx < scale; sx++) {
                        int32_t px = origin_x + (int32_t)(x * scale + sx) -
                                     left;
                        if (px < 0 || px >= width) continue;
                        uint32_t* target = cursor_patch +
                            (size_t)py * CURSOR_PATCH_SIZE + px;
                        *target = alpha_blend(color, *target);
                    }
                }
            }
        }
        ok = ok && mos_graphics_blit_surface(cursor_patch, CURSOR_PATCH_SIZE,
            (uint32_t)height, CURSOR_PATCH_SIZE * sizeof(uint32_t), 0, 0,
            width, height, left, top, SYS_GRAPHICS_BLIT_OPAQUE) ==
            SYS_SUCCESS;
    }
    if (!ok) {
        strcpy(status_message, "Framebuffer update failed");
        scene_valid = false;
        return;
    }

    if (full) {
        ok = mos_graphics_present(0, 0, (int32_t)display.width,
                                  (int32_t)display.height) == SYS_SUCCESS;
    } else {
        if (cursor_drawn && cursor_drawn_width > 0 && cursor_drawn_height > 0)
            ok = mos_graphics_present(cursor_drawn_x, cursor_drawn_y,
                cursor_drawn_width, cursor_drawn_height) == SYS_SUCCESS;
        if (width > 0 && height > 0)
            ok = mos_graphics_present(left, top, width, height) ==
                 SYS_SUCCESS && ok;
    }
    if (!ok) {
        strcpy(status_message, "Display present failed");
        scene_valid = false;
        return;
    }
    if (full) debug_repaints++;
    else debug_pointer_draws++;
    cursor_drawn = true;
    cursor_drawn_x = left;
    cursor_drawn_y = top;
    cursor_drawn_width = width;
    cursor_drawn_height = height;
    cursor_drawn_signature = signature;
}

typedef struct {
    uint16_t year;
    uint8_t month, day, hour, minute, second;
} moswm_datetime_t;

static moswm_datetime_t clock_time;
static bool clock_valid;
static uint32_t clock_frames = 30u;

static void update_clock(void) {
    if (++clock_frames < 30u) return;
    clock_frames = 0;
    moswm_datetime_t now;
    memset(&now, 0, sizeof(now));
    clock_valid = mos_syscall1(SYS_GETTIME, (uint64_t)(uintptr_t)&now) ==
                  SYS_SUCCESS && now.month >= 1 && now.month <= 12;
    /* The panel shows hours and minutes; a ticking seconds field would
     * change the scene signature and repaint everything once a second. */
    now.second = 0;
    if (clock_valid) clock_time = now;
}

/* The Minimal-OS hourglass at panel size: same geometry as the wallpaper. */
static void draw_mini_logo(int32_t x, int32_t y, uint32_t color) {
    for (int32_t i = 0; i < 2; i++) {
        draw_wallpaper_line(x, y + i, x + 24, y + i, color);
        draw_wallpaper_line(x, y + 24 - i, x + 24, y + 24 - i, color);
        draw_wallpaper_line(x + 24 - i, y, x - i, y + 24, color);
        draw_wallpaper_line(x + i, y, x + 11 + i, y + 11, color);
        draw_wallpaper_line(x + 24 - i, y + 24, x + 13 - i, y + 13, color);
    }
}

/* Launcher rows: three fixed entries, then the installed applications. */
static uint32_t start_menu_rows(uint32_t* apps) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < desktop_app_count &&
         count + 3u < START_MENU_MAX_ROWS; i++) {
        if (!strcmp(desktop_apps[i].name, "mosdesk")) continue;
        if (apps) apps[count] = i;
        count++;
    }
    return count;
}

static void start_menu_bounds(int32_t* x, int32_t* y, int32_t* width,
                              int32_t* height) {
    uint32_t apps = start_menu_rows(NULL);
    *width = 286;
    *height = 66 + 26 + 3 * 38 + (apps ? 30 + (int32_t)apps * 38 : 0) + 40;
    *x = 12;
    *y = (int32_t)display.height - (int32_t)TASKBAR_HEIGHT - 10 - *height;
}

static int32_t start_menu_row_y(uint32_t row) {
    int32_t x, y, width, height;
    start_menu_bounds(&x, &y, &width, &height);
    return y + 66 + 26 + (int32_t)row * 38 + (row >= 3u ? 30 : 0);
}

static uint32_t start_menu_row_at(int32_t px, int32_t py) {
    int32_t x, y, width, height;
    start_menu_bounds(&x, &y, &width, &height);
    if (px < x + 8 || px >= x + width - 8) return UINT32_MAX;
    uint32_t rows = 3u + start_menu_rows(NULL);
    for (uint32_t row = 0; row < rows; row++) {
        int32_t row_y = start_menu_row_y(row);
        if (py >= row_y && py < row_y + 36) return row;
    }
    return UINT32_MAX;
}

static void draw_taskbar(const syscall_window_info_t* windows,
                         uint64_t count) {
    static const char* const months[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
    int32_t top = (int32_t)display.height - (int32_t)TASKBAR_HEIGHT;
    int32_t width = (int32_t)display.width;
    uint32_t accent = 0xFF000000u | (accent_color & 0x00FFFFFFu);

    /* Panel: dark, slightly translucent, with a faint highlight edge. */
    fill_rect(0, top, width, (int32_t)TASKBAR_HEIGHT, 0xEC1B1E20u);
    fill_rect(0, top, width, 1, 0x2CFFFFFFu);

    /* Application launcher button. */
    if (start_menu_open || hover_taskbar == 0u)
        fill_rounded_rect(4, top + 4, 40, 40, 8,
                          start_menu_open ? (0x66000000u | (accent & 0xFFFFFFu))
                                          : 0x26FFFFFFu);
    draw_mini_logo(12, top + 12, 0xFFEAF2F8u);
    if (start_menu_open) fill_rect(8, top + 45, 32, 2, accent);

    /* Task manager: one rounded entry per window, Plasma-style indicator. */
    int32_t x = 60;
    for (uint64_t i = 0; i < count && (uint32_t)x + 184u < display.width - 126u;
         i++) {
        const syscall_window_info_t* info = &windows[i];
        bool focused = info->id == focused_id;
        bool minimized = (info->flags & SYSCALL_WINDOW_MINIMIZED) != 0;
        if (focused)
            fill_rounded_rect(x, top + 5, 176, 38, 6, 0x38FFFFFFu);
        else if (hover_taskbar == i + 1u)
            fill_rounded_rect(x, top + 5, 176, 38, 6, 0x24FFFFFFu);
        if (focused)
            fill_rect(x + 6, top + 44, 164, 3, accent);
        else
            fill_rect(x + 78, top + 45, 20, 2,
                      minimized ? 0x50FFFFFFu : 0xA0FFFFFFu);
        char label[SYSCALL_WINDOW_MAX_TITLE];
        cached_window_t* entry = cache_for(info->id);
        bool has_icon = entry && entry->has_icon;
        fit_text_px(label, sizeof(label), info->title, 126u, 2);
        if (has_icon)
            draw_custom_icon(entry->icon_pixels, x + 8, top + 12, 24u);
        else
            moswm_draw_file_icon_sized(canvas, display.width, display.height,
                x + 8, top + 12, info->id == browser_window_id
                    ? MOSWM_ICON_FOLDER : MOSWM_ICON_APP, 24u);
        wm_text(x + 40, top + 16, label,
                minimized ? 0xFF8C9BA6u : 0xFFEAF2F8u, 0, 2);
        x += 180;
    }
    if (!count)
        wm_text(60, top + 16, "MOSdesk", 0x80EAF2F8u, 0, 2);

    /* System tray: separator + two-line clock. */
    if (clock_valid) {
        char hhmm[6] = { (char)('0' + clock_time.hour / 10u),
                         (char)('0' + clock_time.hour % 10u), ':',
                         (char)('0' + clock_time.minute / 10u),
                         (char)('0' + clock_time.minute % 10u), '\0' };
        char date[8];
        uint32_t di = 0;
        if (clock_time.day >= 10u)
            date[di++] = (char)('0' + clock_time.day / 10u);
        date[di++] = (char)('0' + clock_time.day % 10u);
        date[di++] = ' ';
        for (uint32_t k = 0; k < 3u; k++)
            date[di++] = months[clock_time.month - 1u][k];
        date[di] = '\0';
        int32_t center = width - 62;
        fill_rect(width - 124, top + 10, 1, 28, 0x24FFFFFFu);
        wm_text(center - (int32_t)moswm_text_width(hhmm, 2) / 2, top + 9,
                hhmm, 0xFFEAF2F8u, 0, 2);
        wm_text(center - (int32_t)moswm_text_width(date, 1) / 2, top + 30,
                date, 0xFF9BAFBCu, 0, 1);
    }

    if (start_menu_open) {
        /* Kickoff-style launcher: header, system entries, applications. */
        static const struct {
            const char* label;
            moswm_file_icon_t icon;
        } fixed[3] = {
            {"Files", MOSWM_ICON_FOLDER},
            {"Settings", MOSWM_ICON_SETTINGS},
            {"Wallpaper", MOSWM_ICON_IMAGE}
        };
        int32_t mx, my, mw, mh;
        start_menu_bounds(&mx, &my, &mw, &mh);
        uint32_t apps[START_MENU_MAX_ROWS];
        uint32_t app_count = start_menu_rows(apps);
        fill_rounded_rect(mx - 5, my - 3, mw + 10, mh + 10, 12, 0x2E000000u);
        fill_rounded_rect(mx, my, mw, mh, 8, 0xFF3A4249u);
        fill_rounded_rect(mx + 1, my + 1, mw - 2, mh - 2, 7, 0xFF1D2125u);
        fill_rounded_rect(mx + 12, my + 12, 30, 30, 15, accent);
        wm_text(mx + 20, my + 19, "M", 0xFFFFFFFFu, 0, 2);
        wm_text(mx + 52, my + 10, "MOSdesk", 0xFFF2F6F9u, 0, 2);
        wm_text(mx + 52, my + 30, "Minimal-OS", 0xFF9BAFBCu, 0, 1);
        fill_rect(mx + 18, my + 56, mw - 36, 1, 0xFF353C42u);
        wm_text(mx + 18, my + 72, "SYSTEM", accent, 0, 1);
        if (app_count)
            wm_text(mx + 18, start_menu_row_y(3u) - 22, "APPLICATIONS",
                    accent, 0, 1);
        for (uint32_t row = 0; row < 3u + app_count; row++) {
            int32_t row_y = start_menu_row_y(row);
            if (hover_start_row == row)
                fill_rounded_rect(mx + 8, row_y, mw - 16, 36, 6,
                                  0x55000000u | (accent & 0xFFFFFFu));
            const char* label;
            if (row < 3u) {
                label = fixed[row].label;
                moswm_draw_file_icon_sized(canvas, display.width,
                    display.height, mx + 16, row_y + 4, fixed[row].icon,
                    28u);
            } else {
                const desktop_app_t* app = &desktop_apps[apps[row - 3u]];
                label = app->name;
                if (app->has_icon)
                    draw_custom_icon(app->icon_pixels, mx + 16, row_y + 4,
                                     28u);
                else
                    moswm_draw_file_icon_sized(canvas, display.width,
                        display.height, mx + 16, row_y + 4, MOSWM_ICON_APP,
                        28u);
            }
            char shown[DESKTOP_APP_NAME_SIZE];
            fit_text_px(shown, sizeof(shown), label, mw - 80, 2u);
            wm_text(mx + 56, row_y + 10, shown, 0xFFF2F6F9u, 0, 2);
        }
        fill_rect(mx + 18, my + mh - 34, mw - 36, 1, 0xFF353C42u);
        wm_text(mx + 18, my + mh - 22, "Right-click icons for more actions",
                0xFF8A9AA6u, 0, 1);
    }
    if (status_message[0])
        wm_text(8, 8, status_message, 0xFFFFE696u, 0xAA000000u, 2);
}

/* ------------------------------------------- right-click menu and dialogs */

static void menu_add(menu_action_t action, const char* label,
                     moswm_file_icon_t icon, bool separator_before,
                     bool enabled) {
    if (menu_item_count >= MENU_MAX_ITEMS) return;
    menu_items[menu_item_count++] = (menu_item_t){
        action, label, icon, separator_before, enabled};
}

static int32_t menu_height(void) {
    int32_t height = 12;
    for (uint32_t i = 0; i < menu_item_count; i++)
        height += MENU_ITEM_HEIGHT + (menu_items[i].separator_before ? 9 : 0);
    return height;
}

static void menu_show(click_action_t target, const char* path) {
    menu_target = target;
    strcpy(menu_path, path);
    menu_x = cursor_x;
    menu_y = cursor_y;
    int32_t height = menu_height();
    if (menu_x + MENU_WIDTH + 4 > (int32_t)display.width)
        menu_x = (int32_t)display.width - MENU_WIDTH - 4;
    if (menu_y + height + 4 > (int32_t)display.height)
        menu_y = (int32_t)display.height - height - 4;
    if (menu_x < 4) menu_x = 4;
    if (menu_y < 4) menu_y = 4;
    menu_hover = UINT32_MAX;
    menu_open = menu_item_count != 0u;
}

/* Menu for one item: a Files entry, or a Home/System/application icon. */
static void menu_show_for_item(click_action_t target, const char* path) {
    if (strlen(path) >= sizeof(menu_path)) return;
    menu_item_count = 0;
    bool editable = !path_is_protected(path);
    menu_add(MENU_ACTION_OPEN, "Open", MOSWM_ICON_FOLDER, false, true);
    if (target == CLICK_ACTION_APPLICATION)
        menu_add(MENU_ACTION_SHOW_IN_FILES, "Show in Files",
                 MOSWM_ICON_COMPUTER, false, true);
    if (name_has_extension(path, ".qoi"))
        menu_add(MENU_ACTION_SET_WALLPAPER, "Set as wallpaper",
                 MOSWM_ICON_IMAGE, false, true);
    if (target == CLICK_ACTION_APPLICATION ||
        target == CLICK_ACTION_BROWSER_ENTRY) {
        menu_add(MENU_ACTION_COPY, "Copy", MOSWM_ICON_COPY, true, true);
        menu_add(MENU_ACTION_RENAME, "Rename", MOSWM_ICON_EDIT, false,
                 editable);
        menu_add(MENU_ACTION_DELETE, "Delete", MOSWM_ICON_TRASH, false,
                 editable);
    }
    menu_show(target, path);
}

/* Menu for the empty part of a folder shown in Files. */
static void menu_show_for_folder(const char* directory) {
    menu_item_count = 0;
    menu_add(MENU_ACTION_PASTE, "Paste", MOSWM_ICON_PASTE, false,
             clipboard_path[0] != '\0');
    menu_add(MENU_ACTION_NEW_FOLDER, "New folder", MOSWM_ICON_NEWFOLDER,
             false, true);
    menu_add(MENU_ACTION_REFRESH, "Refresh", MOSWM_ICON_REFRESH, true, true);
    menu_show(CLICK_ACTION_NONE, directory);
}

static void menu_show_for_desktop(void) {
    menu_item_count = 0;
    menu_add(MENU_ACTION_FILES, "Open Files", MOSWM_ICON_FOLDER, false, true);
    menu_add(MENU_ACTION_WALLPAPERS, "Change wallpaper", MOSWM_ICON_IMAGE,
             false, true);
    menu_add(MENU_ACTION_SETTINGS, "Settings", MOSWM_ICON_SETTINGS, false,
             true);
    menu_add(MENU_ACTION_REFRESH, "Refresh desktop", MOSWM_ICON_REFRESH,
             true, true);
    menu_show(CLICK_ACTION_NONE, "");
}

static uint32_t menu_item_at(int32_t px, int32_t py) {
    if (!menu_open || px < menu_x || px >= menu_x + MENU_WIDTH)
        return UINT32_MAX;
    int32_t y = menu_y + 6;
    for (uint32_t i = 0; i < menu_item_count; i++) {
        if (menu_items[i].separator_before) y += 9;
        if (py >= y && py < y + MENU_ITEM_HEIGHT) return i;
        y += MENU_ITEM_HEIGHT;
    }
    return UINT32_MAX;
}

static void draw_context_menu(void) {
    if (!menu_open) return;
    uint32_t accent = 0xFF000000u | (accent_color & 0x00FFFFFFu);
    int32_t height = menu_height();
    fill_rounded_rect(menu_x - 4, menu_y - 2, MENU_WIDTH + 8, height + 8, 11,
                      0x30000000u);
    fill_rounded_rect(menu_x, menu_y, MENU_WIDTH, height, 8, 0xFF3A4249u);
    fill_rounded_rect(menu_x + 1, menu_y + 1, MENU_WIDTH - 2, height - 2, 7,
                      0xFF1F2428u);
    int32_t y = menu_y + 6;
    for (uint32_t i = 0; i < menu_item_count; i++) {
        const menu_item_t* item = &menu_items[i];
        if (item->separator_before) {
            fill_rect(menu_x + 12, y + 4, MENU_WIDTH - 24, 1, 0xFF3A4249u);
            y += 9;
        }
        if (menu_hover == i)
            fill_rounded_rect(menu_x + 5, y, MENU_WIDTH - 10,
                              MENU_ITEM_HEIGHT, 5,
                              0x66000000u | (accent & 0x00FFFFFFu));
        moswm_draw_file_icon_sized(canvas, display.width, display.height,
                                   menu_x + 13, y + 6, item->icon, 18u);
        wm_text(menu_x + 42, y + 10, item->label,
                item->enabled ? 0xFFF2F6F9u : 0xFF6F7C86u, 0, 1u);
        y += MENU_ITEM_HEIGHT;
    }
}

static void dialog_bounds(int32_t* x, int32_t* y, int32_t* width,
                          int32_t* height) {
    *width = display.width > 452u ? 420 : (int32_t)display.width - 32;
    *height = 186;
    *x = ((int32_t)display.width - *width) / 2;
    *y = ((int32_t)display.height - (int32_t)TASKBAR_HEIGHT - *height) / 2;
    if (*y < 8) *y = 8;
}

static void draw_dialog(void) {
    if (dialog_kind == DIALOG_NONE) return;
    int32_t x, y, width, height;
    dialog_bounds(&x, &y, &width, &height);
    bool deleting = dialog_kind == DIALOG_DELETE;
    fill_rect(0, 0, (int32_t)display.width, (int32_t)display.height,
              0x660A1218u);
    fill_rounded_rect(x - 4, y - 2, width + 8, height + 10, 14, 0x30000000u);
    fill_rounded_rect(x, y, width, height, 12, 0xFF3A4249u);
    fill_rounded_rect(x + 2, y + 2, width - 4, height - 4, 10, 0xFFF3F5F6u);
    moswm_draw_file_icon_sized(canvas, display.width, display.height,
        x + 22, y + 18, deleting ? MOSWM_ICON_TRASH
            : dialog_kind == DIALOG_RENAME ? MOSWM_ICON_EDIT
            : MOSWM_ICON_NEWFOLDER, 28u);
    wm_text(x + 62, y + 24,
            deleting ? "Delete this item?"
                : dialog_kind == DIALOG_RENAME ? "Rename" : "New folder",
            0xFF273640u, 0, 2);
    if (deleting) {
        char name[SYSCALL_DIRENT_NAME_SIZE];
        fit_text_px(name, sizeof(name), path_name(dialog_path), width - 48,
                    2u);
        wm_text(x + 24, y + 68, name, 0xFF34434Cu, 0, 2);
        wm_text(x + 24, y + 96,
                fs_is_directory(dialog_path)
                    ? "The folder and everything in it will be removed."
                    : "It will be removed permanently.",
                0xFF667680u, 0, 1);
    } else {
        fill_rounded_rect(x + 24, y + 62, width - 48, 36, 6, 0xFFBCC6CDu);
        fill_rounded_rect(x + 25, y + 63, width - 50, 34, 5, 0xFFFFFFFFu);
        char shown[SYSCALL_DIRENT_NAME_SIZE];
        fit_text_px(shown, sizeof(shown), dialog_text, width - 80, 1u);
        wm_text(x + 36, y + 76, shown, 0xFF273640u, 0, 1);
        fill_rect(x + 38 + (int32_t)moswm_text_width(shown, 1u), y + 70, 1,
                  20, accent_color | 0xFF000000u);
    }
    if (dialog_error[0])
        wm_text(x + 24, y + 110, dialog_error, 0xFFB43A32u, 0, 1);
    else if (!deleting)
        wm_text(x + 24, y + 110, "Enter to confirm, Esc to cancel",
                0xFF7A8A95u, 0, 1);
    int32_t button_y = y + height - 52;
    int32_t confirm_x = x + width - 24 - 112;
    int32_t cancel_x = confirm_x - 10 - 100;
    const char* confirm = deleting ? "Delete"
        : dialog_kind == DIALOG_RENAME ? "Rename" : "Create";
    fill_rounded_rect(cancel_x, button_y, 100, 34, 7, 0xFFDCE2E6u);
    wm_text(cancel_x + (100 - (int32_t)moswm_text_width("Cancel", 1u)) / 2,
            button_y + 11, "Cancel", 0xFF34434Cu, 0, 1);
    fill_rounded_rect(confirm_x, button_y, 112, 34, 7,
                      deleting ? 0xFFC8383Du : (accent_color | 0xFF000000u));
    wm_text(confirm_x + (112 - (int32_t)moswm_text_width(confirm, 1u)) / 2,
            button_y + 11, confirm, 0xFFFFFFFFu, 0, 1);
}

static void reload_desktop_apps(void) {
    desktop_app_count = 0;
    load_desktop_apps();
    desktop_apps_generation++;
}

/* Brings Files and the desktop up to date after a file operation. */
static void after_file_change(const char* directory, const char* select) {
    if (browser_window_id && !strcmp(browser_path, directory)) {
        browser_refresh_directory();
        if (select) browser_select_name(select);
    }
    if (path_has_prefix(directory, "0:/programs")) reload_desktop_apps();
}

static void dialog_confirm(void) {
    char parent[BROWSER_PATH_SIZE];
    char target[BROWSER_PATH_SIZE];
    if (dialog_kind == DIALOG_DELETE) {
        if (!path_parent(parent, sizeof(parent), dialog_path) ||
            !fs_delete_tree(dialog_path, 0)) {
            strcpy(dialog_error, "Unable to delete this item.");
            return;
        }
        if (path_has_prefix(clipboard_path, dialog_path))
            clipboard_path[0] = '\0';
        dialog_kind = DIALOG_NONE;
        after_file_change(parent, NULL);
        browser_note("Deleted", false);
        return;
    }
    if (!dialog_length || !strcmp(dialog_text, ".") ||
        !strcmp(dialog_text, "..")) {
        strcpy(dialog_error, "Type a name first.");
        return;
    }
    if (dialog_kind == DIALOG_NEW_FOLDER) {
        if (!path_join(target, sizeof(target), dialog_path, dialog_text)) {
            strcpy(dialog_error, "That name is too long.");
        } else if (fs_exists(target)) {
            strcpy(dialog_error, "That name is already used here.");
        } else if (mos_syscall3(SYS_MKDIR, (uint64_t)(uintptr_t)target, 0,
                                0) != SYS_SUCCESS) {
            strcpy(dialog_error, "Unable to create the folder.");
        } else {
            dialog_kind = DIALOG_NONE;
            after_file_change(dialog_path, dialog_text);
        }
        return;
    }
    if (!path_parent(parent, sizeof(parent), dialog_path) ||
        !path_join(target, sizeof(target), parent, dialog_text)) {
        strcpy(dialog_error, "That name is too long.");
        return;
    }
    if (!strcmp(target, dialog_path)) {
        dialog_kind = DIALOG_NONE;
        return;
    }
    if (fs_exists(target)) {
        strcpy(dialog_error, "That name is already used here.");
        return;
    }
    if (!fs_rename(dialog_path, target)) {
        strcpy(dialog_error, "Unable to rename this item.");
        return;
    }
    if (path_has_prefix(clipboard_path, dialog_path)) clipboard_path[0] = '\0';
    dialog_kind = DIALOG_NONE;
    after_file_change(parent, dialog_text);
}

static void dialog_key(const syscall_input_event_t* input) {
    if (input->key == 41u) {
        dialog_kind = DIALOG_NONE;
        return;
    }
    if (input->key == 40u) {
        dialog_confirm();
        return;
    }
    if (dialog_kind == DIALOG_DELETE) return;
    if (input->key == 42u) {
        if (dialog_length) dialog_text[--dialog_length] = '\0';
        dialog_error[0] = '\0';
        return;
    }
    char character = (char)mos_syscall3(SYS_USB, SYS_USB_KEY_TO_ASCII,
        input->key, mosdesk_usb_modifiers(input->mods));
    if (character >= 32 && character <= 126 && character != '/' &&
        character != ':' && character != '\\' &&
        dialog_length + 1u < sizeof(dialog_text)) {
        dialog_text[dialog_length++] = character;
        dialog_text[dialog_length] = '\0';
        dialog_error[0] = '\0';
    }
}

static void dialog_click(void) {
    int32_t x, y, width, height;
    dialog_bounds(&x, &y, &width, &height);
    int32_t button_y = y + height - 52;
    int32_t confirm_x = x + width - 24 - 112;
    int32_t cancel_x = confirm_x - 10 - 100;
    if (cursor_y < button_y || cursor_y >= button_y + 34) return;
    if (cursor_x >= confirm_x && cursor_x < confirm_x + 112)
        dialog_confirm();
    else if (cursor_x >= cancel_x && cursor_x < cancel_x + 100)
        dialog_kind = DIALOG_NONE;
}

/* Opens MOSdesk Settings, optionally on a named page ("wallpaper"). */
static void open_settings(const char* page) {
    const char* settings_path = "0:/programs/mosdesk/mosdesk.run";
    const char* arguments[1] = {page};
    if ((int64_t)mos_syscall3(SYS_EXEC, (uint64_t)(uintptr_t)settings_path,
            page ? (uint64_t)(uintptr_t)arguments : 0, page ? 1u : 0u) < 0)
        strcpy(status_message, "Unable to open MOSdesk Settings");
}

static void paste_into(const char* directory) {
    char target[BROWSER_PATH_SIZE];
    const char* name = path_name(clipboard_path);
    if (!clipboard_path[0] || !fs_exists(clipboard_path)) {
        clipboard_path[0] = '\0';
        browser_note("The copied item no longer exists", true);
    } else if (path_has_prefix(directory, clipboard_path)) {
        browser_note("A folder cannot be pasted into itself", true);
    } else if (!unique_copy_path(target, sizeof(target), directory, name)) {
        browser_note("Unable to choose a name for the copy", true);
    } else if (!fs_copy_tree(clipboard_path, target, 0)) {
        if (fs_exists(target)) fs_delete_tree(target, 0);
        browser_note("Unable to paste here", true);
    } else {
        after_file_change(directory, path_name(target));
        browser_note("Pasted", false);
    }
}

static void run_menu_action(menu_action_t action) {
    char parent[BROWSER_PATH_SIZE];
    switch (action) {
        case MENU_ACTION_OPEN:
            open_path(menu_target, menu_path);
            break;
        case MENU_ACTION_SHOW_IN_FILES:
            if (path_parent(parent, sizeof(parent), menu_path)) {
                browser_open(parent);
                browser_select_name(path_name(menu_path));
            }
            break;
        case MENU_ACTION_SET_WALLPAPER:
            set_wallpaper_image(menu_path);
            break;
        case MENU_ACTION_COPY:
            strcpy(clipboard_path, menu_path);
            browser_note("Copied", false);
            break;
        case MENU_ACTION_PASTE:
            paste_into(menu_path);
            break;
        case MENU_ACTION_RENAME:
            open_dialog(DIALOG_RENAME, menu_path, path_name(menu_path));
            break;
        case MENU_ACTION_DELETE:
            open_dialog(DIALOG_DELETE, menu_path, "");
            break;
        case MENU_ACTION_NEW_FOLDER:
            open_dialog(DIALOG_NEW_FOLDER, menu_path, "New folder");
            break;
        case MENU_ACTION_REFRESH:
            if (menu_path[0]) browser_refresh_directory();
            else reload_desktop_apps();
            break;
        case MENU_ACTION_FILES:
            browser_open(MOSDESK_USERS_PATH);
            break;
        case MENU_ACTION_WALLPAPERS:
            open_settings("wallpaper");
            break;
        case MENU_ACTION_SETTINGS:
            open_settings(NULL);
            break;
        default:
            break;
    }
}

static void auth_dialog_bounds(int32_t* x, int32_t* y,
                               int32_t* width, int32_t* height) {
    *width = display.width > 552u ? 520 : (int32_t)display.width - 32;
    *height = display.height > 332u ? 300 : (int32_t)display.height - 32;
    *x = ((int32_t)display.width - *width) / 2;
    *y = ((int32_t)display.height - *height) / 2;
}

static void draw_auth_screen(void) {
    copy_wallpaper(canvas);
    fill_rect(0, 0, (int32_t)display.width, (int32_t)display.height,
              0x660A1218u);
    int32_t x, y, width, height;
    auth_dialog_bounds(&x, &y, &width, &height);
    fill_rounded_rect(x, y, width, height, 12, 0xFF3A4249u);
    fill_rounded_rect(x + 2, y + 2, width - 4, height - 4, 10,
                      0xFFF3F5F6u);
    fill_rect(x + 24, y + 64, width - 48, 1, 0xFFD2D9DEu);
    fill_rounded_rect(x + 24, y + 19, 30, 30, 15, accent_color);
    wm_text(x + 32, y + 25, "M", 0xFFFFFFFFu, 0, 2);
    wm_text(x + 68, y + 22, "MOSdesk", 0xFF273640u, 0, 2);

    if (auth_state == AUTH_FIRST_CHOICE) {
        wm_text(x + 28, y + 86, "Welcome to MOSdesk", 0xFF34434Cu, 0, 2);
        wm_text(x + 28, y + 116, "Would you like password sign-in?",
                0xFF667680u, 0, 1);
        int32_t button_y = y + height - 58;
        int32_t button_width = (width - 84) / 2;
        int32_t first_button_x = x + 28;
        int32_t second_button_x = x + width - 28 - button_width;
        fill_rounded_rect(first_button_x, button_y, button_width, 36, 7,
                          accent_color);
        fill_rounded_rect(second_button_x, button_y, button_width, 36, 7,
                          0xFFDCE2E6u);
        wm_text(first_button_x + (button_width -
                (int32_t)moswm_text_width("Set password", 1u)) / 2,
                button_y + 11, "Set password",
                0xFFFFFFFFu, 0, 1);
        wm_text(second_button_x + (button_width -
                (int32_t)moswm_text_width("Auto login", 1u)) / 2,
                button_y + 11, "Auto login",
                0xFF34434Cu, 0, 1);
    } else {
        bool logging_in = auth_state == AUTH_LOGIN;
        wm_text(x + 28, y + 86,
                logging_in ? "Sign in to MOSdesk" : "Set a password",
                0xFF34434Cu, 0, 2);
        wm_text(x + 28, y + 116,
                logging_in ? "Enter your password to continue."
                           : "Use 4 to 63 characters, then press Enter.",
                0xFF667680u, 0, 1);
        fill_rounded_rect(x + 28, y + 140, width - 56, 38, 6,
                          0xFFFFFFFFu);
        fill_rect(x + 28, y + 140, width - 56, 1, 0xFFBCC6CDu);
        fill_rect(x + 28, y + 177, width - 56, 1, 0xFFBCC6CDu);
        char masked[AUTH_PASSWORD_MAX + 1u];
        uint32_t count = auth_password_length;
        if (count > AUTH_PASSWORD_MAX) count = AUTH_PASSWORD_MAX;
        for (uint32_t i = 0; i < count; i++) masked[i] = '*';
        masked[count] = '\0';
        wm_text(x + 42, y + 153, masked, 0xFF34434Cu, 0, 1);
        wm_text(x + 28, y + height - 51,
                "Enter to continue   Esc to go back",
                0xFF667680u, 0, 1);
    }
    if (auth_message[0])
        wm_text(x + 28, y + height - 78, auth_message,
                0xFFB43A32u, 0, 1);
}

static bool save_auth_preferences(bool password_enabled) {
    desktop_settings.onboarding_complete = 1u;
    desktop_settings.login_enabled = password_enabled ? 1u : 0u;
    if (!password_enabled) {
        memset(desktop_settings.password_salt, 0,
               sizeof(desktop_settings.password_salt));
        memset(desktop_settings.password_hash, 0,
               sizeof(desktop_settings.password_hash));
    }
    if (!mosdesk_settings_save(&desktop_settings)) {
        strcpy(auth_message, "Could not save setup. Please try again.");
        return false;
    }
    auth_state = AUTH_NONE;
    auth_message[0] = '\0';
    auth_password_length = 0;
    memset(auth_password, 0, sizeof(auth_password));
    return true;
}

static void submit_auth_password(void) {
    if (auth_state == AUTH_FIRST_PASSWORD) {
        if (auth_password_length < 4u) {
            strcpy(auth_message, "Use at least 4 characters.");
            return;
        }
        if (mos_syscall3(SYS_RANDOM,
                (uint64_t)(uintptr_t)desktop_settings.password_salt,
                sizeof(desktop_settings.password_salt), 0) !=
            sizeof(desktop_settings.password_salt)) {
            strcpy(auth_message, "Could not create a password salt.");
            return;
        }
        if (!mosdesk_password_hash(desktop_settings.password_salt,
                auth_password, desktop_settings.password_hash)) {
            strcpy(auth_message, "Could not set this password.");
            return;
        }
        save_auth_preferences(true);
    } else if (auth_state == AUTH_LOGIN) {
        uint8_t actual[MOSDESK_PASSWORD_HASH_SIZE];
        if (mosdesk_password_hash(desktop_settings.password_salt,
                auth_password, actual)) {
            uint8_t difference = 0;
            for (uint32_t i = 0; i < sizeof(actual); i++)
                difference |= actual[i] ^ desktop_settings.password_hash[i];
            memset(actual, 0, sizeof(actual));
            if (difference == 0) {
                auth_state = AUTH_NONE;
                auth_message[0] = '\0';
                auth_password_length = 0;
                memset(auth_password, 0, sizeof(auth_password));
                return;
            }
        }
        strcpy(auth_message, "Password did not match.");
        memset(auth_password, 0, sizeof(auth_password));
        auth_password_length = 0;
    }
}

static void handle_auth_key(const syscall_input_event_t* input) {
    if (input->type != SYSCALL_INPUT_KEY_DOWN) return;
    if (input->key == 40u) {
        if (auth_state != AUTH_FIRST_CHOICE) submit_auth_password();
        return;
    }
    if (input->key == 41u) {
        if (auth_state == AUTH_FIRST_PASSWORD) {
            auth_state = AUTH_FIRST_CHOICE;
            auth_message[0] = '\0';
            auth_password_length = 0;
            memset(auth_password, 0, sizeof(auth_password));
        }
        return;
    }
    if (input->key == 42u) {
        if (auth_password_length) {
            auth_password[--auth_password_length] = '\0';
            auth_message[0] = '\0';
        }
        return;
    }
    char character = (char)mos_syscall3(SYS_USB,
        SYS_USB_KEY_TO_ASCII, input->key,
        mosdesk_usb_modifiers(input->mods));
    if (character >= 32 && character <= 126 &&
        auth_password_length < AUTH_PASSWORD_MAX) {
        auth_password[auth_password_length++] = character;
        auth_password[auth_password_length] = '\0';
        auth_message[0] = '\0';
    }
}

static void handle_auth_click(void) {
    int32_t x, y, width, height;
    auth_dialog_bounds(&x, &y, &width, &height);
    if (auth_state == AUTH_FIRST_CHOICE) {
        int32_t button_y = y + height - 58;
        int32_t button_width = (width - 84) / 2;
        if (cursor_y < button_y || cursor_y >= button_y + 36) return;
        if (cursor_x >= x + 28 && cursor_x < x + 28 + button_width) {
            auth_state = AUTH_FIRST_PASSWORD;
            auth_password_length = 0;
            memset(auth_password, 0, sizeof(auth_password));
            auth_message[0] = '\0';
        } else if (cursor_x >= x + width - 28 - button_width &&
                   cursor_x < x + width - 28) {
            save_auth_preferences(false);
        }
    } else if (auth_state == AUTH_LOGIN) {
        submit_auth_password();
    }
}

/* ---------------------------------------------------------- debug overlay */

static uint64_t read_tsc(void) {
    uint32_t low, high;
    __asm__ volatile("rdtsc" : "=a"(low), "=d"(high));
    return ((uint64_t)high << 32) | low;
}

/* Charges the time since the previous lap to `stage`. */
static void debug_lap(profile_stage_t stage) {
    if (!debug_fps && !debug_profiler) return;
    uint64_t now = read_tsc();
    uint64_t spent = now - debug_lap_ticks;
    debug_lap_ticks = now;
    debug_stage_ticks[stage] += spent;
    if (spent > debug_stage_peak[stage]) debug_stage_peak[stage] = spent;
}

static void debug_restart_window(uint64_t now_ms) {
    debug_window_start_ms = now_ms;
    debug_window_start_ticks = debug_lap_ticks;
    memset(debug_stage_ticks, 0, sizeof(debug_stage_ticks));
    memset(debug_stage_peak, 0, sizeof(debug_stage_peak));
    debug_frames = 0;
    debug_pointer_draws = 0;
    debug_mouse_events = 0;
    debug_repaints = 0;
}

/* Called once per trip round the main loop; publishes a report every second.
 * The stopwatch counts CPU timestamp ticks and is scaled against the kernel's
 * millisecond clock over the same second. */
static void debug_frame_end(void) {
    if (!debug_fps && !debug_profiler) {
        if (debug_window_open) {
            debug_window_open = false;
            debug_report.valid = false;
        }
        return;
    }
    uint64_t now_ms = mos_syscall1(SYS_UPTIME, 0);
    if (!debug_window_open) {
        /* The laps of the frame that switched the overlay on are partial. */
        debug_window_open = true;
        debug_restart_window(now_ms);
        return;
    }
    debug_frames++;
    uint64_t elapsed_ms = now_ms - debug_window_start_ms;
    if (elapsed_ms < 1000u) return;

    uint64_t ticks_per_us = (debug_lap_ticks - debug_window_start_ticks) /
                            (elapsed_ms * 1000u);
    if (!ticks_per_us) ticks_per_us = 1u;
    debug_report.valid = true;
    debug_report.fps_tenths =
        (uint32_t)((uint64_t)debug_frames * 10000u / elapsed_ms);
    debug_report.frame_us = (uint32_t)(elapsed_ms * 1000u / debug_frames);
    debug_report.pointer_per_second =
        (uint32_t)(((uint64_t)debug_pointer_draws * 1000u + elapsed_ms / 2u) /
                   elapsed_ms);
    debug_report.mouse_per_second =
        (uint32_t)(((uint64_t)debug_mouse_events * 1000u + elapsed_ms / 2u) /
                   elapsed_ms);
    debug_report.repaints_per_second =
        (uint32_t)(((uint64_t)debug_repaints * 1000u + elapsed_ms / 2u) /
                   elapsed_ms);
    for (uint32_t i = 0; i < PROFILE_STAGES; i++) {
        debug_report.average_us[i] =
            (uint32_t)(debug_stage_ticks[i] / debug_frames / ticks_per_us);
        debug_report.peak_us[i] =
            (uint32_t)(debug_stage_peak[i] / ticks_per_us);
    }
    debug_generation++;
    debug_restart_window(now_ms);
}

static char* append_text(char* output, const char* text) {
    while (*text) *output++ = *text++;
    *output = '\0';
    return output;
}

static char* append_number(char* output, uint32_t value) {
    char digits[12];
    uint32_t length = 0;
    do {
        digits[length++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value);
    while (length) *output++ = digits[--length];
    *output = '\0';
    return output;
}

/* Microseconds as milliseconds with two decimals: 16670 -> "16.67". */
static char* append_ms(char* output, uint32_t microseconds) {
    uint32_t hundredths = (microseconds + 5u) / 10u;
    output = append_number(output, hundredths / 100u);
    *output++ = '.';
    *output++ = (char)('0' + hundredths / 10u % 10u);
    *output++ = (char)('0' + hundredths % 10u);
    *output = '\0';
    return output;
}

static void debug_text_right(int32_t right, int32_t y, const char* text,
                             uint32_t color) {
    wm_text(right - (int32_t)moswm_text_width(text, 1u), y, text, color, 0,
            1);
}

static void draw_debug_overlay(void) {
    if (!debug_fps && !debug_profiler) return;
    static const char* const stage_names[PROFILE_STAGES] = {
        "Settings poll", "Files window", "Window surfaces", "Input",
        "Cursor, hover, clock", "Scene draw", "Present", "Sleep"
    };
    const debug_report_t* report = &debug_report;
    const int32_t width = 268, row = 17;
    uint32_t rows = 0;
    if (!report->valid) rows = 1u;
    else rows = (debug_fps ? 4u : 0u) +
                (debug_profiler ? PROFILE_STAGES + 1u : 0u);
    int32_t height = (int32_t)rows * row + 15 +
                     (report->valid && debug_fps && debug_profiler ? 9 : 0);
    int32_t x = (int32_t)display.width - width - 8;
    int32_t y = 8;
    int32_t right = x + width - 12;
    int32_t peak_right = right;
    int32_t average_right = right - 64;
    fill_rounded_rect(x, y, width, height, 6, 0xD910161Bu);
    y += 10;
    if (!report->valid) {
        wm_text(x + 12, y, "Measuring...", 0xFFB7C4CEu, 0, 1);
        return;
    }
    char line[48];
    if (debug_fps) {
        char* end = append_number(line, report->fps_tenths / 10u);
        *end++ = '.';
        *end++ = (char)('0' + report->fps_tenths % 10u);
        append_text(end, " fps");
        /* Green at a full 60 Hz loop, amber when it sags, red when it crawls. */
        wm_text(x + 12, y, line,
                report->fps_tenths >= 450u ? 0xFF7FE0A0u
                    : report->fps_tenths >= 200u ? 0xFFFFD36Bu : 0xFFFF8A80u,
                0, 1);
        append_text(append_ms(line, report->frame_us), " ms/frame");
        debug_text_right(right, y, line, 0xFFE8EEF2u);
        y += row;
        static const char* const rate_names[3] = {
            "Pointer redraws", "Mouse events", "Scene repaints"};
        const uint32_t rates[3] = {report->pointer_per_second,
                                   report->mouse_per_second,
                                   report->repaints_per_second};
        for (uint32_t i = 0; i < 3u; i++) {
            wm_text(x + 12, y, rate_names[i], 0xFFB7C4CEu, 0, 1);
            append_text(append_number(line, rates[i]), " /s");
            debug_text_right(right, y, line, 0xFFE8EEF2u);
            y += row;
        }
    }
    if (debug_profiler) {
        if (debug_fps) {
            fill_rect(x + 12, y + 1, width - 24, 1, 0x40FFFFFFu);
            y += 9;
        }
        wm_text(x + 12, y, "Stage (ms per frame)", 0xFF8EC5FFu, 0, 1);
        debug_text_right(average_right, y, "avg", 0xFF8EC5FFu);
        debug_text_right(peak_right, y, "peak", 0xFF8EC5FFu);
        y += row;
        for (uint32_t i = 0; i < PROFILE_STAGES; i++) {
            /* Sleep is the idle part of a frame; everything else is work,
             * flagged once it alone would break a 60 Hz frame. */
            bool slow = i != PROFILE_SLEEP && report->average_us[i] >= 8000u;
            wm_text(x + 12, y, stage_names[i],
                    slow ? 0xFFFF8A80u : 0xFFB7C4CEu, 0, 1);
            append_ms(line, report->average_us[i]);
            debug_text_right(average_right, y, line,
                             slow ? 0xFFFF8A80u : 0xFFE8EEF2u);
            append_ms(line, report->peak_us[i]);
            debug_text_right(peak_right, y, line, 0xFF94A5B1u);
            y += row;
        }
    }
}

/* Everything the scene shows, folded into one number: when it is unchanged
 * from the last frame there is nothing to repaint but the pointer. */
static uint64_t compute_scene_signature(const syscall_window_info_t* windows,
                                        uint64_t count) {
    uint64_t hash = 0xCBF29CE484222325ull;
    hash = hash_u64(hash, auth_state);
    hash = hash_u64(hash, accent_color);
    hash = hash_text(hash, status_message);
    hash = hash_u64(hash, ((uint64_t)debug_fps << 33) |
                          ((uint64_t)debug_profiler << 32) |
                          debug_generation);
    if (auth_state != AUTH_NONE) {
        hash = hash_u64(hash, wallpaper_generation);
        hash = hash_u64(hash, auth_password_length);
        return hash_text(hash, auth_message);
    }
    hash = hash_u64(hash, desktop_signature());
    hash = hash_bytes(hash, windows, (size_t)count * sizeof(*windows));
    for (uint64_t i = 0; i < count; i++) {
        cached_window_t* entry = cache_for(windows[i].id);
        if (entry && entry->has_icon) hash = hash_u64(hash, entry->icon_sum);
    }
    hash = hash_u64(hash, ((uint64_t)focused_id << 32) | hover_taskbar);
    hash = hash_u64(hash, ((uint64_t)start_menu_open << 32) | hover_start_row);
    hash = hash_u64(hash, clock_valid);
    hash = hash_bytes(hash, &clock_time, sizeof(clock_time));
    hash = hash_u64(hash, ((uint64_t)menu_open << 32) | menu_hover);
    if (menu_open) {
        hash = hash_u64(hash, ((uint64_t)(uint32_t)menu_x << 32) |
                              (uint32_t)menu_y);
        for (uint32_t i = 0; i < menu_item_count; i++)
            hash = hash_u64(hash, ((uint64_t)menu_items[i].action << 1) |
                                  menu_items[i].enabled);
    }
    hash = hash_u64(hash, dialog_kind);
    if (dialog_kind != DIALOG_NONE) {
        hash = hash_text(hash, dialog_path);
        hash = hash_text(hash, dialog_text);
        hash = hash_text(hash, dialog_error);
    }
    return hash;
}

static void render(const syscall_window_info_t* windows, uint64_t count) {
    ensure_wallpaper();
    uint64_t signature = compute_scene_signature(windows, count);
    bool repaint = !scene_valid || signature != scene_signature;
    if (repaint && auth_state != AUTH_NONE) {
        draw_auth_screen();
    } else if (repaint) {
        compose_desktop();
        for (uint32_t focus_pass = 0; focus_pass < 2; focus_pass++) {
            for (uint64_t i = 0; i < count; i++) {
                const syscall_window_info_t* info = &windows[i];
                bool focused = info->id == focused_id;
                if ((focus_pass == 0 && focused) ||
                    (focus_pass == 1 && !focused))
                    continue;
                if (!(info->flags & SYSCALL_WINDOW_VISIBLE) ||
                    (info->flags & SYSCALL_WINDOW_MINIMIZED))
                    continue;
                cached_window_t* entry = cache_for(info->id);
                if (entry) composite_window(info, entry);
            }
        }
        draw_taskbar(windows, count);
        draw_context_menu();
        draw_dialog();
    }
    if (repaint) {
        draw_debug_overlay();
        scene_signature = signature;
        scene_valid = true;
    }
    debug_lap(PROFILE_DRAW);
    present_frame(repaint);
    debug_lap(PROFILE_PRESENT);
}

static syscall_window_info_t* window_at(syscall_window_info_t* windows,
                                        uint64_t count, int32_t px,
                                        int32_t py) {
    for (uint32_t focus_pass = 0; focus_pass < 2; focus_pass++) {
        for (uint64_t i = count; i > 0; i--) {
            syscall_window_info_t* info = &windows[i - 1u];
            if (!(info->flags & SYSCALL_WINDOW_VISIBLE) ||
                (info->flags & SYSCALL_WINDOW_MINIMIZED) ||
                ((info->id == focused_id) != (focus_pass == 0)))
                continue;
            int32_t right = (int32_t)info->x + (int32_t)info->width + 2;
            int32_t bottom = (int32_t)info->y + (int32_t)info->height +
                             (int32_t)TITLE_HEIGHT + 2;
            if (px >= (int32_t)info->x && px < right &&
                py >= (int32_t)info->y && py < bottom)
                return info;
        }
    }
    return NULL;
}

static void update_cursor_appearance(syscall_window_info_t* windows,
                                     uint64_t count) {
    syscall_window_info_t* under =
        window_at(windows, count, cursor_x, cursor_y);
    syscall_window_info_t* app_window =
        under && under->id == focused_id ? under : NULL;
    moswm_cursor_state_t cursor_state = {0};
    uint32_t status = SYSCALL_CURSOR_ARROW;

    if (dragging_id || desktop_icon_drag_index != UINT32_MAX) {
        status = drag_mode == DRAG_RESIZE
            ? SYSCALL_CURSOR_RESIZE_NWSE : SYSCALL_CURSOR_MOVE;
        app_window = NULL;
    } else if (under) {
        int32_t x = cursor_x - (int32_t)under->x;
        int32_t y = cursor_y - (int32_t)under->y;
        int32_t right = (int32_t)under->width + 1;
        if (y < (int32_t)TITLE_HEIGHT) {
            if (x >= right - 72) status = SYSCALL_CURSOR_HAND;
            else status = SYSCALL_CURSOR_MOVE;
            app_window = NULL;
        } else if ((under->flags & SYSCALL_WINDOW_RESIZABLE) &&
                   x >= right - 14 &&
                   y >= (int32_t)under->height +
                            (int32_t)TITLE_HEIGHT - 14) {
            status = SYSCALL_CURSOR_RESIZE_NWSE;
            app_window = NULL;
        } else if (!app_window) {
            status = SYSCALL_CURSOR_ARROW;
        } else {
            status = cursor_state.status;
        }
    }
    ensure_cursor_loaded(app_window ? &cursor_state : NULL, status);
}

static void focus_window(syscall_window_info_t* info) {
    if (!info || focused_id == info->id) return;
    if (mos_window_set_flags(info->id,
            info->flags | SYSCALL_WINDOW_FOCUSED) != SYS_SUCCESS) {
        strcpy(status_message, "Unable to focus window");
        return;
    }
    focused_id = info->id;
}

static void request_close(syscall_window_info_t* info) {
    if (!info) return;
    post_simple_event(info->id, SYSCALL_WINDOW_EVENT_CLOSE_REQUEST);
}

static void maximize_or_restore(syscall_window_info_t* info,
                                cached_window_t* entry) {
    if (!info || !entry) return;
    uint32_t mode = window_mode(info) == MODE_MAXIMIZED
        ? MODE_NORMAL : MODE_MAXIMIZED;
    if (!set_window_mode(info, entry, mode))
        strcpy(status_message, "Unable to change window size");
    else if (mode == MODE_MAXIMIZED)
        post_simple_event(info->id, SYSCALL_WINDOW_EVENT_MAXIMIZED);
    else
        post_simple_event(info->id, SYSCALL_WINDOW_EVENT_RESTORED);
}

static void minimize_window(syscall_window_info_t* info) {
    if (!info) return;
    cached_window_t* entry = cache_for(info->id);
    if (!entry || !set_window_mode(info, entry, MODE_MINIMIZED)) {
        strcpy(status_message, "Unable to minimize window");
        return;
    }
    post_simple_event(info->id, SYSCALL_WINDOW_EVENT_MINIMIZED);
    if (focused_id == info->id) focused_id = 0;
}

static void start_move(syscall_window_info_t* info, bool resize) {
    if (!info || (!(info->flags & SYSCALL_WINDOW_RESIZABLE) && resize)) return;
    cached_window_t* entry = cache_for(info->id);
    syscall_window_info_t current = *info;
    if (entry && window_mode(info) != MODE_NORMAL) {
        if (!set_window_mode(info, entry, MODE_NORMAL) ||
            mos_window_get_info(info->id, &current) != SYS_SUCCESS)
            return;
        post_simple_event(info->id, SYSCALL_WINDOW_EVENT_RESTORED);
    }
    dragging_id = info->id;
    drag_mode = resize ? DRAG_RESIZE : DRAG_MOVE;
    drag_start_x = (uint32_t)cursor_x;
    drag_start_y = (uint32_t)cursor_y;
    drag_start_width = current.width;
    drag_start_height = current.height;
    drag_start_window_x = current.x;
    drag_start_window_y = current.y;
    if (entry) save_bounds(entry, &current);
}

static void move_dragged_window(syscall_window_info_t* windows,
                                uint64_t count) {
    if (desktop_icon_drag_index != UINT32_MAX) {
        int32_t dx = cursor_x - desktop_icon_drag_start_x;
        int32_t dy = cursor_y - desktop_icon_drag_start_y;
        if (dx < -6 || dx > 6 || dy < -6 || dy > 6)
            desktop_icon_drag_moved = true;
        if (desktop_icon_drag_moved) {
            int32_t x = desktop_icon_drag_origin_x + dx;
            int32_t y = desktop_icon_drag_origin_y + dy;
            int32_t max_x = (int32_t)display.width - 84;
            int32_t max_y = (int32_t)display.height -
                            (int32_t)TASKBAR_HEIGHT - 90;
            if (max_x < 0) max_x = 0;
            if (max_y < 0) max_y = 0;
            if (x < 0) x = 0;
            if (y < 0) y = 0;
            if (x > max_x) x = max_x;
            if (y > max_y) y = max_y;
            if (desktop_settings.grid_enabled) {
                if (x >= 24) x = 24 + ((x - 24 + 44) / 88) * 88;
                else x = 24;
                if (y >= 14) y = 14 + ((y - 14 + 49) / 98) * 98;
                else y = 14;
                if (x > max_x) x = max_x;
                if (y > max_y) y = max_y;
            }
            desktop_settings.desktop_icon_x[desktop_icon_drag_index] =
                (uint32_t)x;
            desktop_settings.desktop_icon_y[desktop_icon_drag_index] =
                (uint32_t)y;
        }
    }
    if (!dragging_id || drag_mode == DRAG_NONE) return;
    for (uint64_t i = 0; i < count; i++) {
        syscall_window_info_t* info = &windows[i];
        if (info->id != dragging_id) continue;
        int32_t dx = cursor_x - (int32_t)drag_start_x;
        int32_t dy = cursor_y - (int32_t)drag_start_y;
        uint32_t x = info->x;
        uint32_t y = info->y;
        uint32_t width = drag_start_width;
        uint32_t height = drag_start_height;
        if (drag_mode == DRAG_MOVE) {
            int32_t nx = (int32_t)drag_start_window_x + dx;
            int32_t ny = (int32_t)drag_start_window_y + dy;
            if (nx < 0) nx = 0;
            if (ny < 0) ny = 0;
            x = (uint32_t)nx;
            y = (uint32_t)ny;
        } else {
            int32_t nw = (int32_t)drag_start_width + dx;
            int32_t nh = (int32_t)drag_start_height + dy;
            if (nw < 64) nw = 64;
            if (nh < 48) nh = 48;
            width = (uint32_t)nw;
            height = (uint32_t)nh;
        }
        if (mos_window_set_bounds(info->id, x, y, width, height) != SYS_SUCCESS)
            strcpy(status_message, "Window resize or move failed");
        break;
    }
}

static void on_mouse_down(syscall_window_info_t* windows, uint64_t count) {
    pending_click_action = CLICK_ACTION_NONE;
    desktop_icon_drag_index = UINT32_MAX;
    desktop_icon_drag_moved = false;
    uint32_t taskbar_y = display.height - TASKBAR_HEIGHT;
    int32_t menu_left, menu_top, menu_width, menu_tall;
    start_menu_bounds(&menu_left, &menu_top, &menu_width, &menu_tall);
    if (start_menu_open && cursor_x >= menu_left &&
        cursor_x < menu_left + menu_width && cursor_y >= menu_top &&
        cursor_y < menu_top + menu_tall) {
        last_click_valid = false;
        uint32_t row = start_menu_row_at(cursor_x, cursor_y);
        if (row == UINT32_MAX) return;
        start_menu_open = false;
        uint32_t apps[START_MENU_MAX_ROWS];
        uint32_t app_count = start_menu_rows(apps);
        if (row == 0u) {
            browser_open(MOSDESK_USERS_PATH);
        } else if (row == 1u) {
            open_settings(NULL);
        } else if (row == 2u) {
            open_settings("wallpaper");
        } else if (row - 3u < app_count) {
            open_path(CLICK_ACTION_APPLICATION,
                      desktop_apps[apps[row - 3u]].path);
        }
        return;
    }
    if ((uint32_t)cursor_y >= taskbar_y) {
        last_click_valid = false;
        if (cursor_x < 44) {
            start_menu_open = !start_menu_open;
            return;
        }
        uint32_t index = cursor_x >= 60
            ? ((uint32_t)cursor_x - 60u) / 180u : UINT32_MAX;
        if (cursor_x >= 60 && index < count) {
            syscall_window_info_t* info = &windows[index];
            if (info->flags & SYSCALL_WINDOW_MINIMIZED) {
                cached_window_t* entry = cache_for(info->id);
                if (entry && set_window_mode(info, entry, MODE_NORMAL))
                    post_simple_event(info->id, SYSCALL_WINDOW_EVENT_RESTORED);
                else
                    strcpy(status_message, "Unable to restore window");
            }
            syscall_window_info_t current;
            if (mos_window_get_info(info->id, &current) == SYS_SUCCESS)
                focus_window(&current);
        }
        return;
    }

    syscall_window_info_t* info =
        window_at(windows, count, cursor_x, cursor_y);
    if (!info) {
        start_menu_open = false;
        uint32_t app_index = UINT32_MAX;
        click_action_t action =
            desktop_shortcut_at(cursor_x, cursor_y, &app_index);
        if (action == CLICK_ACTION_HOME) {
            queue_open_click(action, MOSDESK_USERS_PATH);
        } else if (action == CLICK_ACTION_SYSTEM) {
            queue_open_click(action, "0:/");
        } else if (action == CLICK_ACTION_APPLICATION) {
            queue_open_click(action, desktop_apps[app_index].path);
        } else {
            last_click_valid = false;
            return;
        }
        desktop_icon_drag_index = action == CLICK_ACTION_HOME ? 0u
            : action == CLICK_ACTION_SYSTEM ? 1u : app_index + 2u;
        desktop_icon_drag_start_x = cursor_x;
        desktop_icon_drag_start_y = cursor_y;
        desktop_icon_position(desktop_icon_drag_index,
                              &desktop_icon_drag_origin_x,
                              &desktop_icon_drag_origin_y);
        desktop_icon_saved_x =
            desktop_settings.desktop_icon_x[desktop_icon_drag_index];
        desktop_icon_saved_y =
            desktop_settings.desktop_icon_y[desktop_icon_drag_index];
        return;
    }
    focus_window(info);
    int32_t x = cursor_x - (int32_t)info->x;
    int32_t y = cursor_y - (int32_t)info->y;
    int32_t right = (int32_t)info->width + 1;
    start_menu_open = false;
    if (info->id == browser_window_id) {
        if (y >= (int32_t)TITLE_HEIGHT) {
            browser_mouse_down(x - 1,
                y - (int32_t)TITLE_HEIGHT - 1, info);
            return;
        }
        if (x >= right - 24) {
            last_click_valid = false;
            browser_close();
            return;
        }
    }
    last_click_valid = false;
    if (y < (int32_t)TITLE_HEIGHT) {
        if (x >= right - 24) {
            request_close(info);
        } else if (x >= right - 48) {
            maximize_or_restore(info, cache_for(info->id));
        } else if (x >= right - 72) {
            minimize_window(info);
        } else {
            start_move(info, false);
        }
    } else if (x >= right - 14 &&
               y >= (int32_t)info->height + (int32_t)TITLE_HEIGHT - 14) {
        start_move(info, true);
    }
}

static void on_right_down(syscall_window_info_t* windows, uint64_t count) {
    start_menu_open = false;
    pending_click_action = CLICK_ACTION_NONE;
    last_click_valid = false;
    if ((uint32_t)cursor_y >= display.height - TASKBAR_HEIGHT) return;
    syscall_window_info_t* info =
        window_at(windows, count, cursor_x, cursor_y);
    if (info) {
        /* Other applications get the click as an ordinary window event. */
        if (info->id != browser_window_id) return;
        int32_t x = cursor_x - (int32_t)info->x - 1;
        int32_t y = cursor_y - (int32_t)info->y - (int32_t)TITLE_HEIGHT - 1;
        if (y < (int32_t)BROWSER_HEADER_HEIGHT ||
            y >= (int32_t)browser_height - (int32_t)BROWSER_FOOTER_HEIGHT)
            return;
        uint32_t index = browser_row_at(x, y);
        char entry_path[BROWSER_PATH_SIZE];
        if (index == UINT32_MAX) {
            menu_show_for_folder(browser_path);
        } else if (path_join(entry_path, sizeof(entry_path), browser_path,
                             browser_entries[index].name)) {
            browser_selected = index;
            browser_generation++;
            menu_show_for_item(CLICK_ACTION_BROWSER_ENTRY, entry_path);
        }
        return;
    }
    uint32_t app_index = UINT32_MAX;
    click_action_t action =
        desktop_shortcut_at(cursor_x, cursor_y, &app_index);
    if (action == CLICK_ACTION_HOME)
        menu_show_for_item(action, MOSDESK_USERS_PATH);
    else if (action == CLICK_ACTION_SYSTEM)
        menu_show_for_item(action, "0:/");
    else if (action == CLICK_ACTION_APPLICATION)
        menu_show_for_item(action, desktop_apps[app_index].path);
    else
        menu_show_for_desktop();
}

/* Works out what the pointer is resting on, for hover highlights. */
static void update_hover(syscall_window_info_t* windows, uint64_t count) {
    hover_desktop_icon = UINT32_MAX;
    hover_taskbar = UINT32_MAX;
    hover_start_row = UINT32_MAX;
    menu_hover = UINT32_MAX;
    browser_hover = UINT32_MAX;
    browser_tool_hover = UINT32_MAX;
    if (auth_state != AUTH_NONE || dialog_kind != DIALOG_NONE) return;
    if (menu_open) {
        uint32_t item = menu_item_at(cursor_x, cursor_y);
        if (item != UINT32_MAX && menu_items[item].enabled) menu_hover = item;
        return;
    }
    if (dragging_id || desktop_icon_drag_index != UINT32_MAX) return;
    if (start_menu_open) {
        int32_t x, y, width, height;
        start_menu_bounds(&x, &y, &width, &height);
        if (cursor_x >= x && cursor_x < x + width && cursor_y >= y &&
            cursor_y < y + height) {
            hover_start_row = start_menu_row_at(cursor_x, cursor_y);
            return;
        }
    }
    if ((uint32_t)cursor_y >= display.height - TASKBAR_HEIGHT) {
        if (cursor_x < 44) {
            hover_taskbar = 0u;
        } else if (cursor_x >= 60 && (uint32_t)(cursor_x - 60) % 180u < 176u) {
            uint32_t index = (uint32_t)(cursor_x - 60) / 180u;
            if (index < count) hover_taskbar = index + 1u;
        }
        return;
    }
    syscall_window_info_t* info =
        window_at(windows, count, cursor_x, cursor_y);
    if (!info) {
        hover_desktop_icon = desktop_icon_at(cursor_x, cursor_y);
        return;
    }
    if (info->id != browser_window_id) return;
    int32_t x = cursor_x - (int32_t)info->x - 1;
    int32_t y = cursor_y - (int32_t)info->y - (int32_t)TITLE_HEIGHT - 1;
    if (y < 0) return;
    browser_tool_hover = browser_tool_at(x, y);
    if (y < (int32_t)browser_height - (int32_t)BROWSER_FOOTER_HEIGHT)
        browser_hover = browser_row_at(x, y);
}

static void on_mouse_up(syscall_window_info_t* windows, uint64_t count,
                        uint32_t time_ms) {
    if (desktop_icon_drag_index != UINT32_MAX) {
        if (desktop_icon_drag_moved &&
            !mosdesk_settings_save(&desktop_settings)) {
            desktop_settings.desktop_icon_x[desktop_icon_drag_index] =
                desktop_icon_saved_x;
            desktop_settings.desktop_icon_y[desktop_icon_drag_index] =
                desktop_icon_saved_y;
            strcpy(status_message, "Unable to save desktop icon position");
        }
        desktop_icon_drag_index = UINT32_MAX;
        desktop_icon_drag_moved = false;
    }
    if (pending_click_action != CLICK_ACTION_NONE) {
        int32_t dx = cursor_x - pending_click_x;
        int32_t dy = cursor_y - pending_click_y;
        if (dx >= -6 && dx <= 6 && dy >= -6 && dy <= 6)
            activate_pending_click(time_ms);
        else
            pending_click_action = CLICK_ACTION_NONE;
    }
    if (!dragging_id) return;
    for (uint64_t i = 0; i < count; i++) {
        syscall_window_info_t* info = &windows[i];
        if (info->id != dragging_id) continue;
        if (drag_mode == DRAG_MOVE) {
            uint32_t work_height = display.height - TASKBAR_HEIGHT;
            cached_window_t* entry = cache_for(info->id);
            if (cursor_x < SNAP_DISTANCE && cursor_y < SNAP_DISTANCE) {
                if (entry) save_bounds(entry, info);
                mos_window_set_flags(info->id,
                    info->flags & ~(SYSCALL_WINDOW_MAXIMIZED |
                                    SYSCALL_WINDOW_FULLSCREEN |
                                    SYSCALL_WINDOW_MINIMIZED));
                mos_window_set_bounds(info->id, 0, 0,
                    display.width / 2u - 2u,
                    work_height / 2u - TITLE_HEIGHT - 2u);
            } else if (cursor_x >= (int32_t)display.width - SNAP_DISTANCE &&
                       cursor_y < SNAP_DISTANCE) {
                if (entry) save_bounds(entry, info);
                mos_window_set_flags(info->id,
                    info->flags & ~(SYSCALL_WINDOW_MAXIMIZED |
                                    SYSCALL_WINDOW_FULLSCREEN |
                                    SYSCALL_WINDOW_MINIMIZED));
                mos_window_set_bounds(info->id, display.width / 2u, 0,
                    display.width / 2u - 2u,
                    work_height / 2u - TITLE_HEIGHT - 2u);
            } else if (cursor_x < SNAP_DISTANCE) {
                if (entry) save_bounds(entry, info);
                mos_window_set_flags(info->id,
                    info->flags & ~(SYSCALL_WINDOW_MAXIMIZED |
                                    SYSCALL_WINDOW_FULLSCREEN |
                                    SYSCALL_WINDOW_MINIMIZED));
                mos_window_set_bounds(info->id, 0, 0,
                    display.width / 2u - 2u,
                    work_height - TITLE_HEIGHT - 2u);
            } else if (cursor_x >= (int32_t)display.width - SNAP_DISTANCE) {
                if (entry) save_bounds(entry, info);
                mos_window_set_flags(info->id,
                    info->flags & ~(SYSCALL_WINDOW_MAXIMIZED |
                                    SYSCALL_WINDOW_FULLSCREEN |
                                    SYSCALL_WINDOW_MINIMIZED));
                mos_window_set_bounds(info->id, display.width / 2u, 0,
                    display.width / 2u - 2u,
                    work_height - TITLE_HEIGHT - 2u);
            } else if (cursor_y < SNAP_DISTANCE) {
                set_window_mode(info, entry, MODE_MAXIMIZED);
            } else if (window_mode(info) != MODE_NORMAL) {
                set_window_mode(info, entry, MODE_NORMAL);
            }
        }
        break;
    }
    dragging_id = 0;
    drag_mode = DRAG_NONE;
}

static void route_input(const syscall_input_event_t* input,
                        syscall_window_info_t* windows, uint64_t count) {
    if (input->type == SYSCALL_INPUT_MOUSE_MOVE) debug_mouse_events++;
    if (auth_state != AUTH_NONE) {
        if (input->type == SYSCALL_INPUT_MOUSE_MOVE) {
            int32_t scaled_x =
                input->dx * (int32_t)mouse_speed + mouse_remainder_x;
            int32_t scaled_y =
                input->dy * (int32_t)mouse_speed + mouse_remainder_y;
            cursor_x += scaled_x / 100;
            cursor_y += scaled_y / 100;
            mouse_remainder_x = scaled_x % 100;
            mouse_remainder_y = scaled_y % 100;
            if (cursor_x < 0) cursor_x = 0;
            if (cursor_y < 0) cursor_y = 0;
            if ((uint32_t)cursor_x >= display.width)
                cursor_x = (int32_t)display.width - 1;
            if ((uint32_t)cursor_y >= display.height)
                cursor_y = (int32_t)display.height - 1;
        } else if (input->type == SYSCALL_INPUT_MOUSE_BUTTON) {
            bool was_down =
                (previous_buttons & SYSCALL_INPUT_BTN_LEFT) != 0;
            bool is_down =
                (input->buttons & SYSCALL_INPUT_BTN_LEFT) != 0;
            previous_buttons = input->buttons;
            if (!was_down && is_down) handle_auth_click();
        } else if (input->type == SYSCALL_INPUT_KEY_DOWN) {
            handle_auth_key(input);
        }
        return;
    }
    if (input->type == SYSCALL_INPUT_MOUSE_MOVE) {
        int32_t scaled_x = input->dx * (int32_t)mouse_speed + mouse_remainder_x;
        int32_t scaled_y = input->dy * (int32_t)mouse_speed + mouse_remainder_y;
        cursor_x += scaled_x / 100;
        cursor_y += scaled_y / 100;
        mouse_remainder_x = scaled_x % 100;
        mouse_remainder_y = scaled_y % 100;
        if (cursor_x < 0) cursor_x = 0;
        if (cursor_y < 0) cursor_y = 0;
        if ((uint32_t)cursor_x >= display.width)
            cursor_x = (int32_t)display.width - 1;
        if ((uint32_t)cursor_y >= display.height)
            cursor_y = (int32_t)display.height - 1;
        if (menu_open || dialog_kind != DIALOG_NONE) return;
        move_dragged_window(windows, count);
        syscall_window_info_t* under =
            window_at(windows, count, cursor_x, cursor_y);
        if (under) {
            syscall_window_event_t event = {
                .type = SYSCALL_WINDOW_EVENT_MOUSE,
                .x = cursor_x - (int32_t)under->x - 1,
                .y = cursor_y - (int32_t)under->y -
                     (int32_t)TITLE_HEIGHT - 1,
                .buttons = input->buttons,
                .mods = input->mods,
                .time_ms = input->time_ms
            };
            if (event.y >= 0 &&
                mos_window_post_event(under->id, &event) != SYS_SUCCESS)
                strcpy(status_message, "Unable to deliver mouse movement");
        }
    } else if (input->type == SYSCALL_INPUT_MOUSE_BUTTON) {
        bool was_down =
            (previous_buttons & SYSCALL_INPUT_BTN_LEFT) != 0;
        bool is_down = (input->buttons & SYSCALL_INPUT_BTN_LEFT) != 0;
        bool right_was_down =
            (previous_buttons & SYSCALL_MOUSE_BTN_RIGHT) != 0;
        bool right_is_down = (input->buttons & SYSCALL_MOUSE_BTN_RIGHT) != 0;
        bool right_pressed = !right_was_down && right_is_down;
        previous_buttons = input->buttons;
        if (dialog_kind != DIALOG_NONE) {
            if (!was_down && is_down) dialog_click();
            return;
        }
        if (menu_open) {
            /* A click picks an item or dismisses the menu; it never falls
             * through to whatever is underneath. */
            if (!was_down && is_down) {
                uint32_t item = menu_item_at(cursor_x, cursor_y);
                menu_open = false;
                if (item != UINT32_MAX && menu_items[item].enabled)
                    run_menu_action(menu_items[item].action);
                return;
            }
            if (!right_pressed) return;
            menu_open = false;
        }
        syscall_window_info_t* under =
            window_at(windows, count, cursor_x, cursor_y);
        if (under) {
            focus_window(under);
            syscall_window_event_t event = {
                .type = SYSCALL_WINDOW_EVENT_MOUSE,
                .x = cursor_x - (int32_t)under->x - 1,
                .y = cursor_y - (int32_t)under->y -
                     (int32_t)TITLE_HEIGHT - 1,
                .buttons = input->buttons,
                .mods = input->mods,
                .time_ms = input->time_ms
            };
            if (event.y >= 0) mos_window_post_event(under->id, &event);
        }
        if (!was_down && is_down) on_mouse_down(windows, count);
        else if (was_down && !is_down)
            on_mouse_up(windows, count, input->time_ms);
        if (right_pressed) on_right_down(windows, count);
    } else if (input->type == SYSCALL_INPUT_MOUSE_WHEEL) {
        if (menu_open || dialog_kind != DIALOG_NONE) return;
        syscall_window_info_t* under =
            window_at(windows, count, cursor_x, cursor_y);
        if (under && under->id == browser_window_id) {
            uint32_t visible_rows = browser_visible_rows(browser_height);
            uint32_t max_scroll = browser_entry_count > visible_rows
                ? browser_entry_count - visible_rows : 0u;
            uint32_t step = max_scroll < 3u ? max_scroll : 3u;
            if (input->wheel < 0)
                browser_scroll = browser_scroll + step > max_scroll
                    ? max_scroll : browser_scroll + step;
            else if (input->wheel > 0)
                browser_scroll = browser_scroll > step
                    ? browser_scroll - step : 0u;
            last_click_valid = false;
            return;
        }
        if (under) {
            syscall_window_event_t event = {
                .type = SYSCALL_WINDOW_EVENT_MOUSE_WHEEL,
                .x = cursor_x - (int32_t)under->x - 1,
                .y = cursor_y - (int32_t)under->y -
                     (int32_t)TITLE_HEIGHT - 1,
                .wheel = input->wheel,
                .buttons = input->buttons,
                .mods = input->mods,
                .time_ms = input->time_ms
            };
            if (event.y >= 0) mos_window_post_event(under->id, &event);
        }
    } else if (input->type == SYSCALL_INPUT_KEY_DOWN ||
               input->type == SYSCALL_INPUT_KEY_UP) {
        if (dialog_kind != DIALOG_NONE) {
            if (input->type == SYSCALL_INPUT_KEY_DOWN) dialog_key(input);
            return;
        }
        if (menu_open) {
            if (input->type == SYSCALL_INPUT_KEY_DOWN && input->key == 41u)
                menu_open = false;
            return;
        }
        if (!focused_id) return;
        syscall_window_event_t event = {
            .type = SYSCALL_WINDOW_EVENT_KEY,
            .key = input->key,
            .buttons = input->type == SYSCALL_INPUT_KEY_DOWN,
            .mods = input->mods,
            .time_ms = input->time_ms
        };
        if (mos_window_post_event(focused_id, &event) == SYS_ERR_BUSY)
            strcpy(status_message, "Window event queue is full");
    }
}

static void cleanup(void) {
    if (screen_owned) {
        mos_graphics_release();
        screen_owned = false;
    }
    wm_free(canvas);
    canvas = NULL;
    wm_free(wallpaper);
    wallpaper = NULL;
    wm_free(desktop_layer);
    desktop_layer = NULL;
    wm_free(wallpaper_source);
    wallpaper_source = NULL;
    wm_free(font_regular_data);
    font_regular_data = NULL;
    wm_free(font_medium_data);
    font_medium_data = NULL;
    wm_free(cursor_animation_pixels);
    cursor_animation_pixels = NULL;
    for (uint32_t i = 0; i < MAX_WINDOWS; i++) {
        wm_free(cache[i].pixels);
        memset(&cache[i], 0, sizeof(cache[i]));
    }
}

static void cleanup_and_exit(void) {
    cleanup();
    mos_exit();
}

int main(void) {
    if (mos_graphics_acquire() != SYS_SUCCESS) {
        log_message("mosWM: another process owns the display\n");
        return 1;
    }
    screen_owned = true;
    if (mos_window_register_manager() != SYS_SUCCESS) {
        mos_graphics_release();
        screen_owned = false;
        log_message("mosWM: this executable is not the registered window server\n");
        return 1;
    }
    mos_register_cleanup(cleanup_and_exit);
    if (mos_graphics_info(&display) != SYS_SUCCESS ||
        display.pixel_format != SYS_GRAPHICS_PIXEL_FORMAT_ARGB8888 ||
        display.width < 320 || display.height < TASKBAR_HEIGHT + 120u ||
        (size_t)display.width > SIZE_MAX / display.height / sizeof(uint32_t)) {
        log_message("mosWM: unsupported display size\n");
        cleanup();
        return 1;
    }
    canvas = wm_malloc((size_t)display.width * display.height *
                       sizeof(uint32_t));
    if (!canvas) {
        log_message("mosWM: unable to allocate compositor surface\n");
        cleanup();
        return 1;
    }
    wallpaper = wm_malloc((size_t)display.width * display.height *
                          sizeof(uint32_t));
    if (wallpaper)
        desktop_layer = wm_malloc((size_t)display.width * display.height *
                                  sizeof(uint32_t));
    cursor_x = (int32_t)(display.width / 2u);
    cursor_y = (int32_t)(display.height / 2u);
    load_desktop_settings();
    if (mos_syscall3(SYS_IS_DIR,
            (uint64_t)(uintptr_t)MOSDESK_USERS_PATH, 0, 0) != 1u &&
        mos_syscall3(SYS_MKDIR,
            (uint64_t)(uintptr_t)MOSDESK_USERS_PATH, 0, 0) != SYS_SUCCESS) {
        log_message("mosWM: unable to create 0:/users\n");
        cleanup();
        return 1;
    }
    load_desktop_apps();
    if (!desktop_settings.onboarding_complete)
        auth_state = AUTH_FIRST_CHOICE;
    else if (desktop_settings.login_enabled)
        auth_state = AUTH_LOGIN;
    painted_wallpaper_style = UINT32_MAX;
    load_fonts();
    syscall_window_info_t windows[MAX_WINDOWS];
    syscall_input_event_t input[32];

    for (;;) {
        bool refresh_icons = ++icon_poll_frames >= 30u;
        if (refresh_icons) icon_poll_frames = 0;
        if (++settings_poll_frames >= 30u &&
            desktop_icon_drag_index == UINT32_MAX) {
            settings_poll_frames = 0;
            load_desktop_settings();
        }
        debug_lap(PROFILE_SETTINGS);
        browser_render();
        debug_lap(PROFILE_FILES);
        uint64_t count = mos_window_list(windows, MAX_WINDOWS);
        if (count > MAX_WINDOWS) {
            strcpy(status_message, "Unable to enumerate windows");
            count = 0;
        }
        remove_missing_cache(windows, count);
        sync_window_modes(windows, count);
        focused_id = 0;
        for (uint64_t i = 0; i < count; i++) {
            cached_window_t* entry = cache_for(windows[i].id);
            if (windows[i].flags & SYSCALL_WINDOW_FOCUSED)
                focused_id = windows[i].id;
            /* A window that has just appeared takes the keyboard focus. */
            if (entry && !entry->announced) {
                entry->announced = true;
                if (windows[i].flags & SYSCALL_WINDOW_VISIBLE)
                    focus_window(&windows[i]);
            }
            refresh_window_icon(entry, refresh_icons);
            if (entry && !refresh_frame(entry, &windows[i]))
                strcpy(status_message, "Unable to read a window surface");
        }
        debug_lap(PROFILE_WINDOWS);

        uint64_t event_count = mos_input_read(input, 32, 0, NULL);
        if (event_count <= 32) {
            for (uint64_t i = 0; i < event_count; i++)
                route_input(&input[i], windows, count);
        } else if (event_count != SYS_ERR_AGAIN) {
            strcpy(status_message, "Input read failed");
        }
        debug_lap(PROFILE_INPUT);
        update_cursor_appearance(windows, count);
        update_hover(windows, count);
        update_clock();
        debug_lap(PROFILE_POINTER);
        render(windows, count);
        mos_sleep(16);
        debug_lap(PROFILE_SLEEP);
        debug_frame_end();
    }
}