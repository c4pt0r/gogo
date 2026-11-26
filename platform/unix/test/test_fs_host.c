char *mkdtemp(char *);   /* POSIX; not exposed under -std=c99 on every libc */
/* fs_dir (runtime/embed/fs_dir.c) against a temp directory, directly and
 * through vfs: the vfs backend contract, then the sandbox (symlinks, hard
 * links, a path swapped while it is walked, renames across the root,
 * over-long paths) and the quota. */
#define _POSIX_C_SOURCE 200809L
#include "test.h"
#include "vfs_int.h"
#include "fs_dir.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#define S(x) (x), (uint32_t)strlen(x)

static char base[128];      /* the temp dir: root/ and out/ (outside the root) */
static char root[160], out[160];
static const VfsBackend *be;
static void *bc;            /* its ctx (the FsDir) */
static VfsState vs;
static long host_blocks;

static void *m_alloc(void *c, size_t n) { (void)c; host_blocks++; return malloc(n); }
static void m_free(void *c, void *p, size_t n) { (void)c; (void)n; host_blocks--; free(p); }
static const gogo_host mhost = { NULL, m_alloc, m_free, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL };

static void setup_quota(int64_t quota) {
    snprintf(base, sizeof base, "%s/gogofs-XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    CHECK(mkdtemp(base) != NULL);
    snprintf(root, sizeof root, "%s/root", base);
    snprintf(out, sizeof out, "%s/out", base);
    CHECK_EQ(mkdir(root, 0777), 0);
    CHECK_EQ(mkdir(out, 0777), 0);
    bc = fs_dir_open(&mhost, NULL, root, quota);
    CHECK(bc != NULL);
    be = bc ? fs_dir_vfs(bc) : &fs_dir_backend;
}
static void setup(void) { setup_quota(-1); }
static void cleanup(void) {
    char cmd[200];
    fs_dir_close(bc);
    CHECK_EQ(host_blocks, 0);
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", base);
    CHECK_EQ(system(cmd), 0);
}
static int put(const char *rel, const char *data, int mode) {
    uint8_t f[GOGO_FS_SLOT_BYTES] __attribute__((aligned(8)));
    int e = be->open(bc, f, rel, mode);
    if (e) return e;
    e = be->write(bc, f, (const uint8_t *)data, (uint32_t)strlen(data));
    be->close(bc, f);
    return e;
}

typedef struct { int n, stop_after; char names[8][64]; bool dir[8]; } Ls;
static int ls_cb(void *c, const char *name, bool d) {
    Ls *l = c;
    if (l->n < 8) { snprintf(l->names[l->n], 64, "%s", name); l->dir[l->n] = d; }
    l->n++;
    return l->stop_after && l->n >= l->stop_after;
}

TEST(host_rw_size_stat) {
    setup();
    CHECK_EQ(put("/a.txt", "hello", VFS_WRITE), GOGO_E_OK);
    CHECK_EQ(put("/a.txt", " world", VFS_APPEND), GOGO_E_OK);
    uint8_t f[GOGO_FS_SLOT_BYTES] __attribute__((aligned(8)));
    CHECK_EQ(be->open(bc, f, "/a.txt", VFS_READ), GOGO_E_OK);
    int32_t sz = -1;
    CHECK_EQ(be->size(bc, f, &sz), GOGO_E_OK);
    CHECK_EQ(sz, 11);
    uint8_t b[32];
    uint32_t got = 0;
    CHECK_EQ(be->read(bc, f, b, sizeof b, &got), GOGO_E_OK);
    CHECK_EQ(got, 11);
    CHECK(memcmp(b, "hello world", 11) == 0);
    CHECK_EQ(be->read(bc, f, b, sizeof b, &got), GOGO_E_OK);
    CHECK_EQ(got, 0);
    CHECK_EQ(be->close(bc, f), GOGO_E_OK);
    bool dir = true;
    CHECK_EQ(be->stat(bc, "/a.txt", &sz, &dir), GOGO_E_OK);
    CHECK_EQ(sz, 11);
    CHECK(!dir);
    CHECK_EQ(be->stat(bc, "/", &sz, &dir), GOGO_E_OK);
    CHECK(dir);
    CHECK_EQ(be->stat(bc, "/nope", &sz, &dir), GOGO_E_NOTFOUND);
    CHECK_EQ(be->open(bc, f, "/nope", VFS_READ), GOGO_E_NOTFOUND);
    CHECK_EQ(be->open(bc, f, "/nodir/x", VFS_WRITE), GOGO_E_NOTFOUND);
    CHECK_EQ(be->open(bc, f, "/", VFS_READ), GOGO_E_ISDIR);
    CHECK_EQ(put("/a.txt", "x", VFS_WRITE), GOGO_E_OK);          /* truncates */
    CHECK_EQ(be->stat(bc, "/a.txt", &sz, &dir), GOGO_E_OK);
    CHECK_EQ(sz, 1);
    cleanup();
}

TEST(host_write_at) {
    setup();
    uint8_t f[GOGO_FS_SLOT_BYTES] __attribute__((aligned(8))), b[32];
    uint32_t got;
    CHECK_EQ(be->open(bc, f, "/w", VFS_WRITE), GOGO_E_OK);
    CHECK_EQ(be->write(bc, f, (const uint8_t *)"hello", 5), GOGO_E_OK);
    CHECK_EQ(be->write_at(bc, f, 1, (const uint8_t *)"EL", 2), GOGO_E_OK);
    CHECK_EQ(be->write(bc, f, (const uint8_t *)"!", 1), GOGO_E_OK);          /* the position did not move */
    CHECK_EQ(be->write_at(bc, f, 5, (const uint8_t *)"?", 1), GOGO_E_OK);
    CHECK_EQ(be->close(bc, f), GOGO_E_OK);
    CHECK_EQ(be->open(bc, f, "/w", VFS_READ), GOGO_E_OK);
    CHECK_EQ(be->read(bc, f, b, sizeof b, &got), GOGO_E_OK);
    CHECK_EQ(got, 6);
    CHECK(memcmp(b, "hELlo?", 6) == 0);
    be->close(bc, f);
    cleanup();
}

TEST(host_dirs_readdir_remove) {
    setup();
    CHECK_EQ(be->mkdir(bc, "/d"), GOGO_E_OK);
    CHECK_EQ(be->mkdir(bc, "/d"), GOGO_E_EXISTS);
    CHECK_EQ(put("/d/x", "1", VFS_WRITE), GOGO_E_OK);
    CHECK_EQ(put("/d/y", "2", VFS_WRITE), GOGO_E_OK);
    CHECK_EQ(be->mkdir(bc, "/d/sub"), GOGO_E_OK);
    Ls l = {0};
    CHECK_EQ(be->readdir(bc, "/d", ls_cb, &l), GOGO_E_OK);
    CHECK_EQ(l.n, 3);
    for (int i = 0; i < l.n; i++) {
        CHECK(strcmp(l.names[i], ".") && strcmp(l.names[i], ".."));
        CHECK_EQ(l.dir[i], strcmp(l.names[i], "sub") == 0);
    }
    Ls s = {0};
    s.stop_after = 1;
    CHECK_EQ(be->readdir(bc, "/d", ls_cb, &s), GOGO_E_OK);          /* stops early, still OK */
    CHECK_EQ(s.n, 1);
    CHECK_EQ(be->readdir(bc, "/nope", ls_cb, &s), GOGO_E_NOTFOUND);
    CHECK_EQ(be->readdir(bc, "/d/x", ls_cb, &s), GOGO_E_NOTDIR);
    CHECK_EQ(be->remove(bc, "/d"), GOGO_E_NOTEMPTY);
    CHECK_EQ(be->remove(bc, "/d/sub"), GOGO_E_OK);
    CHECK_EQ(be->remove(bc, "/d/x"), GOGO_E_OK);
    CHECK_EQ(be->remove(bc, "/d/x"), GOGO_E_NOTFOUND);
    CHECK_EQ(be->remove(bc, "/d/y"), GOGO_E_OK);
    CHECK_EQ(be->remove(bc, "/d"), GOGO_E_OK);
    cleanup();
}

TEST(host_rename) {
    setup();
    char b[16];
    uint8_t f[GOGO_FS_SLOT_BYTES] __attribute__((aligned(8)));
    uint32_t got;
    CHECK_EQ(put("/a", "A", VFS_WRITE), GOGO_E_OK);
    CHECK_EQ(put("/b", "B", VFS_WRITE), GOGO_E_OK);
    CHECK_EQ(be->rename(bc, "/a", "/b", false), GOGO_E_EXISTS);
    CHECK_EQ(be->rename(bc, "/a", "/c", false), GOGO_E_OK);
    CHECK_EQ(be->rename(bc, "/a", "/z", false), GOGO_E_NOTFOUND);
    CHECK_EQ(be->rename(bc, "/c", "/b", true), GOGO_E_OK);           /* replaces */
    CHECK_EQ(be->open(bc, f, "/b", VFS_READ), GOGO_E_OK);
    CHECK_EQ(be->read(bc, f, (uint8_t *)b, sizeof b, &got), GOGO_E_OK);
    CHECK_EQ(got, 1);
    CHECK_EQ(b[0], 'A');
    be->close(bc, f);
    cleanup();
}

TEST(host_usage_and_vfs) {
    setup();
    int32_t used = -1, total = -1;
    CHECK_EQ(be->usage(bc, &used, &total), GOGO_E_OK);
    CHECK(total > 0);
    CHECK(used > 0);
    CHECK(used <= total);
    vfs_reset(&vs);
    CHECK(vfs_mount(&vs, "/flash", be, bc));
    int s;
    CHECK_EQ(vfs_open(&vs, S("/flash/h.txt"), VFS_WRITE, &s), GOGO_E_OK);
    CHECK_EQ(vfs_write(&vs, s, (const uint8_t *)"hi", 2), GOGO_E_OK);
    vfs_close(&vs, s);
    int32_t sz;
    bool dir;
    CHECK_EQ(vfs_stat(&vs, S("/flash/h.txt"), &sz, &dir), GOGO_E_OK);
    CHECK_EQ(sz, 2);
    CHECK_EQ(vfs_stat(&vs, S("/sd/h.txt"), &sz, &dir), GOGO_E_NOTMOUNTED);
    vfs_reset(&vs);
    cleanup();
}

TEST(host_readdir_lists_rule_breaking_names) {
    setup();
    char long40[41], path[200];
    memset(long40, 'n', 40);
    long40[40] = 0;
    snprintf(path, sizeof path, "%s/my file", root);
    FILE *f = fopen(path, "w");
    CHECK(f != NULL);
    if (f) fclose(f);
    snprintf(path, sizeof path, "%s/%s", root, long40);
    f = fopen(path, "w");
    CHECK(f != NULL);
    if (f) fclose(f);
    Ls l = {0};
    CHECK_EQ(be->readdir(bc, "/", ls_cb, &l), GOGO_E_OK);
    CHECK_EQ(l.n, 2);
    bool a = false, b = false;
    for (int i = 0; i < l.n; i++) {
        if (!strcmp(l.names[i], "my file")) a = true;
        if (!strcmp(l.names[i], long40)) b = true;
    }
    CHECK(a);
    CHECK(b);
    cleanup();
}

/* The vfs path and rename rules on the real host backend (the round-1
 * vfs_bypass repro): a renamed parent of an open file, a trailing-dot
 * alias, and a directory renamed into its own subtree. */
TEST(host_vfs_rename_rules) {
    setup();
    vfs_reset(&vs);
    CHECK(vfs_mount(&vs, "/flash", be, bc));
    int s1, s2;
    int32_t sz; bool dir;
    CHECK_EQ(vfs_mkdir(&vs, S("/flash/d")), GOGO_E_OK);
    CHECK_EQ(vfs_open(&vs, S("/flash/d/x"), VFS_WRITE, &s1), GOGO_E_OK);
    CHECK_EQ(vfs_rename(&vs, S("/flash/d"), S("/flash/e")), GOGO_E_BUSY);
    CHECK_EQ(vfs_open(&vs, S("/flash/e/x"), VFS_APPEND, &s2), GOGO_E_NOTFOUND);
    CHECK_EQ(vfs_open(&vs, S("/flash/d/x"), VFS_APPEND, &s2), GOGO_E_BUSY);
    vfs_close(&vs, s1);
    CHECK_EQ(vfs_rename(&vs, S("/flash/d"), S("/flash/e")), GOGO_E_OK);
    CHECK_EQ(vfs_stat(&vs, S("/flash/e/x"), &sz, &dir), GOGO_E_OK);
    /* trailing dot */
    CHECK_EQ(vfs_open(&vs, S("/flash/a.txt"), VFS_WRITE, &s1), GOGO_E_OK);
    CHECK_EQ(vfs_open(&vs, S("/flash/a.txt."), VFS_WRITE, &s2), GOGO_E_BADPATH);
    CHECK_EQ(vfs_remove(&vs, S("/flash/a.txt..")), GOGO_E_BADPATH);
    vfs_close(&vs, s1);
    /* into its own subtree */
    CHECK_EQ(vfs_mkdir(&vs, S("/flash/p")), GOGO_E_OK);
    CHECK_EQ(vfs_rename(&vs, S("/flash/p"), S("/flash/p/q")), GOGO_E_BADPATH);
    CHECK_EQ(vfs_rename(&vs, S("/flash/p"), S("/flash/p")), GOGO_E_EXISTS);
    CHECK_EQ(vfs_stat(&vs, S("/flash/p"), &sz, &dir), GOGO_E_OK);
    CHECK(dir);
    /* a case-only rename is not refused by vfs; whether it happens is up
     * to the host file system (macOS is usually case-insensitive) */
    bool host_ci = vfs_stat(&vs, S("/flash/P"), &sz, &dir) == GOGO_E_OK;
    CHECK_EQ(vfs_rename(&vs, S("/flash/p"), S("/flash/P")), host_ci ? GOGO_E_EXISTS : GOGO_E_OK);
    vfs_reset(&vs);
    cleanup();
}

/* ---- the sandbox ---- */

static void host_put(const char *path, const char *data) {
    FILE *f = fopen(path, "w");
    CHECK(f != NULL);
    if (!f) return;
    fputs(data, f);
    fclose(f);
}
/* The file's content ("" if it is missing or empty; at most 63 bytes). */
static const char *host_get(const char *path) {
    static char b[64];
    b[0] = 0;
    FILE *f = fopen(path, "r");
    if (!f) return b;
    size_t n = fread(b, 1, sizeof b - 1, f);
    b[n] = 0;
    fclose(f);
    return b;
}
static bool host_exists(const char *path) {
    struct stat st;
    return lstat(path, &st) == 0;
}
static void in_root(char *buf, size_t cap, const char *rel) { snprintf(buf, cap, "%s%s", root, rel); }
static void in_out(char *buf, size_t cap, const char *rel) { snprintf(buf, cap, "%s%s", out, rel); }

/* out/ holds secret (a file) and nothing else; it must stay that way. */
static void out_seed(void) {
    char p[256];
    in_out(p, sizeof p, "/secret");
    host_put(p, "secret");
}
static void out_untouched(void) {
    char p[256];
    in_out(p, sizeof p, "/secret");
    CHECK(strcmp(host_get(p), "secret") == 0);
    Ls l = {0};
    /* count out/'s entries through a second FsDir rooted there */
    FsDir *o = fs_dir_open(&mhost, NULL, out, -1);
    CHECK(o != NULL);
    CHECK_EQ(fs_dir_backend.readdir(o, "/", ls_cb, &l), GOGO_E_OK);
    CHECK_EQ(l.n, 1);
    fs_dir_close(o);
}

TEST(sandbox_symlink_file) {
    setup();
    out_seed();
    char p[256], t[256];
    in_out(t, sizeof t, "/secret");
    in_root(p, sizeof p, "/s");
    CHECK_EQ(symlink(t, p), 0);
    in_root(p, sizeof p, "/dangling");
    in_out(t, sizeof t, "/new");
    CHECK_EQ(symlink(t, p), 0);
    uint8_t f[GOGO_FS_SLOT_BYTES] __attribute__((aligned(8)));
    int32_t sz;
    bool dir;
    CHECK_EQ(be->open(bc, f, "/s", VFS_READ), GOGO_E_BADPATH);
    CHECK_EQ(be->open(bc, f, "/s", VFS_WRITE), GOGO_E_BADPATH);
    CHECK_EQ(be->open(bc, f, "/s", VFS_APPEND), GOGO_E_BADPATH);
    CHECK_EQ(be->open(bc, f, "/dangling", VFS_WRITE), GOGO_E_BADPATH);   /* would create out/new */
    CHECK_EQ(be->stat(bc, "/s", &sz, &dir), GOGO_E_BADPATH);
    CHECK_EQ(be->readdir(bc, "/s", ls_cb, &(Ls){0}), GOGO_E_BADPATH);
    CHECK_EQ(be->mkdir(bc, "/s"), GOGO_E_EXISTS);
    Ls l = {0};
    CHECK_EQ(be->readdir(bc, "/", ls_cb, &l), GOGO_E_OK);               /* listed, as files */
    CHECK_EQ(l.n, 2);
    CHECK(!l.dir[0] && !l.dir[1]);
    /* Rename and Remove act on the link itself. */
    CHECK_EQ(be->rename(bc, "/s", "/s2", false), GOGO_E_OK);
    CHECK_EQ(be->remove(bc, "/s2"), GOGO_E_OK);
    CHECK_EQ(be->remove(bc, "/dangling"), GOGO_E_OK);
    out_untouched();
    cleanup();
}

TEST(sandbox_symlink_dir) {
    setup();
    out_seed();
    char p[256];
    in_root(p, sizeof p, "/d");
    CHECK_EQ(symlink(out, p), 0);
    in_root(p, sizeof p, "/real");
    CHECK_EQ(mkdir(p, 0777), 0);
    in_root(p, sizeof p, "/real/up");
    CHECK_EQ(symlink("..", p), 0);
    in_root(p, sizeof p, "/real/abs");
    CHECK_EQ(symlink("/", p), 0);
    CHECK_EQ(put("/a", "A", VFS_WRITE), GOGO_E_OK);
    uint8_t f[GOGO_FS_SLOT_BYTES] __attribute__((aligned(8)));
    int32_t sz;
    bool dir;
    static const char *const via[] = { "/d/secret", "/d/new", "/real/up/out/secret", "/real/abs/etc/passwd" };
    for (unsigned i = 0; i < sizeof via / sizeof via[0]; i++) {
        CHECK_EQ(be->open(bc, f, via[i], VFS_READ), GOGO_E_BADPATH);
        CHECK_EQ(be->open(bc, f, via[i], VFS_WRITE), GOGO_E_BADPATH);
        CHECK_EQ(be->open(bc, f, via[i], VFS_APPEND), GOGO_E_BADPATH);
        CHECK_EQ(be->stat(bc, via[i], &sz, &dir), GOGO_E_BADPATH);
        CHECK_EQ(be->mkdir(bc, via[i]), GOGO_E_BADPATH);
        CHECK_EQ(be->remove(bc, via[i]), GOGO_E_BADPATH);
        CHECK_EQ(be->rename(bc, "/a", via[i], true), GOGO_E_BADPATH);    /* out of the root */
        CHECK_EQ(be->rename(bc, via[i], "/b", true), GOGO_E_BADPATH);    /* into it */
    }
    CHECK_EQ(be->readdir(bc, "/d", ls_cb, &(Ls){0}), GOGO_E_BADPATH);
    CHECK_EQ(be->readdir(bc, "/real/up", ls_cb, &(Ls){0}), GOGO_E_BADPATH);
    CHECK_EQ(be->stat(bc, "/a", &sz, &dir), GOGO_E_OK);
    out_untouched();
    cleanup();
}

TEST(sandbox_hard_link) {
    setup();
    out_seed();
    char p[256], t[256];
    in_out(t, sizeof t, "/secret");
    in_root(p, sizeof p, "/h");
    if (link(t, p) != 0) {
        printf("  (no hard links here: %s; skipped)\n", strerror(errno));
        cleanup();
        return;
    }
    uint8_t f[GOGO_FS_SLOT_BYTES] __attribute__((aligned(8)));
    CHECK_EQ(be->open(bc, f, "/h", VFS_WRITE), GOGO_E_BADPATH);    /* not truncated first */
    CHECK_EQ(be->open(bc, f, "/h", VFS_APPEND), GOGO_E_BADPATH);
    CHECK_EQ(be->open(bc, f, "/h", VFS_READ), GOGO_E_OK);          /* reading is allowed */
    be->close(bc, f);
    CHECK_EQ(be->remove(bc, "/h"), GOGO_E_OK);                    /* the name only */
    out_untouched();
    cleanup();
}

/* A FIFO is refused without blocking; so is any other non-regular file. */
TEST(sandbox_fifo) {
    setup();
    char p[256];
    in_root(p, sizeof p, "/fifo");
    CHECK_EQ(mkfifo(p, 0666), 0);
    uint8_t f[GOGO_FS_SLOT_BYTES] __attribute__((aligned(8)));
    int32_t sz;
    bool dir;
    CHECK_EQ(be->open(bc, f, "/fifo", VFS_READ), GOGO_E_BADPATH);
    CHECK_EQ(be->open(bc, f, "/fifo", VFS_WRITE), GOGO_E_BADPATH);  /* refused unopened */
    CHECK_EQ(be->open(bc, f, "/fifo", VFS_APPEND), GOGO_E_BADPATH);
    CHECK_EQ(be->stat(bc, "/fifo", &sz, &dir), GOGO_E_BADPATH);
    cleanup();
}

TEST(sandbox_bad_paths) {
    setup();
    out_seed();
    uint8_t f[GOGO_FS_SLOT_BYTES] __attribute__((aligned(8)));
    int32_t sz;
    bool dir;
    char longp[4200];
    /* Over-long: past VFS_PATH_MAX (and past PATH_MAX) at the backend... */
    for (size_t i = 0; i + 2 < sizeof longp; i += 2) { longp[i] = '/'; longp[i + 1] = 'a'; }
    longp[sizeof longp - 2] = 'a';
    longp[sizeof longp - 1] = 0;
    CHECK_EQ(be->open(bc, f, longp, VFS_WRITE), GOGO_E_BADPATH);
    CHECK_EQ(be->mkdir(bc, longp), GOGO_E_BADPATH);
    memset(longp, 'n', 200);
    longp[0] = '/';
    longp[200] = 0;
    CHECK_EQ(be->open(bc, f, longp, VFS_WRITE), GOGO_E_BADPATH);
    /* ... and through vfs (97 bytes). */
    vfs_reset(&vs);
    CHECK(vfs_mount(&vs, "/flash", be, bc));
    char p97[128] = "/flash/";
    memset(p97 + 7, 'b', 90);
    p97[97] = 0;
    int s;
    CHECK_EQ(vfs_open(&vs, p97, 97, VFS_WRITE, &s), GOGO_E_BADPATH);
    vfs_reset(&vs);
    /* Dot components and empty ones, even if vfs let one through. */
    static const char *const bad[] = { "/..", "/../out/secret", "/./a", "/a/../../out/secret", "//a", "/a/", "a" };
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        CHECK_EQ(be->open(bc, f, bad[i], VFS_READ), GOGO_E_BADPATH);
        CHECK_EQ(be->open(bc, f, bad[i], VFS_WRITE), GOGO_E_BADPATH);
        CHECK_EQ(be->stat(bc, bad[i], &sz, &dir), GOGO_E_BADPATH);
        CHECK_EQ(be->rename(bc, bad[i], "/x", true), GOGO_E_BADPATH);
    }
    /* The root itself cannot be removed or renamed. */
    CHECK_EQ(be->remove(bc, "/"), GOGO_E_BADPATH);
    CHECK_EQ(be->rename(bc, "/", "/x", false), GOGO_E_BADPATH);
    CHECK(host_exists(root));
    out_untouched();
    cleanup();
}

