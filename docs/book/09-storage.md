# 9. Storage

gogo gives programs a small file system API, `gogo/fs`, over two media:

- **`/flash`**: part of the board's internal flash (about 5 MB, LittleFS).
  Every board has it. It survives reboots and reflashing of firmware or
  program.
- **`/sd`**: the SD card, on boards with a slot (the Cardputer ADV). FAT12,
  FAT16, FAT32 and exFAT cards work. The card is mounted on first use; a
  card inserted later works from the next call on.

On top of `gogo/fs` there are two small libraries: `gogo/kv`, a key-value
store, and `gogo/ringlog`, a size-bounded log.

On your computer, storage is a directory: `gogo run --fs-root DIR` maps
`/flash` to `DIR/flash` (created if needed) and `/sd` to `DIR/sd` (only if
that directory exists). Without `--fs-root` there is no storage at all and
every call fails with `NotMounted`. Programs stay inside those
directories: a symlink in them is not followed (it is listed as a file,
and opening it is `BadPath`). The examples in this chapter are run that
way.

## 9.1 Paths

Paths are absolute and start with `/flash` or `/sd`. The rules are strict,
so that a path means the same thing on both media and on your computer:

- a name is 1 to 32 bytes of `A-Z a-z 0-9 . _ -`;
- a whole path is at most 96 bytes;
- no empty segments (`/flash//a`), no trailing `/`, and no name ending in
  `.` (so no `.` or `..`).

Anything else is `fs.BadPath`. Prefer lower-case names: `/sd` is FAT and
ignores case (`A.TXT` is `a.txt`), while `/flash` does not. A name made on a
PC that breaks the rules is still listed by `ReadDir`, but cannot be opened.

### Where the media are

| Path | On a board | `gogo run` / `gogovm` on a computer | A program that embeds gogo (chapter 18) |
|---|---|---|---|
| `/flash` | the board's flash (LittleFS) | `DIR/flash` with `--fs-root DIR` (created if missing); without it, every call is `NotMounted` | the host's `fs_root` directory; without it, `NotMounted` |
| `/sd` | the SD card (FAT), when one is inserted (Cardputer ADV) | `DIR/sd`, only if that directory exists | never mounted |
| anything else (`/`, `/tmp`, `/data`, ...) | `BadPath` | `BadPath` | `BadPath` |

On a computer a program stays inside those directories: symlinks, FIFOs
and devices in them are not followed (`BadPath`), a file with other hard
links cannot be opened for writing, and on a file system that ignores case
(macOS by default) `/flash` ignores case too.

### Reserved names

The file system itself reserves no names beyond the rules above, but these
libraries and tools use names of their own. Keep your files out of their
way: put a program's data in a directory of its own (`/flash/myapp/...`).

| Name | Used by | What happens |
|---|---|---|
| `<name>.tmp` | `fs.WriteFile` | the new content is written to `<name>.tmp` beside the file, then renamed over it (section 9.3). Do not keep data in names ending in `.tmp`: a `WriteFile` of the same base name replaces it, and on `/sd` a power cut can leave one behind |
| `/flash/kv/` | `gogo/kv` (section 9.5) | one file per key. Keys may not end in `.tmp`; any other file put there shows up in `kv.Keys()` |
| `0.log`, `1.log` | `gogo/ringlog` (section 9.6) | the two halves of a log, in the directory you pass to `ringlog.Append` and `ringlog.Read` |
| `.gogo-push` | `gogo push` (chapter 12) | the upload is written to this name in the target directory, checked, then renamed; an interrupted push can leave it |
| `/flash/<name>.ggb` | `gogo push` | where an image goes by default; `code.Load` loads images from any path (chapter 14) |

## 9.2 Errors

Every call returns an `fs.Err`; `fs.OK` (0) means success. Storage errors
never crash the caller. `fs.Err` is another name for `errors.Code`, the
shared codes of `gogo/errors` (appendix C): `fs.NotFound` is
`errors.NotFound`, the code `gogo/code` and the other packages use for it,
and an `fs.Err` is an `error` too (`e.Error()` is `"not found"`).

