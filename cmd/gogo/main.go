// Command gogo compiles gogo programs to VM images, runs and disassembles them.
//
//	gogo build <dir> [-o out.gogo]
//	gogo run <dir|file.gogo>
//	gogo dis <file.gogo>
//
// `gogo run` builds a directory to a temporary image first, then runs the VM
// and exits with its status. The VM is $GOGO_VM if set, else
// <repo>/build/gogovm (built by `make vm`), where <repo> is the first
// directory containing a go.mod declaring `module gogo`, searched upwards
// from the gogo executable and then from the current directory.
package main

import (
	"bufio"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"

	"gogo/compiler"
)

const usage = "usage: gogo build <dir> [-o out.gogo]\n       gogo run <dir|file.gogo>\n       gogo dis <file.gogo>"

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
		image = filepath.Join(tmp, "main.gogo")
		if err := build([]string{target, "-o", image}); err != nil {
			return 1, err
		}
	}
	vm, err := findVM()
	if err != nil {
		return 1, err
	}
	cmd := exec.Command(vm, image)
	cmd.Stdin, cmd.Stdout, cmd.Stderr = os.Stdin, os.Stdout, os.Stderr
	if err := cmd.Run(); err != nil {
		var exit *exec.ExitError
		if errors.As(err, &exit) && exit.ExitCode() >= 0 {
			return exit.ExitCode(), nil
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

// findRepo walks up from dir to the directory whose go.mod says `module gogo`.
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
			return fields[1] == "gogo"
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
		out = filepath.Base(abs) + ".gogo"
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
