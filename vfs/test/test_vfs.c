/* vfs core against the memfs fake backend: path rules, routing, the slot
 * table, Busy/Exists/replace rules, NoSpace recovery. */
#include "test.h"
#include "vfs.h"
#include "memfs.h"
#include <string.h>
#include <stdio.h>

#define S(x) (x), (uint32_t)strlen(x)

static void setup(bool ci, bool atomic, int32_t budget) {
    vfs_reset();
    memfs_init(0, ci, atomic, budget);
    memfs_init(1, ci, atomic, -1);
    vfs_mount("/flash", &memfs_be[0]);
    vfs_mount("/sd", &memfs_be[1]);
}
static void setup_default(void) { setup(false, true, -1); }

static int put(const char *path, const char *data) {
    int s, e = vfs_open(S(path), VFS_WRITE, &s);
    if (e) return e;
    e = vfs_write(s, (const uint8_t *)data, (uint32_t)strlen(data));
    vfs_close(s);
    return e;
}
static int get(const char *path, char *out, uint32_t cap) {
    int s, e = vfs_open(S(path), VFS_READ, &s);
    uint32_t got = 0;
    if (e) return e;
    e = vfs_read(s, (uint8_t *)out, cap - 1, &got);
    out[got] = 0;
    vfs_close(s);
    return e;
}

TEST(vfs_roundtrip) {
    setup_default();
    char b[32];
    CHECK_EQ(put("/flash/a.txt", "hello"), VFS_OK);
    CHECK_EQ(get("/flash/a.txt", b, sizeof b), VFS_OK);
    CHECK(!strcmp(b, "hello"));
    int32_t sz; bool d;
    CHECK_EQ(vfs_stat(S("/flash/a.txt"), &sz, &d), VFS_OK);
    CHECK_EQ(sz, 5);
    CHECK(!d);
    int s;
    CHECK_EQ(vfs_open(S("/flash/a.txt"), VFS_APPEND, &s), VFS_OK);
    CHECK_EQ(vfs_slot_mode(s), VFS_APPEND);
    CHECK_EQ(vfs_write(s, (const uint8_t *)"!!", 2), VFS_OK);
    CHECK_EQ(vfs_size(s, &sz), VFS_OK);
    CHECK_EQ(sz, 7);
    CHECK_EQ(vfs_sync(s), VFS_OK);
    vfs_close(s);
    CHECK_EQ(vfs_slot_mode(s), -1);
    CHECK_EQ(get("/flash/a.txt", b, sizeof b), VFS_OK);
    CHECK(!strcmp(b, "hello!!"));
    CHECK_EQ(put("/flash/a.txt", "x"), VFS_OK);     /* truncates */
    CHECK_EQ(get("/flash/a.txt", b, sizeof b), VFS_OK);
    CHECK(!strcmp(b, "x"));
    CHECK_EQ(vfs_open(S("/flash/none"), VFS_READ, &s), VFS_NOTFOUND);
}

