/* Host stand-in for the SDK window API used by ui.c. */
#ifndef MX_HOST_WINDOW_H
#define MX_HOST_WINDOW_H
#include <stdint.h>
static inline uint64_t mos_window_present(uint32_t id, const uint32_t* pixels, uint32_t pitch,
                                          uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    (void)id; (void)pixels; (void)pitch; (void)x; (void)y; (void)w; (void)h;
    return 0;
}
#endif
