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

/* One side of a link: a's entry for its peer holds the ExitMsg a sends the
 * peer when a exits, allocated when the link is made. */
typedef struct Link {
    struct Link *next;
    term peer;                /* PID of the linked actor */
    Msg *note;                /* preallocated ExitMsg for peer */
} Link;

/* One side of a monitor, keyed by the Ref serial. On the monitored actor's
 * list `monitors` (peer = the watcher) it holds the DownMsg the actor sends
 * when it exits, allocated when the monitor is made. On the watcher's list
 * `watching` (peer = the monitored actor) there is no message: the entry
 * lets Demonitor, and the watcher's own exit, find the other side. */
typedef struct Monitor {
    struct Monitor *next;
    uint64_t serial;
    term peer;
    Msg *note;                /* preallocated DownMsg, or NULL on the watcher side */
} Monitor;

struct ActorExtra {
    uint32_t exit_code;       /* valid while AF_EXIT_PENDING is set */
    term exit_func;           /* likewise: function name or T_EMPTYSTR */
    Link *links;
    Monitor *monitors;        /* watchers of this actor */
    Monitor *watching;        /* actors this one watches */
    /* Words charged to this actor besides its heap block and mailbox:
     * pending SendAfter timers it created, its monitors (both entries +
     * the preallocated DownMsg) and its side of each link (entry +
     * preallocated ExitMsg). Read through actor_charged_words. Registered
     * names live in names.c, timers in Vm.timers. */
    uint32_t charged_words;
    uint32_t timers_in;       /* SendAfter timers pending for this actor (timer.c) */
    uint32_t timers_out;      /* pending SendAfter timers charged to this actor (timer.c) */
    uint32_t ports;           /* open ports this actor owns (port.c) */
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
    return (uint32_t)a->mbox_words + (x ? x->charged_words + (x->held ? bytes_words(x->held->bytes) : 0u) : 0u);
}

HeldBytes **actor_held(Actor *a) {
    ActorExtra *x = a->extra;
    return x ? &x->held : NULL;
}

HeldBytes **actor_held_new(Vm *vm, Actor *a) {
    ActorExtra *x = extra_get(vm, a, false);
    return x ? &x->held : NULL;
}

bool actor_timer_ref(Vm *vm, Actor *a) {
    ActorExtra *x = extra_get(vm, a, false);
    if (!x) return false;
    x->timers_in++;
    return true;
}

void actor_timer_unref(Actor *a) {
    ActorExtra *x = a->extra;
    if (x && x->timers_in) x->timers_in--;
}

bool actor_timer_own(Vm *vm, Actor *a) {
    ActorExtra *x = extra_get(vm, a, false);
    if (!x) return false;
    x->timers_out++;
    return true;
}

void actor_timer_disown(Actor *a) {
    ActorExtra *x = a->extra;
    if (x && x->timers_out) x->timers_out--;
}

bool actor_timer_owns(const Actor *a) {
    const ActorExtra *x = a->extra;
    return x && x->timers_out;
}

bool actor_port_own(Vm *vm, Actor *a) {
    ActorExtra *x = extra_get(vm, a, false);
    if (!x) return false;
    x->ports++;
    return true;
}

void actor_port_disown(Actor *a) {
    ActorExtra *x = a->extra;
    if (x && x->ports) x->ports--;
}

bool actor_port_owns(const Actor *a) {
    const ActorExtra *x = a->extra;
    return x && x->ports;
}

bool actor_timer_has(const Actor *a) {
    const ActorExtra *x = a->extra;
    return x && x->timers_in;
}

uint32_t actor_extra_charged(const Actor *a) {
    const ActorExtra *x = a->extra;
    return x ? x->charged_words : 0u;
}

/* True if `words` more charged words keep a within its limit. */
static bool charge_fits(const Actor *a, uint32_t words) {
    return (uint32_t)a->mem_words + actor_charged_words(a) + words <= actor_max_words(a);
}

bool actor_charge(Vm *vm, Actor *a, uint32_t words) {
    ActorExtra *x = extra_get(vm, a, false);
    if (!x) return false;
    x->charged_words += words;
    return true;
}

void actor_uncharge(Actor *a, uint32_t words) {
    ActorExtra *x = a->extra;
    if (x) x->charged_words -= words;
}

