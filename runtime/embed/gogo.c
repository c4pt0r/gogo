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

typedef char caps_match[(GOGO_CAP_NET == CAP_NET && GOGO_CAP_KBD == CAP_KBD && GOGO_CAP_POWER == CAP_POWER &&
                         GOGO_CAP_SD == CAP_SD && GOGO_CAP_IMU == CAP_IMU && GOGO_CAP_IR_TX == CAP_IR_TX &&
                         GOGO_CAP_IR_RX == CAP_IR_RX && GOGO_CAP_MIC == CAP_MIC && GOGO_CAP_SPK == CAP_SPK &&
                         GOGO_CAP_TLS == CAP_TLS) ? 1 : -1];
typedef char steps_match[(GOGO_RUNNABLE == VM_RUNNABLE && GOGO_IDLE == VM_IDLE && GOGO_DONE == VM_DONE) ? 1 : -1];

/* No board hardware: ports, input and network come from the host. */
static const VmBoard embed_board = { NULL, NULL, NULL, NULL, 0 };

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

static void f_write(void *c, int s, const char *p, size_t n) { gogo_vm *g = c; g->uh->write(g->uctx, s, p, n); }
static void *f_alloc(void *c, size_t n) { gogo_vm *g = c; return blk_alloc(g, &g->blocks, g->uh->alloc, n); }
static void f_free(void *c, void *p, size_t n) { (void)n; gogo_vm *g = c; blk_free(g, &g->blocks, g->uh->free, p); }
static uint64_t f_now(void *c) { gogo_vm *g = c; return g->uh->now_ms(g->uctx); }
static void f_wake(void *c) { gogo_vm *g = c; g->uh->wake(g->uctx); }
static void f_wait(void *c, uint32_t ms) { gogo_vm *g = c; g->uh->wait(g->uctx, ms); }
static int64_t f_unix(void *c) { gogo_vm *g = c; return g->uh->unix_time_ms(g->uctx); }
static int f_set_unix(void *c, int64_t ms) { gogo_vm *g = c; return g->uh->set_unix_time_ms(g->uctx, ms); }
static uint32_t f_random(void *c) { gogo_vm *g = c; return g->uh->random(g->uctx); }
static int f_random_bytes(void *c, uint8_t *b, size_t n) { gogo_vm *g = c; return g->uh->random_bytes(g->uctx, b, n); }
static int32_t f_config(void *c, const uint8_t *k, uint32_t kl, uint8_t *b, uint32_t cap) {
    gogo_vm *g = c;
    return g->uh->config_get(g->uctx, k, kl, b, cap);
}
static void *f_code_alloc(void *c, size_t n) { gogo_vm *g = c; return blk_alloc(g, &g->code_blocks, g->uh->code_alloc, n); }
static void f_code_free(void *c, void *p, size_t n) {
    (void)n;
    gogo_vm *g = c;
    blk_free(g, &g->code_blocks, g->uh->code_free, p);
}

static void make_fwd(gogo_vm *g) {
    const gogo_host *u = g->uh;
    gogo_host *f = &g->fwd;
    f->write = f_write;
    f->alloc = f_alloc;
    f->free = f_free;
    f->now_ms = f_now;
    f->wake = u->wake ? f_wake : NULL;
    f->wait = u->wait ? f_wait : NULL;
    f->unix_time_ms = u->unix_time_ms ? f_unix : NULL;
    f->set_unix_time_ms = u->set_unix_time_ms ? f_set_unix : NULL;
    f->random = u->random ? f_random : NULL;
    f->random_bytes = u->random_bytes ? f_random_bytes : NULL;
    f->config_get = u->config_get ? f_config : NULL;
    f->code_alloc = u->code_alloc ? f_code_alloc : NULL;
    f->code_free = u->code_free ? f_code_free : NULL;
}

/* ---- lifecycle ---- */

static bool host_ok(const gogo_host *h) {
    return h && h->write && h->alloc && h->free && h->now_ms && (h->wake || !h->wait) &&
           !h->unix_time_ms == !h->set_unix_time_ms && !h->code_alloc == !h->code_free;
}