TEST(vfs_path_rules) {
    setup_default();
    int32_t sz; bool d;
    /* the roots are valid directories */
    CHECK_EQ(vfs_stat(S("/flash"), &sz, &d), VFS_OK);
    CHECK(d);
    CHECK_EQ(vfs_stat(S("/sd"), &sz, &d), VFS_OK);
    CHECK_EQ(vfs_readdir(S("/flash"), 0, 0), VFS_OK);
    /* whole-segment prefix */
    CHECK_EQ(vfs_stat(S("/flashx/a"), &sz, &d), VFS_NOTMOUNTED);
    CHECK_EQ(vfs_stat(S("/flashx"), &sz, &d), VFS_NOTMOUNTED);
    CHECK_EQ(vfs_stat(S("/other/a"), &sz, &d), VFS_NOTMOUNTED);
    CHECK_EQ(vfs_stat(S("/flas"), &sz, &d), VFS_NOTMOUNTED);
    /* bad paths */
    CHECK_EQ(vfs_stat(S("/flash/"), &sz, &d), VFS_BADPATH);
    CHECK_EQ(vfs_stat(S("/flash//a"), &sz, &d), VFS_BADPATH);
    CHECK_EQ(vfs_stat(S("/flash/a/.."), &sz, &d), VFS_BADPATH);
    CHECK_EQ(vfs_stat(S("/flash/./a"), &sz, &d), VFS_BADPATH);
    CHECK_EQ(vfs_stat(S("/flash/a/"), &sz, &d), VFS_BADPATH);
    CHECK_EQ(vfs_stat(S("flash/a"), &sz, &d), VFS_BADPATH);
    CHECK_EQ(vfs_stat(S(""), &sz, &d), VFS_BADPATH);
    CHECK_EQ(vfs_stat(S("/"), &sz, &d), VFS_BADPATH);
    CHECK_EQ(vfs_stat(S("/flash/a b"), &sz, &d), VFS_BADPATH);
    CHECK_EQ(vfs_stat(S("/flash/a:b"), &sz, &d), VFS_BADPATH);
    CHECK_EQ(vfs_stat("/flash/a\0b", 11, &sz, &d), VFS_BADPATH);
    CHECK_EQ(vfs_stat("/flash/ab", 8, &sz, &d), VFS_NOTFOUND);          /* len bounds the path, not NUL */
    /* names: 32 ok, 33 bad; dots in names ok */
    char p[128];
    strcpy(p, "/flash/");
    memset(p + 7, 'a', 32); p[39] = 0;
    CHECK_EQ(vfs_stat(S(p), &sz, &d), VFS_NOTFOUND);
    memset(p + 7, 'a', 33); p[40] = 0;
    CHECK_EQ(vfs_stat(S(p), &sz, &d), VFS_BADPATH);
    CHECK_EQ(vfs_stat(S("/flash/..a"), &sz, &d), VFS_NOTFOUND);
    CHECK_EQ(vfs_stat(S("/flash/A-b_c.9"), &sz, &d), VFS_NOTFOUND);
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
    CHECK_EQ(vfs_stat(S(p), &sz, &d), VFS_NOTFOUND);          /* well-formed, parent missing */
    p[n] = 'b'; p[n + 1] = 0;
    CHECK_EQ(vfs_stat(S(p), &sz, &d), VFS_BADPATH);
    /* ops on a root */
    CHECK_EQ(vfs_remove(S("/flash")), VFS_BADPATH);
    CHECK_EQ(vfs_mkdir(S("/sd")), VFS_BADPATH);
    int s;
    CHECK_EQ(vfs_open(S("/flash"), VFS_READ, &s), VFS_ISDIR);
    CHECK_EQ(vfs_open(S("/flash/a"), 7, &s), VFS_BADPATH);    /* bad mode */
}

TEST(vfs_routing) {
    setup_default();
    char b[16];
    CHECK_EQ(put("/flash/f", "F"), VFS_OK);
    CHECK_EQ(put("/sd/f", "S"), VFS_OK);
    CHECK_EQ(get("/flash/f", b, sizeof b), VFS_OK);
    CHECK(!strcmp(b, "F"));
    CHECK_EQ(get("/sd/f", b, sizeof b), VFS_OK);
    CHECK(!strcmp(b, "S"));
    /* only one mount: the other prefix is NotMounted */
    vfs_reset();
    memfs_init(0, false, true, -1);
    CHECK(vfs_mount("/flash", &memfs_be[0]));
    int s;
    CHECK_EQ(vfs_open(S("/sd/f"), VFS_READ, &s), VFS_NOTMOUNTED);
    CHECK(!vfs_mount("/flash", &memfs_be[0]));                /* duplicate */
    CHECK(vfs_mount("/sd", &memfs_be[1]));
    CHECK(!vfs_mount("/x", &memfs_be[1]));                    /* third mount */
    vfs_reset();
    CHECK(!vfs_mount("/flash/x", &memfs_be[0]));              /* prefix is one segment */
}

TEST(vfs_single_writer) {
    setup_default();
    int w, w2, r, r2;
    CHECK_EQ(vfs_open(S("/flash/a"), VFS_WRITE, &w), VFS_OK);
    CHECK_EQ(vfs_open(S("/flash/a"), VFS_WRITE, &w2), VFS_BUSY);
    CHECK_EQ(vfs_open(S("/flash/a"), VFS_APPEND, &w2), VFS_BUSY);
    CHECK_EQ(vfs_open(S("/flash/a"), VFS_READ, &r), VFS_OK);
    CHECK_EQ(vfs_open(S("/flash/a"), VFS_READ, &r2), VFS_OK);
    CHECK_EQ(vfs_open(S("/sd/a"), VFS_WRITE, &w2), VFS_OK);   /* other mount, other file */
    vfs_close(w2); vfs_close(r); vfs_close(r2);
    vfs_close(w);
    CHECK_EQ(vfs_open(S("/flash/a"), VFS_APPEND, &w2), VFS_OK);
    vfs_close(w2);
    /* a writer may open while readers are open */
    CHECK_EQ(vfs_open(S("/flash/a"), VFS_READ, &r), VFS_OK);
    CHECK_EQ(vfs_open(S("/flash/a"), VFS_WRITE, &w), VFS_OK);
    vfs_close(r); vfs_close(w);
}

