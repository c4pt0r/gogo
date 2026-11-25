package main

// gogo setup: from a new board to a running program in one command.
//
//	gogo setup [-port P] [-board sticks3|cardputer] [-version vX.Y.Z]
//	           [-release-url URL|DIR] [-esptool path] [-offline]
//	           [-skip-wifi] [-yes] [-dry-run]
//
// Steps: check esptool and find the board's serial port; identify the
// board; get its firmware (a GitHub release, checked against its
// SHA256SUMS, or a local ESP-IDF build); flash it without touching the
// NVS partition (setup_flash.go); set up Wi-Fi; run a first program.
// Each step says what it does; a failure says how to fix it and stops.
//
// -dry-run still runs the steps that only read (esptool's chip check,
// the download, a Wi-Fi scan) and prints every command that would write
// to the board instead of running it.

import (
	"bufio"
	"errors"
	"flag"
	"fmt"
	"io"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"runtime"
	"strconv"
	"strings"
	"time"

	"gogotools/compiler"
)

const setupUsage = "usage: gogo setup [-port P] [-board sticks3|cardputer] [-version vX.Y.Z] [-release-url URL|DIR] [-esptool path] [-offline] [-skip-wifi] [-yes] [-dry-run]"

// version is this gogo's release version, set at link time by
// tools/release.sh (-ldflags "-X main.version=v1.2.3").
var version = "dev"

// File names gogo setup writes: compiled images and gogo source files.
const (
	imageExt  = ".gogo"
	sourceExt = ".go"
)

// setupSteps is the number of steps gogo setup prints.
const setupSteps = 6

var setupBoardTitles = map[string]string{"sticks3": "M5StickS3", "cardputer": "Cardputer ADV"}

type setupOpts struct {
	port, board, version, releaseURL, esptool string
	offline, skipWifi, yes, dryRun            bool
}

// setupEnv holds the side effects of gogo setup, replaced in tests.
type setupEnv struct {
	stdin    io.Reader // the raw stdin, for turning echo off
	in       *bufio.Reader
	out      io.Writer
	goos     string
	getenv   func(string) string
	glob     func(string) ([]string, error)
	lookPath func(string) (string, error)
	openPort func(string) (io.ReadWriteCloser, error)
	client   *http.Client
	cacheDir string
	repoDir  func() (string, bool)
	runIdf   func(lb localBuild, args []string) error
	// retryDelay is the wait between attempts to open the port.
	retryDelay time.Duration
}

func defaultSetupEnv() (*setupEnv, error) {
	cache, err := os.UserCacheDir()
	if err != nil {
		return nil, err
	}
	return &setupEnv{
		stdin:    os.Stdin,
		in:       bufio.NewReader(os.Stdin),
		out:      os.Stdout,
		goos:     runtime.GOOS,
		getenv:   os.Getenv,
		glob:     filepath.Glob,
		lookPath: exec.LookPath,
		openPort: func(p string) (io.ReadWriteCloser, error) { return openSerial(p) },
		client:   &http.Client{Timeout: 2 * time.Minute},
		cacheDir: filepath.Join(cache, "gogo"),
		repoDir:  findSourceRepo,
		runIdf:   runIdfLocal(os.Stdout),

		retryDelay: 500 * time.Millisecond,
	}, nil
}

// findSourceRepo finds the gogo source tree around the current directory
// or this executable.
func findSourceRepo() (string, bool) {
	if wd, err := os.Getwd(); err == nil {
		if r, ok := findRepo(wd); ok {
			return r, true
		}
	}
	if exe, err := os.Executable(); err == nil {
		if real, err := filepath.EvalSymlinks(exe); err == nil {
			exe = real
		}
		return findRepo(filepath.Dir(exe))
	}
	return "", false
}

func setupCmd(args []string) error {
	env, err := defaultSetupEnv()
	if err != nil {
		return err
	}
	return setupMain(args, env)
}

