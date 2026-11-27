package main

// Recovering a board whose USB link went silent (docs/known-issues.md,
// "ESP32-S3 boards"): the chip keeps running its program, but its USB
// device stops answering, even for its string descriptors, so the serial
// console is mute and esptool reports "No serial data received". A USB
// bus reset of that one device brings it back at once without restarting
// the chip.
//
// The commands that talk to a board (gogo device, config, push, monitor,
// flash, setup) run their exchange through usbRecovery.retry: when it
// fails because the board did not answer (errNoAnswer, from
// boardConn.request) or esptool could not connect before writing anything
// (esptoolConnError), the port's USB device is reset once, the port is
// waited for, and the exchange runs once more. -no-usb-reset or
// $GOGO_NO_USB_RESET turns that off; `gogo device usb-reset` resets by
// hand.
//
// The device is found from the serial port (a silent device cannot report
// its serial number): through the IORegistry on macOS
// (usbreset_darwin.go), through sysfs on Linux (usbreset_linux.go). Only
// an Espressif device (vendor id 0x303a) is ever reset, and never while
// another program has the port open (portHolders). Windows has no reset:
// the hint says to replug the board.

import (
	"bufio"
	"bytes"
	"errors"
	"flag"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"slices"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"
)

// espressifVID is the only USB vendor id gogo resets.
const espressifVID = 0x303a

// usbResetEnvVar, set to anything but "" or "0", turns the automatic reset
// off, as -no-usb-reset does.
const usbResetEnvVar = "GOGO_NO_USB_RESET"

// usbResetLine is printed when the automatic reset was done on a device
// that failed the probe (usbEnv.probe), usbResetLineQuiet when it passed
// it or could not be probed: the board still did not answer.
const (
	usbResetLine      = "gogo: the board's USB link was silent; reset it (USB bus reset)"
	usbResetLineQuiet = "gogo: no answer from the board; reset its USB device (USB bus reset)"
)

// errNoAnswer is boardConn.request's timeout: the board sent no reply.
var errNoAnswer = errors.New("no answer from the board (is gogo firmware running? another program using the port?)")

// esptoolConnError is an esptool run that failed to talk to the chip
// before writing anything ("No serial data received", "Failed to
// connect"): retrying it after a USB reset cannot leave a partition half
// written by the first run.
type esptoolConnError struct{ err error }

func (e esptoolConnError) Error() string { return e.err.Error() }
func (e esptoolConnError) Unwrap() error { return e.err }

// esptoolConnFailed reports whether esptool's output shows it never
// reached the chip: a connection failure and no write started.
func esptoolConnFailed(out string) bool {
	if strings.Contains(out, "Writing at") || strings.Contains(out, "Wrote ") {
		return false
	}
	return strings.Contains(out, "No serial data received") || strings.Contains(out, "Failed to connect")
}

// esptoolErr is err, from an esptool run whose output was out, as an
// esptoolConnError when esptool never reached the chip.
func esptoolErr(err error, out string) error {
	if err != nil && esptoolConnFailed(out) {
		return esptoolConnError{err}
	}
	return err
}

// execEsptool runs esptool with its output going to stdout and stderr, and
// returns esptoolErr's error.
func execEsptool(name string, args []string, stdin io.Reader, stdout, stderr io.Writer) error {
	var out lockedBuffer
	cmd := exec.Command(name, args...)
	cmd.Stdin, cmd.Stdout, cmd.Stderr = stdin, io.MultiWriter(stdout, &out), io.MultiWriter(stderr, &out)
	if stdout == stderr {
		// One writer, so os/exec writes it from one goroutine.
		cmd.Stderr = cmd.Stdout
	}
	err := cmd.Run()
	return esptoolErr(err, out.String())
}

// lockedBuffer is a bytes.Buffer two goroutines may write.
type lockedBuffer struct {
	mu sync.Mutex
	b  bytes.Buffer
}

func (l *lockedBuffer) Write(p []byte) (int, error) {
	l.mu.Lock()
	defer l.mu.Unlock()
	return l.b.Write(p)
}

func (l *lockedBuffer) String() string {
	l.mu.Lock()
	defer l.mu.Unlock()
	return l.b.String()
}

// silentLink reports whether err is a failure a USB reset may cure.
func silentLink(err error) bool {
	var ce esptoolConnError
	return errors.Is(err, errNoAnswer) || errors.As(err, &ce)
}

// usbEnv holds the OS side of the reset; the zero value (the tests'
// default) can reset nothing.
type usbEnv struct {
	// reset bus-resets the USB device behind port and describes it.
	reset func(port string) (string, error)
	// holders lists the other processes that have port open.
	holders func(port string) ([]int, error)
	// wait returns once port is back after a reset.
	wait func(port string) error
	// probe reports whether the USB device behind port fails to answer a
	// control request (its string descriptor 0).
	probe func(port string) (silent bool, err error)
	// getenv reads $GOGO_NO_USB_RESET.
	getenv func(string) string
}

