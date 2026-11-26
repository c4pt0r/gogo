/* The gpio driver over a fake GpioHal on a real VM: the edge match, the
 * single-edge bounce filter, and raw_edges (a fall and a rise in the same
 * millisecond, as the unix --key reader makes them, all reported), and the
 * debounce of the port's events (gpio_event, the driver's event source). */
#include "test.h"
#include "test_host.h"
#include "gpio.h"
#include "evq.h"
#include "actor.h"
#include "msg.h"
#include "signals.h"
#include "interp.h"
#include "config.h"
#include "../include/gogo_errors.h"
#include <string.h>

static GpioPin pins[8];
static int f_config(void *ctx, uint32_t pin, int mode, bool watch, uint16_t *deb) {
    (void)ctx; (void)pin; (void)mode; (void)watch; (void)deb;
    return 0;
}
static void f_write(void *ctx, uint32_t pin, int level) { (void)ctx; (void)pin; (void)level; }
static int f_read(void *ctx, uint32_t pin) { (void)ctx; (void)pin; return 1; }   /* pulled up */
static void f_release(void *ctx, uint32_t pin, int mode, bool watch) { (void)ctx; (void)pin; (void)mode; (void)watch; }

static const GpioHal hal = { .npins = 8, .pins = pins, .config = f_config, .write = f_write, .read = f_read,
                             .release = f_release };
static const GpioHal raw_hal = { .npins = 8, .pins = pins, .config = f_config, .write = f_write, .read = f_read,
                                 .release = f_release, .raw_edges = true };
static const PortDriver drv = GPIO_DRIVER(&hal), raw_drv = GPIO_DRIVER(&raw_hal);

static Vm vm;

static int events(void) {
    PortEvent e;
    int n = 0;
    while (evq_pop(&vm, &e)) n++;
    return n;
}

/* Three presses (fall, rise at the same ms), 100 ms apart, on a Rising
 * pin with the default 20 ms debounce: rises found by the edge match. */
static int presses(const PortDriver *d) {
    Port p;
    memset(&p, 0, sizeof p);
    memset(pins, 0, sizeof pins);
    p.drv = d;
    int32_t cfg[4] = { 5, GPIO_PULLUP, GPIO_RISING, 0 };
    CHECK_EQ(gpio_open(&vm, &p, 0, cfg, 4), 0);
    for (uint32_t t = 1000; t < 1300; t += 100) {
        gpio_edge(d->hw, 5, 0, t);
        gpio_edge(d->hw, 5, 1, t);
    }
    gpio_close(&vm, &p);
    return events();
}

TEST(gpio_raw_edges_same_ms) {
    CHECK_EQ(test_vm_init(&vm, NULL, 16), 0);
    events();
    CHECK_EQ(presses(&raw_drv), 3);   /* every press */
    CHECK_EQ(presses(&drv), 0);       /* a real pin: a rise 0 ms after a fall is bounce */
}

TEST(gpio_edge_match_and_filter) {
    CHECK_EQ(test_vm_init(&vm, NULL, 16), 0);
    events();
    Port p;
    memset(&p, 0, sizeof p);
    memset(pins, 0, sizeof pins);
    p.drv = &drv;
    int32_t cfg[4] = { 3, GPIO_PULLUP, GPIO_FALLING, 20 };
    CHECK_EQ(gpio_open(&vm, &p, 0, cfg, 4), 0);
    CHECK(!gpio_edge(&hal, 3, 1, 10));     /* no change */
    CHECK(gpio_edge(&hal, 3, 0, 100));     /* fall: reported */
    CHECK(!gpio_edge(&hal, 3, 1, 150));    /* rise: not the edge asked for */
    CHECK(!gpio_edge(&hal, 3, 0, 160));    /* fall 10 ms after the rise: bounce */
    CHECK(!gpio_edge(&hal, 3, 1, 200));
    CHECK(gpio_edge(&hal, 3, 0, 300));     /* a new press */
    CHECK(!gpio_edge(&hal, 4, 0, 300));    /* a pin not open */
    CHECK(!gpio_edge(&hal, 99, 0, 300));   /* out of range */
    gpio_close(&vm, &p);
    CHECK(!gpio_edge(&hal, 3, 1, 400));    /* closed */
    CHECK_EQ(events(), 2);
}

