#include "signals.h"
#include "alloc.h"
#include "config.h"
#include "opcodes.h"
#include "host.h"
#include "out.h"
#include "msg.h"
#include "heap.h"
#include "timer.h"
#include "names.h"
#include "bytes.h"
#include <string.h>

/* One side of a link or of a monitor: a node on one of the three lists of
 * an actor's extra. L_LINKS (peer = the linked actor) holds the ExitMsg
 * the actor sends the peer when it exits, allocated when the link is made.
 * L_MONITORS (peer = the watcher) holds the DownMsg the monitored actor
 * sends when it exits, allocated when the monitor is made. L_WATCHING
 * (peer = the monitored actor) holds no message: the node lets Demonitor,
 * and the watcher's own exit, find the other side. The two monitor lists
 * hold Monitors, keyed by the Ref serial; links are keyed by the peer. */
typedef struct Link {
    struct Link *next;
    term peer;
    Msg *note;                /* preallocated notice, or NULL (L_WATCHING) */
} Link;

typedef struct Monitor {
    Link l;
    uint64_t serial;
} Monitor;

enum { L_LINKS, L_MONITORS, L_WATCHING, L_LISTS };

struct ActorExtra {
    uint32_t exit_code;       /* valid while AF_EXIT_PENDING is set */
    term exit_func;           /* likewise: function name or T_EMPTYSTR */
    Link *list[L_LISTS];      /* links, its watchers (monitors), the actors it watches */
    /* Words charged to this actor besides its heap block and mailbox:
     * pending SendAfter timers it created, its monitors (both entries +
     * the preallocated DownMsg) and its side of each link (entry +
     * preallocated ExitMsg). Read through actor_charged_words. Registered
     * names live in names.c, timers in Vm.timers. */
    uint32_t n[AX_COUNTS];    /* AX_CHARGED: those words; AX_TIMERS_IN: SendAfter timers
                               * pending for it; AX_TIMERS_OUT: pending SendAfter timers
                               * charged to it (timer.c); AX_PORTS: open ports it owns (port.c) */
    HeldBytes *held;          /* off-heap Bytes it holds (bytes.c), or NULL */
};

/* Words a resource takes, rounded up as the allocator would count them. */
#define WORDS_OF(T) ((uint32_t)((sizeof(T) + 3u) / 4u))

/* ---- emergency reserve (Vm.reserve, Vm.reserve_wanted) ---- */

bool vm_reserve_init(Vm *vm) {
    vm->reserve_wanted = true;
    if (!vm->reserve) vm->reserve = va_alloc(vm, GOGO_RESERVE_BYTES);
    return vm->reserve != NULL;
}

void vm_reserve_release(Vm *vm) {
    va_free(vm, vm->reserve, GOGO_RESERVE_BYTES);
    vm->reserve = NULL;
    vm->reserve_wanted = false;
}

void *signal_alloc(Vm *vm, size_t bytes) {
    void *p = va_alloc(vm, bytes);
    if (!p && vm->reserve) {
        va_free(vm, vm->reserve, GOGO_RESERVE_BYTES);
        vm->reserve = NULL;
        p = va_alloc(vm, bytes);
    }
    return p;
}

/* After an exit or mark: take the reserve again if it was used. */
static void reserve_refill(Vm *vm) {
    if (vm->reserve_wanted && !vm->reserve) vm->reserve = va_alloc(vm, GOGO_RESERVE_BYTES);
}

/* ---- extra ---- */

/* a's extra, allocated on first use. The exit path may fall back on the
 * reserve (from_reserve); other callers (linking) must not. */
static ActorExtra *extra_get(Vm *vm, Actor *a, bool from_reserve) {
    if (!a->extra) {
        ActorExtra *x = from_reserve ? signal_alloc(vm, sizeof(ActorExtra))
                                     : va_alloc(vm, sizeof(ActorExtra));
        if (!x) return NULL;
        memset(x, 0, sizeof *x);
        x->exit_func = T_EMPTYSTR;
        a->extra = x;
    }
    return a->extra;
}

