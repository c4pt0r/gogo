/* Port layer: the port table, PIDs, the driver interface (a fake driver
 * installed with port_set_drivers), event delivery, debounce, owner exit,
 * and the builtins that reach it. */
#include "test.h"
#include "actor.h"
#include "heap.h"
#include "msg.h"
#include "alloc.h"
#include "image.h"
#include "interp.h"
#include "opcodes.h"
#include "signals.h"
#include "port.h"
#include "evq.h"
#include "config.h"
#include "term.h"
#include <string.h>

extern void (*platform_unix_err_hook)(const char *, size_t);
static void drop_err(const char *s, size_t n) { (void)s; (void)n; }

/* ---- fake driver ---- */

#define CFG_TYPE 0u
#define EVT_TYPE 1u

static struct {
    int opens, closes, cmds, refuse;
    uint16_t debounce;     /* debounce_ms the driver sets in open */
    bool live;             /* can_event result */
    uint16_t index;        /* last open */
    uint32_t n;
    int32_t cfg[8];
    uint32_t cmd_type, cmd_n;
    int32_t cmd[8];
    Port *last;
    const Actor *watch;    /* if set, fk_close records its mailbox length */
    int watch_mbox;
} fk;

static int mbox_len(const Actor *a);

static int fk_open(Port *p, uint16_t index, const int32_t *cfg, uint32_t n) {
    if (fk.refuse) return -1;
    fk.opens++;
    fk.index = index;
    fk.n = n;
    for (uint32_t i = 0; i < n && i < 8u; i++) fk.cfg[i] = cfg[i];
    p->debounce_ms = fk.debounce;
    fk.last = p;
    return 0;
}
static void fk_command(Port *p, uint32_t type, const int32_t *f, uint32_t n) {
    (void)p;
    fk.cmds++;
    fk.cmd_type = type;
    fk.cmd_n = n;
    for (uint32_t i = 0; i < n && i < 8u; i++) fk.cmd[i] = f[i];
}
static void fk_close(Port *p) {
    (void)p;
    fk.closes++;
    if (fk.watch) fk.watch_mbox = mbox_len(fk.watch);
}
static bool fk_can_event(const Port *p) { (void)p; return fk.live; }

static const PortDriver fake = {
    "fake", CFG_TYPE, EVT_TYPE, fk_open, fk_command, fk_close, fk_can_event
};
static const PortDriver *const fake_list[] = { &fake, NULL };

/* An image with one trivial function and the two types (cfg, event). */
#define MINI_WORDS 22
static void mini_image(uint32_t *w, Image *img) {
    memset(w, 0, MINI_WORDS * sizeof(uint32_t));
    w[0] = IMAGE_MAGIC; w[1] = IMAGE_VERSION; w[2] = 1; w[3] = 2;
    w[4] = 6; w[5] = 1; w[6] = 0; w[7] = 0;
    w[8] = 0; w[9] = 0 | (1u << 16); w[10] = 0;              /* func main */
    w[11] = 2; w[12] = 3; w[13] = 2; w[14] = 3;               /* types Cfg/2, Cfg/2 */
    w[15] = hdr_make(3, K_STRING); w[16] = 4; memcpy(&w[17], "main", 4);
    w[18] = hdr_make(3, K_STRING); w[19] = 3; memcpy(&w[20], "Cfg", 3);
    w[21] = OP_MAKE(OP_RET, 0, 0, 0);
    CHECK_EQ(image_load(img, w, MINI_WORDS), IMG_OK);
}

static void setup(uint32_t *w, Image *img, Vm *vm) {
    va_reset();
    evq_reset();
    memset(&fk, 0, sizeof fk);
    port_set_drivers(fake_list);
    mini_image(w, img);
    CHECK_EQ(vm_init(vm, img, 8), 0);
}

static void teardown(void) { port_set_drivers(NULL); }

static Actor *waiting_actor(Vm *vm) {
    Actor *a = actor_new(vm, 64);
    CHECK(a != NULL);
    a->state = A_WAITING;
    return a;
}

