/* Registered names: the table, name bytes copied, and the exit hook. */
#include "test.h"
#include "test_host.h"
#include "actor.h"
#include "alloc.h"
#include "heap.h"
#include "image.h"
#include "interp.h"
#include "names.h"
#include "opcodes.h"
#include "signals.h"
#include "term.h"
#include "../include/gogo_errors.h"
#include <string.h>

#define IMG_WORDS 18
static void setup(uint32_t *w, Image *img, Vm *vm) {
    va_reset(vm);
    memset(w, 0, IMG_WORDS * sizeof(uint32_t));
    w[0] = IMAGE_MAGIC; w[1] = IMAGE_VERSION; w[2] = 1; w[3] = 0;
    w[4] = 3; w[5] = 1; w[6] = 0; w[7] = 0;
    w[9] = 0 | (2u << 16);
    w[11] = hdr_make(3, K_STRING); w[12] = 4; memcpy(&w[13], "main", 4);
    w[14] = OP_MAKE(OP_RET, 0, 0, 0);
    CHECK_EQ(image_load(img, w, img_seal(w)), IMG_OK);
    CHECK_EQ(test_vm_init(vm, img, 8), 0);
}

#define NAME(s) ((const uint8_t *)(s)), (uint32_t)(sizeof(s) - 1u)

TEST(names_basic) {
    uint32_t w[IMG_WORDS]; Image img; Vm vm;
    setup(w, &img, &vm);
    size_t base = va_used_bytes(&vm);
    Actor *a = actor_new(&vm, 32), *b = actor_new(&vm, 32);
    term p;
    CHECK_EQ(names_register(&vm, a->id, NAME("svc"), a->id), GOGO_E_OK);
    CHECK_EQ(names_register(&vm, b->id, NAME("svc"), b->id), GOGO_E_EXISTS);   /* taken */
    CHECK_EQ(names_register(&vm, b->id, NAME("svc2"), b->id), GOGO_E_OK);  /* longer name differs */
    CHECK_EQ(names_register(&vm, a->id, NAME(""), a->id), GOGO_E_OK);      /* empty name */
    CHECK(names_whereis(&vm, NAME("svc"), &p) && p == a->id);
    CHECK(names_whereis(&vm, NAME(""), &p) && p == a->id);
    CHECK(!names_whereis(&vm, NAME("sv"), &p));
    names_unregister(&vm, a->id, NAME("nope"));                      /* harmless */
    names_unregister(&vm, a->id, NAME("svc"));
    CHECK(!names_whereis(&vm, NAME("svc"), &p));
    CHECK_EQ(names_register(&vm, b->id, NAME("svc"), b->id), GOGO_E_OK);
    names_drop_actor(&vm, b->id);                             /* both of b's */
    CHECK(!names_whereis(&vm, NAME("svc"), &p));
    CHECK(!names_whereis(&vm, NAME("svc2"), &p));
    CHECK(names_whereis(&vm, NAME(""), &p) && p == a->id);
    actor_exit(&vm, a, R_NORMAL, T_EMPTYSTR);                 /* drops the rest */
    CHECK(vm.names == NULL);
    actor_free(&vm, b);
    CHECK_EQ(va_used_bytes(&vm), base);
}

TEST(names_dead_pid_refused) {
    uint32_t w[IMG_WORDS]; Image img; Vm vm;
    setup(w, &img, &vm);
    size_t base = va_used_bytes(&vm);
    Actor *a = actor_new(&vm, 32);
    term stale = a->id;
    actor_free(&vm, a);
    CHECK_EQ(names_register(&vm, stale, NAME("x"), stale), GOGO_E_NOPROC);
    CHECK_EQ(names_register(&vm, T_NIL, NAME("x"), T_NIL), GOGO_E_NOPROC);
    CHECK(vm.names == NULL);
    /* A dead pid is refused before the name is looked at. */
    Actor *b = actor_new(&vm, 32);
    CHECK_EQ(names_register(&vm, b->id, NAME("x"), b->id), GOGO_E_OK);
    CHECK_EQ(names_register(&vm, b->id, NAME("x"), stale), GOGO_E_NOPROC);
    actor_exit(&vm, b, R_NORMAL, T_EMPTYSTR);
    CHECK_EQ(va_used_bytes(&vm), base);
}

/* The name is copied: the source bytes can move or change afterwards. */
TEST(names_copy_bytes) {
    uint32_t w[IMG_WORDS]; Image img; Vm vm;
    setup(w, &img, &vm);
    size_t base = va_used_bytes(&vm);
    Actor *a = actor_new(&vm, 32);
    uint32_t off = heap_alloc(&vm, a, 3, NULL, 0);
    a->mem[off] = hdr_make(3, K_STRING);
    a->mem[off + 1] = 4;
    memcpy(&a->mem[off + 2], "name", 4);
    term s = term_from_ref(off);
    CHECK_EQ(names_register(&vm, a->id, (const uint8_t *)&a->mem[off + 2], 4, a->id), GOGO_E_OK);
    memset(&a->mem[off + 2], 'X', 4);                          /* clobber the source */
    heap_gc(&vm, a, 0, &s, 1);                                 /* heap is rebuilt */
    term p;
    CHECK(names_whereis(&vm, NAME("name"), &p) && p == a->id);
    CHECK(!names_whereis(&vm, NAME("XXXX"), &p));
    actor_exit(&vm, a, R_NORMAL, T_EMPTYSTR);
    CHECK_EQ(va_used_bytes(&vm), base);
}

