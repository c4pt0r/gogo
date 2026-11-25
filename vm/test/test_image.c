#include "test.h"
#include "image.h"
#include "term.h"
#include "opcodes.h"
#include <string.h>

/* Layout: 8 header + 3 func + 0 types + 3 consts + 1 code + 1 const map
 * = 16 words. consts: [hdr(3,K_STRING)] [4] ["main"]; map: bit 0. */
#define IMG_WORDS 16

static void build(uint32_t *w) {
    memset(w, 0, IMG_WORDS * sizeof(uint32_t));
    w[0] = IMAGE_MAGIC; w[1] = IMAGE_VERSION; w[2] = 1; w[3] = 0;
    w[4] = 3; w[5] = 1; w[6] = 0; w[7] = 0;
    w[8] = 0;            /* code_off */
    w[9] = 2 | (5u << 16);
    w[10] = 0;           /* name offset */
    w[11] = hdr_make(3, K_STRING);
    w[12] = 4;
    memcpy(&w[13], "main", 4);
    w[14] = OP_NOP;
    w[15] = 1u;          /* const map: an object starts at const word 0 */
}

TEST(image_ok) {
    uint32_t w[IMG_WORDS]; Image img;
    build(w);
    CHECK_EQ(image_load(&img, w, IMG_WORDS), IMG_OK);
    CHECK_EQ(img.n_funcs, 1);
    CHECK_EQ(img.code_words, 1);
    CHECK(img.code == &w[14]);   /* used in place */
    CHECK(img.cmap == &w[15]);
    CHECK_EQ(image_func_code_off(&img, 0), 0);
    CHECK_EQ(image_func_nparams(&img, 0), 2);
    CHECK_EQ(image_func_nregs(&img, 0), 5);
    uint32_t len = 0;
    const char *n = image_func_name(&img, 0, &len);
    CHECK_EQ(len, 4);
    CHECK(memcmp(n, "main", 4) == 0);
}

TEST(image_type_nfields) {
    /* 8 header + 3 func + 2 type + 3 consts + 1 code + 1 map = 18 words */
    Image img;
    uint32_t v[18];
    memset(v, 0, sizeof v);
    v[0] = IMAGE_MAGIC; v[1] = IMAGE_VERSION; v[2] = 1; v[3] = 1; v[4] = 3; v[5] = 1;
    v[11] = 3; v[12] = 0;                 /* type: 3 fields, name at 0 */
    v[13] = hdr_make(3, K_STRING); v[14] = 4; memcpy(&v[15], "main", 4);
    v[16] = OP_NOP;
    v[17] = 1u;
    CHECK_EQ(image_load(&img, v, 18), IMG_OK);
    CHECK_EQ(image_type_nfields(&img, 0), 3);
}

TEST(image_bad_magic) {
    uint32_t w[IMG_WORDS]; Image img;
    build(w); w[0] ^= 1;
    CHECK_EQ(image_load(&img, w, IMG_WORDS), IMG_ERR_MAGIC);
}

TEST(image_bad_version) {
    uint32_t w[IMG_WORDS]; Image img;
    CHECK_EQ(IMAGE_VERSION, 7u);
    build(w); w[1] = 1;        /* M1/M2 images: opcode semantics changed in M3 */
    CHECK_EQ(image_load(&img, w, IMG_WORDS), IMG_ERR_VERSION);
    build(w); w[1] = 2;        /* M3 images: RETN, CONV c=2 and builtins added in M4 */
    CHECK_EQ(image_load(&img, w, IMG_WORDS), IMG_ERR_VERSION);
    build(w); w[1] = 3;        /* M4 images: CALLV added in M5 */
    CHECK_EQ(image_load(&img, w, IMG_WORDS), IMG_ERR_VERSION);
    build(w); w[1] = 4;        /* M5 images: bitwise ops added in v5 */
    CHECK_EQ(image_load(&img, w, IMG_WORDS), IMG_ERR_VERSION);
    build(w); w[1] = 5;        /* v5 images: B_FS and the fs reserved types added in v6 */
    CHECK_EQ(image_load(&img, w, IMG_WORDS), IMG_ERR_VERSION);
    build(w); w[1] = 6;        /* v6 images: no const map (added in v7) */
    CHECK_EQ(image_load(&img, w, IMG_WORDS), IMG_ERR_VERSION);
    build(w); w[1] = 8;
    CHECK_EQ(image_load(&img, w, IMG_WORDS), IMG_ERR_VERSION);
    build(w);
    CHECK_EQ(image_load(&img, w, IMG_WORDS), IMG_OK);
}