TEST(vfs_busy_ops) {
    setup_default();
    int s;
    CHECK_EQ(put("/flash/a", "1"), VFS_OK);
    CHECK_EQ(put("/flash/b", "2"), VFS_OK);
    CHECK_EQ(vfs_open(S("/flash/a"), VFS_READ, &s), VFS_OK);
    CHECK_EQ(vfs_remove(S("/flash/a")), VFS_BUSY);
    CHECK_EQ(vfs_rename(S("/flash/a"), S("/flash/c")), VFS_BUSY);   /* source */
    CHECK_EQ(vfs_rename(S("/flash/b"), S("/flash/a")), VFS_BUSY);   /* target */
    CHECK_EQ(vfs_replace(S("/flash/b"), S("/flash/a")), VFS_BUSY);
    CHECK_EQ(vfs_replace(S("/flash/a"), S("/flash/b")), VFS_BUSY);
    int32_t sz; bool d;
    CHECK_EQ(vfs_stat(S("/flash/a"), &sz, &d), VFS_OK);             /* nothing happened */
    CHECK_EQ(vfs_stat(S("/flash/b"), &sz, &d), VFS_OK);
    vfs_close(s);
    CHECK_EQ(vfs_rename(S("/flash/a"), S("/flash/c")), VFS_OK);
    CHECK_EQ(vfs_remove(S("/flash/c")), VFS_OK);
    CHECK_EQ(vfs_stat(S("/flash/c"), &sz, &d), VFS_NOTFOUND);
    CHECK_EQ(vfs_remove(S("/flash/c")), VFS_NOTFOUND);
}

TEST(vfs_rename_exists) {
    setup_default();
    char b[16];
    CHECK_EQ(put("/flash/a", "A"), VFS_OK);
    CHECK_EQ(put("/flash/b", "B"), VFS_OK);
    CHECK_EQ(vfs_rename(S("/flash/a"), S("/flash/b")), VFS_EXISTS);
    CHECK_EQ(vfs_rename(S("/flash/a"), S("/flash/a")), VFS_EXISTS);
    CHECK_EQ(get("/flash/b", b, sizeof b), VFS_OK);
    CHECK(!strcmp(b, "B"));
    CHECK_EQ(vfs_rename(S("/flash/zz"), S("/flash/new")), VFS_NOTFOUND);
    CHECK_EQ(vfs_rename(S("/flash/a"), S("/sd/a")), VFS_BADPATH);   /* cross mount */
    CHECK_EQ(vfs_rename(S("/flash/a"), S("/nope/a")), VFS_NOTMOUNTED);
    CHECK_EQ(vfs_rename(S("/flash/a"), S("/flash/n")), VFS_OK);
}

TEST(vfs_replace_atomic) {
    setup(false, true, -1);
    char b[16];
    CHECK_EQ(put("/flash/f", "old"), VFS_OK);
    CHECK_EQ(put("/flash/f.tmp", "new"), VFS_OK);
    CHECK_EQ(vfs_replace(S("/flash/f.tmp"), S("/flash/f")), VFS_OK);
    CHECK_EQ(memfs_rename_calls(0), 1);
    CHECK_EQ(get("/flash/f", b, sizeof b), VFS_OK);
    CHECK(!strcmp(b, "new"));
    int32_t sz; bool d;
    CHECK_EQ(vfs_stat(S("/flash/f.tmp"), &sz, &d), VFS_NOTFOUND);
    /* target absent */
    CHECK_EQ(put("/flash/g.tmp", "G"), VFS_OK);
    CHECK_EQ(vfs_replace(S("/flash/g.tmp"), S("/flash/g")), VFS_OK);
    CHECK_EQ(get("/flash/g", b, sizeof b), VFS_OK);
    CHECK(!strcmp(b, "G"));
    CHECK_EQ(vfs_replace(S("/flash/none.tmp"), S("/flash/g")), VFS_NOTFOUND);
}

