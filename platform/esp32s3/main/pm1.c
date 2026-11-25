/* M5PM1 power-management chip (I2C SDA G47, SCL G48, address 0x6E): turns
 * on the rail that powers the LCD, and reports the battery (input_esp.c).
 * The bus handle comes from i2c_bus.c.
 *
 * Reference (read only, not vendored; MIT): M5GFX 28d4794 (2026-11-05) and
 * M5Unified 96576a1 (2026-11-06), github.com/m5stack:
 *  - M5GFX src/board_detect/m5/pmic_ops.hpp:87-93 `sticks3_power_on`:
 *      write 0x09 = 0x00; 0x16 &= ~0x04; 0x10 |= 0x04; 0x13 &= ~0x04;
 *      0x11 |= 0x04  (PM1 GPIO2 as a push-pull output driven high: the
 *      L3B / LCD rail enable).
 *  - M5GFX src/board_detect/m5/esp32s3.inl:202-221 `desc_sticks3`: PM1 at
 *      100 kHz on I2C port 1, polled for up to 200 ms (the first transfer
 *      after an idle period can be NACKed), rail state read back from 0x11
 *      bit 2, a 200 ms settle (src/board_detect/prepare.inl:231-253), then
 *      the panel reset on G21.
 *  - M5Unified src/utility/power/M5PM1_Class.inl:17-31 register names
 *      (0x00 DEVICE_ID, 0x09 I2C_CFG, 0x0A WDT_CNT, 0x10 GPIO_MODE,
 *      0x11 GPIO_OUT, 0x13 GPIO_DRV, 0x16 GPIO_FUNC0) and :69-95 begin():
 *      I2C_CFG = 0 (no I2C idle sleep), WDT_CNT = 0 (watchdog off).
 *  - M5GFX src/board_detect/m5/generated/esp32s3_board_data.hpp:310-326
 *      (pins) and :1025-1061 (specs::sticks3).
 * The StickS3 has no ESP-side power-hold GPIO: the PM1 keeps the board on.
 */
#include "platform_esp.h"

#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define PM1_ADDR 0x6E
#define PM1_HZ 100000u
#define PM1_TIMEOUT_MS 20

#define REG_DEVICE_ID 0x00
#define REG_I2C_CFG 0x09
#define REG_WDT_CNT 0x0A
#define REG_GPIO_MODE 0x10
#define REG_GPIO_OUT 0x11
#define REG_GPIO_DRV 0x13
#define REG_GPIO_FUNC0 0x16
#define LCD_RAIL_BIT 0x04 /* PM1 GPIO2 */

static i2c_master_dev_handle_t pm1;

/* The PM1 device on the shared bus, added on first use (the console's
 * bring-up, or the first battery read when the console is off). */
static int pm1_attach(void) {
    if (pm1) return 0;
    i2c_master_bus_handle_t bus = i2c_bus_get();
    if (!bus) return -1;
    i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PM1_ADDR,
        .scl_speed_hz = PM1_HZ,
    };
    return i2c_master_bus_add_device(bus, &dc, &pm1) == ESP_OK ? 0 : (pm1 = NULL, -1);
}

static int rd(uint8_t reg, uint8_t *v) {
    return pm1_read(reg, v, 1);
}

int pm1_read(uint8_t reg, uint8_t *buf, size_t n) {
    if (pm1_attach() != 0) return -1;
    return i2c_master_transmit_receive(pm1, &reg, 1, buf, n, PM1_TIMEOUT_MS) == ESP_OK ? 0 : -1;
}

int pm1_read_u8(uint8_t reg, uint8_t *v) { return pm1_read(reg, v, 1); }

int pm1_read_u16le(uint8_t reg, uint16_t *v) {
    uint8_t b[2];
    if (pm1_read(reg, b, 2) != 0) return -1;
    *v = (uint16_t)(b[0] | b[1] << 8);
    return 0;
}

int pm1_write_u8(uint8_t reg, uint8_t v) {
    if (pm1_attach() != 0) return -1;
    uint8_t b[2] = {reg, v};
    return i2c_master_transmit(pm1, b, 2, PM1_TIMEOUT_MS) == ESP_OK ? 0 : -1;
}

static int wr(uint8_t reg, uint8_t v) { return pm1_write_u8(reg, v); }

static int set_bits(uint8_t reg, uint8_t mask, int on) {
    uint8_t v;
    if (rd(reg, &v) != 0) return -1;
    v = on ? (uint8_t)(v | mask) : (uint8_t)(v & ~mask);
    return wr(reg, v);
}

int pm1_lcd_power_on(void) {
    if (pm1_attach() != 0) return -1;

    /* Poll the chip for up to ~200 ms. */
    uint8_t id;
    int ok = -1;
    for (int i = 0; i < 20 && (ok = rd(REG_DEVICE_ID, &id)) != 0; i++) vTaskDelay(pdMS_TO_TICKS(10));
    if (ok != 0) return -1;

    if (wr(REG_I2C_CFG, 0x00) != 0) return -1;
    if (wr(REG_WDT_CNT, 0x00) != 0) return -1;
    if (set_bits(REG_GPIO_FUNC0, LCD_RAIL_BIT, 0) != 0) return -1;
    if (set_bits(REG_GPIO_MODE, LCD_RAIL_BIT, 1) != 0) return -1;
    if (set_bits(REG_GPIO_DRV, LCD_RAIL_BIT, 0) != 0) return -1;
    if (set_bits(REG_GPIO_OUT, LCD_RAIL_BIT, 1) != 0) return -1;
    /* Let the rail settle: M5GFX waits 200 ms after the power-on ops,
     * warm or cold (prepare.inl:231-253 with desc_sticks3's 200/200). */
    vTaskDelay(pdMS_TO_TICKS(200));
    return 0;
}
