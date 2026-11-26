/* /flash: a VfsBackend over LittleFS (joltwallet/littlefs, its raw lfs.h
 * API only; never esp_vfs_littlefs_register) on the `gogofs` partition.
 *
 * No malloc: the LittleFS state, its read / prog caches and lookahead
 * buffer are static (one struct, L), and each open file lives in its vfs
 * slot (Lf: lfs_file_t + its lfs_file_config + its 64-byte cache), opened
 * with lfs_file_opencfg. Enforced at build time: lfs.c is compiled with
 * LFS_NO_MALLOC (no lfs_file_open) and with malloc / free renamed to
 * gogo_lfs_malloc / gogo_lfs_free below (platform/esp32s3/CMakeLists.txt),
 * so LittleFS cannot reach the heap; a NULL buffer would be LFS_ERR_NOMEM.
 * (The component's CONFIG_LITTLEFS_MALLOC_STRATEGY_DISABLE would do it too,
 * but its unused VFS wrapper does not compile with it.)
 *
 * Size: config fs.flash.size (bytes, optional K/M suffix,
 * rounded down to 4096, clamped to 64 KB .. the partition; unset = the
 * whole partition). Mount reads the size from the superblock (block_count
 * 0); a larger setting grows the file system in place (lfs_fs_grow, data
 * kept); a smaller one keeps the current size and says so (shrinking
 * needs a format). fs.flash.format = yes formats once at the set size and
 * is erased first (if that fails, no format: it would repeat every boot).
 * A blank or unreadable file system (LFS_ERR_CORRUPT / LFS_ERR_INVAL: first
 * boot) is formatted; any other mount error (IO), or a file system bigger
 * than the partition, leaves /flash unmounted and its data untouched.
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
 * Contract: see the VfsBackend comment in runtime/vfs/vfs.h. The VM task
 * calls in through the backend, and the console task through the push
 * functions (`gogo push`), so every LittleFS call holds `lock`. */
#include "vfs_int.h"
#include "platform_esp.h"
#include "lfs.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_partition.h"
#include "esp_attr.h"
#include "esp_rom_crc.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define BLOCK      4096u
#define CACHE      64u
#define LOOKAHEAD  16u
#define MIN_BYTES  (64u * 1024u)
#define STATE_MAX  3072u

/* An open file: what the vfs slot holds. The config must outlive the file
 * (LittleFS keeps a pointer to it), so it lives beside it. */
typedef struct {
    lfs_file_t f;
    struct lfs_file_config fc;
    uint8_t cache[CACHE];
} Lf;
/* GOGO_FS_SLOT_BYTES (main/CMakeLists.txt) is sizeof(Lf) exactly, so the
 * owner is charged what a file costs; fix it there when this changes. */
typedef char lf_fits_slot[(sizeof(Lf) <= GOGO_FS_SLOT_BYTES && GOGO_FS_SLOT_BYTES - sizeof(Lf) < 4u) ? 1 : -1];

static struct {
    lfs_t lfs;
    struct lfs_config cfg;
    const esp_partition_t *part;
    uint32_t read_buf[CACHE / 4u], prog_buf[CACHE / 4u], look_buf[LOOKAHEAD / 4u];
} L;

/* The storage fixed state (vfs tables + this file's statics) fits 3 KB. */
#define FS_STATE_BYTES (VFS_STATE_BYTES + sizeof L)
typedef char fs_state_fits[FS_STATE_BYTES <= STATE_MAX ? 1 : -1];
typedef char lfs_state_bounded[sizeof L <= FS_LFS_STATE_MAX ? 1 : -1];   /* fs_fat.c counts on it */

uint32_t fs_lfs_state_bytes(void) { return (uint32_t)FS_STATE_BYTES; }

/* One LittleFS user at a time: the VM task (backend) or the console task
 * (push). Created by fs_lfs_mount before anything can use it. */
static StaticSemaphore_t lock_buf;
static SemaphoreHandle_t lock;
static volatile bool mounted;
#define LOCKED(expr) __extension__({ xSemaphoreTake(lock, portMAX_DELAY); __typeof__(expr) r_ = (expr); xSemaphoreGive(lock); r_; })

/* lfs.c's malloc / free (renamed there, see the top comment): never
 * called with our buffers, and never reach the heap. */
