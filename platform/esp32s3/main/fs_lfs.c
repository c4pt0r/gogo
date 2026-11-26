/* /flash: LittleFS (runtime/vfs/lfs/lfs_be.c, over joltwallet/littlefs's
 * raw lfs.h API; never esp_vfs_littlefs_register) on the `gogofs`
 * partition. This file is the board's part: the partition as a BlockDev,
 * the volume's static storage, and the size and format settings.
 *
 * Size: config fs.flash.size (bytes, optional K/M suffix, rounded down to
 * 4096, clamped to 64 KB .. the partition; unset = the whole partition).
 * Mount reads the size from the superblock; a larger setting grows the
 * file system in place (data kept); a smaller one keeps the current size
 * and says so (shrinking needs a format). fs.flash.format = yes formats
 * once at the set size and is erased first (if that fails, no format: it
 * would repeat every boot). A blank or unreadable file system (first boot)
 * is formatted; any other mount error (IO), or a file system bigger than
 * the partition, leaves /flash unmounted and its data untouched
 * (lfs_be_mount).
 *
 * A format can also be asked for without the config store:
 * fs_lfs_request_format (`gogo device format-flash`, config_esp.c) sets a
 * flag in RTC_NOINIT memory, which keeps its contents over the software
 * restart that follows. That memory is not cleared by any reset (after a
 * power-on it holds whatever it powered up with), so the flag counts only
 * if both words match (a magic and its complement) and the reset reason
 * is a software restart; the first mount clears it whatever it found,
 * so it formats at most once. fs.flash.format = yes still works
 * (deprecated).
 *
 * The VM task calls in through the backend, and the console task too
 * (`gogo push`, runtime/configsvc), so the volume has a lock
 * (LfsOpts.lock): every backend call holds it.
 *
 * LittleFS cannot reach the heap: lfs.c is compiled with LFS_NO_MALLOC
 * and with malloc / free renamed to lfs_be.c's NULL-returning stubs
 * (platform/esp32s3/CMakeLists.txt). (The component's
 * CONFIG_LITTLEFS_MALLOC_STRATEGY_DISABLE would do it too, but its unused
 * VFS wrapper does not compile with it.) */
#include "vfs_int.h"
#include "lfs_be.h"
#include "platform_esp.h"
#include "host.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define BLOCK      4096u
#define MIN_BYTES  (64u * 1024u)
#define STATE_MAX  3072u

static LfsVol flash_vol;            /* the volume (lfs_be.h): LittleFS state and buffers */
static BlockDev flash_bd;           /* the partition (ctx); set at mount */

/* The storage fixed state (vfs tables + this file's statics) fits 3 KB. */
#define LFS_STATE_BYTES (sizeof flash_vol + sizeof flash_bd)
#define FS_STATE_BYTES (VFS_STATE_BYTES + LFS_STATE_BYTES)
typedef char fs_state_fits[FS_STATE_BYTES <= STATE_MAX ? 1 : -1];
typedef char lfs_state_bounded[LFS_STATE_BYTES <= FS_LFS_STATE_MAX ? 1 : -1];   /* fs_fat.c counts on it */

uint32_t fs_lfs_state_bytes(void) { return (uint32_t)FS_STATE_BYTES; }

/* One LittleFS user at a time: the VM task or the console task (push).
 * Created by fs_lfs_mount before anything can use it. */
static StaticSemaphore_t lock_buf;
static SemaphoreHandle_t lock;
static void vol_lock(bool take) {
    if (take) xSemaphoreTake(lock, portMAX_DELAY);
    else xSemaphoreGive(lock);
}

static const LfsOpts flash_opts = { "/flash", "gogo device format-flash", platform_esp_say, vol_lock };

/* ---- the partition as a BlockDev ---- */

static int bd_read(void *ctx, uint32_t b, uint32_t off, void *buf, uint32_t n) {
    return esp_partition_read(ctx, (size_t)b * BLOCK + off, buf, n) == ESP_OK ? 0 : -1;
}
static int bd_prog(void *ctx, uint32_t b, uint32_t off, const void *buf, uint32_t n) {
    return esp_partition_write(ctx, (size_t)b * BLOCK + off, buf, n) == ESP_OK ? 0 : -1;
}
static int bd_erase(void *ctx, uint32_t b) {
    return esp_partition_erase_range(ctx, (size_t)b * BLOCK, BLOCK) == ESP_OK ? 0 : -1;
}

