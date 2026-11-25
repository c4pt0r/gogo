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
 * int or int64 becomes a heap copy (words: the room those copies take);
 * any other module value fails the copy with `why`: R_BAD_CODE if its
 * module is gone (stale, whatever the destination), else R_BAD_MSG.
 * Inside one module every live value passes unchanged. */
typedef struct Xm {
    const Vm *vm;
    uint32_t tag, why, words;
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
    if (k == K_STRING || k == K_INT || k == K_INT64) return o;   /* no terms inside */
    xm_fail(x, o ? R_BAD_MSG : R_BAD_CODE);
    return NULL;
}

/* Checks heap object o (closure or struct type word) and counts the const
 * copies its terms need. */
static void xm_obj(Xm *x, const uint32_t *o) {
    uint32_t k = hdr_kind(o[0]), first, end;
    if ((k == K_CLOSURE || (k == K_STRUCT && o[1] >= TYPE_RESERVED_COUNT)) &&
        (o[1] & MOD_TAG_MASK) != x->tag)
        xm_fail(x, module_of_tag(x->vm, o[1]) ? R_BAD_MSG : R_BAD_CODE);
    heap_obj_terms(o, &first, &end);
    for (uint32_t i = first; i < end; i++) {
        const uint32_t *c = xm_const(x, o[i]);
        if (c) x->words += hdr_words(c[0]);
    }
}

/* With set, marks the unmarked objects reachable from root and returns their
 * total words; without, unmarks the marked ones (returns 0). */
