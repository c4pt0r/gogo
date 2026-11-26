/* B_FS: the gogo/fs builtin and its private `file` port driver.
 *
 * vm/ only holds the hook (Vm.fs_builtin, actor.h) and one case in the
 * interpreter; vfs_init(vm, st) points the hook here and the state at st. B_FS's first argument is
 * the op (FS_OPEN..FS_WRITEAT, vfs.h), the rest are the op's arguments.
 * Two-result ops write (value, Err) into ra, ra+1 and check that ra+1 is
 * in the frame; the others write Err into ra only.
 *
 * A File is the PID of a `file` port holding a vfs slot (state[0]) and,
 * for Read, the read position (state[1]). The driver is not in the
 * platform list (port.Open("file") fails), never produces events, closes
 * its slot on port close (f.Close(), owner exit, VM shutdown), and charges
 * the owner GOGO_FS_SLOT_BYTES (Handoff moves the charge). Only the owner
 * may Read/Write/Sync/Size; anything else is Closed.
 *
 * Every argument is read (a path copied into a stack buffer) before any
 * allocation; allocations may GC, so the register pointer is re-derived
 * after them. Results: Read/ReadFile Bytes are heap K_BYTES up to
 * GOGO_BYTES_INLINE bytes and off-heap above (charged to the caller), a
 * result that does not fit the caller is (empty, NoMemory) with nothing
 * consumed; Stat/Usage build their reserved struct, ReadDir a []string. */
#include "vfs_int.h"
#include "actor.h"
#include "heap.h"
#include "bytes.h"
#include "port.h"
#include "signals.h"
#include "opcodes.h"
#include "host.h"
#include "config.h"
#include <string.h>

#define CHARGE_WORDS (GOGO_FS_SLOT_BYTES / 4u)

/* Arguments after the op, and the ops with two results. */
static const uint8_t n_args[FS_WRITEAT + 1] = { 2, 2, 2, 1, 1, 2, 2, 1, 1, 1, 1, 2, 1, 3 };
#define TWO_RESULTS ((1u << FS_OPEN) | (1u << FS_READ) | (1u << FS_SIZE) | (1u << FS_READFILE) | \
                     (1u << FS_STAT) | (1u << FS_READDIR) | (1u << FS_USAGE))

/* ---- the `file` port ---- */

static int f_open(struct Vm *vm, Port *p, uint16_t index, const int32_t *cfg, uint32_t n) {
    (void)vm;
    (void)index; (void)n;
    p->state[0] = (uint32_t)cfg[0];
    p->state[1] = 0;
    p->charge_words = CHARGE_WORDS;
    return 0;
}
static void f_close(struct Vm *vm, Port *p) { vfs_close(vm->vfs, (int)p->state[0]); }
static bool f_can_event(const struct Vm *vm, const Port *p) { (void)vm; (void)p; return false; }

static const PortDriver file_drv = {
    PORT_PRIVATE_FILE, UINT32_MAX, UINT32_MAX, f_open, NULL, f_close, f_can_event, NULL, NULL, NULL
};

/* The open file t names if a owns it, else NULL (Closed). */
static Port *file_of(Vm *vm, const Actor *a, term t) {
    if (!port_is(t)) return NULL;
    uint32_t i = term_pid_slot(t) - PORT_SLOT_BASE;
    if (i >= GOGO_MAX_PORTS) return NULL;
    Port *p = &vm->ports[i];
    return p->drv == &file_drv && p->id == t && p->owner == a->id ? p : NULL;
}

/* ---- arguments ---- */

/* Copies path string t into buf (VFS_PATH_MAX + 1 bytes); a longer path is
 * cut to VFS_PATH_MAX + 1 bytes, which vfs rejects (BadPath). */
static bool arg_path(const Vm *vm, const Actor *a, term t, char *buf, uint32_t *len) {
    const uint8_t *p;
    uint32_t n;
    if (!name_bytes(vm, a, t, &p, &n)) return false;
    if (n > VFS_PATH_MAX + 1u) n = VFS_PATH_MAX + 1u;
    if (n) memcpy(buf, p, n);
    *len = n;
    return true;
}

