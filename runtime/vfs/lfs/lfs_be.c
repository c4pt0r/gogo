/* LittleFS as a vfs backend over a BlockDev (lfs_be.h). Contract: see the
 * VfsBackend comment in runtime/vfs/vfs.h. */
#include "lfs_be.h"
#include "vfs_int.h"

#include <stddef.h>
#include <string.h>

#define CACHE     LFS_BE_CACHE
#define LOOKAHEAD LFS_BE_LOOKAHEAD

/* An open file: what the vfs slot holds. The config must outlive the file
 * (LittleFS keeps a pointer to it), so it lives beside it. */
typedef struct {
    lfs_file_t f;
    struct lfs_file_config fc;
    uint8_t cache[CACHE];
} Lf;
/* GOGO_FS_SLOT_BYTES (the board's build) is sizeof(Lf) exactly, so the
 * owner is charged what a file costs; fix it there when this changes. */
typedef char lf_fits_slot[(sizeof(Lf) <= GOGO_FS_SLOT_BYTES && GOGO_FS_SLOT_BYTES - sizeof(Lf) < 4u) ? 1 : -1];

/* lfs.c's malloc / free (renamed there by the build, see lfs_be.h): never
 * called with our buffers, and never reach the heap. */
void *gogo_lfs_malloc(size_t n);
void gogo_lfs_free(void *p);
void *gogo_lfs_malloc(size_t n) { (void)n; return NULL; }
void gogo_lfs_free(void *p) { (void)p; }

/* ---- the BlockDev under LittleFS ---- */

static const BlockDev *bd_of(const struct lfs_config *c) { return ((const LfsVol *)c->context)->bd; }

static int bd_read(const struct lfs_config *c, lfs_block_t b, lfs_off_t off, void *buf, lfs_size_t n) {
    const BlockDev *d = bd_of(c);
    return d->read(d->ctx, (uint32_t)b, (uint32_t)off, buf, (uint32_t)n) == 0 ? 0 : LFS_ERR_IO;
}
static int bd_prog(const struct lfs_config *c, lfs_block_t b, lfs_off_t off, const void *buf, lfs_size_t n) {
    const BlockDev *d = bd_of(c);
    return d->prog(d->ctx, (uint32_t)b, (uint32_t)off, buf, (uint32_t)n) == 0 ? 0 : LFS_ERR_IO;
}
static int bd_erase(const struct lfs_config *c, lfs_block_t b) {
    const BlockDev *d = bd_of(c);
    return d->erase(d->ctx, (uint32_t)b) == 0 ? 0 : LFS_ERR_IO;
}
static int bd_sync(const struct lfs_config *c) {
    const BlockDev *d = bd_of(c);
    return !d->sync || d->sync(d->ctx) == 0 ? 0 : LFS_ERR_IO;
}

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

uint32_t lfs_be_blocks(LfsVol *v) {
    struct lfs_fsinfo fi;
    return v->mounted && lfs_fs_stat(&v->lfs, &fi) == 0 ? (uint32_t)fi.block_count : 0u;
}

/* The mounted file system's size in blocks, while mounting (0 if unknown). */
static uint32_t cur_blocks(LfsVol *v) {
    struct lfs_fsinfo fi;
    return lfs_fs_stat(&v->lfs, &fi) == 0 ? (uint32_t)fi.block_count : 0u;
}

/* The volume of ctx, or NULL when it is not mounted. */
static LfsVol *vol(void *c) {
    LfsVol *v = c;
    return v->mounted ? v : NULL;
}

static int u_open(void *c, void *f, const char *rel, int mode) {
    LfsVol *v = vol(c);
    if (!v) return GOGO_E_NOTMOUNTED;
    Lf *h = f;
    int fl = mode == VFS_READ ? LFS_O_RDONLY
           : mode == VFS_WRITE ? (LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC)
           : (LFS_O_WRONLY | LFS_O_CREAT | LFS_O_APPEND);
    memset(h, 0, sizeof *h);
    h->fc.buffer = h->cache;
    return map_err(lfs_file_opencfg(&v->lfs, &h->f, rel, fl, &h->fc));
}
/* A file can only be open on a mounted volume: the vfs slot exists. */
static int u_read(void *c, void *f, uint8_t *buf, uint32_t n, uint32_t *got) {
    LfsVol *v = c;
    lfs_ssize_t k = lfs_file_read(&v->lfs, &((Lf *)f)->f, buf, n);
    if (k < 0) return map_err((int)k);
    *got = (uint32_t)k;
    return GOGO_E_OK;
}
static int u_write(void *c, void *f, const uint8_t *buf, uint32_t n) {
    LfsVol *v = c;
    lfs_ssize_t k = lfs_file_write(&v->lfs, &((Lf *)f)->f, buf, n);
    if (k < 0) return map_err((int)k);
    return (uint32_t)k == n ? GOGO_E_OK : GOGO_E_NOSPACE;
}
/* Overwrites inside the file (vfs checked the bounds), then goes back to
 * the end so the next write appends. */
