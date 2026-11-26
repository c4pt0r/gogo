/* vfs core against the memfs fake backend: path rules, routing, the slot
 * table, Busy/Exists/replace rules, NoSpace recovery. */
#include "test.h"
#include "vfs.h"
#include "vfs_int.h"
#include "memfs.h"
#include <string.h>
#include <stdio.h>

#define S(x) (x), (uint32_t)strlen(x)

static VfsState vs;

static void setup(bool ci, bool atomic, int32_t budget) {
    vfs_reset(&vs);
    memfs_init(0, ci, atomic, budget);
    memfs_init(1, ci, atomic, -1);
    vfs_mount(&vs, "/flash", &memfs_be[0], memfs_ctx(0));
    vfs_mount(&vs, "/sd", &memfs_be[1], memfs_ctx(1));
}
static void setup_default(void) { setup(false, true, -1); }

static int put(const char *path, const char *data) {
    int s, e = vfs_open(&vs, S(path), VFS_WRITE, &s);
    if (e) return e;
    e = vfs_write(&vs, s, (const uint8_t *)data, (uint32_t)strlen(data));
    vfs_close(&vs, s);
    return e;
}
static int get(const char *path, char *out, uint32_t cap) {
    int s, e = vfs_open(&vs, S(path), VFS_READ, &s);
    uint32_t got = 0;
    if (e) return e;
    e = vfs_read(&vs, s, (uint8_t *)out, cap - 1, &got);
    out[got] = 0;
    vfs_close(&vs, s);
    return e;
}

TEST(vfs_roundtrip) {
    setup_default();
    char b[32];
    CHECK_EQ(put("/flash/a.txt", "hello"), GOGO_E_OK);
    CHECK_EQ(get("/flash/a.txt", b, sizeof b), GOGO_E_OK);
    CHECK(!strcmp(b, "hello"));
    int32_t sz; bool d;
    CHECK_EQ(vfs_stat(&vs, S("/flash/a.txt"), &sz, &d), GOGO_E_OK);
    CHECK_EQ(sz, 5);
    CHECK(!d);
    int s;
    CHECK_EQ(vfs_open(&vs, S("/flash/a.txt"), VFS_APPEND, &s), GOGO_E_OK);
    CHECK_EQ(vfs_slot_mode(&vs, s), VFS_APPEND);
    CHECK_EQ(vfs_write(&vs, s, (const uint8_t *)"!!", 2), GOGO_E_OK);
    CHECK_EQ(vfs_size(&vs, s, &sz), GOGO_E_OK);
    CHECK_EQ(sz, 7);
    CHECK_EQ(vfs_sync(&vs, s), GOGO_E_OK);
    vfs_close(&vs, s);
    CHECK_EQ(vfs_slot_mode(&vs, s), -1);
    CHECK_EQ(get("/flash/a.txt", b, sizeof b), GOGO_E_OK);
    CHECK(!strcmp(b, "hello!!"));
    CHECK_EQ(put("/flash/a.txt", "x"), GOGO_E_OK);     /* truncates */
    CHECK_EQ(get("/flash/a.txt", b, sizeof b), GOGO_E_OK);
    CHECK(!strcmp(b, "x"));
    CHECK_EQ(vfs_open(&vs, S("/flash/none"), VFS_READ, &s), GOGO_E_NOTFOUND);
}