| Value | Name | Meaning |
|---|---|---|
| 0 | `OK` | success |
| 1 | `NotFound` | no such file or directory |
| 2 | `Exists` | the target of `Mkdir` or `Rename` exists |
| 3 | `NoMemory` | the result does not fit the caller's memory limit (it is not killed) |
| 4 | `TooMany` | 4 files are open already |
| 5 | `Busy` | the path is in use (see 9.4) |
| 7 | `Closed` | the file is closed, or not owned by the caller |
| 8 | `IO` | a medium error (card removed, ...) |
| 11 | `NoSpace` | the medium is full |
| 12 | `BadPath` | the path breaks the rules |
| 13 | `IsDir` | a file operation on a directory |
| 14 | `NotDir` | a segment before the last is not a directory (`/sd` says `NotFound`) |
| 15 | `NotEmpty` | `Remove` of a directory with entries |
| 16 | `NotMounted` | unknown prefix, or `/sd` has no working card |

```go
// Storage errors are values, never crashes: every call returns an fs.Err.
// And a File is a port: it closes when its owner exits, even by a crash.
package main

import (
	"gogo/actor"
	"gogo/fs"
)

func main() {
	_, e := fs.ReadFile("/flash/missing.txt", 100)
	println("missing file:", int(e), e == fs.NotFound)
	println("bad name:", int(fs.WriteFile("/flash/bad name.txt", "x")))
	println("relative path:", int(fs.WriteFile("notes.txt", "x")))
	println("no such mount:", int(fs.WriteFile("/usb/a.txt", "x")))
	println("no card (/sd):", int(fs.WriteFile("/sd/a.txt", "x")))
	fs.Mkdir("/flash/d")
	fs.WriteFile("/flash/d/a", "x")
	println("remove non-empty dir:", int(fs.Remove("/flash/d")))

	// Too big for the caller's quota: NoMemory, and nothing is read.
	_, e = fs.ReadFile("/flash/d/a", 0)
	println("max 0:", int(e))

	// A writer that crashes still closes (and commits) its file.
	me := actor.Self()
	w := actor.Spawn(func() {
		f, _ := fs.Open("/flash/crash.txt", fs.Write)
		f.Write("written before the crash\n")
		actor.Send(me, "wrote")
		actor.Receive()
		zero := 0
		println(1 / zero)
	})
	println(actor.Receive())
	// While the writer holds the file open, removing it is Busy.
	println("remove while open:", int(fs.Remove("/flash/crash.txt")))
	ref := actor.Monitor(w)
	actor.Send(w, "crash now")
	switch m := actor.Receive().(type) {
	case actor.DownMsg:
		println("writer down:", m.Ref == ref)
	}
	b, e := fs.ReadFile("/flash/crash.txt", 100)
	println("after the crash:", int(e), b.Len(), "bytes")
	println("remove now:", int(fs.Remove("/flash/crash.txt")))
}
```

```text
missing file: 1 true
bad name: 12
relative path: 12
no such mount: 16
no card (/sd): 16
remove non-empty dir: 15
max 0: 3
wrote
remove while open: 5
writer down: true
after the crash: 0 25 bytes
remove now: 0
```

Standard error:

```text
gogo: actor <pid 1> crashed: divzero in main.main.func1
```

## 9.3 Reading and writing files

The simplest calls work on whole files:

```go
func ReadFile(path string, max int) (bytes.Bytes, Err) // NoMemory if larger than max
func WriteFile(path string, data any) Err              // string, bytes.Bytes or []byte
```

`WriteFile` replaces the file: it writes `<name>.tmp` first and then
renames it over the old file, so on `/flash` a power loss leaves either the
old or the new content. (On `/sd` the replacement is not atomic: a power
loss at the wrong moment leaves `<name>.tmp`.)

For more control, open a file:

```go
func Open(path string, mode int) (File, Err)   // fs.Read, fs.Write (create or truncate), fs.Append

func (f File) Read(max int) (bytes.Bytes, Err) // at most 4096 bytes; empty with OK = end of file
func (f File) Write(data any) Err              // string, bytes.Bytes or []byte
func (f File) WriteAt(off int, data any) Err   // overwrite inside a Write-mode file, never extends it
func (f File) Sync() Err                       // commit without closing
func (f File) Size() (int, Err)
func (f File) Close()
```

and for directories and the rest:

```go
func Stat(path string) (Info, Err)       // Info{Size int; IsDir bool}
func ReadDir(path string) ([]string, Err) // directories end in "/", order not defined
func Mkdir(path string) Err               // one level, not recursive
func Remove(path string) Err              // a file or an empty directory
func Rename(from, to string) Err          // within one medium
func Usage(prefix string) (Space, Err)    // Space{Used, Total int}, "/flash" or "/sd"
```

```go
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
```

```text
hello.txt: [hello, flash]
written 21 bytes
chunk of 10 bytes
chunk of 10 bytes
chunk of 8 bytes
log.txt: [line 1
line 2
line 3
line 4]
size 28 dir? false
entries in /flash: 2
after remove, NotFound: true
```

