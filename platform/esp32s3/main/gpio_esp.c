/* `gpio` port driver for the ESP32-S3: same semantics as the unix driver
 * (platform/unix/gpio_unix.c, spec M3 5.1).
 *
 * cfg {Pin, Mode, Edge, DebounceMs}: modes Input=0 InputPullUp=1
 * InputPullDown=2 Output=3; edges None=0 Rising=1 Falling=2 Both=3;
 * DebounceMs <= 0 means 20. Command TYPE_WRITE {Value} drives an output
 * pin. Events {a = pin, b = level after the edge}.
 *
 * Inputs: an any-edge GPIO interrupt; the ISR reads the level, and for a
 * change that matches the configured edge pushes a PortEvent (timestamped
 * with platform_now_ms's clock) to the event ring and notifies the VM task.
 * The ISR never locks or allocates. The VM debounces (port.c); for a
 * single-edge pin the ISR also drops an edge that follows the previous
 * level change by less than the debounce time, so the bounce of a key's
 * release (rise, fall, rise) is not taken for a new press.
 *
 * Pins the platform uses are refused: USB D-/D+ (19, 20), the in-package
 * flash and PSRAM, the LCD (21, 38-41, 45) and the M5PM1 I2C bus (47, 48).
 * The StickS3's ESP32-S3-PICO-1-N8R8 has 8 MB octal PSRAM in the package
 * (docs.m5stack.com/en/core/StickS3: "8MB Octal PSRAM"; esptool reports
 * "Embedded PSRAM 8MB (AP_3v3)"). The ESP32-S3-PICO-1 datasheet's block
 * diagram wires the flash and PSRAM to SPICS1/SPIHD/SPIWP/SPICS0/SPICLK/
 * SPIQ/SPID (GPIO26-32) and the octal PSRAM's SPIIO4-7/SPIDQS to GPIO33-37,
 * so 26-37 are all refused. */
#include "port.h"
#include "evq.h"
#include "opcodes.h"
#include "platform.h"
#include "platform_esp.h"

#include "driver/gpio.h"
#include "esp_err.h"

enum { M_INPUT = 0, M_PULLUP = 1, M_PULLDOWN = 2, M_OUTPUT = 3 };
enum { E_NONE = 0, E_RISING = 1, E_FALLING = 2, E_BOTH = 3 };

#define NPINS 49

static const uint8_t reserved[] = {19, 20, 21, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 45, 47, 48};

/* Per-pin state shared with the ISR. The VM task fills the config fields
 * before the handler is added and clears `open` after it is removed; the
 * ISR alone writes level and changed_ms while the pin is open. */
typedef struct {
    uint16_t index, gen;
    uint16_t debounce_ms;
    uint8_t open, mode, edge;
    volatile uint8_t level;
    volatile uint32_t changed_ms;
} PinSlot;

static PinSlot pins[NPINS];
static bool isr_service;

static bool pin_reserved(int32_t pin) {
    for (unsigned i = 0; i < sizeof reserved; i++) {
        if (reserved[i] == pin) return true;
    }
    return false;
}

static void gpio_isr(void *arg) {
    PinSlot *s = arg;
    int pin = (int)(s - pins);
    uint8_t level = (uint8_t)gpio_get_level((gpio_num_t)pin);
    if (level == s->level) return;
    s->level = level;
    uint32_t now = (uint32_t)platform_esp_ms();
    uint32_t since = now - s->changed_ms;
    s->changed_ms = now;
    if (!(s->edge == E_BOTH || s->edge == (level ? E_RISING : E_FALLING))) return;
    if (s->edge != E_BOTH && since < s->debounce_ms) return;
    PortEvent e;
    e.port = s->index;
    e.gen = s->gen;
    e.a = pin;
    e.b = level;
    e.time_ms = now;
    evq_push(&e);
    platform_notify();
}

static int g_open(Port *p, uint16_t index, const int32_t *cfg, uint32_t n) {
    if (n != 4) return -1;
    int32_t pin = cfg[0], mode = cfg[1], edge = cfg[2];
    if (pin < 0 || pin >= NPINS || mode < M_INPUT || mode > M_OUTPUT || edge < E_NONE || edge > E_BOTH) return -1;
    if (pin_reserved(pin) || pins[pin].open) return -1;
    if (mode == M_OUTPUT ? !GPIO_IS_VALID_OUTPUT_GPIO(pin) : !GPIO_IS_VALID_GPIO(pin)) return -1;
    bool irq = mode != M_OUTPUT && edge != E_NONE;
    if (irq && !isr_service) {
        esp_err_t err = gpio_install_isr_service(0);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return -1;
        isr_service = true;
    }
    p->debounce_ms = (uint16_t)(cfg[3] > 0 ? (cfg[3] > 65535 ? 65535 : cfg[3]) : 20);
    p->state[0] = (uint32_t)pin;
    p->state[1] = (uint32_t)mode;
    p->state[2] = (uint32_t)edge;

    gpio_config_t c = {
        .pin_bit_mask = 1ULL << pin,
        .mode = mode == M_OUTPUT ? GPIO_MODE_OUTPUT : GPIO_MODE_INPUT,
        .pull_up_en = mode == M_PULLUP ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = mode == M_PULLDOWN ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
        .intr_type = irq ? GPIO_INTR_ANYEDGE : GPIO_INTR_DISABLE,
    };
    if (gpio_config(&c) != ESP_OK) return -1;

    PinSlot *s = &pins[pin];
    s->index = index;
    s->gen = p->gen;
    s->debounce_ms = p->debounce_ms;
    s->mode = (uint8_t)mode;
    s->edge = (uint8_t)edge;
    s->level = (uint8_t)gpio_get_level((gpio_num_t)pin);
    s->changed_ms = (uint32_t)platform_esp_ms() - s->debounce_ms;   /* stable since "long ago" */
    s->open = 1;
    if (irq && gpio_isr_handler_add((gpio_num_t)pin, gpio_isr, s) != ESP_OK) {
        s->open = 0;
        gpio_reset_pin((gpio_num_t)pin);
        return -1;
    }
    return 0;
}

static void g_command(Port *p, uint32_t type, const int32_t *f, uint32_t n) {
    if (type != TYPE_WRITE || n != 1 || p->state[1] != M_OUTPUT) return;
    gpio_set_level((gpio_num_t)p->state[0], f[0] != 0);
}

static void g_close(Port *p) {
    gpio_num_t pin = (gpio_num_t)p->state[0];
    PinSlot *s = &pins[pin];
    if (s->mode != M_OUTPUT && s->edge != E_NONE) {
        gpio_intr_disable(pin);
        gpio_isr_handler_remove(pin);
    }
    gpio_reset_pin(pin);
    s->open = 0;
}

static bool g_can_event(const Port *p) { return p->state[1] != M_OUTPUT && p->state[2] != E_NONE; }

const PortDriver gpio_esp_driver = {
    "gpio", TYPE_GPIO, TYPE_EDGE, g_open, g_command, g_close, g_can_event
};
