/* The "node" port: distributed actors over a link actor's TCP connection
 * (dist.h, docs/design.md §5.13). */
#include "dist.h"
#include "actor.h"
#include "heap.h"
#include "msg.h"
#include "alloc.h"
#include "signals.h"
#include "opcodes.h"
#include "bytes.h"
#include "module.h"
#include "out.h"
#include "host.h"
#include "stream.h"
#include <string.h>

/* Frame types. */
enum {
    F_SEND = 1, F_REPLY = 2, F_LINK = 3, F_UNLINK = 4, F_EXIT = 5,
    F_MONITOR = 6, F_DEMONITOR = 7, F_DOWN = 8, F_NODEDOWN = 9, F_GONE = 10
};
#define HDR 6u              /* type, flags, dst, src */

/* Term tags. */
enum {
    DT_NIL = 0x00, DT_FALSE = 0x01, DT_TRUE = 0x02, DT_INT = 0x03, DT_INT64 = 0x04,
    DT_STRING = 0x05, DT_BYTES = 0x06, DT_BYTESLICE = 0x07, DT_SLICE = 0x08,
    DT_NILSLICE = 0x09, DT_STRUCT = 0x0A, DT_PID = 0x0B, DT_ZEROPID = 0x0C,
    DT_REF = 0x0D, DT_ZEROREF = 0x0E, DT_EMPTYSTR = 0x0F, DT_EMPTYBYTES = 0x10,
    DT_OWN = 0x11            /* a module's own struct: field count, qualified name, fields */
};

#define SERIAL_BITS 48u
#define SERIAL_MASK ((UINT64_C(1) << SERIAL_BITS) - 1u)
#define WHY_DROP 0xFFFFu    /* encoder: drop the message (a full export table) */

/* Watch kinds. */
enum { W_FREE = 0, W_MON_OUT = 1, W_MON_IN = 2, W_LINK = 3 };

typedef struct Link {
    Port *port;              /* NULL: free */
    uint16_t peer;           /* the peer's node id */
    uint8_t relay;           /* the route to every node without a link of its own */
    uint8_t hb_n;            /* bytes of a split length prefix in hb */
    uint8_t hb[2];
    uint16_t rx_need, rx_have;   /* a frame body being reassembled in rx */
    uint8_t *rx;
    uint32_t seq;            /* Seq of the last Chunk made */
} Link;

typedef struct Proxy {
    uint16_t node;           /* 0: free */
    uint16_t gen;            /* the generation of the entry's PID terms */
    term remote;             /* the PID on its node (T_ZEROPID: the node's agent) */
    uint16_t used;           /* DistState.tick at its last use (eviction) */
    uint16_t pad;
} Proxy;

/* A remote link or monitor. MON_OUT: local monitors a remote PID (peer, a
 * proxy); MON_IN: remote watcher (peer, its term on node) monitors local;
 * LINK: local and the remote PID peer (a proxy) are linked. */
typedef struct Watch {
    uint8_t kind;
    uint8_t pad;
    uint16_t node;
    term local, peer;
    uint64_t serial;         /* monitors: MON_OUT the local Ref's, MON_IN the watcher's */
} Watch;

typedef struct DistState {
    VmDist hooks;            /* first: vm->dist points here */
    uint16_t self;           /* this node's id */
    uint16_t tick;           /* proxy use clock (least recently used goes first) */
    term agent;              /* the local actor a SEND to PID 0 reaches */
    DistStats st;
    Link links[DIST_LINKS];
    Proxy proxies[DIST_PROXIES];
    term exports[DIST_EXPORTS];
    Watch watches[DIST_WATCHES];
} DistState;

static const uint8_t nfields[(TYPE_RESERVED_COUNT + 1u) / 2u] = TYPE_NFIELDS;

static uint32_t reserved_nf(uint32_t t) { return (nfields[t / 2u] >> ((t & 1u) * 4u)) & 15u; }

static DistState *state(const Vm *vm) { return (DistState *)(uintptr_t)vm->dist; }

const DistStats *dist_stats(const Vm *vm) { return vm->dist ? &state(vm)->st : NULL; }

/* ---- little-endian fields ---- */

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32; }
static uint8_t *wr16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); return p + 2; }
static uint8_t *wr32(uint8_t *p, uint32_t v) { p = wr16(p, v & 0xFFFFu); return wr16(p, v >> 16); }
static uint8_t *wr64(uint8_t *p, uint64_t v) { p = wr32(p, (uint32_t)v); return wr32(p, (uint32_t)(v >> 32)); }

/* The name of type-table row r of img (a K_STRING in its const area). */
static const uint8_t *type_name(const Image *img, uint32_t r, uint32_t *len) {
    uint32_t off = img->types[2u * r + 1u];
    if (off >= img->consts_words) return NULL;
    const uint32_t *c = img->consts + off;
    uint32_t words = hdr_words(c[0]);
    if (hdr_kind(c[0]) != K_STRING || words < 2u || words > img->consts_words - off || c[1] > (words - 2u) * 4u)
        return NULL;
    *len = c[1];
    return (const uint8_t *)(c + 2);
}

/* ---- proxies and exports ---- */

static Proxy *proxy_of(DistState *ds, term pid) {
    if (!term_is_proxy(pid)) return NULL;
    uint32_t i = term_pid_slot(pid) - DIST_SLOT_BASE;
    if (i >= DIST_PROXIES) return NULL;
    Proxy *x = &ds->proxies[i];
    return x->node && x->gen == term_pid_gen(pid) ? x : NULL;
}

static term proxy_term(const DistState *ds, const Proxy *x) {
    return term_make_pid((uint16_t)(DIST_SLOT_BASE + (uint32_t)(x - ds->proxies)), x->gen);
}

/* Frees entry x: its PID terms name nothing from now on (a send to one
 * is dropped, as to a dead local PID). */
static void proxy_free(Proxy *x) {
    x->node = 0;
    x->gen = (uint16_t)((x->gen + 1u) & 0xFFFu);
}

/* True if a remote link or monitor names proxy p. */
static bool proxy_watched(const DistState *ds, term p) {
    for (uint32_t i = 0; i < DIST_WATCHES; i++) {
        const Watch *w = &ds->watches[i];
        if ((w->kind == W_MON_OUT || w->kind == W_LINK) && w->peer == p) return true;
    }
    return false;
}

/* The proxy of (node, remote), made if new. A full table gives up its
 * least recently used entry that no link or monitor names and that is no
 * node's agent; 0 if there is none. */