static int u_write_at(void *c, void *f, uint32_t off, const uint8_t *buf, uint32_t n) {
    LfsVol *v = c;
    lfs_file_t *lf = &((Lf *)f)->f;
    lfs_soff_t end = lfs_file_seek(&v->lfs, lf, 0, LFS_SEEK_CUR);
    if (end < 0) return map_err((int)end);
    lfs_soff_t at = lfs_file_seek(&v->lfs, lf, (lfs_soff_t)off, LFS_SEEK_SET);
    if (at < 0) return map_err((int)at);
    lfs_ssize_t k = lfs_file_write(&v->lfs, lf, buf, n);
    int e = k < 0 ? map_err((int)k) : (uint32_t)k == n ? GOGO_E_OK : GOGO_E_NOSPACE;
    lfs_soff_t back = lfs_file_seek(&v->lfs, lf, end, LFS_SEEK_SET);
    return e ? e : back < 0 ? map_err((int)back) : GOGO_E_OK;
}
static int u_sync(void *c, void *f) { LfsVol *v = c; return map_err(lfs_file_sync(&v->lfs, &((Lf *)f)->f)); }
static int u_size(void *c, void *f, int32_t *out) {
    LfsVol *v = c;
    lfs_soff_t s = lfs_file_size(&v->lfs, &((Lf *)f)->f);
    if (s < 0) return map_err((int)s);
    *out = clamp32((uint32_t)s);
    return GOGO_E_OK;
}
static int u_close(void *c, void *f) { LfsVol *v = c; return map_err(lfs_file_close(&v->lfs, &((Lf *)f)->f)); }

static int u_stat(void *c, const char *rel, int32_t *size, bool *is_dir) {
    LfsVol *v = vol(c);
    if (!v) return GOGO_E_NOTMOUNTED;
    struct lfs_info in;
    int e = lfs_stat(&v->lfs, rel, &in);
    if (e < 0) return map_err(e);
    *is_dir = in.type == LFS_TYPE_DIR;
    *size = *is_dir ? 0 : clamp32(in.size);
    return GOGO_E_OK;
}
static int u_readdir(void *c, const char *rel, VfsDirCb cb, void *ctx) {
    LfsVol *v = vol(c);
    if (!v) return GOGO_E_NOTMOUNTED;
    lfs_dir_t d;
    struct lfs_info in;
    int e = lfs_dir_open(&v->lfs, &d, rel);
    if (e < 0) return map_err(e);
    while ((e = lfs_dir_read(&v->lfs, &d, &in)) > 0) {
        if (!strcmp(in.name, ".") || !strcmp(in.name, "..")) continue;
        if (cb(ctx, in.name, in.type == LFS_TYPE_DIR) != 0) { e = 0; break; }
    }
    lfs_dir_close(&v->lfs, &d);
    return map_err(e);
}
static int u_mkdir(void *c, const char *rel) {
    LfsVol *v = vol(c);
    return v ? map_err(lfs_mkdir(&v->lfs, rel)) : GOGO_E_NOTMOUNTED;
}
static int u_remove(void *c, const char *rel) {
    LfsVol *v = vol(c);
    return v ? map_err(lfs_remove(&v->lfs, rel)) : GOGO_E_NOTMOUNTED;
}
static int u_rename(void *c, const char *from, const char *to, bool replace) {
    LfsVol *v = vol(c);
    if (!v) return GOGO_E_NOTMOUNTED;
    struct lfs_info in;
    if (!replace) {
        int e = lfs_stat(&v->lfs, to, &in);
        if (e == 0) return GOGO_E_EXISTS;
        if (e != LFS_ERR_NOENT) return map_err(e);
    }
    return map_err(lfs_rename(&v->lfs, from, to));   /* replaces atomically */
}
static int u_usage(void *c, int32_t *used, int32_t *total) {
    LfsVol *v = vol(c);
    if (!v) return GOGO_E_NOTMOUNTED;
    lfs_ssize_t n = lfs_fs_size(&v->lfs);
    if (n < 0) return map_err((int)n);
    uint32_t bs = v->cfg.block_size;
    *used = clamp32((uint32_t)n * bs);
    *total = clamp32(cur_blocks(v) * bs);
    return GOGO_E_OK;
}

