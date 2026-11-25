/* The module table (module.h): boot as module 0, a second module in the
 * same interpreter, module-tagged const refs / closures / structs and what
 * happens to them once their module is gone. */
#define _POSIX_C_SOURCE 200112L   /* setenv, mkstemp */
#include "test.h"
#include "actor.h"
#include "heap.h"
#include "msg.h"
#include "alloc.h"
#include "bytes.h"
#include "image.h"
#include "interp.h"
#include "module.h"
#include "opcodes.h"
#include "platform.h"
#include "signals.h"
#include "term.h"
#include "timer.h"
#include "caps_unix.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

extern void (*platform_unix_err_hook)(const char *, size_t);
static char err_buf[1024]; static size_t err_len;
static void capture_err(const char *s, size_t n) {
    if (err_len + n < sizeof err_buf) { memcpy(err_buf + err_len, s, n); err_len += n; err_buf[err_len] = 0; }
}
static void capture_start(void) { err_len = 0; err_buf[0] = 0; platform_unix_err_hook = capture_err; }
static void capture_stop(void) { platform_unix_err_hook = NULL; }

/* ---- a tiny image builder ---- */

typedef struct TFunc {
    uint16_t nparams, nregs;
    uint32_t name;            /* const offset */
    const uint32_t *code;
    uint32_t ncode;
} TFunc;

#define TYPES (TYPE_RESERVED_COUNT + 1u)   /* the reserved types + one module type */
#define T_MOD TYPE_RESERVED_COUNT          /* the module's own type */
#define MAXW 1024u

/* A 4-byte K_STRING (3 words) at c[off]. */
static void put_str(uint32_t *c, uint32_t off, const char *s) {
    c[off] = hdr_make(3, K_STRING);
    c[off + 1] = (uint32_t)strlen(s);
    c[off + 2] = 0;
    memcpy(&c[off + 2], s, strlen(s));
}

/* Lays out header, funcs, types (every reserved type has 0 fields but
 * Options (1) and code.Module (2); the module type T_MOD has 1),
 * consts, code, the
 * const map, requires and the exports. Returns the length in words. */
static uint32_t build(uint32_t *w, const TFunc *f, uint32_t nf, const uint32_t *c, uint32_t nc,
                      const uint32_t *exp, uint32_t nexp, uint32_t requires) {
    memset(w, 0, MAXW * 4u);
    uint32_t ncode = 0;
    for (uint32_t i = 0; i < nf; i++) ncode += f[i].ncode;
    w[0] = IMAGE_MAGIC; w[1] = IMAGE_VERSION; w[2] = nf; w[3] = TYPES;
    w[4] = nc; w[5] = ncode; w[6] = 0; w[7] = 0;
    uint32_t p = 8, off = 0;
    for (uint32_t i = 0; i < nf; i++) {
        w[p++] = off;
        w[p++] = f[i].nparams | ((uint32_t)f[i].nregs << 16);
        w[p++] = f[i].name;
        off += f[i].ncode;
    }
    for (uint32_t t = 0; t < TYPES; t++) {
        w[p++] = t == TYPE_CODE_MODULE ? 2u : (t == TYPE_OPTIONS || t == T_MOD) ? 1u : 0u;
        w[p++] = 0;
    }
    memcpy(&w[p], c, nc * 4u);
    p += nc;
    for (uint32_t i = 0; i < nf; i++) {
        memcpy(&w[p], f[i].code, f[i].ncode * 4u);
        p += f[i].ncode;
    }
    uint32_t n = img_seal(w);                /* map + requires 0 + n_exports 0 */
    w[n - 2] = requires;
    w[n - 1] = nexp;
    memcpy(&w[n], exp, nexp * 2u * 4u);
    return n + nexp * 2u;
}

/* ---- module A: the boot image (module 0) ----
 * consts: 0 "main", 3 "f", 6 "AAAA". Every function but main takes one
 * parameter x in r0 and has 4 registers. */
#define A_FUNCS 12
static const uint32_t a_main[] = {            /* f0 main */
    OP_MAKE(OP_LOADK, 0, 0, 0), 0x6Au,         /* term_from_const(6) */
    OP_MAKE(OP_RET, 0, 0, 0),
};
static const uint32_t a_len[] = {             /* f1: len(x) */
    OP_MAKE(OP_BUILTIN, 1, 0, 1), B_LEN,
    OP_MAKE(OP_RET, 1, 0, 0),
};
static const uint32_t a_callv[] = {           /* f2: x() */
    OP_MAKE(OP_CALLV, 1, 0, 0),
    OP_MAKE(OP_RET, 1, 0, 0),
};
static const uint32_t a_field[] = {           /* f3: x.0 */
    OP_MAKE(OP_GETFIELD, 1, 0, 0),
    OP_MAKE(OP_RET, 1, 0, 0),
};
static const uint32_t a_spawn[] = {           /* f6: Spawn(x) */
    OP_MAKE(OP_BUILTIN, 1, 0, 1), B_SPAWN,
    OP_MAKE(OP_RET, 1, 0, 0),
};
static const uint32_t a_print[] = {           /* f7: println(x) */
    OP_MAKE(OP_BUILTIN, 1, 0, 1), B_PRINTLN,
    OP_MAKE(OP_RET, 1, 0, 0),
};
/* f8: LOADK of an image const ref whose offset is past the 17-bit range
 * (its top bits are the module tag of slot 1). */
static const uint32_t a_badk[] = {
    OP_MAKE(OP_LOADK, 1, 0, 0), 0x6Au | (1u << MOD_TAG_SHIFT),
    OP_MAKE(OP_RET, 1, 0, 0),
};

/* f4 / f5: TYPEIS x, ty; divide by zero unless it matched. Code offsets
 * are absolute, so the jump target depends on where the function lands. */
static void typeis_fn(uint32_t *code, uint32_t at, uint32_t ty) {
    code[0] = OP_MAKE(OP_TYPEIS, 1, 0, 0); code[1] = ty;
    code[2] = OP_MAKE(OP_JMPT, 1, 0, 0); code[3] = at + 8u;
    code[4] = OP_MAKE(OP_LOADK, 2, 0, 0); code[5] = term_from_int(0);
    code[6] = OP_MAKE(OP_DIV, 3, 2, 2);
    code[7] = OP_MAKE(OP_RET, 0, 0, 0);
    code[8] = OP_MAKE(OP_RET, 0, 0, 0);
}

static uint32_t a_is5[9], a_is34[9];

/* f9 / f10: x == x, x != x. */
static const uint32_t a_eq[] = { OP_MAKE(OP_EQ, 1, 0, 0), OP_MAKE(OP_RET, 1, 0, 0) };
static const uint32_t a_ne[] = { OP_MAKE(OP_NE, 1, 0, 0), OP_MAKE(OP_RET, 1, 0, 0) };
/* f11: Send(r2, r3) (the test sets both registers). */
static const uint32_t a_send[] = { OP_MAKE(OP_BUILTIN, 1, 2, 2), B_SEND, OP_MAKE(OP_RET, 1, 0, 0) };

enum { F_MAIN, F_LEN, F_CALLV, F_FIELD, F_IS5, F_IS34, F_SPAWN, F_PRINT, F_BADK, F_EQ, F_NE, F_SEND };

static uint32_t build_a(uint32_t *w, bool with_main_export, uint32_t requires) {
    uint32_t c[9];
    put_str(c, 0, "main"); put_str(c, 3, "f"); put_str(c, 6, "AAAA");
    TFunc f[A_FUNCS] = {
        { 0, 4, 0, a_main, 3 }, { 1, 4, 3, a_len, 3 }, { 1, 4, 3, a_callv, 2 },
        { 1, 4, 3, a_field, 2 }, { 1, 4, 3, a_is5, 9 }, { 1, 4, 3, a_is34, 9 },
        { 1, 4, 3, a_spawn, 3 }, { 1, 4, 3, a_print, 3 }, { 1, 4, 3, a_badk, 3 },
        { 1, 4, 3, a_eq, 2 }, { 1, 4, 3, a_ne, 2 }, { 1, 4, 3, a_send, 3 },
    };
    uint32_t at = 3 + 3 + 2 + 2;             /* code offset of f4 */
    typeis_fn(a_is5, at, TYPE_OPTIONS);
    typeis_fn(a_is34, at + 9, T_MOD);
    uint32_t exp[2] = { 0, F_MAIN };
    return build(w, f, A_FUNCS, c, 9, exp, with_main_export ? 1u : 0u, requires);
}

/* ---- module B ----
 * consts: 0 "Main", 3 "get", 6 "BBBB" (same offsets as A's, other text),
 * 9 "Bad", then the padding (12: a K_INT64 when pad is 3).
 * f0 Main(pid): s := "BBBB"; st := T{s}; cl := func() { return st.0 };
 *   send(pid, cl()); send(pid, Options{7})
 * f1 get(st): return st.0
 * f2 Bad(pid): send(pid, T{"BBBB"}) (exported as "Bad": a module struct
 *   to an actor of another module is BadMsg for B's actor) */
static const uint32_t b_main[] = {
    OP_MAKE(OP_LOADK, 1, 0, 0), 0x6Au,
    OP_MAKE(OP_NEWSTRUCT, 2, 1, 1), T_MOD,
    OP_MAKE(OP_MKCLOSURE, 3, 2, 1), 1,
    OP_MAKE(OP_CALLV, 4, 3, 0),
    OP_MAKE(OP_MOVE, 6, 0, 0),
    OP_MAKE(OP_MOVE, 7, 4, 0),
    OP_MAKE(OP_BUILTIN, 5, 6, 2), B_SEND,
    OP_MAKE(OP_LOADK, 1, 0, 0), 15u,           /* term_from_int(7) */
    OP_MAKE(OP_NEWSTRUCT, 7, 1, 1), TYPE_OPTIONS,
    OP_MAKE(OP_BUILTIN, 5, 6, 2), B_SEND,
    OP_MAKE(OP_RET, 0, 0, 0),
};
static const uint32_t b_get[] = {
    OP_MAKE(OP_GETFIELD, 1, 0, 0),
    OP_MAKE(OP_RET, 1, 0, 0),
};
static const uint32_t b_bad[] = {
    OP_MAKE(OP_LOADK, 1, 0, 0), 0x6Au,
    OP_MAKE(OP_MOVE, 2, 0, 0),
    OP_MAKE(OP_NEWSTRUCT, 3, 1, 1), T_MOD,
    OP_MAKE(OP_BUILTIN, 1, 2, 2), B_SEND,
    OP_MAKE(OP_RET, 0, 0, 0),
};

/* B with the given requires word and pad extra const words (K_INTs, an
 * even number; 3: one K_INT64, B_INT64) to make it bigger. */
#define B_PAD 12u
#define B_INT64 0x123456789LL
static uint32_t build_b_with(uint32_t *w, uint32_t requires, uint32_t pad) {
    static uint32_t c[B_PAD + MAXW / 2];
    put_str(c, 0, "Main"); put_str(c, 3, "get"); put_str(c, 6, "BBBB"); put_str(c, 9, "Bad");
    for (uint32_t i = 0; i + 1 < pad; i += 2) { c[B_PAD + i] = hdr_make(2, K_INT); c[B_PAD + 1 + i] = i; }
    if (pad == 3) {
        c[B_PAD] = hdr_make(3, K_INT64);
        c[B_PAD + 1] = (uint32_t)B_INT64;
        c[B_PAD + 2] = (uint32_t)(B_INT64 >> 32);
    }
    TFunc f[3] = { { 1, 8, 0, b_main, sizeof b_main / 4 }, { 1, 2, 3, b_get, 2 },
                   { 1, 4, 9, b_bad, sizeof b_bad / 4 } };
    uint32_t exp[4] = { 0, 0, 9, 2 };
    return build(w, f, 3, c, B_PAD + pad, exp, 2, requires);
}