void *gogo_lfs_malloc(size_t n);
void gogo_lfs_free(void *p);
void *gogo_lfs_malloc(size_t n) { (void)n; return NULL; }
void gogo_lfs_free(void *p) { (void)p; }

/* ---- block device: the partition ---- */

static int bd_read(const struct lfs_config *c, lfs_block_t b, lfs_off_t off, void *buf, lfs_size_t n) {
    (void)c;
    return esp_partition_read(L.part, (size_t)b * BLOCK + off, buf, n) == ESP_OK ? 0 : LFS_ERR_IO;
}
static int bd_prog(const struct lfs_config *c, lfs_block_t b, lfs_off_t off, const void *buf, lfs_size_t n) {
    (void)c;
    return esp_partition_write(L.part, (size_t)b * BLOCK + off, buf, n) == ESP_OK ? 0 : LFS_ERR_IO;
}
static int bd_erase(const struct lfs_config *c, lfs_block_t b) {
    (void)c;
    return esp_partition_erase_range(L.part, (size_t)b * BLOCK, BLOCK) == ESP_OK ? 0 : LFS_ERR_IO;
}
static int bd_sync(const struct lfs_config *c) { (void)c; return 0; }

/* ---- backend ---- */

static int map_err(int e) {
    if (e >= 0) return GOGO_E_OK;
    switch (e) {
    case LFS_ERR_NOENT: return GOGO_E_NOTFOUND;
    case LFS_ERR_EXIST: return GOGO_E_EXISTS;
    case LFS_ERR_NOSPC: return GOGO_E_NOSPACE;
    case LFS_ERR_ISDIR: return GOGO_E_ISDIR;
    case LFS_ERR_NOTDIR: return GOGO_E_NOTDIR;
    case LFS_ERR_NOTEMPTY: return GOGO_E_NOTEMPTY;
    default: return GOGO_E_IO;
    }
}

static int32_t clamp32(uint32_t v) { return v > INT32_MAX ? INT32_MAX : (int32_t)v; }

/* The mounted file system's size in blocks (0 if unknown). */
static uint32_t cur_blocks(void) {
    struct lfs_fsinfo fi;
    return lfs_fs_stat(&L.lfs, &fi) == 0 ? (uint32_t)fi.block_count : 0u;
}

static int u_open(void *c, void *f, const char *rel, int mode) {
    (void)c;
    Lf *h = f;
    int fl = mode == VFS_READ ? LFS_O_RDONLY
           : mode == VFS_WRITE ? (LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC)
           : (LFS_O_WRONLY | LFS_O_CREAT | LFS_O_APPEND);
    memset(h, 0, sizeof *h);
    h->fc.buffer = h->cache;
    return map_err(lfs_file_opencfg(&L.lfs, &h->f, rel, fl, &h->fc));
}
static int u_read(void *c, void *f, uint8_t *buf, uint32_t n, uint32_t *got) {
    (void)c;
    lfs_ssize_t k = lfs_file_read(&L.lfs, &((Lf *)f)->f, buf, n);
    if (k < 0) return map_err((int)k);
    *got = (uint32_t)k;
    return GOGO_E_OK;
}
static int u_write(void *c, void *f, const uint8_t *buf, uint32_t n) {
    (void)c;
    lfs_ssize_t k = lfs_file_write(&L.lfs, &((Lf *)f)->f, buf, n);
    if (k < 0) return map_err((int)k);
    return (uint32_t)k == n ? GOGO_E_OK : GOGO_E_NOSPACE;
}
/* Overwrites inside the file (vfs checked the bounds), then goes back to
 * the end so the next write appends. */
