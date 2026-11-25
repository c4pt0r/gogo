/* /sd: a VfsBackend over FatFs (ESP-IDF's fatfs component, its f_* API
 * only; never esp_vfs_fat_*) on a microSD card through the ESP-IDF sdspi
 * driver. Built only for boards with HAS_SD (board.h: Cardputer ADV, SD on
 * SPI2 with CS G12, MOSI G14, CLK G40, MISO G39; SPI3 is the LCD's).
 *
 * Mounting is lazy. Boot only registers /sd with vfs. Every path call
 * (open, stat, readdir, mkdir, remove, rename, usage) first makes sure a
 * card is there: when mounted, one CMD13 (SEND_STATUS) checks the card
 * still answers; when not (or it stopped answering), the card is
 * initialised and FatFs mounted at 20 MHz, then once more at 400 kHz if
 * that failed (not when the card reads fine but holds no FAT). A missing card fails within one CMD0 exchange per
 * speed (MISO pulled up: no answer reads 0xFF), so every call without a
 * card is NotMounted in well under a second, and a card inserted later is
 * mounted by the next call, without a reboot. A card swapped between two
 * calls fails the CMD13 (a fresh card is not in SPI mode) and is remounted
 * before FatFs touches it. A disk error (FR_DISK_ERR / FR_NOT_READY) during
 * any call unmounts, so the next call remounts; open files of the old mount
 * then fail with IO (FatFs checks the volume id) and can still be closed.
 * An unformatted card is NotMounted (never formatted here).
 *
 * No malloc on the file path: the FATFS object (its one 512-byte window,
 * CONFIG_FATFS_USE_DYN_BUFFERS=n, FF_MAX_SS 512 via CONFIG_WL_SECTOR_SIZE
 * _512), the card state and a 512-byte bounce sector are static (F), files
 * live in their vfs slot (FIL without a private buffer: FF_FS_TINY,
 * CONFIG_FATFS_PER_FILE_CACHE=n), long names are on the stack
 * (CONFIG_FATFS_LFN_STACK, max 64). Sector transfers into a buffer the SPI
 * DMA cannot take directly (not 4-aligned, or not DMA-capable) go through
 * the bounce sector, so sdmmc_read/write_sectors never allocate a temporary
 * one. FatFs is built reentrant (IDF hard-codes FF_FS_REENTRANT 1, and its
 * mutexes come from the heap): only the VM task calls in, so ff_mutex_*
 * are replaced at link time by the no-op __wrap_ versions below (CMake
 * -Wl,--wrap). What is still allocated, once, by the IDF drivers on the
 * first call: the SPI bus (descriptors), the sdspi device, the diskio
 * table entry (20 bytes, ff_diskio_register); and on each card
 * initialisation sdmmc_card_init's short-lived buffers for the SCR / SSR
 * reads. None of these are per file or per transfer.
 *
 * FAT specifics: names compare ignoring case (case_insensitive; FatFs
 * itself matches long and short names that way); rename cannot replace, so
 * rename(replace=true) returns Exists and vfs removes then renames (not
 * atomic); case_insensitive also makes vfs refuse a Write open (truncate)
 * while the path is open for reading (runtime/vfs/vfs.h). Usage on a card above 2 GB
 * is clamped to INT32_MAX bytes; the first Usage on a FAT32 card without
 * a valid FSINFO, or on an exFAT card, scans the FAT / allocation bitmap
 * and can take seconds.
 *
 * FAT12/16/32 and exFAT: FF_FS_EXFAT follows GOGO_FS_EXFAT (default y) in the
 * vendored fatfs component (platform/esp32s3/components/fatfs, IDF hard-codes
 * 0); with it off an exFAT card is the "no FAT or exFAT" path. exFAT adds a
 * directory-entry buffer on the stack next to the long name (LFN on stack)
 * and a few bytes to FATFS and FIL (both checked below).
 *
 * Contract: see the VfsBackend comment in runtime/vfs/vfs.h. Only the VM task
 * calls in. */