func parseSetupArgs(args []string) (setupOpts, error) {
	var o setupOpts
	fs := flag.NewFlagSet("setup", flag.ContinueOnError)
	fs.SetOutput(io.Discard)
	fs.StringVar(&o.port, "port", "", "")
	fs.StringVar(&o.board, "board", "", "")
	fs.StringVar(&o.version, "version", "", "")
	fs.StringVar(&o.releaseURL, "release-url", "", "")
	fs.StringVar(&o.esptool, "esptool", "", "")
	fs.BoolVar(&o.offline, "offline", false, "")
	fs.BoolVar(&o.skipWifi, "skip-wifi", false, "")
	fs.BoolVar(&o.yes, "yes", false, "")
	fs.BoolVar(&o.dryRun, "dry-run", false, "")
	if err := fs.Parse(args); err != nil || fs.NArg() != 0 {
		return o, errors.New(setupUsage)
	}
	if o.board != "" && setupBoardTitles[o.board] == "" {
		return o, fmt.Errorf("gogo setup: -board must be one of %s\n%s", boardNames(), setupUsage)
	}
	if o.version != "" && !isReleaseVersion(o.version) {
		return o, fmt.Errorf("gogo setup: -version %q is not a release version like v1.2.3", o.version)
	}
	if o.version != "" && o.releaseURL != "" {
		return o, errors.New("gogo setup: -version and -release-url do not combine (-release-url names the files of one release)")
	}
	if o.offline && o.releaseURL != "" {
		return o, errors.New("gogo setup: -offline builds the firmware locally; drop -release-url")
	}
	return o, nil
}

// setup is one run of gogo setup.
type setup struct {
	o       setupOpts
	env     *setupEnv
	esptool string
	port    string
	board   string
	nvs     []partition // NVS partitions no write may touch
	stepN   int
}

// setupFail is an error with the fix to print under it.
func setupFail(fix string, format string, a ...any) error {
	return fmt.Errorf("gogo setup: "+format+"\n  fix: %s", append(a, fix)...)
}

func setupMain(args []string, env *setupEnv) error {
	o, err := parseSetupArgs(args)
	if err != nil {
		return err
	}
	s := &setup{o: o, env: env, nvs: defaultNVS}
	if o.dryRun {
		fmt.Fprintln(env.out, "dry run: commands that write to the board are printed, not run")
	}
	s.step("Checking the environment")
	if err := s.checkEsptool(); err != nil {
		return err
	}
	if err := s.pickPort(); err != nil {
		return err
	}
	s.step("Identifying the board")
	oldParts, err := s.identify()
	if err != nil {
		return err
	}
	s.step("Getting the firmware")
	fw, err := s.getFirmware()
	if err != nil {
		return err
	}
	s.step("Flashing the firmware")
	if err := s.flashFirmware(fw, oldParts); err != nil {
		return err
	}
	s.step("Setting up Wi-Fi")
	if err := s.setupWifi(); err != nil {
		return err
	}
	s.step("Running a first program")
	if err := s.firstProgram(); err != nil {
		return err
	}
	if o.dryRun {
		fmt.Fprintln(env.out, "\nDry run done: nothing was written to the board.")
		return nil
	}
	s.nextSteps()
	return nil
}

func (s *setup) step(title string) {
	s.stepN++
	fmt.Fprintf(s.env.out, "\n[%d/%d] %s\n", s.stepN, setupSteps, title)
}

// ask prints question and returns the answer line; with -yes, or for an
// empty answer, def. A closed stdin without -yes is an error.
func (s *setup) ask(question, def string) (string, error) {
	if s.o.yes {
		fmt.Fprintf(s.env.out, "%s %s (-yes)\n", question, def)
		return def, nil
	}
	fmt.Fprintf(s.env.out, "%s ", question)
	line, err := s.env.in.ReadString('\n')
	if err != nil && (err != io.EOF || line == "") {
		fmt.Fprintln(s.env.out)
		return "", setupFail("run gogo setup in a terminal, or pass -yes to accept the defaults", "no answer on stdin")
	}
	if line = strings.TrimSpace(line); line == "" {
		return def, nil
	}
	return line, nil
}

// confirm asks a yes/no question whose default is yes.
func (s *setup) confirm(question string) (bool, error) {
	a, err := s.ask(question+" [Y/n]", "y")
	if err != nil {
		return false, err
	}
	return strings.HasPrefix(strings.ToLower(a), "y"), nil
}

