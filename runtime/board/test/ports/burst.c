/* "test.burst": its open (Arg n) posts n bytes to its own port through a
 * poster, once, and sends the owner the post's result as an event's Kind:
 * 0, or GOGO_EFULL past the board's byte budget (ports.post_bytes). */
#include <string.h>
#include "gogo.h"

static int burst_open(void *ctx, gogo_vm *vm, gogo_port port, int32_t arg, const uint8_t *data, size_t len) {
    static uint8_t buf[8192];
    (void)ctx, (void)data, (void)len;
    if (arg < 0 || (size_t)arg > sizeof buf) return GOGO_EINVAL;
    gogo_poster *p = gogo_poster_new(vm);
    if (!p) return GOGO_ENOMEM;
    int rc = gogo_poster_port_post(p, port, 0, buf, (size_t)arg);
    gogo_poster_free(p);
    return gogo_port_emit(vm, port, rc, NULL, 0);
}

static const gogo_port_driver burst = { .size = sizeof(gogo_port_driver), .open = burst_open };

GOGO_BOARD_PORT("test.burst", &burst, NULL);