// defaultUSBEnv is the real one, on this OS.
func defaultUSBEnv() usbEnv {
	return usbEnv{reset: usbResetPort, holders: portHolders, probe: usbProbe, getenv: os.Getenv,
		wait: func(port string) error { return waitPort(port, usbResetRemovesPort, os.Stat, time.Sleep) }}
}

// addNoUSBResetFlag defines -no-usb-reset on fs.
func addNoUSBResetFlag(fs *flag.FlagSet) *bool { return fs.Bool("no-usb-reset", false, "") }

// usbRecovery resets a board's USB device at most once per command.
type usbRecovery struct {
	env    usbEnv
	off    bool
	errOut io.Writer
	used   bool
}

func newUSBRecovery(env usbEnv, noReset bool, errOut io.Writer) *usbRecovery {
	off := noReset
	if env.getenv != nil {
		if v := env.getenv(usbResetEnvVar); v != "" && v != "0" {
			off = true
		}
	}
	return &usbRecovery{env: env, off: off, errOut: errOut}
}

// replugHint is the fix when gogo cannot reset the device.
const replugHint = "unplug the board and plug it in again"

// recover resets port's USB device and waits for the port; nil when it is
// back, else why not, as a hint to print under the original error.
func (r *usbRecovery) recover(port string) error {
	switch {
	case r == nil || r.env.reset == nil:
		return fmt.Errorf("if the board's USB link went silent, %s", replugHint)
	case r.off:
		return fmt.Errorf("if the board's USB link went silent, `gogo device usb-reset` or replugging it brings it back (the automatic reset is off: -no-usb-reset or $%s)", usbResetEnvVar)
	case r.used:
		return fmt.Errorf("still no answer after a USB bus reset: %s", replugHint)
	}
	r.used = true
	if err := checkPortFree(port, r.env.holders); err != nil {
		return fmt.Errorf("not resetting its USB device: %v", err)
	}
	line := usbResetLineQuiet
	if r.linkSilent(port) {
		line = usbResetLine
	}
	if _, err := r.env.reset(port); err != nil {
		return err
	}
	fmt.Fprintln(r.errOut, line)
	if r.env.wait != nil {
		return r.env.wait(port)
	}
	return nil
}

// linkSilent reports whether the USB device behind port failed the probe
// (false when it cannot be probed).
func (r *usbRecovery) linkSilent(port string) bool {
	if r == nil || r.env.probe == nil {
		return false
	}
	silent, err := r.env.probe(port)
	return err == nil && silent
}

// retry runs op, which opens port, talks to the board and closes the
// port. If op fails because the board's USB link may be silent, it resets
// the port's USB device once (recover) and runs op once more. A failure
// is the first one plus a hint, unless the second run failed otherwise.
func (r *usbRecovery) retry(port string, op func() error) error {
	err := op()
	if err == nil || !silentLink(err) {
		return err
	}
	if rerr := r.recover(port); rerr != nil {
		return fmt.Errorf("%w\n  %v", err, rerr)
	}
	err2 := op()
	switch {
	case err2 == nil:
		return nil
	case silentLink(err2):
		return fmt.Errorf("%w\n  still no answer after a USB bus reset: %s (and check it runs gogo firmware)", err, replugHint)
	}
	return err2
}

// checkPortFree refuses a port another process has open: resetting the
// device under it would cut that program off.
func checkPortFree(port string, holders func(string) ([]int, error)) error {
	if holders == nil {
		return nil
	}
	pids, err := holders(port)
	if err != nil {
		return fmt.Errorf("cannot tell whether another program uses %s (%v)", port, err)
	}
	if len(pids) > 0 {
		var s []string
		for _, p := range pids {
			s = append(s, strconv.Itoa(p))
		}
		return fmt.Errorf("another program (pid %s) has %s open; close it first", strings.Join(s, ", "), port)
	}
	return nil
}

// waitPort waits, after a reset, for the port to come back: on an OS where
// the reset removes it (removes), first for it to go (up to 1.5 s); then
// for it to exist (up to 4 s), plus a moment for the driver to settle.
func waitPort(port string, removes bool, stat func(string) (os.FileInfo, error), sleep func(time.Duration)) error {
	const step = 100 * time.Millisecond
	if removes {
		for i := 0; i < 15; i++ {
			if _, err := stat(port); err != nil {
				break
			}
			sleep(step)
		}
	} else {
		sleep(3 * step)
	}
	for i := 0; i < 40; i++ {
		if _, err := stat(port); err == nil {
			sleep(3 * step)
			return nil
		}
		sleep(step)
	}
	return fmt.Errorf("%s did not come back within 4 s of the USB reset: %s", port, replugHint)
}

// deviceUSBReset is `gogo device usb-reset`: reset the USB device behind
// port by hand, whatever the board answers.
func deviceUSBReset(port string, env usbEnv, out io.Writer) error {
	if env.reset == nil {
		return fmt.Errorf("gogo device: usb-reset is not available on this OS: %s", replugHint)
	}
	if err := checkPortFree(port, env.holders); err != nil {
		return fmt.Errorf("gogo device: not resetting: %v", err)
	}
	desc, err := env.reset(port)
	if err != nil {
		return fmt.Errorf("gogo device: %v", err)
	}
	fmt.Fprintf(out, "reset %s (USB bus reset; the chip keeps running)\n", desc)
	if env.wait != nil {
		if err := env.wait(port); err != nil {
			return fmt.Errorf("gogo device: %v", err)
		}
	}
	fmt.Fprintf(out, "%s is back\n", port)
	return nil
}

