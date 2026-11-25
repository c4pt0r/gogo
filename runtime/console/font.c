#include "font.h"

/* Blob layout (little-endian), see runtime/console/font/README.md:
 *   0  "GGFN"        magic
 *   4  u8 version    1
 *   5  u8 height     12
 *   6  u16 reserved  0
 *   8  u32 n_half
 *  12  u32 n_wide
 *  16  n_half x u32 code points (sorted)
 *      n_wide x u32 code points (sorted)
 *      n_half x 12-byte bitmaps
 *      n_wide x 18-byte bitmaps
 */
#define HEADER_BYTES 16

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}

bool font_init(Font *f, const uint8_t *blob, size_t n) {
    if (n < HEADER_BYTES) return false;
    if (blob[0] != 'G' || blob[1] != 'G' || blob[2] != 'F' || blob[3] != 'N') return false;
    if (blob[4] != 1 || blob[5] != FONT_HEIGHT) return false;
    uint32_t nh = rd32(blob + 8), nw = rd32(blob + 12);
    /* every count is bounded by n, so the size sum cannot overflow 64 bits */
    if (nh > n || nw > n) return false;
    uint64_t need = (uint64_t)HEADER_BYTES + 4ull * nh + 4ull * nw +
                    (uint64_t)FONT_HALF_BYTES * nh + (uint64_t)FONT_WIDE_BYTES * nw;
    if (need != (uint64_t)n) return false;
    f->n_half = nh;
    f->n_wide = nw;
    f->half_cp = blob + HEADER_BYTES;
    f->wide_cp = f->half_cp + 4 * (size_t)nh;
    f->half_bm = f->wide_cp + 4 * (size_t)nw;
    f->wide_bm = f->half_bm + (size_t)FONT_HALF_BYTES * nh;
    return true;
}

static int64_t find(const uint8_t *cps, uint32_t n, uint32_t cp) {
    uint32_t lo = 0, hi = n;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        uint32_t v = rd32(cps + 4 * (size_t)mid);
        if (v == cp) return mid;
        if (v < cp) lo = mid + 1;
        else hi = mid;
    }
    return -1;
}

const uint8_t *font_glyph(const Font *f, uint32_t cp, int *width) {
    int64_t i = find(f->half_cp, f->n_half, cp);
    if (i >= 0) {
        *width = FONT_HALF_W;
        return f->half_bm + (size_t)FONT_HALF_BYTES * (size_t)i;
    }
    i = find(f->wide_cp, f->n_wide, cp);
    if (i >= 0) {
        *width = FONT_WIDE_W;
        return f->wide_bm + (size_t)FONT_WIDE_BYTES * (size_t)i;
    }
    return NULL;
}