/* An allocation failure reports -1 and leaves the table unchanged. */
TEST(names_oom) {
    uint32_t w[IMG_WORDS]; Image img; Vm vm;
    setup(w, &img, &vm);
    Actor *a = actor_new(&vm, 32);
    size_t base = va_used_bytes(&vm);
    va_set_limit(&vm, base);
    CHECK_EQ(names_register(&vm, a->id, NAME("x"), a->id), -1);
    va_set_limit(&vm, 0);
    CHECK(vm.names == NULL);
    CHECK_EQ(va_used_bytes(&vm), base);
    actor_exit(&vm, a, R_NORMAL, T_EMPTYSTR);
}

/* Only the actor that registered a name may unregister it: not
 * the named actor, not a third one. */
TEST(names_unregister_owner) {
    uint32_t w[IMG_WORDS]; Image img; Vm vm;
    setup(w, &img, &vm);
    Actor *a = actor_new(&vm, 32), *b = actor_new(&vm, 32), *c = actor_new(&vm, 32);
    term p;
    CHECK_EQ(names_register(&vm, a->id, NAME("svc"), b->id), GOGO_E_OK);   /* a names b */
    names_unregister(&vm, b->id, NAME("svc"));
    names_unregister(&vm, c->id, NAME("svc"));
    CHECK(names_whereis(&vm, NAME("svc"), &p) && p == b->id);
    names_unregister(&vm, a->id, NAME("svc"));
    CHECK(!names_whereis(&vm, NAME("svc"), &p));
    actor_exit(&vm, a, R_NORMAL, T_EMPTYSTR);
    actor_exit(&vm, b, R_NORMAL, T_EMPTYSTR);
    actor_exit(&vm, c, R_NORMAL, T_EMPTYSTR);
    CHECK(vm.names == NULL);
}

/* B_REGISTER (images before 15.7: a bool) and B_REGISTER_ERR (15.7,
 * actor.Register: the shared error code), run by the interpreter:
 *   r2 = Self(); r1 = ""; r3 = REGISTER_ERR(r1, r2) (OK); r4 = the same
 *   (EXISTS); r5 = REGISTER(r1, r2) (false); r2 = zero PID;
 *   r0 = REGISTER_ERR(r1, r2) (NOPROC); r0 = REGISTER(...) is not run;
 *   Receive (blocks, so the registers can be read). */
#define REG_WORDS 34
TEST(names_register_builtins) {
    uint32_t w[REG_WORDS]; Image img; Vm vm;
    va_reset(&vm);
    memset(w, 0, sizeof w);
    w[0] = IMAGE_MAGIC; w[1] = IMAGE_VERSION; w[2] = 1; w[3] = 0;
    w[4] = 3; w[5] = 16; w[6] = 0; w[7] = 0;
    w[9] = 0 | (6u << 16);
    w[11] = hdr_make(3, K_STRING); w[12] = 4; memcpy(&w[13], "main", 4);
    uint32_t *c = w + 14;
    c[0] = OP_MAKE(OP_BUILTIN, 2, 0, 0); c[1] = B_SELF;
    c[2] = OP_MAKE(OP_LOADK, 1, 0, 0); c[3] = T_EMPTYSTR;
    c[4] = OP_MAKE(OP_BUILTIN, 3, 1, 2); c[5] = B_REGISTER_ERR;
    c[6] = OP_MAKE(OP_BUILTIN, 4, 1, 2); c[7] = B_REGISTER_ERR;
    c[8] = OP_MAKE(OP_BUILTIN, 5, 1, 2); c[9] = B_REGISTER;
    c[10] = OP_MAKE(OP_LOADK, 2, 0, 0); c[11] = T_ZEROPID;
    c[12] = OP_MAKE(OP_BUILTIN, 0, 1, 2); c[13] = B_REGISTER_ERR;
    c[14] = OP_MAKE(OP_BUILTIN, 1, 0, 0); c[15] = B_RECEIVE;
    CHECK_EQ(image_load(&img, w, img_seal(w)), IMG_OK);
    CHECK_EQ(test_vm_init(&vm, &img, 8), 0);
    size_t base = va_used_bytes(&vm);
    Actor *a = actor_new(&vm, 64);
    CHECK(heap_push_frame(&vm, a, 6, -1));
    a->pc = img.code;
    CHECK_EQ(interp_run(&vm, a), RUN_WAIT);
    const term *r = a->mem + a->fp;
    CHECK_EQ(r[3], term_from_int(GOGO_E_OK));
    CHECK_EQ(r[4], term_from_int(GOGO_E_EXISTS));
    CHECK_EQ(r[5], T_FALSE);
    CHECK_EQ(r[0], term_from_int(GOGO_E_NOPROC));
    term p;
    CHECK(names_whereis(&vm, NAME(""), &p) && p == a->id);
    a->state = A_RUNNABLE;
    actor_exit(&vm, a, R_NORMAL, T_EMPTYSTR);
    CHECK(vm.names == NULL);
    CHECK_EQ(va_used_bytes(&vm), base);
}