static void link_free(Vm *vm, Link *l) {
    if (!l) return;
    if (l->note) msg_free(vm, l->note);
    va_free(vm, l, sizeof *l);
}

static void monitor_free(Vm *vm, Monitor *m) {
    if (m->note) msg_free(vm, m->note);
    va_free(vm, m, sizeof *m);
}

static void drop_watching(Vm *vm, Actor *a);
static void drop_watchers(Vm *vm, Actor *a);
static void drop_links(Vm *vm, Actor *a);

void actor_extra_free(Vm *vm, Actor *a) {
    ActorExtra *x = a->extra;
    if (!x) return;
    /* After actor_exit the lists are empty. An actor freed without exiting
     * (never ran) may still hold some: unwind them so no peer keeps an
     * entry naming it or a charge for it. */
    drop_watchers(vm, a);
    drop_watching(vm, a);
    drop_links(vm, a);
    bytes_actor_exit(vm, a);
    va_free(vm, x, sizeof(ActorExtra));
    a->extra = NULL;
}

/* ---- ExitMsg blocks ---- */

/* An ExitMsg as a message block, laid out as msg_build would copy it, refs
 * relative to data: [0..3] ExitMsg{From, Reason → 4}; [4..7] Reason{Code,
 * Func}; [8..9] a K_INT box Code points at when the code does not fit a
 * small int (unused otherwise). Fixed size, so it can be preallocated. */
#define NOTE_WORDS 10u

/* The Reason sub-block shared by ExitMsg and DownMsg blocks, at d[at..at+5]:
 * [at..at+3] Reason{Code, Func}, [at+4..at+5] a K_INT box Code points at
 * when the code does not fit a small int (unused otherwise). */
static void reason_init(uint32_t *d, uint32_t at) {
    d[at] = hdr_make(4, K_STRUCT);
    d[at + 1] = TYPE_REASON;
    d[at + 2] = term_from_int(0);
    d[at + 3] = T_EMPTYSTR;
    d[at + 4] = hdr_make(2, K_INT);
    d[at + 5] = 0;
}

static void reason_set(uint32_t *d, uint32_t at, uint32_t code, term func_name) {
    int32_t c = (int32_t)code;
    d[at + 2] = term_int_fits(c) ? term_from_int(c) : term_from_ref(at + 4);
    d[at + 3] = func_name;
    d[at + 5] = code;
}

static Msg *note_new(Vm *vm) {
    Msg *m = va_alloc(vm, msg_alloc_bytes(NOTE_WORDS));
    if (!m) return NULL;
    m->next = NULL;
    m->words = NOTE_WORDS;
    m->flags = 0;
    m->root = term_from_ref(0);
    m->data[0] = hdr_make(4, K_STRUCT);
    m->data[1] = TYPE_EXITMSG;
    m->data[2] = T_NIL;
    m->data[3] = term_from_ref(4);
    reason_init(m->data, 4);
    return m;
}

/* Reason.Func as the notice to `to` carries it: a function name of another
 * module than to's (a const ref with another tag) is blanked (design
 * §5.3), so a notice never names, or keeps reading, a module its receiver
 * does not run; the crash report still prints it. */
static term func_for(const Vm *vm, const Actor *to, term f) {
    return term_is_const(f) && (f & MOD_TAG_MASK) != module_tag(vm, to->mod) ? T_EMPTYSTR : f;
}

static void note_fill(Msg *m, term from, uint32_t code, term func_name) {
    m->data[2] = from;
    reason_set(m->data, 4, code, func_name);
}

/* A DownMsg as a message block, same style: [0..4] DownMsg{Ref → 11, From,
 * Reason → 5}; [5..8] Reason{Code, Func}; [9..10] the K_INT box for a code
 * that does not fit a small int; [11..13] the K_REF. Fixed size. */
#define DOWN_WORDS 14u

static Msg *down_new(Vm *vm) {
    Msg *m = va_alloc(vm, msg_alloc_bytes(DOWN_WORDS));
    if (!m) return NULL;
    m->next = NULL;
    m->words = DOWN_WORDS;
    m->flags = 0;
    m->root = term_from_ref(0);
    m->data[0] = hdr_make(5, K_STRUCT);
    m->data[1] = TYPE_DOWNMSG;
    m->data[2] = term_from_ref(11);
    m->data[3] = T_NIL;
    m->data[4] = term_from_ref(5);
    reason_init(m->data, 5);
    m->data[11] = hdr_make(3, K_REF);
    m->data[12] = 0;
    m->data[13] = 0;
    return m;
}