static int u_write_at(void *c, void *f, uint32_t off, const uint8_t *buf, uint32_t n) {
    (void)c;
    lfs_file_t *lf = &((Lf *)f)->f;
    lfs_soff_t end = lfs_file_seek(&L.lfs, lf, 0, LFS_SEEK_CUR);
    if (end < 0) return map_err((int)end);
    lfs_soff_t at = lfs_file_seek(&L.lfs, lf, (lfs_soff_t)off, LFS_SEEK_SET);
    if (at < 0) return map_err((int)at);
    lfs_ssize_t k = lfs_file_write(&L.lfs, lf, buf, n);
    int e = k < 0 ? map_err((int)k) : (uint32_t)k == n ? GOGO_E_OK : GOGO_E_NOSPACE;
    lfs_soff_t back = lfs_file_seek(&L.lfs, lf, end, LFS_SEEK_SET);
    return e ? e : back < 0 ? map_err((int)back) : GOGO_E_OK;
}
static int u_sync(void *c, void *f) { (void)c; return map_err(lfs_file_sync(&L.lfs, &((Lf *)f)->f)); }
static int u_size(void *c, void *f, int32_t *out) {
    (void)c;
    lfs_soff_t s = lfs_file_size(&L.lfs, &((Lf *)f)->f);
    if (s < 0) return map_err((int)s);
    *out = clamp32((uint32_t)s);
    return GOGO_E_OK;
}
static int u_close(void *c, void *f) { (void)c; return map_err(lfs_file_close(&L.lfs, &((Lf *)f)->f)); }

static int u_stat(void *c, const char *rel, int32_t *size, bool *is_dir) {
    (void)c;
    struct lfs_info in;
    int e = lfs_stat(&L.lfs, rel, &in);
    if (e < 0) return map_err(e);
    *is_dir = in.type == LFS_TYPE_DIR;
    *size = *is_dir ? 0 : clamp32(in.size);
    return GOGO_E_OK;
}
static int u_readdir(void *c, const char *rel, VfsDirCb cb, void *ctx) {
    (void)c;
    lfs_dir_t d;
    struct lfs_info in;
    int e = lfs_dir_open(&L.lfs, &d, rel);
    if (e < 0) return map_err(e);
    while ((e = lfs_dir_read(&L.lfs, &d, &in)) > 0) {
        if (!strcmp(in.name, ".") || !strcmp(in.name, "..")) continue;
        if (cb(ctx, in.name, in.type == LFS_TYPE_DIR) != 0) { e = 0; break; }
    }
    lfs_dir_close(&L.lfs, &d);
    return map_err(e);
}
static int u_mkdir(void *c, const char *rel) { (void)c; return map_err(lfs_mkdir(&L.lfs, rel)); }
static int u_remove(void *c, const char *rel) { (void)c; return map_err(lfs_remove(&L.lfs, rel)); }
static int u_rename(void *c, const char *from, const char *to, bool replace) {
    (void)c;
    struct lfs_info in;
    if (!replace) {
        int e = lfs_stat(&L.lfs, to, &in);
        if (e == 0) return GOGO_E_EXISTS;
        if (e != LFS_ERR_NOENT) return map_err(e);
    }
    return map_err(lfs_rename(&L.lfs, from, to));   /* replaces atomically */
}
static int u_usage(void *c, int32_t *used, int32_t *total) {
    (void)c;
    lfs_ssize_t n = lfs_fs_size(&L.lfs);
    if (n < 0) return map_err((int)n);
    *used = clamp32((uint32_t)n * BLOCK);
    *total = clamp32(cur_blocks() * BLOCK);
    return GOGO_E_OK;
}

/* The backend: each call under the lock. */
static int l_open(void *c, void *f, const char *rel, int mode) { return LOCKED(u_open(c, f, rel, mode)); }
static int l_read(void *c, void *f, uint8_t *buf, uint32_t n, uint32_t *got) { return LOCKED(u_read(c, f, buf, n, got)); }
static int l_write(void *c, void *f, const uint8_t *buf, uint32_t n) { return LOCKED(u_write(c, f, buf, n)); }
static int l_write_at(void *c, void *f, uint32_t off, const uint8_t *buf, uint32_t n) { return LOCKED(u_write_at(c, f, off, buf, n)); }
static int l_sync(void *c, void *f) { return LOCKED(u_sync(c, f)); }
static int l_size(void *c, void *f, int32_t *out) { return LOCKED(u_size(c, f, out)); }
static int l_close(void *c, void *f) { return LOCKED(u_close(c, f)); }
static int l_stat(void *c, const char *rel, int32_t *size, bool *is_dir) { return LOCKED(u_stat(c, rel, size, is_dir)); }
static int l_readdir(void *c, const char *rel, VfsDirCb cb, void *ctx) { return LOCKED(u_readdir(c, rel, cb, ctx)); }
static int l_mkdir(void *c, const char *rel) { return LOCKED(u_mkdir(c, rel)); }
static int l_remove(void *c, const char *rel) { return LOCKED(u_remove(c, rel)); }
static int l_rename(void *c, const char *from, const char *to, bool replace) { return LOCKED(u_rename(c, from, to, replace)); }
static int l_usage(void *c, int32_t *used, int32_t *total) { return LOCKED(u_usage(c, used, total)); }

