/* vfs core: mounts, slot table, the Busy / Exists / replace rules. */
#include "vfs_int.h"
#include <string.h>

typedef VfsMount Mount;
typedef VfsSlot Slot;

uint32_t vfs_state_bytes(void) { return (uint32_t)sizeof(VfsState); }

bool vfs_mount(VfsState *st, const char *prefix, const VfsBackend *b, void *be_ctx) {
    uint32_t n = (uint32_t)strlen(prefix), first;
    if (st->n_mounts >= VFS_MAX_MOUNTS || vfs_path_check(prefix, n, &first) || first != n) return false;
    for (uint32_t i = 0; i < st->n_mounts; i++)
        if (st->mounts[i].plen == n && !memcmp(st->mounts[i].prefix, prefix, n)) return false;
    Mount *m = &st->mounts[st->n_mounts++];
    m->prefix = prefix;
    m->plen = n;
    m->be = b;
    m->ctx = be_ctx;
    return true;
}

void vfs_reset(VfsState *st) { memset(st, 0, sizeof *st); }

/* Validate and route. rel gets the backend path ("/" for the mount root). */
static int resolve(const VfsState *st, const char *p, uint32_t len, bool root_ok, uint32_t *mi, char *rel, uint32_t *hash) {
    uint32_t first;
    int e = vfs_path_check(p, len, &first);
    if (e) return e;
    for (uint32_t i = 0; i < st->n_mounts; i++) {
        const Mount *m = &st->mounts[i];
        if (m->plen != first || memcmp(p, m->prefix, first)) continue;
        if (first == len && !root_ok) return VFS_BADPATH;
        if (first == len) { rel[0] = '/'; rel[1] = 0; }
        else { memcpy(rel, p + first, len - first); rel[len - first] = 0; }
        *mi = i;
        *hash = vfs_path_hash(p, len, m->be->case_insensitive);
        return VFS_OK;
    }
    return VFS_NOTMOUNTED;
}

static bool is_open(const VfsState *st, uint32_t hash) {
    for (uint32_t i = 0; i < GOGO_FS_MAX_OPEN; i++)
        if (st->slots[i].used && st->slots[i].hash == hash) return true;
    return false;
}

int vfs_open(VfsState *st, const char *p, uint32_t len, int mode, int *slot) {
    char rel[VFS_PATH_MAX + 1];
    uint32_t mi, h;
    int e = resolve(st, p, len, true, &mi, rel, &h);
    if (e) return e;
    if (mode < VFS_READ || mode > VFS_APPEND) return VFS_BADPATH;
    const Mount *m = &st->mounts[mi];
    Slot *slots = st->slots;
    int s = -1;
    for (uint32_t i = 0; i < GOGO_FS_MAX_OPEN; i++) {
        if (!slots[i].used) { if (s < 0) s = (int)i; }
        else if (mode != VFS_READ && slots[i].hash == h &&
                 (slots[i].mode != VFS_READ || (mode == VFS_WRITE && m->be->case_insensitive)))
            return VFS_BUSY;   /* one writer; on FAT no truncate under a reader */
    }
    if (s < 0) return VFS_TOOMANY;
    e = m->be->open(m->ctx, &slots[s].f, rel, mode);
    if (e) return e;
    slots[s].used = 1;
    slots[s].mode = (uint8_t)mode;
    slots[s].mnt = (uint8_t)mi;
    slots[s].hash = h;
    *slot = s;
    return VFS_OK;
}

static Slot *get(VfsState *st, int s) {
    return (s >= 0 && s < (int)GOGO_FS_MAX_OPEN && st->slots[s].used) ? &st->slots[s] : 0;
}

/* The mount slot s is open on. */
static const Mount *mnt(const VfsState *st, const Slot *s) { return &st->mounts[s->mnt]; }

int vfs_read(VfsState *st, int slot, uint8_t *buf, uint32_t n, uint32_t *got) {
    Slot *s = get(st, slot);
    if (!s || s->mode != VFS_READ) return VFS_CLOSED;
    const Mount *m = mnt(st, s);
    return m->be->read(m->ctx, &s->f, buf, n, got);
}

int vfs_write(VfsState *st, int slot, const uint8_t *p, uint32_t n) {
    Slot *s = get(st, slot);
    if (!s || s->mode == VFS_READ) return VFS_CLOSED;
    const Mount *m = mnt(st, s);
    return m->be->write(m->ctx, &s->f, p, n);
}

int vfs_write_at(VfsState *st, int slot, uint32_t off, const uint8_t *p, uint32_t n) {
    Slot *s = get(st, slot);
    int32_t size;
    if (!s || s->mode != VFS_WRITE) return VFS_CLOSED;
    const Mount *m = mnt(st, s);
    const VfsBackend *b = m->be;
    if (!b->write_at) return VFS_IO;
    if (n == 0) return VFS_OK;
    int e = b->size(m->ctx, &s->f, &size);
    if (e) return e;
    if (size < 0 || off > (uint32_t)size || n > (uint32_t)size - off) return VFS_BADPATH;
    return b->write_at(m->ctx, &s->f, off, p, n);
}

int vfs_sync(VfsState *st, int slot) {
    Slot *s = get(st, slot);
    if (!s) return VFS_CLOSED;
    const Mount *m = mnt(st, s);
    return m->be->sync(m->ctx, &s->f);
}

int vfs_size(VfsState *st, int slot, int32_t *out) {
    Slot *s = get(st, slot);
    if (!s) return VFS_CLOSED;
    const Mount *m = mnt(st, s);
    return m->be->size(m->ctx, &s->f, out);
}

