/* bytes.Bytes / bytes.Buffer builtins (strs.c) and the off-heap handle
 * table (bytes.c): refcounts and held_bytes under send, GC and exit; the
 * table and per-actor caps; slices keeping their root; the C API for
 * ports. Builtins are called directly on actors with 16-register frames. */
#include "test.h"
#include "actor.h"
#include "heap.h"
#include "msg.h"
#include "alloc.h"
#include "image.h"
#include "strs.h"
#include "bytes.h"
#include "signals.h"
#include "opcodes.h"
#include "term.h"
#include "config.h"
#include <string.h>

#define IMG_WORDS 17

/* An image whose const area holds the string "main" (at const offset 0). */
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

typedef struct Fx { uint32_t w[IMG_WORDS]; Image img; Vm vm; } Fx;

static void fx_init(Fx *f) {
    va_reset();
    build_img(f->w);
    CHECK_EQ(image_load(&f->img, f->w, IMG_WORDS), IMG_OK);
    CHECK_EQ(vm_init(&f->vm, &f->img, 16), 0);
    CHECK_EQ(bytes_table_used(), 0);
}

static Actor *spawn(Fx *f, uint16_t words) {
    Actor *a = actor_new(&f->vm, words);
    CHECK(a != NULL);
    CHECK(heap_push_frame(&f->vm, a, 16, -1));
    return a;
}

#define R(a) ((term *)(a)->mem + (a)->fp)
/* Words a holder list is charged while it exists. */
#define LIST_W ((uint32_t)((sizeof(HeldBytes) + 3u) / 4u))
#define I(v) term_from_int(v)

static uint32_t call(Fx *f, Actor *a, uint32_t id, uint32_t ra, uint32_t rb, uint32_t rc) {
    return strs_builtin(&f->vm, a, id, ra, rb, rc);
}

static void set_str(Fx *f, Actor *a, int r, const char *s) {
    uint32_t len = (uint32_t)strlen(s), words = 2u + (len + 3u) / 4u;
    uint32_t off = heap_alloc(&f->vm, a, words, NULL, 0);
    CHECK(off != UINT32_MAX);
    a->mem[off] = hdr_make(words, K_STRING);
    a->mem[off + 1] = len;
    a->mem[off + words - 1] = 0;
    memcpy(a->mem + off + 2, s, len);
    R(a)[r] = term_from_ref(off);
}

/* r13 = a Buffer holding n bytes 'a', 'b', ... (r12, r14 scratch). */
static void make_buf(Fx *f, Actor *a, uint32_t n) {
    R(a)[12] = I(0);
    CHECK_EQ(call(f, a, B_BUF_NEW, 13, 12, 1), R_NORMAL);
    for (uint32_t i = 0; i < n; i++) {
        R(a)[14] = I((int32_t)('a' + i % 26u));
        CHECK_EQ(call(f, a, B_BUF_WRITE, 12, 13, 2), R_NORMAL);
    }
}

/* r[ra] = Freeze of the Buffer in r13, which is then dropped. */
static uint32_t freeze(Fx *f, Actor *a, uint32_t ra) {
    uint32_t err = call(f, a, B_BUF_FREEZE, ra, 13, 1);
    R(a)[12] = R(a)[13] = R(a)[14] = T_NIL;
    return err;
}

/* r[ra] = a Bytes of n bytes 'a', 'b', ... through a Buffer. */
static uint32_t make_bytes(Fx *f, Actor *a, uint32_t ra, uint32_t n) {
    make_buf(f, a, n);
    return freeze(f, a, ra);
}

/* The Bytes in register r is n bytes 'a', 'b', ... */
static bool alpha_is(Fx *f, Actor *a, int r, uint32_t n) {
    const uint8_t *p;
    uint32_t len;
    if (!bytes_view(&f->vm, a, R(a)[r], &p, &len) || len != n) return false;
    for (uint32_t i = 0; i < n; i++) {
        if (p[i] != 'a' + i % 26u) return false;
    }
    return true;
}

static int32_t int_of(Fx *f, Actor *a, uint32_t id, uint32_t r) {
    R(a)[10] = R(a)[r];
    CHECK_EQ(call(f, a, id, 9, 10, 1), R_NORMAL);
    return term_to_int(R(a)[9]);
}

/* The bytes of the Bytes in register r equal s. */
static bool bytes_is(Fx *f, Actor *a, int r, const char *s) {
    const uint8_t *p;
    uint32_t len;
    if (!bytes_view(&f->vm, a, R(a)[r], &p, &len)) return false;
    return len == strlen(s) && (len == 0 || memcmp(p, s, len) == 0);
}

/* Sends r of from to `to` and receives it into to's register r. */
static void pass(Fx *f, Actor *from, int r, Actor *to) {
    CHECK_EQ(msg_send(&f->vm, from, to->id, R(from)[r]), 0);
    term t;
    CHECK(msg_receive(&f->vm, to, &t));
    R(to)[r] = t;
}

