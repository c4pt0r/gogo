/* The "ble" port on a real VM over a fake BleHal: the Config's decoding,
 * open and its refusals (one port per board, a radio that does not start),
 * events from the radio side's queue in order (Connected, MTU, Written,
 * Subscribed, Disconnected), held back while the owner's mailbox is full
 * and never dropped, the queue's wrap and its spare room, SetValue, and
 * Notify's flow control: one Done each, the stack's "no buffer" retried,
 * refusals (not subscribed, over the MTU, a third in flight) and the
 * Notifies in flight failing when the phone goes away. */
#include "test.h"
#include "test_host.h"
#include "ble.h"
#include "actor.h"
#include "heap.h"
#include "msg.h"
#include "alloc.h"
#include "bytes.h"
#include "opcodes.h"
#include "port.h"
#include "host.h"
#include <string.h>

static Vm vm;

/* ---- the fake radio ---- */
static BleBoard board;
static int start_rc, starts, stops, locks;
static BleConfig started;
static int notify_rc[8], notify_n, notify_calls;   /* scripted results, then 0 */
static bool always_again;                           /* every notify: BLE_AGAIN */
static uint32_t last_chr, last_len;
static uint8_t last_data[BLE_VALUE_MAX];
static uint32_t set_chr, set_len;
static uint8_t set_data[BLE_VALUE_MAX];

static int f_start(void *ctx, const BleConfig *c) { (void)ctx; starts++; started = *c; return start_rc; }
static void f_stop(void *ctx) { (void)ctx; stops++; }
static int f_set(void *ctx, uint32_t chr, const uint8_t *d, uint32_t len) {
    (void)ctx;
    set_chr = chr;
    set_len = len;
    if (len) memcpy(set_data, d, len);
    return 0;
}
static int f_notify(void *ctx, uint32_t chr, const uint8_t *d, uint32_t len) {
    (void)ctx;
    int r = always_again ? BLE_AGAIN : notify_calls < notify_n ? notify_rc[notify_calls] : 0;
    notify_calls++;
    last_chr = chr;
    last_len = len;
    memcpy(last_data, d, len);
    return r;
}
static void f_lock(void *ctx) { (void)ctx; CHECK_EQ(locks, 0); locks++; }
static void f_unlock(void *ctx) { (void)ctx; CHECK_EQ(locks, 1); locks--; }

static const BleHal hal = { NULL, &board, f_start, f_stop, f_set, f_notify, f_lock, f_unlock };
static const PortDriver ble_drv = BLE_DRIVER(&hal);
static const PortDriver *const drivers[] = { &ble_drv, NULL };

static uint64_t now_ms;
static uint64_t fake_clock(void) { return now_ms; }

static Actor *owner;
static term bp;

/* ---- heap values ---- */

static term heap_bytes(Actor *a, const uint8_t *d, uint32_t len) {
    if (!len) return T_EMPTYBYTES;
    uint32_t words = 2u + (len + 3u) / 4u;
    uint32_t off = heap_alloc(&vm, a, words, NULL, 0);
    CHECK(off != UINT32_MAX);
    a->mem[off] = hdr_make(words, K_BYTES);
    a->mem[off + 1] = len;
    a->mem[off + words - 1] = 0;
    memcpy(a->mem + off + 2, d, len);
    return term_from_ref(off);
}

static term heap_string(Actor *a, const char *s) {
    uint32_t len = (uint32_t)strlen(s), words = 2u + (len + 3u) / 4u;
    uint32_t off = heap_alloc(&vm, a, words, NULL, 0);
    CHECK(off != UINT32_MAX);
    a->mem[off] = hdr_make(words, K_STRING);
    a->mem[off + 1] = len;
    a->mem[off + words - 1] = 0;
    memcpy(a->mem + off + 2, s, len);
    return term_from_ref(off);
}

