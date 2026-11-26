package main

import (
	"bufio"
	"bytes"
	"encoding/binary"
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"
)

// esptool v5.6.0 banners, as printed by the two boards.
const (
	chipStickS3 = `esptool v5.6.0
Serial port /dev/cu.usbmodem5101:
Connecting...
Detecting chip type... ESP32-S3
Connected to ESP32-S3 on /dev/cu.usbmodem5101:
Chip type:          ESP32-S3-PICO-1 (LGA56) (revision v0.2)
Features:           Wi-Fi, BT 5 (LE), Dual Core + LP Core, 240MHz, Embedded Flash 8MB (GD), Embedded PSRAM 8MB (AP_3v3)
Crystal frequency:  40MHz
USB mode:           USB-Serial/JTAG
MAC:                70:04:1d:d6:28:40
`
	chipCardputer = `esptool v5.6.0
Serial port /dev/cu.usbmodem4101:
Connecting...
Detecting chip type... ESP32-S3
Connected to ESP32-S3 on /dev/cu.usbmodem4101:
Chip type:          ESP32-S3 (QFN56) (revision v0.2)
Features:           Wi-Fi, BT 5 (LE), Dual Core + LP Core, 240MHz, Embedded Flash 8MB (XMC)
Crystal frequency:  40MHz
USB mode:           USB-Serial/JTAG
MAC:                ac:a7:04:02:9b:54
`
	chipESP32 = `esptool v5.6.0
Connecting....
Detecting chip type... ESP32
Connected to ESP32 on /dev/ttyUSB0:
Chip type:          ESP32-D0WD-V3 (revision v3.1)
Features:           Wi-Fi, BT, Dual Core + LP Core, 240MHz, Vref calibration in eFuse, Coding Scheme None
`
	// a banner without the Features line
	chipNoFeatures = `esptool v5.6.0
Connecting...
Detecting chip type... ESP32-S3
Chip type:          ESP32-S3 (QFN56) (revision v0.2)
`
	// esptool v4 wording
	chipV4 = `esptool.py v4.8.1
Connecting...
Detecting chip type... ESP32-S3
Chip is ESP32-S3 (QFN56) (revision v0.2)
Features: WiFi, BLE, Embedded PSRAM 8MB (AP_3v3)
`
)

func TestParseChip(t *testing.T) {
	for _, c := range []struct {
		out   string
		s3    bool
		psram bool
		board string
	}{
		{chipStickS3, true, true, "sticks3"},
		{chipCardputer, true, false, "cardputer"},
		{chipESP32, false, false, "cardputer"},
		{chipV4, true, true, "sticks3"},
		{chipNoFeatures, true, false, ""},
	} {
		ci, err := parseChip(c.out)
		if err != nil {
			t.Fatal(err)
		}
		if ci.isS3() != c.s3 || ci.psram != c.psram || ci.guessBoard() != c.board {
			t.Errorf("%q: s3 %v psram %v board %s", ci.chip, ci.isS3(), ci.psram, ci.guessBoard())
		}
	}
	if _, err := parseChip("A fatal error occurred: Could not open port"); err == nil {
		t.Error("no banner accepted")
	}
}

// ptBytes encodes a partition table like gen_esp32part.py.
func ptBytes(parts []partition) []byte {
	var b []byte
	for _, p := range parts {
		e := make([]byte, 32)
		e[0], e[1], e[2], e[3] = 0xAA, 0x50, p.typ, p.subtype
		binary.LittleEndian.PutUint32(e[4:], p.offset)
		binary.LittleEndian.PutUint32(e[8:], p.size)
		copy(e[12:28], p.label)
		b = append(b, e...)
	}
	md5 := make([]byte, 32)
	md5[0], md5[1] = 0xEB, 0xEB
	b = append(b, md5...)
	for len(b) < partTableBytes {
		b = append(b, 0xFF)
	}
	return b
}

// gogoParts is platform/esp32s3/partitions.csv.
var gogoParts = []partition{
	{"nvs", partTypeData, partSubNVS, 0x9000, 0x6000},
	{"factory", partTypeApp, 0x00, 0x10000, 0x200000},
	{"gogo", partTypeData, partSubImage, 0x210000, 0x100000},
	{"gogofs", partTypeData, 0x83, 0x310000, 0x4F0000},
}

func TestParsePartitions(t *testing.T) {
	parts, err := parsePartitions(ptBytes(gogoParts))
	if err != nil {
		t.Fatal(err)
	}
	if fmt.Sprint(parts) != fmt.Sprint(gogoParts) {
		t.Fatalf("got %v", parts)
	}
	if n := nvsParts(parts); len(n) != 1 || n[0].offset != 0x9000 || n[0].end() != 0xF000 {
		t.Fatalf("nvs %v", n)
	}
	if err := checkPartitions(parts); err != nil {
		t.Fatal(err)
	}
	if _, err := parsePartitions(bytes.Repeat([]byte{0x12}, 64)); err == nil {
		t.Error("garbage accepted")
	}
	if _, err := parsePartitions(bytes.Repeat([]byte{0xFF}, 64)); err == nil {
		t.Error("blank flash accepted")
	}
	moved := append([]partition{}, gogoParts...)
	moved[2].offset = 0x220000
	if err := checkPartitions(moved); err == nil {
		t.Error("image partition elsewhere accepted")
	}
	if err := checkPartitions(gogoParts[1:]); err == nil {
		t.Error("table without NVS accepted")
	}
}

func TestCheckWrites(t *testing.T) {
	w := func(off, size uint32) flashWrite {
		return flashWrite{offset: off, name: fmt.Sprintf("f%#x", off), size: size}
	}
	protect := protectedRanges(nvsParts(gogoParts))
	good := []flashWrite{w(0x0, 0x5000), w(0x8000, 0xC00), w(0x10000, 0x110000)}
	if err := checkWrites(good, gogoParts, 0x8000, protect); err != nil {
		t.Fatal(err)
	}
	// Edges: the bootloader may fill 0x0..0x8000 exactly; an app may start
	// right after NVS ends (0xF000) when an app partition is there.
	edge := append([]partition{}, gogoParts...)
	edge[1] = partition{"factory", partTypeApp, 0, 0xF000, 0x201000}
	if err := checkWrites([]flashWrite{w(0x0, 0x8000), w(0x8000, 0xC00), w(0xF000, 0x1000)}, edge, 0x8000, protect); err != nil {
		t.Fatalf("edges: %v", err)
	}
	for name, ws := range map[string][]flashWrite{
		"write at NVS":           {w(0x9000, 0x100)},
		"write ends in NVS":      {w(0x8000, 0x1001)},
		"write starts in NVS":    {w(0xE000, 0x2000)},
		"last NVS byte":          {w(0xEFFF, 1)},
		"bootloader too big":     {w(0x0, 0x8001)},
		"outside app partitions": {w(0x210000, 0x100)},
		"into the file system":   {w(0x310000, 0x1000)},
		"overlapping writes":     {w(0x10000, 0x2000), w(0x11000, 0x100)},
		"app past its partition": {w(0x10000, 0x200001)},
		"past the flash":         {w(0xFFFFF000, 0x2000)},
	} {
		if err := checkWrites(ws, gogoParts, 0x8000, protect); err == nil {
			t.Errorf("%s: accepted", name)
		}
	}
	if err := checkWrites(good, gogoParts[1:], 0x8000, protect); err == nil {
		t.Error("no NVS partition: accepted")
	}
	// A table that moves NVS away and puts an app partition over 0x9000:
	// the default NVS range (and the board's) stays protected.
	moved := []partition{
		{"factory", partTypeApp, 0, 0x9000, 0x200000},
		{"nvs", partTypeData, partSubNVS, 0x310000, 0x6000},
		{"gogo", partTypeData, partSubImage, 0x210000, 0x100000},
	}
	if err := checkWrites([]flashWrite{w(0x9000, 0x1000)}, moved, 0x8000, protectedRanges(nvsParts(moved))); err == nil {
		t.Error("app write over the default NVS range accepted")
	}
	boardNVS := []partition{{"nvs", partTypeData, partSubNVS, 0x20000, 0x5000}}
	if err := checkWrites([]flashWrite{w(0x20000, 0x1000)}, moved, 0x8000, protectedRanges(nvsParts(moved), boardNVS)); err == nil {
		t.Error("write over the board's NVS accepted")
	}
	if err := checkWrites([]flashWrite{w(0x0, 0xF000)}, moved, 0x10000, protect); err == nil {
		t.Error("partition table away from 0x8000 accepted")
	}
}

