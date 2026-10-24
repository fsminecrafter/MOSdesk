#ifndef MOSDESK_SETTINGS_H
#define MOSDESK_SETTINGS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "syscall.h"

#ifndef SYS_CREATE
#define SYS_CREATE 34u
#endif
#ifndef SYS_O_RDWR
#define SYS_O_RDWR 1u
#endif
#ifndef SYS_DELETE
#define SYS_DELETE 20u
#endif
#ifndef SYS_MKDIR
#define SYS_MKDIR 14u
#endif
#ifndef SYS_IS_DIR
#define SYS_IS_DIR 10u
#endif
#ifndef MOSDESK_SYSCALL
#define MOSDESK_SYSCALL mos_syscall
#endif

#define MOSDESK_SETTINGS_PATH "0:/programs/mosdesk/settings.dat"
#define MOSDESK_USERS_PATH "0:/users"
#define MOSDESK_SETTINGS_MAGIC 0x4D445331u
#define MOSDESK_CURSOR_PACK_POSY 0u
#define MOSDESK_CURSOR_PACK_CLASSIC 1u
#define MOSDESK_PASSWORD_SALT_SIZE 16u
#define MOSDESK_PASSWORD_HASH_SIZE 32u
#define MOSDESK_WALLPAPER_PATH_SIZE 192u
/* Saved desktop icon positions, by icon: 0 Home, 1 System, then the items of
 * the user's Desktop folder. Up to v6 the installed applications sat in
 * slots 2-33 and the Desktop folder in 34-65; loading such a file moves the
 * Desktop folder positions down. Files from before the Desktop folder held
 * 34 slots. */
#define MOSDESK_DESKTOP_ICON_COUNT 66u
#define MOSDESK_DESKTOP_ICON_COUNT_V5 34u
#define MOSDESK_DESKTOP_ICON_FIRST_ITEM 2u
#define MOSDESK_DESKTOP_ICON_V6_FIRST_FILE 34u
/* Window and panel style, Settings > Appearance. */
#define MOSDESK_UI_STYLE_MODERN 0u
#define MOSDESK_UI_STYLE_AERO 1u
#define MOSDESK_USERNAME_SIZE 32u
#define MOSDESK_WALLPAPER_FILL 0u
#define MOSDESK_WALLPAPER_CROP 1u
#define MOSDESK_WALLPAPER_STRETCH 2u
/* Clock and date shown on the panel (Settings > Time). */
#define MOSDESK_DATE_DAY_MONTH 0u    /* 17 Oct */
#define MOSDESK_DATE_DMY 1u          /* 17/10/2026 */
#define MOSDESK_DATE_MDY 2u          /* 10/17/2026 */
#define MOSDESK_DATE_ISO 3u          /* 2026-10-17 */
#define MOSDESK_DATE_FORMAT_COUNT 4u
#define MOSDESK_UTC_OFFSET_MIN (-720)
#define MOSDESK_UTC_OFFSET_MAX 840
#define MOSDESK_TIME_URL_SIZE 192u
#define MOSDESK_TIME_POLL_MIN 5u
#define MOSDESK_TIME_POLL_MAX 60u
#define MOSDESK_AUDIO_OUTPUT_SIZE 40u
#define MOSDESK_TIME_DEFAULT_URL \
    "https://timeapi.io/api/v1/time/current/unix_ms"

typedef struct {
    uint32_t magic;
    uint32_t mouse_speed;
    uint32_t accent_color;
    uint32_t cursor_fill;
    uint32_t cursor_scale;
    uint32_t cursor_pack;
    uint32_t wallpaper;
    uint32_t onboarding_complete;
    uint32_t login_enabled;
    uint8_t password_salt[MOSDESK_PASSWORD_SALT_SIZE];
    uint8_t password_hash[MOSDESK_PASSWORD_HASH_SIZE];
    uint32_t click_once_open;
    uint32_t grid_enabled;
    uint32_t wallpaper_mode;
    uint32_t wallpaper_rotation;
    char wallpaper_path[MOSDESK_WALLPAPER_PATH_SIZE];
    uint32_t desktop_icon_x[MOSDESK_DESKTOP_ICON_COUNT];
    uint32_t desktop_icon_y[MOSDESK_DESKTOP_ICON_COUNT];
    uint32_t debug_fps;
    uint32_t debug_profiler;
    char username[MOSDESK_USERNAME_SIZE];
    uint32_t ui_style;
    /* Settings > Time. The system clock runs in UTC; the panel shows it
     * shifted by utc_offset_minutes. */
    uint32_t clock_24h;
    uint32_t date_format;
    int32_t utc_offset_minutes;
    uint32_t time_sync_enabled;
    uint32_t time_sync_interval;        /* seconds */
    char time_sync_url[MOSDESK_TIME_URL_SIZE];
    /* Sound (the panel's speaker menu). The output is kept by name, as
     * the order can change when hardware does. */
    uint32_t audio_volume;              /* 0-100 */
    uint32_t audio_muted;
    char audio_output[MOSDESK_AUDIO_OUTPUT_SIZE];
} mosdesk_settings_t;