TEST(vfs_replace_fat_like) {
    setup(false, false, -1);            /* rename(replace=true) returns Exists */
    char b[16];
    CHECK_EQ(put("/flash/f", "old"), VFS_OK);
    CHECK_EQ(put("/flash/f.tmp", "new"), VFS_OK);
    CHECK_EQ(vfs_replace(S("/flash/f.tmp"), S("/flash/f")), VFS_OK);
    CHECK_EQ(memfs_rename_calls(0), 1);
    CHECK_EQ(get("/flash/f", b, sizeof b), VFS_OK);
    CHECK(!strcmp(b, "new"));
    int32_t sz; bool d;
    CHECK_EQ(vfs_stat(S("/flash/f.tmp"), &sz, &d), VFS_NOTFOUND);
    /* user Rename onto existing is Exists here too */
    CHECK_EQ(put("/flash/h", "h"), VFS_OK);
    CHECK_EQ(vfs_rename(S("/flash/h"), S("/flash/f")), VFS_EXISTS);
}

TEST(vfs_too_many) {
    setup_default();
    int s[GOGO_FS_MAX_OPEN + 1];
    char p[16];
    for (unsigned i = 0; i < GOGO_FS_MAX_OPEN; i++) {
        snprintf(p, sizeof p, "/flash/f%u", i);
        CHECK_EQ(vfs_open(S(p), VFS_WRITE, &s[i]), VFS_OK);
    }
    CHECK_EQ(vfs_open(S("/flash/extra"), VFS_WRITE, &s[GOGO_FS_MAX_OPEN]), VFS_TOOMANY);
    CHECK_EQ(vfs_open(S("/flash/extra"), VFS_READ, &s[GOGO_FS_MAX_OPEN]), VFS_TOOMANY);
    vfs_close(s[1]);
    CHECK_EQ(vfs_open(S("/flash/extra"), VFS_WRITE, &s[1]), VFS_OK);   /* freed slot reused */
    for (unsigned i = 0; i < GOGO_FS_MAX_OPEN; i++) vfs_close(s[i]);
}

TEST(vfs_closed_slots) {
    setup_default();
    int s;
    uint8_t b[4];
    uint32_t got;
    int32_t sz;
    CHECK_EQ(vfs_open(S("/flash/a"), VFS_WRITE, &s), VFS_OK);
    CHECK_EQ(vfs_read(s, b, 4, &got), VFS_CLOSED);          /* read on write mode */
    vfs_close(s);
    CHECK_EQ(vfs_write(s, b, 1), VFS_CLOSED);               /* stale handle */
    CHECK_EQ(vfs_read(s, b, 1, &got), VFS_CLOSED);
    CHECK_EQ(vfs_sync(s), VFS_CLOSED);
    CHECK_EQ(vfs_size(s, &sz), VFS_CLOSED);
    vfs_close(s);                                           /* double close harmless */
    CHECK_EQ(vfs_open(S("/flash/a"), VFS_READ, &s), VFS_OK);
    CHECK_EQ(vfs_write(s, b, 1), VFS_CLOSED);               /* write on read mode */
    vfs_close(s);
    CHECK_EQ(vfs_read(-1, b, 1, &got), VFS_CLOSED);
    CHECK_EQ(vfs_write(99, b, 1), VFS_CLOSED);
    vfs_close(-5);
    vfs_close(99);
}

TEST(vfs_nospace_recovery) {
    setup(false, true, 10);
    int s;
    int32_t used, total;
    CHECK_EQ(vfs_open(S("/flash/big"), VFS_WRITE, &s), VFS_OK);
    CHECK_EQ(vfs_write(s, (const uint8_t *)"0123456789", 10), VFS_OK);
    CHECK_EQ(vfs_write(s, (const uint8_t *)"x", 1), VFS_NOSPACE);
    CHECK_EQ(vfs_sync(s), VFS_OK);                          /* still usable */
    CHECK_EQ(vfs_usage(S("/flash"), &used, &total), VFS_OK);
    CHECK_EQ(used, 10);
    CHECK_EQ(total, 10);
    vfs_close(s);
    int s2;
    CHECK_EQ(vfs_open(S("/flash/more"), VFS_WRITE, &s2), VFS_OK);
    CHECK_EQ(vfs_write(s2, (const uint8_t *)"y", 1), VFS_NOSPACE);
    vfs_close(s2);
    CHECK_EQ(vfs_remove(S("/flash/big")), VFS_OK);
    CHECK_EQ(vfs_usage(S("/flash"), &used, &total), VFS_OK);
    CHECK_EQ(used, 0);
    CHECK_EQ(put("/flash/more", "yyyy"), VFS_OK);           /* space is back */
    CHECK_EQ(vfs_usage(S("/sd/a"), &used, &total), VFS_OK);
    CHECK_EQ(total, -1);
}

