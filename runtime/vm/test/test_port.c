#define _POSIX_C_SOURCE 200809L
/* Port layer: the port table, PIDs, the driver interface (a fake driver
 * installed with port_set_drivers), event delivery, owner exit,
 * and the builtins that reach it. */
#include "test.h"
#include "test_host.h"
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
#include <pthread.h>
#include "gpio_unix.h"
#include "bytes.h"

extern void (*platform_unix_err_hook)(const char *, size_t);
static void drop_err(const char *s, size_t n) { (void)s; (void)n; }

/* ---- fake driver ---- */

#define CFG_TYPE 0u
#define EVT_TYPE 1u

static struct {
    int opens, closes, cmds, refuse, handoffs;
    uint16_t charge;       /* charge_words the driver sets in open */
    uint8_t room;          /* room_64w the driver sets in open */
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

static int fk_open(struct Vm *vm, Port *p, uint16_t index, const int32_t *cfg, uint32_t n) {
    (void)vm;
    if (fk.refuse) return -1;
    fk.opens++;
    fk.index = index;
    fk.n = n;
    for (uint32_t i = 0; i < n && i < 8u; i++) fk.cfg[i] = cfg[i];
    p->charge_words = fk.charge;
    p->room_64w = fk.room;
    fk.last = p;
    return 0;
}
static void fk_command(struct Vm *vm, Port *p, uint32_t type, const int32_t *f, uint32_t n) { (void)vm;
    (void)p;
    fk.cmds++;
    fk.cmd_type = type;
    fk.cmd_n = n;
    for (uint32_t i = 0; i < n && i < 8u; i++) fk.cmd[i] = f[i];
}
static void fk_close(struct Vm *vm, Port *p) {
    (void)vm;
    (void)p;
    fk.closes++;
    if (fk.watch) fk.watch_mbox = mbox_len(fk.watch);
}
static bool fk_can_event(const struct Vm *vm, const Port *p) { (void)vm; (void)p; return fk.live; }
static void fk_handoff(Vm *vm, Port *p) { (void)vm; (void)p; fk.handoffs++; }

static const PortDriver fake = {
    .name = "fake", .cfg_type = CFG_TYPE, .event_type = EVT_TYPE, .open = fk_open, .command = fk_command,
    .close = fk_close, .can_event = fk_can_event, .handoff = fk_handoff
};
static const PortDriver *const fake_list[] = { &fake, NULL };

/* An image with one trivial function and the two types (cfg, event). */
#define MINI_WORDS 25
static void mini_image(uint32_t *w, Image *img) {
    memset(w, 0, MINI_WORDS * sizeof(uint32_t));
    w[0] = IMAGE_MAGIC; w[1] = IMAGE_VERSION; w[2] = 1; w[3] = 2;
    w[4] = 6; w[5] = 1; w[6] = 0; w[7] = 0;
    w[8] = 0; w[9] = 0 | (1u << 16); w[10] = 0;              /* func main */
    w[11] = 2; w[12] = 3; w[13] = 2; w[14] = 3;               /* types Cfg/2, Cfg/2 */
    w[15] = hdr_make(3, K_STRING); w[16] = 4; memcpy(&w[17], "main", 4);
    w[18] = hdr_make(3, K_STRING); w[19] = 3; memcpy(&w[20], "Cfg", 3);
    w[21] = OP_MAKE(OP_RET, 0, 0, 0);
    CHECK_EQ(image_load(img, w, img_seal(w)), IMG_OK);
}

static Vm *cur_vm;   /* push()'s ring */

static void setup(uint32_t *w, Image *img, Vm *vm) {
    va_reset(vm);
    memset(&fk, 0, sizeof fk);
    mini_image(w, img);
    CHECK_EQ(test_vm_init(vm, img, 8), 0);
    evq_reset(vm);
    port_set_drivers(vm, fake_list);
    cur_vm = vm;
}

static void teardown(void) { cur_vm = NULL; }

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
    CHECK(evq_push(cur_vm, &e));
}

static void clear_runq(Vm *vm) { while (runq_pop(vm)) {} }

/* ---- tests ---- */

