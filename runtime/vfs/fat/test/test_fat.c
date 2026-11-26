/* The FatFs backend (fat_be.c) over a RAM DiskHal, with the vendored FatFs
 * built for the host (stub/sdkconfig.h: the Cardputer's options): the
 * vfs contract through vfs_*, the lazy mount and its second attempt, a
 * medium without a FAT (said once), a medium pulled and put back (check
 * fails: remount), and a disk error (unmount, then remount). */
#include "test.h"
#include "fat_be.h"
#include "vfs_int.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void test_host_arena_forget(void);
void test_host_arena_forget(void) {}   /* test_main.c calls it; no VM here */

#define SECTORS 8192u   /* 4 MB */
static uint8_t *disk;
static int attaches, fail_attach0, fail_reads, present = 1, says;
static char said[256];

static int d_attach(void *ctx, int attempt) {
    (void)ctx;
    attaches++;
    if (!present) return 1;
    if (attempt == 0 && fail_attach0) { fail_attach0--; return 1; }
    return 0;
}
static int d_check(void *ctx) { (void)ctx; return present ? 0 : -1; }
static int d_read(void *ctx, uint32_t s, uint8_t *buf, uint32_t n) {
    (void)ctx;
    if (fail_reads) { fail_reads--; return -1; }
    if (!present || s + n > SECTORS) return -1;
    memcpy(buf, disk + (size_t)s * 512u, (size_t)n * 512u);
    return 0;
}
static int d_write(void *ctx, uint32_t s, const uint8_t *buf, uint32_t n) {
    (void)ctx;
    if (!present || s + n > SECTORS) return -1;
    memcpy(disk + (size_t)s * 512u, buf, (size_t)n * 512u);
    return 0;
}
static uint32_t d_sectors(void *ctx) { (void)ctx; return SECTORS; }
static const DiskHal hal = { NULL, d_attach, d_check, d_read, d_write, d_sectors, NULL };

static void say(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(said, sizeof said, fmt, ap);
    va_end(ap);
    says++;
}

/* ---- FatFs's glue, as a board writes it (drive 0 -> the volume) ---- */
static FatVol vol;
PARTITION VolToPart[FF_VOLUMES] = { { 0, 0 } };
DSTATUS ff_disk_initialize(BYTE pdrv) { (void)pdrv; return 0; }
DSTATUS ff_disk_status(BYTE pdrv) { (void)pdrv; return fat_be_disk_status(&vol); }
DRESULT ff_disk_read(BYTE pdrv, BYTE *buf, LBA_t sector, UINT n) { (void)pdrv; return fat_be_disk_read(&vol, buf, sector, n); }
DRESULT ff_disk_write(BYTE pdrv, const BYTE *buf, LBA_t sector, UINT n) { (void)pdrv; return fat_be_disk_write(&vol, buf, sector, n); }
DRESULT ff_disk_ioctl(BYTE pdrv, BYTE cmd, void *buf) { (void)pdrv; return fat_be_disk_ioctl(&vol, cmd, buf); }
int ff_mutex_create(int v) { (void)v; return 1; }
void ff_mutex_delete(int v) { (void)v; }
int ff_mutex_take(int v) { (void)v; return 1; }
void ff_mutex_give(int v) { (void)v; }
DWORD get_fattime(void) { return ((DWORD)(2025 - 1980) << 25) | (1u << 21) | (1u << 16); }
void *ff_memalloc(size_t n) { return malloc(n); }
void ff_memfree(void *p) { free(p); }

static VfsState vs;
#define S(x) (x), (uint32_t)strlen(x)

