/* The embedding API (include/gogo.h) over the VM core: a gogo_vm wraps a
 * Vm, forwards its hooks to the host's table (tracking every block of
 * host memory, so gogo_free returns it all), runs the post inbox as one of
 * its event sources and keeps the handle alive for posting threads. */
#include "embed.h"
#include "interp.h"
#include "module.h"
#include "alloc.h"
#include "image.h"
#include <string.h>

typedef char steps_match[((int)GOGO_RUNNABLE == (int)VM_RUNNABLE && (int)GOGO_IDLE == (int)VM_IDLE &&
                          (int)GOGO_DONE == (int)VM_DONE) ? 1 : -1];

/* No board hardware: ports, input and network come from the host. The
 * mounts are /flash when fs_root serves it (and the grant has it). */
static const char *const flash_mount[] = { "/flash", NULL };
static const VmBoard embed_board = { NULL, NULL, NULL, NULL, NULL };
static const VmBoard embed_board_flash = { NULL, NULL, NULL, NULL, flash_mount };

bool embed_granted(const gogo_vm *g, const char *name) {
    const char *const *l = g->cfg.grant;
    if (!l) return true;
    for (; *l; l++)
        if (!strcmp(*l, name)) return true;
    return false;
}

/* Scheduler passes gogo_run runs between checks for a stop. */
#define RUN_BUDGET 256u

/* ---- tracked host memory ---- */

static void *blk_alloc(gogo_vm *g, EmbedBlk **list, void *(*fn)(void *, size_t), size_t n) {
    if (n > (size_t)-1 - EMBED_BLK_HDR) return NULL;
    EmbedBlk *b = fn(g->uctx, n + EMBED_BLK_HDR);
    if (!b) return NULL;
    b->n = n + EMBED_BLK_HDR;
    b->prev = NULL;
    b->next = *list;
    if (b->next) b->next->prev = b;
    *list = b;
    return (char *)b + EMBED_BLK_HDR;
}

static void blk_free(gogo_vm *g, EmbedBlk **list, void (*fn)(void *, void *, size_t), void *p) {
    EmbedBlk *b = (EmbedBlk *)(void *)((char *)p - EMBED_BLK_HDR);
    if (b->prev) b->prev->next = b->next;
    else *list = b->next;
    if (b->next) b->next->prev = b->prev;
    fn(g->uctx, b, b->n);
}

static void blk_free_all(gogo_vm *g, EmbedBlk **list, void (*fn)(void *, void *, size_t)) {
    for (EmbedBlk *b = *list, *next; b; b = next) {
        next = b->next;
        fn(g->uctx, b, b->n);
    }
    *list = NULL;
}

/* ---- the Vm's hooks: forwarders to the host's table ---- */

static void f_write(void *c, int s, const char *p, size_t n) { gogo_vm *g = c; g->uh.write(g->uctx, s, p, n); }
static void *f_alloc(void *c, size_t n) { gogo_vm *g = c; return blk_alloc(g, &g->blocks, g->uh.alloc, n); }
static void f_free(void *c, void *p, size_t n) { (void)n; gogo_vm *g = c; blk_free(g, &g->blocks, g->uh.free, p); }
static uint64_t f_now(void *c) { gogo_vm *g = c; return g->uh.now_ms(g->uctx); }
static void f_wake(void *c) { gogo_vm *g = c; g->uh.wake(g->uctx); }
static void f_wait(void *c, uint32_t ms) { gogo_vm *g = c; g->uh.wait(g->uctx, ms); }
static int64_t f_unix(void *c) { gogo_vm *g = c; return g->uh.unix_time_ms(g->uctx); }
static int f_set_unix(void *c, int64_t ms) { gogo_vm *g = c; return g->uh.set_unix_time_ms(g->uctx, ms); }
static uint32_t f_random(void *c) { gogo_vm *g = c; return g->uh.random(g->uctx); }
static int32_t f_config(void *c, const uint8_t *k, uint32_t kl, uint8_t *b, uint32_t cap) {
    gogo_vm *g = c;
    return g->uh.config_get(g->uctx, k, kl, b, cap);
}
static void *f_code_alloc(void *c, size_t n) { gogo_vm *g = c; return blk_alloc(g, &g->code_blocks, g->uh.code_alloc, n); }
static void f_code_free(void *c, void *p, size_t n) {
    (void)n;
    gogo_vm *g = c;
    blk_free(g, &g->code_blocks, g->uh.code_free, p);
}

