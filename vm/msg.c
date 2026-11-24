#include "msg.h"
#include "heap.h"
#include "alloc.h"
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

/* Appends a copy of the object t refers to; returns its offset in b, or
 * UINT32_MAX on failure. */
static uint32_t copy_obj(Buf *b, const uint32_t *src_mem, term t) {
    const uint32_t *obj = src_mem + term_ref_off(t);
    uint32_t words = hdr_words(obj[0]);
    if (!buf_reserve(b, words)) return UINT32_MAX;
    uint32_t off = b->len;
    memcpy(b->w + off, obj, (size_t)words * 4u);
    b->len += words;
    return off;
}

/* Cheney-style tree copy of t into b: objects are appended in order, and a
 * scan pointer walks the copies, replacing each source ref by a ref to a
 * fresh copy. No recursion, no marks on the sender's heap, and no sharing
 * (an object reached twice is copied twice). Sets *root; false on failure. */
static bool copy_tree(Buf *b, const uint32_t *src_mem, term t, term *root) {
    *root = t;
    if (term_is_ref(t)) {
        uint32_t off = copy_obj(b, src_mem, t);
        if (off == UINT32_MAX) return false;
        *root = term_from_ref(off);
    }
    for (uint32_t scan = 0; scan < b->len; ) {
        uint32_t first, end;
        uint32_t words = hdr_words(b->w[scan]);
        heap_obj_terms(b->w + scan, &first, &end);
        for (uint32_t i = scan + first; i < scan + end; i++) {
            if (!term_is_ref(b->w[i])) continue;
            uint32_t off = copy_obj(b, src_mem, b->w[i]);   /* may move b->w */
            if (off == UINT32_MAX) return false;
            b->w[i] = term_from_ref(off);
        }
        scan += words;
    }
    return true;
}

uint32_t msg_copy_words(const Actor *src, term t) {
    Buf b = { NULL, 0, 0 };
    term root;
    bool ok = copy_tree(&b, src->mem, t, &root);
    uint32_t n = b.len;
    buf_free(&b);
    return ok ? n : UINT32_MAX;
}

/* Size of a queued message in words, for mbox_words. */
static uint32_t footprint_words(uint16_t words) {
    return (uint32_t)((msg_alloc_bytes(words) + 3u) / 4u);
}

Msg *msg_build(const Actor *src, term t) {
    Buf b = { NULL, 0, 0 };
    term root;
    if (!copy_tree(&b, src->mem, t, &root)) {
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

void msg_free(Msg *m) {
    va_free(m, msg_alloc_bytes(m->words));
}

bool msg_unpack(Vm *vm, Actor *a, const Msg *m, term *out) {
    uint32_t base = 0;
    if (m->words) {
        base = heap_alloc(vm, a, m->words, NULL, 0);
        if (base == UINT32_MAX) return false;
        memcpy(a->mem + base, m->data, (size_t)m->words * 4u);
        /* Add the destination offset to every ref, object by object. */
        for (uint32_t scan = base; scan < base + m->words; ) {
            uint32_t first, end;
            heap_obj_terms(a->mem + scan, &first, &end);
            for (uint32_t i = scan + first; i < scan + end; i++) {
                if (term_is_ref(a->mem[i])) a->mem[i] += term_from_ref(base);
            }
            scan += hdr_words(a->mem[scan]);
        }
    }
    *out = term_is_ref(m->root) ? m->root + term_from_ref(base) : m->root;
    return true;
}

int msg_send(Vm *vm, Actor *from, term to_pid, term msg) {
    Actor *to = actor_lookup(vm, to_pid);
    if (!to || to->state == A_EXITED) return 1;

    Msg *m = msg_build(from, msg);
    if (!m) return -1;
    if (footprint_words(m->words) > 65535u - to->mbox_words) {
        msg_free(m);
        return -2;
    }
    if (to->mbox_tail) to->mbox_tail->next = m;
    else to->mbox_head = m;
    to->mbox_tail = m;
    to->mbox_words = (uint16_t)(to->mbox_words + footprint_words(m->words));

    if (to->state == A_WAITING) {
        to->state = A_RUNNABLE;
        runq_push(vm, to);
    }
    return 0;
}

bool msg_receive(Vm *vm, Actor *a, term *out) {
    Msg *m = a->mbox_head;
    if (!m || !msg_unpack(vm, a, m, out)) return false;
    a->mbox_head = m->next;
    if (!a->mbox_head) a->mbox_tail = NULL;
    a->mbox_words = (uint16_t)(a->mbox_words - footprint_words(m->words));
    msg_free(m);
    return true;
}
