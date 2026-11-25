/* BMI270 motion sensor (spec input2 §2, §4): the InputHw imu_* hooks for
 * runtime/input/imu.c. Both boards have it on their shared I2C bus (i2c_bus.c):
 * StickS3 G47/G48 (rail L1 of the M5PM1), Cardputer ADV G8/G9. Address
 * 0x69 is tried first, then 0x68 (as M5Unified IMU_Class.inl:79-83; the
 * StickS3 docs say 0x68, the Cardputer ADV address is not documented).
 * Polled only: neither board routes an IMU interrupt to an ESP32 GPIO
 * (StickS3 INT1 goes to PM1 G4, the Cardputer's is not connected). Every
 * hook runs on the scheduler thread.
 *
 * Bring-up at the first open (Bosch BMI270_SensorAPI 41129fc, bmi2.c
 * bmi2_sec_init / write_config_file / upload_file; register addresses
 * bmi2_defs.h): CHIP_ID 0x24, soft reset (CMD 0xB6, 2 ms), PWR_CONF = 0
 * (advanced power save off, stays off so registers need no 450 us gaps),
 * INIT_CTRL = 0, the 8192-byte legacy configuration (bmi270_legacy_config.c)
 * in 64-byte chunks, each after its word address in INIT_ADDR_0/1,
 * INIT_CTRL = 1, then INTERNAL_STATUS & 0x0F == 1 within 150 ms. That takes
 * about 250 ms of the scheduler thread, once per boot; a failed open redoes it.
 *
 * Every open: sensors off, the legacy features in the gesture mask switched
 * on and the others off (feature pages below), ranges +-8 g / +-2000 dps,
 * ODR, FIFO headerless with gyro and accel (12-byte frames gx gy gz ax ay az,
 * int16 LE: the BMI270's own order, bmi2.c:6745-6752, so no reordering),
 * FIFO flushed, gesture status cleared; then the sensors on. The first
 * frames after that carry the accelerometer's dummy marker (it starts after
 * the gyro): imu_read_fifo drops every frame with a dummy half. When gestures
 * are asked for, the sensors run at 100 Hz at least (the feature engine is
 * specified at the reset ODR of 100 Hz) and the FIFO is downsampled to
 * RateHz (FIFO_DOWNS, 2^n, filtered data). RateHz 0 runs the accelerometer
 * alone, FIFO off. Close switches both sensors off.
 *
 * Legacy features (bmi270_legacy.c/.h): page, byte, enable bit:
 *   any-motion  1, 0x09 bit7 (axes x/y/z: byte 0x07 bits 5-7)
 *   no-motion   1, 0x0D bit7 (axes: byte 0x0B bits 5-7)
 *   orientation 2, 0x00 bit0     flat 3, 0x00 bit0
 *   step        4, 0x01 bits 3 (detector: a status bit per step) and 4 (counter)
 *   tap         5, 0x00 bit0 single, bit1 double, bit2 triple, bit3 data reg
 * Status: INT_STATUS_0 (0x1C, cleared on read; INT_LATCH = 1 so nothing is
 * lost between polls; features mapped to INT1, whose output stays off):
 * 0x02 step, 0x08 tap (kind in register 0x20: 0x20 single, 0x40 double,
 * 0x80 triple), 0x10 flat, 0x20 no-motion, 0x40 any-motion, 0x80
 * orientation. Outputs on page 0: step count bytes 0-3 (LE), orientation
 * byte 6 bits 0-2 (0 portrait upright, 1 landscape left, 2 portrait upside
 * down, 3 landscape right; +4 face down), reported as the Value as it is.
 * Thresholds and durations are the configuration's defaults.
 *
 * Axes are the chip's own; no board remap (M5Unified swaps X/Y on the
 * StickS3 for its screen). */
#include "platform_esp.h"
#include "board.h"

#include <string.h>
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern const uint8_t bmi270_legacy_config_file[8192];

#define IMU_HZ 400000u
#define IMU_TIMEOUT_MS 100   /* a FIFO read of FIFO_READ_MAX bytes takes about 22 ms at 400 kHz */
#define FIFO_READ_MAX 960u   /* 80 frames per read: a backlog drains at about 19 KB/s, input is at most 2.4 KB/s */
#define CHUNK 64

