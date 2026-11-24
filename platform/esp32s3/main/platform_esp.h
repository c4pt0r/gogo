/* ESP32-S3 platform layer: shared between main.c, platform_esp.c,
 * gpio_esp.c and image_part.c. */
#ifndef GOGO_PLATFORM_ESP_H
#define GOGO_PLATFORM_ESP_H

#include <stddef.h>
#include <stdint.h>

/* Called from the VM task before vm_init: records the task to notify and
 * the clock origin. */
void platform_esp_init(void);
/* Milliseconds since platform_esp_init (ISR-safe; platform_now_ms). */
uint64_t platform_esp_ms(void);
size_t platform_esp_arena_used(void);
size_t platform_esp_arena_cap(void);
/* Prints a line on the console (stderr). */
void platform_esp_say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* Never returns. */
void platform_esp_idle(void) __attribute__((noreturn));

/* Maps the image in the `gogo` partition. 0 and sets words, nwords on success;
 * otherwise prints `gogo: no image` or `gogo: bad image (<reason>)` and
 * returns -1. */
int image_part_map(const uint32_t **words, uint32_t *nwords);

struct PortDriver;
extern const struct PortDriver gpio_esp_driver;

#endif
