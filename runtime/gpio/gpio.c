/* The `gpio` port driver over a GpioHal (gpio.h). Port state: state[0]
 * the pin (a whole word: any pin below the HAL's npins); state[1] DB_*
 * flags | mode << 4 | edge << 6 | debounce_ms << 16 (the debounce window
 * and the cfg); state[2..3] the vm_now ms of the last delivery (low,
 * high). The pin's shared state is hal->pins[pin]. */
#include "gpio.h"
#include "actor.h"
#include "evq.h"
#include "host.h"

#define PIN(p)  ((p)->state[0])
#define MODE(p) ((p)->state[1] >> 4 & 3u)
#define EDGE(p) ((p)->state[1] >> 6 & 3u)
#define DEB(p)  ((p)->state[1] >> 16)
/* state[1] flags: a value was delivered (the window runs from last_t); a
 * trailing value is held; the level last delivered; the level held. */
enum { DB_DELIVERED = 1, DB_PENDING = 2, DB_LAST_B = 4, DB_PEND_B = 8 };

static const GpioHal *hal_of(const Port *p) { return (const GpioHal *)p->drv->hw; }

static bool reserved(const GpioHal *h, uint32_t pin) {
    return h->reserved && ((h->reserved[pin / 32u] >> (pin % 32u)) & 1u);
}

static bool watched(uint32_t mode, uint32_t edge) { return mode != GPIO_OUTPUT && edge != GPIO_EDGE_NONE; }

bool gpio_edge(const GpioHal *hal, uint32_t pin, int level, uint32_t now_ms) {
    if (pin >= hal->npins) return false;
    GpioPin *s = &hal->pins[pin];
    if (!s->open || s->mode == GPIO_OUTPUT) return false;
    uint8_t lv = level != 0;
    if (lv == s->level) return false;
    s->level = lv;
    uint32_t since = s->settled ? UINT32_MAX : now_ms - s->changed_ms;
    s->settled = 0;
    s->changed_ms = now_ms;
    if (!(s->edge == GPIO_BOTH || s->edge == (lv ? GPIO_RISING : GPIO_FALLING))) return false;
    if (s->edge != GPIO_BOTH && since < s->debounce_ms) return false;
    PortEvent e;
    e.port = s->index;
    e.gen = s->gen;
    e.a = (int32_t)pin;
    e.b = lv;
    e.time_ms = now_ms;
    evq_push(s->vm, &e);
    return true;
}

int gpio_open(struct Vm *vm, Port *p, uint16_t index, const int32_t *cfg, uint32_t n) {
    const GpioHal *h = hal_of(p);
    if (!h || n != 4) return -1;
    int32_t pin = cfg[0], mode = cfg[1], edge = cfg[2];
    if (pin < 0 || (uint32_t)pin >= h->npins || mode < GPIO_INPUT || mode > GPIO_OUTPUT ||
        edge < GPIO_EDGE_NONE || edge > GPIO_BOTH) return -1;
    GpioPin *s = &h->pins[pin];
    if (reserved(h, (uint32_t)pin) || s->open) return -1;
    bool watch = watched((uint32_t)mode, (uint32_t)edge);
    uint16_t deb = (uint16_t)(cfg[3] > 0 ? (cfg[3] > 65535 ? 65535 : cfg[3]) : 20);
    if (h->config(h->ctx, (uint32_t)pin, mode, watch, &deb) != 0) return -1;
    p->state[0] = (uint32_t)pin;
    p->state[1] = (uint32_t)mode << 4 | (uint32_t)edge << 6 | (uint32_t)deb << 16;   /* deb: gpio_event */
    s->vm = vm;
    s->index = index;
    s->gen = p->gen;
    s->debounce_ms = h->raw_edges ? 0 : deb;   /* the bounce filter's window */
    s->mode = (uint8_t)mode;
    s->edge = (uint8_t)edge;
    s->level = (uint8_t)(h->read(h->ctx, (uint32_t)pin) != 0);
    s->settled = 1;
    vm_barrier();                 /* the slot is whole before gpio_edge sees it open */
    s->open = 1;
    return 0;
}

void gpio_command(struct Vm *vm, Port *p, uint32_t type, const int32_t *f, uint32_t n) {
    (void)vm;
    const GpioHal *h = hal_of(p);
    if (type != TYPE_GPIO_WRITE || n != 1 || MODE(p) != GPIO_OUTPUT) return;
    h->write(h->ctx, PIN(p), f[0] != 0);
}

/* ---- debounce ----
 * Per port, debounce_ms from the cfg (0: off, every edge delivered): an
 * edge at least debounce_ms after the last delivery is delivered at once;
 * one inside the window is held as the trailing value (the latest wins).
 * When the window ends, the held value is delivered if its level differs
 * from the last delivered one, and that delivery opens a new window. The
 * window ends are this driver's event source, registered in a VM when it
 * first holds a value and kept for the VM's life: its ctx is the number of
 * ports holding a value (no memory), so a VM with none pays one test per
 * scheduler pass. All of it runs on the scheduler thread. */

static const PortSource gpio_source;

static bool is_gpio(const Port *p) { return p->drv && p->drv->event == gpio_event; }

