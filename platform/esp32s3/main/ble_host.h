/* The NimBLE host, shared by the firmware's BLE users (Wi-Fi provisioning,
 * ble_prov.c; the program's `ble` port), reference-counted: the first
 * ble_host_acquire initialises the controller and the host and starts the
 * host task, the last ble_host_release stops and deinitialises them, so
 * the radio's memory is held only while someone uses it.
 *
 * ble_host_acquire returns 0 once the host is synced with the controller
 * (ble_hs_synced: GAP and GATT calls may follow), -1 if it could not be
 * started (nothing is held then). Each successful acquire is matched by
 * one release. Callable from any task but not from the host task. The
 * caller registers its own GATT services (ble_gatts_reset ... ble_gatts_start)
 * and gives its own GAP callback to ble_gap_adv_start. */
#ifndef GOGO_BLE_HOST_H
#define GOGO_BLE_HOST_H

int ble_host_acquire(void);
void ble_host_release(void);

#endif
