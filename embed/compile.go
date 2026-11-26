package embed

import (
	"errors"
	"fmt"
	"io/fs"
	"os"
	"path"
	"path/filepath"
	"sort"
	"strings"

	"gogotools/compiler"
)

// CompileOptions says how Compile, CompileFS and CompileDir build a
// program. A nil *CompileOptions is the zero value.
type CompileOptions struct {
	// Path is the package search path: directories searched, in order,
	// for a user package the sources do not hold (as gogo build's -path).
	// nil searches nothing, so a build depends only on its sources and
	// the gogo libraries; compiler.SearchPath() is the one gogo build
	// uses ($GOGOPATH or $HOME/gogo).
	Path []string
	// Root is the name of the build root for Compile and CompileFS: the
	// first element of the import path of the sources' own packages
	// ("app" serves import "app/util" from util/). "" is "main".
	Root string
	// Warn, if not nil, gets each warning gogo build would print (a
	// package found in more than one place of the search, a port.Open
	// whose driver name is not a constant and so is not a requirement).
	Warn func(msg string)
}

func (o *CompileOptions) root() string {
	if o == nil || o.Root == "" {
		return "main"
	}
	return o.Root
}

// Compile compiles a program given as source files to an image, in this
// process: what gogo build makes of a directory holding them. files maps
// slash-separated paths to contents: the .gg files at the top level are
// package main, and those of a subdirectory a user package (util/u.gg is
// imported as Root + "/util"). Files of other kinds are ignored, except
// that a .gg file is read whatever its contents.
//
// The compiler reads a directory, so the files are written to a
// temporary one, removed before Compile returns; error messages name
// them by their keys. Compile may be called from many goroutines at
// once.
func Compile(files map[string][]byte, opts *CompileOptions) ([]byte, error) {
	names := make([]string, 0, len(files))
	for name := range files {
		names = append(names, name)
	}
	sort.Strings(names)
	return compileTree(opts, func(dir string) error {
		for _, name := range names {
			if err := writeSource(dir, name, files[name]); err != nil {
				return err
			}
		}
		return nil
	})
}

// CompileFS is Compile of the files of fsys (every regular file, walked
// from "."), such as an embed.FS or an os.DirFS.
func CompileFS(fsys fs.FS, opts *CompileOptions) ([]byte, error) {
	return compileTree(opts, func(dir string) error {
		return fs.WalkDir(fsys, ".", func(name string, d fs.DirEntry, err error) error {
			if err != nil {
				return err
			}
			if !d.Type().IsRegular() {
				return nil
			}
			data, err := fs.ReadFile(fsys, name)
			if err != nil {
				return err
			}
			return writeSource(dir, name, data)
		})
	})
}

// CompileDir compiles the program at target, a directory (the build
// root) or one .gg file, as gogo build does, reading it in place.
func CompileDir(target string, opts *CompileOptions) ([]byte, error) {
	cfg := compiler.Config{}
	if opts != nil {
		cfg.Path = opts.Path
	}
	var prog *compiler.Program
	var err error
	if fi, serr := os.Stat(target); serr == nil && !fi.IsDir() {
		prog, err = cfg.LoadFile(target)
	} else {
		prog, err = cfg.Load(target)
	}
	if err != nil {
		return nil, err
	}
	return build(prog, opts)
}

// build compiles a loaded program and reports its warnings.
func build(prog *compiler.Program, opts *CompileOptions) ([]byte, error) {
	img, err := compiler.Compile(prog)
	if err != nil {
		return nil, err
	}
	if opts != nil && opts.Warn != nil {
		_, warnings := compiler.Requires(prog)
		for _, w := range append(prog.Warnings, warnings...) {
			opts.Warn(w)
		}
	}
	return img, nil
}

// compileTree writes the sources into a temporary build root (fill) and
// compiles it, naming the files by their paths under the root in errors
// and warnings.
func compileTree(opts *CompileOptions, fill func(dir string) error) ([]byte, error) {
	root := opts.root()
	if root == "." || root == ".." || strings.ContainsAny(root, `/\`) {
		return nil, fmt.Errorf("gogo: bad build root name %q", root)
	}
	tmp, err := os.MkdirTemp("", "gogo-compile-")
	if err != nil {
		return nil, err
	}
	defer os.RemoveAll(tmp)
	dir := filepath.Join(tmp, root)
	if err := os.Mkdir(dir, 0o700); err != nil {
		return nil, err
	}
	if err := fill(dir); err != nil {
		return nil, err
	}
	// Names under the root lose it; the root itself is named by its name.
	strip := func(s string) string {
		s = strings.ReplaceAll(s, dir+string(filepath.Separator), "")
		return strings.ReplaceAll(s, dir, root)
	}
	var o CompileOptions
	if opts != nil {
		o = *opts
	}
	if warn := o.Warn; warn != nil {
		o.Warn = func(msg string) { warn(strip(msg)) }
	}
	prog, err := compiler.Config{Path: o.Path}.Load(dir)
	if err == nil {
		var img []byte
		img, err = build(prog, &o)
		if err == nil {
			return img, nil
		}
	}
	return nil, errors.New(strip(err.Error()))
}

// writeSource writes one source file under the build root dir.
func writeSource(dir, name string, data []byte) error {
	if !fs.ValidPath(name) || name == "." || strings.Contains(name, `\`) {
		return fmt.Errorf("gogo: bad source file name %q", name)
	}
	p := filepath.Join(dir, filepath.FromSlash(name))
	if d := path.Dir(name); d != "." {
		if err := os.MkdirAll(filepath.Join(dir, filepath.FromSlash(d)), 0o700); err != nil {
			return err
		}
	}
	return os.WriteFile(p, data, 0o600)
}
