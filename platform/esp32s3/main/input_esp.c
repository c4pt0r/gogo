/* ESP32-S3 input hardware (spec input §5.6, input2 §4): the InputHw the
 * chip-independent input/ code drives. The IMU hooks are in imu_esp.c
 * (BMI270), the IR hooks in ir_esp.c (RMT).
 *
 *  - Cardputer ADV keyboard: TCA8418 key-scan chip on the shared I2C bus
 *    (0x34, 400 kHz), INT on G11 (open drain, active low). The ISR only sets
 *    a flag and notifies the scheduler; every I2C transfer is on the
 *    scheduler thread. Battery: ADC1 on G10 (divider, x2), curve-fitting
 *    calibration; no charge or external-power signal (-1).
 *  - StickS3: no keyboard. Battery from the M5PM1: VBAT mV (0x22/0x23 LE),
 *    external power = PWR_SRC (0x04) bit0, charging = PM1 GPIO0 level
 *    (GPIO_IN 0x12 bit0, low = charging).
 * Sources and registers: .superpowers/sdd/2026-11-24-gogo-input/hw-research.md
 * (M5Cardputer bf64d31, M5Unified, M5PM1 datasheet). */
#include "platform_esp.h"
#include "board.h"
#include "input.h"
#include "platform.h"

#include "sdkconfig.h"

/* GOGO_KBD / GOGO_BATTERY (main/Kconfig.projbuild): disabled = absent
 * hardware (the port's open fails) and none of its driver code is linked. */
#if HAS_KBD && CONFIG_GOGO_KBD
#define WITH_KBD 1
#else
#define WITH_KBD 0
#endif
#if CONFIG_GOGO_BATTERY
#define WITH_BATTERY 1
#else
#define WITH_BATTERY 0
#endif
/* GOGO_IMU / GOGO_IR: the hooks live in imu_esp.c and ir_esp.c, which are
 * built only with their switch (CMakeLists.txt). */
#if HAS_IMU && CONFIG_GOGO_IMU
#define WITH_IMU 1
#else
#define WITH_IMU 0
#endif
#if HAS_IR_TX && CONFIG_GOGO_IR
#define WITH_IR_TX 1
#else
#define WITH_IR_TX 0
#endif
#if HAS_IR_RX && CONFIG_GOGO_IR
#define WITH_IR_RX 1
#else
#define WITH_IR_RX 0
#endif

#if WITH_KBD
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#endif
#if WITH_BATTERY && defined(GOGO_BOARD_CARDPUTER)
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#endif

#if WITH_KBD

#define TCA_ADDR 0x34
#define TCA_HZ 400000u
#define TCA_TIMEOUT_MS 20
#define TCA_CFG 0x01
#define TCA_INT_STAT 0x02
#define TCA_INT_CLR 0x1F   /* every INT_STAT source: a latched GPI/OVR/LCK bit would hold INT low for good */
#define TCA_KEY_EVENT_A 0x04
#define TCA_KP_GPIO1 0x1D
#define TCA_KP_GPIO2 0x1E
#define TCA_KP_GPIO3 0x1F

static i2c_master_dev_handle_t tca;
static volatile bool kbd_flag;   /* set by the ISR; cleared by kbd_read on empty */
static bool isr_installed, isr_added;

/* Cardputer ADV key matrix, indexed by raw = n - 1 (n is the TCA8418 key
 * number 1..80). `code` is the USB HID usage, `fn_code` the usage under Fn
 * (0 = Fn does not change it). Fn itself is 0xF0 (KEY_FN). */
