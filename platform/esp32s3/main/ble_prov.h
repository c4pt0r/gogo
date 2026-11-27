/* Wi-Fi provisioning over BLE (ble_prov.c, CONFIG_GOGO_BLE_PROV): a phone
 * gives the board its Wi-Fi network with Espressif's provisioning protocol
 * (the "ESP BLE Provisioning" app), security 2 with a one-time PIN shown on
 * the screen. Entered only by holding the board's setup key at boot
 * (StickS3: button A; Cardputer: W); closes on success or after 120 s and
 * then restarts the board. */
#ifndef GOGO_BLE_PROV_H
#define GOGO_BLE_PROV_H

#include <stdbool.h>

/* True while a provisioning session runs (the radio is provisioning's: the
 * program's `ble` port answers Busy). Any task. */
bool ble_prov_active(void);

/* Called once by the VM task at boot, after the console and the config
 * store are up and before Wi-Fi and the image: if the setup key is held,
 * runs a provisioning session and restarts the board (never returns);
 * otherwise returns at once. */
void ble_prov_boot(void);

#endif