func TestParsePartitionsRejectsBadRanges(t *testing.T) {
	for _, p := range []partition{
		{"all", partTypeApp, 0, 0x0, 0xFFFFFFFF},
		{"odd", partTypeApp, 0, 0x10800, 0x1000},
		{"empty", partTypeApp, 0, 0x10000, 0},
	} {
		if _, err := parsePartitions(ptBytes([]partition{gogoParts[0], p})); err == nil {
			t.Errorf("%s accepted", p.label)
		}
	}
}

func TestCheckEsptoolArgs(t *testing.T) {
	dir := t.TempDir()
	small := filepath.Join(dir, "small.bin")
	os.WriteFile(small, make([]byte, 100), 0o644)
	big := filepath.Join(dir, "big.bin")
	os.WriteFile(big, make([]byte, 0x1001), 0o644)
	ptSize := filepath.Join(dir, "pt.bin")
	os.WriteFile(ptSize, make([]byte, 0xC00), 0o644)
	base := []string{"--chip", "esp32s3", "--port", "/dev/x", "--after", "watchdog-reset"}
	ok := [][]string{
		{"--port", "/dev/x", "--after", "watchdog-reset", "read-flash", "0x8000", "3072", small},
		append(base, "write-flash", "--flash-mode", "dio", "--flash-freq", "80m", "--flash-size", "8MB", "0x0", small, "0x8000", ptSize, "0x10000", big),
		append(base, "write-flash", "0x210000", small),
		append(base, "write-flash", "0xF000", big), // starts right after NVS
		{"version"},
	}
	for _, a := range ok {
		if err := checkEsptoolArgs(a, defaultNVS); err != nil {
			t.Errorf("%q: %v", a, err)
		}
	}
	bad := [][]string{
		append(base, "erase-flash"),
		append(base, "erase_flash"),
		append(base, "erase-region", "0x9000", "0x6000"),
		append(base, "write-flash-status", "--non-volatile", "0"),
		append(base, "write-flash", "--erase-all", "0x0", small),
		append(base, "write-flash", "-e", "0x0", small),
		append(base, "merge-bin", "-o", "x.bin", "0x0", small),
		append(base, "write-flash", "0x9000", small),
		append(base, "write-flash", "0xA000", small),
		append(base, "write-flash", "0x8000", big), // reaches the NVS sector
		append(base, "write-flash", "0x0", small, "0xE000", small),
		append(base, "write-flash", "0x0"),
		append(base, "write-flash", "0x0", filepath.Join(dir, "missing.bin")),
		append(base, "read-flash", "0x0", "0x1000", small, "--extra"),
		append(base, "--trace", "read-flash", "0x0", "0x1000", small),
		base,
	}
	for _, a := range bad {
		if err := checkEsptoolArgs(a, defaultNVS); err == nil {
			t.Errorf("%q: accepted", a)
		}
	}
	if err := checkEsptoolArgs(ok[0], nil); err == nil {
		t.Error("unknown NVS: accepted")
	}
}

// fakeEsptool mimics esptool: the v5 banner (or v4's, by
// FAKE_ESPTOOL_VERSION), the chip from $FAKE_ESPTOOL_CHIP, and with a
// --chip that names another chip it fails as the real tool does.
const fakeEsptool = `#!/bin/sh
echo "$*" >> "$FAKE_ESPTOOL_LOG"
ver=${FAKE_ESPTOOL_VERSION:-5.6.0}
case $ver in 4.*) name=esptool.py ;; *) name=esptool ;; esac
if [ "$1" = version ]; then echo "$name v$ver"; echo "$ver"; exit 0; fi
chip=$(sed -n 's/^Detecting chip type[. ]*//p' "$FAKE_ESPTOOL_CHIP" | head -n 1)
want=$FAKE_ESPTOOL_FORCE_CHIP prev= last=
for a; do
	[ "$prev" = --chip ] && want=$a
	prev=$a last=$a
done
if [ -n "$want" ] && [ "$want" != auto ] && [ "$want" != esp32s3 -o "$chip" != ESP32-S3 ]; then
	printf '%s v%s\nConnecting....\n\nA fatal error occurred: This chip is %s, not ESP32-S3. Wrong --chip argument?\n' "$name" "$ver" "$chip"
	exit 2
fi
case " $* " in
*" read-flash "*) cat "$FAKE_ESPTOOL_CHIP"; cp "$FAKE_ESPTOOL_PT" "$last" ;;
*" write-flash "*) cat "$FAKE_ESPTOOL_CHIP"; echo "Hard resetting with a watchdog..." ;;
*) echo "unexpected: $*" >&2; exit 2 ;;
esac
`

// setupBench is a test bench: a fake esptool on PATH, a release directory
// and an environment for setupMain.
type setupBench struct {
	t       *testing.T
	dir     string
	log     string
	release string // release asset directory
	env     *setupEnv
	out     *bytes.Buffer

	mu        sync.Mutex
	boot      []string          // console lines after each open
	store     map[string]string // the board's config store
	sets      []string          // keys set, in order
	scans     int
	scanFails int // the first scanFails scans fail
	// rejectPassword makes the board refuse wifi.password.
	rejectPassword bool
	opens          int
}