static term heap_struct(Actor *a, uint32_t type, uint32_t n, const term *f) {
    uint32_t off = heap_alloc(&vm, a, 2u + n, NULL, 0);
    CHECK(off != UINT32_MAX);
    a->mem[off] = hdr_make(2u + n, K_STRUCT);
    a->mem[off + 1] = type;
    for (uint32_t i = 0; i < n; i++) a->mem[off + 2 + i] = f[i];
    return term_from_ref(off);
}

/* The Config of the tests: "gogo-t", service 180f with char 0 (Read |
 * Notify) and char 1 (Write | WriteNoResp), a 128-bit service with char
 * 2 (Notify). */
static const uint8_t CFG[] = {
    6, 'g', 'o', 'g', 'o', '-', 't',
    2,
    2, 0x0f, 0x18, 2,
        2, 0x19, 0x2a, BLE_F_READ | BLE_F_NOTIFY,
        2, 0x01, 0x00, BLE_F_WRITE | BLE_F_WRITE_NR,
    16, 0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e, 1,
        2, 0x02, 0x00, BLE_F_NOTIFY,
};

static term open_with(Actor *a, int32_t fmt, const uint8_t *d, uint32_t len) {
    a->hp = 0;
    term f[2] = { term_from_int(fmt), heap_bytes(a, d, len) };
    return port_open(&vm, a, heap_string(a, "ble"), heap_struct(a, TYPE_HOST, 2, f));
}

static void setup(void) {
    test_host_arena_forget();
    va_reset(&vm);
    bytes_reset(&vm);
    now_ms = 1000;
    test_host_set_clock(fake_clock);
    CHECK_EQ(test_vm_init(&vm, NULL, 16), 0);
    port_set_drivers(&vm, drivers);
    memset(&board, 0, sizeof board);
    start_rc = starts = stops = locks = 0;
    notify_n = notify_calls = 0;
    always_again = false;
    set_len = set_chr = 0;
    owner = actor_new(&vm, 256);
    CHECK(owner != NULL);
    owner->state = A_WAITING;
    bp = open_with(owner, BLE_CFG_FORMAT, CFG, sizeof CFG);
    CHECK(bp != T_NIL);
}

static void teardown(void) {
    port_close_all(&vm);
    test_host_set_clock(NULL);
}

/* The radio side posts, under the lock, as a NimBLE callback would. */
static bool radio(uint32_t ev, uint32_t arg, const void *d, uint32_t len) {
    hal.lock(hal.ctx);
    bool ok = ble_post(&board, ev, arg, d, len);
    hal.unlock(hal.ctx);
    return ok;
}

static void drain(void) { port_drain(&vm, vm_now(&vm)); }

static Port *bport(void) { return port_at(&vm, term_pid_slot(bp) - vm.port_base); }

/* ---- the mailbox ---- */

static Msg *pop(Actor *a) {
    Msg *m = a->mbox_head;
    if (!m) return NULL;
    a->mbox_head = m->next;
    if (!a->mbox_head) a->mbox_tail = NULL;
    a->mbox_words = (gogo_words_t)(a->mbox_words - msg_charge_words(&vm, m));
    return m;
}

static int32_t word_int(const Msg *m, term t) {
    if (term_is_int(t)) return term_to_int(t);
    CHECK(term_is_ref(t));
    const uint32_t *o = &m->data[term_ref_off(t)];
    CHECK_EQ(o[0], hdr_make(2, K_INT));
    return (int32_t)o[1];
}

typedef struct { int type; int32_t kind; uint32_t len; uint8_t data[BLE_VALUE_MAX]; } Ev;

/* The next message: a Post (type TYPE_POST, kind, data) or a Done (type
 * TYPE_DONE, kind = Seq); false if none. */
