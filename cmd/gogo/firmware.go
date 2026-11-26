package main

// gogo firmware: picks the firmware feature set for a board from the
// requirement names (drivers and mounts) a set of images and modules carry, writes it as an sdkconfig overlay and
// prints (or runs, with -build) the idf.py command that builds it.

import (
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"sort"
	"strings"

	"gogotools/compiler"
)

const firmwareUsage = "usage: gogo firmware (-board sticks3|cardputer | -port P [-esptool path]) [-o dir] [-build] [-with list] [-without list] files..."

// fwFeature is one firmware feature, the same for every board.
type fwFeature struct {
	name  string // the name given to -with / -without
	kconf string // Kconfig symbol
	// provides lists the requirement names (port drivers, mount prefixes)
	// the feature builds; none: never required (a default-on detail)
	provides []string
	onBy     bool   // on by default whenever the board has it
	needs    string // another feature this one depends on (Kconfig depends on)
	hasH     string // board.h macro that says whether the board has it, if any
	// offDefaults is an sdkconfig fragment (in platform/esp32s3) added to
	// SDKCONFIG_DEFAULTS when the feature is off: IDF settings that only
	// the feature needs, put back to IDF's own defaults (sdkconfig.defaults
	// holds the default build's, with every default-on feature on).
	offDefaults string
}

// fwFeatures lists the features in the order of the overlay lines.
//
// This table and fwBoards below are the single place in Go that knows the
// features and boards. They MUST match platform/esp32s3/main/board.h
// (HAS_KBD, HAS_SD, HAS_IMU, HAS_IR_TX, HAS_IR_RX, HAS_MIC, HAS_SPK per board) and platform/esp32s3/main/Kconfig.projbuild
// (the GOGO_* symbols; GOGO_FS_EXFAT depends on GOGO_SD, GOGO_TLS on GOGO_NET);
// TestFirmwareTableMatchesSources checks both. Change them together.
var fwFeatures = []fwFeature{
	{name: "net", kconf: "CONFIG_GOGO_NET", provides: []string{"netif", "udp", "tcp", "tcp_listen"}},
	{name: "kbd", kconf: "CONFIG_GOGO_KBD", provides: []string{"kbd"}, hasH: "HAS_KBD"},
	{name: "power", kconf: "CONFIG_GOGO_BATTERY", provides: []string{"power"}},
	{name: "sd", kconf: "CONFIG_GOGO_SD", provides: []string{"/sd"}, onBy: true, hasH: "HAS_SD"},
	{name: "exfat", kconf: "CONFIG_GOGO_FS_EXFAT", onBy: true, needs: "sd"},
	{name: "cjk", kconf: "CONFIG_GOGO_FONT_CJK", onBy: true},
	{name: "imu", kconf: "CONFIG_GOGO_IMU", provides: []string{"imu"}, onBy: true, hasH: "HAS_IMU"},
	// irtx and irrx are two features of one switch (GOGO_IR builds both
	// drivers; the board's HAS_IR_RX says whether it has a receiver).
	{name: "irtx", kconf: "CONFIG_GOGO_IR", provides: []string{"ir_tx"}, onBy: true, hasH: "HAS_IR_TX"},
	{name: "irrx", kconf: "CONFIG_GOGO_IR", provides: []string{"ir_rx"}, onBy: true, hasH: "HAS_IR_RX"},
	// mic and speaker likewise share GOGO_AUDIO (one codec and I2S bus, half duplex).
	{name: "mic", kconf: "CONFIG_GOGO_AUDIO", provides: []string{"mic"}, onBy: true, hasH: "HAS_MIC"},
	{name: "speaker", kconf: "CONFIG_GOGO_AUDIO", provides: []string{"speaker"}, onBy: true, hasH: "HAS_SPK"},
	// tls is the TLS client over the net stack (mbedTLS): it depends on net.
	// Its mbedTLS settings (-O2, the PSRAM allocator, restartable ECC) would
	// otherwise also apply to Wi-Fi's WPA crypto: sdkconfig.tls-off undoes them.
	{name: "tls", kconf: "CONFIG_GOGO_TLS", provides: []string{"tls"}, onBy: true, needs: "net", offDefaults: "sdkconfig.tls-off"},
	// screen is the text grid port on the LCD (runtime/screen); small, so on
	// by default.
	{name: "screen", kconf: "CONFIG_GOGO_SCREEN", provides: []string{"screen"}, onBy: true},
	// float is floating point in the VM and the float library (requirement
	// "float"): like net, built only when an image needs it (or -with).
	{name: "float", kconf: "CONFIG_GOGO_FLOAT", provides: []string{"float"}},
}

// fwBase lists the requirement names every firmware provides, whatever
// its features (platform/esp32s3/main/platform_esp.c esp_ports, esp_mounts).
var fwBase = []string{"gpio", "/flash"}

