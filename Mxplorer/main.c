#include "mxplorer/browser.h"
#include "mxplorer/devlog.h"
#include "mxplorer/devtools.h"
#include "mxplorer/html.h"
#include "mxplorer/http.h"
#include "mxplorer/js.h"
#include "mxplorer/text.h"
#include "mxplorer/ui.h"
#include "mxplorer/util.h"
#include "stdlib.h"
#include "stdio.h"
#include "string.h"
#include "window.h"
#include "x86_64/user_usb.h"

#define WIN_W 1120u
#define WIN_H 760u
#define MIN_WIN_W 640u
#define MIN_WIN_H 420u
#define MAX_TABS MX_BROWSER_MAX_TABS
#define MAX_HISTORY MX_BROWSER_MAX_HISTORY
#define URL_LENGTH MX_BROWSER_URL_LENGTH
#define TOP MX_BROWSER_PAGE_Y
#define HOMEPAGE_GOOGLE 0
#define HOMEPAGE_MXPLORER 1
#define HOMEPAGE_CUSTOM 2
#define HOMEPAGE_SETTING_PATH "0:/data/mxplorer-homepage.txt"
#define SETTINGS_PATH "0:/data/mxplorer-settings.txt"
#define HOMEPAGE_ASSET_PATH "0:/programs/Mxplorer/Resources/homepage.html"
#define HOMEPAGE_HTML_CAPACITY (64u * 1024u)
#define PAGE_BUDGET_MS 30000

/* USB HID key codes */
enum {
    KEY_A = 4, KEY_D = 7, KEY_I = 12, KEY_J = 13, KEY_L = 15, KEY_Q = 20, KEY_R = 21,
    KEY_T = 23, KEY_W = 26, KEY_ENTER = 40, KEY_ESCAPE = 41, KEY_BACKSPACE = 42, KEY_TAB = 43,
    KEY_SPACE = 44, KEY_F5 = 62, KEY_F12 = 69, KEY_HOME = 74, KEY_PAGE_UP = 75, KEY_DELETE = 76,
    KEY_END = 77, KEY_PAGE_DOWN = 78, KEY_RIGHT = 79, KEY_LEFT = 80, KEY_DOWN = 81, KEY_UP = 82
};

typedef mx_browser_tab_t browser_tab_t;

static browser_tab_t tabs[MAX_TABS];
static int tab_count;
static int active_tab;
static int tab_scroll;
static uint32_t window_id;
static uint32_t* pixels;
static uint32_t surface_width;
static uint32_t surface_height;
static int running = 1;

/* address bar */
static int address_focused;
static int address_select_all;
static int address_cursor;
static int address_scroll;
static char address_text[URL_LENGTH];

/* settings */
static int settings_category;
static int allow_insecure_tls;
static int theme_dark;
static int dark_pages = 1;
static int javascript_enabled = 1;
static int subresources_enabled = 1;
static int devtools_open;
static int homepage_mode = HOMEPAGE_GOOGLE;
static int homepage_previous_mode = HOMEPAGE_GOOGLE;
static char custom_homepage[URL_LENGTH];
static int homepage_editing;

/* interaction */
static int previous_buttons;
static int drag_tab = -1;
static int drag_scrollbar = -1;      /* grab offset inside the thumb */
static int mouse_x, mouse_y;
static int hover_kind, hover_index;
static int pressed_kind;
static char hover_link[URL_LENGTH];
static char pending_navigation[URL_LENGTH];
static int pending_navigation_set;

/* page */
static mx_browser_theme_t theme;
static uint32_t os_accent = 0xFF3478D4u;
static char status_message[160] = "";
static long last_theme_check;
static char* page_buffer;                 /* HTTP response for the document */
static char* resource_buffer;             /* subresources */
static char home_page_html[HOMEPAGE_HTML_CAPACITY];
static size_t home_page_html_length;
static int page_loaded;
static int loading;
static long load_started;
static int chrome_dirty = 1;
static int page_dirty = 1;
static long last_frame_ms;
static syscall_window_style_t original_style;
static int have_original_style;
static int style_is_dark = -1;
static int resources_loaded, resources_skipped;

static const char home_page_fallback[] =
    "<!doctype html><html><head><title>New Tab</title><style>"
    "body{margin:0;font-family:sans-serif;background:#f5f7fa;color:#1f2328}"
    "main{max-width:560px;margin:0 auto;padding:120px 24px;text-align:center}"
    "h1{font-size:44px;color:#3478d4;margin:0 0 8px}p{color:#5e6a78}"
    "@media (prefers-color-scheme: dark){body{background:#1f2023;color:#e8eaed}p{color:#a4aab2}h1{color:#8ab4f8}}"
    "</style></head><body><main><h1>Mxplorer</h1>"
    "<p>Search or type an address in the bar above.</p></main></body></html>";

/* ====================================================================== */
/* Small helpers                                                          */
/* ====================================================================== */

static void set_status(const char* text) {
    mx_copy(status_message, sizeof(status_message), text);
}

static int text_equal(const char* a, const char* b) {
    return !strcmp(a, b);
}

static int is_settings_page(const browser_tab_t* tab) {
    return text_equal(tab->url, "mos://settings");
}

static int is_home_page(const browser_tab_t* tab) {
    return text_equal(tab->url, "mos://home");
}

static int page_view_height(void) {
    int h = (int)surface_height - TOP - (devtools_open ? mx_devtools_height() : 0);
    return h < 60 ? 60 : h;
}

static void clamp_devtools(void) {
    int max = (int)surface_height - TOP - 80;
    if (mx_devtools_height() > max) mx_devtools_set_height(max);
}

static const char* homepage_url(void) {
    if (homepage_mode == HOMEPAGE_MXPLORER) return "mos://home";
    if (homepage_mode == HOMEPAGE_CUSTOM && custom_homepage[0]) return custom_homepage;
    return "https://google.com";
}

/* ====================================================================== */
/* Settings files                                                         */
/* ====================================================================== */

static int write_file(const char* path, const char* data, size_t length) {
    if (!mos_exists("0:/data") && mos_mkdir("0:/data") != SYS_SUCCESS) return 0;
    /* replace the file so shorter contents leave no stale tail */
    if (mos_exists(path)) mos_syscall(SYS_DELETE, (long)(uintptr_t)path, 0, 0);
    if (mos_create(path, "text", "txt") != SYS_SUCCESS) return 0;
    long fd = mos_open(path, SYS_O_RDWR);
    if (fd < 0) return 0;
    long written = mos_fwrite(fd, data, length);
    long closed = mos_close(fd);
    return written == (long)length && closed == SYS_SUCCESS;
}

static long read_file(const char* path, char* out, size_t capacity) {
    long fd = mos_open(path, SYS_O_RDONLY);
    if (fd < 0) return -1;
    long size = mos_size(fd);
    if (size <= 0 || size >= (long)capacity) {
        mos_close(fd);
        return -1;
    }
    long count = mos_read(fd, out, (size_t)size);
    mos_close(fd);
    if (count < 0) return -1;
    out[count] = '\0';
    return count;
}