static bool next(Actor *a, Ev *e) {
    Msg *m = pop(a);
    if (!m) return false;
    e->type = (int)m->data[1];
    CHECK_EQ(m->data[2], bp);            /* From: the port */
    e->len = 0;
    if (e->type == TYPE_POST) {
        CHECK_EQ(m->data[0], hdr_make(5, K_STRUCT));
        e->kind = word_int(m, m->data[3]);
        if (m->data[4] != T_EMPTYBYTES) {
            const uint32_t *b = &m->data[term_ref_off(m->data[4])];
            CHECK_EQ(hdr_kind(b[0]), K_BYTES);
            e->len = b[1];
            memcpy(e->data, &b[2], e->len);
        }
    } else {
        CHECK_EQ(e->type, TYPE_DONE);
        e->kind = word_int(m, m->data[3]);
    }
    msg_free(&vm, m);
    return true;
}

static void want_post(int32_t kind, const void *d, uint32_t len) {
    Ev e;
    CHECK(next(owner, &e));
    CHECK_EQ(e.type, TYPE_POST);
    CHECK_EQ(e.kind, kind);
    CHECK_EQ(e.len, len);
    if (len) CHECK(memcmp(e.data, d, len) == 0);
}

static void want_done(int32_t seq) {
    Ev e;
    CHECK(next(owner, &e));
    CHECK_EQ(e.type, TYPE_DONE);
    CHECK_EQ(e.kind, seq);
}

static void want_none(void) {
    Ev e;
    CHECK(!next(owner, &e));
}

/* port.Post{Kind, Data} from a to the port, as B_SEND does. */
static void command(Actor *a, int32_t kind, const void *d, uint32_t len) {
    a->hp = 0;
    term f[3] = { T_ZEROPID, term_from_int(kind), heap_bytes(a, d, len) };
    port_command(&vm, a, bp, heap_struct(a, TYPE_POST, 3, f));
}

static void connect_sub(void) {
    CHECK(radio(BLE_EV_CONNECTED, 185, "aa:bb", 5));
    CHECK(radio(BLE_EV_SUBSCRIBED, 0 | 1u << 8, NULL, 0));
    drain();
    want_post(BLE_EV_CONNECTED | 185 << 8, "aa:bb", 5);
    want_post(BLE_EV_SUBSCRIBED | (0 | 1 << 8) << 8, NULL, 0);
}

/* ---- the Config ---- */

TEST(ble_config_decode_rules) {
    BleConfig c;
    CHECK_EQ(ble_config_decode(CFG, sizeof CFG, &c), 0);
    CHECK(!strcmp(c.name, "gogo-t"));
    CHECK_EQ(c.nsvc, 2); CHECK_EQ(c.nchr, 3);
    CHECK_EQ(c.first[0], 0); CHECK_EQ(c.first[1], 2); CHECK_EQ(c.first[2], 3);
    CHECK_EQ(c.svc[0].len, 2); CHECK_EQ(c.svc[0].b[1], 0x18);
    CHECK_EQ(c.svc[1].len, 16); CHECK_EQ(c.svc[1].b[15], 0x6e);
    CHECK_EQ(c.flags[1], BLE_F_WRITE | BLE_F_WRITE_NR);
    uint8_t b[sizeof CFG + 1];
    memcpy(b, CFG, sizeof CFG);
    b[sizeof CFG] = 0;
    CHECK_EQ(ble_config_decode(b, sizeof CFG + 1, &c), GOGO_E_BADARG);    /* trailing byte */
    CHECK_EQ(ble_config_decode(b, sizeof CFG - 1, &c), GOGO_E_BADARG);    /* cut short */
    CHECK_EQ(ble_config_decode(NULL, 0, &c), GOGO_E_BADARG);
    memcpy(b, CFG, sizeof CFG);
    b[15] = 0;                                                             /* char 0 without flags */
    CHECK_EQ(ble_config_decode(b, sizeof CFG, &c), GOGO_E_BADARG);
    b[15] = 16;                                                            /* an unknown flag */
    CHECK_EQ(ble_config_decode(b, sizeof CFG, &c), GOGO_E_BADARG);
    memcpy(b, CFG, sizeof CFG);
    b[8] = 4;                                                              /* a 32-bit UUID */
    CHECK_EQ(ble_config_decode(b, sizeof CFG, &c), GOGO_E_BADARG);
    memcpy(b, CFG, sizeof CFG);
    b[3] = 0;                                                              /* a NUL in the name */
    CHECK_EQ(ble_config_decode(b, sizeof CFG, &c), GOGO_E_BADARG);
    memcpy(b, CFG, sizeof CFG);
    b[7] = 5;                                                              /* five services */
    CHECK_EQ(ble_config_decode(b, sizeof CFG, &c), GOGO_E_TOOMANY);
    b[7] = 0;
    CHECK_EQ(ble_config_decode(b, sizeof CFG, &c), GOGO_E_BADARG);
    memcpy(b, CFG, sizeof CFG);
    b[11] = 9;                                                             /* nine in a service */
    CHECK_EQ(ble_config_decode(b, sizeof CFG, &c), GOGO_E_TOOMANY);
    /* 27 bytes of name: too long */
    uint8_t n[40];
    memset(n, 'x', sizeof n);
    n[0] = 27;
    CHECK_EQ(ble_config_decode(n, sizeof n, &c), GOGO_E_BADARG);
    /* 3 services of 6 characteristics: 18 > 16 */
    uint8_t m[128];
    uint32_t k = 0;
    m[k++] = 1; m[k++] = 'g'; m[k++] = 3;
    for (int s = 0; s < 3; s++) {
        m[k++] = 2; m[k++] = (uint8_t)s; m[k++] = 0x18; m[k++] = 6;
        for (int i = 0; i < 6; i++) { m[k++] = 2; m[k++] = (uint8_t)i; m[k++] = 0x2a; m[k++] = BLE_F_READ; }
    }
    CHECK_EQ(ble_config_decode(m, k, &c), GOGO_E_TOOMANY);
}

