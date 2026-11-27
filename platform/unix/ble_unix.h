/* The "ble" port on gogovm (ble_unix.c): a phone simulated over a
 * Unix-domain socket, at $GOGO_BLE (default $TMPDIR/gogo-ble.sock, or
 * /tmp/gogo-ble.sock), which exists while the port is open. */
#ifndef GOGO_BLE_UNIX_H
#define GOGO_BLE_UNIX_H

#include "ble.h"

extern const BleHal ble_unix_hal;

#endif