void vfs_close(VfsState *st, int slot) {
    Slot *s = get(st, slot);
    if (!s) return;
    const Mount *m = mnt(st, s);
    m->be->close(m->ctx, &s->f);
    s->used = 0;
}

int vfs_slot_mode(VfsState *st, int slot) {
    Slot *s = get(st, slot);
    return s ? s->mode : -1;
}

int vfs_stat(VfsState *st, const char *p, uint32_t len, int32_t *size, bool *is_dir) {
    char rel[VFS_PATH_MAX + 1];
    uint32_t mi, h;
    int e = resolve(st, p, len, true, &mi, rel, &h);
    return e ? e : st->mounts[mi].be->stat(st->mounts[mi].ctx, rel, size, is_dir);
}

int vfs_readdir(VfsState *st, const char *p, uint32_t len, VfsDirCb cb, void *ctx) {
    char rel[VFS_PATH_MAX + 1];
    uint32_t mi, h;
    int e = resolve(st, p, len, true, &mi, rel, &h);
    return e ? e : st->mounts[mi].be->readdir(st->mounts[mi].ctx, rel, cb, ctx);
}

int vfs_mkdir(VfsState *st, const char *p, uint32_t len) {
    char rel[VFS_PATH_MAX + 1];
    uint32_t mi, h;
    int e = resolve(st, p, len, false, &mi, rel, &h);
    return e ? e : st->mounts[mi].be->mkdir(st->mounts[mi].ctx, rel);
}

int vfs_remove(VfsState *st, const char *p, uint32_t len) {
    char rel[VFS_PATH_MAX + 1];
    uint32_t mi, h;
    int e = resolve(st, p, len, false, &mi, rel, &h);
    if (e) return e;
    if (is_open(st, h)) return VFS_BUSY;
    return st->mounts[mi].be->remove(st->mounts[mi].ctx, rel);
}

/* Both paths resolved on one mount, neither open. */
static int resolve2(const VfsState *st, const char *a, uint32_t alen, const char *b, uint32_t blen,
                    uint32_t *mi, char *ra, char *rb) {
    uint32_t mj, ha, hb;
    int e = resolve(st, a, alen, false, mi, ra, &ha);
    if (!e) e = resolve(st, b, blen, false, &mj, rb, &hb);
    if (e) return e;
    if (*mi != mj) return VFS_BADPATH;          /* no cross-mount rename */
    if (is_open(st, ha) || is_open(st, hb)) return VFS_BUSY;
    return VFS_OK;
}

static char fold(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

/* The first n bytes of a and b match; case folded when ci. */
static bool same(const char *a, const char *b, uint32_t n, bool ci) {
    for (uint32_t i = 0; i < n; i++)
        if (a[i] != b[i] && (!ci || fold(a[i]) != fold(b[i]))) return false;
    return true;
}

int vfs_rename(VfsState *st, const char *a, uint32_t alen, const char *b, uint32_t blen) {
    char ra[VFS_PATH_MAX + 1], rb[VFS_PATH_MAX + 1];
    uint32_t mi;
    int32_t sz;
    bool d;
    int e = resolve2(st, a, alen, b, blen, &mi, ra, rb);
    if (e) return e;
    const VfsBackend *be = st->mounts[mi].be;
    void *c = st->mounts[mi].ctx;
    bool ci = be->case_insensitive;
    if (blen > alen && b[alen] == '/' && same(a, b, alen, ci)) return VFS_BADPATH;   /* into its own subtree */
    /* A directory with any file open on this mount is Busy: slots keep only
     * the hash of their path, so a renamed ancestor would escape the rules.
     * A missing source falls through to the backend's NotFound. */
    e = be->stat(c, ra, &sz, &d);
    if (e == VFS_OK && d) {
        for (uint32_t i = 0; i < GOGO_FS_MAX_OPEN; i++)
            if (st->slots[i].used && st->slots[i].mnt == mi) return VFS_BUSY;
    } else if (e && e != VFS_NOTFOUND) return e;
    /* A case-only rename on a case-insensitive mount: b names a itself, so
     * skip the Exists check (FatFs renames the same object in place). */
    if (ci && blen == alen && memcmp(a, b, alen) && same(a, b, alen, true)) return be->rename(c, ra, rb, false);
    e = be->stat(c, rb, &sz, &d);
    if (e == VFS_OK) return VFS_EXISTS;
    return e == VFS_NOTFOUND ? be->rename(c, ra, rb, false) : e;
}

int vfs_replace(VfsState *st, const char *tmp, uint32_t tlen, const char *p, uint32_t len) {
    char ra[VFS_PATH_MAX + 1], rb[VFS_PATH_MAX + 1];
    uint32_t mi;
    int e = resolve2(st, tmp, tlen, p, len, &mi, ra, rb);
    if (e) return e;
    const VfsBackend *be = st->mounts[mi].be;
    void *c = st->mounts[mi].ctx;
    e = be->rename(c, ra, rb, true);
    if (e != VFS_EXISTS) return e;
    e = be->remove(c, rb);                      /* FAT-like: not atomic */
    return e ? e : be->rename(c, ra, rb, false);
}

int vfs_usage(VfsState *st, const char *prefix, uint32_t len, int32_t *used, int32_t *total) {
    char rel[VFS_PATH_MAX + 1];
    uint32_t mi, h;
    int e = resolve(st, prefix, len, true, &mi, rel, &h);
    return e ? e : st->mounts[mi].be->usage(st->mounts[mi].ctx, used, total);
}