TEST(image_truncated) {
    uint32_t w[IMG_WORDS]; Image img;
    build(w);
    CHECK_EQ(image_load(&img, w, IMG_WORDS - 1), IMG_ERR_TRUNCATED);   /* no const map */
    CHECK_EQ(image_load(&img, w, 3), IMG_ERR_TRUNCATED);
}

TEST(image_bad_entry) {
    uint32_t w[IMG_WORDS]; Image img;
    build(w); w[6] = 1;
    CHECK_EQ(image_load(&img, w, IMG_WORDS), IMG_ERR_BOUNDS);
}

TEST(image_bad_code_off) {
    uint32_t w[IMG_WORDS]; Image img;
    build(w); w[8] = 1;
    CHECK_EQ(image_load(&img, w, IMG_WORDS), IMG_ERR_BOUNDS);
}

TEST(image_bad_name_offset) {
    uint32_t w[IMG_WORDS]; Image img;
    build(w); w[10] = 3;           /* past consts */
    CHECK_EQ(image_load(&img, w, IMG_WORDS), IMG_ERR_BOUNDS);
    build(w); w[10] = 1;           /* points at byte_len, not a K_STRING */
    CHECK_EQ(image_load(&img, w, IMG_WORDS), IMG_ERR_BOUNDS);
}

TEST(image_string_overruns_consts) {
    uint32_t w[IMG_WORDS]; Image img;
    build(w); w[11] = hdr_make(9, K_STRING);   /* extends past consts */
    CHECK_EQ(image_load(&img, w, IMG_WORDS), IMG_ERR_BOUNDS);
    build(w); w[12] = 100;                     /* byte_len too large */
    CHECK_EQ(image_load(&img, w, IMG_WORDS), IMG_ERR_BOUNDS);
    build(w); w[11] = hdr_make(1, K_STRING);   /* no room for byte_len */
    CHECK_EQ(image_load(&img, w, IMG_WORDS), IMG_ERR_BOUNDS);
}

TEST(image_huge_sizes_no_overflow) {
    uint32_t w[IMG_WORDS]; Image img;
    build(w); w[2] = 0xFFFFFFFFu;
    CHECK_EQ(image_load(&img, w, IMG_WORDS), IMG_ERR_TRUNCATED);
    build(w); w[4] = 0xFFFFFFFFu; w[5] = 0xFFFFFFFFu;
    CHECK_EQ(image_load(&img, w, IMG_WORDS), IMG_ERR_TRUNCATED);
}

/* Two functions: 8 header + 6 func + 0 types + 3 consts + 2 code + 1 map.
 * Function code must be laid out in index order (strictly ascending
 * code_off) so the interpreter can binary-search pc -> function. */
#define IMG2_WORDS 20

static void build2(uint32_t *w, uint32_t off0, uint32_t off1) {
    memset(w, 0, IMG2_WORDS * sizeof(uint32_t));
    w[0] = IMAGE_MAGIC; w[1] = IMAGE_VERSION; w[2] = 2; w[3] = 0;
    w[4] = 3; w[5] = 2; w[6] = 0; w[7] = 0;
    w[8] = off0;  w[9] = 1u << 16;  w[10] = 0;
    w[11] = off1; w[12] = 1u << 16; w[13] = 0;
    w[14] = hdr_make(3, K_STRING);
    w[15] = 4;
    memcpy(&w[16], "main", 4);
    w[17] = OP_NOP;
    w[18] = OP_NOP;
    w[19] = 1u;
}