/* The layout before the sound settings: a file of this size is the
 * leading part of mosdesk_settings_t. */
typedef struct {
    uint32_t magic;
    uint32_t mouse_speed;
    uint32_t accent_color;
    uint32_t cursor_fill;
    uint32_t cursor_scale;
    uint32_t cursor_pack;
    uint32_t wallpaper;
    uint32_t onboarding_complete;
    uint32_t login_enabled;
    uint8_t password_salt[MOSDESK_PASSWORD_SALT_SIZE];
    uint8_t password_hash[MOSDESK_PASSWORD_HASH_SIZE];
    uint32_t click_once_open;
    uint32_t grid_enabled;
    uint32_t wallpaper_mode;
    uint32_t wallpaper_rotation;
    char wallpaper_path[MOSDESK_WALLPAPER_PATH_SIZE];
    uint32_t desktop_icon_x[MOSDESK_DESKTOP_ICON_COUNT];
    uint32_t desktop_icon_y[MOSDESK_DESKTOP_ICON_COUNT];
    uint32_t debug_fps;
    uint32_t debug_profiler;
    char username[MOSDESK_USERNAME_SIZE];
    uint32_t ui_style;
    uint32_t clock_24h;
    uint32_t date_format;
    int32_t utc_offset_minutes;
    uint32_t time_sync_enabled;
    uint32_t time_sync_interval;
    char time_sync_url[MOSDESK_TIME_URL_SIZE];
} mosdesk_settings_v8_t;

/* easyui (Minimal-OS-SDK/include/easyui.h) reads ui_style straight from the
 * file at this offset, so apps can follow the Aero style; keep them equal. */
#define MOSDESK_SETTINGS_UI_STYLE_OFFSET 860u
_Static_assert(offsetof(mosdesk_settings_t, ui_style) ==
                   MOSDESK_SETTINGS_UI_STYLE_OFFSET,
               "easyui reads ui_style at this offset");

/* The layout before the clock and internet time settings: a file of this
 * size is the leading part of mosdesk_settings_t. */
typedef struct {
    uint32_t magic;
    uint32_t mouse_speed;
    uint32_t accent_color;
    uint32_t cursor_fill;
    uint32_t cursor_scale;
    uint32_t cursor_pack;
    uint32_t wallpaper;
    uint32_t onboarding_complete;
    uint32_t login_enabled;
    uint8_t password_salt[MOSDESK_PASSWORD_SALT_SIZE];
    uint8_t password_hash[MOSDESK_PASSWORD_HASH_SIZE];
    uint32_t click_once_open;
    uint32_t grid_enabled;
    uint32_t wallpaper_mode;
    uint32_t wallpaper_rotation;
    char wallpaper_path[MOSDESK_WALLPAPER_PATH_SIZE];
    uint32_t desktop_icon_x[MOSDESK_DESKTOP_ICON_COUNT];
    uint32_t desktop_icon_y[MOSDESK_DESKTOP_ICON_COUNT];
    uint32_t debug_fps;
    uint32_t debug_profiler;
    char username[MOSDESK_USERNAME_SIZE];
    uint32_t ui_style;
} mosdesk_settings_v7_t;

/* The layout before the window style, when applications still had desktop
 * icons (see MOSDESK_DESKTOP_ICON_COUNT). */
typedef struct {
    uint32_t magic;
    uint32_t mouse_speed;
    uint32_t accent_color;
    uint32_t cursor_fill;
    uint32_t cursor_scale;
    uint32_t cursor_pack;
    uint32_t wallpaper;
    uint32_t onboarding_complete;
    uint32_t login_enabled;
    uint8_t password_salt[MOSDESK_PASSWORD_SALT_SIZE];
    uint8_t password_hash[MOSDESK_PASSWORD_HASH_SIZE];
    uint32_t click_once_open;
    uint32_t grid_enabled;
    uint32_t wallpaper_mode;
    uint32_t wallpaper_rotation;
    char wallpaper_path[MOSDESK_WALLPAPER_PATH_SIZE];
    uint32_t desktop_icon_x[MOSDESK_DESKTOP_ICON_COUNT];
    uint32_t desktop_icon_y[MOSDESK_DESKTOP_ICON_COUNT];
    uint32_t debug_fps;
    uint32_t debug_profiler;
    char username[MOSDESK_USERNAME_SIZE];
} mosdesk_settings_v6_t;

