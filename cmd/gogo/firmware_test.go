package main

import (
	"bytes"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"testing"

	"gogotools/compiler"
)

// writeImg writes a minimal v9 image whose requires word is bits.
func writeImg(t *testing.T, dir, name string, bits uint32) string {
	t.Helper()
	b := compiler.NewImageBuilder()
	f := b.AddFunc("main", 0, 1)
	b.SetFuncCode(f, []uint32{uint32(compiler.OpRet)})
	b.SetEntry(f)
	b.AddExport("main", f)
	b.SetRequires(bits)
	img, err := b.Bytes()
	if err != nil {
		t.Fatal(err)
	}
	p := filepath.Join(dir, name)
	if err := os.WriteFile(p, img, 0o644); err != nil {
		t.Fatal(err)
	}
	return p
}

func fwRun(t *testing.T, args ...string) (string, string, error) {
	t.Helper()
	var out bytes.Buffer
	err := firmwareCmd(args, &out, func(string, ...string) error {
		t.Fatal("must not run idf.py")
		return nil
	})
	b, _ := os.ReadFile(filepath.Join(argOut(args), "sdkconfig.gogo"))
	return out.String(), string(b), err
}

func argOut(args []string) string {
	for i, a := range args {
		if a == "-o" {
			return args[i+1]
		}
	}
	return ""
}

func TestFirmwareUnion(t *testing.T) {
	d := t.TempDir()
	a := writeImg(t, d, "a.gogo", compiler.CapNet)
	b := writeImg(t, d, "b.gogo", compiler.CapPower)
	_, cfg, err := fwRun(t, "-board", "sticks3", "-o", d, a, b)
	if err != nil {
		t.Fatal(err)
	}
	want := "CONFIG_GOGO_NET=y\nCONFIG_GOGO_KBD=n\nCONFIG_GOGO_BATTERY=y\nCONFIG_GOGO_SD=n\nCONFIG_GOGO_FS_EXFAT=n\nCONFIG_GOGO_FONT_CJK=y\nCONFIG_GOGO_IMU=y\nCONFIG_GOGO_IR=y\nCONFIG_GOGO_AUDIO=y\n"
	if !strings.HasSuffix(cfg, want) {
		t.Fatalf("overlay:\n%s", cfg)
	}
}

func TestFirmwareCardputerDefaults(t *testing.T) {
	d := t.TempDir()
	a := writeImg(t, d, "a.gogo", 0)
	out, cfg, err := fwRun(t, "-board", "cardputer", "-o", d, a)
	if err != nil {
		t.Fatal(err)
	}
	want := "CONFIG_GOGO_NET=n\nCONFIG_GOGO_KBD=n\nCONFIG_GOGO_BATTERY=n\nCONFIG_GOGO_SD=y\nCONFIG_GOGO_FS_EXFAT=y\nCONFIG_GOGO_FONT_CJK=y\nCONFIG_GOGO_IMU=y\nCONFIG_GOGO_IR=y\nCONFIG_GOGO_AUDIO=y\n"
	if !strings.HasSuffix(cfg, want) {
		t.Fatalf("overlay:\n%s", cfg)
	}
	wantCmd := "idf.py -C platform/esp32s3 -B " + d + " -DGOGO_BOARD=cardputer -DSDKCONFIG=" + d +
		"/sdkconfig -DSDKCONFIG_DEFAULTS=\"sdkconfig.defaults;sdkconfig.cardputer;" + d + "/sdkconfig.gogo\" build"
	if !strings.Contains(out, wantCmd+"\n") {
		t.Fatalf("command missing:\n%s\nwant %s", out, wantCmd)
	}
}

func TestFirmwareAnyPort(t *testing.T) {
	d := t.TempDir()
	a := writeImg(t, d, "a.gogo", compiler.CapAnyPort)
	_, cfg, err := fwRun(t, "-board", "sticks3", "-o", d, a)
	if err != nil {
		t.Fatal(err)
	}
	// every port the StickS3 has: net, power; not kbd, not sd
	if !strings.Contains(cfg, "CONFIG_GOGO_NET=y\nCONFIG_GOGO_KBD=n\nCONFIG_GOGO_BATTERY=y\n") {
		t.Fatalf("overlay:\n%s", cfg)
	}
	_, cfg, err = fwRun(t, "-board", "cardputer", "-o", d, a)
	if err != nil || !strings.Contains(cfg, "CONFIG_GOGO_NET=y\nCONFIG_GOGO_KBD=y\nCONFIG_GOGO_BATTERY=y\n") {
		t.Fatalf("cardputer: %v\n%s", err, cfg)
	}
}