TEST(image_funcs_ascending_ok) {
    uint32_t w[IMG2_WORDS]; Image img;
    build2(w, 0, 1);
    CHECK_EQ(image_load(&img, w, IMG2_WORDS), IMG_OK);
}

TEST(image_funcs_out_of_order) {
    uint32_t w[IMG2_WORDS]; Image img;
    build2(w, 1, 0);
    CHECK_EQ(image_load(&img, w, IMG2_WORDS), IMG_ERR_BOUNDS);
}

TEST(image_funcs_same_offset) {
    uint32_t w[IMG2_WORDS]; Image img;
    build2(w, 1, 1);
    CHECK_EQ(image_load(&img, w, IMG2_WORDS), IMG_ERR_BOUNDS);
}

/* Image with a "main" name string at const 0 followed by `extra` const words. */
/* header 8 + func 3 + name string 3 + extra consts (up to 9) + code 1 +
 * the const map (img_seal: one bit per object start the headers give) */
static uint32_t cimg[8 + 3 + 3 + 9 + 1 + 1];
static int build_consts(Image *img, const uint32_t *extra, uint32_t n) {
    uint32_t *w = cimg;
    memset(w, 0, sizeof cimg);
    w[0] = IMAGE_MAGIC; w[1] = IMAGE_VERSION; w[2] = 1; w[3] = 0;
    w[4] = 3 + n; w[5] = 1; w[6] = 0; w[7] = 0;
    w[9] = 2 | (5u << 16);
    w[11] = hdr_make(3, K_STRING); w[12] = 4; memcpy(&w[13], "main", 4);
    for (uint32_t i = 0; i < n; i++) w[14 + i] = extra[i];
    w[14 + n] = OP_NOP;
    return image_load(img, w, img_seal(w));
}

TEST(const_walk_ok) {
    Image img;
    uint32_t e[] = { hdr_make(3, K_STRUCT), 0, term_from_int(7),
                     hdr_make(2, K_INT), 5,
                     hdr_make(4, K_CLOSURE), 0, term_from_const(0), T_NIL };
    CHECK_EQ(build_consts(&img, e, 9), IMG_OK);
}

TEST(const_struct_with_heap_ref_rejected) {
    Image img;
    uint32_t e[] = { hdr_make(3, K_STRUCT), 0, term_from_ref(0) };
    CHECK_EQ(build_consts(&img, e, 3), IMG_ERR_BOUNDS);
}

/* Off-heap handles only come from the VM: an image cannot forge one. */
TEST(const_struct_with_handle_rejected) {
    Image img;
    uint32_t e[] = { hdr_make(3, K_STRUCT), 0, term_from_handle(0) };
    CHECK_EQ(build_consts(&img, e, 3), IMG_ERR_BOUNDS);
}

TEST(const_closure_with_heap_ref_rejected) {
    Image img;
    uint32_t e[] = { hdr_make(3, K_CLOSURE), 0, term_from_ref(0) };
    CHECK_EQ(build_consts(&img, e, 3), IMG_ERR_BOUNDS);
}

TEST(const_ref_out_of_range_rejected) {
    Image img;
    uint32_t e[] = { hdr_make(3, K_STRUCT), 0, term_from_const(99) };
    CHECK_EQ(build_consts(&img, e, 3), IMG_ERR_BOUNDS);
}

TEST(const_unknown_kind_rejected) {
    Image img;
    uint32_t e[] = { hdr_make(2, 9), 0 };
    CHECK_EQ(build_consts(&img, e, 2), IMG_ERR_BOUNDS);
}

