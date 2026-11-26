#include "msg.h"
#include "heap.h"
#include "alloc.h"
#include "signals.h"
#include "opcodes.h"
#include "bytes.h"
#include "module.h"
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

/* The module rules a copy into module tag follows (docs/design.md §5.4.4), applied
 * to every value it reaches. A value naming a module (a const ref, a
 * closure, a struct of a module's own type: term.h) may enter only that
 * module's actors as it is. Crossing into another module, a const string,
 * int, int64 or float becomes a heap copy (words: the room those copies take);
 * any other module value fails the copy with `why`: R_BAD_CODE if its
 * module is gone (stale, whatever the destination), else R_BAD_MSG.
 * Inside one module every live value passes unchanged. */
typedef struct Xm {
    const Vm *vm;
    uint32_t tag, why, words;
    bool handles, fin;        /* a handle seen; a handle or a crossing const (fin_fn has work) */
} Xm;

static void xm_fail(Xm *x, uint32_t why) {
    if (!x->why) x->why = why;
}

/* The const object to copy for t, or NULL: t stays as it is (not a const
 * ref, or one of module tag), or it fails the copy (x->why). */
static const uint32_t *xm_const(Xm *x, term t) {
    if (!term_is_const(t) || (t & MOD_TAG_MASK) == x->tag) return NULL;
    const uint32_t *o = const_obj_of(x->vm, t);
    uint32_t k = o ? hdr_kind(o[0]) : 0u;
    if (k == K_STRING || k == K_INT || k == K_INT64 || k == K_FLOAT) return o;   /* no terms inside */
    xm_fail(x, o ? R_BAD_MSG : R_BAD_CODE);
    return NULL;
}

/* Counts what the term t, not followed as a ref, adds to the copy. */
static void xm_term(Xm *x, term t) {
    const uint32_t *c = xm_const(x, t);
    if (c) {
        x->words += hdr_words(c[0]);
        x->fin = true;
    }
    if (term_is_handle(t)) x->handles = x->fin = true;
}

/* With x, marks the unmarked objects reachable from root and returns their
 * total words, checking each (a closure's or struct's type word) against
 * the module rules and counting what its other terms add (xm_term);
 * without, unmarks the marked ones (returns 0). */
