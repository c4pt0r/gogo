/* String builtins, called directly on an actor with a 8-register frame. */
#define _POSIX_C_SOURCE 200809L   /* setenv, mkstemp, strdup */
#include "test.h"
#include "test_host.h"
#include "actor.h"
#include "heap.h"
#include "alloc.h"
#include "image.h"
#include "strs.h"
#include "opcodes.h"
#include "term.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define IMG_WORDS 20

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

typedef struct Fx { uint32_t w[IMG_WORDS]; Image img; Vm vm; Actor *a; } Fx;

static void fx_init(Fx *f, uint16_t words) {
    build_img(f->w);
    CHECK_EQ(image_load(&f->img, f->w, img_seal(f->w)), IMG_OK);
    CHECK_EQ(test_vm_init(&f->vm, &f->img, 4), 0);
    f->a = actor_new(&f->vm, words);
    CHECK(f->a != NULL);
    CHECK(heap_push_frame(&f->vm, f->a, 8, -1));
}

#define REGS(f) ((term *)(f)->a->mem + (f)->a->fp)

/* A heap string of the given bytes in register r. */
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

/* True if register r holds a string with exactly the bytes s. */
static bool str_is(Fx *f, int r, const char *s) {
    term k = REGS(f)[r];
    if (k == T_EMPTYSTR) return s[0] == 0;
    const uint32_t *o = obj_of(&f->vm, f->a, k);
    if (!o || hdr_kind(o[0]) != K_STRING) return false;
    return o[1] == strlen(s) && memcmp(o + 2, s, o[1]) == 0;
}

static uint32_t call(Fx *f, uint32_t id, uint32_t ra, uint32_t rb, uint32_t rc) {
    return strs_builtin(&f->vm, f->a, id, ra, rb, rc);
}

TEST(strs_concat_len_index) {
    Fx f; fx_init(&f, 64);
    set_str(&f, 0, "foo");
    REGS(&f)[1] = term_from_const(0);             /* "main" */
    CHECK_EQ(call(&f, B_STR_CONCAT, 2, 0, 2), R_NORMAL);
    CHECK(str_is(&f, 2, "foomain"));
    CHECK_EQ(call(&f, B_LEN, 3, 2, 1), R_NORMAL);
    CHECK_EQ(REGS(&f)[3], term_from_int(7));
    REGS(&f)[3] = term_from_int(3);               /* index: r2 (string), r3 (int) */
    CHECK_EQ(call(&f, B_INDEX, 4, 2, 2), R_NORMAL);
    CHECK_EQ(REGS(&f)[4], term_from_int('m'));
    actor_free(&f.vm, f.a);
}

TEST(strs_empty_concat_shares) {
    Fx f; fx_init(&f, 64);
    set_str(&f, 0, "abc");
    REGS(&f)[1] = T_EMPTYSTR;
    term t = REGS(&f)[0];
    CHECK_EQ(call(&f, B_STR_CONCAT, 2, 0, 2), R_NORMAL);
    CHECK_EQ(REGS(&f)[2], t);
    CHECK_EQ(call(&f, B_STR_CONCAT, 2, 1, 2), R_NORMAL);   /* "" + "abc" */
    CHECK_EQ(REGS(&f)[2], t);   /* "" + "abc": the same string */
    REGS(&f)[2] = T_EMPTYSTR;                     /* "" + "" */
    CHECK_EQ(call(&f, B_STR_CONCAT, 4, 1, 2), R_NORMAL);
    CHECK_EQ(REGS(&f)[4], T_EMPTYSTR);
    actor_free(&f.vm, f.a);
}

TEST(strs_index_and_bounds) {
    Fx f; fx_init(&f, 64);
    set_str(&f, 0, "abc");
    REGS(&f)[1] = term_from_int(2);
    CHECK_EQ(call(&f, B_INDEX, 2, 0, 2), R_NORMAL);
    CHECK_EQ(REGS(&f)[2], term_from_int('c'));
    REGS(&f)[1] = term_from_int(3);
    CHECK_EQ(call(&f, B_INDEX, 2, 0, 2), R_BAD_CODE);
    REGS(&f)[1] = term_from_int(-1);
    CHECK_EQ(call(&f, B_INDEX, 2, 0, 2), R_BAD_CODE);
    REGS(&f)[1] = T_TRUE;                         /* not an int */
    CHECK_EQ(call(&f, B_INDEX, 2, 0, 2), R_BAD_CODE);
    REGS(&f)[0] = term_from_int(1);               /* not a string */
    REGS(&f)[1] = term_from_int(0);
    CHECK_EQ(call(&f, B_INDEX, 2, 0, 2), R_BAD_CODE);
    CHECK_EQ(call(&f, B_LEN, 2, 0, 1), R_BAD_CODE);
    CHECK_EQ(call(&f, B_LEN, 2, 0, 2), R_BAD_CODE);    /* wrong count */
    CHECK_EQ(call(&f, 99, 2, 0, 2), R_BAD_CODE);       /* not a sequence builtin */
    actor_free(&f.vm, f.a);
}

