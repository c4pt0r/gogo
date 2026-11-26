#include "image.h"
#include "term.h"
#include "heap.h"
#include <string.h>

const char *image_err_str(int rc) {
    static const char *const msgs[] = {"ok", "bad magic", "unsupported version", "truncated image", "bad offsets"};
    return (rc <= 0 && rc >= -4) ? msgs[-rc] : "invalid image";
}

/* True if consts[off] starts a well-formed K_STRING lying wholly in consts,
 * at an object start in the const map (func and type names too: a func
 * name becomes a const ref in Reason.Func; the map itself is proven by
 * consts_ok, which image_load runs after the names). */
static int string_ok(const Image *img, uint32_t off) {
    if (!image_const_start(img, off) || img->consts_words - off < 2) return 0;
    uint32_t h = img->consts[off];
    uint32_t words = hdr_words(h);
    if (hdr_kind(h) != K_STRING || words < 2) return 0;
    if (words > img->consts_words - off) return 0;
    /* byte_len must fit in the payload words (compare in 64 bits). */
    return (uint64_t)img->consts[off + 1] <= (uint64_t)(words - 2) * 4u;
}

/* Walks the const area (objects contiguous from offset 0) checking every
 * object's header, extent and term-bearing words, and that the const map
 * marks its first word and none of the others. A const ref checked before
 * the walk reaches its target is still sound: a wrong mark there fails the
 * walk later. */
static int consts_ok(const Image *img) {
    uint32_t off = 0;
    while (off < img->consts_words) {
        const uint32_t *obj = img->consts + off;
        uint32_t h = obj[0];
        uint32_t words = hdr_words(h);
        uint32_t kind = hdr_kind(h);
        if (words == 0 || words > img->consts_words - off) return 0;
        for (uint32_t i = 0; i < words; i++) {
            if (image_const_start(img, off + i) != (i == 0)) return 0;
        }
        switch (kind) {
        case K_STRING:
            if (!string_ok(img, off)) return 0;
            break;
        case K_INT:
            if (words != 2) return 0;
            break;
        case K_INT64:
            if (words != 3) return 0;
            break;
        case K_STRUCT:
        case K_CLOSURE: {
            /* Untagged like every image word: the VM tags them when read. */
            if (words < 2 || (obj[1] & MOD_TAG_MASK)) return 0;
            uint32_t first, end;
            heap_obj_terms(obj, &first, &end);
            for (uint32_t i = first; i < end; i++) {
                term t = obj[i];
                if (term_is_ref(t) || term_is_handle(t)) return 0;   /* handles only come from the VM */
                if (term_is_const(t) && ((t & MOD_TAG_MASK) || !image_const_start(img, term_const_off(t))))
                    return 0;
            }
            break;
        }
        default:
            return 0;
        }
        off += words;
    }
    return 1;
}

int image_load(Image *img, const uint32_t *w, uint32_t n_words) {
    if (n_words < IMAGE_HEADER_WORDS) return IMG_ERR_TRUNCATED;
    if (w[0] != IMAGE_MAGIC) return IMG_ERR_MAGIC;
    /* Same major, minor up to ours: one compare (a lower major wraps). */
    if (w[1] - (IMAGE_MAJOR << 16) > IMAGE_MINOR || w[7] > TYPE_RESERVED_COUNT) return IMG_ERR_VERSION;

    uint64_t total = (uint64_t)IMAGE_HEADER_WORDS + (uint64_t)w[2] * 3u +
                     (uint64_t)w[3] * 2u + (uint64_t)w[4] + (uint64_t)w[5] +
                     ((uint64_t)w[4] + 31u) / 32u;
    if (total + 2u > n_words) return IMG_ERR_TRUNCATED;   /* + requires, n_exports */
    uint32_t n_exp = w[total + 1u];
    if (total + 2u + (uint64_t)n_exp * 2u > n_words) return IMG_ERR_TRUNCATED;
    /* The index ranges a module tag leaves (term.h). */
    if (w[2] > FUNC_MAX || w[3] < w[7] || w[3] - w[7] > TYPE_MAX - TYPE_OWN_BASE || w[4] > CONST_MAX_WORDS)
        return IMG_ERR_BOUNDS;

    img->n_funcs = w[2];
    img->n_types = w[3];
    img->consts_words = w[4];
    img->code_words = w[5];
    img->funcs = w + IMAGE_HEADER_WORDS;
    img->types = img->funcs + (size_t)img->n_funcs * 3u;
    img->consts = img->types + (size_t)img->n_types * 2u;
    img->code = img->consts + img->consts_words;
    img->cmap = img->code + img->code_words;
    img->n_reserved = w[7];
    img->n_exports = n_exp;
    img->exports = w + total + 2u;

    if (w[6] >= img->n_funcs) return IMG_ERR_BOUNDS;
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
    if (!consts_ok(img)) return IMG_ERR_BOUNDS;
    for (uint32_t i = 0; i < img->n_exports; i++) {
        if (!string_ok(img, img->exports[(size_t)i * 2u]) ||
            img->exports[(size_t)i * 2u + 1u] >= img->n_funcs) return IMG_ERR_BOUNDS;
    }
    return IMG_OK;
}

int image_export_find(const Image *img, const uint8_t *name, uint32_t len, uint32_t *func) {
    for (uint32_t i = 0; i < img->n_exports; i++) {
        uint32_t off = img->exports[(size_t)i * 2u];
        if (img->consts[off + 1u] == len &&
            memcmp(&img->consts[off + 2u], name, len) == 0) {
            *func = img->exports[(size_t)i * 2u + 1u];
            return 1;
        }
    }
    return 0;
}

uint32_t image_func_code_off(const Image *img, uint32_t f) { return img->funcs[f * 3u]; }
uint16_t image_func_nparams(const Image *img, uint32_t f) { return (uint16_t)(img->funcs[f * 3u + 1u] & 0xFFFFu); }
uint16_t image_func_nregs(const Image *img, uint32_t f) { return (uint16_t)(img->funcs[f * 3u + 1u] >> 16); }

const char *image_func_name(const Image *img, uint32_t f, uint32_t *len) {
    uint32_t off = img->funcs[f * 3u + 2u];
    *len = img->consts[off + 1u];
    return (const char *)&img->consts[off + 2u];
}

uint32_t image_func_name_off(const Image *img, uint32_t f) { return img->funcs[f * 3u + 2u]; }

uint32_t image_type_nfields(const Image *img, uint32_t r) { return img->types[r * 2u]; }