/* The VM's slot of the driver's source, NULL while it is not registered. */
static PortSrcSlot *src_slot(struct Vm *vm) {
    for (PortSrcSlot *q = vm->sources; q->s; q++) {
        if (q->s == &gpio_source) return q;
    }
    return NULL;
}

/* Adds d to the count of ports holding a value (the source is in). */
static void held_add(struct Vm *vm, intptr_t d) {
    PortSrcSlot *q = src_slot(vm);
    q->ctx = (void *)((intptr_t)q->ctx + d);
}

static uint64_t last_t(const Port *p) { return (uint64_t)p->state[2] | (uint64_t)p->state[3] << 32; }

static void deliver_at(struct Vm *vm, Port *p, int32_t b, uint64_t t) {
    p->state[1] = (p->state[1] & ~(uint32_t)DB_LAST_B) | DB_DELIVERED | (b ? DB_LAST_B : 0u);
    p->state[2] = (uint32_t)t;
    p->state[3] = (uint32_t)(t >> 32);
    port_deliver(vm, p, (int32_t)PIN(p), b);
}

/* Ends p's window if it is over by time t: the held value goes out if it
 * differs from the last delivered one. */
static void flush_due(struct Vm *vm, Port *p, uint64_t t) {
    if (!(p->state[1] & DB_PENDING)) return;
    uint64_t end = last_t(p) + DEB(p);
    if (end > t) return;
    p->state[1] &= ~(uint32_t)DB_PENDING;
    held_add(vm, -1);
    uint32_t f = p->state[1];
    if (!(f & DB_PEND_B) != !(f & DB_LAST_B)) deliver_at(vm, p, (f & DB_PEND_B) != 0, end);
}

/* The event's 32-bit timestamp (made vm_now time by the caller) as a vm_now
 * value, taking it to be at most ~24 days before now (a timestamp after
 * now counts as now). */
static uint64_t event_time(uint32_t ms, uint64_t now) {
    uint32_t back = (uint32_t)now - ms;
    if ((int32_t)back < 0) return now;
    return back > now ? 0u : now - back;
}

void gpio_event(struct Vm *vm, Port *p, const PortEvent *e, uint64_t now) {
    uint32_t deb = DEB(p);
    if (!deb) {
        port_deliver(vm, p, e->a, e->b);
        return;
    }
    uint64_t t = event_time(e->time_ms - (uint32_t)vm->t0, now);
    flush_due(vm, p, t);
    if (!(p->state[1] & DB_DELIVERED) || t >= last_t(p) + deb) {
        deliver_at(vm, p, e->b, t);
        return;
    }
    if (!(p->state[1] & DB_PENDING)) {
        if (!src_slot(vm) && !port_add_source(vm, &gpio_source, NULL)) {
            deliver_at(vm, p, e->b, t);   /* no source slot: not held */
            return;
        }
        held_add(vm, 1);
        p->state[1] |= DB_PENDING;
    }
    p->state[1] = (p->state[1] & ~(uint32_t)DB_PEND_B) | (e->b ? DB_PEND_B : 0u);
}

/* The earliest end of a window holding a value; UINT64_MAX if none. */
static uint64_t next_end(const struct Vm *vm, const void *held) {
    uint64_t best = UINT64_MAX;
    for (uint32_t i = 0; held && i < vm->port_n; i++) {
        const Port *p = port_at(vm, i);
        if (is_gpio(p) && (p->state[1] & DB_PENDING) && last_t(p) + DEB(p) < best) best = last_t(p) + DEB(p);
    }
    return best;
}

static bool src_pending(const struct Vm *vm, void *held) { return held && next_end(vm, held) <= vm_now(vm); }

static void src_drain(struct Vm *vm, void *held, uint64_t now) {
    (void)held;
    for (uint32_t i = 0; i < vm->port_n; i++) {
        Port *p = port_at(vm, i);
        if (is_gpio(p)) flush_due(vm, p, now);
    }
}

static bool src_any_live(const struct Vm *vm, void *held) {
    (void)vm;
    (void)held;
    return false;                 /* a held value keeps no VM alive (port_any_live) */
}

static bool src_next_deadline(const struct Vm *vm, void *held, uint64_t *deadline) {
    *deadline = next_end(vm, held);
    return *deadline != UINT64_MAX;
}

static const PortSource gpio_source = { .pending = src_pending, .drain = src_drain, .any_live = src_any_live,
                                        .next_deadline = src_next_deadline };

void gpio_close(struct Vm *vm, Port *p) {
    const GpioHal *h = hal_of(p);
    uint32_t pin = PIN(p);
    GpioPin *s = &h->pins[pin];
    s->open = 0;
    vm_barrier();
    h->release(h->ctx, pin, (int)MODE(p), watched(MODE(p), EDGE(p)));
    if (p->state[1] & DB_PENDING) held_add(vm, -1);   /* the held value is dropped */
}

bool gpio_can_event(const struct Vm *vm, const Port *p) {
    (void)vm;
    const GpioHal *h = hal_of(p);
    return watched(MODE(p), EDGE(p)) && (!h->live || h->live(h->ctx));
}