/* vm_init with the config, then the memory cap (vm_init clears the
 * allocator) and the inbox. */
static int vm_setup(gogo_vm *g) {
    uint16_t cap = g->cfg.max_actors ? g->cfg.max_actors : (uint16_t)GOGO_TABLE_CAP;
    if (vm_init(&g->vm, &g->fwd, g, &embed_board, g->cfg.caps, cap) != 0) return -1;
    va_set_limit(&g->vm, g->cfg.mem_limit);
    vfs_init(&g->vm, &g->vfs);       /* no fs_root: no mounts, every gogo/fs call is NotMounted */
    embed_ports_install(g);
    return port_add_source(&g->vm, &inbox_source, g) ? 0 : -1;
}

gogo_vm *gogo_new(const gogo_host *h, void *host_ctx, const gogo_config *cfg) {
    static const gogo_config zero_cfg;
    if (!host_ok(h)) return NULL;
    if (!cfg) cfg = &zero_cfg;
    /* Refuse what this build cannot give, rather than mask it. */
    if ((cfg->caps & ~embed_board.caps) || cfg->fs_quota < 0) return NULL;
    gogo_vm *g = h->alloc(host_ctx, sizeof *g);
    if (!g) return NULL;
    memset(g, 0, sizeof *g);
    g->uh = h;
    g->uctx = host_ctx;
    g->cfg = *cfg;
    g->refs = 1;
    make_fwd(g);
    /* /flash needs no capability: every board has it. fs_quota 0 is no
     * quota (fs_dir: -1). */
    if (cfg->fs_root && (!(g->fs = fs_dir_open(h, host_ctx, cfg->fs_root, cfg->fs_quota ? cfg->fs_quota : -1)) ||
                         !vfs_mount(&g->vfs, "/flash", fs_dir_vfs(g->fs), g->fs))) {
        fs_dir_close(g->fs);
        h->free(host_ctx, g, sizeof *g);
        return NULL;
    }
    g->cfg.fs_root = NULL;           /* the caller's string is not kept */
    if (vm_setup(g) != 0) {
        blk_free_all(g, &g->blocks, h->free);
        fs_dir_close(g->fs);
        h->free(host_ctx, g, sizeof *g);
        return NULL;
    }
    return g;
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
        if (!(g->image_copy = g->uh->alloc(g->uctx, len))) return GOGO_ENOMEM;
        memcpy(g->image_copy, image, len);
        g->image_bytes = len;
        words = g->image_copy;
    }
    int rc = vm_boot(&g->vm, words, (uint32_t)(len / 4u));
    if (rc == VM_BOOT_OK) {
        g->loaded = true;
        if (!g->cfg.allow_code_load) g->vm.mod_cap = 0;   /* every code.Load is NoMemory */
        return GOGO_OK;
    }
    if (rc < 0) {
        static const char pre[] = "gogo: bad image: ";
        const char *why = image_err_str(rc);
        g->uh->write(g->uctx, 2, pre, sizeof pre - 1u);
        g->uh->write(g->uctx, 2, why, strlen(why));
        g->uh->write(g->uctx, 2, "\n", 1u);
    }
    if (g->image_copy) {
        g->uh->free(g->uctx, g->image_copy, g->image_bytes);
        g->image_copy = NULL;
    }
    /* Start over with a clean Vm (vm_boot gave its table back). */
    blk_free_all(g, &g->blocks, g->uh->free);
    if (vm_setup(g) != 0) g->vm.table = NULL;
    return rc < 0 ? GOGO_EIMAGE : rc == VM_BOOT_REFUSED ? GOGO_ECAPS : GOGO_ENOMEM;
}