TEST(buffer_write_len_freeze_reuse) {
    Fx f; fx_init(&f);
    Actor *a = spawn(&f, 256);
    R(a)[1] = I(2);
    CHECK_EQ(call(&f, a, B_BUF_NEW, 0, 1, 1), R_NORMAL);
    CHECK_EQ(hdr_kind(a->mem[term_ref_off(R(a)[0])]), K_BUF);
    CHECK_EQ(int_of(&f, a, B_LEN, 0), 0);
    R(a)[1] = R(a)[0];
    set_str(&f, a, 2, "GET ");
    CHECK_EQ(call(&f, a, B_BUF_WRITE, 3, 1, 2), R_NORMAL);      /* string; grows */
    CHECK_EQ(R(a)[3], T_NIL);
    R(a)[1] = R(a)[0]; R(a)[2] = I('/' + 256);                    /* a byte: & 0xFF */
    CHECK_EQ(call(&f, a, B_BUF_WRITE, 3, 1, 2), R_NORMAL);
    set_str(&f, a, 4, "x y");                                     /* a []byte */
    R(a)[10] = R(a)[4];
    CHECK_EQ(call(&f, a, B_BYTESLICE_FROM, 2, 10, 1), R_NORMAL);
    R(a)[1] = R(a)[0];
    CHECK_EQ(call(&f, a, B_BUF_WRITE, 3, 1, 2), R_NORMAL);
    R(a)[1] = R(a)[0]; R(a)[2] = T_NILSLICE;                      /* empty: nothing */
    CHECK_EQ(call(&f, a, B_BUF_WRITE, 3, 1, 2), R_NORMAL);
    CHECK_EQ(int_of(&f, a, B_LEN, 0), 8);
    heap_gc(&f.vm, a, 0, NULL, 0);                                /* K_BUF moves */
    R(a)[1] = R(a)[0];
    CHECK_EQ(call(&f, a, B_BUF_FREEZE, 5, 1, 1), R_NORMAL);
    CHECK(bytes_is(&f, a, 5, "GET /x y"));
    CHECK_EQ(hdr_kind(a->mem[term_ref_off(R(a)[5])]), K_BYTES);  /* small: heap */
    CHECK_EQ(int_of(&f, a, B_LEN, 0), 0);                         /* emptied */
    R(a)[1] = R(a)[0]; R(a)[2] = R(a)[5];                         /* reuse; write a Bytes */
    CHECK_EQ(call(&f, a, B_BUF_WRITE, 3, 1, 2), R_NORMAL);
    R(a)[1] = R(a)[0];
    CHECK_EQ(call(&f, a, B_BUF_FREEZE, 6, 1, 1), R_NORMAL);
    CHECK(bytes_is(&f, a, 6, "GET /x y"));
    R(a)[1] = R(a)[0];
    CHECK_EQ(call(&f, a, B_BUF_FREEZE, 7, 1, 1), R_NORMAL);      /* empty */
    CHECK_EQ(R(a)[7], T_EMPTYBYTES);
    /* The zero Buffer (nil) and non-buffers are badcode. */
    R(a)[1] = T_NIL; R(a)[2] = I(1);
    CHECK_EQ(call(&f, a, B_BUF_WRITE, 3, 1, 2), R_BAD_CODE);
    CHECK_EQ(call(&f, a, B_BUF_FREEZE, 3, 1, 1), R_BAD_CODE);
    R(a)[10] = T_NIL;
    CHECK_EQ(call(&f, a, B_LEN, 9, 10, 1), R_BAD_CODE);
    R(a)[1] = R(a)[0]; R(a)[2] = T_TRUE;
    CHECK_EQ(call(&f, a, B_BUF_WRITE, 3, 1, 2), R_BAD_CODE);
    R(a)[1] = I(-1);
    CHECK_EQ(call(&f, a, B_BUF_NEW, 3, 1, 1), R_BAD_CODE);
    CHECK_EQ(bytes_table_used(), 0);
    actor_free(&f.vm, a);
}