static void setup(bool format) {
    if (!disk) disk = malloc((size_t)SECTORS * 512u);
    memset(disk, 0, (size_t)SECTORS * 512u);
    attaches = fail_attach0 = fail_reads = says = 0;
    present = 1;
    said[0] = 0;
    fat_be_init(&vol, &hal, "/sd", say);
    vfs_reset(&vs);
    CHECK(vfs_mount(&vs, "/sd", &fat_be_backend, &vol));
    if (format) {
        static uint8_t work[4096];
        MKFS_PARM opt = { FM_FAT | FM_SFD, 0, 0, 0, 0 };
        CHECK_EQ(f_mkfs("0:", &opt, work, sizeof work), FR_OK);
    }
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

TEST(fat_files_and_rules) {
    setup(true);
    char b[64];
    int32_t size, used, total;
    bool dir;
    CHECK_EQ(put("/sd/a.txt", "hello"), GOGO_E_OK);
    CHECK_EQ(get("/sd/a.txt", b, sizeof b), GOGO_E_OK);
    CHECK(!strcmp(b, "hello"));
    CHECK_EQ(get("/sd/A.TXT", b, sizeof b), GOGO_E_OK);      /* names ignore case */
    CHECK_EQ(vfs_stat(&vs, S("/sd/a.txt"), &size, &dir), GOGO_E_OK);
    CHECK_EQ(size, 5);
    CHECK(!dir);
    CHECK_EQ(vfs_mkdir(&vs, S("/sd/d")), GOGO_E_OK);
    CHECK_EQ(put("/sd/d/x", "1"), GOGO_E_OK);
    CHECK_EQ(vfs_remove(&vs, S("/sd/d")), GOGO_E_NOTEMPTY);
    CHECK_EQ(vfs_rename(&vs, S("/sd/a.txt"), S("/sd/d/x")), GOGO_E_EXISTS);
    CHECK_EQ(vfs_replace(&vs, S("/sd/a.txt"), S("/sd/d/x")), GOGO_E_OK);   /* remove, then rename */
    CHECK_EQ(get("/sd/d/x", b, sizeof b), GOGO_E_OK);
    CHECK(!strcmp(b, "hello"));
    CHECK_EQ(vfs_stat(&vs, S("/sd/nope"), &size, &dir), GOGO_E_NOTFOUND);
    CHECK_EQ(vfs_usage(&vs, S("/sd"), &used, &total), GOGO_E_OK);
    CHECK(total > 3000000 && used > 0 && used < total);
    CHECK_EQ(attaches, 1);                                   /* mounted once, lazily */
    CHECK_EQ(says, 0);
}

TEST(fat_second_attempt) {
    setup(true);
    fail_attach0 = 1;                                        /* the fast clock fails */
    char b[8];
    CHECK_EQ(get("/sd/none", b, sizeof b), GOGO_E_NOTFOUND);
    CHECK_EQ(attaches, 2);
}

TEST(fat_no_filesystem_said_once) {
    setup(false);                                            /* a blank medium */
    disk[3] = 'N'; disk[4] = 'O'; disk[5] = 'F'; disk[6] = 'S';
    char b[8];
    CHECK_EQ(get("/sd/x", b, sizeof b), GOGO_E_NOTMOUNTED);
    CHECK_EQ(attaches, 1);                                   /* no second attempt: the medium reads fine */
    CHECK_EQ(get("/sd/x", b, sizeof b), GOGO_E_NOTMOUNTED);
    CHECK_EQ(says, 1);
    CHECK(strstr(said, "/sd: no FAT or exFAT") != NULL);
    CHECK(strstr(said, "NOFS") != NULL);
}

TEST(fat_pulled_and_disk_error) {
    setup(true);
    char b[16];
    CHECK_EQ(put("/sd/k", "v1"), GOGO_E_OK);
    present = 0;                                             /* pulled */
    CHECK_EQ(get("/sd/k", b, sizeof b), GOGO_E_NOTMOUNTED);
    present = 1;                                             /* put back: the next call remounts */
    CHECK_EQ(get("/sd/k", b, sizeof b), GOGO_E_OK);
    CHECK(!strcmp(b, "v1"));
    /* a read error inside a call: IO, unmounted, the next call remounts */
    static char big[2049];
    memset(big, 'x', 2048);
    CHECK_EQ(put("/sd/big", big), GOGO_E_OK);
    int a = attaches, slot;
    CHECK_EQ(vfs_open(&vs, S("/sd/big"), VFS_READ, &slot), GOGO_E_OK);
    static uint8_t rb[2048];
    uint32_t got;
    fail_reads = 1000;
    CHECK_EQ(vfs_read(&vs, slot, rb, sizeof rb, &got), GOGO_E_IO);
    fail_reads = 0;
    vfs_close(&vs, slot);
    CHECK_EQ(get("/sd/k", b, sizeof b), GOGO_E_OK);
    CHECK(!strcmp(b, "v1"));
    CHECK_EQ(attaches, a + 1);
}
