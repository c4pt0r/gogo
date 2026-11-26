/* Pixel rendering of the text screen for an LCD (screen.h). */
#include "screen.h"

#define CW FONT_HALF_W    /* 6 */
#define CH FONT_HEIGHT    /* 12 */

#define RGB(r, g, b) (uint16_t)(((r) >> 3) << 11 | ((g) >> 2) << 5 | ((b) >> 3))

/* The VGA text palette, White a lighter grey for the small LCDs. */
const uint16_t screen_rgb565[16] = {
    RGB(0x00, 0x00, 0x00), RGB(0xAA, 0x00, 0x00), RGB(0x00, 0xAA, 0x00), RGB(0xAA, 0x55, 0x00),
    RGB(0x00, 0x00, 0xAA), RGB(0xAA, 0x00, 0xAA), RGB(0x00, 0xAA, 0xAA), RGB(0xC0, 0xC0, 0xC0),
    RGB(0x55, 0x55, 0x55), RGB(0xFF, 0x55, 0x55), RGB(0x55, 0xFF, 0x55), RGB(0xFF, 0xFF, 0x55),
    RGB(0x55, 0x55, 0xFF), RGB(0xFF, 0x55, 0xFF), RGB(0x55, 0xFF, 0xFF), RGB(0xFF, 0xFF, 0xFF),
};

/* Box drawing as arms from the cell's centre: left, right, up, down. */
enum { L = 1, R = 2, U = 4, D = 8 };

static int arms(uint32_t cp) {
    switch (cp) {
    case 0x2500: case 0x2501: case 0x2550: return L | R;            /* ─ ━ ═ */
    case 0x2502: case 0x2503: case 0x2551: return U | D;            /* │ ┃ ║ */
    case 0x250C: case 0x250F: case 0x2554: case 0x256D: return R | D;   /* ┌ ┏ ╔ ╭ */
    case 0x2510: case 0x2513: case 0x2557: case 0x256E: return L | D;   /* ┐ ┓ ╗ ╮ */
    case 0x2514: case 0x2517: case 0x255A: case 0x2570: return R | U;   /* └ ┗ ╚ ╰ */
    case 0x2518: case 0x251B: case 0x255D: case 0x256F: return L | U;   /* ┘ ┛ ╝ ╯ */
    case 0x251C: case 0x2523: case 0x2560: return U | D | R;        /* ├ ┣ ╠ */
    case 0x2524: case 0x252B: case 0x2563: return U | D | L;        /* ┤ ┫ ╣ */
    case 0x252C: case 0x2533: case 0x2566: return L | R | D;        /* ┬ ┳ ╦ */
    case 0x2534: case 0x253B: case 0x2569: return L | R | U;        /* ┴ ┻ ╩ */
    case 0x253C: case 0x254B: case 0x256C: return L | R | U | D;    /* ┼ ╋ ╬ */
    default: return 0;
    }
}

/* Is pixel (x, y) of cell-sized shape cp set? -1: cp is not a shape. */
static int shape(uint32_t cp, int x, int y) {
    int a = arms(cp);
    if (a) {
        enum { MX = 2, MY = 5 };   /* the centre lines */
        return (y == MY && (((a & L) && x <= MX) || ((a & R) && x >= MX))) ||
               (x == MX && (((a & U) && y <= MY) || ((a & D) && y >= MY)));
    }
    switch (cp) {
    case 0x2588: return 1;                          /* █ */
    case 0x2580: return y < CH / 2;                 /* ▀ */
    case 0x2584: return y >= CH / 2;                /* ▄ */
    case 0x258C: return x < CW / 2;                 /* ▌ */
    case 0x2590: return x >= CW / 2;                /* ▐ */
    case 0x2591: return (x % 2 == 0) && (y % 2 == 0);   /* ░ */
    case 0x2592: return (x + y) % 2 == 0;           /* ▒ */
    case 0x2593: return !((x % 2 == 0) && (y % 2 == 0));   /* ▓ */
    default: return -1;
    }
}

static void box(uint16_t *px, int stride, int x0, int cw, uint16_t fg) {
    for (int y = 1; y <= 10; y++)
        for (int x = 0; x < cw - 1; x++)
            if (y == 1 || y == 10 || x == 0 || x == cw - 2) px[y * stride + x0 + x] = fg;
}

void screen_render_row(const ScreenGrid *g, const Font *f, int r, uint16_t *px) {
    int stride = g->cols * CW;
    for (int col = 0; col < g->cols; col++) {
        uint32_t c = screen_cell(g, col, r);
        if (c & SCREEN_F_CONT) continue;   /* drawn with its left half */
        int cw = (c & SCREEN_F_WIDE) ? 2 * CW : CW;
        if (col * CW + cw > stride) cw = stride - col * CW;
        uint16_t fg = screen_rgb565[screen_cell_fg(c)], bg = screen_rgb565[screen_cell_bg(c)];
        bool rev = (c & SCREEN_F_REVERSE) != 0;
        if (g->cursor_on && g->ky == r && (g->kx == col || ((c & SCREEN_F_WIDE) && g->kx == col + 1))) rev = !rev;
        if (rev) { uint16_t t = fg; fg = bg; bg = t; }
        int x0 = col * CW;
        for (int y = 0; y < CH; y++)
            for (int x = 0; x < cw; x++) px[y * stride + x0 + x] = bg;
        uint32_t cp = screen_cell_cp(c);
        if (cp != ' ' && cp != 0) {
            int gw = 0;
            const uint8_t *gl = NULL;
            if (shape(cp, 0, 0) >= 0) {
                for (int y = 0; y < CH; y++)
                    for (int x = 0; x < cw; x++)
                        if (shape(cp, x, y) > 0) px[y * stride + x0 + x] = fg;
            } else if (cp != SCREEN_BOX && f && (gl = font_glyph(f, cp, &gw)) != NULL) {
                bool bold = (c & SCREEN_F_BOLD) != 0;
                for (int y = 0; y < FONT_HEIGHT; y++)
                    for (int x = 0; x < gw && x < cw; x++)
                        if (font_pixel(gl, gw, x, y)) {
                            px[y * stride + x0 + x] = fg;
                            if (bold && x + 1 < cw) px[y * stride + x0 + x + 1] = fg;
                        }
            } else {
                box(px, stride, x0, cw, fg);
            }
        }
        if (c & SCREEN_F_UNDERLINE)
            for (int x = 0; x < cw; x++) px[(CH - 1) * stride + x0 + x] = fg;
    }
}