// sysfsUSBDevice finds, under the sysfs root sys ("/sys"), the USB device
// of the tty named tty (ttyACM0): /sys/class/tty/<tty>/device points into
// the device's interface directory (ttyACM) or below it (ttyUSB); the
// device is the first directory above with busnum, devnum and idVendor.
func sysfsUSBDevice(sys, tty string) (usbDevice, error) {
	link := filepath.Join(sys, "class", "tty", tty, "device")
	dir, err := filepath.EvalSymlinks(link)
	if err != nil {
		return usbDevice{}, fmt.Errorf("%s is not a USB serial device (%v)", tty, err)
	}
	root, err := filepath.EvalSymlinks(sys)
	if err != nil {
		return usbDevice{}, err
	}
	stop := filepath.Join(root, "devices")
	for ; strings.HasPrefix(dir, stop+string(filepath.Separator)); dir = filepath.Dir(dir) {
		bus, e1 := readSysInt(filepath.Join(dir, "busnum"), 10)
		dev, e2 := readSysInt(filepath.Join(dir, "devnum"), 10)
		vid, e3 := readSysInt(filepath.Join(dir, "idVendor"), 16)
		if e1 != nil || e2 != nil || e3 != nil {
			continue
		}
		pid, _ := readSysInt(filepath.Join(dir, "idProduct"), 16)
		return usbDevice{vendor: vid, product: pid, bus: bus, addr: dev}, nil
	}
	return usbDevice{}, fmt.Errorf("%s is not on a USB device", tty)
}

func readSysInt(path string, base int) (int, error) {
	b, err := os.ReadFile(path)
	if err != nil {
		return 0, err
	}
	v, err := strconv.ParseInt(strings.TrimSpace(string(b)), base, 32)
	return int(v), err
}

// usbDevice is the USB device behind a serial port.
type usbDevice struct {
	vendor, product int
	bus, addr       int    // Linux: bus and device numbers
	location        uint32 // macOS: locationID
}

func (d usbDevice) String() string {
	if d.location != 0 {
		return fmt.Sprintf("USB device %04x:%04x at location %#08x", d.vendor, d.product, d.location)
	}
	return fmt.Sprintf("USB device %04x:%04x on bus %d, device %d", d.vendor, d.product, d.bus, d.addr)
}

// checkEspressif refuses any device but an Espressif one.
func checkEspressif(port string, d usbDevice) error {
	if d.vendor != espressifVID {
		return fmt.Errorf("%s is %s, not an Espressif board (vendor id %04x): gogo resets only those", port, d, espressifVID)
	}
	return nil
}

// procHolders lists the processes, other than self, with a descriptor on
// the file target, from the proc file system at proc ("/proc"). The
// processes it cannot look into (another user's) are not listed.
func procHolders(proc, target string, self int) []int {
	ents, _ := os.ReadDir(proc)
	var pids []int
	for _, e := range ents {
		pid, err := strconv.Atoi(e.Name())
		if err != nil || pid == self {
			continue
		}
		fds, err := os.ReadDir(filepath.Join(proc, e.Name(), "fd"))
		if err != nil {
			continue
		}
		for _, fd := range fds {
			if l, err := os.Readlink(filepath.Join(proc, e.Name(), "fd", fd.Name())); err == nil && l == target {
				pids = append(pids, pid)
				break
			}
		}
	}
	sort.Ints(pids)
	return pids
}

// lsofHolders runs lsof -t on paths (those that exist) and returns the pids
// it lists, but self.
func lsofHolders(lsof string, paths []string, self int) ([]int, error) {
	var args []string
	for _, p := range paths {
		if _, err := os.Stat(p); err == nil {
			args = append(args, p)
		}
	}
	if len(args) == 0 {
		return nil, nil
	}
	// lsof exits 1 when one of the files is not open (or none is), and
	// still lists the pids of the others.
	out, err := exec.Command(lsof, append([]string{"-t", "--"}, args...)...).Output()
	var exit *exec.ExitError
	if err != nil && !(errors.As(err, &exit) && exit.ExitCode() == 1) {
		return nil, fmt.Errorf("lsof: %v", err)
	}
	return parseLsofPids(out, self)
}

// parseLsofPids reads lsof -t's output: one pid a line.
func parseLsofPids(out []byte, self int) ([]int, error) {
	var pids []int
	sc := bufio.NewScanner(bytes.NewReader(out))
	for sc.Scan() {
		l := strings.TrimSpace(sc.Text())
		if l == "" {
			continue
		}
		pid, err := strconv.Atoi(l)
		if err != nil {
			return nil, fmt.Errorf("lsof: unexpected output %q", l)
		}
		if pid != self && !slices.Contains(pids, pid) {
			pids = append(pids, pid)
		}
	}
	sort.Ints(pids)
	return pids, nil
}
