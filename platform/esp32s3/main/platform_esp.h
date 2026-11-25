/* ESP32-S3 platform layer: shared between main.c, platform_esp.c,
 * gpio_esp.c and image_part.c. */
#ifndef GOGO_PLATFORM_ESP_H
#define GOGO_PLATFORM_ESP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "input.h"   /* ImuGestures, for the IMU hooks below */
#include "host.h"

/* The ESP board (platform_esp.c): its gogo_host (ctx unused, NULL) and
 * VmBoard (port drivers, input chips, Wi-Fi netif, the build's caps). */
extern const gogo_host esp_host;
extern const VmBoard esp_board;

/* The board functions behind esp_host, for the other ESP files. */
void platform_write(const char *s, size_t n);       /* serial + screen, white */
void platform_write_err(const char *s, size_t n);   /* serial + screen, red */
void platform_wait(uint32_t ms);                    /* latched: see platform_notify */
void platform_notify(void);                         /* ISR-safe; wakes the VM task */
int32_t platform_config_get(const uint8_t *key, uint32_t klen, uint8_t *buf, uint32_t cap);   /* config_esp.c */

/* Called from the VM task before vm_init: records the task to notify and
 * the clock origin. */
void platform_esp_init(void);
/* Milliseconds since platform_esp_init (ISR-safe; esp_host.now_ms, the
 * clock of PortEvent.time_ms). */
uint64_t platform_esp_ms(void);
size_t platform_esp_arena_used(void);
size_t platform_esp_arena_cap(void);
/* Prints the first refused esp_host.alloc (size, cause, arena used,
 * free internal RAM at that moment), if any. */
void platform_esp_report_refusal(void);
/* mbedTLS memory outstanding and its peak since boot, in bytes
 * (CONFIG_MBEDTLS_CUSTOM_MEM_ALLOC; platform_esp.c). */
void platform_esp_tls_mem(size_t *now, size_t *peak);
/* Prints a line on the console (stderr). */
void platform_esp_say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* Prints a line on the serial console only (not the screen). */
void platform_esp_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* Never returns. */
void platform_esp_idle(void) __attribute__((noreturn));

/* Maps the image in the `gogo` partition. 0 and sets words, nwords on success;
 * otherwise prints `gogo: no image` or `gogo: bad image (<reason>)` and
 * returns -1. */
int image_part_map(const uint32_t **words, uint32_t *nwords);

/* Screen (console_task.c, lcd_st7789.c, pm1.c). */
#define LCD_W 240      /* landscape */
#define LCD_H 135
#define LCD_BAND_H 12  /* one console row */

/* Starts the on-screen console: grid and font, then the render task (CPU0),
 * which powers the LCD (M5PM1) and initialises the ST7789 while this task
 * waits for the result. Called once from the VM task before any output.
 * Failures are logged on serial and leave the console off. No-op when built
 * with GOGO_ESP_CONSOLE=0. */
void console_esp_start(void);
/* GOGO_CONSOLE_VERBOSE builds: logs the time the VM spent in console writes. */
void console_esp_report(void);
/* Copies n bytes into the console grid (CON_WHITE / CON_RED) under a short
 * lock and wakes the render task; never waits for the LCD. */
void console_esp_write(const char *s, size_t n, uint8_t color);

/* The board's shared I2C master bus (i2c_bus.c); NULL if it cannot be
 * created. Only before the VM starts or on the scheduler thread. */
struct i2c_master_bus_t;
struct i2c_master_bus_t *i2c_bus_get(void);

/* M5PM1 (StickS3, pm1.c). 0 on success. pm1_wake turns the chip's I2C idle
 * sleep off (once; every user calls it first); pm1_lcd_power_on turns on the
 * LCD rail; pm1_ext5v switches the EXT_5V boost (IR LED and receiver, Grove
 * 5 V out), *was = its state before (may be NULL);
 * pm1_imu_power_on rail L1 (BMI270); pm1_audio_power_on the codec's rail
 * (L3B, PM1 GPIO2, shared with the LCD). The speaker amplifier (PM1 GPIO3)
 * is on while the speaker wants it (pm1_spk_amp) and the IR receiver does
 * not block it (pm1_spk_amp_block; pm1_spk_amp_blocked tells). */
int pm1_wake(void);
int pm1_lcd_power_on(void);
int pm1_ext5v(bool on, bool *was);
int pm1_imu_power_on(void);
int pm1_audio_power_on(void);
int pm1_spk_amp(bool on);
int pm1_spk_amp_block(bool block);
bool pm1_spk_amp_blocked(void);
/* PM1 register reads (little-endian for u16) and a byte write; 0 on success. */
int pm1_read(uint8_t reg, uint8_t *buf, size_t n);
int pm1_read_u8(uint8_t reg, uint8_t *v);
int pm1_read_u16le(uint8_t reg, uint16_t *v);
int pm1_write_u8(uint8_t reg, uint8_t v);
/* ST7789: GPIO, SPI bus, reset and init (display on, backlight still off).
 * 0 on success. */
