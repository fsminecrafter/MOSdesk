/* Renders Mxplorer's browser UI states to PPM files for review.
 *   ui_preview <out-dir> */
#include "mxplorer/browser.h"
#include "mxplorer/devlog.h"
#include "mxplorer/devtools.h"
#include "mxplorer/html.h"
#include "mxplorer/js.h"
#include "mxplorer/text.h"
#include "mxplorer/ui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { W = 1120, H = 760 };
static uint32_t px[W * H];
static mx_browser_tab_t tabs[4];
static int tab_scroll;

static void save(const char* dir, const char* name) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.ppm", dir, name);
    FILE* f = fopen(path, "wb");
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int i = 0; i < W * H; i++) {
        unsigned char rgb[3] = {(unsigned char)(px[i] >> 16), (unsigned char)(px[i] >> 8), (unsigned char)px[i]};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

static char* read_file(const char* path, size_t* n) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* d = malloc((size_t)size + 1);
    *n = fread(d, 1, (size_t)size, f);
    d[*n] = 0;
    fclose(f);
    return d;
}

static mx_ui_state_t base_state(const mx_browser_theme_t* t, int dark) {
    mx_ui_state_t s;
    memset(&s, 0, sizeof(s));
    s.pixels = px;
    s.width = W;
    s.height = H;
    s.tabs = tabs;
    s.tab_count = 3;
    s.active_tab = 0;
    s.tab_scroll = &tab_scroll;
    s.address_text = tabs[0].url;
    s.theme = t;
    s.theme_dark = dark;
    s.dark_pages = 1;
    s.javascript = 1;
    s.subresources = 1;
    s.page_height = H - MX_BROWSER_PAGE_Y;
    s.status_message = "";
    return s;
}

