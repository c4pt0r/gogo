/* Slice builtins (strs.c), GC and message copy of slices. The builtins are
 * called directly on an actor with a 16-register frame. */
#include "test.h"
#include "actor.h"
#include "heap.h"
#include "msg.h"
#include "alloc.h"
#include "image.h"
#include "strs.h"
#include "opcodes.h"
#include "term.h"
#include <string.h>

#define IMG_WORDS 20

/* An image whose const area holds the string "main" (at const offset 0). */

/* Words of the message msg_build makes for t (UINT32_MAX if none): the
 * test-side size check (the VM has no separate size-only entry). */
static uint32_t copy_words(Vm *vm, Actor *a, term t) {
    Msg *m = msg_build(vm, a, t, a->mod, NULL);
    uint32_t w = m ? m->words : UINT32_MAX;
    if (m) msg_free(m);
    return w;
}

static void build_img(uint32_t *w) {
    memset(w, 0, IMG_WORDS * sizeof(uint32_t));
    w[0] = IMAGE_MAGIC; w[1] = IMAGE_VERSION; w[2] = 1; w[3] = 0;
    w[4] = 3; w[5] = 3; w[6] = 0; w[7] = 0;
    w[9] = 0 | (2u << 16);
    w[11] = hdr_make(3, K_STRING);
    w[12] = 4;
    memcpy(&w[13], "main", 4);
    w[14] = OP_MAKE(OP_BUILTIN, 1, 0, 0);
    w[15] = B_RECEIVE;
    w[16] = OP_MAKE(OP_RET, 1, 0, 0);
}

typedef struct Fx { uint32_t w[IMG_WORDS]; Image img; Vm vm; Actor *a; } Fx;

static void fx_init(Fx *f, uint16_t words) {
    va_reset();
    build_img(f->w);
    CHECK_EQ(image_load(&f->img, f->w, img_seal(f->w)), IMG_OK);
    CHECK_EQ(vm_init(&f->vm, &f->img, 4), 0);
    f->a = actor_new(&f->vm, words);
    CHECK(f->a != NULL);
    CHECK(heap_push_frame(&f->vm, f->a, 16, -1));
}

#define REGS(f) ((term *)(f)->a->mem + (f)->a->fp)
#define I(v) term_from_int(v)

static uint32_t call(Fx *f, uint32_t id, uint32_t ra, uint32_t rb, uint32_t rc) {
    return strs_builtin(&f->vm, f->a, id, ra, rb, rc);
}

/* r[ra] = make(kind, len, cap) filled with zero, via r8..r11. */
static uint32_t make(Fx *f, uint32_t ra, int kind, int len, int cap, term zero) {
    REGS(f)[8] = I(kind); REGS(f)[9] = I(len); REGS(f)[10] = I(cap); REGS(f)[11] = zero;
    return call(f, B_MAKE_SLICE, ra, 8, 4);
}

/* r[ra] = r[rs][i] via r8, r9. */
static term get(Fx *f, uint32_t rs, int i) {
    REGS(f)[8] = REGS(f)[rs]; REGS(f)[9] = I(i);
    CHECK_EQ(call(f, B_INDEX, 7, 8, 2), R_NORMAL);
    return REGS(f)[7];
}

static uint32_t set(Fx *f, uint32_t rs, int i, term v) {
    REGS(f)[8] = REGS(f)[rs]; REGS(f)[9] = I(i); REGS(f)[10] = v;
    return call(f, B_SETINDEX, 7, 8, 3);
}

static int32_t len_of(Fx *f, uint32_t rs, uint32_t id) {
    REGS(f)[8] = REGS(f)[rs];
    CHECK_EQ(call(f, id, 7, 8, 1), R_NORMAL);
    return term_to_int(REGS(f)[7]);
}

/* r[ra] = append(r[rs], v) in the given mode, zero 0. */
static uint32_t append(Fx *f, uint32_t ra, uint32_t rs, term v, int mode) {
    REGS(f)[8] = REGS(f)[rs]; REGS(f)[9] = I(mode); REGS(f)[10] = I(0); REGS(f)[11] = v;
    return call(f, B_APPEND, ra, 8, 4);
}