func newSetupBench(t *testing.T, chip string, stdin string) *setupBench {
	t.Helper()
	if runtime.GOOS == "windows" {
		t.Skip("fake esptool is a shell script")
	}
	dir := t.TempDir()
	bin := filepath.Join(dir, "bin")
	os.MkdirAll(bin, 0o755)
	if err := os.WriteFile(filepath.Join(bin, "esptool"), []byte(fakeEsptool), 0o755); err != nil {
		t.Fatal(err)
	}
	t.Setenv("PATH", bin+string(os.PathListSeparator)+os.Getenv("PATH"))
	f := &setupBench{t: t, dir: dir, log: filepath.Join(dir, "esptool.log"), release: filepath.Join(dir, "release"), out: &bytes.Buffer{}}
	t.Setenv("FAKE_ESPTOOL_LOG", f.log)
	chipFile := filepath.Join(dir, "chip.txt")
	os.WriteFile(chipFile, []byte(chip), 0o644)
	t.Setenv("FAKE_ESPTOOL_CHIP", chipFile)
	ptFile := filepath.Join(dir, "board-pt.bin")
	os.WriteFile(ptFile, ptBytes(gogoParts), 0o644)
	t.Setenv("FAKE_ESPTOOL_PT", ptFile)
	t.Setenv("GOGO_RELEASE_URL", "")
	f.env = &setupEnv{
		stdin:    strings.NewReader(stdin),
		out:      f.out,
		goos:     "darwin",
		getenv:   func(string) string { return "" },
		glob:     func(string) ([]string, error) { return []string{"/dev/cu.usbmodem9"}, nil },
		lookPath: exec.LookPath,
		openPort: func(string) (io.ReadWriteCloser, error) { return f.openConsole(), nil },
		client:   http.DefaultClient,
		cacheDir: filepath.Join(dir, "cache"),
		repoDir:  func() (string, bool) { return "", false },

		retryDelay: time.Millisecond,
		runIdf: func(localBuild, []string) error {
			t.Fatal("unexpected local build")
			return nil
		},
	}
	f.env.in = bufio.NewReader(f.env.stdin)
	f.store = map[string]string{}
	f.boot = []string{"gogo vtest: image 1556 bytes, arena 163840 bytes, free RAM 190000 (largest 180000)", "hello from gogo on the board!"}
	return f
}

// fakeConsole is the board's serial console: it prints f.boot when
// opened and answers config frames like platform/esp32s3/main/config_esp.c.
type fakeConsole struct {
	b      *setupBench
	pr     *io.PipeReader
	pw     *io.PipeWriter
	in     []byte
	closed int32
}

func (f *setupBench) openConsole() *fakeConsole {
	pr, pw := io.Pipe()
	c := &fakeConsole{b: f, pr: pr, pw: pw}
	f.mu.Lock()
	f.opens++
	f.mu.Unlock()
	go func() {
		for _, l := range f.boot {
			if _, err := io.WriteString(pw, "noise\r\n"+l+"\r\n"); err != nil {
				return
			}
		}
	}()
	return c
}

func (c *fakeConsole) Read(p []byte) (int, error) { return c.pr.Read(p) }
func (c *fakeConsole) Close() error {
	atomic.StoreInt32(&c.closed, 1)
	c.pr.Close()
	return c.pw.Close()
}

func (c *fakeConsole) Write(p []byte) (int, error) {
	c.in = append(c.in, p...)
	for {
		i := bytes.IndexByte(c.in, '\n')
		if i < 0 {
			return len(p), nil
		}
		frame := string(c.in[:i])
		c.in = c.in[i+1:]
		reply := c.answer(strings.TrimPrefix(frame, "\x02gogo-config "))
		go io.WriteString(c.pw, reply)
	}
}

func (c *fakeConsole) answer(cmd string) string {
	b := c.b
	b.mu.Lock()
	defer b.mu.Unlock()
	op, rest, _ := strings.Cut(cmd, " ")
	key, value, _ := strings.Cut(rest, " ")
	switch op {
	case "scan":
		b.scans++
		if b.scans <= b.scanFails {
			return "\x02err scan failed (wifi busy connecting; try again)\n"
		}
		return "tick\x02kv " + fmt.Sprintf("%-32s ch %2d %4d dBm secured", "Home Net", 6, -50) + "\n" +
			"\x02kv " + fmt.Sprintf("%-32s ch %2d %4d dBm open", "Cafe", 11, -70) + "\n" +
			"\x02kv " + fmt.Sprintf("%-32s ch %2d %4d dBm secured", "Home Net", 1, -80) + "\n\x02ok\n"
	case "set":
		if key == "wifi.password" && b.rejectPassword {
			return "\x02err ESP_ERR_NVS_NOT_ENOUGH_SPACE\n"
		}
		b.store[key] = value
		b.sets = append(b.sets, key)
		if key == "wifi.ssid" {
			return "\x02ok\ngogo: wifi settings changed, reconnecting\ngogo: wifi connected (" + value + ", ch 6, -50 dBm)\n"
		}
		return "more output\x02ok\n"
	case "rm":
		if _, ok := b.store[key]; !ok {
			return "\x02err not set\n"
		}
		delete(b.store, key)
		return "\x02ok\n"
	}
	return "\x02err usage\n"
}

// flasherArgsJSON is ESP-IDF's flasher_args.json for the gogo firmware.
const flasherArgsJSON = `{
    "write_flash_args" : [ "--flash-mode", "dio", "--flash-size", "8MB", "--flash-freq", "80m" ],
    "flash_settings" : { "flash_mode": "dio", "flash_size": "8MB", "flash_freq": "80m" },
    "flash_files" : {
        "0x0" : "bootloader/bootloader.bin",
        "0x8000" : "partition_table/partition-table.bin",
        "0x10000" : "gogo.bin"
    },
    "bootloader" : { "offset" : "0x0", "file" : "bootloader/bootloader.bin", "encrypted" : "false" },
    "partition-table" : { "offset" : "0x8000", "file" : "partition_table/partition-table.bin", "encrypted" : "false" },
    "app" : { "offset" : "0x10000", "file" : "gogo.bin", "encrypted" : "false" },
    "extra_esptool_args" : { "after" : "hard-reset", "before" : "default-reset", "stub" : true, "chip" : "esp32s3" }
}
`

// firmwareFiles writes a build directory's firmware files into dir, named
// by name(file in flasher_args.json).
func firmwareFiles(t *testing.T, dir string, name func(string) string) {
	t.Helper()
	files := map[string][]byte{
		"flasher_args.json":                   []byte(flasherArgsJSON),
		"bootloader/bootloader.bin":           bytes.Repeat([]byte{0xE9}, 0x5000),
		"partition_table/partition-table.bin": ptBytes(gogoParts),
		"gogo.bin":                            bytes.Repeat([]byte{0xE9}, 0x100000),
	}
	for f, b := range files {
		p := filepath.Join(dir, name(f))
		os.MkdirAll(filepath.Dir(p), 0o755)
		if err := os.WriteFile(p, b, 0o644); err != nil {
			t.Fatal(err)
		}
	}
}

// writeRelease fills the release directory for both boards, with sums.
func (f *setupBench) writeRelease() {
	os.MkdirAll(f.release, 0o755)
	for _, board := range []string{"sticks3", "cardputer"} {
		firmwareFiles(f.t, f.release, func(n string) string { return releaseAssetName(board, n) })
	}
	f.writeSums()
}

