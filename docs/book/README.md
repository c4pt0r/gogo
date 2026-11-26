# Programming GogoLang

A book about gogo: a tiny actor VM for microcontrollers, programmed in a
subset of Go, with Erlang-style actors, supervisors and ports.

It is written for programmers who know some Go or another C-like language,
and assumes no knowledge of embedded programming or Erlang.

## Contents

1. [Preface](01-preface.md): what gogo is, what it is good for, and what it is not
2. [Getting Started](02-getting-started.md): `gogo setup`, build the tools, run a program, flash a board
3. [Language Basics](03-language-basics.md): the Go subset, what it leaves out, and what to write instead
4. [Actors](04-actors.md): Spawn, Send, Receive, type switches, Call/Reply, names, timers
5. [Failure and Recovery](05-failure-and-recovery.md): exit reasons, links, monitors, TrapExit, supervisors, "let it crash"
6. [Memory Model](06-memory-model.md): per-actor heaps and limits, copied messages, mailbox quotas, Bytes and Buffer
7. [Ports and Peripherals](07-ports-and-peripherals.md): ports, GPIO, events, debouncing, owners, Handoff
8. [Keyboard and Power](08-keyboard-and-power.md): the `kbd` and `power` ports
9. [Storage](09-storage.md): `/flash` and `/sd`, `gogo/fs`, `gogo/kv`, `gogo/ringlog`
10. [Networking](10-networking.md): DHCP, DNS, UDP, TCP, the HTTP client and server, the wall clock (`gogo/sys`, `gogo/sntp`)
11. [Configuration and Devices](11-configuration-and-devices.md): `gogo config`, `config.Get`, Wi-Fi, the two boards
12. [Toolchain Reference](12-toolchain-reference.md): `gogo build`, `run`, `dis`, `flash`, `monitor`, `firmware`, `config`, `setup`
13. [Appendices](13-appendices.md): library quick reference, exit reasons, error codes, limits, differences from Go, requirement names
14. [Hot Loading](14-hot-loading.md): loading code at run time, modules, supervised upgrades
15. [IMU and IR](15-imu-and-ir.md): the motion sensor, infrared send and receive, streams (`Chunk`, `Out`, `Done`)
16. [Audio](16-audio.md): the microphone and speaker, recording to WAV files, playing them back
17. [HTTPS](17-https.md): https URLs, the clock, the keep-alive `http.Client`, streaming uploads, the `"tls"` port
18. [Embedding gogo](18-embedding.md): running gogo images inside a C program: `libgogo.a`, the host table and the latched wake, stepping, posts, host ports, a sandboxed `/flash`
19. [Text UI](19-text-ui.md): the `"screen"` port, `gogo/tui`: frames, windows, colours, menus, progress bars, status lines, a line editor, keys on both boards

[Known issues](../known-issues.md) lists gogo's open limitations, with
workarounds.

## Examples

Every program in the book is a complete, runnable example in
[`examples/`](examples/), one directory per program, named after its
chapter (`04-call`, `09-kv-boot`, ...). The output printed in the book is
the real output of the host VM, kept next to each program in
`expected.out` and `expected.err`. To run them all and check that the
book is still right:

```sh
make vm
go build -o build/gogo ./cmd/gogo
docs/book/examples/run.sh
```

Chapter 18's C programs are in [`examples/embed-c/`](../../examples/embed-c/);
`make example-embed` builds them, runs them and checks that every C
snippet in the chapter is cut from them.
