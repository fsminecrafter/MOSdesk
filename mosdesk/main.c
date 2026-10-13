#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ttrend.h"
#include "minimalos.h"
#include "window.h"
#include "stdlib.h"
#include "string.h"
#include "settings.h"
#include "password_hash.h"
#include "input.h"
#include "../mosWM/file_icons.h"

#define SETTINGS_WIDTH 580u
#define SETTINGS_HEIGHT 340u
#define NAV_WIDTH 148u
#define NAV_TOP 52
#define NAV_PITCH 44
#define PASSWORD_MAX_LENGTH 63u

/* Page ids. The sidebar lists them in nav_order, not in numeric order. */
#define PAGE_APPEARANCE 0u
#define PAGE_MOUSE 1u
#define PAGE_CURSOR 2u
#define PAGE_LOGIN 3u
#define PAGE_DESKTOP 4u
#define PAGE_DEBUG 5u
#define PAGE_ABOUT 6u
#define PAGE_COUNT 7u

#define WALLPAPER_DIRECTORY "0:/programs/mosWM/assets/wallpapers"
#define THUMB_WIDTH 96u
#define THUMB_HEIGHT 60u
#define TILE_PITCH_X 108
#define TILE_PITCH_Y 86
#define MAX_PHOTOS 16u
#define CONTENT_TOP 43
#define SCROLLBAR_WIDTH 6
#define SCROLLBAR_GRAB 18
#define SCROLLBAR_MIN_THUMB 28

typedef struct {
    char path[MOSDESK_WALLPAPER_PATH_SIZE];
    char label[24];
    bool has_thumb;
    uint32_t pixels[THUMB_WIDTH * THUMB_HEIGHT];
} wallpaper_photo_t;

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t* pixels;
} settings_surface_t;

static settings_surface_t surface;
static mosdesk_settings_t preferences;
static uint32_t window_id;
static uint32_t category;
static bool password_editing;
static char password_entry[PASSWORD_MAX_LENGTH + 1u];
static uint32_t password_entry_length;
static char settings_message[64];
static bool needs_redraw = true;
static bool left_was_down;
static int32_t nav_scroll;
static uint32_t nav_hover = UINT32_MAX;
static int32_t appearance_scroll;
static bool scroll_dragging;
static int32_t scroll_grab_offset;
static wallpaper_photo_t* photos;
static uint32_t photo_count;
static bool photos_loaded;

static const uint32_t nav_order[PAGE_COUNT] = {
    PAGE_APPEARANCE, PAGE_MOUSE, PAGE_CURSOR, PAGE_LOGIN, PAGE_DESKTOP,
    PAGE_DEBUG, PAGE_ABOUT
};
static ttrend_font_t small_font = {1, 1, 0, 0};
static ttrend_font_t large_font = {2, 1, 1, 0};
static ttrend_font_t loaded_font;
static ttrend_font_t loaded_large_font;

static const uint32_t cursor_colors[] = {
    0xFFFFFFFFu, 0xFFFFE4A6u, 0xFF8EC5FFu, 0xFF55D6BEu
};

static const uint32_t accent_colors[] = {
    0xFF59A6D8u, 0xFF48B8A5u, 0xFFD28C54u,
    0xFF69A86Au, 0xFFD66B83u, 0xFFE1B64Cu
};
static const char* const theme_names[] = {
    "Blue", "Teal", "Copper", "Forest", "Rose", "Gold"};

static ttrend_surface_t text_target(void) {
    return (ttrend_surface_t){surface.pixels, surface.width,
                              surface.height, surface.width};
}

static bool resize_surface(uint32_t width, uint32_t height) {
    if (width < 360u || height < 220u ||
        (size_t)width > SIZE_MAX / height / sizeof(uint32_t))
        return false;
    uint32_t* pixels = (uint32_t*)realloc(surface.pixels,
        (size_t)width * height * sizeof(uint32_t));
    if (!pixels) return false;
    surface.pixels = pixels;
    surface.width = width;
    surface.height = height;
    return true;
}

static void fill_rect(int32_t x, int32_t y, int32_t width, int32_t height,
                      uint32_t color) {
    int64_t right = (int64_t)x + width;
    int64_t bottom = (int64_t)y + height;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (right > surface.width) right = surface.width;
    if (bottom > surface.height) bottom = surface.height;
    for (int32_t py = y; py < bottom; py++) {
        uint32_t* row = surface.pixels + (size_t)py * surface.width;
        for (int32_t px = x; px < right; px++) row[px] = color;
    }
}

static void draw_text(int32_t x, int32_t y, const char* text,
                      uint32_t color, bool large);

static void stroke_rect(int32_t x, int32_t y, int32_t width, int32_t height,
                        int32_t thickness, uint32_t color) {
    fill_rect(x, y, width, thickness, color);
    fill_rect(x, y + height - thickness, width, thickness, color);
    fill_rect(x, y, thickness, height, color);
    fill_rect(x + width - thickness, y, thickness, height, color);
}

static int32_t content_left(void) {
    return (int32_t)NAV_WIDTH + 22;
}

static int32_t content_width(void) {
    return (int32_t)surface.width - (int32_t)NAV_WIDTH - 44;
}

/* -------------------------------------------------------------- appearance */

/* "john-towner-3Kv48NS4WUU-unsplash.qoi" -> "John Towner" */
static void photo_label(char* output, size_t capacity, const char* name) {
    size_t used = 0;
    uint32_t words = 0;
    bool word_start = true;
    for (size_t i = 0; name[i] && name[i] != '.' && used + 1u < capacity;
         i++) {
        char character = name[i];
        if (character == '-' || character == '_') {
            if (++words >= 2u) break;
            output[used++] = ' ';
            word_start = true;
            continue;
        }
        if (word_start && character >= 'a' && character <= 'z')
            character = (char)(character - 32);
        word_start = false;
        output[used++] = character;
    }
    output[used] = '\0';
}

static bool read_file_exact(const char* path, void* output, size_t size) {
    long fd = (long)mos_syscall(SYS_OPEN, (long)(uintptr_t)path,
                                SYS_O_RDONLY, 0);
    if (fd < 0) return false;
    size_t total = 0;
    bool ok = (long)mos_syscall(SYS_SIZE, fd, 0, 0) == (long)size;
    while (ok && total < size) {
        long got = (long)mos_syscall(SYS_READ, fd,
            (long)(uintptr_t)((uint8_t*)output + total),
            (long)(size - total));
        if (got <= 0) ok = false;
        else total += (size_t)got;
    }
    mos_syscall(SYS_CLOSE, fd, 0, 0);
    return ok;
}