/* A heap string / struct of ints on a's heap (no GC: the block has room). */
static term heap_str(Vm *vm, Actor *a, const char *s) {
    uint32_t len = (uint32_t)strlen(s), words = 2u + (len + 3u) / 4u;
    uint32_t off = heap_alloc(vm, a, words, NULL, 0);
    CHECK(off != UINT32_MAX);
    a->mem[off] = hdr_make(words, K_STRING);
    a->mem[off + 1] = len;
    memcpy(a->mem + off + 2, s, len);
    return term_from_ref(off);
}

static term heap_struct(Vm *vm, Actor *a, uint32_t type, const int32_t *f, uint32_t n) {
    uint32_t off = heap_alloc(vm, a, 2u + n, NULL, 0);
    CHECK(off != UINT32_MAX);
    a->mem[off] = hdr_make(2u + n, K_STRUCT);
    a->mem[off + 1] = type;
    for (uint32_t i = 0; i < n; i++) a->mem[off + 2 + i] = term_from_int(f[i]);
    return term_from_ref(off);
}

/* Resets owner's heap first, so the two arguments never trigger a GC. */
static term open_fake(Vm *vm, Actor *owner, int32_t pin) {
    int32_t cfg[2] = { pin, 0 };
    owner->hp = 0;
    return port_open(vm, owner, heap_str(vm, owner, "fake"), heap_struct(vm, owner, CFG_TYPE, cfg, 2));
}

static int mbox_len(const Actor *a) {
    int n = 0;
    for (const Msg *m = a->mbox_head; m; m = m->next) n++;
    return n;
}

/* Field i (0 = a, 1 = b) of the n-th queued event message, as an int. */
static int32_t evt_field(const Actor *a, int nth, int i) {
    const Msg *m = a->mbox_head;
    while (m && nth--) m = m->next;
    CHECK(m != NULL);
    if (!m) return -999;
    CHECK(term_is_ref(m->root) && term_ref_off(m->root) == 0u);
    CHECK_EQ(m->data[0], hdr_make(4, K_STRUCT));
    CHECK_EQ(m->data[1], EVT_TYPE);
    term t = m->data[2 + i];
    if (term_is_int(t)) return term_to_int(t);
    CHECK(term_is_ref(t) && term_ref_off(t) + 1u < m->words);
    CHECK_EQ(hdr_kind(m->data[term_ref_off(t)]), K_INT);
    return (int32_t)m->data[term_ref_off(t) + 1];
}

static void push(uint16_t port, uint16_t gen, int32_t a, int32_t b, uint32_t t) {
    PortEvent e = { port, gen, a, b, t };
    CHECK(evq_push(&e));
}

static void clear_runq(Vm *vm) { while (runq_pop(vm)) {} }

/* ---- tests ---- */

TEST(port_open_close) {
    uint32_t w[MINI_WORDS]; Image img; Vm vm;
    setup(w, &img, &vm);
    Actor *a = waiting_actor(&vm);
    term p = open_fake(&vm, a, 25);
    CHECK(port_is(p));
    CHECK(term_is_pid(p));
    CHECK(term_pid_slot(p) >= PORT_SLOT_BASE);
    CHECK_EQ(term_pid_slot(p) - PORT_SLOT_BASE, fk.index);
    CHECK(actor_lookup(&vm, p) == NULL);
    CHECK(!port_is(a->id));
    CHECK_EQ(fk.opens, 1);
    CHECK_EQ(fk.n, 2u);
    CHECK_EQ(fk.cfg[0], 25);
    CHECK(fk.last->owner == a->id && fk.last->id == p);

    port_close(&vm, p);
    CHECK_EQ(fk.closes, 1);
    port_close(&vm, p);                       /* stale: ignored */
    CHECK_EQ(fk.closes, 1);
    int32_t one = 1;
    port_command(&vm, a, p, heap_struct(&vm, a, 7, &one, 1));
    CHECK_EQ(fk.cmds, 0);                     /* stale: ignored */

    term q = open_fake(&vm, a, 26);           /* same slot, next generation */
    CHECK(q != T_NIL && q != p);
    CHECK_EQ(term_pid_slot(q), term_pid_slot(p));
    CHECK_EQ(term_pid_gen(q), (term_pid_gen(p) + 1u) & 0xFFFu);
    port_close(&vm, p);                       /* old PID does not hit the new port */
    CHECK_EQ(fk.closes, 1);

    /* Not linkable: Link to a port PID is a NoProc signal, as for a dead PID. */
    CHECK(sig_link(&vm, a, q));
    uint32_t code; term fn;
    CHECK(actor_exit_pending(a, &code, &fn));
    CHECK_EQ(code, R_NO_PROC);
    clear_runq(&vm);
    platform_unix_err_hook = drop_err;
    actor_exit(&vm, a, code, fn);             /* closes q */
    platform_unix_err_hook = NULL;
    CHECK_EQ(fk.closes, 2);
    CHECK(!port_any_live(&vm));
    teardown();
}