static term proxy_intern(DistState *ds, uint16_t node, term remote) {
    Proxy *free = NULL, *old = NULL;
    for (uint32_t i = 0; i < DIST_PROXIES; i++) {
        Proxy *x = &ds->proxies[i];
        if (x->node == node && x->remote == remote) {
            x->used = ++ds->tick;
            return proxy_term(ds, x);
        }
        if (!x->node && !free) free = x;
    }
    for (uint32_t i = 0; !free && i < DIST_PROXIES; i++) {
        Proxy *x = &ds->proxies[i];
        if (x->remote == T_ZEROPID || proxy_watched(ds, proxy_term(ds, x))) continue;
        if (!old || (uint16_t)(ds->tick - x->used) > (uint16_t)(ds->tick - old->used)) old = x;
    }
    if (!free && old) {
        proxy_free(old);
        free = old;
    }
    if (!free) return 0;
    free->node = node;
    free->remote = remote;
    free->used = ++ds->tick;
    return proxy_term(ds, free);
}

static term proxy_find(DistState *ds, uint16_t node, term remote) {
    for (uint32_t i = 0; i < DIST_PROXIES; i++) {
        Proxy *x = &ds->proxies[i];
        if (x->node == node && x->remote == remote) return proxy_term(ds, x);
    }
    return 0;
}

/* Forgets node's proxies: their terms name nothing from now on. */
static void proxies_drop(DistState *ds, uint16_t node) {
    for (uint32_t i = 0; i < DIST_PROXIES; i++) {
        Proxy *x = &ds->proxies[i];
        if (x->node == node) proxy_free(x);
    }
}

static bool exported(const DistState *ds, term pid) {
    for (uint32_t i = 0; i < DIST_EXPORTS; i++) {
        if (ds->exports[i] == pid) return true;
    }
    return false;
}

/* Records pid as given out; false if the table is full of live actors. */
static bool export_add(Vm *vm, DistState *ds, term pid) {
    term *free = NULL;
    for (uint32_t i = 0; i < DIST_EXPORTS; i++) {
        term *e = &ds->exports[i];
        if (*e == pid) return true;
        if (!free && (!*e || !actor_lookup(vm, *e))) free = e;
    }
    if (!free) return false;
    *free = pid;
    return true;
}

/* ---- links and routes ---- */

static Link *link_of(DistState *ds, const Port *p) {
    for (uint32_t i = 0; i < DIST_LINKS; i++) {
        if (ds->links[i].port == p) return &ds->links[i];
    }
    return NULL;
}

/* The link frames for node take: its own, else the relay; NULL: none. */
static Link *route(DistState *ds, uint16_t node) {
    Link *relay = NULL;
    for (uint32_t i = 0; i < DIST_LINKS; i++) {
        Link *l = &ds->links[i];
        if (!l->port) continue;
        if (l->peer == node) return l;
        if (l->relay) relay = l;
    }
    return relay;
}

/* A Chunk of n frame bytes for l's owner, the bytes left for the caller
 * to write at *data; NULL if it cannot be made. */
static Msg *chunk_new(Vm *vm, Link *l, uint32_t n, uint8_t **data) {
    l->seq = l->seq >= 0x7FFFFFFFu ? 1u : l->seq + 1u;
    return stream_chunk_new(vm, l->port->id, l->seq, (uint32_t)vm_now(vm), 0, n, data);
}

/* Queues m on l's owner; counted as sent, or dropped (m freed). */
static void chunk_send(Vm *vm, DistState *ds, Link *l, Msg *m) {
    Actor *o = port_owner(vm, l->port);
    if (o && msg_try_enqueue(vm, o, m)) {
        ds->st.sent++;
        return;
    }
    msg_free(vm, m);
    ds->st.dropped++;
}

/* Sends the frame b[0..n) (length prefix included) on l. */
static void emit(Vm *vm, DistState *ds, Link *l, const uint8_t *b, uint32_t n) {
    uint8_t *d;
    Msg *m = l ? chunk_new(vm, l, n, &d) : NULL;
    if (!m) {
        ds->st.dropped++;
        return;
    }
    memcpy(d, b, n);
    chunk_send(vm, ds, l, m);
}

/* A control frame (no term) to node: type, then the words w[0..nw) as u32
 * (an u64 is two words, low first). */
static void control(Vm *vm, DistState *ds, uint16_t node, uint8_t type, const uint32_t *w, uint32_t nw) {
    uint8_t b[2u + HDR + 6u * 4u], *p = b;
    p = wr16(p, HDR + nw * 4u);
    *p++ = type;
    *p++ = 0;
    p = wr16(p, node);
    p = wr16(p, ds->self);
    for (uint32_t i = 0; i < nw; i++) p = wr32(p, w[i]);
    emit(vm, ds, route(ds, node), b, (uint32_t)(p - b));
}

/* GONE{pid} on l, as from node src: the actor pid of src exited, so its
 * proxies can go. */
static void gone_to(Vm *vm, DistState *ds, Link *l, uint16_t src, term pid) {
    uint8_t b[2u + HDR + 4u], *p = b;
    p = wr16(p, HDR + 4u);
    *p++ = F_GONE;
    *p++ = 0;
    p = wr16(p, l->peer);
    p = wr16(p, src);
    wr32(p, pid);
    emit(vm, ds, l, b, sizeof b);
}

/* ---- local deliveries ---- */

/* A reason code from the wire: one that is no small int (no reason any
 * VM makes) is taken as NoConnection. */
static uint32_t wire_code(uint32_t code) {
    return term_int_fits((int32_t)code) ? code : R_NO_CONNECTION;
}

/* Reason{code, ""} at d[at..at+4). */
static void put_reason(uint32_t *d, uint32_t at, uint32_t code) {
    d[at] = hdr_make(4, K_STRUCT);
    d[at + 1u] = TYPE_REASON;
    d[at + 2u] = term_from_int((int32_t)wire_code(code));
    d[at + 3u] = T_EMPTYSTR;
}

static Msg *note_new(Vm *vm, uint32_t words) {
    Msg *m = va_alloc(vm, msg_alloc_bytes((uint16_t)words));
    if (!m) return NULL;
    m->next = NULL;
    m->words = (uint16_t)words;
    m->flags = 0;
    m->root = term_from_ref(0);
    return m;
}

/* Queues a notification outside w's quota (as the VM's own link and
 * monitor notes); false only when out of memory. */
static bool note_send(Vm *vm, Actor *w, Msg *m) {
    if (!m) return false;
    if (w->state == A_EXITED || (w->flags & AF_EXIT_PENDING)) msg_free(vm, m);
    else msg_enqueue(vm, w, m, false);
    return true;
}

/* DownMsg{Ref: serial, From: from, Reason{code}} to w. */
static bool down_to(Vm *vm, Actor *w, uint64_t serial, term from, uint32_t code) {
    Msg *m = note_new(vm, 12);
    if (m) {
        uint32_t *d = m->data;
        d[0] = hdr_make(5, K_STRUCT);
        d[1] = TYPE_DOWNMSG;
        d[2] = term_from_ref(9);
        d[3] = from;
        d[4] = term_from_ref(5);
        put_reason(d, 5, code);
        d[9] = hdr_make(3, K_REF);
        d[10] = (uint32_t)serial;
        d[11] = (uint32_t)(serial >> 32);
    }
    return note_send(vm, w, m);
}

