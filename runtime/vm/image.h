/* Image loader. The image is used in place and never copied.
 *
 * Versioning (docs/design.md, Image format): header word 1 is
 * major << 16 | minor. The loader accepts an image of the same major and a
 * minor up to its own (IMAGE_MINOR); anything else is IMG_ERR_VERSION.
 *   Minor (additive, old images keep running): a new builtin number,
 *   opcode, reserved type, or an int field appended to a reserved type
 *   (0 means its default). The compiler stamps the lowest minor the
 *   program needs (compiler/image.go), so it runs on every VM that has
 *   what it uses.
 *   Major: a change to the term encoding, to opcode or builtin semantics,
 *   or a reserved field reordered or removed. Bump IMAGE_MAJOR here and
 *   imageMajor in compiler/image.go together, and reset both minors to 0.
 * History before v15 (one number, every change a break): v1 M1/M2; v2
 * int-only arithmetic, *64 ops; v3 RETN; v4 CALLV; v5 bitwise ops; v6
 * B_FS and the fs types; v7 the const map; v8 kbd and power types; v9
 * hot loading (requires and exports trailer, B_CODE); v10 streams, imu,
 * ir; v11 audio; v12 gogo/sys builtins; v13 net.DialTLS; v14 port.Host,
 * port.Post.
 *   v15.0 own types from TYPE_OWN_BASE (128) with the reserved count in
 *       header word 7; major.minor versions; append-only reserved fields;
 *       named requirements instead of the capability bits; one stream
 *       protocol (port.Chunk, Out, Done, Closed, with From on the events
 *       and on port.Post) for every byte port, gogo/gpio's types, the
 *       reserved table renumbered (44 types); the term tags the later
 *       language phases need reserved (below).
 *   v15.1 reserved type 44, screen.Config (the "screen" port).
 *
 * Reserved encodings (term.h): heap kinds 12-15, the specials from 0x72
 * up, the TYPEIS ids 0x10007-0x100FF and bits 16-20 of a struct's type
 * word are kept free for interface values, pointers and maps. Nothing in
 * a v15 compiler or VM produces them (image_load refuses an unknown heap
 * kind; it does not check the others). Giving one a meaning is a minor:
 * an image that uses it is stamped with that minor, and the version
 * check makes older VMs refuse it.
 *
 * Types: an image's type table holds n_reserved reserved rows (header
 * word 7; at most TYPE_RESERVED_COUNT, else IMG_ERR_VERSION: the image
 * uses a reserved type this VM does not know), then its own types. Code
 * names a reserved type by its fixed index (TYPE_* in opcodes.h) and an
 * own type by TYPE_OWN_BASE + its position after the reserved rows; row
 * image_type_row(). The compiler emits only the reserved rows up to the
 * highest one the program uses. Reserved types only grow, at the end: a
 * row may declare at most the fields this VM knows for it (TYPE_NFIELDS,
 * else IMG_ERR_VERSION), appended fields are ints with 0 as the default,
 * and GETFIELD past the end of an older image's reserved struct reads 0.
 *
 * Layout (little-endian words): header (8: magic, version, n_funcs,
 * n_types, consts_words, code_words, entry, n_reserved), funcs (3 words
 * each), types (2 each: nfields, name), consts, code, const map
 * (ceil(consts_words / 32) words, bit i of word i / 32 for const word i;
 * used in place, no RAM), n_requires (1 word), n_requires const offsets,
 * n_exports (1 word), then n_exports pairs {name_const_off, func_index}.
 * Each requirement and export name is a const string at a marked object
 * start; func_index < n_funcs.
 *
 * Requirements: what the image needs from the board, by name: a port
 * driver ("kbd", "tcp", a host port's name) or a mount prefix ("/sd",
 * starting with '/'). The loader refuses an image whose requirement the
 * VM lacks (module.h), naming the first one missing. */
#ifndef GOGO_IMAGE_H
#define GOGO_IMAGE_H