/* ---- open and close ---- */

TEST(ble_open_starts_one_per_board) {
    setup();
    CHECK_EQ(starts, 1);
    CHECK(!strcmp(started.name, "gogo-t"));
    CHECK_EQ(started.nchr, 3);
    Actor *b = actor_new(&vm, 256);
    CHECK(open_with(b, BLE_CFG_FORMAT, CFG, sizeof CFG) == T_NIL);
    CHECK_EQ(vm.open_err, GOGO_E_BUSY);
    CHECK_EQ(starts, 1);
    CHECK(ble_can_event(&vm, bport()));
    port_close(&vm, bp);
    CHECK_EQ(stops, 1);
    CHECK(!radio(BLE_EV_CONNECTED, 23, "x", 1));      /* closed: the radio side's posts drop */
    bp = open_with(b, BLE_CFG_FORMAT, CFG, sizeof CFG);
    CHECK(bp != T_NIL);
    CHECK_EQ(starts, 2);
    teardown();
    CHECK_EQ(stops, 2);
}

TEST(ble_open_refusals) {
    setup();
    port_close(&vm, bp);
    CHECK(open_with(owner, 2, CFG, sizeof CFG) == T_NIL);          /* another format */
    CHECK_EQ(vm.open_err, GOGO_E_BADARG);
    CHECK(open_with(owner, BLE_CFG_FORMAT, CFG, 5) == T_NIL);
    CHECK_EQ(vm.open_err, GOGO_E_BADARG);
    start_rc = GOGO_E_BUSY;                                       /* provisioning has the radio */
    CHECK(open_with(owner, BLE_CFG_FORMAT, CFG, sizeof CFG) == T_NIL);
    CHECK_EQ(vm.open_err, GOGO_E_BUSY);
    start_rc = GOGO_E_IO;
    CHECK(open_with(owner, BLE_CFG_FORMAT, CFG, sizeof CFG) == T_NIL);
    CHECK_EQ(vm.open_err, GOGO_E_IO);
    CHECK_EQ(stops, 1);                                           /* refused starts need no stop */
    CHECK(!radio(BLE_EV_CONNECTED, 23, "x", 1));
    start_rc = 0;
    bp = open_with(owner, BLE_CFG_FORMAT, CFG, sizeof CFG);       /* the source slot came back */
    CHECK(bp != T_NIL);
    teardown();
}

/* ---- events ---- */

