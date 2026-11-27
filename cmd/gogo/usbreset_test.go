package main

import (
	"bytes"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

// fakeFailOnce is an esptool that fails to connect on its first run (as
// esptool does on a silent board) and succeeds after; $FAKE_COUNT counts
// the runs.
const fakeFailOnce = `#!/bin/sh
n=$(cat "$FAKE_COUNT" 2>/dev/null || echo 0)
n=$((n + 1))
echo $n > "$FAKE_COUNT"
echo "esptool v5.6.0"
if [ "$n" = 1 ]; then
	echo "A fatal error occurred: Failed to connect to ESP32-S3: No serial data received." >&2
	exit 2
fi
echo "Hash of data verified."
`

func fakeEsptoolScript(t *testing.T) (string, string) {
	t.Helper()
	if _, err := os.Stat("/bin/sh"); err != nil {
		t.Skip("no /bin/sh")
	}
	dir := t.TempDir()
	tool := filepath.Join(dir, "esptool")
	if err := os.WriteFile(tool, []byte(fakeFailOnce), 0o755); err != nil {
		t.Fatal(err)
	}
	count := filepath.Join(dir, "count")
	t.Setenv("FAKE_COUNT", count)
	return tool, count
}

func TestExecEsptool(t *testing.T) {
	tool, _ := fakeEsptoolScript(t)
	var out, errOut bytes.Buffer
	err := execEsptool(tool, []string{"write-flash"}, nil, &out, &errOut)
	if !silentLink(err) {
		t.Errorf("first run: %v, want an esptoolExitError", err)
	}
	// The output is passed through as it is, nothing kept or rewritten.
	if out.String() != "esptool v5.6.0\n" || !strings.Contains(errOut.String(), "No serial data received") {
		t.Errorf("out %q, errOut %q", out.String(), errOut.String())
	}
	if err := execEsptool(tool, nil, nil, io.Discard, io.Discard); err != nil {
		t.Errorf("second run: %v", err)
	}
	// An esptool that does not start is not a candidate for a reset.
	if err := execEsptool(filepath.Join(t.TempDir(), "missing"), nil, nil, io.Discard, io.Discard); err == nil || silentLink(err) {
		t.Errorf("missing esptool: %v", err)
	}
	if !silentLink(fmt.Errorf("gogo push: at byte 128: %w", errNoAnswer)) || silentLink(errors.New("board: connection closed")) {
		t.Error("silentLink misreads wrapped errors")
	}
}

// gogo flash with a real esptool process: it fails, the probe decides.
func TestFlashEsptoolRetryDecision(t *testing.T) {
	app := filepath.Join(t.TempDir(), "app.ggb")
	if err := os.WriteFile(app, fakeImage(32), 0o644); err != nil {
		t.Fatal(err)
	}
	for _, silent := range []bool{true, false} {
		tool, count := fakeEsptoolScript(t)
		usb := &fakeUSB{silent: silent}
		var errOut bytes.Buffer
		fl := flasher{glob: globOf("/dev/cu.usbmodem9"), usb: usb.env(), errOut: &errOut,
			run: func(name string, args []string) error { return execEsptool(name, args, nil, io.Discard, io.Discard) }}
		err := fl.flash([]string{app, "-esptool", tool})
		runs, _ := os.ReadFile(count)
		if silent {
			if err != nil || strings.TrimSpace(string(runs)) != "2" || len(usb.resets) != 1 || errOut.String() != usbResetLine+"\n" {
				t.Errorf("silent board: %v, runs %s, resets %d, %q", err, runs, len(usb.resets), errOut.String())
			}
		} else if err == nil || strings.TrimSpace(string(runs)) != "1" || len(usb.resets) != 0 {
			// A board whose USB answers: esptool's failure is reported, no reset.
			t.Errorf("healthy board: %v, runs %s, resets %d", err, runs, len(usb.resets))
		}
	}
}

// fakeUSB is a usbEnv that records what it is asked.
type fakeUSB struct {
	resets   []string
	holders  []int
	silent   bool
	probeErr error
	probes   int
	failRst  error
}

func (f *fakeUSB) env() usbEnv {
	return usbEnv{
		reset: func(port string) (string, error) {
			f.resets = append(f.resets, port)
			if f.failRst != nil {
				return "", f.failRst
			}
			return "USB device 303a:1001 at location 0x05100000", nil
		},
		holders: func(string) ([]int, error) { return f.holders, nil },
		wait:    func(string) error { return nil },
		probe:   func(string) (bool, error) { f.probes++; return f.silent, f.probeErr },
		getenv:  func(string) string { return "" },
	}
}

// failing returns an op that fails with errs in turn, then succeeds.
func failing(errs ...error) (op func() error, calls *int) {
	n := 0
	return func() error {
		n++
		if n <= len(errs) {
			return errs[n-1]
		}
		return nil
	}, &n
}

func TestUSBRetry(t *testing.T) {
	noAnswer := fmt.Errorf("gogo device: %w", errNoAnswer)
	const port = "/dev/cu.usbmodem5101"

	// One failure, a reset, then success.
	f := &fakeUSB{silent: true}
	var errOut bytes.Buffer
	op, calls := failing(noAnswer)
	if err := newUSBRecovery(f.env(), false, &errOut).retry(port, op); err != nil {
		t.Fatalf("retry: %v", err)
	}
	if *calls != 2 || len(f.resets) != 1 || f.resets[0] != port {
		t.Errorf("calls %d, resets %q", *calls, f.resets)
	}
	if errOut.String() != usbResetLine+"\n" {
		t.Errorf("printed %q", errOut.String())
	}
	// A device that answers the probe (no gogo firmware, restarting) is
	// never reset: the original error, alone, nothing printed.
	f = &fakeUSB{}
	errOut.Reset()
	op, calls = failing(noAnswer)
	if err := newUSBRecovery(f.env(), false, &errOut).retry(port, op); err != noAnswer || *calls != 1 || len(f.resets) != 0 || errOut.Len() != 0 || f.probes != 1 {
		t.Errorf("probe passed: %v, calls %d, resets %d, printed %q", err, *calls, len(f.resets), errOut.String())
	}
	// A probe that cannot run (no permission): no reset, its reason as the hint.
	f = &fakeUSB{probeErr: errors.New("no permission to reach it: allow it with a udev rule")}
	op, calls = failing(noAnswer)
	err := newUSBRecovery(f.env(), false, io.Discard).retry(port, op)
	if err == nil || !errors.Is(err, errNoAnswer) || !strings.Contains(err.Error(), "udev rule") || *calls != 1 || len(f.resets) != 0 {
		t.Errorf("probe failed: %v, calls %d, resets %d", err, *calls, len(f.resets))
	}

	// esptool that failed on a silent device is retried the same way.
	f = &fakeUSB{silent: true}
	op, calls = failing(esptoolExitError{errors.New("exit status 2")})
	if err := newUSBRecovery(f.env(), false, io.Discard).retry(port, op); err != nil || *calls != 2 || len(f.resets) != 1 {
		t.Errorf("esptool: %v, calls %d, resets %d", err, *calls, len(f.resets))
	}

	// Still silent after the reset: the first error and a hint; one reset.
	f = &fakeUSB{silent: true}
	op, calls = failing(noAnswer, noAnswer, noAnswer)
	err = newUSBRecovery(f.env(), false, io.Discard).retry(port, op)
	if err == nil || !strings.HasPrefix(err.Error(), noAnswer.Error()+"\n") || !strings.Contains(err.Error(), "unplug the board") || *calls != 2 || len(f.resets) != 1 {
		t.Errorf("still silent: %v, calls %d, resets %d", err, *calls, len(f.resets))
	}
	// The retry failing otherwise reports that failure.
	other := errors.New("gogo device: board: bad key")
	op, _ = failing(noAnswer, other)
	if err := newUSBRecovery((&fakeUSB{silent: true}).env(), false, io.Discard).retry(port, op); err != other {
		t.Errorf("other second failure: %v", err)
	}

	// Another failure is not retried.
	f = &fakeUSB{}
	op, calls = failing(other)
	if err := newUSBRecovery(f.env(), false, io.Discard).retry(port, op); err != other || *calls != 1 || len(f.resets) != 0 {
		t.Errorf("other failure: %v, calls %d, resets %d", err, *calls, len(f.resets))
	}

	// No reset on this OS (the zero usbEnv): the error and the replug hint.
	op, calls = failing(noAnswer)
	err = newUSBRecovery(usbEnv{}, false, io.Discard).retry(port, op)
	if err == nil || !errors.Is(err, errNoAnswer) || !strings.Contains(err.Error(), "unplug the board and plug it in again") || *calls != 1 {
		t.Errorf("no reset available: %v, calls %d", err, *calls)
	}

	// The reset failing (no permission): the error and the reset's hint.
	f = &fakeUSB{silent: true, failRst: errors.New("no permission to reset it: allow it with a udev rule")}
	op, calls = failing(noAnswer)
	err = newUSBRecovery(f.env(), false, io.Discard).retry(port, op)
	if err == nil || !strings.Contains(err.Error(), "udev rule") || *calls != 1 {
		t.Errorf("reset failed: %v, calls %d", err, *calls)
	}

	// Turned off, by the flag or the environment: no reset, a hint.
	for _, viaEnv := range []bool{false, true} {
		f = &fakeUSB{}
		env := f.env()
		if viaEnv {
			env.getenv = func(k string) string {
				if k == usbResetEnvVar {
					return "1"
				}
				return ""
			}
		}
		op, calls = failing(noAnswer)
		err = newUSBRecovery(env, !viaEnv, io.Discard).retry(port, op)
		if err == nil || !strings.Contains(err.Error(), "gogo device usb-reset") || *calls != 1 || len(f.resets) != 0 {
			t.Errorf("off (env %v): %v, calls %d, resets %d", viaEnv, err, *calls, len(f.resets))
		}
	}
	// GOGO_NO_USB_RESET=0 does not turn it off.
	env := f.env()
	env.getenv = func(string) string { return "0" }
	if newUSBRecovery(env, false, io.Discard).off {
		t.Error("GOGO_NO_USB_RESET=0 turned the reset off")
	}

	// Another program has the port: no reset.
	f = &fakeUSB{silent: true, holders: []int{4242}}
	op, calls = failing(noAnswer)
	err = newUSBRecovery(f.env(), false, io.Discard).retry(port, op)
	if err == nil || !strings.Contains(err.Error(), "pid 4242") || *calls != 1 || len(f.resets) != 0 {
		t.Errorf("port held: %v, calls %d, resets %d", err, *calls, len(f.resets))
	}
}

// A push to a board whose link is silent at first: one reset, then the push.
func TestPushRecoversSilentLink(t *testing.T) {
	app := writeImg(t, t.TempDir(), "app.ggb", strings.Split(strings.Repeat("drv,", 50), ",")[:50]...)
	img := readFile(t, app)
	b := &simBoard{}
	f := &fakeUSB{silent: true}
	opens := 0
	var out, errOut bytes.Buffer
	err := pushMain([]string{"-timeout", "0.1", app}, pushEnv{out: &out, errOut: &errOut, glob: globOf("/dev/cu.usbmodem1"),
		open: func(string) (io.ReadWriteCloser, error) {
			opens++
			if opens == 1 {
				return silentConn(), nil
			}
			return b.conn(), nil
		}, usb: f.env()})
	if err != nil {
		t.Fatal(err)
	}
	if opens != 2 || len(f.resets) != 1 || len(img) <= pushChunk || !bytes.Equal(b.files["/flash/app.ggb"], img) {
		t.Errorf("opens %d, resets %d, pushed %d bytes", opens, len(f.resets), len(b.files["/flash/app.ggb"]))
	}
	if !strings.Contains(errOut.String(), usbResetLine) || !strings.Contains(out.String(), "pushed") {
		t.Errorf("out %q, errOut %q", out.String(), errOut.String())
	}
	// -no-usb-reset: the push fails at once, with the hint.
	f = &fakeUSB{silent: true}
	err = pushMain([]string{"-timeout", "0.1", "-no-usb-reset", app}, pushEnv{out: &out, errOut: &errOut, glob: globOf("/dev/cu.usbmodem1"),
		open: func(string) (io.ReadWriteCloser, error) { return silentConn(), nil }, usb: f.env()})
	if err == nil || !strings.Contains(err.Error(), "no answer from the board") || !strings.Contains(err.Error(), "-no-usb-reset") || len(f.resets) != 0 {
		t.Errorf("-no-usb-reset: %v, resets %d", err, len(f.resets))
	}
}

// silentConn is a port that takes everything and never answers.
func silentConn() io.ReadWriteCloser {
	r, w := io.Pipe()
	return struct {
		io.Reader
		io.Writer
		io.Closer
	}{r, io.Discard, w}
}

// gogo flash retries esptool once when it could not reach the chip.
func TestFlashRecoversSilentLink(t *testing.T) {
	dir := t.TempDir()
	app := filepath.Join(dir, "app.ggb")
	if err := os.WriteFile(app, fakeImage(32), 0o644); err != nil {
		t.Fatal(err)
	}
	usb := &fakeUSB{silent: true}
	runs := 0
	fl := flasher{glob: globOf("/dev/cu.usbmodem9"), usb: usb.env(), errOut: io.Discard,
		run: func(string, []string) error {
			runs++
			if runs == 1 {
				return esptoolExitError{errors.New("exit status 2")}
			}
			return nil
		}}
	if err := fl.flash([]string{app, "-esptool", "/x/esptool"}); err != nil || runs != 2 || len(usb.resets) != 1 {
		t.Errorf("flash: %v, runs %d, resets %d", err, runs, len(usb.resets))
	}
	// A failure that is not esptool's exit status (it did not start) is
	// not retried.
	runs, usb.resets = 0, nil
	fl.run = func(string, []string) error { runs++; return errors.New("exit status 2") }
	if err := fl.flash([]string{app, "-esptool", "/x/esptool"}); err == nil || runs != 1 || len(usb.resets) != 0 {
		t.Errorf("write failure: %v, runs %d, resets %d", err, runs, len(usb.resets))
	}
}

// gogo monitor resets a silent link only when the capture got nothing and
// the USB device fails the probe.
func TestMonitorRecoversSilentLink(t *testing.T) {
	var probeErr error
	run := func(silent bool, outputs ...string) (string, string, *fakeUSB, error) {
		f := &fakeUSB{silent: silent, probeErr: probeErr}
		opens := 0
		var out, errOut bytes.Buffer
		err := monitorRun(monitorOpts{port: "/dev/cu.usbmodem9", dur: 50 * time.Millisecond}, monitorEnv{
			open: func(string) (io.ReadWriteCloser, error) {
				s := outputs[opens]
				opens++
				return struct {
					io.Reader
					io.Writer
					io.Closer
				}{strings.NewReader(s), io.Discard, io.NopCloser(nil)}, nil
			},
			out: &out, errOut: &errOut, usb: f.env(),
		})
		return out.String(), errOut.String(), f, err
	}
	if out, errOut, f, err := run(true, "", "boot\n"); err != nil || out != "boot\n" || len(f.resets) != 1 || errOut != usbResetLine+"\n" {
		t.Errorf("silent: %q %q %d %v", out, errOut, len(f.resets), err)
	}
	if out, _, f, err := run(true, "hello\n"); err != nil || out != "hello\n" || len(f.resets) != 0 {
		t.Errorf("output: %q %d %v", out, len(f.resets), err)
	}
	// A quiet program on a healthy link: nothing to recover.
	if out, _, f, err := run(false, ""); err != nil || out != "" || len(f.resets) != 0 {
		t.Errorf("quiet: %q %d %v", out, len(f.resets), err)
	}
	// A link gogo cannot probe is taken for a quiet program too.
	probeErr = errors.New("no permission")
	if out, errOut, f, err := run(true, ""); err != nil || out != "" || errOut != "" || len(f.resets) != 0 {
		t.Errorf("unprobed: %q %q %d %v", out, errOut, len(f.resets), err)
	}
}

func TestWaitPort(t *testing.T) {
	seq := func(present ...bool) func(string) (os.FileInfo, error) {
		i := 0
		return func(string) (os.FileInfo, error) {
			p := present[min(i, len(present)-1)]
			i++
			if p {
				return nil, nil
			}
			return nil, fs.ErrNotExist
		}
	}
	var slept time.Duration
	sleep := func(d time.Duration) { slept += d }
	// macOS: there, gone, back.
	if err := waitPort("/dev/x", true, seq(true, true, false, false, true), sleep); err != nil {
		t.Errorf("removed and back: %v", err)
	}
	// Linux: never gone.
	if err := waitPort("/dev/x", false, seq(true), sleep); err != nil {
		t.Errorf("kept: %v", err)
	}
	slept = 0
	if err := waitPort("/dev/x", true, seq(true, false), sleep); err == nil || !strings.Contains(err.Error(), "did not come back") {
		t.Errorf("never back: %v", err)
	} else if slept > 7*time.Second {
		t.Errorf("waited %v", slept)
	}
}

// A fake sysfs: a ttyACM (cdc_acm: device is the interface), a ttyUSB
// (usb-serial: device is below the interface), a platform UART and a
// non-Espressif device.
func TestSysfsUSBDevice(t *testing.T) {
	sys := t.TempDir()
	mk := func(dir string, files map[string]string) {
		if err := os.MkdirAll(filepath.Join(sys, dir), 0o755); err != nil {
			t.Fatal(err)
		}
		for k, v := range files {
			writeFile(t, filepath.Join(sys, dir), k, v)
		}
	}
	link := func(tty, target string) {
		if err := os.MkdirAll(filepath.Join(sys, "class", "tty", tty), 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.Symlink(filepath.Join("..", "..", "..", target), filepath.Join(sys, "class", "tty", tty, "device")); err != nil {
			t.Fatal(err)
		}
	}
	usbDev := "devices/pci0000:00/0000:00:14.0/usb3/3-2"
	mk(usbDev, map[string]string{"busnum": "3\n", "devnum": "17\n", "idVendor": "303a\n", "idProduct": "1001\n"})
	mk(usbDev+"/3-2:1.0", map[string]string{"bInterfaceNumber": "00\n"})
	link("ttyACM0", usbDev+"/3-2:1.0")
	ftdi := "devices/pci0000:00/0000:00:14.0/usb1/1-1"
	mk(ftdi, map[string]string{"busnum": "1\n", "devnum": "4\n", "idVendor": "0403\n", "idProduct": "6001\n"})
	mk(ftdi+"/1-1:1.0/ttyUSB0", nil)
	link("ttyUSB0", ftdi+"/1-1:1.0/ttyUSB0")
	mk("devices/platform/serial8250/tty/ttyS0", nil)
	link("ttyS0", "devices/platform/serial8250")

	d, err := sysfsUSBDevice(sys, "ttyACM0")
	if err != nil || d.vendor != 0x303a || d.product != 0x1001 || d.bus != 3 || d.addr != 17 {
		t.Errorf("ttyACM0: %+v %v", d, err)
	}
	if err := checkEspressif("/dev/ttyACM0", d); err != nil {
		t.Errorf("ttyACM0 refused: %v", err)
	}
	if got := d.String(); got != "USB device 303a:1001 on bus 3, device 17" {
		t.Errorf("String = %q", got)
	}
	d, err = sysfsUSBDevice(sys, "ttyUSB0")
	if err != nil || d.vendor != 0x0403 || d.bus != 1 || d.addr != 4 {
		t.Errorf("ttyUSB0: %+v %v", d, err)
	}
	if err := checkEspressif("/dev/ttyUSB0", d); err == nil || !strings.Contains(err.Error(), "not an Espressif board") {
		t.Errorf("FTDI not refused: %v", err)
	}
	if _, err := sysfsUSBDevice(sys, "ttyS0"); err == nil || !strings.Contains(err.Error(), "not on a USB device") {
		t.Errorf("ttyS0: %v", err)
	}
	if _, err := sysfsUSBDevice(sys, "ttyACM9"); err == nil {
		t.Error("missing tty found")
	}
	mac := usbDevice{vendor: 0x303a, product: 0x1001, location: 0x05100000}
	if got := mac.String(); got != "USB device 303a:1001 at location 0x05100000" {
		t.Errorf("String = %q", got)
	}
}

func TestProcHolders(t *testing.T) {
	proc := t.TempDir()
	fd := func(pid, n, target string) {
		dir := filepath.Join(proc, pid, "fd")
		if err := os.MkdirAll(dir, 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.Symlink(target, filepath.Join(dir, n)); err != nil {
			t.Fatal(err)
		}
	}
	fd("100", "0", "/dev/null")
	fd("100", "5", "/dev/ttyACM0")
	fd("100", "6", "/dev/ttyACM0")
	fd("200", "3", "/dev/ttyACM1")
	fd("300", "4", "/dev/ttyACM0") // ourselves
	fd("50", "4", "/dev/ttyACM0")
	if err := os.MkdirAll(filepath.Join(proc, "self"), 0o755); err != nil {
		t.Fatal(err)
	}
	if got := fmt.Sprint(procHolders(proc, "/dev/ttyACM0", 300)); got != "[50 100]" {
		t.Errorf("holders = %s", got)
	}
	if got := procHolders(proc, "/dev/ttyACM7", 300); len(got) != 0 {
		t.Errorf("holders of a free port = %v", got)
	}
}

func TestParseLsofPids(t *testing.T) {
	pids, err := parseLsofPids([]byte("96625\n123\n96625\n7\n"), 123)
	if err != nil || fmt.Sprint(pids) != "[7 96625]" {
		t.Errorf("pids %v, %v", pids, err)
	}
	if pids, err := parseLsofPids(nil, 1); err != nil || len(pids) != 0 {
		t.Errorf("empty: %v %v", pids, err)
	}
	if _, err := parseLsofPids([]byte("lsof: WARNING\n"), 1); err == nil {
		t.Error("garbage accepted")
	}
}

func TestDeviceUSBReset(t *testing.T) {
	f := &fakeUSB{}
	var out bytes.Buffer
	err := deviceMain([]string{"usb-reset", "-port", "/dev/cu.usbmodem5101"}, deviceEnv{out: &out, errOut: io.Discard,
		open: func(string) (io.ReadWriteCloser, error) { t.Fatal("usb-reset opened the port"); return nil, nil }, usb: f.env()})
	if err != nil || len(f.resets) != 1 || !strings.Contains(out.String(), "reset USB device 303a:1001") || !strings.Contains(out.String(), "is back") {
		t.Errorf("usb-reset: %v, resets %d, out %q", err, len(f.resets), out.String())
	}
	f = &fakeUSB{holders: []int{77}}
	err = deviceMain([]string{"-port", "/dev/cu.usbmodem5101", "usb-reset"}, deviceEnv{out: io.Discard, errOut: io.Discard, usb: f.env()})
	if err == nil || !strings.Contains(err.Error(), "pid 77") || len(f.resets) != 0 {
		t.Errorf("held port: %v, resets %d", err, len(f.resets))
	}
	err = deviceMain([]string{"-port", "/dev/cu.usbmodem5101", "usb-reset"}, deviceEnv{out: io.Discard, errOut: io.Discard})
	if err == nil || !strings.Contains(err.Error(), "unplug the board") {
		t.Errorf("no reset on this OS: %v", err)
	}
}