TEST(vfs_path_rules) {
    setup_default();
    int32_t sz; bool d;
    /* the roots are valid directories */
    CHECK_EQ(vfs_stat(&vs, S("/flash"), &sz, &d), GOGO_E_OK);
    CHECK(d);
    CHECK_EQ(vfs_stat(&vs, S("/sd"), &sz, &d), GOGO_E_OK);
    CHECK_EQ(vfs_readdir(&vs, S("/flash"), 0, 0), GOGO_E_OK);
    /* whole-segment prefix */
    CHECK_EQ(vfs_stat(&vs, S("/flashx/a"), &sz, &d), GOGO_E_NOTMOUNTED);
    CHECK_EQ(vfs_stat(&vs, S("/flashx"), &sz, &d), GOGO_E_NOTMOUNTED);
    CHECK_EQ(vfs_stat(&vs, S("/other/a"), &sz, &d), GOGO_E_NOTMOUNTED);
    CHECK_EQ(vfs_stat(&vs, S("/flas"), &sz, &d), GOGO_E_NOTMOUNTED);
    /* bad paths */
    CHECK_EQ(vfs_stat(&vs, S("/flash/"), &sz, &d), GOGO_E_BADPATH);
    CHECK_EQ(vfs_stat(&vs, S("/flash//a"), &sz, &d), GOGO_E_BADPATH);
    CHECK_EQ(vfs_stat(&vs, S("/flash/a/.."), &sz, &d), GOGO_E_BADPATH);
    CHECK_EQ(vfs_stat(&vs, S("/flash/./a"), &sz, &d), GOGO_E_BADPATH);
    CHECK_EQ(vfs_stat(&vs, S("/flash/a/"), &sz, &d), GOGO_E_BADPATH);
    CHECK_EQ(vfs_stat(&vs, S("flash/a"), &sz, &d), GOGO_E_BADPATH);
    CHECK_EQ(vfs_stat(&vs, S(""), &sz, &d), GOGO_E_BADPATH);
    CHECK_EQ(vfs_stat(&vs, S("/"), &sz, &d), GOGO_E_BADPATH);
    CHECK_EQ(vfs_stat(&vs, S("/flash/a b"), &sz, &d), GOGO_E_BADPATH);
    CHECK_EQ(vfs_stat(&vs, S("/flash/a:b"), &sz, &d), GOGO_E_BADPATH);
    CHECK_EQ(vfs_stat(&vs, "/flash/a\0b", 11, &sz, &d), GOGO_E_BADPATH);
    CHECK_EQ(vfs_stat(&vs, "/flash/ab", 8, &sz, &d), GOGO_E_NOTFOUND);          /* len bounds the path, not NUL */
    /* names: 32 ok, 33 bad; dots in names ok */
    char p[128];
    strcpy(p, "/flash/");
    memset(p + 7, 'a', 32); p[39] = 0;
    CHECK_EQ(vfs_stat(&vs, S(p), &sz, &d), GOGO_E_NOTFOUND);
    memset(p + 7, 'a', 33); p[40] = 0;
    CHECK_EQ(vfs_stat(&vs, S(p), &sz, &d), GOGO_E_BADPATH);
    CHECK_EQ(vfs_stat(&vs, S("/flash/..a"), &sz, &d), GOGO_E_NOTFOUND);
    CHECK_EQ(vfs_stat(&vs, S("/flash/A-b_c.9"), &sz, &d), GOGO_E_NOTFOUND);
    /* total length: 96 ok, 97 bad (4 names of 22 + prefix) */
    strcpy(p, "/flash");
    size_t n = 6;
    while (n < 96) {
        size_t k = 96 - n - 1 > 20 ? 20 : 96 - n - 1;
        p[n++] = '/';
        memset(p + n, 'b', k); n += k;
    }
    p[n] = 0;
    CHECK_EQ((int)n, 96);
    CHECK_EQ(vfs_stat(&vs, S(p), &sz, &d), GOGO_E_NOTFOUND);          /* well-formed, parent missing */
    p[n] = 'b'; p[n + 1] = 0;
    CHECK_EQ(vfs_stat(&vs, S(p), &sz, &d), GOGO_E_BADPATH);
    /* ops on a root */
    CHECK_EQ(vfs_remove(&vs, S("/flash")), GOGO_E_BADPATH);
    CHECK_EQ(vfs_mkdir(&vs, S("/sd")), GOGO_E_BADPATH);
    int s;
    CHECK_EQ(vfs_open(&vs, S("/flash"), VFS_READ, &s), GOGO_E_ISDIR);
    CHECK_EQ(vfs_open(&vs, S("/flash/a"), 7, &s), GOGO_E_BADPATH);    /* bad mode */
}

TEST(vfs_routing) {
    setup_default();
    char b[16];
    CHECK_EQ(put("/flash/f", "F"), GOGO_E_OK);
    CHECK_EQ(put("/sd/f", "S"), GOGO_E_OK);
    CHECK_EQ(get("/flash/f", b, sizeof b), GOGO_E_OK);
    CHECK(!strcmp(b, "F"));
    CHECK_EQ(get("/sd/f", b, sizeof b), GOGO_E_OK);
    CHECK(!strcmp(b, "S"));
    /* only one mount: the other prefix is NotMounted */
    vfs_reset(&vs);
    memfs_init(0, false, true, -1);
    CHECK(vfs_mount(&vs, "/flash", &memfs_be[0], memfs_ctx(0)));
    int s;
    CHECK_EQ(vfs_open(&vs, S("/sd/f"), VFS_READ, &s), GOGO_E_NOTMOUNTED);
    CHECK(!vfs_mount(&vs, "/flash", &memfs_be[0], memfs_ctx(0)));                /* duplicate */
    CHECK(vfs_mount(&vs, "/sd", &memfs_be[1], memfs_ctx(1)));
    CHECK(!vfs_mount(&vs, "/x", &memfs_be[1], memfs_ctx(1)));                    /* third mount */
    vfs_reset(&vs);
    CHECK(!vfs_mount(&vs, "/flash/x", &memfs_be[0], memfs_ctx(0)));              /* prefix is one segment */
}

