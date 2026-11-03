#include "mxplorer/browser.h"
#include "mxplorer/html.h"
#include "mxplorer/http.h"
#include "mxplorer/text.h"
#include "mxplorer/ui.h"
#include "stdlib.h"
#include "stdio.h"
#include "string.h"
#include "window.h"
#include "x86_64/user_usb.h"

#define WIN_W 1080u
#define WIN_H 720u
#define MIN_WIN_W 640u
#define MIN_WIN_H 400u
#define MAX_TABS MX_BROWSER_MAX_TABS
#define MAX_HISTORY MX_BROWSER_MAX_HISTORY
#define URL_LENGTH MX_BROWSER_URL_LENGTH
#define TAB_WIDTH MX_BROWSER_TAB_WIDTH
#define TOOLBAR_Y MX_BROWSER_TOOLBAR_Y
#define TOOLBAR_H MX_BROWSER_TOOLBAR_HEIGHT
#define PAGE_Y MX_BROWSER_PAGE_Y
#define HOMEPAGE_GOOGLE 0
#define HOMEPAGE_MXPLORER 1
#define HOMEPAGE_CUSTOM 2
#define HOMEPAGE_SETTING_PATH "0:/data/mxplorer-homepage.txt"
#define HOMEPAGE_ASSET_PATH "0:/programs/Mxplorer/Resources/homepage.html"
#define HOMEPAGE_HTML_CAPACITY 8192u

typedef mx_browser_tab_t browser_tab_t;
typedef mx_browser_theme_t browser_theme_t;

static browser_tab_t tabs[MAX_TABS];
static int tab_count;
static int active_tab;
static int tab_scroll;
static uint32_t window_id;
static uint32_t* pixels;
static uint32_t surface_width;
static uint32_t surface_height;
static int running = 1;
static int address_focused;
static int address_select_all;
static int address_cursor;
static int address_scroll;
static char address_text[URL_LENGTH];
static int settings_category;
static int allow_insecure_tls;
static int homepage_mode = HOMEPAGE_GOOGLE;
static int homepage_previous_mode = HOMEPAGE_GOOGLE;
static char custom_homepage[URL_LENGTH];
static int homepage_editing;
static int previous_buttons;
static int drag_tab = -1;
static browser_theme_t theme;
static char status_message[96] = "Browser framework ready";
static long last_theme_check;
static char page_source[MX_HTTP_BODY_MAX];
static char home_page_html[HOMEPAGE_HTML_CAPACITY];
static size_t home_page_html_length;
static char debug_logs[8][80];
static int debug_log_count;
static int debug_open;
static int debug_tab;
static int page_loaded;
static int chrome_dirty = 1;
static int page_dirty = 1;

static const char home_page_fallback[] =
    "<!doctype html><html><head><style>"
    "body{background-color:#f5f8fc;color:#23364a}"
    ".eyebrow{color:#3478d4;font-size:12px;font-weight:bold;text-align:center;margin-top:24px;margin-bottom:8px}"
    "h1{color:#3478d4;font-size:44px;text-align:center;margin-bottom:8px}"
    ".lead{color:#60758b;font-size:16px;text-align:center;margin-bottom:18px}"
    ".search-prompt{background-color:#ffffff;color:#60758b;text-align:center;padding:14px;margin-bottom:20px}"
    "h2{color:#203d5c;font-size:20px;margin-top:12px;margin-bottom:10px}"
    ".card{background-color:#ffffff;color:#465d74;padding:12px;margin-bottom:8px}"
    ".tag{color:#3478d4;font-weight:bold;margin-bottom:4px}"
    ".note{color:#73869a;font-size:12px;margin-top:14px}"
    "</style></head><body>"
    "<p class='eyebrow'>A SIMPLE BROWSER FOR MINIMAL-OS</p>"
    "<h1>Mxplorer</h1>"
    "<p class='lead'>A little space to find what you need.</p>"
    "<p class='search-prompt'>Search or enter a website in the address bar above.</p>"
    "<h2>Quick guide</h2>"
    "<div class='card'><p class='tag'>Search and browse</p>"
    "<p>Type a search or web address above. Use Back, Forward, and + to move between pages and tabs.</p></div>"
    "<div class='card'><p class='tag'>Make it yours</p>"
    "<p>Choose Google, this start page, or a custom homepage in Settings.</p></div>"
    "<div class='card'><p class='tag'>Know what to expect</p>"
    "<p>Mxplorer renders basic HTML and CSS. Scripts are off, and HTTPS attempts are disabled by default.</p></div>"
    "<p class='note'>Unsafe HTTPS in Privacy is not secure. Never use it for passwords or private data.</p>"
    "</body></html>";

static void copy_text(char* destination, const char* source, size_t capacity) {
    size_t i = 0;
    if (!capacity) return;
    while (source && source[i] && i + 1 < capacity) {
        destination[i] = source[i];
        i++;
    }
    destination[i] = '\0';
}

static void add_debug_log(const char* prefix, const char* message) {
    if (debug_log_count == (int)(sizeof(debug_logs) / sizeof(debug_logs[0]))) {
        for (int i = 1; i < debug_log_count; i++)
            copy_text(debug_logs[i - 1], debug_logs[i], sizeof(debug_logs[i - 1]));
        debug_log_count--;
    }
    char* line = debug_logs[debug_log_count++];
    copy_text(line, prefix, sizeof(debug_logs[0]));
    size_t used = strlen(line);
    if (used + 1 < sizeof(debug_logs[0])) {
        line[used++] = ' ';
        line[used] = '\0';
    }
    copy_text(line + used, message, sizeof(debug_logs[0]) - used);
}