/* Lists the bundled photos and loads their small previews (the .thm files in thumbs/:
 * "MTHB", u16 width, u16 height, then 0xAARRGGBB pixels). */
static void load_photos(void) {
    if (photos_loaded) return;
    photos_loaded = true;
    static syscall_dirent_t entries[64];
    long count = (long)mos_syscall(SYS_LISTDIR,
        (long)(uintptr_t)WALLPAPER_DIRECTORY, (long)(uintptr_t)entries, 64);
    if (count <= 0) return;
    if (count > 64) count = 64;
    photos = (wallpaper_photo_t*)malloc(sizeof(wallpaper_photo_t) *
                                        MAX_PHOTOS);
    if (!photos) return;
    static uint8_t thumb_file[8u + THUMB_WIDTH * THUMB_HEIGHT * 4u];
    for (long i = 0; i < count && photo_count < MAX_PHOTOS; i++) {
        const char* name = entries[i].name;
        size_t length = strlen(name);
        if (entries[i].type == SYSCALL_DIRENT_TYPE_DIR || length < 5u ||
            strcmp(name + length - 4u, ".qoi") ||
            sizeof(WALLPAPER_DIRECTORY) + length + 12u >
                MOSDESK_WALLPAPER_PATH_SIZE)
            continue;
        wallpaper_photo_t* photo = &photos[photo_count++];
        memset(photo, 0, sizeof(*photo));
        strcpy(photo->path, WALLPAPER_DIRECTORY "/");
        strcpy(photo->path + sizeof(WALLPAPER_DIRECTORY), name);
        photo_label(photo->label, sizeof(photo->label), name);

        char thumb_path[MOSDESK_WALLPAPER_PATH_SIZE];
        strcpy(thumb_path, WALLPAPER_DIRECTORY "/thumbs/");
        size_t prefix = strlen(thumb_path);
        memcpy(thumb_path + prefix, name, length - 4u);
        strcpy(thumb_path + prefix + length - 4u, ".thm");
        if (read_file_exact(thumb_path, thumb_file, sizeof(thumb_file)) &&
            !memcmp(thumb_file, "MTHB", 4u) &&
            thumb_file[4] == THUMB_WIDTH && thumb_file[5] == 0u &&
            thumb_file[6] == THUMB_HEIGHT && thumb_file[7] == 0u) {
            memcpy(photo->pixels, thumb_file + 8u, sizeof(photo->pixels));
            photo->has_thumb = true;
        }
    }
}

/* A small rendition of the built-in Minimal-OS wallpaper `style`. */
static void draw_theme_tile(int32_t x, int32_t y, uint32_t style) {
    static const uint8_t colors[6][6] = {
        {9u, 20u, 38u, 31u, 104u, 158u},
        {7u, 27u, 31u, 20u, 111u, 105u},
        {35u, 19u, 12u, 139u, 79u, 39u},
        {12u, 31u, 20u, 83u, 132u, 76u},
        {37u, 17u, 25u, 166u, 72u, 104u},
        {39u, 30u, 12u, 155u, 119u, 47u}
    };
    const uint8_t* palette = colors[style < 6u ? style : 0u];
    for (uint32_t row = 0; row < THUMB_HEIGHT; row++) {
        int32_t py = y + (int32_t)row;
        if (py < 0 || py >= (int32_t)surface.height) continue;
        for (uint32_t column = 0; column < THUMB_WIDTH; column++) {
            int32_t px = x + (int32_t)column;
            if (px < 0 || px >= (int32_t)surface.width) continue;
            uint32_t t = (column * 255u / THUMB_WIDTH +
                          row * 255u / THUMB_HEIGHT) / 2u;
            uint32_t red = (palette[0] * (255u - t) + palette[3] * t) / 255u;
            uint32_t green = (palette[1] * (255u - t) + palette[4] * t) / 255u;
            uint32_t blue = (palette[2] * (255u - t) + palette[5] * t) / 255u;
            /* The hourglass: two bars, one long and two short diagonals. */
            int32_t lx = (int32_t)column - 58, ly = (int32_t)row - 14;
            bool logo = lx >= 0 && lx <= 30 && ly >= 0 && ly <= 30 &&
                (ly <= 1 || ly >= 29 || (lx + ly >= 29 && lx + ly <= 31) ||
                 (lx - ly >= -1 && lx - ly <= 1 && (ly <= 12 || ly >= 18)));
            if (logo) {
                red = (red * 3u + 220u) / 4u;
                green = (green * 3u + 235u) / 4u;
                blue = (blue * 3u + 247u) / 4u;
            }
            surface.pixels[(size_t)py * surface.width + (uint32_t)px] =
                0xFF000000u | red << 16 | green << 8 | blue;
        }
    }
}

static void draw_photo_tile(int32_t x, int32_t y,
                            const wallpaper_photo_t* photo) {
    if (!photo->has_thumb) {
        fill_rect(x, y, (int32_t)THUMB_WIDTH, (int32_t)THUMB_HEIGHT,
                  0xFFC9D3DAu);
        moswm_draw_file_icon_sized(surface.pixels, surface.width,
            surface.height, x + 32, y + 14, MOSWM_ICON_IMAGE, 32u);
        return;
    }
    for (uint32_t row = 0; row < THUMB_HEIGHT; row++) {
        int32_t py = y + (int32_t)row;
        if (py < 0 || py >= (int32_t)surface.height) continue;
        for (uint32_t column = 0; column < THUMB_WIDTH; column++) {
            int32_t px = x + (int32_t)column;
            if (px < 0 || px >= (int32_t)surface.width) continue;
            surface.pixels[(size_t)py * surface.width + (uint32_t)px] =
                photo->pixels[row * THUMB_WIDTH + column] | 0xFF000000u;
        }
    }
}

static uint32_t tile_columns(void) {
    int32_t columns = (content_width() + 12) / TILE_PITCH_X;
    return columns < 1 ? 1u : (uint32_t)columns;
}

