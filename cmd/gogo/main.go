// Command gogo compiles gogo programs to VM images, runs and disassembles them.
//
//	gogo build [-path dirs] <dir|file.gg> [-o out.ggb]
//	gogo run [--key c=pin]... [--fs-root DIR] [-path dirs] <dir|file.gg|file.ggb>
//	gogo env [-path dirs]
//	gogo dis <file.ggb>
//	gogo flash [-port P] [-esptool path] app.ggb
//	gogo monitor [-port P] [-t seconds] [-until text] [-reset]
//	gogo firmware -board sticks3|cardputer [-o dir] [-build] [-with list] [-without list] files...
//	gogo config [-port P | -local] set <key> <value|-> | get <key> | list | rm <key> | scan | stats
//	gogo setup [-port P] [-board sticks3|cardputer] [-version vX.Y.Z] [-release-url URL|DIR] [-esptool path] [-offline] [-skip-wifi] [-new-layout] [-yes] [-dry-run]
//	gogo version
//
// `gogo build` compiles a directory (the build root) or one .gg file. Imports
// outside gogo/ are user packages, found in the build root and then in the
// search path: the directories of -path, else of $GOGOPATH, else $HOME/gogo
// (compiler.Config). `gogo env` prints the search path, the VM and the version.
//
// `gogo run` builds a directory or a .gg file (one starting with #!, an
// executable script, counts as one too) to a temporary image first, then
// runs the VM and exits with its status. The VM is found by findVM.
//
// `gogo flash` writes an image to the ESP32-S3 image partition with an
// external esptool (see flash.go); `gogo monitor` captures the board's
// serial console (see monitor.go). `gogo config` edits the configuration
// store that sys.Config reads, on the board or locally (see config.go).
// `gogo setup` takes a new board to a running program (see setup.go).
package main

import (
	"bufio"
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"os/signal"
	"path/filepath"
	"strings"
	"syscall"

	"gogotools/compiler"
)

const usage = "usage: gogo build [-path dirs] <dir|file" + sourceExt + "> [-o out" + imageExt + "]\n       gogo run [--key c=pin]... [--fs-root DIR] [-path dirs] <dir|file" + sourceExt + "|file" + imageExt + ">\n       gogo env [-path dirs]\n       gogo dis <file" + imageExt + ">\n       gogo flash [-port P] [-esptool path] app" + imageExt + "\n       gogo monitor [-port P] [-t seconds] [-until text] [-reset]\n       gogo firmware -board sticks3|cardputer [-o dir] [-build] [-with list] [-without list] files...\n       gogo config [-port P | -local] set <key> <value|-> | get <key> | list | rm <key> | scan | stats\n       gogo setup [-port P] [-board sticks3|cardputer] [-version vX.Y.Z] [-release-url URL|DIR] [-esptool path] [-offline] [-skip-wifi] [-new-layout] [-yes] [-dry-run]\n       gogo version"

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
	// /flash and /sd) are forwarded to gogovm; -path dirs is the package
	// search path of the build.
	var vmArgs, buildArgs []string
	for len(args) > 0 && (args[0] == "--key" || args[0] == "--fs-root" || args[0] == "-path" || args[0] == "--path") {
		if len(args) < 2 {
			switch args[0] {
			case "--fs-root":
				return 1, errors.New("gogo run: --fs-root needs a directory\n" + usage)
			case "--key":
				return 1, errors.New("gogo run: --key needs a value like c=pin\n" + usage)
			}
			return 1, errors.New("gogo run: -path needs a list of directories\n" + usage)
		}
		if args[0] == "-path" || args[0] == "--path" {
			buildArgs = append(buildArgs, "-path", args[1])
		} else {
			vmArgs = append(vmArgs, args[0], args[1])
		}
		args = args[2:]
	}
	if len(args) == 0 {
		return 1, errors.New("gogo run: missing program (a directory, a " + sourceExt + " file or a " + imageExt + " image)\n" + usage)
	}
	if len(args) != 1 {
		if len(args) > 1 && !strings.HasPrefix(args[1], "-") {
			return 1, fmt.Errorf("gogo run: arguments after the program (%s) are not supported yet\n%s", strings.Join(args[1:], " "), usage)
		}
		return 1, errors.New(usage)
	}
	target := args[0]
	fi, err := os.Stat(target)
	if err != nil {
		return 1, err
	}
	image := target
	if fi.IsDir() || isSourceFile(target) {
		tmp, err := os.MkdirTemp("", "gogo-run-")
		if err != nil {
			return 1, err
		}
		defer os.RemoveAll(tmp)
		image = filepath.Join(tmp, "main"+imageExt)
		if err := build(append(buildArgs, target, "-o", image)); err != nil {
			return 1, err
		}
	} else if len(buildArgs) > 0 {
		return 1, errors.New("gogo run: -path is for building a directory or " + sourceExt + " file, not for an image\n" + usage)
	}
	vm, _, err := findVM()
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