TEST(vfs_write_at) {
    setup_default();
    char b[32];
    int w, r;
    CHECK_EQ(vfs_open(&vs, S("/flash/a"), VFS_WRITE, &w), GOGO_E_OK);
    CHECK_EQ(vfs_write(&vs, w, (const uint8_t *)"hello", 5), GOGO_E_OK);
    CHECK_EQ(vfs_write_at(&vs, w, 1, (const uint8_t *)"EL", 2), GOGO_E_OK);            /* inside */
    CHECK_EQ(vfs_write_at(&vs, w, 0, (const uint8_t *)"HELLO", 5), GOGO_E_OK);         /* the whole file */
    CHECK_EQ(vfs_write_at(&vs, w, 3, (const uint8_t *)"abc", 3), GOGO_E_BADPATH);      /* past the end */
    CHECK_EQ(vfs_write_at(&vs, w, 6, (const uint8_t *)"x", 1), GOGO_E_BADPATH);
    CHECK_EQ(vfs_write_at(&vs, w, 5, (const uint8_t *)"x", 1), GOGO_E_BADPATH);        /* exactly at the end: extends */
    CHECK_EQ(vfs_write_at(&vs, w, 0xffffffffu, (const uint8_t *)"x", 2), GOGO_E_BADPATH);   /* off + n wraps */
    CHECK_EQ(vfs_write_at(&vs, w, 5, (const uint8_t *)"", 0), GOGO_E_OK);              /* nothing to write */
    CHECK_EQ(vfs_write(&vs, w, (const uint8_t *)"!", 1), GOGO_E_OK);                   /* still appends */
    CHECK_EQ(vfs_write_at(&vs, w, 5, (const uint8_t *)"?", 1), GOGO_E_OK);
    CHECK_EQ(vfs_write_at(&vs, w, 7, (const uint8_t *)"?", 1), GOGO_E_BADPATH);
    vfs_close(&vs, w);
    CHECK_EQ(get("/flash/a", b, sizeof b), GOGO_E_OK);
    CHECK(strcmp(b, "HELLO?") == 0);
    CHECK_EQ(vfs_open(&vs, S("/flash/a"), VFS_READ, &r), GOGO_E_OK);                   /* wrong mode */
    CHECK_EQ(vfs_write_at(&vs, r, 0, (const uint8_t *)"x", 1), GOGO_E_CLOSED);
    vfs_close(&vs, r);
    CHECK_EQ(vfs_open(&vs, S("/flash/a"), VFS_APPEND, &w), GOGO_E_OK);
    CHECK_EQ(vfs_write_at(&vs, w, 0, (const uint8_t *)"x", 1), GOGO_E_CLOSED);       /* Append refused */
    vfs_close(&vs, w);
    CHECK_EQ(vfs_write_at(&vs, w, 0, (const uint8_t *)"x", 1), GOGO_E_CLOSED);        /* a closed slot */
    CHECK_EQ(vfs_write_at(&vs, -1, 0, (const uint8_t *)"x", 1), GOGO_E_CLOSED);
}

TEST(vfs_single_writer) {
    setup_default();
    int w, w2, r, r2;
    CHECK_EQ(vfs_open(&vs, S("/flash/a"), VFS_WRITE, &w), GOGO_E_OK);
    CHECK_EQ(vfs_open(&vs, S("/flash/a"), VFS_WRITE, &w2), GOGO_E_BUSY);
    CHECK_EQ(vfs_open(&vs, S("/flash/a"), VFS_APPEND, &w2), GOGO_E_BUSY);
    CHECK_EQ(vfs_open(&vs, S("/flash/a"), VFS_READ, &r), GOGO_E_OK);
    CHECK_EQ(vfs_open(&vs, S("/flash/a"), VFS_READ, &r2), GOGO_E_OK);
    CHECK_EQ(vfs_open(&vs, S("/sd/a"), VFS_WRITE, &w2), GOGO_E_OK);   /* other mount, other file */
    vfs_close(&vs, w2); vfs_close(&vs, r); vfs_close(&vs, r2);
    vfs_close(&vs, w);
    CHECK_EQ(vfs_open(&vs, S("/flash/a"), VFS_APPEND, &w2), GOGO_E_OK);
    vfs_close(&vs, w2);
    /* a writer may open while readers are open */
    CHECK_EQ(vfs_open(&vs, S("/flash/a"), VFS_READ, &r), GOGO_E_OK);
    CHECK_EQ(vfs_open(&vs, S("/flash/a"), VFS_WRITE, &w), GOGO_E_OK);
    vfs_close(&vs, r); vfs_close(&vs, w);
}

/* Paths are the same when their length and hash both match. Two paths
 * whose 32-bit hashes collide but whose lengths differ are told apart;
 * a same-length collision is still taken for one path (Busy). */
TEST(vfs_hash_collision) {
    setup_default();
    static const char a[] = "/flash/h5wuja", b[] = "/flash/tgaaaaa";   /* lengths 13, 14 */
    static const char c[] = "/flash/btmaea", d[] = "/flash/7e09ja";    /* both 13 */
    CHECK_EQ(vfs_path_hash(S(a), false), vfs_path_hash(S(b), false));
    CHECK_EQ(vfs_path_hash(S(c), false), vfs_path_hash(S(d), false));
    int wa, wb, wc, wd;
    CHECK_EQ(vfs_open(&vs, S(a), VFS_WRITE, &wa), GOGO_E_OK);
    CHECK_EQ(vfs_open(&vs, S(b), VFS_WRITE, &wb), GOGO_E_OK);
    vfs_close(&vs, wb);
    CHECK_EQ(vfs_remove(&vs, S(b)), GOGO_E_OK);                           /* a is open, not b */
    vfs_close(&vs, wa);
    CHECK_EQ(vfs_open(&vs, S(c), VFS_WRITE, &wc), GOGO_E_OK);
    CHECK_EQ(vfs_open(&vs, S(d), VFS_WRITE, &wd), GOGO_E_BUSY);           /* the remaining risk */
    vfs_close(&vs, wc);
}