/* A link's exit signal `code` from `from` reaching t, as the VM's own link
 * notes: an ExitMsg if t traps exits (whatever the code: Killed is
 * untrappable only when aimed at t itself, by Kill), else t exits with
 * code unless it is Normal. */
static bool exit_to(Vm *vm, Actor *t, term from, uint32_t code) {
    code = wire_code(code);
    if (t->state == A_EXITED || (t->flags & AF_EXIT_PENDING)) return true;
    if (t->flags & AF_TRAP_EXIT) {
        Msg *m = note_new(vm, 8);
        if (m) {
            uint32_t *d = m->data;
            d[0] = hdr_make(4, K_STRUCT);
            d[1] = TYPE_EXITMSG;
            d[2] = from;
            d[3] = term_from_ref(4);
            put_reason(d, 4, code);
        }
        return note_send(vm, t, m);
    }
    if (code != R_NORMAL) actor_mark_exit(vm, t, code, T_EMPTYSTR);
    return true;
}

/* ---- the encoder: a term of actor a's heap into wire bytes ---- */

typedef struct Enc {
    Vm *vm;
    const Actor *a;
    DistState *ds;
    uint8_t *p;              /* NULL: only count */
    uint32_t n, why;
} Enc;

static void put(Enc *e, uint32_t b) {
    if (e->p) e->p[e->n] = (uint8_t)b;
    e->n++;
}

static void put_var(Enc *e, uint64_t v) {
    while (v >= 0x80u) {
        put(e, (uint32_t)(v & 0x7Fu) | 0x80u);
        v >>= 7;
    }
    put(e, (uint32_t)v);
}

static void put_raw(Enc *e, const uint8_t *b, uint32_t n) {
    if (e->p && n) memcpy(e->p + e->n, b, n);
    e->n += n;
}

static void put_u32(Enc *e, uint32_t v) {
    uint8_t b[4];
    wr32(b, v);
    put_raw(e, b, 4);
}

static void put_blob(Enc *e, uint32_t tag, const uint8_t *b, uint32_t n) {
    put(e, tag);
    put_var(e, n);
    put_raw(e, b, n);
}

static void enc_fail(Enc *e, uint32_t why) {
    if (!e->why) e->why = why;
}

/* A term read out of object c: a const object's const refs take c's tag. */
static term field(term c, term f) {
    return (term_is_const(c) && term_is_const(f)) ? f | (c & MOD_TAG_MASK) : f;
}

static void enc_pid(Enc *e, term t) {
    uint16_t slot = term_pid_slot(t);
    if (term_is_proxy(t)) {
        Proxy *x = proxy_of(e->ds, t);
        if (!x) {                       /* a PID of a node gone: names nothing */
            put(e, DT_ZEROPID);
            return;
        }
        put(e, DT_PID);
        put_var(e, x->node);
        put_u32(e, x->remote);
        return;
    }
    if (slot >= e->vm->table_cap) {     /* a port: it cannot leave the node */
        enc_fail(e, R_BAD_MSG);
        return;
    }
    if (e->p && !export_add(e->vm, e->ds, t)) {
        enc_fail(e, WHY_DROP);
        return;
    }
    put(e, DT_PID);
    put_var(e, e->ds->self);
    put_u32(e, t);
}

static void enc(Enc *e, term t, uint32_t depth) {
    if (e->why) return;
    if (depth > DIST_DEPTH || e->n > DIST_FRAME_MAX) {
        enc_fail(e, R_BAD_MSG);
        return;
    }
    if (term_is_int(t)) {
        int32_t v = term_to_int(t);
        put(e, DT_INT);
        put_var(e, ((uint32_t)v << 1) ^ (uint32_t)(v >> 31));
        return;
    }
    switch (t) {
    case T_NIL: put(e, DT_NIL); return;
    case T_FALSE: put(e, DT_FALSE); return;
    case T_TRUE: put(e, DT_TRUE); return;
    case T_NILSLICE: put(e, DT_NILSLICE); return;
    case T_ZEROPID: put(e, DT_ZEROPID); return;
    case T_ZEROREF: put(e, DT_ZEROREF); return;
    case T_EMPTYSTR: put(e, DT_EMPTYSTR); return;
    case T_EMPTYBYTES: put(e, DT_EMPTYBYTES); return;
    default: break;
    }
    if (term_is_pid(t)) {
        enc_pid(e, t);
        return;
    }
    const uint8_t *bp;
    uint32_t blen;
    if (term_is_handle(t)) {
        if (bytes_view(e->vm, e->a, t, &bp, &blen)) put_blob(e, DT_BYTES, bp, blen);
        else enc_fail(e, R_BAD_CODE);
        return;
    }
    const uint32_t *o = obj_of(e->vm, e->a, t);
    if (!o) {
        enc_fail(e, term_is_const(t) ? R_BAD_CODE : R_BAD_MSG);
        return;
    }
    uint32_t words = hdr_words(o[0]);
    switch (hdr_kind(o[0])) {
    case K_INT: {
        int32_t v = (int32_t)o[1];
        put(e, DT_INT);
        put_var(e, ((uint32_t)v << 1) ^ (uint32_t)(v >> 31));
        return;
    }
    case K_INT64: {
        int64_t v = (int64_t)((uint64_t)o[1] | (uint64_t)o[2] << 32);
        put(e, DT_INT64);
        put_var(e, ((uint64_t)v << 1) ^ (uint64_t)(v >> 63));
        return;
    }
    case K_STRING:
    case K_BYTES:
        if (o[1] > (words - 2u) * 4u) break;
        put_blob(e, hdr_kind(o[0]) == K_STRING ? DT_STRING : DT_BYTES, (const uint8_t *)(o + 2), o[1]);
        return;
    case K_REF: {
        uint64_t s = (uint64_t)o[1] | (uint64_t)o[2] << 32;
        uint64_t node = s >> SERIAL_BITS;
        put(e, DT_REF);
        put_var(e, node ? node : e->ds->self);
        put_var(e, s & SERIAL_MASK);
        return;
    }
    case K_STRUCT: {
        uint32_t ty = o[1];
        if (ty >= TYPE_OWN_BASE) {      /* a module's own type: by its name */
            if (term_is_const(t)) ty |= t & MOD_TAG_MASK;
            const ModEntry *me = module_of_tag(e->vm, ty);
            uint32_t row = me ? image_type_row(&me->img, ty & 0xFFFFu) : 0u, nl = 0;
            const uint8_t *nm = me && row < me->img.n_types ? type_name(&me->img, row, &nl) : NULL;
            if (!nm) {
                enc_fail(e, R_BAD_CODE);
                return;
            }
            put(e, DT_OWN);
            put_var(e, words - 2u);
            put_var(e, nl);
            put_raw(e, nm, nl);
        } else {
            if (ty >= TYPE_RESERVED_COUNT) break;
            put(e, DT_STRUCT);
            put_var(e, ty);
            put_var(e, words - 2u);
        }
        for (uint32_t i = 2; i < words; i++) enc(e, field(t, o[i]), depth + 1u);
        return;
    }
    case K_SLICE: {
        if (words != 5u) break;
        uint32_t off = o[1], len = o[2];
        term at = field(t, o[4]);
        const uint32_t *arr = at == T_NIL ? NULL : obj_of(e->vm, e->a, at);
        if (!arr) {
            if (len) break;
            put(e, DT_SLICE);
            put_var(e, 0);
            return;
        }
        if (off > arr[1] || len > arr[1] - off) break;
        if (hdr_kind(arr[0]) == K_BYTEARR) {
            put_blob(e, DT_BYTESLICE, (const uint8_t *)(arr + 2) + off, len);
            return;
        }
        if (hdr_kind(arr[0]) != K_ARRAY) break;
        put(e, DT_SLICE);
        put_var(e, len);
        for (uint32_t i = 0; i < len && !e->why; i++) enc(e, field(at, arr[2u + off + i]), depth + 1u);
        return;
    }
    case K_CLOSURE:
        enc_fail(e, module_of_tag(e->vm, o[1]) ? R_BAD_MSG : R_BAD_CODE);
        return;
    default:
        break;
    }
    enc_fail(e, R_BAD_MSG);   /* a Buffer, or a malformed object */
}

