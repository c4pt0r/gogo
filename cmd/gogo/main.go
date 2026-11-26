// Command gogo compiles gogo programs to VM images, runs and disassembles
// them, and puts them on a board. `gogo` without arguments prints the
// usage of every command (the usage variable); flags.go says how every
// command parses its flags.
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
// `gogo info` prints an image's format, size, requirements and entries
// (see info.go).
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
	"flag"
	"fmt"
	"io"
	"os"
	"os/exec"
	"os/signal"
	"path/filepath"
	"runtime"
	"strings"
	"syscall"

	"gogotools/compiler"
)

// usage lists every command; each command's own usage text (buildUsage,
// runUsage, ...) is printed with its errors.
var usage = "usage: " + strings.Join([]string{
	strings.TrimPrefix(buildUsage, "usage: "),
	strings.TrimPrefix(runUsage, "usage: "),
	strings.TrimPrefix(envUsage, "usage: "),
	strings.TrimPrefix(disUsage, "usage: "),
	strings.TrimPrefix(infoUsage, "usage: "),
	strings.TrimPrefix(flashUsage, "usage: "),
	strings.TrimPrefix(monitorUsage, "usage: "),
	strings.TrimPrefix(firmwareUsage, "usage: "),
	strings.TrimPrefix(configUsage, "usage: "),
	strings.TrimPrefix(setupUsage, "usage: "),
	"gogo version",
}, "\n       ")

const (
	buildUsage = "usage: gogo build [-path dirs] [-o out" + imageExt + "] <dir|file" + sourceExt + ">"
	envUsage   = "usage: gogo env [-path dirs]"
	disUsage   = "usage: gogo dis <file" + imageExt + ">"
)

func main() {
	var code int
	var err error
	if len(os.Args) > 1 && os.Args[1] == "run" {
		code, err = runCmd(os.Args[2:])
	} else if err = run(os.Args[1:]); err != nil {
		code = exitUsage
	}
	var help helpError
	if errors.As(err, &help) {
		fmt.Println(help.usage)
		os.Exit(0)
	}
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
	}
	os.Exit(code)
}

const runUsage = "usage: gogo run [--key c=pin]... [--fs-root DIR] [-path dirs] <dir|file" + sourceExt + "|file" + imageExt + ">"

// runCmd implements `gogo run` and returns the process exit status: the
// VM's (exitcodes.go), or exitUsage for a usage, build or toolchain error.
func runCmd(args []string) (int, error) {
	// -key c=pin (unix tty mode) and -fs-root DIR (host directory behind
	// /flash and /sd) are forwarded to gogovm; -path dirs is the package
	// search path of the build.
	fs := newFlags("run")
	var keys listFlag
	fs.Var(&keys, "key", "")
	fsRoot := fs.String("fs-root", "", "")
	getPath := addPathFlag(fs)
	// Everything after the program is the program's (refused for now),
	// as for `go run`: flags come before it.
	args, err := parseFlagsRaw(fs, args, runUsage, func(pos []string) bool { return len(pos) > 0 })
	if err != nil {
		return exitUsage, err
	}
	var vmArgs []string
	path := getPath()
	for _, k := range keys {
		vmArgs = append(vmArgs, "--key", k)
	}
	if *fsRoot != "" {
		vmArgs = append(vmArgs, "--fs-root", *fsRoot)
	}
	if len(args) == 0 {
		return exitUsage, usageError("run", "missing program (a directory, a "+sourceExt+" file or a "+imageExt+" image)", runUsage)
	}
	if len(args) != 1 {
		return exitUsage, usageError("run", fmt.Sprintf("arguments after the program (%s) are not supported yet", strings.Join(args[1:], " ")), runUsage)
	}
	target := args[0]
	fi, err := os.Stat(target)
	if err != nil {
		return exitUsage, err
	}
	image := target
	if fi.IsDir() || isSourceFile(target) {
		tmp, err := os.MkdirTemp("", "gogo-run-")
		if err != nil {
			return exitUsage, err
		}
		defer os.RemoveAll(tmp)
		image = filepath.Join(tmp, "main"+imageExt)
		if _, err := buildImage(target, image, path.path()); err != nil {
			return exitUsage, err
		}
	} else if path.set {
		return exitUsage, usageError("run", "-path is for building a directory or "+sourceExt+" file, not for an image", runUsage)
	}
	vm, _, err := findVM()
	if err != nil {
		return exitUsage, err
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
		return exitUsage, fmt.Errorf("gogo: running %s: %v", vm, err)
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
		return exitUsage, fmt.Errorf("gogo: running %s: %v", vm, err)
	}
	return 0, nil
}

// vmName is the VM's file name, and vmAsset the name of the release asset
// that holds it for this OS and CPU (tools/release.sh).
var (
	vmName  = "gogovm" + exeSuffix(runtime.GOOS)
	vmAsset = "gogovm-" + runtime.GOOS + "-" + runtime.GOARCH + exeSuffix(runtime.GOOS)
)

func exeSuffix(goos string) string {
	if goos == "windows" {
		return ".exe"
	}
	return ""
}

