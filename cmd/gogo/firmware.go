package main

// gogo firmware: picks the firmware feature set for a board from what a set
// of images and modules require, writes it as an sdkconfig overlay and
// prints (or runs, with -build) the idf.py command that builds it.

import (
	"errors"
	"flag"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"sort"
	"strings"

	"gogo/compiler"
)

const firmwareUsage = "usage: gogo firmware -board sticks3|cardputer [-o dir] [-build] [-with list] [-without list] files..."

// fwFeature is one firmware feature, the same for every board.
type fwFeature struct {
	name  string // the name given to -with / -without
	kconf string // Kconfig symbol
	cap   uint32 // requires bit; 0: never required (a default-on detail)
	port  bool   // covered by CapAnyPort
	onBy  bool   // on by default whenever the board has it
	needs string // another feature this one depends on (Kconfig depends on)
	hasH  string // board.h macro that says whether the board has it, if any
}

// fwFeatures lists the features in the order of the overlay lines.
//
// This table and fwBoards below are the single place in Go that knows the
// features and boards. They MUST match platform/esp32s3/main/board.h
// (HAS_KBD, HAS_SD, HAS_IMU, HAS_IR_TX, HAS_IR_RX per board) and platform/esp32s3/main/Kconfig.projbuild
// (the GOGO_* symbols and GOGO_FS_EXFAT depends on GOGO_SD);
// TestFirmwareTableMatchesSources checks both. Change them together.
var fwFeatures = []fwFeature{
	{name: "net", kconf: "CONFIG_GOGO_NET", cap: compiler.CapNet, port: true},
	{name: "kbd", kconf: "CONFIG_GOGO_KBD", cap: compiler.CapKbd, port: true, hasH: "HAS_KBD"},
	{name: "power", kconf: "CONFIG_GOGO_BATTERY", cap: compiler.CapPower, port: true},
	{name: "sd", kconf: "CONFIG_GOGO_SD", cap: compiler.CapSD, onBy: true, hasH: "HAS_SD"},
	{name: "exfat", kconf: "CONFIG_GOGO_FS_EXFAT", onBy: true, needs: "sd"},
	{name: "cjk", kconf: "CONFIG_GOGO_FONT_CJK", onBy: true},
	{name: "imu", kconf: "CONFIG_GOGO_IMU", cap: compiler.CapIMU, onBy: true, hasH: "HAS_IMU"},
	// irtx and irrx are two capabilities of one switch (GOGO_IR builds both
	// drivers; the board's HAS_IR_RX says whether it has a receiver).
	{name: "irtx", kconf: "CONFIG_GOGO_IR", cap: compiler.CapIRTX, onBy: true, hasH: "HAS_IR_TX"},
	{name: "irrx", kconf: "CONFIG_GOGO_IR", cap: compiler.CapIRRX, onBy: true, hasH: "HAS_IR_RX"},
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
	"sticks3":   {overlay: "sdkconfig.sticks3", has: fwHas("net", "power", "cjk", "imu", "irtx", "irrx")},
	"cardputer": {overlay: "sdkconfig.cardputer", has: fwHas("net", "kbd", "power", "sd", "exfat", "cjk", "imu", "irtx")},
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

// fwPlan computes which features are on for board, given the requires word
// of the files and the -with / -without lists.
func fwPlan(board string, req uint32, with, without []string) (map[string]bool, error) {
	has := func(f fwFeature) bool { return fwBoards[board].has[f.name] }
	const known = compiler.CapNet | compiler.CapKbd | compiler.CapPower | compiler.CapSD | compiler.CapIMU | compiler.CapIRTX | compiler.CapIRRX | compiler.CapAnyPort
	if bad := req &^ known; bad != 0 {
		return nil, fmt.Errorf("gogo firmware: the images require capability bits %#x that this gogo does not know", bad)
	}
	required := map[string]bool{}
	for _, f := range fwFeatures {
		if f.cap != 0 && (req&f.cap != 0 || (req&compiler.CapAnyPort != 0 && f.port && has(f))) {
			required[f.name] = true
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
			if f.cap&req == 0 {
				for _, g := range fwFeatures { // required only through its switch partner
					if g.name != n && g.kconf == f.kconf && g.cap&req != 0 {
						return nil, fmt.Errorf("gogo firmware: cannot remove %s: it shares the %s switch with %s, which the images require", n, f.kconf, g.name)
					}
				}
				return nil, fmt.Errorf("gogo firmware: cannot remove %s: required (an image opens ports by run-time name)", n)
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
func firmwareCmd(args []string, out io.Writer, runIdf func(name string, args ...string) error) error {
	fs := flag.NewFlagSet("firmware", flag.ContinueOnError)
	fs.SetOutput(io.Discard)
	board := fs.String("board", "", "")
	dir := fs.String("o", "", "")
	doBuild := fs.Bool("build", false, "")
	withS := fs.String("with", "", "")
	withoutS := fs.String("without", "", "")
	if err := fs.Parse(args); err != nil {
		return errors.New(firmwareUsage)
	}
	// flags may follow the files
	files := fs.Args()
	for len(files) > 0 {
		var rest []string
		for i, a := range files {
			if a == "-" {
				return errors.New("gogo firmware: unexpected argument \"-\"\n" + firmwareUsage)
			}
			if strings.HasPrefix(a, "-") {
				if err := fs.Parse(files[i:]); err != nil {
					return errors.New(firmwareUsage)
				}
				rest = append(files[:i:i], fs.Args()...)
				break
			}
		}
		if rest == nil {
			break
		}
		files = rest
	}
	fwb, ok := fwBoards[*board]
	if !ok {
		return fmt.Errorf("gogo firmware: -board must be one of %s\n%s", boardNames(), firmwareUsage)
	}
	if len(files) == 0 {
		return errors.New("gogo firmware: no image or module files\n" + firmwareUsage)
	}
	var req uint32
	for _, f := range files {
		img, err := os.ReadFile(f)
		if err != nil {
			return err
		}
		bits, err := compiler.ImageRequires(img)
		if err != nil {
			return fmt.Errorf("gogo firmware: %s: %v", f, err)
		}
		req |= bits
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
		"-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;" + fwb.overlay + ";" + cfg, "build"}
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