static uint32_t build_b(uint32_t *w) { return build_b_with(w, 0, 0); }

/* ---- fixtures ---- */

typedef struct Fx {
    uint32_t a[MAXW], b[MAXW];
    Image img;
    Vm vm;
} Fx;

static Fx *fx_new(void) {
    Fx *f = calloc(1, sizeof *f);
    va_reset();
    CHECK_EQ(image_load(&f->img, f->a, build_a(f->a, true, 0)), IMG_OK);
    CHECK_EQ(vm_init(&f->vm, &f->img, 16), 0);
    return f;
}

static void run_queue(Vm *vm) {
    Actor *a;
    while ((a = runq_pop(vm)) != NULL) {
        int r = interp_run(vm, a);
        if (r == RUN_YIELD) runq_push(vm, a);
        else if (r == RUN_WAIT) a->state = A_WAITING;
    }
}

/* Puts image b into an off-heap Bytes pinned on owner and loads it, owner
 * becoming its holder. Returns the handle. */
static uint32_t b_pad;   /* build_b_with's pad for load_b */
static uint32_t load_b(Fx *f, Actor *owner) {
    uint32_t n = build_b_with(f->b, 0, b_pad);
    uint8_t *data;
    term h = bytes_new(&f->vm, owner, n * 4u, &data);
    CHECK(h != T_NIL);
    memcpy(data, f->b, n * 4u);
    uint32_t handle = 0;
    CHECK_EQ(module_load(&f->vm, owner, h, &handle), CODE_OK);
    bytes_release(&f->vm, owner, h);         /* only the module keeps it now */
    return handle;
}

/* An actor of module 0 about to run A's function fn with x in r0. */
static Actor *a_actor(Fx *f, uint32_t fn, term x) {
    Actor *a = actor_new(&f->vm, 64);
    CHECK(a != NULL);
    CHECK(heap_push_frame(&f->vm, a, 4, -1));
    a->mem[a->fp] = x;
    a->pc = f->img.code + image_func_code_off(&f->img, fn);
    return a;
}

/* Runs a to its exit; returns what it printed on stderr. */
static const char *run_to_exit(Fx *f, Actor *a) {
    capture_start();
    CHECK_EQ(interp_run(&f->vm, a), RUN_EXIT);
    capture_stop();
    return err_buf;
}

/* A heap object [hdr][word1][fields] in a's heap. */
static term plant(Actor *a, uint32_t kind, uint32_t word1, uint32_t nfields, term field) {
    uint32_t off = heap_alloc(NULL, a, 2u + nfields, NULL, 0);
    CHECK(off != UINT32_MAX);
    a->mem[off] = hdr_make(2u + nfields, kind);
    a->mem[off + 1] = word1;
    for (uint32_t i = 0; i < nfields; i++) a->mem[off + 2 + i] = field;
    return term_from_ref(off);
}

/* ---- tests ---- */

TEST(boot_is_module0) {
    static uint32_t w[MAXW];
    Vm vm;
    va_reset();
    uint32_t n = build_a(w, true, 0);
    CHECK_EQ(vm_boot(&vm, w, n, 16), VM_BOOT_OK);
    CHECK(vm.mods[0] != NULL);
    CHECK(module_resolve(&vm, 0, 0) == &vm.mods[0]->img);
    CHECK_EQ(module_mem_used(&vm), 0u);
    CHECK_EQ(vm_run(&vm), 0);
    CHECK(vm.mods[0] != NULL);               /* held by the VM: never unloaded */
    CHECK_EQ(vm.live, 0u);
}

TEST(no_main_refused) {
    static uint32_t w[MAXW];
    Vm vm;
    va_reset();
    uint32_t n = build_a(w, false, 0);
    CHECK_EQ(vm_boot(&vm, w, n, 16), VM_BOOT_OK);
    capture_start();
    int rc = vm_run(&vm);
    capture_stop();
    CHECK(rc != 0);
    CHECK(strcmp(err_buf, "gogo: image has no main\n") == 0);
    CHECK_EQ(vm.live, 0u);
}

/* R1: the unified boot keeps the firmware capability check. */
TEST(boot_caps_refused) {
    static uint32_t w[MAXW];
    Vm vm;
    va_reset();
    uint32_t n = build_a(w, true, CAP_NET);
    platform_unix_set_caps(0);
    capture_start();
    int rc = vm_boot(&vm, w, n, 16);
    capture_stop();
    platform_unix_set_caps(CAP_NET | CAP_KBD | CAP_POWER | CAP_SD);
    CHECK_EQ(rc, VM_BOOT_REFUSED);
    CHECK(strcmp(err_buf, "gogo: image needs net; firmware lacks net\n") == 0);
}

TEST(boot_bad_image) {
    static uint32_t w[MAXW];
    Vm vm;
    va_reset();
    uint32_t n = build_a(w, true, 0);
    w[0] ^= 1u;
    CHECK_EQ(vm_boot(&vm, w, n, 16), IMG_ERR_MAGIC);
    CHECK(strcmp(image_err_str(IMG_ERR_MAGIC), "bad magic") == 0);
}

/* B runs in the same interpreter: its LOADK, struct, closure and CALLV use
 * B's image although A has other constants at the same offsets. The
 * reserved struct it builds (Options) is A's Options. What it sends the
 * host (module 0) crosses modules: its const string arrives as a heap
 * copy (Task 5), so the host keeps it after B unloads. */
TEST(two_modules_same_interpreter) {
    Fx *f = fx_new();
    Vm *vm = &f->vm;
    Actor *h = actor_new(vm, 64);            /* the host: module 0 */
    CHECK(heap_push_frame(vm, h, 4, -1));
    uint32_t handle = load_b(f, h);
    uint32_t slot = handle >> 16;
    CHECK(slot != 0);
    CHECK(module_mem_used(vm) > 0u);
    term pid;
    CHECK_EQ(module_spawn(vm, h, handle, (const uint8_t *)"Main", 4, h->id, 0, &pid), CODE_OK);
    CHECK_EQ(vm->mods[slot]->live, 1);
    capture_start();
    run_queue(vm);
    capture_stop();
    CHECK_EQ(err_len, 0u);                   /* B's actor exited normally */
    CHECK_EQ(vm->mods[slot]->live, 0);
    CHECK(vm->mods[slot] != NULL);           /* h still holds it */

    term s;
    CHECK(msg_receive(vm, h, &s));
    CHECK(term_is_ref(s));                   /* a heap copy, not B's const */
    h->mem[h->fp + 1] = s;
    /* the same offset untagged is A's */
    const uint8_t *p; uint32_t len;
    CHECK(name_bytes(vm, h, term_from_const(6), &p, &len));
    CHECK(len == 4 && memcmp(p, "AAAA", 4) == 0);

    /* Options from B matches A's TYPEIS Options: reserved types are global. */
    CHECK(msg_receive(vm, h, &s));
    h->mem[h->fp] = s;
    module_release(vm, h, handle);           /* B unloads: nothing left of it */
    CHECK(vm->mods[slot] == NULL);
    CHECK_EQ(module_mem_used(vm), 0u);
    CHECK(module_resolve(vm, slot, handle & 0xFFFFu) == NULL);
    CHECK_EQ(bytes_table_used(), 0u);
    CHECK(name_bytes(vm, h, h->mem[h->fp + 1], &p, &len));
    CHECK(len == 4 && memcmp(p, "BBBB", 4) == 0);
    h->pc = f->img.code + image_func_code_off(&f->img, F_IS5);
    CHECK(strcmp(run_to_exit(f, h), "") == 0);
    free(f);
}

/* A module's own struct type is distinct from another module's type with
 * the same index while both are loaded. */
TEST(module_types_distinct) {
    Fx *f = fx_new();
    Vm *vm = &f->vm;
    Actor *h = actor_new(vm, 64);
    uint32_t handle = load_b(f, h);
    uint32_t tag = module_tag(vm, handle >> 16);
    Actor *x = a_actor(f, F_IS34, T_NIL);
    x->mem[x->fp] = plant(x, K_STRUCT, T_MOD | tag, 1, T_NIL);
    CHECK(strstr(run_to_exit(f, x), "crashed: divzero") != NULL);
    Actor *y = a_actor(f, F_IS34, T_NIL);
    y->mem[y->fp] = plant(y, K_STRUCT, T_MOD, 1, T_NIL);
    CHECK(strcmp(run_to_exit(f, y), "") == 0);
    actor_exit(vm, h, R_NORMAL, T_EMPTYSTR);
    CHECK(vm->mods[handle >> 16] == NULL);
    free(f);
}

/* Loads B (held by a loader actor), returns the tag of its slot and
 * unloads it by letting the loader exit. */
static uint32_t load_and_unload_b(Fx *f, uint32_t *slot) {
    Actor *l = actor_new(&f->vm, 64);
    uint32_t handle = load_b(f, l);
    *slot = handle >> 16;
    uint32_t tag = module_tag(&f->vm, *slot);
    CHECK(tag != 0u);
    CHECK(module_resolve(&f->vm, *slot, handle & 0xFFFFu) != NULL);
    actor_exit(&f->vm, l, R_NORMAL, T_EMPTYSTR);
    CHECK(f->vm.mods[*slot] == NULL);
    CHECK(module_resolve(&f->vm, *slot, handle & 0xFFFFu) == NULL);
    return tag;
}

/* Review Focus 5: a const ref of B planted in an A actor; B unloads; using
 * the ref is badcode for that actor; another A actor is unaffected. */
TEST(stale_const_ref_badcode) {
    Fx *f = fx_new();
    uint32_t slot;
    uint32_t tag = load_and_unload_b(f, &slot);
    term stale = term_from_const(6) | tag;
    Actor *v = a_actor(f, F_LEN, stale);
    Actor *ok = a_actor(f, F_LEN, term_from_const(6));
    Actor *p = a_actor(f, F_PRINT, stale);
    CHECK(strstr(run_to_exit(f, v), "crashed: badcode in f") != NULL);
    CHECK(strcmp(run_to_exit(f, ok), "") == 0);
    CHECK(strstr(run_to_exit(f, p), "crashed: badcode") != NULL);
    const uint8_t *s; uint32_t len;
    CHECK(!name_bytes(&f->vm, NULL, stale, &s, &len));
    CHECK_EQ(f->vm.live, 0u);
    free(f);
}

TEST(stale_closure_badcode) {
    Fx *f = fx_new();
    uint32_t slot;
    uint32_t tag = load_and_unload_b(f, &slot);
    Actor *v = a_actor(f, F_CALLV, T_NIL);
    v->mem[v->fp] = plant(v, K_CLOSURE, 1u | tag, 1, T_NIL);
    CHECK(strstr(run_to_exit(f, v), "crashed: badcode") != NULL);
    Actor *s = a_actor(f, F_SPAWN, T_NIL);
    s->mem[s->fp] = plant(s, K_CLOSURE, 1u | tag, 1, T_NIL);
    CHECK(strstr(run_to_exit(f, s), "crashed: badcode") != NULL);
    /* an A closure (tag 0) of A's f3 (1 param: its capture) still runs */
    Actor *ok = a_actor(f, F_CALLV, T_NIL);
    term st = plant(ok, K_STRUCT, T_MOD, 1, term_from_int(5));
    ok->mem[ok->fp] = plant(ok, K_CLOSURE, F_FIELD, 1, st);
    CHECK(strcmp(run_to_exit(f, ok), "") == 0);
    CHECK_EQ(f->vm.live, 0u);
    free(f);
}

