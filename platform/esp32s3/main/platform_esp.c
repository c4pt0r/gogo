/* vm/platform.h for the ESP32-S3 (FreeRTOS, USB-Serial/JTAG console). */
#include "sdkconfig.h"
#include "platform.h"
#include "config.h"
#include "port.h"
#include "platform_esp.h"
#include "console.h"
#if CONFIG_GOGO_NET
#include "net.h"
#endif
#include "input.h"
#include "board.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_random.h"
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

/* Program output never carries \x02 on serial: it starts `gogo config`
 * reply lines (config_esp.c), so a program cannot fake one. Each \x02 is
 * written as '?'. */
static void write_serial(FILE *f, const char *s, size_t n) {
    const char *p;
    while (n > 0 && (p = memchr(s, '\x02', n)) != NULL) {
        size_t k = (size_t)(p - s);
        fwrite(s, 1, k, f);
        fputc('?', f);
        s += k + 1u;
        n -= k + 1u;
    }
    fwrite(s, 1, n, f);
}

/* Program output and diagnostics both go to the serial console (stdout is
 * flushed first so they interleave in order) and to the on-screen console:
 * stdout white, stderr (crash reports) red. */
void platform_write(const char *s, size_t n) {
    write_serial(stdout, s, n);
    console_esp_write(s, n, CON_WHITE);
}
void platform_write_err(const char *s, size_t n) {
    fflush(stdout);
    write_serial(stderr, s, n);
    console_esp_write(s, n, CON_RED);
}

/* Platform lines (boot, exit, image errors): serial and screen, white. */
void platform_esp_say(const char *fmt, ...) {
    char b[160];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b - 1, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n > sizeof b - 2) n = (int)sizeof b - 2;
    b[n++] = '\n';
    fflush(stdout);
    write_serial(stderr, b, (size_t)n);
    console_esp_write(b, (size_t)n, CON_WHITE);
}

void platform_esp_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    flockfile(stderr);
    fflush(stdout);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    funlockfile(stderr);
    va_end(ap);
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

/* The first refused request, recorded where it happened so the exit line
 * can say what limited the VM: the arena cap or the internal heap. */
static struct {
    bool set, by_cap;
    size_t request, used, heap_free, heap_largest;
} refusal;

static void note_refusal(size_t bytes, bool by_cap) {
    if (refusal.set) return;
    refusal.set = true;
    refusal.by_cap = by_cap;
    refusal.request = bytes;
    refusal.used = arena_used;
    refusal.heap_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    refusal.heap_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

void platform_esp_report_refusal(void) {
    if (!refusal.set) return;
    platform_esp_say("gogo: first refused alloc %lu bytes (%s): arena used %lu of %lu, free RAM %lu (largest %lu)",
                     (unsigned long)refusal.request, refusal.by_cap ? "arena cap" : "heap exhausted",
                     (unsigned long)refusal.used, (unsigned long)GOGO_ARENA_BYTES,
                     (unsigned long)refusal.heap_free, (unsigned long)refusal.heap_largest);
}

void *platform_chunk_alloc(size_t bytes) {
    if (GOGO_ARENA_BYTES && bytes > GOGO_ARENA_BYTES - arena_used) {
        note_refusal(bytes, true);
        return NULL;
    }
    void *p = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (p) arena_used += bytes;
    else note_refusal(bytes, false);
    return p;
}

void platform_chunk_free(void *p, size_t bytes) {
    if (!p) return;
    arena_used = bytes > arena_used ? 0 : arena_used - bytes;
    heap_caps_free(p);
}

uint64_t platform_now_ms(void) { return platform_esp_ms(); }

uint32_t platform_random(void) { return esp_random(); }

/* vm_run start: Now() counts from here, after the LCD bring-up. */
void platform_time_reset(void) { t0_us = esp_timer_get_time(); }

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

/* The strong platform_input, defined here (always linked) rather than next
 * to the hardware: input/source.c has a weak default, and an archive member
 * is only pulled in to satisfy an undefined symbol, never to override a
 * weak one that is already defined. */
extern const InputHw input_esp_hw;
const InputHw *platform_input(void) { return &input_esp_hw; }

const struct PortDriver *const *platform_port_drivers(void) {
    static const struct PortDriver *const list[] = {
        &gpio_esp_driver,
#if CONFIG_GOGO_NET
        &net_netif_driver, &net_udp_driver, &net_tcp_driver, &net_tcp_listen_driver,
#endif
        &input_kbd_driver, &input_power_driver, &input_imu_driver, &input_ir_tx_driver, &input_ir_rx_driver,
        &input_mic_driver, &input_spk_driver,
        NULL
    };
    return list;
}

/* Module code memory (vm/platform.h). Only a board with PSRAM (the StickS3,
 * sdkconfig.sticks3) has any: code is only read (as words) by the interpreter,
 * never executed, so the slower external RAM is fine (MALLOC_CAP_32BIT only
 * states the word access; byte access works on the S3 too), and the internal heap (the VM
 * arena, Wi-Fi, LCD buffers) is not touched. PSRAM is reachable only through
 * these caps (CONFIG_SPIRAM_USE_CAPS_ALLOC): plain malloc never returns it.
 * Defined here, in an always-linked object, not next to hardware code: vm/
 * has weak defaults, and an archive member is never pulled in to override
 * one that is already defined. */
#if CONFIG_SPIRAM
void *platform_code_alloc(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_32BIT); }
void platform_code_free(void *p, size_t n) {
    (void)n;
    heap_caps_free(p);
}
#else
void *platform_code_alloc(size_t n) {
    (void)n;
    return NULL;
}
void platform_code_free(void *p, size_t n) {
    (void)p;
    (void)n;
}
#endif

/* The capabilities this build provides (vm/platform.h CAP_*). */
uint32_t platform_caps(void) {
    uint32_t c = 0;
#if CONFIG_GOGO_NET
    c |= CAP_NET;
#endif
#if HAS_KBD && CONFIG_GOGO_KBD
    c |= CAP_KBD;
#endif
#if CONFIG_GOGO_BATTERY
    c |= CAP_POWER;
#endif
#if HAS_SD && CONFIG_GOGO_SD
    c |= CAP_SD;
#endif
#if HAS_IMU && CONFIG_GOGO_IMU
    c |= CAP_IMU;
#endif
#if HAS_IR_TX && CONFIG_GOGO_IR
    c |= CAP_IR_TX;
#endif
#if HAS_IR_RX && CONFIG_GOGO_IR
    c |= CAP_IR_RX;
#endif
#if HAS_MIC && CONFIG_GOGO_AUDIO
    c |= CAP_MIC;
#endif
#if HAS_SPK && CONFIG_GOGO_AUDIO
    c |= CAP_SPK;
#endif
    return c;
}