TEST(const_zero_header_rejected) {
    Image img;
    uint32_t e[] = { 0, 0 };
    CHECK_EQ(build_consts(&img, e, 2), IMG_ERR_BOUNDS);
    uint32_t f[] = { hdr_make(0, K_INT), 0 };
    CHECK_EQ(build_consts(&img, f, 2), IMG_ERR_BOUNDS);
}

TEST(const_object_overruns_rejected) {
    Image img;
    uint32_t e[] = { hdr_make(5, K_STRUCT), 0, T_NIL };
    CHECK_EQ(build_consts(&img, e, 3), IMG_ERR_BOUNDS);
}

TEST(const_string_bad_len_rejected) {
    Image img;
    uint32_t e[] = { hdr_make(3, K_STRING), 100, 0 };
    CHECK_EQ(build_consts(&img, e, 3), IMG_ERR_BOUNDS);
}

TEST(const_short_int_or_struct_rejected) {
    Image img;
    uint32_t e[] = { hdr_make(1, K_INT) };
    CHECK_EQ(build_consts(&img, e, 1), IMG_ERR_BOUNDS);
    uint32_t f[] = { hdr_make(1, K_STRUCT) };
    CHECK_EQ(build_consts(&img, f, 1), IMG_ERR_BOUNDS);
}

TEST(const_int64_is_three_words) {
    Image img;
    uint32_t e[] = { hdr_make(3, K_INT64), 0xFFFFFFFFu, 0x7FFFFFFFu };
    CHECK_EQ(build_consts(&img, e, 3), IMG_OK);
    uint32_t f[] = { hdr_make(2, K_INT64), 5 };
    CHECK_EQ(build_consts(&img, f, 2), IMG_ERR_BOUNDS);
    uint32_t g[] = { hdr_make(4, K_INT64), 5, 0, 0 };
    CHECK_EQ(build_consts(&img, g, 4), IMG_ERR_BOUNDS);
}

/* ---- v7: the const map (one bit per const word, set at object starts) ---- */

TEST(const_map_must_match_headers) {
    uint32_t w[IMG_WORDS]; Image img;
    build(w); w[15] = 0;           /* bit 0 clear: the first object unmarked */
    CHECK_EQ(image_load(&img, w, IMG_WORDS), IMG_ERR_BOUNDS);
    build(w); w[15] = 1u | 4u;     /* an interior word marked */
    CHECK_EQ(image_load(&img, w, IMG_WORDS), IMG_ERR_BOUNDS);
}

/* A const area over 32 words takes a second map word, checked as well. */
TEST(const_map_two_words) {
    Image img;
    uint32_t e[9] = { hdr_make(3, K_INT64), 1, 0, hdr_make(3, K_INT64), 2, 0, hdr_make(3, K_INT64), 3, 0 };
    static uint32_t v[8 + 3 + 39 + 1 + 2];
    memset(v, 0, sizeof v);
    v[0] = IMAGE_MAGIC; v[1] = IMAGE_VERSION; v[2] = 1; v[4] = 39; v[5] = 1;
    v[9] = 2 | (5u << 16);
    v[11] = hdr_make(3, K_STRING); v[12] = 4; memcpy(&v[13], "main", 4);
    for (uint32_t k = 0; k < 4; k++) memcpy(&v[14 + 9 * k], e, sizeof e);   /* 12 K_INT64s */
    v[50] = OP_NOP;
    CHECK_EQ(img_seal(v), 53);
    CHECK_EQ(image_load(&img, v, 53), IMG_OK);
    CHECK_EQ(v[52], 2u | 16u);     /* objects at 33 and 36 */
    v[52] |= 4u;                   /* const word 34: interior */
    CHECK_EQ(image_load(&img, v, 53), IMG_ERR_BOUNDS);
    v[52] = 2u;                    /* the object at 36 unmarked */
    CHECK_EQ(image_load(&img, v, 53), IMG_ERR_BOUNDS);
}