func TestFirmwareWithWithout(t *testing.T) {
	d := t.TempDir()
	a := writeImg(t, d, "a.gogo", 0)
	_, cfg, err := fwRun(t, "-board", "cardputer", "-o", d, "-with", "net,kbd", "-without", "cjk,exfat", a)
	if err != nil {
		t.Fatal(err)
	}
	want := "CONFIG_GOGO_NET=y\nCONFIG_GOGO_KBD=y\nCONFIG_GOGO_BATTERY=n\nCONFIG_GOGO_SD=y\nCONFIG_GOGO_FS_EXFAT=n\nCONFIG_GOGO_FONT_CJK=n\nCONFIG_GOGO_IMU=y\nCONFIG_GOGO_IR=y\nCONFIG_GOGO_AUDIO=y\n"
	if !strings.HasSuffix(cfg, want) {
		t.Fatalf("overlay:\n%s", cfg)
	}
	// without sd drops exfat too (Kconfig dependency)
	_, cfg, err = fwRun(t, "-board", "cardputer", "-o", d, "-without", "sd", a)
	if err != nil || !strings.Contains(cfg, "CONFIG_GOGO_SD=n\nCONFIG_GOGO_FS_EXFAT=n\n") {
		t.Fatalf("%v\n%s", err, cfg)
	}
	// the same name in -with and -without conflicts
	if _, _, err = fwRun(t, "-board", "cardputer", "-o", d, "-with", "net", "-without", "net", a); err == nil {
		t.Fatal("with and without of the same name should conflict")
	}
}

func TestFirmwareRequiredRemoved(t *testing.T) {
	d := t.TempDir()
	a := writeImg(t, d, "a.gogo", compiler.CapNet|compiler.CapSD)
	for _, f := range []string{"net", "sd"} {
		_, _, err := fwRun(t, "-board", "cardputer", "-o", d, "-without", f, a)
		if err == nil || !strings.Contains(err.Error(), f) || !strings.Contains(err.Error(), "required") {
			t.Fatalf("-without %s: %v", f, err)
		}
	}
}

func TestFirmwareBoardLacks(t *testing.T) {
	d := t.TempDir()
	for _, c := range []struct {
		bits uint32
		name string
	}{{compiler.CapKbd, "kbd"}, {compiler.CapSD, "sd"}} {
		a := writeImg(t, d, "a.gogo", c.bits)
		_, _, err := fwRun(t, "-board", "sticks3", "-o", d, a)
		if err == nil || !strings.Contains(err.Error(), c.name) || !strings.Contains(err.Error(), "sticks3") {
			t.Fatalf("%s: %v", c.name, err)
		}
	}
	a := writeImg(t, d, "a.gogo", 0)
	if _, _, err := fwRun(t, "-board", "sticks3", "-o", d, "-with", "kbd", a); err == nil || !strings.Contains(err.Error(), "kbd") {
		t.Fatalf("-with kbd: %v", err)
	}
}

func TestFirmwareBadInput(t *testing.T) {
	d := t.TempDir()
	junk := filepath.Join(d, "junk.gogo")
	os.WriteFile(junk, []byte("this is not an image at all, no"), 0o644)
	v8 := filepath.Join(d, "v8.gogo")
	img, _ := os.ReadFile(writeImg(t, d, "ok.gogo", 0))
	img[4] = 8
	os.WriteFile(v8, img, 0o644)
	for _, p := range []string{junk, v8, filepath.Join(d, "missing.gogo")} {
		_, _, err := fwRun(t, "-board", "sticks3", "-o", d, p)
		if err == nil || !strings.Contains(err.Error(), filepath.Base(p)) {
			t.Fatalf("%s: %v", p, err)
		}
	}
	if _, _, err := fwRun(t, "-board", "sticks3", "-o", d, junk); !strings.Contains(err.Error(), "not a gogo image") {
		t.Fatal(err)
	}
	if _, _, err := fwRun(t, "-board", "sticks3", "-o", d, v8); !strings.Contains(err.Error(), "version 8") {
		t.Fatal(err)
	}
}