/* The bytes of a string, Bytes or []byte (valid until a's next allocation). */
static bool arg_data(const Vm *vm, const Actor *a, term t, const uint8_t **p, uint32_t *n) {
    static const uint8_t none[1];
    *p = none;
    *n = 0;
    if (t == T_NILSLICE) return true;
    if (name_bytes(vm, a, t, p, n) || bytes_view(vm, a, t, p, n)) {
        if (!*p) *p = none;
        return true;
    }
    const uint32_t *o = obj_of(vm, a, t);
    if (!o || o[0] != hdr_make(5, K_SLICE) || o[2] > o[3]) return false;
    if (o[4] == T_NIL) return o[3] == 0;
    const uint32_t *arr = obj_of(vm, a, o[4]);
    if (!arr || hdr_kind(arr[0]) != K_BYTEARR || arr[1] > (hdr_words(arr[0]) - 2u) * 4u ||
        o[1] > arr[1] || o[3] > arr[1] - o[1]) return false;
    *p = (const uint8_t *)(arr + 2) + o[1];
    *n = o[2];
    return true;
}

/* ---- results ---- */

/* A Bytes of n (> 0) bytes for a to fill at *d. False if it does not fit. May GC. */
static bool new_bytes(Vm *vm, Actor *a, uint32_t n, term *out, uint8_t **d) {
    if (n > GOGO_BYTES_INLINE) {
        *out = bytes_make(vm, a, n, d);
        return *out != T_NIL;
    }
    uint32_t words = 2u + (n + 3u) / 4u;
    uint32_t o = heap_alloc(vm, a, words, NULL, 0);
    if (o == UINT32_MAX) return false;
    a->mem[o] = hdr_make(words, K_BYTES);
    a->mem[o + 1] = n;
    a->mem[o + words - 1u] = 0;
    *out = term_from_ref(o);
    *d = (uint8_t *)(a->mem + o + 2);
    return true;
}

/* Field value v at m[*k], boxed after the struct when it is not a small int. */
static term field_int(Actor *a, uint32_t o, uint32_t *k, int32_t v) {
    if (term_int_fits(v)) return term_from_int(v);
    a->mem[o + *k] = hdr_make(2, K_INT);
    a->mem[o + *k + 1u] = (uint32_t)v;
    term t = term_from_ref(o + *k);
    *k += 2u;
    return t;
}

/* A reserved struct {x int, y int | bool}. T_NIL if out of memory. May GC. */
static term new_pair(Vm *vm, Actor *a, uint32_t type, int32_t x, int32_t y, bool y_bool) {
    uint32_t words = 4u + (term_int_fits(x) ? 0u : 2u) + (y_bool || term_int_fits(y) ? 0u : 2u);
    uint32_t o = heap_alloc(vm, a, words, NULL, 0), k = 4;
    if (o == UINT32_MAX) return T_NIL;
    a->mem[o] = hdr_make(4, K_STRUCT);
    a->mem[o + 1] = type;
    a->mem[o + 2] = field_int(a, o, &k, x);
    a->mem[o + 3] = y_bool ? term_from_bool(y != 0) : field_int(a, o, &k, y);
    return term_from_ref(o);
}

/* ReadDir: a first pass counts the names and their words, one allocation
 * takes [K_SLICE][K_ARRAY][strings...], a second pass fills it. */
typedef struct {
    Actor *a;
    uint32_t n, words;     /* pass 1 totals; pass 2: entries filled, block end */
    uint32_t cap, next;    /* pass 2: entries counted, next free word */
    uint32_t arr;          /* pass 2: the array's offset */
} Dir;