// fwProvides reports whether feature f builds requirement name n.
func fwProvides(f fwFeature, n string) bool {
	for _, p := range f.provides {
		if p == n {
			return true
		}
	}
	return false
}

// fwBoard describes one board: its SDKCONFIG_DEFAULTS overlay (see
// platform/esp32s3/README.md) and the features it has.
type fwBoard struct {
	overlay string
	has     map[string]bool
}

func fwHas(names ...string) map[string]bool {
	m := map[string]bool{}
	for _, n := range names {
		m[n] = true
	}
	return m
}

var fwBoards = map[string]fwBoard{
	"sticks3":   {overlay: "sdkconfig.sticks3", has: fwHas("net", "power", "cjk", "imu", "irtx", "irrx", "mic", "speaker", "tls", "screen", "float")},
	"cardputer": {overlay: "sdkconfig.cardputer", has: fwHas("net", "kbd", "power", "sd", "exfat", "cjk", "imu", "irtx", "mic", "speaker", "tls", "screen", "float")},
}

func fwLookup(name string) (fwFeature, bool) {
	for _, f := range fwFeatures {
		if f.name == name {
			return f, true
		}
	}
	return fwFeature{}, false
}

func fwNames() string {
	var n []string
	for _, f := range fwFeatures {
		n = append(n, f.name)
	}
	return strings.Join(n, " ")
}

// fwPlan computes which features are on for board, given the requirement
// names of the files and the -with / -without lists.
func fwPlan(board string, req []string, with, without []string) (map[string]bool, error) {
	has := func(f fwFeature) bool { return fwBoards[board].has[f.name] }
	required := map[string]bool{}
	reqBy := map[string]bool{} // features some image requires directly
	for _, n := range req {
		found := false
		for _, b := range fwBase {
			found = found || b == n
		}
		for _, f := range fwFeatures {
			if fwProvides(f, n) {
				required[f.name], reqBy[f.name], found = true, true, true
			}
		}
		if !found {
			return nil, fmt.Errorf("gogo firmware: the images require %q, which no gogo firmware provides", n)
		}
	}
	// features that share one Kconfig switch are one build option: what one
	// of them needs, the others (that the board has) need too
	for _, f := range fwFeatures {
		for _, g := range fwFeatures {
			if g.kconf == f.kconf && required[g.name] && has(f) {
				required[f.name] = true
			}
		}
	}
	on := map[string]bool{}
	for _, f := range fwFeatures {
		if required[f.name] && !has(f) {
			return nil, fmt.Errorf("gogo firmware: the images require %s, which board %s does not have", f.name, board)
		}
		on[f.name] = has(f) && (required[f.name] || f.onBy)
	}
	for _, n := range with {
		f, ok := fwLookup(n)
		if !ok {
			return nil, fmt.Errorf("gogo firmware: unknown feature %q in -with (features: %s)", n, fwNames())
		}
		if !has(f) {
			return nil, fmt.Errorf("gogo firmware: -with %s: board %s does not have %s", n, board, n)
		}
		on[n] = true
	}
	for _, n := range with { // an explicit -with must not be undone by its switch partner's -without
		f, _ := fwLookup(n)
		for _, w := range without {
			if g, ok := fwLookup(w); ok && g.kconf == f.kconf && g.name != f.name {
				return nil, fmt.Errorf("gogo firmware: -with %s and -without %s conflict: they share the %s switch", n, w, f.kconf)
			}
		}
	}
	for _, n := range without {
		if _, ok := fwLookup(n); !ok {
			return nil, fmt.Errorf("gogo firmware: unknown feature %q in -without (features: %s)", n, fwNames())
		}
		for _, w := range with {
			if w == n {
				return nil, fmt.Errorf("gogo firmware: %s is in both -with and -without", n)
			}
		}
		if required[n] {
			f, _ := fwLookup(n)
			if !reqBy[n] {
				for _, g := range fwFeatures { // required only through its switch partner
					if g.name != n && g.kconf == f.kconf && reqBy[g.name] {
						return nil, fmt.Errorf("gogo firmware: cannot remove %s: it shares the %s switch with %s, which the images require", n, f.kconf, g.name)
					}
				}
			}
			return nil, fmt.Errorf("gogo firmware: cannot remove %s: it is required by the images", n)
		}
		on[n] = false
	}
	for _, f := range fwFeatures { // one switch, one value: off if any of its features is off
		for _, g := range fwFeatures {
			if g.kconf == f.kconf && has(g) && !on[g.name] {
				on[f.name] = false
			}
		}
	}
	for _, f := range fwFeatures { // Kconfig depends-on, in table order
		if f.needs != "" && !on[f.needs] {
			for _, w := range with {
				if w == f.name {
					return nil, fmt.Errorf("gogo firmware: -with %s needs %s, which is off", f.name, f.needs)
				}
			}
			on[f.name] = false
		}
	}
	return on, nil
}