enum {
    R_CHIP_ID = 0x00, R_INT_STATUS_0 = 0x1C, R_TAP_STATUS = 0x20, R_INTERNAL_STATUS = 0x21,
    R_FIFO_LENGTH_0 = 0x24, R_FIFO_DATA = 0x26, R_FEAT_PAGE = 0x2F, R_FEATURES = 0x30,
    R_ACC_CONF = 0x40, R_ACC_RANGE = 0x41, R_GYR_CONF = 0x42, R_GYR_RANGE = 0x43, R_FIFO_DOWNS = 0x45,
    R_FIFO_CONFIG_0 = 0x48, R_FIFO_CONFIG_1 = 0x49, R_INT_LATCH = 0x55, R_INT1_MAP_FEAT = 0x56,
    R_INIT_CTRL = 0x59, R_INIT_ADDR_0 = 0x5B, R_INIT_DATA = 0x5E, R_PWR_CONF = 0x7C, R_PWR_CTRL = 0x7D,
    R_CMD = 0x7E,
};
enum { CHIP_ID = 0x24, CMD_SOFTRESET = 0xB6, CMD_FIFO_FLUSH = 0xB0, PWR_ACC = 0x04, PWR_GYR = 0x02 };
/* INT_STATUS_0 bits of the legacy configuration. */
enum { ST_STEP = 0x02, ST_TAP = 0x08, ST_FLAT = 0x10, ST_NO_MOT = 0x20, ST_ANY_MOT = 0x40, ST_ORIENT = 0x80 };
/* gogo's gesture bits (gogo/imu, spec input2 §2). */
enum { G_TAP = 1, G_DOUBLE = 2, G_TRIPLE = 4, G_ORIENT = IMU_ORIENTATION, G_FLAT = 16, G_ANY = 32, G_NO = 64,
       G_STEP = IMU_STEP };

static i2c_master_dev_handle_t dev;
static bool loaded;          /* the configuration is in the chip */
static bool fifo_on;

static int rd(uint8_t reg, uint8_t *buf, size_t n) {
    return i2c_master_transmit_receive(dev, &reg, 1, buf, n, IMU_TIMEOUT_MS) == ESP_OK ? 0 : -1;
}

static int wr(uint8_t reg, uint8_t v) {
    uint8_t b[2] = {reg, v};
    return i2c_master_transmit(dev, b, 2, IMU_TIMEOUT_MS) == ESP_OK ? 0 : -1;
}

static int wrn(uint8_t reg, const uint8_t *d, size_t n) {
    uint8_t b[1 + CHUNK];
    if (n > CHUNK) return -1;
    b[0] = reg;
    memcpy(b + 1, d, n);
    return i2c_master_transmit(dev, b, n + 1, IMU_TIMEOUT_MS) == ESP_OK ? 0 : -1;
}

static void delay_ms(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms) ? pdMS_TO_TICKS(ms) : 1); }

/* Adds the sensor at 0x69 or 0x68, whichever answers with CHIP_ID 0x24. */
static int attach(void) {
    if (dev) return 0;
    i2c_master_bus_handle_t bus = i2c_bus_get();
    if (!bus) return -1;
    static const uint8_t addrs[2] = {0x69, 0x68};
    for (int i = 0; i < 6; i++) {           /* both addresses, three rounds 10 ms apart */
        if (i == 2 || i == 4) delay_ms(10);
        if (i2c_master_probe(bus, addrs[i % 2], IMU_TIMEOUT_MS) != ESP_OK) continue;
        i2c_device_config_t dc = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = addrs[i % 2],
            .scl_speed_hz = IMU_HZ,
        };
        if (i2c_master_bus_add_device(bus, &dc, &dev) != ESP_OK) { dev = NULL; return -1; }
        uint8_t id;
        if (rd(R_CHIP_ID, &id, 1) == 0 && id == CHIP_ID) return 0;
        i2c_master_bus_rm_device(dev);
        dev = NULL;
    }
    platform_esp_log("gogo: imu: no BMI270 at 0x69 or 0x68");
    return -1;
}

