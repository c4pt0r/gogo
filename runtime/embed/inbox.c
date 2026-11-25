/* The post inbox (inbox.h). */
#include "embed.h"
#include "heap.h"
#include "msg.h"
#include "names.h"
#include "opcodes.h"
#include <string.h>

int inbox_push(Inbox *in, const gogo_host *h, void *hctx, const char *name, size_t name_len,
               int32_t kind, const void *data, size_t len) {
    if (__atomic_fetch_add(&in->depth, 1, __ATOMIC_RELAXED) >= GOGO_POST_MAX) {
        __atomic_fetch_sub(&in->depth, 1, __ATOMIC_RELAXED);
        return GOGO_EFULL;
    }
    size_t bytes = sizeof(InboxNode) + name_len + len;
    InboxNode *n = h->alloc(hctx, bytes);
    if (!n) {
        __atomic_fetch_sub(&in->depth, 1, __ATOMIC_RELAXED);
        return GOGO_ENOMEM;
    }
    n->bytes = bytes;
    n->kind = kind;
    n->name_len = (uint32_t)name_len;
    n->data_len = (uint32_t)len;
    memcpy(n->text, name, name_len);
    if (len) memcpy(n->text + name_len, data, len);
    InboxNode *old = __atomic_load_n(&in->head, __ATOMIC_RELAXED);
    do {
        n->next = old;
    } while (!__atomic_compare_exchange_n(&in->head, &old, n, true, __ATOMIC_RELEASE, __ATOMIC_RELAXED));
    return 0;
}

/* Takes every queued node, oldest first. */
static InboxNode *take(Inbox *in) {
    InboxNode *n = __atomic_exchange_n(&in->head, NULL, __ATOMIC_ACQUIRE), *r = NULL;
    int k = 0;
    while (n) {
        InboxNode *next = n->next;
        n->next = r;
        r = n;
        n = next;
        k++;
    }
    if (k) __atomic_fetch_sub(&in->depth, k, __ATOMIC_RELAXED);
    return r;
}

void inbox_clear(Inbox *in, const gogo_host *h, void *hctx) {
    for (InboxNode *n = take(in), *next; n; n = next) {
        next = n->next;
        h->free(hctx, n, n->bytes);
    }
}

/* An int field: immediate, or a K_INT box at d[*at]. */
static term put_int(uint32_t *d, uint32_t *at, int32_t v) {
    if (term_int_fits(v)) return term_from_int(v);
    d[*at] = hdr_make(2, K_INT);
    d[*at + 1u] = (uint32_t)v;
    term t = term_from_ref(*at);
    *at += 2u;
    return t;
}

/* port.Post{Kind, Data} as one message block: the struct, the Kind box if
 * it needs one, then Data as a K_BYTES (none when empty). The data is
 * inline: the receiver gets an ordinary Bytes on its heap, charged to its
 * mailbox until it receives it. NULL if memory is out (gogo_post keeps
 * the data within one block). */
static Msg *post_msg(Vm *vm, const InboxNode *n) {
    uint32_t len = n->data_len;
    uint32_t words = 4u + (term_int_fits(n->kind) ? 0u : 2u) + (len ? 2u + (len + 3u) / 4u : 0u);
    Msg *m = va_alloc(vm, msg_alloc_bytes((uint16_t)words));
    if (!m) return NULL;
    m->next = NULL;
    m->words = (uint16_t)words;
    m->flags = 0;
    m->root = term_from_ref(0);
    uint32_t *d = m->data, at = 4;
    d[0] = hdr_make(4, K_STRUCT);
    d[1] = TYPE_POST;
    d[2] = put_int(d, &at, n->kind);
    if (len) {
        uint32_t bw = 2u + (len + 3u) / 4u;
        d[at] = hdr_make(bw, K_BYTES);
        d[at + 1u] = len;
        d[at + bw - 1u] = 0;            /* padding */
        memcpy(&d[at + 2u], n->text + n->name_len, len);
        d[3] = term_from_ref(at);
    } else {
        d[3] = T_EMPTYBYTES;
    }
    return m;
}

/* Delivers one post; false if it was dropped. */
static bool deliver(Vm *vm, const InboxNode *n) {
    term pid;
    if (!names_whereis(vm, n->text, n->name_len, &pid)) return false;
    Actor *to = actor_lookup(vm, pid);
    Msg *m = to ? post_msg(vm, n) : NULL;
    if (!m) return false;
    /* Like a port event: never marks the receiver; once more after
     * collecting a waiting receiver's heap slack. */
    for (int k = 0; k < 2; k++) {
        if (msg_try_enqueue(vm, to, m)) return true;
        if (k || !heap_reclaim_waiting(vm, to)) break;
    }
    msg_free(vm, m);
    return false;
}

static bool src_pending(const Vm *vm, void *ctx) {
    (void)vm;
    gogo_vm *g = ctx;
    return __atomic_load_n(&g->inbox.head, __ATOMIC_ACQUIRE) != NULL;
}

static void src_drain(Vm *vm, void *ctx, uint64_t now) {
    (void)now;
    gogo_vm *g = ctx;
    for (InboxNode *n = take(&g->inbox), *next; n; n = next) {
        next = n->next;
        if (deliver(vm, n)) g->inbox.delivered++;
        else g->inbox.dropped++;
        g->uh->free(g->uctx, n, n->bytes);
    }
}

/* Live while a post is queued or some actor holds a name a post can reach. */
static bool src_any_live(const Vm *vm, void *ctx) {
    return src_pending(vm, ctx) || vm->names != NULL;
}

const PortSource inbox_source = { src_pending, src_drain, src_any_live, NULL, NULL };
