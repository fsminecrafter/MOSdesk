#ifndef MXPLORER_IMAGE_DECODE_H
#define MXPLORER_IMAGE_DECODE_H
#include <stddef.h>
#include <stdint.h>

/* Web picture formats, decoded to the same pixels a .img gives:
 * 0xAARRGGBB, not premultiplied, rows packed. PNG (every colour type,
 * transparency, interlacing), JPEG (baseline and progressive) and GIF
 * (first frame). The format is told from the first bytes, not the name.
 *
 * Returns a malloc'd buffer of w*h pixels, or NULL with *why set. */
uint32_t* mx_image_decode(const uint8_t* data, size_t size, int* w, int* h,
                          int* has_alpha, const char** why);

/* Recognised from the first bytes ("PNG", "JPEG", "GIF", "WebP", "SVG"),
 * or NULL. */
const char* mx_image_format(const uint8_t* data, size_t size);

/* The largest picture decoded, in pixels (both sides together). */
#define MX_IMAGE_MAX_PIXELS (24u * 1024u * 1024u)

#endif
