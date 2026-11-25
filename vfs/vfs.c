/* vfs core: mounts, slot table, the Busy / Exists / replace rules. */
#include "vfs_int.h"
#include <string.h>

typedef VfsMount Mount;
typedef VfsSlot Slot;

static Mount mounts[VFS_MAX_MOUNTS];
static uint32_t n_mounts;
static Slot slots[GOGO_FS_MAX_OPEN];

bool vfs_mount(const char *prefix, const VfsBackend *b) {
    uint32_t n = (uint32_t)strlen(prefix), first;
    if (n_mounts >= VFS_MAX_MOUNTS || vfs_path_check(prefix, n, &first) || first != n) return false;
    for (uint32_t i = 0; i < n_mounts; i++)
        if (mounts[i].plen == n && !memcmp(mounts[i].prefix, prefix, n)) return false;
    mounts[n_mounts].prefix = prefix;
    mounts[n_mounts].plen = n;
    mounts[n_mounts].be = b;
    n_mounts++;
    return true;
}

void vfs_reset(void) {
    n_mounts = 0;
    memset(mounts, 0, sizeof mounts);
    memset(slots, 0, sizeof slots);
}

/* Validate and route. rel gets the backend path ("/" for the mount root). */
static int resolve(const char *p, uint32_t len, bool root_ok, uint32_t *mi, char *rel, uint32_t *hash) {
    uint32_t first;
    int e = vfs_path_check(p, len, &first);
    if (e) return e;
    for (uint32_t i = 0; i < n_mounts; i++) {
        if (mounts[i].plen != first || memcmp(p, mounts[i].prefix, first)) continue;
        if (first == len && !root_ok) return VFS_BADPATH;
        if (first == len) { rel[0] = '/'; rel[1] = 0; }
        else { memcpy(rel, p + first, len - first); rel[len - first] = 0; }
        *mi = i;
        *hash = vfs_path_hash(p, len, mounts[i].be->case_insensitive);
        return VFS_OK;
    }
    return VFS_NOTMOUNTED;
}

static bool is_open(uint32_t hash) {
    for (uint32_t i = 0; i < GOGO_FS_MAX_OPEN; i++)
        if (slots[i].used && slots[i].hash == hash) return true;
    return false;
}

int vfs_open(const char *p, uint32_t len, int mode, int *slot) {
    char rel[VFS_PATH_MAX + 1];
    uint32_t mi, h;
    int e = resolve(p, len, true, &mi, rel, &h);
    if (e) return e;
    if (mode < VFS_READ || mode > VFS_APPEND) return VFS_BADPATH;
    int s = -1;
    for (uint32_t i = 0; i < GOGO_FS_MAX_OPEN; i++) {
        if (!slots[i].used) { if (s < 0) s = (int)i; }
        else if (mode != VFS_READ && slots[i].hash == h &&
                 (slots[i].mode != VFS_READ || (mode == VFS_WRITE && mounts[mi].be->case_insensitive)))
            return VFS_BUSY;   /* one writer; on FAT no truncate under a reader */
    }
    if (s < 0) return VFS_TOOMANY;
    e = mounts[mi].be->open(&slots[s].f, rel, mode);
    if (e) return e;
    slots[s].used = 1;
    slots[s].mode = (uint8_t)mode;
    slots[s].mnt = (uint8_t)mi;
    slots[s].hash = h;
    *slot = s;
    return VFS_OK;
}

static Slot *get(int s) {
    return (s >= 0 && s < (int)GOGO_FS_MAX_OPEN && slots[s].used) ? &slots[s] : 0;
}

int vfs_read(int slot, uint8_t *buf, uint32_t n, uint32_t *got) {
    Slot *s = get(slot);
    if (!s || s->mode != VFS_READ) return VFS_CLOSED;
    return mounts[s->mnt].be->read(&s->f, buf, n, got);
}

int vfs_write(int slot, const uint8_t *p, uint32_t n) {
    Slot *s = get(slot);
    if (!s || s->mode == VFS_READ) return VFS_CLOSED;
    return mounts[s->mnt].be->write(&s->f, p, n);
}

int vfs_write_at(int slot, uint32_t off, const uint8_t *p, uint32_t n) {
    Slot *s = get(slot);
    const VfsBackend *b;
    int32_t size;
    if (!s || s->mode != VFS_WRITE) return VFS_CLOSED;
    b = mounts[s->mnt].be;
    if (!b->write_at) return VFS_IO;
    if (n == 0) return VFS_OK;
    int e = b->size(&s->f, &size);
    if (e) return e;
    if (size < 0 || off > (uint32_t)size || n > (uint32_t)size - off) return VFS_BADPATH;
    return b->write_at(&s->f, off, p, n);
}

int vfs_sync(int slot) {
    Slot *s = get(slot);
    return s ? mounts[s->mnt].be->sync(&s->f) : VFS_CLOSED;
}

int vfs_size(int slot, int32_t *out) {
    Slot *s = get(slot);
    return s ? mounts[s->mnt].be->size(&s->f, out) : VFS_CLOSED;
}

