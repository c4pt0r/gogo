/* FatFs as a vfs backend over a DiskHal (fat_be.h). Contract: see the
 * VfsBackend comment in runtime/vfs/vfs.h. */
#include "fat_be.h"
#include "vfs_int.h"

#include <stdio.h>
#include <string.h>

#if FF_MAX_SS != 512 || FF_USE_DYN_BUFFER || !FF_FS_TINY || FF_USE_LFN != 2 || FF_MAX_LFN != 64 || FF_FS_LOCK
#error "FatFs config: 512-byte sectors, static window, tiny, LFN on stack 64, no lock (fat_be.h)"
#endif

#define SECTOR FAT_BE_SECTOR

/* An open file: the FIL itself, in the vfs slot. */
typedef char fil_fits_slot[sizeof(FIL) <= GOGO_FS_SLOT_BYTES ? 1 : -1];

void fat_be_init(FatVol *v, const DiskHal *hal, const char *name, void (*say)(const char *fmt, ...)) {
    memset(v, 0, sizeof *v);
    v->hal = hal;
    v->name = name;
    v->say = say;
}

/* ---- FatFs's disk layer ---- */

DSTATUS fat_be_disk_status(FatVol *v) { (void)v; return 0; }

DRESULT fat_be_disk_read(FatVol *v, BYTE *buf, LBA_t sector, UINT n) {
    const DiskHal *h = v->hal;
    return h->read(h->ctx, (uint32_t)sector, buf, (uint32_t)n) == 0 ? RES_OK : RES_ERROR;
}

DRESULT fat_be_disk_write(FatVol *v, const BYTE *buf, LBA_t sector, UINT n) {
    const DiskHal *h = v->hal;
    return h->write(h->ctx, (uint32_t)sector, buf, (uint32_t)n) == 0 ? RES_OK : RES_ERROR;
}

DRESULT fat_be_disk_ioctl(FatVol *v, BYTE cmd, void *buf) {
    const DiskHal *h = v->hal;
    switch (cmd) {
    case CTRL_SYNC: return RES_OK;                 /* writes are synchronous */
    case GET_SECTOR_COUNT: *(LBA_t *)buf = (LBA_t)h->sectors(h->ctx); return RES_OK;
    case GET_SECTOR_SIZE: *(WORD *)buf = SECTOR; return RES_OK;
    case GET_BLOCK_SIZE: *(DWORD *)buf = 1; return RES_OK;
    case CTRL_TRIM: return RES_OK;                 /* optional; skipped */
    default: return RES_PARERR;
    }
}

/* ---- mount ---- */

/* Says once what the medium holds instead of a FAT: MBR partition 1 type
 * (0x07 = NTFS here, 0xEE = GPT) or, with no MBR, the boot sector's OEM
 * name. Sector 0 is read into FatFs's window: the volume is not mounted
 * (try_mount unregistered it), so the window is free, and nothing goes on
 * the VM task's stack. */
static void say_nofs(FatVol *v) {
    uint8_t *b = v->fs.win;
    char what[24] = "unreadable";
    const DiskHal *h = v->hal;
    v->said_nofs = true;
    if (!v->say) return;
    v->fs.winsect = (LBA_t)0 - 1;   /* the window no longer holds what FatFs read */
    if (h->read(h->ctx, 0, b, 1) == 0) {
        if (b[510] == 0x55 && b[511] == 0xAA && b[446 + 4] != 0 && memcmp(b + 3, "NTFS", 4))
            snprintf(what, sizeof what, "partition type 0x%02X", b[446 + 4]);
        else {
            memcpy(what, b + 3, 8);
            what[8] = 0;
            for (int i = 0; i < 8; i++)
                if (what[i] < ' ' || what[i] > '~') what[i] = '.';
        }
    }
    v->say("gogo: %s: no FAT or exFAT file system on the card (%s; format it FAT32 or exFAT on a computer)",
           v->name, what);
}

/* Medium up and FatFs mounted for attempt: 0 mounted, 1 failed (the next
 * attempt may help), 2 failed for good (no bus, or no FAT on a medium
 * that reads fine). */