TEST(port_open_close) {
    uint32_t w[MINI_WORDS]; Image img; Vm vm;
    setup(w, &img, &vm);
    Actor *a = waiting_actor(&vm);
    term p = open_fake(&vm, a, 25);
    CHECK(test_port_is(p));
    CHECK(term_is_pid(p));
    CHECK(term_pid_slot(p) >= PORT_SLOT_BASE);
    CHECK_EQ(term_pid_slot(p) - PORT_SLOT_BASE, fk.index);
    CHECK(actor_lookup(&vm, p) == NULL);
    CHECK(!test_port_is(a->id));
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
    /* Unknown driver (also a prefix of a known name), or a name that is
     * no string: NotFound in vm->open_err. */
    CHECK(port_open(&vm, a, heap_str(&vm, a, "nope"), heap_struct(&vm, a, CFG_TYPE, cfg, 2)) == T_NIL);
    CHECK_EQ(vm.open_err, GOGO_E_NOTFOUND);
    CHECK(port_open(&vm, a, heap_str(&vm, a, "fak"), heap_struct(&vm, a, CFG_TYPE, cfg, 2)) == T_NIL);
    CHECK_EQ(vm.open_err, GOGO_E_NOTFOUND);
    CHECK(port_open(&vm, a, T_EMPTYSTR, heap_struct(&vm, a, CFG_TYPE, cfg, 2)) == T_NIL);
    CHECK_EQ(vm.open_err, GOGO_E_NOTFOUND);
    CHECK(port_open(&vm, a, term_from_int(7), heap_struct(&vm, a, CFG_TYPE, cfg, 2)) == T_NIL);
    CHECK_EQ(vm.open_err, GOGO_E_NOTFOUND);
    /* Wrong cfg type, or not a struct: BadArg. */
    CHECK(port_open(&vm, a, heap_str(&vm, a, "fake"), heap_struct(&vm, a, EVT_TYPE, cfg, 2)) == T_NIL);
    CHECK_EQ(vm.open_err, GOGO_E_BADARG);
    CHECK(port_open(&vm, a, heap_str(&vm, a, "fake"), term_from_int(3)) == T_NIL);
    CHECK_EQ(vm.open_err, GOGO_E_BADARG);
    CHECK_EQ(fk.opens, 0);
    /* Driver refusal: Busy. */
    fk.refuse = 1;
    CHECK(open_fake(&vm, a, 1) == T_NIL);
    CHECK_EQ(vm.open_err, GOGO_E_BUSY);
    fk.refuse = 0;
    CHECK(!actor_port_owns(a));
    /* Table full: TooMany. */
    for (uint32_t i = 0; i < GOGO_MAX_PORTS; i++) CHECK(open_fake(&vm, a, (int32_t)i) != T_NIL);
    CHECK(open_fake(&vm, a, 99) == T_NIL);
    CHECK_EQ(vm.open_err, GOGO_E_TOOMANY);
    CHECK_EQ(fk.opens, (int)GOGO_MAX_PORTS);
    actor_exit(&vm, a, R_NORMAL, T_EMPTYSTR);
    CHECK_EQ(fk.closes, (int)GOGO_MAX_PORTS);
    teardown();
}

/* The first 16 ports live in the Vm, as the fixed table did: opening
 * them takes nothing from the allocator (a board's actor arena); a
 * larger table's further chunks are allocated as they are reached. */
TEST(port_table_first_chunk_in_vm) {
    uint32_t w[MINI_WORDS]; Image img; Vm vm;
    setup(w, &img, &vm);
    Actor *a = waiting_actor(&vm);
    CHECK(open_fake(&vm, a, 1) != T_NIL);
    CHECK(vm.port_chunk == &vm.port_chunk1 && vm.port_chunk[0] == vm.port_first);
    size_t base = va_used_bytes(&vm);
    for (int i = 1; i < 16; i++) CHECK(open_fake(&vm, a, 1) != T_NIL);
    CHECK_EQ(va_used_bytes(&vm), base);
    teardown();
}