static const VfsBackend backend = {
    false, l_open, l_read, l_write, l_sync, l_size, l_close,
    l_stat, l_readdir, l_mkdir, l_remove, l_rename, l_usage, l_write_at
};

/* ---- mount ---- */

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

/* Formats at `blocks`, then mounts (size read back from the superblock). */
static int format_mount(uint32_t blocks) {
    L.cfg.block_count = blocks;
    int e = lfs_format(&L.lfs, &L.cfg);
    L.cfg.block_count = 0;
    return e < 0 ? e : lfs_mount(&L.lfs, &L.cfg);
}

int fs_lfs_mount(VfsState *vs) {
    lock = xSemaphoreCreateMutexStatic(&lock_buf);
    bool flagged = format_flagged();
    L.part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_LITTLEFS, "gogofs");
    if (!L.part) {
        platform_esp_say("gogo: /flash unavailable (no gogofs partition; flash the partition table)");
        return -1;
    }
    uint32_t part_blocks = (uint32_t)(L.part->size / BLOCK);
    uint32_t want = wanted_blocks(part_blocks);

    L.cfg.read = bd_read;
    L.cfg.prog = bd_prog;
    L.cfg.erase = bd_erase;
    L.cfg.sync = bd_sync;
    L.cfg.read_size = 16;
    L.cfg.prog_size = 16;
    L.cfg.block_size = BLOCK;
    L.cfg.block_count = 0;            /* mount: from the superblock */
    L.cfg.block_cycles = 500;
    L.cfg.cache_size = CACHE;
    L.cfg.lookahead_size = LOOKAHEAD;
    L.cfg.read_buffer = L.read_buf;
    L.cfg.prog_buffer = L.prog_buf;
    L.cfg.lookahead_buffer = L.look_buf;

    int e;
    uint32_t cur = 0;
    if (flagged) {
        platform_esp_say("gogo: formatting /flash (gogo device format-flash)");
        e = format_mount(want);
    } else if (format_requested()) {
        /* Clear the request first: if that fails, formatting now would
         * wipe /flash again at every boot, so do not format at all. */
        if (config_esp_erase("fs.flash.format") != 0) {
            platform_esp_say("gogo: could not clear fs.flash.format; /flash not formatted (gogo config rm fs.flash.format)");
            e = lfs_mount(&L.lfs, &L.cfg);
        } else {
            platform_esp_say("gogo: formatting /flash (fs.flash.format=yes)");
            e = format_mount(want);
        }
    } else if ((e = lfs_mount(&L.lfs, &L.cfg)) == LFS_ERR_CORRUPT || e == LFS_ERR_INVAL) {
        /* blank (first boot) or not a LittleFS we can read: format. Any
         * other error (IO) leaves /flash unmounted, its data untouched. */
        platform_esp_say("gogo: formatting /flash");
        e = format_mount(want);
    } else if (e == 0 && (cur = cur_blocks()) > part_blocks) {
        lfs_unmount(&L.lfs);
        platform_esp_say("gogo: /flash (%lu KB) is bigger than its partition; not mounted (gogo device format-flash formats)",
                         (unsigned long)cur * 4u);
        return -1;
    } else if (e == 0 && want > cur) {
        if ((e = lfs_fs_grow(&L.lfs, want)) < 0) {
            /* the superblock was not rewritten: remount at the old size */
            platform_esp_say("gogo: /flash grow failed (%d), keeping %lu KB", e, (unsigned long)cur * 4u);
            lfs_unmount(&L.lfs);
            e = lfs_mount(&L.lfs, &L.cfg);
        }
    } else if (e == 0 && want < cur) {
        platform_esp_say("gogo: /flash shrink needs format (gogo device format-flash)");
    }
    if (e < 0) {
        platform_esp_say("gogo: /flash mount failed (%d); not mounted", e);
        return -1;
    }
    if (!vfs_mount(vs, "/flash", &backend, NULL)) return -1;
    mounted = true;
    platform_esp_log("gogo: /flash %lu KB (partition %lu KB)", (unsigned long)cur_blocks() * 4u,
                     (unsigned long)part_blocks * 4u);
    return 0;
}

