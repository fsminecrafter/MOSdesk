/* Host renderer: draws a page with the real layout engine into a PPM.
 *   render <url-or-file> <out.ppm> [width] [height] [dark]
 * Subresources are fetched with curl. */
#include "mxplorer/html.h"
#include "mxplorer/text.h"
#include "mxplorer/devlog.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char* read_all(FILE* f, size_t* length) {
    size_t cap = 1 << 16, used = 0;
    char* data = malloc(cap);
    size_t n;
    while ((n = fread(data + used, 1, cap - used - 1, f)) > 0) {
        used += n;
        if (used + 1 >= cap) data = realloc(data, cap *= 2);
    }
    data[used] = 0;
    *length = used;
    return data;
}

static int fetch(void* user, const char* url, const char* kind, char** body,
                 size_t* length) {
    (void)user;
    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "curl -sL --max-time 10 -A 'Mozilla/5.0 Mxplorer' '%s'", url);
    FILE* p = popen(cmd, "r");
    if (!p) return 0;
    *body = read_all(p, length);
    pclose(p);
    mx_network_entry_t* e = mx_network_add(url, kind);
    e->status = *length ? 200 : 0;
    e->size = *length;
    fprintf(stderr, "fetched %s %s (%zu bytes)\n", kind, url, *length);
    return *length > 0;
}

static void nav(const char* url, void* user) { (void)user; fprintf(stderr, "navigate -> %s\n", url); }
static void alert_fn(const char* t, void* user) { (void)user; fprintf(stderr, "alert: %s\n", t); }

int main(int argc, char** argv) {
    if (argc < 3) return 2;
    int width = argc > 3 ? atoi(argv[3]) : 1024;
    int height = argc > 4 ? atoi(argv[4]) : 1400;
    int dark = argc > 5 ? atoi(argv[5]) : 0;
    mx_text_load_fonts();
    size_t length = 0;
    char* source;
    const char* url = argv[1];
    char file_url[1024];
    if (!strncmp(url, "http", 4)) {
        fetch(NULL, url, "document", &source, &length);
    } else {
        FILE* f = fopen(url, "rb");
        if (!f) return 1;
        source = read_all(f, &length);
        fclose(f);
        snprintf(file_url, sizeof(file_url), "mos://local/%s", url);
        url = file_url;
    }
    mx_html_options_t o = {url, width, height, dark, 1, 1, fetch, NULL, nav, alert_fn, NULL};
    mx_html_result_t r = mx_html_load(source, length, &o);
    if (r != MX_HTML_OK) {
        fprintf(stderr, "load failed: %s\n", mx_html_error(r));
        return 1;
    }
    const mx_html_stats_t* st = mx_html_stats();
    fprintf(stderr, "js_heap=%zuKB title='%s' elements=%u items=%u rules=%u height=%d parse=%ldms style=%ldms layout=%ldms script=%ldms\n",
            st->js_bytes / 1024, mx_html_title(), st->elements, st->items, st->rules, mx_html_content_height(),
            st->parse_ms, st->style_ms, st->layout_ms, st->script_ms);
    uint32_t* px = calloc((size_t)width * height, 4);
    mx_canvas_t c = mx_canvas(px, width, height);
    mx_html_draw(&c, 0, height, 0);
    for (int i = 0; i < mx_console_count(); i++) {
        const mx_console_entry_t* e = mx_console_get(i);
        fprintf(stderr, "console[%d] %s:%d %s%s\n", e->level, e->source, e->line, e->text,
                e->repeat > 1 ? " (repeated)" : "");
    }
    FILE* out = fopen(argv[2], "wb");
    fprintf(out, "P6\n%d %d\n255\n", width, height);
    for (int i = 0; i < width * height; i++) {
        unsigned char rgb[3] = {(unsigned char)(px[i] >> 16), (unsigned char)(px[i] >> 8), (unsigned char)px[i]};
        fwrite(rgb, 1, 3, out);
    }
    fclose(out);
    return 0;
}