int gogo_start(gogo_vm *g) {
    if (!g) return GOGO_EINVAL;
    if (g->stepping) return GOGO_EBUSY;
    if (!g->loaded) return GOGO_ESTATE;
    if (g->vm.run_state != VM_ST_NEW) return GOGO_ESTARTED;
    return vm_start(&g->vm) ? GOGO_ESTART : GOGO_OK;
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
        inbox_clear(&g->inbox, g->uh, g->uctx);
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
    if (!g || !g->uh->wait) return GOGO_EINVAL;
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
            g->uh->wait(g->uctx, UINT32_MAX);
        } else {
            uint64_t now = g->uh->now_ms(g->uctx);
            if ((uint64_t)next > now) {
                uint64_t d = (uint64_t)next - now;
                g->uh->wait(g->uctx, d > UINT32_MAX - 1u ? UINT32_MAX - 1u : (uint32_t)d);
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
        r = inbox_push(&g->inbox, g->uh, g->uctx, name, nl, port, kind, data, len);
        if (r == 0 && g->uh->wake) g->uh->wake(g->uctx);
    }
    __atomic_fetch_sub(&g->posting, 1, __ATOMIC_RELEASE);
    return r;
}

int gogo_post(gogo_vm *g, const char *name, int32_t kind, const void *data, size_t len) {
    if (!g || !name || (len && !data)) return GOGO_EINVAL;
    size_t nl = strlen(name);
    if (nl > GOGO_POST_NAME_MAX || len > GOGO_POST_DATA_MAX) return GOGO_E2BIG;
    return post(g, name, nl, 0, kind, data, len);
}

int gogo_port_post(gogo_vm *g, gogo_port port, int32_t kind, const void *data, size_t len) {
    if (!g || (len && !data) || port.index >= GOGO_MAX_PORTS) return GOGO_EINVAL;
    if (len > GOGO_POST_DATA_MAX) return GOGO_E2BIG;
    return post(g, "", 0, embed_port_enc(port), kind, data, len);
}

void gogo_close_posts(gogo_vm *g) {
    if (!g) return;
    /* A VM idle only because a name kept it alive re-checks and ends
     * (once: a later close changes nothing). */
    if (embed_set_state(g, EMBED_CLOSED) < EMBED_CLOSED && g->uh->wake) g->uh->wake(g->uctx);
}

void gogo_retain(gogo_vm *g) { __atomic_fetch_add(&g->refs, 1, __ATOMIC_RELAXED); }

void gogo_release(gogo_vm *g) {
    if (__atomic_fetch_sub(&g->refs, 1, __ATOMIC_ACQ_REL) != 1) return;
    const gogo_host *h = g->uh;
    void *ctx = g->uctx;
    void (*ctx_free)(void *) = g->ctx_free;
    h->free(ctx, g, sizeof *g);
    if (ctx_free) ctx_free(ctx);
}

void gogo_get_stats(gogo_vm *g, gogo_stats *out) {
    if (!g || !out) return;
    out->posts_delivered = g->inbox.delivered;
    out->posts_dropped = g->inbox.dropped;
    out->mem_used = va_used_bytes(&g->vm);
    out->mem_peak = va_peak_bytes(&g->vm);
}

void gogo_free(gogo_vm *g) {
    if (!g) return;
    embed_set_state(g, EMBED_DEAD);
    if (g->thread) embed_thread_stop(g);
    /* Posts in progress finish quickly (a copy and a push); later ones see
     * the state and leave the inbox alone. The read is seq_cst: the
     * handshake (gogo_post). After a while, let a preempted poster run. */
    for (unsigned spins = 0; __atomic_load_n(&g->posting, __ATOMIC_SEQ_CST) != 0; spins++) {
        if (spins >= 1000u && g->uh->wait) g->uh->wait(g->uctx, 1);
    }
    inbox_clear(&g->inbox, g->uh, g->uctx);
    if (g->vm.run_state == VM_ST_RUN) {
        g->stepping = true;              /* host port close callbacks are hooks */
        vm_stop(&g->vm);                 /* ports get their close (open files too) */
        g->stepping = false;
    }
    fs_dir_close(g->fs);
    g->fs = NULL;
    blk_free_all(g, &g->code_blocks, g->uh->code_free);
    blk_free_all(g, &g->blocks, g->uh->free);
    if (g->image_copy) g->uh->free(g->uctx, g->image_copy, g->image_bytes);
    g->image_copy = NULL;
    gogo_release(g);
}
