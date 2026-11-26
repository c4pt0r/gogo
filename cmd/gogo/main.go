// Command gogo compiles gogo programs to VM images, runs and disassembles them.
//
//	gogo build <dir> [-o out.ggb]
//	gogo run [--key c=pin]... [--fs-root DIR] <dir|file.ggb>
//	gogo dis <file.ggb>
//	gogo flash [-port P] [-esptool path] app.ggb
//	gogo monitor [-port P] [-t seconds] [-until text] [-reset]
//	gogo firmware -board sticks3|cardputer [-o dir] [-build] [-with list] [-without list] files...
//	gogo config [-port P | -local] set <key> <value|-> | get <key> | list | rm <key> | scan | stats
//	gogo setup [-port P] [-board sticks3|cardputer] [-version vX.Y.Z] [-release-url URL|DIR] [-esptool path] [-offline] [-skip-wifi] [-new-layout] [-yes] [-dry-run]
//	gogo version
//
// `gogo run` builds a directory to a temporary image first, then runs the VM
// and exits with its status. The VM is $GOGO_VM if set, else
// <repo>/build/gogovm (built by `make vm`), where <repo> is the first
// directory containing a go.mod declaring `module gogotools` (the repository root), searched upwards
// from the gogo executable and then from the current directory.
//
// `gogo flash` writes an image to the ESP32-S3 image partition with an
// external esptool (see flash.go); `gogo monitor` captures the board's
// serial console (see monitor.go). `gogo config` edits the configuration
// store that config.Get reads, on the board or locally (see config.go).
// `gogo setup` takes a new board to a running program (see setup.go).
package main

import (
	"bufio"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"os/signal"
	"path/filepath"
	"strings"
	"syscall"

	"gogotools/compiler"
)

const usage = "usage: gogo build <dir> [-o out" + imageExt + "]\n       gogo run [--key c=pin]... [--fs-root DIR] <dir|file" + imageExt + ">\n       gogo dis <file" + imageExt + ">\n       gogo flash [-port P] [-esptool path] app" + imageExt + "\n       gogo monitor [-port P] [-t seconds] [-until text] [-reset]\n       gogo firmware -board sticks3|cardputer [-o dir] [-build] [-with list] [-without list] files...\n       gogo config [-port P | -local] set <key> <value|-> | get <key> | list | rm <key> | scan | stats\n       gogo setup [-port P] [-board sticks3|cardputer] [-version vX.Y.Z] [-release-url URL|DIR] [-esptool path] [-offline] [-skip-wifi] [-new-layout] [-yes] [-dry-run]\n       gogo version"

func main() {
	if len(os.Args) > 1 && os.Args[1] == "run" {
		code, err := runCmd(os.Args[2:])
		if err != nil {
			fmt.Fprintln(os.Stderr, err)
		}
		os.Exit(code)
	}
	if err := run(os.Args[1:]); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}

// runCmd implements `gogo run` and returns the process exit status.
func runCmd(args []string) (int, error) {
	// --key c=pin (unix tty mode) and --fs-root DIR (host directory behind
	// /flash and /sd) are forwarded to gogovm.
	var vmArgs []string
	for len(args) > 0 && (args[0] == "--key" || args[0] == "--fs-root") {
		if len(args) < 2 {
			if args[0] == "--fs-root" {
				return 1, errors.New("gogo run: --fs-root needs a directory\n" + usage)
			}
			return 1, errors.New("gogo run: --key needs a value like c=pin\n" + usage)
		}
		vmArgs = append(vmArgs, args[0], args[1])
		args = args[2:]
	}
	if len(args) == 0 {
		return 1, errors.New("gogo run: missing image (a directory or " + imageExt + " file)\n" + usage)
	}
	if len(args) != 1 {
		return 1, errors.New(usage)
	}
	target := args[0]
	fi, err := os.Stat(target)
	if err != nil {
		return 1, err
	}
	image := target
	if fi.IsDir() {
		tmp, err := os.MkdirTemp("", "gogo-run-")
		if err != nil {
			return 1, err
		}
		defer os.RemoveAll(tmp)
		image = filepath.Join(tmp, "main"+imageExt)
		if err := build([]string{target, "-o", image}); err != nil {
			return 1, err
		}
	}
	vm, err := findVM()
	if err != nil {
		return 1, err
	}
	cmd := exec.Command(vm, append(vmArgs, image)...)
	cmd.Stdin, cmd.Stdout, cmd.Stderr = os.Stdin, os.Stdout, os.Stderr
	// Forward SIGINT/SIGTERM to the VM and wait for it, so the VM can
	// restore the terminal, nothing is orphaned and the temp image is
	// removed by the defer above.
	sigs := make(chan os.Signal, 4)
	signal.Notify(sigs, os.Interrupt, syscall.SIGTERM)
	defer signal.Stop(sigs)
	if err := cmd.Start(); err != nil {
		return 1, fmt.Errorf("gogo: running %s: %v", vm, err)
	}
	done := make(chan struct{})
	go func() {
		for {
			select {
			case s := <-sigs:
				cmd.Process.Signal(s)
			case <-done:
				return
			}
		}
	}()
	err = cmd.Wait()
	close(done)
	if err != nil {
		var exit *exec.ExitError
		if errors.As(err, &exit) {
			if ws, ok := exit.Sys().(syscall.WaitStatus); ok && ws.Signaled() {
				return 128 + int(ws.Signal()), nil
			}
			if exit.ExitCode() >= 0 {
				return exit.ExitCode(), nil
			}
		}
		return 1, fmt.Errorf("gogo: running %s: %v", vm, err)
	}
	return 0, nil
}