static int text_equal(const char* left, const char* right) {
    while (*left && *right && *left == *right) {
        left++;
        right++;
    }
    return *left == '\0' && *right == '\0';
}

static int starts_with(const char* text, const char* prefix) {
    while (*prefix) {
        if (*text++ != *prefix++) return 0;
    }
    return 1;
}

static const char* homepage_url(void) {
    if (homepage_mode == HOMEPAGE_MXPLORER) return "mos://home";
    if (homepage_mode == HOMEPAGE_CUSTOM && custom_homepage[0])
        return custom_homepage;
    return "https://google.com";
}

static void load_homepage_setting(void) {
    long fd = mos_open(HOMEPAGE_SETTING_PATH, SYS_O_RDONLY);
    if (fd < 0) return;
    char config[URL_LENGTH + 4];
    long size = mos_size(fd);
    if (size <= 0 || size >= (long)sizeof(config)) {
        mos_close(fd);
        return;
    }
    long count = mos_read(fd, config, (size_t)size);
    mos_close(fd);
    if (count < 2 || config[1] != '\n') return;
    config[count] = '\0';
    if (config[0] == '0') {
        homepage_mode = HOMEPAGE_GOOGLE;
    } else if (config[0] == '1') {
        homepage_mode = HOMEPAGE_MXPLORER;
    } else if (config[0] == '2' && count > 2) {
        size_t end = 2;
        while (end < (size_t)count && config[end] != '\n') end++;
        size_t length = end - 2;
        if (length < sizeof(custom_homepage)) {
            memcpy(custom_homepage, config + 2, length);
            custom_homepage[length] = '\0';
            if (length) homepage_mode = HOMEPAGE_CUSTOM;
        } else {
            homepage_mode = HOMEPAGE_GOOGLE;
        }
    }
}

static int save_homepage_setting(void) {
    if (!mos_exists("0:/data") && mos_mkdir("0:/data") != SYS_SUCCESS) {
        copy_text(status_message, "Could not create the browser settings folder.",
                  sizeof(status_message));
        return 0;
    }
    if (!mos_exists(HOMEPAGE_SETTING_PATH) &&
        mos_create(HOMEPAGE_SETTING_PATH, "text", "txt") != SYS_SUCCESS) {
        copy_text(status_message, "Could not create the homepage settings file.",
                  sizeof(status_message));
        return 0;
    }
    long fd = mos_open(HOMEPAGE_SETTING_PATH, SYS_O_RDWR);
    if (fd < 0) {
        copy_text(status_message, "Could not open the homepage settings file.",
                  sizeof(status_message));
        return 0;
    }
    char config[URL_LENGTH + 4];
    config[0] = (char)('0' + homepage_mode);
    config[1] = '\n';
    size_t length = 2;
    if (homepage_mode == HOMEPAGE_CUSTOM) {
        size_t custom_length = strlen(custom_homepage);
        memcpy(config + length, custom_homepage, custom_length);
        length += custom_length;
        config[length++] = '\n';
    }
    long written = mos_fwrite(fd, config, length);
    long close_result = mos_close(fd);
    if (written != (long)length || close_result != SYS_SUCCESS) {
        copy_text(status_message, "Could not save the homepage setting.",
                  sizeof(status_message));
        return 0;
    }
    return 1;
}

static void load_homepage_asset(void) {
    size_t fallback_length = sizeof(home_page_fallback) - 1;
    if (fallback_length >= sizeof(home_page_html))
        fallback_length = sizeof(home_page_html) - 1;
    memcpy(home_page_html, home_page_fallback, fallback_length);
    home_page_html[fallback_length] = '\0';
    home_page_html_length = fallback_length;

    long fd = mos_open(HOMEPAGE_ASSET_PATH, SYS_O_RDONLY);
    if (fd < 0) return;
    long size = mos_size(fd);
    if (size > 0 && size < (long)sizeof(home_page_html)) {
        long count = mos_read(fd, home_page_html, (size_t)size);
        if (count == size) {
            home_page_html[count] = '\0';
            home_page_html_length = (size_t)count;
        }
    }
    mos_close(fd);
}

static void set_default_theme(void) {
    theme.background = 0xFFF4F7FAu;
    theme.surface = 0xFFFFFFFFu;
    theme.border = 0xFFD7E0E9u;
    theme.text = 0xFF1F3042u;
    theme.muted = 0xFF687B8F;
    theme.button = 0xFFE9EFF5u;
    theme.button_hover = 0xFFDCE7F2u;
    theme.accent = 0xFF3478D4u;
    theme.accent_text = 0xFFFFFFFFu;
}

static int read_os_theme(void) {
    uint32_t header[3] = {0, 0, 0};
    uint8_t style_data[4] = {0, 0, 0, 0};
    int aero = 0;
    long fd = mos_open("0:/programs/mosdesk/settings.dat", SYS_O_RDONLY);
    if (fd <= 0) return 0;

    long header_bytes = mos_read(fd, header, sizeof(header));
    if (mos_size(fd) >= 864 && mos_seek(fd, 860) >= 0 &&
        mos_read(fd, style_data, sizeof(style_data)) == sizeof(style_data)) {
        aero = style_data[0] == 1 && !style_data[1] &&
               !style_data[2] && !style_data[3];
    }
    mos_close(fd);
    if (header_bytes != sizeof(header) || header[0] != 0x4D445331u)
        return 0;

    theme.accent = 0xFF000000u | (header[2] & 0x00FFFFFFu);
    if (aero) {
        theme.background = 0xFFF0F0F0u;
        theme.surface = 0xFFFFFFFFu;
        theme.border = 0xFFABADB3u;
        theme.text = 0xFF000000u;
        theme.muted = 0xFF6D6D6Du;
        theme.button = 0xFFE5E5E5u;
        theme.button_hover = 0xFFDCECFCu;
    }
    return 1;
}

