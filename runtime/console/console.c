#include "console.h"

#include <string.h>

/* Cell layout: bits 0..20 code point, bit 24 red, bit 25 right half of a wide
 * character. 0 is a blank cell. */
#define CP_MASK 0x1FFFFFu
#define F_RED (1u << 24)
#define F_CONT (1u << 25)

#define ALL_ROWS ((uint16_t)((1u << CONSOLE_ROWS) - 1u))
#define TAB 8

#define COLOR_FG_WHITE 0xFFFFu
#define COLOR_FG_RED 0xF800u
#define COLOR_BG 0x0000u

void console_init(Console *c) {
    memset(c, 0, sizeof *c);
    c->dirty = ALL_ROWS;
}

/* Narrow (1 column) code points are the ones the font draws 6 px wide:
 * ASCII, the Latin-1/Latin Extended-A/pinyin letters, Greek, Cyrillic and
 * the prime marks. Everything else is full width. The grid cannot ask the
 * font, so test_console.c checks this rule against every glyph in the font.
 */
int console_cp_cols(uint32_t cp) {
    if (cp < 0x80u) return 1;
    if (cp == CONSOLE_BOX) return 1;
    if (cp >= 0xA0u && cp <= 0x2FFu) return cp == 0xB7u ? 2 : 1;
    if (cp >= 0x391u && cp <= 0x3C9u) return 1;
    if (cp >= 0x401u && cp <= 0x451u) return 1;
    if (cp == 0x2032u || cp == 0x2033u) return 1;
    return 2;
}

uint16_t console_take_dirty(Console *c) {
    uint16_t d = c->dirty;
    c->dirty = 0;
    return d;
}

static void scroll(Console *c) {
    memmove(c->cell[0], c->cell[1], sizeof c->cell[0] * (CONSOLE_ROWS - 1));
    memset(c->cell[CONSOLE_ROWS - 1], 0, sizeof c->cell[0]);
    c->row = CONSOLE_ROWS - 1;
    c->dirty = ALL_ROWS;
}

/* Moves to the start of the next line. Scrolling is lazy: the cursor may sit
 * on row CONSOLE_ROWS until something is drawn, so a trailing "\n" does not
 * waste the bottom row. */
static void newline(Console *c) {
    if (c->row >= CONSOLE_ROWS) scroll(c);
    c->row++;
    c->col = 0;
}

static void put_cp(Console *c, uint32_t cp, uint8_t color) {
    int cols = console_cp_cols(cp);
    if (c->col + cols > CONSOLE_COLS) newline(c);
    if (c->row >= CONSOLE_ROWS) scroll(c);
    uint32_t fl = color == CON_RED ? F_RED : 0;
    uint32_t *line = c->cell[c->row];
    line[c->col] = (cp & CP_MASK) | fl;
    if (cols == 2) line[c->col + 1] = F_CONT | fl;
    c->col = (uint8_t)(c->col + cols);
    c->dirty |= (uint16_t)(1u << c->row);
}

/* Feeds one byte to the UTF-8 decoder. A malformed sequence yields one box
 * and the offending byte is then decoded afresh. */
static void put_byte(Console *c, uint8_t b, uint8_t color) {
    if (c->need) {
        if ((b & 0xC0u) == 0x80u) {
            c->acc = (c->acc << 6) | (b & 0x3Fu);
            if (--c->need == 0) {
                uint32_t cp = c->acc;
                if (cp < c->min || cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu))
                    cp = CONSOLE_BOX;
                put_cp(c, cp, color);
            }
            return;
        }
        c->need = 0;
        put_cp(c, CONSOLE_BOX, color);
        /* fall through: b starts something new */
    }
    if (b < 0x80u) {
        if (b == '\n') newline(c);
        else if (b == '\t') {
            int n = TAB - (c->col % TAB);
            while (n-- > 0) put_cp(c, ' ', color);
        } else if (b >= 0x20u && b != 0x7Fu) put_cp(c, b, color);
        /* other C0 controls and \r: ignored */
    } else if (b >= 0xC2u && b <= 0xDFu) {
        c->need = 1, c->acc = b & 0x1Fu, c->min = 0x80u;
    } else if (b >= 0xE0u && b <= 0xEFu) {
        c->need = 2, c->acc = b & 0x0Fu, c->min = 0x800u;
    } else if (b >= 0xF0u && b <= 0xF4u) {
        c->need = 3, c->acc = b & 0x07u, c->min = 0x10000u;
    } else {
        put_cp(c, CONSOLE_BOX, color); /* stray continuation, C0/C1, F5..FF */
    }
}

void console_write(Console *c, const char *s, size_t n, uint8_t color) {
    for (size_t i = 0; i < n; i++) put_byte(c, (uint8_t)s[i], color);
}

uint32_t console_cell_cp(const Console *c, int r, int col) {
    return c->cell[r][col] & CP_MASK;
}

bool console_cell_cont(const Console *c, int r, int col) {
    return (c->cell[r][col] & F_CONT) != 0;
}

uint8_t console_cell_color(const Console *c, int r, int col) {
    return (c->cell[r][col] & F_RED) ? CON_RED : CON_WHITE;
}

static void draw_box(uint16_t *px, int x0, int cw, uint16_t fg) {
    /* outline of (cw-1) x 10 pixels, rows 1..10 */
    for (int y = 1; y <= 10; y++)
        for (int x = 0; x < cw - 1; x++)
            if (y == 1 || y == 10 || x == 0 || x == cw - 2)
                if (x0 + x < CONSOLE_PX_W) px[y * CONSOLE_PX_W + x0 + x] = fg;
}

void console_render_row(const Console *c, const Font *f, int r, uint16_t *px) {
    for (int i = 0; i < CONSOLE_PX_W * CONSOLE_CELL_H; i++) px[i] = COLOR_BG;
    for (int col = 0; col < CONSOLE_COLS; col++) {
        uint32_t v = c->cell[r][col];
        if ((v & F_CONT) || (v & CP_MASK) == 0 || (v & CP_MASK) == ' ') continue;
        uint32_t cp = v & CP_MASK;
        uint16_t fg = (v & F_RED) ? COLOR_FG_RED : COLOR_FG_WHITE;
        int x0 = col * CONSOLE_CELL_W;
        int cw = (col + 1 < CONSOLE_COLS && (c->cell[r][col + 1] & F_CONT)) ? 12 : 6;
        int gw = 0;
        const uint8_t *g = f ? font_glyph(f, cp, &gw) : NULL;
        if (!g) {
            draw_box(px, x0, cw, fg);
            continue;
        }
        for (int y = 0; y < FONT_HEIGHT; y++)
            for (int x = 0; x < gw && x0 + x < CONSOLE_PX_W; x++)
                if (font_pixel(g, gw, x, y)) px[y * CONSOLE_PX_W + x0 + x] = fg;
    }
}
