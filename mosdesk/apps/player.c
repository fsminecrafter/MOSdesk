/*
 * Player - plays .adi audio files.
 *
 *     run 0:/programs/mos_player.run [file.adi | folder]
 *
 * Bundled with MOSdesk: mosWM opens .adi files with it when you
 * double-click them in Files or on the desktop.
 *
 * The other .adi files in the same folder are listed below the
 * controls; double-click one to play it. When a track ends the next one
 * in the list starts. Closing the window stops playback.
 *
 * Playback itself runs in the kernel (SYS_AUDIO, see audio.h), on the
 * Intel HD Audio or AC97 sound card, whichever the machine has.
 */
#include "easyui.h"
#include "audio.h"
#include "stdlib.h"
#include "string.h"
#if defined(__has_include)
#if __has_include("../settings.h")
#include "../settings.h"         /* MOSdesk: the user's home folder */
#define PLAYER_HAS_MOSDESK 1
#endif
#endif

#ifndef SYS_IS_DIR
#define SYS_IS_DIR 10
#endif

#define MAX_TRACKS 128
#define PATH_SIZE  SYSCALL_AUDIO_PATH_MAX
#define W 460
#define H 420

static char g_folder[PATH_SIZE];
static char g_tracks[MAX_TRACKS][SYSCALL_DIRENT_NAME_SIZE];
static int  g_track_count;
static int  g_current = -1;          /* list row of the file we started */
static char g_playing[PATH_SIZE];    /* full path of the file we started */
static int  g_user_volume = -1;      /* kept across tracks once changed */
static uint32_t g_last_state;

static int title, info, seek, time_left, time_right;
static int prev_button, play_button, stop_button, next_button;
static int volume_label, volume;
static int path_input, open_button, list, list_label;
static int g_updating;               /* setting widget values ourselves */
static int g_dragging;

/* ---- small helpers ---- */

static void append(char* dst, size_t size, const char* src) {
    size_t n = strlen(dst);
    while (*src && n + 1 < size) dst[n++] = *src++;
    dst[n] = '\0';
}

static void append_number(char* dst, size_t size, uint32_t v) {
    char t[12];
    int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    char out[12];
    int i = 0;
    while (n) out[i++] = t[--n];
    out[i] = '\0';
    append(dst, size, out);
}

static void format_time(char* out, size_t size, uint32_t ms) {
    uint32_t s = ms / 1000;
    out[0] = '\0';
    append_number(out, size, s / 60);
    append(out, size, ":");
    if (s % 60 < 10) append(out, size, "0");
    append_number(out, size, s % 60);
}

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static int is_adi(const char* name) {
    size_t n = strlen(name);
    if (n < 5) return 0;
    const char* e = name + n - 4;
    return e[0] == '.' && lower(e[1]) == 'a' && lower(e[2]) == 'd' && lower(e[3]) == 'i';
}

static int name_before(const char* a, const char* b) {
    for (;; a++, b++) {
        char x = lower(*a), y = lower(*b);
        if (x != y || !x) return x < y;
    }
}