/* ---- debounce (gpio_event and the driver's event source) ---- */

/* A HAL that lowers every debounce to 0, as the unix script mode does. */
static int f_config_nodeb(void *ctx, uint32_t pin, int mode, bool watch, uint16_t *deb) {
    (void)ctx; (void)pin; (void)mode; (void)watch;
    *deb = 0;
    return 0;
}
static const GpioHal nodeb_hal = { .npins = 8, .pins = pins, .config = f_config_nodeb, .write = f_write,
                                   .read = f_read, .release = f_release };
static const PortDriver nodeb_drv = GPIO_DRIVER(&nodeb_hal);
static const PortDriver *const drv_list[] = { &drv, &nodeb_drv, NULL };

static Actor *owner;
static Port *dport;

/* The host clock, set by each drain: the source reads vm_now as the
 * scheduler's port_drain(vm, vm_now(vm)) would have it. */
static uint64_t fake_now;
static uint64_t fake_clock(void) { return fake_now; }

static void drain_at(uint64_t t) {
    fake_now = t + vm.t0;
    port_drain(&vm, t);
}

/* A waiting owner and an open "gpio" port of driver d on pin 2 (Both
 * edges, debounce deb ms), on a fresh VM whose time base is t0. */
static void db_open_with(const PortDriver *d, int32_t deb, uint64_t t0) {
    CHECK_EQ(test_vm_init(&vm, NULL, 16), 0);
    evq_reset(&vm);
    vm.t0 = t0;
    memset(pins, 0, sizeof pins);
    port_set_drivers(&vm, drv_list);
    test_host_set_clock(fake_clock);
    fake_now = t0;
    owner = actor_new(&vm, 64);
    CHECK(owner != NULL);
    owner->state = A_WAITING;
    int32_t cfg[4] = { 2, GPIO_INPUT, GPIO_BOTH, deb };
    term p = port_open_with(&vm, owner, d, cfg, 4);
    CHECK(p != T_NIL);
    dport = port_at(&vm, term_pid_slot(p) - vm.port_base);
}

static void db_open(int32_t deb, uint64_t t0) { db_open_with(&drv, deb, t0); }

static void db_close(void) {
    while (runq_pop(&vm)) {}
    actor_exit(&vm, owner, R_NORMAL, T_EMPTYSTR);   /* closes the port */
    test_host_set_clock(NULL);
}

static void db_push(int32_t b, uint32_t t) {
    PortEvent e = { (uint16_t)port_index(&vm, dport), dport->gen, 2, b, t };
    CHECK(evq_push(&vm, &e));
}

static int mbox_len(void) {
    int n = 0;
    for (const Msg *m = owner->mbox_head; m; m = m->next) n++;
    return n;
}

/* The level (field Value) of the n-th queued gpio.Edge. */
static int32_t level(int nth) {
    const Msg *m = owner->mbox_head;
    while (m && nth--) m = m->next;
    CHECK(m != NULL);
    if (!m) return -1;
    CHECK_EQ(m->data[1], (uint32_t)TYPE_GPIO_EDGE);
    CHECK_EQ(m->data[2], term_from_int(2));
    return term_to_int(m->data[3]);
}

/* The driver's source is registered (the test VM has no other). */
static bool source_in(void) { return vm.sources[0].s != NULL; }