TEST(strs_slice) {
    Fx f; fx_init(&f, 64);
    set_str(&f, 0, "hello");
    term whole = REGS(&f)[0];
    REGS(&f)[1] = term_from_int(1); REGS(&f)[2] = term_from_int(3);
    CHECK_EQ(call(&f, B_SLICE, 3, 0, 3), R_NORMAL);
    CHECK(str_is(&f, 3, "el"));
    REGS(&f)[2] = term_from_int(-1);               /* negative hi is badcode, not len */
    CHECK_EQ(call(&f, B_SLICE, 3, 0, 3), R_BAD_CODE);
    REGS(&f)[2] = term_from_int(5);
    CHECK_EQ(call(&f, B_SLICE, 3, 0, 3), R_NORMAL);
    CHECK(str_is(&f, 3, "ello"));
    REGS(&f)[1] = term_from_int(0);
    CHECK_EQ(call(&f, B_SLICE, 3, 0, 3), R_NORMAL);
    CHECK_EQ(REGS(&f)[3], whole);                  /* all of it: shared */
    REGS(&f)[1] = term_from_int(5);
    CHECK_EQ(call(&f, B_SLICE, 3, 0, 3), R_NORMAL);
    CHECK_EQ(REGS(&f)[3], T_EMPTYSTR);
    REGS(&f)[1] = term_from_int(6);                /* lo > len */
    CHECK_EQ(call(&f, B_SLICE, 3, 0, 3), R_BAD_CODE);
    REGS(&f)[1] = term_from_int(3); REGS(&f)[2] = term_from_int(2);   /* lo > hi */
    CHECK_EQ(call(&f, B_SLICE, 3, 0, 3), R_BAD_CODE);
    REGS(&f)[1] = term_from_int(0); REGS(&f)[2] = term_from_int(6);   /* hi > len */
    CHECK_EQ(call(&f, B_SLICE, 3, 0, 3), R_BAD_CODE);
    REGS(&f)[1] = term_from_int(-2); REGS(&f)[2] = term_from_int(2);
    CHECK_EQ(call(&f, B_SLICE, 3, 0, 3), R_BAD_CODE);
    actor_free(&f.vm, f.a);
}

TEST(strs_cmp_and_equal) {
    Fx f; fx_init(&f, 64);
    set_str(&f, 0, "main");                        /* heap copy of the const "main" */
    REGS(&f)[1] = term_from_const(0);
    bool eq = false;
    CHECK(strs_equal(&f.vm, f.a, REGS(&f)[0], REGS(&f)[1], &eq) && eq);
    CHECK_EQ(call(&f, B_STR_CMP, 2, 0, 2), R_NORMAL);
    CHECK_EQ(REGS(&f)[2], term_from_int(0));
    set_str(&f, 1, "mai");
    CHECK(strs_equal(&f.vm, f.a, REGS(&f)[0], REGS(&f)[1], &eq) && !eq);
    CHECK_EQ(call(&f, B_STR_CMP, 2, 0, 2), R_NORMAL);   /* "main" > "mai" */
    CHECK_EQ(REGS(&f)[2], term_from_int(1));
    actor_free(&f.vm, f.a);
}

TEST(strs_cmp_empty_and_order) {
    Fx f; fx_init(&f, 64);
    REGS(&f)[0] = T_EMPTYSTR;
    set_str(&f, 1, "a");
    CHECK_EQ(call(&f, B_STR_CMP, 2, 0, 2), R_NORMAL);
    CHECK_EQ(REGS(&f)[2], term_from_int(-1));
    REGS(&f)[1] = T_EMPTYSTR;
    CHECK_EQ(call(&f, B_STR_CMP, 2, 0, 2), R_NORMAL);
    CHECK_EQ(REGS(&f)[2], term_from_int(0));
    set_str(&f, 0, "\xe4\xbd\xa0"); set_str(&f, 1, "z");   /* bytes compare unsigned */
    CHECK_EQ(call(&f, B_STR_CMP, 2, 0, 2), R_NORMAL);
    CHECK_EQ(REGS(&f)[2], term_from_int(1));
    bool eq;
    REGS(&f)[1] = T_TRUE;
    CHECK(!strs_equal(&f.vm, f.a, REGS(&f)[0], REGS(&f)[1], &eq));
    CHECK_EQ(call(&f, B_STR_CMP, 2, 0, 2), R_BAD_CODE);
    actor_free(&f.vm, f.a);
}

