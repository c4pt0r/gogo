/* Per-board pins and features. The build defines GOGO_BOARD_STICKS3 or
 * GOGO_BOARD_CARDPUTER (CMake GOGO_BOARD=sticks3|cardputer, default
 * sticks3; platform/esp32s3/main/CMakeLists.txt). Both boards: ESP32-S3,
 * 8 MB flash, a 240x135 ST7789 on SPI3 with the same panel setup
 * (lcd_st7789.c), the same partition table.
 *
 * Reference (read only, not vendored; MIT): M5GFX src/M5GFX.cpp autodetect
 * (Cardputer / Cardputer ADV: MOSI G35, SCLK G36, DC G34, CS G37, RST G33,
 * backlight G38, SPI3 at 40 MHz, panel 135x240 offset 52/40, rotation 1,
 * invert: the StickS3's panel numbers too); M5Cardputer SD pins (CS G12,
 * MOSI G14, CLK G40, MISO G39; docs/m3-followups.md). */
#ifndef GOGO_BOARD_H
#define GOGO_BOARD_H

#if defined(GOGO_BOARD_CARDPUTER)

/* M5Stack Cardputer ADV (M5Stamp S3A: ESP32-S3FN8, 8 MB flash, no PSRAM).
 * No power-management chip: the LCD is powered with the board. */
#define BOARD_NAME "cardputer"
#define LCD_PIN_MOSI 35
#define LCD_PIN_SCLK 36
#define LCD_PIN_DC   34
#define LCD_PIN_CS   37
#define LCD_PIN_RST  33
#define LCD_PIN_BL   38
#define HAS_PM1 0
#define HAS_SD  1
#define HAS_KBD 1
#define HAS_IMU 1
#define HAS_IR_TX 1
#define HAS_IR_RX 0
/* Shared I2C bus (i2c_bus.c): TCA8418 keyboard (0x34) with INT on G11, and
 * the battery divider on G10 (ADC1). */
#define I2C_BUS_PORT 0
#define I2C_BUS_SDA  8
#define I2C_BUS_SCL  9
#define KBD_PIN_INT  11
#define BAT_PIN_ADC  10
/* IR LED (RMT TX, ir_esp.c): G44 through 22 ohms; also the Stamp's U0RXD,
 * unused here (the console is USB-Serial/JTAG). No IR receiver. The BMI270
 * is on the I2C bus above (0x68 or 0x69, imu_esp.c). */
#define IR_PIN_TX    44
/* microSD over SPI (fs_fat.c), on SPI2: SPI3 is the LCD's. */
#define SD_PIN_CS   12
#define SD_PIN_MOSI 14
#define SD_PIN_CLK  40
#define SD_PIN_MISO 39
/* Refused by the gpio port: USB D-/D+ (19, 20), the in-package flash
 * (26-32), the LCD (33-38), the SD card (12, 14, 39, 40), the I2C bus
 * (8, 9), the keyboard INT (11), the battery ADC (10) and the IR LED (44). */
#define BOARD_RESERVED_PINS {8, 9, 10, 11, 12, 14, 19, 20, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 44}

#elif defined(GOGO_BOARD_STICKS3)

/* M5StickS3 (ESP32-S3-PICO-1-N8R8: 8 MB flash, 8 MB octal PSRAM: module code only,
 * sdkconfig.sticks3, platform_esp.c platform_code_alloc).
 * The M5PM1 (I2C G47/G48, pm1.c) switches the LCD rail. No SD. */
#define BOARD_NAME "sticks3"
#define LCD_PIN_MOSI 39
#define LCD_PIN_SCLK 40
#define LCD_PIN_DC   45
#define LCD_PIN_CS   41
#define LCD_PIN_RST  21
#define LCD_PIN_BL   38
#define HAS_PM1 1
#define HAS_SD  0
#define HAS_KBD 0
#define HAS_IMU 1
#define HAS_IR_TX 1
#define HAS_IR_RX 1
/* Shared I2C bus (i2c_bus.c): the M5PM1 (0x6E) and the BMI270 (0x68 or
 * 0x69, imu_esp.c). */
#define I2C_BUS_PORT 1
#define I2C_BUS_SDA  47
#define I2C_BUS_SCL  48
/* IR (RMT, ir_esp.c): LED on G46, receiver module on G42, both powered from
 * EXT_5V (M5PM1 PWR_CFG bit 3, pm1.c). */
#define IR_PIN_TX    46
#define IR_PIN_RX    42
/* Refused by the gpio port: USB D-/D+ (19, 20), the in-package flash and
 * PSRAM (26-37, see gpio_esp.c), the LCD (21, 38-41, 45), the M5PM1 I2C
 * bus (47, 48) and IR (42, 46). */
#define BOARD_RESERVED_PINS {19, 20, 21, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 45, 46, 47, 48}

#else
#error "define GOGO_BOARD_STICKS3 or GOGO_BOARD_CARDPUTER (CMake GOGO_BOARD)"
#endif

#endif
