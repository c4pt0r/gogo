/* dice: a port driver written the way a third party would write one, with
 * gogo/port's generic types only, so it needs no new reserved type and no
 * image format change. It is compiled into the test board next to it
 * (board.c) and nowhere else: no firmware and no gogovm has it.
 *
 *   port.Open("dice", port.Host{Arg: seed})   seed > 0, else errors.BadArg
 *   actor.Send(p, port.Post{Kind: n})         roll n dice (1..10; others ignored)
 *   event port.Post{From: p, Kind: face}      one per die, face 1..6
 *
 * The port's whole state is two words of Port.state, so it keeps nothing
 * else per VM and charges its owner nothing. */
#include "port.h"
#include "heap.h"      /* obj_of, int_val: reading the cfg and commands in place */
#include "opcodes.h"   /* TYPE_POST */
#include "../../runtime/include/gogo_errors.h"   /* GOGO_E_BADARG */

enum { SEED, ROLLS };   /* Port.state[] */

/* port.Open("dice", port.Host{Arg, Data}): port_open has checked that cfg
 * is a port.Host (cfg_type). A shared error code (> 0) refuses the open
 * with that error; a negative return would refuse it as errors.Busy. */
static int dice_open(struct Vm *vm, const struct Actor *owner, Port *p, uint16_t index, term cfg) {
    (void)index;
    const uint32_t *o = obj_of(vm, owner, cfg);
    int32_t seed;
    if (hdr_words(o[0]) != 4u || !int_val(vm, owner, o[2], &seed) || seed <= 0) return GOGO_E_BADARG;
    p->state[SEED] = (uint32_t)seed;
    p->state[ROLLS] = 0;
    return 0;
}

/* The next face, 1..6, from a xorshift generator in the port's state. */
static int32_t next_face(Port *p) {
    uint32_t x = p->state[SEED];
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    p->state[SEED] = x;
    p->state[ROLLS]++;
    return (int32_t)(1u + x % 6u);
}

/* port.Post{From: the port, Kind: face, Data: empty} to the owner. Every
 * field is an immediate (a PID, a small int, the empty Bytes), so
 * port_deliver_ints can build it. False (dropped, nobody killed) when the
 * owner is gone or its mailbox quota is full. */
static bool emit(struct Vm *vm, const Port *p, int32_t face) {
    int32_t v[3] = { (int32_t)p->id, face, (int32_t)T_EMPTYBYTES };
    return port_deliver_ints(vm, p, TYPE_POST, v, 3, PORT_RAW(0) | PORT_RAW(2));
}

/* actor.Send(p, port.Post{Kind: n}), from any actor, read in place on the
 * sender's heap. Anything else sent to the port is ignored. */
static void dice_command(struct Vm *vm, const struct Actor *from, Port *p, term msg) {
    const uint32_t *o = obj_of(vm, from, msg);
    int32_t n;
    if (!o || hdr_kind(o[0]) != K_STRUCT || o[1] != TYPE_POST || hdr_words(o[0]) != 5u ||
        !int_val(vm, from, o[3], &n) || n < 1 || n > 10)
        return;
    while (n-- > 0) {
        if (!emit(vm, p, next_face(p))) return;   /* the owner cannot take more now */
    }
}

/* Events come only in answer to commands, so an open dice port does not
 * keep the VM alive by itself. */
static bool dice_can_event(const struct Vm *vm, const Port *p) {
    (void)vm;
    (void)p;
    return false;
}

const PortDriver dice_driver = {
    .name = "dice",
    .cfg_type = TYPE_HOST,
    .event_type = TYPE_POST,   /* documentation only: dice never uses the event ring */
    .can_event = dice_can_event,
    .command_term = dice_command,
    .open_term = dice_open,
};