/* A concat that must collect: the block is full of garbage. The arguments
 * (registers) survive and the result has the right bytes. */
TEST(strs_concat_gc_and_oom) {
    Fx f; fx_init(&f, 128);
    set_str(&f, 0, "0123456789");
    set_str(&f, 1, "abcdefghij");
    for (int i = 0; i < 40; i++) {                 /* garbage: unreferenced strings */
        set_str(&f, 5, "garbagegarbagegarbage");
    }
    REGS(&f)[5] = T_NIL;
    for (int i = 0; i < 200; i++) {
        CHECK_EQ(call(&f, B_STR_CONCAT, 2, 0, 2), R_NORMAL);
        CHECK(str_is(&f, 2, "0123456789abcdefghij"));
        CHECK(str_is(&f, 0, "0123456789") && str_is(&f, 1, "abcdefghij"));
    }
    /* Doubling a string eventually exceeds the actor's limit: OutOfMemory,
     * with every earlier result intact. */
    f.a->max_words = 256;
    set_str(&f, 0, "0123456789012345678901234567890123456789");
    uint32_t len = 40;
    uint32_t err = R_NORMAL;
    for (int i = 0; i < 10 && err == R_NORMAL; i++) {
        REGS(&f)[1] = REGS(&f)[0];
        err = call(&f, B_STR_CONCAT, 2, 0, 2);
        if (err == R_NORMAL) {
            len *= 2;
            REGS(&f)[0] = REGS(&f)[2];
            const uint32_t *o = obj_of(&f.vm, f.a, REGS(&f)[0]);
            CHECK(o && o[1] == len);
        }
    }
    CHECK_EQ(err, R_OUT_OF_MEMORY);
    CHECK(len >= 160 && len <= 640);
    actor_free(&f.vm, f.a);
}

/* sys.Config through the unix store: a temp $GOGO_CONFIG of key=value
 * lines; "wifi." keys hidden; missing keys and files; the empty value; a
 * value bigger than the heap. */
TEST(strs_config_get) {
    char path[] = "/tmp/gogo-config-test-XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    FILE *fp = fdopen(fd, "w");
    CHECK(fp != NULL);
    fputs("# comment\nnet.ip=10.0.0.9\r\nempty=\nwifi.ssid=home\nnet.ipx=no\nk=a=b\nnet.ip=second\nbig=", fp);
    for (int i = 0; i < 600; i++) fputc('x', fp);
    fputs("\nlast=end", fp);   /* no final newline */
    fclose(fp);
    const char *old = getenv("GOGO_CONFIG");
    char *saved = old ? strdup(old) : NULL;
    setenv("GOGO_CONFIG", path, 1);

    Fx f; fx_init(&f, 128);
    struct { const char *key, *val; bool ok; } cases[] = {
        {"net.ip", "10.0.0.9", true}, {"empty", "", true}, {"k", "a=b", true}, {"last", "end", true},
        {"wifi.ssid", "", false}, {"net", "", false}, {"missing", "", false}, {"", "", false},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        if (cases[i].key[0]) set_str(&f, 0, cases[i].key);
        else REGS(&f)[0] = T_EMPTYSTR;
        REGS(&f)[2] = T_NIL;
        CHECK_EQ(call(&f, B_CONFIG_GET, 1, 0, 1), R_NORMAL);
        CHECK(str_is(&f, 1, cases[i].val));
        CHECK_EQ(REGS(&f)[2], term_from_bool(cases[i].ok));
    }
    set_str(&f, 0, "big");
    CHECK_EQ(call(&f, B_CONFIG_GET, 1, 0, 1), R_NORMAL);
    const uint32_t *o = obj_of(&f.vm, f.a, REGS(&f)[1]);
    CHECK(o && o[1] == 600 && REGS(&f)[2] == T_TRUE);
    f.a->max_words = 96;   /* 600 bytes do not fit */
    CHECK_EQ(call(&f, B_CONFIG_GET, 3, 0, 1), R_OUT_OF_MEMORY);
    f.a->max_words = 0xFFFF;
    REGS(&f)[0] = term_from_int(1);   /* not a string */
    CHECK_EQ(call(&f, B_CONFIG_GET, 1, 0, 1), R_BAD_CODE);
    remove(path);
    set_str(&f, 0, "net.ip");   /* no file */
    CHECK_EQ(call(&f, B_CONFIG_GET, 1, 0, 1), R_NORMAL);
    CHECK(REGS(&f)[1] == T_EMPTYSTR && REGS(&f)[2] == T_FALSE);
    actor_free(&f.vm, f.a);
    if (saved) { setenv("GOGO_CONFIG", saved, 1); free(saved); }
    else unsetenv("GOGO_CONFIG");
}
