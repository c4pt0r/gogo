// Files on /flash: whole-file helpers, an open File, directories.
// Run on the host with: gogo run --fs-root /tmp/somedir .
package main

import (
	"gogo/bytes"
	"gogo/fs"
)

func check(what string, e fs.Err) {
	if e != fs.OK {
		println(what, "failed, error", int(e))
	}
}

func main() {
	// WriteFile replaces the whole file (atomically on /flash).
	check("write", fs.WriteFile("/flash/hello.txt", "hello, flash\n"))
	b, e := fs.ReadFile("/flash/hello.txt", 1024)
	check("read", e)
	print1("hello.txt:", b)

	// An open File: write in pieces, then read back in chunks.
	check("mkdir", fs.Mkdir("/flash/data"))
	f, e := fs.Open("/flash/data/log.txt", fs.Write)
	check("open", e)
	for i := 0; i < 3; i++ {
		check("write", f.Write("line "+string([]byte{byte('1' + i)})+"\n"))
	}
	n, _ := f.Size()
	println("written", n, "bytes")
	f.Close()

	f, e = fs.Open("/flash/data/log.txt", fs.Append)
	check("append", e)
	f.Write(bytes.FromString("line 4\n"))
	f.Close()

	f, _ = fs.Open("/flash/data/log.txt", fs.Read)
	for {
		chunk, e := f.Read(10) // at most 10 bytes per call
		if e != fs.OK || chunk.Len() == 0 {
			break // an empty chunk with OK is the end of the file
		}
		println("chunk of", chunk.Len(), "bytes")
	}
	f.Close()
	all, _ := fs.ReadFile("/flash/data/log.txt", 1024)
	print1("log.txt:", all)

	info, _ := fs.Stat("/flash/data/log.txt")
	println("size", info.Size, "dir?", info.IsDir)
	names, _ := fs.ReadDir("/flash")
	println("entries in /flash:", len(names)) // order is not defined
	check("rename", fs.Rename("/flash/hello.txt", "/flash/data/hello.txt"))
	check("remove", fs.Remove("/flash/data/hello.txt"))
	_, e = fs.Stat("/flash/data/hello.txt")
	println("after remove, NotFound:", e == fs.NotFound)
}

// print1 prints a Bytes as text without its trailing newline.
func print1(label string, b bytes.Bytes) {
	s := b.String()
	if len(s) > 0 && s[len(s)-1] == '\n' {
		s = s[:len(s)-1]
	}
	println(label, "["+s+"]")
}