#define K(n, c, f) [(n) - 1] = {c, f}
static const KbdKey keys[70] = {
    K(1, 0x35, 0x29), K(2, 0x2B, 0), K(3, 0xF0, 0), K(4, 0xE0, 0), K(5, 0x1E, 0x3A), K(6, 0x14, 0),
    K(7, 0xE1, 0), K(8, 0xE3, 0),
    K(11, 0x1F, 0x3B), K(12, 0x1A, 0), K(13, 0x04, 0), K(14, 0xE2, 0), K(15, 0x20, 0x3C), K(16, 0x08, 0),
    K(17, 0x16, 0), K(18, 0x1D, 0),
    K(21, 0x21, 0x3D), K(22, 0x15, 0), K(23, 0x07, 0), K(24, 0x1B, 0), K(25, 0x22, 0x3E), K(26, 0x17, 0),
    K(27, 0x09, 0), K(28, 0x06, 0),
    K(31, 0x23, 0x3F), K(32, 0x1C, 0), K(33, 0x0A, 0), K(34, 0x19, 0), K(35, 0x24, 0x40), K(36, 0x18, 0),
    K(37, 0x0B, 0), K(38, 0x05, 0),
    K(41, 0x25, 0x41), K(42, 0x0C, 0), K(43, 0x0D, 0), K(44, 0x11, 0), K(45, 0x26, 0x42), K(46, 0x12, 0),
    K(47, 0x0E, 0), K(48, 0x10, 0),
    K(51, 0x27, 0x43), K(52, 0x13, 0), K(53, 0x0F, 0), K(54, 0x36, 0x50), K(55, 0x2D, 0x44), K(56, 0x2F, 0),
    K(57, 0x33, 0x52), K(58, 0x37, 0x51),
    K(61, 0x2E, 0x45), K(62, 0x30, 0), K(63, 0x34, 0), K(64, 0x38, 0x4F), K(65, 0x2A, 0x4C), K(66, 0x31, 0),
    K(67, 0x28, 0), K(68, 0x2C, 0),
};

static int tca_rd(uint8_t reg, uint8_t *v) {
    return i2c_master_transmit_receive(tca, &reg, 1, v, 1, TCA_TIMEOUT_MS) == ESP_OK ? 0 : -1;
}

static int tca_wr(uint8_t reg, uint8_t v) {
    uint8_t b[2] = {reg, v};
    return i2c_master_transmit(tca, b, 2, TCA_TIMEOUT_MS) == ESP_OK ? 0 : -1;
}

static void kbd_isr(void *arg) {
    (void)arg;
    kbd_flag = true;
    platform_notify();
}

static int kbd_open(void) {
    if (!tca) {
        i2c_master_bus_handle_t bus = i2c_bus_get();
        if (!bus) return -1;
        i2c_device_config_t dc = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = TCA_ADDR,
            .scl_speed_hz = TCA_HZ,
        };
        if (i2c_master_bus_add_device(bus, &dc, &tca) != ESP_OK) { tca = NULL; return -1; }
    }
    /* Matrix: rows 0-6 (KP_GPIO1 0x7F), columns 0-7 (KP_GPIO2 0xFF), no
     * column 8-9 (KP_GPIO3 0); then key-event and GPI interrupts on. */
    uint8_t cfg;
    if (tca_wr(TCA_KP_GPIO1, 0x7F) != 0 || tca_wr(TCA_KP_GPIO2, 0xFF) != 0 || tca_wr(TCA_KP_GPIO3, 0x00) != 0 ||
        tca_rd(TCA_CFG, &cfg) != 0 || tca_wr(TCA_CFG, (uint8_t)(cfg | 0x03)) != 0)
        return -1;
    /* Flush: drop whatever is queued, then clear the chip's interrupt. */
    uint8_t ev;
    for (int i = 0; i < 20; i++) {                    /* the FIFO holds 10 */
        if (tca_rd(TCA_KEY_EVENT_A, &ev) != 0) return -1;
        if (ev == 0) break;
    }
    if (tca_wr(TCA_INT_STAT, TCA_INT_CLR) != 0) return -1;

    gpio_config_t c = {
        .pin_bit_mask = 1ULL << KBD_PIN_INT,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    if (gpio_config(&c) != ESP_OK) return -1;
    if (!isr_installed) {
        esp_err_t err = gpio_install_isr_service(0);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return -1;
        isr_installed = true;
    }
    if (!isr_added) {
        if (gpio_isr_handler_add(KBD_PIN_INT, kbd_isr, NULL) != ESP_OK) return -1;
        isr_added = true;
    }
    /* An event between the flush and the ISR going live raised no edge: look
     * once (kbd_read on an empty FIFO then clears the flag). */
    kbd_flag = true;
    gpio_intr_enable(KBD_PIN_INT);
    return 0;
}