static void load_homepage_setting(void) {
    char config[URL_LENGTH + 4];
    long count = read_file(HOMEPAGE_SETTING_PATH, config, sizeof(config));
    if (count < 2 || config[1] != '\n') return;
    if (config[0] == '0') {
        homepage_mode = HOMEPAGE_GOOGLE;
    } else if (config[0] == '1') {
        homepage_mode = HOMEPAGE_MXPLORER;
    } else if (config[0] == '2' && count > 2) {
        size_t end = 2;
        while (end < (size_t)count && config[end] != '\n') end++;
        mx_copy_n(custom_homepage, sizeof(custom_homepage), config + 2, end - 2);
        if (custom_homepage[0]) homepage_mode = HOMEPAGE_CUSTOM;
    }
}

static int save_homepage_setting(void) {
    char config[URL_LENGTH + 4];
    config[0] = (char)('0' + homepage_mode);
    config[1] = '\n';
    config[2] = '\0';
    if (homepage_mode == HOMEPAGE_CUSTOM) {
        mx_append(config, sizeof(config), custom_homepage);
        mx_append(config, sizeof(config), "\n");
    }
    if (!write_file(HOMEPAGE_SETTING_PATH, config, strlen(config))) {
        set_status("Could not save the homepage setting.");
        return 0;
    }
    return 1;
}

static void load_settings(void) {
    char text[512];
    if (read_file(SETTINGS_PATH, text, sizeof(text)) <= 0) return;
    for (char* line = text; *line;) {
        char* end = strchr(line, '\n');
        if (end) *end = '\0';
        char* eq = strchr(line, '=');
        if (eq) {
            *eq = '\0';
            int value = atoi(eq + 1);
            if (!strcmp(line, "theme")) theme_dark = value == 1;
            else if (!strcmp(line, "javascript")) javascript_enabled = value != 0;
            else if (!strcmp(line, "subresources")) subresources_enabled = value != 0;
            else if (!strcmp(line, "darkpages")) dark_pages = value != 0;
            else if (!strcmp(line, "devtools")) mx_devtools_set_height(value);
        }
        if (!end) break;
        line = end + 1;
    }
}

static void save_settings(void) {
    char text[256] = "theme=";
    mx_append_int(text, sizeof(text), theme_dark);
    mx_append(text, sizeof(text), "\njavascript=");
    mx_append_int(text, sizeof(text), javascript_enabled);
    mx_append(text, sizeof(text), "\nsubresources=");
    mx_append_int(text, sizeof(text), subresources_enabled);
    mx_append(text, sizeof(text), "\ndarkpages=");
    mx_append_int(text, sizeof(text), dark_pages);
    mx_append(text, sizeof(text), "\ndevtools=");
    mx_append_int(text, sizeof(text), mx_devtools_height());
    mx_append(text, sizeof(text), "\n");
    if (!write_file(SETTINGS_PATH, text, strlen(text))) set_status("Could not save settings.");
}

static void load_homepage_asset(void) {
    size_t fallback_length = sizeof(home_page_fallback) - 1;
    memcpy(home_page_html, home_page_fallback, fallback_length);
    home_page_html[fallback_length] = '\0';
    home_page_html_length = fallback_length;
    long count = read_file(HOMEPAGE_ASSET_PATH, home_page_html, sizeof(home_page_html));
    if (count > 0) home_page_html_length = (size_t)count;
    else {
        memcpy(home_page_html, home_page_fallback, fallback_length + 1);
        home_page_html_length = fallback_length;
    }
}

/* ====================================================================== */
/* Theme                                                                  */
/* ====================================================================== */

static void read_os_accent(void) {
    uint32_t header[3] = {0, 0, 0};
    long fd = mos_open("0:/programs/mosdesk/settings.dat", SYS_O_RDONLY);
    if (fd <= 0) return;
    long bytes = mos_read(fd, header, sizeof(header));
    mos_close(fd);
    if (bytes == sizeof(header) && header[0] == 0x4D445331u)
        os_accent = 0xFF000000u | (header[2] & 0x00FFFFFFu);
}

static void apply_window_style(void) {
    if (!have_original_style || style_is_dark == theme_dark) return;
    style_is_dark = theme_dark;
    syscall_window_style_t style = original_style;
    if (theme_dark) {
        style.title_bar = theme.frame;
        style.title_text = theme.text;
        style.close_button = theme.frame;
        style.minimize_button = theme.frame;
        style.maximize_button = theme.frame;
        style.close_icon = theme.text;
        style.minimize_icon = theme.text;
        style.maximize_icon = theme.text;
        style.border = theme.border;
        style.content_background = theme.page;
    }
    mos_window_set_style(window_id, style);
}

static int rebuild_theme(void) {
    mx_browser_theme_t previous = theme;
    mx_theme_build(&theme, theme_dark, os_accent);
    return memcmp(&previous, &theme, sizeof(theme)) != 0;
}

/* ====================================================================== */
/* Window                                                                 */
/* ====================================================================== */

static int resize_surface(uint32_t width, uint32_t height) {
    if (width < MIN_WIN_W || height < MIN_WIN_H) return 0;
    uint32_t* fresh = (uint32_t*)malloc((size_t)width * height * sizeof(uint32_t));
    if (!fresh) return 0;
    free(pixels);
    pixels = fresh;
    surface_width = width;
    surface_height = height;
    clamp_devtools();
    return 1;
}

static void update_title(void) {
    browser_tab_t* tab = &tabs[active_tab];
    char title[160];
    mx_copy(title, sizeof(title), tab->title[0] ? tab->title : "New Tab");
    mx_append(title, sizeof(title), " - Mxplorer");
    mos_window_set_title(window_id, title);
}

static mx_ui_state_t ui_state(void);

static void present_chrome_now(void) {
    mx_ui_state_t ui = ui_state();
    mx_ui_draw_chrome(&ui);
}

/* ====================================================================== */
/* Tabs and history                                                       */
/* ====================================================================== */

static void set_tab_title_from_url(browser_tab_t* tab) {
    if (is_settings_page(tab)) {
        mx_copy(tab->title, sizeof(tab->title), "Settings");
        return;
    }
    if (is_home_page(tab)) {
        mx_copy(tab->title, sizeof(tab->title), "New Tab");
        return;
    }
    const char* host = strstr(tab->url, "://");
    host = host ? host + 3 : tab->url;
    size_t length = 0;
    while (host[length] && host[length] != '/' && host[length] != '?' && host[length] != ':')
        length++;
    mx_copy_n(tab->title, sizeof(tab->title), host, length);
    if (!tab->title[0]) mx_copy(tab->title, sizeof(tab->title), "New Tab");
}

static void init_tab(browser_tab_t* tab) {
    memset(tab, 0, sizeof(*tab));
    mx_copy(tab->url, sizeof(tab->url), homepage_url());
    set_tab_title_from_url(tab);
    mx_copy(tab->history[0], sizeof(tab->history[0]), tab->url);
    tab->history_count = 1;
}