static const char* base_name(const char* path) {
    const char* slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static int is_dir(const char* path) {
    return mos_syscall(SYS_IS_DIR, (long)(uintptr_t)path, 0, 0) == 1;
}

static void join(char* out, size_t size, const char* folder, const char* name) {
    out[0] = '\0';
    append(out, size, folder);
    size_t n = strlen(out);
    if (n && out[n - 1] != '/') append(out, size, "/");
    append(out, size, name);
}

static void set_value(int widget, int value) {
    g_updating = 1;
    easyui.setvalue(widget, value);
    g_updating = 0;
}

static void note(const char* text) { easyui.settext(info, text); }

/* ---- folder and tracks ---- */

static void scan_folder(const char* folder) {
    static syscall_dirent_t entries[256];
    strlcpy_safe(g_folder, folder, sizeof g_folder);
    g_track_count = 0;
    long n = mos_listdir(folder, entries, 256);
    for (long i = 0; i < n && g_track_count < MAX_TRACKS; i++) {
        if (entries[i].type == SYSCALL_DIRENT_TYPE_DIR || !is_adi(entries[i].name)) continue;
        /* insertion sort by name */
        int at = g_track_count;
        while (at > 0 && name_before(entries[i].name, g_tracks[at - 1])) {
            memcpy(g_tracks[at], g_tracks[at - 1], SYSCALL_DIRENT_NAME_SIZE);
            at--;
        }
        strlcpy_safe(g_tracks[at], entries[i].name, SYSCALL_DIRENT_NAME_SIZE);
        g_track_count++;
    }

    easyui.clearitems(list);
    for (int i = 0; i < g_track_count; i++) easyui.additem(list, g_tracks[i]);

    char label[PATH_SIZE + 32];
    label[0] = '\0';
    append(label, sizeof label, g_track_count ? "Audio files in " : "No .adi files in ");
    append(label, sizeof label, folder);
    easyui.settext(list_label, label);
    g_current = -1;
}

static void play_track(int index) {
    if (index < 0 || index >= g_track_count) return;
    char path[PATH_SIZE];
    join(path, sizeof path, g_folder, g_tracks[index]);
    long r = mos_audio_open(path);
    if (r != SYS_SUCCESS) {
        syscall_audio_status_t st;
        mos_audio_status(&st);
        if (!st.driver[0])                note("No sound card found (needs Intel HD Audio or AC97)");
        else if (r == (long)SYS_ERR_NOTFOUND) note("File not found");
        else                              note("This file could not be played (not a valid .adi?)");
        return;
    }
    if (g_user_volume >= 0) mos_audio_volume((uint32_t)g_user_volume);
    g_current = index;
    strlcpy_safe(g_playing, path, sizeof g_playing);
    set_value(list, index);
}

/* A file opens and plays; a folder is listed. */
static void open_path(const char* path) {
    if (!path || !path[0]) return;
    if (is_dir(path)) {
        scan_folder(path);
        return;
    }
    char folder[PATH_SIZE];
    strlcpy_safe(folder, path, sizeof folder);
    char* slash = strrchr(folder, '/');
    if (slash) {
        if (slash > folder && slash[-1] == ':') slash[1] = '\0';   /* keep "0:/" */
        else *slash = '\0';
    }
    scan_folder(folder);
    const char* name = base_name(path);
    for (int i = 0; i < g_track_count; i++)
        if (!strcmp(g_tracks[i], name)) { play_track(i); return; }
    /* Not listed (other extension?): try it anyway. */
    if (mos_audio_open(path) == SYS_SUCCESS) {
        strlcpy_safe(g_playing, path, sizeof g_playing);
        if (g_user_volume >= 0) mos_audio_volume((uint32_t)g_user_volume);
    } else {
        note("This file could not be played");
    }
}

static void default_folder(char* out, size_t size) {
#ifdef PLAYER_HAS_MOSDESK
    mosdesk_settings_t s = mosdesk_settings_load();
    if (mosdesk_user_path(out, size, s.username, 0) && is_dir(out)) return;
#endif
    strlcpy_safe(out, "0:/", size);
}

/* ---- refresh from the kernel's player ---- */

static void refresh(int unused) {
    (void)unused;
    syscall_audio_status_t st;
    if (mos_audio_status(&st) != SYS_SUCCESS) return;

    /* Track finished: go on with the next one in the list. */
    if (st.state == SYSCALL_AUDIO_ENDED && g_last_state == SYSCALL_AUDIO_PLAYING &&
        !strcmp(st.path, g_playing) && g_current >= 0 && g_current + 1 < g_track_count) {
        g_last_state = st.state;
        play_track(g_current + 1);
        return;
    }
    g_last_state = st.state;

    if (st.state == SYSCALL_AUDIO_IDLE) {
        easyui.settext(title, "Nothing playing");
        easyui.settext(info, st.driver[0] ? "Double-click a track below, or open a file"
                                          : "No sound card found (needs Intel HD Audio or AC97)");
        easyui.settext(play_button, "Play");
        set_value(seek, 0);
        easyui.setenabled(seek, 0);
        easyui.settext(time_left, "0:00");
        easyui.settext(time_right, "0:00");
        return;
    }

    easyui.settext(title, base_name(st.path));
    char line[96];
    line[0] = '\0';
    append(line, sizeof line, st.format);
    append(line, sizeof line, "  -  ");
    append_number(line, sizeof line, st.sample_rate);
    append(line, sizeof line, st.channels == 1 ? " Hz mono  -  " : " Hz stereo  -  ");
    append(line, sizeof line, st.driver);
    if (st.state == SYSCALL_AUDIO_PAUSED) append(line, sizeof line, "  (paused)");
    if (st.state == SYSCALL_AUDIO_ENDED) append(line, sizeof line, "  (finished)");
    easyui.settext(info, line);
    easyui.settext(play_button, st.state == SYSCALL_AUDIO_PLAYING ? "Pause" : "Play");

    easyui.setenabled(seek, 1);
    if (!g_dragging) {
        g_updating = 1;
        easyui.setrange(seek, 0, (int)(st.duration_ms ? st.duration_ms : 1));
        easyui.setvalue(seek, (int)st.position_ms);
        g_updating = 0;
        char t[16];
        format_time(t, sizeof t, st.position_ms);
        easyui.settext(time_left, t);
    }
    char t[16];
    format_time(t, sizeof t, st.duration_ms);
    easyui.settext(time_right, t);

    if (easyui.focused() != volume) set_value(volume, (int)st.volume);
}

/* ---- controls ---- */

static void on_play(int v) {
    (void)v;
    syscall_audio_status_t st;
    mos_audio_status(&st);
    if (st.state == SYSCALL_AUDIO_PLAYING) mos_audio_pause();
    else if (st.state == SYSCALL_AUDIO_PAUSED || st.state == SYSCALL_AUDIO_ENDED) mos_audio_resume();
    else {
        int row = easyui.getvalue(list);
        play_track(row >= 0 && row < g_track_count ? row : 0);
    }
    refresh(0);
}

static void on_stop(int v) { (void)v; mos_audio_stop(); refresh(0); }

static void on_prev(int v) {
    (void)v;
    syscall_audio_status_t st;
    mos_audio_status(&st);
    if (st.position_ms > 3000 || g_current <= 0) mos_audio_seek(0);
    else play_track(g_current - 1);
    refresh(0);
}

static void on_next(int v) {
    (void)v;
    if (g_current + 1 < g_track_count) play_track(g_current + 1);
    refresh(0);
}

static void on_seek_press(int v) { (void)v; g_dragging = 1; }

static void on_seek_change(int v) {
    if (g_updating) return;
    char t[16];
    format_time(t, sizeof t, (uint32_t)v);
    easyui.settext(time_left, t);
    if (!g_dragging) mos_audio_seek((uint32_t)v);   /* arrow keys */
}

static void on_seek_release(int v) {
    (void)v;
    if (!g_dragging) return;
    g_dragging = 0;
    mos_audio_seek((uint32_t)easyui.getvalue(seek));
    refresh(0);
}

static void on_volume(int v) {
    if (g_updating) return;
    g_user_volume = v;
    mos_audio_volume((uint32_t)v);
}

static void on_list_submit(int row) { play_track(row); refresh(0); }

static void on_open(int v) {
    (void)v;
    open_path(easyui.gettext(path_input));
    refresh(0);
}

static void on_close(int v) {
    (void)v;
    mos_audio_stop();
    easyui.quit();
}

int main(int argc, char** argv) {
    if (!easyui.init("Player", W, H)) return 1;
    easyui.setsmoothtext(1);

    title = easyui.createlabel(16, 12, W - 32, 30, "Nothing playing");
    easyui.settextsize(title, 2);
    info = easyui.createlabel(16, 44, W - 32, 20, "");

    seek = easyui.createslider(16, 76, W - 32, 22, 0, 1, 0);
    time_left = easyui.createlabel(16, 100, 80, 20, "0:00");
    time_right = easyui.createlabel(W - 96, 100, 80, 20, "0:00");
    easyui.setalign(time_right, EASYUI_ALIGN_RIGHT);

    prev_button = easyui.createbutton(16, 130, 64, 34, "Prev");
    play_button = easyui.createbutton(88, 130, 84, 34, "Play");
    stop_button = easyui.createbutton(180, 130, 64, 34, "Stop");
    next_button = easyui.createbutton(252, 130, 64, 34, "Next");
    easyui.setstyle(prev_button, EASYUI_SECONDARY);
    easyui.setstyle(stop_button, EASYUI_SECONDARY);
    easyui.setstyle(next_button, EASYUI_SECONDARY);

    volume_label = easyui.createlabel(332, 126, W - 348, 18, "Volume");
    volume = easyui.createslider(332, 146, W - 348, 20, 0, 100, 80);

    path_input = easyui.createtextinput(16, 180, W - 120, 30, "0:/path/to/file.adi or folder");
    open_button = easyui.createbutton(W - 96, 180, 80, 30, "Open");
    easyui.setstyle(open_button, EASYUI_SECONDARY);

    list_label = easyui.createlabel(16, 220, W - 32, 18, "");
    list = easyui.createlistbox(16, 240, W - 32, H - 256);

    /* Grow with the window: the seek bar and list widen, the list gets taller. */
    easyui.setanchor(title, EASYUI_ANCHOR_LEFT | EASYUI_ANCHOR_RIGHT | EASYUI_ANCHOR_TOP);
    easyui.setanchor(info, EASYUI_ANCHOR_LEFT | EASYUI_ANCHOR_RIGHT | EASYUI_ANCHOR_TOP);
    easyui.setanchor(seek, EASYUI_ANCHOR_LEFT | EASYUI_ANCHOR_RIGHT | EASYUI_ANCHOR_TOP);
    easyui.setanchor(time_right, EASYUI_ANCHOR_RIGHT | EASYUI_ANCHOR_TOP);
    easyui.setanchor(volume_label, EASYUI_ANCHOR_LEFT | EASYUI_ANCHOR_RIGHT | EASYUI_ANCHOR_TOP);
    easyui.setanchor(volume, EASYUI_ANCHOR_LEFT | EASYUI_ANCHOR_RIGHT | EASYUI_ANCHOR_TOP);
    easyui.setanchor(path_input, EASYUI_ANCHOR_LEFT | EASYUI_ANCHOR_RIGHT | EASYUI_ANCHOR_TOP);
    easyui.setanchor(open_button, EASYUI_ANCHOR_RIGHT | EASYUI_ANCHOR_TOP);
    easyui.setanchor(list_label, EASYUI_ANCHOR_LEFT | EASYUI_ANCHOR_RIGHT | EASYUI_ANCHOR_TOP);
    easyui.setanchor(list, EASYUI_ANCHOR_LEFT | EASYUI_ANCHOR_RIGHT |
                           EASYUI_ANCHOR_TOP | EASYUI_ANCHOR_BOTTOM);

    easyui.recallfunc(play_button, ONCLICK, on_play);
    easyui.recallfunc(stop_button, ONCLICK, on_stop);
    easyui.recallfunc(prev_button, ONCLICK, on_prev);
    easyui.recallfunc(next_button, ONCLICK, on_next);
    easyui.recallfunc(seek, ONPRESS, on_seek_press);
    easyui.recallfunc(seek, ONCHANGE, on_seek_change);
    easyui.recallfunc(seek, ONRELEASE, on_seek_release);
    easyui.recallfunc(volume, ONCHANGE, on_volume);
    easyui.recallfunc(list, ONSUBMIT, on_list_submit);
    easyui.recallfunc(path_input, ONSUBMIT, on_open);
    easyui.recallfunc(open_button, ONCLICK, on_open);
    easyui.recallfunc(EASYUI_WINDOW, ONCLOSE, on_close);

    if (argc > 1 && argv[1] && argv[1][0]) {
        easyui.settext(path_input, argv[1]);
        open_path(argv[1]);
    } else {
        char folder[PATH_SIZE];
        default_folder(folder, sizeof folder);
        easyui.settext(path_input, folder);
        scan_folder(folder);
    }

    refresh(0);
    easyui.settimer(250, refresh);
    return easyui.run();
}
