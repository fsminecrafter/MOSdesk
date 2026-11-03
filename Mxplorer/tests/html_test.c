/* Host test for the HTML/CSS engine (run from the Mxplorer directory):
 *   gcc -std=gnu11 -DMX_HOST -I include -idirafter ../../Minimal-OS-SDK/include \
 *       tests/html_test.c html.c css.c dom.c util.c text.c gfx.c icons_data.c \
 *       devlog.c tests/host/js_stub.c -o /tmp/mxplorer-html-test -lm */
#include "mxplorer/css.h"
#include "mxplorer/html.h"
#include "mxplorer/text.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { WIDTH = 480, HEIGHT = 640 };
static uint32_t pixels[WIDTH * HEIGHT];

static mx_html_options_t options(int dark) {
    mx_html_options_t o;
    memset(&o, 0, sizeof(o));
    o.url = "http://test.local/page.html";
    o.viewport_width = WIDTH;
    o.viewport_height = HEIGHT;
    o.dark = dark;
    return o;
}

int main(void) {
    mx_text_load_fonts();
    static const char html[] =
        "<!doctype html><html><head><title>  Test &amp; page </title><style>"
        "body { background-color: #f8f8f8; color: #222; margin: 0 }"
        "#hidden { display: none }"
        ".box { width: 100px; padding: 10px; border: 2px solid red; margin: 0 auto }"
        "@media (prefers-color-scheme: dark) { body { background: #101010 } }"
        "</style></head><body><h1>Title</h1>"
        "<div class=box id=box>Boxed</div>"
        "<p id=hidden>secret</p>"
        "<p><a href='/next?x=1'>link</a></p>"
        "<script>document.write('scripted')</script></body></html>";
    mx_html_options_t o = options(0);
    assert(mx_html_load(html, sizeof(html) - 1, &o) == MX_HTML_OK);
    assert(!strcmp(mx_html_title(), "Test & page"));
    assert(mx_html_background() == 0xFFF8F8F8u);
    mx_document_t* doc = mx_html_document();
    mx_node_t* box = mx_dom_by_id(doc->root, "box");
    assert(box && box->box_w == 100 + 20 + 4);
    assert(box->box_x == (WIDTH - box->box_w) / 2);          /* margin: auto */
    assert(mx_dom_by_id(doc->root, "hidden")->style->display == MX_DISPLAY_NONE);
    assert(mx_css_matches(box, "body > div.box#box") == 1);
    mx_canvas_t c = mx_canvas(pixels, WIDTH, HEIGHT);
    mx_html_draw(&c, 0, HEIGHT, 0);
    assert(pixels[0] == 0xFFF8F8F8u);

    /* the link resolves against the page */
    int found = 0;
    for (int y = 0; y < mx_html_content_height() && !found; y += 2)
        for (int x = 0; x < WIDTH && !found; x += 4) {
            const char* link = mx_html_link_at(x, y);
            if (link) {
                assert(!strcmp(link, "http://test.local/next?x=1"));
                found = 1;
            }
        }
    assert(found);

    /* dark mode switches the media query */
    o = options(1);
    assert(mx_html_load(html, sizeof(html) - 1, &o) == MX_HTML_OK);
    assert(mx_html_background() == 0xFF101010u);

    /* the bundled start page parses */
    FILE* f = fopen("Resources/homepage.html", "rb");
    assert(f);
    static char home[65536];
    size_t n = fread(home, 1, sizeof(home) - 1, f);
    fclose(f);
    o = options(0);
    assert(mx_html_load(home, n, &o) == MX_HTML_OK);
    assert(mx_html_content_height() > 300);
    puts("Mxplorer HTML/CSS engine tests passed");
    return 0;
}
