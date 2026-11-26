/* runtime/screen: the grid's draw operations (decoding, clipping, wide
 * characters, scrolling, dirty rows, malformed sequences), the LCD
 * renderer, and the "screen" port on a real VM (open, info Chunk, Out,
 * Done, refusals, close). The cell array sits between guard words: no
 * sequence may write outside it. */
#include "test.h"
#include "screen.h"
#include "screen_port.h"
#include "actor.h"
#include "alloc.h"
#include "bytes.h"
#include "heap.h"
#include "msg.h"
#include "opcodes.h"
#include "test_host.h"
#include "signals.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GUARD 0xA5A5A5A5u
#define MAXC (40 * 11)

static uint32_t mem[MAXC + 2];
static ScreenGrid g;

static void reset(uint8_t cols, uint8_t rows) {
    mem[0] = GUARD;
    mem[1 + (size_t)cols * rows] = GUARD;
    screen_init(&g, mem + 1, cols, rows);
    screen_take_dirty(&g);
}

static bool guards_ok(void) { return mem[0] == GUARD && mem[1 + (size_t)g.cols * g.rows] == GUARD; }

static const char *row(int r) {
    static char b[256];
    screen_row_utf8(&g, r, b, sizeof b);
    return b;
}

/* Applies d[0..n) if it is well-formed; returns whether it was. */
static bool run(const uint8_t *d, uint32_t n) {
    if (!screen_ops_check(d, n)) return false;
    screen_ops_apply(&g, d, n);
    return true;
}
#define RUN(...) do { static const uint8_t ops_[] = { __VA_ARGS__ }; CHECK(run(ops_, sizeof ops_)); } while (0)

static uint32_t at(int x, int y) { return screen_cell(&g, x, y); }

TEST(screen_init_blank) {
    reset(10, 3);
    for (int y = 0; y < 3; y++) CHECK(strcmp(row(y), "          ") == 0);
    CHECK_EQ(screen_cell_fg(at(0, 0)), SCREEN_DEF_FG);
    CHECK_EQ(screen_cell_bg(at(0, 0)), SCREEN_DEF_BG);
    screen_init(&g, mem + 1, 10, 3);
    CHECK_EQ(screen_take_dirty(&g), 7u);
    CHECK_EQ(screen_take_dirty(&g), 0u);
    CHECK_EQ(screen_cell(&g, 10, 0), 0u);   /* outside: 0 */
    CHECK_EQ(screen_cell(&g, -1, 0), 0u);
}

TEST(screen_text_move_style) {
    reset(10, 3);
    RUN(SCREEN_OP_MOVE, 2, 1, SCREEN_OP_COLOR, 0x14, SCREEN_OP_ATTR, SCREEN_A_BOLD | SCREEN_A_REVERSE,
        SCREEN_OP_TEXT, 2, 'h', 'i');
    CHECK(strcmp(row(1), "  hi      ") == 0);
    uint32_t c = at(2, 1);
    CHECK_EQ(screen_cell_cp(c), (uint32_t)'h');
    CHECK_EQ(screen_cell_fg(c), 1u);
    CHECK_EQ(screen_cell_bg(c), 4u);
    CHECK(c & SCREEN_F_BOLD);
    CHECK(c & SCREEN_F_REVERSE);
    CHECK(!(c & SCREEN_F_UNDERLINE));
    CHECK_EQ(screen_style_char(c), 'R');
    CHECK_EQ(screen_style_char(at(0, 0)), '.');
    CHECK_EQ(g.cx, 4);
    CHECK_EQ(screen_take_dirty(&g), 2u);
    /* the same text again changes no cell: nothing dirty */
    RUN(SCREEN_OP_MOVE, 2, 1, SCREEN_OP_TEXT, 2, 'h', 'i');
    CHECK_EQ(screen_take_dirty(&g), 0u);
    /* nor does clearing and drawing it all again */
    RUN(SCREEN_OP_ATTR, 0, SCREEN_OP_COLOR, 0x70, SCREEN_OP_CLEAR, SCREEN_OP_MOVE, 2, 1, SCREEN_OP_COLOR, 0x14,
        SCREEN_OP_ATTR, SCREEN_A_BOLD | SCREEN_A_REVERSE, SCREEN_OP_TEXT, 2, 'h', 'i');
    CHECK_EQ(screen_take_dirty(&g), 0u);
    /* a change and a change back of a row: not dirty; a real change: dirty */
    RUN(SCREEN_OP_MOVE, 0, 0, SCREEN_OP_TEXT, 1, 'q', SCREEN_OP_MOVE, 0, 0, SCREEN_OP_COLOR, 0x70, SCREEN_OP_ATTR, 0,
        SCREEN_OP_TEXT, 1, ' ', SCREEN_OP_MOVE, 0, 2, SCREEN_OP_TEXT, 1, 'z');
    CHECK_EQ(screen_take_dirty(&g), 4u);
    RUN(SCREEN_OP_COLOR, 0x14, SCREEN_OP_ATTR, SCREEN_A_BOLD | SCREEN_A_REVERSE, SCREEN_OP_MOVE, 4, 1);
    /* the next frame keeps the colours and the cursor */
    RUN(SCREEN_OP_TEXT, 1, '!');
    CHECK(strcmp(row(1), "  hi!     ") == 0);
    CHECK_EQ(screen_cell_fg(at(4, 1)), 1u);
    RUN(SCREEN_OP_ATTR, 0, SCREEN_OP_COLOR, 0x30, SCREEN_OP_MOVE, 0, 0, SCREEN_OP_TEXT, 1, 'x');
    CHECK_EQ(screen_style_char(at(0, 0)), '3');
    RUN(SCREEN_OP_COLOR, 0x72, SCREEN_OP_TEXT, 1, 'y');
    CHECK_EQ(screen_style_char(at(1, 0)), '#');
    CHECK(guards_ok());
}