/* The layout before the username and the Desktop folder icons. */
typedef struct {
    uint32_t magic;
    uint32_t mouse_speed;
    uint32_t accent_color;
    uint32_t cursor_fill;
    uint32_t cursor_scale;
    uint32_t cursor_pack;
    uint32_t wallpaper;
    uint32_t onboarding_complete;
    uint32_t login_enabled;
    uint8_t password_salt[MOSDESK_PASSWORD_SALT_SIZE];
    uint8_t password_hash[MOSDESK_PASSWORD_HASH_SIZE];
    uint32_t click_once_open;
    uint32_t grid_enabled;
    uint32_t wallpaper_mode;
    uint32_t wallpaper_rotation;
    char wallpaper_path[MOSDESK_WALLPAPER_PATH_SIZE];
    uint32_t desktop_icon_x[MOSDESK_DESKTOP_ICON_COUNT_V5];
    uint32_t desktop_icon_y[MOSDESK_DESKTOP_ICON_COUNT_V5];
    uint32_t debug_fps;
    uint32_t debug_profiler;
} mosdesk_settings_v5_t;

/* The layout before the debug switches were appended: a file of this size
 * is the leading part of mosdesk_settings_v5_t. */
typedef struct {
    uint32_t magic;
    uint32_t mouse_speed;
    uint32_t accent_color;
    uint32_t cursor_fill;
    uint32_t cursor_scale;
    uint32_t cursor_pack;
    uint32_t wallpaper;
    uint32_t onboarding_complete;
    uint32_t login_enabled;
    uint8_t password_salt[MOSDESK_PASSWORD_SALT_SIZE];
    uint8_t password_hash[MOSDESK_PASSWORD_HASH_SIZE];
    uint32_t click_once_open;
    uint32_t grid_enabled;
    uint32_t wallpaper_mode;
    uint32_t wallpaper_rotation;
    char wallpaper_path[MOSDESK_WALLPAPER_PATH_SIZE];
    uint32_t desktop_icon_x[MOSDESK_DESKTOP_ICON_COUNT_V5];
    uint32_t desktop_icon_y[MOSDESK_DESKTOP_ICON_COUNT_V5];
} mosdesk_settings_v4_t;

typedef struct {
    uint32_t magic;
    uint32_t mouse_speed;
    uint32_t accent_color;
    uint32_t cursor_fill;
    uint32_t cursor_scale;
    uint32_t cursor_pack;
    uint32_t wallpaper;
    uint32_t onboarding_complete;
    uint32_t login_enabled;
    uint8_t password_salt[MOSDESK_PASSWORD_SALT_SIZE];
    uint8_t password_hash[MOSDESK_PASSWORD_HASH_SIZE];
    uint32_t click_once_open;
} mosdesk_settings_v3_t;

typedef struct {
    uint32_t magic;
    uint32_t mouse_speed;
    uint32_t accent_color;
    uint32_t cursor_fill;
    uint32_t cursor_scale;
    uint32_t cursor_pack;
    uint32_t wallpaper;
    uint32_t onboarding_complete;
    uint32_t login_enabled;
    uint8_t password_salt[MOSDESK_PASSWORD_SALT_SIZE];
    uint8_t password_hash[MOSDESK_PASSWORD_HASH_SIZE];
} mosdesk_settings_v2_t;

typedef struct {
    uint32_t magic;
    uint32_t mouse_speed;
    uint32_t accent_color;
    uint32_t cursor_fill;
    uint32_t cursor_scale;
    uint32_t cursor_pack;
} mosdesk_settings_v1_t;

typedef struct {
    uint32_t magic;
    uint32_t mouse_speed;
    uint32_t accent_color;
    uint32_t cursor_fill;
    uint32_t cursor_scale;
} mosdesk_settings_v0_t;

/* ------------------------------------------------------------ user folders
 *
 * Everything that belongs to the user lives in 0:/users/<username>:
 *
 *     0:/users/<username>/Documents
 *     0:/users/<username>/Pictures
 *     0:/users/<username>/Desktop      (shown on the desktop)
 */
#define MOSDESK_USER_DOCUMENTS "Documents"
#define MOSDESK_USER_PICTURES "Pictures"
#define MOSDESK_USER_DESKTOP "Desktop"