static void push_history(browser_tab_t* tab, const char* url) {
    if (tab->history_count > 0 && text_equal(tab->history[tab->history_index], url)) return;
    if (tab->history_index + 1 < tab->history_count) tab->history_count = tab->history_index + 1;
    if (tab->history_count == MAX_HISTORY) {
        memmove(tab->history[0], tab->history[1], sizeof(tab->history[0]) * (MAX_HISTORY - 1));
        tab->history_count--;
        tab->history_index--;
    }
    tab->history_index = tab->history_count++;
    mx_copy(tab->history[tab->history_index], URL_LENGTH, url);
}

static void reset_address(void) {
    mx_copy(address_text, sizeof(address_text), tabs[active_tab].url);
    address_focused = 0;
    address_select_all = 0;
    address_cursor = (int)strlen(address_text);
    address_scroll = 0;
}

/* ====================================================================== */
/* Loading                                                                */
/* ====================================================================== */

static void classify_error(browser_tab_t* tab, const char* error) {
    mx_copy(tab->error_detail, sizeof(tab->error_detail), error);
    if (strstr(error, "Unsafe HTTPS is disabled")) tab->error = MX_PAGE_ERROR_HTTPS_OFF;
    else if (strstr(error, "Network is unavailable")) tab->error = MX_PAGE_ERROR_OFFLINE;
    else if (strstr(error, "larger") || strstr(error, "HTML") || strstr(error, "memory"))
        tab->error = MX_PAGE_ERROR_CONTENT;
    else tab->error = MX_PAGE_ERROR_NETWORK;
}

/* Subresources for pages and scripts (stylesheets, scripts, fetch()). */
static int fetch_resource(void* user, const char* url, const char* kind, char** body,
                          size_t* length) {
    (void)user;
    mx_network_entry_t* entry = mx_network_add(url, kind);
    if (mx_now_ms() - load_started > PAGE_BUDGET_MS && loading) {
        mx_copy(entry->error, sizeof(entry->error), "Skipped: the page took too long to load.");
        resources_skipped++;
        return 0;
    }
    if (loading) {
        char text[160] = "Loading ";
        mx_append(text, sizeof(text), kind);
        mx_append(text, sizeof(text), " ");
        mx_append_int(text, sizeof(text), resources_loaded + 1);
        mx_append(text, sizeof(text), "...");
        set_status(text);
        loading = loading < 90 ? loading + 3 : 90;
        present_chrome_now();
    }
    size_t received = 0;
    char error[128];
    mx_http_info_t info;
    int ok = mx_http_get(url, resource_buffer, MX_HTTP_BODY_MAX, &received, allow_insecure_tls,
                         error, sizeof(error), &info);
    entry->status = info.status;
    entry->duration_ms = mx_now_ms() - entry->started_ms;
    entry->redirects = info.redirects;
    entry->size = received;
    mx_copy(entry->content_type, sizeof(entry->content_type), info.content_type);
    if (!ok || info.status >= 400) {
        mx_copy(entry->error, sizeof(entry->error), ok ? "HTTP error status" : error);
        if (!ok) entry->status = 0;
        char message[200];
        mx_copy(message, sizeof(message), "Failed to load ");
        mx_append(message, sizeof(message), kind);
        mx_append(message, sizeof(message), ": ");
        mx_append(message, sizeof(message), url);
        mx_console_add(MX_LOG_WARN, "Network", 0, message);
        return 0;
    }
    char* copy = (char*)malloc(received + 1);
    if (!copy) return 0;
    memcpy(copy, resource_buffer, received);
    copy[received] = '\0';
    *body = copy;
    *length = received;
    resources_loaded++;
    return 1;
}

static void page_navigate_request(const char* url, void* user) {
    (void)user;
    mx_copy(pending_navigation, sizeof(pending_navigation), url);
    pending_navigation_set = 1;
}

static void page_alert(const char* text, void* user) {
    (void)user;
    char message[160] = "Page says: ";
    mx_append(message, sizeof(message), text);
    set_status(message);
    chrome_dirty = 1;
}

static int render_source(const char* source, size_t length, const char* url, int scripts,
                         int dark) {
    mx_html_options_t options;
    memset(&options, 0, sizeof(options));
    options.url = url;
    options.viewport_width = (int)surface_width;
    options.viewport_height = page_view_height();
    options.dark = dark;
    options.scripts = scripts;
    options.subresources = subresources_enabled;
    options.fetch = fetch_resource;
    options.navigate = page_navigate_request;
    options.alert = page_alert;
    mx_html_result_t result = mx_html_load(source, length, &options);
    if (result != MX_HTML_OK) {
        classify_error(&tabs[active_tab], mx_html_error(result));
        tabs[active_tab].error = MX_PAGE_ERROR_CONTENT;
        return 0;
    }
    return 1;
}

static void load_page(int force_reload) {
    browser_tab_t* tab = &tabs[active_tab];
    page_loaded = 0;
    tab->error = MX_PAGE_ERROR_NONE;
    tab->error_detail[0] = '\0';
    mx_devtools_reset_page();
    mx_html_unload();
    page_dirty = chrome_dirty = 1;
    if (is_settings_page(tab)) {
        set_status("");
        return;
    }
    mx_network_clear();
    resources_loaded = resources_skipped = 0;
    load_started = mx_now_ms();
    if (is_home_page(tab)) {
        page_loaded = render_source(home_page_html, home_page_html_length, "mos://home", 0,
                                    theme_dark);
        tab->scroll_y = 0;
        return;
    }
    char status[160] = "Connecting to ";
    const char* host = strstr(tab->url, "://");
    mx_append(status, sizeof(status), host ? host + 3 : tab->url);
    set_status(status);
    char message[300] = "Navigated to ";
    mx_append(message, sizeof(message), tab->url);
    mx_console_add(MX_LOG_SYSTEM, "", 0, message);
    loading = 10;
    /* show the loading state before the blocking fetch */
    {
        mx_ui_state_t ui = ui_state();
        mx_ui_draw_chrome(&ui);
        mx_ui_draw_blank(&ui);
        mos_window_present(window_id, pixels, surface_width * 4u, 0, TOP, surface_width,
                           (uint32_t)page_view_height());
    }
    int from_cache = !force_reload && tab->page_source && text_equal(tab->source_url, tab->url);
    if (!from_cache) {
        mx_network_entry_t* entry = mx_network_add(tab->url, "document");
        char error[160];
        size_t body_length = 0;
        mx_http_info_t info;
        int ok = mx_http_get(tab->url, page_buffer, MX_HTTP_BODY_MAX, &body_length,
                             allow_insecure_tls, error, sizeof(error), &info);
        entry->status = ok ? info.status : 0;
        entry->duration_ms = mx_now_ms() - entry->started_ms;
        entry->size = body_length;
        entry->redirects = info.redirects;
        mx_copy(entry->content_type, sizeof(entry->content_type), info.content_type);
        if (!ok) {
            mx_copy(entry->error, sizeof(entry->error), error);
            classify_error(tab, error);
            mx_console_add(MX_LOG_ERROR, "Network", 0, error);
            set_status(error);
            loading = 0;
            return;
        }
        if (info.final_url[0] && !text_equal(info.final_url, tab->url)) {
            /* redirects update the address */
            mx_copy(tab->url, sizeof(tab->url), info.final_url);
            mx_copy(tab->history[tab->history_index], URL_LENGTH, info.final_url);
            if (!address_focused) reset_address();
        }
        char* saved = (char*)malloc(body_length + 1);
        if (!saved) {
            classify_error(tab, "Not enough memory to keep this page.");
            loading = 0;
            return;
        }
        memcpy(saved, page_buffer, body_length);
        saved[body_length] = '\0';
        free(tab->page_source);
        tab->page_source = saved;
        tab->page_source_length = body_length;
        mx_copy(tab->source_url, sizeof(tab->source_url), tab->url);
        tab->scroll_y = 0;
        if (info.status >= 400) {
            char text[96] = "Server answered with status ";
            mx_append_int(text, sizeof(text), info.status);
            mx_console_add(MX_LOG_WARN, "Network", 0, text);
        }
    } else {
        mx_network_entry_t* entry = mx_network_add(tab->url, "document");
        entry->status = 200;
        entry->from_cache = 1;
        entry->size = tab->page_source_length;
    }
    loading = 60;
    set_status("Rendering...");
    present_chrome_now();
    page_loaded = render_source(tab->page_source, tab->page_source_length, tab->url,
                                javascript_enabled, theme_dark && dark_pages);
    loading = 0;
    if (page_loaded) {
        const char* title = mx_html_title();
        if (title && *title) mx_copy(tab->title, sizeof(tab->title), title);
        mx_html_set_scroll(tab->scroll_y, (uint32_t)page_view_height());
        const mx_html_stats_t* stats = mx_html_stats();
        char done[160] = "Done in ";
        mx_append_int(done, sizeof(done), mx_now_ms() - load_started);
        mx_append(done, sizeof(done), " ms");
        if (resources_skipped) {
            mx_append(done, sizeof(done), ", skipped ");
            mx_append_int(done, sizeof(done), resources_skipped);
            mx_append(done, sizeof(done), " slow resources");
        }
        set_status(done);
        (void)stats;
    }
    update_title();
}