func (f *setupBench) writeSums() {
	ents, _ := os.ReadDir(f.release)
	var sums strings.Builder
	for _, e := range ents {
		if e.Name() == releaseSums {
			continue
		}
		b, _ := os.ReadFile(filepath.Join(f.release, e.Name()))
		fmt.Fprintf(&sums, "%s  %s\n", sha256hex(b), e.Name())
	}
	os.WriteFile(filepath.Join(f.release, releaseSums), []byte(sums.String()), 0o644)
}

// serve serves the release directory; hits counts the requests.
func (f *setupBench) serve(hits *int32) string {
	h := http.FileServer(http.Dir(f.release))
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if hits != nil {
			atomic.AddInt32(hits, 1)
		}
		h.ServeHTTP(w, r)
	}))
	f.t.Cleanup(srv.Close)
	return srv.URL
}

func (f *setupBench) esptoolLog() string {
	b, _ := os.ReadFile(f.log)
	return string(b)
}

// writeFlashLines returns the logged esptool commands that write.
func (f *setupBench) writeFlashLines() []string {
	var out []string
	for _, l := range strings.Split(f.esptoolLog(), "\n") {
		if strings.Contains(l, "write-flash") {
			out = append(out, l)
		}
	}
	return out
}

func (f *setupBench) run(args ...string) error {
	return setupMain(args, f.env)
}

// checkNoNVSWrite parses every logged write-flash and checks it against
// the NVS partition of the table written.
func checkNoNVSWrite(t *testing.T, lines []string) {
	t.Helper()
	stand := filepath.Join(t.TempDir(), "sector.bin") // for removed temp files: one sector
	os.WriteFile(stand, make([]byte, flashSector), 0o644)
	for _, l := range lines {
		fields := strings.Fields(l)
		for i, f := range fields {
			if strings.HasPrefix(f, "/") {
				if _, err := os.Stat(f); err != nil {
					fields[i] = stand
				}
			}
		}
		if err := checkEsptoolArgs(fields, nvsParts(gogoParts)); err != nil {
			t.Errorf("logged command fails the NVS check: %v\n%s", err, l)
		}
		for _, f := range fields {
			if strings.Contains(f, "erase") {
				t.Errorf("erase in %s", l)
			}
		}
	}
}

func TestSetupFlashesAtFlasherArgsOffsets(t *testing.T) {
	f := newSetupBench(t, chipStickS3, "")
	f.writeRelease()
	url := f.serve(nil)
	if err := f.run("-yes", "-skip-wifi", "-release-url", url); err != nil {
		t.Fatalf("%v\n%s", err, f.out)
	}
	lines := f.writeFlashLines()
	if len(lines) == 0 {
		t.Fatalf("no write-flash:\n%s", f.esptoolLog())
	}
	fw := strings.Fields(lines[0])
	want := []string{"--chip", "esp32s3", "--port", "/dev/cu.usbmodem9", "--after", "watchdog-reset", "write-flash",
		"--flash-mode", "dio", "--flash-freq", "80m", "--flash-size", "8MB"}
	if strings.Join(fw[:len(want)], " ") != strings.Join(want, " ") {
		t.Fatalf("firmware command %q", fw)
	}
	pairs := fw[len(want):]
	if len(pairs) != 6 || pairs[0] != "0x0" || pairs[2] != "0x8000" || pairs[4] != "0x10000" ||
		filepath.Dir(pairs[1]) != filepath.Join(f.env.cacheDir, "release") {
		t.Fatalf("writes %q", pairs)
	}
	for i := 1; i < 6; i += 2 {
		b, _ := os.ReadFile(pairs[i])
		name := map[int]string{1: "sticks3-bootloader.bin", 3: "sticks3-partition-table.bin", 5: "sticks3-gogo.bin"}[i]
		want, _ := os.ReadFile(filepath.Join(f.release, name))
		if !bytes.Equal(b, want) {
			t.Errorf("%s: flashed file differs from %s", pairs[i], name)
		}
	}
	checkNoNVSWrite(t, lines)
	if !strings.Contains(f.out.String(), "not written: NVS 0x9000..0xf000") {
		t.Errorf("NVS note missing:\n%s", f.out)
	}

	// A second run takes the files from the cache.
	f.out.Reset()
	if err := f.run("-yes", "-skip-wifi", "-release-url", url); err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(f.out.String(), "sticks3-gogo.bin (cached)") {
		t.Errorf("cache not used:\n%s", f.out)
	}
}

func TestSetupDryRun(t *testing.T) {
	f := newSetupBench(t, chipCardputer, "")
	f.writeRelease()
	if err := f.run("-yes", "-dry-run", "-skip-wifi", "-release-url", f.release); err != nil {
		t.Fatalf("%v\n%s", err, f.out)
	}
	if n := len(f.writeFlashLines()); n != 0 {
		t.Fatalf("dry run wrote %d times", n)
	}
	out := f.out.String()
	for _, want := range []string{
		"dry run:",
		"This looks like the Cardputer ADV. Right? [Y/n] y (-yes)",
		"read-flash 0x8000 3072",
		"would run: " + filepath.Join(f.dir, "bin", "esptool") + " --chip esp32s3 --port /dev/cu.usbmodem9 --after watchdog-reset write-flash --flash-mode dio --flash-freq 80m --flash-size 8MB 0x0 ",
		"cardputer-bootloader.bin",
	} {
		if !strings.Contains(out, want) {
			t.Errorf("output lacks %q:\n%s", want, out)
		}
	}
	if !strings.Contains(f.esptoolLog(), "read-flash 0x8000 3072") {
		t.Errorf("the board was not identified:\n%s", f.esptoolLog())
	}
}

func TestSetupChecksumMismatch(t *testing.T) {
	f := newSetupBench(t, chipStickS3, "")
	f.writeRelease()
	app := filepath.Join(f.release, "sticks3-gogo.bin")
	b, _ := os.ReadFile(app)
	b[100] ^= 1
	os.WriteFile(app, b, 0o644)
	err := f.run("-yes", "-skip-wifi", "-release-url", f.serve(nil))
	if err == nil || !strings.Contains(err.Error(), "checksum mismatch") {
		t.Fatalf("err = %v", err)
	}
	if len(f.writeFlashLines()) != 0 {
		t.Fatal("flashed a file that failed its checksum")
	}
	if _, err := os.Stat(filepath.Join(f.env.cacheDir, "release", sha256hex(b))); err == nil {
		t.Error("the bad file was cached")
	}
}

func TestSetupRefusesOtherChips(t *testing.T) {
	f := newSetupBench(t, chipESP32, "")
	f.writeRelease()
	err := f.run("-yes", "-skip-wifi", "-release-url", f.release)
	if err == nil || !strings.Contains(err.Error(), "not an ESP32-S3") || !strings.Contains(err.Error(), "fix:") {
		t.Fatalf("err = %v", err)
	}
	if len(f.writeFlashLines()) != 0 {
		t.Fatal("flashed a non-S3 chip")
	}
	if strings.Contains(f.esptoolLog(), "--chip") {
		t.Errorf("identify forced the chip type:\n%s", f.esptoolLog())
	}
	// esptool's own refusal of a wrong --chip says the same.
	g := newSetupBench(t, chipESP32, "")
	t.Setenv("FAKE_ESPTOOL_FORCE_CHIP", "esp32s3")
	err = g.run("-yes", "-skip-wifi", "-release-url", g.release)
	if err == nil || !strings.Contains(err.Error(), "is ESP32, not an ESP32-S3") || strings.Contains(err.Error(), "boot button") {
		t.Fatalf("wrong --chip: err = %v", err)
	}
}