TEST(vfs_busy_ops) {
    setup_default();
    int s;
    CHECK_EQ(put("/flash/a", "1"), GOGO_E_OK);
    CHECK_EQ(put("/flash/b", "2"), GOGO_E_OK);
    CHECK_EQ(vfs_open(&vs, S("/flash/a"), VFS_READ, &s), GOGO_E_OK);
    CHECK_EQ(vfs_remove(&vs, S("/flash/a")), GOGO_E_BUSY);
    CHECK_EQ(vfs_rename(&vs, S("/flash/a"), S("/flash/c")), GOGO_E_BUSY);   /* source */
    CHECK_EQ(vfs_rename(&vs, S("/flash/b"), S("/flash/a")), GOGO_E_BUSY);   /* target */
    CHECK_EQ(vfs_replace(&vs, S("/flash/b"), S("/flash/a")), GOGO_E_BUSY);
    CHECK_EQ(vfs_replace(&vs, S("/flash/a"), S("/flash/b")), GOGO_E_BUSY);
    int32_t sz; bool d;
    CHECK_EQ(vfs_stat(&vs, S("/flash/a"), &sz, &d), GOGO_E_OK);             /* nothing happened */
    CHECK_EQ(vfs_stat(&vs, S("/flash/b"), &sz, &d), GOGO_E_OK);
    vfs_close(&vs, s);
    CHECK_EQ(vfs_rename(&vs, S("/flash/a"), S("/flash/c")), GOGO_E_OK);
    CHECK_EQ(vfs_remove(&vs, S("/flash/c")), GOGO_E_OK);
    CHECK_EQ(vfs_stat(&vs, S("/flash/c"), &sz, &d), GOGO_E_NOTFOUND);
    CHECK_EQ(vfs_remove(&vs, S("/flash/c")), GOGO_E_NOTFOUND);
}

TEST(vfs_rename_exists) {
    setup_default();
    char b[16];
    CHECK_EQ(put("/flash/a", "A"), GOGO_E_OK);
    CHECK_EQ(put("/flash/b", "B"), GOGO_E_OK);
    CHECK_EQ(vfs_rename(&vs, S("/flash/a"), S("/flash/b")), GOGO_E_EXISTS);
    CHECK_EQ(vfs_rename(&vs, S("/flash/a"), S("/flash/a")), GOGO_E_EXISTS);
    CHECK_EQ(get("/flash/b", b, sizeof b), GOGO_E_OK);
    CHECK(!strcmp(b, "B"));
    CHECK_EQ(vfs_rename(&vs, S("/flash/zz"), S("/flash/new")), GOGO_E_NOTFOUND);
    CHECK_EQ(vfs_rename(&vs, S("/flash/a"), S("/sd/a")), GOGO_E_BADPATH);   /* cross mount */
    CHECK_EQ(vfs_rename(&vs, S("/flash/a"), S("/nope/a")), GOGO_E_NOTMOUNTED);
    CHECK_EQ(vfs_rename(&vs, S("/flash/a"), S("/flash/n")), GOGO_E_OK);
}

TEST(vfs_replace_atomic) {
    setup(false, true, -1);
    char b[16];
    CHECK_EQ(put("/flash/f", "old"), GOGO_E_OK);
    CHECK_EQ(put("/flash/f.tmp", "new"), GOGO_E_OK);
    CHECK_EQ(vfs_replace(&vs, S("/flash/f.tmp"), S("/flash/f")), GOGO_E_OK);
    CHECK_EQ(memfs_rename_calls(0), 1);
    CHECK_EQ(get("/flash/f", b, sizeof b), GOGO_E_OK);
    CHECK(!strcmp(b, "new"));
    int32_t sz; bool d;
    CHECK_EQ(vfs_stat(&vs, S("/flash/f.tmp"), &sz, &d), GOGO_E_NOTFOUND);
    /* target absent */
    CHECK_EQ(put("/flash/g.tmp", "G"), GOGO_E_OK);
    CHECK_EQ(vfs_replace(&vs, S("/flash/g.tmp"), S("/flash/g")), GOGO_E_OK);
    CHECK_EQ(get("/flash/g", b, sizeof b), GOGO_E_OK);
    CHECK(!strcmp(b, "G"));
    CHECK_EQ(vfs_replace(&vs, S("/flash/none.tmp"), S("/flash/g")), GOGO_E_NOTFOUND);
}

TEST(vfs_replace_fat_like) {
    setup(false, false, -1);            /* rename(replace=true) returns Exists */
    char b[16];
    CHECK_EQ(put("/flash/f", "old"), GOGO_E_OK);
    CHECK_EQ(put("/flash/f.tmp", "new"), GOGO_E_OK);
    CHECK_EQ(vfs_replace(&vs, S("/flash/f.tmp"), S("/flash/f")), GOGO_E_OK);
    CHECK_EQ(memfs_rename_calls(0), 1);
    CHECK_EQ(get("/flash/f", b, sizeof b), GOGO_E_OK);
    CHECK(!strcmp(b, "new"));
    int32_t sz; bool d;
    CHECK_EQ(vfs_stat(&vs, S("/flash/f.tmp"), &sz, &d), GOGO_E_NOTFOUND);
    /* user Rename onto existing is Exists here too */
    CHECK_EQ(put("/flash/h", "h"), GOGO_E_OK);
    CHECK_EQ(vfs_rename(&vs, S("/flash/h"), S("/flash/f")), GOGO_E_EXISTS);
}