static int32_t tile_rows(uint32_t tiles) {
    uint32_t columns = tile_columns();
    return (int32_t)((tiles + columns - 1u) / columns);
}

/* Vertical layout of the Appearance page, before scrolling is applied: the
 * accent swatches, then the wallpaper gallery (themes, photos, photo fit). */
static int32_t accent_y(void) { return 96; }
static int32_t wallpaper_themes_y(void) { return accent_y() + 102; }
static int32_t wallpaper_photos_y(void) {
    return wallpaper_themes_y() + tile_rows(6u) * TILE_PITCH_Y + 34;
}
static int32_t wallpaper_fit_y(void) {
    return wallpaper_photos_y() +
           (photo_count ? tile_rows(photo_count) * TILE_PITCH_Y : 24) + 34;
}
static int32_t appearance_page_height(void) { return wallpaper_fit_y() + 52; }

static int32_t appearance_max_scroll(void) {
    int32_t overflow = appearance_page_height() - (int32_t)surface.height;
    return overflow > 0 ? overflow : 0;
}

/* The scrollbar runs down the right edge of the page, below the header. */
static int32_t scroll_track_top(void) { return CONTENT_TOP + 6; }
static int32_t scroll_track_length(void) {
    return (int32_t)surface.height - CONTENT_TOP - 12;
}
static int32_t scroll_thumb_length(void) {
    int32_t track = scroll_track_length();
    int32_t thumb = track * (int32_t)surface.height / appearance_page_height();
    if (thumb < SCROLLBAR_MIN_THUMB) thumb = SCROLLBAR_MIN_THUMB;
    return thumb > track ? track : thumb;
}
static int32_t scroll_thumb_top(void) {
    int32_t max_scroll = appearance_max_scroll();
    if (max_scroll <= 0) return scroll_track_top();
    return scroll_track_top() +
           (scroll_track_length() - scroll_thumb_length()) *
               appearance_scroll / max_scroll;
}

static void draw_tile_frame(int32_t x, int32_t y, bool selected,
                            const char* label) {
    stroke_rect(x - 1, y - 1, (int32_t)THUMB_WIDTH + 2,
                (int32_t)THUMB_HEIGHT + 2, 1, 0xFFB5C1C9u);
    if (selected) {
        stroke_rect(x - 4, y - 4, (int32_t)THUMB_WIDTH + 8,
                    (int32_t)THUMB_HEIGHT + 8, 2, preferences.accent_color);
        fill_rect(x + (int32_t)THUMB_WIDTH - 20, y + 4, 16, 16,
                  preferences.accent_color);
        fill_rect(x + (int32_t)THUMB_WIDTH - 16, y + 12, 3, 3, 0xFFFFFFFFu);
        fill_rect(x + (int32_t)THUMB_WIDTH - 14, y + 13, 3, 3, 0xFFFFFFFFu);
        fill_rect(x + (int32_t)THUMB_WIDTH - 12, y + 10, 3, 3, 0xFFFFFFFFu);
        fill_rect(x + (int32_t)THUMB_WIDTH - 10, y + 8, 3, 3, 0xFFFFFFFFu);
    }
    draw_text(x, y + (int32_t)THUMB_HEIGHT + 7, label,
              selected ? 0xFF273640u : 0xFF667680u, false);
}

static void draw_appearance_page(void) {
    load_photos();
    static const char* const fit_names[4] = {
        "Fill", "Crop", "Stretch", "Rotate"};
    int32_t left = content_left();
    int32_t scroll = appearance_scroll;
    uint32_t columns = tile_columns();
    bool using_photo = preferences.wallpaper_path[0] != '\0';

    draw_text(left, accent_y() - 33 - scroll, "Desktop accent",
              0xFF34434Cu, true);
    for (uint32_t i = 0; i < 6u; i++) {
        int32_t x = left + (int32_t)i * 50;
        int32_t y = accent_y() - scroll;
        fill_rect(x, y, 30, 30, accent_colors[i]);
        if ((preferences.accent_color & 0x00FFFFFFu) ==
            (accent_colors[i] & 0x00FFFFFFu))
            fill_rect(x - 2, y - 2, 34, 2, 0xFF34434Cu);
        /* One label per swatch: proportional text cannot be aligned with
         * runs of spaces the way the monospace bitmap font was. */
        draw_text(x, y + 36, theme_names[i], 0xFF667680u, false);
    }
    draw_text(left, wallpaper_themes_y() - 33 - scroll,
              "Minimal-OS wallpapers", 0xFF34434Cu, true);
    for (uint32_t i = 0; i < 6u; i++) {
        int32_t x = left + (int32_t)(i % columns) * TILE_PITCH_X;
        int32_t y = wallpaper_themes_y() - scroll +
                    (int32_t)(i / columns) * TILE_PITCH_Y;
        draw_theme_tile(x, y, i);
        draw_tile_frame(x, y, !using_photo && preferences.wallpaper == i,
                        theme_names[i]);
    }
    draw_text(left, wallpaper_photos_y() - 33 - scroll, "Photo wallpapers",
              0xFF34434Cu, true);
    if (!photo_count)
        draw_text(left, wallpaper_photos_y() - scroll,
                  "No photos are installed.", 0xFF667680u, false);
    for (uint32_t i = 0; i < photo_count; i++) {
        int32_t x = left + (int32_t)(i % columns) * TILE_PITCH_X;
        int32_t y = wallpaper_photos_y() - scroll +
                    (int32_t)(i / columns) * TILE_PITCH_Y;
        draw_photo_tile(x, y, &photos[i]);
        draw_tile_frame(x, y, using_photo &&
            !strcmp(preferences.wallpaper_path, photos[i].path),
            photos[i].label);
    }
    draw_text(left, wallpaper_fit_y() - 33 - scroll, "Photo fit",
              0xFF34434Cu, true);
    for (uint32_t i = 0; i < 4u; i++) {
        int32_t x = left + (int32_t)i * 84 + (i == 3u ? 16 : 0);
        int32_t y = wallpaper_fit_y() - scroll;
        bool selected = i < 3u && preferences.wallpaper_mode == i;
        fill_rect(x, y, 76, 28,
                  selected ? preferences.accent_color : 0xFFDCE2E6u);
        draw_text(x + 14, y + 9, fit_names[i],
                  selected ? 0xFFFFFFFFu : 0xFF34434Cu, false);
    }

    /* Scrolled content must not show through the page header. */
    fill_rect((int32_t)NAV_WIDTH + 1, 0,
              (int32_t)surface.width - (int32_t)NAV_WIDTH - 1, CONTENT_TOP,
              0xFFF3F5F6u);
    if (appearance_max_scroll() > 0) {
        int32_t x = (int32_t)surface.width - SCROLLBAR_WIDTH - 5;
        fill_rect(x, scroll_track_top(), SCROLLBAR_WIDTH,
                  scroll_track_length(), 0xFFDDE4E9u);
        fill_rect(x, scroll_thumb_top(), SCROLLBAR_WIDTH,
                  scroll_thumb_length(),
                  scroll_dragging ? preferences.accent_color : 0xFF94A5B1u);
    }
}

