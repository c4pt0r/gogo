package main

// gogo push: copies an image to the board's /flash, where code.Load
// finds it (book chapter 14), over the serial console (the push, data,
// end and abort frames of platform/esp32s3/main/config_esp.c):
//
//	gogo push [-port P] [-timeout seconds] <file.ggb> [/flash/path]
//
// The board writes the bytes to a temporary file beside the target, in
// chunks of pushChunk bytes, each answered before the next goes out; then
// it reads the file back, checks its length and CRC-32 against the ones
// announced, and only then renames it over the target. A failure leaves
// the target as it was. A push cut off (the cable pulled) leaves the
// temporary file, pushTmp, until the next push into that directory.

import (
	"encoding/base64"
	"fmt"
	"hash/crc32"
	"io"
	"os"
	"path"
	"path/filepath"
	"strconv"
	"strings"
	"time"

	"gogotools/compiler"
)

const pushUsage = "usage: gogo push [-port P] [-timeout seconds] <file" + imageExt + "> [/flash/path]"

// pushChunk is the bytes per data frame (CFG_PUSH_CHUNK in config_esp.c):
// a frame stays under the board's 256-byte console input buffer.
const pushChunk = 128

// pushTmp is the temporary file a push is written to, in the target's
// directory (PUSH_TMP in fs_lfs.c); it cannot be a target.
const pushTmp = ".gogo-push"

// pushEnv holds the side effects of gogo push, replaced in tests.
type pushEnv struct {
	out  io.Writer
	glob func(string) ([]string, error)
	open func(string) (io.ReadWriteCloser, error)
}

func pushCmd(args []string) error {
	return pushMain(args, pushEnv{out: os.Stdout, glob: filepath.Glob,
		open: func(p string) (io.ReadWriteCloser, error) { return openSerial(p) }})
}

// pushTarget checks the /flash path a file is pushed to (by default
// /flash/<its name>): a path the board's file system takes.
func pushTarget(file, dest string) (string, error) {
	if dest == "" {
		dest = "/flash/" + filepath.Base(file)
	}
	if !strings.HasPrefix(dest, "/flash/") || path.Clean(dest) != dest || len(dest) > 96 {
		return "", fmt.Errorf("gogo push: %s is not a path under /flash/", dest)
	}
	if path.Base(dest) == pushTmp {
		return "", fmt.Errorf("gogo push: %s is the name the board writes a push to first; pick another", pushTmp)
	}
	for _, name := range strings.Split(dest[1:], "/") {
		if name == "" || len(name) > 32 || strings.HasSuffix(name, ".") || strings.Trim(name, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-") != "" {
			return "", fmt.Errorf("gogo push: bad name %q in %s (1 to 32 of A-Z a-z 0-9 . _ -, not ending in .)", name, dest)
		}
	}
	return dest, nil
}

func pushMain(args []string, env pushEnv) error {
	fs := newFlags("push")
	port := fs.String("port", "", "")
	timeout := 5 * time.Second
	fs.Var(secondsFlag{&timeout}, "timeout", "")
	pos, err := parseFlags(fs, args, pushUsage)
	if err != nil {
		return err
	}
	if len(pos) < 1 || len(pos) > 2 {
		return usageError("push", "want an image and, optionally, where it goes on /flash", pushUsage)
	}
	file, dest := pos[0], ""
	if len(pos) == 2 {
		dest = pos[1]
	}
	if dest, err = pushTarget(file, dest); err != nil {
		return err
	}
	img, err := os.ReadFile(file)
	if err != nil {
		return err
	}
	if _, err := compiler.ReadImageInfo(img); err != nil {
		return fmt.Errorf("gogo push: %s: %v", file, err)
	}
	if *port == "" {
		if *port, err = detectPort(env.glob); err != nil {
			return err
		}
	}
	f, err := env.open(*port)
	if err != nil {
		return err
	}
	defer f.Close()
	c := newBoardConn(f)
	defer c.close()
	start := time.Now()
	if err := pushBytes(c, img, dest, timeout); err != nil {
		// Best effort: drop the temporary file now rather than at the next push.
		c.request("abort", nil, timeout)
		return fmt.Errorf("gogo push: %v", err)
	}
	fmt.Fprintf(env.out, "pushed %s to %s (%d bytes, %.1f s)\n", file, dest, len(img), time.Since(start).Seconds())
	return nil
}

// pushBytes sends data to dest through the board's push frames.
func pushBytes(c *boardConn, data []byte, dest string, timeout time.Duration) error {
	sum := fmt.Sprintf("%08x", crc32.ChecksumIEEE(data))
	if _, _, err := c.request("push", []string{dest, strconv.Itoa(len(data)), sum}, timeout); err != nil {
		if be, ok := err.(errBoard); ok && (be.reason == "bad key" || strings.HasPrefix(be.reason, "usage: set")) {
			return fmt.Errorf("this firmware has no push (update it)")
		}
		return err
	}
	for off := 0; off < len(data); off += pushChunk {
		chunk := data[off:min(off+pushChunk, len(data))]
		_, text, err := c.request("data", []string{strconv.Itoa(off), base64.StdEncoding.EncodeToString(chunk)}, timeout)
		if err != nil {
			return fmt.Errorf("at byte %d: %v", off, err)
		}
		if text != strconv.Itoa(off+len(chunk)) {
			return fmt.Errorf("at byte %d: the board has %s bytes", off, text)
		}
	}
	_, _, err := c.request("end", nil, timeout)
	return err
}
