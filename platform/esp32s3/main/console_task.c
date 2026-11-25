/* On-screen debug console: platform_write / platform_write_err mirror into a
 * platform/common Console grid; a low-priority render task draws the dirty
 * rows on the ST7789.
 *
 * The VM task only takes `lock` (a spinlock) long enough to append bytes to
 * the grid (console_write, in chunks of at most WRITE_CHUNK bytes) and, once
 * per frame, gives the render task a notification; it never waits for SPI.
 * The render task holds `lock` only to take the dirty mask and copy the
 * grid (~1.8 KB) into a snapshot, renders and sends the dirty rows from the
 * snapshot outside the lock, and starts at most one frame per
 * RENDER_PERIOD_MS, so a stream of prints coalesces into few frames.
 *
 * Built with GOGO_ESP_CONSOLE=0 the whole thing compiles to no-ops (the
 * timing baseline); GOGO_ESP_CONSOLE_VERBOSE=1 logs bring-up and frame
 * stats on serial.
 */
#include "platform_esp.h"
#include "board.h"
#include "console.h"
#include "font.h"

#ifndef GOGO_ESP_CONSOLE
#define GOGO_ESP_CONSOLE 1
#endif
#ifndef GOGO_ESP_CONSOLE_VERBOSE
#define GOGO_ESP_CONSOLE_VERBOSE 0
#endif

#if GOGO_ESP_CONSOLE

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define RENDER_PERIOD_MS 50
#define RENDER_PRIORITY 1 /* below the VM task (5); just above idle */
#define RENDER_CORE 0     /* the VM task is pinned to CPU1 */
#define RENDER_STACK 4096

/* platform/common/font/gogo12.font, embedded by main/CMakeLists.txt. */
extern const uint8_t font_blob_start[] asm("_binary_gogo12_font_start");
extern const uint8_t font_blob_end[] asm("_binary_gogo12_font_end");

static Console con;        /* shared; guarded by lock */
static Console snap;       /* render task only */
static Font font;
static const Font *fontp;  /* NULL: draw box glyphs */
/* A spinlock, not a mutex: every hold is a few microseconds (a bounded
 * console_write chunk, or the snapshot memcpy), far cheaper than a mutex
 * round trip on the VM's print path. */
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
#define WRITE_CHUNK 64 /* bytes appended per lock hold */
static TaskHandle_t render_task;
static volatile bool started;
static bool kicked; /* guarded by lock: render task already notified */

static void serial_log(const char *msg) {
    fflush(stdout);
    fputs(msg, stderr);
    fputc('\n', stderr);
}

static uint16_t *px; /* one 240x12 row band, DMA-capable; render task only */

/* Bring-up result for console_esp_start: 0 ok, else the failure text. */
static const char *volatile bringup_err;
static TaskHandle_t starter;

#define BRINGUP_NOTIFY_INDEX 1 /* the VM task's platform_wait uses index 0 */
#define BRINGUP_TIMEOUT_MS 3000

/* Runs on CPU0. The M5PM1 (StickS3) / SPI bring-up happens here rather than on the VM
 * task so the SPI driver's interrupt is allocated on this core and its
 * completions never interrupt the VM. */
static void render_main(void *arg) {
    (void)arg;
#if HAS_PM1
    if (pm1_lcd_power_on() != 0) bringup_err = "M5PM1 not responding";
    else
#endif
    if (lcd_init() != 0) bringup_err = "ST7789 init failed";
    bool ok = bringup_err == NULL;
    xTaskNotifyGiveIndexed(starter, BRINGUP_NOTIFY_INDEX);
    if (!ok) vTaskDelete(NULL);

    /* The 3 pixel rows under the 11 console rows (132..134) stay black. */
    memset(px, 0, (size_t)LCD_W * LCD_BAND_H * sizeof(uint16_t));
    lcd_write_rows(CONSOLE_ROWS * CONSOLE_CELL_H, LCD_H - CONSOLE_ROWS * CONSOLE_CELL_H, px);

    bool lit = false;
    TickType_t last = xTaskGetTickCount() - pdMS_TO_TICKS(RENDER_PERIOD_MS);
#if GOGO_ESP_CONSOLE_VERBOSE
    uint32_t frames = 0, rows = 0;
    int64_t busy_us = 0;
#endif
    for (;;) {
        /* All rows start dirty (console_init), so the first pass runs at
         * once and clears the screen. */
        if (lit) ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        TickType_t now = xTaskGetTickCount();
        if (now - last < pdMS_TO_TICKS(RENDER_PERIOD_MS))
            vTaskDelay(pdMS_TO_TICKS(RENDER_PERIOD_MS) - (now - last));
        last = xTaskGetTickCount();
        (void)ulTaskNotifyTake(pdTRUE, 0); /* this frame covers writes so far */

        taskENTER_CRITICAL(&lock);
        kicked = false;
        uint16_t dirty = console_take_dirty(&con);
        if (dirty) memcpy(&snap, &con, sizeof snap);
        taskEXIT_CRITICAL(&lock);

#if GOGO_ESP_CONSOLE_VERBOSE
        int64_t t0 = esp_timer_get_time();
#endif
        for (int r = 0; r < CONSOLE_ROWS; r++) {
            if (!(dirty & (1u << r))) continue;
            console_render_row(&snap, fontp, r, px);
            /* console_render_row gives host-order (little-endian) RGB565;
             * the panel takes the high byte first (RAMCTRL 0x00 0xC0). */
            for (int i = 0; i < LCD_W * LCD_BAND_H; i++) px[i] = (uint16_t)((px[i] << 8) | (px[i] >> 8));
            lcd_write_rows(r * CONSOLE_CELL_H, CONSOLE_CELL_H, px);
#if GOGO_ESP_CONSOLE_VERBOSE
            rows++;
#endif
        }
#if GOGO_ESP_CONSOLE_VERBOSE
        busy_us += esp_timer_get_time() - t0;
        if (dirty && (++frames == 1 || frames % 20 == 0)) {
            char b[96];
            snprintf(b, sizeof b, "gogo: lcd frames %lu rows %lu busy %lld us", (unsigned long)frames,
                     (unsigned long)rows, (long long)busy_us);
            serial_log(b);
        }
#endif
        if (!lit) {
            lcd_backlight(1);
            lit = true;
        }
    }
}