/* ---- the decoder: wire bytes into a message block ---- */

typedef struct Dec {
    Vm *vm;
    DistState *ds;
    const uint8_t *p;
    uint32_t n, at;          /* the term bytes and the read position */
    uint32_t *w;             /* NULL: only size (words) */
    uint32_t words;          /* words used (from skip) */
    bool bad;
    const Image *img;        /* the receiver's module: own types are its */
    uint32_t tag;            /* and its tag */
} Dec;

static uint32_t get8(Dec *d) {
    if (d->at >= d->n) {
        d->bad = true;
        return 0;
    }
    return d->p[d->at++];
}

/* A LEB128 varint of at most `bits` bits (1..64): a group that would set
 * a bit at or above `bits`, or more groups than `bits` needs, is bad. */
static uint64_t get_var(Dec *d, uint32_t bits) {
    uint64_t v = 0;
    for (uint32_t s = 0; s < bits; s += 7u) {
        uint32_t b = get8(d), g = b & 0x7Fu;
        if (bits - s < 7u && (g >> (bits - s))) break;
        v |= (uint64_t)g << s;
        if (!(b & 0x80u)) return v;
    }
    d->bad = true;
    return 0;
}

static uint32_t get_u32(Dec *d) {
    if (d->n - d->at < 4u || d->at > d->n) {
        d->bad = true;
        return 0;
    }
    d->at += 4u;
    return rd32(d->p + d->at - 4u);
}

/* Room for an object of n words: its offset (from the block's start). */
static uint32_t obj(Dec *d, uint32_t n) {
    uint32_t off = d->words;
    if (n > 0xFFFFu - d->words) {
        d->bad = true;
        return 0;
    }
    d->words += n;
    return off;
}

/* A K_STRING / K_BYTES / K_BYTEARR of len bytes from the input. */
static term blob(Dec *d, uint32_t kind, uint32_t len) {
    if (len > d->n - d->at) {
        d->bad = true;
        return T_NIL;
    }
    uint32_t n = 2u + (len + 3u) / 4u, off = obj(d, n);
    if (d->w && !d->bad) {
        d->w[off] = hdr_make(n, kind);
        d->w[off + 1u] = len;
        d->w[off + n - 1u] = 0;          /* padding */
        memcpy(&d->w[off + 2u], d->p + d->at, len);
    }
    d->at += len;
    return term_from_ref(off);
}

static term slice_hdr(Dec *d, uint32_t len, term arr) {
    uint32_t off = obj(d, 5);
    if (d->w && !d->bad) {
        d->w[off] = hdr_make(5, K_SLICE);
        d->w[off + 1u] = 0;
        d->w[off + 2u] = len;
        d->w[off + 3u] = len;
        d->w[off + 4u] = arr;
    }
    return term_from_ref(off);
}