#include "vfs_int.h"
#include "platform_esp.h"
#include "board.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ff.h"
#include "diskio_impl.h"
#include "driver/gpio.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_memory_utils.h"
#include "sdmmc_cmd.h"

#if !HAS_SD
#error "fs_fat.c is only built for boards with an SD slot (board.h HAS_SD)"
#endif
#if FF_MAX_SS != 512 || FF_USE_DYN_BUFFER || !FF_FS_TINY || FF_USE_LFN != 2 || FF_MAX_LFN != 64 || FF_FS_LOCK
#error "FatFs config: see sdkconfig.cardputer (512-byte sectors, static window, tiny, LFN on stack 64, no lock)"
#endif
#if FF_FS_EXFAT != !!CONFIG_GOGO_FS_EXFAT
#error "FF_FS_EXFAT does not follow GOGO_FS_EXFAT: the vendored platform/esp32s3/components/fatfs is not the one being built"
#endif

#define SD_HOST   SPI2_HOST
#define PDRV      0
#define SECTOR    512u
#define FAST_KHZ  20000
#define STATE_MAX 3072u

/* An open file: the FIL itself, in the vfs slot. */
typedef char fil_fits_slot[sizeof(FIL) <= GOGO_FS_SLOT_BYTES ? 1 : -1];

static struct {
    FATFS fs;
    sdmmc_card_t card;
    uint32_t bounce[SECTOR / 4u];   /* DMA-able, 4-aligned */
    sdspi_dev_handle_t dev;
    uint8_t bus;                    /* 0 not tried, 1 up, 2 failed */
    bool mounted;
    bool said_nofs;
} F;

/* Fixed state: vfs tables + LittleFS (bounded, fs_lfs.c) + this file. */
typedef char fs_state_fits[VFS_STATE_BYTES + FS_LFS_STATE_MAX + sizeof F <= STATE_MAX ? 1 : -1];

uint32_t fs_fat_state_bytes(void) { return (uint32_t)sizeof F; }

/* ---- FatFs locks: no-ops (one caller, the VM task) ---- */

int __wrap_ff_mutex_create(int vol);
void __wrap_ff_mutex_delete(int vol);
int __wrap_ff_mutex_take(int vol);
void __wrap_ff_mutex_give(int vol);
int __wrap_ff_mutex_create(int vol) { (void)vol; return 1; }
void __wrap_ff_mutex_delete(int vol) { (void)vol; }
int __wrap_ff_mutex_take(int vol) { (void)vol; return 1; }
void __wrap_ff_mutex_give(int vol) { (void)vol; }

/* ---- FatFs timestamps: a fixed time ----
 * IDF's get_fattime (fatfs/diskio/diskio.c) calls localtime_r, which pulls
 * picolibc's tzset, sscanf and vfscanf (about 7.6 KB) into the image. gogo has
 * no clock (spec storage: no timestamps), so the link wraps it to a constant:
 * 2025-01-01 00:00:00. The IDF version then has no caller and is discarded. */
uint32_t __wrap_get_fattime(void);
uint32_t __wrap_get_fattime(void) { return ((uint32_t)(2025 - 1980) << 25) | (1u << 21) | (1u << 16); }

/* ---- diskio: the card's sectors ---- */

static bool direct(const void *p) { return ((uintptr_t)p & 3u) == 0 && esp_ptr_dma_capable(p); }