void console_esp_start(void) {
    console_init(&con);
    if (font_init(&font, font_blob_start, (size_t)(font_blob_end - font_blob_start))) {
        fontp = &font;
        /* The code-point tables and the half-width bitmaps (~31 KB, contiguous
         * in the blob) are what every glyph lookup's binary search touches.
         * Read from flash they go through the cache the VM's mmapped image
         * shares, and rendering evicted the VM's code and image; keep a copy
         * in internal RAM when there is room. Wide bitmaps stay in flash. */
        size_t hot = (size_t)(font.wide_bm - font.half_cp);
        uint8_t *ram = heap_caps_malloc(hot, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (ram) {
            memcpy(ram, font.half_cp, hot);
            font.wide_cp = ram + (font.wide_cp - font.half_cp);
            font.half_bm = ram + (font.half_bm - font.half_cp);
            font.half_cp = ram;
        }
    } else {
        fontp = NULL;
        serial_log("gogo: font blob invalid, console uses box glyphs");
    }
    px = heap_caps_malloc((size_t)LCD_W * LCD_BAND_H * sizeof(uint16_t), MALLOC_CAP_DMA);
    if (!px) {
        serial_log("gogo: lcd console off (no DMA buffer)");
        return;
    }
    int64_t t0 = esp_timer_get_time();
    starter = xTaskGetCurrentTaskHandle();
    if (xTaskCreatePinnedToCore(render_main, "gogo_lcd", RENDER_STACK, NULL, RENDER_PRIORITY, &render_task,
                                RENDER_CORE) != pdPASS) {
        serial_log("gogo: lcd console off (no render task)");
        return;
    }
    if (ulTaskNotifyTakeIndexed(BRINGUP_NOTIFY_INDEX, pdTRUE, pdMS_TO_TICKS(BRINGUP_TIMEOUT_MS)) == 0) {
        serial_log("gogo: lcd console off (bring-up timed out)");
        return;
    }
    if (bringup_err) {
        char b[64];
        snprintf(b, sizeof b, "gogo: lcd console off (%s)", bringup_err);
        serial_log(b);
        return;
    }
    started = true;
#if GOGO_ESP_CONSOLE_VERBOSE
    char b[64];
    snprintf(b, sizeof b, "gogo: lcd ready in %lld ms", (long long)((esp_timer_get_time() - t0) / 1000));
    serial_log(b);
#else
    (void)t0;
#endif
}

#if GOGO_ESP_CONSOLE_VERBOSE
static int64_t write_us;
static uint32_t write_calls;
#endif

void console_esp_write(const char *s, size_t n, uint8_t color) {
    if (!started || n == 0) return;
#if GOGO_ESP_CONSOLE_VERBOSE
    int64_t t0 = esp_timer_get_time();
#endif
    bool kick = false;
    while (n > 0) {
        size_t k = n < WRITE_CHUNK ? n : WRITE_CHUNK;
        taskENTER_CRITICAL(&lock);
        console_write(&con, s, k, color);
        if (!kicked) kicked = kick = true;
        taskEXIT_CRITICAL(&lock);
        s += k;
        n -= k;
    }
    /* At most one notification per frame: the render task clears `kicked`
     * when it snapshots the grid. */
    if (kick) xTaskNotifyGive(render_task);
#if GOGO_ESP_CONSOLE_VERBOSE
    write_us += esp_timer_get_time() - t0;
    write_calls++;
#endif
}

void console_esp_report(void) {
#if GOGO_ESP_CONSOLE_VERBOSE
    char b[80];
    snprintf(b, sizeof b, "gogo: console writes %lu, %lld us on the VM task", (unsigned long)write_calls,
             (long long)write_us);
    serial_log(b);
#endif
}

#else /* !GOGO_ESP_CONSOLE */

void console_esp_start(void) {}
void console_esp_report(void) {}
void console_esp_write(const char *s, size_t n, uint8_t color) {
    (void)s;
    (void)n;
    (void)color;
}

#endif