TEST(gpio_debounce_burst) {
    db_open(20, 0);
    uint64_t d;
    CHECK(!port_next_deadline(&vm, &d));
    CHECK(source_in());                         /* registered by the open of a debounced input */
    CHECK(vm.sources[0].ctx == NULL);
    /* 5 edges within 20 ms: first delivered at once. */
    db_push(1, 0);
    db_push(0, 4);
    db_push(1, 8);
    db_push(1, 12);
    db_push(0, 16);
    drain_at(16);
    CHECK_EQ(mbox_len(), 1);
    CHECK_EQ(level(0), 1);
    CHECK(source_in());
    CHECK(port_next_deadline(&vm, &d));
    CHECK_EQ(d, 20);
    drain_at(19);
    CHECK_EQ(mbox_len(), 1);
    /* The final level, which differs, is delivered once when the window ends. */
    drain_at(20);
    CHECK_EQ(mbox_len(), 2);
    CHECK_EQ(level(1), 0);
    CHECK(!port_next_deadline(&vm, &d));
    drain_at(100);
    CHECK_EQ(mbox_len(), 2);
    /* A burst ending on the delivered level sends nothing more. */
    db_push(1, 200);
    db_push(0, 205);
    db_push(1, 210);
    drain_at(210);
    CHECK_EQ(mbox_len(), 3);
    CHECK(port_next_deadline(&vm, &d));
    CHECK_EQ(d, 220);
    drain_at(230);
    CHECK_EQ(mbox_len(), 3);
    CHECK(!port_next_deadline(&vm, &d));
    /* A pending trailing value is flushed before a later event in the ring. */
    db_push(0, 300);
    db_push(1, 310);
    db_push(0, 400);
    drain_at(400);
    CHECK_EQ(mbox_len(), 6);
    CHECK_EQ(level(3), 0);
    CHECK_EQ(level(4), 1);
    CHECK_EQ(level(5), 0);
    /* Close drops the pending trailing value (the source stays). */
    db_push(1, 500);
    db_push(0, 505);
    drain_at(505);
    CHECK(port_next_deadline(&vm, &d));
    port_close(&vm, dport->id);
    CHECK(!port_next_deadline(&vm, &d));
    CHECK(source_in());
    CHECK(vm.sources[0].ctx == NULL);           /* no port holds a value */
    db_close();
}

/* A host clock ahead of VM time (t0 != 0, as on ESP after bring-up): event
 * stamps are host time and get -t0 exactly once; the debounce window and the
 * deadline port_next_deadline reports are VM time. */
TEST(gpio_debounce_t0) {
    db_open(20, 5000);
    uint64_t d;
    db_push(1, 5100);                           /* VM time 100 */
    db_push(0, 5104);                           /* VM time 104: trailing */
    drain_at(104);
    CHECK_EQ(mbox_len(), 1);
    CHECK(port_next_deadline(&vm, &d));
    CHECK_EQ(d, 120);                           /* 100 + 20, not 104 + 20 (no offset) */
    drain_at(119);
    CHECK_EQ(mbox_len(), 1);
    drain_at(120);
    CHECK_EQ(mbox_len(), 2);
    CHECK_EQ(level(1), 0);
    /* An edge after the window is delivered at once. */
    db_push(1, 5200);
    drain_at(200);
    CHECK_EQ(mbox_len(), 3);
    db_push(0, 5210);                           /* inside the window again */
    drain_at(210);
    CHECK(port_next_deadline(&vm, &d));
    CHECK_EQ(d, 220);
    db_close();
}

/* Debounce 0 (the unix script mode's config sets it): every edge at once,
 * and no event source. */
TEST(gpio_debounce_off) {
    db_open_with(&nodeb_drv, 20, 0);
    for (uint32_t i = 0; i < 5; i++) db_push((int32_t)(i & 1u), i);
    drain_at(5);
    CHECK_EQ(mbox_len(), 5);
    for (int i = 0; i < 5; i++) CHECK_EQ(level(i), i & 1);
    uint64_t d;
    CHECK(!port_next_deadline(&vm, &d));
    CHECK(!source_in());
    db_close();
}

/* A port holding a trailing value is due only at the end of its window:
 * not work due now (port_due_now) before it, so an idle scheduler sleeps
 * until port_next_deadline instead of spinning; the source's pending()
 * turns true once the window has ended. */

TEST(gpio_debounce_due_at_deadline) {
    db_open(100, 0);
    db_push(1, 0);
    db_push(0, 10);
    drain_at(10);
    CHECK_EQ(mbox_len(), 1);
    uint64_t d;
    CHECK(port_next_deadline(&vm, &d));
    CHECK_EQ(d, 100);
    fake_now = 99;
    CHECK(!port_due_now(&vm));
    CHECK(!port_work_pending(&vm));
    fake_now = 100;
    CHECK(port_due_now(&vm));
    port_drain(&vm, vm_now(&vm));
    CHECK_EQ(mbox_len(), 2);
    CHECK(!port_due_now(&vm));
    db_close();
}