uint32_t actor_charged_words(const Actor *a) {
    const ActorExtra *x = a->extra;
    return (uint32_t)a->mbox_words + (x ? x->n[AX_CHARGED] + (x->held ? bytes_words(x->held->bytes) : 0u) : 0u);
}

HeldBytes **actor_held(Actor *a) {
    ActorExtra *x = a->extra;
    return x ? &x->held : NULL;
}

HeldBytes **actor_held_new(Vm *vm, Actor *a) {
    ActorExtra *x = extra_get(vm, a, false);
    return x ? &x->held : NULL;
}

bool actor_count_add(Vm *vm, Actor *a, uint32_t which, uint32_t n) {
    ActorExtra *x = extra_get(vm, a, false);
    if (!x) return false;
    x->n[which] += n;
    return true;
}

void actor_count_sub(Actor *a, uint32_t which, uint32_t n) {
    ActorExtra *x = a->extra;
    if (x && x->n[which] >= n) x->n[which] -= n;
}

uint32_t actor_count(const Actor *a, uint32_t which) {
    const ActorExtra *x = a->extra;
    return x ? x->n[which] : 0u;
}

/* True if `words` more charged words keep a within its limit. */
static bool charge_fits(const Actor *a, uint32_t words) {
    return (uint32_t)a->mem_words + actor_charged_words(a) + words <= actor_max_words(a);
}

static void unwind(Vm *vm, Actor *a, uint32_t code, term func_name, bool notify);

void actor_extra_free(Vm *vm, Actor *a) {
    ActorExtra *x = a->extra;
    if (!x) return;
    /* After actor_exit the lists are empty. An actor freed without exiting
     * (never ran) may still hold some: unwind them so no peer keeps an
     * entry naming it or a charge for it. */
    unwind(vm, a, 0, T_EMPTYSTR, false);
    bytes_actor_exit(vm, a);
    va_free(vm, x, sizeof(ActorExtra));
    a->extra = NULL;
}

/* ---- notices ---- */

/* An ExitMsg or a DownMsg as a message block, laid out as msg_build would
 * copy it, refs relative to data; `at` is the struct's size and tells
 * them apart. ExitMsg (at 4, NOTE_WORDS): [0..3] ExitMsg{From, Reason ->
 * 4}. DownMsg (at 5, DOWN_WORDS): [0..4] DownMsg{Ref -> 11, From, Reason
 * -> 5}, [11..13] the K_REF. Both: [at..at+3] Reason{Code, Func},
 * [at+4..at+5] a K_INT box Code points at when the code does not fit a
 * small int (unused otherwise). Fixed size, so it can be preallocated;
 * From, the Reason and the Ref are filled in when it is sent. */
#define AT_EXIT 4u
#define AT_DOWN 5u
#define NOTE_WORDS 10u
#define DOWN_WORDS 14u

static inline Msg *note_new(Vm *vm, uint32_t at) {
    uint32_t words = at == AT_EXIT ? NOTE_WORDS : DOWN_WORDS;
    Msg *m = va_alloc(vm, msg_alloc_bytes((uint16_t)words));
    if (!m) return NULL;
    m->next = NULL;
    m->words = (uint16_t)words;
    m->flags = 0;
    m->root = term_from_ref(0);
    uint32_t *d = m->data, *r = d + at;
    d[0] = hdr_make(at, K_STRUCT);
    if (at == AT_EXIT) {
        d[1] = TYPE_EXITMSG;
        d[2] = T_NIL;
    } else {
        d[1] = TYPE_DOWNMSG;
        d[2] = term_from_ref(11);
        d[3] = T_NIL;
        d[11] = hdr_make(3, K_REF);
    }
    r[-1] = term_from_ref(at);
    r[0] = hdr_make(4, K_STRUCT);
    r[1] = TYPE_REASON;
    r[2] = term_from_int(0);
    r[3] = T_EMPTYSTR;
    r[4] = hdr_make(2, K_INT);
    r[5] = 0;
    return m;
}