TEST(port_open_fails) {
    uint32_t w[MINI_WORDS]; Image img; Vm vm;
    setup(w, &img, &vm);
    Actor *a = waiting_actor(&vm);
    int32_t cfg[2] = { 1, 2 };
    /* Unknown driver (also a prefix of a known name). */
    CHECK(port_open(&vm, a, heap_str(&vm, a, "nope"), heap_struct(&vm, a, CFG_TYPE, cfg, 2)) == T_NIL);
    CHECK(port_open(&vm, a, heap_str(&vm, a, "fak"), heap_struct(&vm, a, CFG_TYPE, cfg, 2)) == T_NIL);
    CHECK(port_open(&vm, a, T_EMPTYSTR, heap_struct(&vm, a, CFG_TYPE, cfg, 2)) == T_NIL);
    /* Wrong cfg type, or not a struct. */
    CHECK(port_open(&vm, a, heap_str(&vm, a, "fake"), heap_struct(&vm, a, EVT_TYPE, cfg, 2)) == T_NIL);
    CHECK(port_open(&vm, a, heap_str(&vm, a, "fake"), term_from_int(3)) == T_NIL);
    CHECK_EQ(fk.opens, 0);
    /* Driver refusal. */
    fk.refuse = 1;
    CHECK(open_fake(&vm, a, 1) == T_NIL);
    fk.refuse = 0;
    CHECK(!actor_port_owns(a));
    /* Table full. */
    for (uint32_t i = 0; i < GOGO_MAX_PORTS; i++) CHECK(open_fake(&vm, a, (int32_t)i) != T_NIL);
    CHECK(open_fake(&vm, a, 99) == T_NIL);
    CHECK_EQ(fk.opens, (int)GOGO_MAX_PORTS);
    actor_exit(&vm, a, R_NORMAL, T_EMPTYSTR);
    CHECK_EQ(fk.closes, (int)GOGO_MAX_PORTS);
    teardown();
}

TEST(port_owner_exit) {
    uint32_t w[MINI_WORDS]; Image img; Vm vm;
    setup(w, &img, &vm);
    size_t base = va_used_bytes();
    Actor *a = waiting_actor(&vm), *b = waiting_actor(&vm);
    term p1 = open_fake(&vm, a, 1);
    term p2 = open_fake(&vm, a, 2);
    CHECK(p1 != T_NIL && p2 != T_NIL);
    CHECK(actor_port_owns(a));
    CHECK(!actor_port_owns(b));
    platform_unix_err_hook = drop_err;
    actor_exit(&vm, a, R_KILLED, T_EMPTYSTR);
    platform_unix_err_hook = NULL;
    CHECK_EQ(fk.closes, 2);
    /* Slot reusable by another actor (a restarted child). */
    term p3 = open_fake(&vm, b, 1);
    CHECK(p3 != T_NIL);
    CHECK_EQ(term_pid_slot(p3), term_pid_slot(p1));
    CHECK(p3 != p1);
    actor_exit(&vm, b, R_NORMAL, T_EMPTYSTR);
    CHECK_EQ(fk.closes, 3);
    CHECK_EQ(va_used_bytes(), base);          /* extras freed */
    teardown();
}