static term dec(Dec *d, uint32_t depth) {
    if (depth > DIST_DEPTH) d->bad = true;
    if (d->bad) return T_NIL;
    uint32_t tag = get8(d);
    switch (tag) {
    case DT_NIL: return T_NIL;
    case DT_FALSE: return T_FALSE;
    case DT_TRUE: return T_TRUE;
    case DT_NILSLICE: return T_NILSLICE;
    case DT_ZEROPID: return T_ZEROPID;
    case DT_ZEROREF: return T_ZEROREF;
    case DT_EMPTYSTR: return T_EMPTYSTR;
    case DT_EMPTYBYTES: return T_EMPTYBYTES;
    case DT_INT: {
        uint32_t z = (uint32_t)get_var(d, 32);
        int32_t v = (int32_t)((z >> 1) ^ (0u - (z & 1u)));
        if (term_int_fits(v)) return term_from_int(v);
        uint32_t off = obj(d, 2);
        if (d->w && !d->bad) {
            d->w[off] = hdr_make(2, K_INT);
            d->w[off + 1u] = (uint32_t)v;
        }
        return term_from_ref(off);
    }
    case DT_INT64: {
        uint64_t z = get_var(d, 64);
        uint64_t v = (z >> 1) ^ (0u - (z & 1u));
        uint32_t off = obj(d, 3);
        if (d->w && !d->bad) {
            d->w[off] = hdr_make(3, K_INT64);
            d->w[off + 1u] = (uint32_t)v;
            d->w[off + 2u] = (uint32_t)(v >> 32);
        }
        return term_from_ref(off);
    }
    case DT_STRING:
    case DT_BYTES: {
        uint32_t len = (uint32_t)get_var(d, 32);
        if (!len) return tag == DT_STRING ? T_EMPTYSTR : T_EMPTYBYTES;
        return blob(d, tag == DT_STRING ? K_STRING : K_BYTES, len);
    }
    case DT_BYTESLICE: {
        uint32_t len = (uint32_t)get_var(d, 32);
        term h = slice_hdr(d, len, T_NIL);
        if (!len) return h;
        term arr = blob(d, K_BYTEARR, len);
        if (d->w && !d->bad) d->w[term_ref_off(h) + 4u] = arr;
        return h;
    }
    case DT_SLICE: {
        uint32_t len = (uint32_t)get_var(d, 32);
        if (len > d->n - d->at) d->bad = true;   /* every element takes a byte */
        term h = slice_hdr(d, len, T_NIL);
        if (!len || d->bad) return h;
        uint32_t off = obj(d, 2u + len);
        if (d->w && !d->bad) {
            d->w[off] = hdr_make(2u + len, K_ARRAY);
            d->w[off + 1u] = len;
            d->w[term_ref_off(h) + 4u] = term_from_ref(off);
        }
        for (uint32_t i = 0; i < len && !d->bad; i++) {
            term v = dec(d, depth + 1u);
            if (d->w && !d->bad) d->w[off + 2u + i] = v;
        }
        return h;
    }
    case DT_STRUCT: {
        uint32_t ty = (uint32_t)get_var(d, 32), nf = (uint32_t)get_var(d, 32);
        /* A reserved type with exactly the fields this VM knows. */
        if (d->bad || ty >= TYPE_RESERVED_COUNT || nf != reserved_nf(ty)) {
            d->bad = true;
            return T_NIL;
        }
        uint32_t off = obj(d, 2u + nf);
        if (d->w && !d->bad) {
            d->w[off] = hdr_make(2u + nf, K_STRUCT);
            d->w[off + 1u] = ty;
        }
        for (uint32_t i = 0; i < nf && !d->bad; i++) {
            term v = dec(d, depth + 1u);
            if (d->w && !d->bad) d->w[off + 2u + i] = v;
        }
        return term_from_ref(off);
    }
    case DT_OWN: {
        /* An own type of the receiver's module with this name and field count. */
        uint32_t nf = (uint32_t)get_var(d, 32), nl = (uint32_t)get_var(d, 32), ty = 0;
        if (d->bad || nl > d->n - d->at) {
            d->bad = true;
            return T_NIL;
        }
        const Image *img = d->img;
        for (uint32_t r = img->n_reserved; r < img->n_types && !ty; r++) {
            uint32_t len;
            const uint8_t *nm = type_name(img, r, &len);
            if (nm && len == nl && image_type_nfields(img, r) == nf && memcmp(nm, d->p + d->at, nl) == 0)
                ty = (TYPE_OWN_BASE + r - img->n_reserved) | d->tag;
        }
        d->at += nl;
        if (!ty) {
            d->bad = true;
            return T_NIL;
        }
        uint32_t off = obj(d, 2u + nf);
        if (d->w && !d->bad) {
            d->w[off] = hdr_make(2u + nf, K_STRUCT);
            d->w[off + 1u] = ty;
        }
        for (uint32_t i = 0; i < nf && !d->bad; i++) {
            term v = dec(d, depth + 1u);
            if (d->w && !d->bad) d->w[off + 2u + i] = v;
        }
        return term_from_ref(off);
    }
    case DT_PID: {
        uint16_t node = (uint16_t)get_var(d, 16);
        term t = get_u32(d);
        if (d->bad || !node || !term_is_pid(t) || term_pid_slot(t) >= DIST_SLOT_BASE) {
            d->bad = true;
            return T_NIL;
        }
        if (node == d->ds->self) {
            if (term_pid_slot(t) >= d->vm->table_cap) d->bad = true;
            return t;
        }
        /* A node this one has no route to is refused; the proxy is made
         * only when writing, after the whole term was checked, so a bad
         * message takes no slot. */
        if (!route(d->ds, node)) {
            d->bad = true;
            return T_NIL;
        }
        if (!d->w) return T_NIL;
        t = proxy_intern(d->ds, node, t);
        if (!t) d->bad = true;
        return t;
    }
    case DT_REF: {
        uint64_t node = get_var(d, 16), s = get_var(d, SERIAL_BITS);
        if (!node || !s) d->bad = true;
        if (node != d->ds->self) s |= node << SERIAL_BITS;
        uint32_t off = obj(d, 3);
        if (d->w && !d->bad) {
            d->w[off] = hdr_make(3, K_REF);
            d->w[off + 1u] = (uint32_t)s;
            d->w[off + 2u] = (uint32_t)(s >> 32);
        }
        return term_from_ref(off);
    }
    default:
        d->bad = true;
        return T_NIL;
    }
}

/* Delivers the term p[0..n) to `to` (a reply block when reply); a
 * malformed term is counted and dropped. */
static void deliver(Vm *vm, DistState *ds, Actor *to, const uint8_t *p, uint32_t n, bool reply, uint64_t serial) {
    uint32_t skip = reply ? MSG_REPLY_HDR : 0u;
    const ModEntry *me = vm->mods[to->mod];
    if (!me) {
        ds->st.dropped++;
        return;
    }
    Dec d = { vm, ds, p, n, 0, NULL, skip, false, &me->img, module_tag(vm, to->mod) };
    dec(&d, 0);
    /* Amplification: at most DIST_WORDS_PER_BYTE words per frame byte
     * (plus a little), checked before anything is allocated. Ordinary
     * values stay well below it; a frame of nested empty slices does not. */
    if (d.bad || d.at != n || d.words - skip > DIST_WORDS_PER_BYTE * n + 64u) {
        ds->st.bad++;
        return;
    }
    uint32_t words = d.words;
    Msg *m = va_alloc(vm, msg_alloc_bytes((uint16_t)words));
    if (!m) {
        ds->st.dropped++;
        return;
    }
    m->next = NULL;
    m->words = (uint16_t)words;
    m->flags = reply ? MSG_REPLY : 0u;
    Dec w = { vm, ds, p, n, 0, m->data, skip, false, &me->img, module_tag(vm, to->mod) };
    m->root = dec(&w, 0);
    if (w.bad) {                    /* no proxy slot for a PID in it */
        msg_free(vm, m);
        ds->st.dropped++;
        return;
    }
    if (reply) {
        m->data[0] = (uint32_t)serial;
        m->data[1] = (uint32_t)(serial >> 32);
    }
    /* The receiver's quota applies, but a remote message over it is
     * dropped (at most once), never the receiver killed: a peer must not
     * be able to end a local actor by sending it data. */
    if (!msg_try_enqueue(vm, to, m)) {
        msg_free(vm, m);
        ds->st.dropped++;
    }
}

/* ---- watches ---- */

static Watch *watch_new(DistState *ds, uint8_t kind, uint16_t node, term local, term peer, uint64_t serial) {
    for (uint32_t i = 0; i < DIST_WATCHES; i++) {
        Watch *w = &ds->watches[i];
        if (w->kind == W_FREE) {
            w->kind = kind;
            w->node = node;
            w->local = local;
            w->peer = peer;
            w->serial = serial;
            return w;
        }
    }
    return NULL;
}

static Watch *watch_find(DistState *ds, uint8_t kind, term local, term peer, uint64_t serial) {
    for (uint32_t i = 0; i < DIST_WATCHES; i++) {
        Watch *w = &ds->watches[i];
        if (w->kind == kind && w->local == local && (kind == W_MON_OUT || w->peer == peer) &&
            (kind == W_LINK || w->serial == serial)) return w;
    }
    return NULL;
}

/* A live local actor that frames may name as their target: the agent for
 * the zero PID, else an exported PID; NULL otherwise. */
