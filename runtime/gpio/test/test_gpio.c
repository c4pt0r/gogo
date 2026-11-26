/* The gpio driver over a fake GpioHal on a real VM: the edge match, the
 * single-edge bounce filter, and raw_edges (a fall and a rise in the same
 * millisecond, as the unix --key reader makes them, all reported). */
#include "test.h"
#include "test_host.h"
#include "gpio.h"
#include "evq.h"
#include "actor.h"
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