TEST(screen_text_no_wrap) {
    reset(5, 2);
    RUN(SCREEN_OP_MOVE, 3, 0, SCREEN_OP_TEXT, 5, 'a', 'b', 'c', 'd', 'e');
    CHECK(strcmp(row(0), "   ab") == 0);
    CHECK(strcmp(row(1), "     ") == 0);
    CHECK_EQ(g.cx, 8);   /* the cursor runs on past the edge */
    /* off the grid entirely: nothing drawn, cursor saturates at 255 */
    RUN(SCREEN_OP_MOVE, 250, 9, SCREEN_OP_TEXT, 8, 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h');
    CHECK_EQ(g.cx, 255);
    CHECK(guards_ok());
}

TEST(screen_utf8_wide_and_controls) {
    reset(10, 2);
    /* 你 (wide), é (narrow), a tab and a NUL skipped, a bad byte a box */
    RUN(SCREEN_OP_TEXT, 9, 0xE4, 0xBD, 0xA0, 0xC3, 0xA9, '\t', 0, 0xFF, 'z');
    uint32_t c = at(0, 0);
    CHECK_EQ(screen_cell_cp(c), 0x4F60u);
    CHECK(c & SCREEN_F_WIDE);
    CHECK(at(1, 0) & SCREEN_F_CONT);
    CHECK_EQ(screen_cell_cp(at(2, 0)), 0xE9u);
    CHECK_EQ(screen_cell_cp(at(3, 0)), SCREEN_BOX);
    CHECK_EQ(screen_cell_cp(at(4, 0)), (uint32_t)'z');
    CHECK(strcmp(row(0), "你é\xEF\xBF\xBDz     ") == 0);
    /* a sequence cut by the end of the op, a surrogate, an overlong form and
     * a non-BMP character: one box each */
    RUN(SCREEN_OP_MOVE, 0, 1, SCREEN_OP_TEXT, 2, 0xE4, 0xBD, SCREEN_OP_TEXT, 3, 0xED, 0xA0, 0x80,
        SCREEN_OP_TEXT, 2, 0xC0, 0x80, SCREEN_OP_TEXT, 4, 0xF0, 0x9F, 0x98, 0x80);
    for (int x = 0; x < 4; x++) CHECK_EQ(screen_cell_cp(at(x, 1)), SCREEN_BOX);
    /* box drawing is narrow */
    CHECK_EQ(screen_cp_cols(0x2500), 1);
    CHECK_EQ(screen_cp_cols(0x2588), 1);
    CHECK_EQ(screen_cp_cols(0x4F60), 2);
    CHECK(guards_ok());
}

TEST(screen_wide_overwrite) {
    reset(6, 1);
    RUN(SCREEN_OP_TEXT, 6, 0xE4, 0xBD, 0xA0, 0xE5, 0xA5, 0xBD);   /* 你好 at 0-1, 2-3 */
    /* overwrite the right half of 你: its left half becomes a blank */
    RUN(SCREEN_OP_MOVE, 1, 0, SCREEN_OP_TEXT, 1, 'x');
    CHECK(strcmp(row(0), " x好  ") == 0);
    CHECK(!(at(0, 0) & (SCREEN_F_WIDE | SCREEN_F_CONT)));
    /* overwrite the left half of 好: its right half becomes a blank */
    RUN(SCREEN_OP_MOVE, 2, 0, SCREEN_OP_TEXT, 1, 'y');
    CHECK(strcmp(row(0), " xy   ") == 0);
    CHECK(!(at(3, 0) & SCREEN_F_CONT));
    /* a wide character over two halves of two others */
    reset(6, 1);
    RUN(SCREEN_OP_TEXT, 6, 0xE4, 0xBD, 0xA0, 0xE5, 0xA5, 0xBD, SCREEN_OP_MOVE, 1, 0,
        SCREEN_OP_TEXT, 3, 0xE4, 0xBD, 0xA0);
    CHECK(strcmp(row(0), " 你   ") == 0);
    /* a wide character in the last column: clipped to a blank */
    reset(3, 1);
    RUN(SCREEN_OP_MOVE, 2, 0, SCREEN_OP_TEXT, 3, 0xE4, 0xBD, 0xA0);
    CHECK(strcmp(row(0), "   ") == 0);
    CHECK(!(at(2, 0) & SCREEN_F_WIDE));
    CHECK(guards_ok());
}

TEST(screen_clip) {
    reset(10, 4);
    RUN(SCREEN_OP_CLIP, 2, 1, 4, 2, SCREEN_OP_MOVE, 0, 1, SCREEN_OP_TEXT, 8, 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h',
        SCREEN_OP_MOVE, 3, 0, SCREEN_OP_TEXT, 1, 'X', SCREEN_OP_MOVE, 3, 3, SCREEN_OP_TEXT, 1, 'Y');
    CHECK(strcmp(row(0), "          ") == 0);
    CHECK(strcmp(row(1), "  cdef    ") == 0);
    CHECK(strcmp(row(3), "          ") == 0);
    /* Clear blanks the clip only, and homes the cursor to it */
    RUN(SCREEN_OP_CLIP, 0, 0, 255, 255, SCREEN_OP_MOVE, 0, 2, SCREEN_OP_TEXT, 4, '1', '2', '3', '4',
        SCREEN_OP_CLIP, 1, 2, 2, 1, SCREEN_OP_CLEAR);
    CHECK(strcmp(row(2), "1  4      ") == 0);
    CHECK_EQ(g.cx, 1);
    CHECK_EQ(g.cy, 2);
    /* a clip past the grid is cut to it; one starting past it is empty */
    RUN(SCREEN_OP_CLIP, 8, 3, 200, 200);
    CHECK_EQ(g.x1, 10);
    CHECK_EQ(g.y1, 4);
    RUN(SCREEN_OP_CLIP, 200, 200, 10, 10, SCREEN_OP_CLEAR, SCREEN_OP_FILL, 0, 0, 255, 255, '#', 0);
    CHECK(strcmp(row(1), "  cdef    ") == 0);
    CHECK_EQ(g.x0, g.x1);
    CHECK(guards_ok());
}

TEST(screen_fill) {
    reset(8, 3);
    RUN(SCREEN_OP_FILL, 1, 1, 3, 5, '#', 0, SCREEN_OP_FILL, 6, 0, 9, 1, 0x00, 0x25);   /* ─ at 6, 7 */
    CHECK(strcmp(row(0), "      ──") == 0);
    CHECK(strcmp(row(1), " ###    ") == 0);
    CHECK(strcmp(row(2), " ###    ") == 0);
    /* a wide fill steps two columns; an odd width leaves a blank */
    reset(5, 1);
    RUN(SCREEN_OP_FILL, 0, 0, 5, 1, 0x60, 0x4F);
    CHECK(strcmp(row(0), "你你 ") == 0);
    CHECK(guards_ok());
}

TEST(screen_scroll) {
    reset(4, 4);
    RUN(SCREEN_OP_MOVE, 0, 0, SCREEN_OP_TEXT, 4, 'a', 'a', 'a', 'a', SCREEN_OP_MOVE, 0, 1, SCREEN_OP_TEXT, 4, 'b', 'b', 'b', 'b',
        SCREEN_OP_MOVE, 0, 2, SCREEN_OP_TEXT, 4, 'c', 'c', 'c', 'c', SCREEN_OP_MOVE, 0, 3, SCREEN_OP_TEXT, 4, 'd', 'd', 'd', 'd');
    screen_take_dirty(&g);
    RUN(SCREEN_OP_SCROLL, 1, 0, 2, 4, 1);   /* columns 1-2 up one */
    CHECK(strcmp(row(0), "abba") == 0);
    CHECK(strcmp(row(2), "cddc") == 0);
    CHECK(strcmp(row(3), "d  d") == 0);
    CHECK_EQ(screen_take_dirty(&g), 15u);
    RUN(SCREEN_OP_SCROLL, 0, 0, 4, 4, 0xFE);   /* everything down two */
    CHECK(strcmp(row(0), "    ") == 0);
    CHECK(strcmp(row(1), "    ") == 0);
    CHECK(strcmp(row(2), "abba") == 0);
    RUN(SCREEN_OP_SCROLL, 0, 0, 4, 4, 100);    /* more than the height: all blank */
    for (int y = 0; y < 4; y++) CHECK(strcmp(row(y), "    ") == 0);
    /* a scroll inside the clip only */
    RUN(SCREEN_OP_MOVE, 0, 3, SCREEN_OP_TEXT, 1, 'z', SCREEN_OP_CLIP, 0, 0, 4, 3, SCREEN_OP_SCROLL, 0, 0, 4, 4, 0xFF);
    CHECK(strcmp(row(3), "z   ") == 0);
    /* a wide character across the rectangle's side is blanked first */
    reset(4, 2);
    RUN(SCREEN_OP_MOVE, 1, 1, SCREEN_OP_TEXT, 3, 0xE4, 0xBD, 0xA0, SCREEN_OP_SCROLL, 0, 0, 2, 2, 1);
    for (int y = 0; y < 2; y++)
        for (int x = 0; x < 4; x++) CHECK(!(at(x, y) & (SCREEN_F_WIDE | SCREEN_F_CONT)));
    CHECK(guards_ok());
}

TEST(screen_cursor_dirty) {
    reset(4, 4);
    RUN(SCREEN_OP_MOVE, 1, 2, SCREEN_OP_CURSOR, 1);
    CHECK_EQ(screen_take_dirty(&g), 4u);   /* row 2: the cursor appeared */
    RUN(SCREEN_OP_MOVE, 1, 0);
    CHECK_EQ(screen_take_dirty(&g), 0u);   /* moving does not move the shown cursor */
    RUN(SCREEN_OP_CURSOR, 1);
    CHECK_EQ(screen_take_dirty(&g), 5u);   /* left row 2, entered row 0 */
    RUN(SCREEN_OP_MOVE, 3, 3, SCREEN_OP_TEXT, 1, 'x', SCREEN_OP_MOVE, 1, 0, SCREEN_OP_CURSOR, 1);
    CHECK_EQ(screen_take_dirty(&g), 8u);   /* the text's row only: the cursor stayed */
    CHECK_EQ(g.kx, 1);
    CHECK_EQ(g.ky, 0);
    RUN(SCREEN_OP_CURSOR, 0);
    CHECK_EQ(screen_take_dirty(&g), 1u);
    RUN(SCREEN_OP_MOVE, 3, 3, SCREEN_OP_CURSOR, 0);
    CHECK_EQ(screen_take_dirty(&g), 0u);   /* hidden: nothing to draw */
    RUN(SCREEN_OP_MOVE, 9, 9, SCREEN_OP_CURSOR, 1);   /* outside the grid */
    CHECK_EQ(screen_take_dirty(&g), 0u);
}

TEST(screen_malformed) {
    static const uint8_t bad[][8] = {
        {0}, {10}, {0xFF}, {SCREEN_OP_MOVE, 1}, {SCREEN_OP_TEXT}, {SCREEN_OP_TEXT, 0},
        {SCREEN_OP_TEXT, 3, 'a', 'b'}, {SCREEN_OP_ATTR, 8}, {SCREEN_OP_CURSOR, 2},
        {SCREEN_OP_FILL, 0, 0, 1, 1, 0x1F, 0}, {SCREEN_OP_FILL, 0, 0, 1, 1, 0x00, 0xD8},
        {SCREEN_OP_FILL, 0, 0, 1, 1, 0x7F, 0}, {SCREEN_OP_CLIP, 0, 0, 1}, {SCREEN_OP_SCROLL, 0, 0, 1, 1},
        {SCREEN_OP_COLOR},
    };
    static const uint8_t lens[] = {1, 1, 1, 2, 1, 2, 4, 2, 2, 7, 7, 7, 4, 5, 1};
    for (unsigned i = 0; i < sizeof lens; i++) {
        CHECK(!screen_ops_check(bad[i], lens[i]));
        /* a good prefix does not make it good */
        uint8_t d[16] = {SCREEN_OP_CLEAR};
        memcpy(d + 1, bad[i], lens[i]);
        CHECK(!screen_ops_check(d, 1u + lens[i]));
    }
    CHECK(screen_ops_check(NULL, 0));
    /* apply stops at a malformed op, keeping what came before */
    reset(4, 1);
    static const uint8_t half[] = {SCREEN_OP_TEXT, 1, 'a', 77, SCREEN_OP_TEXT, 1, 'b'};
    screen_ops_apply(&g, half, sizeof half);
    CHECK(strcmp(row(0), "a   ") == 0);
    CHECK(guards_ok());
}

/* Random byte strings, valid or not, applied to small grids: nothing may
 * be written outside the cell array, and the grid stays consistent (no
 * orphaned half of a wide character). */
TEST(screen_fuzz) {
    uint32_t x = 12345;
    uint8_t d[64];
    for (int round = 0; round < 20000; round++) {
        uint8_t cols = (uint8_t)(1 + round % 13), rows = (uint8_t)(1 + (round / 13) % 11);
        if ((size_t)cols * rows > MAXC) continue;
        reset(cols, rows);
        uint32_t n = 1 + round % 63;
        for (uint32_t i = 0; i < n; i++) {
            x ^= x << 13, x ^= x >> 17, x ^= x << 5;
            uint8_t b = (uint8_t)x;
            /* bias towards op bytes and small coordinates */
            d[i] = (x >> 8) % 3 == 0 ? (uint8_t)(1 + b % 9) : (x >> 8) % 3 == 1 ? (uint8_t)(b % 16) : b;
        }
        screen_ops_apply(&g, d, n);   /* malformed or not: must stay inside */
        CHECK(guards_ok());
        for (int yy = 0; yy < rows; yy++)
            for (int xx = 0; xx < cols; xx++) {
                uint32_t c = at(xx, yy);
                if (c & SCREEN_F_WIDE) CHECK(xx + 1 < cols && (at(xx + 1, yy) & SCREEN_F_CONT));
                if (c & SCREEN_F_CONT) CHECK(xx > 0 && (at(xx - 1, yy) & SCREEN_F_WIDE));
            }
        CHECK(g.x0 <= g.x1 && g.x1 <= cols && g.y0 <= g.y1 && g.y1 <= rows);
    }
}

/* Long-lived grids (state carried across frames, up to 255 x 32), with
 * sequences biased towards CJK text, wide fills, edge coordinates and
 * signed scrolls: still nothing outside the cells, whole wide pairs, a
 * clip inside the grid, and the renderer and the text dump stay inside
 * their buffers. */
static uint32_t big[255 * 32 + 2];
static uint16_t bigpx[255 * 6 * 12];

TEST(screen_fuzz_long) {
    static const uint8_t sizes[][2] = { {255, 32}, {40, 11}, {1, 1}, {3, 32}, {255, 1} };
    uint32_t x = 777;
#define RND() (x ^= x << 13, x ^= x >> 17, x ^= x << 5, x)
    for (unsigned si = 0; si < sizeof sizes / sizeof sizes[0]; si++) {
        uint8_t cols = sizes[si][0], rows = sizes[si][1];
        size_t n = (size_t)cols * rows;
        big[0] = GUARD;
        big[1 + n] = GUARD;
        ScreenGrid gg;
        screen_init(&gg, big + 1, cols, rows);
        for (int frame = 0; frame < 3000; frame++) {
            uint8_t d[96];
            uint32_t len = 0;
            while (len < sizeof d - 16) {
                uint8_t edge[] = { 0, 1, (uint8_t)(cols - 1), cols, (uint8_t)(cols + 2), 254, 255, (uint8_t)RND() };
                uint8_t c0 = edge[RND() % 8], c1 = edge[RND() % 8];
                switch (RND() % 9) {
                case 0: d[len++] = SCREEN_OP_MOVE; d[len++] = c0; d[len++] = (uint8_t)(RND() % (rows + 3)); break;
                case 1: {   /* CJK and box text */
                    unsigned k = 1 + RND() % 4;
                    d[len++] = SCREEN_OP_TEXT; d[len++] = (uint8_t)(3 * k);
                    for (unsigned i = 0; i < k; i++) {
                        bool box = RND() % 3 == 0;
                        d[len++] = 0xE4 + (box ? -2 : 0); d[len++] = box ? 0x94 : 0xBD; d[len++] = 0x80 + RND() % 64;
                    }
                    break;
                }
                case 2: d[len++] = SCREEN_OP_FILL; d[len++] = c0; d[len++] = (uint8_t)(RND() % (rows + 2));
                        d[len++] = c1; d[len++] = (uint8_t)(RND() % 40); d[len++] = 0x60; d[len++] = RND() % 2 ? 0x4F : 0x25; break;
                case 3: d[len++] = SCREEN_OP_CLIP; d[len++] = c0; d[len++] = (uint8_t)(RND() % (rows + 2));
                        d[len++] = c1; d[len++] = (uint8_t)RND(); break;
                case 4: d[len++] = SCREEN_OP_SCROLL; d[len++] = c0; d[len++] = 0; d[len++] = c1;
                        d[len++] = (uint8_t)RND(); d[len++] = (uint8_t)RND(); break;
                case 5: d[len++] = SCREEN_OP_CLEAR; break;
                case 6: d[len++] = SCREEN_OP_CURSOR; d[len++] = RND() % 2; break;
                case 7: d[len++] = SCREEN_OP_COLOR; d[len++] = (uint8_t)RND(); break;
                default: d[len++] = (uint8_t)RND(); break;   /* noise: often malformed */
                }
            }
            screen_ops_apply(&gg, d, len);
            CHECK(big[0] == GUARD && big[1 + n] == GUARD);
            CHECK(gg.x0 <= gg.x1 && gg.x1 <= cols && gg.y0 <= gg.y1 && gg.y1 <= rows);
            if (frame % 100 == 0) {
                for (int yy = 0; yy < rows; yy++)
                    for (int xx = 0; xx < cols; xx++) {
                        uint32_t c = screen_cell(&gg, xx, yy);
                        if (c & SCREEN_F_WIDE) CHECK(xx + 1 < cols && (screen_cell(&gg, xx + 1, yy) & SCREEN_F_CONT));
                        if (c & SCREEN_F_CONT) CHECK(xx > 0 && (screen_cell(&gg, xx - 1, yy) & SCREEN_F_WIDE));
                    }
                int r = (int)(RND() % rows);
                screen_render_row(&gg, NULL, r, bigpx);
                char txt[255 * 3 + 1];
                CHECK(screen_row_utf8(&gg, r, txt, sizeof txt) < sizeof txt);
            }
        }
    }
#undef RND
}

TEST(screen_render) {
    reset(4, 1);
    RUN(SCREEN_OP_COLOR, 0xF1, SCREEN_OP_TEXT, 3, 0xE2, 0x96, 0x88, SCREEN_OP_COLOR, 0x70, SCREEN_OP_ATTR, SCREEN_A_UNDERLINE,
        SCREEN_OP_TEXT, 1, ' ', SCREEN_OP_ATTR, 0, SCREEN_OP_TEXT, 3, 0xE2, 0x94, 0x80);   /* █, underlined blank, ─ */
    static uint16_t px[4 * 6 * 12];
    screen_render_row(&g, NULL, 0, px);
    /* █ in BrightWhite: every pixel of cell 0 */
    for (int y = 0; y < 12; y++)
        for (int xx = 0; xx < 6; xx++) CHECK_EQ(px[y * 24 + xx], screen_rgb565[15]);
    /* the underlined blank: bottom row White, the rest Black */
    CHECK_EQ(px[11 * 24 + 6], screen_rgb565[7]);
    CHECK_EQ(px[5 * 24 + 6], screen_rgb565[0]);
    /* ─: the middle row only */
    CHECK_EQ(px[5 * 24 + 12], screen_rgb565[7]);
    CHECK_EQ(px[4 * 24 + 12], screen_rgb565[0]);
    /* the last cell is blank; with the cursor on it, reversed */
    CHECK_EQ(px[0 * 24 + 18], screen_rgb565[0]);
    RUN(SCREEN_OP_CURSOR, 1);
    screen_render_row(&g, NULL, 0, px);
    CHECK_EQ(px[0 * 24 + 18], screen_rgb565[7]);
    /* a glyph the (absent) font lacks: a box outline in the foreground */
    RUN(SCREEN_OP_CURSOR, 0, SCREEN_OP_MOVE, 0, 0, SCREEN_OP_TEXT, 1, 'A');
    screen_render_row(&g, NULL, 0, px);
    CHECK_EQ(px[1 * 24 + 0], screen_rgb565[7]);
    CHECK_EQ(px[5 * 24 + 2], screen_rgb565[0]);
}

/* ---- the port, on a VM ---- */

static int presents, opens, closes, busy;
static uint32_t last_dirty;
static int s_open(uint8_t c, uint8_t r) { (void)c; (void)r; opens++; return busy ? -1 : 0; }
static void s_present(const ScreenGrid *gr, uint32_t d) { (void)gr; presents++; last_dirty = d; }
static void s_close(void) { closes++; }
static const ScreenHw hw = { 8, 3, 20, 5, SCREEN_A_ALL, s_open, s_present, s_close };
static int t_open(struct Vm *vm, Port *p, uint16_t i, const int32_t *cfg, uint32_t n) {
    (void)i;
    return screen_port_open(vm, p, cfg, n, &hw);
}
static const PortDriver drv = SCREEN_PORT_DRIVER(t_open);
static const PortDriver *const drivers[] = { &drv, NULL };

static bool f_pending(const struct Vm *v, void *c) { (void)v; (void)c; return false; }
static void f_drain(struct Vm *v, void *c, uint64_t now) { (void)v; (void)c; (void)now; }
static bool f_live(const struct Vm *v, void *c) { (void)v; (void)c; return false; }
static const PortSource filler = { f_pending, f_drain, f_live, NULL, NULL };

static Vm vm;
static Actor *owner;

static void vm_setup(void) {
    presents = opens = closes = busy = 0;
    test_host_arena_forget();
    va_reset(&vm);
    bytes_reset(&vm);
    CHECK_EQ(test_vm_init(&vm, NULL, 16), 0);
    port_set_drivers(&vm, drivers);
    owner = actor_new(&vm, 256);
    CHECK(owner != NULL);
    owner->state = A_WAITING;
}

static uint32_t halloc(uint32_t words) {
    uint32_t off = heap_alloc(&vm, owner, words, NULL, 0);
    CHECK(off != UINT32_MAX);
    return off;
}

/* A heap struct of `type` with int fields v[0..n) on owner's heap. */
static term mk_struct(uint32_t type, const int32_t *v, uint32_t n) {
    uint32_t off = halloc(2u + n);
    owner->mem[off] = hdr_make(2u + n, K_STRUCT);
    owner->mem[off + 1] = type;
    for (uint32_t i = 0; i < n; i++) owner->mem[off + 2 + i] = term_from_int(v[i]);
    return term_from_ref(off);
}

static term name(const char *s) {
    uint32_t len = (uint32_t)strlen(s), w = 2u + (len + 3u) / 4u;
    uint32_t off = halloc(w);
    owner->mem[off] = hdr_make(w, K_STRING);
    owner->mem[off + 1] = len;
    owner->mem[off + w - 1] = 0;
    memcpy(owner->mem + off + 2, s, len);
    return term_from_ref(off);
}

static term open_screen(int32_t cols, int32_t rows) {
    int32_t cfg[2] = { cols, rows };
    owner->hp = 0;
    term nm = name("screen");
    return port_open(&vm, owner, nm, mk_struct(TYPE_SCREEN_CONFIG, cfg, 2));
}

/* Out{Data} with a heap K_BYTES of d[0..n). */
static term mk_out(const uint8_t *d, uint32_t n) {
    owner->hp = 0;
    uint32_t bw = 2u + (n + 3u) / 4u;
    uint32_t b = halloc(bw);
    owner->mem[b] = hdr_make(bw, K_BYTES);
    owner->mem[b + 1] = n;
    owner->mem[b + bw - 1] = 0;
    memcpy(owner->mem + b + 2, d, n);
    uint32_t o = halloc(3);
    owner->mem[o] = hdr_make(3, K_STRUCT);
    owner->mem[o + 1] = TYPE_OUT;
    owner->mem[o + 2] = term_from_ref(b);
    return term_from_ref(o);
}

/* Pops the owner's next message: its type, first int field and (a Chunk)
 * its data. */
static bool pop(uint32_t *type, int32_t *f0, const uint8_t **data, uint32_t *len) {
    Msg *m = owner->mbox_head;
    if (!m) return false;
    owner->mbox_head = m->next;
    if (!owner->mbox_head) owner->mbox_tail = NULL;
    owner->mbox_words = (uint16_t)(owner->mbox_words - msg_charge_words(&vm, m));
    const uint32_t *o = m->data + term_ref_off(m->root);
    *type = o[1];
    *f0 = term_is_int(o[2]) ? term_to_int(o[2]) : 0;
    if (data && *type == TYPE_CHUNK) {
        static uint8_t copy[16];
        const uint32_t *b = m->data + term_ref_off(o[5]);
        *len = b[1];
        memcpy(copy, b + 2, *len < sizeof copy ? *len : sizeof copy);
        *data = copy;
    }
    msg_free(&vm, m);
    return true;
}

TEST(screen_port_open_info) {
    vm_setup();
    term p = open_screen(0, 0);
    CHECK(p != T_NIL);
    CHECK_EQ(opens, 1);
    CHECK_EQ(presents, 1);   /* the blank grid, at once */
    CHECK_EQ(last_dirty, 7u);
    uint32_t type, len = 0;
    int32_t f0;
    const uint8_t *d = NULL;
    CHECK(pop(&type, &f0, &d, &len));
    CHECK_EQ(type, TYPE_CHUNK);
    CHECK_EQ(len, 4u);
    CHECK(d[0] == 8 && d[1] == 3 && d[2] == 16 && d[3] == 7);
    const ScreenGrid *gr = screen_port_grid(&vm.ports[0]);
    CHECK(gr && gr->cols == 8 && gr->rows == 3);
    /* the grid is charged to the owner */
    CHECK(vm.ports[0].charge_words >= 8 * 3);
    /* one screen per Vm */
    CHECK_EQ(open_screen(0, 0), T_NIL);
    port_close(&vm, p);
    CHECK_EQ(closes, 1);
    CHECK(screen_port_grid(&vm.ports[0]) == NULL);
    /* sizes: up to the hook's maximum */
    p = open_screen(20, 5);
    CHECK(p != T_NIL);
    port_close(&vm, p);
    CHECK_EQ(open_screen(21, 5), T_NIL);
    CHECK_EQ(open_screen(20, 6), T_NIL);
    CHECK_EQ(open_screen(-1, 2), T_NIL);
    CHECK_EQ(open_screen(0, 2), T_NIL);
    busy = 1;   /* the display is another Vm's */
    CHECK_EQ(open_screen(0, 0), T_NIL);
    busy = 0;
    /* no free event-source slot (for the Done retry): refused */
    int filled = 0;
    while (filled < 8 && port_add_source(&vm, &filler, NULL)) filled++;
    int o = opens;
    CHECK_EQ(open_screen(0, 0), T_NIL);
    CHECK_EQ(opens, o);   /* refused before taking the display */
    port_remove_source(&vm, &filler);
    p = open_screen(0, 0);
    CHECK(p != T_NIL);
    port_close(&vm, p);
}

TEST(screen_port_out_done) {
    vm_setup();
    term p = open_screen(0, 0);
    uint32_t type;
    int32_t seq;
    CHECK(pop(&type, &seq, NULL, NULL));   /* the info Chunk */
    static const uint8_t f1[] = { SCREEN_OP_MOVE, 1, 1, SCREEN_OP_TEXT, 2, 'o', 'k' };
    port_command(&vm, owner, p, mk_out(f1, sizeof f1));
    CHECK(pop(&type, &seq, NULL, NULL));
    CHECK_EQ(type, TYPE_DONE);
    CHECK_EQ(seq, 1);
    CHECK_EQ(presents, 2);
    CHECK_EQ(last_dirty, 2u);
    const ScreenGrid *gr = screen_port_grid(&vm.ports[0]);
    CHECK_EQ(screen_cell_cp(screen_cell(gr, 2, 1)), (uint32_t)'k');
    /* malformed: Done{-1}, nothing drawn, no Seq used */
    static const uint8_t bad[] = { SCREEN_OP_TEXT, 1, 'X', 42 };
    port_command(&vm, owner, p, mk_out(bad, sizeof bad));
    CHECK(pop(&type, &seq, NULL, NULL));
    CHECK_EQ(seq, -1);
    CHECK_EQ(presents, 2);
    CHECK_EQ(screen_cell_cp(screen_cell(gr, 0, 0)), (uint32_t)' ');
    /* a frame that changes nothing is not presented, but is answered */
    static const uint8_t same[] = { SCREEN_OP_MOVE, 0, 0 };
    port_command(&vm, owner, p, mk_out(same, sizeof same));
    CHECK(pop(&type, &seq, NULL, NULL));
    CHECK_EQ(seq, 2);
    CHECK_EQ(presents, 2);
    /* other commands are ignored */
    int32_t one = 1;
    port_command(&vm, owner, p, mk_struct(TYPE_WRITE, &one, 1));
    CHECK(!pop(&type, &seq, NULL, NULL));
    port_close(&vm, p);
}

static uint64_t clk;
static uint64_t clock_fn(void) { return clk; }

/* A full mailbox: Dones are owed, then delivered by the port's own retry
 * (no further Out needed), in order. */
TEST(screen_port_owed) {
    clk = 1000;
    test_host_set_clock(clock_fn);
    vm_setup();
    term p = open_screen(0, 0);
    uint32_t type;
    int32_t seq;
    CHECK(pop(&type, &seq, NULL, NULL));
    owner->max_words = (uint16_t)(owner->mem_words + vm.ports[0].charge_words);   /* no room for a message */
    static const uint8_t f[] = { SCREEN_OP_TEXT, 1, 'a' };
    static const uint8_t bad[] = { 99 };
    port_command(&vm, owner, p, mk_out(f, sizeof f));
    port_command(&vm, owner, p, mk_out(bad, sizeof bad));
    port_command(&vm, owner, p, mk_out(f, sizeof f));
    CHECK(!pop(&type, &seq, NULL, NULL));
    CHECK(port_any_live(&vm));                 /* Dones still to come: the VM keeps running */
    uint64_t dl = 0;
    CHECK(port_next_deadline(&vm, &dl));
    port_drain(&vm, clk);                       /* still full: nothing */
    CHECK(!pop(&type, &seq, NULL, NULL));
    owner->max_words = 0;                       /* the owner has room again */
    clk = dl;
    CHECK(port_due_now(&vm));
    port_drain(&vm, clk);
    static const int32_t want[] = { 1, -1, 2 };
    for (int i = 0; i < 3; i++) {
        CHECK(pop(&type, &seq, NULL, NULL));
        CHECK_EQ(type, TYPE_DONE);
        CHECK_EQ(seq, want[i]);
    }
    CHECK(!port_any_live(&vm));
    CHECK(!port_next_deadline(&vm, &dl));
    port_close(&vm, p);
    test_host_set_clock(NULL);
}

/* SCREEN_OWED_MAX owed: the next Outs are not drawn and get Done{-1}, owed
 * after the others, so every Out still gets its Done. */
TEST(screen_port_owed_full) {
    clk = 1000;
    test_host_set_clock(clock_fn);
    vm_setup();
    term p = open_screen(0, 0);
    uint32_t type;
    int32_t seq;
    CHECK(pop(&type, &seq, NULL, NULL));
    owner->max_words = (uint16_t)(owner->mem_words + vm.ports[0].charge_words);
    static const uint8_t f[] = { SCREEN_OP_MOVE, 0, 0, SCREEN_OP_TEXT, 1, 'a' };
    static const uint8_t g2[] = { SCREEN_OP_MOVE, 0, 0, SCREEN_OP_TEXT, 1, 'b' };
    for (unsigned i = 0; i < SCREEN_OWED_MAX; i++) port_command(&vm, owner, p, mk_out(f, sizeof f));
    port_command(&vm, owner, p, mk_out(g2, sizeof g2));   /* refused: 'b' not drawn */
    port_command(&vm, owner, p, mk_out(g2, sizeof g2));
    CHECK_EQ(screen_cell_cp(screen_cell(screen_port_grid(&vm.ports[0]), 0, 0)), (uint32_t)'a');
    owner->max_words = 0;
    clk += 100;
    port_drain(&vm, clk);
    for (unsigned i = 0; i < SCREEN_OWED_MAX + 2u; i++) {
        CHECK(pop(&type, &seq, NULL, NULL));
        CHECK_EQ(seq, i < SCREEN_OWED_MAX ? (int32_t)i + 1 : -1);
    }
    CHECK(!pop(&type, &seq, NULL, NULL));
    port_close(&vm, p);
    test_host_set_clock(NULL);
}