void vfs_close(int slot) {
    Slot *s = get(slot);
    if (!s) return;
    mounts[s->mnt].be->close(&s->f);
    s->used = 0;
}

int vfs_slot_mode(int slot) {
    Slot *s = get(slot);
    return s ? s->mode : -1;
}

int vfs_stat(const char *p, uint32_t len, int32_t *size, bool *is_dir) {
    char rel[VFS_PATH_MAX + 1];
    uint32_t mi, h;
    int e = resolve(p, len, true, &mi, rel, &h);
    return e ? e : mounts[mi].be->stat(rel, size, is_dir);
}

int vfs_readdir(const char *p, uint32_t len, VfsDirCb cb, void *ctx) {
    char rel[VFS_PATH_MAX + 1];
    uint32_t mi, h;
    int e = resolve(p, len, true, &mi, rel, &h);
    return e ? e : mounts[mi].be->readdir(rel, cb, ctx);
}

int vfs_mkdir(const char *p, uint32_t len) {
    char rel[VFS_PATH_MAX + 1];
    uint32_t mi, h;
    int e = resolve(p, len, false, &mi, rel, &h);
    return e ? e : mounts[mi].be->mkdir(rel);
}

int vfs_remove(const char *p, uint32_t len) {
    char rel[VFS_PATH_MAX + 1];
    uint32_t mi, h;
    int e = resolve(p, len, false, &mi, rel, &h);
    if (e) return e;
    if (is_open(h)) return VFS_BUSY;
    return mounts[mi].be->remove(rel);
}

/* Both paths resolved on one mount, neither open. */
static int resolve2(const char *a, uint32_t alen, const char *b, uint32_t blen,
                    uint32_t *mi, char *ra, char *rb) {
    uint32_t mj, ha, hb;
    int e = resolve(a, alen, false, mi, ra, &ha);
    if (!e) e = resolve(b, blen, false, &mj, rb, &hb);
    if (e) return e;
    if (*mi != mj) return VFS_BADPATH;          /* no cross-mount rename */
    if (is_open(ha) || is_open(hb)) return VFS_BUSY;
    return VFS_OK;
}

static char fold(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

/* The first n bytes of a and b match; case folded when ci. */
static bool same(const char *a, const char *b, uint32_t n, bool ci) {
    for (uint32_t i = 0; i < n; i++)
        if (a[i] != b[i] && (!ci || fold(a[i]) != fold(b[i]))) return false;
    return true;
}

int vfs_rename(const char *a, uint32_t alen, const char *b, uint32_t blen) {
    char ra[VFS_PATH_MAX + 1], rb[VFS_PATH_MAX + 1];
    uint32_t mi;
    int32_t sz;
    bool d;
    int e = resolve2(a, alen, b, blen, &mi, ra, rb);
    if (e) return e;
    const VfsBackend *be = mounts[mi].be;
    bool ci = be->case_insensitive;
    if (blen > alen && b[alen] == '/' && same(a, b, alen, ci)) return VFS_BADPATH;   /* into its own subtree */
    /* A directory with any file open on this mount is Busy: slots keep only
     * the hash of their path, so a renamed ancestor would escape the rules.
     * A missing source falls through to the backend's NotFound. */
    e = be->stat(ra, &sz, &d);
    if (e == VFS_OK && d) {
        for (uint32_t i = 0; i < GOGO_FS_MAX_OPEN; i++)
            if (slots[i].used && slots[i].mnt == mi) return VFS_BUSY;
    } else if (e && e != VFS_NOTFOUND) return e;
    /* A case-only rename on a case-insensitive mount: b names a itself, so
     * skip the Exists check (FatFs renames the same object in place). */
    if (ci && blen == alen && memcmp(a, b, alen) && same(a, b, alen, true)) return be->rename(ra, rb, false);
    e = be->stat(rb, &sz, &d);
    if (e == VFS_OK) return VFS_EXISTS;
    return e == VFS_NOTFOUND ? be->rename(ra, rb, false) : e;
}

int vfs_replace(const char *tmp, uint32_t tlen, const char *p, uint32_t len) {
    char ra[VFS_PATH_MAX + 1], rb[VFS_PATH_MAX + 1];
    uint32_t mi;
    int e = resolve2(tmp, tlen, p, len, &mi, ra, rb);
    if (e) return e;
    const VfsBackend *be = mounts[mi].be;
    e = be->rename(ra, rb, true);
    if (e != VFS_EXISTS) return e;
    e = be->remove(rb);                         /* FAT-like: not atomic */
    return e ? e : be->rename(ra, rb, false);
}

int vfs_usage(const char *prefix, uint32_t len, int32_t *used, int32_t *total) {
    char rel[VFS_PATH_MAX + 1];
    uint32_t mi, h;
    int e = resolve(prefix, len, true, &mi, rel, &h);
    return e ? e : mounts[mi].be->usage(used, total);
}