static uint32_t slice(Fx *f, uint32_t ra, uint32_t rs, int lo, int hi) {
    REGS(f)[8] = REGS(f)[rs]; REGS(f)[9] = I(lo); REGS(f)[10] = I(hi);
    return call(f, B_SLICE, ra, 8, 3);
}

/* The array term of a K_SLICE in register r. */
static term arr_of(Fx *f, uint32_t r) {
    return f->a->mem[term_ref_off(REGS(f)[r]) + 4];
}

static void set_str(Fx *f, int r, const char *s) {
    uint32_t len = (uint32_t)strlen(s), words = 2u + (len + 3u) / 4u;
    uint32_t off = heap_alloc(&f->vm, f->a, words, NULL, 0);
    CHECK(off != UINT32_MAX);
    f->a->mem[off] = hdr_make(words, K_STRING);
    f->a->mem[off + 1] = len;
    f->a->mem[off + words - 1] = 0;
    memcpy(f->a->mem + off + 2, s, len);
    REGS(f)[r] = term_from_ref(off);
}

static bool str_is(Fx *f, int r, const char *s) {
    term k = REGS(f)[r];
    if (k == T_EMPTYSTR) return s[0] == 0;
    const uint32_t *o = obj_of(&f->vm, f->a, k);
    if (!o || hdr_kind(o[0]) != K_STRING) return false;
    return o[1] == strlen(s) && memcmp(o + 2, s, o[1]) == 0;
}

TEST(slices_make_index_set) {
    Fx f; fx_init(&f, 128);
    CHECK_EQ(make(&f, 0, 0, 3, 5, I(7)), R_NORMAL);
    CHECK_EQ(len_of(&f, 0, B_LEN), 3);
    CHECK_EQ(len_of(&f, 0, B_CAP), 5);
    CHECK_EQ(get(&f, 0, 2), I(7));
    CHECK_EQ(set(&f, 0, 1, T_TRUE), R_NORMAL);
    CHECK_EQ(REGS(&f)[7], T_NIL);
    CHECK_EQ(get(&f, 0, 1), T_TRUE);
    REGS(&f)[8] = REGS(&f)[0]; REGS(&f)[9] = I(3);       /* index == len */
    CHECK_EQ(call(&f, B_INDEX, 7, 8, 2), R_BAD_CODE);
    REGS(&f)[9] = I(-1);
    CHECK_EQ(call(&f, B_INDEX, 7, 8, 2), R_BAD_CODE);
    CHECK_EQ(set(&f, 0, 3, I(1)), R_BAD_CODE);
    CHECK_EQ(set(&f, 0, -1, I(1)), R_BAD_CODE);
    CHECK_EQ(make(&f, 1, 0, 4, 3, I(0)), R_BAD_CODE);     /* len > cap */
    CHECK_EQ(make(&f, 1, 0, -1, 3, I(0)), R_BAD_CODE);
    CHECK_EQ(make(&f, 1, 2, 1, 1, I(0)), R_BAD_CODE);     /* bad kind */
    CHECK_EQ(make(&f, 1, 0, 0, 0, I(0)), R_NORMAL);       /* empty, not nil */
    CHECK(REGS(&f)[1] != T_NILSLICE);
    CHECK_EQ(len_of(&f, 1, B_LEN), 0);
    CHECK_EQ(arr_of(&f, 1), T_NIL);
    REGS(&f)[2] = T_NILSLICE;
    CHECK_EQ(len_of(&f, 2, B_LEN), 0);
    CHECK_EQ(len_of(&f, 2, B_CAP), 0);
    CHECK_EQ(set(&f, 2, 0, I(1)), R_BAD_CODE);
    REGS(&f)[3] = I(5);                                    /* not a slice */
    REGS(&f)[8] = I(5);
    CHECK_EQ(call(&f, B_CAP, 7, 8, 1), R_BAD_CODE);
    CHECK_EQ(make(&f, 1, 0, 1, 100000, I(0)), R_OUT_OF_MEMORY);
    actor_free(&f.vm, f.a);
}

