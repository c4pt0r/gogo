/* The text screen grid and its draw operations (screen.h). */
#include "screen.h"
#include "console.h"

#include <string.h>

#define CELL(g, x, y) ((g)->cell[(size_t)(y) * (g)->cols + (size_t)(x)])

static uint32_t style(const ScreenGrid *g) {
    uint32_t s = (uint32_t)(g->color >> 4) << SCREEN_FG_SHIFT | (uint32_t)(g->color & 15u) << SCREEN_BG_SHIFT;
    if (g->attr & SCREEN_A_BOLD) s |= SCREEN_F_BOLD;
    if (g->attr & SCREEN_A_UNDERLINE) s |= SCREEN_F_UNDERLINE;
    if (g->attr & SCREEN_A_REVERSE) s |= SCREEN_F_REVERSE;
    return s;
}

/* A blank cell that keeps c's colours and attributes. */
static uint32_t blank_of(uint32_t c) { return (c & ~(SCREEN_CP_MASK | SCREEN_F_CONT | SCREEN_F_WIDE)) | ' '; }

void screen_init(ScreenGrid *g, uint32_t *cells, uint8_t cols, uint8_t rows) {
    memset(g, 0, sizeof *g);
    g->cell = cells;
    g->cols = cols;
    g->rows = rows;
    g->x1 = cols;
    g->y1 = rows;
    g->color = (uint8_t)(SCREEN_DEF_FG << 4 | SCREEN_DEF_BG);
    uint32_t b = style(g) | ' ';
    for (size_t i = 0; i < (size_t)cols * rows; i++) cells[i] = b;
    g->dirty = rows >= 32u ? UINT32_MAX : (1u << rows) - 1u;
}

int screen_cp_cols(uint32_t cp) {
    if (cp >= 0x2500u && cp <= 0x259Fu) return 1;
    return console_cp_cols(cp);
}

uint32_t screen_cell(const ScreenGrid *g, int x, int y) {
    if (x < 0 || y < 0 || x >= g->cols || y >= g->rows) return 0;
    return CELL(g, x, y);
}

uint32_t screen_take_dirty(ScreenGrid *g) {
    uint32_t d = g->dirty;
    g->dirty = 0;
    return d;
}

static void mark(ScreenGrid *g, unsigned y) { g->dirty |= 1u << y; }

/* Makes (x, y) (inside the grid) no part of a wide character: if it is
 * one half of one, both halves become blanks. */
static void split(ScreenGrid *g, unsigned x, unsigned y) {
    uint32_t c = CELL(g, x, y);
    if ((c & SCREEN_F_CONT) && x > 0) CELL(g, x - 1, y) = blank_of(CELL(g, x - 1, y));
    if ((c & SCREEN_F_WIDE) && x + 1u < g->cols) CELL(g, x + 1, y) = blank_of(CELL(g, x + 1, y));
    if (c & (SCREEN_F_CONT | SCREEN_F_WIDE)) CELL(g, x, y) = blank_of(c);
}

/* Puts cp (taking w columns) at (x, y) if it fits inside the clip; a wide
 * character whose right half is clipped leaves a blank in its left half. */
static void put(ScreenGrid *g, unsigned x, unsigned y, uint32_t cp, int w, uint32_t st) {
    if (y < g->y0 || y >= g->y1 || x < g->x0 || x >= g->x1) return;
    if (w == 2 && x + 1u < g->x1) {
        uint32_t a = st | cp | SCREEN_F_WIDE, b = st | SCREEN_F_CONT;
        if (CELL(g, x, y) == a && CELL(g, x + 1, y) == b) return;   /* unchanged: not dirty */
        split(g, x, y);
        split(g, x + 1, y);
        CELL(g, x, y) = a;
        CELL(g, x + 1, y) = b;
    } else {
        uint32_t a = st | (w == 2 ? ' ' : cp);
        if (CELL(g, x, y) == a) return;
        split(g, x, y);
        CELL(g, x, y) = a;
    }
    mark(g, y);
}

/* Fills [x0,x1) x [y0,y1) (inside the grid) with cp. */
static void fill(ScreenGrid *g, unsigned x0, unsigned y0, unsigned x1, unsigned y1, uint32_t cp) {
    uint32_t st = style(g);
    int w = screen_cp_cols(cp);
    for (unsigned y = y0; y < y1; y++)
        for (unsigned x = x0; x < x1; x += (unsigned)w) put(g, x, y, cp, w, st);
}

/* The rectangle x y w h (bytes) cut to the clip, in *r as x0 y0 x1 y1;
 * false if empty. */
static bool cut(const ScreenGrid *g, const uint8_t *a, unsigned r[4]) {
    unsigned x0 = a[0], y0 = a[1], x1 = x0 + a[2], y1 = y0 + a[3];
    if (x0 < g->x0) x0 = g->x0;
    if (y0 < g->y0) y0 = g->y0;
    if (x1 > g->x1) x1 = g->x1;
    if (y1 > g->y1) y1 = g->y1;
    if (x0 >= x1 || y0 >= y1) return false;
    r[0] = x0, r[1] = y0, r[2] = x1, r[3] = y1;
    return true;
}

