/* The LittleFS backend (lfs_be.c) over a RAM BlockDev, with the LittleFS
 * every firmware build fetches (managed_components): the mount policy
 * (first-boot format, grow in place, shrink refused, a file system bigger
 * than the device refused in every mode), the block bounds, the geometry
 * check, the lock (every take given back), files through vfs, and gogo
 * push through the config service into the volume. */
#include "test.h"
#include "lfs_be.h"
#include "vfs_int.h"
#include "configsvc.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void test_host_arena_forget(void);
void test_host_arena_forget(void) {}   /* test_main.c calls it; no VM here */

#define BLOCK  4096u
#define NBLOCK 64u
static uint8_t flash[NBLOCK * BLOCK];
static uint32_t dev_blocks;   /* what the BlockDev says it has */
static int outside;           /* calls past dev_blocks or a block's end */
static int says;
static char said[256];

static int f_read(void *ctx, uint32_t b, uint32_t off, void *buf, uint32_t n) {
    (void)ctx;
    if (b >= dev_blocks || off + n > BLOCK) { outside++; return -1; }
    memcpy(buf, flash + b * BLOCK + off, n);
    return 0;
}
static int f_prog(void *ctx, uint32_t b, uint32_t off, const void *buf, uint32_t n) {
    (void)ctx;
    if (b >= dev_blocks || off + n > BLOCK) { outside++; return -1; }
    memcpy(flash + b * BLOCK + off, buf, n);
    return 0;
}
static int f_erase(void *ctx, uint32_t b) {
    (void)ctx;
    if (b >= dev_blocks) { outside++; return -1; }
    memset(flash + b * BLOCK, 0xFF, BLOCK);
    return 0;
}

static int depth, takes;
static void lock(bool take) {
    if (take) { depth++; takes++; CHECK_EQ(depth, 1); }
    else { depth--; CHECK_EQ(depth, 0); }
}
static void say(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(said, sizeof said, fmt, ap);
    va_end(ap);
    says++;
}
static const LfsOpts opts = { "/flash", "gogo device format-flash", say, lock };

static LfsVol vol;
static BlockDev bd;
static VfsState vs;
#define S(x) (x), (uint32_t)strlen(x)

static int mount(uint32_t blocks, uint32_t want, int mode) {
    dev_blocks = blocks;
    bd = (BlockDev){ .block_size = BLOCK, .block_count = blocks, .read = f_read, .prog = f_prog, .erase = f_erase };
    says = 0;
    said[0] = 0;
    int e = lfs_be_mount(&vol, &bd, &opts, want, mode);
    vfs_reset(&vs);
    CHECK(vfs_mount(&vs, "/flash", &lfs_be_backend, &vol));
    return e;
}

static void blank(void) {
    memset(flash, 0xFF, sizeof flash);
    outside = 0;
}

static int put(const char *p, const char *data) {
    int slot, e = vfs_open(&vs, S(p), VFS_WRITE, &slot);
    if (e) return e;
    e = vfs_write(&vs, slot, (const uint8_t *)data, (uint32_t)strlen(data));
    vfs_close(&vs, slot);
    return e;
}
static int get(const char *p, char *out, uint32_t cap) {
    int slot, e = vfs_open(&vs, S(p), VFS_READ, &slot);
    uint32_t got = 0;
    if (e) return e;
    e = vfs_read(&vs, slot, (uint8_t *)out, cap - 1u, &got);
    out[got] = 0;
    vfs_close(&vs, slot);
    return e;
}

TEST(lfs_first_boot_files_and_lock) {
    blank();
    CHECK_EQ(mount(NBLOCK, 32, LFS_BE_AUTO), 0);
    CHECK(strstr(said, "formatting /flash") != NULL);
    CHECK_EQ(lfs_be_blocks(&vol), 32);
    char b[32];
    int32_t used, total, size;
    bool dir;
    takes = 0;
    CHECK_EQ(put("/flash/a", "hello"), GOGO_E_OK);
    CHECK_EQ(get("/flash/a", b, sizeof b), GOGO_E_OK);
    CHECK(!strcmp(b, "hello"));
    CHECK_EQ(get("/flash/A", b, sizeof b), GOGO_E_NOTFOUND);   /* case-sensitive */
    CHECK_EQ(vfs_mkdir(&vs, S("/flash/d")), GOGO_E_OK);
    CHECK_EQ(put("/flash/d/x", "1"), GOGO_E_OK);
    CHECK_EQ(vfs_remove(&vs, S("/flash/d")), GOGO_E_NOTEMPTY);
    CHECK_EQ(vfs_rename(&vs, S("/flash/a"), S("/flash/d/x")), GOGO_E_EXISTS);
    CHECK_EQ(vfs_replace(&vs, S("/flash/a"), S("/flash/d/x")), GOGO_E_OK);
    CHECK_EQ(vfs_stat(&vs, S("/flash/d/x"), &size, &dir), GOGO_E_OK);
    CHECK_EQ(size, 5);
    CHECK_EQ(vfs_usage(&vs, S("/flash"), &used, &total), GOGO_E_OK);
    CHECK_EQ(total, 32 * BLOCK);
    CHECK(takes > 10);
    CHECK_EQ(depth, 0);                                       /* every take given back */
    CHECK_EQ(outside, 0);
}

