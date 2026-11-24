/* Compiled with a 32-bit freestanding target by `make check32` to verify
 * layout assumptions. */
#include "term.h"
#include "opcodes.h"
#include "platform.h"
#include "image.h"
#include "actor.h"
#include "heap.h"
#include "msg.h"
#include "interp.h"

_Static_assert(sizeof(term) == 4, "term");
_Static_assert(sizeof(Actor) == 48, "Actor must be 48 bytes on 32-bit");
