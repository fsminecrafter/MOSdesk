#ifndef MOSDESK_SETTINGS_H
#define MOSDESK_SETTINGS_H

#include <stdbool.h>
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
#define MOSDESK_DESKTOP_ICON_COUNT 34u
#define MOSDESK_WALLPAPER_FILL 0u
#define MOSDESK_WALLPAPER_CROP 1u
#define MOSDESK_WALLPAPER_STRETCH 2u

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
} mosdesk_settings_t;

/* The layout before the debug switches were appended: a file of this size
 * is the leading part of mosdesk_settings_t. */
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
    } else if (size == (long)sizeof(mosdesk_settings_v4_t)) {
        if ((long)MOSDESK_SYSCALL(SYS_READ, (uintptr_t)fd,
                (uintptr_t)&settings, sizeof(mosdesk_settings_v4_t)) !=
            (long)sizeof(mosdesk_settings_v4_t))
            settings = mosdesk_settings_default();
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
        settings.click_once_open > 1u)
        return mosdesk_settings_default();
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

#endif