/* The parts of a DownMsg known when the monitor is made. */
static void down_set_ref(Msg *m, uint64_t serial, term from) {
    m->data[3] = from;
    m->data[12] = (uint32_t)serial;
    m->data[13] = (uint32_t)(serial >> 32);
}

static void down_set_reason(Msg *m, uint32_t code, term func_name) {
    reason_set(m->data, 5, code, func_name);
}

/* ---- charges ---- */

/* What one link side costs its owner: the entry and the preallocated
 * ExitMsg it holds. */
static uint32_t link_words(void) {
    return WORDS_OF(Link) + msg_footprint_words(NOTE_WORDS);
}

/* What one monitor costs the watcher: both entries and the preallocated
 * DownMsg (all made for the watcher's benefit). */
static uint32_t monitor_words(void) {
    return 2u * WORDS_OF(Monitor) + msg_footprint_words(DOWN_WORDS);
}

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

/* ---- links ---- */

/* The link in a's list naming pid, as the pointer that holds it. */
static Link **link_slot(const Actor *a, term pid) {
    ActorExtra *x = a->extra;
    if (!x) return NULL;
    Link **pl = &x->links;
    while (*pl && (*pl)->peer != pid) pl = &(*pl)->next;
    return *pl ? pl : NULL;
}

/* Removes a's link to pid, freeing its entry and preallocated message,
 * and releases a's charge for it. */
static void link_remove(Vm *vm, Actor *a, term pid) {
    Link **pl = link_slot(a, pid);
    if (!pl) return;
    Link *l = *pl;
    *pl = l->next;
    link_free(vm, l);
    actor_uncharge(a, link_words());
}

bool sig_is_linked(const Actor *a, term pid) {
    return link_slot(a, pid) != NULL;
}

static Link *link_new(Vm *vm, term peer) {
    Link *l = va_alloc(vm, sizeof *l);
    if (!l) return NULL;
    l->next = NULL;
    l->peer = peer;
    l->note = note_new(vm);
    if (!l->note) {
        va_free(vm, l, sizeof *l);
        return NULL;
    }
    return l;
}

/* Exit signal `code` (no function name) from `from` reaching t. */
static bool exit_signal(Vm *vm, Actor *t, term from, uint32_t code) {
    if (t->flags & AF_EXIT_PENDING) return true;
    if ((t->flags & AF_TRAP_EXIT) && code != R_KILLED) {
        Msg *m = note_new(vm);
        if (!m) return false;
        note_fill(m, from, code, T_EMPTYSTR);
        msg_enqueue(vm, t, m, true);
        return true;
    }
    actor_mark_exit(vm, t, code, T_EMPTYSTR);
    return true;
}

bool sig_link(Vm *vm, Actor *a, term pid) {
    if (pid == a->id || sig_is_linked(a, pid)) return true;
    Actor *p = actor_lookup(vm, pid);
    if (!p || p->state == A_EXITED) return exit_signal(vm, a, pid, R_NO_PROC);
    if (!charge_fits(a, link_words())) return false;
    Link *la = link_new(vm, pid), *lp = link_new(vm, a->id);
    ActorExtra *xa = extra_get(vm, a, false), *xp = extra_get(vm, p, false);
    if (!la || !lp || !xa || !xp) {
        link_free(vm, la);
        link_free(vm, lp);
        return false;     /* an extra allocated here stays; freed with its actor */
    }
    la->next = xa->links;
    xa->links = la;
    lp->next = xp->links;
    xp->links = lp;
    xa->charged_words += link_words();
    xp->charged_words += link_words();   /* even past p's limit: see signals.h */
    return true;
}

void sig_unlink(Vm *vm, Actor *a, term pid) {
    link_remove(vm, a, pid);
    Actor *p = actor_lookup(vm, pid);
    if (p) link_remove(vm, p, a->id);
}

bool sig_kill(Vm *vm, Actor *from, term pid, uint32_t code) {
    Actor *t = actor_lookup(vm, pid);
    if (!t || t->state == A_EXITED) return true;
    if (code == R_NORMAL && t != from && !(t->flags & AF_TRAP_EXIT)) return true;
    return exit_signal(vm, t, from->id, code);
}