TEST(stale_struct_badcode) {
    Fx *f = fx_new();
    uint32_t slot;
    uint32_t tag = load_and_unload_b(f, &slot);
    Actor *v = a_actor(f, F_FIELD, T_NIL);
    v->mem[v->fp] = plant(v, K_STRUCT, T_MOD | tag, 1, term_from_int(1));
    CHECK(strstr(run_to_exit(f, v), "crashed: badcode") != NULL);
    Actor *t = a_actor(f, F_IS34, T_NIL);
    t->mem[t->fp] = plant(t, K_STRUCT, T_MOD | tag, 1, term_from_int(1));
    CHECK(strstr(run_to_exit(f, t), "crashed: badcode") != NULL);
    /* a reserved struct is global: no module, never stale */
    Actor *r = a_actor(f, F_IS5, T_NIL);
    r->mem[r->fp] = plant(r, K_STRUCT, TYPE_OPTIONS, 1, term_from_int(1));
    CHECK(strcmp(run_to_exit(f, r), "") == 0);
    free(f);
}

/* A new module in a reused slot gets a new generation: refs to the old one
 * stay stale. (Slots go round-robin: load until one lands there again.) */
TEST(slot_reuse_new_generation) {
    Fx *f = fx_new();
    uint32_t slot;
    uint32_t tag = load_and_unload_b(f, &slot);
    Actor *h;
    uint32_t handle;
    for (;;) {                               /* at most one lap of the slots */
        h = actor_new(&f->vm, 64);
        handle = load_b(f, h);
        if (handle >> 16 == slot) break;
        actor_exit(&f->vm, h, R_NORMAL, T_EMPTYSTR);
    }
    CHECK(module_tag(&f->vm, slot) != tag);
    Actor *v = a_actor(f, F_LEN, term_from_const(6) | tag);
    CHECK(strstr(run_to_exit(f, v), "crashed: badcode") != NULL);
    Actor *ok = a_actor(f, F_LEN, term_from_const(6) | module_tag(&f->vm, slot));
    CHECK(strcmp(run_to_exit(f, ok), "") == 0);
    actor_exit(&f->vm, h, R_NORMAL, T_EMPTYSTR);
    free(f);
}

TEST(reserved_types_global) {
    Fx *f = fx_new();
    Vm *vm = &f->vm;
    Actor *h = actor_new(vm, 64);
    CHECK(heap_push_frame(vm, h, 4, -1));
    uint32_t handle = load_b(f, h);
    term pid;
    CHECK_EQ(module_spawn(vm, h, handle, (const uint8_t *)"Main", 4, h->id, 0, &pid), CODE_OK);
    run_queue(vm);
    term s;
    CHECK(msg_receive(vm, h, &s));           /* the string */
    CHECK(msg_receive(vm, h, &s));           /* Options{7}, built in B */
    h->mem[h->fp] = s;
    h->pc = f->img.code + image_func_code_off(&f->img, F_IS5);
    CHECK(strcmp(run_to_exit(f, h), "") == 0);
    free(f);
}

TEST(spawn_errors) {
    Fx *f = fx_new();
    Vm *vm = &f->vm;
    Actor *h = actor_new(vm, 64);
    uint32_t handle = load_b(f, h);
    term pid;
    CHECK_EQ(module_spawn(vm, h, handle, (const uint8_t *)"Nope", 4, T_NIL, 0, &pid), CODE_NOT_FOUND);
    CHECK_EQ(module_spawn(vm, h, handle + 1u, (const uint8_t *)"Main", 4, T_NIL, 0, &pid), CODE_NOT_FOUND);
    CHECK_EQ(module_spawn(vm, h, 7u << 16, (const uint8_t *)"Main", 4, T_NIL, 0, &pid), CODE_NOT_FOUND);
    actor_exit(vm, h, R_NORMAL, T_EMPTYSTR);
    CHECK_EQ(module_spawn(vm, NULL, handle, (const uint8_t *)"Main", 4, T_NIL, 0, &pid), CODE_NOT_FOUND);
    free(f);
}

TEST(load_bad_image) {
    Fx *f = fx_new();
    Vm *vm = &f->vm;
    Actor *h = actor_new(vm, 64);
    uint8_t *data;
    term b = bytes_new(vm, h, 64, &data);
    memset(data, 0xAB, 64);
    uint32_t handle;
    CHECK_EQ(module_load(vm, h, b, &handle), CODE_BAD_IMAGE);
    CHECK_EQ(module_load(vm, h, term_from_int(3), &handle), CODE_BAD_ARG);
    CHECK_EQ(module_mem_used(vm), 0u);
    actor_exit(vm, h, R_NORMAL, T_EMPTYSTR);
    free(f);
}

/* The const offset range shrank to 17 bits: a bigger const area is
 * IMG_ERR_BOUNDS at load; an image const ref with bits above the range is
 * badcode when LOADK runs it. */
TEST(offset_limit_checked) {
    uint32_t nc = CONST_MAX_WORDS + 3u;      /* "main" + K_INTs */
    uint32_t total = 8u + 3u + nc + 1u + (nc + 31u) / 32u + 2u;
    uint32_t *w = calloc(total, 4u);
    w[0] = IMAGE_MAGIC; w[1] = IMAGE_VERSION; w[2] = 1; w[3] = 0;
    w[4] = nc; w[5] = 1; w[6] = 0; w[7] = 0;
    w[8] = 0; w[9] = 1u << 16; w[10] = 0;
    uint32_t *c = w + 11;
    put_str(c, 0, "main");
    for (uint32_t off = 3; off < nc; off += 2) { c[off] = hdr_make(2, K_INT); c[off + 1] = off; }
    c[nc] = OP_MAKE(OP_RET, 0, 0, 0);
    Image img;
    CHECK_EQ(image_load(&img, w, img_seal(w)), IMG_ERR_BOUNDS);
    /* exactly the limit loads: "main", K_INTs, then one K_INT64 ending at
     * word CONST_MAX_WORDS - 1 (the largest offset a const ref can name) */
    nc = CONST_MAX_WORDS;
    w[4] = nc;
    c[nc - 3] = hdr_make(3, K_INT64); c[nc - 2] = 1; c[nc - 1] = 2;
    c[nc] = OP_MAKE(OP_RET, 0, 0, 0);
    CHECK_EQ(image_load(&img, w, img_seal(w)), IMG_OK);
    CHECK(image_const_start(&img, CONST_MAX_WORDS - 3u));
    free(w);

    Fx *f = fx_new();
    Actor *v = a_actor(f, F_BADK, T_NIL);
    CHECK(strstr(run_to_exit(f, v), "crashed: badcode") != NULL);
    free(f);
}

/* Struct type indexes are bounded too (16 bits: TYPEIS's basic types are
 * 0x10000 up). An image with 0x10001 well-formed types is refused. */
TEST(type_limit_checked) {
    uint32_t nt = TYPE_MAX + 1u, total = 8u + 3u + nt * 2u + 3u + 1u + 1u + 2u;
    uint32_t *w = calloc(total, 4u);
    w[0] = IMAGE_MAGIC; w[1] = IMAGE_VERSION; w[2] = 1; w[3] = nt;
    w[4] = 3; w[5] = 1; w[6] = 0; w[7] = 0;
    w[8] = 0; w[9] = 1u << 16; w[10] = 0;    /* main; every type: 0 fields, name "main" */
    uint32_t *c = w + 11 + nt * 2u;
    put_str(c, 0, "main");
    c[3] = OP_MAKE(OP_RET, 0, 0, 0);
    Image img;
    CHECK_EQ(image_load(&img, w, img_seal(w)), IMG_ERR_BOUNDS);
    w[3] = TYPE_MAX;                         /* the limit itself loads */
    c = w + 11 + TYPE_MAX * 2u;
    put_str(c, 0, "main");
    c[3] = OP_MAKE(OP_RET, 0, 0, 0);
    CHECK_EQ(image_load(&img, w, img_seal(w)), IMG_OK);
    free(w);
}

/* I1: comparisons, type tests and prints of a stale value are badcode
 * (not "unequal", "not this type" or "?"): const ref, closure and struct
 * of an unloaded module, through EQ, NE and TYPEIS; and a stale const
 * nested in a printed struct. */
TEST(stale_compare_typeis_print_badcode) {
    Fx *f = fx_new();
    uint32_t slot;
    uint32_t tag = load_and_unload_b(f, &slot);
    static const uint32_t fns[] = { F_EQ, F_NE, F_IS5 };
    for (int kind = 0; kind < 3; kind++) {
        for (int i = 0; i < 3; i++) {
            Actor *v = a_actor(f, fns[i], T_NIL);
            term x = kind == 0 ? term_from_const(6) | tag
                   : kind == 1 ? plant(v, K_CLOSURE, 1u | tag, 1, T_NIL)
                               : plant(v, K_STRUCT, T_MOD | tag, 1, term_from_int(1));
            v->mem[v->fp] = x;
            const char *err = run_to_exit(f, v);
            CHECK(strstr(err, "crashed: badcode") != NULL);
        }
    }
    /* the live counterparts compare and test normally */
    Actor *ok = a_actor(f, F_EQ, term_from_const(6));
    CHECK(strcmp(run_to_exit(f, ok), "") == 0);
    Actor *ok2 = a_actor(f, F_NE, T_NIL);
    ok2->mem[ok2->fp] = plant(ok2, K_STRUCT, T_MOD, 1, term_from_int(1));
    CHECK(strcmp(run_to_exit(f, ok2), "") == 0);
    /* M1: a stale const inside a printed struct */
    Actor *p = a_actor(f, F_PRINT, T_NIL);
    p->mem[p->fp] = plant(p, K_STRUCT, TYPE_OPTIONS, 1, term_from_const(6) | tag);
    CHECK(strstr(run_to_exit(f, p), "crashed: badcode") != NULL);
    CHECK_EQ(f->vm.live, 0u);
    free(f);
}

/* Loads B held by a fresh actor and unloads it again by letting that
 * actor exit; returns the slot it went to. */
static uint32_t cycle_b(Fx *f) {
    Actor *l = actor_new(&f->vm, 64);
    uint32_t slot = load_b(f, l) >> 16;
    actor_exit(&f->vm, l, R_NORMAL, T_EMPTYSTR);
    CHECK(f->vm.mods[slot] == NULL);
    return slot;
}

/* I2 / R3: slots are picked round-robin, so 300 load/unload cycles spread
 * over the 7 free slots (about 43 loads each) and a ref kept from the first
 * module stays badcode: its slot's 8-bit tag generation is far from
 * wrapping (that takes 256 loads into the one slot, ~1792 cycles). */
TEST(stale_ref_survives_300_reloads) {
    Fx *f = fx_new();
    uint32_t slot;
    uint32_t tag = load_and_unload_b(f, &slot);
    uint32_t used[GOGO_MAX_MODULES] = {0};
    for (int i = 0; i < 300; i++) used[cycle_b(f)]++;
    CHECK_EQ(used[0], 0u);                   /* the boot image's */
    for (uint32_t s = 1; s < GOGO_MAX_MODULES; s++) {
        CHECK(used[s] >= 300u / (GOGO_MAX_MODULES - 1u) - 1u);
        CHECK(f->vm.mod_gen[s] < 256u);
    }
    CHECK(module_tag(&f->vm, slot) != tag);
    Actor *v = a_actor(f, F_LEN, term_from_const(6) | tag);
    CHECK(strstr(run_to_exit(f, v), "crashed: badcode") != NULL);
    free(f);
}

/* The residual bound, exactly: with every other slot taken, all loads go
 * to one slot; a ref from it is stale for the next 255 loads there and
 * aliases (resolves to the newest module) at the 256th. */
