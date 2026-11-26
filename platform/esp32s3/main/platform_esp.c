/* The ESP32-S3 board: its gogo_host and VmBoard (runtime/vm/host.h) on
 * FreeRTOS, with the USB-Serial/JTAG console. */
#include "sdkconfig.h"
#include "host.h"
#include "config.h"
#include "port.h"
#include "platform_esp.h"
#include "console.h"
#if CONFIG_GOGO_NET
#include "net.h"
#endif
#include "input.h"
#include "board.h"
#if CONFIG_MBEDTLS_CUSTOM_MEM_ALLOC
#include "mbedtls/platform.h"
#endif
#if CONFIG_GOGO_TLS
#include "tls.h"
#include "anchor.h"
#include "mbedtls/ssl.h"
#include "esp_crt_bundle.h"
#if !CONFIG_MBEDTLS_CUSTOM_MEM_ALLOC
#error "GOGO_TLS needs CONFIG_MBEDTLS_CUSTOM_MEM_ALLOC (sdkconfig.defaults): mbedTLS memory is esp_tls_hw's"
#endif
#endif

#include <stdbool.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_heap_caps.h"

static TaskHandle_t vm_task;
static int64_t t0_us;

#if CONFIG_MBEDTLS_CUSTOM_MEM_ALLOC
static void mbedtls_mem_init(void);
#endif

