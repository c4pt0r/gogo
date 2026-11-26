#include "memfs.h"
#include <string.h>

#define NODES 16
#define DATA 4200     /* > 4096: lets the builtin tests see Read's clamp */
#define PMAX 97

typedef struct { bool used, dir; char path[PMAX]; uint32_t size; uint8_t data[DATA]; } Node;
typedef struct {
    Node n[NODES];
    bool ci, atomic;
    int32_t budget, used;
    int rename_calls;
    int stat_err;          /* nonzero: stat returns it */
    bool close_fails;      /* close returns GOGO_E_IO (after closing) */
    int rename_fail;       /* nonzero: the next rename(replace=false) returns it */
} Mem;
typedef struct { Mem *m; Node *nd; uint32_t pos; int mode; } H;
typedef char h_fits[sizeof(H) <= GOGO_FS_SLOT_BYTES ? 1 : -1];

static Mem mem[2];
VfsBackend memfs_be[2];

static char lc(char c, bool ci) { return (ci && c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
static bool eq(const Mem *m, const char *a, const char *b) {
    for (; *a || *b; a++, b++) if (lc(*a, m->ci) != lc(*b, m->ci)) return false;
    return true;
}
static Node *find(Mem *m, const char *p) {
    for (int i = 0; i < NODES; i++) if (m->n[i].used && eq(m, m->n[i].path, p)) return &m->n[i];
    return 0;
}
static int parent_ok(Mem *m, const char *p) {
    char b[PMAX];
    size_t n = strlen(p);
    memcpy(b, p, n + 1);
    while (n > 0 && b[n - 1] != '/') n--;
    if (n <= 1) return GOGO_E_OK;
    b[n - 1] = 0;
    Node *d = find(m, b);
    return !d ? GOGO_E_NOTFOUND : d->dir ? GOGO_E_OK : GOGO_E_NOTDIR;
}
static Node *alloc(Mem *m, const char *p, bool dir) {
    for (int i = 0; i < NODES; i++)
        if (!m->n[i].used) {
            Node *nd = &m->n[i];
            memset(nd, 0, sizeof *nd);
            nd->used = true; nd->dir = dir;
            strcpy(nd->path, p);
            return nd;
        }
    return 0;
}
static bool is_root(const char *p) { return p[0] == '/' && !p[1]; }

static int m_open(Mem *m, void *f, const char *rel, int mode) {
    H *h = f;
    Node *nd = find(m, rel);
    if (is_root(rel)) return GOGO_E_ISDIR;
    if (nd && nd->dir) return GOGO_E_ISDIR;
    if (!nd) {
        if (mode == VFS_READ) return GOGO_E_NOTFOUND;
        int e = parent_ok(m, rel);
        if (e) return e;
        if (!(nd = alloc(m, rel, false))) return GOGO_E_NOMEMORY;
    } else if (mode == VFS_WRITE) {
        m->used -= (int32_t)nd->size;
        nd->size = 0;
    }
    h->m = m; h->nd = nd; h->mode = mode;
    h->pos = mode == VFS_APPEND ? nd->size : 0;
    return GOGO_E_OK;
}
static int m_read(void *f, uint8_t *buf, uint32_t n, uint32_t *got) {
    H *h = f;
    uint32_t k = h->pos < h->nd->size ? h->nd->size - h->pos : 0;
    if (k > n) k = n;
    memcpy(buf, h->nd->data + h->pos, k);
    h->pos += k;
    *got = k;
    return GOGO_E_OK;
}
static int m_write(void *f, const uint8_t *buf, uint32_t n) {
    H *h = f;
    uint32_t end = h->nd->size + n;                 /* append-only writes */
    if (end > DATA) return GOGO_E_NOSPACE;
    if (h->m->budget >= 0 && h->m->used + (int32_t)n > h->m->budget) return GOGO_E_NOSPACE;
    memcpy(h->nd->data + h->nd->size, buf, n);
    h->nd->size = end;
    h->m->used += (int32_t)n;
    return GOGO_E_OK;
}
static int m_write_at(void *f, uint32_t off, const uint8_t *buf, uint32_t n) {
    H *h = f;
    memcpy(h->nd->data + off, buf, n);              /* vfs checked the bounds */
    return GOGO_E_OK;
}
static int m_ok(void *f) { (void)f; return GOGO_E_OK; }
static int m_close(void *f) { return ((H *)f)->m->close_fails ? GOGO_E_IO : GOGO_E_OK; }
static int m_size(void *f, int32_t *out) { *out = (int32_t)((H *)f)->nd->size; return GOGO_E_OK; }
static int m_stat(Mem *m, const char *rel, int32_t *size, bool *is_dir) {
    if (m->stat_err) return m->stat_err;
    if (is_root(rel)) { *size = 0; *is_dir = true; return GOGO_E_OK; }
    Node *nd = find(m, rel);
    if (!nd) return GOGO_E_NOTFOUND;
    *size = (int32_t)nd->size;
    *is_dir = nd->dir;
    return GOGO_E_OK;
}
static bool child_of(const Mem *m, const char *dir, const char *path, const char **name) {
    size_t n = is_root(dir) ? 0 : strlen(dir);
    for (size_t i = 0; i < n; i++) if (lc(dir[i], m->ci) != lc(path[i], m->ci)) return false;
    if (path[n] != '/' || !path[n + 1]) return false;
    *name = path + n + 1;
    return true;
}
static int m_readdir(Mem *m, const char *rel, VfsDirCb cb, void *ctx) {
    if (!is_root(rel)) {
        Node *d = find(m, rel);
        if (!d) return GOGO_E_NOTFOUND;
        if (!d->dir) return GOGO_E_NOTDIR;
    }
    for (int i = 0; i < NODES; i++) {
        const char *name;
        if (m->n[i].used && child_of(m, rel, m->n[i].path, &name) && !strchr(name, '/'))
            if (cb(ctx, name, m->n[i].dir)) break;
    }
    return GOGO_E_OK;
}
static int m_mkdir(Mem *m, const char *rel) {
    if (find(m, rel)) return GOGO_E_EXISTS;
    int e = parent_ok(m, rel);
    if (e) return e;
    return alloc(m, rel, true) ? GOGO_E_OK : GOGO_E_NOMEMORY;
}
static int m_remove(Mem *m, const char *rel) {
    Node *nd = find(m, rel);
    if (!nd) return GOGO_E_NOTFOUND;
    if (nd->dir)
        for (int i = 0; i < NODES; i++) {
            const char *name;
            if (m->n[i].used && child_of(m, rel, m->n[i].path, &name)) return GOGO_E_NOTEMPTY;
        }
    m->used -= (int32_t)nd->size;
    nd->used = false;
    return GOGO_E_OK;
}
static int m_rename(Mem *m, const char *a, const char *b, bool replace) {
    Node *src = find(m, a), *dst = find(m, b);
    if (replace) m->rename_calls++;
    if (!replace && m->rename_fail) { int e = m->rename_fail; m->rename_fail = 0; return e; }
    if (!src) return GOGO_E_NOTFOUND;
    if (dst == src) { strcpy(src->path, b); return GOGO_E_OK; }   /* case-only, like FatFs */
    if (dst) {
        if (!replace || !m->atomic) return GOGO_E_EXISTS;
        if (dst->dir) return GOGO_E_ISDIR;
        m->used -= (int32_t)dst->size;
        dst->used = false;
    }
    int e = parent_ok(m, b);
    if (e) return e;
    strcpy(src->path, b);
    return GOGO_E_OK;
}
static int m_usage(Mem *m, int32_t *used, int32_t *total) {
    *used = m->used;
    *total = m->budget;
    return GOGO_E_OK;
}

static int open_(void *c, void *f, const char *r, int mo) { return m_open(c, f, r, mo); }
static int read_(void *c, void *f, uint8_t *b, uint32_t n, uint32_t *g) { (void)c; return m_read(f, b, n, g); }
static int write_(void *c, void *f, const uint8_t *b, uint32_t n) { (void)c; return m_write(f, b, n); }
static int ok_(void *c, void *f) { (void)c; return m_ok(f); }
static int size_(void *c, void *f, int32_t *o) { (void)c; return m_size(f, o); }
static int close_(void *c, void *f) { (void)c; return m_close(f); }
static int stat_(void *c, const char *r, int32_t *s, bool *d) { return m_stat(c, r, s, d); }
static int rdir_(void *c, const char *r, VfsDirCb cb, void *x) { return m_readdir(c, r, cb, x); }
static int mkd_(void *c, const char *r) { return m_mkdir(c, r); }
static int rm_(void *c, const char *r) { return m_remove(c, r); }
static int mv_(void *c, const char *a, const char *b, bool rp) { return m_rename(c, a, b, rp); }
static int use_(void *c, int32_t *u, int32_t *t) { return m_usage(c, u, t); }
static int wat_(void *c, void *f, uint32_t off, const uint8_t *b, uint32_t n) { (void)c; return m_write_at(f, off, b, n); }

void memfs_init(int i, bool ci, bool atomic, int32_t budget) {
    memset(&mem[i], 0, sizeof mem[i]);
    mem[i].ci = ci; mem[i].atomic = atomic; mem[i].budget = budget;
    VfsBackend b = { ci, open_, read_, write_, ok_, size_, close_, stat_, rdir_, mkd_, rm_, mv_, use_, wat_ };
    memfs_be[i] = b;
}
void *memfs_ctx(int i) { return &mem[i]; }
void memfs_set_budget(int i, int32_t budget) { mem[i].budget = budget; }
int memfs_rename_calls(int i) { return mem[i].rename_calls; }
void memfs_set_stat_err(int i, int err) { mem[i].stat_err = err; }
void memfs_set_close_fails(int i, bool fails) { mem[i].close_fails = fails; }

int memfs_name(int i, const char *rel, char *out, uint32_t cap) {
    Node *nd = find(&mem[i], rel);
    if (!nd) return GOGO_E_NOTFOUND;
    if (strlen(nd->path) >= cap) return GOGO_E_IO;
    strcpy(out, nd->path);
    return GOGO_E_OK;
}

void memfs_fail_next_rename(int i, int err) { mem[i].rename_fail = err; }