// choose asks for one of options by number (1-based; def is the default).
func (s *setup) choose(question string, options []string, def int) (int, error) {
	for i, opt := range options {
		fmt.Fprintf(s.env.out, "  %d) %s\n", i+1, opt)
	}
	for tries := 0; tries < 3; tries++ {
		a, err := s.ask(fmt.Sprintf("%s [%d]", question, def), strconv.Itoa(def))
		if err != nil {
			return 0, err
		}
		if n, err := strconv.Atoi(a); err == nil && n >= 1 && n <= len(options) {
			return n, nil
		}
		fmt.Fprintf(s.env.out, "  please answer a number from 1 to %d\n", len(options))
	}
	return 0, setupFail("answer with the number of an entry", "no valid choice")
}

var esptoolVersionRe = regexp.MustCompile(`v?(\d+)\.(\d+)(?:\.(\d+))?`)

// checkEsptool finds esptool (version 5 or newer: gogo setup uses its
// v5 command names).
func (s *setup) checkEsptool() error {
	const fix = "install it with `pip install esptool` or `brew install esptool`, or pass -esptool /path/to/esptool"
	if s.o.esptool != "" {
		s.esptool = s.o.esptool
	} else {
		for _, name := range []string{"esptool", "esptool.py"} {
			if p, err := s.env.lookPath(name); err == nil {
				s.esptool = p
				break
			}
		}
		if s.esptool == "" {
			return setupFail(fix, "esptool not found")
		}
	}
	out, err := exec.Command(s.esptool, "version").CombinedOutput()
	m := esptoolVersionRe.FindStringSubmatch(string(out))
	if err != nil || m == nil {
		return setupFail(fix, "%s does not run (%v)", s.esptool, err)
	}
	if major, _ := strconv.Atoi(m[1]); major < 5 {
		return setupFail("upgrade it: `pip install --upgrade esptool`", "esptool %s.%s is too old (version 5 or newer is needed)", m[1], m[2])
	}
	fmt.Fprintf(s.env.out, "  esptool %s (%s)\n", strings.TrimPrefix(m[0], "v"), s.esptool)
	return nil
}

// setupPortGlobs are the serial ports a board shows up as, per OS.
func setupPortGlobs(goos string) []string {
	switch goos {
	case "darwin":
		return []string{"/dev/cu.usbmodem*"}
	case "linux":
		return []string{"/dev/ttyACM*"}
	}
	return nil
}

// pickPort uses -port, the only serial port found, or asks which one.
func (s *setup) pickPort() error {
	if s.o.port != "" {
		s.port = s.o.port
		fmt.Fprintf(s.env.out, "  port %s (-port)\n", s.port)
		return nil
	}
	globs := setupPortGlobs(s.env.goos)
	if globs == nil {
		return setupFail("pass the board's port, e.g. -port COM5 (Device Manager, Ports)", "cannot list serial ports on %s", s.env.goos)
	}
	var ports []string
	for _, g := range globs {
		m, _ := s.env.glob(g)
		ports = append(ports, m...)
	}
	switch len(ports) {
	case 0:
		return setupFail("connect the board with a USB data cable (some cables only charge) and check it is switched on, or pass -port",
			"no board found (%s)", strings.Join(globs, ", "))
	case 1:
		s.port = ports[0]
		fmt.Fprintf(s.env.out, "  port %s\n", s.port)
		return nil
	}
	if s.o.yes {
		return setupFail("pick one with -port", "several serial ports found: %s", strings.Join(ports, ", "))
	}
	n, err := s.choose("Which port is the board on?", ports, 1)
	if err != nil {
		return err
	}
	s.port = ports[n-1]
	return nil
}

// esptoolArgs is the start of every esptool command: the chip, the port,
// and the watchdog reset afterwards. The Cardputer ADV does not come back
// from esptool's default RTS reset on its USB port; the watchdog reset
// works on both boards, so it is used for both.
func (s *setup) esptoolArgs() []string {
	return []string{"--chip", "esp32s3", "--port", s.port, "--after", "watchdog-reset"}
}

// runEsptool checks args (checkEsptoolArgs) and runs esptool, its output
// going to the terminal; with -dry-run, a command that writes is printed
// instead.
func (s *setup) runEsptool(args []string, writes bool) error {
	if err := checkEsptoolArgs(args, s.nvs); err != nil {
		return err
	}
	line := s.esptool + " " + strings.Join(args, " ")
	if writes && s.o.dryRun {
		fmt.Fprintf(s.env.out, "  would run: %s\n", line)
		return nil
	}
	fmt.Fprintf(s.env.out, "  $ %s\n", line)
	cmd := exec.Command(s.esptool, args...)
	cmd.Stdout, cmd.Stderr = s.env.out, s.env.out
	return cmd.Run()
}