static void make_fwd(gogo_vm *g) {
    const gogo_host *u = &g->uh;
    gogo_host *f = &g->fwd;
    f->size = sizeof *f;
    f->write = f_write;
    f->alloc = f_alloc;
    f->free = f_free;
    f->now_ms = f_now;
    f->wake = u->wake ? f_wake : NULL;
    f->wait = u->wait ? f_wait : NULL;
    f->unix_time_ms = u->unix_time_ms ? f_unix : NULL;
    f->set_unix_time_ms = u->set_unix_time_ms ? f_set_unix : NULL;
    f->random = u->random ? f_random : NULL;
    f->config_get = u->config_get ? f_config : NULL;
    f->code_alloc = u->code_alloc ? f_code_alloc : NULL;
    f->code_free = u->code_free ? f_code_free : NULL;
}

/* ---- versions and errors ---- */

uint32_t gogo_version(void) { return GOGO_VERSION; }

uint32_t gogo_image_format(void) { return IMAGE_VERSION; }

/* gogo.h's results are the shared codes negated (gogo_errors.h). */
typedef char shared_codes_end[GOGO_E_SHARED_LAST <= GOGO_E_SHARED_MAX && GOGO_E_SHARED_MAX < 36 ? 1 : -1];

/* By code: the shared meanings (lib/errors/errors.gg), then the API's own. */
static const char *const code_text[] = {
    "ok",
    "not found",
    "exists already",
    "out of memory",
    "too many: a fixed table is full",
    "busy",
    "bad argument",
    "closed",
    "I/O error",
    "timed out",
    "unsupported: a driver or mount is missing",
    "no space left",
    "bad path",
    "is a directory",
    "not a directory",
    "directory not empty",
    "not mounted",
    "bad image",
    "connection refused",
    "connection reset",
    "no route",
    "bad certificate",
    "wall clock not set",
    "TLS error",
    "too large",
    "bad URL",
    "bad format",
    "unsupported image format version",
};
typedef char code_text_complete[sizeof code_text / sizeof code_text[0] == GOGO_E_SHARED_LAST + 1 ? 1 : -1];

const char *gogo_strerror(int rc) {
    if (rc == GOGO_ESTATE) return "called out of order";
    if (rc == GOGO_IDLE) return "idle: nothing to do until a deadline or a wake";
    if (rc == GOGO_DONE) return "done: the program has finished";
    if (rc > 0) return "unknown error";
    unsigned c = 0u - (unsigned)rc;
    return c < sizeof code_text / sizeof code_text[0] ? code_text[c] : "unknown error";
}

/* *dst = the first size bytes of src (a struct of the host's), the rest
 * zero: a larger struct's extra fields are ignored, and a struct of a
 * later size than the first one's would get the defaults for the fields
 * it lacks. False below min, the struct's first (v0.2) size: a size that
 * ends inside a field is never accepted. */
bool embed_copy_sized(void *dst, size_t cap, const void *src, uint32_t size, size_t min) {
    memset(dst, 0, cap);
    if (size < min) return false;
    memcpy(dst, src, size < cap ? size : cap);
    return true;
}

/* The first sizes, which never change: up to the last field of v0.2. */
#define HOST_SIZE_V1 (offsetof(gogo_host, destroy) + sizeof(void (*)(void *)))
#define CONFIG_SIZE_V1 (offsetof(gogo_config, fs_quota) + sizeof(int64_t))