// findVM returns $GOGO_VM, or build/gogovm in the gogo repository.
func findVM() (string, error) {
	if vm := os.Getenv("GOGO_VM"); vm != "" {
		return vm, nil
	}
	var starts []string
	if exe, err := os.Executable(); err == nil {
		if real, err := filepath.EvalSymlinks(exe); err == nil {
			exe = real
		}
		starts = append(starts, filepath.Dir(exe))
	}
	if wd, err := os.Getwd(); err == nil {
		starts = append(starts, wd)
	}
	for _, dir := range starts {
		if repo, ok := findRepo(dir); ok {
			vm := filepath.Join(repo, "build", "gogovm")
			if _, err := os.Stat(vm); err != nil {
				return "", fmt.Errorf("gogo: VM not found at %s (run `make vm` in %s, or set GOGO_VM)", vm, repo)
			}
			return vm, nil
		}
	}
	return "", errors.New("gogo: cannot find the VM: set GOGO_VM or run from inside the gogo repository")
}

// findRepo walks up from dir to the directory whose go.mod says `module gogotools`.
func findRepo(dir string) (string, bool) {
	for {
		if isGogoModule(filepath.Join(dir, "go.mod")) {
			return dir, true
		}
		parent := filepath.Dir(dir)
		if parent == dir {
			return "", false
		}
		dir = parent
	}
}

func isGogoModule(path string) bool {
	f, err := os.Open(path)
	if err != nil {
		return false
	}
	defer f.Close()
	sc := bufio.NewScanner(f)
	for sc.Scan() {
		fields := strings.Fields(sc.Text())
		if len(fields) == 2 && fields[0] == "module" {
			return fields[1] == "gogotools"
		}
	}
	return false
}

func run(args []string) error {
	if len(args) == 0 {
		return errors.New(usage)
	}
	switch args[0] {
	case "build":
		return build(args[1:])
	case "dis":
		if len(args) != 2 {
			return errors.New(usage)
		}
		img, err := os.ReadFile(args[1])
		if err != nil {
			return err
		}
		text, err := compiler.Disassemble(img)
		if err != nil {
			return err
		}
		fmt.Print(text)
		return nil
	case "flash":
		return defaultFlasher().flash(args[1:])
	case "monitor":
		return monitorCmd(args[1:])
	case "config":
		return configCmd(args[1:], os.Stdout)
	case "firmware":
		return firmwareCmd(args[1:], os.Stdout, runIdfInRepo)
	case "setup":
		return setupCmd(args[1:])
	case "version":
		if len(args) != 1 {
			return errors.New(usage)
		}
		fmt.Printf("gogo %s (image version %d)\n", version, compiler.ImageVersion)
		return nil
	}
	return errors.New(usage)
}

func build(args []string) error {
	var dir, out string
	for i := 0; i < len(args); i++ {
		switch {
		case args[i] == "-o" && i+1 < len(args):
			out = args[i+1]
			i++
		case dir == "" && args[i] != "-o":
			dir = args[i]
		default:
			return errors.New(usage)
		}
	}
	if dir == "" {
		return errors.New(usage)
	}
	if out == "" {
		abs, err := filepath.Abs(dir)
		if err != nil {
			return err
		}
		out = filepath.Base(abs) + imageExt
	}
	prog, err := compiler.Load(dir)
	if err != nil {
		return err
	}
	img, err := compiler.Compile(prog)
	if err != nil {
		return err
	}
	_, warnings := compiler.Requires(prog)
	for _, w := range warnings {
		fmt.Fprintln(os.Stderr, "gogo: warning:", w)
	}
	return os.WriteFile(out, img, 0o644)
}
