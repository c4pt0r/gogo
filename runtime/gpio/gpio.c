/* The `gpio` port driver over a GpioHal (gpio.h). Port state: state[0]
 * pin, [1] mode, [2] edge; the pin's shared state is hal->pins[pin]. */
#include "gpio.h"
#include "evq.h"
#include "host.h"

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
    p->debounce_ms = deb;
    p->state[0] = (uint32_t)pin;
    p->state[1] = (uint32_t)mode;
    p->state[2] = (uint32_t)edge;
    s->vm = vm;
    s->index = index;
    s->gen = p->gen;
    s->debounce_ms = deb;
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
    if (type != TYPE_GPIO_WRITE || n != 1 || p->state[1] != GPIO_OUTPUT) return;
    h->write(h->ctx, p->state[0], f[0] != 0);
}

void gpio_close(struct Vm *vm, Port *p) {
    (void)vm;
    const GpioHal *h = hal_of(p);
    uint32_t pin = p->state[0];
    GpioPin *s = &h->pins[pin];
    s->open = 0;
    vm_barrier();
    h->release(h->ctx, pin, (int)p->state[1], watched(p->state[1], p->state[2]));
}

bool gpio_can_event(const struct Vm *vm, const Port *p) {
    (void)vm;
    const GpioHal *h = hal_of(p);
    return watched(p->state[1], p->state[2]) && (!h->live || h->live(h->ctx));
}
