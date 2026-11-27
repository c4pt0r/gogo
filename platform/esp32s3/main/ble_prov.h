/* The firmware's Wi-Fi provisioning over BLE (ble_prov.c,
 * CONFIG_GOGO_BLE_PROV): a phone gives the board its Wi-Fi network with
 * Espressif's provisioning protocol (the "ESP BLE Provisioning" app),
 * security 2 with a one-time PIN shown on the screen. Entered only by
 * holding the board's setup key at boot (StickS3: button A; Cardputer: W);
 * closes on success or after 120 s and then restarts the board.
 *
 * bool ble_prov_active(void): true while provisioning is on air (from the
 * moment it has acquired the NimBLE host, ble_host.h, until it has
 * released it). While it is, port.Open("ble") fails with errors.Busy
 * (ble_esp.c), so the two never advertise at once. Any task may call it;
 * it does not block. ble_host.c holds a weak default that returns false,
 * for a firmware without provisioning (GOGO_BLE_PROV off); ble_prov.c's
 * definition replaces it at link time.
 *
 * void ble_prov_boot(void): called once by the VM task at boot, after the
 * console and the config store are up and before Wi-Fi and the image: if
 * the setup key is held, runs a provisioning session and restarts the
 * board (never returns); otherwise returns at once. */
#ifndef GOGO_BLE_PROV_H
#define GOGO_BLE_PROV_H

#include <stdbool.h>

bool ble_prov_active(void);
void ble_prov_boot(void);

#endif