func TestFirmwareArgs(t *testing.T) {
	d := t.TempDir()
	a := writeImg(t, d, "a.gogo", 0)
	for _, args := range [][]string{
		{"-o", d, a},                                      // no board
		{"-board", "x", "-o", d, a},                       // unknown board
		{"-board", "sticks3", "-o", d},                    // no files
		{"-board", "sticks3", "-o", d, "-with", "foo", a}, // unknown feature
	} {
		if _, _, err := fwRun(t, args...); err == nil {
			t.Fatalf("%v: want error", args)
		}
	}
}

func TestFirmwareBuildRuns(t *testing.T) {
	d := t.TempDir()
	a := writeImg(t, d, "a.gogo", 0)
	var got []string
	var out bytes.Buffer
	err := firmwareCmd([]string{"-board", "sticks3", "-o", d, "-build", a}, &out, func(name string, args ...string) error {
		got = append([]string{name}, args...)
		return nil
	})
	if err != nil || len(got) == 0 || got[0] != "idf.py" || got[len(got)-1] != "build" {
		t.Fatalf("%v %v", err, got)
	}
	if got[1] != "-C" || got[2] != "platform/esp32s3" {
		t.Fatalf("%v", got)
	}
}

func TestFirmwareQuotesSpaces(t *testing.T) {
	d := filepath.Join(t.TempDir(), "my build")
	a := writeImg(t, t.TempDir(), "a.gogo", 0)
	out, _, err := fwRun(t, "-board", "cardputer", "-o", d, a)
	if err != nil {
		t.Fatal(err)
	}
	for _, want := range []string{`-B "` + d + `"`, `-DSDKCONFIG="` + d + `/sdkconfig"`,
		`-DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.cardputer;` + d + `/sdkconfig.gogo"`} {
		if !strings.Contains(out, want) {
			t.Fatalf("missing %s in\n%s", want, out)
		}
	}
}

func TestFirmwareStaleSdkconfigRemoved(t *testing.T) {
	d := t.TempDir()
	a := writeImg(t, d, "a.gogo", 0)
	sc := filepath.Join(d, "sdkconfig")
	os.WriteFile(sc, []byte("CONFIG_FOO=1\nCONFIG_GOGO_NET=y\n# CONFIG_GOGO_KBD is not set\n"), 0o644)
	out, _, err := fwRun(t, "-board", "sticks3", "-o", d, a) // net off by default
	if err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(sc); err == nil {
		t.Fatal("stale sdkconfig kept")
	}
	if !strings.Contains(out, "removed") || !strings.Contains(out, "CONFIG_GOGO_NET") {
		t.Fatalf("no notice:\n%s", out)
	}
}

func TestFirmwareMatchingSdkconfigKept(t *testing.T) {
	d := t.TempDir()
	a := writeImg(t, d, "a.gogo", 0)
	sc := filepath.Join(d, "sdkconfig")
	os.WriteFile(sc, []byte("CONFIG_GOGO_NET=n\n# CONFIG_GOGO_KBD is not set\nCONFIG_GOGO_SD=n\n"), 0o644)
	out, _, err := fwRun(t, "-board", "sticks3", "-o", d, a)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(sc); err != nil {
		t.Fatal("matching sdkconfig removed")
	}
	if strings.Contains(out, "removed") {
		t.Fatalf("unexpected notice:\n%s", out)
	}
}

func TestFirmwareLoneDash(t *testing.T) {
	d := t.TempDir()
	a := writeImg(t, d, "a.gogo", 0)
	if _, _, err := fwRun(t, "-board", "sticks3", "-o", d, a, "-"); err == nil {
		t.Fatal("no error for lone -")
	}
	if _, _, err := fwRun(t, "-board", "sticks3", "-o", d, "-", a); err == nil {
		t.Fatal("no error for leading -")
	}
}

func TestFirmwareUnknownRequiresBit(t *testing.T) {
	d := t.TempDir()
	a := writeImg(t, d, "a.gogo", 1<<20)
	if _, _, err := fwRun(t, "-board", "sticks3", "-o", d, a); err == nil || !strings.Contains(err.Error(), "0x100000") {
		t.Fatalf("%v", err)
	}
}