/* ---- gogo push (config_esp.c), on the console task ---- */

/* The file being pushed: written under a temporary name in the target's
 * directory, then renamed over the target (atomic in LittleFS). */
static struct {
    bool open;
    Lf f;
    char tmp[VFS_PATH_MAX + 16u];
    uint32_t len, crc;
} P;

#define PUSH_TMP ".gogo-push"

/* rel's directory + PUSH_TMP into P.tmp. */
static void push_tmp_name(const char *rel) {
    const char *slash = strrchr(rel, '/');
    size_t d = slash ? (size_t)(slash - rel) + 1u : 0u;
    memcpy(P.tmp, rel, d);
    memcpy(P.tmp + d, PUSH_TMP, sizeof PUSH_TMP);
}

static int push_drop(void) {
    if (!P.open) return GOGO_E_OK;
    P.open = false;
    lfs_file_close(&L.lfs, &P.f.f);
    return map_err(lfs_remove(&L.lfs, P.tmp));
}

int fs_lfs_push_begin(const char *rel) {
    if (!mounted) return GOGO_E_NOTMOUNTED;
    xSemaphoreTake(lock, portMAX_DELAY);
    push_drop();
    push_tmp_name(rel);
    memset(&P.f, 0, sizeof P.f);
    P.f.fc.buffer = P.f.cache;
    int e = map_err(lfs_file_opencfg(&L.lfs, &P.f.f, P.tmp, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC, &P.f.fc));
    P.open = e == GOGO_E_OK;
    P.len = P.crc = 0;
    xSemaphoreGive(lock);
    return e;
}

int fs_lfs_push_write(const uint8_t *buf, uint32_t n, uint32_t *total) {
    if (!P.open) return GOGO_E_BADARG;
    xSemaphoreTake(lock, portMAX_DELAY);
    lfs_ssize_t k = lfs_file_write(&L.lfs, &P.f.f, buf, n);
    int e = k < 0 ? map_err((int)k) : (uint32_t)k == n ? GOGO_E_OK : GOGO_E_NOSPACE;
    if (e == GOGO_E_OK) {
        P.crc = esp_rom_crc32_le(P.crc, buf, n);
        P.len += n;
    } else {
        push_drop();
    }
    *total = P.len;
    xSemaphoreGive(lock);
    return e;
}

/* Closes the pushed file, reads it back from flash, and renames it over
 * rel if it is len bytes with CRC-32 crc; otherwise removes it and
 * returns GOGO_E_IO. */
int fs_lfs_push_end(const char *rel, uint32_t len, uint32_t crc) {
    if (!P.open) return GOGO_E_BADARG;
    xSemaphoreTake(lock, portMAX_DELAY);
    P.open = false;
    int e = map_err(lfs_file_close(&L.lfs, &P.f.f));
    if (e == GOGO_E_OK && (P.len != len || P.crc != crc)) e = GOGO_E_IO;   /* not what was sent */
    if (e == GOGO_E_OK) {
        memset(&P.f, 0, sizeof P.f);
        P.f.fc.buffer = P.f.cache;
        e = map_err(lfs_file_opencfg(&L.lfs, &P.f.f, P.tmp, LFS_O_RDONLY, &P.f.fc));
        if (e == GOGO_E_OK) {
            uint8_t b[64];
            uint32_t got = 0, c = 0;
            lfs_ssize_t k;
            while ((k = lfs_file_read(&L.lfs, &P.f.f, b, sizeof b)) > 0) {
                c = esp_rom_crc32_le(c, b, (uint32_t)k);
                got += (uint32_t)k;
            }
            lfs_file_close(&L.lfs, &P.f.f);
            if (k < 0) e = map_err((int)k);
            else if (got != len || c != crc) e = GOGO_E_IO;              /* not what was stored */
        }
    }
    if (e == GOGO_E_OK) e = map_err(lfs_rename(&L.lfs, P.tmp, rel));
    if (e != GOGO_E_OK) lfs_remove(&L.lfs, P.tmp);
    xSemaphoreGive(lock);
    return e;
}

void fs_lfs_push_abort(void) {
    if (!mounted) return;
    xSemaphoreTake(lock, portMAX_DELAY);
    push_drop();
    xSemaphoreGive(lock);
}