TEST(vfs_too_many) {
    setup_default();
    int s[GOGO_FS_MAX_OPEN + 1];
    char p[16];
    for (unsigned i = 0; i < GOGO_FS_MAX_OPEN; i++) {
        snprintf(p, sizeof p, "/flash/f%u", i);
        CHECK_EQ(vfs_open(&vs, S(p), VFS_WRITE, &s[i]), GOGO_E_OK);
    }
    CHECK_EQ(vfs_open(&vs, S("/flash/extra"), VFS_WRITE, &s[GOGO_FS_MAX_OPEN]), GOGO_E_TOOMANY);
    CHECK_EQ(vfs_open(&vs, S("/flash/extra"), VFS_READ, &s[GOGO_FS_MAX_OPEN]), GOGO_E_TOOMANY);
    vfs_close(&vs, s[1]);
    CHECK_EQ(vfs_open(&vs, S("/flash/extra"), VFS_WRITE, &s[1]), GOGO_E_OK);   /* freed slot reused */
    for (unsigned i = 0; i < GOGO_FS_MAX_OPEN; i++) vfs_close(&vs, s[i]);
}

TEST(vfs_closed_slots) {
    setup_default();
    int s;
    uint8_t b[4];
    uint32_t got;
    int32_t sz;
    CHECK_EQ(vfs_open(&vs, S("/flash/a"), VFS_WRITE, &s), GOGO_E_OK);
    CHECK_EQ(vfs_read(&vs, s, b, 4, &got), GOGO_E_CLOSED);          /* read on write mode */
    vfs_close(&vs, s);
    CHECK_EQ(vfs_write(&vs, s, b, 1), GOGO_E_CLOSED);               /* stale handle */
    CHECK_EQ(vfs_read(&vs, s, b, 1, &got), GOGO_E_CLOSED);
    CHECK_EQ(vfs_sync(&vs, s), GOGO_E_CLOSED);
    CHECK_EQ(vfs_size(&vs, s, &sz), GOGO_E_CLOSED);
    vfs_close(&vs, s);                                           /* double close harmless */
    CHECK_EQ(vfs_open(&vs, S("/flash/a"), VFS_READ, &s), GOGO_E_OK);
    CHECK_EQ(vfs_write(&vs, s, b, 1), GOGO_E_CLOSED);               /* write on read mode */
    vfs_close(&vs, s);
    CHECK_EQ(vfs_read(&vs, -1, b, 1, &got), GOGO_E_CLOSED);
    CHECK_EQ(vfs_write(&vs, 99, b, 1), GOGO_E_CLOSED);
    vfs_close(&vs, -5);
    vfs_close(&vs, 99);
}

TEST(vfs_nospace_recovery) {
    setup(false, true, 10);
    int s;
    int32_t used, total;
    CHECK_EQ(vfs_open(&vs, S("/flash/big"), VFS_WRITE, &s), GOGO_E_OK);
    CHECK_EQ(vfs_write(&vs, s, (const uint8_t *)"0123456789", 10), GOGO_E_OK);
    CHECK_EQ(vfs_write(&vs, s, (const uint8_t *)"x", 1), GOGO_E_NOSPACE);
    CHECK_EQ(vfs_sync(&vs, s), GOGO_E_OK);                          /* still usable */
    CHECK_EQ(vfs_usage(&vs, S("/flash"), &used, &total), GOGO_E_OK);
    CHECK_EQ(used, 10);
    CHECK_EQ(total, 10);
    vfs_close(&vs, s);
    int s2;
    CHECK_EQ(vfs_open(&vs, S("/flash/more"), VFS_WRITE, &s2), GOGO_E_OK);
    CHECK_EQ(vfs_write(&vs, s2, (const uint8_t *)"y", 1), GOGO_E_NOSPACE);
    vfs_close(&vs, s2);
    CHECK_EQ(vfs_remove(&vs, S("/flash/big")), GOGO_E_OK);
    CHECK_EQ(vfs_usage(&vs, S("/flash"), &used, &total), GOGO_E_OK);
    CHECK_EQ(used, 0);
    CHECK_EQ(put("/flash/more", "yyyy"), GOGO_E_OK);           /* space is back */
    CHECK_EQ(vfs_usage(&vs, S("/sd/a"), &used, &total), GOGO_E_OK);
    CHECK_EQ(total, -1);
}