static void relayout(void) {
    if (!page_loaded) return;
    int scroll = mx_html_scroll_y();
    int dark = is_home_page(&tabs[active_tab]) ? theme_dark : theme_dark && dark_pages;
    mx_html_relayout((int)surface_width, page_view_height(), dark);
    mx_html_set_scroll(scroll, (uint32_t)page_view_height());
    page_dirty = 1;
}

/* ====================================================================== */
/* Navigation                                                             */
/* ====================================================================== */

static int encode_query(char* output, size_t capacity, const char* query) {
    static const char hex[] = "0123456789ABCDEF";
    size_t used = 0;
    for (size_t i = 0; query[i]; i++) {
        unsigned char c = (unsigned char)query[i];
        int safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                   c == '-' || c == '_' || c == '.' || c == '~';
        if (used + (safe ? 1 : 3) >= capacity) return 0;
        if (c == ' ') {
            output[used++] = '+';
        } else if (safe) {
            output[used++] = (char)c;
        } else {
            output[used++] = '%';
            output[used++] = hex[c >> 4];
            output[used++] = hex[c & 15];
        }
    }
    output[used] = '\0';
    return 1;
}

static int normalize_address(const char* input, char* output, size_t capacity, int* is_search) {
    while (*input == ' ') input++;
    size_t length = strlen(input);
    while (length && input[length - 1] == ' ') length--;
    char trimmed[URL_LENGTH];
    mx_copy_n(trimmed, sizeof(trimmed), input, length);
    int has_dot = strchr(trimmed, '.') != NULL;
    int has_colon = strchr(trimmed, ':') != NULL;
    int has_space = strchr(trimmed, ' ') != NULL;
    *is_search = 0;
    if (!length) return 0;
    if (mx_starts_with_nocase(trimmed, "mos://") || mx_starts_with_nocase(trimmed, "http://") ||
        mx_starts_with_nocase(trimmed, "https://")) {
        if (length >= capacity) return 0;
        mx_copy(output, capacity, trimmed);
        for (size_t i = 0; i < 8 && output[i] && output[i] != ':'; i++) output[i] = mx_lower(output[i]);
        /* "https://example.com" without a path gets one */
        return 1;
    }
    if (has_space || (!has_dot && !has_colon && strcmp(trimmed, "localhost"))) {
        static const char prefix[] = "https://www.google.com/search?q=";
        char encoded[URL_LENGTH];
        if (!encode_query(encoded, sizeof(encoded), trimmed) ||
            sizeof(prefix) - 1 + strlen(encoded) >= capacity)
            return 0;
        mx_copy(output, capacity, prefix);
        mx_append(output, capacity, encoded);
        *is_search = 1;
        return 1;
    }
    mx_copy(output, capacity, "https://");
    mx_append(output, capacity, trimmed);
    return strlen(output) < capacity;
}

static void navigate_to(const char* input) {
    char destination[URL_LENGTH];
    int is_search = 0;
    if (!normalize_address(input, destination, sizeof(destination), &is_search)) {
        set_status("The address is empty or too long.");
        chrome_dirty = 1;
        return;
    }
    browser_tab_t* tab = &tabs[active_tab];
    tab->scroll_y = mx_html_scroll_y();
    homepage_editing = 0;
    mx_copy(tab->url, sizeof(tab->url), destination);
    set_tab_title_from_url(tab);
    if (is_search) mx_copy(tab->title, sizeof(tab->title), "Google Search");
    push_history(tab, destination);
    tab->scroll_y = 0;
    reset_address();
    load_page(0);
    update_title();
}

static void go_history(int delta) {
    browser_tab_t* tab = &tabs[active_tab];
    int index = tab->history_index + delta;
    if (index < 0 || index >= tab->history_count) return;
    tab->scroll_y = mx_html_scroll_y();
    tab->history_index = index;
    mx_copy(tab->url, sizeof(tab->url), tab->history[index]);
    set_tab_title_from_url(tab);
    reset_address();
    load_page(0);
    update_title();
}

static void switch_tab(int index) {
    if (index < 0 || index >= tab_count || index == active_tab) return;
    tabs[active_tab].scroll_y = mx_html_scroll_y();
    active_tab = index;
    reset_address();
    load_page(0);
    update_title();
}

static void new_tab(void) {
    if (tab_count >= MAX_TABS) {
        set_status("You can have up to 12 tabs.");
        chrome_dirty = 1;
        return;
    }
    tabs[active_tab].scroll_y = mx_html_scroll_y();
    init_tab(&tabs[tab_count]);
    active_tab = tab_count++;
    reset_address();
    load_page(0);
    if (!is_home_page(&tabs[active_tab]) && !is_settings_page(&tabs[active_tab])) {
        /* new tabs focus the address bar */
    }
    address_focused = 1;
    address_select_all = 1;
    update_title();
}