TEST(slices_bytes) {
    Fx f; fx_init(&f, 128);
    CHECK_EQ(make(&f, 0, 1, 2, 6, I(0)), R_NORMAL);
    CHECK_EQ(arr_of(&f, 0) != T_NIL, 1);
    CHECK_EQ(hdr_kind(f.a->mem[term_ref_off(arr_of(&f, 0))]), K_BYTEARR);
    CHECK_EQ(set(&f, 0, 0, I(300)), R_NORMAL);           /* stores 300 & 0xFF */
    CHECK_EQ(get(&f, 0, 0), I(44));
    CHECK_EQ(set(&f, 0, 1, I(-1)), R_NORMAL);
    CHECK_EQ(get(&f, 0, 1), I(255));
    CHECK_EQ(set(&f, 0, 1, T_TRUE), R_BAD_CODE);         /* not an int */
    /* []byte("GO") -> string */
    set_str(&f, 1, "GO");
    REGS(&f)[8] = REGS(&f)[1];
    CHECK_EQ(call(&f, B_BYTESLICE_FROM, 2, 8, 1), R_NORMAL);
    CHECK_EQ(len_of(&f, 2, B_LEN), 2);
    CHECK_EQ(len_of(&f, 2, B_CAP), 2);
    CHECK_EQ(get(&f, 2, 0), I('G'));
    CHECK_EQ(set(&f, 2, 1, I('O' + 256)), R_NORMAL);
    REGS(&f)[8] = REGS(&f)[2];
    CHECK_EQ(call(&f, B_STR_FROM, 3, 8, 1), R_NORMAL);
    CHECK(str_is(&f, 3, "GO"));
    CHECK_EQ(set(&f, 2, 0, I('N')), R_NORMAL);           /* string(b) copied */
    CHECK(str_is(&f, 3, "GO"));
    REGS(&f)[8] = T_EMPTYSTR;                              /* []byte("") is not nil */
    CHECK_EQ(call(&f, B_BYTESLICE_FROM, 4, 8, 1), R_NORMAL);
    CHECK(REGS(&f)[4] != T_NILSLICE);
    CHECK_EQ(len_of(&f, 4, B_LEN), 0);
    REGS(&f)[8] = T_NILSLICE;                              /* string(nil) == "" */
    CHECK_EQ(call(&f, B_STR_FROM, 5, 8, 1), R_NORMAL);
    CHECK_EQ(REGS(&f)[5], T_EMPTYSTR);
    REGS(&f)[8] = term_from_const(0);                      /* a const string */
    CHECK_EQ(call(&f, B_BYTESLICE_FROM, 4, 8, 1), R_NORMAL);
    CHECK_EQ(get(&f, 4, 3), I('n'));
    CHECK_EQ(make(&f, 6, 0, 1, 1, I(0)), R_NORMAL);       /* string(term slice): bad */
    REGS(&f)[8] = REGS(&f)[6];
    CHECK_EQ(call(&f, B_STR_FROM, 5, 8, 1), R_BAD_CODE);
    actor_free(&f.vm, f.a);
}