TEST(port_owner_exit) {
    uint32_t w[MINI_WORDS]; Image img; Vm vm;
    setup(w, &img, &vm);
    Actor *c = waiting_actor(&vm);
    port_close(&vm, open_fake(&vm, c, 1));
    actor_exit(&vm, c, R_NORMAL, T_EMPTYSTR);
    fk.closes = 0;
    size_t base = va_used_bytes(&vm);
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
    CHECK_EQ(va_used_bytes(&vm), base);          /* extras freed */
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

TEST(port_events_in_order) {
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
    Vm big;                                 /* each Vm has its own allocator: nothing allocated */
    CHECK_EQ(test_vm_init(&big, &img, (uint16_t)(PORT_SLOT_BASE + 1u)), -1);
    CHECK_EQ(va_used_bytes(&big), 0u);
    CHECK_EQ(test_vm_init(&big, &img, 0xFFFFu), -1);
    CHECK_EQ(va_used_bytes(&big), 0u);
    teardown();
}

/* The builtins: PORT_OPEN, Send and Kill to a port PID, PORT_CLOSE.
 *   r0 = "fake"; r2 = 7; r1 = Cfg{r2}; r3, r4 = PORT_OPEN(r0, r1)
 *   r5 = Cmd{r2}; SEND(r3, r5); KILL(r3, 1)
 *   r3, r4 = PORT_OPEN(r0, r1); PORT_CLOSE(r3); RET */
#define BI_WORDS 55
TEST(port_builtins) {
    uint32_t w[BI_WORDS]; Image img; Vm vm;
    va_reset(&vm);
    memset(&fk, 0, sizeof fk);
    memset(w, 0, sizeof w);
    w[0] = IMAGE_MAGIC; w[1] = IMAGE_VERSION; w[2] = 1; w[3] = 2;
    w[4] = 12; w[5] = 23; w[6] = 0; w[7] = 2;                 /* both rows reserved: types 0, 1 */
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
    CHECK_EQ(image_load(&img, w, img_seal_main(w)), IMG_OK);
    CHECK_EQ(test_vm_init(&vm, &img, 8), 0);
    port_set_drivers(&vm, fake_list);
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

/* ---- bounded drain ---- */

static volatile int flood_done;
static Vm *flood_vm;

static void *flood(void *arg) {
    uint16_t idx = (uint16_t)(((uintptr_t)arg) >> 16), gen = (uint16_t)((uintptr_t)arg & 0xFFFFu);
    for (int32_t i = 0; i < 20000; i++) {
        PortEvent e = { idx, gen, 1, i & 1, 0 };
        while (!evq_push(flood_vm, &e)) {}   /* keep the ring full: retry when it is */
    }
    flood_done = 1;
    return NULL;
}

TEST(port_drain_bounded) {
    uint32_t w[MINI_WORDS]; Image img; Vm vm;
    setup(w, &img, &vm);
    Actor *a = waiting_actor(&vm);
    a->max_words = 65535u;
    term p = open_fake(&vm, a, 1);
    uint16_t idx = (uint16_t)(term_pid_slot(p) - PORT_SLOT_BASE);
    uintptr_t arg = ((uintptr_t)idx << 16) | term_pid_gen(p);
    pthread_t th;
    flood_done = 0;
    flood_vm = &vm;
    CHECK_EQ(pthread_create(&th, NULL, flood, (void *)arg), 0);
    for (int spin = 0; spin < 100000000 && evq_empty(&vm); spin++) {}
    port_drain(&vm, 0);                       /* a producer keeps refilling the ring */
    CHECK(mbox_len(a) <= (int)GOGO_EVQ_CAP);
    CHECK(mbox_len(a) > 0);
    clear_runq(&vm);
    PortEvent e;
    while (!flood_done) { while (evq_pop(&vm, &e)) {} }   /* let the producer finish */
    pthread_join(th, NULL);
    actor_exit(&vm, a, R_NORMAL, T_EMPTYSTR);
    teardown();
}

/* An input pin with Edge None can never produce events: not live. */
TEST(gpio_unix_edge_none_not_live) {
    Port p;
    Vm vm;
    CHECK_EQ(test_vm_init(&vm, NULL, 16), 0);
    int32_t none[4] = { 5, 1, 0, 0 }, both[4] = { 6, 1, 3, 0 }, out[4] = { 7, 3, 0, 0 };
    memset(&p, 0, sizeof p);
    p.drv = &gpio_unix_driver;
    CHECK_EQ(gpio_unix_driver.open(&vm, &p, 0, none, 4), 0);
    CHECK(!gpio_unix_driver.can_event(&vm, &p));
    gpio_unix_driver.close(&vm, &p);
    memset(&p, 0, sizeof p);
    p.drv = &gpio_unix_driver;
    CHECK_EQ(gpio_unix_driver.open(&vm, &p, 0, both, 4), 0);
    CHECK(gpio_unix_driver.can_event(&vm, &p));
    gpio_unix_driver.close(&vm, &p);
    memset(&p, 0, sizeof p);
    p.drv = &gpio_unix_driver;
    CHECK_EQ(gpio_unix_driver.open(&vm, &p, 0, out, 4), 0);
    CHECK(!gpio_unix_driver.can_event(&vm, &p));
    gpio_unix_driver.close(&vm, &p);
}

/* ---- Handoff, port-held Bytes, extra sources (M5) ---- */

/* Handoff moves the port, its driver-state charge and the Bytes it pins
 * from the owner to another actor; only the owner may hand off. */
TEST(handoff_moves_charge) {
    uint32_t w[MINI_WORDS]; Image img; Vm vm;
    setup(w, &img, &vm);
    Actor *a = waiting_actor(&vm), *b = waiting_actor(&vm), *c = waiting_actor(&vm);
    fk.charge = 16;
    term p = open_fake(&vm, a, 1);
    CHECK(p != T_NIL);
    Port *pt = fk.last;
    CHECK_EQ(actor_extra_charged(a), 16u);
    uint8_t *d = NULL;
    term h = port_bytes_new(&vm, pt, 200, &d);
    CHECK(h != T_NIL && d != NULL);
    CHECK_EQ(bytes_held_bytes(a), 200u);
    CHECK_EQ(bytes_held_count(a), 1u);

    CHECK(!port_handoff(&vm, b, p, c->id));    /* not the owner */
    CHECK(!port_handoff(&vm, a, p, p));         /* to a port */
    CHECK(!port_handoff(&vm, a, a->id, b->id));       /* not a port pid */
    CHECK_EQ(fk.handoffs, 0);
    CHECK(pt->owner == a->id);

    uint32_t list = bytes_list_words(b);       /* b's new holder list */
    CHECK(list > 0u);
    CHECK(port_handoff(&vm, a, p, b->id));
    CHECK_EQ(fk.handoffs, 1);
    CHECK(pt->owner == b->id);
    CHECK_EQ(actor_extra_charged(a), 0u);
    CHECK_EQ(bytes_held_bytes(a), 0u);
    CHECK_EQ(bytes_held_count(a), 0u);
    CHECK(!actor_port_owns(a));
    CHECK_EQ(actor_extra_charged(b), 16u + list);
    CHECK_EQ(bytes_held_bytes(b), 200u);
    CHECK(actor_port_owns(b));
    CHECK(bytes_valid(&vm, h));

    /* Events now go to b. */
    uint16_t idx = (uint16_t)(term_pid_slot(p) - PORT_SLOT_BASE);
    push(idx, term_pid_gen(p), 3, 4, 0);
    port_drain(&vm, 1);
    CHECK_EQ(mbox_len(a), 0);
    CHECK_EQ(mbox_len(b), 1);

    /* a exiting no longer closes it; b exiting does, releasing all. */
    clear_runq(&vm);
    actor_exit(&vm, a, R_NORMAL, T_EMPTYSTR);
    CHECK_EQ(fk.closes, 0);
    actor_exit(&vm, b, R_NORMAL, T_EMPTYSTR);
    CHECK_EQ(fk.closes, 1);
    CHECK(!bytes_valid(&vm, h));
    CHECK_EQ(bytes_table_used(&vm), 0u);
    actor_exit(&vm, c, R_NORMAL, T_EMPTYSTR);
    teardown();
}

/* Handoff to a dead actor, or to one that cannot take the charge within
 * its limit, fails and changes nothing. */
TEST(handoff_refused) {
    uint32_t w[MINI_WORDS]; Image img; Vm vm;
    setup(w, &img, &vm);
    Actor *a = waiting_actor(&vm), *b = waiting_actor(&vm), *dead = waiting_actor(&vm);
    term gone = dead->id;
    actor_exit(&vm, dead, R_NORMAL, T_EMPTYSTR);
    fk.charge = 8;
    term p = open_fake(&vm, a, 1);
    Port *pt = fk.last;
    uint32_t alist = bytes_list_words(a);
    uint8_t *d;
    term h = port_bytes_new(&vm, pt, 1000, &d);
    CHECK(h != T_NIL);
    CHECK(!port_handoff(&vm, a, p, gone));
    b->max_words = (uint16_t)(b->mem_words + 100u);   /* 1000 bytes do not fit */
    CHECK(!port_handoff(&vm, a, p, b->id));
    CHECK(pt->owner == a->id);
    CHECK_EQ(actor_extra_charged(a), 8u + alist);
    CHECK_EQ(bytes_held_bytes(a), 1000u);
    CHECK_EQ(bytes_held_count(b), 0u);
    CHECK_EQ(actor_extra_charged(b), 0u);
    CHECK_EQ(fk.handoffs, 0);
    CHECK(port_handoff(&vm, a, p, a->id));      /* to itself: ok, no move */
    CHECK_EQ(fk.handoffs, 1);
    CHECK(pt->owner == a->id);
    clear_runq(&vm);
    actor_exit(&vm, a, R_NORMAL, T_EMPTYSTR);
    actor_exit(&vm, b, R_NORMAL, T_EMPTYSTR);
    teardown();
}

/* A port's driver-state charge must fit its owner's limit at open. */
TEST(port_open_charge_limit) {
    uint32_t w[MINI_WORDS]; Image img; Vm vm;
    setup(w, &img, &vm);
    Actor *a = waiting_actor(&vm);
    a->max_words = (uint16_t)(a->mem_words + 10u);
    fk.charge = 11;
    CHECK(open_fake(&vm, a, 1) == T_NIL);
    CHECK_EQ(vm.open_err, GOGO_E_NOMEMORY);
    CHECK_EQ(fk.opens, 1);
    CHECK_EQ(fk.closes, 1);                     /* the driver undid its open */
    CHECK(!actor_port_owns(a));
    CHECK_EQ(actor_extra_charged(a), 0u);
    fk.charge = 10;
    term p = open_fake(&vm, a, 1);
    CHECK(p != T_NIL);
    CHECK_EQ(actor_extra_charged(a), 10u);
    port_close(&vm, p);
    CHECK_EQ(actor_extra_charged(a), 0u);
    actor_exit(&vm, a, R_NORMAL, T_EMPTYSTR);
    teardown();
}

/* room_64w (64-word units) must fit beside the charge at open, and is not
 * charged. */
TEST(port_open_room_limit) {
    uint32_t w[MINI_WORDS]; Image img; Vm vm;
    setup(w, &img, &vm);
    Actor *a = waiting_actor(&vm);
    a->max_words = (uint16_t)(a->mem_words + 133u);
    fk.charge = 6;
    fk.room = 2;                                /* 6 + 128 > 133 */
    CHECK(open_fake(&vm, a, 1) == T_NIL);
    CHECK_EQ(fk.closes, 1);                     /* the driver undid its open */
    CHECK_EQ(actor_extra_charged(a), 0u);
    fk.charge = 5;
    term p = open_fake(&vm, a, 1);
    CHECK(p != T_NIL);
    CHECK_EQ(actor_extra_charged(a), 5u);       /* only the charge */
    port_close(&vm, p);
    fk.charge = 0;
    fk.room = 3;                                /* room alone is checked too */
    CHECK(open_fake(&vm, a, 1) == T_NIL);
    CHECK_EQ(vm.open_err, GOGO_E_NOMEMORY);
    fk.room = 2;
    p = open_fake(&vm, a, 1);
    CHECK(p != T_NIL);
    CHECK_EQ(actor_extra_charged(a), 0u);
    port_close(&vm, p);
    actor_exit(&vm, a, R_NORMAL, T_EMPTYSTR);
    teardown();
}

/* Closing a port releases the Bytes it pinned for its owner (M4 carry);
 * retain/release track them, and a port holds at most PORT_MAX_HELD. */
TEST(port_close_releases_held) {
    uint32_t w[MINI_WORDS]; Image img; Vm vm;
    setup(w, &img, &vm);
    Actor *a = waiting_actor(&vm), *b = waiting_actor(&vm);
    term p = open_fake(&vm, a, 1);
    Port *pt = fk.last;
    uint8_t *d;
    term h1 = port_bytes_new(&vm, pt, 100, &d);
    CHECK(h1 != T_NIL);
    /* b's handle, pinned by a's port on a's behalf (a Write's data). */
    term hb = bytes_new(&vm, b, 300, &d);
    CHECK(hb != T_NIL);
    for (uint32_t i = 1; i < PORT_MAX_HELD; i++) CHECK(port_bytes_retain(&vm, pt, hb));
    CHECK(!port_bytes_retain(&vm, pt, hb));     /* full */
    CHECK(port_bytes_new(&vm, pt, 4, &d) == T_NIL);
    CHECK_EQ(bytes_held_bytes(a), 400u);
    port_bytes_release(&vm, pt, h1);
    CHECK(!bytes_valid(&vm, h1));
    CHECK_EQ(bytes_held_bytes(a), 300u);
    port_bytes_release(&vm, pt, h1);            /* not held: ignored */
    CHECK_EQ(bytes_held_bytes(a), 300u);
    term h2 = port_bytes_new(&vm, pt, 40, &d);  /* the slot is free again */
    CHECK(h2 != T_NIL);
    port_close(&vm, p);
    CHECK_EQ(bytes_held_bytes(a), 0u);
    CHECK_EQ(bytes_held_count(a), 0u);
    CHECK(!bytes_valid(&vm, h2));
    CHECK(bytes_valid(&vm, hb));                     /* still b's */
    bytes_release(&vm, b, hb);
    CHECK_EQ(bytes_table_used(&vm), 0u);
    actor_exit(&vm, a, R_NORMAL, T_EMPTYSTR);
    actor_exit(&vm, b, R_NORMAL, T_EMPTYSTR);
    teardown();
}

/* An extra event source (the net stack's RX / link slots) is drained,
 * keeps the VM alive and contributes deadlines through the port layer. */
static struct { bool pending, live, have_dl; uint64_t dl; int drains; uint64_t now; } src;
static bool src_pending(const Vm *vm, void *ctx) { (void)vm; (void)ctx; return src.pending; }
static void src_drain(Vm *vm, void *ctx, uint64_t now) { (void)vm; (void)ctx; src.drains++; src.now = now; src.pending = false; }
static bool src_live(const Vm *vm, void *ctx) { (void)vm; (void)ctx; return src.live; }
static bool src_deadline(const Vm *vm, void *ctx, uint64_t *d) { (void)vm; (void)ctx; if (src.have_dl) *d = src.dl; return src.have_dl; }
static const PortSource test_src = { src_pending, src_drain, src_live, src_deadline, NULL };

/* A source whose drain leaves work behind (as a sliced TLS handshake
 * does) is drained again at once: vm_step checks pending() before it
 * reports idle. Its only deadline is 300 ms away, so without that check each
 * re-drain would wait for it (6 x 300 ms). */
static struct { int left, drains; } busy;
static bool busy_pending(const Vm *vm, void *ctx) { (void)vm; (void)ctx; return busy.left > 0; }
static void busy_drain(Vm *vm, void *ctx, uint64_t now) { (void)vm; (void)ctx; (void)now; busy.drains++; busy.left--; }
static bool busy_live(const Vm *vm, void *ctx) { (void)vm; (void)ctx; return busy.left > 0; }
static bool busy_deadline(const Vm *vm, void *ctx, uint64_t *d) {
    (void)ctx;
    *d = vm_now(vm) + 300u;
    return busy.left > 0;
}
static const PortSource busy_src = { busy_pending, busy_drain, busy_live, busy_deadline, NULL };

TEST(port_source_busy) {
    uint32_t w[MINI_WORDS + 2]; Image img; Vm vm;
    setup(w, &img, &vm);
    CHECK_EQ(image_load(&img, w, img_seal_main(w)), IMG_OK);    /* main runnable */
    CHECK_EQ(test_vm_init(&vm, &img, 8), 0);
    memset(&busy, 0, sizeof busy);
    busy.left = 6;
    CHECK(port_add_source(&vm, &busy_src, NULL));
    CHECK_EQ(vm_run(&vm), 0);
    CHECK_EQ(busy.drains, 6);
    CHECK(vm_now(&vm) < 250u);                  /* the clock restarts at vm_start */
    port_remove_source(&vm, &busy_src);
    teardown();
}

TEST(port_source_hook) {
    uint32_t w[MINI_WORDS]; Image img; Vm vm;
    setup(w, &img, &vm);
    memset(&src, 0, sizeof src);
    CHECK(!port_work_pending(&vm));
    CHECK(port_add_source(&vm, &test_src, NULL));
    CHECK(!port_work_pending(&vm));
    CHECK(!port_any_live(&vm));
    uint64_t dl = 0;
    CHECK(!port_next_deadline(&vm, &dl));
    src.pending = true;
    CHECK(port_work_pending(&vm));
    port_drain(&vm, 42);
    CHECK_EQ(src.drains, 1);
    CHECK_EQ(src.now, 42u);
    CHECK(!port_work_pending(&vm));
    src.live = true;
    CHECK(port_any_live(&vm));
    src.have_dl = true;
    src.dl = 500;
    CHECK(port_next_deadline(&vm, &dl));
    CHECK_EQ(dl, 500u);
    port_remove_source(&vm, &test_src);
    CHECK(!port_any_live(&vm));
    teardown();
}

/* A driver with command_term gets every command raw (nested structs,
 * non-int fields), with the sender; Subscribe goes to the source's hook. */
static struct { int raw; const Actor *from; term msg; int subs; } rawk;
static void rk_command_term(Vm *vm, const Actor *from, Port *p, term msg) {
    (void)vm; (void)p;
    rawk.raw++;
    rawk.from = from;
    rawk.msg = msg;
}
static bool rk_subscribe(Vm *vm, void *ctx, Actor *a) { (void)vm; (void)ctx; (void)a; rawk.subs++; return true; }
static const PortSource sub_src = { src_pending, src_drain, src_live, NULL, rk_subscribe };

TEST(port_command_term_and_subscribe) {
    uint32_t w[MINI_WORDS]; Image img; Vm vm;
    setup(w, &img, &vm);
    memset(&rawk, 0, sizeof rawk);
    static const PortDriver raw = {
        .name = "fake", .cfg_type = CFG_TYPE, .event_type = EVT_TYPE, .open = fk_open, .command = fk_command,
        .close = fk_close, .can_event = fk_can_event, .command_term = rk_command_term
    };
    static const PortDriver *const raw_list[] = { &raw, NULL };
    port_set_drivers(&vm, raw_list);
    Actor *a = waiting_actor(&vm);
    term p = open_fake(&vm, a, 3);
    CHECK(test_port_is(p));
    term msg = heap_str(&vm, a, "not a struct");
    port_command(&vm, a, p, msg);
    CHECK_EQ(rawk.raw, 1);
    CHECK_EQ(fk.cmds, 0);                     /* not decoded */
    CHECK(rawk.from == a && rawk.msg == msg);
    CHECK(!port_subscribe(&vm, a));           /* no source */
    CHECK(port_add_source(&vm, &test_src, NULL));
    CHECK(!port_subscribe(&vm, a));           /* no hook */
    port_remove_source(&vm, &test_src);
    CHECK(port_add_source(&vm, &sub_src, NULL));
    CHECK(port_subscribe(&vm, a));
    CHECK_EQ(rawk.subs, 1);
    port_remove_source(&vm, &sub_src);
    clear_runq(&vm);
    actor_exit(&vm, a, R_NORMAL, T_EMPTYSTR);
    teardown();
}

/* Handoff's limit check counts what the new owner would really be
 * charged: a pinned slice charges its root's full length, and a first
 * handle makes the holder list. */
TEST(handoff_slice_of_large_root) {
    uint32_t w[MINI_WORDS]; Image img; Vm vm;
    setup(w, &img, &vm);
    Actor *a = waiting_actor(&vm), *c = waiting_actor(&vm);
    term p = open_fake(&vm, a, 1);
    Port *pt = fk.last;
    uint8_t *d;
    term root = bytes_new(&vm, a, 1000, &d);
    CHECK(root != T_NIL);
    term sl = bytes_sub(&vm, a, root, 0, 8);
    CHECK(sl != T_NIL);
    CHECK(port_bytes_retain(&vm, pt, sl));
    bytes_release(&vm, a, root);              /* a's own pin; the slice keeps the root */
    uint32_t need = 250u + bytes_list_words(c);
    CHECK_EQ(bytes_extra_words(&vm, c, pt->held, PORT_MAX_HELD), need);
    c->max_words = (uint16_t)(c->mem_words + need - 1u);
    CHECK(!port_handoff(&vm, a, p, c->id));
    CHECK(pt->owner == a->id);
    CHECK_EQ(bytes_held_count(c), 0u);
    c->max_words = (uint16_t)(c->mem_words + need);
    CHECK(port_handoff(&vm, a, p, c->id));
    CHECK((uint32_t)c->mem_words + actor_charged_words(c) <= actor_max_words(c));
    CHECK_EQ(bytes_held_bytes(c), 1000u);
    /* An owner marked to exit gets no new pins. */
    c->flags |= AF_EXIT_PENDING;
    CHECK(port_bytes_new(&vm, pt, 4, &d) == T_NIL);
    c->flags &= (uint8_t)~AF_EXIT_PENDING;
    clear_runq(&vm);
    actor_exit(&vm, a, R_NORMAL, T_EMPTYSTR);
    actor_exit(&vm, c, R_NORMAL, T_EMPTYSTR);
    CHECK_EQ(bytes_table_used(&vm), 0u);
    teardown();
}

/* ---- several sources, multi-field delivery ---- */

static struct { bool pending, live, have_dl; uint64_t dl; int drains; } s2;
static bool s2_pending(const Vm *vm, void *ctx) { (void)vm; (void)ctx; return s2.pending; }
static void s2_drain(Vm *vm, void *ctx, uint64_t now) { (void)vm; (void)ctx; (void)now; s2.drains++; s2.pending = false; }
static bool s2_live(const Vm *vm, void *ctx) { (void)vm; (void)ctx; return s2.live; }
static bool s2_deadline(const Vm *vm, void *ctx, uint64_t *d) { (void)vm; (void)ctx; if (s2.have_dl) *d = s2.dl; return s2.have_dl; }
static const PortSource src2 = { s2_pending, s2_drain, s2_live, s2_deadline, NULL };
static bool s3_pending(const Vm *vm, void *ctx) { (void)vm; (void)ctx; return false; }
static bool s3_live(const Vm *vm, void *ctx) { (void)vm; (void)ctx; return false; }
static void s3_drain(Vm *vm, void *ctx, uint64_t now) { (void)vm; (void)ctx; (void)now; }
static const PortSource src3 = { s3_pending, s3_drain, s3_live, NULL, NULL };
static const PortSource src4 = { s3_pending, s3_drain, s3_live, NULL, NULL };
static const PortSource src5 = { s3_pending, s3_drain, s3_live, NULL, NULL };

TEST(two_sources_both_drain) {
    uint32_t w[MINI_WORDS]; Image img; Vm vm;
    setup(w, &img, &vm);
    memset(&src, 0, sizeof src);
    memset(&s2, 0, sizeof s2);
    CHECK(port_add_source(&vm, &test_src, NULL));
    CHECK(port_add_source(&vm, &src2, NULL));
    src.pending = s2.pending = true;
    CHECK(port_work_pending(&vm));
    port_drain(&vm, 7);
    CHECK_EQ(src.drains, 1);
    CHECK_EQ(s2.drains, 1);
    CHECK(!port_work_pending(&vm));
    CHECK(!port_any_live(&vm));
    s2.live = true;
    CHECK(port_any_live(&vm));
    uint64_t dl = 0;
    CHECK(!port_next_deadline(&vm, &dl));
    src.have_dl = s2.have_dl = true;
    src.dl = 900;
    s2.dl = 300;
    CHECK(port_next_deadline(&vm, &dl));
    CHECK_EQ(dl, 300u);
    s2.dl = 1000;
    CHECK(port_next_deadline(&vm, &dl));
    CHECK_EQ(dl, 900u);
    port_remove_source(&vm, &test_src);
    port_remove_source(&vm, &src2);
    teardown();
}

TEST(fifth_source_refused) {
    uint32_t w[MINI_WORDS]; Image img; Vm vm;
    setup(w, &img, &vm);
    CHECK_EQ(GOGO_MAX_SOURCES, 4u);
    CHECK(port_add_source(&vm, &test_src, NULL));
    CHECK(port_add_source(&vm, &src2, NULL));
    CHECK(port_add_source(&vm, &src3, NULL));
    CHECK(port_add_source(&vm, &src4, NULL));
    CHECK(!port_add_source(&vm, &src5, NULL));
    port_remove_source(&vm, &src5);                /* unknown: ignored */
    CHECK(!port_add_source(&vm, &src5, NULL));
    port_remove_source(&vm, &test_src);
    CHECK(port_add_source(&vm, &src5, NULL));
    port_remove_source(&vm, &src2);
    port_remove_source(&vm, &src3);
    port_remove_source(&vm, &src4);
    port_remove_source(&vm, &src5);
    teardown();
}

static const Port *first_port(Vm *vm) { return port_at(vm, 0); }

TEST(deliver_ints_struct) {
    uint32_t w[MINI_WORDS]; Image img; Vm vm;
    setup(w, &img, &vm);
    Actor *a = waiting_actor(&vm);
    term p = open_fake(&vm, a, 1);
    CHECK(test_port_is(p));
    int32_t v[2] = { 7, 1 << 30 };
    CHECK(port_deliver_ints(&vm, first_port(&vm), TYPE_GPIO_EDGE, v, 2, 0));
    CHECK_EQ(mbox_len(a), 1);
    const Msg *m = a->mbox_head;
    CHECK_EQ(m->data[0], hdr_make(4, K_STRUCT));
    CHECK_EQ(m->data[1], (uint32_t)TYPE_GPIO_EDGE);
    CHECK_EQ(m->data[2], term_from_int(7));
    CHECK(term_is_ref(m->data[3]));
    CHECK_EQ(m->data[term_ref_off(m->data[3]) + 1], 1u << 30);
    clear_runq(&vm);
    actor_exit(&vm, a, R_NORMAL, T_EMPTYSTR);
    teardown();
}

TEST(deliver_ints_bool) {
    uint32_t w[MINI_WORDS]; Image img; Vm vm;
    setup(w, &img, &vm);
    Actor *a = waiting_actor(&vm);
    term p = open_fake(&vm, a, 1);
    CHECK(test_port_is(p));
    int32_t v[2] = { 5, PORT_BOOL(1) };
    CHECK(port_deliver_ints(&vm, first_port(&vm), TYPE_GPIO_EDGE, v, 2, PORT_RAW(1)));
    const Msg *m = a->mbox_head;
    CHECK_EQ(m->data[2], term_from_int(5));
    CHECK_EQ(m->data[3], T_TRUE);
    v[1] = PORT_BOOL(0);
    CHECK(port_deliver_ints(&vm, first_port(&vm), TYPE_GPIO_EDGE, v, 2, PORT_RAW(1)));
    CHECK_EQ(a->mbox_head->next->data[3], T_FALSE);
    v[0] = (int32_t)first_port(&vm)->id;   /* a PID, as an event's From */
    CHECK(port_deliver_ints(&vm, first_port(&vm), TYPE_GPIO_EDGE, v, 2, PORT_RAW(0) | PORT_RAW(1)));
    CHECK_EQ(a->mbox_head->next->next->data[2], first_port(&vm)->id);
    clear_runq(&vm);
    actor_exit(&vm, a, R_NORMAL, T_EMPTYSTR);
    teardown();
}

TEST(deliver_ints_full_mailbox_not_fatal) {
    uint32_t w[MINI_WORDS]; Image img; Vm vm;
    setup(w, &img, &vm);
    Actor *a = waiting_actor(&vm);
    term p = open_fake(&vm, a, 1);
    CHECK(test_port_is(p));
    a->max_words = 96;
    int32_t v[2] = { 1, 2 };
    int sent = 0;
    for (int i = 0; i < 40; i++) sent += port_deliver_ints(&vm, first_port(&vm), TYPE_GPIO_EDGE, v, 2, 0);
    CHECK(sent > 0 && sent < 40);
    CHECK_EQ(mbox_len(a), sent);
    uint32_t code; term fn;
    CHECK(!actor_exit_pending(a, &code, &fn));
    clear_runq(&vm);
    actor_exit(&vm, a, R_NORMAL, T_EMPTYSTR);
    teardown();
}

static char stale_err[256];
static void stale_cap(const char *s, size_t n) {
    size_t l = strlen(stale_err);
    if (l + n < sizeof stale_err) { memcpy(stale_err + l, s, n); stale_err[l + n] = 0; }
}

/* A stale value (a const ref or struct type of a module that is gone; here
 * a slot no module is in) given to port.Open or sent as a port command is
 * badcode for the actor, never a silent failure. The gate is the
 * builtin, so every driver's command handler is covered. */
TEST(port_stale_values_badcode) {
    uint32_t w[40]; Image img; Vm vm;
    va_reset(&vm);
    memset(&fk, 0, sizeof fk);
    memset(w, 0, sizeof w);
    w[0] = IMAGE_MAGIC; w[1] = IMAGE_VERSION; w[2] = 1; w[3] = 2;
    w[4] = 6; w[5] = 6; w[6] = 0; w[7] = 0;
    w[8] = 0; w[9] = 0 | (4u << 16); w[10] = 0;
    w[11] = 2; w[12] = 3; w[13] = 2; w[14] = 3;
    w[15] = hdr_make(3, K_STRING); w[16] = 4; memcpy(&w[17], "main", 4);
    w[18] = hdr_make(3, K_STRING); w[19] = 3; memcpy(&w[20], "Cfg", 3);
    uint32_t *c = w + 21;
    c[0] = OP_MAKE(OP_BUILTIN, 2, 0, 2); c[1] = B_PORT_OPEN;   /* port.Open(r0, r1) */
    c[2] = OP_MAKE(OP_RET, 0, 0, 0);
    c[3] = OP_MAKE(OP_BUILTIN, 2, 0, 2); c[4] = B_SEND;        /* Send(r0, r1) */
    c[5] = OP_MAKE(OP_RET, 0, 0, 0);
    CHECK_EQ(image_load(&img, w, img_seal(w)), IMG_OK);
    CHECK_EQ(test_vm_init(&vm, &img, 8), 0);
    port_set_drivers(&vm, fake_list);
    uint32_t tag = mod_tag_make(6, 3);
    term stale = term_from_const(0) | tag;
    int32_t cfg[2] = { 1, 0 };
    for (int k = 0; k < 5; k++) {
        Actor *a = actor_new(&vm, 128);
        CHECK(heap_push_frame(&vm, a, 4, -1));
        term *R = (term *)a->mem + a->fp;
        a->pc = img.code + (k < 3 ? 0u : 3u);
        term ok_cfg = heap_struct(&vm, a, CFG_TYPE, cfg, 2);
        term name = heap_str(&vm, a, "fake");
        if (k == 0) { R[0] = stale; R[1] = ok_cfg; }                         /* stale name */
        if (k == 1) { R[0] = name; R[1] = heap_struct(&vm, a, 40u | tag, cfg, 2); }   /* stale cfg type */
        if (k == 2) { a->mem[term_ref_off(ok_cfg) + 3] = stale; R[0] = name; R[1] = ok_cfg; }
        Actor *o = NULL;
        if (k >= 3) {
            o = actor_new(&vm, 64);
            R[0] = open_fake(&vm, o, 1);
            CHECK(test_port_is(R[0]));
            term cmd = heap_struct(&vm, a, 7, cfg, 2);
            if (k == 3) a->mem[term_ref_off(cmd) + 2] = stale;               /* stale field */
            else a->mem[term_ref_off(cmd) + 1] = 40u | tag;                 /* stale type */
            R[1] = cmd;
        }
        stale_err[0] = 0;
        platform_unix_err_hook = stale_cap;
        int r = interp_run(&vm, a);
        platform_unix_err_hook = NULL;
        CHECK_EQ(r, RUN_EXIT);
        CHECK(strstr(stale_err, "crashed: badcode") != NULL);
        CHECK_EQ(fk.opens, k >= 3 ? k - 2 : 0);   /* only open_fake's */
        CHECK_EQ(fk.cmds, 0);
        if (o) actor_exit(&vm, o, R_NORMAL, T_EMPTYSTR);
    }
    CHECK_EQ(vm.live, 0u);
    teardown();
}

/* A driver with open_term gets the cfg raw (a struct of cfg_type that may
 * hold strings), with the owner; the charge and room checks still apply,
 * and a cfg of another type never reaches it. */
static struct { int opens; const Actor *owner; term cfg; uint16_t charge; int refuse; } otk;
static int ot_open_term(Vm *vm, const Actor *owner, Port *p, uint16_t index, term cfg) {
    (void)vm; (void)index;
    otk.opens++;
    otk.owner = owner;
    otk.cfg = cfg;
    p->charge_words = otk.charge;
    return otk.refuse ? -1 : 0;
}

TEST(port_open_term) {
    uint32_t w[MINI_WORDS]; Image img; Vm vm;
    setup(w, &img, &vm);
    memset(&otk, 0, sizeof otk);
    static const PortDriver ot = {
        .name = "fake", .cfg_type = CFG_TYPE, .event_type = EVT_TYPE, .command = fk_command,
        .close = fk_close, .can_event = fk_can_event, .open_term = ot_open_term
    };
    static const PortDriver *const ot_list[] = { &ot, NULL };
    port_set_drivers(&vm, ot_list);
    Actor *a = waiting_actor(&vm);
    /* A cfg struct with a string field (not decodable as ints). */
    a->hp = 0;
    term s = heap_str(&vm, a, "example.com");
    uint32_t off = heap_alloc(&vm, a, 4, NULL, 0);
    a->mem[off] = hdr_make(4, K_STRUCT);
    a->mem[off + 1] = CFG_TYPE;
    a->mem[off + 2] = term_from_int(443);
    a->mem[off + 3] = s;
    term cfg = term_from_ref(off);
    term name = heap_str(&vm, a, "fake");
    term p = port_open(&vm, a, name, cfg);
    CHECK(test_port_is(p));
    CHECK_EQ(otk.opens, 1);
    CHECK_EQ(fk.opens, 0);                      /* open is not used */
    CHECK(otk.owner == a && otk.cfg == cfg);
    port_close(&vm, p);
    CHECK_EQ(fk.closes, 1);
    /* A C caller's port_open_with: refused (no `open`), never a NULL call. */
    CHECK(port_open_with(&vm, a, &ot, NULL, 0) == T_NIL);
    CHECK_EQ(otk.opens, 1);
    /* Another struct type: refused before the driver. */
    int32_t f[1] = { 1 };
    p = port_open(&vm, a, name, heap_struct(&vm, a, EVT_TYPE, f, 1));
    CHECK(p == T_NIL);
    CHECK_EQ(otk.opens, 1);
    /* The driver refuses: nothing owned. */
    otk.refuse = 1;
    CHECK(port_open(&vm, a, name, cfg) == T_NIL);
    CHECK(!actor_port_owns(a));
    otk.refuse = 0;
    /* Over the limit: the driver's close undoes it. */
    a->max_words = (uint16_t)(a->mem_words + 10u);
    otk.charge = 11;
    CHECK(port_open(&vm, a, name, cfg) == T_NIL);
    CHECK_EQ(fk.closes, 2);
    CHECK_EQ(actor_extra_charged(a), 0u);
    actor_exit(&vm, a, R_NORMAL, T_EMPTYSTR);
    teardown();
}