static void close_tab(int index) {
    if (index < 0 || index >= tab_count) return;
    free(tabs[index].page_source);
    if (tab_count == 1) {
        init_tab(&tabs[0]);
        active_tab = 0;
    } else {
        for (int i = index + 1; i < tab_count; i++) tabs[i - 1] = tabs[i];
        tab_count--;
        if (active_tab > index) active_tab--;
        else if (active_tab == index && active_tab >= tab_count) active_tab = tab_count - 1;
    }
    drag_tab = -1;
    reset_address();
    load_page(0);
    update_title();
}

static void reorder_tab(int from, int to) {
    if (from < 0 || to < 0 || from >= tab_count || to >= tab_count || from == to) return;
    browser_tab_t moved = tabs[from];
    if (from < to) for (int i = from; i < to; i++) tabs[i] = tabs[i + 1];
    else for (int i = from; i > to; i--) tabs[i] = tabs[i - 1];
    tabs[to] = moved;
    if (active_tab == from) active_tab = to;
    else if (from < active_tab && active_tab <= to) active_tab--;
    else if (to <= active_tab && active_tab < from) active_tab++;
    drag_tab = to;
}

static void reload(void) {
    if (loading) return;
    tabs[active_tab].refresh_count++;
    tabs[active_tab].scroll_y = mx_html_scroll_y();
    load_page(1);
}

/* ====================================================================== */
/* Settings actions                                                       */
/* ====================================================================== */

static void set_theme(int dark) {
    if (theme_dark == dark) return;
    theme_dark = dark;
    rebuild_theme();
    apply_window_style();
    save_settings();
    relayout();
    chrome_dirty = page_dirty = 1;
}

static void toggle_devtools(int open) {
    devtools_open = open;
    if (!open) {
        mx_devtools_blur();
        mx_html_set_highlight(NULL);
    }
    clamp_devtools();
    relayout();
    chrome_dirty = page_dirty = 1;
}

static void begin_custom_homepage(void) {
    homepage_previous_mode = homepage_mode;
    homepage_mode = HOMEPAGE_CUSTOM;
    homepage_editing = 1;
    address_focused = 1;
    mx_copy(address_text, sizeof(address_text), custom_homepage);
    address_cursor = (int)strlen(address_text);
    address_scroll = 0;
    address_select_all = address_cursor != 0;
    set_status("Type the address of your home page, then press Enter.");
}

static void apply_setting(int id) {
    if (id >= MX_SET_CATEGORY && id < MX_SET_CATEGORY + MX_SETTINGS_CATEGORIES) {
        settings_category = id - MX_SET_CATEGORY;
        return;
    }
    switch (id) {
    case MX_SET_HOME_GOOGLE:
    case MX_SET_HOME_MXPLORER:
        homepage_mode = id == MX_SET_HOME_GOOGLE ? HOMEPAGE_GOOGLE : HOMEPAGE_MXPLORER;
        homepage_editing = 0;
        reset_address();
        if (save_homepage_setting())
            set_status(homepage_mode == HOMEPAGE_GOOGLE ? "Home page set to Google."
                                                        : "Home page set to the Mxplorer start page.");
        break;
    case MX_SET_HOME_CUSTOM:
    case MX_SET_HOME_FIELD:
        begin_custom_homepage();
        break;
    case MX_SET_THEME_LIGHT: set_theme(0); break;
    case MX_SET_THEME_DARK: set_theme(1); break;
    case MX_SET_DARK_PAGES:
        dark_pages = !dark_pages;
        save_settings();
        break;
    case MX_SET_JAVASCRIPT:
        javascript_enabled = !javascript_enabled;
        save_settings();
        set_status(javascript_enabled ? "JavaScript is on for new page loads." : "JavaScript is off.");
        break;
    case MX_SET_SUBRESOURCES:
        subresources_enabled = !subresources_enabled;
        save_settings();
        break;
    case MX_SET_UNSAFE_HTTPS:
        allow_insecure_tls = !allow_insecure_tls;
        set_status(allow_insecure_tls ? "Unsafe HTTPS is on for this session. Certificates are not checked."
                                      : "Unsafe HTTPS is off.");
        break;
    case MX_SET_CLEAR_DATA:
        for (int i = 0; i < tab_count; i++) {
            free(tabs[i].page_source);
            tabs[i].page_source = NULL;
            tabs[i].source_url[0] = '\0';
        }
        mx_js_clear_storage();
        mx_network_clear();
        set_status("Cookies, site storage and cached pages were cleared.");
        break;
    case MX_SET_DEVTOOLS:
        toggle_devtools(!devtools_open);
        break;
    }
}

static void save_custom_homepage(void) {
    char normalized[URL_LENGTH];
    int is_search = 0;
    if (!address_text[0] || !normalize_address(address_text, normalized, sizeof(normalized), &is_search) ||
        is_search) {
        set_status("Enter a website address for your home page.");
        return;
    }
    mx_copy(custom_homepage, sizeof(custom_homepage), normalized);
    homepage_mode = HOMEPAGE_CUSTOM;
    homepage_editing = 0;
    reset_address();
    if (save_homepage_setting()) set_status("Home page saved.");
}

/* ====================================================================== */
/* Address bar editing                                                    */
/* ====================================================================== */

static void update_address_scroll(void) {
    mx_ui_state_t ui = ui_state();
    int text_x, available;
    mx_ui_omnibox_text_rect(&ui, &text_x, &available);
    if (available < 20) available = 20;
    int length = (int)strlen(address_text);
    if (address_cursor < 0) address_cursor = 0;
    if (address_cursor > length) address_cursor = length;
    if (address_select_all) address_scroll = 0;
    if (address_scroll > address_cursor) address_scroll = address_cursor;
    while (address_scroll < address_cursor &&
           mx_text_width_px(address_text + address_scroll, (size_t)(address_cursor - address_scroll), 14, 0) >
               available)
        address_scroll++;
}

static int address_cursor_at_x(int x) {
    mx_ui_state_t ui = ui_state();
    int text_x, available;
    mx_ui_omnibox_text_rect(&ui, &text_x, &available);
    int length = (int)strlen(address_text);
    int previous = 0;
    for (int i = address_scroll; i < length; i++) {
        int w = mx_text_width_px(address_text + address_scroll, (size_t)(i - address_scroll + 1), 14, 0);
        if (x - text_x < previous + (w - previous) / 2) return i;
        previous = w;
    }
    return length;
}

