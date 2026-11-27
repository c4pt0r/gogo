package main

// gogo device: operations on the board itself, over its serial console
// (the \x02gogo-config frames of config.go's boardConn):
//
//	gogo device [-port P] info                  the firmware it runs
//	gogo device [-port P] scan                  the Wi-Fi networks it sees
//	gogo device [-port P] stats                 its counters and memory
//	gogo device [-port P] [-yes] format-flash   format /flash
//	gogo device [-port P] usb-reset             reset its USB device
//
// info shows the firmware's version, the board it was built for, the image
// format it runs, the VM's parameters (its limits: actor memory, actors,
// ports, links, code memory, Bytes handles) and the requirement names it
// provides (the board's `info` answer, configsvc.h). A firmware older
// than info says so.
//
// format-flash formats the gogo LittleFS partition (gogofs) and nothing
// else: the board restarts and formats it at mount (fs_lfs.c). It never
// erases the chip and never writes NVS (the configuration and Wi-Fi
// settings stay). It asks for "yes" on stdin unless -yes is given.
//
// usb-reset bus-resets the board's USB device (usbreset.go), which
// brings back a USB link gone silent without restarting the chip. The
// other operations do it on their own when the board does not answer,
// unless -no-usb-reset (or $GOGO_NO_USB_RESET) is given.
//
// scan and stats were `gogo config scan|stats`, and format-flash was
// `gogo config set fs.flash.format yes`; those still work for one release.

import (
	"bufio"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
	"time"
)

const deviceUsage = "usage: gogo device [-port P] [-yes] [-no-usb-reset] info | scan | stats | format-flash | usb-reset"

// deviceEnv holds the side effects of gogo device, replaced in tests.
type deviceEnv struct {
	out, errOut io.Writer
	in          io.Reader
	glob        func(string) ([]string, error)
	open        func(string) (io.ReadWriteCloser, error)
	usb         usbEnv
}

func deviceCmd(args []string) error {
	return deviceMain(args, deviceEnv{
		out: os.Stdout, errOut: os.Stderr, in: os.Stdin, glob: filepath.Glob,
		open: func(p string) (io.ReadWriteCloser, error) { return openSerial(p) },
		usb:  defaultUSBEnv(),
	})
}

func deviceMain(args []string, env deviceEnv) error {
	fs := newFlags("device")
	port := fs.String("port", "", "")
	yes := fs.Bool("yes", false, "")
	noReset := addNoUSBResetFlag(fs)
	pos, err := parseFlags(fs, args, deviceUsage)
	if err != nil {
		return err
	}
	if len(pos) != 1 {
		return usageError("device", "want one operation", deviceUsage)
	}
	op := pos[0]
	if op != "info" && op != "scan" && op != "stats" && op != "format-flash" && op != "usb-reset" {
		return usageError("device", fmt.Sprintf("unknown operation %q", op), deviceUsage)
	}
	if *port == "" {
		if *port, err = detectPort(env.glob); err != nil {
			return err
		}
	}
	if op == "usb-reset" {
		return deviceUSBReset(*port, env.usb, env.out)
	}
	if op == "format-flash" && !*yes {
		fmt.Fprintf(env.errOut, "This deletes every file on /flash of the board on %s (settings in NVS stay).\nType yes to format it: ", *port)
		line, _ := bufio.NewReader(env.in).ReadString('\n')
		if strings.TrimSpace(line) != "yes" {
			return fmt.Errorf("gogo device: not formatted (answer yes, or pass -yes)")
		}
	}
	usb := newUSBRecovery(env.usb, *noReset, env.errOut)
	err = usb.retry(*port, func() error {
		f, err := env.open(*port)
		if err != nil {
			return err
		}
		defer f.Close()
		return deviceOp(f, op, env.out)
	})
	if err != nil && !strings.HasPrefix(err.Error(), "gogo") {
		err = fmt.Errorf("gogo device: %w", err)
	}
	return err
}

// deviceOp runs one operation (but usb-reset) on the open port.
func deviceOp(f io.ReadWriter, op string, out io.Writer) error {
	if op == "info" {
		return deviceInfo(f, out)
	}
	if op != "format-flash" {
		timeout := 5 * time.Second
		if op == "scan" {
			timeout = 20 * time.Second
		}
		if err := boardConfig(f, op, nil, out, timeout); err != nil {
			return renameErr(err, "gogo config: ", "gogo device: ")
		}
		return nil
	}
	c := newBoardConn(f)
	defer c.close()
	if _, _, err := c.request("format-flash", nil, 5*time.Second); err != nil {
		if be, ok := err.(errBoard); ok && (be.reason == "bad key" || strings.HasPrefix(be.reason, "usage:")) {
			return fmt.Errorf("gogo device: this firmware has no format-flash (update it); `gogo config set fs.flash.format yes` and a restart do the same")
		}
		return fmt.Errorf("gogo device: %w", err)
	}
	fmt.Fprintf(out, "the board restarts and formats /flash; `gogo monitor -timeout 5` shows \"gogo: formatting /flash\"\n")
	return nil
}

// renamedErr is an error whose message starts with another command's
// name; it still wraps the original.
type renamedErr struct {
	msg string
	err error
}

func (e renamedErr) Error() string { return e.msg }
func (e renamedErr) Unwrap() error { return e.err }

// renameErr replaces the prefix from of err's message with to.
func renameErr(err error, from, to string) error {
	return renamedErr{to + strings.TrimPrefix(err.Error(), from), err}
}

// deviceParams are the info fields of the VM's parameters
// (runtime/board/params.h; runtime/board board_params_format), in their order.
var deviceParams = []string{"actor_memory", "memory", "max_actors", "max_ports", "max_links", "code_mem", "bytes_handles"}

// deviceInfo asks the board for its info line and prints it, one field a
// line.
func deviceInfo(rw io.ReadWriter, out io.Writer) error {
	c := newBoardConn(rw)
	defer c.close()
	_, text, err := c.request("info", nil, 5*time.Second)
	if be, ok := err.(errBoard); ok && (be.reason == "bad key" || strings.HasPrefix(be.reason, "usage:")) {
		return fmt.Errorf("gogo device: this firmware has no info (it is older than gogo device info; update it with gogo setup); its boot line, shown by `gogo monitor -reset`, has its version")
	}
	if err != nil {
		return fmt.Errorf("gogo device: %w", err)
	}
	m := parseBoardInfo(text)
	if v, ok := m["fw"]; ok {
		fmt.Fprintf(out, "firmware  gogo %s\n", v)
	}
	if v, ok := m["board"]; ok {
		if t := setupBoardTitles[v]; t != "" {
			v += " (" + t + ")"
		}
		fmt.Fprintf(out, "board     %s\n", v)
	}
	if v, ok := m["image"]; ok {
		major, _, _ := strings.Cut(v, ".")
		fmt.Fprintf(out, "image     format %s (runs images %s.0 to %s)\n", v, major, v)
	}
	// The VM's parameters (a firmware since they were reported), in the
	// order the board gives them.
	var lim []string
	for _, k := range deviceParams {
		if v, ok := m[k]; ok {
			lim = append(lim, k+"="+v)
		}
	}
	if len(lim) > 0 {
		fmt.Fprintf(out, "limits    %s\n", strings.Join(lim, " "))
	}
	if v, ok := m["provides"]; ok {
		fmt.Fprintf(out, "provides  %s\n", strings.ReplaceAll(v, ",", " "))
	}
	return nil
}