/* A path component swapped for a symlink to out/ while it is walked: a
 * thread flips root/d between a real directory and the link while the
 * VM side opens, creates, lists and renames under /d. Nothing outside may
 * change and nothing outside may be read. */
typedef struct { volatile int stop; long flips; } Flip;
static void *flipper(void *arg) {
    Flip *fl = arg;
    char d[256], real[256];
    in_root(d, sizeof d, "/d");
    in_root(real, sizeof real, "/d.real");
    while (!fl->stop) {
        if (rename(d, real) != 0) continue;      /* d: the real directory */
        if (symlink(out, d) != 0) { rename(real, d); continue; }
        sched_yield();
        unlink(d);
        rename(real, d);
        fl->flips++;
    }
    return NULL;
}

TEST(sandbox_swap_race) {
    setup();
    out_seed();
    char p[256];
    in_root(p, sizeof p, "/d");
    CHECK_EQ(mkdir(p, 0777), 0);
    in_root(p, sizeof p, "/d/secret");
    host_put(p, "inside");
    Flip fl = { 0, 0 };
    pthread_t th;
    CHECK_EQ(pthread_create(&th, NULL, flipper, &fl), 0);
    uint8_t f[GOGO_FS_SLOT_BYTES] __attribute__((aligned(8))), b[16];
    long ok_reads = 0;
    for (int i = 0; i < 20000; i++) {
        uint32_t got;
        if (be->open(bc, f, "/d/secret", VFS_READ) == GOGO_E_OK) {
            got = 0;
            CHECK_EQ(be->read(bc, f, b, sizeof b, &got), GOGO_E_OK);
            CHECK(got == 6 && memcmp(b, "inside", 6) == 0);
            be->close(bc, f);
            ok_reads++;
        }
        if (be->open(bc, f, "/d/secret", VFS_APPEND) == GOGO_E_OK) be->close(bc, f);
        if (be->open(bc, f, "/d/new", VFS_WRITE) == GOGO_E_OK) be->close(bc, f);
        be->mkdir(bc, "/d/sub");
        be->rename(bc, "/d/new", "/d/new2", true);
        be->remove(bc, "/d/new2");
        be->remove(bc, "/d/sub");
        Ls l = {0};
        be->readdir(bc, "/d", ls_cb, &l);
        for (int k = 0; k < l.n && k < 8; k++) CHECK(strcmp(l.names[k], "secret") != 0 || !l.dir[k]);
    }
    fl.stop = 1;
    pthread_join(th, NULL);
    CHECK(fl.flips > 0);
    CHECK(ok_reads > 0);
    out_untouched();
    cleanup();
}

