package main

// gogo firmware: picks the firmware feature set for a board from the
// requirement names (drivers and mounts) a set of images and modules carry, writes it as an sdkconfig overlay and
// prints (or runs, with -build) the idf.py command that builds it.

import (
	"encoding/json"
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

const firmwareUsage = "usage: gogo firmware (-board sticks3|cardputer | -port P [-esptool path]) [-o dir] [-build] [-with list] [-without list] [-component dir]... files..."

// fwFeature is one firmware feature, the same for every board.
type fwFeature struct {
	name  string // the name given to -with / -without
	kconf string // Kconfig symbol
	// provides lists the requirement names (port drivers, mount prefixes)
	// the feature builds; none: never required (a default-on detail)
	provides []string
	onBy     bool     // on by default whenever the board has it
	needs    []string // other features this one depends on (Kconfig depends on)
	hasH     string   // board.h macro that says whether the board has it, if any
	// offDefaults is an sdkconfig fragment (in platform/esp32s3) added to
	// SDKCONFIG_DEFAULTS when the feature is off: IDF settings that only
	// the feature needs, put back to IDF's own defaults (sdkconfig.defaults
	// holds the default build's, with every default-on feature on).
	offDefaults string
	// onDefaults is an sdkconfig fragment (in platform/esp32s3) added when
	// the feature is on, before the board's overlay: IDF settings of a
	// feature kept out of sdkconfig.defaults (sdkconfig.ble turns Bluetooth
	// on; the default build lists it too: platform/esp32s3/CMakeLists.txt,
	// tools/release.sh).
	onDefaults string
}

// fwFeatures lists the features in the order of the overlay lines.
//
// This table and fwBoards below are the single place in Go that knows the
// features and boards. They MUST match platform/esp32s3/main/board.h
// (HAS_KBD, HAS_SD, HAS_IMU, HAS_IR_TX, HAS_IR_RX, HAS_MIC, HAS_SPK per board) and platform/esp32s3/main/Kconfig.projbuild
// (the GOGO_* symbols; GOGO_FS_EXFAT depends on GOGO_SD, GOGO_TLS and GOGO_DIST on GOGO_NET,
// GOGO_BLE_PROV on GOGO_BLE and GOGO_NET);
// TestFirmwareTableMatchesSources checks both. Change them together.
var fwFeatures = []fwFeature{
	{name: "net", kconf: "CONFIG_GOGO_NET", provides: []string{"netif", "udp", "tcp", "tcp_listen"}},
	{name: "kbd", kconf: "CONFIG_GOGO_KBD", provides: []string{"kbd"}, hasH: "HAS_KBD"},
	{name: "power", kconf: "CONFIG_GOGO_BATTERY", provides: []string{"power"}},
	{name: "sd", kconf: "CONFIG_GOGO_SD", provides: []string{"/sd"}, onBy: true, hasH: "HAS_SD"},
	{name: "exfat", kconf: "CONFIG_GOGO_FS_EXFAT", onBy: true, needs: []string{"sd"}},
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
	{name: "tls", kconf: "CONFIG_GOGO_TLS", provides: []string{"tls"}, onBy: true, needs: []string{"net"}, offDefaults: "sdkconfig.tls-off"},
	// screen is the text grid port on the LCD (runtime/screen); small, so on
	// by default.
	{name: "screen", kconf: "CONFIG_GOGO_SCREEN", provides: []string{"screen"}, onBy: true},
	// node is the port of distributed actors (runtime/dist, gogo/node), over
	// the net stack's tcp: it depends on net.
	{name: "node", kconf: "CONFIG_GOGO_DIST", provides: []string{"node"}, onBy: true, needs: []string{"net"}},
	// float is floating point in the VM and the float library (requirement
	// "float"): like net, built only when an image needs it (or -with).
	{name: "float", kconf: "CONFIG_GOGO_FLOAT", provides: []string{"float"}},
	// ble is the Bluetooth LE peripheral port (runtime/ble, NimBLE): on by
	// default on both boards; its IDF settings (Bluetooth itself) are in
	// sdkconfig.ble, which GOGO_BLE depends on.
	{name: "ble", kconf: "CONFIG_GOGO_BLE", provides: []string{"ble"}, onBy: true, onDefaults: "sdkconfig.ble"},
	// bleprov is Wi-Fi provisioning over BLE (hold the setup key at boot;
	// main/ble_prov.c): firmware only, no requirement name. On with ble
	// whenever there is Wi-Fi: it runs on ble's NimBLE (sdkconfig.ble,
	// which also holds its protocomm lines) and writes the wifi.* keys.
	{name: "bleprov", kconf: "CONFIG_GOGO_BLE_PROV", onBy: true, needs: []string{"ble", "net"}},
}

// fwTestKconf lists the Kconfig switches for test builds only, which
// gogo firmware always writes as n (main/Kconfig.projbuild), and a reused
// build directory that has one on is rebuilt from scratch.
var fwTestKconf = []string{"CONFIG_GOGO_PROV_DRYRUN", "CONFIG_GOGO_PROV_TEST_ENTER"}

// fwBase lists the requirement names every firmware provides, whatever
// its features (platform/esp32s3/main/platform_esp.c esp_ports, esp_mounts).
var fwBase = []string{"gpio", "/flash"}

// fwBoardPortsKconf is the switch of the board-port layer (gogo.h
// GOGO_BOARD_PORT): on exactly when -component gives driver components.
const fwBoardPortsKconf = "CONFIG_GOGO_BOARD_PORTS"

// fwBoardPortNamesKconf holds the components' port names, which the
// firmware checks at boot (runtime/board/board_ports.c board_ports_expect).
const fwBoardPortNamesKconf = "CONFIG_GOGO_BOARD_PORT_NAMES"

// fwComponent is a driver component given with -component: an ESP-IDF
// component directory with a gogo-driver.json naming the requirement names
// (board ports) it provides, {"format": 1, "ports": ["acme.dice"]}.
type fwComponent struct {
	dir   string // absolute
	ports []string
}

// fwReadComponent reads -component dir.
func fwReadComponent(dir string) (fwComponent, error) {
	abs, err := filepath.Abs(dir)
	if err != nil {
		return fwComponent{}, err
	}
	if strings.Contains(abs, ";") {
		return fwComponent{}, fmt.Errorf("gogo firmware: -component %s: a path with ';' (EXTRA_COMPONENT_DIRS is a CMake list)", dir)
	}
	if err := fwComponentName(filepath.Base(abs)); err != nil {
		return fwComponent{}, fmt.Errorf("gogo firmware: -component %s: %v", dir, err)
	}
	if _, err := os.Stat(filepath.Join(abs, "CMakeLists.txt")); err != nil {
		return fwComponent{}, fmt.Errorf("gogo firmware: -component %s: not an ESP-IDF component (no CMakeLists.txt)", dir)
	}
	b, err := os.ReadFile(filepath.Join(abs, "gogo-driver.json"))
	if err != nil {
		return fwComponent{}, fmt.Errorf("gogo firmware: -component %s: no gogo-driver.json (it names the ports the component provides: {\"ports\": [\"name\"]})", dir)
	}
	// Exactly the keys of this format, spelled exactly: a key this tool
	// does not know may be one a newer format needs understood.
	var raw map[string]json.RawMessage
	if err := json.Unmarshal(b, &raw); err != nil {
		return fwComponent{}, fmt.Errorf("gogo firmware: -component %s: gogo-driver.json: %v", dir, err)
	}
	var j struct {
		Format int      `json:"format"`
		Ports  []string `json:"ports"`
	}
	for k, v := range raw {
		var err error
		switch k {
		case "format":
			err = json.Unmarshal(v, &j.Format)
		case "ports":
			err = json.Unmarshal(v, &j.Ports)
		default:
			err = fmt.Errorf("unknown key %q (format %d has \"format\" and \"ports\")", k, fwDriverFormat)
		}
		if err != nil {
			return fwComponent{}, fmt.Errorf("gogo firmware: -component %s: gogo-driver.json: %v", dir, err)
		}
	}
	if j.Format != fwDriverFormat {
		return fwComponent{}, fmt.Errorf("gogo firmware: -component %s: gogo-driver.json: \"format\" is %d, this gogo reads %d", dir, j.Format, fwDriverFormat)
	}
	if len(j.Ports) == 0 {
		return fwComponent{}, fmt.Errorf("gogo firmware: -component %s: gogo-driver.json names no ports", dir)
	}
	if len(j.Ports) > fwBoardPortsMax {
		return fwComponent{}, fmt.Errorf("gogo firmware: -component %s: %d ports; a firmware takes %d board ports", dir, len(j.Ports), fwBoardPortsMax)
	}
	for _, p := range j.Ports {
		if err := fwBoardPortName(p); err != nil {
			return fwComponent{}, fmt.Errorf("gogo firmware: -component %s: %v", dir, err)
		}
	}
	return fwComponent{dir: abs, ports: j.Ports}, nil
}

// fwBoardPortsMax is the most board ports a firmware registers
// (EMBED_HOST_PORTS in platform/esp32s3/main/CMakeLists.txt).
const fwBoardPortsMax = 8

var fwPortNameRe = regexp.MustCompile(`^[A-Za-z0-9_-]+(\.[A-Za-z0-9_-]+)+$`)

// fwBoardPortName checks a board port's name as the firmware does at boot
// (runtime/embed/hostport.c embed_name_check): 1 to 31 bytes of letters,
// digits, '_', '-' and '.', with a vendor prefix: at least one '.', never
// first, last or doubled. The runtime's own names never have a '.', so
// the two cannot collide.
func fwBoardPortName(p string) error {
	if len(p) > 31 {
		return fmt.Errorf("port name %q is longer than 31 bytes", p)
	}
	if !fwPortNameRe.MatchString(p) {
		return fmt.Errorf("bad port name %q: a vendor prefix and a '.', then letters, digits, '_' and '-' (\"acme.sensor\")", p)
	}
	return nil
}

// fwComponentName refuses a component directory name ESP-IDF would mix up
// with another component (a component's name is its directory's): main,
// driver (IDF's legacy driver component, and the likeliest name for a
// driver's directory), one of gogo's own components
// (platform/esp32s3/components) or managed ones, or, with IDF_PATH set,
// one of ESP-IDF's.
func fwComponentName(name string) error {
	taken := name == "main" || name == "driver"
	if repo, ok := findRepoFromWd(); ok {
		for _, d := range []string{"platform/esp32s3/components", "platform/esp32s3/managed_components"} {
			if _, err := os.Stat(filepath.Join(repo, d, name)); err == nil {
				taken = true
			}
		}
	}
	if idf := os.Getenv("IDF_PATH"); idf != "" {
		if _, err := os.Stat(filepath.Join(idf, "components", name)); err == nil {
			taken = true
		}
	}
	if taken {
		return fmt.Errorf("the component name %q (its directory's) is gogo's or ESP-IDF's: name the directory after your driver (acme_sensor)", name)
	}
	return nil
}

// findRepoFromWd is findRepo from the working directory.
func findRepoFromWd() (string, bool) {
	wd, err := os.Getwd()
	if err != nil {
		return "", false
	}
	return findRepo(wd)
}

// fwDriverFormat is the gogo-driver.json format this tool reads:
// {"format": 1, "ports": ["acme.dice"]}.
const fwDriverFormat = 1

// fwComponentsFlag collects repeated -component flags.
type fwComponentsFlag []string

func (c *fwComponentsFlag) String() string     { return strings.Join(*c, ",") }
func (c *fwComponentsFlag) Set(v string) error { *c = append(*c, v); return nil }

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
	"sticks3":   {overlay: "sdkconfig.sticks3", has: fwHas("net", "power", "cjk", "imu", "irtx", "irrx", "mic", "speaker", "tls", "screen", "node", "float", "ble", "bleprov")},
	"cardputer": {overlay: "sdkconfig.cardputer", has: fwHas("net", "kbd", "power", "sd", "exfat", "cjk", "imu", "irtx", "mic", "speaker", "tls", "screen", "node", "float", "ble", "bleprov")},
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
// names of the files, the -with / -without lists and the names the
// driver components provide.
func fwPlan(board string, req []string, with, without []string, extra ...string) (map[string]bool, error) {
	has := func(f fwFeature) bool { return fwBoards[board].has[f.name] }
	required := map[string]bool{}
	reqBy := map[string]bool{} // features some image requires directly
	for _, n := range req {
		found := false
		for _, b := range fwBase {
			found = found || b == n
		}
		for _, b := range extra {
			found = found || b == n
		}
		for _, f := range fwFeatures {
			if fwProvides(f, n) {
				required[f.name], reqBy[f.name], found = true, true, true
			}
		}
		if !found {
			return nil, fmt.Errorf("gogo firmware: the images require %q, which no gogo firmware provides (a driver of your own: -component DIR)", n)
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
		for _, n := range f.needs {
			if on[n] {
				continue
			}
			for _, w := range with {
				if w == f.name {
					return nil, fmt.Errorf("gogo firmware: -with %s needs %s, which is off", f.name, n)
				}
			}
			on[f.name] = false
		}
	}
	return on, nil
}

// fwOverlay renders the sdkconfig fragment; ports: the components' board
// ports (none: the layer is off).
func fwOverlay(board string, on map[string]bool, ports []string) string {
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
	for _, k := range fwTestKconf { // never in a gogo firmware build, even from a reused -o directory
		fmt.Fprintf(&b, "%s=n\n", k)
	}
	v := "n"
	if len(ports) > 0 {
		v = "y"
	}
	fmt.Fprintf(&b, "%s=%s\n", fwBoardPortsKconf, v)
	if len(ports) > 0 {
		fmt.Fprintf(&b, "%s=\"%s\"\n", fwBoardPortNamesKconf, strings.Join(ports, ","))
	}
	return b.String()
}

// fwDefaults is the SDKCONFIG_DEFAULTS list: the common defaults, the
// on-fragment of every feature that is on and has one, the board's
// overlay, the off-fragment of every feature that is off and has one,
// then the generated overlay (the GOGO switches, last so they win).
func fwDefaults(boardOverlay string, on map[string]bool, cfg string) string {
	list := []string{"sdkconfig.defaults"}
	for _, f := range fwFeatures {
		if f.onDefaults != "" && on[f.name] {
			list = append(list, f.onDefaults)
		}
	}
	list = append(list, boardOverlay)
	for _, f := range fwFeatures {
		if f.offDefaults != "" && !on[f.name] {
			list = append(list, f.offDefaults)
		}
	}
	return strings.Join(append(list, cfg), ";")
}

var fwCfgRe = regexp.MustCompile(`^(?:# )?(CONFIG_GOGO_\w+)(?:=(\S+)| is not set)$`)

// fwCheckMap checks the linker map of a firmware built with components:
// each component must have put a GOGO_BOARD_PORT entry in the
// .gogo_board_port section (the map lists its archive there). A component
// that is not linked whole (WHOLE_ARCHIVE) contributes none: the linker
// drops its registration file, which nothing refers to, and the build
// succeeds without the driver.
func fwCheckMap(mapText string, comps []fwComponent) error {
	in := map[string]bool{} // archives with an entry: "libdice.a"
	lines := strings.Split(mapText, "\n")
	for i, l := range lines {
		f := strings.Fields(l)
		if len(f) == 0 || f[0] != ".gogo_board_port" {
			continue
		}
		where := l
		if len(f) == 1 && i+1 < len(lines) {
			where = lines[i+1]
		}
		if m := regexp.MustCompile(`/(lib[^/(]+\.a)\(`).FindStringSubmatch(where); m != nil {
			in[m[1]] = true
		}
	}
	for _, c := range comps {
		if !in["lib"+filepath.Base(c.dir)+".a"] {
			return fmt.Errorf("gogo firmware: component %s put no GOGO_BOARD_PORT in the firmware: register it with WHOLE_ARCHIVE in its CMakeLists.txt (idf_component_register(... WHOLE_ARCHIVE)), or the linker drops the file nothing calls into", filepath.Base(c.dir))
		}
	}
	return nil
}

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
	for _, k := range append([]string{fwBoardPortsKconf, fwBoardPortNamesKconf}, fwTestKconf...) {
		if o, ok := old[k]; ok && o != want[k] {
			diff = append(diff, k)
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
	var compDirs fwComponentsFlag
	fs.Var(&compDirs, "component", "")
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
	var comps []fwComponent
	var extra, compAbs []string
	for _, d := range compDirs {
		c, err := fwReadComponent(d)
		if err != nil {
			return err
		}
		for _, o := range comps {
			if filepath.Base(o.dir) == filepath.Base(c.dir) {
				return fmt.Errorf("gogo firmware: components %s and %s have one name, %q (a component's name is its directory's): rename one", o.dir, c.dir, filepath.Base(c.dir))
			}
		}
		for _, p := range c.ports {
			for _, q := range extra {
				if p == q {
					return fmt.Errorf("gogo firmware: port %q is provided by two components", p)
				}
			}
			extra = append(extra, p)
		}
		comps = append(comps, c)
		compAbs = append(compAbs, c.dir)
	}
	if len(extra) > fwBoardPortsMax {
		return fmt.Errorf("gogo firmware: the components provide %d ports; a firmware takes %d board ports", len(extra), fwBoardPortsMax)
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
	on, err := fwPlan(*board, req, splitList(*withS), splitList(*withoutS), extra...)
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
	overlay := fwOverlay(*board, on, extra)
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
		"-DSDKCONFIG_DEFAULTS=" + fwDefaults(fwb.overlay, on, cfg)}
	// Always given, so a build directory that had components loses them.
	idf = append(idf, "-DEXTRA_COMPONENT_DIRS="+strings.Join(compAbs, ";"), "build")
	var quoted []string
	for _, a := range idf {
		quoted = append(quoted, shQuote(a))
	}
	fmt.Fprintf(out, "wrote %s\nfeatures: %s\n", cfg, fwSummary(on))
	if len(extra) > 0 {
		fmt.Fprintf(out, "board ports: %s\n", strings.Join(extra, " "))
	}
	fmt.Fprintf(out, "run from the gogo repository root (needs `. ~/esp/esp-idf/export.sh`):\n")
	fmt.Fprintf(out, "idf.py %s\n", strings.Join(quoted, " "))
	if *doBuild {
		if err := runIdf("idf.py", idf...); err != nil {
			return err
		}
		if len(comps) > 0 {
			m, err := os.ReadFile(filepath.Join(abs, "gogo.map"))
			if err != nil {
				return fmt.Errorf("gogo firmware: cannot check the components were linked: %v", err)
			}
			if err := fwCheckMap(string(m), comps); err != nil {
				return err
			}
		}
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