/* 1 to 31 letters, digits, '-' or '_': safe as a folder name. */
static inline bool mosdesk_username_valid(const char* name) {
    if (!name || !name[0]) return false;
    uint32_t length = 0;
    for (; name[length]; length++) {
        char c = name[length];
        if (length >= MOSDESK_USERNAME_SIZE - 1u) return false;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_'))
            return false;
    }
    return true;
}

/* "0:/users/<username>" or, with a folder, "0:/users/<username>/<folder>".
 * Returns false when the name is not valid or the path does not fit. */
static inline bool mosdesk_user_path(char* output, size_t capacity,
                                     const char* username,
                                     const char* folder) {
    if (!output || !capacity || !mosdesk_username_valid(username))
        return false;
    const char* parts[4] = {MOSDESK_USERS_PATH, "/", username, NULL};
    size_t at = 0;
    for (uint32_t part = 0; part < 5u; part++) {
        const char* text = part < 3u ? parts[part]
            : part == 3u ? (folder ? "/" : NULL) : folder;
        if (!text) continue;
        for (; *text; text++) {
            if (at + 1u >= capacity) return false;
            output[at++] = *text;
        }
    }
    output[at] = '\0';
    return true;
}

static inline bool mosdesk_make_directory(const char* path) {
    if (MOSDESK_SYSCALL(SYS_IS_DIR, (uintptr_t)path, 0, 0) == 1u) return true;
    MOSDESK_SYSCALL(SYS_MKDIR, (uintptr_t)path, 0, 0);
    return MOSDESK_SYSCALL(SYS_IS_DIR, (uintptr_t)path, 0, 0) == 1u;
}

/* Creates 0:/users/<username> and its Documents, Pictures and Desktop
 * folders where they are missing. */
static inline bool mosdesk_user_prepare(const char* username) {
    static const char* const folders[] = {
        NULL, MOSDESK_USER_DOCUMENTS, MOSDESK_USER_PICTURES,
        MOSDESK_USER_DESKTOP
    };
    char path[MOSDESK_WALLPAPER_PATH_SIZE];
    if (!mosdesk_make_directory(MOSDESK_USERS_PATH)) return false;
    for (uint32_t i = 0; i < sizeof(folders) / sizeof(folders[0]); i++) {
        if (!mosdesk_user_path(path, sizeof(path), username, folders[i]) ||
            !mosdesk_make_directory(path))
            return false;
    }
    return true;
}

static inline void mosdesk_settings_time_defaults(mosdesk_settings_t* settings) {
    static const char url[] = MOSDESK_TIME_DEFAULT_URL;
    settings->clock_24h = 1u;
    settings->date_format = MOSDESK_DATE_DAY_MONTH;
    settings->utc_offset_minutes = 0;
    settings->time_sync_enabled = 0u;
    settings->time_sync_interval = 30u;
    for (uint32_t i = 0; i < MOSDESK_TIME_URL_SIZE; i++)
        settings->time_sync_url[i] = i < sizeof(url) ? url[i] : '\0';
}

static inline mosdesk_settings_t mosdesk_settings_default(void) {
    mosdesk_settings_t settings = {0};
    settings.magic = MOSDESK_SETTINGS_MAGIC;
    settings.mouse_speed = 100u;
    settings.accent_color = 0xFF3DAEE9u;
    settings.cursor_fill = 0xFFFFFFFFu;
    settings.cursor_scale = 1u;
    settings.cursor_pack = MOSDESK_CURSOR_PACK_POSY;
    settings.click_once_open = 0u;
    settings.grid_enabled = 1u;
    for (uint32_t i = 0; i < MOSDESK_DESKTOP_ICON_COUNT; i++) {
        settings.desktop_icon_x[i] = UINT32_MAX;
        settings.desktop_icon_y[i] = UINT32_MAX;
    }
    mosdesk_settings_time_defaults(&settings);
    settings.audio_volume = 80u;
    return settings;
}

