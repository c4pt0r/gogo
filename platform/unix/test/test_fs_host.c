char *mkdtemp(char *);   /* POSIX; not exposed under -std=c99 on every libc */
/* fs_host against a temp directory, directly and through vfs. */
#include "test.h"
#include "vfs_int.h"
#include "fs_host.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>

#define S(x) (x), (uint32_t)strlen(x)

static char root[128];
static const VfsBackend *be;
static void *bc;            /* its ctx (the root) */
static VfsState vs;

static void setup(void) {
    snprintf(root, sizeof root, "%s/gogofs-XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    CHECK(mkdtemp(root) != NULL);
    bc = fs_host_open(0, root);
    be = &fs_host_be;
    CHECK(bc != NULL);
}
static void cleanup(void) {
    char cmd[200];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
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
    CHECK_EQ(put("/a.txt", "hello", VFS_WRITE), VFS_OK);
    CHECK_EQ(put("/a.txt", " world", VFS_APPEND), VFS_OK);
    uint8_t f[GOGO_FS_SLOT_BYTES] __attribute__((aligned(8)));
    CHECK_EQ(be->open(bc, f, "/a.txt", VFS_READ), VFS_OK);
    int32_t sz = -1;
    CHECK_EQ(be->size(bc, f, &sz), VFS_OK);
    CHECK_EQ(sz, 11);
    uint8_t b[32];
    uint32_t got = 0;
    CHECK_EQ(be->read(bc, f, b, sizeof b, &got), VFS_OK);
    CHECK_EQ(got, 11);
    CHECK(memcmp(b, "hello world", 11) == 0);
    CHECK_EQ(be->read(bc, f, b, sizeof b, &got), VFS_OK);
    CHECK_EQ(got, 0);
    CHECK_EQ(be->close(bc, f), VFS_OK);
    bool dir = true;
    CHECK_EQ(be->stat(bc, "/a.txt", &sz, &dir), VFS_OK);
    CHECK_EQ(sz, 11);
    CHECK(!dir);
    CHECK_EQ(be->stat(bc, "/", &sz, &dir), VFS_OK);
    CHECK(dir);
    CHECK_EQ(be->stat(bc, "/nope", &sz, &dir), VFS_NOTFOUND);
    CHECK_EQ(be->open(bc, f, "/nope", VFS_READ), VFS_NOTFOUND);
    CHECK_EQ(be->open(bc, f, "/nodir/x", VFS_WRITE), VFS_NOTFOUND);
    CHECK_EQ(be->open(bc, f, "/", VFS_READ), VFS_ISDIR);
    CHECK_EQ(put("/a.txt", "x", VFS_WRITE), VFS_OK);          /* truncates */
    CHECK_EQ(be->stat(bc, "/a.txt", &sz, &dir), VFS_OK);
    CHECK_EQ(sz, 1);
    cleanup();
}

TEST(host_write_at) {
    setup();
    uint8_t f[GOGO_FS_SLOT_BYTES] __attribute__((aligned(8))), b[32];
    uint32_t got;
    CHECK_EQ(be->open(bc, f, "/w", VFS_WRITE), VFS_OK);
    CHECK_EQ(be->write(bc, f, (const uint8_t *)"hello", 5), VFS_OK);
    CHECK_EQ(be->write_at(bc, f, 1, (const uint8_t *)"EL", 2), VFS_OK);
    CHECK_EQ(be->write(bc, f, (const uint8_t *)"!", 1), VFS_OK);          /* the position did not move */
    CHECK_EQ(be->write_at(bc, f, 5, (const uint8_t *)"?", 1), VFS_OK);
    CHECK_EQ(be->close(bc, f), VFS_OK);
    CHECK_EQ(be->open(bc, f, "/w", VFS_READ), VFS_OK);
    CHECK_EQ(be->read(bc, f, b, sizeof b, &got), VFS_OK);
    CHECK_EQ(got, 6);
    CHECK(memcmp(b, "hELlo?", 6) == 0);
    be->close(bc, f);
    cleanup();
}

TEST(host_dirs_readdir_remove) {
    setup();
    CHECK_EQ(be->mkdir(bc, "/d"), VFS_OK);
    CHECK_EQ(be->mkdir(bc, "/d"), VFS_EXISTS);
    CHECK_EQ(put("/d/x", "1", VFS_WRITE), VFS_OK);
    CHECK_EQ(put("/d/y", "2", VFS_WRITE), VFS_OK);
    CHECK_EQ(be->mkdir(bc, "/d/sub"), VFS_OK);
    Ls l = {0};
    CHECK_EQ(be->readdir(bc, "/d", ls_cb, &l), VFS_OK);
    CHECK_EQ(l.n, 3);
    for (int i = 0; i < l.n; i++) {
        CHECK(strcmp(l.names[i], ".") && strcmp(l.names[i], ".."));
        CHECK_EQ(l.dir[i], strcmp(l.names[i], "sub") == 0);
    }
    Ls s = {0};
    s.stop_after = 1;
    CHECK_EQ(be->readdir(bc, "/d", ls_cb, &s), VFS_OK);          /* stops early, still OK */
    CHECK_EQ(s.n, 1);
    CHECK_EQ(be->readdir(bc, "/nope", ls_cb, &s), VFS_NOTFOUND);
    CHECK_EQ(be->readdir(bc, "/d/x", ls_cb, &s), VFS_NOTDIR);
    CHECK_EQ(be->remove(bc, "/d"), VFS_NOTEMPTY);
    CHECK_EQ(be->remove(bc, "/d/sub"), VFS_OK);
    CHECK_EQ(be->remove(bc, "/d/x"), VFS_OK);
    CHECK_EQ(be->remove(bc, "/d/x"), VFS_NOTFOUND);
    CHECK_EQ(be->remove(bc, "/d/y"), VFS_OK);
    CHECK_EQ(be->remove(bc, "/d"), VFS_OK);
    cleanup();
}

TEST(host_rename) {
    setup();
    char b[16];
    uint8_t f[GOGO_FS_SLOT_BYTES] __attribute__((aligned(8)));
    uint32_t got;
    CHECK_EQ(put("/a", "A", VFS_WRITE), VFS_OK);
    CHECK_EQ(put("/b", "B", VFS_WRITE), VFS_OK);
    CHECK_EQ(be->rename(bc, "/a", "/b", false), VFS_EXISTS);
    CHECK_EQ(be->rename(bc, "/a", "/c", false), VFS_OK);
    CHECK_EQ(be->rename(bc, "/a", "/z", false), VFS_NOTFOUND);
    CHECK_EQ(be->rename(bc, "/c", "/b", true), VFS_OK);           /* replaces */
    CHECK_EQ(be->open(bc, f, "/b", VFS_READ), VFS_OK);
    CHECK_EQ(be->read(bc, f, (uint8_t *)b, sizeof b, &got), VFS_OK);
    CHECK_EQ(got, 1);
    CHECK_EQ(b[0], 'A');
    be->close(bc, f);
    cleanup();
}

TEST(host_usage_and_vfs) {
    setup();
    int32_t used = -1, total = -1;
    CHECK_EQ(be->usage(bc, &used, &total), VFS_OK);
    CHECK(total > 0);
    CHECK(used > 0);
    CHECK(used <= total);
    vfs_reset(&vs);
    CHECK(vfs_mount(&vs, "/flash", be, bc));
    int s;
    CHECK_EQ(vfs_open(&vs, S("/flash/h.txt"), VFS_WRITE, &s), VFS_OK);
    CHECK_EQ(vfs_write(&vs, s, (const uint8_t *)"hi", 2), VFS_OK);
    vfs_close(&vs, s);
    int32_t sz;
    bool dir;
    CHECK_EQ(vfs_stat(&vs, S("/flash/h.txt"), &sz, &dir), VFS_OK);
    CHECK_EQ(sz, 2);
    CHECK_EQ(vfs_stat(&vs, S("/sd/h.txt"), &sz, &dir), VFS_NOTMOUNTED);
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
    CHECK_EQ(be->readdir(bc, "/", ls_cb, &l), VFS_OK);
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
    CHECK_EQ(vfs_mkdir(&vs, S("/flash/d")), VFS_OK);
    CHECK_EQ(vfs_open(&vs, S("/flash/d/x"), VFS_WRITE, &s1), VFS_OK);
    CHECK_EQ(vfs_rename(&vs, S("/flash/d"), S("/flash/e")), VFS_BUSY);
    CHECK_EQ(vfs_open(&vs, S("/flash/e/x"), VFS_APPEND, &s2), VFS_NOTFOUND);
    CHECK_EQ(vfs_open(&vs, S("/flash/d/x"), VFS_APPEND, &s2), VFS_BUSY);
    vfs_close(&vs, s1);
    CHECK_EQ(vfs_rename(&vs, S("/flash/d"), S("/flash/e")), VFS_OK);
    CHECK_EQ(vfs_stat(&vs, S("/flash/e/x"), &sz, &dir), VFS_OK);
    /* trailing dot */
    CHECK_EQ(vfs_open(&vs, S("/flash/a.txt"), VFS_WRITE, &s1), VFS_OK);
    CHECK_EQ(vfs_open(&vs, S("/flash/a.txt."), VFS_WRITE, &s2), VFS_BADPATH);
    CHECK_EQ(vfs_remove(&vs, S("/flash/a.txt..")), VFS_BADPATH);
    vfs_close(&vs, s1);
    /* into its own subtree */
    CHECK_EQ(vfs_mkdir(&vs, S("/flash/p")), VFS_OK);
    CHECK_EQ(vfs_rename(&vs, S("/flash/p"), S("/flash/p/q")), VFS_BADPATH);
    CHECK_EQ(vfs_rename(&vs, S("/flash/p"), S("/flash/p")), VFS_EXISTS);
    CHECK_EQ(vfs_stat(&vs, S("/flash/p"), &sz, &dir), VFS_OK);
    CHECK(dir);
    /* a case-only rename is not refused by vfs; whether it happens is up
     * to the host file system (macOS is usually case-insensitive) */
    bool host_ci = vfs_stat(&vs, S("/flash/P"), &sz, &dir) == VFS_OK;
    CHECK_EQ(vfs_rename(&vs, S("/flash/p"), S("/flash/P")), host_ci ? VFS_EXISTS : VFS_OK);
    vfs_reset(&vs);
    cleanup();
}