static void address_key(const syscall_window_event_t* event) {
    int key = (int)event->key;
    if (key == KEY_ENTER) {
        if (homepage_editing) save_custom_homepage();
        else navigate_to(address_text);
        return;
    }
    if (key == KEY_ESCAPE) {
        if (homepage_editing) homepage_mode = homepage_previous_mode;
        homepage_editing = 0;
        reset_address();
        page_dirty = 1;
        return;
    }
    if (key == KEY_BACKSPACE || key == KEY_DELETE) {
        if (address_select_all) {
            address_text[0] = '\0';
            address_cursor = 0;
            address_select_all = 0;
        } else {
            size_t length = strlen(address_text);
            if (key == KEY_BACKSPACE && address_cursor > 0) {
                memmove(address_text + address_cursor - 1, address_text + address_cursor,
                        length - (size_t)address_cursor + 1);
                address_cursor--;
            } else if (key == KEY_DELETE && address_cursor < (int)length) {
                memmove(address_text + address_cursor, address_text + address_cursor + 1,
                        length - (size_t)address_cursor);
            }
        }
        update_address_scroll();
        return;
    }
    if (key == KEY_LEFT || key == KEY_RIGHT || key == KEY_HOME || key == KEY_END) {
        int length = (int)strlen(address_text);
        if (address_select_all) {
            address_cursor = key == KEY_LEFT || key == KEY_HOME ? 0 : length;
            address_select_all = 0;
        } else if (key == KEY_LEFT && address_cursor > 0) address_cursor--;
        else if (key == KEY_RIGHT && address_cursor < length) address_cursor++;
        else if (key == KEY_HOME) address_cursor = 0;
        else if (key == KEY_END) address_cursor = length;
        update_address_scroll();
        return;
    }
    if ((event->mods & SYSCALL_INPUT_MOD_CTRL) && key == KEY_A) {
        address_select_all = 1;
        address_cursor = (int)strlen(address_text);
        address_scroll = 0;
        return;
    }
    char c = mos_usb_key_to_ascii((uint8_t)event->key, (uint8_t)event->mods);
    if (c < 32 || c >= 127) return;
    if (address_select_all) {
        address_text[0] = '\0';
        address_cursor = 0;
        address_scroll = 0;
        address_select_all = 0;
    }
    size_t length = strlen(address_text);
    if (length + 1 < sizeof(address_text)) {
        memmove(address_text + address_cursor + 1, address_text + address_cursor,
                length - (size_t)address_cursor + 1);
        address_text[address_cursor++] = c;
    }
    update_address_scroll();
}

/* ====================================================================== */
/* Input                                                                  */
/* ====================================================================== */

static int devtools_top(void) {
    return (int)surface_height - mx_devtools_height();
}

static void page_scroll_to(int y) {
    mx_html_set_scroll(y, (uint32_t)page_view_height());
    page_dirty = 1;
}

static void handle_hover(int x, int y) {
    mx_ui_state_t ui = ui_state();
    mx_hit_t hit = mx_ui_hit(&ui, x, y);
    int in_devtools = devtools_open && y >= devtools_top();
    if (in_devtools) hit.kind = MX_HIT_NONE;
    if (hit.kind != hover_kind || hit.index != hover_index) {
        int chrome = (hover_kind != MX_HIT_NONE && hover_kind < MX_HIT_PAGE) ||
                     (hit.kind != MX_HIT_NONE && hit.kind < MX_HIT_PAGE);
        int page = hover_kind >= MX_HIT_SETTING || hit.kind >= MX_HIT_SETTING ||
                   hover_kind == MX_HIT_SCROLLBAR || hit.kind == MX_HIT_SCROLLBAR;
        hover_kind = hit.kind;
        hover_index = hit.index;
        if (chrome) chrome_dirty = 1;
        if (page) page_dirty = 1;
    }
    const char* link = NULL;
    if (hit.kind == MX_HIT_PAGE && page_loaded) {
        int doc_y = y - TOP + mx_html_scroll_y();
        if (mx_devtools_picking() && devtools_open) {
            mx_devtools_pick(mx_html_hit(x, doc_y), 0);
            page_dirty = 1;
        } else {
            link = mx_html_link_at(x, doc_y);
        }
    }
    if (strcmp(hover_link, link ? link : "")) {
        mx_copy(hover_link, sizeof(hover_link), link ? link : "");
        page_dirty = 1;
    }
}

static void handle_page_click(int x, int y) {
    if (!page_loaded) return;
    int doc_y = y - TOP + mx_html_scroll_y();
    if (devtools_open && mx_devtools_picking()) {
        mx_devtools_pick(mx_html_hit(x, doc_y), 1);
        page_dirty = 1;
        return;
    }
    char target[URL_LENGTH];
    target[0] = '\0';
    int navigate = mx_html_click(x, doc_y, target, sizeof(target));
    page_dirty = 1;
    if (navigate && target[0]) {
        if (mx_starts_with_nocase(target, "javascript:")) return;
        navigate_to(target);
    }
}

