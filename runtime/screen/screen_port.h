/* The "screen" port (gogo/screen): one ScreenGrid per open port, drawn on
 * the board's display through a ScreenHw.
 *
 *  - Open: port.Open("screen", screen.Config{Cols, Rows}); 0 means the
 *    board's default size; larger than its maximum is refused, as is a
 *    second screen port in the same Vm or a display the hook says is busy.
 *    The grid (4 bytes per cell) and the port's state come from the VM
 *    allocator and are charged to the owner. The owner is then sent one
 *    port.Chunk{Data: cols, rows, 16, attrs}; the display shows the blank
 *    grid at once.
 *  - Commands: every port.Out{Data} is a frame of operations (screen.h).
 *    It is checked whole and applied at once, the rows it changed are
 *    handed to the display, and the owner gets port.Done{Seq} (Seq counts
 *    the frames drawn, from 1); a malformed or empty frame is not drawn and
 *    gets Done{-1}. The data is read in place: nothing is copied or held.
 *    A Done the owner has no mailbox room for is owed and retried every
 *    10 ms (after collecting a waiting owner's heap slack, as stream/
 *    does), in order, by the port's own event source, so every Out gets
 *    its Done eventually; owed Dones keep the VM running. With
 *    SCREEN_OWED_MAX owed, a further Out is not drawn and gets Done{-1}
 *    (counted, never lost). Without a free event-source slot the retry
 *    happens only at the next Out. Other commands are ignored.
 *  - Close (or the owner's exit): the display is handed back (the text
 *    console shows again) and the memory freed.
 *
 * Scheduler thread only, like every port callback. */
#ifndef GOGO_SCREEN_PORT_H
#define GOGO_SCREEN_PORT_H

#include <stdint.h>
#include <stdbool.h>
#include "port.h"
#include "opcodes.h"
#include "screen.h"

struct Vm;
struct Actor;

#define SCREEN_OWED_MAX 8u

/* A board's display. Every hook runs on the scheduler thread.
 * open: the display is now the grid's (cols x rows); 0 ok, -1 busy (another
 * Vm has it). present: rows `dirty` of g changed (and the cursor, when
 * g->cursor_on); copy what is needed before returning, g changes
 * afterwards. close: give the display back. */
typedef struct ScreenHw {
    uint8_t def_cols, def_rows;   /* the size for Config{0, 0} */
    uint8_t max_cols, max_rows;   /* the largest grid (<= SCREEN_MAX_COLS / _ROWS) */
    uint8_t attrs;                /* the attributes it shows (SCREEN_A_*) */
    int  (*open)(uint8_t cols, uint8_t rows);
    void (*present)(const ScreenGrid *g, uint32_t dirty);
    void (*close)(void);
} ScreenHw;

/* The PortDriver pieces. A board defines its driver with its own open
 * that passes its ScreenHw:
 *
 *   static int my_open(struct Vm *vm, Port *p, uint16_t i, const int32_t *cfg, uint32_t n) {
 *       (void)i; return screen_port_open(vm, p, cfg, n, &my_screen_hw);
 *   }
 *   const PortDriver my_screen_driver = SCREEN_PORT_DRIVER(my_open);
 */
int  screen_port_open(struct Vm *vm, Port *p, const int32_t *cfg, uint32_t n, const ScreenHw *hw);
void screen_port_command(struct Vm *vm, const struct Actor *from, Port *p, term msg);
void screen_port_close(struct Vm *vm, Port *p);
bool screen_port_can_event(const struct Vm *vm, const Port *p);
void screen_port_ignore(struct Vm *vm, Port *p, uint32_t type, const int32_t *f, uint32_t n);

#define SCREEN_PORT_DRIVER(open_fn) \
    { "screen", TYPE_SCREEN_CONFIG, TYPE_DONE, (open_fn), screen_port_ignore, screen_port_close, \
      screen_port_can_event, NULL, screen_port_command, NULL }

/* Tests: the grid of an open screen port, NULL if p is not one. */
const ScreenGrid *screen_port_grid(const Port *p);

#endif