### A File is a port

`fs.File` is the PID of a file port, so files follow the port rules of
chapter 7: the actor that opened the file owns it, and the file is closed
(and what was written is committed) when the owner exits, even by a crash,
as `09-fs-errors` above showed. Only the owner can read, write, sync or
size it; anyone else gets `Closed`. `port.Handoff(actor.PID(f), other)`
gives a file to another actor.

`Close` has no result, so it cannot report a failed commit. If you need to
know that the data reached the medium, call `Sync` before `Close`.

A file opened for writing cannot be read and vice versa (`Closed`).

`WriteAt(off, data)` overwrites bytes that are already in a file opened with
`fs.Write`, for fixing up what was written (a length field in a header, as
`gogo/wav` does). It never extends the file: `off + len(data)` must be at
most `Size`, else the result is `BadPath` and nothing is written. A file
opened `Read` or `Append` gives `Closed`. It does not move the position
of the next `Write`, which still appends at the end. On `/sd` it needs no
free space, so it works on a full card; on `/flash` it needs a free block.

> **Note.** `fs.File` cannot be a case in a type switch (`type switch case
> fs.File not supported; wrap it in a struct`), because at run time it is
> indistinguishable from an `actor.PID`. To send a file to another actor,
> put it in a struct field.

## 9.4 Limits and rules

- **At most 4 files are open at once in the whole VM** (`TooMany`).
  `ReadFile`, `WriteFile` and the `kv` calls use one of those 4 slots while
  they run.
- **One writer per path.** A second `Write` or `Append` open of the same
  path is `Busy`. While any file is open on a path, `Remove`, `Rename` and
  the final step of `WriteFile` on it are `Busy` too. Readers can open a
  file that is being written, but what they see of unclosed data is not
  guaranteed.
- Renaming a directory while any file is open on the same medium is
  `Busy`. Renaming into its own subtree (`/flash/d` to `/flash/d/x`) and
  across media are `BadPath`. Renaming onto an existing name is `Exists`.
- Operations on `/`, and `Mkdir`, `Remove` or `Rename` of `/flash` or `/sd`
  themselves, are `BadPath`.
- Sizes are reported up to 2 GB - 1; larger files or cards read as that.
- **Every storage call is synchronous and stops all actors** while it
  runs. Erasing flash takes tens of milliseconds, and an SD card
  occasionally hundreds. Write in large pieces, and keep storage out of
  actors that must react quickly.

Copying between media takes a read and a write, since `Rename` cannot
cross them:

```go
// /sd works like /flash, on boards with a card slot (the Cardputer ADV).
// Copy a file from /flash to /sd: Rename cannot cross mounts, so read and
// write. On the host, --fs-root DIR gives /sd only if DIR/sd exists.
package main

import "gogo/fs"

func copyFile(from, to string) fs.Err {
	b, e := fs.ReadFile(from, 4096)
	if e != fs.OK {
		return e
	}
	return fs.WriteFile(to, b)
}

func main() {
	fs.WriteFile("/flash/settings.txt", "volume=3\n")
	println("rename across mounts:", int(fs.Rename("/flash/settings.txt", "/sd/settings.txt")) == int(fs.BadPath))
	e := copyFile("/flash/settings.txt", "/sd/settings.txt")
	if e == fs.NotMounted {
		println("no SD card")
		return
	}
	b, _ := fs.ReadFile("/sd/settings.txt", 100)
	println("copied:", b.Len(), "bytes")
	sp, e := fs.Usage("/sd")
	println("usage ok:", e == fs.OK, sp.Total > 0, sp.Used <= sp.Total)
}
```

With `--fs-root` on a directory that has an `sd` subdirectory:

```text
rename across mounts: true
copied: 9 bytes
usage ok: true true true
```

On a board without a card the program prints `no SD card`.

## 9.5 gogo/kv: a key-value store

`gogo/kv` stores small values under names, one file per key in
`/flash/kv/`:

```go
func Get(key string) (bytes.Bytes, fs.Err) // fs.NotFound if there is none
func Put(key string, v any) fs.Err         // a string or a bytes.Bytes, at most 4096 bytes
func Delete(key string) fs.Err
func Keys() ([]string, fs.Err)
```

A key follows the file name rules (and must not end in `.tmp`); a bad
key is `fs.BadPath`. `Put` replaces the value atomically. A value over
4096 bytes is the shared code `errors.Overflow` and a value of another
type `errors.BadArg` (compare with `errors.Overflow`). A `[]byte` must
go through
`bytes.FromSlice` first.