/* The backend: each call under the volume's lock (LfsOpts.lock). */
static void take(void *c) { const LfsOpts *o = ((LfsVol *)c)->opts; if (o && o->lock) o->lock(true); }
static int give(void *c, int r) { const LfsOpts *o = ((LfsVol *)c)->opts; if (o && o->lock) o->lock(false); return r; }
static int l_open(void *c, void *f, const char *rel, int mode) { take(c); return give(c, u_open(c, f, rel, mode)); }
static int l_read(void *c, void *f, uint8_t *buf, uint32_t n, uint32_t *got) { take(c); return give(c, u_read(c, f, buf, n, got)); }
static int l_write(void *c, void *f, const uint8_t *buf, uint32_t n) { take(c); return give(c, u_write(c, f, buf, n)); }
static int l_write_at(void *c, void *f, uint32_t off, const uint8_t *buf, uint32_t n) { take(c); return give(c, u_write_at(c, f, off, buf, n)); }
static int l_sync(void *c, void *f) { take(c); return give(c, u_sync(c, f)); }
static int l_size(void *c, void *f, int32_t *out) { take(c); return give(c, u_size(c, f, out)); }
static int l_close(void *c, void *f) { take(c); return give(c, u_close(c, f)); }
static int l_stat(void *c, const char *rel, int32_t *size, bool *is_dir) { take(c); return give(c, u_stat(c, rel, size, is_dir)); }
static int l_readdir(void *c, const char *rel, VfsDirCb cb, void *ctx) { take(c); return give(c, u_readdir(c, rel, cb, ctx)); }
static int l_mkdir(void *c, const char *rel) { take(c); return give(c, u_mkdir(c, rel)); }
static int l_remove(void *c, const char *rel) { take(c); return give(c, u_remove(c, rel)); }
static int l_rename(void *c, const char *from, const char *to, bool replace) { take(c); return give(c, u_rename(c, from, to, replace)); }
static int l_usage(void *c, int32_t *used, int32_t *total) { take(c); return give(c, u_usage(c, used, total)); }

const VfsBackend lfs_be_backend = {
    false, l_open, l_read, l_write, l_sync, l_size, l_close,
    l_stat, l_readdir, l_mkdir, l_remove, l_rename, l_usage, l_write_at
};

/* ---- mount ---- */

/* Formats at `blocks`, then mounts (size read back from the superblock). */
static int format_mount(LfsVol *v, uint32_t blocks) {
    v->cfg.block_count = blocks;
    int e = lfs_format(&v->lfs, &v->cfg);
    v->cfg.block_count = 0;
    return e < 0 ? e : lfs_mount(&v->lfs, &v->cfg);
}

int lfs_be_mount(LfsVol *v, const BlockDev *bd, const LfsOpts *o, uint32_t want, int mode) {
    const char *name = o->name, *hint = o->format_hint;
    void (*say)(const char *fmt, ...) = o->say;
    v->mounted = false;
    v->bd = bd;
    v->opts = o;
    uint32_t dev_blocks = bd->block_count, kb = bd->block_size / 1024u;
    if (want == 0 || want > dev_blocks) want = dev_blocks;

    memset(&v->cfg, 0, sizeof v->cfg);
    v->cfg.context = v;
    v->cfg.read = bd_read;
    v->cfg.prog = bd_prog;
    v->cfg.erase = bd_erase;
    v->cfg.sync = bd_sync;
    v->cfg.read_size = 16;
    v->cfg.prog_size = 16;
    v->cfg.block_size = bd->block_size;
    v->cfg.block_count = 0;            /* mount: from the superblock */
    v->cfg.block_cycles = 500;
    v->cfg.cache_size = CACHE;
    v->cfg.lookahead_size = LOOKAHEAD;
    v->cfg.read_buffer = v->read_buf;
    v->cfg.prog_buffer = v->prog_buf;
    v->cfg.lookahead_buffer = v->look_buf;

    int e;
    uint32_t cur = 0;
    if (mode == LFS_BE_FORMAT) {
        e = format_mount(v, want);
    } else if (mode == LFS_BE_MOUNT_ONLY) {
        e = lfs_mount(&v->lfs, &v->cfg);
    } else if ((e = lfs_mount(&v->lfs, &v->cfg)) == LFS_ERR_CORRUPT || e == LFS_ERR_INVAL) {
        /* blank (first boot) or not a LittleFS we can read: format. Any
         * other error (IO) leaves the volume unmounted, its data untouched. */
        if (say) say("gogo: formatting %s", name);
        e = format_mount(v, want);
    } else if (e == 0 && (cur = cur_blocks(v)) > dev_blocks) {
        lfs_unmount(&v->lfs);
        if (say) say("gogo: %s (%lu KB) is bigger than its partition; not mounted (%s formats)", name,
                     (unsigned long)cur * kb, hint);
        return LFS_ERR_INVAL;
    } else if (e == 0 && want > cur) {
        if ((e = lfs_fs_grow(&v->lfs, want)) < 0) {
            /* the superblock was not rewritten: remount at the old size */
            if (say) say("gogo: %s grow failed (%d), keeping %lu KB", name, e, (unsigned long)cur * kb);
            lfs_unmount(&v->lfs);
            e = lfs_mount(&v->lfs, &v->cfg);
        }
    } else if (e == 0 && want < cur) {
        if (say) say("gogo: %s shrink needs format (%s)", name, hint);
    }
    if (e < 0) {
        if (say) say("gogo: %s mount failed (%d); not mounted", name, e);
        return e;
    }
    v->mounted = true;
    return 0;
}