static DSTATUS d_init(BYTE pdrv) { (void)pdrv; return 0; }
static DSTATUS d_status(BYTE pdrv) { (void)pdrv; return 0; }
static DRESULT d_read(BYTE pdrv, BYTE *buf, uint32_t sector, UINT n) {
    (void)pdrv;
    if (direct(buf)) return sdmmc_read_sectors(&F.card, buf, sector, n) == ESP_OK ? RES_OK : RES_ERROR;
    for (UINT i = 0; i < n; i++) {
        if (sdmmc_read_sectors(&F.card, F.bounce, sector + i, 1) != ESP_OK) return RES_ERROR;
        memcpy(buf + i * SECTOR, F.bounce, SECTOR);
    }
    return RES_OK;
}
static DRESULT d_write(BYTE pdrv, const BYTE *buf, uint32_t sector, UINT n) {
    (void)pdrv;
    if (direct(buf)) return sdmmc_write_sectors(&F.card, buf, sector, n) == ESP_OK ? RES_OK : RES_ERROR;
    for (UINT i = 0; i < n; i++) {
        memcpy(F.bounce, buf + i * SECTOR, SECTOR);
        if (sdmmc_write_sectors(&F.card, F.bounce, sector + i, 1) != ESP_OK) return RES_ERROR;
    }
    return RES_OK;
}
static DRESULT d_ioctl(BYTE pdrv, BYTE cmd, void *buf) {
    (void)pdrv;
    switch (cmd) {
    case CTRL_SYNC: return RES_OK;                 /* writes are synchronous */
    case GET_SECTOR_COUNT: *(LBA_t *)buf = (LBA_t)F.card.csd.capacity; return RES_OK;
    case GET_SECTOR_SIZE: *(WORD *)buf = SECTOR; return RES_OK;
    case GET_BLOCK_SIZE: *(DWORD *)buf = 1; return RES_OK;
    case CTRL_TRIM: return RES_OK;                 /* optional; skipped */
    default: return RES_PARERR;
    }
}
static const ff_diskio_impl_t disk = { d_init, d_status, d_read, d_write, d_ioctl };

/* ---- mount ---- */

