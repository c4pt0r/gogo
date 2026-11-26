/* The text screen (gogo/screen): a grid of character cells, the draw
 * operations that change it, and its rendering.
 *
 * Pure C99, no allocation, no I/O: the "screen" port (port.c) owns one
 * ScreenGrid per open port, applies each port.Out's operations to it and
 * hands the rows that changed to the board's display hook (ScreenHw); the
 * board renders them with screen_render_row (LCD) or screen_row_utf8 (a
 * terminal).
 *
 * A cell is one uint32_t: bits 0-15 the code point (the Basic Multilingual
 * Plane; anything else is stored as the box U+FFFD), 16-19 the foreground
 * colour, 20-23 the background colour, 24 bold, 25 underline, 26 reverse,
 * 27 the right half of a wide character (its code point 0), 28 the left
 * half. The grid keeps wide characters whole: overwriting either half of
 * one blanks the other.
 *
 * Operations (lib/screen/screen.gg has the reference): an op byte and its
 * arguments, coordinates in cells. A sequence is checked whole first
 * (screen_ops_check); a port applies nothing of a malformed one. Drawing is
 * clipped to the clip rectangle, which is always inside the grid, so no
 * operation, malformed or not, writes outside the cell array.
 */
#ifndef GOGO_SCREEN_H
#define GOGO_SCREEN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "font.h"

/* The largest grid: coordinates are bytes, the dirty mask has 32 bits. */
#define SCREEN_MAX_COLS 255u
#define SCREEN_MAX_ROWS 32u

/* Operations (screen.Op*). */
enum {
    SCREEN_OP_CLEAR = 1,   /* blank the clip rectangle; cursor to its top-left */
    SCREEN_OP_MOVE = 2,    /* x y */
    SCREEN_OP_TEXT = 3,    /* n (1..255), n bytes of UTF-8 */
    SCREEN_OP_COLOR = 4,   /* c: fg c>>4, bg c&15 */
    SCREEN_OP_ATTR = 5,    /* a: SCREEN_A_* mask */
    SCREEN_OP_FILL = 6,    /* x y w h lo hi: fill with code point lo | hi<<8 */
    SCREEN_OP_CLIP = 7,    /* x y w h, cut to the grid */
    SCREEN_OP_SCROLL = 8,  /* x y w h n: up n rows (signed; negative: down), inside the clip */
    SCREEN_OP_CURSOR = 9   /* on (0 or 1): show the cursor at the current position, or hide it */
};

/* Attributes (screen.Bold ...). */
enum { SCREEN_A_BOLD = 1, SCREEN_A_UNDERLINE = 2, SCREEN_A_REVERSE = 4, SCREEN_A_ALL = 7 };

/* Cell fields. */
#define SCREEN_CP_MASK 0xFFFFu
#define SCREEN_FG_SHIFT 16
#define SCREEN_BG_SHIFT 20
#define SCREEN_F_BOLD (1u << 24)
#define SCREEN_F_UNDERLINE (1u << 25)
#define SCREEN_F_REVERSE (1u << 26)
#define SCREEN_F_CONT (1u << 27)   /* right half of a wide character */
#define SCREEN_F_WIDE (1u << 28)   /* left half of a wide character */
#define SCREEN_BOX 0xFFFDu         /* stored for undecodable or non-BMP text */

/* The default colours: White (7) on Black (0). */
#define SCREEN_DEF_FG 7u
#define SCREEN_DEF_BG 0u

static inline uint32_t screen_cell_cp(uint32_t c) { return c & SCREEN_CP_MASK; }
static inline uint32_t screen_cell_fg(uint32_t c) { return (c >> SCREEN_FG_SHIFT) & 15u; }
static inline uint32_t screen_cell_bg(uint32_t c) { return (c >> SCREEN_BG_SHIFT) & 15u; }

typedef struct ScreenGrid {
    uint32_t *cell;      /* rows * cols cells, row-major, owned by the caller */
    uint32_t dirty;      /* bit r: row r changed since the last screen_take_dirty */
    uint8_t cols, rows;
    uint8_t cx, cy;      /* the cursor (may lie outside the grid) */
    uint8_t x0, y0, x1, y1;   /* the clip: x0 <= x < x1, y0 <= y < y1, inside the grid */
    uint8_t color;       /* fg << 4 | bg */
    uint8_t attr;        /* SCREEN_A_* */
    uint8_t cursor_on;   /* the cursor is shown ... */
    uint8_t kx, ky;      /* ... at kx, ky: where cx, cy were at the last OpCursor 1 */
} ScreenGrid;

/* Points g at cells (cols * rows of them; 1 <= cols <= SCREEN_MAX_COLS,
 * 1 <= rows <= SCREEN_MAX_ROWS) and resets it: every cell blank in the
 * default colours, no attributes, the clip the whole grid, the cursor
 * hidden at 0, 0, every row dirty. */
void screen_init(ScreenGrid *g, uint32_t *cells, uint8_t cols, uint8_t rows);

/* True if d[0..n) is a well-formed operation sequence (n 0 is). Pure. */
bool screen_ops_check(const uint8_t *d, uint32_t n);

/* Applies the operations in d[0..n) to g, in order; stops at the first
 * malformed one (which a caller avoids with screen_ops_check). Rows whose
 * cells differ at the end from what they were at the start, and the rows
 * the cursor leaves or enters while shown, are marked dirty: a frame that
 * clears and redraws everything marks only the rows that changed (rows
 * are compared by a 64-bit hash). */
void screen_ops_apply(ScreenGrid *g, const uint8_t *d, uint32_t n);

/* Returns the dirty mask and clears it. */
uint32_t screen_take_dirty(ScreenGrid *g);

/* Columns (1 or 2) code point cp takes on the screen: the console's rule
 * (console_cp_cols), except that box drawing and block elements
 * (U+2500..U+259F) are narrow. */
int screen_cp_cols(uint32_t cp);

/* The cell at (x, y); 0 outside the grid. */
uint32_t screen_cell(const ScreenGrid *g, int x, int y);

/* Writes row r as UTF-8 into buf (at most cap - 1 bytes, NUL-terminated):
 * one character per cell, a blank cell as a space, a wide character once
 * (its right half adds nothing). Returns the length. */
size_t screen_row_utf8(const ScreenGrid *g, int r, char *buf, size_t cap);

/* Writes code point cp (< 0x10000) as UTF-8 into b (1-3 bytes); returns
 * the length. */
size_t screen_utf8(uint32_t cp, char *b);

/* One character for the style of cell c, for text dumps: '.' default
 * colours and no attributes; else 'R' reverse, 'U' underline, 'B' bold,
 * '#' a background other than Black, or the foreground colour as a hex
 * digit (0-9, a-f), the first that applies. */
char screen_style_char(uint32_t c);

/* The palette as RGB565 (index = colour). */
extern const uint16_t screen_rgb565[16];

/* Renders row r into px: cols * 6 by 12 RGB565 pixels, row-major, host
 * byte order. Box drawing and the block elements are drawn as lines and
 * fills; other glyphs come from f (NULL, or a glyph it lacks: a box).
 * Bold doubles each glyph pixel to the right, underline is the bottom
 * pixel row, reverse swaps the colours; the cursor, when shown, reverses
 * its cell (kx, ky). */
void screen_render_row(const ScreenGrid *g, const Font *f, int r, uint16_t *px);

#endif
