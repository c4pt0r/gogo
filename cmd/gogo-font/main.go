// Command gogo-font converts a 12 px BDF bitmap font into the compact blob
// runtime/console/font.c reads.
//
//	gogo-font [-ascii] -o gogo12.font in.bdf
//
// It keeps printable ASCII (U+0020..U+007E) plus the GB 2312 characters the
// BDF has glyphs for; with -ascii, printable ASCII only (the GOGO_FONT_CJK=n
// firmware font). Glyphs 6 px wide (DWIDTH 6) go in the half-width
// table, 12 px wide ones in the full-width table. The blob format is
// documented in runtime/console/font/README.md.
package main

import (
	"bufio"
	"encoding/binary"
	"errors"
	"flag"
	"fmt"
	"io"
	"os"
	"sort"
	"strconv"
	"strings"
)

const (
	height    = 12
	halfW     = 6
	wideW     = 12
	halfBytes = 12
	wideBytes = 18
	version   = 1
)

type stats struct {
	Half, Wide int
	GBTotal    int // GB 2312 code points
	GBCovered  int // of which the BDF has a glyph for
}

type glyph struct {
	cp uint32
	bm []byte
}

func main() {
	if err := run(os.Args[1:]); err != nil {
		fmt.Fprintln(os.Stderr, "gogo-font:", err)
		os.Exit(1)
	}
}

func run(args []string) error {
	fs := flag.NewFlagSet("gogo-font", flag.ContinueOnError)
	out := fs.String("o", "", "output font blob (required)")
	ascii := fs.Bool("ascii", false, "keep printable ASCII only (no GB 2312)")
	if err := fs.Parse(args); err != nil {
		return err
	}
	if *out == "" || fs.NArg() != 1 {
		return errors.New("usage: gogo-font [-ascii] -o gogo12.font in.bdf")
	}
	in, err := os.Open(fs.Arg(0))
	if err != nil {
		return err
	}
	defer in.Close()
	blob, st, err := convert(in, *ascii)
	if err != nil {
		return fmt.Errorf("%s: %w", fs.Arg(0), err)
	}
	if err := os.WriteFile(*out, blob, 0o644); err != nil {
		return err
	}
	fmt.Fprintf(os.Stderr, "gogo-font: %d half + %d wide glyphs, %d bytes; GB2312 coverage %d/%d (%.2f%%)\n",
		st.Half, st.Wide, len(blob), st.GBCovered, st.GBTotal,
		100*float64(st.GBCovered)/float64(st.GBTotal))
	return nil
}

// gb2312Set returns the GB 2312 code points as a set.
func gb2312Set() map[rune]bool {
	set := make(map[rune]bool, 8000)
	for _, r := range gb2312Chars {
		set[r] = true
	}
	return set
}