static inline mosdesk_settings_t mosdesk_settings_load(void) {
    mosdesk_settings_t settings = mosdesk_settings_default();
    long fd = (long)MOSDESK_SYSCALL(SYS_OPEN,
        (uintptr_t)MOSDESK_SETTINGS_PATH, SYS_O_RDONLY, 0);
    if (fd < 0) return settings;
    long size = (long)MOSDESK_SYSCALL(SYS_SIZE, (uintptr_t)fd, 0, 0);
    if (size == (long)sizeof(settings)) {
        if ((long)MOSDESK_SYSCALL(SYS_READ, (uintptr_t)fd,
                (uintptr_t)&settings, sizeof(settings)) !=
            (long)sizeof(settings))
            settings = mosdesk_settings_default();
    } else if (size == (long)sizeof(mosdesk_settings_v8_t)) {
        /* v8 is the leading part; the sound settings keep their defaults. */
        if ((long)MOSDESK_SYSCALL(SYS_READ, (uintptr_t)fd,
                (uintptr_t)&settings, (uint64_t)size) != size)
            settings = mosdesk_settings_default();
    } else if (size == (long)sizeof(mosdesk_settings_v7_t)) {
        /* v7 is the leading part; the time settings keep their defaults. */
        if ((long)MOSDESK_SYSCALL(SYS_READ, (uintptr_t)fd,
                (uintptr_t)&settings, (uint64_t)size) != size)
            settings = mosdesk_settings_default();
    } else if (size == (long)sizeof(mosdesk_settings_v6_t)) {
        /* v6 is the leading part of the current layout; only the icon
         * slots change meaning (applications are no longer on the desktop,
         * so the Desktop folder items move down into their slots). */
        if ((long)MOSDESK_SYSCALL(SYS_READ, (uintptr_t)fd,
                (uintptr_t)&settings, (uint64_t)size) == size) {
            settings.ui_style = MOSDESK_UI_STYLE_MODERN;
            for (uint32_t i = MOSDESK_DESKTOP_ICON_FIRST_ITEM;
                 i < MOSDESK_DESKTOP_ICON_COUNT; i++) {
                uint32_t from = i - MOSDESK_DESKTOP_ICON_FIRST_ITEM +
                                MOSDESK_DESKTOP_ICON_V6_FIRST_FILE;
                bool moved = from < MOSDESK_DESKTOP_ICON_COUNT;
                settings.desktop_icon_x[i] =
                    moved ? settings.desktop_icon_x[from] : UINT32_MAX;
                settings.desktop_icon_y[i] =
                    moved ? settings.desktop_icon_y[from] : UINT32_MAX;
            }
        } else {
            settings = mosdesk_settings_default();
        }
    } else if (size == (long)sizeof(mosdesk_settings_v5_t) ||
               size == (long)sizeof(mosdesk_settings_v4_t)) {
        /* v4 is the leading part of v5; the debug switches stay off. */
        mosdesk_settings_v5_t legacy = {0};
        if ((long)MOSDESK_SYSCALL(SYS_READ, (uintptr_t)fd,
                (uintptr_t)&legacy, (uint64_t)size) == size) {
            /* Everything up to the icon positions has the same layout. */
            const uint8_t* from = (const uint8_t*)&legacy;
            uint8_t* to = (uint8_t*)&settings;
            for (size_t i = 0;
                 i < offsetof(mosdesk_settings_v5_t, desktop_icon_x); i++)
                to[i] = from[i];
            /* Only Home and System keep their places: the other slots held
             * applications, which are no longer on the desktop. */
            for (uint32_t i = 0; i < MOSDESK_DESKTOP_ICON_FIRST_ITEM; i++) {
                settings.desktop_icon_x[i] = legacy.desktop_icon_x[i];
                settings.desktop_icon_y[i] = legacy.desktop_icon_y[i];
            }
            settings.debug_fps = legacy.debug_fps;
            settings.debug_profiler = legacy.debug_profiler;
        } else {
            settings = mosdesk_settings_default();
        }
    } else if (size == (long)sizeof(mosdesk_settings_v3_t)) {
        mosdesk_settings_v3_t legacy;
        if ((long)MOSDESK_SYSCALL(SYS_READ, (uintptr_t)fd,
                (uintptr_t)&legacy, sizeof(legacy)) ==
            (long)sizeof(legacy)) {
            settings.magic = legacy.magic;
            settings.mouse_speed = legacy.mouse_speed;
            settings.accent_color = legacy.accent_color;
            settings.cursor_fill = legacy.cursor_fill;
            settings.cursor_scale = legacy.cursor_scale;
            settings.cursor_pack = legacy.cursor_pack;
            settings.wallpaper = legacy.wallpaper;
            settings.onboarding_complete = legacy.onboarding_complete;
            settings.login_enabled = legacy.login_enabled;
            for (uint32_t i = 0; i < MOSDESK_PASSWORD_SALT_SIZE; i++)
                settings.password_salt[i] = legacy.password_salt[i];
            for (uint32_t i = 0; i < MOSDESK_PASSWORD_HASH_SIZE; i++)
                settings.password_hash[i] = legacy.password_hash[i];
            settings.click_once_open = legacy.click_once_open;
        } else {
            settings = mosdesk_settings_default();
        }
    } else if (size == (long)sizeof(mosdesk_settings_v2_t)) {
        mosdesk_settings_v2_t legacy;
        if ((long)MOSDESK_SYSCALL(SYS_READ, (uintptr_t)fd,
                (uintptr_t)&legacy, sizeof(legacy)) ==
            (long)sizeof(legacy)) {
            settings.magic = legacy.magic;
            settings.mouse_speed = legacy.mouse_speed;
            settings.accent_color = legacy.accent_color;
            settings.cursor_fill = legacy.cursor_fill;
            settings.cursor_scale = legacy.cursor_scale;
            settings.cursor_pack = legacy.cursor_pack;
            settings.wallpaper = legacy.wallpaper;
            settings.onboarding_complete = legacy.onboarding_complete;
            settings.login_enabled = legacy.login_enabled;
            for (uint32_t i = 0; i < MOSDESK_PASSWORD_SALT_SIZE; i++)
                settings.password_salt[i] = legacy.password_salt[i];
            for (uint32_t i = 0; i < MOSDESK_PASSWORD_HASH_SIZE; i++)
                settings.password_hash[i] = legacy.password_hash[i];
        } else {
            settings = mosdesk_settings_default();
        }
    } else if (size == (long)sizeof(mosdesk_settings_v1_t)) {
        mosdesk_settings_v1_t legacy;
        if ((long)MOSDESK_SYSCALL(SYS_READ, (uintptr_t)fd,
                (uintptr_t)&legacy, sizeof(legacy)) ==
            (long)sizeof(legacy)) {
            settings.magic = legacy.magic;
            settings.mouse_speed = legacy.mouse_speed;
            settings.accent_color = legacy.accent_color;
            settings.cursor_fill = legacy.cursor_fill;
            settings.cursor_scale = legacy.cursor_scale;
            settings.cursor_pack = legacy.cursor_pack;
            settings.onboarding_complete = 1u;
        } else {
            settings = mosdesk_settings_default();
        }
    } else if (size == (long)sizeof(mosdesk_settings_v0_t)) {
        mosdesk_settings_v0_t legacy;
        if ((long)MOSDESK_SYSCALL(SYS_READ, (uintptr_t)fd,
                (uintptr_t)&legacy, sizeof(legacy)) ==
            (long)sizeof(legacy)) {
            settings.magic = legacy.magic;
            settings.mouse_speed = legacy.mouse_speed;
            settings.accent_color = legacy.accent_color;
            settings.cursor_fill = legacy.cursor_fill;
            settings.cursor_scale = legacy.cursor_scale;
            settings.onboarding_complete = 1u;
        } else {
            settings = mosdesk_settings_default();
        }
    }
    MOSDESK_SYSCALL(SYS_CLOSE, (uintptr_t)fd, 0, 0);
    if (settings.magic != MOSDESK_SETTINGS_MAGIC ||
        settings.mouse_speed < 50u || settings.mouse_speed > 200u ||
        (settings.cursor_scale != 1u && settings.cursor_scale != 2u) ||
        settings.cursor_pack > MOSDESK_CURSOR_PACK_CLASSIC ||
        settings.wallpaper > 5u ||
        settings.grid_enabled > 1u ||
        settings.debug_fps > 1u || settings.debug_profiler > 1u ||
        settings.wallpaper_mode > MOSDESK_WALLPAPER_STRETCH ||
        settings.wallpaper_path[MOSDESK_WALLPAPER_PATH_SIZE - 1u] != '\0' ||
        settings.onboarding_complete > 1u ||
        settings.login_enabled > 1u ||
        settings.click_once_open > 1u ||
        settings.ui_style > MOSDESK_UI_STYLE_AERO ||
        settings.username[MOSDESK_USERNAME_SIZE - 1u] != '\0')
        return mosdesk_settings_default();
    if (settings.clock_24h > 1u ||
        settings.date_format >= MOSDESK_DATE_FORMAT_COUNT ||
        settings.utc_offset_minutes < MOSDESK_UTC_OFFSET_MIN ||
        settings.utc_offset_minutes > MOSDESK_UTC_OFFSET_MAX ||
        settings.time_sync_enabled > 1u ||
        settings.time_sync_interval < MOSDESK_TIME_POLL_MIN ||
        settings.time_sync_interval > MOSDESK_TIME_POLL_MAX ||
        settings.time_sync_url[MOSDESK_TIME_URL_SIZE - 1u] != '\0')
        mosdesk_settings_time_defaults(&settings);
    if (settings.audio_volume > 100u || settings.audio_muted > 1u ||
        settings.audio_output[MOSDESK_AUDIO_OUTPUT_SIZE - 1u] != '\0') {
        settings.audio_volume = 80u;
        settings.audio_muted = 0u;
        settings.audio_output[0] = '\0';
    }
    /* An unusable name is asked for again rather than used as a path. */
    if (!mosdesk_username_valid(settings.username))
        settings.username[0] = '\0';
    settings.accent_color |= 0xFF000000u;
    settings.cursor_fill |= 0xFF000000u;
    return settings;
}