func TestSetupUnknownPSRAM(t *testing.T) {
	f := newSetupBench(t, chipNoFeatures, "")
	f.writeRelease()
	if err := f.run("-yes", "-skip-wifi", "-release-url", f.release); err == nil || !strings.Contains(err.Error(), "-board") {
		t.Fatalf("err = %v", err)
	}
	if err := f.run("-yes", "-skip-wifi", "-board", "sticks3", "-release-url", f.release); err != nil {
		t.Fatalf("%v\n%s", err, f.out)
	}
	g := newSetupBench(t, chipNoFeatures, "2\ny\n") // asked which board, no guess offered
	g.writeRelease()
	if err := g.run("-skip-wifi", "-release-url", g.release); err != nil {
		t.Fatalf("%v\n%s", err, g.out)
	}
	if strings.Contains(g.out.String(), "looks like") || !strings.Contains(g.out.String(), "to the Cardputer ADV") {
		t.Errorf("output:\n%s", g.out)
	}
}

// replaceAsset rewrites one release file and the sums.
func (f *setupBench) replaceAsset(name string, b []byte) {
	if err := os.WriteFile(filepath.Join(f.release, name), b, 0o644); err != nil {
		f.t.Fatal(err)
	}
	f.writeSums()
}

// A release whose own partition table moves NVS cannot make setup write
// over the NVS the board has (the review's probe, as a test).
func TestSetupNeverWritesNVSWhateverTheRelease(t *testing.T) {
	moved := ptBytes([]partition{
		{"factory", partTypeApp, 0, 0x9000, 0x200000},
		{"gogo", partTypeData, partSubImage, 0x210000, 0x100000},
		{"nvs", partTypeData, partSubNVS, 0x310000, 0x6000},
	})
	appAt9000 := strings.Replace(flasherArgsJSON, `"0x10000" : "gogo.bin"`, `"0x9000" : "gogo.bin"`, 1)
	ptAway := strings.NewReplacer(`"0x8000" : "partition_table`, `"0x10000" : "partition_table`,
		`"offset" : "0x8000"`, `"offset" : "0x10000"`, `"0x10000" : "gogo.bin"`, `"0x20000" : "gogo.bin"`).Replace(flasherArgsJSON)
	for name, c := range map[string]struct {
		pt   []byte
		args string
		want string
	}{
		"app over 0x9000":            {moved, appAt9000, "would overwrite the NVS partition"},
		"partition table at 0x10000": {moved, ptAway, "partition table at"},
		"bootloader into NVS":        {nil, strings.Replace(flasherArgsJSON, `"0x10000" : "gogo.bin"`, `"0x5000" : "gogo.bin"`, 1), "NVS"},
	} {
		f := newSetupBench(t, chipStickS3, "")
		f.writeRelease()
		if c.pt != nil {
			f.replaceAsset("sticks3-partition-table.bin", c.pt)
		}
		f.replaceAsset("sticks3-flasher_args.json", []byte(c.args))
		err := f.run("-yes", "-skip-wifi", "-new-layout", "-release-url", f.release)
		if err == nil || !strings.Contains(err.Error(), c.want) {
			t.Errorf("%s: err = %v", name, err)
		}
		if l := f.writeFlashLines(); len(l) != 0 {
			t.Errorf("%s: flashed %q", name, l)
		}
	}
}

func TestSetupRefusesNewLayout(t *testing.T) {
	// The board keeps NVS at 0x9000..0xE000 (an Arduino-style table); the
	// release at 0x9000..0xF000.
	board := ptBytes([]partition{
		{"nvs", partTypeData, partSubNVS, 0x9000, 0x5000},
		{"otadata", partTypeData, 0x00, 0xE000, 0x2000},
		{"app0", partTypeApp, 0x10, 0x10000, 0x140000},
	})
	f := newSetupBench(t, chipStickS3, "")
	os.WriteFile(os.Getenv("FAKE_ESPTOOL_PT"), board, 0o644)
	f.writeRelease()
	err := f.run("-yes", "-skip-wifi", "-release-url", f.release)
	if err == nil || !strings.Contains(err.Error(), "0x9000..0xe000") || !strings.Contains(err.Error(), "-new-layout") {
		t.Fatalf("err = %v", err)
	}
	if len(f.writeFlashLines()) != 0 {
		t.Fatal("flashed over a different layout")
	}
	f.out.Reset()
	if err := f.run("-yes", "-skip-wifi", "-new-layout", "-release-url", f.release); err != nil {
		t.Fatalf("%v\n%s", err, f.out)
	}
	if !strings.Contains(f.out.String(), "not written: NVS 0x9000..0xe000, 0x9000..0xf000") {
		t.Errorf("protected ranges:\n%s", f.out)
	}
	checkNoNVSWrite(t, f.writeFlashLines())

	// A board whose layout puts NVS where the new app goes cannot be
	// flashed by gogo setup at all.
	g := newSetupBench(t, chipStickS3, "")
	os.WriteFile(os.Getenv("FAKE_ESPTOOL_PT"), ptBytes([]partition{{"nvs", partTypeData, partSubNVS, 0x20000, 0x6000}}), 0o644)
	g.writeRelease()
	if err := g.run("-yes", "-skip-wifi", "-new-layout", "-release-url", g.release); err == nil || !strings.Contains(err.Error(), "would overwrite the NVS partition") {
		t.Fatalf("err = %v", err)
	}
	if len(g.writeFlashLines()) != 0 {
		t.Fatal("flashed over the board's NVS")
	}
}

func TestSetupBlankBoard(t *testing.T) {
	f := newSetupBench(t, chipStickS3, "")
	os.WriteFile(os.Getenv("FAKE_ESPTOOL_PT"), bytes.Repeat([]byte{0xFF}, partTableBytes), 0o644)
	f.writeRelease()
	if err := f.run("-yes", "-skip-wifi", "-release-url", f.release); err != nil {
		t.Fatalf("%v\n%s", err, f.out)
	}
	if !strings.Contains(f.out.String(), "no readable partition table") {
		t.Errorf("output:\n%s", f.out)
	}
	checkNoNVSWrite(t, f.writeFlashLines())
}

func TestSetupForeignPartitionData(t *testing.T) {
	// The sector at 0x8000 holds something that is not a partition table.
	f := newSetupBench(t, chipStickS3, "")
	junk := bytes.Repeat([]byte{0xFF}, partTableBytes)
	copy(junk, "not a partition table")
	os.WriteFile(os.Getenv("FAKE_ESPTOOL_PT"), junk, 0o644)
	f.writeRelease()
	err := f.run("-yes", "-skip-wifi", "-release-url", f.release)
	if err == nil || !strings.Contains(err.Error(), "cannot read") || !strings.Contains(err.Error(), "-new-layout") {
		t.Fatalf("err = %v", err)
	}
	if len(f.writeFlashLines()) != 0 {
		t.Fatal("flashed over foreign partition data")
	}
	if err := f.run("-yes", "-skip-wifi", "-new-layout", "-release-url", f.release); err != nil {
		t.Fatalf("%v\n%s", err, f.out)
	}
	checkNoNVSWrite(t, f.writeFlashLines())
}