/* port_close_all closes every open port (VM shutdown), whoever owns it. */
TEST(port_close_all) {
    uint32_t w[MINI_WORDS]; Image img; Vm vm;
    setup(w, &img, &vm);
    Actor *a = waiting_actor(&vm), *b = waiting_actor(&vm);
    term p1 = open_fake(&vm, a, 1);
    term p2 = open_fake(&vm, b, 2);
    CHECK(p1 != T_NIL && p2 != T_NIL);
    port_close_all(&vm);
    CHECK_EQ(fk.closes, 2);
    CHECK(!actor_port_owns(a) && !actor_port_owns(b));
    CHECK(!port_any_live(&vm));
    port_close_all(&vm);                      /* nothing left: no-op */
    CHECK_EQ(fk.closes, 2);
    port_close(&vm, p1);                      /* stale */
    CHECK_EQ(fk.closes, 2);
    CHECK(open_fake(&vm, a, 1) != T_NIL);     /* slots reusable */
    teardown();
}

/* Closing ports at exit happens before the exit notifications: a linked
 * trapping actor that gets the ExitMsg can reopen the pin at once. */
TEST(port_owner_exit_before_notify) {
    uint32_t w[MINI_WORDS]; Image img; Vm vm;
    setup(w, &img, &vm);
    Actor *a = waiting_actor(&vm), *s = waiting_actor(&vm);
    sig_set_trap(s, true);
    CHECK(sig_link(&vm, s, a->id));
    CHECK(open_fake(&vm, a, 1) != T_NIL);
    fk.watch = s;
    fk.watch_mbox = -1;
    platform_unix_err_hook = drop_err;
    actor_exit(&vm, a, 100, T_EMPTYSTR);
    platform_unix_err_hook = NULL;
    CHECK_EQ(fk.closes, 1);
    CHECK_EQ(fk.watch_mbox, 0);               /* closed before the ExitMsg went out */
    CHECK_EQ(mbox_len(s), 1);
    fk.watch = NULL;
    clear_runq(&vm);
    actor_exit(&vm, s, R_NORMAL, T_EMPTYSTR);
    teardown();
}

TEST(port_stale_events) {
    uint32_t w[MINI_WORDS]; Image img; Vm vm;
    setup(w, &img, &vm);
    Actor *a = waiting_actor(&vm), *b = waiting_actor(&vm);
    term p = open_fake(&vm, a, 1);
    uint16_t idx = (uint16_t)(term_pid_slot(p) - PORT_SLOT_BASE);
    push(idx, term_pid_gen(p), 1, 1, 0);
    port_close(&vm, p);
    term q = open_fake(&vm, b, 1);
    CHECK_EQ(term_pid_slot(q), term_pid_slot(p));
    push(GOGO_MAX_PORTS, 0, 1, 1, 0);         /* out-of-range index: dropped */
    port_drain(&vm, 5);
    CHECK_EQ(mbox_len(a), 0);
    CHECK_EQ(mbox_len(b), 0);
    /* A current event does reach b. */
    push(idx, term_pid_gen(q), 1, 0, 6);
    port_drain(&vm, 6);
    CHECK_EQ(mbox_len(b), 1);
    CHECK_EQ(evt_field(b, 0, 0), 1);
    CHECK_EQ(evt_field(b, 0, 1), 0);
    clear_runq(&vm);
    actor_exit(&vm, a, R_NORMAL, T_EMPTYSTR);
    actor_exit(&vm, b, R_NORMAL, T_EMPTYSTR);
    teardown();
}