// identify reads the chip banner and the partition table now on the board
// (one read-only esptool command), and settles the board.
func (s *setup) identify() ([]partition, error) {
	tmp, err := os.MkdirTemp("", "gogo-setup-")
	if err != nil {
		return nil, err
	}
	defer os.RemoveAll(tmp)
	ptFile := filepath.Join(tmp, "partitions.bin")
	args := append(s.esptoolArgs(), "read-flash", "0x8000", strconv.Itoa(partTableBytes), ptFile)
	if err := checkEsptoolArgs(args, s.nvs); err != nil {
		return nil, err
	}
	fmt.Fprintf(s.env.out, "  $ %s %s\n", s.esptool, strings.Join(args, " "))
	out, err := exec.Command(s.esptool, args...).CombinedOutput()
	if err != nil {
		tail := strings.TrimSpace(string(out))
		if i := strings.LastIndex(tail, "\n"); i >= 0 && len(tail)-i < 300 {
			tail = tail[i+1:]
		}
		return nil, setupFail("hold the board's boot button (G0) while plugging it in to force download mode, close any serial monitor using the port, and run gogo setup again",
			"esptool cannot talk to the board on %s: %s", s.port, tail)
	}
	chip, err := parseChip(string(out))
	if err != nil {
		return nil, setupFail("update esptool (`pip install --upgrade esptool`)", "%v", err)
	}
	if !chip.isS3() {
		return nil, setupFail("gogo supports the M5StickS3 and the Cardputer ADV (both ESP32-S3); connect one of them",
			"the chip on %s is %s, not an ESP32-S3", s.port, chip.chip)
	}
	psram := "no PSRAM"
	if chip.psram {
		psram = "embedded PSRAM"
	}
	fmt.Fprintf(s.env.out, "  chip: %s, %s\n", chip.chip, psram)
	if s.o.board != "" {
		s.board = s.o.board
		fmt.Fprintf(s.env.out, "  board: %s (-board)\n", setupBoardTitles[s.board])
		if s.board != chip.guessBoard() {
			fmt.Fprintf(s.env.out, "  note: a chip with %s looks like the %s\n", psram, setupBoardTitles[chip.guessBoard()])
		}
	} else {
		guess := chip.guessBoard()
		ok, err := s.confirm(fmt.Sprintf("  This looks like the %s. Right?", setupBoardTitles[guess]))
		if err != nil {
			return nil, err
		}
		if ok {
			s.board = guess
		} else {
			n, err := s.choose("Which board is it?", []string{"M5StickS3", "Cardputer ADV"}, 1)
			if err != nil {
				return nil, err
			}
			s.board = []string{"sticks3", "cardputer"}[n-1]
		}
	}
	b, err := os.ReadFile(ptFile)
	if err != nil {
		return nil, nil // nothing read: the board's layout is unknown
	}
	parts, _ := parsePartitions(b)
	return parts, nil
}

// getFirmware downloads the release firmware for the board, or builds it
// locally (-offline, or when there is no release to download).
func (s *setup) getFirmware() (*firmwareSet, error) {
	var relErr error
	if !s.o.offline {
		fw, err := s.releaseFirmware(s.board)
		if err == nil {
			return fw, nil
		}
		if !errors.Is(err, errUnavailable) {
			return nil, setupFail("try again; if it keeps failing, the release is broken: report it", "%v", err)
		}
		relErr = err
		fmt.Fprintf(s.env.out, "  cannot download the firmware: %v\n", err)
	}
	lb, err := s.findLocalBuild()
	if err != nil {
		if relErr == nil {
			return nil, setupFail("install ESP-IDF v6.1.1 (see the gogo book, chapter 2) and run gogo setup from the gogo source tree",
				"-offline needs a local firmware build: %v", err)
		}
		return nil, setupFail("either check the network (a private release needs `gh release download` and -release-url DIR), "+
			"or install ESP-IDF v6.1.1 and run gogo setup from the gogo source tree to build the firmware locally",
			"no firmware: the release is not available and %v", err)
	}
	where := "with idf.py from PATH"
	if lb.export != "" {
		where = "with ESP-IDF at " + filepath.Dir(lb.export)
	}
	if relErr != nil {
		ok, err := s.confirm(fmt.Sprintf("  Build the firmware locally %s instead (a few minutes)?", where))
		if err != nil {
			return nil, err
		}
		if !ok {
			return nil, setupFail("pass -release-url with a downloaded release, or answer yes to build locally", "no firmware")
		}
	} else {
		fmt.Fprintf(s.env.out, "  building the firmware locally %s (-offline)\n", where)
	}
	return s.localFirmware(s.board, lb)
}

