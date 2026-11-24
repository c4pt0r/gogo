package main

import (
	"bytes"
	"encoding/binary"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestGB2312Table(t *testing.T) {
	set := gb2312Set()
	if len(set) != 7445 {
		t.Fatalf("gb2312 set has %d code points, want 7445", len(set))
	}
	for _, r := range []rune{'中', '。', '，', '　', 'ā', 'α', 'я', '啊', '座'} {
		if !set[r] {
			t.Errorf("U+%04X missing from GB2312 set", r)
		}
	}
	if set['A'] || set['\U0001F600'] {
		t.Error("ASCII letters / emoji must not be in the GB2312 set")
	}
}

func TestConvertTiny(t *testing.T) {
	in, err := os.Open("testdata/tiny.bdf")
	if err != nil {
		t.Fatal(err)
	}
	defer in.Close()
	blob, st, err := convert(in)
	if err != nil {
		t.Fatal(err)
	}
	// kept: A, B (ASCII) and U+3002, U+4E2D (GB2312); dropped: U+0100, emoji
	if st.Half != 2 || st.Wide != 2 {
		t.Fatalf("got %d half %d wide, want 2 and 2", st.Half, st.Wide)
	}
	want := []byte("GGFN")
	want = append(want, 1, 12, 0, 0)
	want = binary.LittleEndian.AppendUint32(want, 2)
	want = binary.LittleEndian.AppendUint32(want, 2)
	for _, cp := range []uint32{0x41, 0x42, 0x3002, 0x4E2D} {
		want = binary.LittleEndian.AppendUint32(want, cp)
	}
	a := make([]byte, 12)
	a[0], a[1] = 0xFC, 0x84
	b := make([]byte, 12)
	b[9] = 0x38 // 0x70 shifted right by x offset 1, on row 10-(0+1)=9
	want = append(want, a...)
	want = append(want, b...)
	period := make([]byte, 18) // rows 8,9: x=0,1 -> bits 96,97 and 108,109
	period[12], period[13] = 0xC0, 0x0C
	zhong := make([]byte, 18) // row 10 all 12 px, row 11 x=0 and x=11
	zhong[15], zhong[16], zhong[17] = 0xFF, 0xF8, 0x01
	want = append(want, period...)
	want = append(want, zhong...)
	if !bytes.Equal(blob, want) {
		t.Fatalf("blob mismatch\n got % x\nwant % x", blob, want)
	}
}

func TestRejectsNot12px(t *testing.T) {
	in, err := os.Open("testdata/tall.bdf")
	if err != nil {
		t.Fatal(err)
	}
	defer in.Close()
	if _, _, err := convert(in); err == nil || !strings.Contains(err.Error(), "12") {
		t.Fatalf("want a 12 px error, got %v", err)
	}
}

func TestRejectsOversizeGlyph(t *testing.T) {
	src := `STARTFONT 2.1
FONTBOUNDINGBOX 12 12 0 -2
STARTPROPERTIES 2
PIXEL_SIZE 12
FONT_ASCENT 10
ENDPROPERTIES
CHARS 1
STARTCHAR A
ENCODING 65
DWIDTH 6 0
BBX 8 1 0 0
BITMAP
FF
ENDCHAR
ENDFONT
`
	if _, _, err := convert(strings.NewReader(src)); err == nil {
		t.Fatal("want error for a glyph wider than its cell")
	}
}

func TestRunWritesFile(t *testing.T) {
	out := filepath.Join(t.TempDir(), "tiny.font")
	if err := run([]string{"-o", out, "testdata/tiny.bdf"}); err != nil {
		t.Fatal(err)
	}
	b, err := os.ReadFile(out)
	if err != nil {
		t.Fatal(err)
	}
	if len(b) != 16+4*4+12*2+18*2 {
		t.Fatalf("file is %d bytes", len(b))
	}
}