// fwOverlay renders the sdkconfig fragment.
func fwOverlay(board string, on map[string]bool) string {
	var b strings.Builder
	fmt.Fprintf(&b, "# gogo firmware overlay for %s (generated by `gogo firmware`)\n", board)
	done := map[string]bool{} // a switch shared by several features is written once
	for _, f := range fwFeatures {
		if done[f.kconf] {
			continue
		}
		done[f.kconf] = true
		v := "n"
		if on[f.name] {
			v = "y"
		}
		fmt.Fprintf(&b, "%s=%s\n", f.kconf, v)
	}
	return b.String()
}

// fwDefaults is the SDKCONFIG_DEFAULTS list: the common defaults, the
// board's overlay, the fragment of every feature that is off and has one,
// then the generated overlay (the GOGO switches, last so they win).
func fwDefaults(boardOverlay string, on map[string]bool, cfg string) string {
	list := []string{"sdkconfig.defaults", boardOverlay}
	for _, f := range fwFeatures {
		if f.offDefaults != "" && !on[f.name] {
			list = append(list, f.offDefaults)
		}
	}
	return strings.Join(append(list, cfg), ";")
}

var fwCfgRe = regexp.MustCompile(`^(?:# )?(CONFIG_GOGO_\w+)(?:=(\S+)| is not set)$`)

// fwCfgValues maps CONFIG_GOGO_* symbols to "y" / "n" in an sdkconfig or an
// overlay (a "# X is not set" line means n).
func fwCfgValues(text string) map[string]string {
	m := map[string]string{}
	for _, line := range strings.Split(text, "\n") {
		if g := fwCfgRe.FindStringSubmatch(strings.TrimSpace(line)); g != nil {
			if g[2] == "" {
				g[2] = "n"
			}
			m[g[1]] = g[2]
		}
	}
	return m
}

// fwStaleSdkconfig returns the CONFIG_GOGO_* symbols whose value in the
// sdkconfig file differs from the overlay; nil if there is no file.
func fwStaleSdkconfig(path, overlay string) []string {
	b, err := os.ReadFile(path)
	if err != nil {
		return nil
	}
	old, want := fwCfgValues(string(b)), fwCfgValues(overlay)
	var diff []string
	for _, f := range fwFeatures {
		if o, ok := old[f.kconf]; ok && o != want[f.kconf] {
			diff = append(diff, f.kconf)
		}
	}
	return diff
}

func splitList(s string) []string {
	var out []string
	for _, p := range strings.Split(s, ",") {
		if p = strings.TrimSpace(p); p != "" {
			out = append(out, p)
		}
	}
	return out
}

// firmwareCmd implements `gogo firmware`. runIdf runs the build for -build.
// detectBoard names the board on port from its chip: one read-only
// esptool command (chip-id), whose banner says whether the ESP32-S3 has
// embedded PSRAM (chipInfo.guessBoard). A variable so tests replace it.
var detectBoard = func(esptool, port string) (string, error) {
	if esptool == "" {
		var err error
		if esptool, err = findEsptool(); err != nil {
			return "", err
		}
	}
	out, err := exec.Command(esptool, "--port", port, "--after", "watchdog-reset", "chip-id").CombinedOutput()
	if err != nil {
		return "", fmt.Errorf("gogo firmware: esptool cannot read the chip on %s (%v); pass -board", port, err)
	}
	chip, err := parseChip(string(out))
	if err != nil {
		return "", fmt.Errorf("gogo firmware: %v; pass -board", err)
	}
	if !chip.isS3() {
		return "", fmt.Errorf("gogo firmware: the chip on %s is %s, not an ESP32-S3", port, chip.chip)
	}
	b := chip.guessBoard()
	if b == "" {
		return "", fmt.Errorf("gogo firmware: esptool did not say whether the chip on %s has PSRAM; pass -board", port)
	}
	return b, nil
}