/* Reason.Func as the notice to `to` carries it: a function name of another
 * module than to's (a const ref with another tag) is blanked (design
 * §5.3), so a notice never names, or keeps reading, a module its receiver
 * does not run; the crash report still prints it. */
static term func_for(const Vm *vm, const Actor *to, term f) {
    return term_is_const(f) && (f & MOD_TAG_MASK) != module_tag(vm, to->mod) ? T_EMPTYSTR : f;
}

/* Fills notice m (From, Reason) and queues it for `to`: with quota as an
 * ordinary message, without as a preallocated one (guaranteed). */
static void note_send(Vm *vm, Actor *to, Msg *m, uint32_t at, term from, uint32_t code, term func_name,
                      bool quota) {
    uint32_t *d = m->data;
    int32_t c = (int32_t)code;
    d[at - 2] = from;
    d[at + 2] = term_int_fits(c) ? term_from_int(c) : term_from_ref(at + 4);
    d[at + 3] = func_for(vm, to, func_name);
    d[at + 5] = code;
    msg_enqueue(vm, to, m, quota);
}

/* The Ref of a DownMsg, known when the monitor is made. */
static void down_set_ref(Msg *m, uint64_t serial) {
    m->data[12] = (uint32_t)serial;
    m->data[13] = (uint32_t)(serial >> 32);
}

/* ---- charges ---- */

/* What a node on list `which` is charged to the list's actor. A link side
 * costs its owner the node and the preallocated ExitMsg it holds; a
 * monitor costs the watcher both nodes and the preallocated DownMsg (all
 * made for the watcher's benefit), charged with its L_WATCHING node. */
#define FOOTPRINT(w) ((uint32_t)((sizeof(Msg) + (w) * 4u + 3u) / 4u))   /* msg_footprint_words */
static const uint8_t node_words_of[L_LISTS] = {
    WORDS_OF(Link) + FOOTPRINT(NOTE_WORDS), 0u, 2u * WORDS_OF(Monitor) + FOOTPRINT(DOWN_WORDS)
};
static const uint8_t node_size_of[L_LISTS] = { sizeof(Link), sizeof(Monitor), sizeof(Monitor) };
#define node_words(which) ((uint32_t)node_words_of[which])
#define node_size(which) ((size_t)node_size_of[which])

/* ---- crash report ---- */

/* Kind spelling for a reason code; NULL for user codes (exit(<code>)). */
static const char *kind_name(uint32_t code) {
    switch (code) {
    case R_KILLED:           return "killed";
    case R_OUT_OF_MEMORY:    return "oom";
    case R_MAILBOX_OVERFLOW: return "mailbox_overflow";
    case R_NO_PROC:          return "noproc";
    case R_DIV_ZERO:         return "divzero";
    case R_BAD_CODE:         return "badcode";
    case R_BAD_SHIFT:        return "badshift";
    case R_BAD_MSG:          return "badmsg";
    case R_PANIC:            return "panic";
    case R_NIL:              return "nil";
    case R_NO_CONNECTION:    return "noconnection";
    default:                 return NULL;
    }
}