void sig_set_trap(Actor *a, bool on) {
    if (on) a->flags |= AF_TRAP_EXIT;
    else a->flags = (uint8_t)(a->flags & ~AF_TRAP_EXIT);
}

/* ---- monitors ---- */

/* The entry for `serial` in a list, as the pointer that holds it. */
static Monitor **monitor_slot(Monitor **head, uint64_t serial) {
    Monitor **pm = head;
    while (*pm && (*pm)->serial != serial) pm = &(*pm)->next;
    return *pm ? pm : NULL;
}

/* Removes and frees the entry for `serial` (and its message, if any).
 * False if there was none. */
static bool monitor_remove(Vm *vm, Monitor **head, uint64_t serial) {
    Monitor **pm = monitor_slot(head, serial);
    if (!pm) return false;
    Monitor *m = *pm;
    *pm = m->next;
    monitor_free(vm, m);
    return true;
}

/* Drops w's watching entry for `serial` and, if it had one, releases the
 * monitor's charge (w is the watcher, found by a generation-checked
 * lookup, or NULL). */
static void watcher_forget(Vm *vm, Actor *w, uint64_t serial) {
    if (w && w->extra && monitor_remove(vm, &((ActorExtra *)w->extra)->watching, serial))
        actor_uncharge(w, monitor_words());
}

static Monitor *monitor_new(Vm *vm, term peer, uint64_t serial, Msg *note) {
    Monitor *m = va_alloc(vm, sizeof *m);
    if (!m) return NULL;
    m->next = NULL;
    m->serial = serial;
    m->peer = peer;
    m->note = note;
    return m;
}

term sig_monitor(Vm *vm, Actor *a, term pid) {
    uint64_t serial = vm->next_ref;
    term ref = heap_new_ref(vm, a);
    if (ref == T_NIL) return T_NIL;
    Actor *t = actor_lookup(vm, pid);
    if (t == a) return ref;
    if (!t || t->state == A_EXITED) {
        Msg *m = down_new(vm);
        if (!m) return T_NIL;
        down_set_ref(m, serial, pid);
        down_set_reason(m, R_NO_PROC, T_EMPTYSTR);
        if (a->flags & AF_EXIT_PENDING) msg_free(vm, m);
        else msg_enqueue(vm, a, m, true);
        return ref;
    }
    if (!charge_fits(a, monitor_words())) return T_NIL;
    Msg *note = down_new(vm);
    Monitor *mt = note ? monitor_new(vm, a->id, serial, note) : NULL;
    Monitor *mw = monitor_new(vm, pid, serial, NULL);
    ActorExtra *xa = extra_get(vm, a, false), *xt = extra_get(vm, t, false);
    if (!mt || !mw || !xa || !xt) {
        if (mt) monitor_free(vm, mt);
        else if (note) msg_free(vm, note);
        if (mw) monitor_free(vm, mw);
        return T_NIL;     /* an extra allocated here stays; freed with its actor */
    }
    down_set_ref(note, serial, pid);
    mt->next = xt->monitors;
    xt->monitors = mt;
    mw->next = xa->watching;
    xa->watching = mw;
    xa->charged_words += monitor_words();
    return ref;
}

void sig_demonitor(Vm *vm, Actor *a, uint64_t serial) {
    ActorExtra *x = a->extra;
    if (!x) return;
    Monitor **pm = monitor_slot(&x->watching, serial);
    if (!pm) return;
    Actor *t = actor_lookup(vm, (*pm)->peer);
    if (t && t->extra) monitor_remove(vm, &((ActorExtra *)t->extra)->monitors, serial);
    monitor_remove(vm, &x->watching, serial);
    actor_uncharge(a, monitor_words());
}

/* a is exiting with code/func_name: hand each watcher its preallocated
 * DownMsg (whatever the code, never cascading), dropping the watcher's own
 * entry first. A watcher already marked for exit gets nothing. Every block
 * is queued or freed here exactly once. */