/* The quota: what the files hold (existing ones counted at open), per
 * instance; truncation, removal and replacement give room back. */
TEST(quota_tracks_usage) {
    setup_quota(10);
    fs_dir_close(bc);
    char p[256];
    in_root(p, sizeof p, "/old");
    host_put(p, "1234");                              /* 4 bytes already there */
    bc = fs_dir_open(&mhost, NULL, root, 10);
    CHECK(bc != NULL);
    FsDir *other = fs_dir_open(&mhost, NULL, out, 3);  /* a second instance, its own quota */
    CHECK(other != NULL);
    int32_t used, total;
    CHECK_EQ(be->usage(bc, &used, &total), GOGO_E_OK);
    CHECK_EQ(used, 4);
    CHECK_EQ(total, 10);
    CHECK_EQ(put("/a", "abcdef", VFS_WRITE), GOGO_E_OK);         /* 10 */
    CHECK_EQ(put("/b", "x", VFS_WRITE), GOGO_E_NOSPACE);
    uint8_t f[GOGO_FS_SLOT_BYTES] __attribute__((aligned(8)));
    CHECK_EQ(fs_dir_backend.open(other, f, "/o", VFS_WRITE), GOGO_E_OK);
    CHECK_EQ(fs_dir_backend.write(other, f, (const uint8_t *)"12345", 5), GOGO_E_NOSPACE);   /* 3 stored */
    fs_dir_backend.close(other, f);
    CHECK_EQ(fs_dir_backend.usage(other, &used, &total), GOGO_E_OK);
    CHECK_EQ(used, 3);
    CHECK_EQ(total, 3);
    in_out(p, sizeof p, "/o");
    CHECK(strcmp(host_get(p), "123") == 0);
    CHECK_EQ(be->remove(bc, "/old"), GOGO_E_OK);                 /* 6 */
    CHECK_EQ(put("/a", "xy", VFS_WRITE), GOGO_E_OK);             /* truncated: 2 */
    CHECK_EQ(be->usage(bc, &used, &total), GOGO_E_OK);
    CHECK_EQ(used, 2);
    CHECK_EQ(put("/c", "12345678", VFS_WRITE), GOGO_E_OK);       /* 10 */
    CHECK_EQ(be->rename(bc, "/a", "/c", true), GOGO_E_OK);       /* c's 8 go: 2 */
    CHECK_EQ(be->usage(bc, &used, &total), GOGO_E_OK);
    CHECK_EQ(used, 2);
    CHECK_EQ(put("/d", "12345678", VFS_APPEND), GOGO_E_OK);
    CHECK_EQ(put("/d", "9", VFS_APPEND), GOGO_E_NOSPACE);
    fs_dir_close(other);
    cleanup();
}