TEST(port_command_any_sender) {
    uint32_t w[MINI_WORDS]; Image img; Vm vm;
    setup(w, &img, &vm);
    Actor *a = waiting_actor(&vm), *b = waiting_actor(&vm);
    term p = open_fake(&vm, a, 1);
    int32_t f[3] = { 1, -5, 2000000000 };     /* the last one is boxed */
    uint32_t off = heap_alloc(&vm, b, 2, NULL, 0);
    b->mem[off] = hdr_make(2, K_INT);
    b->mem[off + 1] = (uint32_t)f[2];
    term cmd = heap_struct(&vm, b, 7, f, 3);
    b->mem[term_ref_off(cmd) + 4] = term_from_ref(off);
    uint16_t hp = b->hp, mbox = b->mbox_words;
    port_command(&vm, b, p, cmd);
    CHECK_EQ(fk.cmds, 1);
    CHECK_EQ(fk.cmd_type, 7u);
    CHECK_EQ(fk.cmd_n, 3u);
    CHECK_EQ(fk.cmd[0], 1);
    CHECK_EQ(fk.cmd[1], -5);
    CHECK_EQ(fk.cmd[2], 2000000000);
    CHECK_EQ(b->hp, hp);                      /* no copy, no mailbox */
    CHECK_EQ(b->mbox_words, mbox);
    CHECK_EQ(a->mbox_words, 0);
    /* Not a struct of ints: ignored. */
    port_command(&vm, b, p, term_from_int(1));
    port_command(&vm, b, p, heap_str(&vm, b, "x"));
    uint32_t o2 = heap_alloc(&vm, b, 3, NULL, 0);
    b->mem[o2] = hdr_make(3, K_STRUCT); b->mem[o2 + 1] = 7; b->mem[o2 + 2] = T_TRUE;
    port_command(&vm, b, p, term_from_ref(o2));
    CHECK_EQ(fk.cmds, 1);
    actor_exit(&vm, a, R_NORMAL, T_EMPTYSTR);
    actor_exit(&vm, b, R_NORMAL, T_EMPTYSTR);
    teardown();
}

TEST(port_debounce_burst) {
    uint32_t w[MINI_WORDS]; Image img; Vm vm;
    setup(w, &img, &vm);
    fk.debounce = 20;
    Actor *a = waiting_actor(&vm);
    term p = open_fake(&vm, a, 1);
    uint16_t idx = (uint16_t)(term_pid_slot(p) - PORT_SLOT_BASE), gen = term_pid_gen(p);
    uint64_t d;
    CHECK(!port_next_deadline(&vm, &d));
    /* 5 edges within 20 ms: first delivered at once. */
    push(idx, gen, 1, 1, 0);
    push(idx, gen, 1, 0, 4);
    push(idx, gen, 1, 1, 8);
    push(idx, gen, 1, 1, 12);
    push(idx, gen, 1, 0, 16);
    port_drain(&vm, 16);
    CHECK_EQ(mbox_len(a), 1);
    CHECK_EQ(evt_field(a, 0, 1), 1);
    CHECK(port_next_deadline(&vm, &d));
    CHECK_EQ(d, 20);
    port_drain(&vm, 19);
    CHECK_EQ(mbox_len(a), 1);
    /* The final level, which differs, is delivered once when the window ends. */
    port_drain(&vm, 20);
    CHECK_EQ(mbox_len(a), 2);
    CHECK_EQ(evt_field(a, 1, 1), 0);
    CHECK(!port_next_deadline(&vm, &d));
    port_drain(&vm, 100);
    CHECK_EQ(mbox_len(a), 2);
    /* A burst ending on the delivered level sends nothing more. */
    push(idx, gen, 1, 1, 200);
    push(idx, gen, 1, 0, 205);
    push(idx, gen, 1, 1, 210);
    port_drain(&vm, 210);
    CHECK_EQ(mbox_len(a), 3);
    CHECK(port_next_deadline(&vm, &d));
    CHECK_EQ(d, 220);
    port_drain(&vm, 230);
    CHECK_EQ(mbox_len(a), 3);
    CHECK(!port_next_deadline(&vm, &d));
    /* A pending trailing value is flushed before a later event in the ring. */
    push(idx, gen, 1, 0, 300);
    push(idx, gen, 1, 1, 310);
    push(idx, gen, 1, 0, 400);
    port_drain(&vm, 400);
    CHECK_EQ(mbox_len(a), 6);
    CHECK_EQ(evt_field(a, 3, 1), 0);
    CHECK_EQ(evt_field(a, 4, 1), 1);
    CHECK_EQ(evt_field(a, 5, 1), 0);
    /* Close drops the pending trailing value. */
    push(idx, gen, 1, 1, 500);
    push(idx, gen, 1, 0, 505);
    port_drain(&vm, 505);
    CHECK(port_next_deadline(&vm, &d));
    port_close(&vm, p);
    CHECK(!port_next_deadline(&vm, &d));
    clear_runq(&vm);
    actor_exit(&vm, a, R_NORMAL, T_EMPTYSTR);
    teardown();
}

