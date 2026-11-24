/* Host tests for the console text grid and the font lookup. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "console.h"
#include "font.h"
#include "test.h"

#ifndef FONT_PATH
#define FONT_PATH "platform/common/font/gogo12.font"
#endif

/* test_main.c (shared with the VM tests) calls this; nothing to forget here. */
void platform_unix_arena_forget(void) {}

static uint8_t *blob;
static size_t blob_n;
static Font font;

static void load_font(void) {
    if (blob) return;
    FILE *fp = fopen(FONT_PATH, "rb");
    if (!fp) {
        printf("cannot open %s (run from the repo root)\n", FONT_PATH);
        exit(2);
    }
    fseek(fp, 0, SEEK_END);
    blob_n = (size_t)ftell(fp);
    fseek(fp, 0, SEEK_SET);
    blob = malloc(blob_n);
    if (!blob || fread(blob, 1, blob_n, fp) != blob_n) exit(2);
    fclose(fp);
    if (!font_init(&font, blob, blob_n)) {
        printf("bad font blob %s\n", FONT_PATH);
        exit(2);
    }
}

static void put(Console *c, const char *s, uint8_t color) {
    console_write(c, s, strlen(s), color);
}

/* Number of non-blank cells (cp != 0 or continuation) in row r. */
static int row_cells(const Console *c, int r) {
    int n = 0;
    for (int i = 0; i < CONSOLE_COLS; i++)
        if (console_cell_cp(c, r, i) != 0 || console_cell_cont(c, r, i)) n++;
    return n;
}

static void fresh(Console *c) {
    console_init(c);
    (void)console_take_dirty(c);
}

TEST(con_ascii_wrap) {
    Console c;
    fresh(&c);
    put(&c, "0123456789012345678901234567890123456789ABCDE", CON_WHITE);
    CHECK_EQ(row_cells(&c, 0), 40);
    CHECK_EQ(row_cells(&c, 1), 5);
    CHECK_EQ(console_cell_cp(&c, 0, 39), '9');
    CHECK_EQ(console_cell_cp(&c, 1, 0), 'A');
    CHECK_EQ(console_cell_cp(&c, 1, 4), 'E');
    CHECK_EQ(console_take_dirty(&c), 3);
}

TEST(con_exact_line_then_newline) {
    Console c;
    fresh(&c);
    put(&c, "0123456789012345678901234567890123456789\nX", CON_WHITE);
    CHECK_EQ(row_cells(&c, 0), 40);
    CHECK_EQ(console_cell_cp(&c, 1, 0), 'X'); /* no blank line in between */
    CHECK_EQ(row_cells(&c, 2), 0);
}

TEST(con_utf8_split) {
    Console a, b;
    fresh(&a);
    fresh(&b);
    const char *s = "a\xe4\xb8\xad\xe6\x96\x87z";
    console_write(&a, s, strlen(s), CON_WHITE);
    for (size_t i = 0; i < strlen(s); i++) console_write(&b, s + i, 1, CON_WHITE);
    CHECK(memcmp(&a, &b, sizeof a) == 0);
    CHECK_EQ(console_cell_cp(&a, 0, 0), 'a');
    CHECK_EQ(console_cell_cp(&a, 0, 1), 0x4E2D);
    CHECK(console_cell_cont(&a, 0, 2));
    CHECK_EQ(console_cell_cp(&a, 0, 3), 0x6587);
    CHECK(console_cell_cont(&a, 0, 4));
    CHECK_EQ(console_cell_cp(&a, 0, 5), 'z');
}

TEST(con_invalid) {
    Console c;
    fresh(&c);
    put(&c, "\xff", CON_WHITE);
    CHECK_EQ(console_cell_cp(&c, 0, 0), CONSOLE_BOX);
    CHECK_EQ(row_cells(&c, 0), 1);

    /* truncated sequence followed by ASCII: one box, then the ASCII byte */
    fresh(&c);
    put(&c, "\xe4\xb8z", CON_WHITE);
    CHECK_EQ(console_cell_cp(&c, 0, 0), CONSOLE_BOX);
    CHECK_EQ(console_cell_cp(&c, 0, 1), 'z');
    CHECK_EQ(row_cells(&c, 0), 2);

    /* stray continuation, overlong NUL-ish (C0 80), surrogate (ED A0 80) */
    fresh(&c);
    put(&c, "\x80", CON_WHITE);
    CHECK_EQ(console_cell_cp(&c, 0, 0), CONSOLE_BOX);
    fresh(&c);
    put(&c, "\xc0\x80", CON_WHITE);
    CHECK_EQ(console_cell_cp(&c, 0, 0), CONSOLE_BOX);
    fresh(&c);
    put(&c, "\xed\xa0\x80", CON_WHITE);
    CHECK_EQ(console_cell_cp(&c, 0, 0), CONSOLE_BOX);
    CHECK_EQ(console_cell_cp(&c, 0, 1), 0);
}