/* The same through the scheduler: a held value is due only at the end of
 * its window, so an idle vm_run sleeps until then (port_next_deadline)
 * instead of spinning. The clock is read a few times per scheduler pass,
 * so the reads bound the passes: a handful, against millions when the
 * pre-sleep check counted the held value as work due now. */
static unsigned db_reads;
static uint64_t db_clock(void);
static uint64_t db_clock(void) {            /* the real clock, counted */
    db_reads++;
    test_host_set_clock(NULL);
    uint64_t t = test_host_now();
    test_host_set_clock(db_clock);
    return t;
}

/* An image whose main returns at once. */
#define MINI_WORDS 27
static void mini_image(uint32_t *w, Image *img) {
    memset(w, 0, MINI_WORDS * sizeof(uint32_t));
    w[0] = IMAGE_MAGIC; w[1] = IMAGE_VERSION; w[2] = 1; w[3] = 2;
    w[4] = 6; w[5] = 1; w[6] = 0; w[7] = 0;
    w[8] = 0; w[9] = 0 | (1u << 16); w[10] = 0;              /* func main */
    w[11] = 2; w[12] = 3; w[13] = 2; w[14] = 3;               /* types Cfg/2, Cfg/2 */
    w[15] = hdr_make(3, K_STRING); w[16] = 4; memcpy(&w[17], "main", 4);
    w[18] = hdr_make(3, K_STRING); w[19] = 3; memcpy(&w[20], "Cfg", 3);
    w[21] = OP_MAKE(OP_RET, 0, 0, 0);
    CHECK_EQ(image_load(img, w, img_seal_main(w)), IMG_OK);
}

static bool f_dead(void *ctx) { (void)ctx; return false; }   /* no edge can come */
static const GpioHal dead_hal = { .npins = 8, .pins = pins, .config = f_config, .write = f_write, .read = f_read,
                                  .release = f_release, .live = f_dead };
static const PortDriver dead_drv = GPIO_DRIVER(&dead_hal);
static const PortDriver *const dead_list[] = { &dead_drv, NULL };

TEST(gpio_debounce_idle_no_spin) {
    uint32_t w[MINI_WORDS]; Image img;
    mini_image(w, &img);
    CHECK_EQ(test_vm_init(&vm, &img, 8), 0);
    evq_reset(&vm);
    memset(pins, 0, sizeof pins);
    port_set_drivers(&vm, dead_list);
    Actor *a = actor_new(&vm, 64);
    CHECK(a != NULL);
    a->state = A_WAITING;
    int32_t cfg[4] = { 2, GPIO_INPUT, GPIO_BOTH, 100 };
    term pid = port_open_with(&vm, a, &dead_drv, cfg, 4);
    CHECK(pid != T_NIL);
    Port *p = port_at(&vm, term_pid_slot(pid) - vm.port_base);
    /* Hold a value: one edge delivered at 0, one inside the window. */
    PortEvent e0 = { (uint16_t)port_index(&vm, p), p->gen, 2, 1, (uint32_t)vm.t0 };
    PortEvent e1 = e0;
    e1.b = 0;
    CHECK(evq_push(&vm, &e0) && evq_push(&vm, &e1));
    port_drain(&vm, 0);
    uint64_t d;
    CHECK(port_next_deadline(&vm, &d));
    CHECK_EQ(d, 100);
    while (runq_pop(&vm)) {}
    /* The port's owner is gone (the flushed value is dropped, nothing runs). */
    Actor *g = actor_new(&vm, 24);
    CHECK(g != NULL);
    term gone = g->id;
    actor_exit(&vm, g, R_NORMAL, T_EMPTYSTR);
    p->owner = gone;
    db_reads = 0;
    test_host_set_clock(db_clock);
    int rc = vm_run(&vm);
    test_host_set_clock(NULL);
    CHECK_EQ(rc, 0);
    CHECK(!port_next_deadline(&vm, &d));    /* flushed at the window's end */
    CHECK(db_reads < 30u);
}