void platform_esp_init(void) {
#if CONFIG_MBEDTLS_CUSTOM_MEM_ALLOC
    mbedtls_mem_init();
#endif
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

static void *chunk_alloc(void *ctx, size_t bytes) {
    (void)ctx;
    if (GOGO_ARENA_BYTES && bytes > GOGO_ARENA_BYTES - arena_used) {
        note_refusal(bytes, true);
        return NULL;
    }
    void *p = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (p) arena_used += bytes;
    else note_refusal(bytes, false);
    return p;
}

static void chunk_free(void *ctx, void *p, size_t bytes) {
    (void)ctx;
    arena_used = bytes > arena_used ? 0 : arena_used - bytes;
    heap_caps_free(p);
}

/* The wall clock (esp_host's unix_time_ms / set_unix_time_ms) is the
 * system clock (gettimeofday), so everything that reads time() agrees with
 * gogo/sys: ESP-IDF's mbedTLS (CONFIG_MBEDTLS_HAVE_TIME_DATE) checks
 * certificate validity with time() and gmtime_r, and the tls port runs with
 * GOGO_TLS_MBEDTLS_HOOKS 0 (it installs no time hook into the shared
 * mbedTLS). Unknown until set: the system clock starts at 1970 on every
 * boot (no battery-backed RTC), and wall_known keeps that from counting as
 * a time. vm_set_unix_time_ms applies the floor before it calls set.
 * Scheduler thread only. */
static bool wall_known;

static int64_t host_unix_time_ms(void *ctx) {
    (void)ctx;
    if (!wall_known) return 0;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    int64_t ms = (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
    return ms > 0 ? ms : 1;
}

static int host_set_unix_time_ms(void *ctx, int64_t ms) {
    (void)ctx;
    struct timeval tv = {0, 0};
    if (ms > 0) {
        tv.tv_sec = (time_t)(ms / 1000);
        tv.tv_usec = (suseconds_t)(ms % 1000) * 1000;
    }
    settimeofday(&tv, NULL);
    wall_known = ms > 0;
    return 1;
}

/* mbedTLS memory (CONFIG_MBEDTLS_CUSTOM_MEM_ALLOC: IDF leaves mbedTLS on
 * plain calloc until the application installs its own, which
 * platform_esp_init does first thing, before Wi-Fi starts; from then on
 * every mbedTLS allocation, the tls port's and Wi-Fi's WPA crypto alike,
 * comes from here; an earlier one (IDF's PSA init) is freed here too,
 * heap_caps_free takes any heap pointer). On the
 * StickS3 it prefers PSRAM, so a TLS connection (about 34 KB at its peak:
 * the 16 KB + 4 KB record buffers, the peer's chain, ECDHE and PSA state)
 * does not compete with the VM arena for internal RAM; internal RAM when
 * PSRAM is full or absent (the Cardputer). The bytes outstanding and their
 * peak are counted for `gogo config stats` (any task may allocate, hence
 * the lock). */
#if CONFIG_MBEDTLS_CUSTOM_MEM_ALLOC
static portMUX_TYPE tls_mem_lock = portMUX_INITIALIZER_UNLOCKED;
static size_t tls_mem_now, tls_mem_peak;

static void *tls_mem_calloc(size_t n, size_t size) {
    if (size && n > SIZE_MAX / size) return NULL;
#if CONFIG_SPIRAM
    void *p = heap_caps_calloc_prefer(n, size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#else
    void *p = heap_caps_calloc(n, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#endif
    if (!p) return NULL;
    size_t got = heap_caps_get_allocated_size(p);
    portENTER_CRITICAL(&tls_mem_lock);
    tls_mem_now += got;
    if (tls_mem_now > tls_mem_peak) tls_mem_peak = tls_mem_now;
    portEXIT_CRITICAL(&tls_mem_lock);
    return p;
}

static void tls_mem_free(void *p) {
    if (!p) return;
    size_t got = heap_caps_get_allocated_size(p);
    portENTER_CRITICAL(&tls_mem_lock);
    tls_mem_now = got > tls_mem_now ? 0 : tls_mem_now - got;
    portEXIT_CRITICAL(&tls_mem_lock);
    heap_caps_free(p);
}

static void mbedtls_mem_init(void) { mbedtls_platform_set_calloc_free(tls_mem_calloc, tls_mem_free); }

void platform_esp_tls_mem(size_t *now, size_t *peak) {
    portENTER_CRITICAL(&tls_mem_lock);
    *now = tls_mem_now;
    *peak = tls_mem_peak;
    portEXIT_CRITICAL(&tls_mem_lock);
}
#endif

/* The tls port's hooks (esp_tls_hw, a TlsHw: runtime/net/tls/tls.h). */
#if CONFIG_GOGO_TLS
static void *tls_alloc(size_t n) { return tls_mem_calloc(1, n ? n : 1u); }

/* Trust anchors: IDF's certificate bundle in flash (esp_crt_bundle,
 * CONFIG_MBEDTLS_CERTIFICATE_BUNDLE). esp_crt_bundle_attach gives mbedTLS
 * its dummy CA chain, so the peer's chain is checked link by link
 * restartably (sliced) up to the last certificate it sent; the verify
 * callback then decides that one:
 *  - it IS a bundle root (same subject and key: runtime/net/tls/anchor.h),
 *    whatever signed it: trusted. Needed for cross-signed roots
 *    (example.com sends "SSL.com TLS ECC Root CA 2022" signed by Comodo's
 *    "AAA Certificate Services", which is not in the bundle);
 *  - otherwise IDF's esp_crt_verify_callback: its issuer must be a bundle
 *    root, whose key checks its signature (one unsliced verify).
 * IDF's cross-signed mode (CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_CROSS_SIGNED_VERIFY)
 * would do the first case too, but through a CA callback, with which
 * mbedTLS checks the whole chain in one unsliced call (sdkconfig.defaults).
 *
 * IDF internals relied on (ESP-IDF v6.1.1; check them on an IDF upgrade):
 * the bundle's embed symbols _binary_x509_crt_bundle_start / _end (the
 * default bundle; a bundle set with esp_crt_bundle_set, which gogo never
 * calls, is not seen), its layout (runtime/net/tls/anchor.h), and
 * esp_crt_verify_callback, which esp_crt_bundle.c defines non-static but
 * esp_crt_bundle.h does not declare. Nothing is allocated per connection. */
extern const uint8_t gogo_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t gogo_crt_bundle_end[] asm("_binary_x509_crt_bundle_end");
int esp_crt_verify_callback(void *buf, mbedtls_x509_crt *crt, int depth, uint32_t *flags);

static int tls_esp_verify(void *ctx, mbedtls_x509_crt *crt, int depth, uint32_t *flags) {
    if (tls_anchor_trust(gogo_crt_bundle_start, (size_t)(gogo_crt_bundle_end - gogo_crt_bundle_start), crt, flags))
        return 0;
    return esp_crt_verify_callback(ctx, crt, depth, flags);
}

static int tls_verify_setup(struct mbedtls_ssl_config *conf, void **state) {
    *state = NULL;
    if (esp_crt_bundle_attach(conf) != ESP_OK) return -1;
    mbedtls_ssl_conf_verify(conf, tls_esp_verify, NULL);
    return 0;
}
static void tls_verify_free(void *state) { (void)state; }

static uint64_t tls_clock_us(void) { return (uint64_t)esp_timer_get_time(); }

/* mbedTLS's x509_crt.c matches IP-address names with the platform's
 * inet_pton, which ESP-IDF's headers map to lwip_inet_pton. lwIP is not
 * linked (gogo has its own stack, and lwIP's tcp_* names clash with it), so
 * it is defined here: dotted-quad IPv4 only (AF_INET is 2 in lwIP), 1 if
 * src is one, else 0; anything else (AF_INET6: gogo has no IPv6) is 0, so
 * such a name matches no certificate. */
int lwip_inet_pton(int af, const char *src, void *dst);
int lwip_inet_pton(int af, const char *src, void *dst) {
    uint8_t a[4];
    if (af != 2) return 0;
    for (int i = 0; i < 4; i++) {
        unsigned v = 0, digits = 0;
        while (*src >= '0' && *src <= '9') {
            if (digits && v == 0) return 0;         /* no leading zeros */
            v = v * 10u + (unsigned)(*src++ - '0');
            if (++digits > 3u || v > 255u) return 0;
        }
        if (!digits || *src != (i < 3 ? '.' : '\0')) return 0;
        src++;
        a[i] = (uint8_t)v;
    }
    memcpy(dst, a, sizeof a);
    return 1;
}

/* The hardware RNG: a CSPRNG while the radio is on, which it is whenever a
 * TLS connection can exist. IDF's mbedTLS draws its keys from the same
 * source (its mbedtls_psa_external_get_random), so this only guards open. */
static int tls_random(uint8_t *out, size_t n) {
    esp_fill_random(out, n);
    return 0;
}

static const TlsHw esp_tls_hw = { tls_alloc, tls_mem_free, tls_verify_setup, tls_verify_free, tls_clock_us, tls_random };
#endif

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

/* Module code memory (gogo_host.code_alloc). Only a board with PSRAM (the StickS3,
 * sdkconfig.sticks3) has any: code is only read (as words) by the interpreter,
 * never executed, so the slower external RAM is fine (MALLOC_CAP_32BIT only
 * states the word access; byte access works on the S3 too), and the internal heap (the VM
 * arena, Wi-Fi, LCD buffers) is not touched. PSRAM is reachable only through
 * these caps (CONFIG_SPIRAM_USE_CAPS_ALLOC): plain malloc never returns it.
 * Without PSRAM the host has no code_alloc (NULL: the VM keeps the code). */
#if CONFIG_SPIRAM
static void *code_alloc(void *ctx, size_t n) {
    (void)ctx;
    return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_32BIT);
}
static void code_free(void *ctx, void *p, size_t n) {
    (void)ctx;
    (void)n;
    heap_caps_free(p);
}
#else
#define code_alloc NULL
#define code_free NULL
#endif

/* ---- the host table ---- */

static void host_write(void *ctx, int stream, const char *s, size_t n) {
    (void)ctx;
    if (stream == 2) platform_write_err(s, n);
    else platform_write(s, n);
}
static uint64_t host_now(void *ctx) { (void)ctx; return platform_esp_ms(); }
static void host_wake(void *ctx) { (void)ctx; platform_notify(); }
static void host_wait(void *ctx, uint32_t ms) { (void)ctx; platform_wait(ms); }
static uint32_t host_random(void *ctx) { (void)ctx; return esp_random(); }
static int32_t host_config_get(void *ctx, const uint8_t *k, uint32_t kl, uint8_t *buf, uint32_t cap) {
    (void)ctx;
    return platform_config_get(k, kl, buf, cap);
}

/* The wall clock is the system clock (host_unix_time_ms above), unknown
 * after boot until SNTP or sys.SetTime sets it. */
const gogo_host esp_host = {
    host_write, chunk_alloc, chunk_free, host_now, host_wake, host_wait, host_unix_time_ms, host_set_unix_time_ms,
    host_random, NULL,
    host_config_get, code_alloc, code_free
};

/* ---- the board table ---- */

/* What this build provides: the drivers and mounts it really has (board.h
 * HAS_* and the Kconfig switches). An image's requirements are checked
 * against these lists at load. */
static const struct PortDriver *const esp_ports[] = {
    &gpio_esp_driver,
#if CONFIG_GOGO_NET
    &net_netif_driver, &net_udp_driver, &net_tcp_driver, &net_tcp_listen_driver,
#endif
#if CONFIG_GOGO_TLS
    &net_tls_driver,
#endif
#if HAS_KBD && CONFIG_GOGO_KBD
    &input_kbd_driver,
#endif
#if CONFIG_GOGO_BATTERY
    &input_power_driver,
#endif
#if HAS_IMU && CONFIG_GOGO_IMU
    &input_imu_driver,
#endif
#if HAS_IR_TX && CONFIG_GOGO_IR
    &input_ir_tx_driver,
#endif
#if HAS_IR_RX && CONFIG_GOGO_IR
    &input_ir_rx_driver,
#endif
#if HAS_MIC && CONFIG_GOGO_AUDIO
    &input_mic_driver,
#endif
#if HAS_SPK && CONFIG_GOGO_AUDIO
    &input_spk_driver,
#endif
    NULL
};

static const char *const esp_mounts[] = {
    "/flash",
#if HAS_SD && CONFIG_GOGO_SD
    "/sd",
#endif
    NULL
};

extern const InputHw input_esp_hw;

const VmBoard esp_board = {
    esp_ports, &input_esp_hw,
#if CONFIG_GOGO_NET
    &wifi_esp_netif,
#else
    NULL,
#endif
#if CONFIG_GOGO_TLS
    &esp_tls_hw,
#else
    NULL,
#endif
    esp_mounts
};