TEST(vfs_case_insensitive) {
    setup(true, false, -1);
    int w, s;
    CHECK_EQ(vfs_open(&vs, S("/sd/a.txt"), VFS_WRITE, &w), GOGO_E_OK);
    CHECK_EQ(vfs_open(&vs, S("/sd/A.TXT"), VFS_WRITE, &s), GOGO_E_BUSY);
    CHECK_EQ(vfs_open(&vs, S("/sd/A.TXT"), VFS_APPEND, &s), GOGO_E_BUSY);
    CHECK_EQ(vfs_remove(&vs, S("/sd/A.TXT")), GOGO_E_BUSY);
    CHECK_EQ(vfs_rename(&vs, S("/sd/a.txt"), S("/sd/b.txt")), GOGO_E_BUSY);
    CHECK_EQ(vfs_rename(&vs, S("/sd/b.txt"), S("/sd/A.Txt")), GOGO_E_BUSY);
    vfs_close(&vs, w);
    CHECK_EQ(vfs_open(&vs, S("/sd/A.TXT"), VFS_READ, &s), GOGO_E_OK);
    /* FAT: no Write (truncate) under a reader; Append is fine */
    CHECK_EQ(vfs_open(&vs, S("/sd/a.txt"), VFS_WRITE, &w), GOGO_E_BUSY);
    CHECK_EQ(vfs_open(&vs, S("/sd/a.txt"), VFS_APPEND, &w), GOGO_E_OK);
    vfs_close(&vs, w);
    vfs_close(&vs, s);
    CHECK_EQ(vfs_open(&vs, S("/sd/a.txt"), VFS_WRITE, &w), GOGO_E_OK);
    vfs_close(&vs, w);
    /* case-sensitive backend: different names */
    setup(false, true, -1);
    CHECK_EQ(vfs_open(&vs, S("/flash/a.txt"), VFS_WRITE, &w), GOGO_E_OK);
    CHECK_EQ(vfs_open(&vs, S("/flash/A.TXT"), VFS_WRITE, &s), GOGO_E_OK);
    vfs_close(&vs, w); vfs_close(&vs, s);
    /* ...and a Write open under a reader is allowed there */
    CHECK_EQ(vfs_open(&vs, S("/flash/a.txt"), VFS_READ, &s), GOGO_E_OK);
    CHECK_EQ(vfs_open(&vs, S("/flash/a.txt"), VFS_WRITE, &w), GOGO_E_OK);
    vfs_close(&vs, w); vfs_close(&vs, s);
}

static int count_cb(void *ctx, const char *name, bool is_dir) {
    (void)name; (void)is_dir;
    (*(int *)ctx)++;
    return 0;
}

TEST(vfs_dirs) {
    setup_default();
    int n = 0;
    int32_t sz; bool d;
    CHECK_EQ(vfs_mkdir(&vs, S("/flash/d")), GOGO_E_OK);
    CHECK_EQ(vfs_mkdir(&vs, S("/flash/d")), GOGO_E_EXISTS);
    CHECK_EQ(vfs_mkdir(&vs, S("/flash/x/y")), GOGO_E_NOTFOUND);
    CHECK_EQ(put("/flash/d/f", "1"), GOGO_E_OK);
    CHECK_EQ(put("/flash/top", "1"), GOGO_E_OK);
    CHECK_EQ(vfs_readdir(&vs, S("/flash"), count_cb, &n), GOGO_E_OK);
    CHECK_EQ(n, 2);
    n = 0;
    CHECK_EQ(vfs_readdir(&vs, S("/flash/d"), count_cb, &n), GOGO_E_OK);
    CHECK_EQ(n, 1);
    CHECK_EQ(vfs_readdir(&vs, S("/flash/top"), count_cb, &n), GOGO_E_NOTDIR);
    CHECK_EQ(vfs_readdir(&vs, S("/flash/none"), count_cb, &n), GOGO_E_NOTFOUND);
    CHECK_EQ(vfs_remove(&vs, S("/flash/d")), GOGO_E_NOTEMPTY);
    CHECK_EQ(vfs_remove(&vs, S("/flash/d/f")), GOGO_E_OK);
    CHECK_EQ(vfs_remove(&vs, S("/flash/d")), GOGO_E_OK);
    CHECK_EQ(vfs_stat(&vs, S("/flash/d"), &sz, &d), GOGO_E_NOTFOUND);
    int s;
    CHECK_EQ(vfs_mkdir(&vs, S("/flash/e")), GOGO_E_OK);
    CHECK_EQ(vfs_open(&vs, S("/flash/e"), VFS_READ, &s), GOGO_E_ISDIR);
}

/* Rename onto a target whose stat fails with anything but NotFound
 * propagates the error instead of treating the target as absent. */
TEST(vfs_rename_stat_error) {
    setup_default();
    CHECK_EQ(put("/flash/a", "1"), GOGO_E_OK);
    memfs_set_stat_err(0, GOGO_E_IO);
    CHECK_EQ(vfs_rename(&vs, S("/flash/a"), S("/flash/b")), GOGO_E_IO);
    memfs_set_stat_err(0, GOGO_E_NOTFOUND);
    CHECK_EQ(vfs_rename(&vs, S("/flash/a"), S("/flash/b")), GOGO_E_OK);   /* absent: renamed */
    memfs_set_stat_err(0, 0);
    int32_t sz; bool d;
    CHECK_EQ(vfs_stat(&vs, S("/flash/b"), &sz, &d), GOGO_E_OK);
}

/* A backend close that fails still releases the slot. */
TEST(vfs_close_failure_releases_slot) {
    setup_default();
    int s, t;
    memfs_set_close_fails(0, true);
    CHECK_EQ(vfs_open(&vs, S("/flash/a"), VFS_WRITE, &s), GOGO_E_OK);
    vfs_close(&vs, s);
    CHECK_EQ(vfs_slot_mode(&vs, s), -1);
    CHECK_EQ(vfs_open(&vs, S("/flash/a"), VFS_WRITE, &t), GOGO_E_OK);     /* no stale Busy */
    vfs_close(&vs, t);
    CHECK_EQ(vfs_remove(&vs, S("/flash/a")), GOGO_E_OK);
    memfs_set_close_fails(0, false);
}

/* On a case-insensitive backend a reader opened under another case blocks
 * Remove and Rename of the file. */
