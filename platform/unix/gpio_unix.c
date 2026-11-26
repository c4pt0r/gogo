/* unix gpio simulation.
 *
 * Output pins: a Write prints "gpio: pin N = V" on stderr.
 *
 * Input pins get events from stdin, read by the shared input reader
 * (input_unix.c): script lines "<pin> <0|1>" and, in tty mode, the
 * characters mapped with --key <c>=<pin> (a press yields level 0 then 1).
 * Debounce is forced off in script mode. After EOF no input port can
 * produce events (can_event false), so the VM may exit.
 */
#include "gpio_unix.h"
#include "platform_unix.h"
#include "input_unix.h"
#include "evq.h"
#include "opcodes.h"

#include <stdio.h>

#define NPINS GPIO_UNIX_NPINS

enum { M_INPUT = 0, M_PULLUP = 1, M_PULLDOWN = 2, M_OUTPUT = 3 };
enum { E_NONE = 0, E_RISING = 1, E_FALLING = 2, E_BOTH = 3 };

/* Port state: state[0] pin, [1] mode, [2] edge, [3] level | index << 8.
 * pin_vm: the VM whose event ring gets the pin's events. */
static Port *pins[NPINS];
static struct Vm *pin_vm[NPINS];

/* ---- driver ---- */

static int g_open(struct Vm *vm, Port *p, uint16_t index, const int32_t *cfg, uint32_t n) {
    if (n != 4) return -1;
    int32_t pin = cfg[0], mode = cfg[1], edge = cfg[2];
    if (pin < 0 || pin >= NPINS || mode < M_INPUT || mode > M_OUTPUT || edge < E_NONE || edge > E_BOTH) return -1;
    if (pins[pin]) return -1;
    p->state[0] = (uint32_t)pin;
    p->state[1] = (uint32_t)mode;
    p->state[2] = (uint32_t)edge;
    p->state[3] = (mode == M_PULLUP ? 1u : 0u) | ((uint32_t)index << 8);
    p->debounce_ms = input_unix_tty() ? (uint16_t)(cfg[3] > 0 ? (cfg[3] > 65535 ? 65535 : cfg[3]) : 20) : 0;
    pins[pin] = p;
    pin_vm[pin] = vm;
    if (mode != M_OUTPUT) input_unix_attach();
    return 0;
}

static void g_command(struct Vm *vm, Port *p, uint32_t type, const int32_t *f, uint32_t n) { (void)vm;
    char b[48];
    if (type != TYPE_GPIO_WRITE || n != 1 || p->state[1] != M_OUTPUT) return;
    int len = snprintf(b, sizeof b, "gpio: pin %u = %d\n", (unsigned)p->state[0], f[0] != 0);
    if (len > 0) platform_write_err(b, (size_t)len);
}

static void g_close(struct Vm *vm, Port *p) {
    (void)vm;
    unsigned pin = (unsigned)p->state[0];
    if (pin < NPINS && pins[pin] == p) pins[pin] = NULL;
    if (p->state[1] != M_OUTPUT) input_unix_detach();
}

static bool g_can_event(const struct Vm *vm, const Port *p) { (void)vm; return p->state[1] != M_OUTPUT && p->state[2] != E_NONE && input_unix_live(); }

const PortDriver gpio_unix_driver = {
    "gpio", TYPE_GPIO_CONFIG, TYPE_GPIO_EDGE, g_open, g_command, g_close, g_can_event, NULL, NULL, NULL
};

/* ---- input (the shared reader, input_unix.c, calls this) ---- */

/* Pin `pin` is now at `level`: an event if it changed and the edge matches. */
void gpio_unix_set_level(int pin, int level) {
    Port *p = pins[pin];
    if (!p || p->state[1] == M_OUTPUT) return;
    int old = (int)(p->state[3] & 1u);
    if (old == level) return;
    p->state[3] = (p->state[3] & ~1u) | (uint32_t)level;
    uint32_t edge = p->state[2];
    if (!(edge == (level ? E_RISING : E_FALLING) || edge == E_BOTH)) return;
    PortEvent e;
    e.port = (uint16_t)(p->state[3] >> 8);
    e.gen = p->gen;
    e.a = pin;
    e.b = level;
    e.time_ms = (uint32_t)platform_now_ms();
    evq_push(pin_vm[pin], &e);
    platform_notify();
}