// flashFirmware writes the firmware's files at their flasher_args.json
// offsets (checked by planFirmware), never NVS.
func (s *setup) flashFirmware(fw *firmwareSet, oldParts []partition) error {
	fmt.Fprintf(s.env.out, "  firmware: %s\n", fw.source)
	if fw.plan == nil { // dry run of a local build: nothing built to flash
		fmt.Fprintf(s.env.out, "  would flash the files of flasher_args.json after the build, then reset with a watchdog reset\n")
		return nil
	}
	s.nvs = nvsParts(fw.plan.parts)
	for _, w := range fw.plan.writes {
		fmt.Fprintf(s.env.out, "  %#08x  %s (%d bytes)\n", w.offset, filepath.Base(w.name), w.size)
	}
	for _, p := range s.nvs {
		fmt.Fprintf(s.env.out, "  not written: NVS %#x..%#x (settings such as Wi-Fi stay)\n", p.offset, p.end())
	}
	if len(oldParts) > 0 && !sameNVS(oldParts, fw.plan.parts) {
		fmt.Fprintf(s.env.out, "  note: the board's current partition table puts NVS elsewhere; settings stored by the old firmware will not carry over\n")
	}
	ok, err := s.confirm(fmt.Sprintf("  Flash this firmware to the %s on %s?", setupBoardTitles[s.board], s.port))
	if err != nil {
		return err
	}
	if !ok {
		return setupFail("run gogo setup again when ready", "stopped before flashing")
	}
	fs := fw.plan.args.FlashSettings
	args := append(s.esptoolArgs(), "write-flash", "--flash-mode", fs.Mode, "--flash-freq", fs.Freq, "--flash-size", fs.Size)
	for _, w := range fw.plan.writes {
		args = append(args, fmt.Sprintf("%#x", w.offset), w.path)
	}
	if err := s.runEsptool(args, true); err != nil {
		return setupFail("check the cable and that nothing else uses the port, then run gogo setup again", "flashing failed: %v", err)
	}
	return nil
}

// sameNVS reports whether two partition tables place NVS identically.
func sameNVS(a, b []partition) bool {
	na, nb := nvsParts(a), nvsParts(b)
	if len(na) != len(nb) {
		return false
	}
	for i := range na {
		if na[i].offset != nb[i].offset || na[i].size != nb[i].size {
			return false
		}
	}
	return true
}

// helloSource is the first program gogo setup runs on the board: a
// greeting, then a line every two seconds for a minute, so the monitor
// sees it however long the board takes to come back after flashing.
const helloSource = `// The first program gogo setup puts on the board.
package main

import "gogo/actor"

func main() {
	println("hello from gogo on the BOARD!")
	for i := 1; i <= 30; i++ {
		actor.Sleep(2000)
		println("hello from gogo, tick", i)
	}
}
`

// writeHello compiles the first program for board to the image file out.
func writeHello(board, out string) error {
	dir, err := os.MkdirTemp("", "gogo-hello-")
	if err != nil {
		return err
	}
	defer os.RemoveAll(dir)
	src := strings.Replace(helloSource, "BOARD", setupBoardTitles[board], 1)
	if err := os.WriteFile(filepath.Join(dir, "main"+sourceExt), []byte(src), 0o644); err != nil {
		return err
	}
	prog, err := compiler.Load(dir)
	if err != nil {
		return err
	}
	img, err := compiler.Compile(prog)
	if err != nil {
		return err
	}
	return os.WriteFile(out, img, 0o644)
}
