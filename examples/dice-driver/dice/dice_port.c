/* The board's dice port: registered at boot, before the image loads, in
 * any firmware or gogovm this file is built into. A board port's name
 * has a vendor prefix ("example."), so it never collides with a name of
 * the runtime's, which never has a '.'. */
#include "dice.h"

static DiceTable dice_table;   /* a board runs one VM */

GOGO_BOARD_PORT("example.dice", &dice_driver, &dice_table);
