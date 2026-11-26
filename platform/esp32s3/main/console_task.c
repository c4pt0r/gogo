/* On-screen debug console: platform_write / platform_write_err mirror into a
 * runtime/console Console grid; a low-priority render task draws the dirty
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
 * The "screen" port (GOGO_SCREEN, runtime/screen/) takes the LCD over
 * while it is open: its hooks (esp_screen_hw) copy the grid rows a frame
 * changed into `scr_cells` under the same lock and wake the render task,
 * which then draws those rows (screen_render_row, one row copied out under
 * the lock at a time) instead of the console. The console grid keeps
 * taking writes meanwhile; closing the port redraws it whole.
 *
 * Built with GOGO_ESP_CONSOLE=0 the whole thing compiles to no-ops (the
 * timing baseline; the screen port then refuses to open);
 * GOGO_ESP_CONSOLE_VERBOSE=1 logs bring-up and frame stats on serial, and
 * the screen's grid as text when the port closes.
 */
#include "sdkconfig.h"
#include "platform_esp.h"
#include "board.h"
#include "console.h"
#include "font.h"
#if CONFIG_GOGO_SCREEN
#include "screen_port.h"
#endif

#ifndef GOGO_ESP_CONSOLE
#define GOGO_ESP_CONSOLE 1
#endif
#ifndef GOGO_ESP_CONSOLE_VERBOSE
#define GOGO_ESP_CONSOLE_VERBOSE 0
#endif

#if GOGO_ESP_CONSOLE

#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define RENDER_PERIOD_MS 50
#define RENDER_PRIORITY 1 /* below the VM task (5); just above idle */
#define RENDER_CORE 0     /* the VM task is pinned to CPU1 */
#define RENDER_STACK 4096

/* The blob main/CMakeLists.txt embeds for GOGO_FONT_CJK (the symbol is named
 * after the file); font_init takes either, a glyph the blob lacks is a box. */
#if CONFIG_GOGO_FONT_CJK
extern const uint8_t font_blob_start[] asm("_binary_gogo12_font_start");
extern const uint8_t font_blob_end[] asm("_binary_gogo12_font_end");
#else
extern const uint8_t font_blob_start[] asm("_binary_gogo12_ascii_font_start");
extern const uint8_t font_blob_end[] asm("_binary_gogo12_ascii_font_end");
#endif

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

#if CONFIG_GOGO_SCREEN
/* The screen port's grid as the render task sees it; guarded by lock. */
#define SCR_COLS 40
#define SCR_ROWS 11
static uint32_t *scr_cells;   /* SCR_COLS * SCR_ROWS, allocated at the first open, kept */
static ScreenGrid scr;        /* cols, rows, cursor of the open grid (cell unused) */
static uint32_t scr_dirty;    /* rows to draw */
static bool scr_on;           /* the screen port has the LCD */
static bool scr_clear;        /* black the whole LCD first (the port just opened) */

/* Render task only: draws the screen's dirty rows. */
static void render_screen(uint32_t dirty, bool clear) {
    static uint32_t row[SCR_COLS];
    if (clear) {
        memset(px, 0, (size_t)LCD_W * LCD_BAND_H * sizeof(uint16_t));
        for (int y = 0; y < LCD_H; y += LCD_BAND_H)
            lcd_write_rows(y, y + LCD_BAND_H <= LCD_H ? LCD_BAND_H : LCD_H - y, px);
    }
    for (int r = 0; r < SCR_ROWS; r++) {
        if (!(dirty & (1u << r))) continue;
        ScreenGrid one;
        taskENTER_CRITICAL(&lock);
        if (!scr_on || r >= scr.rows) {
            taskEXIT_CRITICAL(&lock);
            break;
        }
        one = scr;
        memcpy(row, &scr_cells[r * SCR_COLS], (size_t)scr.cols * sizeof row[0]);
        taskEXIT_CRITICAL(&lock);
        one.cell = row;
        one.rows = 1;
        one.ky = one.ky == r ? 0 : 255;   /* the cursor, if it is on this row */
        int w = one.cols * CONSOLE_CELL_W;
        screen_render_row(&one, fontp, 0, px);
        /* rendered with stride w; spread to the LCD's rows (w <= LCD_W) */
        for (int y = LCD_BAND_H - 1; y >= 0; y--) {
            memmove(px + y * LCD_W, px + y * w, (size_t)w * sizeof(uint16_t));
            memset(px + y * LCD_W + w, 0, (size_t)(LCD_W - w) * sizeof(uint16_t));
        }
        for (int i = 0; i < LCD_W * LCD_BAND_H; i++) px[i] = (uint16_t)((px[i] << 8) | (px[i] >> 8));
        lcd_write_rows(r * CONSOLE_CELL_H, CONSOLE_CELL_H, px);
    }
}
#endif

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
#if CONFIG_GOGO_SCREEN
        if (scr_on) {
            uint32_t sd = scr_dirty;
            bool clear = scr_clear;
            scr_dirty = 0;
            scr_clear = false;
            taskEXIT_CRITICAL(&lock);
            render_screen(sd, clear);
            if (!lit) {
                lcd_backlight(1);
                lit = true;
            }
            continue;
        }
