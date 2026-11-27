/* The names of the runtime's port drivers: every driver takes its name
 * from here, and the embedding layer refuses them as host port names, on
 * any board (runtime/embed/hostport.c). tests/portnames_test.go checks
 * PORT_NAME_* against the names gogo/... passes to port.Open and the
 * firmware's provides lists (cmd/gogo/firmware.go), so a new driver
 * fails `make test` until it is listed here. Macros only: no code. */
#ifndef GOGO_PORT_NAMES_H
#define GOGO_PORT_NAMES_H

#define PORT_NAME_GPIO       "gpio"
#define PORT_NAME_NETIF      "netif"
#define PORT_NAME_UDP        "udp"
#define PORT_NAME_TCP        "tcp"
#define PORT_NAME_TCP_LISTEN "tcp_listen"
#define PORT_NAME_TLS        "tls"
#define PORT_NAME_KBD        "kbd"
#define PORT_NAME_POWER      "power"
#define PORT_NAME_IMU        "imu"
#define PORT_NAME_IR_TX      "ir_tx"
#define PORT_NAME_IR_RX      "ir_rx"
#define PORT_NAME_MIC        "mic"
#define PORT_NAME_SPEAKER    "speaker"
#define PORT_NAME_SCREEN     "screen"
#define PORT_NAME_NODE       "node"
#define PORT_NAME_BLE        "ble"

/* gogo/fs's private driver of an open File (runtime/vfs/builtin.c): no
 * program opens it by name, but no host port may take its name either. */
#define PORT_PRIVATE_FILE    "file"

/* A requirement name that is not a driver: the VM's floating point
 * (GOGO_FLOAT; module.c). No host port may take it, or it would satisfy
 * the requirement on a VM without floats. */
#define PORT_REQ_FLOAT       "float"

/* Every name above, for a table of them. */
#define PORT_RESERVED_NAMES                                                            \
    PORT_NAME_GPIO, PORT_NAME_NETIF, PORT_NAME_UDP, PORT_NAME_TCP, PORT_NAME_TCP_LISTEN, \
    PORT_NAME_TLS, PORT_NAME_KBD, PORT_NAME_POWER, PORT_NAME_IMU, PORT_NAME_IR_TX,      \
    PORT_NAME_IR_RX, PORT_NAME_MIC, PORT_NAME_SPEAKER, PORT_NAME_SCREEN, PORT_NAME_NODE, PORT_NAME_BLE, PORT_PRIVATE_FILE, \
    PORT_REQ_FLOAT

#endif