/* A write past the end of a file the host shrank under the handle pays
 * for the hole; bytes the host added after the count never drive the
 * count below 0. */
TEST(quota_charges_growth) {
    setup_quota(100);
    uint8_t f[GOGO_FS_SLOT_BYTES] __attribute__((aligned(8))), b[100];
    memset(b, 'x', sizeof b);
    char p[256];
    int32_t used, total;
    in_root(p, sizeof p, "/late");
    host_put(p, "12345678");                              /* not counted */
    CHECK_EQ(be->remove(bc, "/late"), GOGO_E_OK);
    CHECK_EQ(be->usage(bc, &used, &total), GOGO_E_OK);
    CHECK_EQ(used, 0);                                    /* not -8 */
    CHECK_EQ(be->open(bc, f, "/a", VFS_WRITE), GOGO_E_OK);
    CHECK_EQ(be->write(bc, f, b, 30), GOGO_E_OK);            /* 30 */
    in_root(p, sizeof p, "/a");
    CHECK_EQ(truncate(p, 0), 0);                          /* behind the count: still 30 */
    CHECK_EQ(be->write(bc, f, b, 60), GOGO_E_NOSPACE);       /* at 30: up to 70 fits */
    struct stat st;
    CHECK_EQ(stat(p, &st), 0);
    CHECK_EQ(st.st_size, 70);                             /* the hole is paid for */
    CHECK_EQ(be->usage(bc, &used, &total), GOGO_E_OK);
    CHECK_EQ(used, 100);
    be->close(bc, f);
    cleanup();
}