/* The output structs' first sizes, which never change (v0.2). */
#define STATS_SIZE_V1 (offsetof(gogo_stats, mem_peak) + sizeof(uint64_t))
#define IMAGE_INFO_SIZE_V1 (offsetof(gogo_image_info, n_exports) + sizeof(uint32_t))

/* An output struct's size: what the caller allows of what the library
 * fills; 0 below the struct's v0.2 size (v1), as for input structs, so
 * no size ends inside a field. */
static uint32_t out_size(uint32_t caller, size_t v1, size_t lib) {
    if (caller < v1) return 0;
    return caller < lib ? caller : (uint32_t)lib;
}

/* ---- inspecting an image ---- */

/* Validates the image as vm_boot does (image_load), into *img. */
static int image_check(Image *img, const void *image, size_t len) {
    if (!image || ((uintptr_t)image & 3u)) return GOGO_EINVAL;
    if (len == 0 || len % 4u || len / 4u > UINT32_MAX) return GOGO_EIMAGE;
    int rc = image_load(img, image, (uint32_t)(len / 4u));
    return rc == IMG_OK ? GOGO_OK : rc == IMG_ERR_VERSION ? GOGO_EVERSION : GOGO_EIMAGE;
}

/* Every field the caller's size covers is written, 0 where there is no
 * value (an error), and size says how much this library filled. */
int gogo_image_info_get(const void *image, size_t len, gogo_image_info *info) {
    uint32_t n = info ? out_size(info->size, IMAGE_INFO_SIZE_V1, sizeof(gogo_image_info)) : 0;
    if (!n) return GOGO_EINVAL;
    gogo_image_info r;
    memset(&r, 0, sizeof r);
    r.size = n;
    Image img;
    int rc = image_check(&img, image, len);
    if (rc == GOGO_OK) {
        r.n_requires = image_requires(&img)[0];
        r.n_exports = img.n_exports;
    }
    if (rc != GOGO_EINVAL && len >= 8u && ((const uint32_t *)image)[0] == IMAGE_MAGIC) r.format = ((const uint32_t *)image)[1];
    memcpy(info, &r, n);
    return rc;
}

int gogo_image_requires(const void *image, size_t len, gogo_name *out, uint32_t cap, uint32_t *n) {
    if (!n || (cap && !out)) return GOGO_EINVAL;
    *n = 0;
    Image img;
    int rc = image_check(&img, image, len);
    if (rc != GOGO_OK) return rc;
    const uint32_t *r = image_requires(&img);
    *n = r[0];
    for (uint32_t i = 0; i < r[0] && i < cap; i++) {
        const uint32_t *s = img.consts + r[1u + i];   /* a K_STRING: header, length, bytes (image_load checked) */
        out[i].ptr = (const char *)(s + 2);
        out[i].len = s[1];
    }
    return GOGO_OK;
}

/* ---- lifecycle ---- */

static bool host_ok(const gogo_host *h) {
    return h && h->write && h->alloc && h->free && h->now_ms && (h->wake || !h->wait) &&
           !h->unix_time_ms == !h->set_unix_time_ms && !h->code_alloc == !h->code_free;
}

/* vm_init with the config, then the memory cap (vm_init clears the
 * allocator) and the inbox. */
static int vm_setup(gogo_vm *g) {
    uint16_t cap = g->cfg.max_actors ? (uint16_t)g->cfg.max_actors : (uint16_t)GOGO_TABLE_CAP;
    if (vm_init(&g->vm, &g->fwd, g, g->fs ? &embed_board_flash : &embed_board, cap) != 0) return -1;
    va_set_limit(&g->vm, g->cfg.mem_limit > (uint64_t)(size_t)-1 ? (size_t)-1 : (size_t)g->cfg.mem_limit);
    vfs_init(&g->vm, &g->vfs);       /* no fs_root: no mounts, every gogo/fs call is NotMounted */
    embed_ports_install(g);
    return port_add_source(&g->vm, &inbox_source, g) ? 0 : -1;
}