/* Without a free event-source slot a debounced input is refused with
 * TooMany (its debounce could not work), its pin left unconfigured; an
 * output, an input without an edge and an undebounced input still open. */
static bool noop_pending(const struct Vm *v, void *c) { (void)v; (void)c; return false; }
static void noop_drain(struct Vm *v, void *c, uint64_t now) { (void)v; (void)c; (void)now; }
static bool noop_live(const struct Vm *v, void *c) { (void)v; (void)c; return false; }
static const PortSource other[4] = {
    { noop_pending, noop_drain, noop_live, NULL, NULL }, { noop_pending, noop_drain, noop_live, NULL, NULL },
    { noop_pending, noop_drain, noop_live, NULL, NULL }, { noop_pending, noop_drain, noop_live, NULL, NULL },
};

TEST(gpio_debounce_needs_a_source_slot) {
    CHECK_EQ(test_vm_init(&vm, NULL, 16), 0);
    memset(pins, 0, sizeof pins);
    port_set_drivers(&vm, drv_list);
    for (uint32_t i = 0; i < GOGO_MAX_SOURCES; i++) CHECK(port_add_source(&vm, &other[i], NULL));
    Actor *a = actor_new(&vm, 64);
    CHECK(a != NULL);
    int32_t in[4] = { 2, GPIO_PULLUP, GPIO_BOTH, 0 }, out[4] = { 3, GPIO_OUTPUT, GPIO_EDGE_NONE, 0 },
            quiet[4] = { 4, GPIO_INPUT, GPIO_EDGE_NONE, 0 };
    CHECK(port_open_with(&vm, a, &drv, in, 4) == T_NIL);
    CHECK_EQ(vm.open_err, GOGO_E_TOOMANY);
    CHECK(!pins[2].open);
    CHECK(port_open_with(&vm, a, &drv, out, 4) != T_NIL);
    CHECK(port_open_with(&vm, a, &drv, quiet, 4) != T_NIL);
    CHECK(port_open_with(&vm, a, &nodeb_drv, in, 4) != T_NIL);   /* debounce 0: no source needed */
    for (uint32_t i = 0; i < GOGO_MAX_SOURCES; i++) port_remove_source(&vm, &other[i]);
    a->state = A_RUNNABLE;
    actor_exit(&vm, a, R_NORMAL, T_EMPTYSTR);
}

/* A HAL with more than 256 pins: the port keeps the whole pin number, so
 * pin 256 is written and released, not pin 0. */
static GpioPin many[300];
static uint32_t wrote_pin, released_pin;
static void w_write(void *ctx, uint32_t pin, int level) { (void)ctx; (void)level; wrote_pin = pin; }
static void w_release(void *ctx, uint32_t pin, int mode, bool watch) {
    (void)ctx; (void)mode; (void)watch;
    released_pin = pin;
}
static const GpioHal many_hal = { .npins = 300, .pins = many, .config = f_config, .write = w_write,
                                  .read = f_read, .release = w_release };
static const PortDriver many_drv = GPIO_DRIVER(&many_hal);

TEST(gpio_pin_above_255) {
    CHECK_EQ(test_vm_init(&vm, NULL, 16), 0);
    Port p;
    memset(&p, 0, sizeof p);
    p.drv = &many_drv;
    int32_t cfg[4] = { 256, GPIO_OUTPUT, GPIO_EDGE_NONE, 0 }, one = 1;
    CHECK_EQ(gpio_open(&vm, &p, 0, cfg, 4), 0);
    CHECK(many[256].open && !many[0].open);
    gpio_command(&vm, &p, TYPE_GPIO_WRITE, &one, 1);
    CHECK_EQ(wrote_pin, 256u);
    CHECK(!gpio_can_event(&vm, &p));
    gpio_close(&vm, &p);
    CHECK_EQ(released_pin, 256u);
    CHECK(!many[256].open);
}