TEST(lfs_grow_shrink_too_big) {
    blank();
    CHECK_EQ(mount(NBLOCK, 32, LFS_BE_AUTO), 0);
    CHECK_EQ(put("/flash/keep", "data"), GOGO_E_OK);
    CHECK_EQ(mount(NBLOCK, 48, LFS_BE_AUTO), 0);               /* grows in place */
    CHECK_EQ(lfs_be_blocks(&vol), 48);
    char b[16];
    CHECK_EQ(get("/flash/keep", b, sizeof b), GOGO_E_OK);
    CHECK(!strcmp(b, "data"));
    CHECK_EQ(mount(NBLOCK, 16, LFS_BE_AUTO), 0);               /* a shrink is not applied */
    CHECK(strstr(said, "shrink needs format (gogo device format-flash)") != NULL);
    CHECK_EQ(lfs_be_blocks(&vol), 48);
    /* a device smaller than the file system: refused in every mode, data kept */
    CHECK(mount(40, 0, LFS_BE_AUTO) < 0);
    CHECK(strstr(said, "is bigger than its partition") != NULL);
    CHECK(mount(40, 0, LFS_BE_MOUNT_ONLY) < 0);
    CHECK(strstr(said, "is bigger than its partition") != NULL);
    CHECK_EQ(get("/flash/keep", b, sizeof b), GOGO_E_NOTMOUNTED);
    CHECK_EQ(outside, 0);                                     /* the device never saw a block past its end */
    CHECK_EQ(mount(NBLOCK, 0, LFS_BE_MOUNT_ONLY), 0);
    CHECK_EQ(get("/flash/keep", b, sizeof b), GOGO_E_OK);
    /* LFS_BE_MOUNT_ONLY never formats a blank device */
    blank();
    CHECK(mount(NBLOCK, 0, LFS_BE_MOUNT_ONLY) < 0);
    CHECK(strstr(said, "mount failed") != NULL);
    /* LFS_BE_FORMAT formats at the size asked */
    CHECK_EQ(mount(NBLOCK, 20, LFS_BE_FORMAT), 0);
    CHECK_EQ(lfs_be_blocks(&vol), 20);
}

TEST(lfs_geometry) {
    blank();
    dev_blocks = NBLOCK;
    bd = (BlockDev){ .block_size = BLOCK, .block_count = NBLOCK, .prog_size = 48, .read = f_read, .prog = f_prog,
                     .erase = f_erase };
    CHECK(lfs_be_mount(&vol, &bd, &opts, 0, LFS_BE_AUTO) < 0);
    CHECK(strstr(said, "geometry") != NULL);
    bd.prog_size = 32;                                        /* divides the 64-byte cache */
    CHECK_EQ(lfs_be_mount(&vol, &bd, &opts, 0, LFS_BE_AUTO), 0);
}

/* ---- gogo push into the volume ---- */
static char out[1024];
static void b_write(void *ctx, const char *const *parts, const size_t *lens, int n) {
    (void)ctx;
    size_t o = 0;
    for (int i = 0; i < n; i++) { memcpy(out + o, parts[i], lens[i]); o += lens[i]; }
    out[o] = 0;
}
static const ConfigBoard cfg_board = { .write = b_write };
static ConfigSvc svc;
static const char *req(const char *line) {
    char b[300];
    int n = snprintf(b, sizeof b, "\x02gogo-config %s\n", line);
    configsvc_feed(&svc, (const uint8_t *)b, (size_t)n);
    return out;
}

TEST(lfs_push_through_configsvc) {
    blank();
    CHECK_EQ(mount(NBLOCK, 0, LFS_BE_AUTO), 0);
    configsvc_init(&svc, NULL, &cfg_board, &lfs_be_backend, &vol);
    char l[96], b[32];
    uint32_t crc = configsvc_crc32(0, (const uint8_t *)"hello world", 11);
    snprintf(l, sizeof l, "push /flash/app.ggb 11 %x", (unsigned)crc);
    CHECK(!strcmp(req(l), "\x02ok 0\n"));
    CHECK(!strcmp(req("data 0 aGVsbG8g"), "\x02ok 6\n"));
    CHECK(!strcmp(req("data 6 d29ybGQ="), "\x02ok 11\n"));
    CHECK(!strcmp(req("end"), "\x02ok\n"));
    CHECK_EQ(get("/flash/app.ggb", b, sizeof b), GOGO_E_OK);
    CHECK(!strcmp(b, "hello world"));
    int32_t size;
    bool dir;
    CHECK_EQ(vfs_stat(&vs, S("/flash/.gogo-push"), &size, &dir), GOGO_E_NOTFOUND);
    CHECK_EQ(depth, 0);
}