static inline bool mosdesk_settings_save(const mosdesk_settings_t* settings) {
    if (!settings) return false;
    long fd = (long)MOSDESK_SYSCALL(SYS_OPEN,
        (uintptr_t)MOSDESK_SETTINGS_PATH, SYS_O_RDWR, 0);
    if (fd >= 0) {
        long size = (long)MOSDESK_SYSCALL(SYS_SIZE, (uintptr_t)fd, 0, 0);
        if (size != (long)sizeof(*settings)) {
            MOSDESK_SYSCALL(SYS_CLOSE, (uintptr_t)fd, 0, 0);
            if (MOSDESK_SYSCALL(SYS_DELETE,
                    (uintptr_t)MOSDESK_SETTINGS_PATH, 0, 0) != SYS_SUCCESS)
                return false;
            fd = -1;
        }
    }
    if (fd < 0) {
        MOSDESK_SYSCALL(SYS_CREATE,
            (uintptr_t)MOSDESK_SETTINGS_PATH,
            (uintptr_t)"data", (uintptr_t)"bin");
        fd = (long)MOSDESK_SYSCALL(SYS_OPEN,
            (uintptr_t)MOSDESK_SETTINGS_PATH, SYS_O_RDWR, 0);
    }
    if (fd < 0) return false;
    if (MOSDESK_SYSCALL(SYS_SEEK, (uintptr_t)fd, 0, 0) != SYS_SUCCESS) {
        MOSDESK_SYSCALL(SYS_CLOSE, (uintptr_t)fd, 0, 0);
        return false;
    }
    long written = (long)MOSDESK_SYSCALL(SYS_FWRITE, (uintptr_t)fd,
        (uintptr_t)settings, sizeof(*settings));
    MOSDESK_SYSCALL(SYS_CLOSE, (uintptr_t)fd, 0, 0);
    return written == (long)sizeof(*settings);
}