TEST(slices_append_alias_and_growth) {
    Fx f; fx_init(&f, 256);
    REGS(&f)[0] = T_NILSLICE;
    CHECK_EQ(append(&f, 0, 0, I(1), 0), R_NORMAL);       /* append to nil */
    CHECK_EQ(len_of(&f, 0, B_LEN), 1);
    CHECK_EQ(len_of(&f, 0, B_CAP), 4);                    /* min 4 */
    CHECK_EQ(append(&f, 1, 0, I(2), 0), R_NORMAL);       /* within cap: aliases */
    CHECK_EQ(append(&f, 2, 0, I(3), 0), R_NORMAL);       /* also writes index 1 */
    CHECK_EQ(arr_of(&f, 1), arr_of(&f, 0));
    CHECK_EQ(get(&f, 1, 1), I(3));                        /* Go aliasing */
    CHECK_EQ(append(&f, 3, 2, I(4), 0), R_NORMAL);
    CHECK_EQ(append(&f, 3, 3, I(5), 0), R_NORMAL);
    CHECK_EQ(len_of(&f, 3, B_LEN), 4);
    CHECK_EQ(append(&f, 4, 3, I(6), 0), R_NORMAL);       /* beyond cap: detaches */
    CHECK_EQ(len_of(&f, 4, B_CAP), 8);                    /* x2 */
    CHECK(arr_of(&f, 4) != arr_of(&f, 3));
    CHECK_EQ(set(&f, 4, 0, I(100)), R_NORMAL);
    CHECK_EQ(get(&f, 3, 0), I(1));
    CHECK_EQ(get(&f, 4, 0), I(100));
    CHECK_EQ(get(&f, 4, 4), I(6));
    /* spread: append(s4, s3...) */
    REGS(&f)[8] = REGS(&f)[4]; REGS(&f)[9] = I(2); REGS(&f)[10] = I(0); REGS(&f)[11] = REGS(&f)[3];
    CHECK_EQ(call(&f, B_APPEND, 5, 8, 4), R_NORMAL);
    CHECK_EQ(len_of(&f, 5, B_LEN), 9);
    CHECK_EQ(len_of(&f, 5, B_CAP), 16);
    CHECK_EQ(get(&f, 5, 8), I(5));
    /* spreading nothing returns the slice itself */
    REGS(&f)[8] = REGS(&f)[4]; REGS(&f)[9] = I(2); REGS(&f)[10] = I(0); REGS(&f)[11] = T_NILSLICE;
    CHECK_EQ(call(&f, B_APPEND, 6, 8, 4), R_NORMAL);
    CHECK_EQ(REGS(&f)[6], REGS(&f)[4]);
    /* zero fills the new capacity */
    REGS(&f)[8] = T_NILSLICE; REGS(&f)[9] = I(0); REGS(&f)[10] = T_TRUE; REGS(&f)[11] = I(1);
    CHECK_EQ(call(&f, B_APPEND, 6, 8, 4), R_NORMAL);
    CHECK_EQ(slice(&f, 6, 6, 0, 4), R_NORMAL);
    CHECK_EQ(get(&f, 6, 3), T_TRUE);
    /* byte append, byte spread of a string; mode mismatches are bad */
    REGS(&f)[0] = T_NILSLICE;
    CHECK_EQ(append(&f, 0, 0, I(0x147), 1), R_NORMAL);
    CHECK_EQ(get(&f, 0, 0), I(0x47));
    set_str(&f, 1, "OAT");
    CHECK_EQ(append(&f, 0, 0, REGS(&f)[1], 3), R_NORMAL);
    REGS(&f)[8] = REGS(&f)[0];
    CHECK_EQ(call(&f, B_STR_FROM, 2, 8, 1), R_NORMAL);
    CHECK(str_is(&f, 2, "GOAT"));
    CHECK_EQ(append(&f, 2, 0, I(1), 0), R_BAD_CODE);     /* term append to bytes */
    CHECK_EQ(append(&f, 2, 3, I(1), 1), R_BAD_CODE);     /* byte append to terms */
    CHECK_EQ(append(&f, 2, 0, I(1), 4), R_BAD_CODE);     /* bad mode */
    CHECK_EQ(append(&f, 2, 3, REGS(&f)[1], 2), R_BAD_CODE); /* string spread into terms */
    actor_free(&f.vm, f.a);
}