func TestFirmwareWithExfatWithoutSd(t *testing.T) {
	d := t.TempDir()
	a := writeImg(t, d, "a.gogo", 0)
	if _, _, err := fwRun(t, "-board", "cardputer", "-o", d, "-with", "exfat", "-without", "sd", a); err == nil || !strings.Contains(err.Error(), "exfat") {
		t.Fatalf("%v", err)
	}
}

func TestImageRequiresTruncated(t *testing.T) {
	d := t.TempDir()
	img, _ := os.ReadFile(writeImg(t, d, "a.gogo", compiler.CapNet))
	for n := 0; n < len(img); n += 4 {
		if _, err := compiler.ImageRequires(img[:n]); err == nil && n < len(img)-8 {
			// only the full image (or one missing just the export entries) may parse
			t.Fatalf("%d of %d bytes parsed", n, len(img))
		}
	}
	huge := append([]byte(nil), img...)
	for i := 8; i < 24; i++ {
		huge[i] = 0xff
	}
	if _, err := compiler.ImageRequires(huge); err == nil {
		t.Fatal("huge counts parsed")
	}
}

// The Go board table must match board.h and Kconfig.projbuild.
func TestFirmwareTableMatchesSources(t *testing.T) {
	repo, ok := findRepo(".")
	if !ok {
		t.Skip("not in the repository")
	}
	main := filepath.Join(repo, "platform/esp32s3/main")
	kc, err := os.ReadFile(filepath.Join(main, "Kconfig.projbuild"))
	if err != nil {
		t.Fatal(err)
	}
	for _, f := range fwFeatures {
		if !regexp.MustCompile(`(?m)^\s*config ` + strings.TrimPrefix(f.kconf, "CONFIG_") + `\s*$`).Match(kc) {
			t.Errorf("Kconfig has no %s", f.kconf)
		}
		if f.needs != "" {
			dep, _ := fwLookup(f.needs)
			re := regexp.MustCompile(`depends on ` + strings.TrimPrefix(dep.kconf, "CONFIG_"))
			if !re.Match(kc) {
				t.Errorf("Kconfig: %s should depend on %s", f.kconf, dep.kconf)
			}
		}
	}
	// every Kconfig switch must be reachable through gogo firmware
	for _, m := range regexp.MustCompile(`(?m)^\s*config (GOGO_\w+)\s*$`).FindAllSubmatch(kc, -1) {
		found := false
		for _, f := range fwFeatures {
			found = found || f.kconf == "CONFIG_"+string(m[1])
		}
		if !found {
			t.Errorf("Kconfig %s is not in fwFeatures", m[1])
		}
	}
	bh, err := os.ReadFile(filepath.Join(main, "board.h"))
	if err != nil {
		t.Fatal(err)
	}
	board := ""
	seen := map[string]bool{}
	for _, line := range strings.Split(string(bh), "\n") {
		fl := strings.Fields(line)
		if len(fl) >= 3 && fl[0] == "#define" {
			if fl[1] == "BOARD_NAME" {
				board = strings.Trim(fl[2], `"`)
				continue
			}
			for _, f := range fwFeatures {
				if f.hasH == fl[1] && board != "" {
					seen[board+f.name] = true
					if want := fl[2] == "1"; fwBoards[board].has[f.name] != want {
						t.Errorf("board %s feature %s: table %v, board.h %s", board, f.name, fwBoards[board].has[f.name], line)
					}
				}
			}
		}
	}
	for b := range fwBoards {
		for _, f := range fwFeatures {
			if f.hasH != "" && !seen[b+f.name] {
				t.Errorf("board.h has no %s for %s", f.hasH, b)
			}
		}
	}
}

