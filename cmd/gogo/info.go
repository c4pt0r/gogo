package main

// gogo info: what an image is and needs, read from its bytes
// (compiler.ReadImageInfo). Given a directory or a source file instead,
// it builds it in memory first and also lists the drivers its packages
// mark //gogo:optional, which the image does not record.

import (
	"fmt"
	"io"
	"os"
	"strings"

	"gogotools/compiler"
)

const infoUsage = "usage: gogo info [-path dirs] <file" + imageExt + "|dir|file" + sourceExt + ">"

func infoCmd(args []string, out io.Writer) error {
	fs := newFlags("info")
	getPath := addPathFlag(fs)
	pos, err := parseFlags(fs, args, infoUsage)
	if err != nil {
		return err
	}
	if len(pos) != 1 {
		return usageError("info", "want one image, directory or "+sourceExt+" file", infoUsage)
	}
	target := pos[0]
	fi, err := os.Stat(target)
	if err != nil {
		return err
	}
	var img []byte
	var optional []string
	if fi.IsDir() || isSourceFile(target) {
		prog, err := loadSource(target, getPath().path())
		if err != nil {
			return err
		}
		if img, err = compiler.Compile(prog); err != nil {
			return err
		}
		optional = compiler.OptionalDrivers(prog)
		if optional == nil {
			optional = []string{}
		}
	} else if img, err = os.ReadFile(target); err != nil {
		return err
	}
	info, err := compiler.ReadImageInfo(img)
	if err != nil {
		return fmt.Errorf("gogo info: %s: %v", target, err)
	}
	writeInfo(out, info, optional)
	return nil
}

// writeInfo prints info; optional is nil when it is unknown (an image).
func writeInfo(out io.Writer, info compiler.ImageInfo, optional []string) {
	list := func(l []string) string {
		if len(l) == 0 {
			return "none"
		}
		return strings.Join(l, ", ")
	}
	fmt.Fprintf(out, "format     %s\n", compiler.VersionString(info.Version))
	fmt.Fprintf(out, "size       %d bytes: %d functions, %d types, %d code words, %d constant words\n",
		info.Size, info.Funcs, info.Types, info.CodeWords, info.ConstWords)
	fmt.Fprintf(out, "requires   %s\n", list(info.Requires))
	if optional != nil {
		fmt.Fprintf(out, "optional   %s\n", list(optional))
	}
	entries := make([]string, len(info.Exports))
	for i, e := range info.Exports {
		entries[i] = e
		if e == info.Entry {
			entries[i] += " (boot)"
		}
	}
	fmt.Fprintf(out, "entries    %s\n", list(entries))
	if info.Size > partSize-partHdrSize {
		fmt.Fprintf(out, "note       over the %d bytes the board's program partition holds; fine as a module on /flash\n", partSize-partHdrSize)
	}
}