/* ---- settings ---- */

/* fs.flash.size in blocks: unset (or bad, said) = the whole partition. */
static uint32_t wanted_blocks(uint32_t part_blocks) {
    char v[16];
    int32_t n = config_esp_get("fs.flash.size", (uint8_t *)v, sizeof v - 1u);
    if (n < 0) return part_blocks;
    uint64_t bytes = 0;
    int32_t i = 0;
    bool ok = n > 0 && n < (int32_t)sizeof v;
    for (; ok && i < n && v[i] >= '0' && v[i] <= '9'; i++) {
        bytes = bytes * 10u + (uint64_t)(v[i] - '0');
        if (bytes > 0xFFFFFFFFull) ok = false;
    }
    if (ok && i == 0) ok = false;
    if (ok && i < n) {
        char s = v[i++];
        if (s == 'K' || s == 'k') bytes *= 1024u;
        else if (s == 'M' || s == 'm') bytes *= 1024u * 1024u;
        else ok = false;
        if (i != n) ok = false;
    }
    if (!ok) {
        platform_esp_say("gogo: bad fs.flash.size (bytes, K or M suffix); using the whole partition");
        return part_blocks;
    }
    if (bytes < MIN_BYTES) bytes = MIN_BYTES;
    uint64_t b = bytes / BLOCK;
    return b > part_blocks ? part_blocks : (uint32_t)b;
}

/* fs_lfs_request_format's flag, in RTC_NOINIT memory (see the top): two
 * words, so stray contents after a power-on cannot look like it. */
#define FORMAT_MAGIC 0x464D5447u   /* "GTMF" */
static RTC_NOINIT_ATTR uint32_t format_flag[2];

void fs_lfs_request_format(void) {
    format_flag[0] = FORMAT_MAGIC;
    format_flag[1] = ~FORMAT_MAGIC;
}

/* The RTC flag (cleared here: it formats once), after a software restart. */
static bool format_flagged(void) {
    bool on = format_flag[0] == FORMAT_MAGIC && format_flag[1] == ~FORMAT_MAGIC && esp_reset_reason() == ESP_RST_SW;
    format_flag[0] = format_flag[1] = 0;
    return on;
}

/* The deprecated config key fs.flash.format = yes. */
static bool format_requested(void) {
    char v[4];
    return config_esp_get("fs.flash.format", (uint8_t *)v, sizeof v) == 3 && !memcmp(v, "yes", 3);
}

/* ---- mount ---- */

/* The board's /flash (platform_esp.c lists it; vfs_init mounts it). */
const VmMount esp_flash_mount = { "/flash", &lfs_be_backend, &flash_vol };

int fs_lfs_mount(void) {
    lock = xSemaphoreCreateMutexStatic(&lock_buf);
    bool flagged = format_flagged();
    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_LITTLEFS, "gogofs");
    if (!part) {
        platform_esp_say("gogo: /flash unavailable (no gogofs partition; flash the partition table)");
        return -1;
    }
    uint32_t part_blocks = (uint32_t)(part->size / BLOCK);
    flash_bd = (BlockDev){ (void *)part, BLOCK, part_blocks, bd_read, bd_prog, bd_erase, NULL };
    uint32_t want = wanted_blocks(part_blocks);

    int mode = LFS_BE_AUTO;
    if (flagged) {
        platform_esp_say("gogo: formatting /flash (gogo device format-flash)");
        mode = LFS_BE_FORMAT;
    } else if (format_requested()) {
        /* Clear the request first: if that fails, formatting now would
         * wipe /flash again at every boot, so do not format at all. */
        if (config_esp_erase("fs.flash.format") != 0) {
            platform_esp_say("gogo: could not clear fs.flash.format; /flash not formatted (gogo config rm fs.flash.format)");
            mode = LFS_BE_MOUNT_ONLY;
        } else {
            platform_esp_say("gogo: formatting /flash (fs.flash.format=yes)");
            mode = LFS_BE_FORMAT;
        }
    }
    if (lfs_be_mount(&flash_vol, &flash_bd, &flash_opts, want, mode) != 0) return -1;
    platform_esp_log("gogo: /flash %lu KB (partition %lu KB)", (unsigned long)lfs_be_blocks(&flash_vol) * 4u,
                     (unsigned long)part_blocks * 4u);
    return 0;
}