TEST(ble_events_in_order) {
    setup();
    const uint8_t w[3] = { 1, 2, 3 };
    CHECK(radio(BLE_EV_CONNECTED, 23, "11:22:33:44:55:66", 17));
    CHECK(radio(BLE_EV_MTU, 247, NULL, 0));
    CHECK(radio(BLE_EV_WRITTEN, 1, w, 3));
    CHECK(radio(BLE_EV_SUBSCRIBED, 2 | 1u << 8, NULL, 0));
    CHECK(radio(BLE_EV_DISCONNECTED, 0x13, NULL, 0));
    want_none();
    drain();
    want_post(BLE_EV_CONNECTED | 23 << 8, "11:22:33:44:55:66", 17);
    want_post(BLE_EV_MTU | 247 << 8, NULL, 0);
    want_post(BLE_EV_WRITTEN | 1 << 8, w, 3);
    want_post(BLE_EV_SUBSCRIBED | (2 | 1 << 8) << 8, NULL, 0);
    want_post(BLE_EV_DISCONNECTED | 0x13 << 8, NULL, 0);
    want_none();
    CHECK(!board.connected);
    teardown();
}

/* A full mailbox holds the events back (in the queue), in order, and
 * retries them after BLE_RETRY_MS; the owner is never killed. */
TEST(ble_events_held_while_mailbox_full) {
    setup();
    owner->max_words = 200;
    uint8_t w[100];
    for (int i = 0; i < 100; i++) w[i] = (uint8_t)i;
    int n = 0;
    while (radio(BLE_EV_WRITTEN, (uint32_t)n, w, 100)) n++;
    CHECK(n >= 3);
    CHECK_EQ(board.lost, 1u);
    CHECK(radio(BLE_EV_DISCONNECTED, 0x08, NULL, 0));   /* the spare room takes it */
    drain();
    CHECK(!(owner->flags & AF_EXIT_PENDING));
    CHECK(board.retry_at != 0);
    int got = 0;
    Ev e;
    while (got < n + 1) {
        while (next(owner, &e)) {
            if (got < n) {
                CHECK_EQ(e.kind, BLE_EV_WRITTEN | got << 8);
                CHECK_EQ(e.len, 100u);
                CHECK(memcmp(e.data, w, 100) == 0);
            } else {
                CHECK_EQ(e.kind, BLE_EV_DISCONNECTED | 0x08 << 8);
            }
            got++;
        }
        drain();                                          /* not due yet: nothing */
        CHECK(!next(owner, &e));
        now_ms += BLE_RETRY_MS;
        drain();
    }
    CHECK_EQ(got, n + 1);
    CHECK(board.head == board.tail);
    teardown();
}

/* Records never wrap: one that does not fit before the end goes to the
 * start, after a skip mark, many times round. */
TEST(ble_queue_wraps) {
    setup();
    uint8_t w[200];
    for (int round = 0; round < 40; round++) {
        uint32_t len = (uint32_t)(37 * round % 200) + 1u;
        for (uint32_t i = 0; i < len; i++) w[i] = (uint8_t)(round + (int)i);
        CHECK(radio(BLE_EV_WRITTEN, (uint32_t)round, w, len));
        CHECK(radio(BLE_EV_MTU, 100u + (uint32_t)round, NULL, 0));
        if (round % 3 == 2) continue;                     /* let some pile up */
        drain();
        Ev e;
        while (next(owner, &e)) {
            if ((e.kind & 0xFF) == BLE_EV_WRITTEN) {
                int r = e.kind >> 8;
                uint32_t l = (uint32_t)(37 * r % 200) + 1u;
                CHECK_EQ(e.len, l);
                for (uint32_t i = 0; i < l; i++) CHECK_EQ(e.data[i], (uint8_t)(r + (int)i));
            } else {
                CHECK_EQ(e.kind & 0xFF, BLE_EV_MTU);
            }
        }
    }
    CHECK_EQ(board.lost, 0u);
    teardown();
}