TEST(vfs_case_insensitive) {
    setup(true, false, -1);
    int w, s;
    CHECK_EQ(vfs_open(S("/sd/a.txt"), VFS_WRITE, &w), VFS_OK);
    CHECK_EQ(vfs_open(S("/sd/A.TXT"), VFS_WRITE, &s), VFS_BUSY);
    CHECK_EQ(vfs_open(S("/sd/A.TXT"), VFS_APPEND, &s), VFS_BUSY);
    CHECK_EQ(vfs_remove(S("/sd/A.TXT")), VFS_BUSY);
    CHECK_EQ(vfs_rename(S("/sd/a.txt"), S("/sd/b.txt")), VFS_BUSY);
    CHECK_EQ(vfs_rename(S("/sd/b.txt"), S("/sd/A.Txt")), VFS_BUSY);
    vfs_close(w);
    CHECK_EQ(vfs_open(S("/sd/A.TXT"), VFS_READ, &s), VFS_OK);
    /* FAT: no Write (truncate) under a reader; Append is fine */
    CHECK_EQ(vfs_open(S("/sd/a.txt"), VFS_WRITE, &w), VFS_BUSY);
    CHECK_EQ(vfs_open(S("/sd/a.txt"), VFS_APPEND, &w), VFS_OK);
    vfs_close(w);
    vfs_close(s);
    CHECK_EQ(vfs_open(S("/sd/a.txt"), VFS_WRITE, &w), VFS_OK);
    vfs_close(w);
    /* case-sensitive backend: different names */
    setup(false, true, -1);
    CHECK_EQ(vfs_open(S("/flash/a.txt"), VFS_WRITE, &w), VFS_OK);
    CHECK_EQ(vfs_open(S("/flash/A.TXT"), VFS_WRITE, &s), VFS_OK);
    vfs_close(w); vfs_close(s);
    /* ...and a Write open under a reader is allowed there */
    CHECK_EQ(vfs_open(S("/flash/a.txt"), VFS_READ, &s), VFS_OK);
    CHECK_EQ(vfs_open(S("/flash/a.txt"), VFS_WRITE, &w), VFS_OK);
    vfs_close(w); vfs_close(s);
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
    CHECK_EQ(vfs_mkdir(S("/flash/d")), VFS_OK);
    CHECK_EQ(vfs_mkdir(S("/flash/d")), VFS_EXISTS);
    CHECK_EQ(vfs_mkdir(S("/flash/x/y")), VFS_NOTFOUND);
    CHECK_EQ(put("/flash/d/f", "1"), VFS_OK);
    CHECK_EQ(put("/flash/top", "1"), VFS_OK);
    CHECK_EQ(vfs_readdir(S("/flash"), count_cb, &n), VFS_OK);
    CHECK_EQ(n, 2);
    n = 0;
    CHECK_EQ(vfs_readdir(S("/flash/d"), count_cb, &n), VFS_OK);
    CHECK_EQ(n, 1);
    CHECK_EQ(vfs_readdir(S("/flash/top"), count_cb, &n), VFS_NOTDIR);
    CHECK_EQ(vfs_readdir(S("/flash/none"), count_cb, &n), VFS_NOTFOUND);
    CHECK_EQ(vfs_remove(S("/flash/d")), VFS_NOTEMPTY);
    CHECK_EQ(vfs_remove(S("/flash/d/f")), VFS_OK);
    CHECK_EQ(vfs_remove(S("/flash/d")), VFS_OK);
    CHECK_EQ(vfs_stat(S("/flash/d"), &sz, &d), VFS_NOTFOUND);
    int s;
    CHECK_EQ(vfs_mkdir(S("/flash/e")), VFS_OK);
    CHECK_EQ(vfs_open(S("/flash/e"), VFS_READ, &s), VFS_ISDIR);
}