TEST(vfs_mixed_case_reader_blocks_remove) {
    setup(true, false, -1);
    CHECK_EQ(put("/sd/a.txt", "x"), GOGO_E_OK);
    int r;
    CHECK_EQ(vfs_open(&vs, S("/sd/A.TXT"), VFS_READ, &r), GOGO_E_OK);
    CHECK_EQ(vfs_remove(&vs, S("/sd/a.txt")), GOGO_E_BUSY);
    CHECK_EQ(vfs_rename(&vs, S("/sd/a.txt"), S("/sd/b.txt")), GOGO_E_BUSY);
    vfs_close(&vs, r);
    CHECK_EQ(vfs_remove(&vs, S("/sd/a.txt")), GOGO_E_OK);
}

/* A name ending in '.' is BadPath in any segment, on every mount: FatFs
 * strips trailing dots, so "a.txt." would alias "a.txt" past the slot
 * hash. Dots elsewhere in a name stay fine. */
TEST(vfs_trailing_dot) {
    setup_default();
    int32_t sz; bool d;
    int w, s;
    CHECK_EQ(vfs_stat(&vs, S("/flash/a."), &sz, &d), GOGO_E_BADPATH);
    CHECK_EQ(vfs_stat(&vs, S("/flash/a.txt.."), &sz, &d), GOGO_E_BADPATH);
    CHECK_EQ(vfs_stat(&vs, S("/flash/..."), &sz, &d), GOGO_E_BADPATH);
    CHECK_EQ(vfs_stat(&vs, S("/flash/d./x"), &sz, &d), GOGO_E_BADPATH);
    CHECK_EQ(vfs_stat(&vs, S("/sd/a.txt."), &sz, &d), GOGO_E_BADPATH);
    CHECK_EQ(vfs_mkdir(&vs, S("/sd/d.")), GOGO_E_BADPATH);
    CHECK_EQ(vfs_stat(&vs, S("/flash/.a"), &sz, &d), GOGO_E_NOTFOUND);
    CHECK_EQ(vfs_stat(&vs, S("/flash/a.b.c"), &sz, &d), GOGO_E_NOTFOUND);
    CHECK_EQ(vfs_open(&vs, S("/flash/a.txt"), VFS_WRITE, &w), GOGO_E_OK);
    CHECK_EQ(vfs_open(&vs, S("/flash/a.txt."), VFS_WRITE, &s), GOGO_E_BADPATH);
    CHECK_EQ(vfs_remove(&vs, S("/flash/a.txt.")), GOGO_E_BADPATH);
    CHECK_EQ(vfs_rename(&vs, S("/flash/b"), S("/flash/a.txt.")), GOGO_E_BADPATH);
    vfs_close(&vs, w);
    CHECK(!vfs_mount(&vs, "/x.", &memfs_be[0], memfs_ctx(0)));
}

/* Rename into its own subtree is BadPath (LittleFS and FatFs would unlink
 * the directory and leak its blocks). The match folds case only on a
 * case-insensitive mount; a case-only rename there reaches the backend. */
TEST(vfs_rename_into_subtree) {
    setup_default();
    int32_t sz; bool d;
    char b[8];
    CHECK_EQ(vfs_mkdir(&vs, S("/flash/p")), GOGO_E_OK);
    CHECK_EQ(vfs_rename(&vs, S("/flash/p"), S("/flash/p/q")), GOGO_E_BADPATH);
    CHECK_EQ(vfs_rename(&vs, S("/flash/p"), S("/flash/p/q/r")), GOGO_E_BADPATH);
    CHECK_EQ(vfs_rename(&vs, S("/flash/p"), S("/flash/p")), GOGO_E_EXISTS);
    CHECK_EQ(vfs_rename(&vs, S("/flash/p"), S("/flash/P/q")), GOGO_E_NOTFOUND);  /* exact on /flash: no P */
    CHECK_EQ(vfs_stat(&vs, S("/flash/p"), &sz, &d), GOGO_E_OK);
    CHECK(d);
    CHECK_EQ(vfs_rename(&vs, S("/flash/p"), S("/flash/pq")), GOGO_E_OK);         /* not a subtree */
    CHECK_EQ(vfs_rename(&vs, S("/flash/pq"), S("/flash/P")), GOGO_E_OK);         /* case-only rename */
    CHECK_EQ(vfs_rename(&vs, S("/flash/P"), S("/flash/p")), GOGO_E_OK);
    CHECK_EQ(vfs_stat(&vs, S("/flash/P"), &sz, &d), GOGO_E_NOTFOUND);
    setup(true, false, -1);
    CHECK_EQ(vfs_mkdir(&vs, S("/sd/d")), GOGO_E_OK);
    CHECK_EQ(vfs_rename(&vs, S("/sd/d"), S("/sd/D/x")), GOGO_E_BADPATH);
    CHECK_EQ(vfs_rename(&vs, S("/sd/d"), S("/sd/d/x")), GOGO_E_BADPATH);
    CHECK_EQ(vfs_rename(&vs, S("/sd/d"), S("/sd/d")), GOGO_E_EXISTS);
    CHECK_EQ(put("/sd/a.txt", "A"), GOGO_E_OK);
    CHECK_EQ(vfs_rename(&vs, S("/sd/a.txt"), S("/sd/A.TXT")), GOGO_E_OK);       /* case-only: to the backend */
    CHECK_EQ(memfs_name(1, "/a.txt", b, sizeof b), GOGO_E_OK);
    CHECK(!strcmp(b, "/A.TXT"));
    CHECK_EQ(vfs_rename(&vs, S("/sd/d"), S("/sd/dir2")), GOGO_E_OK);
}

