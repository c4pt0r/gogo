/* /sd: FatFs (runtime/vfs/fat/fat_be.c, over ESP-IDF's fatfs component,
 * its f_* API only; never esp_vfs_fat_*) on a microSD card through the
 * ESP-IDF sdspi driver. Built only for boards with HAS_SD (board.h:
 * Cardputer ADV, SD on SPI2 with CS G12, MOSI G14, CLK G40, MISO G39; SPI3
 * is the LCD's). This file is the board's part: the card as a DiskHal,
 * FatFs's disk layer hooked to it, and the volume's static storage.
 *
 * Mounting is lazy (fat_be.h). Boot only registers /sd with vfs; the first
 * path call brings the card up. attach initialises the card at 20 MHz
 * (attempt 0) or 400 kHz (attempt 1); a missing card fails within one CMD0
 * exchange per speed (MISO pulled up: no answer reads 0xFF), so every call
 * without a card is NotMounted in well under a second. check is one CMD13
 * (SEND_STATUS): a card swapped between two calls fails it (a fresh card
 * is not in SPI mode) and is remounted before FatFs touches it.
 *
 * No malloc on the file path: the FATFS object (its one 512-byte window,
 * CONFIG_FATFS_USE_DYN_BUFFERS=n, FF_MAX_SS 512 via CONFIG_WL_SECTOR_SIZE
 * _512) is in the FatVol, the card state and a 512-byte bounce sector are
 * static here, files live in their vfs slot (FIL without a private buffer:
 * FF_FS_TINY, CONFIG_FATFS_PER_FILE_CACHE=n), long names are on the stack
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
 * The first Usage on a FAT32 card without a valid FSINFO, or on an exFAT
 * card, scans the FAT / allocation bitmap and can take seconds.
 *
 * FAT12/16/32 and exFAT: FF_FS_EXFAT follows GOGO_FS_EXFAT (default y) in the
 * vendored fatfs component (platform/esp32s3/components/fatfs, IDF hard-codes
 * 0); with it off an exFAT card is the "no FAT or exFAT" path. exFAT adds a
 * directory-entry buffer on the stack next to the long name (LFN on stack)
 * and a few bytes to FATFS and FIL (both checked).
 *
 * Only the VM task calls in. */
#include "vfs_int.h"
#include "fat_be.h"
#include "platform_esp.h"
#include "host.h"
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
#if FF_FS_EXFAT != !!CONFIG_GOGO_FS_EXFAT
#error "FF_FS_EXFAT does not follow GOGO_FS_EXFAT: the vendored platform/esp32s3/components/fatfs is not the one being built"
#endif

#define SD_HOST   SPI2_HOST
#define PDRV      0
#define SECTOR    512u
#define FAST_KHZ  20000
#define STATE_MAX 3072u

static FatVol sd_vol;                /* the volume (fat_be.h): the FATFS object */
static struct {
    sdmmc_card_t card;
    uint32_t bounce[SECTOR / 4u];   /* DMA-able, 4-aligned */
    sdspi_dev_handle_t dev;
    uint8_t bus;                    /* 0 not tried, 1 up, 2 failed */
} F;

/* Fixed state: vfs tables + LittleFS (bounded, fs_lfs.c) + this file. */
typedef char fs_state_fits[VFS_STATE_BYTES + FS_LFS_STATE_MAX + sizeof sd_vol + sizeof F <= STATE_MAX ? 1 : -1];

uint32_t fs_fat_state_bytes(void) { return (uint32_t)(sizeof sd_vol + sizeof F); }

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

/* ---- the card as a DiskHal ---- */

static bool direct(const void *p) { return ((uintptr_t)p & 3u) == 0 && esp_ptr_dma_capable(p); }

static int c_read(void *ctx, uint32_t sector, uint8_t *buf, uint32_t n) {
    (void)ctx;
    if (direct(buf)) return sdmmc_read_sectors(&F.card, buf, sector, n) == ESP_OK ? 0 : -1;
    for (uint32_t i = 0; i < n; i++) {
        if (sdmmc_read_sectors(&F.card, F.bounce, sector + i, 1) != ESP_OK) return -1;
        memcpy(buf + i * SECTOR, F.bounce, SECTOR);
    }
    return 0;
}
static int c_write(void *ctx, uint32_t sector, const uint8_t *buf, uint32_t n) {
    (void)ctx;
    if (direct(buf)) return sdmmc_write_sectors(&F.card, buf, sector, n) == ESP_OK ? 0 : -1;
    for (uint32_t i = 0; i < n; i++) {
        memcpy(F.bounce, buf + i * SECTOR, SECTOR);
        if (sdmmc_write_sectors(&F.card, F.bounce, sector + i, 1) != ESP_OK) return -1;
    }
    return 0;
}
static uint32_t c_sectors(void *ctx) { (void)ctx; return (uint32_t)F.card.csd.capacity; }
static int c_check(void *ctx) { (void)ctx; return sdmmc_get_status(&F.card) == ESP_OK ? 0 : -1; }

/* ---- FatFs's disk layer (IDF's driver table for drive 0) ---- */

static DSTATUS d_init(BYTE pdrv) { (void)pdrv; return 0; }
static DSTATUS d_status(BYTE pdrv) { (void)pdrv; return fat_be_disk_status(&sd_vol); }
static DRESULT d_read(BYTE pdrv, BYTE *buf, uint32_t sector, UINT n) { (void)pdrv; return fat_be_disk_read(&sd_vol, buf, sector, n); }
static DRESULT d_write(BYTE pdrv, const BYTE *buf, uint32_t sector, UINT n) { (void)pdrv; return fat_be_disk_write(&sd_vol, buf, sector, n); }
static DRESULT d_ioctl(BYTE pdrv, BYTE cmd, void *buf) { (void)pdrv; return fat_be_disk_ioctl(&sd_vol, cmd, buf); }
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

/* Card init for mount attempt 0 (20 MHz) or 1 (400 kHz): 0 ok, 1 failed,
 * 2 no bus. */
static int c_attach(void *ctx, int attempt) {
    (void)ctx;
    if (!bus_up()) return 2;
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = F.dev;
    host.max_freq_khz = attempt == 0 ? FAST_KHZ : SDMMC_FREQ_PROBING;
    return sdmmc_card_init(&host, &F.card) == ESP_OK ? 0 : 1;
}

static void c_mounted(void *ctx, bool exfat) {
    (void)ctx;
    platform_esp_log("gogo: /sd mounted (%s, %lu MB, %d kHz)", exfat ? "exFAT" : "FAT",
                     (unsigned long)((uint64_t)F.card.csd.capacity * SECTOR >> 20), F.card.real_freq_khz);
}

static const DiskHal sd_hal = {
    .attach = c_attach, .check = c_check, .read = c_read, .write = c_write, .sectors = c_sectors,
    .mounted = c_mounted,
};

/* The board's /sd (platform_esp.c lists it; vfs_init mounts it). */
const VmMount esp_sd_mount = { .prefix = "/sd", .be = &fat_be_backend, .ctx = &sd_vol };

void fs_fat_init(void) { fat_be_init(&sd_vol, &sd_hal, "/sd", platform_esp_say); }
