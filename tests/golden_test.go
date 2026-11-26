// Package tests holds end-to-end tests that compile gogo programs and run
// them on the C VM.
package tests

import (
	"bytes"
	"context"
	"os"
	"os/exec"
	"path/filepath"
	"slices"
	"strings"
	"testing"
	"time"

	"gogotools/compiler"
)

// TestGolden compiles every tests/golden/*/ program (and every
// examples/*/ with an expected.out), runs it on the VM named
// by $GOGO_VM (30s timeout) and requires exit status 0 and stdout equal
// byte-for-byte to expected.out; if expected.err exists, each of its
// non-empty lines must be a substring of stderr, and no non-empty line of
// forbidden.err may be. An `input` file is fed to stdin; a `config` file
// is the config store ($GOGO_CONFIG). A file `fsroot` runs the VM with
// --fs-root on a fresh temporary directory (gogovm creates flash/ in it;
// the runner creates sd/ too if a line of fsroot has the field "sd", and a line
// "file flash/My File" creates that file, containing "seed", first). A file
// `vmargs` holds extra VM flags (whitespace-separated, before the image); a
// file `exitfail` makes a non-zero exit status the expected one. A file `mcu`
// also runs it on $GOGO_VM_MCU (the MCU-config VM). `make test-go` builds
// both VMs and sets GOGO_VM and GOGO_VM_MCU.
func TestGolden(t *testing.T) {
	vm := os.Getenv("GOGO_VM")
	if vm == "" {
		t.Fatal("GOGO_VM is not set: run `make test-go`, or build the VM with `make vm` and set GOGO_VM=$PWD/build/gogovm")
	}
	dirs, err := filepath.Glob(filepath.Join("golden", "*"))
	if err != nil {
		t.Fatal(err)
	}
	// ../examples/*/ that ship an expected.out are goldens too.
	exs, err := filepath.Glob(filepath.Join("..", "examples", "*", "expected.out"))
	if err != nil {
		t.Fatal(err)
	}
	for _, e := range exs {
		dirs = append(dirs, filepath.Dir(e))
	}
	if len(dirs) == 0 {
		t.Fatal("no golden programs found")
	}
	for _, dir := range dirs {
		if fi, err := os.Stat(dir); err != nil || !fi.IsDir() {
			continue
		}
		t.Run(filepath.Base(dir), func(t *testing.T) {
			want, err := os.ReadFile(filepath.Join(dir, "expected.out"))
			if err != nil {
				t.Fatal(err)
			}
			prog, err := compiler.Load(dir)
			if err != nil {
				t.Fatalf("load: %v", err)
			}
			img, err := compiler.Compile(prog)
			if err != nil {
				t.Fatalf("compile: %v", err)
			}
			imgPath := filepath.Join(t.TempDir(), filepath.Base(dir)+".ggb")
			if err := os.WriteFile(imgPath, img, 0o644); err != nil {
				t.Fatal(err)
			}
			runOn := func(t *testing.T, vm string) {
				ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
				defer cancel()
				var stdout, stderr bytes.Buffer
				vmArgs := []string{imgPath}
				var mods []string // mods/<name>/ directories only
				if ents, err := os.ReadDir(filepath.Join(dir, "mods")); err == nil {
					for _, e := range ents {
						if e.IsDir() {
							mods = append(mods, filepath.Join(dir, "mods", e.Name()))
						}
					}
				}
				mark, markErr := os.ReadFile(filepath.Join(dir, "fsroot"))
				if markErr == nil || len(mods) > 0 {
					root := t.TempDir()
					if len(mods) > 0 {
						if err := os.MkdirAll(filepath.Join(root, "flash"), 0o755); err != nil {
							t.Fatal(err)
						}
					}
					for _, mod := range mods {
						mprog, err := compiler.Load(mod)
						if err != nil {
							t.Fatalf("load module %s: %v", mod, err)
						}
						mimg, err := compiler.Compile(mprog)
						if err != nil {
							t.Fatalf("compile module %s: %v", mod, err)
						}
						if err := os.WriteFile(filepath.Join(root, "flash", filepath.Base(mod)+".gmod"), mimg, 0o644); err != nil {
							t.Fatal(err)
						}
					}
					for _, line := range strings.Split(string(mark), "\n") {
						if rest, ok := strings.CutPrefix(line, "file "); ok { // seed file "<medium>/<path>"
							p := filepath.Join(root, rest)
							if err := os.MkdirAll(filepath.Dir(p), 0o755); err != nil {
								t.Fatal(err)
							}
							if err := os.WriteFile(p, []byte("seed"), 0o644); err != nil {
								t.Fatal(err)
							}
						} else if slices.Contains(strings.Fields(line), "sd") {
							if err := os.MkdirAll(filepath.Join(root, "sd"), 0o755); err != nil {
								t.Fatal(err)
							}
						}
					}
					vmArgs = []string{"--fs-root", root, imgPath}
				}
				if extra, err := os.ReadFile(filepath.Join(dir, "vmargs")); err == nil {
					vmArgs = append(strings.Fields(string(extra)), vmArgs...)
				}
				_, statErr := os.Stat(filepath.Join(dir, "exitfail"))
				wantFail := statErr == nil
				cmd := exec.CommandContext(ctx, vm, vmArgs...)
				cmd.Stdout = &stdout
				cmd.Stderr = &stderr
				// The config store (sys.Config) is the optional `config`
				// file, else a file that does not exist: never ~/.gogo/config.
				cfg := filepath.Join(t.TempDir(), "no-config")
				if _, err := os.Stat(filepath.Join(dir, "config")); err == nil {
					cfg, _ = filepath.Abs(filepath.Join(dir, "config"))
				}
				cmd.Env = append(os.Environ(), "GOGO_CONFIG="+cfg)
				// An optional `input` file is stdin (simulated gpio events);
				// otherwise stdin is /dev/null.
				if in, err := os.Open(filepath.Join(dir, "input")); err == nil {
					defer in.Close()
					cmd.Stdin = in
				} else if !os.IsNotExist(err) {
					t.Fatal(err)
				}
				cmd.WaitDelay = time.Second // don't wait on pipes held by orphans
				runErr := cmd.Run()
				if ctx.Err() != nil {
					t.Fatalf("VM timed out after 30s\n--- stdout ---\n%s\n--- stderr ---\n%s", stdout.Bytes(), stderr.Bytes())
				}
				if wantFail {
					if runErr == nil {
						t.Errorf("VM exited 0, want a non-zero status")
					}
				} else if runErr != nil {
					t.Errorf("VM failed: %v (want exit status 0)\n--- stderr ---\n%s", runErr, stderr.Bytes())
				}
				if !bytes.Equal(stdout.Bytes(), want) {
					t.Errorf("stdout mismatch\n--- got ---\n%s\n--- want ---\n%s\n--- stderr ---\n%s",
						stdout.Bytes(), want, stderr.Bytes())
				}
				// Optional expected.err: each non-empty line must appear
				// somewhere in stderr.
				wantErr, err := os.ReadFile(filepath.Join(dir, "expected.err"))
				if err != nil && !os.IsNotExist(err) {
					t.Fatal(err)
				}
				for _, line := range strings.Split(string(wantErr), "\n") {
					if line != "" && !strings.Contains(stderr.String(), line) {
						t.Errorf("stderr lacks %q\n--- stderr ---\n%s", line, stderr.Bytes())
					}
				}
				// Optional forbidden.err: no non-empty line may appear in stderr.
				noErr, err := os.ReadFile(filepath.Join(dir, "forbidden.err"))
				if err != nil && !os.IsNotExist(err) {
					t.Fatal(err)
				}
				for _, line := range strings.Split(string(noErr), "\n") {
					if line != "" && strings.Contains(stderr.String(), line) {
						t.Errorf("stderr has forbidden %q\n--- stderr ---\n%s", line, stderr.Bytes())
					}
				}
			}
			runOn(t, vm)
			// A file `mcu` runs the golden again on $GOGO_VM_MCU (the VM
			// with the board actor memory limit), when it is set.
			if _, err := os.Stat(filepath.Join(dir, "mcu")); err == nil {
				if mcuVM := os.Getenv("GOGO_VM_MCU"); mcuVM != "" {
					t.Run("mcu", func(t *testing.T) { runOn(t, mcuVM) })
				} else {
					t.Log("GOGO_VM_MCU is not set: the mcu run is skipped")
				}
			}
		})
	}
}
