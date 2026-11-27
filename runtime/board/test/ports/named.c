/* Board ports for the board-port tests (test.sh): "test.zeta" and
 * "test.beta", registered in this order in the source but in name order
 * at boot; two "test.alpha"s (here and in dup.c), "gpio" (no vendor
 * prefix, and the board's), a name too long and one with a space are
 * refused with a boot line each. Each port's open sends its owner one
 * event whose Data is the port's ctx. */
#include <string.h>
#include "gogo.h"

static int named_open(void *ctx, gogo_vm *vm, gogo_port port, int32_t arg, const uint8_t *data, size_t len) {
    (void)arg, (void)data, (void)len;
    const char *who = ctx;
    gogo_close_posts(vm);   /* a no-op on a board: the ticker's posts still arrive */
    return gogo_port_emit(vm, port, 1, who, strlen(who));
}

const gogo_port_driver named_driver = { .size = sizeof(gogo_port_driver), .open = named_open };

GOGO_BOARD_PORT("test.zeta", &named_driver, "zeta");
GOGO_BOARD_PORT("test.alpha", &named_driver, "alpha (first)");
GOGO_BOARD_PORT("test.beta", &named_driver, "beta");