func TestSetupHostileFlasherArgs(t *testing.T) {
	for name, c := range map[string]struct {
		args string
		want string // in the error; "" means it must succeed
	}{
		"flag in flash_mode":        {strings.Replace(flasherArgsJSON, `"flash_mode": "dio"`, `"flash_mode": "dio --erase-all"`, 1), "bad flash setting"},
		"path out of the cache":     {strings.Replace(flasherArgsJSON, `"0x10000" : "gogo.bin"`, `"0x10000" : "../../../etc/passwd"`, 1), "no firmware"},
		"erase in write_flash_args": {strings.Replace(flasherArgsJSON, `"--flash-mode", "dio"`, `"--erase-all", "--flash-mode", "dio"`, 1), ""},
		"other chip":                {strings.Replace(flasherArgsJSON, `"chip" : "esp32s3"`, `"chip" : "esp32"`, 1), "not esp32s3"},
	} {
		f := newSetupBench(t, chipStickS3, "")
		f.writeRelease()
		f.replaceAsset("sticks3-flasher_args.json", []byte(c.args))
		err := f.run("-yes", "-skip-wifi", "-release-url", f.release)
		if c.want == "" {
			if err != nil {
				t.Errorf("%s: %v", name, err)
			}
			if strings.Contains(f.esptoolLog(), "erase") {
				t.Errorf("%s: erase reached esptool", name)
			}
			continue
		}
		if err == nil || !strings.Contains(err.Error(), c.want) {
			t.Errorf("%s: err = %v", name, err)
		}
		if len(f.writeFlashLines()) != 0 {
			t.Errorf("%s: flashed", name)
		}
	}
}

func TestSetupPinsTheLatestRelease(t *testing.T) {
	// /latest/download redirects to the newest tag, which changes after
	// the first request: every file must still come from the first one.
	f := newSetupBench(t, chipStickS3, "")
	f.writeRelease()
	old := f.release
	newer := filepath.Join(f.dir, "newer")
	os.MkdirAll(newer, 0o755)
	g := &setupBench{t: t, release: newer}
	g.writeRelease()
	g.replaceAsset("sticks3-gogo.bin", []byte("a newer app"))
	var latest atomic.Value
	latest.Store("v1.0.0")
	files := map[string]http.Handler{"v1.0.0": http.FileServer(http.Dir(old)), "v1.1.0": http.FileServer(http.Dir(newer))}
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if name, ok := strings.CutPrefix(r.URL.Path, "/releases/latest/download/"); ok {
			http.Redirect(w, r, "/releases/download/"+latest.Load().(string)+"/"+name, http.StatusFound)
			latest.Store("v1.1.0") // published meanwhile
			return
		}
		rest, _ := strings.CutPrefix(r.URL.Path, "/releases/download/")
		tag, name, _ := strings.Cut(rest, "/")
		r.URL.Path = "/" + name
		files[tag].ServeHTTP(w, r)
	}))
	defer srv.Close()
	if err := f.run("-yes", "-skip-wifi", "-release-url", srv.URL+"/releases/latest/download"); err != nil {
		t.Fatalf("%v\n%s", err, f.out)
	}
	if !strings.Contains(f.out.String(), "that is "+srv.URL+"/releases/download/v1.0.0") {
		t.Errorf("not pinned:\n%s", f.out)
	}
}

func TestSetupNotAChecksumList(t *testing.T) {
	f := newSetupBench(t, chipStickS3, "")
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		io.WriteString(w, "<html>Sign in to the guest network</html>")
	}))
	defer srv.Close()
	err := f.run("-yes", "-skip-wifi", "-release-url", srv.URL)
	if err == nil || !strings.Contains(err.Error(), "release is not available") || strings.Contains(err.Error(), "broken") {
		t.Fatalf("err = %v", err)
	}
}

func TestSetupOldEsptool(t *testing.T) {
	f := newSetupBench(t, chipStickS3, "")
	t.Setenv("FAKE_ESPTOOL_VERSION", "4.8.1")
	err := f.run("-yes", "-release-url", f.release)
	if err == nil || !strings.Contains(err.Error(), "too old") {
		t.Fatalf("err = %v", err)
	}
}

func TestSetupBoardChoice(t *testing.T) {
	// The user says the guess is wrong and picks the other board.
	f := newSetupBench(t, chipCardputer, "n\n1\ny\n")
	f.writeRelease()
	if err := f.run("-skip-wifi", "-release-url", f.release); err != nil {
		t.Fatalf("%v\n%s", err, f.out)
	}
	if l := f.writeFlashLines(); len(l) != 2 || !strings.Contains(l[0], filepath.Join(f.env.cacheDir, "release")) {
		t.Fatalf("writes %q", l)
	}
	if !strings.Contains(f.out.String(), "Flash this firmware to the M5StickS3") {
		t.Errorf("board choice ignored:\n%s", f.out)
	}
	// Saying no to flashing stops before any write.
	g := newSetupBench(t, chipStickS3, "y\nn\n")
	g.writeRelease()
	if err := g.run("-skip-wifi", "-release-url", g.release); err == nil || !strings.Contains(err.Error(), "stopped before flashing") {
		t.Fatalf("err = %v", err)
	}
	if len(g.writeFlashLines()) != 0 {
		t.Fatal("flashed after a no")
	}
	// A closed stdin without -yes stops too.
	h := newSetupBench(t, chipStickS3, "")
	h.writeRelease()
	if err := h.run("-skip-wifi", "-release-url", h.release); err == nil || !strings.Contains(err.Error(), "-yes") {
		t.Fatalf("err = %v", err)
	}
}

// localBench makes the fake board fall back to a local build: a gogo
// tree and an ESP-IDF directory; the fake idf.py fills the build
// directory with firmware files.
func localBench(t *testing.T, f *setupBench) *[]string {
	repo := filepath.Join(f.dir, "repo")
	idf := filepath.Join(f.dir, "esp-idf")
	os.MkdirAll(repo, 0o755)
	os.MkdirAll(idf, 0o755)
	os.WriteFile(filepath.Join(idf, "export.sh"), nil, 0o644)
	f.env.repoDir = func() (string, bool) { return repo, true }
	f.env.getenv = func(k string) string {
		if k == "IDF_PATH" {
			return idf
		}
		return ""
	}
	var calls []string
	f.env.runIdf = func(lb localBuild, args []string) error {
		if lb.repo != repo || lb.export != filepath.Join(idf, "export.sh") {
			t.Errorf("local build %+v", lb)
		}
		calls = append(calls, strings.Join(args, " "))
		for i, a := range args {
			if a == "-B" {
				firmwareFiles(t, args[i+1], func(n string) string { return n })
			}
		}
		return nil
	}
	return &calls
}

