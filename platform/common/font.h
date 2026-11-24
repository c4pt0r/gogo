/* In-memory 12 px bitmap font (blob built by cmd/gogo-font; format in
 * platform/common/font/README.md). Pure C99, no allocation. */
#ifndef GOGO_FONT_H
#define GOGO_FONT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FONT_HEIGHT 12
#define FONT_HALF_W 6
#define FONT_WIDE_W 12
#define FONT_HALF_BYTES 12 /* 12 rows x 1 byte, bit 7 = leftmost pixel */
#define FONT_WIDE_BYTES 18 /* 12x12 bits packed row-major, MSB first */

typedef struct Font {
    const uint8_t *half_cp; /* n_half sorted u32 little-endian code points */
    const uint8_t *wide_cp;
    const uint8_t *half_bm;
    const uint8_t *wide_bm;
    uint32_t n_half, n_wide;
} Font;

/* Validates the blob (magic, version, sizes) and points the Font into it.
 * The blob must outlive the Font. */
bool font_init(Font *f, const uint8_t *blob, size_t n);

/* Bitmap of cp, or NULL if the font has no glyph. *width is set to 6 or 12
 * (FONT_HALF_BYTES / FONT_WIDE_BYTES bitmap bytes) when found. */
const uint8_t *font_glyph(const Font *f, uint32_t cp, int *width);

/* Pixel (x, y) of a glyph returned by font_glyph with that width. */
static inline bool font_pixel(const uint8_t *bm, int width, int x, int y) {
    if (width == FONT_HALF_W) return (bm[y] >> (7 - x)) & 1;
    int bit = y * FONT_WIDE_W + x;
    return (bm[bit >> 3] >> (7 - (bit & 7))) & 1;
}

#endif