TEST(con_wide_at_edge) {
    Console c;
    fresh(&c);
    put(&c, "012345678901234567890123456789012345678", CON_WHITE); /* 39 */
    put(&c, "\xe4\xb8\xad", CON_WHITE);                            /* U+4E2D */
    CHECK_EQ(row_cells(&c, 0), 39);
    CHECK_EQ(console_cell_cp(&c, 1, 0), 0x4E2D);
    CHECK(console_cell_cont(&c, 1, 1));
}

TEST(con_wide_fills_line) {
    Console c;
    fresh(&c);
    for (int i = 0; i < 20; i++) put(&c, "\xe4\xb8\xad", CON_WHITE);
    CHECK_EQ(row_cells(&c, 0), 40);
    CHECK_EQ(row_cells(&c, 1), 0);
    put(&c, "\xe4\xb8\xad", CON_WHITE);
    CHECK_EQ(console_cell_cp(&c, 1, 0), 0x4E2D);
}

TEST(con_scroll) {
    Console c;
    fresh(&c);
    for (int i = 0; i < 12; i++) {
        char line[16];
        snprintf(line, sizeof line, "line%02d\n", i);
        put(&c, line, CON_WHITE);
    }
    /* 12 lines in 11 rows: line00 is gone, line01 is on top */
    CHECK_EQ(console_cell_cp(&c, 0, 4), '0');
    CHECK_EQ(console_cell_cp(&c, 0, 5), '1');
    CHECK_EQ(console_cell_cp(&c, 10, 4), '1');
    CHECK_EQ(console_cell_cp(&c, 10, 5), '1');
    CHECK_EQ(console_take_dirty(&c), (1 << CONSOLE_ROWS) - 1);
    CHECK_EQ(console_take_dirty(&c), 0);
    CHECK_EQ(row_cells(&c, 10), 6);
}

TEST(con_scroll_by_wrap) {
    Console c;
    fresh(&c);
    for (int i = 0; i < 10; i++) put(&c, "\n", CON_WHITE);
    put(&c, "0123456789012345678901234567890123456789", CON_WHITE);
    (void)console_take_dirty(&c);
    put(&c, "Q", CON_WHITE); /* wraps below the last row: scroll */
    CHECK_EQ(console_cell_cp(&c, 10, 0), 'Q');
    CHECK_EQ(console_cell_cp(&c, 9, 39), '9');
    CHECK_EQ(console_take_dirty(&c), (1 << CONSOLE_ROWS) - 1);
}

TEST(con_dirty_rows) {
    Console c;
    fresh(&c);
    put(&c, "a\nb", CON_WHITE);
    CHECK_EQ(console_take_dirty(&c), 3);
    CHECK_EQ(console_take_dirty(&c), 0);
    put(&c, "c", CON_WHITE);
    CHECK_EQ(console_take_dirty(&c), 2);
    console_init(&c);
    CHECK_EQ(console_take_dirty(&c), (1 << CONSOLE_ROWS) - 1);
}

TEST(con_color) {
    Console c;
    fresh(&c);
    put(&c, "ab", CON_WHITE);
    put(&c, "\xe4\xb8\xad", CON_RED);
    put(&c, "c", CON_RED);
    CHECK_EQ(console_cell_color(&c, 0, 0), CON_WHITE);
    CHECK_EQ(console_cell_color(&c, 0, 2), CON_RED);
    CHECK_EQ(console_cell_color(&c, 0, 3), CON_RED); /* right half of the wide cell */
    CHECK_EQ(console_cell_color(&c, 0, 4), CON_RED);
}

TEST(con_ctrl_chars) {
    Console c;
    fresh(&c);
    put(&c, "a\r\nb\tc", CON_WHITE);
    CHECK_EQ(console_cell_cp(&c, 0, 0), 'a');
    CHECK_EQ(row_cells(&c, 0), 1); /* \r ignored */
    CHECK_EQ(console_cell_cp(&c, 1, 0), 'b');
    CHECK_EQ(console_cell_cp(&c, 1, 8), 'c'); /* tab to column 8 */
}

TEST(con_cols_rule) {
    CHECK_EQ(console_cp_cols('A'), 1);
    CHECK_EQ(console_cp_cols(0x4E2D), 2);
    CHECK_EQ(console_cp_cols(0x3002), 2);
    CHECK_EQ(console_cp_cols(0xFF0C), 2);
    CHECK_EQ(console_cp_cols(0x1F600), 2);
}