/* Dragging the scrollbar thumb, or pressing the track to jump there. Returns
 * true while the pointer belongs to the scrollbar. */
static bool appearance_scroll_drag(int32_t x, int32_t y, bool pressed) {
    int32_t max_scroll = appearance_max_scroll();
    if (pressed && max_scroll > 0 && y >= CONTENT_TOP &&
        x >= (int32_t)surface.width - SCROLLBAR_GRAB) {
        int32_t offset = y - scroll_thumb_top();
        scroll_grab_offset = offset >= 0 && offset < scroll_thumb_length()
            ? offset : scroll_thumb_length() / 2;
        scroll_dragging = true;
    }
    if (!scroll_dragging) return false;
    int32_t travel = scroll_track_length() - scroll_thumb_length();
    int32_t position = y - scroll_grab_offset - scroll_track_top();
    if (position < 0) position = 0;
    if (position > travel) position = travel;
    appearance_scroll = travel > 0 ? position * max_scroll / travel : 0;
    return true;
}

static bool appearance_page_click(int32_t x, int32_t y) {
    int32_t left = content_left();
    int32_t page_y = y + appearance_scroll;
    uint32_t columns = tile_columns();
    if (y < CONTENT_TOP || x < left) return false;
    if (page_y >= accent_y() - 4 && page_y < accent_y() + 36) {
        int32_t index = (x - left) / 50;
        if (index >= 6) return false;
        preferences.accent_color = accent_colors[index];
        return true;
    }
    for (uint32_t i = 0; i < 6u + photo_count; i++) {
        uint32_t index = i < 6u ? i : i - 6u;
        int32_t tile_x = left + (int32_t)(index % columns) * TILE_PITCH_X;
        int32_t tile_y = (i < 6u ? wallpaper_themes_y()
                                 : wallpaper_photos_y()) +
                         (int32_t)(index / columns) * TILE_PITCH_Y;
        if (x < tile_x || x >= tile_x + (int32_t)THUMB_WIDTH ||
            page_y < tile_y || page_y >= tile_y + (int32_t)THUMB_HEIGHT + 20)
            continue;
        memset(preferences.wallpaper_path, 0,
               sizeof(preferences.wallpaper_path));
        if (i < 6u) {
            preferences.wallpaper = i;
            preferences.accent_color = accent_colors[i];
        } else {
            strcpy(preferences.wallpaper_path, photos[index].path);
        }
        return true;
    }
    if (page_y >= wallpaper_fit_y() && page_y < wallpaper_fit_y() + 28) {
        for (uint32_t i = 0; i < 4u; i++) {
            int32_t button_x = left + (int32_t)i * 84 + (i == 3u ? 16 : 0);
            if (x < button_x || x >= button_x + 76) continue;
            if (i < 3u) preferences.wallpaper_mode = i;
            else preferences.wallpaper_rotation =
                (preferences.wallpaper_rotation + 1u) % 4u;
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------------ sidebar */

static int32_t nav_max_scroll(void) {
    int32_t overflow = NAV_TOP + (int32_t)PAGE_COUNT * NAV_PITCH + 8 -
                       (int32_t)surface.height;
    return overflow > 0 ? overflow : 0;
}

static uint32_t nav_row_at(int32_t x, int32_t y) {
    if (x < 0 || x >= (int32_t)NAV_WIDTH || y < NAV_TOP - 4) return UINT32_MAX;
    int32_t row = (y + nav_scroll - NAV_TOP) / NAV_PITCH;
    if (y + nav_scroll < NAV_TOP || row < 0 || row >= (int32_t)PAGE_COUNT)
        return UINT32_MAX;
    return (uint32_t)row;
}

static void draw_about_page(void) {
    int32_t left = content_left();
    moswm_draw_file_icon_sized(surface.pixels, surface.width, surface.height,
                               left, 60, MOSWM_ICON_COMPUTER, 64u);
    draw_text(left + 80, 70, "MOSdesk", 0xFF273640u, true);
    draw_text(left + 80, 96, "Desktop and window manager for Minimal-OS",
              0xFF667680u, false);
    static const char* const lines[] = {
        "Right-click desktop icons and files for more actions.",
        "Scroll the sidebar when the window is too short for it.",
        "",
        "Typeface: Inter.  Cursors: Posy's Cursor by Michiel de Boer.",
        "Photos: Unsplash contributors, credited under each picture."
    };
    for (uint32_t i = 0; i < sizeof(lines) / sizeof(lines[0]); i++)
        draw_text(left, 150 + (int32_t)i * 20, lines[i], 0xFF4D5E69u, false);
}

static void draw_cursor_icon(int32_t x, int32_t y, uint32_t scale,
                             uint32_t fill_color) {
    static const char* const shape[] = {
        "#...........", "##..........", "#x#.........",
        "#xx#........", "#xxx#.......", "#xxxx#......",
        "#xxxxx#.....", "#xxxxxx#....", "#xxxxxxx#...",
        "#xxxxxxxx#..", "#xxxxx###...", "#xx#xx#......",
        "...#..#......", "...#..#......", "....##......."
    };
    for (uint32_t row = 0; row < sizeof(shape) / sizeof(shape[0]); row++) {
        for (uint32_t column = 0; column < 12u; column++) {
            uint32_t color = shape[row][column] == '#'
                ? 0xFF263640u : shape[row][column] == 'x'
                    ? fill_color : 0u;
            if (color)
                fill_rect(x + (int32_t)(column * scale),
                          y + (int32_t)(row * scale),
                          (int32_t)scale, (int32_t)scale, color);
        }
    }
}

static void draw_text(int32_t x, int32_t y, const char* text,
                      uint32_t color, bool large) {
    ttrend_surface_t target = text_target();
    ttrend_draw_text(&target, x, y, text, color,
                     large ? &large_font : &small_font);
}

static void draw_control_labels(void) {
    if (category == 1u) {
        char speed[5];
        uint32_t length;
        if (preferences.mouse_speed >= 100u) {
            speed[0] = (char)('0' + preferences.mouse_speed / 100u);
            speed[1] = (char)('0' + (preferences.mouse_speed / 10u) % 10u);
            speed[2] = (char)('0' + preferences.mouse_speed % 10u);
            length = 3u;
        } else {
            speed[0] = (char)('0' + preferences.mouse_speed / 10u);
            speed[1] = (char)('0' + preferences.mouse_speed % 10u);
            length = 2u;
        }
        speed[length] = '%';
        speed[length + 1u] = '\0';
        draw_text((int32_t)NAV_WIDTH + 22, 63, "Pointer speed",
                  0xFF34434Cu, true);
        draw_text((int32_t)NAV_WIDTH + 22, 129, speed,
                  0xFF667680u, false);
        draw_text((int32_t)NAV_WIDTH + 22, 176, "Drag to adjust",
                  0xFF667680u, false);
    } else if (category == 2u) {
        draw_text((int32_t)NAV_WIDTH + 22, 63, "Cursor fill",
                  0xFF34434Cu, true);
        draw_text((int32_t)NAV_WIDTH + 22, 159, "Size",
                  0xFF34434Cu, true);
        draw_text((int32_t)NAV_WIDTH + 29, 201, "100%",
                  0xFF34434Cu, false);
        draw_text((int32_t)NAV_WIDTH + 85, 201, "200%",
                  0xFF34434Cu, false);
        draw_text((int32_t)NAV_WIDTH + 22, 220, "Pack",
                  0xFF34434Cu, false);
        draw_text((int32_t)NAV_WIDTH + 43, 243, "Posy",
                  preferences.cursor_pack == MOSDESK_CURSOR_PACK_POSY
                      ? 0xFFFFFFFFu : 0xFF34434Cu, false);
        draw_text((int32_t)NAV_WIDTH + 139, 243, "Classic",
                  preferences.cursor_pack == MOSDESK_CURSOR_PACK_CLASSIC
                      ? 0xFFFFFFFFu : 0xFF34434Cu, false);
    } else if (category == 3u) {
        draw_text((int32_t)NAV_WIDTH + 22, 63, "Password sign-in",
                  0xFF34434Cu, true);
        if (password_editing) {
            draw_text((int32_t)NAV_WIDTH + 22, 167,
                      "Enter a password of at least 4 characters.",
                      0xFF667680u, false);
            draw_text((int32_t)NAV_WIDTH + 22, 188, "Press Enter to save",
                      0xFF667680u, false);
        } else if (preferences.login_enabled) {
            /* Below the Change/Disable password buttons (y 106-185) so the
             * note no longer draws on top of the button labels. */
            draw_text((int32_t)NAV_WIDTH + 22, 199,
                      "Sign-in is required after restart.",
                      0xFF667680u, false);
        } else {
            draw_text((int32_t)NAV_WIDTH + 22, 161,
                      "The desktop opens automatically.",
                      0xFF667680u, false);
        }
    } else if (category == 4u) {
        draw_text((int32_t)NAV_WIDTH + 22, 63, "Click once to open / run",
                  0xFF34434Cu, true);
        draw_text((int32_t)NAV_WIDTH + 22, 161,
                  "Off uses double-click to open / run.",
                  0xFF667680u, false);
        draw_text((int32_t)NAV_WIDTH + 22, 195, "Desktop icon grid",
                  0xFF34434Cu, true);
        draw_text((int32_t)NAV_WIDTH + 22, 273,
                  "Off allows free placement.", 0xFF667680u, false);
    } else if (category == PAGE_DEBUG) {
        draw_text((int32_t)NAV_WIDTH + 22, 63, "FPS meter",
                  0xFF34434Cu, true);
        draw_text((int32_t)NAV_WIDTH + 22, 161,
                  "Frame, pointer and mouse-event rates, top right.",
                  0xFF667680u, false);
        draw_text((int32_t)NAV_WIDTH + 22, 195, "Profiler",
                  0xFF34434Cu, true);
        draw_text((int32_t)NAV_WIDTH + 22, 273,
                  "Time mosWM spends in each stage of a frame.",
                  0xFF667680u, false);
    }
}

static void draw_settings_message(void) {
    if (settings_message[0])
        draw_text((int32_t)NAV_WIDTH + 22,
                  (int32_t)surface.height - 22, settings_message,
                  0xFFB43A32u, false);
}

static void draw_window(void) {
    fill_rect(0, 0, (int32_t)surface.width, (int32_t)surface.height,
              0xFFF3F5F6u);
    if (nav_scroll > nav_max_scroll()) nav_scroll = nav_max_scroll();
    if (appearance_scroll > appearance_max_scroll())
        appearance_scroll = appearance_max_scroll();
    if (category == PAGE_APPEARANCE) draw_appearance_page();
    else if (category == PAGE_ABOUT) draw_about_page();

    /* Sidebar: rows scroll underneath the fixed title. */
    static const moswm_file_icon_t nav_icons[PAGE_COUNT] = {
        MOSWM_ICON_APPEARANCE, MOSWM_ICON_MOUSE, MOSWM_ICON_POINTER,
        MOSWM_ICON_LOCK, MOSWM_ICON_DESKTOP, MOSWM_ICON_TERMINAL,
        MOSWM_ICON_INFO};
    static const char* const nav_names[PAGE_COUNT] = {
        "Appearance", "Mouse", "Cursor", "Login", "Desktop", "Debug",
        "About"};
    fill_rect(0, 0, (int32_t)NAV_WIDTH, (int32_t)surface.height,
              0xFFE6EAEDu);
    for (uint32_t i = 0; i < PAGE_COUNT; i++) {
        uint32_t page = nav_order[i];
        int32_t row = NAV_TOP + (int32_t)i * NAV_PITCH - nav_scroll;
        if (row + NAV_PITCH <= NAV_TOP - 4 || row >= (int32_t)surface.height)
            continue;
        if (page == category) {
            fill_rect(10, row, 128, NAV_PITCH - 4, 0xFFD4E4EFu);
            fill_rect(10, row, 3, NAV_PITCH - 4, preferences.accent_color);
        } else if (nav_hover == i) {
            fill_rect(10, row, 128, NAV_PITCH - 4, 0xFFDCE4EAu);
        }
        moswm_draw_file_icon_sized(surface.pixels, surface.width,
                                   surface.height, 18, row + 3,
                                   nav_icons[page], 32u);
        draw_text(58, row + 15, nav_names[page], 0xFF34434Cu, false);
    }
    fill_rect(0, 0, (int32_t)NAV_WIDTH, NAV_TOP - 4, 0xFFE6EAEDu);
    draw_text(16, 15, "MOSdesk Settings", 0xFF273640u, true);
    if (nav_max_scroll() > 0) {
        int32_t track = (int32_t)surface.height - NAV_TOP - 8;
        int32_t thumb = track * track /
            ((int32_t)PAGE_COUNT * NAV_PITCH + 8);
        if (thumb < 20) thumb = 20;
        fill_rect((int32_t)NAV_WIDTH - 6, NAV_TOP + (track - thumb) *
                  nav_scroll / nav_max_scroll(), 3, thumb, 0xFFA9B8C3u);
    }
    fill_rect((int32_t)NAV_WIDTH, 0, 1, (int32_t)surface.height,
              0xFFD2D9DEu);
    fill_rect((int32_t)NAV_WIDTH + 14, 42,
              (int32_t)surface.width - (int32_t)NAV_WIDTH - 28,
              1, 0xFFD2D9DEu);
    if (category == PAGE_APPEARANCE) {
        draw_text(content_left(), 15, "Appearance", 0xFF273640u, true);
        draw_settings_message();
        return;
    }
    if (category == PAGE_ABOUT) {
        draw_text(content_left(), 15, "About", 0xFF273640u, true);
        return;
    }

    if (category == PAGE_DEBUG) {
        draw_text(content_left(), 15, "Debug", 0xFF273640u, true);
        fill_rect((int32_t)NAV_WIDTH + 22, 106, 160, 34,
                  preferences.debug_fps
                      ? preferences.accent_color : 0xFFDCE2E6u);
        fill_rect((int32_t)NAV_WIDTH + 22, 229, 160, 34,
                  preferences.debug_profiler
                      ? preferences.accent_color : 0xFFDCE2E6u);
    } else if (category == 1u) {
        int32_t track_x = (int32_t)NAV_WIDTH + 22;
        int32_t track_y = 101;
        int32_t track_width = (int32_t)surface.width - track_x - 24;
        int32_t marker = track_width *
            (int32_t)(preferences.mouse_speed - 50u) / 150;
        fill_rect(track_x, track_y, track_width, 5, 0xFFBCC6CDu);
        fill_rect(track_x, track_y, marker, 5, preferences.accent_color);
        fill_rect(track_x + marker - 5, track_y - 5, 11, 16,
                  preferences.accent_color);
    } else if (category == 2u) {
        draw_cursor_icon((int32_t)surface.width - 52, 58, 2u,
                         preferences.cursor_fill);
        for (uint32_t i = 0; i < 4; i++) {
            int32_t x = (int32_t)NAV_WIDTH + 22 + (int32_t)i * 37;
            fill_rect(x, 106, 25, 25, cursor_colors[i]);
            if ((preferences.cursor_fill & 0x00FFFFFFu) ==
                (cursor_colors[i] & 0x00FFFFFFu))
                fill_rect(x - 2, 104, 29, 2, preferences.accent_color);
        }
        fill_rect((int32_t)NAV_WIDTH + 22, 191, 48, 27, 0xFFDCE2E6u);
        fill_rect((int32_t)NAV_WIDTH + 78, 191, 48, 27, 0xFFDCE2E6u);
        if (preferences.cursor_scale == 1u)
            fill_rect((int32_t)NAV_WIDTH + 22, 215, 48, 3,
                      preferences.accent_color);
        else
            fill_rect((int32_t)NAV_WIDTH + 78, 215, 48, 3,
                      preferences.accent_color);
        fill_rect((int32_t)NAV_WIDTH + 22, 234, 84, 28,
                  preferences.cursor_pack == MOSDESK_CURSOR_PACK_POSY
                      ? preferences.accent_color : 0xFFDCE2E6u);
        fill_rect((int32_t)NAV_WIDTH + 116, 234, 84, 28,
                  preferences.cursor_pack == MOSDESK_CURSOR_PACK_CLASSIC
                      ? preferences.accent_color : 0xFFDCE2E6u);
    } else if (category == 3u) {
        if (password_editing) {
            fill_rect((int32_t)NAV_WIDTH + 22, 106,
                      (int32_t)surface.width - (int32_t)NAV_WIDTH - 44,
                      38, 0xFFFFFFFFu);
            fill_rect((int32_t)NAV_WIDTH + 22, 106,
                      (int32_t)surface.width - (int32_t)NAV_WIDTH - 44,
                      1, 0xFFBCC6CDu);
            fill_rect((int32_t)NAV_WIDTH + 22, 143,
                      (int32_t)surface.width - (int32_t)NAV_WIDTH - 44,
                      1, 0xFFBCC6CDu);
            char masked[PASSWORD_MAX_LENGTH + 1u];
            for (uint32_t i = 0; i < password_entry_length; i++)
                masked[i] = '*';
            masked[password_entry_length] = '\0';
            draw_text((int32_t)NAV_WIDTH + 34, 118, masked,
                      0xFF34434Cu, false);
        } else if (preferences.login_enabled) {
            fill_rect((int32_t)NAV_WIDTH + 22, 106, 160, 34,
                      preferences.accent_color);
            fill_rect((int32_t)NAV_WIDTH + 22, 151, 160, 34,
                      0xFFDCE2E6u);
        } else {
            fill_rect((int32_t)NAV_WIDTH + 22, 106, 160, 34,
                      preferences.accent_color);
        }
    } else {
        fill_rect((int32_t)NAV_WIDTH + 22, 106, 160, 34,
                  preferences.click_once_open
                      ? preferences.accent_color : 0xFFDCE2E6u);
        fill_rect((int32_t)NAV_WIDTH + 22, 229, 160, 34,
                  preferences.grid_enabled
                      ? preferences.accent_color : 0xFFDCE2E6u);
    }
    draw_control_labels();
    if (category == 3u && !password_editing) {
        draw_text((int32_t)NAV_WIDTH + 33, 117,
                  preferences.login_enabled ? "Change password"
                                            : "Set password",
                  0xFFFFFFFFu, false);
        if (preferences.login_enabled)
            draw_text((int32_t)NAV_WIDTH + 48, 162, "Disable password",
                      0xFF34434Cu, false);
    } else if (category == 4u || category == PAGE_DEBUG) {
        bool first = category == 4u ? preferences.click_once_open
                                    : preferences.debug_fps;
        bool second = category == 4u ? preferences.grid_enabled
                                     : preferences.debug_profiler;
        draw_text((int32_t)NAV_WIDTH + 33, 117, first ? "On" : "Off",
                  first ? 0xFFFFFFFFu : 0xFF34434Cu, false);
        draw_text((int32_t)NAV_WIDTH + 33, 240, second ? "On" : "Off",
                  second ? 0xFFFFFFFFu : 0xFF34434Cu, false);
    }
    draw_settings_message();
}

static void save_preferences(const mosdesk_settings_t* previous) {
    if (!mosdesk_settings_save(&preferences)) {
        preferences = *previous;
        strcpy(settings_message, "Unable to save settings.");
        return;
    }
    settings_message[0] = '\0';
}

static void save_login_password(void) {
    if (password_entry_length < 4u) {
        strcpy(settings_message, "Use at least 4 characters.");
        return;
    }
    mosdesk_settings_t updated = preferences;
    if (mos_syscall(SYS_RANDOM,
            (long)(uintptr_t)updated.password_salt,
            (long)sizeof(updated.password_salt), 0) !=
        (long)sizeof(updated.password_salt)) {
        strcpy(settings_message, "Could not create password salt.");
        return;
    }
    if (!mosdesk_password_hash(updated.password_salt,
            password_entry, updated.password_hash)) {
        strcpy(settings_message, "Could not set password.");
        return;
    }
    updated.login_enabled = 1u;
    updated.onboarding_complete = 1u;
    if (!mosdesk_settings_save(&updated)) {
        strcpy(settings_message, "Unable to save password settings.");
        return;
    }
    preferences = updated;
    password_editing = false;
    password_entry_length = 0;
    memset(password_entry, 0, sizeof(password_entry));
    settings_message[0] = '\0';
}

static void handle_wheel(const syscall_window_event_t* event) {
    int32_t step = event->wheel < 0 ? 44 : -44;
    if (event->x < (int32_t)NAV_WIDTH) {
        nav_scroll += step;
        if (nav_scroll < 0) nav_scroll = 0;
        if (nav_scroll > nav_max_scroll()) nav_scroll = nav_max_scroll();
    } else if (category == PAGE_APPEARANCE) {
        appearance_scroll += step;
        if (appearance_scroll < 0) appearance_scroll = 0;
        if (appearance_scroll > appearance_max_scroll())
            appearance_scroll = appearance_max_scroll();
    }
}

static void handle_mouse(const syscall_window_event_t* event) {
    bool left_down = (event->buttons & SYSCALL_MOUSE_BTN_LEFT) != 0;
    bool pressed = left_down && !left_was_down;
    left_was_down = left_down;
    nav_hover = nav_row_at(event->x, event->y);
    if (!left_down) {
        scroll_dragging = false;
        return;
    }
    mosdesk_settings_t previous = preferences;
    int32_t x = event->x;
    int32_t y = event->y;
    if (category == PAGE_APPEARANCE && appearance_scroll_drag(x, y, pressed))
        return;
    /* Only the pointer-speed slider follows a held button; everything else
     * acts once per click. */
    if (!pressed && !(category == 1u && x >= (int32_t)NAV_WIDTH)) return;
    if (x < (int32_t)NAV_WIDTH) {
        uint32_t row = nav_row_at(x, y);
        if (row == UINT32_MAX) return;
        category = nav_order[row];
        password_editing = false;
        settings_message[0] = '\0';
        return;
    } else if (category == PAGE_APPEARANCE) {
        if (!appearance_page_click(x, y)) return;
    } else if (category == PAGE_ABOUT) {
        return;
    } else if (category == 1u && y >= 94 && y < 118) {
        int32_t track_x = (int32_t)NAV_WIDTH + 22;
        int32_t track_width = (int32_t)surface.width - track_x - 24;
        if (track_width < 1) return;
        int32_t relative = x - track_x;
        if (relative < 0) relative = 0;
        if (relative > track_width) relative = track_width;
        preferences.mouse_speed = 50u +
            (uint32_t)(relative * 150 / track_width);
    } else if (category == 2u && y >= 102 && y < 138) {
        int32_t index = (x - (int32_t)NAV_WIDTH - 22) / 37;
        if (index < 0 || index >= 4) return;
        preferences.cursor_fill = cursor_colors[index];
    } else if (category == 2u && y >= 188 && y < 222) {
        preferences.cursor_scale = x < (int32_t)NAV_WIDTH + 74 ? 1u : 2u;
    } else if (category == 2u && y >= 234 && y < 262) {
        if (x >= (int32_t)NAV_WIDTH + 22 &&
            x < (int32_t)NAV_WIDTH + 106)
            preferences.cursor_pack = MOSDESK_CURSOR_PACK_POSY;
        else if (x >= (int32_t)NAV_WIDTH + 116 &&
                 x < (int32_t)NAV_WIDTH + 200)
            preferences.cursor_pack = MOSDESK_CURSOR_PACK_CLASSIC;
        else
            return;
    } else if (category == 3u && !password_editing &&
               y >= 104 && y < 144 &&
               x >= (int32_t)NAV_WIDTH + 22 &&
               x < (int32_t)NAV_WIDTH + 182) {
        password_editing = true;
        password_entry_length = 0;
        memset(password_entry, 0, sizeof(password_entry));
        settings_message[0] = '\0';
        return;
    } else if (category == 3u && preferences.login_enabled &&
               !password_editing && y >= 149 && y < 189 &&
               x >= (int32_t)NAV_WIDTH + 22 &&
               x < (int32_t)NAV_WIDTH + 182) {
        preferences.login_enabled = 0u;
        memset(preferences.password_salt, 0,
               sizeof(preferences.password_salt));
        memset(preferences.password_hash, 0,
               sizeof(preferences.password_hash));
    } else if (category == 4u && y >= 104 && y < 144 &&
               x >= (int32_t)NAV_WIDTH + 22 &&
               x < (int32_t)NAV_WIDTH + 182) {
        preferences.click_once_open = !preferences.click_once_open;
    } else if (category == 4u && y >= 227 && y < 267 &&
               x >= (int32_t)NAV_WIDTH + 22 &&
               x < (int32_t)NAV_WIDTH + 182) {
        preferences.grid_enabled = !preferences.grid_enabled;
    } else if (category == PAGE_DEBUG && y >= 104 && y < 144 &&
               x >= (int32_t)NAV_WIDTH + 22 &&
               x < (int32_t)NAV_WIDTH + 182) {
        preferences.debug_fps = !preferences.debug_fps;
    } else if (category == PAGE_DEBUG && y >= 227 && y < 267 &&
               x >= (int32_t)NAV_WIDTH + 22 &&
               x < (int32_t)NAV_WIDTH + 182) {
        preferences.debug_profiler = !preferences.debug_profiler;
    } else {
        return;
    }
    save_preferences(&previous);
}

static void handle_key(const syscall_window_event_t* event) {
    if (category != 3u || !password_editing || !event->buttons) return;
    if (event->key == 40u) {
        save_login_password();
        return;
    }
    if (event->key == 41u) {
        password_editing = false;
        password_entry_length = 0;
        memset(password_entry, 0, sizeof(password_entry));
        settings_message[0] = '\0';
        return;
    }
    if (event->key == 42u) {
        if (password_entry_length)
            password_entry[--password_entry_length] = '\0';
        settings_message[0] = '\0';
        return;
    }
    char character = (char)mos_syscall(SYS_USB,
        SYS_USB_KEY_TO_ASCII, event->key,
        mosdesk_usb_modifiers(event->mods));
    if (character >= 32 && character <= 126 &&
        password_entry_length < PASSWORD_MAX_LENGTH) {
        password_entry[password_entry_length++] = character;
        password_entry[password_entry_length] = '\0';
        settings_message[0] = '\0';
    }
}

static void close_requested(uint32_t id) {
    mos_window_close(id);
    free(surface.pixels);
    surface.pixels = NULL;
    mos_exit(0);
}

static void load_fonts(void) {
    /* Prefer smooth OpenType text (Inter); fall back to the bitmap font.
     * For outline fonts ttrend_draw_text() puts y at the top of capitals, so
     * the layout below is the same for both. */
    if (ttrend_font_load_path(&loaded_font,
            "0:/programs/mosdesk/fonts/Inter-Regular.otf", 12u) == 0) {
        small_font = loaded_font;
        small_font.tracking = 0u;
        if (ttrend_font_load_path(&loaded_large_font,
                "0:/programs/mosdesk/fonts/Inter-Medium.otf", 16u) == 0) {
            large_font = loaded_large_font;
        } else {
            large_font = loaded_font;
            large_font.pixel_size = 16u;
        }
        large_font.tracking = 0u;
    } else if (ttrend_font_load_path(&loaded_font,
            "0:/programs/mosdesk/fonts/default.font", 1u) == 0) {
        small_font = loaded_font;
        large_font = loaded_font;
        large_font.scale = 2u;
        large_font.pixel_size = 2u;
        large_font.tracking = 1u;
        large_font.bold = 1u;
    }
}

int main(int argc, char** argv) {
    preferences = mosdesk_settings_load();
    load_fonts();
    for (int i = 1; i < argc; i++)
        if (argv[i] && !strcmp(argv[i], "wallpaper")) {
            /* The gallery is the second section of Appearance; draw_window()
             * clamps this once the surface exists. */
            category = PAGE_APPEARANCE;
            appearance_scroll = wallpaper_themes_y() - 45 - CONTENT_TOP;
        }

    window_id = mos_window_create(0, 72, 56, SETTINGS_WIDTH, SETTINGS_HEIGHT,
                                  "MOSdesk Settings");
    if (!window_id || !resize_surface(SETTINGS_WIDTH, SETTINGS_HEIGHT)) {
        if (window_id) mos_window_close(window_id);
        return 1;
    }
    mos_window_set_resizable(window_id, true);
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
    mos_window_set_style(window_id, style);

    for (;;) {
        syscall_window_event_t event = {0};
        while (mos_window_next_event(window_id, &event) == SYS_SUCCESS) {
            if (event.type == SYSCALL_WINDOW_EVENT_CLOSE_REQUEST) {
                close_requested(window_id);
                return 0;
            }
            if (event.type == SYSCALL_WINDOW_EVENT_RESIZED &&
                event.width && event.height)
                resize_surface((uint32_t)event.width,
                               (uint32_t)event.height);
            if (event.type == SYSCALL_WINDOW_EVENT_MOUSE) handle_mouse(&event);
            if (event.type == SYSCALL_WINDOW_EVENT_MOUSE_WHEEL)
                handle_wheel(&event);
            if (event.type == SYSCALL_WINDOW_EVENT_KEY) handle_key(&event);
            needs_redraw = true;
        }
        syscall_window_info_t info = {0};
        if (mos_window_get_info(window_id, &info) != SYS_SUCCESS) {
            free(surface.pixels);
            return 1;
        }
        if (surface.width != info.width || surface.height != info.height) {
            resize_surface(info.width, info.height);
            needs_redraw = true;
        }
        /* Drawing and presenting only when something changed keeps the
         * compositor from recompositing this window every frame. */
        if (needs_redraw) {
            needs_redraw = false;
            draw_window();
            mos_window_present(window_id, surface.pixels,
                surface.width * sizeof(uint32_t), 0, 0,
                surface.width, surface.height);
        }
        mos_sleep(16);
    }
}
