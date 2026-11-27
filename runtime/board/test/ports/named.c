/* Board ports for the board-port tests (test.sh): "zeta" and "alpha",
 * registered in this order in the source but in name order at boot; a
 * second "alpha" (dup.c, linked after this file), "gpio" (the board's)
 * and a name too long are refused with a boot line. Each port's open
 * sends its owner one event whose Data is the port's ctx. */
#include <string.h>
#include "gogo.h"

static int named_open(void *ctx, gogo_vm *vm, gogo_port port, int32_t arg, const uint8_t *data, size_t len) {
    (void)arg, (void)data, (void)len;
    const char *who = ctx;
    return gogo_port_emit(vm, port, 1, who, strlen(who));
}

const gogo_port_driver named_driver = { .size = sizeof(gogo_port_driver), .open = named_open };

GOGO_BOARD_PORT("zeta", &named_driver, "zeta");
GOGO_BOARD_PORT("alpha", &named_driver, "alpha (first)");
