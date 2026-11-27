/* The firmware's Wi-Fi provisioning over BLE, as the rest of the firmware
 * sees it: the one function the program's "ble" port needs from it.
 *
 * bool ble_prov_active(void): true while provisioning is on air (from the
 * moment it acquires the NimBLE host, ble_host.h, until it has released
 * it). While it is, port.Open("ble") fails with errors.Busy (ble_esp.c),
 * so the two never advertise at once. Any task may call it; it does not
 * block.
 *
 * ble_host.c holds a weak default that returns false, for a firmware
 * without provisioning; the provisioning code's own definition replaces
 * it at link time. Keep this interface stable: the port and provisioning
 * are built separately against it. */
#ifndef GOGO_BLE_PROV_H
#define GOGO_BLE_PROV_H

#include <stdbool.h>

bool ble_prov_active(void);

#endif