TEST(slices_slicing) {
    Fx f; fx_init(&f, 128);
    CHECK_EQ(make(&f, 0, 0, 3, 5, I(0)), R_NORMAL);
    for (int i = 0; i < 3; i++) CHECK_EQ(set(&f, 0, i, I(10 + i)), R_NORMAL);
    CHECK_EQ(slice(&f, 1, 0, 1, 4), R_NORMAL);           /* hi up to cap */
    CHECK_EQ(len_of(&f, 1, B_LEN), 3);
    CHECK_EQ(len_of(&f, 1, B_CAP), 4);
    CHECK_EQ(get(&f, 1, 0), I(11));
    CHECK_EQ(get(&f, 1, 2), I(0));
    CHECK_EQ(set(&f, 1, 0, I(99)), R_NORMAL);            /* shared array */
    CHECK_EQ(get(&f, 0, 1), I(99));
    CHECK_EQ(slice(&f, 2, 0, 0, 3), R_NORMAL);           /* the whole: shared header */
    CHECK_EQ(REGS(&f)[2], REGS(&f)[0]);
    CHECK_EQ(slice(&f, 2, 0, 5, 5), R_NORMAL);           /* cap 0 drops the array */
    CHECK_EQ(arr_of(&f, 2), T_NIL);
    CHECK_EQ(slice(&f, 2, 0, 0, 6), R_BAD_CODE);         /* hi > cap */
    CHECK_EQ(slice(&f, 2, 0, 3, 2), R_BAD_CODE);
    CHECK_EQ(slice(&f, 2, 0, -1, 2), R_BAD_CODE);
    CHECK_EQ(slice(&f, 2, 0, 0, -1), R_BAD_CODE);
    REGS(&f)[3] = T_NILSLICE;
    CHECK_EQ(slice(&f, 2, 3, 0, 0), R_NORMAL);
    CHECK_EQ(REGS(&f)[2], T_NILSLICE);
    CHECK_EQ(slice(&f, 2, 3, 0, 1), R_BAD_CODE);
    actor_free(&f.vm, f.a);
}

TEST(slices_copy) {
    Fx f; fx_init(&f, 128);
    CHECK_EQ(make(&f, 0, 0, 5, 5, I(0)), R_NORMAL);
    for (int i = 0; i < 5; i++) CHECK_EQ(set(&f, 0, i, I(i)), R_NORMAL);
    CHECK_EQ(slice(&f, 1, 0, 1, 5), R_NORMAL);
    REGS(&f)[8] = REGS(&f)[1]; REGS(&f)[9] = REGS(&f)[0];  /* copy(s[1:], s): overlap */
    CHECK_EQ(call(&f, B_COPY, 2, 8, 2), R_NORMAL);
    CHECK_EQ(REGS(&f)[2], I(4));
    for (int i = 0; i < 5; i++) CHECK_EQ(get(&f, 0, i), I(i ? i - 1 : 0));
    CHECK_EQ(make(&f, 3, 1, 3, 3, I(0)), R_NORMAL);
    set_str(&f, 4, "hello");
    REGS(&f)[8] = REGS(&f)[3]; REGS(&f)[9] = REGS(&f)[4];  /* copy(b, "hello") */
    CHECK_EQ(call(&f, B_COPY, 2, 8, 2), R_NORMAL);
    CHECK_EQ(REGS(&f)[2], I(3));
    CHECK_EQ(get(&f, 3, 2), I('l'));
    REGS(&f)[8] = REGS(&f)[0]; REGS(&f)[9] = REGS(&f)[4];  /* copy(terms, string) */
    CHECK_EQ(call(&f, B_COPY, 2, 8, 2), R_BAD_CODE);
    REGS(&f)[8] = T_NILSLICE;
    CHECK_EQ(call(&f, B_COPY, 2, 8, 2), R_NORMAL);
    CHECK_EQ(REGS(&f)[2], I(0));
    actor_free(&f.vm, f.a);
}

/* Appending in a loop on a small heap collects many times; the result is
 * intact and the block stays within its limit. */
TEST(slices_append_churn) {
    Fx f; fx_init(&f, 64);
    f.a->max_words = 512;
    REGS(&f)[0] = T_NILSLICE;
    for (int i = 0; i < 100; i++) {
        CHECK_EQ(append(&f, 0, 0, I(i), 0), R_NORMAL);
        set_str(&f, 1, "garbage garbage");
    }
    for (int i = 0; i < 100; i++) CHECK_EQ(get(&f, 0, i), I(i));
    uint32_t err = R_NORMAL;
    for (int i = 0; i < 1000 && err == R_NORMAL; i++) err = append(&f, 0, 0, I(i), 0);
    CHECK_EQ(err, R_OUT_OF_MEMORY);
    actor_free(&f.vm, f.a);
}