static void handle_mouse(const syscall_window_event_t* event) {
    int x = event->x, y = event->y;
    mouse_x = x;
    mouse_y = y;
    int left_down = (event->buttons & SYSCALL_MOUSE_BTN_LEFT) != 0;
    int pressed = left_down && !(previous_buttons & SYSCALL_MOUSE_BTN_LEFT);
    int released = !left_down && (previous_buttons & SYSCALL_MOUSE_BTN_LEFT);
    previous_buttons = event->buttons;

    if (event->type == SYSCALL_WINDOW_EVENT_MOUSE_WHEEL) {
        if (devtools_open && y >= devtools_top()) {
            if (mx_devtools_wheel(x, y, event->wheel)) page_dirty = 1;
            return;
        }
        if (y < MX_BROWSER_TAB_HEIGHT) {
            tab_scroll += event->wheel < 0 ? 1 : -1;
            mx_ui_state_t ui = ui_state();
            int visible = mx_ui_visible_tabs(&ui);
            if (tab_scroll > tab_count - visible) tab_scroll = tab_count - visible;
            if (tab_scroll < 0) tab_scroll = 0;
            chrome_dirty = 1;
        } else if (y >= TOP && page_loaded && !is_settings_page(&tabs[active_tab])) {
            mx_html_scroll(event->wheel < 0 ? 64 : -64, (uint32_t)page_view_height());
            page_dirty = 1;
            handle_hover(x, y);
        }
        return;
    }

    /* drags in progress */
    if (devtools_open && mx_devtools_resizing()) {
        if (mx_devtools_mouse(x, y, left_down, pressed, released)) {
            if (!left_down) save_settings();
            relayout();
            page_dirty = 1;
        }
        return;
    }
    if (drag_scrollbar >= 0) {
        if (!left_down) {
            drag_scrollbar = -1;
            pressed_kind = MX_HIT_NONE;
            page_dirty = 1;
            return;
        }
        int view = page_view_height();
        int content = mx_html_content_height();
        mx_ui_state_t ui = ui_state();
        int sx, sy, sw, sh;
        if (mx_ui_scrollbar(&ui, &sx, &sy, &sw, &sh)) {
            int track = view - 8 - sh;
            int pos = y - TOP - 4 - drag_scrollbar;
            if (track > 0) page_scroll_to((int)((long)pos * (content - view) / track));
        }
        return;
    }
    if (left_down && drag_tab >= 0 && y < MX_BROWSER_TAB_HEIGHT) {
        mx_ui_state_t ui = ui_state();
        mx_hit_t hit = mx_ui_hit(&ui, x, y);
        if ((hit.kind == MX_HIT_TAB || hit.kind == MX_HIT_TAB_CLOSE) && hit.index != drag_tab) {
            reorder_tab(drag_tab, hit.index);
            chrome_dirty = 1;
        }
        return;
    }
    if (released) {
        drag_tab = -1;
        if (pressed_kind != MX_HIT_NONE) {
            pressed_kind = MX_HIT_NONE;
            chrome_dirty = 1;
        }
    }

    if (devtools_open && y >= devtools_top() && !pressed_kind) {
        if (mx_devtools_mouse(x, y, left_down, pressed, released)) page_dirty = 1;
        if (mx_devtools_take_close_request()) toggle_devtools(0);
        if (pressed) {
            if (address_focused) {
                reset_address();
                chrome_dirty = 1;
            }
            mx_html_blur();
        }
        if (hover_kind != MX_HIT_NONE) {
            hover_kind = MX_HIT_NONE;
            chrome_dirty = 1;
        }
        return;
    }
    if (!pressed) {
        handle_hover(x, y);
        if (devtools_open) mx_devtools_mouse(x, y, left_down, 0, released);
        return;
    }

    mx_ui_state_t ui = ui_state();
    mx_hit_t hit = mx_ui_hit(&ui, x, y);
    pressed_kind = hit.kind;
    if (hit.kind != MX_HIT_OMNIBOX && address_focused && !homepage_editing) {
        reset_address();
        chrome_dirty = 1;
    }
    if (hit.kind != MX_HIT_PAGE) mx_html_blur();
    if (devtools_open) mx_devtools_blur();
    switch (hit.kind) {
    case MX_HIT_TAB:
        drag_tab = hit.index;
        switch_tab(hit.index);
        break;
    case MX_HIT_TAB_CLOSE: close_tab(hit.index); break;
    case MX_HIT_NEW_TAB: new_tab(); break;
    case MX_HIT_BACK: go_history(-1); break;
    case MX_HIT_FORWARD: go_history(1); break;
    case MX_HIT_RELOAD: reload(); break;
    case MX_HIT_HOME: navigate_to(homepage_url()); break;
    case MX_HIT_OMNIBOX:
        if (!address_focused) {
            address_focused = 1;
            address_select_all = 1;
            address_cursor = (int)strlen(address_text);
            address_scroll = 0;
        } else {
            address_select_all = 0;
            address_cursor = address_cursor_at_x(x);
            update_address_scroll();
        }
        break;
    case MX_HIT_THEME: set_theme(!theme_dark); break;
    case MX_HIT_DEVTOOLS: toggle_devtools(!devtools_open); break;
    case MX_HIT_SETTINGS: navigate_to("mos://settings"); break;
    case MX_HIT_SETTING:
        apply_setting(hit.index);
        page_dirty = 1;
        break;
    case MX_HIT_ERROR_ACTION:
        if (hit.index == 0) reload();
        else {
            settings_category = 2;
            navigate_to("mos://settings");
        }
        break;
    case MX_HIT_SCROLLBAR: {
        int sx, sy, sw, sh;
        if (mx_ui_scrollbar(&ui, &sx, &sy, &sw, &sh)) {
            if (y >= sy && y < sy + sh) drag_scrollbar = y - sy;
            else {
                /* page up/down toward the click */
                int view = page_view_height();
                mx_html_scroll(y < sy ? -(view - 40) : view - 40, (uint32_t)view);
                drag_scrollbar = -1;
            }
        }
        page_dirty = 1;
        break;
    }
    case MX_HIT_PAGE:
        handle_page_click(x, y);
        break;
    default:
        break;
    }
    chrome_dirty = 1;
}

static void handle_key(const syscall_window_event_t* event) {
    if (!event->buttons) return;
    int key = (int)event->key;
    int ctrl = (event->mods & SYSCALL_INPUT_MOD_CTRL) != 0;
    int shift = (event->mods & SYSCALL_INPUT_MOD_SHIFT) != 0;
    int alt = (event->mods & SYSCALL_INPUT_MOD_ALT) != 0;
    chrome_dirty = 1;
    /* browser shortcuts */
    if (key == KEY_F12 || (ctrl && shift && key == KEY_I) || (ctrl && key == KEY_Q)) {
        toggle_devtools(!devtools_open);
        return;
    }
    if (ctrl && shift && key == KEY_J) {
        if (!devtools_open) toggle_devtools(1);
        mx_devtools_focus_console();
        page_dirty = 1;
        return;
    }
    if (ctrl && key == KEY_T) { new_tab(); return; }
    if (ctrl && key == KEY_W) { close_tab(active_tab); return; }
    if ((ctrl && key == KEY_R) || key == KEY_F5) { reload(); return; }
    if (ctrl && key == KEY_TAB) {
        switch_tab((active_tab + (shift ? tab_count - 1 : 1)) % tab_count);
        return;
    }
    if (alt && key == KEY_LEFT) { go_history(-1); return; }
    if (alt && key == KEY_RIGHT) { go_history(1); return; }
    if (ctrl && key == KEY_L) {
        mx_html_blur();
        mx_devtools_blur();
        address_focused = 1;
        address_select_all = 1;
        mx_copy(address_text, sizeof(address_text), tabs[active_tab].url);
        address_cursor = (int)strlen(address_text);
        address_scroll = 0;
        return;
    }
    if (devtools_open && mx_devtools_focused()) {
        char c = mos_usb_key_to_ascii((uint8_t)event->key, (uint8_t)event->mods);
        if (mx_devtools_key(key, c, event->mods)) {
            page_dirty = 1;
            return;
        }
    }
    if (address_focused) {
        address_key(event);
        return;
    }
    if (page_loaded && !is_settings_page(&tabs[active_tab])) {
        char c = mos_usb_key_to_ascii((uint8_t)event->key, (uint8_t)event->mods);
        char target[URL_LENGTH];
        target[0] = '\0';
        int focused = mx_html_has_focus();
        int consumed = mx_html_key(key, c, event->mods, target, sizeof(target));
        page_dirty = 1;
        if (consumed == 2 && target[0]) {
            navigate_to(target);
            return;
        }
        if (focused || consumed) return;
        int view = page_view_height();
        switch (key) {
        case KEY_DOWN: mx_html_scroll(48, (uint32_t)view); break;
        case KEY_UP: mx_html_scroll(-48, (uint32_t)view); break;
        case KEY_PAGE_DOWN: mx_html_scroll(view - 48, (uint32_t)view); break;
        case KEY_PAGE_UP: mx_html_scroll(-(view - 48), (uint32_t)view); break;
        case KEY_SPACE: mx_html_scroll(shift ? -(view - 48) : view - 48, (uint32_t)view); break;
        case KEY_HOME: page_scroll_to(0); break;
        case KEY_END: page_scroll_to(1 << 28); break;
        case KEY_BACKSPACE: go_history(-1); break;
        default: break;
        }
    }
}

/* ====================================================================== */
/* Drawing                                                                */
/* ====================================================================== */