static uint32_t walk(uint32_t *mem, term root, Xm *x) {
    uint32_t total = 0;
    if (!term_is_ref(root)) return 0;
    uint32_t cur = term_ref_off(root), up = WALK_TOP, i, end;
    uint32_t *o;
    if (((mem[cur] & WALK_MARK) != 0) == (x != NULL)) return 0;
    goto enter;
    for (;;) {
        while (i < end) {
            term t = o[i];
            if (term_is_ref(t) && ((mem[term_ref_off(t)] & WALK_MARK) != 0) != (x != NULL)) {
                o[i] = up;
                up = (cur << 16) | i;
                cur = term_ref_off(t);
            enter:
                o = mem + cur;
                o[0] &= ~WALK_MARK;   /* unmarking (marking: it is not marked) */
                heap_obj_terms(o, &i, &end);
                if (x) {
                    uint32_t k = hdr_kind(o[0]);
                    if ((k == K_CLOSURE || (k == K_STRUCT && o[1] >= TYPE_OWN_BASE)) &&
                        (o[1] & MOD_TAG_MASK) != x->tag)
                        xm_fail(x, module_of_tag(x->vm, o[1]) ? R_BAD_MSG : R_BAD_CODE);
                    total += hdr_words(o[0]);
                    o[0] |= WALK_MARK;
                }
                continue;
            }
            if (x) xm_term(x, t);
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

/* Words a copy of t into module x->tag takes (UINT32_MAX when more than
 * 65535), the const copies included; x->why set if the rules refuse it.
 * Leaves the reachable objects marked (WALK_MARK): the copy clears the
 * marks (copy_fn, restore); a caller that does not copy unmarks them with
 * walk(mem, t, NULL), by the same pointer reversal, so a send that fails
 * or drops costs the size of the message, not of the sender's heap. */
static uint32_t msg_size(uint32_t *src_mem, term t, Xm *x) {
    uint32_t n = walk(src_mem, t, x);
    xm_term(x, t);
    n += x->words;
    return n > MSG_MAX_WORDS ? UINT32_MAX : n;
}

/* The copy being built: b, the sender's heap, the module rules. */
typedef struct Build {
    Buf b;
    uint32_t *src;
    Xm *x;
    Vm *vm;
} Build;

/* Sharing-preserving copy. While copying, each source
 * object that has been copied gets its header overwritten with a one-word
 * forward marker, hdr_make(offset in b, K_FORWARD), so a second ref to it
 * reuses the copy (this also makes cycles terminate). The marker keeps the
 * source's word 1 intact, and the copy's word 1, which is never a term (see
 * heap_obj_terms), temporarily holds the source offset instead. restore()
 * then walks the copies, puts every original header back and refills word
 * 1, so no side table is needed. This mutates the sender's heap only for
 * the duration of the copy; the VM runs actors on a single scheduler
 * thread, and nothing else reads that heap meanwhile. Every heap object is
 * at least 2 words, so word 1 always exists. */

/* Replaces the source ref at p by a ref into b, copying the object on
 * first visit; the copy's header loses msg_size's mark, and restore puts
 * that header back in the source. */
static void copy_fn(void *ctx, term *p) {
    Build *c = ctx;
    Buf *b = &c->b;
    uint32_t src = term_ref_off(*p);
    uint32_t *obj = c->src + src;
    uint32_t off = hdr_words(obj[0]);
    if (hdr_kind(obj[0]) != K_FORWARD) {
        uint32_t words = off;
        GOGO_ASSERT(words <= b->cap - b->len);   /* sized by msg_size */
        off = b->len;
        memcpy(b->w + off, obj, (size_t)words * 4u);
        b->len += words;
        b->w[off] &= ~WALK_MARK;
        b->w[off + 1] = src;
        obj[0] = hdr_make(off, K_FORWARD);
    }
    *p = term_from_ref(off);
}

/* Undoes copy_fn's marks: every copy in b names its source object. */
static void restore(Buf *b, uint32_t *src_mem, uint32_t first) {
    for (uint32_t scan = first; scan < b->len; scan += hdr_words(b->w[scan])) {
        uint32_t src = b->w[scan + 1];
        src_mem[src] = b->w[scan];
        b->w[scan + 1] = src_mem[src + 1];
    }
}

/* After the copy, on the root and every term of the copied objects: a
 * heap copy of const t appended to b if t crosses into x->tag (msg_size
 * counted it; the const copies hold no terms), and one reference taken
 * per handle. */
static void fin_fn(void *ctx, term *p) {
    Build *c = ctx;
    const uint32_t *k = xm_const(c->x, *p);
    if (k) {
        uint32_t w = hdr_words(k[0]);
        memcpy(c->b.w + c->b.len, k, (size_t)w * 4u);
        *p = term_from_ref(c->b.len);
        c->b.len += w;
    }
    if (term_is_handle(*p)) bytes_msg_ref(c->vm, *p);
}

static uint32_t msg_skip(const Msg *m) { return (m->flags & MSG_REPLY) ? MSG_REPLY_HDR : 0u; }

/* Calls fn on every handle term of a message: the root and the term words
 * of its objects (fn does not write them). */
static void each_handle(const Msg *m, void (*fn)(void *, term *), void *ctx) {
    uint32_t end = m->words;
    term root = m->root;
    heap_map_terms((uint32_t *)(uintptr_t)m->data, msg_skip(m), &end, &root, 1, TAGS_HANDLE, fn, ctx);
}

static void unref_fn(void *ctx, term *h) { bytes_msg_unref(ctx, *h); }

typedef struct Charge {
    const Vm *vm;
    BytesSet s;
} Charge;

static void charge_fn(void *ctx, term *h) {
    Charge *c = ctx;
    bytes_set_add(c->vm, &c->s, *h);
}

uint32_t msg_charge_words(Vm *vm, const Msg *m) {
    uint32_t w = msg_footprint_words(m->words);
    if (m->flags & MSG_HANDLES) {
        Charge c;
        c.vm = vm;
        bytes_set_init(&c.s);
        each_handle(m, charge_fn, &c);
        w += bytes_words(c.s.bytes);
    }
    return w;
}

/* msg_build with `skip` reserved words (zeroed) before the objects: sizes
 * the copy first (checking the module rules), allocates the message once,
 * then copies into it: a Cheney copy (objects appended in order, a scan
 * replacing each source ref by a ref to its copy, no recursion), then the
 * const copies and handle references. The source heap is restored byte
 * for byte on every path. */
static Msg *build_skip(Vm *vm, Actor *src, term t, uint32_t skip, uint32_t dst_mod,
                       uint32_t *why) {
    Xm x = { vm, module_tag(vm, dst_mod), 0, 0, false, false };
    uint32_t n = msg_size(src->mem, t, &x);
    if (why) *why = x.why ? x.why : R_OUT_OF_MEMORY;
    uint32_t words = skip + n;
    Msg *m = NULL;
    if (!x.why && n <= MSG_MAX_WORDS - skip) m = va_alloc(vm, msg_alloc_bytes((uint16_t)words));
    if (!m) {
        walk(src->mem, t, NULL);      /* no copy follows: unmark */
        return NULL;
    }
    memset(m->data, 0, (size_t)skip * 4u);
    Build c = { { m->data, words, skip }, src->mem, &x, vm };
    m->root = t;
    heap_map_terms(m->data, skip, &c.b.len, &m->root, 1, TAGS_REF, copy_fn, &c);
    restore(&c.b, src->mem, skip);
    if (x.fin) heap_map_terms(m->data, skip, &c.b.len, &m->root, 1, TAGS_CONST | TAGS_HANDLE, fin_fn, &c);
    m->next = NULL;
    m->words = (uint16_t)words;
    m->flags = x.handles ? MSG_HANDLES : 0u;
    return m;
}

Msg *msg_build(Vm *vm, Actor *src, term t, uint32_t dst_mod, uint32_t *why) {
    return build_skip(vm, src, t, 0, dst_mod, why);
}

void msg_free(Vm *vm, Msg *m) {
    if (m->flags & MSG_HANDLES) each_handle(m, unref_fn, vm);
    va_free(vm, m, msg_alloc_bytes(m->words));
}

typedef struct Adopt {
    Vm *vm;
    Actor *a;
    bool ok;
} Adopt;

static void adopt_fn(void *ctx, term *h) {
    Adopt *d = ctx;
    if (!bytes_adopt(d->vm, d->a, *h)) d->ok = false;
}

static void reloc_fn(void *ctx, term *t) { *t += *(const uint32_t *)ctx; }

bool msg_unpack(Vm *vm, Actor *a, const Msg *m, term *out) {
    uint32_t base = 0;
    /* A reply's serial words are not copied; refs relative to data become
     * relative to the heap by adding (base - skip). */
    uint32_t skip = msg_skip(m);
    uint32_t n = m->words - skip;
    if (n) {
        base = heap_alloc(vm, a, n, NULL, 0);
        if (base == UINT32_MAX) return false;
        memcpy(a->mem + base, m->data + skip, (size_t)n * 4u);
    }
    /* Add the destination offset to every ref, object by object. */
    uint32_t end = base + n, delta = term_from_ref(base) - term_from_ref(skip);
    *out = m->root;
    heap_map_terms(a->mem, base, &end, out, 1, TAGS_REF, reloc_fn, &delta);
    if (m->flags & MSG_HANDLES) {
        Adopt d = { vm, a, true };
        for (bool again = false;; again = true) {
            each_handle(m, adopt_fn, &d);
            if (d.ok) break;
            if (again) return false;
            heap_gc(vm, a, 0, out, 1);    /* list full: drop what a no longer reaches */
            d.ok = true;
        }
    }
    return true;
}

/* True if `to` can queue m (charged fw words): its mailbox counter can
 * hold it and, with quota, its limit too. */
static bool fits(const Actor *to, const Msg *m, uint32_t fw, bool quota) {
    /* Receiving the first handle makes the holder list, charged to `to`:
     * count it now (not queued: mbox_words stays fw). */
    uint32_t list = (m->flags & MSG_HANDLES) ? bytes_list_words(to) : 0u;
    return fw <= 65535u - to->mbox_words &&
           (!quota || (uint32_t)to->mem_words + actor_charged_words(to) + fw + list <= actor_max_words(to));
}

/* Queues m on `to` (charged as in mbox_words) and wakes it, if it fits:
 * the mailbox counter, and with quota the limit. */
static bool enqueue(Vm *vm, Actor *to, Msg *m, bool quota) {
    uint32_t fw = msg_charge_words(vm, m);
    if (!fits(to, m, fw, quota)) return false;
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

bool msg_enqueue(Vm *vm, Actor *to, Msg *m, bool quota) {
    if (enqueue(vm, to, m, quota)) return true;
    msg_free(vm, m);
    actor_mark_exit(vm, to, R_MAILBOX_OVERFLOW, T_EMPTYSTR);
    return false;
}

bool msg_try_enqueue(Vm *vm, Actor *to, Msg *m) {
    return to->state != A_EXITED && !(to->flags & AF_EXIT_PENDING) && enqueue(vm, to, m, true);
}

/* A send that drops (no live target) still checks t, against from's own
 * module as SendAfter does for a dead target: a stale value is the
 * sender's badcode whether or not its receiver lives. 1, or the negated
 * reason. Only the size walk: no allocation. */
static int dropped(const Vm *vm, Actor *from, term t) {
    Xm x = { vm, module_tag(vm, from->mod), 0, 0, false, false };
    msg_size(from->mem, t, &x);
    walk(from->mem, t, NULL);
    return x.why ? -(int)x.why : 1;
}

/* msg_send (reply false) and msg_send_reply (reply true: a reply block
 * carrying serial; serial 0 drops). A PID naming no local actor that is a
 * remote one goes to the distribution hook. */
static int send_to(Vm *vm, Actor *from, term to_pid, uint64_t serial, bool reply, term msg) {
    Actor *to = actor_lookup_live(vm, to_pid);
    const VmDist *d;
    if (!to && (serial || !reply) && (d = vm_dist_of(vm, to_pid))) return d->send(vm, from, to_pid, serial, msg);
    if (!to || (reply && !serial))
        return dropped(vm, from, msg);

    uint32_t why;
    Msg *m = build_skip(vm, from, msg, reply ? MSG_REPLY_HDR : 0u, to->mod, &why);
    if (!m) return -(int)why;
    if (reply) {
        m->flags |= MSG_REPLY;
        m->data[0] = (uint32_t)serial;
        m->data[1] = (uint32_t)(serial >> 32);
    }
    msg_enqueue(vm, to, m, true);
    return 0;
}

int msg_send(Vm *vm, Actor *from, term to_pid, term msg) {
    return send_to(vm, from, to_pid, 0, false, msg);
}

/* Unpacks queued m into a's heap. Only m's own block stops counting
 * against a's limit first (so it is not counted twice while unpacking).
 * Its off-heap Bytes, and the holder list adopting them may make, stay
 * charged to the mailbox until bytes_adopt has charged them to a: the GC
 * that makes room cannot grow the block into their room, so a never ends
 * a successful receive over its limit. On failure m stays queued. */
static bool take(Vm *vm, Actor *a, Msg *m, term *out) {
    uint32_t fw = msg_charge_words(vm, m), keep = fw - msg_footprint_words(m->words);
    if (m->flags & MSG_HANDLES) keep += bytes_list_words(a);
    GOGO_ASSERT(a->mbox_words >= fw && a->mbox_words - fw + keep <= 65535u);
    a->mbox_words = (uint16_t)(a->mbox_words - fw + keep);
    if (!msg_unpack(vm, a, m, out)) {
        a->mbox_words = (uint16_t)(a->mbox_words + fw - keep);
        return false;
    }
    a->mbox_words = (uint16_t)(a->mbox_words - keep);
    return true;
}

bool msg_receive(Vm *vm, Actor *a, term *out) { return msg_take_reply(vm, a, 0, out) > 0; }

int msg_send_reply(Vm *vm, Actor *from, term to_pid, uint64_t serial, term body) {
    return send_to(vm, from, to_pid, serial, true, body);
}

/* Serial 0 (msg_receive): the oldest ordinary message; the replies before
 * it are stale (replies are for Call only) and are freed. Otherwise the
 * reply with that serial; ordinary messages stay, the other replies met
 * on the way are stale (an earlier, timed-out Call) and are freed. */
int msg_take_reply(Vm *vm, Actor *a, uint64_t serial, term *out) {
    Msg *prev = NULL, *m = a->mbox_head;
    int got = 0;
    while (m && !got) {
        Msg *next = m->next;
        if (!(m->flags & MSG_REPLY)) {
            if (serial) {
                prev = m;
                m = next;
                continue;
            }
            got = 1;
        } else if (serial && ((uint64_t)m->data[0] | ((uint64_t)m->data[1] << 32)) == serial) {
            got = (m->flags & MSG_FAILED) ? 2 : 1;
        }
        if (got == 1) {
            if (!take(vm, a, m, out)) return -1;   /* unpack may GC but never touches the queue */
        } else {                      /* a stale reply, or the failed Call's block */
            GOGO_ASSERT(a->mbox_words >= msg_charge_words(vm, m));
            a->mbox_words = (uint16_t)(a->mbox_words - msg_charge_words(vm, m));
        }
        if (prev) prev->next = next; else a->mbox_head = next;
        if (!next) a->mbox_tail = prev;
        msg_free(vm, m);
        m = next;
    }
    return got;
}
