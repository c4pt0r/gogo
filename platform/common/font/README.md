# gogo12.font

12 px bitmap font for the on-screen debug console (`platform/common/console.c`):
printable ASCII (6x12 cells) plus the GB 2312 characters (12x12 cells).

## Source and license

[Fusion Pixel Font](https://github.com/TakWolf/fusion-pixel-font) 12px
monospaced, `zh_hans` variant, release **2026.11.10**, SIL OFL 1.1 (`OFL.txt`).

- Download: `https://github.com/TakWolf/fusion-pixel-font/releases/download/2026.11.10/fusion-pixel-font-12px-monospaced-bdf-v2026.11.10.zip`
  (sha256 `bbf8efa39f165dee5e52ed1c2ccc21f7d7d32dd6e536190cba55065220b2c0a5`)
- The BDF is not committed. Unzip it somewhere outside the repo and run, from
  the repo root:

      go run ./cmd/gogo-font -o platform/common/font/gogo12.font fusion-pixel-12px-monospaced-zh_hans.bdf

The ASCII-only font for `GOGO_FONT_CJK=n` (`gogo12-ascii.font`, 95 half-width
glyphs, 1536 bytes, same blob format) comes from the same BDF:

      go run ./cmd/gogo-font -ascii -o platform/common/font/gogo12-ascii.font fusion-pixel-12px-monospaced-zh_hans.bdf

`font_init` / `font_glyph` do not care which font they get; a code point it
lacks (every CJK one) is drawn as the console's box. The host test
`make test-console` runs against both blobs.

Why Fusion Pixel: its GB 2312 coverage is 7378 of 7445 code points (99.10%;
6727 of 6763 hanzi, 99.47%), over the 99% bar, so Ark Pixel was not needed.
Missing: 36 rare hanzi and 31 math symbols (for example U+2264 and U+2260);
they render as the console's box glyph.

The GB 2312 code point list lives in `cmd/gogo-font/gb2312.go`, generated once
by `cmd/gogo-font/gen_gb2312.py` from Python's `gb2312` codec (the tool itself
uses only the Go standard library).

Result: 244 half-width + 7229 full-width glyphs, 162958 bytes.

## Blob format (version 1, little-endian)

    offset  size
    0       4     magic "GGFN"
    4       1     version = 1
    5       1     glyph height = 12
    6       2     reserved = 0
    8       4     n_half
    12      4     n_wide
    16      4*n_half   half-width code points, ascending (u32)
            4*n_wide   full-width code points, ascending (u32)
            12*n_half  half-width bitmaps
            18*n_wide  full-width bitmaps

A half-width (6 px) bitmap is 12 bytes, one per row, top row first, bit 7 =
leftmost pixel (bits 1..0 unused). A full-width (12 px) bitmap is the 12x12
pixels packed row-major, MSB first, into 18 bytes (pixel index y*12+x). Glyphs
are positioned on a 12-row cell with the baseline at row 10 (the BDF
FONT_ASCENT). Whether a glyph is half or full width is its BDF advance width.

`font_init` checks magic, version, height and that the size is exactly what the
counts imply; `font_glyph` binary-searches the half table, then the full table.

## Console column rule

The console cannot ask the font while buffering text, so
`console_cp_cols` (console.c) decides 1 or 2 columns from the code point alone:
narrow for ASCII, U+00A0..U+02FF (except U+00B7), Greek, Cyrillic, U+2032/2033
and the box; wide for everything else. The host test `font_console_agree`
verifies this against every glyph in the font, so regenerating the font from a
different BDF must keep that test green.

## Visual check

    make console-ppm TEXT=platform/common/font/sample.txt OUT=build/sample.ppm
    sips -s format png build/sample.ppm --out build/sample.png   # macOS

Lines starting with `!` in the text file are drawn red (stderr style).