TEST(bytes_from_index_slice_equal_copyto) {
    Fx f; fx_init(&f);
    Actor *a = spawn(&f, 512);
    set_str(&f, a, 1, "hello world");
    CHECK_EQ(call(&f, a, B_BYTES_FROM, 0, 1, 1), R_NORMAL);
    CHECK_EQ(hdr_kind(a->mem[term_ref_off(R(a)[0])]), K_BYTES);
    CHECK(bytes_is(&f, a, 0, "hello world"));
    CHECK_EQ(int_of(&f, a, B_LEN, 0), 11);
    R(a)[1] = T_EMPTYSTR;
    CHECK_EQ(call(&f, a, B_BYTES_FROM, 2, 1, 1), R_NORMAL);
    CHECK_EQ(R(a)[2], T_EMPTYBYTES);
    CHECK_EQ(int_of(&f, a, B_LEN, 2), 0);
    R(a)[10] = R(a)[0]; R(a)[11] = I(4);
    CHECK_EQ(call(&f, a, B_INDEX, 9, 10, 2), R_NORMAL);
    CHECK_EQ(R(a)[9], I('o'));
    R(a)[11] = I(11);
    CHECK_EQ(call(&f, a, B_INDEX, 9, 10, 2), R_BAD_CODE);
    R(a)[11] = I(-1);
    CHECK_EQ(call(&f, a, B_INDEX, 9, 10, 2), R_BAD_CODE);
    /* Slice: a heap copy; the whole is itself; empty is T_EMPTYBYTES. */
    R(a)[10] = R(a)[0]; R(a)[11] = I(6); R(a)[12] = I(11);
    CHECK_EQ(call(&f, a, B_SLICE, 3, 10, 3), R_NORMAL);
    CHECK(bytes_is(&f, a, 3, "world"));
    R(a)[10] = R(a)[0]; R(a)[11] = I(0); R(a)[12] = I(11);
    CHECK_EQ(call(&f, a, B_SLICE, 4, 10, 3), R_NORMAL);
    CHECK_EQ(R(a)[4], R(a)[0]);
    R(a)[10] = R(a)[0]; R(a)[11] = I(3); R(a)[12] = I(3);
    CHECK_EQ(call(&f, a, B_SLICE, 4, 10, 3), R_NORMAL);
    CHECK_EQ(R(a)[4], T_EMPTYBYTES);
    R(a)[10] = R(a)[0]; R(a)[11] = I(3); R(a)[12] = I(12);
    CHECK_EQ(call(&f, a, B_SLICE, 4, 10, 3), R_BAD_CODE);
    R(a)[11] = I(-1); R(a)[12] = I(2);
    CHECK_EQ(call(&f, a, B_SLICE, 4, 10, 3), R_BAD_CODE);
    /* Equal by content, of any representations. */
    set_str(&f, a, 1, "world");
    R(a)[10] = R(a)[1];
    CHECK_EQ(call(&f, a, B_BYTESLICE_FROM, 5, 10, 1), R_NORMAL);   /* []byte("world") */
    R(a)[10] = R(a)[5];
    CHECK_EQ(call(&f, a, B_BYTES_FROM, 6, 10, 1), R_NORMAL);       /* FromSlice */
    R(a)[10] = R(a)[3]; R(a)[11] = R(a)[6];
    CHECK_EQ(call(&f, a, B_BYTES_EQUAL, 9, 10, 2), R_NORMAL);
    CHECK_EQ(R(a)[9], T_TRUE);
    R(a)[11] = R(a)[0];
    CHECK_EQ(call(&f, a, B_BYTES_EQUAL, 9, 10, 2), R_NORMAL);
    CHECK_EQ(R(a)[9], T_FALSE);
    R(a)[10] = T_EMPTYBYTES; R(a)[11] = T_EMPTYBYTES;
    CHECK_EQ(call(&f, a, B_BYTES_EQUAL, 9, 10, 2), R_NORMAL);
    CHECK_EQ(R(a)[9], T_TRUE);
    R(a)[11] = R(a)[1];                                            /* a string: bad */
    CHECK_EQ(call(&f, a, B_BYTES_EQUAL, 9, 10, 2), R_BAD_CODE);
    /* CopyTo, copy(p, x), String, []byte(x). */
    R(a)[10] = I(1); R(a)[11] = I(3); R(a)[12] = I(3); R(a)[13] = I(0);
    CHECK_EQ(call(&f, a, B_MAKE_SLICE, 7, 10, 4), R_NORMAL);
    R(a)[10] = R(a)[0]; R(a)[11] = R(a)[7];
    CHECK_EQ(call(&f, a, B_BYTES_COPYTO, 9, 10, 2), R_NORMAL);
    CHECK_EQ(R(a)[9], I(3));
    R(a)[10] = R(a)[7];
    CHECK_EQ(call(&f, a, B_STR_FROM, 8, 10, 1), R_NORMAL);
    R(a)[10] = R(a)[8];
    CHECK_EQ(call(&f, a, B_BYTES_FROM, 8, 10, 1), R_NORMAL);
    CHECK(bytes_is(&f, a, 8, "hel"));
    R(a)[10] = R(a)[7]; R(a)[11] = R(a)[3];
    CHECK_EQ(call(&f, a, B_COPY, 9, 10, 2), R_NORMAL);
    CHECK_EQ(R(a)[9], I(3));
    R(a)[10] = R(a)[3];
    CHECK_EQ(call(&f, a, B_STR_FROM, 8, 10, 1), R_NORMAL);
    CHECK_EQ(hdr_kind(a->mem[term_ref_off(R(a)[8])]), K_STRING);
    R(a)[10] = R(a)[3];
    CHECK_EQ(call(&f, a, B_BYTESLICE_FROM, 8, 10, 1), R_NORMAL);
    CHECK_EQ(int_of(&f, a, B_LEN, 8), 5);
    R(a)[10] = T_EMPTYBYTES;
    CHECK_EQ(call(&f, a, B_STR_FROM, 8, 10, 1), R_NORMAL);
    CHECK_EQ(R(a)[8], T_EMPTYSTR);
    /* TYPEIS-relevant: none of this made a handle. */
    CHECK_EQ(bytes_table_used(), 0);
    actor_free(&f.vm, a);
}

/* Freeze of more than GOGO_BYTES_INLINE bytes makes a handle charged in
 * full; sending it to 3 actors makes each hold it and be charged in full;
 * the table empties only when the last holder exits. */
TEST(bytes_refcount) {
    Fx f; fx_init(&f);
    Actor *a = spawn(&f, 256);
    Actor *r[3];
    for (int i = 0; i < 3; i++) r[i] = spawn(&f, 256);
    uint32_t base = actor_charged_words(a);
    CHECK_EQ(make_bytes(&f, a, 0, 200), R_NORMAL);
    CHECK(term_is_handle(R(a)[0]));
    CHECK(alpha_is(&f, a, 0, 200));
    CHECK_EQ(int_of(&f, a, B_LEN, 0), 200);
    CHECK_EQ(bytes_table_used(), 1);
    CHECK_EQ(bytes_held_bytes(a), 200);
    CHECK_EQ(bytes_held_count(a), 1);
    CHECK_EQ(actor_charged_words(a), base + 50 + LIST_W);
    for (int i = 0; i < 3; i++) {
        uint32_t mb = r[i]->mbox_words;
        CHECK_EQ(msg_send(&f.vm, a, r[i]->id, R(a)[0]), 0);
        CHECK_EQ(r[i]->mbox_words, mb + msg_footprint_words(0) + 50u);   /* queued: charged */
        term t;
        CHECK(msg_receive(&f.vm, r[i], &t));
        R(r[i])[0] = t;
        CHECK_EQ(t, R(a)[0]);                                  /* the same handle */
        CHECK_EQ(r[i]->mbox_words, mb);
        CHECK_EQ(bytes_held_bytes(r[i]), 200);
    }
    CHECK_EQ(bytes_table_used(), 1);
    actor_exit(&f.vm, a, R_NORMAL, T_EMPTYSTR);
    actor_exit(&f.vm, r[0], R_NORMAL, T_EMPTYSTR);
    actor_exit(&f.vm, r[1], R_NORMAL, T_EMPTYSTR);
    CHECK_EQ(bytes_table_used(), 1);
    CHECK(alpha_is(&f, r[2], 0, 200));
    actor_exit(&f.vm, r[2], R_NORMAL, T_EMPTYSTR);
    CHECK_EQ(bytes_table_used(), 0);
    CHECK_EQ(f.vm.live, 0);
}

