/* Posts and posters (gogo.h): gogo_post, gogo_close_posts and the
 * gogo_poster calls, over an EmbedPorts (ports.h), so libgogo and a
 * board with GOGO_BOARD_PORT drivers share them. A post is pushed on the
 * inbox (inbox.c) from any thread and wakes the host through the
 * EmbedPorts' table; the VM thread delivers it at its next step. */
#include "ports.h"
#include <string.h>

/* The free/post handshake is a store-buffering pair: a post writes
 * `posting` then reads `state`; gogo_free writes `state` then reads
 * `posting`. Only if all four accesses are seq_cst can both sides not read
 * the old value (with an acquire read, arm64 may satisfy the load before
 * its earlier store is visible). Either the post sees the close and backs
 * off, or the free sees the post in progress and waits for it. */
int embed_set_state(EmbedPorts *g, int s) {
    int old = __atomic_load_n(&g->state, __ATOMIC_SEQ_CST);
    while (old < s && !__atomic_compare_exchange_n(&g->state, &old, s, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
    }
    return old;
}

/* Queues a post for the name (port 0) or the host port, and wakes. */
static int post(EmbedPorts *g, const char *name, size_t nl, uint32_t port, int32_t kind, const void *data, size_t len) {
    __atomic_fetch_add(&g->posting, 1, __ATOMIC_SEQ_CST);   /* handshake: seq_cst (above) */
    int r = GOGO_ECLOSED;
    if (__atomic_load_n(&g->state, __ATOMIC_SEQ_CST) == EMBED_LIVE) {
        r = inbox_push(&g->inbox, g->uh, g->uctx, name, nl, port, kind, data, len);
        if (r == 0 && g->uh->wake) g->uh->wake(g->uctx);
    }
    __atomic_fetch_sub(&g->posting, 1, __ATOMIC_RELEASE);
    return r;
}

static int post_name(EmbedPorts *g, const char *name, size_t nl, int32_t kind, const void *data, size_t len) {
    if (!g || !name || (len && !data)) return GOGO_EINVAL;
    if (nl > GOGO_POST_NAME_MAX || len > GOGO_POST_DATA_MAX) return GOGO_E2BIG;
    return post(g, name, nl, 0, kind, data, len);
}

int gogo_post(gogo_vm *g, const char *name, size_t name_len, int32_t kind, const void *data, size_t len) {
    return post_name(g ? embed_ports(g) : NULL, name, name_len, kind, data, len);
}

static void close_posts(EmbedPorts *g) {
    /* On a board, many drivers share one shell: one of them closing posts
     * would stop every other's, for good. Only the run's end closes them. */
    if (!g || g->board) return;
    /* A VM idle only because a name kept it alive re-checks and ends
     * (once: a later close changes nothing). */
    if (embed_set_state(g, EMBED_CLOSED) < EMBED_CLOSED && g->uh->wake) g->uh->wake(g->uctx);
}

void gogo_close_posts(gogo_vm *g) { close_posts(g ? embed_ports(g) : NULL); }

void embed_unref(EmbedPorts *g) {
    int old = __atomic_fetch_sub(&g->refs, 1, __ATOMIC_ACQ_REL);
    GOGO_ASSERT(old > 0);
    if (old == 1 && g->last_ref) g->last_ref(g);
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
    EmbedPorts *ep;
};

/* The poster's ports, or NULL for no poster or a freed one. */
static EmbedPorts *ports_of(gogo_poster *p) {
    if (!p) return NULL;
    bool live = __atomic_load_n(&p->state, __ATOMIC_ACQUIRE) == POSTER_LIVE;
    GOGO_ASSERT(live);
    return live ? p->ep : NULL;
}

gogo_poster *gogo_poster_new(gogo_vm *vm) {
    if (!vm) return NULL;
    EmbedPorts *g = embed_ports(vm);
    gogo_poster *p = g->uh->alloc(g->uctx, sizeof *p);
    if (!p) return NULL;
    p->ep = g;
    __atomic_store_n(&p->state, POSTER_LIVE, __ATOMIC_RELEASE);
    __atomic_fetch_add(&g->refs, 1, __ATOMIC_RELAXED);   /* the caller holds the owner's */
    return p;
}

int gogo_poster_post(gogo_poster *p, const char *name, size_t name_len, int32_t kind, const void *data, size_t len) {
    return post_name(ports_of(p), name, name_len, kind, data, len);
}

int gogo_poster_port_post(gogo_poster *p, gogo_port port, int32_t kind, const void *data, size_t len) {
    EmbedPorts *g = ports_of(p);
    if (!g || (len && !data) || !embed_port_valid(port)) return GOGO_EINVAL;
    if (len > GOGO_POST_DATA_MAX) return GOGO_E2BIG;
    return post(g, "", 0, port, kind, data, len);
}

void gogo_poster_close_posts(gogo_poster *p) { close_posts(ports_of(p)); }

void gogo_poster_free(gogo_poster *p) {
    if (!p) return;
    uint32_t live = POSTER_LIVE;
    bool first = __atomic_compare_exchange_n(&p->state, &live, POSTER_DEAD, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    GOGO_ASSERT(first);          /* freed twice */
    if (!first) return;
    EmbedPorts *g = p->ep;
    g->uh->free(g->uctx, p, sizeof *p);   /* the VM's reference keeps uh and uctx */
    embed_unref(g);
}