static Actor *target(Vm *vm, DistState *ds, term pid) {
    if (pid == T_ZEROPID || pid == 0u) pid = ds->agent;
    else if (!exported(ds, pid)) return NULL;
    Actor *a = pid ? actor_lookup(vm, pid) : NULL;
    return a && a->state != A_EXITED && !(a->flags & AF_EXIT_PENDING) ? a : NULL;
}

/* Node `node` is unreachable: its monitors fire, its links signal, its
 * other watches and its proxies go. */
static void node_gone(Vm *vm, DistState *ds, uint16_t node) {
    for (uint32_t i = 0; i < DIST_WATCHES; i++) {
        Watch w = ds->watches[i];
        if (w.kind == W_FREE || w.node != node) continue;
        ds->watches[i].kind = W_FREE;
        Actor *a = actor_lookup(vm, w.local);
        if (!a) continue;
        if (w.kind == W_MON_OUT) down_to(vm, a, w.serial, w.peer, R_NO_CONNECTION);
        else if (w.kind == W_LINK) exit_to(vm, a, w.peer, R_NO_CONNECTION);
    }
    proxies_drop(ds, node);
}

/* ---- the hooks (VmDist) ---- */

static int h_send(Vm *vm, Actor *from, term to, uint64_t serial, term msg) {
    DistState *ds = state(vm);
    Proxy *x = proxy_of(ds, to);
    Enc e = { vm, from, ds, NULL, 0, 0 };
    enc(&e, msg, 0);
    uint32_t len = HDR + 4u + (serial ? 8u : 0u) + e.n;
    if (!e.why && 2u + len > DIST_FRAME_MAX) e.why = R_BAD_MSG;
    if (e.why && e.why != WHY_DROP) return -(int)e.why;
    Link *l = x ? route(ds, x->node) : NULL;
    if (x) x->used = ++ds->tick;
    uint8_t *p;
    Msg *m = l && !e.why ? chunk_new(vm, l, 2u + len, &p) : NULL;
    if (!m) {
        ds->st.dropped++;
        return 0;
    }
    p = wr16(p, len);
    *p++ = serial ? F_REPLY : F_SEND;
    *p++ = 0;
    p = wr16(p, x->node);
    p = wr16(p, ds->self);
    p = wr32(p, x->remote);
    if (serial) p = wr64(p, serial & SERIAL_MASK);
    Enc w = { vm, from, ds, p, 0, 0 };
    enc(&w, msg, 0);
    if (w.why) {                    /* the export table filled up */
        msg_free(vm, m);
        ds->st.dropped++;
        return 0;
    }
    chunk_send(vm, ds, l, m);
    return 0;
}

static bool h_monitor(Vm *vm, Actor *a, term pid, uint64_t serial) {
    DistState *ds = state(vm);
    Proxy *x = proxy_of(ds, pid);
    if (x && route(ds, x->node) && watch_new(ds, W_MON_OUT, x->node, a->id, pid, serial)) {
        uint32_t w[4] = { a->id, x->remote, (uint32_t)serial, (uint32_t)(serial >> 32) };
        control(vm, ds, x->node, F_MONITOR, w, 4);
        return true;
    }
    return down_to(vm, a, serial, pid, R_NO_CONNECTION);   /* no route, or no room */
}

static void h_demonitor(Vm *vm, Actor *a, uint64_t serial) {
    DistState *ds = state(vm);
    Watch *w = watch_find(ds, W_MON_OUT, a->id, 0, serial);
    if (!w) return;
    w->kind = W_FREE;
    Proxy *x = proxy_of(ds, w->peer);
    if (!x) return;
    uint32_t f[4] = { a->id, x->remote, (uint32_t)serial, (uint32_t)(serial >> 32) };
    control(vm, ds, x->node, F_DEMONITOR, f, 4);
}

static bool h_link(Vm *vm, Actor *a, term pid, bool on) {
    DistState *ds = state(vm);
    Proxy *x = proxy_of(ds, pid);
    Watch *w = watch_find(ds, W_LINK, a->id, pid, 0);
    if (!on) {
        if (w) {
            w->kind = W_FREE;
            if (x) {
                uint32_t f[2] = { a->id, x->remote };
                control(vm, ds, x->node, F_UNLINK, f, 2);
            }
        }
        return true;
    }
    if (w) return true;
    if (x && route(ds, x->node) && watch_new(ds, W_LINK, x->node, a->id, pid, 0)) {
        uint32_t f[2] = { a->id, x->remote };
        control(vm, ds, x->node, F_LINK, f, 2);
        return true;
    }
    return exit_to(vm, a, pid, R_NO_CONNECTION);
}

static void h_exited(Vm *vm, Actor *a, uint32_t code) {
    DistState *ds = state(vm);
    for (uint32_t i = 0; i < DIST_WATCHES; i++) {
        Watch *w = &ds->watches[i];
        if (w->kind == W_FREE || w->local != a->id) continue;
        uint8_t kind = w->kind;
        w->kind = W_FREE;
        if (kind == W_MON_IN) {
            uint32_t f[5] = { w->peer, a->id, (uint32_t)w->serial, (uint32_t)(w->serial >> 32), code };
            control(vm, ds, w->node, F_DOWN, f, 5);
            continue;
        }
        Proxy *x = proxy_of(ds, w->peer);
        if (!x) continue;
        if (kind == W_LINK) {
            uint32_t f[3] = { a->id, x->remote, code };
            control(vm, ds, x->node, F_EXIT, f, 3);
        } else {
            uint32_t f[4] = { a->id, x->remote, (uint32_t)w->serial, (uint32_t)(w->serial >> 32) };
            control(vm, ds, x->node, F_DEMONITOR, f, 4);
        }
    }
    if (exported(ds, a->id)) {
        /* Peers may hold proxies of it: tell them it is gone. */
        for (uint32_t i = 0; i < DIST_LINKS; i++) {
            if (ds->links[i].port) gone_to(vm, ds, &ds->links[i], ds->self, a->id);
        }
    }
    for (uint32_t i = 0; i < DIST_EXPORTS; i++) {
        if (ds->exports[i] == a->id) ds->exports[i] = 0;
    }
    if (ds->agent == a->id) ds->agent = 0;
}

static void h_print(Out *o, const Vm *vm, term pid) {
    Proxy *x = proxy_of(state(vm), pid);
    if (!x) {
        out_str(o, "?@?");
        return;
    }
    out_int(o, term_pid_slot(x->remote));
    out_str(o, "@");
    out_int(o, x->node);
}

/* NodeEvent{From: the port, Kind, Node, Agent} to p's owner (dropped if
 * it has no room: the agent learns of a lost node from its peers too). */
static void node_event(Vm *vm, const Port *p, int32_t kind, int32_t node, term agent) {
    Actor *o = port_owner(vm, p);
    Msg *m = o ? note_new(vm, 6) : NULL;
    if (!m) return;
    m->data[0] = hdr_make(6, K_STRUCT);
    m->data[1] = TYPE_NET_NODEEVENT;
    m->data[2] = p->id;
    m->data[3] = term_from_int(kind);
    m->data[4] = term_from_int(node);
    m->data[5] = agent;
    if (!msg_try_enqueue(vm, o, m)) msg_free(vm, m);
}