TEST(ble_set_value) {
    setup();
    command(owner, BLE_CMD_SET_VALUE | 0 << 8, "\x64", 1);
    CHECK_EQ(set_chr, 0u); CHECK_EQ(set_len, 1u); CHECK_EQ(set_data[0], 0x64);
    command(owner, BLE_CMD_SET_VALUE | 2 << 8, NULL, 0);
    CHECK_EQ(set_chr, 2u); CHECK_EQ(set_len, 0u);
    set_chr = 99;
    command(owner, BLE_CMD_SET_VALUE | 3 << 8, "x", 1);       /* no characteristic 3 */
    CHECK_EQ(set_chr, 99u);
    want_none();                                                /* no reply */
    teardown();
}

/* ---- notifications ---- */

TEST(ble_notify_done_each) {
    setup();
    connect_sub();
    command(owner, BLE_CMD_NOTIFY | 0 << 8, "abc", 3);
    CHECK_EQ(notify_calls, 1); CHECK_EQ(last_chr, 0u); CHECK_EQ(last_len, 3u);
    CHECK(memcmp(last_data, "abc", 3) == 0);
    want_done(1);
    command(owner, BLE_CMD_NOTIFY | 0 << 8, "de", 2);
    want_done(2);
    want_none();
    teardown();
}

/* The stack has no buffer: the Notify waits (no Done), tried again after
 * BLE_RETRY_MS; a second one queues behind it; both are Done in order. */
TEST(ble_notify_again_retries) {
    setup();
    connect_sub();
    notify_rc[0] = BLE_AGAIN; notify_rc[1] = BLE_AGAIN; notify_n = 2;
    command(owner, BLE_CMD_NOTIFY | 0 << 8, "one", 3);
    command(owner, BLE_CMD_NOTIFY | 0 << 8, "two", 3);
    command(owner, BLE_CMD_NOTIFY | 0 << 8, "three", 5);       /* a third in flight: refused */
    want_done(-1);
    want_none();
    CHECK_EQ(notify_calls, 1);
    drain();                                                    /* not due */
    CHECK_EQ(notify_calls, 1);
    now_ms += BLE_RETRY_MS;
    drain();
    CHECK_EQ(notify_calls, 2);                                  /* AGAIN again */
    want_none();
    now_ms += BLE_RETRY_MS;
    drain();
    CHECK_EQ(notify_calls, 4);
    CHECK(memcmp(last_data, "two", 3) == 0);
    want_done(1);
    want_done(2);
    want_none();
    teardown();
}

/* A stack that never has a buffer: the Notify fails after BLE_RETRY_MAX tries. */
TEST(ble_notify_gives_up) {
    setup();
    connect_sub();
    always_again = true;
    command(owner, BLE_CMD_NOTIFY | 0 << 8, "x", 1);
    for (unsigned k = 0; k < BLE_RETRY_MAX; k++) {
        want_none();
        now_ms += BLE_RETRY_MS;
        drain();
    }
    want_done(-1);
    CHECK_EQ(notify_calls, (int)BLE_RETRY_MAX + 1);
    always_again = false;
    command(owner, BLE_CMD_NOTIFY | 0 << 8, "y", 1);    /* the next one starts afresh */
    want_done(1);
    teardown();
}