/* GC moves two headers on one array and keeps them sharing it; a byte
 * array's contents are not scanned (words that look like refs stay). */
TEST(slices_gc_keeps_sharing) {
    Fx f; fx_init(&f, 128);
    set_str(&f, 5, "garbage first, garbage first");
    REGS(&f)[5] = T_NIL;
    set_str(&f, 6, "elem");
    CHECK_EQ(make(&f, 0, 0, 2, 2, REGS(&f)[6]), R_NORMAL);
    CHECK_EQ(slice(&f, 1, 0, 1, 2), R_NORMAL);
    CHECK_EQ(make(&f, 2, 1, 8, 8, I(0)), R_NORMAL);
    uint32_t *bytes = f.a->mem + term_ref_off(arr_of(&f, 2)) + 2;
    bytes[0] = term_from_ref(0); bytes[1] = term_from_ref(4);   /* look like refs */
    REGS(&f)[6] = T_NIL;
    uint16_t hp = f.a->hp;
    heap_gc(&f.vm, f.a, 0, NULL, 0);
    CHECK(f.a->hp < hp);
    CHECK_EQ(arr_of(&f, 0), arr_of(&f, 1));
    CHECK_EQ(set(&f, 1, 0, I(42)), R_NORMAL);
    CHECK_EQ(get(&f, 0, 1), I(42));
    REGS(&f)[3] = get(&f, 0, 0);
    CHECK(str_is(&f, 3, "elem"));
    bytes = f.a->mem + term_ref_off(arr_of(&f, 2)) + 2;
    CHECK_EQ(bytes[0], term_from_ref(0));
    CHECK_EQ(bytes[1], term_from_ref(4));
    actor_free(&f.vm, f.a);
}

/* A struct holding two overlapping slices of one array: the receiver gets
 * one copy of the whole array, shared by both headers; later writes on
 * either side are not seen by the other. */
TEST(slices_msg_copy_shares) {
    Fx f; fx_init(&f, 128);
    Actor *b = actor_new(&f.vm, 128);
    CHECK(b != NULL);
    CHECK_EQ(make(&f, 0, 0, 4, 6, I(0)), R_NORMAL);
    for (int i = 0; i < 4; i++) CHECK_EQ(set(&f, 0, i, I(i)), R_NORMAL);
    CHECK_EQ(slice(&f, 1, 0, 1, 3), R_NORMAL);
    uint32_t off = heap_alloc(&f.vm, f.a, 4, NULL, 0);
    CHECK(off != UINT32_MAX);
    f.a->mem[off] = hdr_make(4, K_STRUCT);
    f.a->mem[off + 1] = 9;
    f.a->mem[off + 2] = REGS(&f)[0];
    f.a->mem[off + 3] = REGS(&f)[1];
    term st = term_from_ref(off);
    /* struct 4 + two headers 5 + 5 + array 2 + 6 */
    CHECK_EQ(copy_words(&f.vm, f.a, st), 4 + 5 + 5 + 8);
    uint16_t hp = f.a->hp;
    uint32_t before[256];
    memcpy(before, f.a->mem, (size_t)hp * 4u);
    Msg *m = msg_build(&f.vm, f.a, st, 0, NULL);
    CHECK(m != NULL);
    if (!m) return;
    CHECK_EQ(memcmp(before, f.a->mem, (size_t)hp * 4u), 0);   /* source restored */
    term r = T_NIL;
    CHECK(msg_unpack(&f.vm, b, m, &r));
    msg_free(m);
    term s0 = b->mem[term_ref_off(r) + 2], s1 = b->mem[term_ref_off(r) + 3];
    term a0 = b->mem[term_ref_off(s0) + 4], a1 = b->mem[term_ref_off(s1) + 4];
    CHECK_EQ(a0, a1);
    CHECK_EQ(hdr_kind(b->mem[term_ref_off(a0)]), K_ARRAY);
    CHECK_EQ(b->mem[term_ref_off(s1) + 1], 1);    /* off, len, cap kept */
    CHECK_EQ(b->mem[term_ref_off(s1) + 2], 2);
    CHECK_EQ(b->mem[term_ref_off(s1) + 3], 5);
    b->mem[term_ref_off(a0) + 2 + 2] = I(77);     /* receiver writes s1[1] */
    CHECK_EQ(get(&f, 0, 2), I(2));                /* sender unaffected */
    CHECK_EQ(set(&f, 0, 3, I(55)), R_NORMAL);
    CHECK_EQ(b->mem[term_ref_off(a0) + 2 + 3], I(3));
    actor_free(&f.vm, b);
    actor_free(&f.vm, f.a);
}

