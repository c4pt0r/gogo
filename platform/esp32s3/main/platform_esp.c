/* vm/platform.h for the ESP32-S3 (FreeRTOS, USB-Serial/JTAG console). */
#include "platform.h"
#include "config.h"
#include "port.h"
#include "platform_esp.h"

#include <stdio.h>
#include <stdarg.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

static TaskHandle_t vm_task;
static int64_t t0_us;

void platform_esp_init(void) {
    vm_task = xTaskGetCurrentTaskHandle();
    t0_us = esp_timer_get_time();
    setvbuf(stdout, NULL, _IOLBF, 0);
}

uint64_t platform_esp_ms(void) {
    return (uint64_t)((esp_timer_get_time() - t0_us) / 1000);
}

/* Program output and diagnostics both go to the console; stdout is flushed
 * first so they interleave in order. */
void platform_write(const char *s, size_t n) { fwrite(s, 1, n, stdout); }
void platform_write_err(const char *s, size_t n) {
    fflush(stdout);
    fwrite(s, 1, n, stderr);
}

void platform_esp_say(const char *fmt, ...) {
    va_list ap;
    fflush(stdout);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

void platform_esp_idle(void) {
    fflush(stdout);
    fflush(stderr);
    for (;;) vTaskDelay(portMAX_DELAY);
}

/* VM memory from the internal RAM heap, capped at GOGO_ARENA_BYTES. Only
 * the VM task allocates, so the counter needs no lock. */
static size_t arena_used;

size_t platform_esp_arena_used(void) { return arena_used; }
size_t platform_esp_arena_cap(void) { return GOGO_ARENA_BYTES; }

void *platform_chunk_alloc(size_t bytes) {
    if (GOGO_ARENA_BYTES && bytes > GOGO_ARENA_BYTES - arena_used) return NULL;
    void *p = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (p) arena_used += bytes;
    return p;
}

void platform_chunk_free(void *p, size_t bytes) {
    if (!p) return;
    arena_used = bytes > arena_used ? 0 : arena_used - bytes;
    heap_caps_free(p);
}

uint64_t platform_now_ms(void) { return platform_esp_ms(); }

/* Latched wakeup: the VM task's notification value counts notifies; a wait
 * takes (and clears) it, so a notify before or during a wait ends it. */
void platform_wait(uint32_t ms) {
    TickType_t ticks;
    if (ms == UINT32_MAX) {
        ticks = portMAX_DELAY;
    } else {
        /* round up so a short timeout never becomes 0 ticks (a busy spin) */
        uint64_t t = ((uint64_t)ms * configTICK_RATE_HZ + 999u) / 1000u;
        ticks = t >= portMAX_DELAY ? portMAX_DELAY - 1 : (TickType_t)t;
    }
    (void)ulTaskNotifyTake(pdTRUE, ticks);
}

/* ISR-safe; never locks or allocates. */
void platform_notify(void) {
    if (!vm_task) return;
    if (xPortInIsrContext()) {
        BaseType_t woken = pdFALSE;
        vTaskNotifyGiveFromISR(vm_task, &woken);
        portYIELD_FROM_ISR(woken);
    } else {
        xTaskNotifyGive(vm_task);
    }
}

void platform_barrier(void) { __sync_synchronize(); }

const struct PortDriver *const *platform_port_drivers(void) {
    static const struct PortDriver *const list[] = { &gpio_esp_driver, NULL };
    return list;
}