int lcd_init(void);
void lcd_backlight(int on);
/* Writes h landscape rows starting at y from px (LCD_W * h big-endian
 * RGB565 pixels, DMA-capable memory). Blocks until sent. 0 on success. */
int lcd_write_rows(int y, int h, const uint16_t *px);

/* /flash (fs_lfs.c): mounts LittleFS on the `gogofs` partition (size from
 * fs.flash.size, grow / format / first-boot format) and registers it with
 * vfs. Needs config_esp_init first. 0 on success; failures are said on the
 * console and leave /flash unmounted (NotMounted). fs_lfs_state_bytes is
 * the storage fixed state: vfs tables + LittleFS state and buffers. */
int fs_lfs_mount(void);
uint32_t fs_lfs_state_bytes(void);
/* Bound on fs_lfs.c's own state (asserted there), so fs_fat.c can check
 * the whole storage fixed state (vfs + LittleFS + FatFs <= 3072) at
 * compile time. */
#define FS_LFS_STATE_MAX 384u

/* /sd (fs_fat.c, boards with HAS_SD): registers /sd with vfs; the card is
 * mounted lazily by the first call (NotMounted without a card). 0 on
 * success. fs_fat_state_bytes is its static state (FATFS, card, bounce
 * sector). */
int fs_fat_init(void);
uint32_t fs_fat_state_bytes(void);

struct PortDriver;
extern const struct PortDriver gpio_esp_driver;

/* BMI270 motion sensor (imu_esp.c, built with CONFIG_GOGO_IMU on a board
 * with HAS_IMU): the InputHw imu_* hooks, contracts in runtime/input/input.h. */
int imu_esp_open(uint16_t rate_hz, uint8_t gestures);
void imu_esp_close(void);
int imu_esp_read_fifo(uint8_t *buf, uint32_t cap);
int imu_esp_read_gestures(ImuGestures *g);

/* Infrared on the RMT (ir_esp.c, built with CONFIG_GOGO_IR): the InputHw
 * ir_* hooks, contracts in runtime/input/input.h. The receiver hooks exist only on
 * a board with HAS_IR_RX. */
int ir_esp_tx_open(uint32_t carrier_hz, uint8_t duty_pct);
void ir_esp_tx_close(void);
int ir_esp_tx_start(const uint8_t *timings, uint32_t n, uint32_t carrier_hz, uint8_t duty_pct);
bool ir_esp_tx_busy(void);
int ir_esp_rx_open(uint32_t gap_us);
int ir_esp_rx_read(uint16_t *buf, uint32_t cap, uint32_t *start_ms);
void ir_esp_rx_close(void);

/* Audio on the ES8311 codec (audio_esp.c, built with CONFIG_GOGO_AUDIO):
 * the InputHw mic_* and spk_* hooks, contracts in runtime/input/input.h. */
int audio_esp_mic_open(uint32_t rate, uint8_t gain_db);
int audio_esp_mic_read(uint8_t *buf, uint32_t cap, uint32_t *lost);
void audio_esp_mic_close(void);
int audio_esp_spk_open(uint32_t rate, uint8_t volume);
int audio_esp_spk_write(const uint8_t *data, uint32_t len);
uint32_t audio_esp_spk_queued(void);
void audio_esp_spk_close(void);

/* Config store (config_esp.c): NVS namespace "gogo". config_esp_init
 * opens it (0 on success); config_esp_get reads key into buf (at most cap
 * bytes) and returns the value's full length or -1 if unset;
 * config_esp_start_serial starts the `gogo config` command handler task
 * on the USB-Serial/JTAG console input. */
int config_esp_init(void);
int32_t config_esp_get(const char *key, uint8_t *buf, uint32_t cap);
/* Removes key (0 on success; -1 if unset or on error). */
int config_esp_erase(const char *key);
void config_esp_start_serial(void);

/* Wi-Fi station (wifi.c): wifi_esp_start initialises and starts the driver
 * (no esp_netif/lwIP) and connects with the stored wifi.ssid/password;
 * wifi_esp_reconfigure reconnects with the current settings; wifi_esp_scan
 * writes one "kv" line per nearby network through out (-1 if busy);
 * wifi_esp_report_heap logs the measured driver memory on serial. */
void wifi_esp_start(void);
void wifi_esp_reconfigure(void);
int wifi_esp_scan(char *line, size_t cap, void (*out)(const char *tag, const char *text, size_t n));
void wifi_esp_report_heap(void);
/* The station as the stack's network interface (VmBoard.netif). */
struct NetIf;
extern const struct NetIf wifi_esp_netif;

#endif