/* ---- frames from a link ---- */

/* A frame body b[0..n) (n >= HDR) from link l. */
static void frame_in(Vm *vm, DistState *ds, Link *l, const uint8_t *b, uint32_t n) {
    uint32_t type = b[0];
    uint16_t dst = rd16(b + 2), src = rd16(b + 4);
    const uint8_t *f = b + HDR;
    uint32_t fn = n - HDR;
    ds->st.received++;
    /* Only the relay may speak for other nodes. */
    if ((!l->relay && src != l->peer) || !src || src == ds->self) {
        ds->st.bad++;
        return;
    }
    if (type == F_NODEDOWN) {
        if (l->relay && fn == 2u && rd16(f) != ds->self) {
            node_gone(vm, ds, rd16(f));
            node_event(vm, l->port, 2, rd16(f), T_ZEROPID);
        } else {
            ds->st.bad++;
        }
        return;
    }
    if (type == F_GONE) {                        /* src's actor pid exited */
        if (fn != 4u) {
            ds->st.bad++;
            return;
        }
        term p = proxy_find(ds, src, rd32(f));
        Proxy *x = p ? proxy_of(ds, p) : NULL;
        if (x && !proxy_watched(ds, p)) proxy_free(x);
        if (!l->relay) {                         /* the server: tell the other boards */
            for (uint32_t i = 0; i < DIST_LINKS; i++) {
                Link *k = &ds->links[i];
                if (k->port && k != l) gone_to(vm, ds, k, src, rd32(f));
            }
        }
        return;
    }
    if (dst != ds->self) {                       /* pass it on */
        Link *r = route(ds, dst);
        if (r && r != l) {
            uint8_t *p;
            Msg *m = chunk_new(vm, r, 2u + n, &p);
            if (m) {
                memcpy(wr16(p, n), b, n);
                ds->st.relayed++;
                chunk_send(vm, ds, r, m);
                return;
            }
        }
        ds->st.dropped++;
        /* The sender would wait forever for a monitor or link answer. */
        if (type == F_MONITOR && fn == 16u) {
            uint32_t w[5] = { rd32(f), rd32(f + 4), rd32(f + 8), rd32(f + 12), R_NO_CONNECTION };
            uint16_t self = ds->self;
            ds->self = dst;                      /* answer as the node that is gone */
            control(vm, ds, src, F_DOWN, w, 5);
            ds->self = self;
        } else if (type == F_LINK && fn == 8u) {
            uint32_t w[3] = { rd32(f + 4), rd32(f), R_NO_CONNECTION };
            uint16_t self = ds->self;
            ds->self = dst;
            control(vm, ds, src, F_EXIT, w, 3);
            ds->self = self;
        }
        return;
    }
    switch (type) {
    case F_SEND:
    case F_REPLY: {
        uint32_t k = type == F_REPLY ? 12u : 4u;
        if (fn < k + 1u) break;
        term to = rd32(f);
        if (type == F_REPLY && !to) break;
        Actor *a = target(vm, ds, to);
        if (!a) {
            ds->st.dropped++;
            return;
        }
        deliver(vm, ds, a, f + k, fn - k, type == F_REPLY, type == F_REPLY ? rd64(f + 4) : 0u);
        return;
    }
    case F_LINK: {
        if (fn != 8u) break;
        term from = rd32(f), to = rd32(f + 4);
        if (!term_is_pid(from) || term_pid_slot(from) >= DIST_SLOT_BASE) break;
        Actor *a = target(vm, ds, to);
        term peer = proxy_intern(ds, src, from);
        if (a && peer && (watch_find(ds, W_LINK, a->id, peer, 0) ||
                          watch_new(ds, W_LINK, src, a->id, peer, 0))) return;
        uint32_t w[3] = { to, from, a ? R_NO_CONNECTION : R_NO_PROC };
        control(vm, ds, src, F_EXIT, w, 3);
        return;
    }
    case F_UNLINK:
    case F_EXIT: {
        if (fn != (type == F_EXIT ? 12u : 8u)) break;
        term peer = proxy_find(ds, src, rd32(f));
        Actor *a = actor_lookup(vm, rd32(f + 4));
        Watch *w = a && peer ? watch_find(ds, W_LINK, a->id, peer, 0) : NULL;
        if (!w) {                                 /* no such link: not the peer's to signal */
            if (type == F_EXIT) ds->st.bad++;
            return;
        }
        w->kind = W_FREE;
        if (type == F_EXIT) exit_to(vm, a, peer, rd32(f + 8));
        return;
    }
    case F_MONITOR: {
        if (fn != 16u) break;
        term watcher = rd32(f), tgt = rd32(f + 4);
        uint64_t serial = rd64(f + 8);
        Actor *a = target(vm, ds, tgt);
        if (a && watch_new(ds, W_MON_IN, src, a->id, watcher, serial)) return;
        uint32_t w[5] = { watcher, tgt, (uint32_t)serial, (uint32_t)(serial >> 32), a ? R_NO_CONNECTION : R_NO_PROC };
        control(vm, ds, src, F_DOWN, w, 5);
        return;
    }
    case F_DEMONITOR: {
        if (fn != 16u) break;
        for (uint32_t i = 0; i < DIST_WATCHES; i++) {
            Watch *w = &ds->watches[i];
            if (w->kind == W_MON_IN && w->node == src && w->peer == rd32(f) && w->serial == rd64(f + 8))
                w->kind = W_FREE;
        }
        return;
    }
    case F_DOWN: {
        if (fn != 20u) break;
        Watch *w = watch_find(ds, W_MON_OUT, rd32(f), 0, rd64(f + 8));
        Proxy *x = w ? proxy_of(ds, w->peer) : NULL;
        if (!x || x->node != src || (x->remote != rd32(f + 4) && x->remote != T_ZEROPID)) {
            ds->st.bad++;
            return;
        }
        w->kind = W_FREE;
        Actor *a = actor_lookup(vm, w->local);
        if (a) down_to(vm, a, w->serial, w->peer, rd32(f + 16));
        return;
    }
    default:
        break;
    }
    ds->st.bad++;
}

/* The link's byte stream broke (a length out of range): it ends. */
static void link_broken(Vm *vm, Port *p) {
    Actor *o = port_owner(vm, p);
    Msg *m = o ? note_new(vm, 4) : NULL;
    if (m) {
        m->data[0] = hdr_make(4, K_STRUCT);
        m->data[1] = TYPE_CLOSED;
        m->data[2] = p->id;
        m->data[3] = term_from_int(GOGO_E_BADARG);
        if (!msg_try_enqueue(vm, o, m)) msg_free(vm, m);
    }
    port_close(vm, p->id);
}