static uint32_t name_words(const char *name, bool is_dir) {
    return 2u + ((uint32_t)strlen(name) + (is_dir ? 1u : 0u) + 3u) / 4u;
}
static int dir_count(void *ctx, const char *name, bool is_dir) {
    Dir *d = ctx;
    d->n++;
    d->words += name_words(name, is_dir);
    return d->words > HEAP_MAX_WORDS;      /* stop: it cannot fit anyway */
}
static int dir_fill(void *ctx, const char *name, bool is_dir) {
    Dir *d = ctx;
    uint32_t w = name_words(name, is_dir), len = (uint32_t)strlen(name);
    if (d->n == d->cap || d->next + w > d->words) return 1;   /* it changed */
    uint32_t *m = d->a->mem + d->next;
    m[0] = hdr_make(w, K_STRING);
    m[1] = len + (is_dir ? 1u : 0u);
    m[w - 1u] = 0;
    memcpy(m + 2, name, len);
    if (is_dir) ((uint8_t *)(m + 2))[len] = '/';
    d->a->mem[d->arr + 2u + d->n++] = term_from_ref(d->next);
    d->next += w;
    return 0;
}

/* The names in directory p (directories with a '/' suffix) into *out
 * (nil when empty); the Err. May GC. */
static int read_dir(Vm *vm, Actor *a, const char *p, uint32_t len, term *out) {
    Dir d = { a, 0, 7, 0, 0, 0 };
    *out = T_NILSLICE;
    int e = vfs_readdir(vm->vfs, p, len, dir_count, &d);
    if (e || d.n == 0) return e;
    d.words += d.n;
    if (d.words > HEAP_MAX_WORDS) return VFS_NOMEMORY;
    uint32_t o = heap_alloc(vm, a, d.words, NULL, 0);
    if (o == UINT32_MAX) return VFS_NOMEMORY;
    uint32_t *m = a->mem + o;
    d.cap = d.n;
    d.n = 0;
    d.arr = o + 5u;
    d.next = d.arr + 2u + d.cap;
    d.words += o;
    m[0] = hdr_make(5, K_SLICE);
    m[1] = 0;
    m[4] = term_from_ref(d.arr);
    m[5] = hdr_make(2u + d.cap, K_ARRAY);
    m[6] = d.cap;
    for (uint32_t i = 0; i < d.cap; i++) m[7 + i] = T_EMPTYSTR;
    /* Words a changed directory leaves unused are never reached (the GC
     * only follows refs), so they need no filling. */
    e = vfs_readdir(vm->vfs, p, len, dir_fill, &d);
    if (e) return e;
    m[2] = m[3] = d.n;
    *out = term_from_ref(o);
    return VFS_OK;
}

/* Reads exactly n bytes from slot into d: IO on a short read. */
static int read_full(VfsState *st, int slot, uint8_t *d, uint32_t n) {
    uint32_t done = 0, got;
    while (done < n) {
        int e = vfs_read(st, slot, d + done, n - done, &got);
        if (e) return e;
        if (got == 0) return VFS_IO;
        done += got;
    }
    return VFS_OK;
}

/* Bytes of n (>= 0) bytes read from slot into *out, Err. */
static int read_bytes(Vm *vm, Actor *a, int slot, uint32_t n, term *out) {
    uint8_t *d;
    *out = T_EMPTYBYTES;
    if (n == 0) return VFS_OK;
    term t;
    if (!new_bytes(vm, a, n, &t, &d)) return VFS_NOMEMORY;
    int e = read_full(vm->vfs, slot, d, n);
    if (!e) *out = t;
    return e;
}

/* WriteFile: <name>.tmp beside the file (the name cut so the temporary
 * fits the name and path limits and differs from the file), then
 * vfs_replace. A failure removes the temporary, unless the replace
 * failed with the target already gone. */
