#include "font.h"
#include "mxplorer/text.h"

#define MX_FONT_REGULAR_PATH \
    "0:/programs/Mxplorer/Resources/fonts/Inter-Regular.otf"
#define MX_FONT_MEDIUM_PATH \
    "0:/programs/Mxplorer/Resources/fonts/Inter-Medium.otf"

int mx_text_load_fonts(void) {
    int regular_loaded = font_load(MX_FONT_REGULAR_PATH, 0);
    int medium_loaded = font_load(MX_FONT_MEDIUM_PATH, 1);
    return regular_loaded && medium_loaded;
}

int mx_text_width(const char* text, int size) {
    return text_width(text, size);
}

void mx_text_draw(uint32_t* pixels, uint32_t width, uint32_t height,
                  int x, int y, const char* text, uint32_t color, int size) {
    draw_text(pixels, width, height, x, y, text, color, size);
}