TEST(alias_bound_one_slot) {
    Fx *f = fx_new();
    Actor *holders[GOGO_MAX_MODULES];
    uint32_t free_slot = 0;
    for (uint32_t s = 1; s < GOGO_MAX_MODULES; s++) {
        holders[s] = actor_new(&f->vm, 64);
        free_slot = load_b(f, holders[s]) >> 16;
    }
    actor_exit(&f->vm, holders[free_slot], R_NORMAL, T_EMPTYSTR);   /* the one free slot */
    uint32_t tag = module_tag(&f->vm, free_slot);
    for (int i = 0; i < 255; i++) CHECK_EQ(cycle_b(f), free_slot);
    CHECK(module_tag(&f->vm, free_slot) != tag);
    Actor *v = a_actor(f, F_LEN, term_from_const(6) | tag);
    CHECK(strstr(run_to_exit(f, v), "crashed: badcode") != NULL);
    CHECK_EQ(cycle_b(f), free_slot);         /* the 256th */
    CHECK_EQ(module_tag(&f->vm, free_slot), tag);
    for (uint32_t s = 1; s < GOGO_MAX_MODULES; s++)
        if (s != free_slot) actor_exit(&f->vm, holders[s], R_NORMAL, T_EMPTYSTR);
    free(f);
}

/* ModEntry derives its code size from the image (no words/n_words). */
TEST(module_code_words_counted) {
    Fx *f = fx_new();
    Actor *h = actor_new(&f->vm, 64);
    uint32_t n = build_b(f->b);
    uint32_t handle = load_b(f, h);
    CHECK_EQ(module_code_words(f->vm.mods[handle >> 16]), n);
    CHECK_EQ(module_mem_used(&f->vm), n * 4u);
    actor_exit(&f->vm, h, R_NORMAL, T_EMPTYSTR);
    CHECK_EQ(module_mem_used(&f->vm), 0u);
    free(f);
}

/* ---- Task 4: gogo/code (Load / Spawn / Hold / Release, the cap) ---- */

/* The PSRAM hook, overridden for this test binary: off (NULL, "no special
 * memory", the weak default's answer) unless plat_on. */
static bool plat_on;
static uint32_t plat_allocs, plat_frees;
static size_t plat_last;
static bool plat_misalign;
void *platform_code_alloc(size_t n) {
    if (!plat_on) return NULL;
    plat_allocs++;
    plat_last = n;
    char *p = malloc(n + 4u);
    return plat_misalign ? p + 1 : p;
}
void platform_code_free(void *p, size_t n) {
    CHECK_EQ(n, plat_last);
    plat_frees++;
    free(plat_misalign ? (char *)p - 1 : p);
}

/* An off-heap Bytes of image w (n words) pinned on owner. */
static term offheap(Fx *f, Actor *owner, const uint32_t *w, uint32_t n) {
    uint8_t *data;
    term h = bytes_new(&f->vm, owner, n * 4u, &data);
    CHECK(h != T_NIL);
    memcpy(data, w, n * 4u);
    return h;
}

/* A heap K_BYTES of image w (n words) in a's heap. */
static term heapbytes(Actor *a, const uint32_t *w, uint32_t n) {
    uint32_t off = heap_alloc(NULL, a, 2u + n, NULL, 0);
    CHECK(off != UINT32_MAX);
    a->mem[off] = hdr_make(2u + n, K_BYTES);
    a->mem[off + 1] = n * 4u;
    memcpy(&a->mem[off + 2], w, n * 4u);
    return term_from_ref(off);
}

/* Runs B's Main(h) in module `handle` and checks h got "BBBB" from it. */
static void b_main_runs(Fx *f, Actor *h, uint32_t handle) {
    term pid, s;
    int rc = module_spawn(&f->vm, h, handle, (const uint8_t *)"Main", 4, h->id, 0, &pid);
    CHECK_EQ(rc, CODE_OK);
    if (rc != CODE_OK) return;
    capture_start();
    run_queue(&f->vm);
    capture_stop();
    CHECK_EQ(err_len, 0u);
    CHECK(msg_receive(&f->vm, h, &s));
    const uint8_t *p = NULL; uint32_t len = 0;
    CHECK(name_bytes(&f->vm, h, s, &p, &len));
    CHECK(len == 4 && p && memcmp(p, "BBBB", 4) == 0);
    while (msg_receive(&f->vm, h, &s)) {}
}

/* A host with a frame, for messages. */
static Actor *host(Fx *f) {
    Actor *h = actor_new(&f->vm, 256);
    CHECK(h != NULL);
    CHECK(heap_push_frame(&f->vm, h, 4, -1));
    return h;
}

/* Main(arg) of a loaded module runs: here from a heap K_BYTES, which is
 * copied once (the copy is the module's code and is counted). */
TEST(load_spawn_main_runs) {
    Fx *f = fx_new();
    Actor *h = host(f);
    uint32_t n = build_b(f->b), handle;
    size_t before = va_used_bytes();
    CHECK_EQ(module_load(&f->vm, h, heapbytes(h, f->b, n), &handle), CODE_OK);
    CHECK_EQ(module_mem_used(&f->vm), n * 4u);
    CHECK(va_used_bytes() > before);
    CHECK(f->vm.mods[handle >> 16]->bytes == T_NIL);      /* its own copy */
    h->hp = 0;                                             /* the heap bytes are gone */
    b_main_runs(f, h, handle);
    actor_exit(&f->vm, h, R_NORMAL, T_EMPTYSTR);
    CHECK_EQ(module_mem_used(&f->vm), 0u);
    CHECK_EQ(f->vm.live, 0u);
    free(f);
}

/* A program image (with main) loads like any module; its "main" export
 * starts like the boot image's. */
TEST(load_main_program_image) {
    Fx *f = fx_new();
    Actor *h = host(f);
    uint32_t n = build_a(f->b, true, 0), handle;
    term b = offheap(f, h, f->b, n);
    CHECK_EQ(module_load(&f->vm, h, b, &handle), CODE_OK);
    bytes_release(&f->vm, h, b);
    CHECK(handle >> 16 != 0);
    term pid;
    CHECK_EQ(module_spawn(&f->vm, h, handle, (const uint8_t *)"main", 4, T_NIL, 0, &pid), CODE_OK);
    CHECK_EQ(f->vm.mods[handle >> 16]->live, 1);
    capture_start();
    run_queue(&f->vm);
    capture_stop();
    CHECK_EQ(err_len, 0u);
    CHECK_EQ(f->vm.mods[handle >> 16]->live, 0);
    actor_exit(&f->vm, h, R_NORMAL, T_EMPTYSTR);
    CHECK(f->vm.mods[handle >> 16] == NULL);
    CHECK_EQ(bytes_table_used(), 0u);
    free(f);
}

TEST(entry_not_found) {
    Fx *f = fx_new();
    Actor *h = host(f);
    uint32_t handle = load_b(f, h);
    term pid = T_NIL;
    CHECK_EQ(module_spawn(&f->vm, h, handle, (const uint8_t *)"main", 4, T_NIL, 0, &pid), CODE_NOT_FOUND);
    CHECK_EQ(module_spawn(&f->vm, h, handle, (const uint8_t *)"main2", 5, T_NIL, 0, &pid), CODE_NOT_FOUND);
    CHECK_EQ(module_spawn(&f->vm, h, handle, (const uint8_t *)"get", 3, T_NIL, 0, &pid), CODE_NOT_FOUND);
    CHECK(pid == T_NIL);
    CHECK_EQ(f->vm.mods[handle >> 16]->live, 0);
    actor_exit(&f->vm, h, R_NORMAL, T_EMPTYSTR);
    free(f);
}

/* Review Focus 3: truncated, bit-flipped header, garbage exports, trailing
 * words: BadImage, nothing counted or kept (the bytes are not retained),
 * the loader unaffected. Also from unaligned bytes (validated as a copy,
 * which is freed again). */
TEST(bad_image) {
    Fx *f = fx_new();
    Actor *h = host(f);
    uint32_t good = load_b(f, h);                           /* one module already counted */
    uint32_t used = module_mem_used(&f->vm);
    static uint32_t w[MAXW + 1];
    uint32_t n = build_b(f->b);
    for (int c = 0; c < 9; c++) {
        memcpy(w, f->b, n * 4u);
        uint32_t len = n;
        switch (c) {
        case 0: len = n - 1; break;                        /* truncated */
        case 1: len = 7; break;                            /* not even a header */
        case 2: w[0] ^= 0x100u; break;                     /* magic */
        case 3: w[1] ^= 1u; break;                         /* version */
        case 4: w[2] ^= 0x40u; break;                      /* n_funcs */
        case 5: w[n - 3] = 1000; break;                    /* n_exports */
        case 6: w[n - 1] = 99; break;                      /* export's func index */
        case 7: w[n - 2] = 1; break;                       /* export name not a string start */
        default: w[n] = 0; len = n + 1; break;             /* a trailing word */
        }
        uint32_t handle = 12345;
        term b = offheap(f, h, w, len);
        size_t va0 = va_used_bytes();
        CHECK_EQ(module_load(&f->vm, h, b, &handle), CODE_BAD_IMAGE);
        CHECK_EQ(va_used_bytes(), va0);
        bytes_release(&f->vm, h, b);
        CHECK_EQ(handle, 12345u);
        /* the same, unaligned */
        uint8_t *data;
        term r = bytes_new(&f->vm, h, len * 4u + 1u, &data);
        memcpy(data + 1, w, len * 4u);
        term sl = bytes_sub(&f->vm, h, r, 1, len * 4u);
        CHECK(sl != T_NIL);
        va0 = va_used_bytes();
        CHECK_EQ(module_load(&f->vm, h, sl, &handle), CODE_BAD_IMAGE);
        CHECK_EQ(va_used_bytes(), va0);                     /* the copy is freed */
        bytes_release(&f->vm, h, r);
        heap_gc(&f->vm, h, 0, NULL, 0);
        CHECK_EQ(module_mem_used(&f->vm), used);
    }
    CHECK_EQ(bytes_table_used(), 1u);                       /* only good's: nothing retained */
    /* not Bytes at all */
    uint32_t handle;
    CHECK_EQ(module_load(&f->vm, h, term_from_int(3), &handle), CODE_BAD_ARG);
    CHECK_EQ(module_load(&f->vm, h, T_EMPTYBYTES, &handle), CODE_BAD_IMAGE);
    /* the loader still works */
    b_main_runs(f, h, good);
    actor_exit(&f->vm, h, R_NORMAL, T_EMPTYSTR);
    CHECK_EQ(module_mem_used(&f->vm), 0u);
    free(f);
}

/* Writes a $GOGO_CONFIG with `line` and boots A through vm_boot (which
 * reads code.mem); restores the environment. */
static void boot_with_config(Fx *f, const char *line) {
    char path[] = "/tmp/gogo-codemem-XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    FILE *fp = fdopen(fd, "w");
    fputs(line, fp);
    fclose(fp);
    const char *old = getenv("GOGO_CONFIG");
    char *saved = old ? strdup(old) : NULL;
    setenv("GOGO_CONFIG", path, 1);
    va_reset();
    uint32_t n = build_a(f->a, true, 0);
    capture_start();
    CHECK_EQ(vm_boot(&f->vm, f->a, n, 16), VM_BOOT_OK);
    capture_stop();
    f->img = f->vm.mods[0]->img;
    remove(path);
    if (saved) { setenv("GOGO_CONFIG", saved, 1); free(saved); }
    else unsetenv("GOGO_CONFIG");
}

/* code.mem = 1024 and a 2 KB module: NoMemory, nothing kept; a small one
 * still fits; K / M suffixes; a bad value says so and keeps the default. */
