/* ESP32-S3 platform layer: shared between main.c, platform_esp.c,
 * gpio_esp.c and image_part.c. */
#ifndef GOGO_PLATFORM_ESP_H
#define GOGO_PLATFORM_ESP_H

#include <stddef.h>
#include <stdint.h>

/* Called from the VM task before vm_init: records the task to notify and
 * the clock origin. */
void platform_esp_init(void);
/* Milliseconds since platform_esp_init or the last platform_time_reset
 * (ISR-safe; platform_now_ms). */
uint64_t platform_esp_ms(void);
size_t platform_esp_arena_used(void);
size_t platform_esp_arena_cap(void);
/* Prints the first refused platform_chunk_alloc (size, cause, arena used,
 * free internal RAM at that moment), if any. */
void platform_esp_report_refusal(void);
/* Prints a line on the console (stderr). */
void platform_esp_say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* Never returns. */
void platform_esp_idle(void) __attribute__((noreturn));

/* Maps the image in the `gogo` partition. 0 and sets words, nwords on success;
 * otherwise prints `gogo: no image` or `gogo: bad image (<reason>)` and
 * returns -1. */
int image_part_map(const uint32_t **words, uint32_t *nwords);

/* Screen (console_task.c, lcd_st7789.c, pm1.c). */
#define LCD_W 240      /* landscape */
#define LCD_H 135
#define LCD_BAND_H 12  /* one console row */

/* Starts the on-screen console: grid and font, then the render task (CPU0),
 * which powers the LCD (M5PM1) and initialises the ST7789 while this task
 * waits for the result. Called once from the VM task before any output.
 * Failures are logged on serial and leave the console off. No-op when built
 * with GOGO_ESP_CONSOLE=0. */
void console_esp_start(void);
/* GOGO_CONSOLE_VERBOSE builds: logs the time the VM spent in console writes. */
void console_esp_report(void);
/* Copies n bytes into the console grid (CON_WHITE / CON_RED) under a short
 * lock and wakes the render task; never waits for the LCD. */
void console_esp_write(const char *s, size_t n, uint8_t color);

/* M5PM1: turns on the LCD rail. 0 on success. */
int pm1_lcd_power_on(void);
/* ST7789: GPIO, SPI bus, reset and init (display on, backlight still off).
 * 0 on success. */
int lcd_init(void);
void lcd_backlight(int on);
/* Writes h landscape rows starting at y from px (LCD_W * h big-endian
 * RGB565 pixels, DMA-capable memory). Blocks until sent. 0 on success. */
int lcd_write_rows(int y, int h, const uint16_t *px);

struct PortDriver;
extern const struct PortDriver gpio_esp_driver;

#endif