/* A handle no root reaches is released by the holder's next GC; one inside
 * a live struct, and one only a GC root (heap_alloc roots) holds, stay. */
TEST(bytes_gc_drop) {
    Fx f; fx_init(&f);
    Actor *a = spawn(&f, 256);
    CHECK_EQ(make_bytes(&f, a, 0, 100), R_NORMAL);
    CHECK_EQ(make_bytes(&f, a, 1, 90), R_NORMAL);
    CHECK_EQ(make_bytes(&f, a, 2, 80), R_NORMAL);
    CHECK_EQ(bytes_table_used(), 3);
    CHECK_EQ(bytes_held_bytes(a), 270);
    /* r0 into a struct (kept in r3), r2 only in a C root. */
    uint32_t off = heap_alloc(&f.vm, a, 3, NULL, 0);
    a->mem[off] = hdr_make(3, K_STRUCT);
    a->mem[off + 1] = 0;
    a->mem[off + 2] = R(a)[0];
    R(a)[3] = term_from_ref(off);
    term root = R(a)[2];
    R(a)[0] = R(a)[1] = R(a)[2] = T_NIL;
    heap_gc(&f.vm, a, 0, &root, 1);
    CHECK_EQ(bytes_table_used(), 2);
    CHECK_EQ(bytes_held_bytes(a), 180);
    CHECK_EQ(bytes_held_count(a), 2);
    heap_gc(&f.vm, a, 0, NULL, 0);                           /* the root is gone */
    CHECK_EQ(bytes_held_bytes(a), 100);
    R(a)[3] = T_NIL;
    heap_gc(&f.vm, a, 0, NULL, 0);
    CHECK_EQ(bytes_table_used(), 0);
    CHECK_EQ(bytes_held_bytes(a), 0);
    CHECK_EQ(bytes_held_count(a), 0);
    actor_free(&f.vm, a);
}

/* Exit releases what the actor holds and what its mailbox carries. */
TEST(bytes_exit_releases) {
    Fx f; fx_init(&f);
    Actor *a = spawn(&f, 256), *b = spawn(&f, 256);
    CHECK_EQ(make_bytes(&f, a, 0, 300), R_NORMAL);
    CHECK_EQ(make_bytes(&f, a, 1, 70), R_NORMAL);
    CHECK_EQ(msg_send(&f.vm, a, b->id, R(a)[0]), 0);       /* queued, unread */
    CHECK_EQ(b->mbox_words, msg_footprint_words(0) + 75u);
    actor_exit(&f.vm, a, R_NORMAL, T_EMPTYSTR);
    CHECK_EQ(bytes_table_used(), 1);                         /* the message keeps it */
    actor_exit(&f.vm, b, R_NORMAL, T_EMPTYSTR);
    CHECK_EQ(bytes_table_used(), 0);
    CHECK_EQ(va_used_bytes() > 0, 1);
}

/* Per-actor cap: the 17th live handle fails (OutOfMemory) even after a GC;
 * a dropped one makes room again. Table full: only the offender fails. */
TEST(bytes_table_full_and_per_actor_cap) {
    Fx f; fx_init(&f);
    Actor *act[5];
    for (int i = 0; i < 5; i++) act[i] = spawn(&f, 1024);
    Actor *a = act[0];
    for (uint32_t i = 0; i < GOGO_BYTES_PER_ACTOR; i++) {
        CHECK_EQ(make_bytes(&f, a, 0, 65), R_NORMAL);
        /* keep each in a struct chain so all stay live */
        uint32_t off = heap_alloc(&f.vm, a, 4, NULL, 0);
        CHECK(off != UINT32_MAX);
        a->mem[off] = hdr_make(4, K_STRUCT);
        a->mem[off + 1] = 0;
        a->mem[off + 2] = R(a)[0];
        a->mem[off + 3] = i ? R(a)[1] : T_NIL;
        R(a)[1] = term_from_ref(off);
    }
    CHECK_EQ(bytes_held_count(a), GOGO_BYTES_PER_ACTOR);
    CHECK_EQ(make_bytes(&f, a, 0, 65), R_OUT_OF_MEMORY);
    CHECK_EQ(bytes_held_count(a), GOGO_BYTES_PER_ACTOR);
    R(a)[0] = T_NIL;
    R(a)[1] = a->mem[term_ref_off(R(a)[1]) + 3];             /* drop the newest */
    CHECK_EQ(make_bytes(&f, a, 0, 65), R_NORMAL);            /* the GC made room */
    CHECK_EQ(bytes_held_count(a), GOGO_BYTES_PER_ACTOR);
    /* Fill the table from three more actors. */
    uint32_t want = GOGO_BYTES_HANDLES - GOGO_BYTES_PER_ACTOR;
    for (uint32_t k = 0; k < want; k++) {
        Actor *h = act[1 + k / GOGO_BYTES_PER_ACTOR];
        uint32_t r = 2u + k % GOGO_BYTES_PER_ACTOR % 8u;
        CHECK_EQ(make_bytes(&f, h, 0, 65), R_NORMAL);
        uint32_t off = heap_alloc(&f.vm, h, 4, NULL, 0);
        CHECK(off != UINT32_MAX);
        h->mem[off] = hdr_make(4, K_STRUCT);
        h->mem[off + 1] = 0;
        h->mem[off + 2] = R(h)[0];
        h->mem[off + 3] = R(h)[r] == T_NIL ? T_NIL : R(h)[r];
        R(h)[r] = term_from_ref(off);
        R(h)[0] = T_NIL;
    }
    CHECK_EQ(bytes_table_used(), GOGO_BYTES_HANDLES);
    Actor *e = act[4];
    CHECK_EQ(make_bytes(&f, e, 0, 65), R_OUT_OF_MEMORY);     /* the offender */
    CHECK_EQ(bytes_held_count(e), 0);
    CHECK_EQ(make_bytes(&f, e, 0, 64), R_NORMAL);            /* inline: no handle */
    CHECK_EQ(bytes_table_used(), GOGO_BYTES_HANDLES);
    for (int i = 0; i < 5; i++) actor_exit(&f.vm, act[i], R_NORMAL, T_EMPTYSTR);
    CHECK_EQ(bytes_table_used(), 0);
}

