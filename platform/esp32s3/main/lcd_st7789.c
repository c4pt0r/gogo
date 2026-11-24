/* ST7789P3 LCD of the M5StickS3: 135x240 panel used as landscape 240x135,
 * 4-wire SPI (MOSI G39, SCK G40, DC G45, CS G41, RST G21) on SPI3 at 40 MHz
 * with DMA; backlight on G38 (on = high).
 *
 * Reference (read only, not vendored; MIT / FreeBSD): M5GFX 28d4794
 * (2026-11-05), github.com/m5stack/M5GFX:
 *  - src/board_detect/m5/generated/esp32s3_board_data.hpp:310-326 pins and
 *    :1025-1053 specs::sticks3: SPI3_HOST, 40 MHz write, panel 135x240,
 *    offset_x 52, offset_y 40, rotation_offset 0, invert 1; backlight not
 *    inverted.
 *  - src/board_detect/m5/esp32s3_construct.inl:353-385 (Panel_ST7789 with
 *    the specs above, DMA channel auto).
 *  - src/lgfx/v1/panel/Panel_ST7789.hpp:31-33 (memory 240x320) and :52-83
 *    the init command list sent below.
 *  - src/lgfx/v1/panel/Panel_Device.inl:64-89 reset: RST low 8 ms, high,
 *    64 ms.
 *  - src/lgfx/v1/panel/Panel_LCD.inl:128-158 setRotation(1): offsets and
 *    sizes swap (ox 40, oy 52, 240x135 of memory 320x240), colstart = ox =
 *    40, rowstart = mh - (ph + oy) = 240 - (135 + 52) = 53; :160-172
 *    MADCTL = madctl_table[1] | MAD_BGR (rgb_order defaults to false,
 *    Panel_Device.hpp:100), Panel_LCD.hpp:85-97 madctl_table[1] =
 *    MV|MX|MH = 0x64, so 0x6C; COLMOD 0x55 (RGB565); :95-102 setInvert:
 *    INVON since invert = 1.
 */
#include "platform_esp.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define PIN_MOSI 39
#define PIN_SCLK 40
#define PIN_DC 45
#define PIN_CS 41
#define PIN_RST 21
#define PIN_BL 38

#define LCD_HOST SPI3_HOST
#define LCD_HZ 40000000
#define COL_START 40
#define ROW_START 53

#define MADCTL_LANDSCAPE 0x6C

static spi_device_handle_t dev;

static void delay_ms(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }

/* Blocking send of one command and its parameters (DMA for long data). */
static int send(int dc, const void *data, size_t n) {
    if (n == 0) return 0;
    gpio_set_level(PIN_DC, dc);
    spi_transaction_t t = {.length = n * 8};
    if (n <= 4) { /* inline: no DMA bounce buffer for commands/params */
        t.flags = SPI_TRANS_USE_TXDATA;
        memcpy(t.tx_data, data, n);
    } else {
        t.tx_buffer = data;
    }
    return spi_device_transmit(dev, &t) == ESP_OK ? 0 : -1;
}

static int cmd(uint8_t c, const uint8_t *p, size_t n) {
    if (send(0, &c, 1) != 0) return -1;
    return send(1, p, n);
}

/* Panel_ST7789.hpp:52-83: {cmd, n, params...}, delay flag 0x80 on n. */
static const uint8_t init_cmds[] = {
    0xB7, 1, 0x35,                                    /* GCTRL */
    0xBB, 1, 0x28,                                    /* VCOMS */
    0xC0, 1, 0x0C,                                    /* LCMCTRL */
    0xC2, 2, 0x01, 0xFF,                              /* VDVVRHEN */
    0xC3, 1, 0x10,                                    /* VRHS */
    0xC4, 1, 0x20,                                    /* VDVSET */
    0xD0, 2, 0xA4, 0xA1,                              /* PWCTRL1 */
    0xB0, 2, 0x00, 0xC0,                              /* RAMCTRL: MSB-first pixels */
    0xE0, 14, 0xD0, 0x00, 0x02, 0x07, 0x0A, 0x28, 0x32, 0x44,
              0x42, 0x06, 0x0E, 0x12, 0x14, 0x17,     /* PVGAMCTRL */
    0xE1, 14, 0xD0, 0x00, 0x02, 0x07, 0x0A, 0x28, 0x31, 0x54,
              0x47, 0x0E, 0x1C, 0x17, 0x1B, 0x1E,     /* NVGAMCTRL */
    0x11, 0x80, 130,                                  /* SLPOUT, 130 ms */
    0x38, 0,                                          /* IDMOFF */
    0x3A, 1, 0x55,                                    /* COLMOD RGB565 */
    0x36, 1, MADCTL_LANDSCAPE,                        /* MADCTL */
    0x21, 0,                                          /* INVON */
    0x29, 0,                                          /* DISPON */
};

int lcd_init(void) {
    gpio_config_t out = {
        .pin_bit_mask = (1ull << PIN_DC) | (1ull << PIN_RST) | (1ull << PIN_BL),
        .mode = GPIO_MODE_OUTPUT,
    };
    if (gpio_config(&out) != ESP_OK) return -1;
    gpio_set_level(PIN_BL, 0);
    gpio_set_level(PIN_DC, 1);

    spi_bus_config_t bus = {
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .data4_io_num = -1,
        .data5_io_num = -1,
        .data6_io_num = -1,
        .data7_io_num = -1,
        .max_transfer_sz = LCD_W * LCD_BAND_H * 2,
    };
    if (spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) return -1;
    spi_device_interface_config_t dc = {
        .mode = 0,
        .clock_speed_hz = LCD_HZ,
        .spics_io_num = PIN_CS,
        .queue_size = 1,
        .flags = SPI_DEVICE_NO_DUMMY,
    };
    if (spi_bus_add_device(LCD_HOST, &dc, &dev) != ESP_OK) return -1;

    gpio_set_level(PIN_RST, 0);
    delay_ms(8);
    gpio_set_level(PIN_RST, 1);
    delay_ms(64);

    for (size_t i = 0; i < sizeof init_cmds;) {
        uint8_t c = init_cmds[i], n = init_cmds[i + 1];
        if (n & 0x80) {
            if (cmd(c, NULL, 0) != 0) return -1;
            delay_ms(init_cmds[i + 2]);
            i += 3;
        } else {
            if (cmd(c, &init_cmds[i + 2], n) != 0) return -1;
            i += 2u + n;
        }
    }
    return 0;
}

void lcd_backlight(int on) { gpio_set_level(PIN_BL, on ? 1 : 0); }

/* Writes h rows of 240 RGB565 big-endian pixels at landscape row y. */
int lcd_write_rows(int y, int h, const uint16_t *px) {
    uint16_t x0 = COL_START, x1 = COL_START + LCD_W - 1;
    uint16_t y0 = (uint16_t)(ROW_START + y), y1 = (uint16_t)(ROW_START + y + h - 1);
    uint8_t ca[4] = {(uint8_t)(x0 >> 8), (uint8_t)x0, (uint8_t)(x1 >> 8), (uint8_t)x1};
    uint8_t ra[4] = {(uint8_t)(y0 >> 8), (uint8_t)y0, (uint8_t)(y1 >> 8), (uint8_t)y1};
    if (cmd(0x2A, ca, 4) != 0 || cmd(0x2B, ra, 4) != 0) return -1; /* CASET, RASET */
    return cmd(0x2C, (const uint8_t *)px, (size_t)LCD_W * (size_t)h * 2u); /* RAMWR */
}
