/* Host-only: render a text file through the console grid and font into a
 * 240x135 PPM, for checking glyphs and layout without the board.
 *
 *   console_ppm gogo12.font in.txt out.ppm
 *
 * Lines starting with '!' are written in red (like stderr), others white.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "console.h"

#define SCREEN_H 135

static uint8_t *slurp(const char *path, size_t *n) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        perror(path);
        exit(1);
    }
    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)len + 1);
    if (!buf || fread(buf, 1, (size_t)len, fp) != (size_t)len) {
        fprintf(stderr, "cannot read %s\n", path);
        exit(1);
    }
    fclose(fp);
    *n = (size_t)len;
    return buf;
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s gogo12.font in.txt out.ppm\n", argv[0]);
        return 2;
    }
    size_t fn, tn;
    uint8_t *fb = slurp(argv[1], &fn);
    uint8_t *tb = slurp(argv[2], &tn);
    static Font font;
    if (!font_init(&font, fb, fn)) {
        fprintf(stderr, "%s: not a gogo font blob\n", argv[1]);
        return 1;
    }

    static Console con;
    console_init(&con);
    size_t i = 0;
    while (i < tn) {
        size_t j = i;
        while (j < tn && tb[j] != '\n') j++;
        uint8_t color = CON_WHITE;
        if (j > i && tb[i] == '!') {
            color = CON_RED;
            i++;
        }
        console_write(&con, (const char *)tb + i, j - i, color);
        if (j < tn) console_write(&con, "\n", 1, CON_WHITE);
        i = j + 1;
    }

    FILE *out = fopen(argv[3], "wb");
    if (!out) {
        perror(argv[3]);
        return 1;
    }
    fprintf(out, "P6\n%d %d\n255\n", CONSOLE_PX_W, SCREEN_H);
    static uint16_t row[CONSOLE_PX_W * CONSOLE_CELL_H];
    for (int r = 0; r < CONSOLE_ROWS; r++) {
        console_render_row(&con, &font, r, row);
        for (int k = 0; k < CONSOLE_PX_W * CONSOLE_CELL_H; k++) {
            uint16_t p = row[k];
            uint8_t rgb[3] = {(uint8_t)(((p >> 11) & 31) * 255 / 31),
                              (uint8_t)(((p >> 5) & 63) * 255 / 63),
                              (uint8_t)((p & 31) * 255 / 31)};
            fwrite(rgb, 1, 3, out);
        }
    }
    for (int k = 0; k < CONSOLE_PX_W * (SCREEN_H - CONSOLE_ROWS * CONSOLE_CELL_H); k++)
        fputc(0, out), fputc(0, out), fputc(0, out);
    fclose(out);
    return 0;
}