/* Renaming a directory while any file is open on that mount is Busy: the
 * slots keep only a hash of the path they were opened under, so a renamed
 * parent would let a second writer or a Remove reach the open file. */
TEST(vfs_rename_dir_with_open_file) {
    setup_default();
    int w, w2;
    int32_t sz; bool d;
    CHECK_EQ(vfs_mkdir(&vs, S("/flash/d")), GOGO_E_OK);
    CHECK_EQ(vfs_mkdir(&vs, S("/flash/other")), GOGO_E_OK);
    CHECK_EQ(vfs_open(&vs, S("/flash/d/x"), VFS_WRITE, &w), GOGO_E_OK);
    CHECK_EQ(vfs_rename(&vs, S("/flash/d"), S("/flash/e")), GOGO_E_BUSY);
    CHECK_EQ(vfs_rename(&vs, S("/flash/other"), S("/flash/o2")), GOGO_E_BUSY);   /* any open file on the mount */
    CHECK_EQ(vfs_stat(&vs, S("/flash/d"), &sz, &d), GOGO_E_OK);
    CHECK_EQ(vfs_open(&vs, S("/flash/d/x"), VFS_APPEND, &w2), GOGO_E_BUSY);
    CHECK_EQ(vfs_remove(&vs, S("/flash/d/x")), GOGO_E_BUSY);
    /* a plain file still renames beside an open file */
    CHECK_EQ(put("/flash/f", "1"), GOGO_E_OK);
    CHECK_EQ(vfs_rename(&vs, S("/flash/f"), S("/flash/g")), GOGO_E_OK);
    /* a file open on the other mount does not block */
    CHECK_EQ(vfs_mkdir(&vs, S("/sd/s")), GOGO_E_OK);
    CHECK_EQ(vfs_rename(&vs, S("/sd/s"), S("/sd/t")), GOGO_E_OK);
    vfs_close(&vs, w);
    CHECK_EQ(vfs_open(&vs, S("/sd/f"), VFS_WRITE, &w), GOGO_E_OK);
    CHECK_EQ(vfs_rename(&vs, S("/flash/d"), S("/flash/e")), GOGO_E_OK);
    vfs_close(&vs, w);
}

/* Two VfsStates (two instances) on the same backend table with different
 * ctx: neither sees the other's mounts, files or slots. */
TEST(vfs_two_states_isolated) {
    static VfsState s1, s2;
    memfs_init(0, false, true, -1);
    memfs_init(1, false, true, -1);
    vfs_reset(&s1);
    vfs_reset(&s2);
    CHECK(vfs_mount(&s1, "/flash", &memfs_be[0], memfs_ctx(0)));
    CHECK(vfs_mount(&s2, "/flash", &memfs_be[1], memfs_ctx(1)));
    CHECK(vfs_mount(&s2, "/sd", &memfs_be[0], memfs_ctx(0)));
    int a, b;
    int32_t sz;
    bool d;
    CHECK_EQ(vfs_open(&s1, S("/flash/a"), VFS_WRITE, &a), GOGO_E_OK);
    CHECK_EQ(vfs_write(&s1, a, (const uint8_t *)"one", 3), GOGO_E_OK);
    CHECK_EQ(vfs_open(&s2, S("/flash/a"), VFS_WRITE, &b), GOGO_E_OK);   /* no Busy: another medium, another table */
    CHECK_EQ(vfs_write(&s2, b, (const uint8_t *)"two!", 4), GOGO_E_OK);
    CHECK_EQ(a, 0);
    CHECK_EQ(b, 0);                                                     /* each has its own slot 0 */
    vfs_close(&s1, a);
    CHECK_EQ(vfs_slot_mode(&s2, b), VFS_WRITE);                         /* s1's close left s2 alone */
    vfs_close(&s2, b);
    CHECK_EQ(vfs_stat(&s1, S("/flash/a"), &sz, &d), GOGO_E_OK);
    CHECK_EQ(sz, 3);
    CHECK_EQ(vfs_stat(&s2, S("/flash/a"), &sz, &d), GOGO_E_OK);
    CHECK_EQ(sz, 4);
    CHECK_EQ(vfs_stat(&s2, S("/sd/a"), &sz, &d), GOGO_E_OK);              /* s1's medium, mounted by s2 at /sd */
    CHECK_EQ(sz, 3);
    CHECK_EQ(vfs_stat(&s1, S("/sd/a"), &sz, &d), GOGO_E_NOTMOUNTED);
    vfs_reset(&s1);
    CHECK_EQ(vfs_stat(&s1, S("/flash/a"), &sz, &d), GOGO_E_NOTMOUNTED);
    CHECK_EQ(vfs_stat(&s2, S("/flash/a"), &sz, &d), GOGO_E_OK);
    CHECK_EQ(vfs_state_bytes(), (uint32_t)sizeof(VfsState));
}