static void init_tab(browser_tab_t* tab) {
    memset(tab, 0, sizeof(*tab));
    copy_text(tab->url, homepage_url(), sizeof(tab->url));
    if (homepage_mode == HOMEPAGE_MXPLORER)
        copy_text(tab->title, "Mxplorer Home", sizeof(tab->title));
    else if (homepage_mode == HOMEPAGE_CUSTOM)
        copy_text(tab->title, "Custom Home", sizeof(tab->title));
    else
        copy_text(tab->title, "Google", sizeof(tab->title));
    copy_text(tab->history[0], tab->url, sizeof(tab->history[0]));
    tab->history_count = 1;
}

static int is_settings_page(const browser_tab_t* tab) {
    return text_equal(tab->url, "mos://settings");
}

static int is_home_page(const browser_tab_t* tab) {
    return text_equal(tab->url, "mos://home");
}

static int resize_surface(uint32_t width, uint32_t height) {
    if (width < MIN_WIN_W || height < MIN_WIN_H ||
        (size_t)width > SIZE_MAX / height / sizeof(uint32_t))
        return 0;
    uint32_t* new_pixels =
        (uint32_t*)malloc((size_t)width * height * sizeof(uint32_t));
    if (!new_pixels) return 0;
    free(pixels);
    pixels = new_pixels;
    surface_width = width;
    surface_height = height;
    return 1;
}

static void update_title(void) {
    if (is_settings_page(&tabs[active_tab]))
        mos_window_set_title(window_id, "Mxplorer - Settings");
    else if (is_home_page(&tabs[active_tab]))
        mos_window_set_title(window_id, "Mxplorer Home");
    else
        mos_window_set_title(window_id, "Mxplorer");
}

static void ensure_active_visible(void) {
    int visible = ((int)surface_width - 64) / TAB_WIDTH;
    if (visible < 1) visible = 1;
    if (active_tab < tab_scroll) tab_scroll = active_tab;
    if (active_tab >= tab_scroll + visible)
        tab_scroll = active_tab - visible + 1;
    if (tab_scroll < 0) tab_scroll = 0;
}

static void set_tab_title(browser_tab_t* tab, int search_query) {
    if (text_equal(tab->url, "mos://settings")) {
        copy_text(tab->title, "Settings", sizeof(tab->title));
        return;
    }
    if (text_equal(tab->url, "mos://home")) {
        copy_text(tab->title, "Mxplorer Home", sizeof(tab->title));
        return;
    }
    if (search_query) {
        copy_text(tab->title, "Google Search", sizeof(tab->title));
        return;
    }
    const char* host = strstr(tab->url, "://");
    host = host ? host + 3 : tab->url;
    size_t length = 0;
    while (host[length] && host[length] != '/' && host[length] != '?' &&
           host[length] != ':' && length + 1 < sizeof(tab->title)) {
        tab->title[length] = host[length];
        length++;
    }
    tab->title[length] = '\0';
    if (!length) copy_text(tab->title, "New Tab", sizeof(tab->title));
}

static void load_page(int force_reload) {
    browser_tab_t* tab = &tabs[active_tab];
    page_loaded = 0;
    if (is_settings_page(tab)) {
        copy_text(status_message, "Mxplorer internal settings", sizeof(status_message));
        add_debug_log("PAGE", status_message);
        page_dirty = 1;
        return;
    }
    if (is_home_page(tab)) {
        mx_html_result_t result =
            mx_html_parse(home_page_html, home_page_html_length, surface_width);
        page_loaded = result == MX_HTML_OK;
        copy_text(status_message,
                  page_loaded ? "Mxplorer home rendered"
                              : mx_html_error(result),
                  sizeof(status_message));
        add_debug_log(page_loaded ? "HTML" : "ERR", status_message);
        page_dirty = 1;
        return;
    }

    char error[sizeof(status_message)];
    size_t body_length = 0;
    copy_text(status_message, "Loading page...", sizeof(status_message));
    add_debug_log("GET", tab->url);
    page_dirty = 1;
    if (!force_reload && tab->page_source &&
        text_equal(tab->source_url, tab->url)) {
        mx_html_result_t cached =
            mx_html_parse(tab->page_source, tab->page_source_length,
                          surface_width);
        if (cached == MX_HTML_OK) {
            page_loaded = 1;
            copy_text(status_message, "Page restored from this tab's cache",
                      sizeof(status_message));
            return;
        }
    }
    if (!mx_http_get(tab->url, page_source, sizeof(page_source), &body_length,
                     allow_insecure_tls, error, sizeof(error))) {
        copy_text(status_message, error, sizeof(status_message));
        add_debug_log("ERR", status_message);
        return;
    }
    mx_html_result_t result =
        mx_html_parse(page_source, body_length, surface_width);
    if (result != MX_HTML_OK) {
        copy_text(status_message, mx_html_error(result), sizeof(status_message));
        add_debug_log("ERR", status_message);
        return;
    }
    char* saved_source = (char*)malloc(body_length + 1);
    if (!saved_source) {
        copy_text(status_message, "Not enough memory to cache this page.",
                  sizeof(status_message));
        return;
    }
    memcpy(saved_source, page_source, body_length + 1);
    free(tab->page_source);
    tab->page_source = saved_source;
    tab->page_source_length = body_length;
    copy_text(tab->source_url, tab->url, sizeof(tab->source_url));
    page_loaded = 1;
    copy_text(status_message, "HTML page loaded", sizeof(status_message));
    add_debug_log("HTML", status_message);
    page_dirty = 1;
}