/* Takes n bytes of the link's stream; false if it broke. */
static bool feed(Vm *vm, DistState *ds, Link *l, const uint8_t *d, uint32_t n) {
    while (n) {
        if (!l->rx) {
            if (!l->hb_n && n >= 2u) {
                uint32_t len = rd16(d);
                if (len < HDR || len + 2u > DIST_FRAME_MAX) return false;
                if (n >= 2u + len) {             /* whole: in place */
                    frame_in(vm, ds, l, d + 2, len);
                    if (!l->port) return true;   /* closed meanwhile */
                    d += 2u + len;
                    n -= 2u + len;
                    continue;
                }
            }
            while (l->hb_n < 2u && n) {
                l->hb[l->hb_n++] = *d++;
                n--;
            }
            if (l->hb_n < 2u) return true;
            uint32_t len = rd16(l->hb);
            if (len < HDR || len + 2u > DIST_FRAME_MAX) return false;
            l->rx = va_alloc(vm, len);
            if (!l->rx) return false;
            l->rx_need = (uint16_t)len;
            l->rx_have = 0;
        }
        uint32_t k = l->rx_need - l->rx_have;
        if (k > n) k = n;
        memcpy(l->rx + l->rx_have, d, k);
        l->rx_have = (uint16_t)(l->rx_have + k);
        d += k;
        n -= k;
        if (l->rx_have == l->rx_need) {
            uint8_t *rx = l->rx;
            uint32_t len = l->rx_need;
            l->rx = NULL;
            l->hb_n = 0;
            frame_in(vm, ds, l, rx, len);
            va_free(vm, rx, len);
            if (!l->port) return true;
        }
    }
    return true;
}

/* ---- the port ---- */

static DistState *state_new(Vm *vm) {
    if (vm->dist) return state(vm);
    DistState *ds = va_alloc(vm, sizeof *ds);
    if (!ds) return NULL;
    memset(ds, 0, sizeof *ds);
    ds->hooks.send = h_send;
    ds->hooks.monitor = h_monitor;
    ds->hooks.link = h_link;
    ds->hooks.demonitor = h_demonitor;
    ds->hooks.exited = h_exited;
    ds->hooks.print = h_print;
    vm->dist = &ds->hooks;
    return ds;
}

/* NodeLink{Self, Peer, Relay int; Agent PID}. */
static int n_open(Vm *vm, const Actor *owner, Port *p, uint16_t index, term cfg) {
    (void)index;
    const uint32_t *o = obj_of(vm, owner, cfg);
    int32_t self, peer, relay;
    if (!o || hdr_words(o[0]) != 6u || !int_val(vm, owner, o[2], &self) || !int_val(vm, owner, o[3], &peer) ||
        !int_val(vm, owner, o[4], &relay) || !term_is_pid(o[5]))
        return -1;
    term agent = o[5];
    if (self < 1 || self > 0xFFFF || peer < 1 || peer > 0xFFFF || self == peer ||
        vm->table_cap > DIST_SLOT_BASE)
        return -1;
    DistState *ds = vm->dist ? state(vm) : NULL;
    Link *l = NULL;
    for (uint32_t i = 0; ds && i < DIST_LINKS; i++) {
        Link *k = &ds->links[i];
        /* One id per VM while it has links (a new connection may get a
         * new one); one link per peer; one relay. */
        if (k->port && (k->peer == peer || (relay && k->relay) || ds->self != self)) return -1;
    }
    if (!ds && !(ds = state_new(vm))) return -1;
    for (uint32_t i = 0; i < DIST_LINKS && !l; i++) {
        if (!ds->links[i].port) l = &ds->links[i];
    }
    if (!l) return -1;
    term up = proxy_intern(ds, (uint16_t)peer, T_ZEROPID);
    if (!up) return -1;
    memset(l, 0, sizeof *l);
    l->port = p;
    l->peer = (uint16_t)peer;
    l->relay = relay != 0;
    ds->self = (uint16_t)self;
    if (agent != T_ZEROPID && actor_lookup(vm, agent)) ds->agent = agent;
    /* NodeEvent{Kind: Up, Node: peer, Agent: the peer's agent}. The port
     * charges nothing, so the open cannot fail after this. p->owner is set
     * already (port_open), so it goes to the owner. */
    node_event(vm, p, 1, peer, up);
    return 0;
}

/* port.Out{Data} from the owner: bytes from the peer. */
static void n_command(Vm *vm, const Actor *from, Port *p, term msg) {
    DistState *ds = state(vm);
    Link *l = link_of(ds, p);
    const uint32_t *o = obj_of(vm, from, msg);
    const uint8_t *d;
    uint32_t n;
    if (!l || from->id != p->owner || !o || hdr_kind(o[0]) != K_STRUCT || o[1] != TYPE_OUT ||
        hdr_words(o[0]) != 3u || !bytes_view(vm, from, o[2], &d, &n))
        return;
    if (!feed(vm, ds, l, d, n) && l->port == p) link_broken(vm, p);
}

/* The link is gone: every node it was the route to is unreachable, and
 * the other links hear that its peer is gone. */
static void n_close(Vm *vm, Port *p) {
    if (!vm || !vm->dist) return;
    DistState *ds = state(vm);
    Link *l = link_of(ds, p);
    if (!l) return;
    uint16_t gone[DIST_PROXIES + DIST_WATCHES + 1u];
    uint32_t ng = 0;
    gone[ng++] = l->peer;
    /* Nodes reached through l only (through the relay): every node of a
     * proxy or watch whose route is l. */
    for (uint32_t i = 0; i < DIST_PROXIES; i++) {
        uint16_t nd = ds->proxies[i].node;
        if (nd && route(ds, nd) == l) gone[ng++] = nd;
    }
    for (uint32_t i = 0; i < DIST_WATCHES; i++) {
        uint16_t nd = ds->watches[i].node;
        if (ds->watches[i].kind != W_FREE && route(ds, nd) == l) gone[ng++] = nd;
    }
    if (l->rx) va_free(vm, l->rx, l->rx_need);
    uint16_t peer = l->peer;
    memset(l, 0, sizeof *l);
    for (uint32_t i = 0; i < ng; i++) node_gone(vm, ds, gone[i]);
    for (uint32_t i = 0; i < DIST_LINKS; i++) {
        Link *k = &ds->links[i];
        if (!k->port) continue;
        uint8_t b[2u + HDR + 2u];
        wr16(wr16(wr16(wr16(b + 2, F_NODEDOWN), k->peer), ds->self), peer);
        wr16(b, HDR + 2u);
        emit(vm, ds, k, b, sizeof b);
    }
}

static bool n_can_event(const Vm *vm, const Port *p) { (void)vm; (void)p; return true; }

const PortDriver dist_node_driver = {
    .name = PORT_NAME_NODE, .cfg_type = TYPE_NET_NODELINK, .event_type = TYPE_NET_NODEEVENT,
    .close = n_close, .can_event = n_can_event, .command_term = n_command, .open_term = n_open
};