/* Rename onto a target whose stat fails with anything but NotFound
 * propagates the error instead of treating the target as absent. */
TEST(vfs_rename_stat_error) {
    setup_default();
    CHECK_EQ(put("/flash/a", "1"), VFS_OK);
    memfs_set_stat_err(0, VFS_IO);
    CHECK_EQ(vfs_rename(S("/flash/a"), S("/flash/b")), VFS_IO);
    memfs_set_stat_err(0, VFS_NOTFOUND);
    CHECK_EQ(vfs_rename(S("/flash/a"), S("/flash/b")), VFS_OK);   /* absent: renamed */
    memfs_set_stat_err(0, 0);
    int32_t sz; bool d;
    CHECK_EQ(vfs_stat(S("/flash/b"), &sz, &d), VFS_OK);
}

/* A backend close that fails still releases the slot. */
TEST(vfs_close_failure_releases_slot) {
    setup_default();
    int s, t;
    memfs_set_close_fails(0, true);
    CHECK_EQ(vfs_open(S("/flash/a"), VFS_WRITE, &s), VFS_OK);
    vfs_close(s);
    CHECK_EQ(vfs_slot_mode(s), -1);
    CHECK_EQ(vfs_open(S("/flash/a"), VFS_WRITE, &t), VFS_OK);     /* no stale Busy */
    vfs_close(t);
    CHECK_EQ(vfs_remove(S("/flash/a")), VFS_OK);
    memfs_set_close_fails(0, false);
}

/* On a case-insensitive backend a reader opened under another case blocks
 * Remove and Rename of the file. */
TEST(vfs_mixed_case_reader_blocks_remove) {
    setup(true, false, -1);
    CHECK_EQ(put("/sd/a.txt", "x"), VFS_OK);
    int r;
    CHECK_EQ(vfs_open(S("/sd/A.TXT"), VFS_READ, &r), VFS_OK);
    CHECK_EQ(vfs_remove(S("/sd/a.txt")), VFS_BUSY);
    CHECK_EQ(vfs_rename(S("/sd/a.txt"), S("/sd/b.txt")), VFS_BUSY);
    vfs_close(r);
    CHECK_EQ(vfs_remove(S("/sd/a.txt")), VFS_OK);
}

/* A name ending in '.' is BadPath in any segment, on every mount: FatFs
 * strips trailing dots, so "a.txt." would alias "a.txt" past the slot
 * hash. Dots elsewhere in a name stay fine. */
TEST(vfs_trailing_dot) {
    setup_default();
    int32_t sz; bool d;
    int w, s;
    CHECK_EQ(vfs_stat(S("/flash/a."), &sz, &d), VFS_BADPATH);
    CHECK_EQ(vfs_stat(S("/flash/a.txt.."), &sz, &d), VFS_BADPATH);
    CHECK_EQ(vfs_stat(S("/flash/..."), &sz, &d), VFS_BADPATH);
    CHECK_EQ(vfs_stat(S("/flash/d./x"), &sz, &d), VFS_BADPATH);
    CHECK_EQ(vfs_stat(S("/sd/a.txt."), &sz, &d), VFS_BADPATH);
    CHECK_EQ(vfs_mkdir(S("/sd/d.")), VFS_BADPATH);
    CHECK_EQ(vfs_stat(S("/flash/.a"), &sz, &d), VFS_NOTFOUND);
    CHECK_EQ(vfs_stat(S("/flash/a.b.c"), &sz, &d), VFS_NOTFOUND);
    CHECK_EQ(vfs_open(S("/flash/a.txt"), VFS_WRITE, &w), VFS_OK);
    CHECK_EQ(vfs_open(S("/flash/a.txt."), VFS_WRITE, &s), VFS_BADPATH);
    CHECK_EQ(vfs_remove(S("/flash/a.txt.")), VFS_BADPATH);
    CHECK_EQ(vfs_rename(S("/flash/b"), S("/flash/a.txt.")), VFS_BADPATH);
    vfs_close(w);
    CHECK(!vfs_mount("/x.", &memfs_be[0]));
}

/* Rename into its own subtree is BadPath (LittleFS and FatFs would unlink
 * the directory and leak its blocks). The match folds case only on a
 * case-insensitive mount; a case-only rename there reaches the backend. */
