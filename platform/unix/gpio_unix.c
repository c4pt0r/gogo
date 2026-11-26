/* unix gpio simulation: the GpioHal under runtime/gpio/gpio.c.
 *
 * Output pins: a Write prints "gpio: pin N = V" on stderr.
 *
 * Input pins get their levels from stdin, read by the shared input reader
 * (input_unix.c): script lines "<pin> <0|1>" and, in tty mode, the
 * characters mapped with --key <c>=<pin> (a press yields level 0 then 1).
 * A pin starts at 1 with InputPullUp, else at 0. Debounce is forced off in
 * script mode. After EOF no input port can produce events (live false), so
 * the VM may exit.
 */
#include "gpio_unix.h"
#include "platform_unix.h"
#include "input_unix.h"

#include <stdio.h>

static GpioPin pins[GPIO_UNIX_NPINS];     /* the driver's per-pin state (gpio.h) */
static uint8_t pulled_up[GPIO_UNIX_NPINS];   /* the level an input starts at */

static int h_config(void *ctx, uint32_t pin, int mode, bool watch, uint16_t *debounce_ms) {
    (void)ctx;
    (void)watch;
    pulled_up[pin] = mode == GPIO_PULLUP;
    if (!input_unix_tty()) *debounce_ms = 0;
    if (mode != GPIO_OUTPUT) input_unix_attach();
    return 0;
}

static void h_write(void *ctx, uint32_t pin, int level) {
    (void)ctx;
    char b[48];
    int len = snprintf(b, sizeof b, "gpio: pin %u = %d\n", (unsigned)pin, level != 0);
    if (len > 0) platform_write_err(b, (size_t)len);
}

static int h_read(void *ctx, uint32_t pin) {
    (void)ctx;
    return pulled_up[pin];
}

static void h_release(void *ctx, uint32_t pin, int mode, bool watch) {
    (void)ctx;
    (void)pin;
    (void)watch;
    if (mode != GPIO_OUTPUT) input_unix_detach();
}

static bool h_live(void *ctx) {
    (void)ctx;
    return input_unix_live();
}

static const GpioHal gpio_unix_hal = { NULL, GPIO_UNIX_NPINS, NULL, pins, h_config, h_write, h_read, h_release, h_live };

const PortDriver gpio_unix_driver = GPIO_DRIVER(&gpio_unix_hal);

/* ---- input (the shared reader, input_unix.c, calls this) ---- */

void gpio_unix_set_level(int pin, int level) {
    if (gpio_edge(&gpio_unix_hal, (uint32_t)pin, level, (uint32_t)platform_now_ms())) platform_notify();
}
