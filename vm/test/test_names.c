/* Registered names: the table, name bytes copied, and the exit hook. */
#include "test.h"
#include "actor.h"
#include "alloc.h"
#include "heap.h"
#include "image.h"
#include "names.h"
#include "opcodes.h"
#include "signals.h"
#include "term.h"
#include <string.h>

#define IMG_WORDS 15
static void setup(uint32_t *w, Image *img, Vm *vm) {
    va_reset();
    memset(w, 0, IMG_WORDS * sizeof(uint32_t));
    w[0] = 0x4F474F47; w[1] = 1; w[2] = 1; w[3] = 0;
    w[4] = 3; w[5] = 1; w[6] = 0; w[7] = 0;
    w[9] = 0 | (2u << 16);
    w[11] = hdr_make(3, K_STRING); w[12] = 4; memcpy(&w[13], "main", 4);
    w[14] = OP_MAKE(OP_RET, 0, 0, 0);
    CHECK_EQ(image_load(img, w, IMG_WORDS), IMG_OK);
    CHECK_EQ(vm_init(vm, img, 8), 0);
}

#define NAME(s) ((const uint8_t *)(s)), (uint32_t)(sizeof(s) - 1u)

TEST(names_basic) {
    uint32_t w[IMG_WORDS]; Image img; Vm vm;
    setup(w, &img, &vm);
    size_t base = va_used_bytes();
    Actor *a = actor_new(&vm, 32), *b = actor_new(&vm, 32);
    term p;
    CHECK_EQ(names_register(&vm, NAME("svc"), a->id), 1);
    CHECK_EQ(names_register(&vm, NAME("svc"), b->id), 0);     /* taken */
    CHECK_EQ(names_register(&vm, NAME("svc2"), b->id), 1);    /* longer name differs */
    CHECK_EQ(names_register(&vm, NAME(""), a->id), 1);        /* empty name */
    CHECK(names_whereis(&vm, NAME("svc"), &p) && p == a->id);
    CHECK(names_whereis(&vm, NAME(""), &p) && p == a->id);
    CHECK(!names_whereis(&vm, NAME("sv"), &p));
    names_unregister(&vm, NAME("nope"));                      /* harmless */
    names_unregister(&vm, NAME("svc"));
    CHECK(!names_whereis(&vm, NAME("svc"), &p));
    CHECK_EQ(names_register(&vm, NAME("svc"), b->id), 1);
    names_drop_actor(&vm, b->id);                             /* both of b's */
    CHECK(!names_whereis(&vm, NAME("svc"), &p));
    CHECK(!names_whereis(&vm, NAME("svc2"), &p));
    CHECK(names_whereis(&vm, NAME(""), &p) && p == a->id);
    actor_exit(&vm, a, R_NORMAL, T_EMPTYSTR);                 /* drops the rest */
    CHECK(vm.names == NULL);
    actor_free(&vm, b);
    CHECK_EQ(va_used_bytes(), base);
}

TEST(names_dead_pid_refused) {
    uint32_t w[IMG_WORDS]; Image img; Vm vm;
    setup(w, &img, &vm);
    size_t base = va_used_bytes();
    Actor *a = actor_new(&vm, 32);
    term stale = a->id;
    actor_free(&vm, a);
    CHECK_EQ(names_register(&vm, NAME("x"), stale), 0);
    CHECK_EQ(names_register(&vm, NAME("x"), T_NIL), 0);
    CHECK(vm.names == NULL);
    CHECK_EQ(va_used_bytes(), base);
}

/* The name is copied: the source bytes can move or change afterwards. */
TEST(names_copy_bytes) {
    uint32_t w[IMG_WORDS]; Image img; Vm vm;
    setup(w, &img, &vm);
    size_t base = va_used_bytes();
    Actor *a = actor_new(&vm, 32);
    uint32_t off = heap_alloc(&vm, a, 3, NULL, 0);
    a->mem[off] = hdr_make(3, K_STRING);
    a->mem[off + 1] = 4;
    memcpy(&a->mem[off + 2], "name", 4);
    term s = term_from_ref(off);
    CHECK_EQ(names_register(&vm, (const uint8_t *)&a->mem[off + 2], 4, a->id), 1);
    memset(&a->mem[off + 2], 'X', 4);                          /* clobber the source */
    heap_gc(&vm, a, 0, &s, 1);                                 /* heap is rebuilt */
    term p;
    CHECK(names_whereis(&vm, NAME("name"), &p) && p == a->id);
    CHECK(!names_whereis(&vm, NAME("XXXX"), &p));
    actor_exit(&vm, a, R_NORMAL, T_EMPTYSTR);
    CHECK_EQ(va_used_bytes(), base);
}

/* An allocation failure reports -1 and leaves the table unchanged. */
TEST(names_oom) {
    uint32_t w[IMG_WORDS]; Image img; Vm vm;
    setup(w, &img, &vm);
    Actor *a = actor_new(&vm, 32);
    size_t base = va_used_bytes();
    va_set_limit(base);
    CHECK_EQ(names_register(&vm, NAME("x"), a->id), -1);
    va_set_limit(0);
    CHECK(vm.names == NULL);
    CHECK_EQ(va_used_bytes(), base);
    actor_exit(&vm, a, R_NORMAL, T_EMPTYSTR);
}
