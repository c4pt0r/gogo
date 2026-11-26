/* The ESP32-S3's GpioHal: the `gpio` port is runtime/gpio/gpio.c over it.
 *
 * A watched input gets an any-edge GPIO interrupt; the handler reads the
 * level and hands it to gpio_edge (timestamped with esp_host.now_ms,
 * platform_esp_ms), then notifies the VM task when an event was pushed.
 * The handler never locks or allocates.
 *
 * Pins the platform uses are refused (board.h BOARD_RESERVED_MASK). On the
 * StickS3: USB D-/D+ (19, 20), the in-package flash and PSRAM, the LCD
 * (21, 38-41, 45) and the M5PM1 I2C bus (47, 48); on the Cardputer ADV:
 * USB, the flash (26-32), the LCD (33-38) and the SD card (12, 14, 39, 40).
 * The StickS3's ESP32-S3-PICO-1-N8R8 has 8 MB octal PSRAM in the package
 * (docs.m5stack.com/en/core/StickS3: "8MB Octal PSRAM"; esptool reports
 * "Embedded PSRAM 8MB (AP_3v3)"). The ESP32-S3-PICO-1 datasheet's block
 * diagram wires the flash and PSRAM to SPICS1/SPIHD/SPIWP/SPICS0/SPICLK/
 * SPIQ/SPID (GPIO26-32) and the octal PSRAM's SPIIO4-7/SPIDQS to GPIO33-37,
 * so 26-37 are all refused. A pin the chip cannot use in the mode asked
 * (an input-only pin as an output) is refused by config. */
#include "gpio.h"
#include "platform_esp.h"
#include "board.h"

#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"

#define NPINS 49

static const uint32_t reserved[2] = { (uint32_t)(BOARD_RESERVED_MASK), (uint32_t)((BOARD_RESERVED_MASK) >> 32) };

static const GpioHal gpio_esp_hal;
static GpioPin pins[NPINS];     /* the driver's per-pin state (gpio.h) */
static bool isr_service;

static void gpio_isr(void *arg) {
    uint32_t pin = (uint32_t)(uintptr_t)arg;
    int level = gpio_get_level((gpio_num_t)pin);
    if (gpio_edge(&gpio_esp_hal, pin, level, (uint32_t)platform_esp_ms())) platform_notify();
}

static int h_config(void *ctx, uint32_t pin, int mode, bool watch, uint16_t *debounce_ms) {
    (void)ctx;
    (void)debounce_ms;
    if (mode == GPIO_OUTPUT ? !GPIO_IS_VALID_OUTPUT_GPIO((int)pin) : !GPIO_IS_VALID_GPIO((int)pin)) return -1;
    if (watch && !isr_service) {
        esp_err_t err = gpio_install_isr_service(0);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return -1;
        isr_service = true;
    }
    gpio_config_t c = {
        .pin_bit_mask = 1ULL << pin,
        .mode = mode == GPIO_OUTPUT ? GPIO_MODE_OUTPUT : GPIO_MODE_INPUT,
        .pull_up_en = mode == GPIO_PULLUP ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = mode == GPIO_PULLDOWN ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
        .intr_type = watch ? GPIO_INTR_ANYEDGE : GPIO_INTR_DISABLE,
    };
    if (gpio_config(&c) != ESP_OK) return -1;
    if (watch && gpio_isr_handler_add((gpio_num_t)pin, gpio_isr, (void *)(uintptr_t)pin) != ESP_OK) {
        gpio_reset_pin((gpio_num_t)pin);
        return -1;
    }
    return 0;
}

static void h_write(void *ctx, uint32_t pin, int level) {
    (void)ctx;
    gpio_set_level((gpio_num_t)pin, level != 0);
}

static int h_read(void *ctx, uint32_t pin) {
    (void)ctx;
    return gpio_get_level((gpio_num_t)pin);
}

static void h_release(void *ctx, uint32_t pin, int mode, bool watch) {
    (void)ctx;
    (void)mode;
    if (watch) {
        gpio_intr_disable((gpio_num_t)pin);
        gpio_isr_handler_remove((gpio_num_t)pin);
    }
    gpio_reset_pin((gpio_num_t)pin);
}

static const GpioHal gpio_esp_hal = {
    .npins = NPINS, .reserved = reserved, .pins = pins, .config = h_config, .write = h_write, .read = h_read,
    .release = h_release,
};

const PortDriver gpio_esp_driver = GPIO_DRIVER(&gpio_esp_hal);