TEST(cap_exceeded) {
    Fx *f = calloc(1, sizeof *f);
    boot_with_config(f, "code.mem=1024\n");
    CHECK_EQ(f->vm.mod_cap, 1024u);
    Actor *h = host(f);
    uint32_t n = build_b_with(f->b, 0, 400), handle;
    CHECK(n * 4u > 1500u);
    term b = offheap(f, h, f->b, n);
    CHECK_EQ(module_load(&f->vm, h, b, &handle), CODE_NO_MEMORY);
    bytes_release(&f->vm, h, b);
    CHECK_EQ(module_mem_used(&f->vm), 0u);
    CHECK_EQ(bytes_table_used(), 0u);
    uint32_t small = load_b(f, h);                          /* < 1024 B */
    CHECK(module_mem_used(&f->vm) <= 1024u);
    /* two small ones exceed it together */
    n = build_b(f->b);
    if (2u * n * 4u > 1024u) {
        b = offheap(f, h, f->b, n);
        CHECK_EQ(module_load(&f->vm, h, b, &handle), CODE_NO_MEMORY);
        bytes_release(&f->vm, h, b);
    }
    b_main_runs(f, h, small);
    actor_exit(&f->vm, h, R_NORMAL, T_EMPTYSTR);
    CHECK_EQ(module_mem_used(&f->vm), 0u);
    free(f);

    static const struct { const char *line; uint32_t cap; bool said; } cases[] = {
        { "code.mem=2K\n", 2048u, false }, { "code.mem=1m\n", 1048576u, false },
        { "other=1\n", 65536u, false }, { "code.mem=12x\n", 65536u, true },
        { "code.mem=\n", 65536u, true }, { "code.mem=K\n", 65536u, true },
        { "code.mem=99999999999\n", 65536u, true }, { "code.mem=5000000M\n", 65536u, true },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        f = calloc(1, sizeof *f);
        boot_with_config(f, cases[i].line);
        CHECK_EQ(f->vm.mod_cap, cases[i].cap);
        CHECK_EQ(strstr(err_buf, "gogo: bad code.mem") != NULL, cases[i].said);
        free(f);
    }
}

/* A module needing kbd on a firmware without it: Unsupported with the
 * missing bits, nothing counted. */
TEST(unsupported) {
    Fx *f = fx_new();
    Actor *h = host(f);
    platform_unix_set_caps(CAP_NET | CAP_POWER | CAP_SD);
    uint32_t n = build_b_with(f->b, CAP_KBD | CAP_NET | CAP_ANYPORT, 0), handle = 0;
    term b = offheap(f, h, f->b, n);
    CHECK_EQ(module_load(&f->vm, h, b, &handle), CODE_UNSUPPORTED);
    CHECK_EQ(handle, CAP_KBD);
    platform_unix_set_caps(CAP_NET | CAP_KBD | CAP_POWER | CAP_SD);
    CHECK_EQ(module_load(&f->vm, h, b, &handle), CODE_OK);  /* the same bytes, now supported */
    bytes_release(&f->vm, h, b);
    actor_exit(&f->vm, h, R_NORMAL, T_EMPTYSTR);
    CHECK_EQ(module_mem_used(&f->vm), 0u);
    CHECK_EQ(bytes_table_used(), 0u);
    free(f);
}

/* Released by its loader while its actor runs: unloaded when that actor
 * exits, and the code memory with it. */
TEST(unload_after_last_actor) {
    Fx *f = fx_new();
    Actor *h = host(f);
    uint32_t handle = load_b(f, h), s = handle >> 16;
    term pid;
    CHECK_EQ(module_spawn(&f->vm, h, handle, (const uint8_t *)"Main", 4, h->id, 0, &pid), CODE_OK);
    module_release(&f->vm, h, handle);
    CHECK(f->vm.mods[s] != NULL);                           /* its actor still runs */
    CHECK(module_mem_used(&f->vm) > 0u);
    run_queue(&f->vm);
    CHECK(f->vm.mods[s] == NULL);
    CHECK_EQ(module_mem_used(&f->vm), 0u);
    CHECK_EQ(bytes_table_used(), 0u);
    module_release(&f->vm, h, handle);                      /* stale: nothing */
    CHECK_EQ(module_hold(&f->vm, h, handle), CODE_NOT_FOUND);
    actor_exit(&f->vm, h, R_NORMAL, T_EMPTYSTR);
    free(f);
}

/* Review Focus 4: the loader exits before Spawn, a new module reuses the
 * slot: the old handle is NotFound for Spawn and Hold and never runs the
 * new code. */
TEST(stale_handle_after_reuse) {
    Fx *f = fx_new();
    Actor *l = actor_new(&f->vm, 64);
    uint32_t old = load_b(f, l), slot = old >> 16;
    actor_exit(&f->vm, l, R_NORMAL, T_EMPTYSTR);
    Actor *h = host(f);
    uint32_t handle;
    for (;;) {
        Actor *x = actor_new(&f->vm, 64);
        handle = load_b(f, x);
        if (handle >> 16 == slot) { module_hold(&f->vm, h, handle); actor_exit(&f->vm, x, R_NORMAL, T_EMPTYSTR); break; }
        actor_exit(&f->vm, x, R_NORMAL, T_EMPTYSTR);
    }
    CHECK(handle != old);
    term pid = T_NIL;
    CHECK_EQ(module_spawn(&f->vm, h, old, (const uint8_t *)"Main", 4, h->id, 0, &pid), CODE_NOT_FOUND);
    CHECK_EQ(module_hold(&f->vm, h, old), CODE_NOT_FOUND);
    CHECK(pid == T_NIL);
    CHECK_EQ(f->vm.mods[slot]->live, 0);
    b_main_runs(f, h, handle);
    actor_exit(&f->vm, h, R_NORMAL, T_EMPTYSTR);
    CHECK(f->vm.mods[slot] == NULL);
    free(f);
}

/* Hold from another actor keeps the module past its loader's exit; holds
 * are per actor (twice is one), at most MOD_HOLDERS; Release ends one. */
TEST(holder_keeps_module) {
    Fx *f = fx_new();
    Actor *l = actor_new(&f->vm, 64);
    uint32_t handle = load_b(f, l), s = handle >> 16;
    Actor *o = host(f);
    CHECK_EQ(module_hold(&f->vm, o, handle), CODE_OK);
    CHECK_EQ(module_hold(&f->vm, o, handle), CODE_OK);     /* already: still one */
    Actor *x[3];
    for (int i = 0; i < 2; i++) { x[i] = actor_new(&f->vm, 64); CHECK_EQ(module_hold(&f->vm, x[i], handle), CODE_OK); }
    x[2] = actor_new(&f->vm, 64);
    CHECK_EQ(module_hold(&f->vm, x[2], handle), CODE_TOO_MANY);   /* l, o, x0, x1 */
    actor_exit(&f->vm, l, R_NORMAL, T_EMPTYSTR);
    CHECK_EQ(module_hold(&f->vm, x[2], handle), CODE_OK);   /* l's hold ended with it */
    for (int i = 0; i < 3; i++) module_release(&f->vm, x[i], handle);
    CHECK(f->vm.mods[s] != NULL);
    b_main_runs(f, o, handle);
    module_release(&f->vm, o, handle);
    CHECK(f->vm.mods[s] == NULL);                           /* no holder, no actor */
    CHECK_EQ(module_mem_used(&f->vm), 0u);
    for (int i = 0; i < 3; i++) actor_exit(&f->vm, x[i], R_NORMAL, T_EMPTYSTR);
    actor_exit(&f->vm, o, R_NORMAL, T_EMPTYSTR);
    free(f);
}

/* GOGO_MAX_MODULES - 1 loads fill the table (slot 0 is the boot image);
 * one more is TooMany and counts nothing. The same Bytes may back them all. */
TEST(table_full) {
    Fx *f = fx_new();
    Actor *h = host(f);
    uint32_t n = build_b(f->b), handle;
    term b = offheap(f, h, f->b, n);
    for (uint32_t i = 1; i < GOGO_MAX_MODULES; i++) CHECK_EQ(module_load(&f->vm, h, b, &handle), CODE_OK);
    uint32_t used = module_mem_used(&f->vm);
    CHECK_EQ(used, (GOGO_MAX_MODULES - 1u) * n * 4u);
    CHECK_EQ(module_load(&f->vm, h, b, &handle), CODE_TOO_MANY);
    CHECK_EQ(module_mem_used(&f->vm), used);
    bytes_release(&f->vm, h, b);
    actor_exit(&f->vm, h, R_NORMAL, T_EMPTYSTR);
    CHECK_EQ(module_mem_used(&f->vm), 0u);
    CHECK_EQ(bytes_table_used(), 0u);
    free(f);
}

/* Bytes sliced at offset 1 (unaligned): copied once, the copy counted and
 * run; the Bytes is not kept. */
TEST(unaligned_bytes_copied) {
    Fx *f = fx_new();
    Actor *h = host(f);
    uint32_t n = build_b(f->b), handle;
    uint8_t *data;
    term r = bytes_new(&f->vm, h, n * 4u + 1u, &data);
    memcpy(data + 1, f->b, n * 4u);
    term sl = bytes_sub(&f->vm, h, r, 1, n * 4u);
    CHECK(sl != T_NIL);
    const uint8_t *p; uint32_t len;
    CHECK(bytes_view(&f->vm, h, sl, &p, &len) && ((uintptr_t)p & 3u) != 0);
    CHECK_EQ(module_load(&f->vm, h, sl, &handle), CODE_OK);
    CHECK_EQ(module_mem_used(&f->vm), n * 4u);
    const ModEntry *e = f->vm.mods[handle >> 16];
    CHECK(e->bytes == T_NIL && ((uintptr_t)e->img.funcs & 3u) == 0);
    bytes_release(&f->vm, h, r);
    heap_gc(&f->vm, h, 0, NULL, 0);
    CHECK_EQ(bytes_table_used(), 0u);                       /* the module did not keep it */
    b_main_runs(f, h, handle);
    actor_exit(&f->vm, h, R_NORMAL, T_EMPTYSTR);
    CHECK_EQ(module_mem_used(&f->vm), 0u);
    free(f);
}

/* platform_code_alloc (PSRAM): when it gives memory the module is copied
 * there, even from in-place-able Bytes, which it then does not keep; the
 * same size is freed at unload. */
TEST(platform_code_memory) {
    Fx *f = fx_new();
    Actor *h = host(f);
    plat_on = true; plat_allocs = plat_frees = 0;
    uint32_t n = build_b(f->b), handle;
    term b = offheap(f, h, f->b, n);
    CHECK_EQ(module_load(&f->vm, h, b, &handle), CODE_OK);
    bytes_release(&f->vm, h, b);
    CHECK_EQ(bytes_table_used(), 0u);
    CHECK_EQ(plat_allocs, 1u);
    CHECK_EQ(plat_last, n * 4u);
    CHECK_EQ(module_mem_used(&f->vm), n * 4u);
    b_main_runs(f, h, handle);
    /* bad images and refusals give it back */
    f->b[0] ^= 1u;
    b = offheap(f, h, f->b, n);
    CHECK_EQ(module_load(&f->vm, h, b, &handle), CODE_BAD_IMAGE);
    bytes_release(&f->vm, h, b);
    CHECK_EQ(plat_allocs - plat_frees, 1u);
    actor_exit(&f->vm, h, R_NORMAL, T_EMPTYSTR);
    CHECK_EQ(plat_frees, plat_allocs);
    plat_on = false;
    free(f);
}

/* B_CODE as the interpreter calls it: Load builds a code.Module{H}, Spawn
 * takes it back; Unsupported leaves the missing bits for Missing; Hold,
 * Release, MemUsed; bad operands are badcode. */