TEST(port_debounce_off) {
    uint32_t w[MINI_WORDS]; Image img; Vm vm;
    setup(w, &img, &vm);
    Actor *a = waiting_actor(&vm);
    term p = open_fake(&vm, a, 1);
    uint16_t idx = (uint16_t)(term_pid_slot(p) - PORT_SLOT_BASE), gen = term_pid_gen(p);
    for (int i = 0; i < 5; i++) push(idx, gen, 9, i & 1, (uint32_t)i);
    push(idx, gen, 2147483647, -2147483647 - 1, 5);   /* boxed fields */
    port_drain(&vm, 5);
    CHECK_EQ(mbox_len(a), 6);
    for (int i = 0; i < 5; i++) {
        CHECK_EQ(evt_field(a, i, 0), 9);
        CHECK_EQ(evt_field(a, i, 1), i & 1);
    }
    CHECK_EQ(evt_field(a, 5, 0), 2147483647);
    CHECK_EQ(evt_field(a, 5, 1), -2147483647 - 1);
    uint64_t d;
    CHECK(!port_next_deadline(&vm, &d));
    CHECK(runq_pop(&vm) == a);                /* the first event woke it */
    CHECK(runq_pop(&vm) == NULL);
    /* A received event is an ordinary struct on the owner's heap. */
    term t;
    CHECK(msg_receive(&vm, a, &t));
    const uint32_t *o = a->mem + term_ref_off(t);
    CHECK_EQ(o[1], EVT_TYPE);
    CHECK_EQ(term_to_int(o[2]), 9);
    actor_exit(&vm, a, R_NORMAL, T_EMPTYSTR);
    teardown();
}

TEST(port_quota) {
    uint32_t w[MINI_WORDS]; Image img; Vm vm;
    setup(w, &img, &vm);
    Actor *a = waiting_actor(&vm);
    term p = open_fake(&vm, a, 1);
    a->max_words = 96;                        /* 64-word block + a few messages */
    uint16_t idx = (uint16_t)(term_pid_slot(p) - PORT_SLOT_BASE), gen = term_pid_gen(p);
    for (int i = 0; i < 20; i++) push(idx, gen, 1, i & 1, (uint32_t)i);
    port_drain(&vm, 20);
    uint32_t code; term fn;
    CHECK(actor_exit_pending(a, &code, &fn));
    CHECK_EQ(code, R_MAILBOX_OVERFLOW);
    CHECK(mbox_len(a) > 0 && mbox_len(a) < 20);
    clear_runq(&vm);
    platform_unix_err_hook = drop_err;
    actor_exit(&vm, a, code, fn);
    platform_unix_err_hook = NULL;
    CHECK_EQ(fk.closes, 1);
    teardown();
}

TEST(port_any_live) {
    uint32_t w[MINI_WORDS]; Image img; Vm vm;
    setup(w, &img, &vm);
    Actor *a = waiting_actor(&vm);
    CHECK(!port_any_live(&vm));
    term p = open_fake(&vm, a, 1);
    CHECK(!port_any_live(&vm));               /* an output: cannot produce events */
    fk.live = true;
    CHECK(port_any_live(&vm));
    port_close(&vm, p);
    CHECK(!port_any_live(&vm));
    actor_exit(&vm, a, R_NORMAL, T_EMPTYSTR);
    teardown();
}

