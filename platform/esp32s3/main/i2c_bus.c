/* The board's shared I2C master bus (spec input §5.6): the Cardputer ADV's
 * internal bus (port 0, SDA G8, SCL G9: TCA8418 keyboard, also the IMU and
 * codec) and the StickS3's (port 1, SDA G47, SCL G48: M5PM1, IMU). One bus
 * per board, so every device on it shares the handle. Created on first use,
 * which is before the VM starts (StickS3 console bring-up) or on the
 * scheduler thread (input drivers); never from an ISR. The driver
 * serialises transfers itself. */
#include "platform_esp.h"
#include "board.h"

#include "driver/i2c_master.h"

i2c_master_bus_handle_t i2c_bus_get(void) {
    static i2c_master_bus_handle_t bus;
    if (bus) return bus;
    i2c_master_bus_config_t bc = {
        .i2c_port = I2C_BUS_PORT,
        .sda_io_num = I2C_BUS_SDA,
        .scl_io_num = I2C_BUS_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = 1,
    };
    if (i2c_new_master_bus(&bc, &bus) != ESP_OK) bus = NULL;
    return bus;
}