#include <stdint.h>
#include <stddef.h>
#include "opcodes.h"

#define IMAGE_MAGIC   0x4F474F47u
#define IMAGE_MAJOR   15u
#define IMAGE_MINOR   1u
#define IMAGE_VERSION (IMAGE_MAJOR << 16 | IMAGE_MINOR)   /* the newest this VM runs */
#define IMAGE_HEADER_WORDS 8u

typedef struct Image {
    const uint32_t *funcs, *types, *consts, *code, *cmap, *exports;
    uint32_t n_funcs, n_types, consts_words, code_words;
    uint32_t n_reserved, n_exports; /* header word 6 (entry) is only validated: boot starts the "main" export */
} Image;

enum {
    IMG_OK = 0,
    IMG_ERR_MAGIC = -1,
    IMG_ERR_VERSION = -2,
    IMG_ERR_TRUNCATED = -3,
    IMG_ERR_BOUNDS = -4
};

/* Validates `words` and fills `img` with pointers into it. The const area,
 * the function count and the type count must fit the index ranges a module
 * tag leaves (CONST_MAX_WORDS, FUNC_MAX, TYPE_MAX, term.h), and image const
 * refs (in const objects) and const struct/closure type words must be
 * untagged (IMG_ERR_BOUNDS). Function code
 * offsets must be strictly ascending in function-index order. The const
 * map must mark exactly the object starts the const headers give, and
 * every const ref in a const object, and every func and type name, must
 * name one (IMG_ERR_BOUNDS). LOADK operands are not checked here: the
 * interpreter checks each when it executes (a linear decode at load could
 * not be complete, since a jump may land on an ext word). */
int image_load(Image *img, const uint32_t *words, uint32_t n_words);

/* True if an object of the loaded image's const area starts at const word
 * off. A ref into the middle of an object could forge one (a fake header
 * inside a string's payload), so every const ref is checked with this
 * (obj_of, LOADK, the loader). */
static inline int image_const_start(const Image *img, uint32_t off) {
    return off < img->consts_words && ((img->cmap[off >> 5] >> (off & 31u)) & 1u);
}

/* "bad magic", "unsupported version", "truncated image", "bad offsets" for
 * an IMG_ERR_* (boot messages), "invalid image" for anything else. */
const char *image_err_str(int rc);

/* Looks up an exported entry by name (exact bytes); returns 1 and sets
 * *func to its function index when found, else 0. Call it only on an image
 * that image_load accepted: it reads the export name strings through
 * consts[off+1] unchecked, relying on image_load's string_ok validation of
 * every export name offset. */
int image_export_find(const Image *img, const uint8_t *name, uint32_t len, uint32_t *func);

/* The image's requirement list: n_requires, then that many const offsets
 * of the names (validated by image_load). */
static inline const uint32_t *image_requires(const Image *img) {
    return img->cmap + (img->consts_words + 31u) / 32u;
}

uint32_t image_func_code_off(const Image *img, uint32_t f);
uint16_t image_func_nparams(const Image *img, uint32_t f);
uint16_t image_func_nregs(const Image *img, uint32_t f);
/* Name bytes (not NUL-terminated) and length. */
const char *image_func_name(const Image *img, uint32_t f, uint32_t *len);
/* Const-area word offset of the function's name (a K_STRING). */
uint32_t image_func_name_off(const Image *img, uint32_t f);
/* Field count of type-table row r (r < n_types). */
uint32_t image_type_nfields(const Image *img, uint32_t r);

/* The type-table row of type t (a reserved TYPE_* index, or TYPE_OWN_BASE
 * plus the position of an own type), or img->n_types if the image has no
 * such row. */
static inline uint32_t image_type_row(const Image *img, uint32_t t) {
    if (t < TYPE_OWN_BASE) return t < img->n_reserved ? t : img->n_types;
    t -= TYPE_OWN_BASE;
    return t < img->n_types - img->n_reserved ? t + img->n_reserved : img->n_types;
}

#endif
