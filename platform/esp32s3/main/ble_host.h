/* The NimBLE host's lifetime, shared by the program's "ble" port
 * (ble_esp.c) and the firmware's Wi-Fi provisioning (ble_prov.h): the
 * controller and host are up while anyone holds them, and down (their
 * memory free) otherwise. Built whenever the firmware has NimBLE
 * (CONFIG_BT_NIMBLE_ENABLED: sdkconfig.ble).
 *
 * The contract (keep it stable: both users are built against it):
 *
 *   int ble_host_acquire(void)
 *     The first holder initializes the controller and the NimBLE host
 *     (nimble_port_init), starts the host task (pinned to core 0) and
 *     waits until the host has synced with the controller, at most
 *     BLE_HOST_SYNC_MS; later holders only count. 0: held; -1: the stack
 *     did not start (nothing held, nothing to release). On return the
 *     host is synced: ble_gatts_*, ble_gap_* and ble_hs_id_* may be called.
 *   void ble_host_release(void)
 *     Drops one hold; the last one stops the host (nimble_port_stop, which
 *     ends any connection and advertising) and deinitializes host and
 *     controller (nimble_port_deinit).
 *
 *   - Callers are tasks (never an ISR, never the NimBLE host task itself,
 *     which a release would wait for); calls are serialized by a mutex
 *     inside, so they may come from different tasks.
 *   - GAP and GATT state is the holder's: after acquire it registers its
 *     services (ble_gatts_reset, ble_svc_gap_init, ble_svc_gatt_init,
 *     ble_gatts_count_cfg, ble_gatts_add_svcs, ble_gatts_start; the host
 *     is synced, so its own start-time registration is over) and starts
 *     advertising with its own GAP callback; before release it stops what
 *     it started. Only one holder is on air at a time: the port refuses
 *     to open while ble_prov_active() (ble_prov.h).
 *   - The host's sync and reset callbacks (ble_hs_cfg.sync_cb, reset_cb)
 *     are this module's; a holder does not set them. */
#ifndef GOGO_BLE_HOST_H
#define GOGO_BLE_HOST_H

#define BLE_HOST_SYNC_MS 2000

int ble_host_acquire(void);
void ble_host_release(void);

#endif