/* On a host file system that ignores case, /a and /A are one file: the
 * mount says so, vfs refuses a second writer, and the quota holds
 * (the alias attack: truncate through /A under /a's writer, write past
 * the hole, remove, repeat). Skipped where the file system respects case. */
TEST(quota_case_alias) {
    setup_quota(100000);
    char lo[256], up[256];
    struct stat st;
    in_root(lo, sizeof lo, "/probe");
    in_root(up, sizeof up, "/PROBE");
    host_put(lo, "");
    bool host_ci = stat(up, &st) == 0;
    unlink(lo);
    CHECK_EQ(fs_dir_vfs(bc)->case_insensitive, host_ci);
    if (!host_ci) {
        printf("  (case-sensitive file system here; skipped)\n");
        cleanup();
        return;
    }
    static uint8_t buf[100000];
    memset(buf, 'x', sizeof buf);
    vfs_reset(&vs);
    CHECK(vfs_mount(&vs, "/flash", be, bc));
    int32_t used, total;
    for (int round = 0; round < 6; round++) {
        int a, b;
        CHECK_EQ(vfs_open(&vs, S("/flash/a"), VFS_WRITE, &a), GOGO_E_OK);
        CHECK_EQ(vfs_write(&vs, a, buf, 50000), GOGO_E_OK);
        CHECK_EQ(vfs_open(&vs, S("/flash/A"), VFS_WRITE, &b), GOGO_E_BUSY);
        CHECK_EQ(vfs_remove(&vs, S("/flash/A")), GOGO_E_BUSY);
        CHECK_EQ(vfs_usage(&vs, S("/flash"), &used, &total), GOGO_E_OK);
        CHECK_EQ(vfs_write(&vs, a, buf, (uint32_t)(total - used)), GOGO_E_OK);
        vfs_close(&vs, a);
        CHECK_EQ(vfs_remove(&vs, S("/flash/A")), GOGO_E_OK);   /* the same file */
        CHECK_EQ(vfs_usage(&vs, S("/flash"), &used, &total), GOGO_E_OK);
        CHECK_EQ(used, 0);
    }
    int g;
    long long stored = 0;
    CHECK_EQ(vfs_open(&vs, S("/flash/big"), VFS_WRITE, &g), GOGO_E_OK);
    while (vfs_write(&vs, g, buf, sizeof buf) == GOGO_E_OK && stored < 10000000) stored += (long long)sizeof buf;
    vfs_close(&vs, g);
    char p[256];
    in_root(p, sizeof p, "/big");
    CHECK_EQ(stat(p, &st), 0);
    CHECK_EQ(st.st_size, 100000);
    vfs_reset(&vs);
    cleanup();
}