TEST(vfs_rename_into_subtree) {
    setup_default();
    int32_t sz; bool d;
    char b[8];
    CHECK_EQ(vfs_mkdir(S("/flash/p")), VFS_OK);
    CHECK_EQ(vfs_rename(S("/flash/p"), S("/flash/p/q")), VFS_BADPATH);
    CHECK_EQ(vfs_rename(S("/flash/p"), S("/flash/p/q/r")), VFS_BADPATH);
    CHECK_EQ(vfs_rename(S("/flash/p"), S("/flash/p")), VFS_EXISTS);
    CHECK_EQ(vfs_rename(S("/flash/p"), S("/flash/P/q")), VFS_NOTFOUND);  /* exact on /flash: no P */
    CHECK_EQ(vfs_stat(S("/flash/p"), &sz, &d), VFS_OK);
    CHECK(d);
    CHECK_EQ(vfs_rename(S("/flash/p"), S("/flash/pq")), VFS_OK);         /* not a subtree */
    CHECK_EQ(vfs_rename(S("/flash/pq"), S("/flash/P")), VFS_OK);         /* case-only rename */
    CHECK_EQ(vfs_rename(S("/flash/P"), S("/flash/p")), VFS_OK);
    CHECK_EQ(vfs_stat(S("/flash/P"), &sz, &d), VFS_NOTFOUND);
    setup(true, false, -1);
    CHECK_EQ(vfs_mkdir(S("/sd/d")), VFS_OK);
    CHECK_EQ(vfs_rename(S("/sd/d"), S("/sd/D/x")), VFS_BADPATH);
    CHECK_EQ(vfs_rename(S("/sd/d"), S("/sd/d/x")), VFS_BADPATH);
    CHECK_EQ(vfs_rename(S("/sd/d"), S("/sd/d")), VFS_EXISTS);
    CHECK_EQ(put("/sd/a.txt", "A"), VFS_OK);
    CHECK_EQ(vfs_rename(S("/sd/a.txt"), S("/sd/A.TXT")), VFS_OK);       /* case-only: to the backend */
    CHECK_EQ(memfs_name(1, "/a.txt", b, sizeof b), VFS_OK);
    CHECK(!strcmp(b, "/A.TXT"));
    CHECK_EQ(vfs_rename(S("/sd/d"), S("/sd/dir2")), VFS_OK);
}

/* Renaming a directory while any file is open on that mount is Busy: the
 * slots keep only a hash of the path they were opened under, so a renamed
 * parent would let a second writer or a Remove reach the open file. */
TEST(vfs_rename_dir_with_open_file) {
    setup_default();
    int w, w2;
    int32_t sz; bool d;
    CHECK_EQ(vfs_mkdir(S("/flash/d")), VFS_OK);
    CHECK_EQ(vfs_mkdir(S("/flash/other")), VFS_OK);
    CHECK_EQ(vfs_open(S("/flash/d/x"), VFS_WRITE, &w), VFS_OK);
    CHECK_EQ(vfs_rename(S("/flash/d"), S("/flash/e")), VFS_BUSY);
    CHECK_EQ(vfs_rename(S("/flash/other"), S("/flash/o2")), VFS_BUSY);   /* any open file on the mount */
    CHECK_EQ(vfs_stat(S("/flash/d"), &sz, &d), VFS_OK);
    CHECK_EQ(vfs_open(S("/flash/d/x"), VFS_APPEND, &w2), VFS_BUSY);
    CHECK_EQ(vfs_remove(S("/flash/d/x")), VFS_BUSY);
    /* a plain file still renames beside an open file */
    CHECK_EQ(put("/flash/f", "1"), VFS_OK);
    CHECK_EQ(vfs_rename(S("/flash/f"), S("/flash/g")), VFS_OK);
    /* a file open on the other mount does not block */
    CHECK_EQ(vfs_mkdir(S("/sd/s")), VFS_OK);
    CHECK_EQ(vfs_rename(S("/sd/s"), S("/sd/t")), VFS_OK);
    vfs_close(w);
    CHECK_EQ(vfs_open(S("/sd/f"), VFS_WRITE, &w), VFS_OK);
    CHECK_EQ(vfs_rename(S("/flash/d"), S("/flash/e")), VFS_OK);
    vfs_close(w);
}
