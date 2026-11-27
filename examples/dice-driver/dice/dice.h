/* dice: a port driver on gogo's stable driver API (gogo.h) alone. The
 * same table serves a firmware or gogovm (dice_port.c registers it with
 * GOGO_BOARD_PORT) and an embedding host (gogo_register_port).
 *
 *   port.Open("example.dice", port.Host{Arg: seed})  seed > 0, else errors.BadArg
 *   actor.Send(p, port.Post{Kind: n})         roll n dice (1..10; others ignored)
 *   event port.Post{From: p, Kind: face}      one per die, face 1..6 */
#ifndef DICE_H
#define DICE_H

#include "gogo.h"

/* The ports open at once in one table (more: errors.TooMany). */
#define DICE_PORTS 8

/* The driver's state, its ctx: one table per VM. */
typedef struct DiceTable {
    struct {
        gogo_port port;          /* 0: free */
        uint32_t seed;
    } slot[DICE_PORTS];
} DiceTable;

extern const gogo_port_driver dice_driver;

#endif
