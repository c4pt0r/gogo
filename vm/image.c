#include "image.h"
#include "term.h"

/* True if consts[off] starts a well-formed K_STRING lying wholly in consts. */
static int string_ok(const Image *img, uint32_t off) {
    if (off >= img->consts_words || img->consts_words - off < 2) return 0;
    uint32_t h = img->consts[off];
    uint32_t words = hdr_words(h);
    if (hdr_kind(h) != K_STRING || words < 2) return 0;
    if (words > img->consts_words - off) return 0;
    /* byte_len must fit in the payload words (compare in 64 bits). */
    return (uint64_t)img->consts[off + 1] <= (uint64_t)(words - 2) * 4u;
}

int image_load(Image *img, const uint32_t *w, uint32_t n_words) {
    if (n_words < IMAGE_HEADER_WORDS) return IMG_ERR_TRUNCATED;
    if (w[0] != IMAGE_MAGIC) return IMG_ERR_MAGIC;
    if (w[1] != IMAGE_VERSION) return IMG_ERR_VERSION;

    uint64_t total = (uint64_t)IMAGE_HEADER_WORDS + (uint64_t)w[2] * 3u +
                     (uint64_t)w[3] * 2u + (uint64_t)w[4] + (uint64_t)w[5];
    if (total > n_words) return IMG_ERR_TRUNCATED;

    img->n_funcs = w[2];
    img->n_types = w[3];
    img->consts_words = w[4];
    img->code_words = w[5];
    img->entry = w[6];
    img->funcs = w + IMAGE_HEADER_WORDS;
    img->types = img->funcs + (size_t)img->n_funcs * 3u;
    img->consts = img->types + (size_t)img->n_types * 2u;
    img->code = img->consts + img->consts_words;

    if (img->entry >= img->n_funcs) return IMG_ERR_BOUNDS;
    for (uint32_t i = 0; i < img->n_funcs; i++) {
        const uint32_t *f = img->funcs + (size_t)i * 3u;
        if (f[0] >= img->code_words) return IMG_ERR_BOUNDS;
        /* Code is laid out in function-index order (strictly ascending
         * code_off), so the interpreter can binary-search pc -> function. */
        if (i > 0 && f[0] <= f[-3]) return IMG_ERR_BOUNDS;
        if (!string_ok(img, f[2])) return IMG_ERR_BOUNDS;
    }
    for (uint32_t i = 0; i < img->n_types; i++) {
        if (!string_ok(img, img->types[(size_t)i * 2u + 1u])) return IMG_ERR_BOUNDS;
    }
    return IMG_OK;
}

uint32_t image_func_code_off(const Image *img, uint32_t f) { return img->funcs[f * 3u]; }
uint16_t image_func_nparams(const Image *img, uint32_t f) { return (uint16_t)(img->funcs[f * 3u + 1u] & 0xFFFFu); }
uint16_t image_func_nregs(const Image *img, uint32_t f) { return (uint16_t)(img->funcs[f * 3u + 1u] >> 16); }

const char *image_func_name(const Image *img, uint32_t f, uint32_t *len) {
    uint32_t off = img->funcs[f * 3u + 2u];
    *len = img->consts[off + 1u];
    return (const char *)&img->consts[off + 2u];
}

uint32_t image_type_nfields(const Image *img, uint32_t t) { return img->types[t * 2u]; }
