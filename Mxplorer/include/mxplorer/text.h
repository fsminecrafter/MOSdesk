#ifndef MXPLORER_TEXT_H
#define MXPLORER_TEXT_H

#include <stdint.h>

int mx_text_load_fonts(void);
int mx_text_width(const char* text, int size);
void mx_text_draw(uint32_t* pixels, uint32_t width, uint32_t height,
                  int x, int y, const char* text, uint32_t color, int size);

#endif