static void kbd_close(void) {
    gpio_intr_disable(KBD_PIN_INT);
    if (isr_added) {
        gpio_isr_handler_remove(KBD_PIN_INT);
        isr_added = false;
    }
    kbd_flag = false;
}

static bool kbd_pending(void) { return kbd_flag; }

static int kbd_read(uint16_t *raw, bool *down) {
    uint8_t ev;
    if (tca_rd(TCA_KEY_EVENT_A, &ev) != 0) return -1;   /* flag stays set: retried next pass */
    if (ev != 0) {
        *down = (ev & 0x80) != 0;
        *raw = (uint16_t)((ev & 0x7F) - 1);
        return 1;
    }
    /* FIFO empty: clear the chip's interrupt, then the flag. If INT is still
     * low, an event arrived before the clear (the chip keeps INT asserted
     * while its FIFO is not empty, so no edge will announce it): keep the
     * flag. The pin cannot fail to read, unlike an I2C re-check. */
    if (tca_wr(TCA_INT_STAT, TCA_INT_CLR) != 0) return -1;
    kbd_flag = gpio_get_level(KBD_PIN_INT) == 0;
    return 0;
}

#endif /* WITH_KBD */

#if WITH_BATTERY && defined(GOGO_BOARD_CARDPUTER)

static adc_oneshot_unit_handle_t adc;
static adc_cali_handle_t cali;
static adc_channel_t adc_chan;
static bool adc_tried;

static int adc_setup(void) {
    if (adc) return 0;
    if (adc_tried) return -1;           /* a failed setup is not retried */
    adc_tried = true;
    adc_unit_t unit;
    if (adc_oneshot_io_to_channel(BAT_PIN_ADC, &unit, &adc_chan) != ESP_OK || unit != ADC_UNIT_1) return -1;
    adc_oneshot_unit_init_cfg_t uc = {.unit_id = ADC_UNIT_1};
    adc_oneshot_unit_handle_t h;
    if (adc_oneshot_new_unit(&uc, &h) != ESP_OK) return -1;
    adc_oneshot_chan_cfg_t cc = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
    adc_cali_curve_fitting_config_t kc = {
        .unit_id = ADC_UNIT_1, .chan = adc_chan, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_oneshot_config_channel(h, adc_chan, &cc) != ESP_OK) {
        adc_oneshot_del_unit(h);
        return -1;
    }
    /* No eFuse calibration: fall back to the nominal 12 dB range (cali stays
     * NULL), which is only roughly right but keeps the port delivering. */
    if (adc_cali_create_scheme_curve_fitting(&kc, &cali) != ESP_OK) {
        cali = NULL;
        platform_esp_log("gogo: battery ADC uncalibrated");
    }
    adc = h;
    return 0;
}

/* A cell outside this range is a bad sample, not a reading: power_read fails,
 * so input/ keeps it out of its average (a failed read is no sample). Seen
 * on a StickS3: the PM1 occasionally returns ~0 for VBAT. */
#define BAT_MV_MIN 2500
#define BAT_MV_MAX 4500

