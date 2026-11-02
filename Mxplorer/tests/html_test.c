#include "mxplorer/html.h"
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static char drawn_text[4096];
static size_t drawn_length;
static int notice_seen;
static int hidden_seen;
static int script_seen;

int text_width(const char* text, int size) {
    return (int)strlen(text) * 8 * size;
}

void draw_text(uint32_t* pixels, uint32_t width, uint32_t height,
               int x, int y, const char* text, uint32_t color, int size) {
    (void)size;
    if (strstr(text, "Styled") && color == 0xFF123456u) notice_seen = 1;
    if (strstr(text, "secret")) hidden_seen = 1;
    if (strstr(text, "scripted")) script_seen = 1;
    size_t length = strlen(text);
    if (length < sizeof(drawn_text) - drawn_length - 1) {
        memcpy(drawn_text + drawn_length, text, length);
        drawn_length += length;
        drawn_text[drawn_length] = '\0';
    }
    if (x >= 0 && y >= 0 && (uint32_t)x < width && (uint32_t)y < height)
        pixels[(size_t)y * width + (uint32_t)x] = color;
}

int main(void) {
    static const char html[] =
        "<!doctype html><html><head><style>"
        "body { background-color: #f8f8f8; color: #222222; }"
        "h1 { color: #cc0000; }"
        ".notice { color: #123456; background-color: #eeeeee; font-size: 24px; }"
        "#hidden { display: none; }"
        "</style></head><body><h1>Title</h1>"
        "<p class='notice'>Styled &amp; text</p>"
        "<p id='hidden'>secret text</p>"
        "<script>document.write('scripted')</script>"
        "<hr></body></html>";
    enum { WIDTH = 320, HEIGHT = 480 };
    uint32_t pixels[WIDTH * HEIGHT] = {0};

    assert(mx_html_parse(html, sizeof(html) - 1, WIDTH) == MX_HTML_OK);
    assert(mx_html_content_height() > 0);
    mx_html_draw(pixels, WIDTH, HEIGHT, 0);
    assert(pixels[MX_HTML_VIEW_TOP * WIDTH] == 0xFFF8F8F8u);
    assert(notice_seen);
    assert(!hidden_seen);
    assert(!script_seen);
    assert(strstr(drawn_text, "Title"));
    assert(strstr(drawn_text, "Styled"));
    assert(!strstr(drawn_text, "secret"));
    assert(!strstr(drawn_text, "scripted"));

    mx_html_scroll(10000, HEIGHT - MX_HTML_VIEW_TOP);
    int maximum_scroll = mx_html_content_height() - (HEIGHT - MX_HTML_VIEW_TOP);
    if (maximum_scroll < 0) maximum_scroll = 0;
    assert(mx_html_scroll_y() == maximum_scroll);
    assert(mx_html_parse(html, MX_HTML_SOURCE_MAX + 1, WIDTH) ==
           MX_HTML_TOO_LARGE);

    FILE* homepage = fopen("Mxplorer/Resources/homepage.html", "rb");
    assert(homepage);
    char homepage_source[8192];
    size_t homepage_length =
        fread(homepage_source, 1, sizeof(homepage_source) - 1, homepage);
    assert(!ferror(homepage));
    assert(fclose(homepage) == 0);
    homepage_source[homepage_length] = '\0';
    drawn_text[0] = '\0';
    drawn_length = 0;
    assert(mx_html_parse(homepage_source, homepage_length, WIDTH) == MX_HTML_OK);
    mx_html_draw(pixels, WIDTH, HEIGHT, 0);
    assert(strstr(drawn_text, "fresh"));
    assert(strstr(drawn_text, "start."));
    assert(strstr(homepage_source, "Google"));
    assert(strstr(homepage_source, "own homepage"));

    puts("Mxplorer HTML/CSS renderer tests passed");
    return 0;
}