static uint32_t walk(uint32_t *mem, term root, bool set, Xm *x) {
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
                    xm_obj(x, o);   /* set: x is never NULL */
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

/* Words a copy of t into module x->tag takes (UINT32_MAX when more than
 * 65535), the const copies included; x->why set if the rules refuse it. */
static uint32_t msg_size(uint32_t *src_mem, term t, Xm *x) {
    uint32_t n = walk(src_mem, t, true, x);
    walk(src_mem, t, false, NULL);
    const uint32_t *c = xm_const(x, t);
    if (c) x->words += hdr_words(c[0]);
    n += x->words;
    return n > MSG_MAX_WORDS ? UINT32_MAX : n;
}

/* A heap copy of const t appended to b if t crosses into x->tag (msg_size
 * counted it): the term to store for t. */
static term xm_put(Buf *b, Xm *x, term t) {
    const uint32_t *c = xm_const(x, t);
    if (!c) return t;
    uint32_t w = hdr_words(c[0]);
    memcpy(b->w + b->len, c, (size_t)w * 4u);
    t = term_from_ref(b->len);
    b->len += w;
    return t;
}

/* After copy_dag: xm_put on every term of the copied objects (from skip)
 * and on the root, which it returns. The const copies hold no terms. */
static term xm_copy(Buf *b, Xm *x, uint32_t skip, term root) {
    uint32_t end = b->len;
    for (uint32_t scan = skip; scan < end; scan += hdr_words(b->w[scan])) {
        uint32_t first, last;
        heap_obj_terms(b->w + scan, &first, &last);
        for (uint32_t i = scan + first; i < scan + last; i++) b->w[i] = xm_put(b, x, b->w[i]);
    }
    return xm_put(b, x, root);
}

/* Sharing-preserving copy. While copying, each source
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

/* Calls fn on every handle term of a message: the root and the term words
 * of its objects in data[skip, words). */
static void each_handle(const uint32_t *data, uint32_t skip, uint32_t words, term root,
                        void (*fn)(term, void *), void *ctx) {
    if (term_is_handle(root)) fn(root, ctx);
    for (uint32_t scan = skip; scan < words; scan += hdr_words(data[scan])) {
        uint32_t first, end;
        heap_obj_terms(data + scan, &first, &end);
        for (uint32_t i = scan + first; i < scan + end; i++) {
            if (term_is_handle(data[i])) fn(data[i], ctx);
        }
    }
}

static uint32_t msg_skip(const Msg *m) { return (m->flags & MSG_REPLY) ? MSG_REPLY_HDR : 0u; }

static void ref_fn(term h, void *ctx) {
    bytes_msg_ref(h);
    *(bool *)ctx = true;
}

static void unref_fn(term h, void *ctx) {
    (void)ctx;
    bytes_msg_unref(h);
}

static void charge_fn(term h, void *ctx) { bytes_set_add(ctx, h); }

uint32_t msg_charge_words(const Msg *m) {
    uint32_t w = msg_footprint_words(m->words);
    if (m->flags & MSG_HANDLES) {
        BytesSet s;
        bytes_set_init(&s);
        each_handle(m->data, msg_skip(m), m->words, m->root, charge_fn, &s);
        w += bytes_words(s.bytes);
    }
    return w;
}

uint32_t msg_footprint_words(uint16_t words) {
    return (uint32_t)((msg_alloc_bytes(words) + 3u) / 4u);
}

/* msg_build with `skip` reserved words (zeroed) before the objects: sizes
 * the copy first (checking the module rules), allocates the message once,
 * then copies into it. */
static Msg *build_skip(const Vm *vm, Actor *src, term t, uint32_t skip, uint32_t dst_mod,
                       uint32_t *why) {
    Xm x = { vm, module_tag(vm, dst_mod), 0, 0 };
    uint32_t n = msg_size(src->mem, t, &x);
    if (why) *why = x.why ? x.why : R_OUT_OF_MEMORY;
    if (x.why || n == UINT32_MAX || n > MSG_MAX_WORDS - skip) return NULL;
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
    m->root = x.words ? xm_copy(&b, &x, skip, root) : root;
    bool any = false;
    each_handle(m->data, skip, words, m->root, ref_fn, &any);
    if (any) m->flags |= MSG_HANDLES;
    return m;
}

Msg *msg_build(const Vm *vm, Actor *src, term t, uint32_t dst_mod, uint32_t *why) {
    return build_skip(vm, src, t, 0, dst_mod, why);
}

void msg_free(Msg *m) {
    if (m->flags & MSG_HANDLES) each_handle(m->data, msg_skip(m), m->words, m->root, unref_fn, NULL);
    va_free(m, msg_alloc_bytes(m->words));
}

typedef struct Adopt { Actor *a; bool ok; } Adopt;

static void adopt_fn(term h, void *ctx) {
    Adopt *d = ctx;
    if (!bytes_adopt(d->a, h)) d->ok = false;
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
    if (m->flags & MSG_HANDLES) {
        Adopt d = { a, true };
        each_handle(m->data, skip, m->words, m->root, adopt_fn, &d);
        if (!d.ok) {                  /* list full: drop what a no longer reaches */
            heap_gc(vm, a, 0, out, 1);
            d.ok = true;
            each_handle(m->data, skip, m->words, m->root, adopt_fn, &d);
            if (!d.ok) return false;
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

static void queue(Vm *vm, Actor *to, Msg *m, uint32_t fw) {
    m->next = NULL;
    if (to->mbox_tail) to->mbox_tail->next = m;
    else to->mbox_head = m;
    to->mbox_tail = m;
    to->mbox_words = (uint16_t)(to->mbox_words + fw);

    if (to->state == A_WAITING) {
        to->state = A_RUNNABLE;
        runq_push(vm, to);
    }
}

bool msg_enqueue(Vm *vm, Actor *to, Msg *m, bool quota) {
    uint32_t fw = msg_charge_words(m);
    if (!fits(to, m, fw, quota)) {
        msg_free(m);
        actor_mark_exit(vm, to, R_MAILBOX_OVERFLOW, T_EMPTYSTR);
        return false;
    }
    queue(vm, to, m, fw);
    return true;
}

bool msg_try_enqueue(Vm *vm, Actor *to, Msg *m) {
    if (to->state == A_EXITED || (to->flags & AF_EXIT_PENDING)) return false;
    uint32_t fw = msg_charge_words(m);
    if (!fits(to, m, fw, true)) return false;
    queue(vm, to, m, fw);
    return true;
}

/* A send that drops (no live target) still checks t, against from's own
 * module as SendAfter does for a dead target: a stale value is the
 * sender's badcode whether or not its receiver lives. 1, or the negated
 * reason. Only the size walk: no allocation. */
static int dropped(const Vm *vm, Actor *from, term t) {
    Xm x = { vm, module_tag(vm, from->mod), 0, 0 };
    msg_size(from->mem, t, &x);
    return x.why ? -(int)x.why : 1;
}

int msg_send(Vm *vm, Actor *from, term to_pid, term msg) {
    Actor *to = actor_lookup(vm, to_pid);
    if (!to || to->state == A_EXITED || (to->flags & AF_EXIT_PENDING)) return dropped(vm, from, msg);

    uint32_t why;
    Msg *m = msg_build(vm, from, msg, to->mod, &why);
    if (!m) return -(int)why;
    msg_enqueue(vm, to, m, true);
    return 0;
}

/* Removes the head message and stops counting it in mbox_words. */
static void drop_head(Actor *a) {
    Msg *m = a->mbox_head;
    a->mbox_words = (uint16_t)(a->mbox_words - msg_charge_words(m));
    a->mbox_head = m->next;
    if (!a->mbox_head) a->mbox_tail = NULL;
    msg_free(m);
}

/* Unpacks queued m into a's heap. Only m's own block stops counting
 * against a's limit first (so it is not counted twice while unpacking).
 * Its off-heap Bytes, and the holder list adopting them may make, stay
 * charged to the mailbox until bytes_adopt has charged them to a: the GC
 * that makes room cannot grow the block into their room, so a never ends
 * a successful receive over its limit. On failure m stays queued. */
static bool take(Vm *vm, Actor *a, Msg *m, term *out) {
    uint32_t fw = msg_charge_words(m), keep = fw - msg_footprint_words(m->words);
    if (m->flags & MSG_HANDLES) keep += bytes_list_words(a);
    a->mbox_words = (uint16_t)(a->mbox_words - fw + keep);
    if (!msg_unpack(vm, a, m, out)) {
        a->mbox_words = (uint16_t)(a->mbox_words + fw - keep);
        return false;
    }
    a->mbox_words = (uint16_t)(a->mbox_words - keep);
    return true;
}

bool msg_receive(Vm *vm, Actor *a, term *out) {
    /* Replies are for Call only; any at the head are stale. */
    while (a->mbox_head && (a->mbox_head->flags & MSG_REPLY)) drop_head(a);
    Msg *m = a->mbox_head;
    if (!m) return false;
    if (!take(vm, a, m, out)) return false;
    a->mbox_head = m->next;
    if (!a->mbox_head) a->mbox_tail = NULL;
    msg_free(m);
    return true;
}

int msg_send_reply(Vm *vm, Actor *from, term to_pid, term ref, term body) {
    Actor *to = actor_lookup(vm, to_pid);
    const uint32_t *r = term_is_ref(ref) ? from->mem + term_ref_off(ref) : NULL;
    if (!to || to->state == A_EXITED || (to->flags & AF_EXIT_PENDING) || !r || hdr_kind(r[0]) != K_REF)
        return dropped(vm, from, body);

    uint32_t why;
    Msg *m = build_skip(vm, from, body, MSG_REPLY_HDR, to->mod, &why);
    if (!m) return -(int)why;
    m->flags |= MSG_REPLY;
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
            if (!take(vm, a, m, out)) return -1;
            /* unpack may GC but never touches the queue; unlink m. */
            if (prev) prev->next = next; else a->mbox_head = next;
            if (!next) a->mbox_tail = prev;
            msg_free(m);
            return 1;
        } else {   /* a stale reply of an earlier, timed-out Call */
            if (prev) prev->next = next; else a->mbox_head = next;
            if (!next) a->mbox_tail = prev;
            a->mbox_words = (uint16_t)(a->mbox_words - msg_charge_words(m));
            msg_free(m);
        }
        m = next;
    }
    return 0;
}