static int write_file(VfsState *st, const char *p, uint32_t len, const uint8_t *data, uint32_t n) {
    char tmp[VFS_PATH_MAX + 1];
    uint32_t first, slash = len, k;
    int32_t sz;
    bool dir;
    int e = vfs_path_check(p, len, &first), s;
    if (e) return e;
    if (first == len) return VFS_BADPATH;                /* a mount root */
    if (!vfs_stat(st, p, len, &sz, &dir) && dir) return VFS_ISDIR;
    while (p[slash - 1u] != '/') slash--;
    k = len - slash;
    /* <name>.tmp is the file itself when the name ends in ".tmp" and was
     * cut to it (any case: FAT): one byte shorter then. */
    if (k > VFS_NAME_MAX - 4u) k = VFS_NAME_MAX - 4u;
    if (slash + k + 4u > VFS_PATH_MAX) k = slash + 4u > VFS_PATH_MAX ? 0u : VFS_PATH_MAX - 4u - slash;
    if (slash + k + 4u == len && vfs_path_hash(p + len - 4u, 4, true) == vfs_path_hash(".tmp", 4, true))
        k--;
    if (k > VFS_NAME_MAX || slash + k + 4u > VFS_PATH_MAX) return VFS_BADPATH;   /* no room */
    memcpy(tmp, p, slash + k);
    memcpy(tmp + slash + k, ".tmp", 4);
    uint32_t tl = slash + k + 4u;
    e = vfs_open(st, tmp, tl, VFS_WRITE, &s);
    if (e) return e;
    e = vfs_write(st, s, data, n);
    if (!e) e = vfs_sync(st, s);
    vfs_close(st, s);
    if (!e) {
        e = vfs_replace(st, tmp, tl, p, len);
        /* A failed replace with the target gone (FAT: removed, then the
         * rename failed): the temporary is the only copy, keep it (§5.6). */
        if (e && vfs_stat(st, p, len, &sz, &dir) != VFS_OK) return e;
    }
    if (e) vfs_remove(st, tmp, tl);
    return e;
}

/* ---- the builtin ---- */

