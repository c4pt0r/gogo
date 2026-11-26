/* The `gpio` port driver, one for every board (pure C99), over the
 * board's GpioHal: pin numbers, which ones are refused, and four pin
 * operations. Validation, the reserved pins, the edge match and the
 * single-edge bounce filter live here once; a board writes only the HAL.
 *
 * cfg {Pin, Mode, Edge, DebounceMs}: modes Input=0 InputPullUp=1
 * InputPullDown=2 Output=3; edges None=0 Rising=1 Falling=2 Both=3;
 * DebounceMs <= 0 means 20. Command TYPE_GPIO_WRITE {Value} drives an
 * output pin. Events {a = pin, b = level after the edge}.
 *
 * Inputs: the HAL reports every level change of a pin it was asked to
 * watch by calling gpio_edge (from an interrupt handler, or the thread that
 * reads a simulation's input). gpio_edge keeps the pin's last level, and
 * for a change that matches the configured edge pushes a PortEvent to the
 * event ring of the VM that opened the pin; the caller then wakes that VM.
 * It never locks or allocates. The VM debounces (port.c); for a
 * single-edge pin gpio_edge also drops an edge that follows the previous
 * level change by less than the debounce time, so the bounce of a key's
 * release (rise, fall, rise) is not taken for a new press. */
#ifndef GOGO_GPIO_H
#define GOGO_GPIO_H

#include <stdbool.h>
#include <stdint.h>
#include "port.h"
#include "opcodes.h"

enum { GPIO_INPUT = 0, GPIO_PULLUP = 1, GPIO_PULLDOWN = 2, GPIO_OUTPUT = 3 };
enum { GPIO_EDGE_NONE = 0, GPIO_RISING = 1, GPIO_FALLING = 2, GPIO_BOTH = 3 };

/* One pin's state, shared between the VM thread and gpio_edge. The board
 * gives npins of them, zeroed (static storage), in GpioHal.pins; only the
 * gpio driver writes them. The VM thread fills a slot before it sets
 * `open` and clears `open` before it releases the pin; gpio_edge alone
 * writes level, changed_ms and settled while the pin is open. */
typedef struct GpioPin {
    struct Vm *vm;              /* whose event ring gets the pin's events */
    uint16_t index, gen;        /* the port's */
    uint16_t debounce_ms;
    uint8_t mode, edge;
    volatile uint8_t open;
    volatile uint8_t level;
    volatile uint8_t settled;   /* no change seen since open: stable "long ago" */
    volatile uint32_t changed_ms;
} GpioPin;

/* A board's pins. Every hook gets ctx and a pin below npins that is not
 * reserved; the driver calls them on the VM thread only.
 *  - config: makes pin an input (with the pull of mode) or an output. With
 *    watch true (an input with an edge) the HAL then calls gpio_edge on
 *    every level change it sees until release. *debounce_ms holds the
 *    port's debounce; the HAL may lower it (the unix script mode sets 0).
 *    0 ok; else the pin is refused (the chip cannot do that mode there),
 *    with nothing left configured.
 *  - write: drives an output pin to level (0 or 1).
 *  - read: the pin's level now (0 or 1); called after config.
 *  - release: undoes config (watch as given to it); no gpio_edge call
 *    for the pin may start after it returns.
 *  - live (optional, NULL = always): false once no edge can come any more
 *    (the unix stdin script hit EOF), so an input port stops keeping the
 *    VM alive.
 *  - raw_edges: the HAL reports clean edges, both of a press possibly in
 *    the same millisecond (the unix simulation's key presses), so the
 *    single-edge bounce filter is off; the port's debounce (port.c) still
 *    applies.
 *
 * Interrupts on more than one core: gpio_edge for a pin must not run
 * while gpio_close releases that pin. On the ESP32-S3 the GPIO interrupt
 * is installed from the VM task, so it runs on the VM's core and cannot
 * overlap a close. A board whose edge interrupt may run on another core
 * (an RP2040 with the IRQ on core 0 and the VM on core 1) must route it to
 * the VM's core, or have release wait until a handler already running has
 * returned ("no call may start after it returns" does not cover one in
 * flight). An event pushed late is harmless: its generation is stale and
 * port_drain drops it. */
typedef struct GpioHal {
    void *ctx;
    uint32_t npins;
    const uint32_t *reserved;   /* bit p of reserved[p / 32]: pin p refused; NULL = none */
    GpioPin *pins;              /* npins slots */
    int  (*config)(void *ctx, uint32_t pin, int mode, bool watch, uint16_t *debounce_ms);
    void (*write)(void *ctx, uint32_t pin, int level);
    int  (*read)(void *ctx, uint32_t pin);
    void (*release)(void *ctx, uint32_t pin, int mode, bool watch);
    bool (*live)(void *ctx);
    bool raw_edges;
} GpioHal;

/* Pin `pin` of hal is now at level (0 or 1), at the host's now_ms (low 32
 * bits) now_ms. True when an event was pushed: the caller wakes the VM.
 * Interrupt-safe; ignored for a pin that is not open as a watched input. */
bool gpio_edge(const GpioHal *hal, uint32_t pin, int level, uint32_t now_ms);

/* The driver's entry points, for GPIO_DRIVER. */
int  gpio_open(struct Vm *vm, Port *p, uint16_t index, const int32_t *cfg, uint32_t n);
void gpio_command(struct Vm *vm, Port *p, uint32_t type, const int32_t *f, uint32_t n);
void gpio_close(struct Vm *vm, Port *p);
bool gpio_can_event(const struct Vm *vm, const Port *p);

/* A board's gpio driver instance over its HAL:
 *   const PortDriver my_gpio_driver = GPIO_DRIVER(&my_gpio_hal); */
#define GPIO_DRIVER(hal) \
    { .name = PORT_NAME_GPIO, .cfg_type = TYPE_GPIO_CONFIG, .event_type = TYPE_GPIO_EDGE, .open = gpio_open, \
      .command = gpio_command, .close = gpio_close, .can_event = gpio_can_event, .hw = (hal) }

#endif
