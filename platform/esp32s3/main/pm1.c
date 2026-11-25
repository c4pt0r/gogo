/* M5PM1 power-management chip (I2C SDA G47, SCL G48, address 0x6E): turns
 * on the rail that powers the LCD, reports the battery (input_esp.c), and
 * switches EXT_5V and the speaker amplifier for the IR ports and the IMU
 * rail (ir_esp.c, imu_esp.c), and the codec rail and the amplifier for
 * audio (audio_esp.c). The bus handle comes from i2c_bus.c.
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

#include <stdbool.h>
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define PM1_ADDR 0x6E
#define PM1_HZ 100000u
#define PM1_TIMEOUT_MS 20

#define REG_DEVICE_ID 0x00
#define REG_PWR_CFG 0x06
#define REG_I2C_CFG 0x09
#define REG_WDT_CNT 0x0A
#define REG_GPIO_MODE 0x10
#define REG_GPIO_OUT 0x11
#define REG_GPIO_DRV 0x13
#define REG_GPIO_FUNC0 0x16
#define LCD_RAIL_BIT 0x04 /* PM1 GPIO2 */
#define SPK_AMP_BIT 0x08  /* PM1 GPIO3: the speaker amplifier (AW8737) enable */
#define PWR_CFG_LDO_EN 0x04   /* rail L1 (LDO3V3): the BMI270 */
#define PWR_CFG_BOOST_EN 0x08 /* EXT_5V: Grove/Hat 5 V out, IR LED and receiver */

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

/* Wakes the chip and turns its I2C idle sleep off (I2C_CFG = 0): after a
 * pause the first transfer can be NACKed, so the chip is polled for up to
 * ~200 ms. Done once; the console bring-up, the battery reader and the IR
 * and IMU hooks all start with it, so it holds whether or not the console
 * is built in. */
static bool awake;   /* assumes the PM1 keeps I2C_CFG for the whole boot (it resets only with a power loss) */

int pm1_wake(void) {
    if (awake) return 0;
    if (pm1_attach() != 0) return -1;
    uint8_t id;
    int ok = -1;
    for (int i = 0; i < 20 && (ok = rd(REG_DEVICE_ID, &id)) != 0; i++) vTaskDelay(pdMS_TO_TICKS(10));
    if (ok != 0 || wr(REG_I2C_CFG, 0x00) != 0) return -1;
    awake = true;
    return 0;
}

int pm1_lcd_power_on(void) {
    if (pm1_wake() != 0) return -1;
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

/* EXT_5V (PWR_CFG 0x06 bit 3, M5Unified M5PM1_Class.inl:97-100
 * setExtOutput): powers the IR LED and receiver, and makes the Grove/Hat 5 V
 * pins outputs (nothing may feed 5 V into them while it is on). *was (may
 * be NULL) gets the state before. Turning it on waits 10 ms for the boost
 * converter. */
int pm1_ext5v(bool on, bool *was) {
    uint8_t v;
    if (pm1_wake() != 0 || rd(REG_PWR_CFG, &v) != 0) return -1;
    bool cur = (v & PWR_CFG_BOOST_EN) != 0;
    if (was) *was = cur;
    if (cur == on) return 0;
    if (wr(REG_PWR_CFG, on ? (uint8_t)(v | PWR_CFG_BOOST_EN) : (uint8_t)(v & ~PWR_CFG_BOOST_EN)) != 0) return -1;
    if (on) vTaskDelay(pdMS_TO_TICKS(10));
    return 0;
}

/* Rail L1 (PWR_CFG bit 2, LDO3V3): the BMI270's supply. The PM1 turns it on
 * at power-up; this only makes sure. */
int pm1_imu_power_on(void) {
    uint8_t v;
    if (pm1_wake() != 0 || rd(REG_PWR_CFG, &v) != 0) return -1;
    if (v & PWR_CFG_LDO_EN) return 0;
    if (wr(REG_PWR_CFG, (uint8_t)(v | PWR_CFG_LDO_EN)) != 0) return -1;
    vTaskDelay(pdMS_TO_TICKS(10));
    return 0;
}

/* The speaker amplifier enable, PM1 GPIO3 (M5Unified.inl:1975-1983 and
 * :662-695): GPIO function, push-pull output, high = on. The level is
 * written before the pin becomes an output, so a stale OUT bit never
 * glitches the amplifier on. */
static int spk_amp_set(bool on) {
    if (pm1_wake() != 0) return -1;
    if (set_bits(REG_GPIO_OUT, SPK_AMP_BIT, on) != 0 || set_bits(REG_GPIO_FUNC0, SPK_AMP_BIT, 0) != 0 ||
        set_bits(REG_GPIO_DRV, SPK_AMP_BIT, 0) != 0)
        return -1;
    return set_bits(REG_GPIO_MODE, SPK_AMP_BIT, 1);
}

/* Two users share the amplifier: the speaker wants it on while spk is
 * open (audio_esp.c), and the IR receiver needs it off while ir_rx is open
 * (docs.m5stack.com StickS3, "IR reception notes": the receiver does not
 * work while the amplifier is on; ir_esp.c). The pin is driven to
 * "wanted and not blocked". Both run on the scheduler thread. */
static bool amp_want, amp_block;

int pm1_spk_amp(bool on) {
    amp_want = on;
    return spk_amp_set(on && !amp_block);
}

int pm1_spk_amp_block(bool block) {
    amp_block = block;
    return spk_amp_set(amp_want && !block);
}

bool pm1_spk_amp_blocked(void) { return amp_block; }

/* The ES8311 codec's supply: rail L3B, switched by PM1 GPIO2 like the LCD
 * (3P Kmal/sticks3-local-automation board_audio_power.c; M5Unified never
 * switches it, because M5GFX's sticks3_power_on already did). On with the
 * console; without it this turns the rail on and waits 10 ms for it. */
int pm1_audio_power_on(void) {
    uint8_t out, mode;
    if (pm1_wake() != 0 || rd(REG_GPIO_OUT, &out) != 0 || rd(REG_GPIO_MODE, &mode) != 0) return -1;
    if ((out & LCD_RAIL_BIT) && (mode & LCD_RAIL_BIT)) return 0;
    if (set_bits(REG_GPIO_OUT, LCD_RAIL_BIT, 1) != 0 || set_bits(REG_GPIO_FUNC0, LCD_RAIL_BIT, 0) != 0 ||
        set_bits(REG_GPIO_DRV, LCD_RAIL_BIT, 0) != 0 || set_bits(REG_GPIO_MODE, LCD_RAIL_BIT, 1) != 0)
        return -1;
    vTaskDelay(pdMS_TO_TICKS(10));
    return 0;
}
