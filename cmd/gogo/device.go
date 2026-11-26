package main

// gogo device: operations on the board itself, over its serial console
// (the \x02gogo-config frames of config.go's boardConn):
//
//	gogo device [-port P] scan                  the Wi-Fi networks it sees
//	gogo device [-port P] stats                 its counters and memory
//	gogo device [-port P] [-yes] format-flash   format /flash
//
// format-flash formats the gogo LittleFS partition (gogofs) and nothing
// else: the board restarts and formats it at mount (fs_lfs.c). It never
// erases the chip and never writes NVS (the configuration and Wi-Fi
// settings stay). It asks for "yes" on stdin unless -yes is given.
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

const deviceUsage = "usage: gogo device [-port P] [-yes] scan | stats | format-flash"

// deviceEnv holds the side effects of gogo device, replaced in tests.
type deviceEnv struct {
	out, errOut io.Writer
	in          io.Reader
	glob        func(string) ([]string, error)
	open        func(string) (io.ReadWriteCloser, error)
}

func deviceCmd(args []string) error {
	return deviceMain(args, deviceEnv{
		out: os.Stdout, errOut: os.Stderr, in: os.Stdin, glob: filepath.Glob,
		open: func(p string) (io.ReadWriteCloser, error) { return openSerial(p) },
	})
}

func deviceMain(args []string, env deviceEnv) error {
	fs := newFlags("device")
	port := fs.String("port", "", "")
	yes := fs.Bool("yes", false, "")
	pos, err := parseFlags(fs, args, deviceUsage)
	if err != nil {
		return err
	}
	if len(pos) != 1 {
		return usageError("device", "want one operation", deviceUsage)
	}
	op := pos[0]
	if op != "scan" && op != "stats" && op != "format-flash" {
		return usageError("device", fmt.Sprintf("unknown operation %q", op), deviceUsage)
	}
	if *port == "" {
		if *port, err = detectPort(env.glob); err != nil {
			return err
		}
	}
	if op == "format-flash" && !*yes {
		fmt.Fprintf(env.errOut, "This deletes every file on /flash of the board on %s (settings in NVS stay).\nType yes to format it: ", *port)
		line, _ := bufio.NewReader(env.in).ReadString('\n')
		if strings.TrimSpace(line) != "yes" {
			return fmt.Errorf("gogo device: not formatted (answer yes, or pass -yes)")
		}
	}
	f, err := env.open(*port)
	if err != nil {
		return err
	}
	defer f.Close()
	if op != "format-flash" {
		timeout := 5 * time.Second
		if op == "scan" {
			timeout = 20 * time.Second
		}
		if err := boardConfig(f, op, nil, env.out, timeout); err != nil {
			return fmt.Errorf("gogo device: %s", strings.TrimPrefix(err.Error(), "gogo config: "))
		}
		return nil
	}
	c := newBoardConn(f)
	defer c.close()
	if _, _, err := c.request("format-flash", nil, 5*time.Second); err != nil {
		if be, ok := err.(errBoard); ok && (be.reason == "bad key" || strings.HasPrefix(be.reason, "usage:")) {
			return fmt.Errorf("gogo device: this firmware has no format-flash (update it); `gogo config set fs.flash.format yes` and a restart do the same")
		}
		return fmt.Errorf("gogo device: %v", err)
	}
	fmt.Fprintf(env.out, "the board restarts and formats /flash; `gogo monitor -timeout 5` shows \"gogo: formatting /flash\"\n")
	return nil
}