func TestFirmwareImuIr(t *testing.T) {
	d := t.TempDir()
	rx := writeImg(t, d, "rx.gogo", compiler.CapIRRX)
	tx := writeImg(t, d, "tx.gogo", compiler.CapIRTX)
	im := writeImg(t, d, "im.gogo", compiler.CapIMU)
	// the Cardputer has no receiver
	if _, _, err := fwRun(t, "-board", "cardputer", "-o", d, rx); err == nil || !strings.Contains(err.Error(), "irrx") {
		t.Errorf("cardputer irrx: %v", err)
	}
	if _, cfg, err := fwRun(t, "-board", "cardputer", "-o", d, tx); err != nil || !strings.Contains(cfg, "CONFIG_GOGO_IR=y\n") {
		t.Errorf("cardputer irtx: %v\n%s", err, cfg)
	}
	// irtx and irrx share one switch: removing one that the images do not
	// need is still refused while the other is required
	if _, _, err := fwRun(t, "-board", "sticks3", "-o", d, "-without", "irtx", rx); err == nil || !strings.Contains(err.Error(), "irtx") {
		t.Errorf("sticks3 -without irtx with an irrx image: %v", err)
	}
	// nothing requires them: both go together
	if _, cfg, err := fwRun(t, "-board", "sticks3", "-o", d, "-without", "irtx", im); err != nil || !strings.Contains(cfg, "CONFIG_GOGO_IR=n\n") || !strings.Contains(cfg, "CONFIG_GOGO_IMU=y\n") {
		t.Errorf("sticks3 -without irtx: %v\n%s", err, cfg)
	}
	if _, cfg, err := fwRun(t, "-board", "sticks3", "-o", d, "-without", "imu", tx); err != nil || !strings.Contains(cfg, "CONFIG_GOGO_IMU=n\n") || !strings.Contains(cfg, "CONFIG_GOGO_IR=y\n") {
		t.Errorf("sticks3 -without imu: %v\n%s", err, cfg)
	}
	if _, _, err := fwRun(t, "-board", "sticks3", "-o", d, "-without", "imu", im); err == nil {
		t.Error("removing a required imu should fail")
	}
}

func TestFirmwareAudio(t *testing.T) {
	d := t.TempDir()
	mic := writeImg(t, d, "mic.gogo", compiler.CapMic)
	spk := writeImg(t, d, "spk.gogo", compiler.CapSpk)
	im := writeImg(t, d, "im.gogo", compiler.CapIMU)
	for _, board := range []string{"sticks3", "cardputer"} {
		if _, cfg, err := fwRun(t, "-board", board, "-o", d, mic); err != nil || !strings.Contains(cfg, "CONFIG_GOGO_AUDIO=y\n") {
			t.Errorf("%s mic: %v\n%s", board, err, cfg)
		}
		// mic and spk share one switch: an image that needs one keeps both
		if _, _, err := fwRun(t, "-board", board, "-o", d, "-without", "spk", mic); err == nil || !strings.Contains(err.Error(), "GOGO_AUDIO") {
			t.Errorf("%s -without spk with a mic image: %v", board, err)
		}
		if _, cfg, err := fwRun(t, "-board", board, "-o", d, "-without", "mic", im); err != nil || !strings.Contains(cfg, "CONFIG_GOGO_AUDIO=n\n") {
			t.Errorf("%s -without mic: %v\n%s", board, err, cfg)
		}
		if _, _, err := fwRun(t, "-board", board, "-o", d, "-without", "mic", spk); err == nil {
			t.Errorf("%s: removing a required switch should fail", board)
		}
	}
	if _, err := fwPlan("sticks3", 0, []string{"mic"}, []string{"spk"}); err == nil || !strings.Contains(err.Error(), "GOGO_AUDIO") {
		t.Errorf("-with mic -without spk: %v", err)
	}
	on, err := fwPlan("sticks3", 0, nil, []string{"spk"})
	if err != nil || on["mic"] || on["spk"] {
		t.Errorf("-without spk alone: %v %v", on, err)
	}
}

func TestFirmwarePlanSharedIRSwitch(t *testing.T) {
	// an irrx image: -without irtx must name the shared switch, not "required by run-time name"
	_, err := fwPlan("sticks3", compiler.CapIRRX, nil, []string{"irtx"})
	if err == nil || !strings.Contains(err.Error(), "GOGO_IR") || !strings.Contains(err.Error(), "irrx") {
		t.Errorf("-without irtx with an irrx image: %v", err)
	}
	// -with irrx -without irtx contradicts itself through the shared switch
	_, err = fwPlan("sticks3", 0, []string{"irrx"}, []string{"irtx"})
	if err == nil || !strings.Contains(err.Error(), "GOGO_IR") {
		t.Errorf("-with irrx -without irtx: %v", err)
	}
	// -without irtx alone is still fine: both go off
	on, err := fwPlan("sticks3", 0, nil, []string{"irtx"})
	if err != nil || on["irtx"] || on["irrx"] {
		t.Errorf("-without irtx alone: %v %v", on, err)
	}
}
