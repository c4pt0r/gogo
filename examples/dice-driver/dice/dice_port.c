/* The board's dice port: registered at boot, before the image loads, in
 * any firmware or gogovm this file is built into. */
#include "dice.h"

static DiceTable dice_table;   /* a board runs one VM */

GOGO_BOARD_PORT("dice", &dice_driver, &dice_table);
