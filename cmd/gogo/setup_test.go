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
	"sync/atomic"
	"testing"
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
	good := []flashWrite{w(0x0, 0x5000), w(0x8000, 0xC00), w(0x10000, 0x110000)}
	if err := checkWrites(good, gogoParts, 0x8000); err != nil {
		t.Fatal(err)
	}
	for name, ws := range map[string][]flashWrite{
		"write at NVS":           {w(0x9000, 0x100)},
		"write ends in NVS":      {w(0x8000, 0x1001)},
		"write starts in NVS":    {w(0xE000, 0x2000)},
		"bootloader too big":     {w(0x0, 0x8001)},
		"outside app partitions": {w(0x210000, 0x100)},
		"into the file system":   {w(0x310000, 0x1000)},
		"overlapping writes":     {w(0x10000, 0x2000), w(0x11000, 0x100)},
		"app past its partition": {w(0x10000, 0x200001)},
	} {
		if err := checkWrites(ws, gogoParts, 0x8000); err == nil {
			t.Errorf("%s: accepted", name)
		}
	}
	if err := checkWrites(good, gogoParts[1:], 0x8000); err == nil {
		t.Error("no NVS partition: accepted")
	}
}

func TestCheckEsptoolArgs(t *testing.T) {
	dir := t.TempDir()
	small := filepath.Join(dir, "small.bin")
	os.WriteFile(small, make([]byte, 100), 0o644)
	big := filepath.Join(dir, "big.bin")
	os.WriteFile(big, make([]byte, 0x1001), 0o644)
	base := []string{"--chip", "esp32s3", "--port", "/dev/x", "--after", "watchdog-reset"}
	ok := [][]string{
		append(base, "read-flash", "0x8000", "3072", small),
		append(base, "write-flash", "--flash-mode", "dio", "--flash-freq", "80m", "--flash-size", "8MB", "0x0", small, "0x8000", small, "0x10000", big),
		append(base, "write-flash", "0x210000", small),
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
		append(base, "write-flash", "--erase-all", "0x0", small),
		append(base, "write-flash", "-e", "0x0", small),
		append(base, "merge-bin", "-o", "x.bin", "0x0", small),
		append(base, "write-flash", "0x9000", small),
		append(base, "write-flash", "0xA000", small),
		append(base, "write-flash", "0x8000", big), // reaches the NVS sector
		append(base, "write-flash", "0x0", small, "0xE000", small),
		append(base, "write-flash", "0x0"),
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

const fakeEsptool = `#!/bin/sh
echo "$*" >> "$FAKE_ESPTOOL_LOG"
if [ "$1" = version ]; then echo "esptool v${FAKE_ESPTOOL_VERSION:-5.6.0}"; exit 0; fi
for a; do last="$a"; done
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
		openPort: func(string) (io.ReadWriteCloser, error) { return nil, fmt.Errorf("no serial port in this test") },
		client:   http.DefaultClient,
		cacheDir: filepath.Join(dir, "cache"),
		repoDir:  func() (string, bool) { return "", false },
		runIdf: func(localBuild, []string) error {
			t.Fatal("unexpected local build")
			return nil
		},
	}
	f.env.in = bufio.NewReader(f.env.stdin)
	return f
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
	for _, l := range lines {
		fields := strings.Fields(l)
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
	if l := f.writeFlashLines(); len(l) != 1 || !strings.Contains(l[0], filepath.Join(f.env.cacheDir, "release")) {
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
	if len(lines) != 1 || !strings.Contains(lines[0], "0x10000 "+filepath.Join(build, "gogo.bin")) {
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
		out := filepath.Join(t.TempDir(), "hello.gogo")
		if err := writeHello(board, out); err != nil {
			t.Fatal(err)
		}
		img, _ := os.ReadFile(out)
		if err := checkImage(img); err != nil {
			t.Fatal(err)
		}
	}
}