The classic use is a boot counter:

```go
// A boot counter in gogo/kv: read, increment, store. Each run of the
// program (each boot, on a board) adds one. The example counts three
// "boots" in one run by calling boot three times.
package main

import (
	"gogo/fs"
	"gogo/kv"
	"gogo/strconv"
)

func boot() int {
	n := 0
	v, e := kv.Get("boots")
	if e == fs.OK {
		n, _ = strconv.Atoi(v.String())
	} else if e != fs.NotFound {
		println("cannot read the counter, error", int(e))
		return -1
	}
	n++
	if e := kv.Put("boots", strconv.Itoa(n)); e != fs.OK {
		println("cannot store the counter, error", int(e))
	}
	return n
}

func main() {
	for i := 0; i < 3; i++ {
		println("boot number", boot())
	}
	kv.Put("owner", "dev board 7")
	keys, _ := kv.Keys()
	println("keys stored:", len(keys))
	v, _ := kv.Get("owner")
	println("owner:", v.String())
	println("delete:", int(kv.Delete("owner")), "again:", int(kv.Delete("owner")))
	println("bad key:", kv.Put("a/b", "x") == fs.BadPath)
}
```

```text
boot number 1
boot number 2
boot number 3
keys stored: 2
owner: dev board 7
delete: 0 again: 1
bad key: true
```

Only `fs.NotFound` means "no such key". Any other error from `Get` is a real
failure (the medium, or `NoMemory` when the value does not fit your
actor's limit), and should not be treated as "use the default and
overwrite": that would destroy a value you merely failed to read.

## 9.6 gogo/ringlog: a bounded log

Logs on a device must not fill the flash. `gogo/ringlog` keeps a log in two
files, `dir/0.log` (newest) and `dir/1.log`, and keeps the total within a
size you choose:

```go
func Append(dir string, maxBytes int, line any) fs.Err
func Read(dir string, max int) (bytes.Bytes, fs.Err) // the newest max bytes, oldest first
```

When `0.log` passes `maxBytes/2`, `1.log` is deleted and `0.log` becomes
`1.log`. So the log holds between half of `maxBytes` and `maxBytes` (plus
one line) of the most recent lines. `Append` writes the line exactly as
given, so include the newline yourself. Each `Append` opens, writes, syncs
and closes, so a power loss costs at most the line being written.

```go
// gogo/ringlog keeps a log on flash that never grows past a size bound:
// two files, the older one dropped when the newer one is half full.
package main

import (
	"gogo/fs"
	"gogo/ringlog"
	"gogo/strconv"
)

func main() {
	for i := 1; i <= 30; i++ {
		e := ringlog.Append("/flash/events", 100, "event "+strconv.Itoa(i)+"\n")
		if e != fs.OK {
			println("append", i, "error", int(e))
		}
	}
	// Read returns the newest bytes, oldest first.
	b, _ := ringlog.Read("/flash/events", 1000)
	println("kept", b.Len(), "bytes:")
	println(b.String())
	tail, _ := ringlog.Read("/flash/events", 9)
	println("last 9 bytes: [" + tail.String() + "]")
}
```

```text
kept 99 bytes:
event 20
event 21
event 22
event 23
event 24
event 25
event 26
event 27
event 28
event 29
event 30

last 9 bytes: [event 30
]
```

One actor should write each log directory: while a file holds `0.log`
open for writing, `Append` returns `Busy` and the line is not written.

## 9.7 /flash size and formatting

By default `/flash` uses the whole storage partition (5056 KB). The first
boot formats it. Two settings in the configuration store (chapter 11)
change that:

```sh
gogo config set fs.flash.size 1M        # bytes, K or M suffix; at least 64K
gogo config rm fs.flash.size            # back to the whole partition
```

A larger size grows the file system in place at the next boot, keeping
files. A smaller one needs a format, which erases everything on `/flash`:

```sh
gogo device format-flash                # asks first; restarts the board and formats /flash
```

It formats the `/flash` partition only: the configuration store, the
program and the firmware stay. (It was `gogo config set fs.flash.format
yes` before, which still works in this release.) Reflashing firmware or a
program does not touch `/flash`; only `idf.py erase-flash` wipes it.

To put a file on a board's `/flash` from your computer, `gogo push`
copies an image there (chapter 14); other files go through a program
(over HTTP, chapter 10) or the SD card.

`examples/fsdemo` (a boot counter, button presses logged to a ringlog,
`Usage` on the screen) and `examples/sddemo` in the gogo tree exercise
storage on real boards.