TEST(vm_init_rejects_big_table) {
    uint32_t w[MINI_WORDS]; Image img; Vm vm;
    setup(w, &img, &vm);
    size_t base = va_used_bytes();
    CHECK_EQ(vm_init(&vm, &img, (uint16_t)(PORT_SLOT_BASE + 1u)), -1);
    CHECK_EQ(vm_init(&vm, &img, 0xFFFFu), -1);
    CHECK_EQ(va_used_bytes(), base);
    teardown();
}

/* The builtins: PORT_OPEN, Send and Kill to a port PID, PORT_CLOSE.
 *   r0 = "fake"; r2 = 7; r1 = Cfg{r2}; r3, r4 = PORT_OPEN(r0, r1)
 *   r5 = Cmd{r2}; SEND(r3, r5); KILL(r3, 1)
 *   r3, r4 = PORT_OPEN(r0, r1); PORT_CLOSE(r3); RET */
#define BI_WORDS 50
TEST(port_builtins) {
    uint32_t w[BI_WORDS]; Image img; Vm vm;
    va_reset();
    evq_reset();
    memset(&fk, 0, sizeof fk);
    port_set_drivers(fake_list);
    memset(w, 0, sizeof w);
    w[0] = IMAGE_MAGIC; w[1] = IMAGE_VERSION; w[2] = 1; w[3] = 2;
    w[4] = 12; w[5] = 23; w[6] = 0; w[7] = 0;
    w[8] = 0; w[9] = 0 | (9u << 16); w[10] = 0;              /* main, 9 regs */
    w[11] = 1; w[12] = 6; w[13] = 1; w[14] = 9;               /* Cfg/1, Cmd/1 */
    w[15] = hdr_make(3, K_STRING); w[16] = 4; memcpy(&w[17], "main", 4);
    w[18] = hdr_make(3, K_STRING); w[19] = 4; memcpy(&w[20], "fake", 4);
    w[21] = hdr_make(3, K_STRING); w[22] = 3; memcpy(&w[23], "Cfg", 3);
    w[24] = hdr_make(3, K_STRING); w[25] = 3; memcpy(&w[26], "Cmd", 3);
    uint32_t *c = w + 27;
    c[0] = OP_MAKE(OP_LOADK, 0, 0, 0); c[1] = term_from_const(3);
    c[2] = OP_MAKE(OP_LOADK, 2, 0, 0); c[3] = term_from_int(7);
    c[4] = OP_MAKE(OP_NEWSTRUCT, 1, 2, 1); c[5] = 0;
    c[6] = OP_MAKE(OP_BUILTIN, 3, 0, 2); c[7] = B_PORT_OPEN;
    c[8] = OP_MAKE(OP_NEWSTRUCT, 5, 2, 1); c[9] = 1;
    c[10] = OP_MAKE(OP_MOVE, 6, 3, 0);
    c[11] = OP_MAKE(OP_MOVE, 7, 5, 0);
    c[12] = OP_MAKE(OP_BUILTIN, 8, 6, 2); c[13] = B_SEND;
    c[14] = OP_MAKE(OP_LOADK, 7, 0, 0); c[15] = term_from_int(1);
    c[16] = OP_MAKE(OP_BUILTIN, 8, 6, 2); c[17] = B_KILL;
    c[18] = OP_MAKE(OP_BUILTIN, 3, 0, 2); c[19] = B_PORT_OPEN;
    c[20] = OP_MAKE(OP_BUILTIN, 8, 3, 1); c[21] = B_PORT_CLOSE;
    c[22] = OP_MAKE(OP_RET, 0, 0, 0);
    CHECK_EQ(image_load(&img, w, BI_WORDS), IMG_OK);
    CHECK_EQ(vm_init(&vm, &img, 8), 0);
    CHECK_EQ(vm_run(&vm), 0);
    CHECK_EQ(fk.opens, 2);
    CHECK_EQ(fk.cfg[0], 7);
    CHECK_EQ(fk.cmds, 1);
    CHECK_EQ(fk.cmd_type, 1u);
    CHECK_EQ(fk.cmd[0], 7);
    CHECK_EQ(fk.closes, 2);
    CHECK_EQ(vm.live, 0u);
    teardown();
}
