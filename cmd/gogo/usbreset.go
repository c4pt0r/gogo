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
// boardConn.request) or esptool exited with an error (esptoolExitError),
// the port's USB device is probed (usbEnv.probe: its
// string descriptor 0, which a silent device cannot return). Only when the
// probe confirms the device is silent is it reset, once; the port is
// waited for and the exchange runs once more. A device that answers the
// probe (a board without gogo firmware, one restarting, a busy console) is
// never reset: the original error is reported as it is. -no-usb-reset or
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
	"context"
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
	"time"
)

// espressifVID is the only USB vendor id gogo resets.
const espressifVID = 0x303a

// usbResetEnvVar, set to anything but "" or "0", turns the automatic reset
// off, as -no-usb-reset does.
const usbResetEnvVar = "GOGO_NO_USB_RESET"

// usbResetLine is printed when the automatic reset was done.
const usbResetLine = "gogo: the board's USB link was silent; reset it (USB bus reset)"

// errNoAnswer is boardConn.request's timeout: the board sent no reply.
var errNoAnswer = errors.New("no answer from the board (is gogo firmware running? another program using the port?)")

// esptoolExitError is an esptool run that exited with an error status.
// Whether it is retried after a USB reset is the probe's decision, not
// its output's: esptool keeps the terminal (its progress bar redraws in
// place) and nothing of what it prints is read.
//
// A retry is safe even if the first run had begun writing: it is the very
// same command, already checked (setup's NVS rules), and esptool erases
// and writes every region again from its start and checks each file's
// digest. If the retry fails as well, the board is as after any failed
// flash: the firmware refuses an image partition whose header CRC does not
// match, and the bootloader an app image whose checksum does not. In
// practice a silent device cannot be reached at all (esptool's own
// reconnects fail too), so the first run wrote nothing.
type esptoolExitError struct{ err error }

func (e esptoolExitError) Error() string { return e.err.Error() }
func (e esptoolExitError) Unwrap() error { return e.err }

// esptoolErr is err, from running esptool, as an esptoolExitError when
// esptool ran and exited with an error status (not when it did not start).
func esptoolErr(err error) error {
	var exit *exec.ExitError
	if errors.As(err, &exit) {
		return esptoolExitError{err}
	}
	return err
}

// execEsptool runs esptool on the given standard streams, untouched (so a
// terminal stays a terminal), and returns esptoolErr's error.
func execEsptool(name string, args []string, stdin io.Reader, stdout, stderr io.Writer) error {
	cmd := exec.Command(name, args...)
	cmd.Stdin, cmd.Stdout, cmd.Stderr = stdin, stdout, stderr
	return esptoolErr(cmd.Run())
}

// silentLink reports whether err is a failure a USB reset may cure.
func silentLink(err error) bool {
	var ee esptoolExitError
	return errors.Is(err, errNoAnswer) || errors.As(err, &ee)
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
	// probe reports whether the USB device behind port is silent: its
	// string descriptor 0 times out or cannot be read. Any other failure
	// of the request, or of reaching the device, is an error.
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

// errLinkHealthy is recover's answer for a device that passed the probe:
// it was not reset, and the original error stands alone.
var errLinkHealthy = errors.New("the board's USB device answers")

// probeError is recover's answer when the probe could not run (no
// permission, no such device): nothing was reset.
type probeError struct{ err error }

func (e probeError) Error() string {
	return fmt.Sprintf("cannot check the board's USB link, so it was not reset: %v", e.err)
}
func (e probeError) Unwrap() error { return e.err }

// recover resets port's USB device, if the probe confirms it is silent,
// and waits for the port; nil when it is back, errLinkHealthy when the
// device answered the probe, else why not, as a hint to print under the
// original error.
func (r *usbRecovery) recover(port string) error {
	switch {
	case r == nil || r.env.reset == nil:
		return fmt.Errorf("if the board's USB link went silent, %s", replugHint)
	case r.off:
		return fmt.Errorf("if the board's USB link went silent, `gogo device usb-reset` or replugging it brings it back (the automatic reset is off: -no-usb-reset or $%s)", usbResetEnvVar)
	case r.used:
		return fmt.Errorf("still no answer after a USB bus reset: %s", replugHint)
	}
	if r.env.probe == nil {
		return fmt.Errorf("if the board's USB link went silent, %s", replugHint)
	}
	silent, err := r.env.probe(port)
	if err != nil {
		return probeError{err}
	}
	if !silent {
		return errLinkHealthy
	}
	r.used = true
	if err := checkPortFree(port, r.env.holders); err != nil {
		return fmt.Errorf("the board's USB link is silent, but not resetting it: %v", err)
	}
	if _, err := r.env.reset(port); err != nil {
		return err
	}
	fmt.Fprintln(r.errOut, usbResetLine)
	if r.env.wait != nil {
		return r.env.wait(port)
	}
	return nil
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
	if rerr := r.recover(port); rerr == errLinkHealthy {
		return err
	} else if rerr != nil {
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

// checkPortFree refuses a port another process of the user's has open:
// resetting the device under it would cut that program off. Processes of
// root and other users cannot be seen (the probe gate keeps a healthy link
// from being reset anyway).
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
// processes it cannot look into (root's, another user's) are not listed:
// only the user's own holders are found.
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

// lsofTimeout bounds an lsof run (it can hang on a dead network mount).
const lsofTimeout = 5 * time.Second

// lsofHolders runs lsof -t on paths (those that exist) and returns the pids
// it lists, but self. lsof run by a user sees only that user's processes.
// Anything but a clean answer is an error, so the caller does not reset:
// a run past timeout, an exit status other than 0 or 1, or a complaint on
// standard error (a failing lsof also exits 1).
func lsofHolders(lsof string, paths []string, self int, timeout time.Duration) ([]int, error) {
	var args []string
	for _, p := range paths {
		if _, err := os.Stat(p); err == nil {
			args = append(args, p)
		}
	}
	if len(args) == 0 {
		return nil, nil
	}
	ctx, cancel := context.WithTimeout(context.Background(), timeout)
	defer cancel()
	var stdout, stderr bytes.Buffer
	cmd := exec.CommandContext(ctx, lsof, append([]string{"-t", "--"}, args...)...)
	cmd.Stdout, cmd.Stderr = &stdout, &stderr
	err := cmd.Run()
	if ctx.Err() != nil {
		return nil, fmt.Errorf("lsof did not answer within %v", timeout)
	}
	if msg := strings.TrimSpace(stderr.String()); msg != "" {
		if i := strings.IndexByte(msg, '\n'); i >= 0 {
			msg = msg[:i]
		}
		return nil, fmt.Errorf("lsof: %s", msg)
	}
	// lsof exits 1 when one of the files is not open (or none is), and
	// still lists the pids of the others.
	var exit *exec.ExitError
	if err != nil && !(errors.As(err, &exit) && exit.ExitCode() == 1) {
		return nil, fmt.Errorf("lsof: %v", err)
	}
	return parseLsofPids(stdout.Bytes(), self)
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