int gogo_new(const gogo_host *h, void *host_ctx, const gogo_config *cfg, gogo_vm **out) {
    gogo_host uh;
    gogo_config c;
    if (!out) return GOGO_EINVAL;
    *out = NULL;
    if (!h || !embed_copy_sized(&uh, sizeof uh, h, h->size, HOST_SIZE_V1) || uh.reserved || !host_ok(&uh))
        return GOGO_EINVAL;
    if (!cfg) {
        memset(&c, 0, sizeof c);
    } else if (!embed_copy_sized(&c, sizeof c, cfg, cfg->size, CONFIG_SIZE_V1) || c.reserved ||
               (c.flags & ~GOGO_CONFIG_CODE_LOAD) ||
               c.max_actors > PORT_SLOT_BASE || c.fs_quota < 0) {
        return GOGO_EINVAL;
    }
    gogo_vm *g = uh.alloc(host_ctx, sizeof *g);
    if (!g) return GOGO_ENOMEM;
    memset(g, 0, sizeof *g);
    g->uh = uh;
    g->uctx = host_ctx;
    g->cfg = c;
    g->refs = 1;
    make_fwd(g);
    /* fs_quota 0 is no quota (fs_dir: -1). */
    int rc = GOGO_OK;
    if (c.fs_root && embed_granted(g, "/flash")) {
        g->fs = fs_dir_open(&g->uh, host_ctx, c.fs_root, c.fs_quota ? c.fs_quota : -1, &rc);
        if (rc == GOGO_OK && !vfs_mount(&g->vfs, "/flash", fs_dir_vfs(g->fs), g->fs)) rc = GOGO_ENOMEM;
    }
    g->cfg.fs_root = NULL;           /* the caller's string is not kept */
    if (rc == GOGO_OK && vm_setup(g) != 0) rc = GOGO_ENOMEM;
    if (rc != GOGO_OK) {
        blk_free_all(g, &g->blocks, uh.free);
        fs_dir_close(g->fs);
        uh.free(host_ctx, g, sizeof *g);
        return rc;
    }
    *out = g;
    return GOGO_OK;
}

static char *put_u(char *p, uint32_t v) {
    char d[10];
    int n = 0;
    do d[n++] = (char)('0' + v % 10u); while ((v /= 10u) != 0);
    while (n) *p++ = d[--n];
    return p;
}

static char *put_s(char *p, const char *s) {
    while (*s) *p++ = *s++;
    return p;
}

/* "gogo: unsupported image format M.m (this VM runs 15.0 to 15.x)\n" for
 * an image with version word ver; returns its length (buf: 80 bytes). */
static size_t version_msg(char *buf, uint32_t ver) {
    char *p = put_s(buf, "gogo: unsupported image format ");
    p = put_u(p, ver >> 16);
    *p++ = '.';
    p = put_u(p, ver & 0xFFFFu);
    p = put_s(p, " (this VM runs ");
    p = put_u(p, IMAGE_MAJOR);
    p = put_s(p, ".0 to ");
    p = put_u(p, IMAGE_MAJOR);
    *p++ = '.';
    p = put_u(p, IMAGE_MINOR);
    p = put_s(p, ")\n");
    return (size_t)(p - buf);
}