static term *regs(Actor *a) { return (term *)a->mem + a->fp; }
static uint32_t code_op(Fx *f, Actor *a, uint32_t ra, uint32_t op, uint32_t nargs) {
    regs(a)[2] = term_from_int((int32_t)op);
    return module_builtin(&f->vm, a, ra, 2, nargs + 1u, 8);
}
static int32_t field_h(Actor *a, term m) {
    const uint32_t *o = a->mem + term_ref_off(m);
    CHECK(term_is_ref(m) && o[0] == hdr_make(4, K_STRUCT) && o[1] == TYPE_CODE_MODULE);
    return term_to_int(o[2]);
}
/* A code.Module{H, Missing} in a's heap; H boxed when it is no small int. */
static term forge(Actor *a, int32_t h, int32_t missing) {
    uint32_t o = heap_alloc(NULL, a, 6, NULL, 0);
    CHECK(o != UINT32_MAX);
    a->mem[o] = hdr_make(4, K_STRUCT); a->mem[o + 1] = TYPE_CODE_MODULE;
    a->mem[o + 4] = hdr_make(2, K_INT); a->mem[o + 5] = (uint32_t)h;
    a->mem[o + 2] = term_int_fits(h) ? term_from_int(h) : term_from_ref(o + 4);
    a->mem[o + 3] = term_int_fits(missing) ? term_from_int(missing) : term_from_ref(o + 4);
    if (!term_int_fits(h) && !term_int_fits(missing)) CHECK(h == missing);   /* one shared box */
    a->mem[o + 5] = term_int_fits(h) ? (uint32_t)missing : (uint32_t)h;
    return term_from_ref(o);
}
TEST(code_builtin_ops) {
    Fx *f = fx_new();
    Actor *a = actor_new(&f->vm, 512);
    CHECK(heap_push_frame(&f->vm, a, 8, -1));
    uint32_t n = build_b(f->b);
    term b = offheap(f, a, f->b, n);
    regs(a)[3] = b;
    CHECK_EQ(code_op(f, a, 0, CODE_OP_LOAD, 1), R_NORMAL);
    CHECK_EQ(regs(a)[1], term_from_int(CODE_OK));
    term m = regs(a)[0];
    int32_t H = field_h(a, m);
    CHECK(H > 0);
    /* MemUsed, Missing of a good module */
    CHECK_EQ(code_op(f, a, 6, CODE_OP_MEM_USED, 0), R_NORMAL);
    CHECK_EQ(regs(a)[6], term_from_int((int32_t)(n * 4u)));
    regs(a)[3] = m;
    CHECK_EQ(code_op(f, a, 6, CODE_OP_MISSING, 1), R_NORMAL);
    CHECK_EQ(regs(a)[6], term_from_int(0));
    /* Spawn(m, "Main", self) */
    regs(a)[3] = m;
    regs(a)[4] = term_from_const(0) | module_tag(&f->vm, (uint32_t)(H - 1) >> 16);   /* B's "Main" */
    regs(a)[5] = a->id;
    CHECK_EQ(code_op(f, a, 0, CODE_OP_SPAWN, 3), R_NORMAL);
    CHECK_EQ(regs(a)[1], term_from_int(CODE_OK));
    CHECK(term_is_pid(regs(a)[0]) && regs(a)[0] != T_ZEROPID);
    /* NotFound: a name it does not export; the zero Module */
    regs(a)[4] = term_from_const(3);                        /* A's "f" */
    CHECK_EQ(code_op(f, a, 0, CODE_OP_SPAWN_LINK, 3), R_NORMAL);
    CHECK_EQ(regs(a)[1], term_from_int(CODE_NOT_FOUND));
    CHECK_EQ(regs(a)[0], T_ZEROPID);
    term zero = forge(a, 0, 0);
    uint32_t o;
    regs(a)[3] = zero;
    regs(a)[4] = term_from_const(0);                        /* A's "main" */
    CHECK_EQ(code_op(f, a, 0, CODE_OP_SPAWN, 3), R_NORMAL);  /* never module 0 */
    CHECK_EQ(regs(a)[1], term_from_int(CODE_NOT_FOUND));
    CHECK_EQ(code_op(f, a, 6, CODE_OP_HOLD, 1), R_NORMAL);
    CHECK_EQ(regs(a)[6], term_from_int(CODE_NOT_FOUND));
    /* Hold / Release */
    regs(a)[3] = m;
    CHECK_EQ(code_op(f, a, 6, CODE_OP_HOLD, 1), R_NORMAL);
    CHECK_EQ(regs(a)[6], term_from_int(CODE_OK));
    /* SpawnOpt with Options{MaxMemory: 4096} */
    o = heap_alloc(&f->vm, a, 3, NULL, 0);
    a->mem[o] = hdr_make(3, K_STRUCT); a->mem[o + 1] = TYPE_OPTIONS; a->mem[o + 2] = term_from_int(4096);
    regs(a)[3] = m;
    regs(a)[4] = term_from_const(0) | module_tag(&f->vm, (uint32_t)(H - 1) >> 16);
    regs(a)[5] = a->id;
    regs(a)[6] = term_from_ref(o);
    CHECK_EQ(code_op(f, a, 0, CODE_OP_SPAWN_OPT, 4), R_NORMAL);
    CHECK_EQ(regs(a)[1], term_from_int(CODE_OK));
    Actor *c = actor_lookup(&f->vm, regs(a)[0]);
    CHECK(c && c->max_words == 1024u);
    regs(a)[6] = term_from_int(1);                          /* not Options */
    CHECK_EQ(code_op(f, a, 0, CODE_OP_SPAWN_OPT, 4), R_BAD_CODE);
    run_queue(&f->vm);
    regs(a)[3] = m;
    CHECK_EQ(code_op(f, a, 6, CODE_OP_RELEASE, 1), R_NORMAL);
    CHECK_EQ(regs(a)[6], T_NIL);
    CHECK(f->vm.mods[(uint32_t)(H - 1) >> 16] == NULL);    /* no holder, no actor */
    regs(a)[3] = m;
    regs(a)[4] = term_from_const(0);                        /* (B's "Main" is gone with B) */
    CHECK_EQ(code_op(f, a, 0, CODE_OP_SPAWN, 3), R_NORMAL);
    CHECK_EQ(regs(a)[1], term_from_int(CODE_NOT_FOUND));   /* stale */
    /* Unsupported: Module{H: 0, Missing: the bits} */
    platform_unix_set_caps(CAP_NET);
    b = offheap(f, a, f->b, build_b_with(f->b, CAP_KBD | CAP_SD, 0));
    regs(a)[3] = b;
    CHECK_EQ(code_op(f, a, 0, CODE_OP_LOAD, 1), R_NORMAL);
    platform_unix_set_caps(CAP_NET | CAP_KBD | CAP_POWER | CAP_SD);
    CHECK_EQ(regs(a)[1], term_from_int(CODE_UNSUPPORTED));
    CHECK_EQ(field_h(a, regs(a)[0]), 0);
    regs(a)[3] = regs(a)[0];
    CHECK_EQ(code_op(f, a, 6, CODE_OP_MISSING, 1), R_NORMAL);
    CHECK_EQ(regs(a)[6], term_from_int((int32_t)(CAP_KBD | CAP_SD)));
    CHECK_EQ(code_op(f, a, 0, CODE_OP_SPAWN, 3), R_NORMAL);
    CHECK_EQ(regs(a)[1], term_from_int(CODE_NOT_FOUND));
    /* badcode: unknown op, wrong arity, not a Module, no room for 2 results */
    CHECK_EQ(code_op(f, a, 0, 99, 0), R_BAD_CODE);
    CHECK_EQ(code_op(f, a, 0, CODE_OP_LOAD, 2), R_BAD_CODE);
    regs(a)[3] = term_from_int(5);
    CHECK_EQ(code_op(f, a, 6, CODE_OP_HOLD, 1), R_BAD_CODE);
    regs(a)[3] = b;
    CHECK_EQ(code_op(f, a, 7, CODE_OP_LOAD, 1), R_BAD_CODE);
    actor_exit(&f->vm, a, R_NORMAL, T_EMPTYSTR);
    CHECK_EQ(module_mem_used(&f->vm), 0u);
    CHECK_EQ(bytes_table_used(), 0u);
    free(f);
}

/* I1: whatever H or Missing a program forges, B_CODE gives NotFound /
 * nothing, never module 0, never UB (run under UBSan); an Unsupported
 * load's Module is H 0 whatever bits are missing, and Missing reports them
 * all, boxed when they do not fit a small int. */
TEST(forged_module_values_safe) {
    Fx *f = fx_new();
    Actor *a = actor_new(&f->vm, 1024);
    CHECK(heap_push_frame(&f->vm, a, 8, -1));
    uint32_t live = load_b(f, a);                           /* a module to aim at */
    CHECK_EQ(live, 1u << 16);                                /* slot 1, generation 0: H 65537 */
    const int32_t hs[] = { INT32_MIN, INT32_MIN + 1, -1, -65536, 0, 1, 2, 65536,
                           (int32_t)live + 2, (int32_t)(live | 0xFFFFu) + 1, 8 << 16, (8 << 16) + 1,
                           TERM_INT_MAX, TERM_INT_MAX + 1, INT32_MAX };
    for (size_t i = 0; i < sizeof hs / sizeof hs[0]; i++) {
        term m = forge(a, hs[i], term_int_fits(hs[i]) ? INT32_MIN : hs[i]);
        regs(a)[3] = m; regs(a)[4] = term_from_const(0); regs(a)[5] = T_NIL;
        CHECK_EQ(code_op(f, a, 0, CODE_OP_SPAWN, 3), R_NORMAL);
        CHECK_EQ(regs(a)[1], term_from_int(CODE_NOT_FOUND));
        regs(a)[3] = m;
        CHECK_EQ(code_op(f, a, 6, CODE_OP_HOLD, 1), R_NORMAL);
        CHECK_EQ(regs(a)[6], term_from_int(CODE_NOT_FOUND));
        regs(a)[3] = m;
        CHECK_EQ(code_op(f, a, 6, CODE_OP_RELEASE, 1), R_NORMAL);
        regs(a)[3] = m;
        CHECK_EQ(code_op(f, a, 6, CODE_OP_MISSING, 1), R_NORMAL);   /* the field, as is */
        int32_t mv = 0;
        CHECK(int_val(&f->vm, a, regs(a)[6], &mv) && mv == (term_int_fits(hs[i]) ? INT32_MIN : hs[i]));
    }
    CHECK(f->vm.mods[0] != NULL && f->vm.mods[live >> 16] != NULL);
    CHECK_EQ(f->vm.mods[0]->holders[0], MOD_NO_HOLDER);    /* never held through code */
    /* the real handle still works */
    term ok = forge(a, (int32_t)live + 1, 0);
    regs(a)[3] = ok;
    CHECK_EQ(code_op(f, a, 6, CODE_OP_HOLD, 1), R_NORMAL);
    CHECK_EQ(regs(a)[6], term_from_int(CODE_OK));
    /* the probe: requires 0x7FFFFFFF on caps 0 */
    platform_unix_set_caps(0);
    term b = offheap(f, a, f->b, build_b_with(f->b, 0x7FFFFFFFu, 0));
    regs(a)[3] = b;
    CHECK_EQ(code_op(f, a, 0, CODE_OP_LOAD, 1), R_NORMAL);
    platform_unix_set_caps(CAP_NET | CAP_KBD | CAP_POWER | CAP_SD);
    CHECK_EQ(regs(a)[1], term_from_int(CODE_UNSUPPORTED));
    term m = regs(a)[0];
    CHECK_EQ(field_h(a, m), 0);
    regs(a)[3] = m;
    CHECK_EQ(code_op(f, a, 6, CODE_OP_MISSING, 1), R_NORMAL);
    int32_t got = 0;
    CHECK(int_val(&f->vm, a, regs(a)[6], &got));
    CHECK_EQ(got, 0x7FFFFFFF);
    regs(a)[3] = m;
    CHECK_EQ(code_op(f, a, 6, CODE_OP_HOLD, 1), R_NORMAL);
    CHECK_EQ(regs(a)[6], term_from_int(CODE_NOT_FOUND));
    CHECK_EQ(f->vm.mods[0]->holders[0], MOD_NO_HOLDER);
    actor_exit(&f->vm, a, R_NORMAL, T_EMPTYSTR);
    CHECK_EQ(module_mem_used(&f->vm), 0u);
    free(f);
}

/* I2: a module from a small aligned slice of a big (200 KB) Bytes is a
 * copy of the slice (counted), so the big Bytes is not kept alive; a whole root runs
 * in place. Over-cap Bytes are refused before any copy (M1). */