func TestSetupFallsBackToLocalBuild(t *testing.T) {
	f := newSetupBench(t, chipStickS3, "")
	calls := localBench(t, f)
	var hits int32
	url := f.serve(&hits) // empty: every asset is a 404
	if err := f.run("-yes", "-skip-wifi", "-release-url", url); err != nil {
		t.Fatalf("%v\n%s", err, f.out)
	}
	if hits == 0 || len(*calls) != 1 || !strings.Contains((*calls)[0], "-DGOGO_BOARD=sticks3") {
		t.Fatalf("hits %d, idf.py calls %q", hits, *calls)
	}
	lines := f.writeFlashLines()
	build := filepath.Join(f.dir, "repo", "build", "setup-sticks3")
	if len(lines) != 2 || !strings.Contains(lines[0], "0x10000 "+filepath.Join(build, "gogo.bin")) {
		t.Fatalf("writes %q", lines)
	}
	checkNoNVSWrite(t, lines)
	cfg, _ := os.ReadFile(filepath.Join(build, "sdkconfig.gogo"))
	if !strings.Contains(string(cfg), "CONFIG_GOGO_NET=y") {
		t.Errorf("local firmware lacks the network:\n%s", cfg)
	}
}

func TestSetupOfflineNeverDownloads(t *testing.T) {
	f := newSetupBench(t, chipCardputer, "")
	calls := localBench(t, f)
	var hits int32
	f.writeRelease()
	t.Setenv("GOGO_RELEASE_URL", f.serve(&hits))
	if err := f.run("-yes", "-skip-wifi", "-offline"); err != nil {
		t.Fatalf("%v\n%s", err, f.out)
	}
	if hits != 0 || len(*calls) != 1 || !strings.Contains((*calls)[0], "-DGOGO_BOARD=cardputer") {
		t.Fatalf("hits %d, idf.py calls %q", hits, *calls)
	}
}

func TestSetupNoFirmwareExplainsBoth(t *testing.T) {
	f := newSetupBench(t, chipStickS3, "")
	err := f.run("-yes", "-skip-wifi", "-release-url", f.serve(nil))
	if err == nil || !strings.Contains(err.Error(), "-release-url") || !strings.Contains(err.Error(), "ESP-IDF") {
		t.Fatalf("err = %v", err)
	}
	g := newSetupBench(t, chipStickS3, "")
	err = g.run("-yes", "-skip-wifi", "-offline")
	if err == nil || !strings.Contains(err.Error(), "ESP-IDF") {
		t.Fatalf("offline err = %v", err)
	}
}

func TestSetupDryRunLocalBuild(t *testing.T) {
	f := newSetupBench(t, chipStickS3, "")
	calls := localBench(t, f)
	if err := f.run("-yes", "-skip-wifi", "-offline", "-dry-run"); err != nil {
		t.Fatalf("%v\n%s", err, f.out)
	}
	if len(*calls) != 0 || len(f.writeFlashLines()) != 0 {
		t.Fatalf("dry run built or flashed: %q", *calls)
	}
	if !strings.Contains(f.out.String(), "idf.py -C platform/esp32s3") {
		t.Errorf("build command not shown:\n%s", f.out)
	}
}

func TestReleaseBase(t *testing.T) {
	t.Setenv("GOGO_RELEASE_URL", "")
	old := version
	defer func() { version = old }()
	version = "dev"
	if got := releaseBase("", ""); got != releaseRepo+"/latest/download" {
		t.Error(got)
	}
	if got := releaseBase("", "v1.2.3"); got != releaseRepo+"/download/v1.2.3" {
		t.Error(got)
	}
	version = "v0.3.0"
	if got := releaseBase("", ""); got != releaseRepo+"/download/v0.3.0" {
		t.Error(got)
	}
	for _, v := range []string{"v0.3.0-4-gabcdef1", "v0.1.0-local", "v0.3.0-dirty"} {
		version = v
		if got := releaseBase("", ""); got != releaseRepo+"/latest/download" {
			t.Errorf("%s: %s", v, got)
		}
	}
	for _, v := range []string{"v1.2.3", "v1.2.3-rc1", "v1.2.3-rc.2", "v1.2.3-beta.1"} {
		if !isReleaseVersion(v) {
			t.Errorf("%s not a release version", v)
		}
	}
	t.Setenv("GOGO_RELEASE_URL", "http://127.0.0.1:1/r")
	if got := releaseBase("", ""); got != "http://127.0.0.1:1/r" {
		t.Error(got)
	}
	if got := releaseBase("/tmp/r", ""); got != "/tmp/r" {
		t.Error(got)
	}
	for _, args := range [][]string{
		{"-version", "latest"},
		{"-version", "v1.0.0", "-release-url", "x"},
		{"-offline", "-release-url", "x"},
		{"-board", "esp32"},
		{"extra"},
	} {
		if _, err := parseSetupArgs(args); err == nil {
			t.Errorf("%q accepted", args)
		}
	}
}

func TestParseSums(t *testing.T) {
	h := strings.Repeat("ab", 32)
	s, err := parseSums([]byte(h + "  a.bin\n" + strings.ToUpper(h) + " *b.bin\n\n"))
	if err != nil || s["a.bin"] != h || s["b.bin"] != h {
		t.Fatalf("%v %v", s, err)
	}
	for _, bad := range []string{"", "xyz  a.bin\n", h + "\n", strings.Repeat("zz", 32) + "  a\n"} {
		if _, err := parseSums([]byte(bad)); err == nil {
			t.Errorf("%q accepted", bad)
		}
	}
}

func TestHelloCompiles(t *testing.T) {
	for board := range setupBoardTitles {
		out := filepath.Join(t.TempDir(), "hello.ggb")
		if err := writeHello(board, out); err != nil {
			t.Fatal(err)
		}
		img, _ := os.ReadFile(out)
		if err := checkImage(img); err != nil {
			t.Fatal(err)
		}
	}
}

func TestParseScan(t *testing.T) {
	text := fmt.Sprintf("%-32s ch %2d %4d dBm secured\n%-32s ch %2d %4d dBm open\n%-32s ch %2d %4d dBm secured\n garbage\n", "Home Net", 6, -50, "Cafe", 11, -70, "Home Net", 1, -80)
	nets := parseScan(text)
	if len(nets) != 2 || nets[0].ssid != "Home Net" || nets[0].open || nets[0].detail != "ch 6 -50 dBm" || nets[1].ssid != "Cafe" || !nets[1].open {
		t.Fatalf("%+v", nets)
	}
}

const testPassword = "correct-horse-battery"

func TestSetupWifiAndFirstProgram(t *testing.T) {
	// Pick network 1, type the password; the first scan fails while the
	// board starts Wi-Fi.
	f := newSetupBench(t, chipStickS3, "y\ny\ny\n1\n"+testPassword+"\n")
	f.scanFails = 1
	f.writeRelease()
	if err := f.run("-release-url", f.release); err != nil {
		t.Fatalf("%v\n%s", err, f.out)
	}
	if f.store["wifi.ssid"] != "Home Net" || f.store["wifi.password"] != testPassword {
		t.Fatalf("store %q", f.store)
	}
	if strings.Join(f.sets, " ") != "wifi.password wifi.ssid" {
		t.Errorf("set order %q", f.sets)
	}
	out := f.out.String()
	for _, want := range []string{
		"[5/6] Setting up Wi-Fi",
		"wifi.ssid=Home Net, wifi.password=(set)",
		"| gogo: wifi connected (Home Net",
		"Wi-Fi works",
		"[6/6] Running a first program",
		"write-flash 0x210000 ",
		"| hello from gogo on the board!",
		"Your M5StickS3 runs gogo",
	} {
		if !strings.Contains(out, want) {
			t.Errorf("output lacks %q:\n%s", want, out)
		}
	}
	checkNoNVSWrite(t, f.writeFlashLines())
	checkNoSecret(t, f)
}