int gogo_load(gogo_vm *g, const void *image, size_t len, int flags) {
    if (!g || !image) return GOGO_EINVAL;
    if (g->stepping) return GOGO_EBUSY;
    if (g->loaded) return GOGO_ESTATE;
    if (len == 0 || len % 4u || len / 4u > UINT32_MAX) return GOGO_EIMAGE;
    if (!(flags & GOGO_LOAD_COPY) && ((uintptr_t)image & 3u)) return GOGO_EINVAL;
    if (!g->vm.table && vm_setup(g) != 0) return GOGO_ENOMEM;   /* after a failed load */
    const void *words = image;
    if (flags & GOGO_LOAD_COPY) {
        if (!(g->image_copy = g->uh.alloc(g->uctx, len))) return GOGO_ENOMEM;
        memcpy(g->image_copy, image, len);
        g->image_bytes = len;
        words = g->image_copy;
    }
    int rc = vm_boot(&g->vm, words, (uint32_t)(len / 4u));
    if (rc == VM_BOOT_OK) {
        g->loaded = true;
        if (!(g->cfg.flags & GOGO_CONFIG_CODE_LOAD)) g->vm.mod_cap = 0;   /* every code.Load is NoMemory */
        return GOGO_OK;
    }
    if (rc == IMG_ERR_VERSION) {
        char msg[80];
        size_t n = version_msg(msg, ((const uint32_t *)words)[1]);
        g->uh.write(g->uctx, 2, msg, n);
    } else if (rc < 0) {
        static const char pre[] = "gogo: bad image: ";
        const char *why = image_err_str(rc);
        g->uh.write(g->uctx, 2, pre, sizeof pre - 1u);
        g->uh.write(g->uctx, 2, why, strlen(why));
        g->uh.write(g->uctx, 2, "\n", 1u);
    }
    if (g->image_copy) {
        g->uh.free(g->uctx, g->image_copy, g->image_bytes);
        g->image_copy = NULL;
    }
    /* Start over with a clean Vm (vm_boot gave its table back). */
    blk_free_all(g, &g->blocks, g->uh.free);
    if (vm_setup(g) != 0) g->vm.table = NULL;
    return rc == IMG_ERR_VERSION ? GOGO_EVERSION : rc < 0 ? GOGO_EIMAGE : rc == VM_BOOT_REFUSED ? GOGO_EREQUIRES : GOGO_ENOMEM;
}

int gogo_start(gogo_vm *g) {
    if (!g) return GOGO_EINVAL;
    if (g->stepping) return GOGO_EBUSY;
    if (!g->loaded) return GOGO_ESTATE;
    if (g->vm.run_state != VM_ST_NEW) return GOGO_ESTATE;
    if (!vm_start(&g->vm)) return GOGO_OK;
    uint32_t f;
    const ModEntry *m = g->vm.mods[0];
    return m && !image_export_find(&m->img, (const uint8_t *)"main", 4u, &f) ? GOGO_ENOTFOUND : GOGO_ENOMEM;
}

int gogo_step(gogo_vm *g, uint32_t budget) {
    if (!g) return GOGO_EINVAL;
    /* vm_step is not re-entrant: a hook it calls must not step again. */
    if (g->stepping) return GOGO_EBUSY;
    if (g->vm.run_state == VM_ST_NEW) return GOGO_ESTATE;
    bool running = g->vm.run_state == VM_ST_RUN;
    g->stepping = true;
    int r = vm_step(&g->vm, budget);
    if (r == VM_DONE && running) vm_stop(&g->vm);
    /* Done: later posts are refused (as after gogo_close_posts), and a
     * post that raced the end has no one to go to: it is counted as
     * dropped now rather than held until free. */
    if (r == VM_DONE) {
        embed_set_state(g, EMBED_CLOSED);
        inbox_clear(&g->inbox, &g->uh, g->uctx);
    }
    g->stepping = false;
    return r;
}

int64_t gogo_next_deadline(gogo_vm *g) {
    uint64_t t;
    if (!g || g->vm.run_state != VM_ST_RUN || !vm_next_deadline(&g->vm, &t)) return GOGO_NEVER;
    t = vm_host_ms(&g->vm, t);
    return t > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)t;
}

int gogo_run(gogo_vm *g) {
    if (!g || !g->uh.wait) return GOGO_EINVAL;
    if (g->stepping) return GOGO_EBUSY;
    if (g->vm.run_state == VM_ST_NEW) {
        int r = gogo_start(g);
        if (r) return r;
    }
    for (;;) {
        if (embed_dead(g)) return GOGO_ECLOSED;
        int r = gogo_step(g, RUN_BUDGET);
        if (r < 0) return r;
        if (r == GOGO_DONE) return GOGO_OK;
        if (r != GOGO_IDLE) continue;
        /* Idle: wait for the deadline or a wake. A post or event after the
         * step's last check is not lost: it wakes the host, and the wait
         * is latched. */
        int64_t next = gogo_next_deadline(g);
        if (next == GOGO_NEVER) {
            g->uh.wait(g->uctx, UINT32_MAX);
        } else {
            uint64_t now = g->uh.now_ms(g->uctx);
            if ((uint64_t)next > now) {
                uint64_t d = (uint64_t)next - now;
                g->uh.wait(g->uctx, d > UINT32_MAX - 1u ? UINT32_MAX - 1u : (uint32_t)d);
            }
        }
    }
}