/* ------------------------------------------------------------ clock & date */

/* Shifts a UTC date and time by `offset_minutes`. */
static inline void mosdesk_apply_utc_offset(uint16_t* year, uint8_t* month,
                                            uint8_t* day, uint8_t* hour,
                                            uint8_t* minute,
                                            int32_t offset_minutes) {
    static const uint8_t lengths[12] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int32_t total = (int32_t)*hour * 60 + (int32_t)*minute + offset_minutes;
    int32_t day_shift = 0;
    while (total < 0) { total += 1440; day_shift--; }
    while (total >= 1440) { total -= 1440; day_shift++; }
    *hour = (uint8_t)(total / 60);
    *minute = (uint8_t)(total % 60);
    for (; day_shift > 0; day_shift--) {
        uint32_t length = lengths[*month - 1u];
        if (*month == 2u && (*year % 4u == 0u &&
            (*year % 100u != 0u || *year % 400u == 0u)))
            length = 29u;
        if (++*day > length) {
            *day = 1u;
            if (++*month > 12u) { *month = 1u; ++*year; }
        }
    }
    for (; day_shift < 0; day_shift++) {
        if (--*day == 0u) {
            if (--*month == 0u) { *month = 12u; --*year; }
            uint32_t length = lengths[*month - 1u];
            if (*month == 2u && (*year % 4u == 0u &&
                (*year % 100u != 0u || *year % 400u == 0u)))
                length = 29u;
            *day = (uint8_t)length;
        }
    }
}

/* "14:05" or "2:05 PM". `output` holds at least 9 bytes. */
static inline void mosdesk_format_time(char* output, uint8_t hour,
                                       uint8_t minute, bool clock_24h) {
    uint32_t at = 0;
    uint32_t shown = hour;
    if (!clock_24h) {
        shown = hour % 12u;
        if (!shown) shown = 12u;
    }
    if (clock_24h || shown >= 10u) output[at++] = (char)('0' + shown / 10u);
    output[at++] = (char)('0' + shown % 10u);
    output[at++] = ':';
    output[at++] = (char)('0' + minute / 10u);
    output[at++] = (char)('0' + minute % 10u);
    if (!clock_24h) {
        output[at++] = ' ';
        output[at++] = hour < 12u ? 'A' : 'P';
        output[at++] = 'M';
    }
    output[at] = '\0';
}