static mx_ui_state_t ui_state(void) {
    browser_tab_t* tab = &tabs[active_tab];
    mx_ui_state_t s;
    memset(&s, 0, sizeof(s));
    s.window_id = window_id;
    s.pixels = pixels;
    s.width = surface_width;
    s.height = surface_height;
    s.tabs = tabs;
    s.tab_count = tab_count;
    s.active_tab = active_tab;
    s.tab_scroll = &tab_scroll;
    s.address_focused = address_focused;
    s.address_select_all = address_select_all;
    s.address_cursor = address_cursor;
    s.address_scroll = address_scroll;
    s.address_text = address_text;
    s.loading = loading;
    s.page_loaded = page_loaded;
    s.internal_page = is_settings_page(tab) ? 1 : tab->error ? 2 : 0;
    s.status_message = status_message;
    s.hover_link = hover_link[0] ? hover_link : NULL;
    s.hover_kind = hover_kind;
    s.hover_index = hover_index;
    s.pressed_kind = pressed_kind;
    s.page_height = page_view_height();
    s.content_height = page_loaded ? mx_html_content_height() : 0;
    s.scroll_y = page_loaded ? mx_html_scroll_y() : 0;
    s.settings_category = settings_category;
    s.theme_dark = theme_dark;
    s.dark_pages = dark_pages;
    s.javascript = javascript_enabled;
    s.subresources = subresources_enabled;
    s.allow_insecure_tls = allow_insecure_tls;
    s.homepage_mode = homepage_mode;
    s.homepage_editing = homepage_editing;
    s.custom_homepage = custom_homepage;
    s.devtools_open = devtools_open;
    s.theme = &theme;
    return s;
}

static void draw_page(void) {
    long start = mx_now_ms();
    mx_ui_state_t ui = ui_state();
    browser_tab_t* tab = &tabs[active_tab];
    mx_canvas_t canvas = mx_canvas(pixels, (int)surface_width, (int)surface_height);
    int view = page_view_height();
    if (is_settings_page(tab)) {
        mx_ui_draw_settings(&ui);
    } else if (tab->error) {
        mx_ui_draw_error(&ui);
    } else if (page_loaded) {
        mx_html_draw(&canvas, TOP, view, mx_html_scroll_y());
        mx_ui_draw_page_overlay(&ui);
    } else {
        mx_ui_draw_blank(&ui);
    }
    if (devtools_open) {
        mx_devtools_context_t ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.theme = &theme;
        if (is_home_page(tab)) {
            ctx.source = home_page_html;
            ctx.source_length = home_page_html_length;
        } else if (!is_settings_page(tab)) {
            ctx.source = tab->page_source;
            ctx.source_length = tab->page_source_length;
        }
        ctx.url = tab->url;
        ctx.last_frame_ms = last_frame_ms;
        mx_devtools_draw(&canvas, 0, devtools_top(), (int)surface_width, mx_devtools_height(), &ctx);
    }
    mos_window_present(window_id, pixels, surface_width * 4u, 0, TOP, surface_width,
                       surface_height - TOP);
    last_frame_ms = mx_now_ms() - start;
}

/* ====================================================================== */
/* Main                                                                   */
/* ====================================================================== */

int main(void) {
    if (!mx_text_load_fonts())
        printf("Mxplorer: bundled fonts unavailable; using fallback text\n");
    page_buffer = (char*)malloc(MX_HTTP_BODY_MAX);
    resource_buffer = (char*)malloc(MX_HTTP_BODY_MAX);
    if (!page_buffer || !resource_buffer) {
        printf("Mxplorer: not enough memory for page buffers\n");
        return 1;
    }
    read_os_accent();
    load_settings();
    load_homepage_setting();
    load_homepage_asset();
    rebuild_theme();
    init_tab(&tabs[0]);
    tab_count = 1;
    reset_address();

    window_id = mos_window_create(0, 64, 40, WIN_W, WIN_H, "Mxplorer");
    if (!window_id) {
        printf("Mxplorer: unable to create browser window\n");
        return 1;
    }
    mos_window_set_resizable(window_id, true);
    syscall_window_info_t info;
    memset(&info, 0, sizeof(info));
    if (mos_window_get_info(window_id, &info) == SYS_SUCCESS) {
        original_style = info.style;
        have_original_style = 1;
    }
    apply_window_style();
    if (!resize_surface(WIN_W, WIN_H)) {
        printf("Mxplorer: unable to allocate browser surface\n");
        mos_window_close(window_id);
        return 1;
    }
    mx_console_add(MX_LOG_SYSTEM, "", 0, "Mxplorer 0.3 started. Press F12 for developer tools.");
    load_page(0);

    for (;;) {
        syscall_window_event_t event;
        memset(&event, 0, sizeof(event));
        while (mos_window_next_event(window_id, &event) == SYS_SUCCESS) {
            if (event.type == SYSCALL_WINDOW_EVENT_CLOSE_REQUEST) {
                running = 0;
                break;
            }
            if (event.type == SYSCALL_WINDOW_EVENT_RESIZED && event.width > 0 && event.height > 0 &&
                (surface_width != (uint32_t)event.width || surface_height != (uint32_t)event.height)) {
                if (!resize_surface((uint32_t)event.width, (uint32_t)event.height)) continue;
                relayout();
                chrome_dirty = page_dirty = 1;
            } else if (event.type == SYSCALL_WINDOW_EVENT_MOUSE ||
                       event.type == SYSCALL_WINDOW_EVENT_MOUSE_WHEEL) {
                handle_mouse(&event);
            } else if (event.type == SYSCALL_WINDOW_EVENT_KEY) {
                handle_key(&event);
            }
            if (pending_navigation_set) {
                pending_navigation_set = 0;
                navigate_to(pending_navigation);
            }
            memset(&event, 0, sizeof(event));
        }
        if (!running) break;

        syscall_window_info_t now_info;
        if (mos_window_get_info(window_id, &now_info) != SYS_SUCCESS) break;
        if (now_info.width && now_info.height &&
            (surface_width != now_info.width || surface_height != now_info.height)) {
            if (resize_surface(now_info.width, now_info.height)) {
                relayout();
                chrome_dirty = page_dirty = 1;
            }
        }
        long now = mx_now_ms();
        if (page_loaded && javascript_enabled) {
            if (mx_html_tick(now)) page_dirty = 1;
            if (pending_navigation_set) {
                pending_navigation_set = 0;
                navigate_to(pending_navigation);
            }
            if (mx_html_update()) page_dirty = 1;
            const char* title = mx_html_title();
            if (title && *title && strcmp(title, tabs[active_tab].title)) {
                mx_copy(tabs[active_tab].title, sizeof(tabs[active_tab].title), title);
                update_title();
                chrome_dirty = 1;
            }
        }
        static unsigned console_seen;
        if (devtools_open && console_seen != mx_console_version()) {
            console_seen = mx_console_version();
            page_dirty = 1;
        }
        if (now - last_theme_check >= 1500) {
            last_theme_check = now;
            uint32_t old = os_accent;
            read_os_accent();
            if (old != os_accent && rebuild_theme()) chrome_dirty = page_dirty = 1;
        }
        if (chrome_dirty) {
            mx_ui_state_t ui = ui_state();
            mx_ui_draw_chrome(&ui);
            chrome_dirty = 0;
        }
        if (page_dirty) {
            draw_page();
            page_dirty = 0;
        }
        mos_sleep(16);
    }

    mx_html_unload();
    free(pixels);
    mos_window_close(window_id);
    return 0;
}