int main(int argc, char** argv) {
    const char* dir = argc > 1 ? argv[1] : ".";
    mx_text_load_fonts();
    for (int i = 0; i < 4; i++) memset(&tabs[i], 0, sizeof(tabs[i]));
    strcpy(tabs[0].url, "mos://home"); strcpy(tabs[0].title, "New Tab");
    strcpy(tabs[1].url, "https://en.wikipedia.org/wiki/Web_browser"); strcpy(tabs[1].title, "Web browser - Wikipedia");
    strcpy(tabs[2].url, "http://info.cern.ch/hypertext/WWW/TheProject.html"); strcpy(tabs[2].title, "The World Wide Web project");
    tabs[0].history_count = 2; tabs[0].history_index = 1;
    size_t n;
    char* home = read_file("Resources/homepage.html", &n);
    for (int dark = 0; dark < 2; dark++) {
        mx_browser_theme_t t;
        mx_theme_build(&t, dark, 0xFF3478D4u);
        mx_canvas_t c = mx_canvas(px, W, H);
        /* start page */
        mx_html_options_t o = {"mos://home", W, H - MX_BROWSER_PAGE_Y, dark, 0, 0, NULL, NULL, NULL, NULL, NULL};
        mx_html_load(home, n, &o);
        mx_ui_state_t s = base_state(&t, dark);
        s.page_loaded = 1;
        s.content_height = mx_html_content_height();
        s.hover_kind = MX_HIT_NEW_TAB;
        mx_ui_draw_chrome(&s);
        mx_html_draw(&c, MX_BROWSER_PAGE_Y, s.page_height, 0);
        s.hover_link = "http://frogfind.com/";
        mx_ui_draw_page_overlay(&s);
        save(dir, dark ? "home_dark" : "home_light");
        /* focused address bar + settings */
        s = base_state(&t, dark);
        strcpy(tabs[0].url, "mos://settings");
        strcpy(tabs[0].title, "Settings");
        s.internal_page = 1;
        s.settings_category = dark ? 2 : 1;
        s.hover_kind = MX_HIT_SETTING;
        s.hover_index = MX_SET_JAVASCRIPT;
        mx_ui_draw_chrome(&s);
        mx_ui_draw_settings(&s);
        save(dir, dark ? "settings_dark" : "settings_light");
        /* error page */
        s = base_state(&t, dark);
        strcpy(tabs[0].url, "https://example.com/");
        strcpy(tabs[0].title, "example.com");
        tabs[0].error = MX_PAGE_ERROR_HTTPS_OFF;
        strcpy(tabs[0].error_detail, "Unsafe HTTPS is disabled. Enable it in Settings to continue.");
        s.internal_page = 2;
        s.address_focused = 1;
        s.address_select_all = 0;
        s.address_cursor = 12;
        mx_ui_draw_chrome(&s);
        mx_ui_draw_error(&s);
        save(dir, dark ? "error_dark" : "error_light");
        tabs[0].error = 0;
        /* devtools over a JS page */
        strcpy(tabs[0].url, "http://example.org/app.html");
        strcpy(tabs[0].title, "Todo app");
        static const char page[] = "<!doctype html><html><head><title>Todo</title><style>body{font-family:sans-serif;margin:24px}"
            ".todo{display:flex;gap:8px;padding:10px;border:1px solid #ddd;border-radius:8px;margin:6px 0} h1{color:#3d5afe}</style></head>"
            "<body><h1 id=title class='big header'>Todo</h1><div id=list></div><script>"
            "for (const t of ['Write code','Test it','Ship it']) { const d = document.createElement('div'); d.className='todo'; d.textContent=t; list.appendChild(d); }"
            "console.log('rendered', 3, 'items', {ok: true}); console.warn('deprecated API'); console.info('info message');"
            "fetchData();</script></body></html>";
        mx_console_clear();
        mx_network_clear();
        mx_network_entry_t* e = mx_network_add("http://example.org/app.html", "document");
        e->status = 200; e->size = 1834; e->duration_ms = 120; strcpy(e->content_type, "text/html");
        e = mx_network_add("http://example.org/style.css", "stylesheet");
        e->status = 200; e->size = 5300; e->duration_ms = 60; e->started_ms += 100;
        e = mx_network_add("http://example.org/missing.js", "script");
        e->status = 404; e->size = 0; e->duration_ms = 40; e->started_ms += 150;
        mx_html_options_t o2 = {"http://example.org/app.html", W, 300, dark, 1, 0, NULL, NULL, NULL, NULL, NULL};
        mx_html_load(page, sizeof(page) - 1, &o2);
        char result[200];
        mx_js_eval("document.querySelectorAll('.todo').length * 7", result, sizeof(result));
        mx_console_add(MX_LOG_INPUT, "", 0, "document.querySelectorAll('.todo').length * 7");
        mx_console_add(MX_LOG_RESULT, "", 0, result);
        for (int tab = 0; tab < MX_DEVTOOLS_TAB_COUNT; tab++) {
            s = base_state(&t, dark);
            s.devtools_open = 1;
            s.page_loaded = 1;
            int dh = 330;
            s.page_height = H - MX_BROWSER_PAGE_Y - dh;
            mx_devtools_set_tab(tab);
            if (tab == MX_DEVTOOLS_ELEMENTS) {
                mx_document_t* doc = mx_html_document();
                mx_devtools_pick(mx_dom_by_id(doc->root, "title"), 1);
            }
            mx_ui_draw_chrome(&s);
            mx_html_draw(&c, MX_BROWSER_PAGE_Y, s.page_height, 0);
            mx_devtools_context_t ctx = {&t, page, sizeof(page) - 1, tabs[0].url, 9};
            mx_devtools_draw(&c, 0, H - dh, W, dh, &ctx);
            static const char* names[] = {"console", "elements", "network", "sources", "performance"};
            char name[64];
            snprintf(name, sizeof(name), "devtools_%s_%s", names[tab], dark ? "dark" : "light");
            save(dir, name);
        }
        strcpy(tabs[0].url, "mos://home");
        strcpy(tabs[0].title, "New Tab");
    }
    return 0;
}