static int load_config(void) {
    if (wr(R_CMD, CMD_SOFTRESET) != 0) return -1;
    delay_ms(2);
    if (wr(R_PWR_CONF, 0x00) != 0) return -1;
    delay_ms(1);
    if (wr(R_INIT_CTRL, 0x00) != 0) return -1;
    for (uint32_t i = 0; i < sizeof bmi270_legacy_config_file; i += CHUNK) {
        uint8_t a[2] = {(uint8_t)((i / 2) & 0x0F), (uint8_t)((i / 2) >> 4)};   /* word address */
        if (wrn(R_INIT_ADDR_0, a, 2) != 0 || wrn(R_INIT_DATA, bmi270_legacy_config_file + i, CHUNK) != 0)
            return -1;
    }
    if (wr(R_INIT_CTRL, 0x01) != 0) return -1;
    delay_ms(20);
    uint8_t st = 0;
    for (int i = 0; i < 13; i++) {
        if (rd(R_INTERNAL_STATUS, &st, 1) == 0 && (st & 0x0F) == 1) return 0;
        delay_ms(10);
    }
    platform_esp_log("gogo: imu: configuration not accepted (INTERNAL_STATUS 0x%02x)", st);
    return -1;
}

/* Read-modify-write of the 16 feature bytes of one page: byte `idx` gets
 * `bits` set (on) or cleared. */
static int feat_set(uint8_t page, uint8_t idx, uint8_t bits, bool on) {
    uint8_t f[16];
    if (wr(R_FEAT_PAGE, page) != 0 || rd(R_FEATURES, f, 16) != 0) return -1;
    uint8_t v = on ? (uint8_t)(f[idx] | bits) : (uint8_t)(f[idx] & ~bits);
    if (v == f[idx]) return 0;
    f[idx] = v;
    return wrn(R_FEATURES, f, 16);
}

static int features(uint8_t g) {
    uint8_t tap = (uint8_t)(g & (G_TAP | G_DOUBLE | G_TRIPLE));
    if (feat_set(1, 0x07, 0xE0, true) != 0 || feat_set(1, 0x09, 0x80, g & G_ANY) != 0 ||
        feat_set(1, 0x0B, 0xE0, true) != 0 || feat_set(1, 0x0D, 0x80, g & G_NO) != 0 ||
        feat_set(2, 0x00, 0x01, g & G_ORIENT) != 0 || feat_set(3, 0x00, 0x01, g & G_FLAT) != 0 ||
        feat_set(4, 0x01, 0x18, g & G_STEP) != 0 ||
        feat_set(5, 0x00, 0x0F, false) != 0 || (tap && feat_set(5, 0x00, (uint8_t)(tap | 0x08), true) != 0))
        return -1;
    /* The features' status bits, latched until INT_STATUS_0 is read. */
    uint8_t map = (uint8_t)((g & (G_TAP | G_DOUBLE | G_TRIPLE) ? ST_TAP : 0) | (g & G_ORIENT ? ST_ORIENT : 0) |
                            (g & G_FLAT ? ST_FLAT : 0) | (g & G_ANY ? ST_ANY_MOT : 0) |
                            (g & G_NO ? ST_NO_MOT : 0) | (g & G_STEP ? ST_STEP : 0));
    return wr(R_INT_LATCH, 0x01) != 0 || wr(R_INT1_MAP_FEAT, map) != 0 ? -1 : 0;
}

static uint8_t odr_code(uint16_t hz) { return hz <= 25 ? 0x06 : hz <= 50 ? 0x07 : hz <= 100 ? 0x08 : 0x09; }

int imu_esp_open(uint16_t rate_hz, uint8_t gestures) {
#if HAS_PM1
    if (pm1_imu_power_on() != 0) return -1;
#endif
    if (attach() != 0) return -1;
    if (!loaded) {
        if (load_config() != 0) return -1;
        loaded = true;
    }
    /* The sensor ODR: RateHz, at least 100 Hz with gestures; the FIFO keeps
     * every 2^downs-th (filtered) sample so it delivers RateHz. */
    uint16_t odr = rate_hz;
    if (gestures && odr < 100) odr = 100;   /* at RateHz 200 the engine runs at 200 Hz: thresholds may act differently */
    uint8_t downs = 0;
    while (rate_hz && (uint16_t)(rate_hz << downs) < odr) downs++;
    uint8_t code = odr_code(odr);
    /* The temperature sensor (PWR_CTRL 0x08) stays off: nothing reads it. */
    uint8_t pwr = (uint8_t)(rate_hz ? PWR_ACC | PWR_GYR : gestures ? PWR_ACC : 0);   /* nothing asked: stay off */
    if (wr(R_PWR_CTRL, 0x00) != 0 || wr(R_FIFO_CONFIG_1, 0x00) != 0 || features(gestures) != 0 ||
        wr(R_ACC_CONF, (uint8_t)(0xA0 | code)) != 0 ||      /* filter perf, normal avg4 */
        wr(R_ACC_RANGE, 0x02) != 0 ||                       /* +-8 g */
        wr(R_GYR_CONF, (uint8_t)(0xA0 | code)) != 0 ||      /* filter perf, normal */
        wr(R_GYR_RANGE, 0x00) != 0 ||                       /* +-2000 dps */
        wr(R_FIFO_DOWNS, (uint8_t)(0x88 | downs << 4 | downs)) != 0 ||
        wr(R_FIFO_CONFIG_0, 0x00) != 0 ||                   /* no stop-on-full, no sensortime */
        wr(R_FIFO_CONFIG_1, rate_hz ? 0xC0 : 0x00) != 0 ||  /* gyr + acc, headerless */
        wr(R_CMD, CMD_FIFO_FLUSH) != 0 || wr(R_PWR_CTRL, pwr) != 0)
        goto fail;
    uint8_t st;
    if (rd(R_INT_STATUS_0, &st, 1) != 0) goto fail;     /* clears what an earlier open left */
    fifo_on = rate_hz != 0;
    return 0;
fail:
    loaded = false;                                     /* start from a reset next time */
    return -1;
}