// findVM returns the VM `gogo run` starts and where it was found: $GOGO_VM
// if set; else gogovm (or the release asset name gogovm-<os>-<arch>) in
// the directory of the gogo executable, as a release installs them; else
// build/gogovm in the gogo repository (built by `make vm`), the first
// directory containing a go.mod declaring `module gogotools`, searched
// upwards from the gogo executable and then from the current directory.
func findVM() (vm, from string, err error) {
	if vm := os.Getenv("GOGO_VM"); vm != "" {
		return vm, "$GOGO_VM", nil
	}
	var exeDir string
	if exe, err := os.Executable(); err == nil {
		if real, err := filepath.EvalSymlinks(exe); err == nil {
			exe = real
		}
		exeDir = filepath.Dir(exe)
	}
	wd, _ := os.Getwd()
	return lookVM(exeDir, wd)
}

// lookVM is findVM without $GOGO_VM: exeDir is the gogo executable's
// directory, wd the current directory ("" for unknown).
func lookVM(exeDir, wd string) (string, string, error) {
	var starts []string
	if exeDir != "" {
		for _, name := range []string{vmName, vmAsset} {
			vm := filepath.Join(exeDir, name)
			if fi, err := os.Stat(vm); err == nil && !fi.IsDir() {
				return vm, "next to gogo", nil
			}
		}
		starts = append(starts, exeDir)
	}
	if wd != "" {
		starts = append(starts, wd)
	}
	for _, dir := range starts {
		if repo, ok := findRepo(dir); ok {
			vm := filepath.Join(repo, "build", vmName)
			if _, err := os.Stat(vm); err != nil {
				return "", "", fmt.Errorf("gogo: VM not found at %s (run `make vm` in %s, or set GOGO_VM)", vm, repo)
			}
			return vm, "the gogo repository", nil
		}
	}
	where := "next to gogo"
	if exeDir != "" {
		where = "in " + exeDir
	}
	fix := "download " + vmAsset + " from a release into the directory of gogo (under that name, or renamed " + vmName + "), or set GOGO_VM"
	if runtime.GOOS != "darwin" && runtime.GOOS != "linux" {
		fix = "releases have gogovm for macOS and Linux only: build it (make vm) and set GOGO_VM"
	}
	return "", "", fmt.Errorf("gogo: cannot find the VM: no $GOGO_VM, no %s or %s %s, and not inside the gogo repository; %s", vmName, vmAsset, where, fix)
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
		return disCmd(args[1:], os.Stdout)
	case "info":
		return infoCmd(args[1:], os.Stdout)
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
		if _, err := parseFlags(newFlags("version"), args[1:], "usage: gogo version"); err != nil {
			return err
		} else if len(args) > 1 {
			return usageError("version", "no arguments expected", "usage: gogo version")
		}
		fmt.Printf("gogo %s (image format %d.%d)\n", version, compiler.ImageMajor, compiler.ImageMinor)
		return nil
	case "-h", "-help", "--help", "help":
		return helpError{usage}
	}
	return fmt.Errorf("gogo: unknown command %q\n%s", args[0], usage)
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

// addPathFlag defines -path on fs; call get after parsing.
func addPathFlag(fs *flag.FlagSet) (get func() pathFlag) {
	v := fs.String("path", "", "")
	return func() pathFlag {
		if !flagSet(fs, "path") {
			return pathFlag{}
		}
		return pathFlag{true, compiler.SplitPath(*v)}
	}
}

func build(args []string) error {
	fs := newFlags("build")
	out := fs.String("o", "", "")
	path := addPathFlag(fs)
	pos, err := parseFlags(fs, args, buildUsage)
	if err != nil {
		return err
	}
	if len(pos) != 1 {
		return usageError("build", "want one directory or "+sourceExt+" file", buildUsage)
	}
	_, err = buildImage(pos[0], *out, path().path())
	return err
}

// buildImage compiles target (a directory or a source file) with search
// path path to the image file out (default: named after target, in the
// current directory) and returns out. Warnings go to standard error.
func buildImage(target, out string, path []string) (string, error) {
	if out == "" {
		abs, err := filepath.Abs(target)
		if err != nil {
			return "", err
		}
		out = strings.TrimSuffix(filepath.Base(abs), sourceExt) + imageExt
	}
	prog, err := loadSource(target, path)
	if err != nil {
		return "", err
	}
	img, err := compiler.Compile(prog)
	if err != nil {
		return "", err
	}
	_, warnings := compiler.Requires(prog)
	for _, w := range warnings {
		fmt.Fprintln(os.Stderr, "gogo: warning:", w)
	}
	return out, os.WriteFile(out, img, 0o644)
}

// disCmd implements `gogo dis`.
func disCmd(args []string, out io.Writer) error {
	pos, err := parseFlags(newFlags("dis"), args, disUsage)
	if err != nil {
		return err
	}
	if len(pos) != 1 {
		return usageError("dis", "want one image file", disUsage)
	}
	img, err := os.ReadFile(pos[0])
	if err != nil {
		return err
	}
	text, err := compiler.Disassemble(img)
	if err != nil {
		return err
	}
	fmt.Fprint(out, text)
	return nil
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
	fs := newFlags("env")
	getPath := addPathFlag(fs)
	pos, err := parseFlags(fs, args, envUsage)
	if err != nil {
		return err
	}
	if len(pos) != 0 {
		return usageError("env", "no arguments expected", envUsage)
	}
	path := getPath()
	from := "$GOGOPATH"
	if _, set := os.LookupEnv("GOGOPATH"); !set {
		from = "$HOME/gogo, GOGOPATH is unset"
	}
	if path.set {
		from = "-path"
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
