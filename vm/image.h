/* Image v1 loader. The image is used in place and never copied. */
#ifndef GOGO_IMAGE_H
#define GOGO_IMAGE_H

#include <stdint.h>
#include <stddef.h>

#define IMAGE_MAGIC   0x4F474F47u
#define IMAGE_VERSION 1u
#define IMAGE_HEADER_WORDS 8u

typedef struct Image {
    const uint32_t *funcs, *types, *consts, *code;
    uint32_t n_funcs, n_types, consts_words, code_words, entry;
} Image;

enum {
    IMG_OK = 0,
    IMG_ERR_MAGIC = -1,
    IMG_ERR_VERSION = -2,
    IMG_ERR_TRUNCATED = -3,
    IMG_ERR_BOUNDS = -4
};

/* Validates `words` and fills `img` with pointers into it. Function code
 * offsets must be strictly ascending in function-index order. */
int image_load(Image *img, const uint32_t *words, uint32_t n_words);

uint32_t image_func_code_off(const Image *img, uint32_t f);
uint16_t image_func_nparams(const Image *img, uint32_t f);
uint16_t image_func_nregs(const Image *img, uint32_t f);
/* Name bytes (not NUL-terminated) and length. */
const char *image_func_name(const Image *img, uint32_t f, uint32_t *len);
uint32_t image_type_nfields(const Image *img, uint32_t t);

#endif