/* `gogo: actor <pid N> crashed: <kind>[ in <func>]` on stderr. */
static void report_crash(const Vm *vm, const Actor *a, uint32_t code, term func_name) {
    Out o;
    out_init(&o, vm, 2);
    out_str(&o, CRASH_REPORT_PID);
    out_int(&o, term_pid_slot(a->id));
    out_str(&o, CRASH_REPORT_KIND);
    const char *kind = kind_name(code);
    if (kind) {
        out_str(&o, kind);
    } else {
        out_str(&o, "exit(");
        out_int(&o, (int32_t)code);
        out_str(&o, ")");
    }
    /* The function's module, through the module table like any const ref
     * (no name if the module is gone). */
    const ModEntry *e = term_is_const(func_name) ? module_of_tag(vm, func_name) : NULL;
    if (e) {
        /* A K_STRING wholly inside the const area: [hdr][len][bytes]. */
        const uint32_t *c = e->img.consts;
        uint32_t off = term_const_off(func_name), area = e->img.consts_words;
        uint32_t words = off < area ? hdr_words(c[off]) : 0u;
        if (words >= 2u && words <= area - off && hdr_kind(c[off]) == K_STRING &&
            c[off + 1] <= (words - 2u) * 4u) {
            out_str(&o, " in ");
            out_bytes(&o, (const char *)(c + off + 2), c[off + 1]);
        }
    }
    out_str(&o, "\n");
    out_flush(&o);
}

/* ---- links and monitors ---- */

/* The node of a's list `which` for pid (links) or serial (monitors), as
 * the pointer that holds it; NULL if there is none (or a is NULL). */
static inline Link **node_slot(const Actor *a, uint32_t which, term pid, uint64_t serial) {
    ActorExtra *x = a ? a->extra : NULL;
    if (!x) return NULL;
    for (Link **pl = &x->list[which]; *pl; pl = &(*pl)->next) {
        if (which == L_LINKS ? (*pl)->peer == pid : ((Monitor *)*pl)->serial == serial) return pl;
    }
    return NULL;
}

static inline void node_free(Vm *vm, Link *l, uint32_t which) {
    if (!l) return;
    if (l->note) msg_free(vm, l->note);
    va_free(vm, l, node_size(which));
}

/* Removes that node, freeing it and its preallocated message, and
 * releases a's charge for it. Nothing if there is none (or a is NULL). */
static void node_remove(Vm *vm, Actor *a, uint32_t which, term pid, uint64_t serial) {
    Link **pl = node_slot(a, which, pid, serial);
    if (!pl) return;
    Link *l = *pl;
    *pl = l->next;
    node_free(vm, l, which);
    actor_uncharge(a, node_words(which));
}

/* A node for list `which`, with its preallocated notice if the list holds
 * one (a DownMsg gets its Ref now). NULL if out of memory. */
static inline Link *node_new(Vm *vm, uint32_t which, term peer, uint64_t serial) {
    Link *l = va_alloc(vm, node_size(which));
    if (!l) return NULL;
    l->next = NULL;
    l->peer = peer;
    l->note = NULL;
    if (which != L_LINKS) ((Monitor *)l)->serial = serial;
    if (which != L_WATCHING) {
        l->note = note_new(vm, which == L_LINKS ? AT_EXIT : AT_DOWN);
        if (!l->note) {
            va_free(vm, l, node_size(which));
            return NULL;
        }
        if (which == L_MONITORS) down_set_ref(l->note, serial);
    }
    return l;
}

static inline void node_push(ActorExtra *x, uint32_t which, Link *l) {
    l->next = x->list[which];
    x->list[which] = l;
    x->n[AX_CHARGED] += node_words(which);
}

bool sig_is_linked(const Actor *a, term pid) {
    return node_slot(a, L_LINKS, pid, 0) != NULL;
}

/* Exit signal `code` (no function name) from `from` reaching t. */
static bool exit_signal(Vm *vm, Actor *t, term from, uint32_t code) {
    if (t->flags & AF_EXIT_PENDING) return true;
    if ((t->flags & AF_TRAP_EXIT) && code != R_KILLED) {
        Msg *m = note_new(vm, AT_EXIT);
        if (!m) return false;
        note_send(vm, t, m, AT_EXIT, from, code, T_EMPTYSTR, true);
        return true;
    }
    actor_mark_exit(vm, t, code, T_EMPTYSTR);
    return true;
}

/* Makes the pair of nodes that is a link or a monitor: wa on a's list
 * (charged to a, which must fit it), wp on p's. False, with nothing made
 * or charged, if out of memory or over a's limit. */