/* append(b, 5, 6) with room for one value only: growth is decided once,
 * so nothing is written into the old array (Go semantics). */
TEST(slices_append_many_detaches) {
    Fx f; fx_init(&f, 256);
    CHECK_EQ(make(&f, 0, 0, 4, 4, I(0)), R_NORMAL);
    for (int i = 0; i < 4; i++) CHECK_EQ(set(&f, 0, i, I(i + 1)), R_NORMAL);
    CHECK_EQ(slice(&f, 1, 0, 0, 3), R_NORMAL);              /* len 3, cap 4 */
    REGS(&f)[8] = REGS(&f)[1]; REGS(&f)[9] = I(0); REGS(&f)[10] = I(0);
    REGS(&f)[11] = I(5); REGS(&f)[12] = I(6);
    CHECK_EQ(call(&f, B_APPEND, 2, 8, 5), R_NORMAL);
    CHECK_EQ(get(&f, 0, 3), I(4));                          /* old array untouched */
    CHECK_EQ(len_of(&f, 2, B_LEN), 5);
    CHECK_EQ(len_of(&f, 2, B_CAP), 8);
    CHECK_EQ(get(&f, 2, 3), I(5));
    CHECK_EQ(get(&f, 2, 4), I(6));
    REGS(&f)[8] = REGS(&f)[1]; REGS(&f)[9] = I(0);          /* one fits: in place */
    CHECK_EQ(call(&f, B_APPEND, 3, 8, 4), R_NORMAL);
    CHECK_EQ(get(&f, 0, 3), I(5));
    /* bytes: same; a non-int byte value is bad */
    CHECK_EQ(make(&f, 4, 1, 3, 4, I(0)), R_NORMAL);
    REGS(&f)[8] = REGS(&f)[4]; REGS(&f)[9] = I(1); REGS(&f)[10] = I(0);
    REGS(&f)[11] = I('a'); REGS(&f)[12] = I('b');
    CHECK_EQ(call(&f, B_APPEND, 5, 8, 5), R_NORMAL);
    CHECK_EQ(len_of(&f, 5, B_LEN), 5);
    CHECK_EQ(slice(&f, 6, 4, 0, 4), R_NORMAL);
    CHECK_EQ(get(&f, 6, 3), I(0));                          /* old byte array untouched */
    CHECK_EQ(get(&f, 5, 4), I('b'));
    REGS(&f)[8] = REGS(&f)[4]; REGS(&f)[9] = I(1); REGS(&f)[12] = T_TRUE;
    CHECK_EQ(call(&f, B_APPEND, 5, 8, 5), R_BAD_CODE);
    REGS(&f)[8] = REGS(&f)[4];                              /* spread takes one value */
    REGS(&f)[9] = I(3);
    CHECK_EQ(call(&f, B_APPEND, 5, 8, 5), R_BAD_CODE);
    CHECK_EQ(call(&f, B_APPEND, 5, 8, 3), R_BAD_CODE);      /* no value */
    actor_free(&f.vm, f.a);
}
