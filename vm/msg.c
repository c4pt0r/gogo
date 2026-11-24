#include "msg.h"
#include "heap.h"
#include "alloc.h"
#include "signals.h"
#include "opcodes.h"
#include <string.h>

#define MSG_MAX_WORDS 65535u

/* Growable word buffer the copy is built in. */
typedef struct Buf {
    uint32_t *w;
    uint32_t cap, len;
} Buf;

static bool buf_reserve(Buf *b, uint32_t extra) {
    if (extra > MSG_MAX_WORDS - b->len) return false;
    uint32_t need = b->len + extra;
    if (need <= b->cap) return true;
    uint32_t cap = b->cap ? b->cap : 16u;
    while (cap < need) cap *= 2u;
    if (cap > MSG_MAX_WORDS) cap = MSG_MAX_WORDS;
    uint32_t *w = va_alloc((size_t)cap * 4u);
    if (!w) return false;
    if (b->w) {
        memcpy(w, b->w, (size_t)b->len * 4u);
        va_free(b->w, (size_t)b->cap * 4u);
    }
    b->w = w;
    b->cap = cap;
    return true;
}

static void buf_free(Buf *b) {
    if (b->w) va_free(b->w, (size_t)b->cap * 4u);
    b->w = NULL;
}

/* Sharing-preserving copy (spec M2 §5.1.7). While copying, each source
 * object that has been copied gets its header overwritten with a one-word
 * forward marker, hdr_make(offset in b, K_FORWARD), so a second ref to it
 * reuses the copy (this also makes cycles terminate). The marker keeps the
 * source's word 1 intact, and the copy's word 1, which is never a term (see
 * heap_obj_terms), temporarily holds the source offset instead. restore()
 * then walks the copies, puts every original header back and refills word
 * 1, so no side table is needed. This mutates the sender's heap only for
 * the duration of copy_dag; the VM runs actors on a single scheduler
 * thread, and nothing else reads that heap meanwhile. Every heap object is
 * at least 2 words, so word 1 always exists. */

/* Returns a ref into b for the source object t refers to, copying it on
 * first visit; UINT32_MAX on failure. */
static uint32_t copy_obj(Buf *b, uint32_t *src_mem, term t) {
    uint32_t src = term_ref_off(t);
    uint32_t *obj = src_mem + src;
    if (hdr_kind(obj[0]) == K_FORWARD) return term_from_ref(hdr_words(obj[0]));
    uint32_t words = hdr_words(obj[0]);
    if (!buf_reserve(b, words)) return UINT32_MAX;
    uint32_t off = b->len;
    memcpy(b->w + off, obj, (size_t)words * 4u);
    b->len += words;
    b->w[off + 1] = src;
    obj[0] = hdr_make(off, K_FORWARD);
    return term_from_ref(off);
}

/* Undoes copy_obj's marks: every copy in b names its source object. */
static void restore(Buf *b, uint32_t *src_mem, uint32_t first) {
    for (uint32_t scan = first; scan < b->len; scan += hdr_words(b->w[scan])) {
        uint32_t src = b->w[scan + 1];
        src_mem[src] = b->w[scan];
        b->w[scan + 1] = src_mem[src + 1];
    }
}

/* Cheney-style copy of t into b: objects are appended in order, and a scan
 * pointer walks the copies, replacing each source ref by a ref to its copy.
 * No recursion. Sets *root; false on failure. The source heap is restored
 * byte for byte on every path. b already holds `first` words (a reply's
 * serial) before the first object. */
static bool copy_dag(Buf *b, uint32_t *src_mem, term t, term *root, uint32_t skip) {
    bool ok = true;
    *root = t;
    if (term_is_ref(t)) {
        *root = copy_obj(b, src_mem, t);
        ok = *root != UINT32_MAX;
    }
    for (uint32_t scan = skip; ok && scan < b->len; ) {
        uint32_t first, end;
        uint32_t words = hdr_words(b->w[scan]);
        heap_obj_terms(b->w + scan, &first, &end);
        for (uint32_t i = scan + first; ok && i < scan + end; i++) {
            if (!term_is_ref(b->w[i])) continue;
            uint32_t r = copy_obj(b, src_mem, b->w[i]);   /* may move b->w */
            if (r == UINT32_MAX) ok = false;
            else b->w[i] = r;
        }
        scan += words;
    }
    restore(b, src_mem, skip);
    return ok;
}

uint32_t msg_copy_words(Actor *src, term t) {
    Buf b = { NULL, 0, 0 };
    term root;
    bool ok = copy_dag(&b, src->mem, t, &root, 0);
    uint32_t n = b.len;
    buf_free(&b);
    return ok ? n : UINT32_MAX;
}

uint32_t msg_footprint_words(uint16_t words) {
    return (uint32_t)((msg_alloc_bytes(words) + 3u) / 4u);
}

/* msg_build with `skip` reserved words (zeroed) before the objects. */
static Msg *build_skip(Actor *src, term t, uint32_t skip) {
    Buf b = { NULL, 0, 0 };
    term root;
    if (skip) {
        if (!buf_reserve(&b, skip)) return NULL;
        memset(b.w, 0, (size_t)skip * 4u);
        b.len = skip;
    }
    if (!copy_dag(&b, src->mem, t, &root, skip)) {
        buf_free(&b);
        return NULL;
    }
    Msg *m = va_alloc(msg_alloc_bytes((uint16_t)b.len));
    if (m) {
        m->next = NULL;
        m->words = (uint16_t)b.len;
        m->flags = 0;
        m->root = root;
        if (b.len) memcpy(m->data, b.w, (size_t)b.len * 4u);
    }
    buf_free(&b);
    return m;
}

Msg *msg_build(Actor *src, term t) {
    return build_skip(src, t, 0);
}

