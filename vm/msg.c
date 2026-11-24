#include "msg.h"
#include "heap.h"
#include "alloc.h"
#include "signals.h"
#include "opcodes.h"
#include <string.h>

#define MSG_MAX_WORDS 65535u

/* Fixed-size word buffer the copy is built in (the message's data). */
typedef struct Buf {
    uint32_t *w;
    uint32_t cap, len;
} Buf;

/* Size pass. The copy's size is found by walking the reachable objects with
 * a visited bit (WALK_MARK, in the kind field: real kinds are below 32) and
 * no side storage: a depth-first walk by pointer reversal. While the walk is
 * inside an object, the field it descended through holds the way back
 * ((parent offset << 16) | parent field index) and is put back on return;
 * this relies on heap offsets and object sizes fitting 16 bits and on no
 * object at offset 65535, which an object of 2+ words in a 65535-word heap
 * cannot be. Like the copy, it mutates the sender's heap only while it
 * runs and restores it. */
#define WALK_MARK 0x20u
#define WALK_TOP  UINT32_MAX

/* With set, marks the unmarked objects reachable from root and returns their
 * total words; without, unmarks the marked ones (returns 0). */
static uint32_t walk(uint32_t *mem, term root, bool set) {
    uint32_t total = 0;
    if (!term_is_ref(root)) return 0;
    uint32_t cur = term_ref_off(root), up = WALK_TOP, i, end;
    uint32_t *o;
    if (((mem[cur] & WALK_MARK) != 0) == set) return 0;
    goto enter;
    for (;;) {
        while (i < end) {
            term t = o[i];
            if (term_is_ref(t) && ((mem[term_ref_off(t)] & WALK_MARK) != 0) != set) {
                o[i] = up;
                up = (cur << 16) | i;
                cur = term_ref_off(t);
            enter:
                o = mem + cur;
                if (set) {
                    heap_obj_terms(o, &i, &end);
                    total += hdr_words(o[0]);
                    o[0] |= WALK_MARK;
                } else {
                    o[0] &= ~WALK_MARK;
                    heap_obj_terms(o, &i, &end);
                }
                continue;
            }
            i++;
        }
        if (up == WALK_TOP) return total;
        uint32_t child = cur;
        cur = up >> 16;
        i = up & 0xFFFFu;
        o = mem + cur;
        up = o[i];
        o[i] = term_from_ref(child);
        i++;
        end = hdr_words(o[0]);
    }
}

/* Words a copy of t takes (UINT32_MAX when more than 65535). */
static uint32_t msg_size(uint32_t *src_mem, term t) {
    uint32_t n = walk(src_mem, t, true);
    walk(src_mem, t, false);
    return n > MSG_MAX_WORDS ? UINT32_MAX : n;
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
    if (words > b->cap - b->len) return UINT32_MAX;   /* cannot happen: sized by walk */
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
    return msg_size(src->mem, t);
}

uint32_t msg_footprint_words(uint16_t words) {
    return (uint32_t)((msg_alloc_bytes(words) + 3u) / 4u);
}

/* msg_build with `skip` reserved words (zeroed) before the objects: sizes
 * the copy first, allocates the message once, then copies into it. */
static Msg *build_skip(Actor *src, term t, uint32_t skip) {
    uint32_t n = msg_size(src->mem, t);
    if (n == UINT32_MAX || n > MSG_MAX_WORDS - skip) return NULL;
    uint32_t words = skip + n;
    Msg *m = va_alloc(msg_alloc_bytes((uint16_t)words));
    if (!m) return NULL;
    memset(m->data, 0, (size_t)skip * 4u);
    Buf b = { m->data, words, skip };
    term root;
    if (!copy_dag(&b, src->mem, t, &root, skip)) {   /* cannot happen: sized by walk */
        va_free(m, msg_alloc_bytes((uint16_t)words));
        return NULL;
    }
    m->next = NULL;
    m->words = (uint16_t)words;
    m->flags = 0;
    m->root = root;
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
