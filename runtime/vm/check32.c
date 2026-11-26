/* Compiled with a 32-bit freestanding target by `make check32` to verify
 * layout assumptions. */
#include "term.h"
#include "opcodes.h"
#include "host.h"
#include "image.h"
#include "actor.h"
#include "heap.h"
#include "msg.h"
#include "interp.h"
#include "module.h"
#include "port.h"

_Static_assert(sizeof(term) == 4, "term");
_Static_assert(sizeof(Actor) == 48, "Actor must be 48 bytes on 32-bit");
/* Module table entry (module.h): 48 B Image + 16 B; allocated per loaded
 * module, the boot image's included. */
_Static_assert(sizeof(ModEntry) == 64, "ModEntry is 64 bytes on 32-bit");
/* Port table entry (port.h). */
_Static_assert(sizeof(Port) == 44, "Port is 44 bytes on 32-bit");