func convert(r io.Reader, asciiOnly bool) ([]byte, stats, error) {
	var st stats
	gb := gb2312Set()
	st.GBTotal = len(gb)

	sc := bufio.NewScanner(r)
	sc.Buffer(make([]byte, 1<<16), 1<<20)

	pixelSize, fbbH, ascent := -1, -1, -1
	var half, wide []glyph
	seen := map[uint32]bool{}
	lineNo := 0
	fail := func(f string, a ...any) ([]byte, stats, error) {
		return nil, st, fmt.Errorf("line %d: %s", lineNo, fmt.Sprintf(f, a...))
	}

	// per-glyph state
	var (
		inGlyph       bool
		enc           int
		dw, bw, bh    int
		bx, by        int
		rows          []string
		inBitmap      bool
		haveDW, haveB bool
	)
	checked := false
	check := func() error {
		if checked {
			return nil
		}
		checked = true
		if pixelSize != -1 && pixelSize != height {
			return fmt.Errorf("font is %d px (PIXEL_SIZE), need 12", pixelSize)
		}
		if fbbH != -1 && fbbH != height {
			return fmt.Errorf("font bounding box is %d px high, need 12", fbbH)
		}
		if pixelSize == -1 && fbbH == -1 {
			return errors.New("cannot tell the font height (no PIXEL_SIZE or FONTBOUNDINGBOX), need 12 px")
		}
		if ascent == -1 {
			return errors.New("no FONT_ASCENT")
		}
		return nil
	}

	for sc.Scan() {
		lineNo++
		line := strings.TrimSpace(sc.Text())
		f := strings.Fields(line)
		if len(f) == 0 {
			continue
		}
		if inBitmap {
			if f[0] == "ENDCHAR" {
				inBitmap = false
				inGlyph = false
				if err := check(); err != nil {
					return nil, st, err
				}
				if enc < 0 || !haveDW || !haveB {
					continue
				}
				cp := rune(enc)
				if !(cp >= 0x20 && cp <= 0x7E) && (asciiOnly || !gb[cp]) {
					continue
				}
				if seen[uint32(cp)] {
					return fail("duplicate glyph U+%04X", cp)
				}
				seen[uint32(cp)] = true
				if len(rows) != bh {
					return fail("U+%04X: %d bitmap rows, BBX says %d", cp, len(rows), bh)
				}
				var cw int
				switch dw {
				case halfW:
					cw = halfW
				case wideW:
					cw = wideW
				default:
					return fail("U+%04X: advance width %d, need 6 or 12", cp, dw)
				}
				bm, err := pack(rows, bw, bh, bx, by, ascent, cw)
				if err != nil {
					return fail("U+%04X: %v", cp, err)
				}
				if cw == halfW {
					half = append(half, glyph{uint32(cp), bm})
				} else {
					wide = append(wide, glyph{uint32(cp), bm})
				}
				continue
			}
			rows = append(rows, f[0])
			continue
		}
		switch f[0] {
		case "PIXEL_SIZE":
			pixelSize = atoi(f, 1)
		case "FONT_ASCENT":
			ascent = atoi(f, 1)
		case "FONTBOUNDINGBOX":
			if len(f) == 5 {
				fbbH = atoi(f, 2)
			}
		case "STARTCHAR":
			inGlyph = true
			enc, dw, bw, bh, bx, by = -1, 0, 0, 0, 0, 0
			haveDW, haveB = false, false
			rows = rows[:0]
		case "ENCODING":
			if inGlyph {
				enc = atoi(f, 1)
			}
		case "DWIDTH":
			if inGlyph {
				dw, haveDW = atoi(f, 1), true
			}
		case "BBX":
			if inGlyph && len(f) == 5 {
				bw, bh, bx, by, haveB = atoi(f, 1), atoi(f, 2), atoi(f, 3), atoi(f, 4), true
			}
		case "BITMAP":
			if inGlyph {
				inBitmap = true
			}
		}
	}
	if err := sc.Err(); err != nil {
		return nil, st, err
	}
	if err := check(); err != nil {
		return nil, st, err
	}
	for r := range gb {
		if seen[uint32(r)] {
			st.GBCovered++
		}
	}
	st.Half, st.Wide = len(half), len(wide)

	sort.Slice(half, func(i, j int) bool { return half[i].cp < half[j].cp })
	sort.Slice(wide, func(i, j int) bool { return wide[i].cp < wide[j].cp })

	blob := []byte("GGFN")
	blob = append(blob, version, height, 0, 0)
	blob = binary.LittleEndian.AppendUint32(blob, uint32(len(half)))
	blob = binary.LittleEndian.AppendUint32(blob, uint32(len(wide)))
	for _, g := range half {
		blob = binary.LittleEndian.AppendUint32(blob, g.cp)
	}
	for _, g := range wide {
		blob = binary.LittleEndian.AppendUint32(blob, g.cp)
	}
	for _, g := range half {
		blob = append(blob, g.bm...)
	}
	for _, g := range wide {
		blob = append(blob, g.bm...)
	}
	return blob, st, nil
}

func atoi(f []string, i int) int {
	if i >= len(f) {
		return -1
	}
	n, err := strconv.Atoi(f[i])
	if err != nil {
		return -1
	}
	return n
}

// pack renders a BDF glyph into the fixed 12-row cell: half-width cells are
// 12 bytes (one per row, bit 7 leftmost), full-width cells 18 bytes (12x12
// bits row-major, MSB first). ascent is the baseline row from the cell top.
func pack(rows []string, bw, bh, bx, by, ascent, cw int) ([]byte, error) {
	n := halfBytes
	if cw == wideW {
		n = wideBytes
	}
	out := make([]byte, n)
	top := ascent - (by + bh)
	perRow := (bw + 7) / 8
	for i, hex := range rows {
		if len(hex) != 2*perRow {
			return nil, fmt.Errorf("bitmap row %q has the wrong length for width %d", hex, bw)
		}
		v, err := strconv.ParseUint(hex, 16, 64)
		if err != nil {
			return nil, err
		}
		for x := 0; x < bw; x++ {
			if v>>(uint(perRow*8-1-x))&1 == 0 {
				continue
			}
			px, py := bx+x, top+i
			if px < 0 || px >= cw || py < 0 || py >= height {
				return nil, fmt.Errorf("pixel (%d,%d) outside the %dx%d cell", px, py, cw, height)
			}
			if cw == halfW {
				out[py] |= 0x80 >> uint(px)
			} else {
				bit := py*wideW + px
				out[bit>>3] |= 0x80 >> uint(bit&7)
			}
		}
	}
	return out, nil
}