#endif
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

#if CONFIG_GOGO_SCREEN
static int scr_open(uint8_t cols, uint8_t rows) {
    if (!started || scr_on) return -1;   /* no LCD, or another Vm has it */
    if (!scr_cells) {
        scr_cells = heap_caps_malloc(SCR_COLS * SCR_ROWS * sizeof(uint32_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!scr_cells) return -1;
    }
    taskENTER_CRITICAL(&lock);
    memset(&scr, 0, sizeof scr);
    scr.cols = cols;
    scr.rows = rows;
    scr_dirty = 0;
    scr_clear = true;
    scr_on = true;
    if (!kicked) kicked = true;
    taskEXIT_CRITICAL(&lock);
    xTaskNotifyGive(render_task);
#if GOGO_ESP_CONSOLE_VERBOSE
    platform_esp_log("gogo: screen %ux%u on", (unsigned)cols, (unsigned)rows);
#endif
    return 0;
}

static void scr_present(const ScreenGrid *g, uint32_t dirty) {
    bool kick = false;
    for (int r = 0; r < g->rows && r < SCR_ROWS; r++) {
        if (!(dirty & (1u << r))) continue;
        taskENTER_CRITICAL(&lock);   /* one row per hold: a few microseconds */
        memcpy(&scr_cells[r * SCR_COLS], &g->cell[r * g->cols], (size_t)g->cols * sizeof(uint32_t));
        scr_dirty |= 1u << r;
        taskEXIT_CRITICAL(&lock);
    }
    taskENTER_CRITICAL(&lock);
    scr.kx = g->kx;
    scr.ky = g->ky;
    scr.cursor_on = g->cursor_on;
    if (!kicked) kicked = kick = true;
    taskEXIT_CRITICAL(&lock);
    if (kick) xTaskNotifyGive(render_task);
}

static void scr_close(void) {
#if GOGO_ESP_CONSOLE_VERBOSE
    /* the last grid, as text, for checking a board without looking at it */
    static char line[SCR_COLS * 3 + 1];
    ScreenGrid view;
    taskENTER_CRITICAL(&lock);
    view = scr;
    taskEXIT_CRITICAL(&lock);
    view.cell = scr_cells;   /* the VM task is the only writer: safe to read here */
    view.cols = SCR_COLS;    /* rows are SCR_COLS apart */
    for (int r = 0; r < view.rows; r++) {
        screen_row_utf8(&view, r, line, sizeof line);
        platform_esp_log("gogo: screen |%s|", line);
    }
#endif
    taskENTER_CRITICAL(&lock);
    scr_on = false;
    scr_dirty = 0;
    con.dirty = (uint16_t)((1u << CONSOLE_ROWS) - 1u);   /* the console comes back whole */
    bool kick = !kicked;
    kicked = true;
    taskEXIT_CRITICAL(&lock);
    if (kick) xTaskNotifyGive(render_task);
#if GOGO_ESP_CONSOLE_VERBOSE
    platform_esp_log("gogo: screen off");
#endif
}

static const ScreenHw esp_screen_hw = { SCR_COLS, SCR_ROWS, SCR_COLS, SCR_ROWS, SCREEN_A_ALL, scr_open, scr_present, scr_close };

static int esp_screen_open(struct Vm *vm, Port *p, uint16_t index, const int32_t *cfg, uint32_t n) {
    (void)index;
    return screen_port_open(vm, p, cfg, n, &esp_screen_hw);
}
#endif

#else /* !GOGO_ESP_CONSOLE */

#if CONFIG_GOGO_SCREEN
static int esp_screen_open(struct Vm *vm, Port *p, uint16_t index, const int32_t *cfg, uint32_t n) {
    (void)vm, (void)p, (void)index, (void)cfg, (void)n;
    return -1;   /* no console task: no LCD */
}
#endif

void console_esp_start(void) {}
void console_esp_report(void) {}
void console_esp_write(const char *s, size_t n, uint8_t color) {
    (void)s;
    (void)n;
    (void)color;
}

#endif

#if CONFIG_GOGO_SCREEN
const PortDriver esp_screen_driver = SCREEN_PORT_DRIVER(esp_screen_open);
#endif