TEST(ble_notify_refusals) {
    setup();
    command(owner, BLE_CMD_NOTIFY | 0 << 8, "x", 1);           /* not connected */
    want_done(-1);
    connect_sub();
    command(owner, BLE_CMD_NOTIFY | 2 << 8, "x", 1);           /* not subscribed */
    want_done(-1);
    command(owner, BLE_CMD_NOTIFY | 1 << 8, "x", 1);           /* no Notify flag */
    want_done(-1);
    command(owner, BLE_CMD_NOTIFY | 7 << 8, "x", 1);           /* no such characteristic */
    want_done(-1);
    command(owner, BLE_CMD_NOTIFY | 0 << 8, NULL, 0);          /* empty */
    want_done(-1);
    uint8_t big[200];
    memset(big, 7, sizeof big);
    command(owner, BLE_CMD_NOTIFY | 0 << 8, big, 183);         /* MTU 185: 182 at most */
    want_done(-1);
    command(owner, BLE_CMD_NOTIFY | 0 << 8, big, 182);
    want_done(1);
    CHECK_EQ(notify_calls, 1);
    notify_rc[1] = GOGO_E_IO; notify_n = 2;                     /* the stack refuses it */
    command(owner, BLE_CMD_NOTIFY | 0 << 8, "y", 1);
    want_done(-1);
    CHECK(radio(BLE_EV_SUBSCRIBED, 0, NULL, 0));                /* unsubscribed */
    drain();
    want_post(BLE_EV_SUBSCRIBED, NULL, 0);
    command(owner, BLE_CMD_NOTIFY | 0 << 8, "z", 1);
    want_done(-1);
    teardown();
}

/* A Done the owner has no room for is owed, then delivered by the
 * source once there is room, without another command to carry it. */
TEST(ble_done_owed_until_room) {
    setup();
    connect_sub();
    notify_rc[0] = BLE_AGAIN; notify_n = 1;
    command(owner, BLE_CMD_NOTIFY | 0 << 8, "a", 1);
    gogo_words_t max = owner->max_words;
    owner->max_words = (gogo_words_t)(owner->mem_words + owner->mbox_words);   /* no room now */
    now_ms += BLE_RETRY_MS;
    drain();
    CHECK_EQ(notify_calls, 2);                                  /* the stack took it: its Done is owed */
    want_none();
    CHECK(board.retry_at != 0);
    owner->max_words = max;
    now_ms += BLE_RETRY_MS;
    drain();
    want_done(1);
    want_none();
    teardown();
}

TEST(ble_disconnect_fails_in_flight) {
    setup();
    connect_sub();
    notify_rc[0] = BLE_AGAIN; notify_n = 1;
    command(owner, BLE_CMD_NOTIFY | 0 << 8, "a", 1);
    command(owner, BLE_CMD_NOTIFY | 0 << 8, "b", 1);
    want_none();
    CHECK(radio(BLE_EV_DISCONNECTED, 0x08, NULL, 0));
    now_ms += BLE_RETRY_MS;
    drain();
    want_done(-1);
    want_done(-1);
    want_post(BLE_EV_DISCONNECTED | 0x08 << 8, NULL, 0);
    want_none();
    CHECK_EQ(notify_calls, 1);
    CHECK(radio(BLE_EV_CONNECTED, 23, "c", 1));                 /* a new connection: nothing subscribed */
    drain();
    want_post(BLE_EV_CONNECTED | 23 << 8, "c", 1);
    command(owner, BLE_CMD_NOTIFY | 0 << 8, "a", 1);
    want_done(-1);
    teardown();
}

/* Close with Notifies in flight: no Done, the data released, the radio stopped. */
TEST(ble_close_in_flight) {
    setup();
    connect_sub();
    notify_rc[0] = BLE_AGAIN; notify_n = 1;
    command(owner, BLE_CMD_NOTIFY | 0 << 8, "a", 1);
    Port *p = bport();
    CHECK(p->held[0] != 0);
    port_close(&vm, bp);
    CHECK_EQ(stops, 1);
    want_none();
    CHECK(board.port == NULL);
    teardown();
}

/* A command from another actor than the owner works too, a malformed one is ignored. */
TEST(ble_commands_from_anyone) {
    setup();
    connect_sub();
    Actor *other = actor_new(&vm, 256);
    command(other, BLE_CMD_NOTIFY | 0 << 8, "q", 1);
    want_done(1);                                               /* to the owner */
    CHECK(other->mbox_head == NULL);
    other->hp = 0;
    term f[2] = { term_from_int(1), term_from_int(2) };
    port_command(&vm, other, bp, heap_struct(other, TYPE_DONE, 2, f));
    command(other, 99, "q", 1);
    want_none();
    CHECK_EQ(notify_calls, 1);
    teardown();
}