/* Table full of handles only idle actors' stale registers name (they
 * received them and blocked again, never collecting): a Freeze elsewhere
 * collects those holders and succeeds; what an idle actor still reaches
 * stays, and the dropped entries' charges go exactly. */
TEST(bytes_table_full_idle_holders) {
    Fx f; fx_init(&f);
    Actor *a = spawn(&f, 1024), *e = spawn(&f, 1024), *sink[4];
    for (int i = 0; i < 4; i++) sink[i] = spawn(&f, 1024);
    for (uint32_t k = 0; k < GOGO_BYTES_HANDLES; k++) {
        CHECK_EQ(make_bytes(&f, a, 0, 65), R_NORMAL);
        CHECK_EQ(msg_send(&f.vm, a, sink[k % 4u]->id, R(a)[0]), 0);
        R(a)[0] = T_NIL;
        heap_gc(&f.vm, a, 0, NULL, 0);                       /* a lets go of it */
    }
    for (int i = 0; i < 4; i++) {
        term t;
        while (msg_receive(&f.vm, sink[i], &t)) R(sink[i])[0] = t;
        CHECK_EQ(bytes_held_count(sink[i]), GOGO_BYTES_HANDLES / 4u);
        if (i < 3) R(sink[i])[0] = T_NIL;                     /* sink[3] keeps its last */
    }
    CHECK_EQ(bytes_table_used(), GOGO_BYTES_HANDLES);
    CHECK_EQ(make_bytes(&f, e, 0, 100), R_NORMAL);
    CHECK(alpha_is(&f, e, 0, 100));
    for (int i = 0; i < 3; i++) {
        CHECK_EQ(bytes_held_count(sink[i]), 0);
        CHECK_EQ(actor_extra_charged(sink[i]), 0);
        CHECK_EQ(actor_charged_words(sink[i]), (uint32_t)sink[i]->mbox_words);
    }
    CHECK_EQ(bytes_held_count(sink[3]), 1);
    CHECK_EQ(bytes_held_bytes(sink[3]), 65);
    CHECK_EQ(actor_charged_words(sink[3]), (uint32_t)sink[3]->mbox_words + LIST_W + 17u);
    CHECK_EQ(bytes_table_used(), 2);
    actor_exit(&f.vm, a, R_NORMAL, T_EMPTYSTR);
    actor_exit(&f.vm, e, R_NORMAL, T_EMPTYSTR);
    for (int i = 0; i < 4; i++) actor_exit(&f.vm, sink[i], R_NORMAL, T_EMPTYSTR);
    CHECK_EQ(bytes_table_used(), 0);
}

/* A Freeze that would take the actor past its limit is OutOfMemory; the
 * same size within the limit works. */
TEST(bytes_limit_on_create) {
    Fx f; fx_init(&f);
    Actor *a = spawn(&f, 128);
    make_buf(&f, a, 1000);
    heap_gc(&f.vm, a, 0, NULL, 0);
    uint32_t live = (uint32_t)a->hp + (a->mem_words - a->sp);
    a->max_words = (uint16_t)(live + 200u);                   /* 800 bytes beside the live data */
    CHECK_EQ(call(&f, a, B_BUF_FREEZE, 0, 13, 1), R_OUT_OF_MEMORY);
    CHECK_EQ(bytes_table_used(), 0);
    a->max_words = (uint16_t)(live + 260u + LIST_W);          /* 1040 bytes + list */
    CHECK_EQ(freeze(&f, a, 0), R_NORMAL);
    CHECK(alpha_is(&f, a, 0, 1000));
    CHECK_EQ(bytes_held_bytes(a), 1000);
    actor_exit(&f.vm, a, R_NORMAL, T_EMPTYSTR);
    CHECK_EQ(bytes_table_used(), 0);
}

/* The retry after a Freeze fails against the limit collects without
 * growth slack: a block the GC grew to twice its live data (the buffer's
 * old arrays now garbage) shrinks to the live data, so a Bytes that fits
 * beside it is made. */