static void scroll(ScreenGrid *g, const uint8_t *a) {
    unsigned r[4];
    int n = (int8_t)a[4];
    if (!cut(g, a, r) || n == 0) return;
    unsigned x0 = r[0], y0 = r[1], x1 = r[2], y1 = r[3], h = y1 - y0, w = x1 - x0;
    /* Characters straddling the rectangle's sides are blanked first, so
     * the moved row segments hold whole characters only. */
    for (unsigned y = y0; y < y1; y++) {
        split(g, x0, y);
        split(g, x1 - 1u, y);
    }
    unsigned k = (unsigned)(n < 0 ? -n : n);
    if (k > h) k = h;
    for (unsigned i = 0; i < h - k; i++) {
        unsigned to = n > 0 ? y0 + i : y1 - 1u - i, from = n > 0 ? to + k : to - k;
        memmove(&CELL(g, x0, to), &CELL(g, x0, from), w * sizeof(uint32_t));
    }
    uint32_t b = style(g) | ' ';
    for (unsigned i = 0; i < k; i++) {
        unsigned y = n > 0 ? y1 - 1u - i : y0 + i;
        for (unsigned x = x0; x < x1; x++) CELL(g, x, y) = b;
    }
    for (unsigned y = y0; y < y1; y++) mark(g, y);
}

/* Draws n bytes of UTF-8 at the cursor; it advances by each character's
 * width, saturating at 255. C0 controls and DEL are skipped; a malformed
 * or non-BMP sequence is one box. */
static void text(ScreenGrid *g, const uint8_t *s, unsigned n) {
    uint32_t st = style(g);
    unsigned i = 0;
    while (i < n) {
        uint32_t b = s[i++], cp, min;
        unsigned need;
        if (b < 0x80u) cp = b, need = 0, min = 0;
        else if (b >= 0xC2u && b <= 0xDFu) cp = b & 0x1Fu, need = 1, min = 0x80u;
        else if (b >= 0xE0u && b <= 0xEFu) cp = b & 0x0Fu, need = 2, min = 0x800u;
        else if (b >= 0xF0u && b <= 0xF4u) cp = b & 0x07u, need = 3, min = 0x10000u;
        else cp = SCREEN_BOX, need = 0, min = 0;
        while (need && i < n && (s[i] & 0xC0u) == 0x80u) cp = cp << 6 | (s[i++] & 0x3Fu), need--;
        if (need || cp < min || (cp >= 0xD800u && cp <= 0xDFFFu) || cp > 0xFFFFu) cp = SCREEN_BOX;
        if (cp < 0x20u || cp == 0x7Fu) continue;
        int w = screen_cp_cols(cp);
        put(g, g->cx, g->cy, cp, w, st);
        unsigned nx = (unsigned)g->cx + (unsigned)w;
        g->cx = (uint8_t)(nx > 255u ? 255u : nx);
    }
}

/* Argument bytes after each op byte (TEXT: 1 plus its length). */
static int argc_of(uint8_t op) {
    switch (op) {
    case SCREEN_OP_CLEAR: return 0;
    case SCREEN_OP_MOVE: return 2;
    case SCREEN_OP_TEXT: case SCREEN_OP_COLOR: case SCREEN_OP_ATTR: case SCREEN_OP_CURSOR: return 1;
    case SCREEN_OP_FILL: return 6;
    case SCREEN_OP_CLIP: return 4;
    case SCREEN_OP_SCROLL: return 5;
    default: return -1;
    }
}

/* Length of the op at d[0..n), or 0 if it is malformed. */
static uint32_t op_len(const uint8_t *d, uint32_t n) {
    int a = argc_of(d[0]);
    if (a < 0 || n < 1u + (uint32_t)a) return 0;
    const uint8_t *p = d + 1;
    switch (d[0]) {
    case SCREEN_OP_TEXT:
        if (p[0] == 0 || n < 2u + p[0]) return 0;
        return 2u + p[0];
    case SCREEN_OP_ATTR:
        if (p[0] & ~SCREEN_A_ALL) return 0;
        break;
    case SCREEN_OP_CURSOR:
        if (p[0] > 1u) return 0;
        break;
    case SCREEN_OP_FILL: {
        uint32_t cp = (uint32_t)p[4] | (uint32_t)p[5] << 8;
        if (cp < 0x20u || cp == 0x7Fu || (cp >= 0xD800u && cp <= 0xDFFFu)) return 0;
        break;
    }
    }
    return 1u + (uint32_t)a;
}

bool screen_ops_check(const uint8_t *d, uint32_t n) {
    uint32_t i = 0;
    while (i < n) {
        uint32_t k = op_len(d + i, n - i);
        if (!k) return false;
        i += k;
    }
    return true;
}