/* The battery sits behind a 1:2 divider on G10. */
static int power_read(int32_t *mv, int8_t *charging, int8_t *external) {
    if (adc_setup() != 0) return -1;
    int sum = 0;
    for (int i = 0; i < 8; i++) {
        int raw;
        if (adc_oneshot_read(adc, adc_chan, &raw) != ESP_OK) return -1;
        sum += raw;
    }
    int pin_mv;
    if (!cali) pin_mv = sum / 8 * 3100 / 4095;   /* ~3.1 V full scale at 12 dB */
    else if (adc_cali_raw_to_voltage(cali, sum / 8, &pin_mv) != ESP_OK) return -1;
    if (pin_mv * 2 < BAT_MV_MIN || pin_mv * 2 > BAT_MV_MAX) return -1;
    *mv = pin_mv * 2;
    *charging = -1;
    *external = -1;
    return 0;
}

#elif WITH_BATTERY && defined(GOGO_BOARD_STICKS3)

#define PM1_REG_PWR_SRC 0x04
#define PM1_REG_GPIO_MODE 0x10
#define PM1_REG_GPIO_IN 0x12
#define PM1_REG_GPIO_FUNC0 0x16
#define PM1_REG_VBAT 0x22
#define BAT_MV_MIN 2500   /* outside this: a bad sample, power_read fails (see below) */
#define BAT_MV_MAX 4500
static bool gpio0_ready;

static int power_read(int32_t *mv, int8_t *charging, int8_t *external) {
    uint8_t v;
    if (!gpio0_ready) {
        /* No I2C idle sleep, or the chip NACKs after a pause (pm1_wake; the
         * console bring-up does it too, but not when built with GOGO_CONSOLE=0). */
        if (pm1_wake() != 0) return -1;
        /* PM1 GPIO0 is the charger's status line: function GPIO (0x16
         * bits[1:0] = 00, as M5Unified) and an input. */
        if (pm1_read_u8(PM1_REG_GPIO_FUNC0, &v) != 0) return -1;
        if ((v & 3) && pm1_write_u8(PM1_REG_GPIO_FUNC0, (uint8_t)(v & ~3)) != 0) return -1;
        if (pm1_read_u8(PM1_REG_GPIO_MODE, &v) != 0) return -1;
        if ((v & 1) && pm1_write_u8(PM1_REG_GPIO_MODE, (uint8_t)(v & ~1)) != 0) return -1;
        gpio0_ready = true;
    }
    uint16_t bat;
    uint8_t src, in;
    if (pm1_read_u16le(PM1_REG_VBAT, &bat) != 0 || pm1_read_u8(PM1_REG_PWR_SRC, &src) != 0 ||
        pm1_read_u8(PM1_REG_GPIO_IN, &in) != 0 || bat < BAT_MV_MIN || bat > BAT_MV_MAX)
        return -1;
    *mv = bat;                          /* mV, 16 bits: no mask (a full cell is 4.2 V; M5Unified reads it whole) */
    *external = src & 1;
    *charging = (in & 1) ? 0 : 1;
    return 0;
}

#endif

const InputHw input_esp_hw = {
#if WITH_KBD
    .keys = keys,
    .nkeys = 70,
    .kbd_open = kbd_open,
    .kbd_close = kbd_close,
    .kbd_pending = kbd_pending,
    .kbd_read = kbd_read,
#endif
#if WITH_BATTERY
    .power_read = power_read,
#endif
#if WITH_IMU
    .imu_open = imu_esp_open,
    .imu_close = imu_esp_close,
    .imu_read_fifo = imu_esp_read_fifo,
    .imu_read_gestures = imu_esp_read_gestures,
#endif
#if WITH_IR_TX
    .ir_tx_start = ir_esp_tx_start,
    .ir_tx_busy = ir_esp_tx_busy,
#endif
#if WITH_IR_RX
    .ir_rx_open = ir_esp_rx_open,
    .ir_rx_read = ir_esp_rx_read,
    .ir_rx_close = ir_esp_rx_close,
#endif
#if WITH_IR_TX
    .ir_tx_open = ir_esp_tx_open,
    .ir_tx_close = ir_esp_tx_close,
#endif
};