static uint32_t fs_builtin(Vm *vm, Actor *a, uint32_t ra, uint32_t rb, uint32_t rc, uint32_t nregs) {
    term *R = (term *)a->mem + a->fp;
    VfsState *st = vm->vfs;
    int32_t op, i = 0;
    if (rc == 0 || !int_val(vm, a, R[rb], &op) || op < 0 || op > FS_WRITEAT || rc != 1u + n_args[op])
        return R_BAD_CODE;
    bool two = (TWO_RESULTS >> op) & 1u;
    if (two && ra + 1u >= nregs) return R_BAD_CODE;
    char pa[VFS_PATH_MAX + 1], pb[VFS_PATH_MAX + 1];
    uint32_t la = 0, lb = 0, n = 0;
    const uint8_t *data = NULL;
    Port *f = NULL;
    term x = R[rb + 1], y = rc > 2u ? R[rb + 2] : T_NIL, res = T_NIL;
    int e = VFS_OK;

    /* Decode. */
    switch (op) {
    case FS_READ: case FS_WRITE: case FS_SYNC: case FS_SIZE: case FS_WRITEAT:
        f = file_of(vm, a, x);
        if (op == FS_READ && !int_val(vm, a, y, &i)) return R_BAD_CODE;
        if (op == FS_WRITE && !arg_data(vm, a, y, &data, &n)) return R_BAD_CODE;
        if (op == FS_WRITEAT && (!int_val(vm, a, y, &i) || !arg_data(vm, a, R[rb + 3], &data, &n))) return R_BAD_CODE;
        if (!f) e = VFS_CLOSED;
        break;
    default:
        if (!arg_path(vm, a, x, pa, &la)) return R_BAD_CODE;
        if ((op == FS_OPEN || op == FS_READFILE) && !int_val(vm, a, y, &i)) return R_BAD_CODE;
        if (op == FS_WRITEFILE && !arg_data(vm, a, y, &data, &n)) return R_BAD_CODE;
        if (op == FS_RENAME && !arg_path(vm, a, y, pb, &lb)) return R_BAD_CODE;
    }

    int slot = f ? (int)f->state[0] : -1;
    int32_t v = 0, w = 0;
    bool d = false;
    switch (op) {
    case FS_OPEN: {
        uint32_t k = 0;
        while (k < GOGO_MAX_PORTS && vm->ports[k].drv) k++;
        res = T_ZEROPID;
        if (k == GOGO_MAX_PORTS) e = VFS_TOOMANY;
        else if ((uint32_t)a->mem_words + actor_charged_words(a) + CHARGE_WORDS > actor_max_words(a))
            e = VFS_NOMEMORY;
        else if (!(e = vfs_open(st, pa, la, i, &slot))) {
            int32_t c = slot;
            res = port_open_with(vm, a, &file_drv, &c, 1);   /* no GC */
            if (res == T_NIL) {
                vfs_close(st, slot);
                res = T_ZEROPID;
                e = VFS_TOOMANY;
            }
        }
        break;
    }
    case FS_READ:
        res = T_EMPTYBYTES;
        if (e || vfs_slot_mode(st, slot) != VFS_READ) { e = VFS_CLOSED; break; }
        if (i <= 0) break;
        if (i > VFS_READ_MAX) i = VFS_READ_MAX;
        if ((e = vfs_size(st, slot, &v))) break;
        v = v > (int32_t)f->state[1] ? v - (int32_t)f->state[1] : 0;   /* left to read */
        if (v > i) v = i;
        e = read_bytes(vm, a, slot, (uint32_t)v, &res);   /* may GC; f stays valid */
        if (!e) f->state[1] += (uint32_t)v;
        break;
    case FS_WRITE:
        if (!e) e = vfs_write(st, slot, data, n);
        break;
    case FS_WRITEAT:
        if (!e) e = i < 0 ? VFS_BADPATH : vfs_write_at(st, slot, (uint32_t)i, data, n);
        break;
    case FS_SYNC:
        if (!e) e = vfs_sync(st, slot);
        break;
    case FS_SIZE:
        if (!e) e = vfs_size(st, slot, &v);
        if (e) v = 0;
        res = term_from_int(v);
        if (!term_int_fits(v)) {                                   /* boxed */
            uint32_t o = heap_alloc(vm, a, 2, NULL, 0), k = 0;
            res = o == UINT32_MAX ? T_NIL : field_int(a, o, &k, v);
        }
        break;
    case FS_READFILE:
        res = T_EMPTYBYTES;
        if ((e = vfs_open(st, pa, la, VFS_READ, &slot))) break;
        e = vfs_size(st, slot, &v);
        if (!e && v > i) e = VFS_NOMEMORY;
        if (!e) e = read_bytes(vm, a, slot, (uint32_t)v, &res);   /* may GC */
        vfs_close(st, slot);
        break;
    case FS_WRITEFILE:
        e = write_file(st, pa, la, data, n);
        break;
    case FS_STAT:
        if ((e = vfs_stat(st, pa, la, &v, &d))) { v = 0; d = false; }
        res = new_pair(vm, a, TYPE_FS_INFO, v, d, true);
        break;
    case FS_READDIR:
        e = read_dir(vm, a, pa, la, &res);
        break;
    case FS_MKDIR:
        e = vfs_mkdir(st, pa, la);
        break;
    case FS_REMOVE:
        e = vfs_remove(st, pa, la);
        break;
    case FS_RENAME:
        e = vfs_rename(st, pa, la, pb, lb);
        break;
    default: /* FS_USAGE */
        if ((e = vfs_usage(st, pa, la, &v, &w))) v = w = 0;
        res = new_pair(vm, a, TYPE_FS_SPACE, v, w, false);
        break;
    }
    if (res == T_NIL && two) return R_OUT_OF_MEMORY;   /* a 4-word result did not fit */
    R = (term *)a->mem + a->fp;
    if (two) R[ra + 1] = term_from_int(e);
    R[ra] = two ? res : term_from_int(e);
    return R_NORMAL;
}

void vfs_init(Vm *vm, VfsState *st) {
    vm->vfs = st;
    vm->fs_builtin = fs_builtin;
}
