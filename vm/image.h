/* Image loader. The image is used in place and never copied.
 *
 * Versioning rule: any change to opcode numbers or semantics, builtin
 * numbers, reserved types or the term encoding bumps IMAGE_VERSION (here
 * and imageVersion in compiler/image.go); the compiler and the VM move in
 * lockstep and the loader rejects every other version (IMG_ERR_VERSION).
 *   v1  M1/M2.
 *   v2  M3: generic arithmetic is int-only, *64 ops and BOX64, OP_CLEAR,
 *       typed zero PID/Ref (T_ZEROPID, T_ZEROREF), port builtins/types.
 *   v3  M4: OP_RETN (multi-result return), OP_CONV c=2 (byte).
 *   v4  M5: OP_CALLV (calling a func value).
 *   v5  bitwise ops AND..SHR64 (40-51), R_BAD_SHIFT.
 *   v6  storage: B_FS (49), reserved types fs.Info (27), fs.Space (28).
 *   v7  const map after the code: one bit per const word, set where an
 *       object starts; every const ref must name a marked start (const
 *       object fields and func/type names checked at load, LOADK operands
 *       when executed).
 *
 * Layout (little-endian words): header (8: magic, version, n_funcs,
 * n_types, consts_words, code_words, entry, 0), funcs (3 words each),
 * types (2 each), consts, code, const map (ceil(consts_words / 32) words,
 * bit i of word i / 32 for const word i; used in place, no RAM). */
#ifndef GOGO_IMAGE_H
#define GOGO_IMAGE_H

#include <stdint.h>
#include <stddef.h>

#define IMAGE_MAGIC   0x4F474F47u
#define IMAGE_VERSION 7u
#define IMAGE_HEADER_WORDS 8u

typedef struct Image {
    const uint32_t *funcs, *types, *consts, *code, *cmap;
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
 * offsets must be strictly ascending in function-index order. The const
 * map must mark exactly the object starts the const headers give, and
 * every const ref in a const object, and every func and type name, must
 * name one (IMG_ERR_BOUNDS). LOADK operands are not checked here: the
 * interpreter checks each when it executes (a linear decode at load could
 * not be complete, since a jump may land on an ext word). */
int image_load(Image *img, const uint32_t *words, uint32_t n_words);

/* True if an object of the loaded image's const area starts at const word
 * off. A ref into the middle of an object could forge one (a fake header
 * inside a string's payload), so every const ref is checked with this. */
int image_const_start(const Image *img, uint32_t off);

uint32_t image_func_code_off(const Image *img, uint32_t f);
uint16_t image_func_nparams(const Image *img, uint32_t f);
uint16_t image_func_nregs(const Image *img, uint32_t f);
/* Name bytes (not NUL-terminated) and length. */
const char *image_func_name(const Image *img, uint32_t f, uint32_t *len);
/* Const-area word offset of the function's name (a K_STRING). */
uint32_t image_func_name_off(const Image *img, uint32_t f);
uint32_t image_type_nfields(const Image *img, uint32_t t);

#endif