TEST(slice_not_pinned) {
    Fx *f = fx_new();
    Actor *h = host(f);
    uint32_t n = build_b(f->b), handle;
    uint8_t *data;
    h->max_words = 0xFFFF;
    /* 200 KB; 64 KB under the MCU arena (160 KB) */
    term big = bytes_new(&f->vm, h, GOGO_ARENA_BYTES ? 64u * 1024u : 200u * 1024u, &data);
    CHECK(big != T_NIL);
    memcpy(data + 1024, f->b, n * 4u);
    term sl = bytes_sub(&f->vm, h, big, 1024, n * 4u);
    CHECK(sl != T_NIL);
    size_t va0 = va_used_bytes();
    CHECK_EQ(module_load(&f->vm, h, sl, &handle), CODE_OK);
    CHECK(va_used_bytes() - va0 < 8u * 1024u);              /* about the slice, not the root */
    CHECK(f->vm.mods[handle >> 16]->bytes == T_NIL);
    CHECK_EQ(module_mem_used(&f->vm), n * 4u);
    bytes_release(&f->vm, h, big);
    heap_gc(&f->vm, h, 0, NULL, 0);
    CHECK_EQ(bytes_table_used(), 0u);                       /* the big root is gone */
    b_main_runs(f, h, handle);
    /* a whole root: in place */
    term r = offheap(f, h, f->b, n);
    CHECK_EQ(module_load(&f->vm, h, r, &handle), CODE_OK);
    CHECK(f->vm.mods[handle >> 16]->bytes == r);
    bytes_release(&f->vm, h, r);
    /* over the cap: NoMemory before validating (no copy of the unaligned bytes) */
    f->vm.mod_cap = 1024;
    term u = bytes_new(&f->vm, h, 4001, &data);
    term us = bytes_sub(&f->vm, h, u, 1, 4000);
    va0 = va_used_bytes();
    CHECK_EQ(module_load(&f->vm, h, us, &handle), CODE_NO_MEMORY);
    CHECK_EQ(va_used_bytes(), va0);
    bytes_release(&f->vm, h, u);
    actor_exit(&f->vm, h, R_NORMAL, T_EMPTYSTR);
    CHECK_EQ(module_mem_used(&f->vm), 0u);
    CHECK_EQ(bytes_table_used(), 0u);
    free(f);
}

/* M1/M2: on a platform-memory board an unaligned Bytes is validated in
 * platform memory (one copy, no VM copy); an unaligned platform block is
 * given back and the VM falls back to its own copy. */
TEST(platform_memory_single_copy) {
    Fx *f = fx_new();
    Actor *h = host(f);
    uint32_t n = build_b(f->b), handle;
    uint8_t *data;
    term r = bytes_new(&f->vm, h, n * 4u + 1u, &data);
    memcpy(data + 1, f->b, n * 4u);
    term sl = bytes_sub(&f->vm, h, r, 1, n * 4u);
    plat_on = true; plat_allocs = plat_frees = 0;
    size_t va0 = va_used_bytes();
    CHECK_EQ(module_load(&f->vm, h, sl, &handle), CODE_OK);
    CHECK_EQ(plat_allocs, 1u);
    CHECK(va_used_bytes() - va0 <= sizeof(ModEntry) + 8u); /* the entry only */
    b_main_runs(f, h, handle);
    module_release(&f->vm, h, handle);
    CHECK_EQ(plat_frees, 1u);
    plat_misalign = true;
    CHECK_EQ(module_load(&f->vm, h, sl, &handle), CODE_OK);
    CHECK_EQ(plat_frees, 2u);                               /* the bad block went back */
    CHECK(f->vm.mods[handle >> 16]->flags & MOD_OWN_COPY);
    b_main_runs(f, h, handle);
    plat_misalign = false; plat_on = false;
    bytes_release(&f->vm, h, r);
    actor_exit(&f->vm, h, R_NORMAL, T_EMPTYSTR);
    CHECK_EQ(module_mem_used(&f->vm), 0u);
    free(f);
}

/* ---- cross-module rules (Task 5, design §5.3) ---- */

/* An actor of module slot (B's), with a frame. */
static Actor *mod_actor(Fx *f, uint32_t slot) {
    Actor *a = actor_new_in(&f->vm, 256, (uint8_t)slot);
    CHECK(a != NULL);
    CHECK(heap_push_frame(&f->vm, a, 4, -1));
    return a;
}

/* A slot no module is in: any tag of it is stale. */
#define STALE_TAG mod_tag_make(6, 3)

static bool is_str(Vm *vm, Actor *a, term t, const char *want) {
    const uint8_t *p; uint32_t len;
    return name_bytes(vm, a, t, &p, &len) && len == strlen(want) && memcmp(p, want, len) == 0;
}

static int mbox_n(const Actor *a) {
    int n = 0;
    for (const Msg *m = a->mbox_head; m; m = m->next) n++;
    return n;
}

/* B's const string reaches the host as a heap copy, also inside a reserved
 * struct; the host reads both after B unloaded (ASan: no use of freed code). */
TEST(xmod_const_string_copied) {
    Fx *f = fx_new();
    Vm *vm = &f->vm;
    Actor *h = host(f), *l = host(f);
    uint32_t slot = load_b(f, l) >> 16;
    Actor *m = mod_actor(f, slot);
    term s = term_from_const(6) | module_tag(vm, slot);
    CHECK_EQ(msg_send(vm, m, h->id, s), 0);
    CHECK_EQ(msg_send(vm, m, h->id, plant(m, K_STRUCT, TYPE_REASON, 2, s)), 0);
    actor_exit(vm, m, R_NORMAL, T_EMPTYSTR);
    actor_exit(vm, l, R_NORMAL, T_EMPTYSTR);
    CHECK(vm->mods[slot] == NULL);
    term t;
    CHECK(msg_receive(vm, h, &t));
    CHECK(term_is_ref(t));
    CHECK(is_str(vm, h, t, "BBBB"));
    CHECK(msg_receive(vm, h, &t));
    const uint32_t *o = obj_of(vm, h, t);
    CHECK(o && o[1] == TYPE_REASON && term_is_ref(o[2]) && term_is_ref(o[3]));
    if (o) CHECK(is_str(vm, h, o[2], "BBBB") && is_str(vm, h, o[3], "BBBB"));
    actor_exit(vm, h, R_NORMAL, T_EMPTYSTR);
    CHECK_EQ(vm->live, 0u);
    free(f);
}

/* B's const int64 (and int) boxes are copied too. */
TEST(xmod_int64_copied) {
    Fx *f = fx_new();
    Vm *vm = &f->vm;
    Actor *h = host(f), *l = host(f);
    b_pad = 3;
    uint32_t slot = load_b(f, l) >> 16;
    b_pad = 0;
    Actor *m = mod_actor(f, slot);
    CHECK_EQ(msg_send(vm, m, h->id, term_from_const(B_PAD) | module_tag(vm, slot)), 0);
    actor_exit(vm, m, R_NORMAL, T_EMPTYSTR);
    actor_exit(vm, l, R_NORMAL, T_EMPTYSTR);
    CHECK(vm->mods[slot] == NULL);
    term t;
    CHECK(msg_receive(vm, h, &t));
    CHECK(term_is_ref(t));
    const uint32_t *o = obj_of(vm, h, t);
    CHECK(o && o[0] == hdr_make(3, K_INT64));
    if (o) CHECK(((uint64_t)o[1] | (uint64_t)o[2] << 32) == (uint64_t)B_INT64);

    b_pad = 2;                                 /* a K_INT at B_PAD */
    slot = load_b(f, l = host(f)) >> 16;
    b_pad = 0;
    m = mod_actor(f, slot);
    CHECK_EQ(msg_send(vm, m, h->id, term_from_const(B_PAD) | module_tag(vm, slot)), 0);
    actor_exit(vm, m, R_NORMAL, T_EMPTYSTR);
    actor_exit(vm, l, R_NORMAL, T_EMPTYSTR);
    CHECK(msg_receive(vm, h, &t));
    o = obj_of(vm, h, t);
    CHECK(term_is_ref(t) && o && o[0] == hdr_make(2, K_INT));
    actor_exit(vm, h, R_NORMAL, T_EMPTYSTR);
    free(f);
}

/* A module's own struct sent to another module is BadMsg for the sender,
 * here B's (also nested in a reserved struct); nothing is queued. Through
 * the interpreter: B's Bad(host) exits with badmsg, the host lives on. */
TEST(xmod_module_struct_badmsg) {
    Fx *f = fx_new();
    Vm *vm = &f->vm;
    Actor *h = host(f);
    uint32_t handle = load_b(f, h), slot = handle >> 16, tag = module_tag(vm, slot);
    Actor *m = mod_actor(f, slot);
    term st = plant(m, K_STRUCT, T_MOD | tag, 1, term_from_int(1));
    CHECK_EQ(msg_send(vm, m, h->id, st), -(int)R_BAD_MSG);
    CHECK_EQ(msg_send(vm, m, h->id, plant(m, K_STRUCT, TYPE_OPTIONS, 1, st)), -(int)R_BAD_MSG);
    CHECK_EQ(mbox_n(h), 0);
    /* to an actor of its own module it goes */
    Actor *m2 = mod_actor(f, slot);
    CHECK_EQ(msg_send(vm, m, m2->id, st), 0);
    actor_exit(vm, m, R_NORMAL, T_EMPTYSTR);
    actor_exit(vm, m2, R_NORMAL, T_EMPTYSTR);

    term pid;
    CHECK_EQ(module_spawn(vm, h, handle, (const uint8_t *)"Bad", 3, h->id, 0, &pid), CODE_OK);
    capture_start();
    run_queue(vm);
    capture_stop();
    CHECK(strstr(err_buf, "crashed: badmsg in Bad") != NULL);
    CHECK_EQ(mbox_n(h), 0);
    CHECK(h->state != A_EXITED && !(h->flags & AF_EXIT_PENDING));
    actor_exit(vm, h, R_NORMAL, T_EMPTYSTR);
    free(f);
}

/* Symmetric: the host's own struct as the reply to a module's Call is
 * BadMsg for the host; the module gets no reply. */
TEST(xmod_host_struct_badmsg) {
    Fx *f = fx_new();
    Vm *vm = &f->vm;
    Actor *h = host(f);
    uint32_t slot = load_b(f, h) >> 16;
    Actor *m = mod_actor(f, slot);
    term ref = plant(h, K_REF, 5, 1, 0);
    term st = plant(h, K_STRUCT, T_MOD, 1, term_from_int(1));
    CHECK_EQ(msg_send_reply(vm, h, m->id, ref, st), -(int)R_BAD_MSG);
    CHECK_EQ(msg_send(vm, h, m->id, st), -(int)R_BAD_MSG);
    CHECK_EQ(mbox_n(m), 0);
    CHECK_EQ(msg_send_reply(vm, h, m->id, ref, term_from_int(1)), 0);
    CHECK_EQ(mbox_n(m), 1);
    actor_exit(vm, m, R_NORMAL, T_EMPTYSTR);
    actor_exit(vm, h, R_NORMAL, T_EMPTYSTR);
    free(f);
}

/* Any closure is BadMsg across modules, either way. */
TEST(xmod_closure_badmsg) {
    Fx *f = fx_new();
    Vm *vm = &f->vm;
    Actor *h = host(f);
    uint32_t slot = load_b(f, h) >> 16;
    Actor *m = mod_actor(f, slot);
    CHECK_EQ(msg_send(vm, h, m->id, plant(h, K_CLOSURE, F_FIELD, 1, T_NIL)), -(int)R_BAD_MSG);
    CHECK_EQ(msg_send(vm, m, h->id, plant(m, K_CLOSURE, 1u | module_tag(vm, slot), 1, T_NIL)),
             -(int)R_BAD_MSG);
    CHECK_EQ(mbox_n(h) + mbox_n(m), 0);
    actor_exit(vm, m, R_NORMAL, T_EMPTYSTR);
    actor_exit(vm, h, R_NORMAL, T_EMPTYSTR);
    free(f);
}

