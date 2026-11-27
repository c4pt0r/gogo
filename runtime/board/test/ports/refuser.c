/* "refuser": its open returns the Arg (0 accepts), so the program sees
 * what port.Open makes of each refusal on a board. */
#include "gogo.h"

static int refuser_open(void *ctx, gogo_vm *vm, gogo_port port, int32_t arg, const uint8_t *data, size_t len) {
    (void)ctx, (void)vm, (void)port, (void)data, (void)len;
    return arg;
}

static const gogo_port_driver refuser = { .size = sizeof(gogo_port_driver), .open = refuser_open };

GOGO_BOARD_PORT("refuser", &refuser, NULL);