TEST(bytes_make_retry_drops_slack) {
    Fx f; fx_init(&f);
    Actor *a = spawn(&f, 128);
    make_buf(&f, a, 1000);                     /* grows: 16, 32, ... 1024-byte arrays */
    heap_gc(&f.vm, a, 0, NULL, 0);             /* the policy keeps room: block >> live */
    uint32_t live = (uint32_t)a->hp + (a->mem_words - a->sp);
    CHECK(a->mem_words > live + 100u);
    /* Room for live + the 1000-byte Bytes + the list, not for the block's slack. */
    a->max_words = (uint16_t)(live + 250u + LIST_W + 4u);
    CHECK((uint32_t)a->mem_words + 250u > a->max_words);
    CHECK_EQ(freeze(&f, a, 0), R_NORMAL);
    CHECK(alpha_is(&f, a, 0, 1000));
    CHECK((uint32_t)a->mem_words + actor_charged_words(a) <= a->max_words);
    actor_exit(&f.vm, a, R_NORMAL, T_EMPTYSTR);
    CHECK_EQ(bytes_table_used(), 0);
}

/* A heap allocation that fails because garbage Bytes still count against
 * the limit succeeds on the tight retry, which reads the limit after the
 * collection released them. */
TEST(heap_alloc_retry_after_bytes_sweep) {
    Fx f; fx_init(&f);
    Actor *a = spawn(&f, 64);
    CHECK_EQ(make_bytes(&f, a, 0, 2000), R_NORMAL);           /* 500 words held */
    heap_gc(&f.vm, a, 0, NULL, 0);
    a->max_words = (uint16_t)(a->mem_words + 500u + LIST_W + 20u);
    R(a)[0] = T_NIL;                                          /* now garbage, still charged */
    uint32_t off = heap_alloc(&f.vm, a, 300, NULL, 0);
    CHECK(off != UINT32_MAX);
    CHECK_EQ(bytes_held_bytes(a), 0);
    CHECK((uint32_t)a->mem_words + actor_charged_words(a) <= a->max_words);
    actor_exit(&f.vm, a, R_NORMAL, T_EMPTYSTR);
    CHECK_EQ(bytes_table_used(), 0);
}

/* A message carrying a handle charges the receiver's mailbox in full on
 * enqueue: over its limit, the receiver is marked MailboxOverflow, the
 * sender is untouched and the message's reference goes. */
TEST(bytes_mailbox_overflow) {
    Fx f; fx_init(&f);
    Actor *a = spawn(&f, 256), *b = spawn(&f, 64);
    b->max_words = 512;                                       /* 2048 bytes */
    CHECK_EQ(make_bytes(&f, a, 0, 3000), R_NORMAL);
    CHECK_EQ(msg_send(&f.vm, a, b->id, R(a)[0]), 0);
    CHECK(b->flags & AF_EXIT_PENDING);
    CHECK_EQ(b->mbox_words, 0);
    uint32_t code; term fn;
    CHECK(actor_exit_pending(b, &code, &fn));
    CHECK_EQ(code, R_MAILBOX_OVERFLOW);
    CHECK_EQ(bytes_table_used(), 1);
    CHECK_EQ(bytes_held_bytes(a), 3000);
    actor_free(&f.vm, b);
    actor_exit(&f.vm, a, R_NORMAL, T_EMPTYSTR);
    CHECK_EQ(bytes_table_used(), 0);
}

/* The holder list a first handle makes is in the enqueue quota: a message
 * that fits only without it overflows the mailbox on enqueue
 * (MailboxOverflow), not later as OutOfMemory; with room for it, the
 * receive succeeds within the limit. */
TEST(bytes_enqueue_counts_list) {
    Fx f; fx_init(&f);
    Actor *a = spawn(&f, 256), *b = spawn(&f, 64), *c = spawn(&f, 64);
    CHECK_EQ(make_bytes(&f, a, 0, 400), R_NORMAL);
    uint32_t fw = msg_footprint_words(0) + 100u;
    CHECK_EQ(bytes_list_words(b), LIST_W);
    b->max_words = (uint16_t)(b->mem_words + fw + LIST_W - 1u);
    CHECK_EQ(msg_send(&f.vm, a, b->id, R(a)[0]), 0);
    CHECK(b->flags & AF_EXIT_PENDING);
    uint32_t code; term fn;
    CHECK(actor_exit_pending(b, &code, &fn));
    CHECK_EQ(code, R_MAILBOX_OVERFLOW);
    CHECK_EQ(b->mbox_words, 0);
    c->max_words = (uint16_t)(c->mem_words + fw + LIST_W);
    CHECK_EQ(msg_send(&f.vm, a, c->id, R(a)[0]), 0);
    CHECK(!(c->flags & AF_EXIT_PENDING));
    CHECK_EQ(c->mbox_words, fw);
    term t;
    CHECK(msg_receive(&f.vm, c, &t));
    R(c)[0] = t;
    CHECK_EQ(bytes_list_words(c), 0);
    CHECK((uint32_t)c->mem_words + actor_charged_words(c) <= actor_max_words(c));
    actor_free(&f.vm, b);
    actor_exit(&f.vm, c, R_NORMAL, T_EMPTYSTR);
    actor_exit(&f.vm, a, R_NORMAL, T_EMPTYSTR);
    CHECK_EQ(bytes_table_used(), 0);
}

/* Slice of a handle shares its root: the holder is charged the root's full
 * size, and the root lives until the slice goes. Small slices copy. */