// findVM returns the VM `gogo run` starts and where it was found: $GOGO_VM
// if set, else build/gogovm in the gogo repository (built by `make vm`),
// the first directory containing a go.mod declaring `module gogotools`,
// searched upwards from the gogo executable and then from the current
// directory.
func findVM() (vm, from string, err error) {
	if vm := os.Getenv("GOGO_VM"); vm != "" {
		return vm, "$GOGO_VM", nil
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
				return "", "", fmt.Errorf("gogo: VM not found at %s (run `make vm` in %s, or set GOGO_VM)", vm, repo)
			}
			return vm, "the gogo repository", nil
		}
	}
	return "", "", errors.New("gogo: cannot find the VM: set GOGO_VM or run from inside the gogo repository")
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
	case "env":
		return envCmd(args[1:], os.Stdout)
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
		fmt.Printf("gogo %s (image format %d.%d)\n", version, compiler.ImageMajor, compiler.ImageMinor)
		return nil
	}
	return errors.New(usage)
}

// pathFlag parses the value of -path: nil (the default search path,
// compiler.SearchPath) when not given.
type pathFlag struct {
	set  bool
	dirs []string
}

func (p pathFlag) path() []string {
	if p.set {
		return p.dirs
	}
	return compiler.SearchPath()
}

func build(args []string) error {
	var dir, out string
	var path pathFlag
	for i := 0; i < len(args); i++ {
		switch {
		case args[i] == "-o" && i+1 < len(args):
			out = args[i+1]
			i++
		case (args[i] == "-path" || args[i] == "--path") && i+1 < len(args):
			path = pathFlag{true, compiler.SplitPath(args[i+1])}
			i++
		case dir == "" && !strings.HasPrefix(args[i], "-"):
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
		out = strings.TrimSuffix(filepath.Base(abs), sourceExt) + imageExt
	}
	prog, err := loadSource(dir, path.path())
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

// isSourceFile reports whether target is a gogo source file rather than an
// image: a file ending in .gg, or starting with #! (an executable script).
func isSourceFile(target string) bool {
	fi, err := os.Stat(target)
	if err != nil || fi.IsDir() {
		return false
	}
	if strings.HasSuffix(target, sourceExt) {
		return true
	}
	f, err := os.Open(target)
	if err != nil {
		return false
	}
	defer f.Close()
	head := make([]byte, 2)
	n, _ := f.Read(head)
	return string(head[:n]) == "#!"
}

// loadSource loads the program at target, a directory (the build root) or
// one source file, with search path path, and prints the warnings about
// packages found in more than one place.
func loadSource(target string, path []string) (*compiler.Program, error) {
	cfg := compiler.Config{Path: path}
	var prog *compiler.Program
	var err error
	if isSourceFile(target) {
		prog, err = cfg.LoadFile(target)
	} else {
		prog, err = cfg.Load(target)
	}
	if err != nil {
		return nil, err
	}
	for _, w := range prog.Warnings {
		fmt.Fprintln(os.Stderr, "gogo: warning:", w)
	}
	return prog, nil
}

// envCmd implements `gogo env`: the version, the package search path and
// the VM `gogo run` would use.
func envCmd(args []string, out io.Writer) error {
	var path pathFlag
	from := "$GOGOPATH"
	if _, set := os.LookupEnv("GOGOPATH"); !set {
		from = "$HOME/gogo, GOGOPATH is unset"
	}
	switch {
	case len(args) == 2 && (args[0] == "-path" || args[0] == "--path"):
		path = pathFlag{true, compiler.SplitPath(args[1])}
		from = "-path"
	case len(args) != 0:
		return errors.New(usage)
	}
	fmt.Fprintf(out, "version   %s (image format %d.%d)\n", version, compiler.ImageMajor, compiler.ImageMinor)
	dirs := path.path()
	fmt.Fprintf(out, "GOGOPATH  %s (%s)\n", strings.Join(dirs, string(os.PathListSeparator)), from)
	fmt.Fprintf(out, "search    1. the build root (imports starting with its directory's name)\n")
	for i, d := range dirs {
		fmt.Fprintf(out, "          %d. %s\n", i+2, d)
	}
	if vm, vmFrom, err := findVM(); err != nil {
		fmt.Fprintf(out, "gogovm    not found: %v\n", err)
	} else {
		fmt.Fprintf(out, "gogovm    %s (%s)\n", vm, vmFrom)
	}
	return nil
}