func firmwareCmd(args []string, out io.Writer, runIdf func(name string, args ...string) error) error {
	fs := newFlags("firmware")
	board := fs.String("board", "", "")
	dir := fs.String("o", "", "")
	doBuild := fs.Bool("build", false, "")
	withS := fs.String("with", "", "")
	withoutS := fs.String("without", "", "")
	port := fs.String("port", "", "")
	esptool := fs.String("esptool", "", "")
	files, err := parseFlags(fs, args, firmwareUsage)
	if err != nil {
		return err
	}
	if *board == "" && *port != "" {
		if *board, err = detectBoard(*esptool, *port); err != nil {
			return err
		}
		fmt.Fprintf(out, "board: %s (detected on %s)\n", *board, *port)
	}
	for _, a := range files {
		if a == "-" {
			return errors.New("gogo firmware: unexpected argument \"-\"\n" + firmwareUsage)
		}
	}
	fwb, ok := fwBoards[*board]
	if !ok {
		return fmt.Errorf("gogo firmware: -board must be one of %s\n%s", boardNames(), firmwareUsage)
	}
	if len(files) == 0 {
		return errors.New("gogo firmware: no image or module files\n" + firmwareUsage)
	}
	var req []string
	for _, f := range files {
		img, err := os.ReadFile(f)
		if err != nil {
			return err
		}
		names, err := compiler.ImageRequires(img)
		if err != nil {
			return fmt.Errorf("gogo firmware: %s: %v", f, err)
		}
		req = append(req, names...)
	}
	on, err := fwPlan(*board, req, splitList(*withS), splitList(*withoutS))
	if err != nil {
		return err
	}
	if *dir == "" {
		*dir = "build-fw-" + *board
	}
	abs, err := filepath.Abs(*dir)
	if err != nil {
		return err
	}
	if err := os.MkdirAll(abs, 0o755); err != nil {
		return err
	}
	cfg := filepath.Join(abs, "sdkconfig.gogo")
	overlay := fwOverlay(*board, on)
	// ESP-IDF lets an existing sdkconfig win over SDKCONFIG_DEFAULTS, so a
	// generated sdkconfig with other GOGO switches would silently override
	// the new feature set. This tool owns the directory: drop it.
	stale := fwStaleSdkconfig(filepath.Join(abs, "sdkconfig"), overlay)
	if len(stale) > 0 {
		if err := os.Remove(filepath.Join(abs, "sdkconfig")); err != nil {
			return err
		}
		fmt.Fprintf(out, "removed %s: it differed from the new features (%s)\n", filepath.Join(abs, "sdkconfig"), strings.Join(stale, ", "))
	}
	if err := os.WriteFile(cfg, []byte(overlay), 0o644); err != nil {
		return err
	}
	idf := []string{"-C", "platform/esp32s3", "-B", abs, "-DGOGO_BOARD=" + *board,
		"-DSDKCONFIG=" + filepath.Join(abs, "sdkconfig"),
		"-DSDKCONFIG_DEFAULTS=" + fwDefaults(fwb.overlay, on, cfg), "build"}
	var quoted []string
	for _, a := range idf {
		quoted = append(quoted, shQuote(a))
	}
	fmt.Fprintf(out, "wrote %s\nfeatures: %s\n", cfg, fwSummary(on))
	fmt.Fprintf(out, "run from the gogo repository root (needs `. ~/esp/esp-idf/export.sh`):\n")
	fmt.Fprintf(out, "idf.py %s\n", strings.Join(quoted, " "))
	if *doBuild {
		return runIdf("idf.py", idf...)
	}
	return nil
}

// shQuote quotes a command line argument for a POSIX shell: arguments of
// the form -Dname=value keep the name outside the double quotes.
func shQuote(a string) string {
	safe := func(s string) bool {
		for _, r := range s {
			if !(r >= 'a' && r <= 'z' || r >= 'A' && r <= 'Z' || r >= '0' && r <= '9' || strings.ContainsRune("_@%+=:,./-", r)) {
				return false
			}
		}
		return true
	}
	if safe(a) {
		return a
	}
	pre := ""
	if strings.HasPrefix(a, "-D") {
		if i := strings.Index(a, "="); i > 0 {
			pre, a = a[:i+1], a[i+1:]
		}
	}
	return pre + `"` + strings.NewReplacer(`\`, `\\`, `"`, `\"`, "$", `\$`, "`", "\\`").Replace(a) + `"`
}

func boardNames() string {
	var n []string
	for b := range fwBoards {
		n = append(n, b)
	}
	sort.Strings(n)
	return strings.Join(n, ", ")
}

func fwSummary(on map[string]bool) string {
	var s []string
	for _, f := range fwFeatures {
		if on[f.name] {
			s = append(s, f.name)
		}
	}
	if len(s) == 0 {
		return "(none)"
	}
	return strings.Join(s, " ")
}

// runIdfInRepo runs idf.py from the gogo repository root (the printed
// command uses the repo-relative -C platform/esp32s3).
func runIdfInRepo(name string, args ...string) error {
	wd, _ := os.Getwd()
	repo, ok := findRepo(wd)
	if !ok {
		return errors.New("gogo firmware: -build must run inside the gogo repository")
	}
	cmd := exec.Command(name, args...)
	cmd.Dir = repo
	cmd.Stdin, cmd.Stdout, cmd.Stderr = os.Stdin, os.Stdout, os.Stderr
	return cmd.Run()
}