/* consts: "main" (0..2), then a string (3..7) whose payload looks like a
 * K_STRUCT at 5 holding a heap ref, then `tail` words. The code is
 * LOADK r0, k; RET r0. */
static uint32_t kimg[8 + 3 + 8 + 3 + 3 + 1];
static int build_loadk(Image *img, term k, const uint32_t *tail, uint32_t n) {
    uint32_t *w = kimg;
    memset(w, 0, sizeof kimg);
    w[0] = IMAGE_MAGIC; w[1] = IMAGE_VERSION; w[2] = 1; w[3] = 0;
    w[4] = 8 + n; w[5] = 3; w[6] = 0; w[7] = 0;
    w[9] = 0 | (1u << 16);
    w[11] = hdr_make(3, K_STRING); w[12] = 4; memcpy(&w[13], "main", 4);
    w[14] = hdr_make(5, K_STRING); w[15] = 12;
    w[16] = hdr_make(3, K_STRUCT); w[17] = 0; w[18] = term_from_ref(0x100000);
    for (uint32_t i = 0; i < n; i++) w[19 + i] = tail[i];
    uint32_t *code = w + 19 + n;
    code[0] = OP_MAKE(OP_LOADK, 0, 0, 0); code[1] = k;
    code[2] = OP_MAKE(OP_RET, 0, 0, 0);
    return image_load(img, w, img_seal(w));
}

/* Function and type names must be object starts too: a name offset at a
 * well-formed fake K_STRING inside another string's payload is refused
 * (the func name becomes a const ref in Reason.Func). */
static uint32_t nimg[8 + 3 + 2 + 9 + 1 + 1];
static int build_names(Image *img, uint32_t fname, uint32_t tname) {
    uint32_t *w = nimg;
    memset(w, 0, sizeof nimg);
    w[0] = IMAGE_MAGIC; w[1] = IMAGE_VERSION; w[2] = 1; w[3] = 1;
    w[4] = 9; w[5] = 1;
    w[9] = 0 | (1u << 16); w[10] = fname;
    w[11] = 2; w[12] = tname;
    w[13] = hdr_make(3, K_STRING); w[14] = 4; memcpy(&w[15], "main", 4);
    w[16] = hdr_make(6, K_STRING); w[17] = 16;           /* payload: a fake "main" at 5 */
    w[18] = hdr_make(3, K_STRING); w[19] = 4; memcpy(&w[20], "main", 4);
    w[22] = OP_NOP;
    return image_load(img, w, img_seal(w));
}

TEST(names_must_be_object_starts) {
    Image img;
    CHECK_EQ(build_names(&img, 0, 3), IMG_OK);
    CHECK_EQ(build_names(&img, 5, 0), IMG_ERR_BOUNDS);
    CHECK_EQ(build_names(&img, 0, 5), IMG_ERR_BOUNDS);
}

/* VM-1 (round 2): a real const struct whose field is a const ref into
 * the middle of an object is refused at load (const refs inside const
 * objects are checked at load; LOADK operands when executed). */
TEST(const_struct_interior_ref_rejected) {
    Image img;
    uint32_t ok[3] = { hdr_make(3, K_STRUCT), 0, term_from_const(3) };
    CHECK_EQ(build_loadk(&img, term_from_const(8), ok, 3), IMG_OK);
    uint32_t bad[3] = { hdr_make(3, K_STRUCT), 0, term_from_const(5) };
    CHECK_EQ(build_loadk(&img, term_from_const(8), bad, 3), IMG_ERR_BOUNDS);
    uint32_t cl[3] = { hdr_make(3, K_CLOSURE), 0, term_from_const(6) };
    CHECK_EQ(build_loadk(&img, term_from_const(8), cl, 3), IMG_ERR_BOUNDS);
    uint32_t fwd[3] = { hdr_make(3, K_STRUCT), 0, term_from_const(8) };     /* itself: a start */
    CHECK_EQ(build_loadk(&img, term_from_const(8), fwd, 3), IMG_OK);
}
