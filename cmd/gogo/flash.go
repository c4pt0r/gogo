package main

// gogo flash: writes an image to the ESP32-S3 image partition; given a
// directory or a source file, it builds it first (to a temporary image).
//
// The partition (platform/esp32s3/partitions.csv: `gogo`, data subtype 0x40
// at 0x210000, 1 MB) holds a 16-byte little-endian header
// {magic "GGIM" = 0x4D494747, image_bytes, crc32_ieee(image), 0} followed by
// the image (the compiler's current format version); the firmware (platform/esp32s3/main/image_part.c)
// checks all three before using the image, so a half-written partition is
// refused. The write itself is delegated to an external esptool.

import (
	"encoding/binary"
	"errors"
	"fmt"
	"hash/crc32"
	"os"
	"os/exec"
	"path/filepath"
	"strings"

	"gogotools/compiler"
)

const (
	imageMagic  = 0x4F474F47 // word 0 of every gogo image
	partMagic   = 0x4D494747 // "GGIM": image partition header
	partOffset  = "0x210000" // `gogo` partition offset (partitions.csv)
	partSize    = 0x100000   // `gogo` partition size
	partHdrSize = 16
	portGlob    = "/dev/cu.usbmodem*"
	flashUsage  = "usage: gogo flash [-port P] [-esptool path] [-path dirs] <app" + imageExt + "|dir|file" + sourceExt + ">"
)

// checkImage reports why img cannot go into the image partition, or nil.
func checkImage(img []byte) error {
	switch {
	case len(img) < 4 || binary.LittleEndian.Uint32(img) != imageMagic:
		return errors.New("not a gogo image (bad magic)")
	case len(img)%4 != 0:
		return errors.New("not a gogo image (size is not a multiple of 4)")
	case len(img) < 8:
		return errors.New("not a gogo image (truncated header)")
	case compiler.CheckVersion(binary.LittleEndian.Uint32(img[4:])) != nil:
		return compiler.CheckVersion(binary.LittleEndian.Uint32(img[4:]))
	case len(img) > partSize-partHdrSize:
		return fmt.Errorf("image is %d bytes, the partition holds at most %d", len(img), partSize-partHdrSize)
	}
	return nil
}

// packImage returns the partition header followed by img, or nil if img
// is not a gogo image that fits the partition (see checkImage).
func packImage(img []byte) []byte {
	if checkImage(img) != nil {
		return nil
	}
	out := make([]byte, partHdrSize+len(img))
	le := binary.LittleEndian
	le.PutUint32(out[0:], partMagic)
	le.PutUint32(out[4:], uint32(len(img)))
	le.PutUint32(out[8:], crc32.ChecksumIEEE(img))
	le.PutUint32(out[12:], 0)
	copy(out[partHdrSize:], img)
	return out
}

// detectPort returns the single board serial port matching portGlob.
func detectPort(glob func(string) ([]string, error)) (string, error) {
	ports, err := glob(portGlob)
	if err != nil {
		return "", err
	}
	switch len(ports) {
	case 1:
		return ports[0], nil
	case 0:
		return "", fmt.Errorf("gogo: no board found (%s); connect it or pass -port", portGlob)
	}
	return "", fmt.Errorf("gogo: several boards found (%s); pick one with -port", strings.Join(ports, ", "))
}

// flasher holds the side effects of `gogo flash` so tests can replace them.
type flasher struct {
	glob func(string) ([]string, error)
	run  func(name string, args []string) error
}

func defaultFlasher() flasher {
	return flasher{
		glob: filepath.Glob,
		run: func(name string, args []string) error {
			cmd := exec.Command(name, args...)
			cmd.Stdin, cmd.Stdout, cmd.Stderr = os.Stdin, os.Stdout, os.Stderr
			return cmd.Run()
		},
	}
}

// findEsptool returns esptool (or esptool.py) from PATH.
func findEsptool() (string, error) {
	for _, name := range []string{"esptool", "esptool.py"} {
		if p, err := exec.LookPath(name); err == nil {
			return p, nil
		}
	}
	return "", errors.New("gogo flash: esptool not found in PATH; pass -esptool (e.g. after `. ~/esp/esp-idf/export.sh`)")
}

// flash implements `gogo flash`.
func (f flasher) flash(args []string) error {
	fs := newFlags("flash")
	port := fs.String("port", "", "")
	esptool := fs.String("esptool", "", "")
	getPath := addPathFlag(fs)
	pos, err := parseFlags(fs, args, flashUsage)
	if err != nil {
		return err
	}
	if len(pos) != 1 {
		return usageError("flash", "want one image, directory or "+sourceExt+" file", flashUsage)
	}
	file := pos[0]
	// A directory or a source file is built first, as gogo run does.
	if fi, err := os.Stat(file); err == nil && (fi.IsDir() || isSourceFile(file)) {
		tmp, err := os.MkdirTemp("", "gogo-flash-")
		if err != nil {
			return err
		}
		defer os.RemoveAll(tmp)
		if file, err = buildImage(file, filepath.Join(tmp, "app"+imageExt), getPath().path()); err != nil {
			return err
		}
	} else if getPath().set {
		return usageError("flash", "-path is for building a directory or "+sourceExt+" file, not for an image", flashUsage)
	}
	img, err := os.ReadFile(file)
	if err != nil {
		return err
	}
	if err := checkImage(img); err != nil {
		return fmt.Errorf("gogo flash: %s: %v", file, err)
	}
	if *port == "" {
		if *port, err = detectPort(f.glob); err != nil {
			return err
		}
	}
	if *esptool == "" {
		if *esptool, err = findEsptool(); err != nil {
			return err
		}
	}
	if err := writeImage(f.run, *esptool, *port, img, nil); err != nil {
		return fmt.Errorf("gogo flash: %v", err)
	}
	return nil
}

// writeImage writes img (a gogo image that passed checkImage) with its
// partition header to the image partition. opts are esptool options put
// before the command (gogo setup passes --after watchdog-reset).
func writeImage(run func(string, []string) error, esptool, port string, img []byte, opts []string) error {
	tmp, err := os.CreateTemp("", "gogo-flash-*.bin")
	if err != nil {
		return err
	}
	defer os.Remove(tmp.Name())
	_, err = tmp.Write(packImage(img))
	if cerr := tmp.Close(); err == nil {
		err = cerr
	}
	if err != nil {
		return err
	}
	args := append(append([]string{"--port", port}, opts...), imageWriteArgs(tmp.Name())...)
	if err := run(esptool, args); err != nil {
		return fmt.Errorf("%s: %v", esptool, err)
	}
	return nil
}

// imageWriteArgs is the esptool command that writes a packed image file.
func imageWriteArgs(file string) []string {
	return []string{"write-flash", partOffset, file}
}