void msg_free(Msg *m) {
    va_free(m, msg_alloc_bytes(m->words));
}

bool msg_unpack(Vm *vm, Actor *a, const Msg *m, term *out) {
    uint32_t base = 0;
    /* A reply's serial words are not copied; refs relative to data become
     * relative to the heap by adding (base - skip). */
    uint32_t skip = (m->flags & MSG_REPLY) ? MSG_REPLY_HDR : 0u;
    uint32_t n = m->words - skip;
    if (n) {
        base = heap_alloc(vm, a, n, NULL, 0);
        if (base == UINT32_MAX) return false;
        memcpy(a->mem + base, m->data + skip, (size_t)n * 4u);
        /* Add the destination offset to every ref, object by object. */
        for (uint32_t scan = base; scan < base + n; ) {
            uint32_t first, end;
            heap_obj_terms(a->mem + scan, &first, &end);
            for (uint32_t i = scan + first; i < scan + end; i++) {
                if (term_is_ref(a->mem[i])) a->mem[i] += term_from_ref(base) - term_from_ref(skip);
            }
            scan += hdr_words(a->mem[scan]);
        }
    }
    *out = term_is_ref(m->root) ? m->root + term_from_ref(base) - term_from_ref(skip) : m->root;
    return true;
}

bool msg_enqueue(Vm *vm, Actor *to, Msg *m, bool quota) {
    uint32_t fw = msg_footprint_words(m->words);
    if (fw > 65535u - to->mbox_words ||
        (quota && (uint32_t)to->mem_words + actor_charged_words(to) + fw > actor_max_words(to))) {
        msg_free(m);
        actor_mark_exit(vm, to, R_MAILBOX_OVERFLOW, T_EMPTYSTR);
        return false;
    }
    m->next = NULL;
    if (to->mbox_tail) to->mbox_tail->next = m;
    else to->mbox_head = m;
    to->mbox_tail = m;
    to->mbox_words = (uint16_t)(to->mbox_words + fw);

    if (to->state == A_WAITING) {
        to->state = A_RUNNABLE;
        runq_push(vm, to);
    }
    return true;
}

int msg_send(Vm *vm, Actor *from, term to_pid, term msg) {
    Actor *to = actor_lookup(vm, to_pid);
    if (!to || to->state == A_EXITED || (to->flags & AF_EXIT_PENDING)) return 1;

    Msg *m = msg_build(from, msg);
    if (!m) return -1;
    msg_enqueue(vm, to, m, true);
    return 0;
}

/* Removes the head message and stops counting it in mbox_words. */
static void drop_head(Actor *a) {
    Msg *m = a->mbox_head;
    a->mbox_words = (uint16_t)(a->mbox_words - msg_footprint_words(m->words));
    a->mbox_head = m->next;
    if (!a->mbox_head) a->mbox_tail = NULL;
    msg_free(m);
}

bool msg_receive(Vm *vm, Actor *a, term *out) {
    /* Replies are for Call only; any at the head are stale. */
    while (a->mbox_head && (a->mbox_head->flags & MSG_REPLY)) drop_head(a);
    Msg *m = a->mbox_head;
    if (!m) return false;
    /* The message moves from the mailbox into the heap: stop counting it
     * against the limit first, so it is not counted twice while unpacking. */
    uint16_t fw = (uint16_t)msg_footprint_words(m->words);
    a->mbox_words = (uint16_t)(a->mbox_words - fw);
    if (!msg_unpack(vm, a, m, out)) {
        a->mbox_words = (uint16_t)(a->mbox_words + fw);  /* stays queued */
        return false;
    }
    a->mbox_head = m->next;
    if (!a->mbox_head) a->mbox_tail = NULL;
    msg_free(m);
    return true;
}

int msg_send_reply(Vm *vm, Actor *from, term to_pid, term ref, term body) {
    Actor *to = actor_lookup(vm, to_pid);
    if (!to || to->state == A_EXITED || (to->flags & AF_EXIT_PENDING)) return 1;
    if (!term_is_ref(ref)) return 1;
    const uint32_t *r = from->mem + term_ref_off(ref);
    if (hdr_kind(r[0]) != K_REF) return 1;

    Msg *m = build_skip(from, body, MSG_REPLY_HDR);
    if (!m) return -1;
    m->flags = MSG_REPLY;
    m->data[0] = r[1];
    m->data[1] = r[2];
    msg_enqueue(vm, to, m, true);
    return 0;
}

int msg_take_reply(Vm *vm, Actor *a, uint64_t serial, term *out) {
    Msg *prev = NULL, *m = a->mbox_head;
    while (m) {
        Msg *next = m->next;
        if (!(m->flags & MSG_REPLY)) {
            prev = m;
        } else if (((uint64_t)m->data[0] | ((uint64_t)m->data[1] << 32)) == serial) {
            /* As in msg_receive: not counted twice while unpacking. */
            uint16_t fw = (uint16_t)msg_footprint_words(m->words);
            a->mbox_words = (uint16_t)(a->mbox_words - fw);
            if (!msg_unpack(vm, a, m, out)) {
                a->mbox_words = (uint16_t)(a->mbox_words + fw);
                return -1;
            }
            /* unpack may GC but never touches the queue; unlink m. */
            if (prev) prev->next = next; else a->mbox_head = next;
            if (!next) a->mbox_tail = prev;
            msg_free(m);
            return 1;
        } else {   /* a stale reply of an earlier, timed-out Call */
            if (prev) prev->next = next; else a->mbox_head = next;
            if (!next) a->mbox_tail = prev;
            a->mbox_words = (uint16_t)(a->mbox_words - msg_footprint_words(m->words));
            msg_free(m);
        }
        m = next;
    }
    return 0;
}