/* The free/post handshake is a store-buffering pair: gogo_post writes
 * `posting` then reads `state`; gogo_free writes `state` then reads
 * `posting`. Only if all four accesses are seq_cst can both sides not read
 * the old value (with an acquire read, arm64 may satisfy the load before
 * its earlier store is visible). Either the post sees the close and backs
 * off, or the free sees the post in progress and waits for it. */
int embed_set_state(gogo_vm *g, int s) {
    int old = __atomic_load_n(&g->state, __ATOMIC_SEQ_CST);
    while (old < s && !__atomic_compare_exchange_n(&g->state, &old, s, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
    }
    return old;
}

/* Queues a post for the name (port 0) or the host port, and wakes. */
static int post(gogo_vm *g, const char *name, size_t nl, uint32_t port, int32_t kind, const void *data, size_t len) {
    __atomic_fetch_add(&g->posting, 1, __ATOMIC_SEQ_CST);   /* handshake: seq_cst (above) */
    int r = GOGO_ECLOSED;
    if (__atomic_load_n(&g->state, __ATOMIC_SEQ_CST) == EMBED_LIVE) {
        r = inbox_push(&g->inbox, &g->uh, g->uctx, name, nl, port, kind, data, len);
        if (r == 0 && g->uh.wake) g->uh.wake(g->uctx);
    }
    __atomic_fetch_sub(&g->posting, 1, __ATOMIC_RELEASE);
    return r;
}

static int post_name(gogo_vm *g, const char *name, size_t nl, int32_t kind, const void *data, size_t len) {
    if (!g || !name || (len && !data)) return GOGO_EINVAL;
    if (nl > GOGO_POST_NAME_MAX || len > GOGO_POST_DATA_MAX) return GOGO_E2BIG;
    return post(g, name, nl, 0, kind, data, len);
}

int gogo_post(gogo_vm *g, const char *name, size_t name_len, int32_t kind, const void *data, size_t len) {
    return post_name(g, name, name_len, kind, data, len);
}

void gogo_close_posts(gogo_vm *g) {
    if (!g) return;
    /* A VM idle only because a name kept it alive re-checks and ends
     * (once: a later close changes nothing). */
    if (embed_set_state(g, EMBED_CLOSED) < EMBED_CLOSED && g->uh.wake) g->uh.wake(g->uctx);
}

/* Drops a reference (the owner's at gogo_free, a poster's): the last one
 * gives the gogo_vm back and runs the host's destroy. */
static void unref(gogo_vm *g) {
    int old = __atomic_fetch_sub(&g->refs, 1, __ATOMIC_ACQ_REL);
    GOGO_ASSERT(old > 0);
    if (old != 1) return;
    void (*hfree)(void *, void *, size_t) = g->uh.free;
    void (*destroy)(void *) = g->uh.destroy;
    void *ctx = g->uctx;
    hfree(ctx, g, sizeof *g);
    if (destroy) destroy(ctx);
}

/* ---- posters: a small block of host memory holding a VM reference ---- */

/* A poster's state word: live until gogo_poster_free, which marks it
 * dead before it gives the block back. A use after the free (a second
 * free too) is undefined behaviour; while the block has not been reused,
 * a debug build's assertion usually catches it. */
#define POSTER_LIVE 0x504F5354u
#define POSTER_DEAD 0x44454144u

struct gogo_poster {
    uint32_t state;              /* atomic: POSTER_LIVE, then POSTER_DEAD */
    gogo_vm *vm;
};

/* The poster's VM, or NULL for no poster or a freed one. */
static gogo_vm *vm_of(gogo_poster *p) {
    if (!p) return NULL;
    bool live = __atomic_load_n(&p->state, __ATOMIC_ACQUIRE) == POSTER_LIVE;
    GOGO_ASSERT(live);
    return live ? p->vm : NULL;
}

gogo_poster *gogo_poster_new(gogo_vm *g) {
    if (!g) return NULL;
    gogo_poster *p = g->uh.alloc(g->uctx, sizeof *p);
    if (!p) return NULL;
    p->vm = g;
    __atomic_store_n(&p->state, POSTER_LIVE, __ATOMIC_RELEASE);
    __atomic_fetch_add(&g->refs, 1, __ATOMIC_RELAXED);   /* the caller holds the owner's */
    return p;
}

int gogo_poster_post(gogo_poster *p, const char *name, size_t name_len, int32_t kind, const void *data, size_t len) {
    return post_name(vm_of(p), name, name_len, kind, data, len);
}

int gogo_poster_port_post(gogo_poster *p, gogo_port port, int32_t kind, const void *data, size_t len) {
    gogo_vm *g = vm_of(p);
    if (!g || (len && !data) || !embed_port_valid(port)) return GOGO_EINVAL;
    if (len > GOGO_POST_DATA_MAX) return GOGO_E2BIG;
    return post(g, "", 0, port, kind, data, len);
}

void gogo_poster_close_posts(gogo_poster *p) { gogo_close_posts(vm_of(p)); }

void gogo_poster_free(gogo_poster *p) {
    if (!p) return;
    uint32_t live = POSTER_LIVE;
    bool first = __atomic_compare_exchange_n(&p->state, &live, POSTER_DEAD, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    GOGO_ASSERT(first);          /* freed twice */
    if (!first) return;
    gogo_vm *g = p->vm;
    g->uh.free(g->uctx, p, sizeof *p);   /* the VM's reference keeps uh and uctx */
    unref(g);
}

int gogo_get_stats(gogo_vm *g, gogo_stats *out) {
    uint32_t n = out ? out_size(out->size, STATS_SIZE_V1, sizeof(gogo_stats)) : 0;
    if (!g || !n) return GOGO_EINVAL;
    gogo_stats st;
    memset(&st, 0, sizeof st);
    st.size = n;
    st.posts_delivered = g->inbox.delivered;
    st.posts_dropped = g->inbox.dropped;
    st.mem_used = va_used_bytes(&g->vm);
    st.mem_peak = va_peak_bytes(&g->vm);
    memcpy(out, &st, n);
    return GOGO_OK;
}

void gogo_free(gogo_vm *g) {
    if (!g) return;
    embed_set_state(g, EMBED_DEAD);
    if (g->thread) embed_thread_stop(g);
    /* Posts in progress finish quickly (a copy and a push); later ones see
     * the state and leave the inbox alone. The read is seq_cst: the
     * handshake (gogo_post). After a while, let a preempted poster run. */
    for (unsigned spins = 0; __atomic_load_n(&g->posting, __ATOMIC_SEQ_CST) != 0; spins++) {
        if (spins >= 1000u && g->uh.wait) g->uh.wait(g->uctx, 1);
    }
    inbox_clear(&g->inbox, &g->uh, g->uctx);
    if (g->vm.run_state == VM_ST_RUN) {
        g->stepping = true;              /* host port close callbacks are hooks */
        vm_stop(&g->vm);                 /* ports get their close (open files too) */
        g->stepping = false;
    }
    fs_dir_close(g->fs);
    g->fs = NULL;
    blk_free_all(g, &g->code_blocks, g->uh.code_free);
    blk_free_all(g, &g->blocks, g->uh.free);
    if (g->image_copy) g->uh.free(g->uctx, g->image_copy, g->image_bytes);
    g->image_copy = NULL;
    unref(g);
}