TEST(font_lookup) {
    load_font();
    int w = 0;
    CHECK(font_glyph(&font, 'A', &w) != NULL);
    CHECK_EQ(w, 6);
    CHECK(font_glyph(&font, 0x4E2D, &w) != NULL);
    CHECK_EQ(w, 12);
    CHECK(font_glyph(&font, 0x3002, &w) != NULL);
    CHECK_EQ(w, 12);
    CHECK(font_glyph(&font, 0x1F600, &w) == NULL);
    CHECK(font_glyph(&font, 0, &w) == NULL);
    CHECK(font_glyph(&font, CONSOLE_BOX, &w) == NULL);
    /* 'A' is not blank */
    const uint8_t *g = font_glyph(&font, 'A', &w);
    int on = 0;
    for (int y = 0; y < 12; y++)
        for (int x = 0; x < 6; x++) on += font_pixel(g, 6, x, y);
    CHECK(on > 8);
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* The grid decides column count without the font, so it must agree with the
 * width of every glyph in the font. */
TEST(font_console_agree) {
    load_font();
    uint32_t prev = 0;
    for (uint32_t i = 0; i < font.n_half; i++) {
        uint32_t cp = rd32(font.half_cp + 4 * i);
        CHECK(i == 0 || cp > prev);
        prev = cp;
        if (console_cp_cols(cp) != 1) {
            printf("  half glyph U+%04X is %d cols\n", (unsigned)cp, console_cp_cols(cp));
            CHECK(0);
            break;
        }
    }
    prev = 0;
    for (uint32_t i = 0; i < font.n_wide; i++) {
        uint32_t cp = rd32(font.wide_cp + 4 * i);
        CHECK(i == 0 || cp > prev);
        prev = cp;
        if (console_cp_cols(cp) != 2) {
            printf("  wide glyph U+%04X is %d cols\n", (unsigned)cp, console_cp_cols(cp));
            CHECK(0);
            break;
        }
    }
    CHECK(font.n_wide > 6700);
}

TEST(font_init_rejects_garbage) {
    Font f;
    uint8_t junk[16] = {0};
    CHECK(!font_init(&f, junk, sizeof junk));
    CHECK(!font_init(&f, junk, 3));
    load_font();
    CHECK(!font_init(&f, blob, blob_n - 1)); /* truncated */
}

#define WHITE 0xFFFF
#define RED 0xF800

static int count_px(const uint16_t *px, int x0, int x1, uint16_t color) {
    int n = 0;
    for (int y = 0; y < 12; y++)
        for (int x = x0; x < x1; x++)
            if (px[y * CONSOLE_PX_W + x] == color) n++;
    return n;
}

TEST(con_render) {
    load_font();
    static uint16_t px[CONSOLE_PX_W * 12];
    Console c;
    fresh(&c);
    put(&c, "A", CON_WHITE);
    put(&c, "\xe4\xb8\xad", CON_RED);
    put(&c, "\xff", CON_WHITE);
    console_render_row(&c, &font, 0, px);
    CHECK(count_px(px, 0, 6, WHITE) > 8);   /* 'A' */
    CHECK_EQ(count_px(px, 0, 6, RED), 0);
    CHECK(count_px(px, 6, 18, RED) > 20);   /* red wide glyph */
    CHECK_EQ(count_px(px, 6, 18, WHITE), 0);
    CHECK(count_px(px, 18, 24, WHITE) > 8); /* box for the invalid byte */
    CHECK_EQ(count_px(px, 24, 240, WHITE) + count_px(px, 24, 240, RED), 0);
    /* an empty row is all background */
    console_render_row(&c, &font, 5, px);
    CHECK_EQ(count_px(px, 0, 240, WHITE) + count_px(px, 0, 240, RED), 0);
}

TEST(con_render_missing_glyph_is_box) {
    load_font();
    static uint16_t a[CONSOLE_PX_W * 12], b[CONSOLE_PX_W * 12];
    Console c;
    fresh(&c);
    put(&c, "\xf0\x9f\x98\x80", CON_WHITE); /* U+1F600: not in the font */
    CHECK_EQ(console_cell_cp(&c, 0, 0), 0x1F600);
    CHECK(console_cell_cont(&c, 0, 1));
    console_render_row(&c, &font, 0, a);
    fresh(&c);
    put(&c, "\xef\xbf\xbd", CON_WHITE); /* U+FFFD, wide? no: box */
    console_render_row(&c, &font, 0, b);
    CHECK(count_px(a, 0, 12, WHITE) > 20);
    /* a wide missing glyph is a wider box than the 6 px one */
    CHECK(count_px(a, 0, 12, WHITE) > count_px(b, 0, 12, WHITE));
}