/* SPI2 bus and the sdspi device, once (the drivers allocate here). */
static bool bus_up(void) {
    if (F.bus) return F.bus == 1;
    F.bus = 2;
    spi_bus_config_t bus = {
        .mosi_io_num = SD_PIN_MOSI,
        .miso_io_num = SD_PIN_MISO,
        .sclk_io_num = SD_PIN_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .data4_io_num = -1,
        .data5_io_num = -1,
        .data6_io_num = -1,
        .data7_io_num = -1,
        .max_transfer_sz = 4096,
    };
    sdspi_device_config_t dc = SDSPI_DEVICE_CONFIG_DEFAULT();
    dc.host_id = SD_HOST;
    dc.gpio_cs = SD_PIN_CS;
    if (spi_bus_initialize(SD_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK ||
        sdspi_host_init() != ESP_OK || sdspi_host_init_device(&dc, &F.dev) != ESP_OK) {
        platform_esp_say("gogo: /sd: SPI bus init failed");
        return false;
    }
    /* No card: MISO floats; pulled up it reads 0xFF (no answer) at once. */
    gpio_pullup_en(SD_PIN_MISO);
    ff_diskio_register(PDRV, &disk);
    F.bus = 1;
    return true;
}

/* Card init and mount at khz: 0 mounted, 1 failed (a slower clock may
 * help), 2 failed for good (no FAT on a card that reads fine). */
static int try_mount(int khz) {
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = F.dev;
    host.max_freq_khz = khz;
    if (sdmmc_card_init(&host, &F.card) != ESP_OK) return 1;
    FRESULT r = f_mount(&F.fs, "0:", 1);
    if (r == FR_OK) return 0;
    if (r == FR_NO_FILESYSTEM && !F.said_nofs) {
        /* Say what is there: MBR partition 1 type (0x07 = NTFS here,
         * 0xEE = GPT) or, with no MBR, the boot sector's OEM name. */
        const uint8_t *b = (const uint8_t *)F.bounce;
        char what[24] = "unreadable";
        if (sdmmc_read_sectors(&F.card, F.bounce, 0, 1) == ESP_OK) {
            if (b[510] == 0x55 && b[511] == 0xAA && b[446 + 4] != 0 && memcmp(b + 3, "NTFS", 4))
                snprintf(what, sizeof what, "partition type 0x%02X", b[446 + 4]);
            else {
                memcpy(what, b + 3, 8);
                what[8] = 0;
                for (int i = 0; i < 8; i++)
                    if (what[i] < ' ' || what[i] > '~') what[i] = '.';
            }
        }
        F.said_nofs = true;
        platform_esp_say("gogo: /sd: no FAT or exFAT file system on the card (%s; format it FAT32 or exFAT on a computer)", what);
    }
    f_mount(NULL, "0:", 0);
    return r == FR_NO_FILESYSTEM ? 2 : 1;
}

static void unmount(void) {
    if (!F.mounted) return;
    f_mount(NULL, "0:", 0);
    F.mounted = false;
}

/* A card is there and mounted: VFS_OK, else VFS_NOTMOUNTED. */
static int ready(void) {
    if (F.mounted) {
        if (sdmmc_get_status(&F.card) == ESP_OK) return VFS_OK;
        unmount();
    }
    if (!bus_up()) return VFS_NOTMOUNTED;
    int m = try_mount(FAST_KHZ);
    if (m == 1) m = try_mount(SDMMC_FREQ_PROBING);
    if (m != 0) return VFS_NOTMOUNTED;
    F.mounted = true;
    F.said_nofs = false;
    platform_esp_log("gogo: /sd mounted (%s, %lu MB, %d kHz)", F.fs.fs_type == FS_EXFAT ? "exFAT" : "FAT",
                     (unsigned long)((uint64_t)F.card.csd.capacity * SECTOR >> 20), F.card.real_freq_khz);
    return VFS_OK;
}

/* ---- backend ---- */

static int map_err(FRESULT r) {
    switch (r) {
    case FR_OK: return VFS_OK;
    case FR_NO_FILE: case FR_NO_PATH: return VFS_NOTFOUND;
    case FR_EXIST: return VFS_EXISTS;
    case FR_INVALID_NAME: return VFS_BADPATH;
    case FR_NOT_ENABLED: case FR_NO_FILESYSTEM: return VFS_NOTMOUNTED;
    case FR_DISK_ERR: case FR_NOT_READY:
        unmount();                                 /* the next call remounts */
        return VFS_IO;
    default: return VFS_IO;
    }
}

static int32_t clamp32(uint64_t v) { return v > INT32_MAX ? INT32_MAX : (int32_t)v; }

static bool is_root(const char *rel) { return rel[0] == '/' && rel[1] == 0; }

/* After FR_DENIED / FR_NO_FILE on a path that may be a directory. */
static bool is_dir(const char *rel) {
    FILINFO fi;
    return is_root(rel) || (f_stat(rel, &fi) == FR_OK && (fi.fattrib & AM_DIR));
}

static int t_open(void *f, const char *rel, int mode) {
    int e = ready();
    if (e) return e;
    if (is_root(rel)) return VFS_ISDIR;
    BYTE fl = mode == VFS_READ ? (FA_READ | FA_OPEN_EXISTING)
            : mode == VFS_WRITE ? (FA_WRITE | FA_CREATE_ALWAYS)
            : (FA_WRITE | FA_OPEN_APPEND);
    memset(f, 0, sizeof(FIL));
    FRESULT r = f_open(f, rel, fl);
    if (r == FR_NO_FILE || r == FR_DENIED) {
        if (is_dir(rel)) return VFS_ISDIR;
        if (r == FR_DENIED) {
            FILINFO fi;
            /* an existing (read-only) file: IO; else the directory is full */
            return f_stat(rel, &fi) == FR_OK ? VFS_IO : VFS_NOSPACE;
        }
    }
    return map_err(r);
}
static int t_read(void *f, uint8_t *buf, uint32_t n, uint32_t *got) {
    UINT br = 0;
    int e = map_err(f_read(f, buf, n, &br));
    if (e) return e;
    *got = br;
    return VFS_OK;
}
static int t_write(void *f, const uint8_t *buf, uint32_t n) {
    UINT bw = 0;
    int e = map_err(f_write(f, buf, n, &bw));
    if (e) return e;
    return bw == n ? VFS_OK : VFS_NOSPACE;
}
/* Overwrites inside the file (vfs checked the bounds), then goes back to
 * the end so the next write appends. FAT patches the clusters in place. */
static int t_write_at(void *f, uint32_t off, const uint8_t *buf, uint32_t n) {
    FIL *fp = f;
    FSIZE_t end = f_tell(fp);
    int e = map_err(f_lseek(fp, off));
    UINT bw = 0;
    if (!e) {
        e = map_err(f_write(fp, buf, n, &bw));
        if (!e && bw != n) e = VFS_NOSPACE;
    }
    int e2 = map_err(f_lseek(fp, end));
    return e ? e : e2;
}
static int t_sync(void *f) { return map_err(f_sync(f)); }
static int t_size(void *f, int32_t *out) {
    *out = clamp32(f_size((FIL *)f));
    return VFS_OK;
}
static int t_close(void *f) { return map_err(f_close(f)); }

static int t_stat(const char *rel, int32_t *size, bool *dir) {
    int e = ready();
    if (e) return e;
    if (is_root(rel)) { *dir = true; *size = 0; return VFS_OK; }
    FILINFO fi;
    e = map_err(f_stat(rel, &fi));
    if (e) return e;
    *dir = (fi.fattrib & AM_DIR) != 0;
    *size = *dir ? 0 : clamp32(fi.fsize);
    return VFS_OK;
}
static int t_readdir(const char *rel, VfsDirCb cb, void *ctx) {
    int e = ready();
    if (e) return e;
    FF_DIR d;
    FILINFO fi;
    FRESULT r = f_opendir(&d, rel);
    if (r == FR_NO_PATH || r == FR_NO_FILE) {
        FILINFO x;
        if (f_stat(rel, &x) == FR_OK) return VFS_NOTDIR;   /* a file */
    }
    if (r != FR_OK) return map_err(r);
    while ((r = f_readdir(&d, &fi)) == FR_OK && fi.fname[0]) {
        if (!strcmp(fi.fname, ".") || !strcmp(fi.fname, "..")) continue;
        if (cb(ctx, fi.fname, (fi.fattrib & AM_DIR) != 0) != 0) break;
    }
    f_closedir(&d);
    return map_err(r);
}
static int t_mkdir(const char *rel) {
    int e = ready();
    if (e) return e;
    FRESULT r = f_mkdir(rel);
    return r == FR_DENIED ? VFS_NOSPACE : map_err(r);   /* FatFs: no space / directory full */
}
static int t_remove(const char *rel) {
    int e = ready();
    if (e) return e;
    FRESULT r = f_unlink(rel);
    if (r == FR_DENIED) return is_dir(rel) ? VFS_NOTEMPTY : VFS_IO;   /* else read-only */
    return map_err(r);
}
static int t_rename(const char *from, const char *to, bool replace) {
    int e = ready();
    if (e) return e;
    (void)replace;               /* FAT cannot replace: Exists either way */
    return map_err(f_rename(from, to));
}
static int t_usage(int32_t *used, int32_t *total) {
    int e = ready();
    if (e) return e;
    DWORD nfree;
    FATFS *fs;
    e = map_err(f_getfree("0:", &nfree, &fs));
    if (e) return e;
    uint64_t clust = (uint64_t)fs->csize * SECTOR, all = (uint64_t)(fs->n_fatent - 2u) * clust;
    *total = clamp32(all);
    *used = clamp32(all - (uint64_t)nfree * clust);
    return VFS_OK;
}

static const VfsBackend backend = {
    true, t_open, t_read, t_write, t_sync, t_size, t_close,
    t_stat, t_readdir, t_mkdir, t_remove, t_rename, t_usage, t_write_at
};

int fs_fat_init(void) { return vfs_mount("/sd", &backend) ? 0 : -1; }