TEST(bytes_slice_parent) {
    Fx f; fx_init(&f);
    Actor *a = spawn(&f, 256), *b = spawn(&f, 256);
    CHECK_EQ(make_bytes(&f, a, 0, 200), R_NORMAL);
    R(a)[10] = R(a)[0]; R(a)[11] = I(26); R(a)[12] = I(126);
    CHECK_EQ(call(&f, a, B_SLICE, 1, 10, 3), R_NORMAL);
    CHECK(term_is_handle(R(a)[1]));
    CHECK(R(a)[1] != R(a)[0]);
    CHECK_EQ(bytes_table_used(), 2);
    CHECK_EQ(bytes_held_bytes(a), 200);                      /* one root, counted once */
    CHECK_EQ(int_of(&f, a, B_LEN, 1), 100);
    R(a)[10] = R(a)[1]; R(a)[11] = I(1);
    CHECK_EQ(call(&f, a, B_INDEX, 9, 10, 2), R_NORMAL);
    CHECK_EQ(R(a)[9], I('b'));
    /* A slice of the slice names the root directly. */
    R(a)[10] = R(a)[1]; R(a)[11] = I(10); R(a)[12] = I(90);
    CHECK_EQ(call(&f, a, B_SLICE, 2, 10, 3), R_NORMAL);
    CHECK(term_is_handle(R(a)[2]));
    CHECK_EQ(bytes_table_used(), 3);
    R(a)[10] = R(a)[2]; R(a)[11] = I(0);
    CHECK_EQ(call(&f, a, B_INDEX, 9, 10, 2), R_NORMAL);
    CHECK_EQ(R(a)[9], I('k'));
    /* <= GOGO_BYTES_INLINE: a heap copy. */
    R(a)[10] = R(a)[0]; R(a)[11] = I(0); R(a)[12] = I(GOGO_BYTES_INLINE);
    CHECK_EQ(call(&f, a, B_SLICE, 3, 10, 3), R_NORMAL);
    CHECK(term_is_ref(R(a)[3]));
    CHECK_EQ(bytes_table_used(), 3);
    /* b gets only the slice: charged the root in full. */
    pass(&f, a, 1, b);
    CHECK_EQ(bytes_held_bytes(b), 200);
    CHECK_EQ(bytes_held_count(b), 1);
    /* a drops everything; the root survives for b's slice. */
    for (int r = 0; r < 16; r++) R(a)[r] = T_NIL;            /* args too */
    heap_gc(&f.vm, a, 0, NULL, 0);
    CHECK_EQ(bytes_held_bytes(a), 0);
    CHECK_EQ(bytes_table_used(), 2);                         /* root + b's slice */
    R(b)[10] = R(b)[1]; R(b)[11] = I(99);
    CHECK_EQ(call(&f, b, B_INDEX, 9, 10, 2), R_NORMAL);
    CHECK_EQ(R(b)[9], I('a' + (26 + 99) % 26));
    R(b)[1] = R(b)[10] = T_NIL;
    heap_gc(&f.vm, b, 0, NULL, 0);
    CHECK_EQ(bytes_table_used(), 0);
    actor_free(&f.vm, a);
    actor_free(&f.vm, b);
}

/* Handles inside structs and buffers survive message copy and GC; TYPEIS
 * material: the copy holds the same handle term. */
TEST(bytes_in_struct_through_message) {
    Fx f; fx_init(&f);
    Actor *a = spawn(&f, 256), *b = spawn(&f, 256);
    CHECK_EQ(make_bytes(&f, a, 0, 150), R_NORMAL);
    CHECK_EQ(make_bytes(&f, a, 1, 20), R_NORMAL);            /* heap K_BYTES */
    uint32_t off = heap_alloc(&f.vm, a, 5, NULL, 0);
    a->mem[off] = hdr_make(5, K_STRUCT);
    a->mem[off + 1] = 0;
    a->mem[off + 2] = R(a)[0];
    a->mem[off + 3] = R(a)[1];
    a->mem[off + 4] = R(a)[0];                                 /* twice */
    R(a)[2] = term_from_ref(off);
    pass(&f, a, 2, b);
    const uint32_t *s = b->mem + term_ref_off(R(b)[2]);
    CHECK_EQ(s[2], R(a)[0]);
    CHECK_EQ(s[4], R(a)[0]);
    CHECK_EQ(hdr_kind(b->mem[term_ref_off(s[3])]), K_BYTES);
    CHECK_EQ(bytes_held_count(b), 1);
    CHECK_EQ(bytes_held_bytes(b), 150);
    heap_gc(&f.vm, b, 0, NULL, 0);
    CHECK_EQ(bytes_held_count(b), 1);                         /* reached via the struct */
    R(b)[2] = T_NIL;
    heap_gc(&f.vm, b, 0, NULL, 0);
    CHECK_EQ(bytes_held_count(b), 0);
    CHECK_EQ(bytes_table_used(), 1);
    actor_exit(&f.vm, a, R_NORMAL, T_EMPTYSTR);
    actor_exit(&f.vm, b, R_NORMAL, T_EMPTYSTR);
    CHECK_EQ(bytes_table_used(), 0);
}

/* The C API as a port would use it: a handle made for (and charged to) the
 * port's owner, pinned across the owner's GCs, released by the port;
 * Handoff = retain by the new owner, release by the old. */