// checkNoSecret: the password is in no output, log or cached file.
func checkNoSecret(t *testing.T, f *setupBench) {
	t.Helper()
	if strings.Contains(f.out.String(), testPassword) {
		t.Error("password in the output")
	}
	if strings.Contains(f.esptoolLog(), testPassword) {
		t.Error("password on an esptool command line")
	}
	filepath.Walk(f.dir, func(p string, info os.FileInfo, err error) error {
		if err == nil && !info.IsDir() {
			if b, _ := os.ReadFile(p); bytes.Contains(b, []byte(testPassword)) {
				t.Errorf("password in %s", p)
			}
		}
		return nil
	})
}

func TestSetupWifiPasswordRefused(t *testing.T) {
	f := newSetupBench(t, chipStickS3, "y\ny\ny\n1\n"+testPassword+"\n")
	f.rejectPassword = true
	f.writeRelease()
	err := f.run("-release-url", f.release)
	if err == nil || !strings.Contains(err.Error(), "cannot write the Wi-Fi password") {
		t.Fatalf("err = %v", err)
	}
	if strings.Contains(err.Error(), testPassword) {
		t.Error("password in the error")
	}
	checkNoSecret(t, f)
}

func TestSetupWifiDryRun(t *testing.T) {
	f := newSetupBench(t, chipCardputer, "y\ny\ny\n1\n"+testPassword+"\n")
	f.writeRelease()
	if err := f.run("-dry-run", "-release-url", f.release); err != nil {
		t.Fatalf("%v\n%s", err, f.out)
	}
	if len(f.store) != 0 || len(f.writeFlashLines()) != 0 {
		t.Fatalf("dry run wrote: store %q, flash %q", f.store, f.writeFlashLines())
	}
	out := f.out.String()
	for _, want := range []string{
		`would set wifi.ssid to "Home Net" and wifi.password (not shown)`,
		"would run: " + filepath.Join(f.dir, "bin", "esptool") + " --port /dev/cu.usbmodem9 --chip esp32s3 --after watchdog-reset write-flash 0x210000 ",
		"Dry run done",
	} {
		if !strings.Contains(out, want) {
			t.Errorf("output lacks %q:\n%s", want, out)
		}
	}
	checkNoSecret(t, f)
}

func TestSetupWifiOpenAndTyped(t *testing.T) {
	// An open network from the list: no password, an old one is removed.
	f := newSetupBench(t, chipStickS3, "y\ny\ny\n2\n")
	f.store["wifi.password"] = "old-password"
	f.writeRelease()
	if err := f.run("-release-url", f.release); err != nil {
		t.Fatalf("%v\n%s", err, f.out)
	}
	if _, ok := f.store["wifi.password"]; ok || f.store["wifi.ssid"] != "Cafe" {
		t.Fatalf("store %q", f.store)
	}
	// A typed name; a too short password is asked again.
	g := newSetupBench(t, chipStickS3, "y\ny\ny\n3\nHidden Lab\ny\nshort\n"+testPassword+"\n")
	g.writeRelease()
	if err := g.run("-release-url", g.release); err != nil {
		t.Fatalf("%v\n%s", err, g.out)
	}
	if g.store["wifi.ssid"] != "Hidden Lab" || g.store["wifi.password"] != testPassword {
		t.Fatalf("store %q", g.store)
	}
	if !strings.Contains(g.out.String(), "8 to 63 characters") {
		t.Errorf("short password accepted:\n%s", g.out)
	}
	checkNoSecret(t, g)
}

func TestSetupSkipWifiKeepsSettings(t *testing.T) {
	f := newSetupBench(t, chipStickS3, "")
	f.store["wifi.ssid"] = "Home Net"
	f.store["wifi.password"] = testPassword
	f.writeRelease()
	if err := f.run("-yes", "-skip-wifi", "-release-url", f.release); err != nil {
		t.Fatalf("%v\n%s", err, f.out)
	}
	if len(f.sets) != 0 || f.store["wifi.password"] != testPassword {
		t.Fatalf("settings touched: %q", f.sets)
	}
	checkNoSecret(t, f)
}

func TestSetupProgramRefused(t *testing.T) {
	f := newSetupBench(t, chipStickS3, "")
	f.boot = []string{"gogo vold: image 1556 bytes, arena 1 bytes, free RAM 1 (largest 1)", "gogo: bad image (unsupported version)"}
	f.writeRelease()
	err := f.run("-yes", "-skip-wifi", "-release-url", f.release)
	if err == nil || !strings.Contains(err.Error(), "unsupported version") || !strings.Contains(err.Error(), "gogo version") {
		t.Fatalf("err = %v", err)
	}
}

func TestConsoleViews(t *testing.T) {
	// Program output, then two config commands back to back and a watch:
	// each exchange gets its own reply, nothing is lost between them.
	f := &setupBench{store: map[string]string{}}
	c := newConsole(f.openConsole())
	defer c.Close()
	for i := 0; i < 20; i++ {
		if err := c.config("set", []string{"k", fmt.Sprint(i)}, io.Discard, time.Second); err != nil {
			t.Fatal(i, err)
		}
	}
	if err := c.config("set", []string{"wifi.ssid", "X"}, io.Discard, time.Second); err != nil {
		t.Fatal(err)
	}
	if !c.watch(time.Second, func(l string) bool { return strings.HasPrefix(l, "gogo: wifi connected (X") }) {
		t.Fatal("the line after the reply was lost")
	}
	if err := c.config("rm", []string{"nope"}, io.Discard, time.Second); err == nil || !strings.Contains(err.Error(), "not set") {
		t.Fatal(err)
	}
}

func TestSetupWindowsSkipsConsole(t *testing.T) {
	f := newSetupBench(t, chipStickS3, "y\ny\n")
	f.env.goos = "windows"
	f.writeRelease()
	if err := f.run("-board", "sticks3", "-release-url", f.release); err == nil || !strings.Contains(err.Error(), "-port COM5") {
		t.Fatalf("no -port on Windows: %v", err)
	}
	f.out.Reset()
	if err := f.run("-board", "sticks3", "-port", "COM5", "-release-url", f.release); err != nil {
		t.Fatalf("%v\n%s", err, f.out)
	}
	if f.opens != 0 || len(f.writeFlashLines()) != 2 || !strings.Contains(f.out.String(), "Wi-Fi from macOS or Linux") {
		t.Fatalf("opens %d, writes %q:\n%s", f.opens, f.writeFlashLines(), f.out)
	}
}
