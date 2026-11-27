/* The dice driver (dice.h): written the way a third party writes one,
 * against gogo.h only. No VM header, no reserved type, no image format
 * change: the program sees port.Host, port.Post and the shared errors. */
#include "dice.h"

/* The slot of port p (0: a free one), or -1. */
static int slot_of(const DiceTable *t, gogo_port p) {
    for (int i = 0; i < DICE_PORTS; i++)
        if (t->slot[i].port == p) return i;
    return -1;
}

/* port.Open("dice", port.Host{Arg: seed}): a negative GOGO_E* refuses the
 * open with that error (GOGO_EINVAL is errors.BadArg). */
static int dice_open(void *ctx, gogo_vm *vm, gogo_port port, int32_t arg, const uint8_t *data, size_t len) {
    DiceTable *t = ctx;
    (void)vm, (void)data, (void)len;
    if (arg <= 0) return GOGO_EINVAL;
    int i = slot_of(t, 0);
    if (i < 0) return GOGO_EFULL;   /* errors.TooMany */
    t->slot[i].port = port;
    t->slot[i].seed = (uint32_t)arg;
    return 0;
}

/* The next face, 1..6, from a xorshift generator. */
static int32_t next_face(uint32_t *seed) {
    uint32_t x = *seed;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *seed = x;
    return (int32_t)(1u + x % 6u);
}

/* port.Post{Kind: n}, from any actor: n events port.Post{Kind: face} to
 * the port's owner, whoever it is now (port.Handoff needs nothing here). */
static void dice_command(void *ctx, gogo_vm *vm, gogo_port port, int32_t kind, const uint8_t *data, size_t len) {
    DiceTable *t = ctx;
    int i = slot_of(t, port);
    (void)data, (void)len;
    if (i < 0 || kind < 1 || kind > 10) return;
    while (kind-- > 0) {
        if (gogo_port_emit(vm, port, next_face(&t->slot[i].seed), NULL, 0) != 0) return;   /* the owner cannot take more now */
    }
}

/* The port has closed, for any reason: its slot is free again. */
static void dice_close(void *ctx, gogo_vm *vm, gogo_port port) {
    DiceTable *t = ctx;
    int i = slot_of(t, port);
    (void)vm;
    if (i >= 0) t->slot[i].port = 0;
}

const gogo_port_driver dice_driver = {
    .size = sizeof(gogo_port_driver),
    .open = dice_open,
    .command = dice_command,
    .close = dice_close,
};