static void reflow_page(void) {
    if (!page_loaded) {
        page_dirty = 1;
        return;
    }
    mx_html_result_t result =
        mx_html_parse(tabs[active_tab].page_source,
                      tabs[active_tab].page_source_length, surface_width);
    if (result != MX_HTML_OK) {
        page_loaded = 0;
        copy_text(status_message, mx_html_error(result), sizeof(status_message));
    }
    page_dirty = 1;
}

static void push_history(browser_tab_t* tab, const char* url) {
    if (tab->history_count > 0 &&
        text_equal(tab->history[tab->history_index], url))
        return;
    if (tab->history_index + 1 < tab->history_count)
        tab->history_count = tab->history_index + 1;
    if (tab->history_count == MAX_HISTORY) {
        for (int i = 1; i < MAX_HISTORY; i++)
            copy_text(tab->history[i - 1], tab->history[i], URL_LENGTH);
        tab->history_count--;
        tab->history_index--;
    }
    tab->history_index = tab->history_count++;
    copy_text(tab->history[tab->history_index], url, URL_LENGTH);
}

static int encode_query(char* output, size_t capacity, const char* query) {
    static const char hex[] = "0123456789ABCDEF";
    size_t used = 0;
    for (size_t i = 0; query[i]; i++) {
        unsigned char c = (unsigned char)query[i];
        int safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                   c == '.' || c == '~';
        size_t needed = safe ? 1 : 3;
        if (used + needed >= capacity) return 0;
        if (safe) {
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

static int starts_with_nocase(const char* text, const char* prefix) {
    while (*prefix) {
        char a = *text++;
        char b = *prefix++;
        if (a >= 'A' && a <= 'Z') a = (char)(a + ('a' - 'A'));
        if (b >= 'A' && b <= 'Z') b = (char)(b + ('a' - 'A'));
        if (a != b) return 0;
    }
    return 1;
}

static int normalize_address(const char* input, char* output, size_t capacity,
                             int* is_search) {
    char encoded[URL_LENGTH];
    int has_dot = 0;
    int has_colon = 0;
    int has_space = 0;
    size_t length = 0;
    while (input[length]) {
        if (input[length] == '.') has_dot = 1;
        if (input[length] == ':') has_colon = 1;
        if (input[length] == ' ' || input[length] == '\t') has_space = 1;
        length++;
    }
    *is_search = 0;
    if (!length) return 0;
    if (starts_with_nocase(input, "mos://") ||
        starts_with_nocase(input, "http://") ||
        starts_with_nocase(input, "https://")) {
        size_t scheme_length =
            starts_with_nocase(input, "mos://") ? 6 :
            starts_with_nocase(input, "http://") ? 7 : 8;
        if (length >= capacity) return 0;
        for (size_t i = 0; i < scheme_length; i++)
            output[i] = input[i] >= 'A' && input[i] <= 'Z'
                            ? (char)(input[i] + ('a' - 'A')) : input[i];
        copy_text(output + scheme_length, input + scheme_length,
                  capacity - scheme_length);
        return 1;
    }
    if (has_space || (!has_dot && !has_colon)) {
        static const char prefix[] = "https://google.com/search?q=";
        if (!encode_query(encoded, sizeof(encoded), input) ||
            sizeof(prefix) - 1 + strlen(encoded) >= capacity)
            return 0;
        copy_text(output, prefix, capacity);
        copy_text(output + sizeof(prefix) - 1, encoded,
                  capacity - (sizeof(prefix) - 1));
        *is_search = 1;
        return 1;
    }
    if (length + sizeof("https://") > capacity) return 0;
    copy_text(output, "https://", capacity);
    copy_text(output + sizeof("https://") - 1, input,
              capacity - (sizeof("https://") - 1));
    return 1;
}

static void navigate_to(const char* input) {
    char destination[URL_LENGTH];
    int is_search = 0;
    if (!normalize_address(input, destination, sizeof(destination), &is_search)) {
        copy_text(status_message, "Address is empty or too long", sizeof(status_message));
        return;
    }
    browser_tab_t* tab = &tabs[active_tab];
    homepage_editing = 0;
    copy_text(tab->url, destination, sizeof(tab->url));
    set_tab_title(tab, is_search);
    push_history(tab, destination);
    copy_text(address_text, destination, sizeof(address_text));
    address_focused = 0;
    address_select_all = 0;
    address_cursor = (int)strlen(address_text);
    address_scroll = 0;
    load_page(0);
    chrome_dirty = 1;
    page_dirty = 1;
    update_title();
}

static void go_back(void) {
    browser_tab_t* tab = &tabs[active_tab];
    if (tab->history_index <= 0) return;
    tab->history_index--;
    copy_text(tab->url, tab->history[tab->history_index], sizeof(tab->url));
    set_tab_title(tab, 0);
    copy_text(address_text, tab->url, sizeof(address_text));
    address_cursor = (int)strlen(address_text);
    address_scroll = 0;
    load_page(0);
    chrome_dirty = 1;
    page_dirty = 1;
    update_title();
}

static void go_forward(void) {
    browser_tab_t* tab = &tabs[active_tab];
    if (tab->history_index + 1 >= tab->history_count) return;
    tab->history_index++;
    copy_text(tab->url, tab->history[tab->history_index], sizeof(tab->url));
    set_tab_title(tab, 0);
    copy_text(address_text, tab->url, sizeof(address_text));
    address_cursor = (int)strlen(address_text);
    address_scroll = 0;
    load_page(0);
    chrome_dirty = 1;
    page_dirty = 1;
    update_title();
}

static void new_tab(void) {
    if (tab_count >= MAX_TABS) {
        copy_text(status_message, "Maximum number of tabs reached", sizeof(status_message));
        return;
    }
    init_tab(&tabs[tab_count]);
    active_tab = tab_count++;
    copy_text(address_text, tabs[active_tab].url, sizeof(address_text));
    address_focused = 0;
    address_cursor = (int)strlen(address_text);
    address_scroll = 0;
    load_page(0);
    chrome_dirty = 1;
    page_dirty = 1;
    ensure_active_visible();
    update_title();
}

static void close_tab(int index) {
    if (index < 0 || index >= tab_count) return;
    if (tab_count == 1) {
        free(tabs[0].page_source);
        init_tab(&tabs[0]);
        active_tab = 0;
    } else {
        free(tabs[index].page_source);
        for (int i = index + 1; i < tab_count; i++) tabs[i - 1] = tabs[i];
        tab_count--;
        if (active_tab > index) active_tab--;
        else if (active_tab == index && active_tab >= tab_count) active_tab = tab_count - 1;
    }
    drag_tab = -1;
    copy_text(address_text, tabs[active_tab].url, sizeof(address_text));
    address_focused = 0;
    address_cursor = (int)strlen(address_text);
    address_scroll = 0;
    load_page(0);
    chrome_dirty = 1;
    page_dirty = 1;
    ensure_active_visible();
    update_title();
}

static void reorder_tab(int from, int to) {
    if (from < 0 || to < 0 || from >= tab_count || to >= tab_count ||
        from == to)
        return;
    browser_tab_t moved = tabs[from];
    if (from < to) {
        for (int i = from; i < to; i++) tabs[i] = tabs[i + 1];
    } else {
        for (int i = from; i > to; i--) tabs[i] = tabs[i - 1];
    }
    tabs[to] = moved;
    if (active_tab == from) active_tab = to;
    else if (from < active_tab && active_tab <= to) active_tab--;
    else if (to <= active_tab && active_tab < from) active_tab++;
    drag_tab = to;
}

static int visible_tab_at(int x) {
    int first_x = 8;
    int index = tab_scroll + (x - first_x) / TAB_WIDTH;
    if (x < first_x || index < tab_scroll || index >= tab_count ||
        x >= (int)surface_width - 48)
        return -1;
    int tab_x = first_x + (index - tab_scroll) * TAB_WIDTH;
    if (x >= tab_x + TAB_WIDTH) return -1;
    return index;
}

static void activate_category_at(int x, int y) {
    int top = PAGE_Y + 78;
    if (x >= 12 && x < 194 && y >= top && y < top + 4 * 44) {
        int category = (y - top) / 44;
        if (category >= 0 && category < 4) settings_category = category;
    }
}

static int activate_privacy_setting_at(int x, int y) {
    int content_x = 238;
    int setting_y = PAGE_Y + 20 + 90;
    if (settings_category != 2 || x < content_x ||
        x >= (int)surface_width - 28 || y < setting_y ||
        y >= setting_y + 76)
        return 0;
    allow_insecure_tls = !allow_insecure_tls;
    copy_text(status_message,
              allow_insecure_tls
                  ? "Unsafe HTTPS enabled: the SDK TLS transport is not secure."
                  : "Unsafe HTTPS disabled.",
              sizeof(status_message));
    return 1;
}

static int activate_homepage_setting_at(int x, int y) {
    int content_x = 238;
    int content_y = PAGE_Y + 20;
    if (settings_category != 0 || x < content_x ||
        x >= (int)surface_width - 24)
        return 0;
    int selected_mode = -1;
    if (y >= content_y + 112 && y < content_y + 140)
        selected_mode = HOMEPAGE_GOOGLE;
    else if (y >= content_y + 140 && y < content_y + 168)
        selected_mode = HOMEPAGE_MXPLORER;
    else if (y >= content_y + 168 && y < content_y + 196)
        selected_mode = HOMEPAGE_CUSTOM;
    else if (y >= content_y + 201 && y < content_y + 244)
        selected_mode = HOMEPAGE_CUSTOM;
    if (selected_mode < 0) return 0;

    if (selected_mode == HOMEPAGE_CUSTOM) {
        homepage_previous_mode = homepage_mode;
        homepage_mode = HOMEPAGE_CUSTOM;
        homepage_editing = 1;
        address_focused = 1;
        copy_text(address_text, custom_homepage, sizeof(address_text));
        address_cursor = (int)strlen(address_text);
        address_scroll = 0;
        address_select_all = address_cursor != 0;
        copy_text(status_message,
                  "Edit the address above, then press Enter to save your homepage.",
                  sizeof(status_message));
    } else {
        homepage_mode = selected_mode;
        homepage_editing = 0;
        address_focused = 0;
        address_select_all = 0;
        copy_text(address_text, tabs[active_tab].url, sizeof(address_text));
        address_cursor = (int)strlen(address_text);
        address_scroll = 0;
        if (save_homepage_setting())
            copy_text(status_message,
                      selected_mode == HOMEPAGE_GOOGLE
                          ? "Homepage set to Google."
                          : "Homepage set to Mxplorer alternative.",
                      sizeof(status_message));
    }
    return 1;
}

static int address_cursor_at_x(int x) {
    int text_x = 200;
    if (x <= text_x) return address_scroll;
    int cursor = 0;
    int previous_width = 0;
    char prefix[URL_LENGTH];
    size_t length = strlen(address_text);
    if (length >= sizeof(prefix)) length = sizeof(prefix) - 1;
    for (size_t i = (size_t)address_scroll; i < length; i++) {
        size_t visible_length = i - (size_t)address_scroll + 1;
        memcpy(prefix, address_text + address_scroll, visible_length);
        prefix[visible_length] = '\0';
        int width = mx_text_width(prefix, 1);
        if (x - text_x < previous_width + (width - previous_width) / 2)
            return (int)i;
        cursor = (int)i + 1;
        previous_width = width;
    }
    return cursor ? cursor : address_scroll;
}

static void update_address_scroll(void) {
    int available = (int)surface_width - 310;
    if (available < 20) available = 20;
    int length = (int)strlen(address_text);
    if (address_cursor < 0) address_cursor = 0;
    if (address_cursor > length) address_cursor = length;
    if (address_select_all) address_scroll = 0;
    if (address_scroll > address_cursor) address_scroll = address_cursor;
    char visible_prefix[URL_LENGTH];
    while (address_scroll < address_cursor) {
        int count = address_cursor - address_scroll;
        memcpy(visible_prefix, address_text + address_scroll, (size_t)count);
        visible_prefix[count] = '\0';
        if (mx_text_width(visible_prefix, 1) <= available) break;
        address_scroll++;
    }
}

static void handle_mouse(const syscall_window_event_t* event) {
    int left_down = (event->buttons & SYSCALL_MOUSE_BTN_LEFT) != 0;
    int pressed = left_down && !(previous_buttons & SYSCALL_MOUSE_BTN_LEFT);
    int released = !left_down && (previous_buttons & SYSCALL_MOUSE_BTN_LEFT);
    if (event->type == SYSCALL_WINDOW_EVENT_MOUSE_WHEEL) {
        if (event->y < 44) {
            int visible = ((int)surface_width - 64) / TAB_WIDTH;
            if (visible < 1) visible = 1;
            tab_scroll += event->wheel < 0 ? 1 : -1;
            if (tab_scroll < 0) tab_scroll = 0;
            if (tab_scroll > tab_count - visible)
                tab_scroll = tab_count > visible ? tab_count - visible : 0;
            chrome_dirty = 1;
        } else if (event->y >= (int)MX_HTML_VIEW_TOP &&
                   (!debug_open ||
                    event->y < (int)surface_height -
                                   MX_BROWSER_DEBUG_PANEL_HEIGHT) &&
                   !is_settings_page(&tabs[active_tab]) && page_loaded) {
            mx_html_scroll(event->wheel < 0 ? 48 : -48,
                           surface_height - MX_HTML_VIEW_TOP -
                               (debug_open ? MX_BROWSER_DEBUG_PANEL_HEIGHT : 0));
            page_dirty = 1;
        }
        return;
    }
    if (pressed) {
        if (debug_open &&
            event->y >= (int)surface_height - MX_BROWSER_DEBUG_PANEL_HEIGHT) {
            int panel_top =
                (int)surface_height - MX_BROWSER_DEBUG_PANEL_HEIGHT;
            if (event->y < panel_top + 38) {
                if (event->x >= (int)surface_width - 44)
                    debug_open = 0;
                else {
                    debug_tab = (event->x - 170) / 112;
                    if (debug_tab < 0) debug_tab = 0;
                    if (debug_tab > 2) debug_tab = 2;
                }
            }
            page_dirty = 1;
            previous_buttons = event->buttons;
            return;
        }
        address_focused = 0;
        if (event->y < 44) {
            if (event->x >= (int)surface_width - 46) {
                new_tab();
            } else {
                int index = visible_tab_at(event->x);
                if (index >= 0) {
                    int tab_x = 8 + (index - tab_scroll) * TAB_WIDTH;
                    if (event->x >= tab_x + TAB_WIDTH - 38)
                        close_tab(index);
                    else {
                        active_tab = index;
                        drag_tab = index;
                        copy_text(address_text, tabs[active_tab].url,
                                  sizeof(address_text));
                        address_cursor = (int)strlen(address_text);
                        address_scroll = 0;
                        update_title();
                        load_page(0);
                        page_dirty = 1;
                    }
                }
            }
            chrome_dirty = 1;
        } else if (event->y >= TOOLBAR_Y &&
                   event->y < TOOLBAR_Y + TOOLBAR_H) {
            if (event->x < 40) go_back();
            else if (event->x < 82) go_forward();
            else if (event->x < 122) {
                tabs[active_tab].refresh_count++;
                load_page(1);
            } else if (event->x < 156) {
                navigate_to(homepage_url());
            } else if (event->x >= (int)surface_width - 92) {
                navigate_to("mos://settings");
            } else {
                address_focused = 1;
                address_select_all = 0;
                address_cursor = address_cursor_at_x(event->x);
                update_address_scroll();
            }
            chrome_dirty = 1;
            page_dirty = 1;
        } else if (is_settings_page(&tabs[active_tab])) {
            activate_category_at(event->x, event->y);
            if (!activate_homepage_setting_at(event->x, event->y))
                activate_privacy_setting_at(event->x, event->y);
            page_dirty = 1;
        }
    }
    if (left_down && drag_tab >= 0 && event->y < 44) {
        int target = visible_tab_at(event->x);
        if (target >= 0 && target != drag_tab) {
            reorder_tab(drag_tab, target);
            ensure_active_visible();
            chrome_dirty = 1;
        }
    }
    if (released) drag_tab = -1;
    previous_buttons = event->buttons;
}

static void replace_selected_address(void) {
    if (!address_select_all) return;
    address_text[0] = '\0';
    address_cursor = 0;
    address_scroll = 0;
    address_select_all = 0;
}

static void insert_address_character(char character) {
    if (character < 32 || character >= 127) return;
    replace_selected_address();
    size_t length = strlen(address_text);
    if (length + 1 < sizeof(address_text) &&
        address_cursor >= 0 && address_cursor <= (int)length) {
        memmove(address_text + address_cursor + 1,
                address_text + address_cursor, length - (size_t)address_cursor + 1);
        address_text[address_cursor++] = character;
    }
    update_address_scroll();
}

static void save_custom_homepage(void) {
    char normalized[URL_LENGTH];
    int is_search = 0;
    if (!address_text[0] ||
        !normalize_address(address_text, normalized, sizeof(normalized),
                           &is_search) ||
        is_search ||
        (!starts_with_nocase(normalized, "http://") &&
         !starts_with_nocase(normalized, "https://"))) {
        copy_text(status_message,
                  "Enter a website address for your custom homepage.",
                  sizeof(status_message));
        page_dirty = 1;
        return;
    }
    copy_text(custom_homepage, normalized, sizeof(custom_homepage));
    homepage_mode = HOMEPAGE_CUSTOM;
    homepage_editing = 0;
    address_focused = 0;
    address_select_all = 0;
    copy_text(address_text, custom_homepage, sizeof(address_text));
    address_cursor = (int)strlen(address_text);
    address_scroll = 0;
    if (save_homepage_setting())
        copy_text(status_message, "Custom homepage saved.", sizeof(status_message));
    chrome_dirty = 1;
    page_dirty = 1;
}

static void handle_key(const syscall_window_event_t* event) {
    if (!event->buttons) return;
    if ((event->mods & SYSCALL_INPUT_MOD_CTRL) && event->key == 20u) {
        debug_open = !debug_open;
        if (debug_open && page_loaded)
            mx_html_scroll(0, surface_height - MX_HTML_VIEW_TOP -
                                  MX_BROWSER_DEBUG_PANEL_HEIGHT);
        page_dirty = 1;
        return;
    }
    if ((event->mods & SYSCALL_INPUT_MOD_ALT) && event->key == 80u) {
        go_back();
        return;
    }
    if ((event->mods & SYSCALL_INPUT_MOD_ALT) && event->key == 79u) {
        go_forward();
        return;
    }
    if ((event->mods & SYSCALL_INPUT_MOD_CTRL) && event->key == 15u) {
        address_focused = 1;
        address_select_all = 1;
        copy_text(address_text, tabs[active_tab].url, sizeof(address_text));
        address_cursor = (int)strlen(address_text);
        address_scroll = 0;
        chrome_dirty = 1;
        return;
    }
    if (address_focused &&
        (event->mods & SYSCALL_INPUT_MOD_CTRL) && event->key == 4u) {
        address_select_all = 1;
        address_cursor = (int)strlen(address_text);
        address_scroll = 0;
        chrome_dirty = 1;
        return;
    }
    if (!address_focused) return;
    if (event->key == 40u) {
        if (homepage_editing)
            save_custom_homepage();
        else
            navigate_to(address_text);
    } else if (event->key == 41u) {
        if (homepage_editing) {
            homepage_mode = homepage_previous_mode;
            page_dirty = 1;
        }
        homepage_editing = 0;
        address_focused = 0;
        address_select_all = 0;
        address_scroll = 0;
        copy_text(address_text, tabs[active_tab].url, sizeof(address_text));
        address_cursor = (int)strlen(address_text);
        chrome_dirty = 1;
    } else if (event->key == 42u) {
        replace_selected_address();
        if (address_cursor > 0) {
            size_t length = strlen(address_text);
            memmove(address_text + address_cursor - 1,
                    address_text + address_cursor,
                    length - (size_t)address_cursor + 1);
            address_cursor--;
        }
        update_address_scroll();
        chrome_dirty = 1;
    } else if (event->key == 76u) {
        replace_selected_address();
        size_t length = strlen(address_text);
        if (address_cursor < (int)length)
            memmove(address_text + address_cursor,
                    address_text + address_cursor + 1,
                    length - (size_t)address_cursor);
        update_address_scroll();
        chrome_dirty = 1;
    } else if (event->key == 80u || event->key == 79u ||
               event->key == 74u || event->key == 77u) {
        int length = (int)strlen(address_text);
        if (address_select_all) {
            address_cursor = event->key == 80u || event->key == 74u ? 0 : length;
            address_select_all = 0;
        } else if (event->key == 80u && address_cursor > 0) address_cursor--;
        else if (event->key == 79u && address_cursor < length) address_cursor++;
        else if (event->key == 74u) address_cursor = 0;
        else if (event->key == 77u) address_cursor = length;
        update_address_scroll();
        chrome_dirty = 1;
    } else {
        char character = mos_usb_key_to_ascii((uint8_t)event->key,
                                              (uint8_t)event->mods);
        insert_address_character(character);
        chrome_dirty = 1;
    }
}

static mx_ui_state_t current_ui_state(void) {
    const char* debug_source = NULL;
    size_t debug_source_length = 0;
    if (is_home_page(&tabs[active_tab])) {
        debug_source = home_page_html;
        debug_source_length = home_page_html_length;
    } else {
        debug_source = tabs[active_tab].page_source;
        debug_source_length = tabs[active_tab].page_source_length;
    }
    mx_ui_state_t state = {
        .window_id = window_id,
        .pixels = pixels,
        .width = surface_width,
        .height = surface_height,
        .tabs = tabs,
        .tab_count = tab_count,
        .active_tab = active_tab,
        .tab_scroll = &tab_scroll,
        .address_focused = address_focused,
        .address_select_all = address_select_all,
        .address_cursor = address_cursor,
        .address_scroll = address_scroll,
        .settings_category = settings_category,
        .page_loaded = page_loaded,
        .allow_insecure_tls = allow_insecure_tls,
        .homepage_mode = homepage_mode,
        .homepage_editing = homepage_editing,
        .custom_homepage = custom_homepage,
        .debug_open = debug_open,
        .debug_tab = debug_tab,
        .debug_logs = debug_logs,
        .debug_log_count = debug_log_count,
        .debug_source = debug_source,
        .debug_source_length = debug_source_length,
        .address_text = address_text,
        .status_message = status_message,
        .theme = &theme
    };
    return state;
}

static int reload_theme(void) {
    browser_theme_t previous = theme;
    set_default_theme();
    read_os_theme();
    return memcmp(&previous, &theme, sizeof(theme)) != 0;
}

int main(void) {
    if (!mx_text_load_fonts())
        printf("Mxplorer: bundled Inter fonts unavailable; using fallback text\n");
    set_default_theme();
    read_os_theme();
    load_homepage_setting();
    load_homepage_asset();
    init_tab(&tabs[0]);
    tab_count = 1;
    copy_text(address_text, tabs[0].url, sizeof(address_text));

    window_id = mos_window_create(0, 72, 54, WIN_W, WIN_H, "Mxplorer");
    if (!window_id) {
        printf("Mxplorer: unable to create browser window\n");
        return 1;
    }
    mos_window_set_resizable(window_id, true);
    if (!resize_surface(WIN_W, WIN_H)) {
        printf("Mxplorer: unable to allocate browser surface\n");
        mos_window_close(window_id);
        return 1;
    }
    load_page(0);
    chrome_dirty = 1;
    page_dirty = 1;

    for (;;) {
        syscall_window_event_t event = {0};
        while (mos_window_next_event(window_id, &event) == SYS_SUCCESS) {
            if (event.type == SYSCALL_WINDOW_EVENT_CLOSE_REQUEST) {
                running = 0;
                break;
            }
            if (event.type == SYSCALL_WINDOW_EVENT_RESIZED &&
                event.width > 0 && event.height > 0 &&
                (surface_width != (uint32_t)event.width ||
                 surface_height != (uint32_t)event.height)) {
                if (!resize_surface((uint32_t)event.width,
                                    (uint32_t)event.height)) {
                    printf("Mxplorer: unable to resize browser surface\n");
                    free(pixels);
                    mos_window_close(window_id);
                    return 1;
                }
                reflow_page();
                chrome_dirty = 1;
            } else if (event.type == SYSCALL_WINDOW_EVENT_MOUSE ||
                       event.type == SYSCALL_WINDOW_EVENT_MOUSE_WHEEL) {
                handle_mouse(&event);
            } else if (event.type == SYSCALL_WINDOW_EVENT_KEY) {
                handle_key(&event);
            }
        }
        if (!running) break;

        syscall_window_info_t info = {0};
        if (mos_window_get_info(window_id, &info) != SYS_SUCCESS) {
            printf("Mxplorer: browser window is no longer available\n");
            free(pixels);
            return 1;
        }
        if (info.width && info.height &&
            (surface_width != info.width || surface_height != info.height)) {
            if (!resize_surface(info.width, info.height)) {
                printf("Mxplorer: unable to resize browser surface\n");
                free(pixels);
                mos_window_close(window_id);
                return 1;
            }
            reflow_page();
            chrome_dirty = 1;
        }
        long now = mos_uptime();
        if (now - last_theme_check >= 1000) {
            last_theme_check = now;
            if (reload_theme()) {
                chrome_dirty = 1;
                page_dirty = 1;
            }
        }
        mx_ui_state_t ui = current_ui_state();
        if (chrome_dirty) {
            mx_ui_draw_chrome(&ui);
            chrome_dirty = 0;
        }
        if (page_dirty) {
            mx_ui_draw_content(&ui);
            page_dirty = 0;
        }
        mos_sleep(16);
    }

    free(pixels);
    mos_window_close(window_id);
    return 0;
}