static bool pair_new(Vm *vm, Actor *a, uint32_t wa, Actor *p, uint32_t wp, uint64_t serial) {
    if (!charge_fits(a, node_words(wa))) return false;
    Link *la = node_new(vm, wa, p->id, serial), *lp = node_new(vm, wp, a->id, serial);
    ActorExtra *xa = extra_get(vm, a, false), *xp = extra_get(vm, p, false);
    if (!la || !lp || !xa || !xp) {
        node_free(vm, la, wa);
        node_free(vm, lp, wp);
        return false;     /* an extra allocated here stays; freed with its actor */
    }
    node_push(xa, wa, la);
    node_push(xp, wp, lp);   /* a link: even past p's limit, see signals.h */
    return true;
}

bool sig_link(Vm *vm, Actor *a, term pid) {
    if (pid == a->id || sig_is_linked(a, pid)) return true;
    Actor *p = actor_lookup(vm, pid);
    const VmDist *d = p ? NULL : vm_dist_of(vm, pid);
    if (d) return d->link(vm, a, pid, true);
    if (!p || p->state == A_EXITED) return exit_signal(vm, a, pid, R_NO_PROC);
    return pair_new(vm, a, L_LINKS, p, L_LINKS, 0);
}

void sig_unlink(Vm *vm, Actor *a, term pid) {
    node_remove(vm, a, L_LINKS, pid, 0);
    Actor *p = actor_lookup(vm, pid);
    if (p) node_remove(vm, p, L_LINKS, a->id, 0);
    else if (vm_dist_of(vm, pid)) vm->dist->link(vm, a, pid, false);
}

bool sig_kill(Vm *vm, Actor *from, term pid, uint32_t code) {
    Actor *t = actor_lookup(vm, pid);
    const VmDist *d = t ? NULL : vm_dist_of(vm, pid);
    if (d) return d->kill(vm, from, pid, code);
    if (!t || t->state == A_EXITED) return true;
    if (code == R_NORMAL && t != from && !(t->flags & AF_TRAP_EXIT)) return true;
    return exit_signal(vm, t, from->id, code);
}

void sig_set_trap(Actor *a, bool on) {
    if (on) a->flags |= AF_TRAP_EXIT;
    else a->flags = (uint8_t)(a->flags & ~AF_TRAP_EXIT);
}

term sig_monitor(Vm *vm, Actor *a, term pid) {
    uint64_t serial = vm->next_ref;
    term ref = heap_new_ref(vm, a);
    if (ref == T_NIL) return T_NIL;
    Actor *t = actor_lookup(vm, pid);
    if (t == a) return ref;
    const VmDist *d = t ? NULL : vm_dist_of(vm, pid);
    if (d) return d->monitor(vm, a, pid, serial) ? ref : T_NIL;
    if (!t || t->state == A_EXITED) {
        Msg *m = note_new(vm, AT_DOWN);
        if (!m) return T_NIL;
        down_set_ref(m, serial);
        if (a->flags & AF_EXIT_PENDING) msg_free(vm, m);
        else note_send(vm, a, m, AT_DOWN, pid, R_NO_PROC, T_EMPTYSTR, true);
        return ref;
    }
    return pair_new(vm, a, L_WATCHING, t, L_MONITORS, serial) ? ref : T_NIL;
}

void sig_demonitor(Vm *vm, Actor *a, uint64_t serial) {
    if (vm->dist) vm->dist->demonitor(vm, a, serial);
    Link **pl = node_slot(a, L_WATCHING, 0, serial);
    if (!pl) return;
    node_remove(vm, actor_lookup(vm, (*pl)->peer), L_MONITORS, 0, serial);
    node_remove(vm, a, L_WATCHING, 0, serial);
}