static int try_mount(FatVol *v, int attempt) {
    const DiskHal *h = v->hal;
    int a = h->attach(h->ctx, attempt);
    if (a != 0) return a == 2 ? 2 : 1;
    FRESULT r = f_mount(&v->fs, "0:", 1);
    if (r == FR_OK) return 0;
    f_mount(NULL, "0:", 0);
    if (r == FR_NO_FILESYSTEM && !v->said_nofs) say_nofs(v);
    return r == FR_NO_FILESYSTEM ? 2 : 1;
}

static void unmount(FatVol *v) {
    if (!v->mounted) return;
    f_mount(NULL, "0:", 0);
    v->mounted = false;
}

/* A medium is there and mounted: GOGO_E_OK, else GOGO_E_NOTMOUNTED. */
static int ready(FatVol *v) {
    const DiskHal *h = v->hal;
    if (v->mounted) {
        if (h->check(h->ctx) == 0) return GOGO_E_OK;
        unmount(v);
    }
    int m = try_mount(v, 0);
    if (m == 1) m = try_mount(v, 1);
    if (m != 0) return GOGO_E_NOTMOUNTED;
    v->mounted = true;
    v->said_nofs = false;
    if (h->mounted) h->mounted(h->ctx, v->fs.fs_type == FS_EXFAT);
    return GOGO_E_OK;
}

/* ---- backend ---- */

static int map_err(FatVol *v, FRESULT r) {
    switch (r) {
    case FR_OK: return GOGO_E_OK;
    case FR_NO_FILE: case FR_NO_PATH: return GOGO_E_NOTFOUND;
    case FR_EXIST: return GOGO_E_EXISTS;
    case FR_INVALID_NAME: return GOGO_E_BADPATH;
    case FR_NOT_ENABLED: case FR_NO_FILESYSTEM: return GOGO_E_NOTMOUNTED;
    case FR_DISK_ERR: case FR_NOT_READY:
        unmount(v);                                /* the next call remounts */
        return GOGO_E_IO;
    default: return GOGO_E_IO;
    }
}

static int32_t clamp32(uint64_t x) { return x > INT32_MAX ? INT32_MAX : (int32_t)x; }

static bool is_root(const char *rel) { return rel[0] == '/' && rel[1] == 0; }

/* After FR_DENIED / FR_NO_FILE on a path that may be a directory. */
static bool is_dir(const char *rel) {
    FILINFO fi;
    return is_root(rel) || (f_stat(rel, &fi) == FR_OK && (fi.fattrib & AM_DIR));
}

static int t_open(void *c, void *f, const char *rel, int mode) {
    FatVol *v = c;
    int e = ready(v);
    if (e) return e;
    if (is_root(rel)) return GOGO_E_ISDIR;
    BYTE fl = mode == VFS_READ ? (FA_READ | FA_OPEN_EXISTING)
            : mode == VFS_WRITE ? (FA_WRITE | FA_CREATE_ALWAYS)
            : (FA_WRITE | FA_OPEN_APPEND);
    memset(f, 0, sizeof(FIL));
    FRESULT r = f_open(f, rel, fl);
    if (r == FR_NO_FILE || r == FR_DENIED) {
        if (is_dir(rel)) return GOGO_E_ISDIR;
        if (r == FR_DENIED) {
            FILINFO fi;
            /* an existing (read-only) file: IO; else the directory is full */
            return f_stat(rel, &fi) == FR_OK ? GOGO_E_IO : GOGO_E_NOSPACE;
        }
    }
    return map_err(v, r);
}
static int t_read(void *c, void *f, uint8_t *buf, uint32_t n, uint32_t *got) {
    UINT br = 0;
    int e = map_err(c, f_read(f, buf, n, &br));
    if (e) return e;
    *got = br;
    return GOGO_E_OK;
}
static int t_write(void *c, void *f, const uint8_t *buf, uint32_t n) {
    UINT bw = 0;
    int e = map_err(c, f_write(f, buf, n, &bw));
    if (e) return e;
    return bw == n ? GOGO_E_OK : GOGO_E_NOSPACE;
}
/* Overwrites inside the file (vfs checked the bounds), then goes back to
 * the end so the next write appends. FAT patches the clusters in place. */