TEST(bytes_port_api) {
    Fx f; fx_init(&f);
    Actor *owner = spawn(&f, 256), *other = spawn(&f, 256);
    uint32_t base = actor_charged_words(owner);
    uint8_t *d = NULL;
    term h = bytes_new(&f.vm, owner, 1000, &d);
    CHECK(term_is_handle(h));
    CHECK(d != NULL);
    CHECK_EQ(d[0], 0);
    memcpy(d, "frame", 5);
    uint32_t len = 0;
    const uint8_t *p = bytes_data(h, &len);
    CHECK_EQ(len, 1000);
    CHECK(memcmp(p, "frame", 5) == 0);
    CHECK_EQ(bytes_held_bytes(owner), 1000);
    CHECK_EQ(actor_charged_words(owner), base + 250 + LIST_W);
    heap_gc(&f.vm, owner, 0, NULL, 0);                       /* pinned: stays */
    CHECK_EQ(bytes_held_bytes(owner), 1000);
    /* Handoff to other. */
    CHECK(bytes_retain(&f.vm, other, h));
    bytes_release(&f.vm, owner, h);
    CHECK_EQ(bytes_held_bytes(owner), 0);
    CHECK_EQ(actor_charged_words(owner), base);
    CHECK_EQ(bytes_held_bytes(other), 1000);
    CHECK_EQ(bytes_table_used(), 1);
    /* The owner's code holding it too: release keeps it until a GC finds
     * it unreachable. */
    R(other)[0] = h;
    CHECK(bytes_adopt(other, h));
    bytes_release(&f.vm, other, h);
    CHECK_EQ(bytes_held_bytes(other), 1000);
    heap_gc(&f.vm, other, 0, NULL, 0);
    CHECK_EQ(bytes_held_bytes(other), 1000);                  /* r0 has it */
    R(other)[0] = T_NIL;
    heap_gc(&f.vm, other, 0, NULL, 0);
    CHECK_EQ(bytes_held_bytes(other), 0);
    CHECK_EQ(bytes_table_used(), 0);
    CHECK_EQ(actor_extra_charged(other), 0);                  /* list gone, uncharged */
    CHECK(!bytes_retain(&f.vm, other, h));                    /* stale */
    CHECK(bytes_data(h, &len) == NULL);
    bytes_release(&f.vm, other, h);                           /* no-op */
    /* Over the limit, a port allocation fails. */
    owner->max_words = 200;
    CHECK_EQ(bytes_new(&f.vm, owner, 4000, &d), T_NIL);
    CHECK_EQ(bytes_table_used(), 0);
    /* A pinned handle still held at exit is released. */
    CHECK(term_is_handle(bytes_new(&f.vm, other, 10, &d)));
    actor_exit(&f.vm, other, R_NORMAL, T_EMPTYSTR);
    actor_exit(&f.vm, owner, R_NORMAL, T_EMPTYSTR);
    CHECK_EQ(bytes_table_used(), 0);
}

/* The receiver's per-actor cap applies when it takes a message: over it
 * (after a GC), the receive fails and the message stays queued. */
TEST(bytes_receive_cap) {
    Fx f; fx_init(&f);
    Actor *a = spawn(&f, 1024), *b = spawn(&f, 1024);
    for (uint32_t i = 0; i <= GOGO_BYTES_PER_ACTOR; i++) {
        CHECK_EQ(make_bytes(&f, a, 0, 65), R_NORMAL);
        CHECK_EQ(msg_send(&f.vm, a, b->id, R(a)[0]), 0);
        R(a)[0] = T_NIL;
        heap_gc(&f.vm, a, 0, NULL, 0);                       /* a lets go of it */
    }
    CHECK_EQ(bytes_held_count(a), 0);
    CHECK_EQ(bytes_table_used(), GOGO_BYTES_PER_ACTOR + 1u);
    for (uint32_t i = 0; i < GOGO_BYTES_PER_ACTOR; i++) {
        term t;
        CHECK(msg_receive(&f.vm, b, &t));
        R(b)[i % 16u] = t;
        if (i >= 16u) CHECK(0);
    }
    term t;
    CHECK(!msg_receive(&f.vm, b, &t));
    CHECK(b->mbox_head != NULL);
    R(b)[0] = T_NIL;                                          /* one dropped: fits */
    CHECK(msg_receive(&f.vm, b, &t));
    CHECK_EQ(bytes_held_count(b), GOGO_BYTES_PER_ACTOR);
    actor_exit(&f.vm, a, R_NORMAL, T_EMPTYSTR);
    actor_exit(&f.vm, b, R_NORMAL, T_EMPTYSTR);
    CHECK_EQ(bytes_table_used(), 0);
}

/* The holder list is charged while it exists: it appears with the first
 * handle and goes with the last (GC sweep, release, exit). */
TEST(bytes_list_charged) {
    Fx f; fx_init(&f);
    Actor *a = spawn(&f, 256);
    CHECK_EQ(actor_extra_charged(a), 0);
    CHECK_EQ(make_bytes(&f, a, 0, 100), R_NORMAL);
    CHECK_EQ(actor_extra_charged(a), LIST_W);
    CHECK_EQ(actor_charged_words(a), (uint32_t)a->mbox_words + LIST_W + 25u);
    CHECK_EQ(make_bytes(&f, a, 1, 100), R_NORMAL);           /* one list for both */
    CHECK_EQ(actor_extra_charged(a), LIST_W);
    R(a)[1] = T_NIL;
    heap_gc(&f.vm, a, 0, NULL, 0);
    CHECK_EQ(actor_extra_charged(a), LIST_W);
    R(a)[0] = T_NIL;
    heap_gc(&f.vm, a, 0, NULL, 0);
    CHECK_EQ(actor_extra_charged(a), 0);
    CHECK_EQ(actor_charged_words(a), 0);
    uint8_t *d;
    term h = bytes_new(&f.vm, a, 8, &d);                      /* a port's buffer */
    CHECK_EQ(actor_extra_charged(a), LIST_W);
    bytes_release(&f.vm, a, h);
    CHECK_EQ(actor_extra_charged(a), 0);
    CHECK(term_is_handle(bytes_new(&f.vm, a, 8, &d)));
    actor_exit(&f.vm, a, R_NORMAL, T_EMPTYSTR);              /* exit frees it */
    CHECK_EQ(bytes_table_used(), 0);
}