/* The date in one of the MOSDESK_DATE_* styles. `output` holds 12 bytes. */
static inline void mosdesk_format_date(char* output, uint16_t year,
                                       uint8_t month, uint8_t day,
                                       uint32_t format) {
    static const char months[12][4] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    char d[3] = {(char)('0' + day / 10u), (char)('0' + day % 10u), 0};
    char m[3] = {(char)('0' + month / 10u), (char)('0' + month % 10u), 0};
    char y[5] = {(char)('0' + year / 1000u % 10u),
                 (char)('0' + year / 100u % 10u),
                 (char)('0' + year / 10u % 10u),
                 (char)('0' + year % 10u), 0};
    const char* parts[5] = {0};
    if (format == MOSDESK_DATE_DMY) {
        parts[0] = d; parts[1] = "/"; parts[2] = m; parts[3] = "/";
        parts[4] = y;
    } else if (format == MOSDESK_DATE_MDY) {
        parts[0] = m; parts[1] = "/"; parts[2] = d; parts[3] = "/";
        parts[4] = y;
    } else if (format == MOSDESK_DATE_ISO) {
        parts[0] = y; parts[1] = "-"; parts[2] = m; parts[3] = "-";
        parts[4] = d;
    } else {
        parts[0] = day >= 10u ? d : d + 1; parts[1] = " ";
        parts[2] = month >= 1u && month <= 12u ? months[month - 1u] : "?";
    }
    uint32_t at = 0;
    for (uint32_t i = 0; i < 5u; i++)
        for (const char* p = parts[i]; p && *p; p++) output[at++] = *p;
    output[at] = '\0';
}

/* "UTC+02:00". `output` holds 10 bytes. */
static inline void mosdesk_format_utc_offset(char* output, int32_t minutes) {
    uint32_t value = (uint32_t)(minutes < 0 ? -minutes : minutes);
    output[0] = 'U'; output[1] = 'T'; output[2] = 'C';
    output[3] = minutes < 0 ? '-' : '+';
    output[4] = (char)('0' + value / 600u);
    output[5] = (char)('0' + value / 60u % 10u);
    output[6] = ':';
    output[7] = (char)('0' + value % 60u / 10u);
    output[8] = (char)('0' + value % 10u);
    output[9] = '\0';
}

/* ----------------------------------------------------------- internet time
 * SYS_TIMESYNC, as defined in the kernel's x86_64/syscall.h (the SDK copy
 * may predate it). */
#ifndef SYS_TIMESYNC
#define SYS_TIMESYNC 38
#define SYS_TIMESYNC_CONFIGURE 1u
#define SYS_TIMESYNC_STATUS    2u
#define SYS_TIMESYNC_URL_SIZE   192u
#define SYS_TIMESYNC_ERROR_SIZE 64u
typedef struct {
    uint32_t op;
    uint32_t enabled;
    uint32_t interval_s;
    uint32_t synced;
    char url[SYS_TIMESYNC_URL_SIZE];
    uint64_t last_unix_ms;
    uint64_t last_sync_age_ms;
    uint64_t attempts;
    char error[SYS_TIMESYNC_ERROR_SIZE];
} syscall_timesync_t;
#endif

/* Hands the internet time settings to the kernel. */
static inline bool mosdesk_timesync_apply(const mosdesk_settings_t* settings) {
    syscall_timesync_t request;
    uint8_t* bytes = (uint8_t*)&request;
    for (size_t i = 0; i < sizeof(request); i++) bytes[i] = 0;
    request.op = SYS_TIMESYNC_CONFIGURE;
    request.enabled = settings->time_sync_enabled;
    request.interval_s = settings->time_sync_interval;
    for (uint32_t i = 0; i + 1u < SYS_TIMESYNC_URL_SIZE &&
         i < MOSDESK_TIME_URL_SIZE && settings->time_sync_url[i]; i++)
        request.url[i] = settings->time_sync_url[i];
    return MOSDESK_SYSCALL(SYS_TIMESYNC, (uintptr_t)&request, 0, 0) ==
           SYS_SUCCESS;
}

static inline bool mosdesk_timesync_status(syscall_timesync_t* status) {
    uint8_t* bytes = (uint8_t*)status;
    for (size_t i = 0; i < sizeof(*status); i++) bytes[i] = 0;
    status->op = SYS_TIMESYNC_STATUS;
    return MOSDESK_SYSCALL(SYS_TIMESYNC, (uintptr_t)status, 0, 0) ==
           SYS_SUCCESS;
}

#endif