/* Reserved structs (actor.Reason, net.Addr, code.Module) cross freely. */
TEST(xmod_reserved_types_ok) {
    Fx *f = fx_new();
    Vm *vm = &f->vm;
    Actor *h = host(f);
    uint32_t slot = load_b(f, h) >> 16;
    Actor *m = mod_actor(f, slot);
    CHECK_EQ(msg_send(vm, h, m->id, plant(h, K_STRUCT, TYPE_REASON, 2, term_from_int(3))), 0);
    CHECK_EQ(msg_send(vm, h, m->id, plant(h, K_STRUCT, TYPE_NET_ADDR, 2, term_from_int(3))), 0);
    CHECK_EQ(msg_send(vm, m, h->id, plant(m, K_STRUCT, TYPE_CODE_MODULE, 2, term_from_int(3))), 0);
    CHECK_EQ(mbox_n(m), 2);
    CHECK_EQ(mbox_n(h), 1);
    actor_exit(vm, m, R_NORMAL, T_EMPTYSTR);
    actor_exit(vm, h, R_NORMAL, T_EMPTYSTR);
    free(f);
}

/* code.Spawn's arg follows the rules: a host struct or closure is BadArg
 * (no actor, no crash); a host const string arrives as a copy; a stale
 * value is the caller's badcode. */
TEST(xmod_spawn_arg_badarg) {
    Fx *f = fx_new();
    Vm *vm = &f->vm;
    Actor *h = host(f);
    uint32_t handle = load_b(f, h), slot = handle >> 16;
    term pid = T_NIL;
    CHECK_EQ(module_spawn(vm, h, handle, (const uint8_t *)"Main", 4,
                          plant(h, K_STRUCT, T_MOD, 1, T_NIL), 0, &pid), CODE_BAD_ARG);
    CHECK_EQ(module_spawn(vm, h, handle, (const uint8_t *)"Main", 4,
                          plant(h, K_CLOSURE, F_FIELD, 1, T_NIL), 0, &pid), CODE_BAD_ARG);
    CHECK_EQ(module_spawn(vm, h, handle, (const uint8_t *)"Main", 4,
                          term_from_const(6) | STALE_TAG, 0, &pid), -(int)R_BAD_CODE);
    CHECK(pid == T_NIL);
    CHECK_EQ(vm->mods[slot]->live, 0);
    CHECK_EQ(module_spawn(vm, h, handle, (const uint8_t *)"Main", 4, term_from_const(6), 0, &pid),
             CODE_OK);
    Actor *c = actor_lookup(vm, pid);
    CHECK(c && term_is_ref(c->mem[c->fp]) && is_str(vm, c, c->mem[c->fp], "AAAA"));
    capture_start();
    run_queue(vm);                             /* Main's sends to a string drop */
    capture_stop();
    CHECK_EQ(err_len, 0u);
    actor_exit(vm, h, R_NORMAL, T_EMPTYSTR);
    free(f);
}

/* SendAfter checks when it is called, not when the timer fires. */
TEST(xmod_sendafter_checked_at_call) {
    Fx *f = fx_new();
    Vm *vm = &f->vm;
    Actor *h = host(f);
    uint32_t slot = load_b(f, h) >> 16;
    Actor *m = mod_actor(f, slot);
    uint32_t why = 0;
    CHECK(timer_add_send(vm, h, 5, m->id, plant(h, K_STRUCT, T_MOD, 1, T_NIL), &why) == T_NIL);
    CHECK_EQ(why, R_BAD_MSG);
    CHECK(timer_add_send(vm, h, 5, m->id, term_from_const(6) | STALE_TAG, &why) == T_NIL);
    CHECK_EQ(why, R_BAD_CODE);
    CHECK_EQ(vm->ntimers, 0u);
    CHECK(term_is_ref(timer_add_send(vm, h, 5, m->id, term_from_const(6), &why)));
    CHECK_EQ(vm->ntimers, 1u);
    actor_exit(vm, m, R_NORMAL, T_EMPTYSTR);
    actor_exit(vm, h, R_NORMAL, T_EMPTYSTR);
    free(f);
}

/* Review Focus 2: the ExitMsg / DownMsg of a B actor that crashed in B's
 * "get" carry Func "" for host watchers, the name for a B watcher; the
 * crash report names it. Read after B unloaded. */
TEST(exit_notice_func_blank_across_modules) {
    Fx *f = fx_new();
    Vm *vm = &f->vm;
    Actor *h = host(f), *w = host(f), *l = host(f);
    uint32_t slot = load_b(f, l) >> 16, tag = module_tag(vm, slot);
    Actor *m = mod_actor(f, slot), *m2 = mod_actor(f, slot);
    sig_set_trap(h, true);
    sig_set_trap(m2, true);
    CHECK(sig_link(vm, h, m->id));
    CHECK(sig_link(vm, m2, m->id));
    CHECK(sig_monitor(vm, w, m->id) != T_NIL);
    capture_start();
    actor_exit(vm, m, R_DIV_ZERO, term_from_const(3) | tag);
    capture_stop();
    CHECK(strstr(err_buf, "crashed: divzero in get") != NULL);
    term t;
    CHECK(msg_receive(vm, m2, &t));
    const uint32_t *o = obj_of(vm, m2, t), *r = o ? obj_of(vm, m2, o[3]) : NULL;
    CHECK(o && o[1] == TYPE_EXITMSG && r && r[3] == (term_from_const(3) | tag));
    actor_exit(vm, m2, R_NORMAL, T_EMPTYSTR);
    actor_exit(vm, l, R_NORMAL, T_EMPTYSTR);
    CHECK(vm->mods[slot] == NULL);
    CHECK(msg_receive(vm, h, &t));
    o = obj_of(vm, h, t);
    r = o ? obj_of(vm, h, o[3]) : NULL;
    CHECK(o && o[1] == TYPE_EXITMSG && r && r[2] == term_from_int(R_DIV_ZERO) && r[3] == T_EMPTYSTR);
    CHECK(msg_receive(vm, w, &t));
    o = obj_of(vm, w, t);
    r = o ? obj_of(vm, w, o[4]) : NULL;
    CHECK(o && o[1] == TYPE_DOWNMSG && r && r[3] == T_EMPTYSTR);
    actor_exit(vm, h, R_NORMAL, T_EMPTYSTR);
    actor_exit(vm, w, R_NORMAL, T_EMPTYSTR);
    free(f);
}

/* Inside one module nothing changes: a const stays a const ref, a module
 * struct and a closure go through. */
TEST(same_module_unchanged) {
    Fx *f = fx_new();
    Vm *vm = &f->vm;
    Actor *h = host(f), *h2 = host(f);
    CHECK_EQ(msg_send(vm, h, h2->id, term_from_const(6)), 0);
    CHECK_EQ(msg_send(vm, h, h2->id, plant(h, K_STRUCT, T_MOD, 1, term_from_const(6))), 0);
    CHECK_EQ(msg_send(vm, h, h2->id, plant(h, K_CLOSURE, F_FIELD, 1, T_NIL)), 0);
    term t;
    CHECK(msg_receive(vm, h2, &t));
    CHECK(t == term_from_const(6));
    CHECK(msg_receive(vm, h2, &t));
    const uint32_t *o = obj_of(vm, h2, t);
    CHECK(o && o[1] == T_MOD && o[2] == term_from_const(6));
    CHECK(msg_receive(vm, h2, &t));
    actor_exit(vm, h, R_NORMAL, T_EMPTYSTR);
    actor_exit(vm, h2, R_NORMAL, T_EMPTYSTR);
    free(f);
}

/* A stale value in a message (const ref, module struct, closure; at the
 * root or nested), a SendAfter or Spawn's captures is the sender's
 * badcode, never a silent copy. */
TEST(stale_send_badcode) {
    Fx *f = fx_new();
    Vm *vm = &f->vm;
    Actor *h = host(f), *h2 = host(f);
    term sc = term_from_const(6) | STALE_TAG;
    CHECK_EQ(msg_send(vm, h, h2->id, sc), -(int)R_BAD_CODE);
    CHECK_EQ(msg_send(vm, h, h2->id, plant(h, K_STRUCT, TYPE_OPTIONS, 1, sc)), -(int)R_BAD_CODE);
    CHECK_EQ(msg_send(vm, h, h2->id, plant(h, K_STRUCT, T_MOD | STALE_TAG, 1, T_NIL)), -(int)R_BAD_CODE);
    CHECK_EQ(msg_send(vm, h, h2->id, plant(h, K_CLOSURE, 1u | STALE_TAG, 1, T_NIL)), -(int)R_BAD_CODE);
    CHECK_EQ(mbox_n(h2), 0);
    /* actor.Spawn of an A closure capturing a stale const */
    Actor *s = a_actor(f, F_SPAWN, T_NIL);
    s->mem[s->fp] = plant(s, K_CLOSURE, F_FIELD, 1, sc);
    CHECK(strstr(run_to_exit(f, s), "crashed: badcode") != NULL);
    actor_exit(vm, h, R_NORMAL, T_EMPTYSTR);
    actor_exit(vm, h2, R_NORMAL, T_EMPTYSTR);
    CHECK_EQ(vm->live, 0u);
    free(f);
}

/* Review I-1: a send that drops (dead target, not a PID, a Reply to a dead
 * caller or with no Ref) still checks its value, against the sender's own
 * module as SendAfter does: a stale value is the sender's badcode whether
 * or not the receiver lives; a live value just drops. */
TEST(stale_send_to_dead_badcode) {
    Fx *f = fx_new();
    Vm *vm = &f->vm;
    Actor *h = host(f), *d = host(f);
    term dead = d->id;
    actor_exit(vm, d, R_NORMAL, T_EMPTYSTR);
    term sc = term_from_const(6) | STALE_TAG;
    term ref = plant(h, K_REF, 5, 1, 0);
    CHECK_EQ(msg_send(vm, h, dead, sc), -(int)R_BAD_CODE);
    CHECK_EQ(msg_send(vm, h, term_from_int(3), plant(h, K_STRUCT, T_MOD | STALE_TAG, 1, T_NIL)),
             -(int)R_BAD_CODE);
    CHECK_EQ(msg_send_reply(vm, h, dead, ref, sc), -(int)R_BAD_CODE);
    CHECK_EQ(msg_send_reply(vm, h, h->id, term_from_int(1), sc), -(int)R_BAD_CODE);
    uint32_t why = 0;
    CHECK(timer_add_send(vm, h, 5, dead, sc, &why) == T_NIL);
    CHECK_EQ(why, R_BAD_CODE);
    /* live values to nobody: dropped, no crash */
    CHECK_EQ(msg_send(vm, h, dead, term_from_const(6)), 1);
    CHECK_EQ(msg_send(vm, h, dead, plant(h, K_STRUCT, T_MOD, 1, T_NIL)), 1);
    CHECK_EQ(msg_send_reply(vm, h, dead, ref, term_from_int(1)), 1);
    CHECK_EQ(mbox_n(h), 0);
    /* through the interpreter: the sender exits badcode */
    Actor *s = a_actor(f, F_SEND, T_NIL);
    s->mem[s->fp + 2] = dead;
    s->mem[s->fp + 3] = sc;
    CHECK(strstr(run_to_exit(f, s), "crashed: badcode") != NULL);
    Actor *ok = a_actor(f, F_SEND, T_NIL);
    ok->mem[ok->fp + 2] = dead;
    ok->mem[ok->fp + 3] = term_from_const(6);
    CHECK(strcmp(run_to_exit(f, ok), "") == 0);
    actor_exit(vm, h, R_NORMAL, T_EMPTYSTR);
    CHECK_EQ(vm->live, 0u);
    free(f);
}