static int t_write_at(void *c, void *f, uint32_t off, const uint8_t *buf, uint32_t n) {
    FIL *fp = f;
    FSIZE_t end = f_tell(fp);
    int e = map_err(c, f_lseek(fp, off));
    UINT bw = 0;
    if (!e) {
        e = map_err(c, f_write(fp, buf, n, &bw));
        if (!e && bw != n) e = GOGO_E_NOSPACE;
    }
    int e2 = map_err(c, f_lseek(fp, end));
    return e ? e : e2;
}
static int t_sync(void *c, void *f) { return map_err(c, f_sync(f)); }
static int t_size(void *c, void *f, int32_t *out) {
    (void)c;
    *out = clamp32(f_size((FIL *)f));
    return GOGO_E_OK;
}
static int t_close(void *c, void *f) { return map_err(c, f_close(f)); }

static int t_stat(void *c, const char *rel, int32_t *size, bool *dir) {
    FatVol *v = c;
    int e = ready(v);
    if (e) return e;
    if (is_root(rel)) { *dir = true; *size = 0; return GOGO_E_OK; }
    FILINFO fi;
    e = map_err(v, f_stat(rel, &fi));
    if (e) return e;
    *dir = (fi.fattrib & AM_DIR) != 0;
    *size = *dir ? 0 : clamp32(fi.fsize);
    return GOGO_E_OK;
}
static int t_readdir(void *c, const char *rel, VfsDirCb cb, void *ctx) {
    FatVol *v = c;
    int e = ready(v);
    if (e) return e;
    FF_DIR d;
    FILINFO fi;
    FRESULT r = f_opendir(&d, rel);
    if (r == FR_NO_PATH || r == FR_NO_FILE) {
        FILINFO x;
        if (f_stat(rel, &x) == FR_OK) return GOGO_E_NOTDIR;   /* a file */
    }
    if (r != FR_OK) return map_err(v, r);
    while ((r = f_readdir(&d, &fi)) == FR_OK && fi.fname[0]) {
        if (!strcmp(fi.fname, ".") || !strcmp(fi.fname, "..")) continue;
        if (cb(ctx, fi.fname, (fi.fattrib & AM_DIR) != 0) != 0) break;
    }
    f_closedir(&d);
    return map_err(v, r);
}
static int t_mkdir(void *c, const char *rel) {
    FatVol *v = c;
    int e = ready(v);
    if (e) return e;
    FRESULT r = f_mkdir(rel);
    return r == FR_DENIED ? GOGO_E_NOSPACE : map_err(v, r);   /* FatFs: no space / directory full */
}
static int t_remove(void *c, const char *rel) {
    FatVol *v = c;
    int e = ready(v);
    if (e) return e;
    FRESULT r = f_unlink(rel);
    if (r == FR_DENIED) return is_dir(rel) ? GOGO_E_NOTEMPTY : GOGO_E_IO;   /* else read-only */
    return map_err(v, r);
}
static int t_rename(void *c, const char *from, const char *to, bool replace) {
    FatVol *v = c;
    int e = ready(v);
    if (e) return e;
    (void)replace;               /* FAT cannot replace: Exists either way */
    return map_err(v, f_rename(from, to));
}
static int t_usage(void *c, int32_t *used, int32_t *total) {
    FatVol *v = c;
    int e = ready(v);
    if (e) return e;
    DWORD nfree;
    FATFS *fs;
    e = map_err(v, f_getfree("0:", &nfree, &fs));
    if (e) return e;
    uint64_t clust = (uint64_t)fs->csize * SECTOR, all = (uint64_t)(fs->n_fatent - 2u) * clust;
    *total = clamp32(all);
    *used = clamp32(all - (uint64_t)nfree * clust);
    return GOGO_E_OK;
}

const VfsBackend fat_be_backend = {
    true, t_open, t_read, t_write, t_sync, t_size, t_close,
    t_stat, t_readdir, t_mkdir, t_remove, t_rename, t_usage, t_write_at
};
