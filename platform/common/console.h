/* Hardware-independent text grid for the on-screen debug console.
 *
 * 40 columns x 11 rows of 6x12 pixel cells (240x132 of a 240x135 landscape
 * screen). ASCII takes one column, CJK/full-width characters two. Input is a
 * UTF-8 byte stream decoded incrementally. No allocation; not thread-safe:
 * the caller holds a lock around console_write / console_take_dirty /
 * console_render_row.
 */
#ifndef GOGO_CONSOLE_H
#define GOGO_CONSOLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "font.h"

#define CONSOLE_COLS 40
#define CONSOLE_ROWS 11
#define CONSOLE_CELL_W 6
#define CONSOLE_CELL_H 12
#define CONSOLE_PX_W (CONSOLE_COLS * CONSOLE_CELL_W) /* 240 */

enum { CON_WHITE = 0, CON_RED = 1 };

#define CONSOLE_BOX 0xFFFDu /* code point stored for invalid input */

typedef struct Console {
    uint32_t cell[CONSOLE_ROWS][CONSOLE_COLS]; /* layout in console.c; 0 = blank */
    uint16_t dirty;                            /* bit r = row r changed */
    uint8_t row, col;                          /* cursor; col == COLS: wrap pending */
    uint8_t need;                              /* UTF-8: continuation bytes still expected */
    uint32_t acc;                              /* UTF-8: bits decoded so far */
    uint32_t min;                              /* UTF-8: smallest cp this sequence may encode */
} Console;

void console_init(Console *c); /* blank grid, all rows dirty */

/* Appends n bytes of UTF-8 text in the given color (CON_WHITE / CON_RED). */
void console_write(Console *c, const char *s, size_t n, uint8_t color);

/* Returns and clears the dirty mask (bit r = row r changed). */
uint16_t console_take_dirty(Console *c);

/* Number of columns (1 or 2) a code point occupies. Pure function of cp. */
int console_cp_cols(uint32_t cp);

/* Cell inspection (tests): cp stored at (r, col), 0 for blank; the right half
 * of a wide character reads 0 and console_cell_cont is true. */
uint32_t console_cell_cp(const Console *c, int r, int col);
bool console_cell_cont(const Console *c, int r, int col);
uint8_t console_cell_color(const Console *c, int r, int col);

/* Renders row r into px: 240 x 12 RGB565 pixels, row-major, host byte order.
 * Glyphs missing from the font are drawn as a box. */
void console_render_row(const Console *c, const Font *f, int r, uint16_t *px);

#endif