/* Empties a's three lists, in this order: links, watchers, watched. Each
 * peer's own node for a goes first, so no list ever names a dead actor,
 * and both sides' charges end. With notify (a is exiting with
 * code/func_name): a linked actor that traps exits gets its preallocated
 * ExitMsg, any other is marked to exit too (unless code is Normal); every
 * watcher gets its preallocated DownMsg (whatever the code, never
 * cascading; several monitors deliver newest first). A peer already
 * marked for exit gets nothing. Without notify (a goes away without
 * exiting, actor_extra_free) nothing is sent. Every preallocated block is
 * queued or freed here exactly once. */
static void unwind(Vm *vm, Actor *a, uint32_t code, term func_name, bool notify) {
    ActorExtra *x = a->extra;
    if (!x) return;
    for (uint32_t which = 0; which < L_LISTS; which++) {
        /* The list on the peer that holds the other side. */
        uint32_t other = which == L_LINKS ? L_LINKS : which ^ (L_MONITORS ^ L_WATCHING);
        Link *l = x->list[which];
        x->list[which] = NULL;
        while (l) {
            Link *next = l->next;
            Msg *note = l->note;
            Actor *p = actor_lookup(vm, l->peer);
            node_remove(vm, p, other, a->id, which == L_LINKS ? 0 : ((Monitor *)l)->serial);
            va_free(vm, l, node_size(which));
            actor_uncharge(a, node_words(which));
            bool cascade = which == L_LINKS && !(p && (p->flags & AF_TRAP_EXIT));
            if (note && notify && p && !cascade && !(p->flags & AF_EXIT_PENDING)) {
                note_send(vm, p, note, which == L_LINKS ? AT_EXIT : AT_DOWN, a->id, code, func_name, false);
            } else {
                if (note) msg_free(vm, note);
                /* The cascade keeps a's func_name, possibly of another module
                 * than p's (M-6, intended): it only travels on as a tagged
                 * const. p's crash report reads it through the module table
                 * (no name once that module is gone), and p's own notices
                 * pass it through func_for per receiver, so it never reaches
                 * an actor of another module. */
                if (cascade && notify && p && code != R_NORMAL) actor_mark_exit(vm, p, code, func_name);
            }
            l = next;
        }
    }
}

/* ---- exit ---- */

void actor_exit(Vm *vm, Actor *a, uint32_t code, term func_name) {
    if (code != R_NORMAL && code != R_SHUTDOWN) report_crash(vm, a, code, func_name);
    a->state = A_EXITED;
    port_close_owned(vm, a->id);       /* before any notification goes out */
    unwind(vm, a, code, func_name, true);
    if (vm->dist) vm->dist->exited(vm, a, code);   /* its remote links and monitors */
    names_drop_actor(vm, a->id);       /* frees only; cannot fail */
    timer_cancel_actor(vm, a);         /* its wakeup and SendAfters to it */
    actor_free(vm, a);                 /* mailbox, extra, block, slot */
    reserve_refill(vm);
}

void actor_mark_exit(Vm *vm, Actor *a, uint32_t code, term func_name) {
    if (a->state == A_EXITED || (a->flags & AF_EXIT_PENDING)) return;
    ActorExtra *x = extra_get(vm, a, true);
    if (x) {
        x->exit_code = code;
        x->exit_func = func_name;
    }
    a->flags |= AF_EXIT_PENDING;
    if (a->state == A_WAITING) {
        a->state = A_RUNNABLE;
        runq_push(vm, a);
    }
    /* A runnable actor is already on the run queue (or is the one running,
     * which checks the flag after each builtin). */
    reserve_refill(vm);
}

bool actor_exit_pending(const Actor *a, uint32_t *code, term *func_name) {
    if (!(a->flags & AF_EXIT_PENDING)) return false;
    const ActorExtra *x = a->extra;
    *code = x ? x->exit_code : R_OUT_OF_MEMORY;
    *func_name = x ? x->exit_func : T_EMPTYSTR;
    return true;
}
