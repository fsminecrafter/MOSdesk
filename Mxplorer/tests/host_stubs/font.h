#ifndef MXPLORER_TEST_FONT_H
#define MXPLORER_TEST_FONT_H

#include <stdint.h>

int text_width(const char* text, int size);
void draw_text(uint32_t* pixels, uint32_t width, uint32_t height,
               int x, int y, const char* text, uint32_t color, int size);

#endif