void imu_esp_close(void) {
    fifo_on = false;
    if (wr(R_PWR_CTRL, 0x00) != 0 || wr(R_FIFO_CONFIG_1, 0x00) != 0) loaded = false;
}

static bool dummy(const uint8_t *h) {
    return (h[0] == 0x01 || h[0] == 0x02) && h[1] == 0x7F && h[2] == 0x00 && h[3] == 0x80;
}

int imu_esp_read_fifo(uint8_t *buf, uint32_t cap) {
    if (!fifo_on) return 0;
    uint8_t l[2];
    if (rd(R_FIFO_LENGTH_0, l, 2) != 0) return -1;
    uint32_t n = (uint32_t)l[0] | (uint32_t)(l[1] & 0x3F) << 8;
    if (n > cap) n = cap;
    if (n > FIFO_READ_MAX) n = FIFO_READ_MAX;
    n -= n % 12;                                        /* whole frames; the rest stays for the next read */
    if (n == 0) return 0;
    if (rd(R_FIFO_DATA, buf, n) != 0) return -1;
    /* Drop frames with a dummy half: a sensor that has not produced its
     * first sample yet (the accelerometer, right after power-up) fills its
     * 6 bytes with 0x01|0x02 0x7F 0x00 0x80 ... (bmi2.c check_dummy_frame;
     * seen on both boards as the first two frames after open). A real
     * sample whose gx (or ax) is exactly 0x7F01/0x7F02 with gy (or ay)
     * -32768 is dropped too; Bosch's parser has the same blind spot. */
    uint32_t k = 0;
    for (uint32_t f = 0; f < n; f += 12) {
        if (dummy(buf + f) || dummy(buf + f + 6)) continue;
        if (k != f) memmove(buf + k, buf + f, 12);
        k += 12;
    }
    return (int)k;
}

int imu_esp_read_gestures(ImuGestures *g) {
    uint8_t st;
    memset(g, 0, sizeof *g);
    if (rd(R_INT_STATUS_0, &st, 1) != 0) return -1;
    if (st & ST_TAP) {
        uint8_t t = 0;
        if (rd(R_TAP_STATUS, &t, 1) != 0) return -1;
        g->bits |= (t & 0x20 ? G_TAP : 0) | (t & 0x40 ? G_DOUBLE : 0) | (t & 0x80 ? G_TRIPLE : 0);
        if (!(t & 0xE0)) g->bits |= G_TAP;
    }
    if (st & ST_FLAT) g->bits |= G_FLAT;
    if (st & ST_ANY_MOT) g->bits |= G_ANY;
    if (st & ST_NO_MOT) g->bits |= G_NO;
    if (st & (ST_ORIENT | ST_STEP)) {
        uint8_t o[8];
        if (wr(R_FEAT_PAGE, 0) != 0 || rd(R_FEATURES, o, 8) != 0) return -1;
        if (st & ST_ORIENT) {
            g->bits |= G_ORIENT;
            g->orient = (uint8_t)(o[6] & 0x07);
        }
        if (st & ST_STEP) {
            g->bits |= G_STEP;
            g->steps = (uint32_t)o[0] | (uint32_t)o[1] << 8 | (uint32_t)o[2] << 16 | (uint32_t)o[3] << 24;
        }
    }
    return 0;
}