/* FNV-1a over row r's cells, 64 bits. */
static uint64_t row_hash(const ScreenGrid *g, unsigned r) {
    uint64_t h = 0xCBF29CE484222325u;
    for (unsigned x = 0; x < g->cols; x++) {
        uint32_t c = CELL(g, x, r);
        for (int k = 0; k < 4; k++, c >>= 8) h = (h ^ (c & 0xFFu)) * 0x100000001B3u;
    }
    return h;
}

void screen_ops_apply(ScreenGrid *g, const uint8_t *d, uint32_t n) {
    uint8_t ckx = g->kx, cky = g->ky, con = g->cursor_on;
    /* A program that redraws everything (Clear, then all of it) writes
     * every cell; a row whose cells end the frame as they began is not
     * dirty. Compared by hash, not by a copy of the grid. */
    uint64_t h[SCREEN_MAX_ROWS];
    for (unsigned r = 0; r < g->rows; r++) h[r] = row_hash(g, r);
    uint32_t pending = g->dirty;
    g->dirty = 0;
    uint32_t i = 0;
    while (i < n) {
        uint32_t k = op_len(d + i, n - i);
        if (!k) break;
        const uint8_t *a = d + i + 1;
        unsigned r[4];
        switch (d[i]) {
        case SCREEN_OP_CLEAR:
            fill(g, g->x0, g->y0, g->x1, g->y1, ' ');
            g->cx = g->x0, g->cy = g->y0;
            break;
        case SCREEN_OP_MOVE: g->cx = a[0], g->cy = a[1]; break;
        case SCREEN_OP_TEXT: text(g, a + 1, a[0]); break;
        case SCREEN_OP_COLOR: g->color = a[0]; break;
        case SCREEN_OP_ATTR: g->attr = a[0]; break;
        case SCREEN_OP_FILL:
            if (cut(g, a, r)) fill(g, r[0], r[1], r[2], r[3], (uint32_t)a[4] | (uint32_t)a[5] << 8);
            break;
        case SCREEN_OP_CLIP: {
            unsigned x0 = a[0], y0 = a[1], x1 = x0 + a[2], y1 = y0 + a[3];
            if (x1 > g->cols) x1 = g->cols;
            if (y1 > g->rows) y1 = g->rows;
            if (x0 > x1) x0 = x1;
            if (y0 > y1) y0 = y1;
            g->x0 = (uint8_t)x0, g->y0 = (uint8_t)y0, g->x1 = (uint8_t)x1, g->y1 = (uint8_t)y1;
            break;
        }
        case SCREEN_OP_SCROLL: scroll(g, a); break;
        case SCREEN_OP_CURSOR:
            g->cursor_on = a[0];
            if (a[0]) g->kx = g->cx, g->ky = g->cy;
            break;
        }
        i += k;
    }
    /* The cursor is drawn by the renderer: the rows it left or entered
     * changed on the display. */
    for (unsigned r = 0; r < g->rows; r++)
        if ((g->dirty & (1u << r)) && row_hash(g, r) == h[r]) g->dirty &= ~(1u << r);
    g->dirty |= pending;
    bool moved = con != g->cursor_on || ckx != g->kx || cky != g->ky;
    if (con && moved && cky < g->rows) mark(g, cky);
    if (g->cursor_on && moved && g->ky < g->rows) mark(g, g->ky);
}

size_t screen_utf8(uint32_t cp, char *b) {
    if (cp < 0x80u) { b[0] = (char)cp; return 1; }
    if (cp < 0x800u) { b[0] = (char)(0xC0u | cp >> 6); b[1] = (char)(0x80u | (cp & 0x3Fu)); return 2; }
    b[0] = (char)(0xE0u | cp >> 12);
    b[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
    b[2] = (char)(0x80u | (cp & 0x3Fu));
    return 3;
}

size_t screen_row_utf8(const ScreenGrid *g, int r, char *buf, size_t cap) {
    size_t n = 0;
    if (cap == 0) return 0;
    for (int x = 0; r >= 0 && r < g->rows && x < g->cols; x++) {
        uint32_t c = CELL(g, x, r);
        if (c & SCREEN_F_CONT) continue;
        uint32_t cp = screen_cell_cp(c);
        if (cp < 0x20u) cp = ' ';
        if (n + 4u > cap) break;
        n += screen_utf8(cp, buf + n);
    }
    buf[n] = 0;
    return n;
}

char screen_style_char(uint32_t c) {
    if (c & SCREEN_F_REVERSE) return 'R';
    if (c & SCREEN_F_UNDERLINE) return 'U';
    if (c & SCREEN_F_BOLD) return 'B';
    if (screen_cell_bg(c) != SCREEN_DEF_BG) return '#';
    uint32_t fg = screen_cell_fg(c);
    if (fg == SCREEN_DEF_FG) return '.';
    return "0123456789abcdef"[fg];
}