static void signal_monitors(Vm *vm, Actor *a, uint32_t code, term func_name) {
    ActorExtra *x = a->extra;
    Monitor *m = x ? x->monitors : NULL;
    if (x) x->monitors = NULL;
    while (m) {
        Monitor *next = m->next;
        Msg *note = m->note;
        Actor *w = actor_lookup(vm, m->peer);
        watcher_forget(vm, w, m->serial);      /* the charge ends here */
        va_free(vm, m, sizeof *m);
        if (w && !(w->flags & AF_EXIT_PENDING)) {
            down_set_reason(note, code, func_for(vm, w, func_name));
            msg_enqueue(vm, w, note, false);   /* guaranteed: no quota */
        } else {
            msg_free(vm, note);
        }
        m = next;
    }
}

/* a is exiting: it no longer watches anyone, so remove each monitored
 * actor's entry (and free its message). */
static void drop_watching(Vm *vm, Actor *a) {
    ActorExtra *x = a->extra;
    Monitor *m = x ? x->watching : NULL;
    if (x) x->watching = NULL;
    while (m) {
        Monitor *next = m->next;
        Actor *t = actor_lookup(vm, m->peer);
        if (t && t->extra) monitor_remove(vm, &((ActorExtra *)t->extra)->monitors, m->serial);
        va_free(vm, m, sizeof *m);
        actor_uncharge(a, monitor_words());
        m = next;
    }
}

/* a goes away without exiting (actor_extra_free): its watchers' entries
 * and charges go, and no DownMsg is sent. */
static void drop_watchers(Vm *vm, Actor *a) {
    ActorExtra *x = a->extra;
    Monitor *m = x ? x->monitors : NULL;
    if (x) x->monitors = NULL;
    while (m) {
        Monitor *next = m->next;
        watcher_forget(vm, actor_lookup(vm, m->peer), m->serial);
        monitor_free(vm, m);
        m = next;
    }
}

/* a goes away without exiting: each peer's entry for a (and its charge)
 * goes, a's own entries are freed, and no signal is sent. */
static void drop_links(Vm *vm, Actor *a) {
    ActorExtra *x = a->extra;
    Link *l = x ? x->links : NULL;
    if (x) x->links = NULL;
    while (l) {
        Link *next = l->next;
        Actor *p = actor_lookup(vm, l->peer);
        if (p) link_remove(vm, p, a->id);
        link_free(vm, l);
        actor_uncharge(a, link_words());
        l = next;
    }
}

/* a is exiting with code/func_name: hand each linked actor its
 * preallocated ExitMsg if it traps exits, else mark it to exit too (unless
 * code is Normal). Each peer's own entry for a goes first, so no list ever
 * names a dead actor. Every preallocated block is either queued or freed
 * here exactly once. */
static void signal_links(Vm *vm, Actor *a, uint32_t code, term func_name) {
    ActorExtra *x = a->extra;
    Link *l = x ? x->links : NULL;
    if (x) x->links = NULL;
    while (l) {
        Link *next = l->next;
        Msg *note = l->note;
        Actor *p = actor_lookup(vm, l->peer);
        va_free(vm, l, sizeof *l);
        actor_uncharge(a, link_words());
        if (p) link_remove(vm, p, a->id);      /* releases p's charge */
        if (p && (p->flags & AF_TRAP_EXIT) && !(p->flags & AF_EXIT_PENDING)) {
            note_fill(note, a->id, code, func_for(vm, p, func_name));
            msg_enqueue(vm, p, note, false);   /* guaranteed: no quota */
        } else {
            msg_free(vm, note);
            /* The cascade keeps a's func_name, possibly of another module
             * than p's (M-6, intended): it only travels on as a tagged
             * const. p's crash report reads it through the module table
             * (no name once that module is gone), and p's own notices
             * pass it through func_for per receiver, so it never reaches
             * an actor of another module. */
            if (p && code != R_NORMAL) actor_mark_exit(vm, p, code, func_name);
        }
        l = next;
    }
}

/* ---- exit ---- */

void actor_exit(Vm *vm, Actor *a, uint32_t code, term func_name) {
    if (code != R_NORMAL && code != R_SHUTDOWN) report_crash(vm, a, code, func_name);
    a->state = A_EXITED;
    port_close_owned(vm, a->id);       /* before any notification goes out */
    signal_links(vm, a, code, func_name);
    signal_monitors(vm, a, code, func_name);
    drop_watching(vm, a);
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
