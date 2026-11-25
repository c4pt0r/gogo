# 18. Embedding gogo

So far a gogo program has run on a board or under `gogovm`. **Embed mode**
puts the same VM inside a program of your own, the way Lua is embedded: a
desktop or server program, a phone app, a test tool. The program (the
**host**) loads an image that `gogo build` made, runs it, sends its actors
messages, and gives it ports of its own. Any number of VMs can live in one
process, on any threads; they share nothing.

The API is one C header, [`runtime/include/gogo.h`](../../runtime/include/gogo.h),
and one library, `libgogo.a`. This chapter walks through it with the
programs in [`examples/embed-c/`](../../examples/embed-c/). Every C snippet
here is cut from them, and `make example-embed` builds them and checks
their output.

What an embedded VM does not have in this version: the board peripherals
(GPIO, keyboard, power, IMU, IR, audio), the
network and TLS. It has actors, timers, `Bytes`, host ports, posts,
`/flash` on a host directory, and hot loading if the host allows it.

## 18.1 Building

```sh
make lib                  # build/libgogo.a
make example-embed        # builds and tests examples/embed-c
```

`libgogo.a` is the VM core, the file system layer, streams and the
embedding layer, compiled as strict C99. Link it with `-pthread`:

```sh
cc -std=c99 -Iruntime/include -o host host.c build/libgogo.a -pthread
```

Only the `gogo_*` functions are public. The library also defines internal
symbols that a static archive cannot hide (`vm_`, `va_`, `port_`, `actor_`
and a few more prefixes); [`runtime/include/README.md`](../../runtime/include/README.md)
lists them, and your program must not define symbols with those prefixes.

## 18.2 The smallest host

`examples/embed-c/main.c` runs one image. It reads the file, makes a VM on
the POSIX host and loads the image:

```c
    gogo_vm *vm = h ? gogo_new(h, ctx, cfg) : gogo_new_posix(cfg);
    if (!vm) { fprintf(stderr, "gogo_new failed\n"); free(img); return NULL; }
    int rc = gogo_load(vm, img, len, GOGO_LOAD_COPY);   /* copied: img can go */
    free(img);
    if (rc != GOGO_OK) {
        fprintf(stderr, "gogo_load %s: error %d\n", path, rc);
        gogo_free(vm);
        return NULL;
    }
    return vm;
```

then runs it to the end:

```c
static int run_one(const char *path, const gogo_config *cfg) {
    gogo_vm *vm = load_vm(NULL, NULL, cfg, path);
    if (!vm) return 1;
    int rc = gogo_run(vm);   /* starts main, runs until every actor is done */
    gogo_free(vm);
    return rc == GOGO_OK ? 0 : 1;
}
```

The life of a VM is always the same four steps:

1. **`gogo_new`** (or `gogo_new_posix`) makes it, from a host table and a
   configuration;
2. **`gogo_load`** gives it its image, once. With `GOGO_LOAD_COPY` the VM
   copies the image; without it the image is borrowed and must stay valid,
   unchanged and 4-byte aligned until `gogo_free`;
3. **`gogo_start`** starts the image's `main`, and the host runs the VM:
   with `gogo_run` as here (it starts `main` itself if needed), or by
   stepping it (section 18.5);
4. **`gogo_free`** closes its ports and gives back every byte it took.

```sh
gogo build examples/many_idle -o many_idle.gogo
build/embed-c/main many_idle.gogo
```

```
spawned 1000
idle memory total: 132000 B
```

The program is done when every actor has exited or is blocked for good,
as on a board: `many_idle`'s 1000 actors wait in `Receive` forever, so the
run ends when `main` returns. Every call returns a negative `GOGO_E*` on
error; `gogo_load`'s reasons (a bad image, a missing capability) are also
written to the host's error stream.

`gogo_new_posix` is the ready-made host for POSIX systems: `malloc`,
`CLOCK_MONOTONIC`, the system's wall clock (which the program may not
set), `getentropy`, stdout and stderr, and a latched wait. The VM owns
that host's state and frees it with the VM.

## 18.3 The configuration

The second argument of `gogo_new` (or the only one of `gogo_new_posix`)
is a `gogo_config`. All zero (or `NULL`) is a VM with no capabilities, no
memory cap, no storage and no hot loading. `main.c` fills in three fields
from its options:

```c
        if (strcmp(argv[i], "--flash") == 0 && i + 1 < argc - 1) cfg.fs_root = argv[++i];
        else if (strcmp(argv[i], "--quota") == 0 && i + 1 < argc - 1) cfg.fs_quota = strtoll(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--allow-load") == 0) cfg.allow_code_load = 1;
```

| field | meaning |
|---|---|
| `caps` | the capability bits granted (section 18.9); 0 in this version |
| `mem_limit` | bytes the whole VM may allocate, on top of each actor's own limit; 0 = no cap |
| `max_actors` | live actors at once; 0 = the build's default (16384) |
| `fs_root` | a host directory served as `/flash`, or `NULL` for no storage (section 18.8) |
| `fs_quota` | bytes the files under `fs_root` may hold; 0 = no quota |
| `allow_code_load` | 1 lets `code.Load` load modules (section 18.10) |

Over `mem_limit`, the VM's allocations fail and the actor that asked is
stopped with `oom`, as when an actor goes over its own limit
(chapter 6). `gogo_new` returns `NULL` for a configuration it cannot
honour: capability bits it cannot give, a negative quota, an `fs_root`
that is not a directory.

## 18.4 The host table and the latched wake

`gogo_new` takes a `gogo_host`: the hooks through which the VM reaches the
outside world, and a `ctx` pointer that every hook gets back. `main.c`'s
`--two` mode has a table of its own:

```c
/* No wait hook: this loop sleeps in poll() itself. */
static const gogo_host pipe_host = {
    w_write, w_alloc, w_free, w_now, w_wake, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL
};
```

| hook | |
|---|---|
| `write(ctx, stream, s, n)` | output: stream 1 is `println`, stream 2 crash reports and refusals. Required |
| `alloc(ctx, n)`, `free(ctx, p, n)` | raw memory; `NULL` from `alloc` is out of memory. Required, and thread-safe if other threads post |
| `now_ms(ctx)` | a monotonic clock in milliseconds, any epoch. Required |
| `wake(ctx)` | wakes the host's wait, from any thread |
| `wait(ctx, timeout_ms)` | the idle wait `gogo_run` uses; `NULL` if the host steps the VM itself |
| `unix_time_ms`, `set_unix_time_ms` | a wall clock (an RTC); `NULL`: the VM keeps its own, unknown until a program sets it |
| `random`, `random_bytes` | random numbers; `NULL`: a clock-seeded mixer |
| `config_get` | the values `config.Get` reads; `NULL`: nothing is set |
| `code_alloc`, `code_free` | special memory for hot-loaded code; `NULL`: ordinary memory |

The hooks run on the VM's thread, inside `gogo_step`, except `wake` (and
`alloc`/`free` for posts). A hook must not call `gogo_step`, `gogo_run`,
`gogo_load` or `gogo_start` (they return `GOGO_EBUSY`), nor `gogo_free`.

The one rule that matters is **the latched wake**. A VM that has nothing
to do returns `GOGO_IDLE`, and the host sleeps until the next timer or
until something wakes it: a post from another thread, an event from a
host port. Such a wake can come at any moment, including just after the
step returned `GOGO_IDLE` and just before the host goes to sleep. If the
host's sleep missed it, the VM would sleep through work that is already
there. So:

- `wake` must **latch**: a wake while the host is not waiting makes the
  next wait return at once;
- the host consumes the latch **before** it steps, never between an idle
  step and its wait.

`main.c`'s latch is a byte in a pipe. `wake` writes one; the loop drains
the pipe before it steps the VMs and then sleeps in `poll()`, which returns
at once if a byte arrived after the drain:

```c
static void w_wake(void *c) {
    char b = 1;
    ssize_t r = write(((Ctx *)c)->wake_fd, &b, 1);   /* full pipe: already latched */
    (void)r;
}
```

A condition variable with a flag works the same way, which is what
`gogo_host_posix` does: `wake` sets the flag and signals, `wait` returns
at once while the flag is set and clears it on the way out.

`wake` is required with `wait`, and whenever the host sleeps between steps
in a wait of its own. It may be `NULL` only for a host that never sleeps
(it steps the VM in a busy loop), or one where nothing can make work while
it sleeps (no posts, no host ports, no other threads).

## 18.5 Stepping the VM yourself

`gogo_run` is the simple way: it steps the VM, waits in the host's `wait`
while the VM is idle, and returns when the program is done.
`gogo_run_thread` does the same on a new thread; the VM then belongs to
that thread, the host only posts to it, and `gogo_join` returns the
result (`gogo_free` stops the thread and joins it first).

A host with its own event loop (a GUI, a game, a server) steps the VM
itself:

- `gogo_step(vm, budget)` runs at most `budget` scheduler passes (0: until
  idle or done) and returns `GOGO_RUNNABLE` (work remains), `GOGO_IDLE`
  (nothing to do until a deadline or a wake) or `GOGO_DONE`;
- `gogo_next_deadline(vm)` says when the next timer is due, as a `now_ms`
  value, or `GOGO_NEVER`.

`main.c --two` steps two VMs from one loop on one thread, a slice of each
in turn, and sleeps when both are idle:

```c
    while (live) {
        /* Consume the latch before stepping, never between an idle
         * result and the poll below. */
        char buf[64];
        while (read(fds[0], buf, sizeof buf) > 0) {}
        int busy = 0;
        int64_t next = GOGO_NEVER;
        for (int i = 0; i < 2; i++) {
            if (!(live & 1 << i)) continue;
            int r = gogo_step(vm[i], 64);   /* a slice of work, then the other VM */
            if (r == GOGO_DONE || r < 0) { live &= ~(1 << i); if (r < 0) rc = 1; continue; }
            if (r == GOGO_RUNNABLE) { busy = 1; continue; }
            int64_t d = gogo_next_deadline(vm[i]);   /* GOGO_IDLE */
            if (d != GOGO_NEVER && (next == GOGO_NEVER || d < next)) next = d;
        }
        if (busy || !live) continue;
        int timeout = -1;   /* no deadline: only a wake ends the wait */
        if (next != GOGO_NEVER) {
            int64_t now = (int64_t)w_now(NULL);
            timeout = next <= now ? 0 : next - now > 60000 ? 60000 : (int)(next - now);
        }
        struct pollfd p = { fds[0], POLLIN, 0 };
        if (poll(&p, 1, timeout) < 0 && errno != EINTR) { rc = 1; break; }
    }
```

```sh
build/embed-c/main --two httpparse.gogo many_idle.gogo
```

```
a| GET /index.html
a|   Host = [example.org]
...
a| malformed request, 40 bytes
b| spawned 1000
b| idle memory total: 132000 B
```

A VM is single-threaded: every call on it happens on the thread that owns
it, except `gogo_post`, `gogo_port_post`, `gogo_close_posts`,
`gogo_retain` and `gogo_release`, which any thread may call. Different VMs
may run on different threads at once; the embed tests run 8 VMs on 4
threads, each thread stepping two of them as above
(`runtime/embed/test/test_multi.c`).

## 18.6 Posts: messages from the host

The host sends an actor a message with `gogo_post(vm, name, kind, data,
len)`. The actor must have registered `name` (`actor.Register`, chapter
4), and gets a `port.Post{Kind, Data}`:

```go
func main() {
	actor.Register("greeter", actor.Self())
	println("greeter ready")
	for {
		switch p := actor.Receive().(type) {
		case port.Post:
			println("hello,", p.Data.String(), "( kind", p.Kind, ")")
		}
	}
}
```

`examples/embed-c/post.c` starts this program, steps it until it is idle
(by then it has registered its name), and posts three names:

```c
    while (gogo_step(vm, 0) == GOGO_RUNNABLE) {}
    greet(vm, "Ada");
    greet(vm, "Grace");
    gogo_post(vm, "greeter", 2, "Alan", 4);
```

Then a second thread posts two more and **closes posts**, while the main
thread runs the VM with `gogo_run`:

```c
static void *poster(void *arg) {
    gogo_vm *vm = arg;
    greet(vm, "Edsger");
    greet(vm, "Barbara");
    gogo_close_posts(vm);   /* no more posts: the VM ends when its actors do */
    gogo_release(vm);
    return NULL;
}
```

```
greeter ready
hello, Ada ( kind 1 )
hello, Grace ( kind 1 )
hello, Alan ( kind 2 )
hello, Edsger ( kind 1 )
hello, Barbara ( kind 1 )
posts delivered: 5, dropped: 0
```

The rules:

- **Posts are queued**, in a lock-free inbox, and delivered at the VM's
  next step, in order per posting thread. `gogo_post` copies the data and
  wakes the host. At most `GOGO_POST_MAX` (256) wait at once
  (`GOGO_EFULL` beyond).
- **A post to a name nobody holds is dropped** when it is delivered, and
  counted (`gogo_get_stats`). That includes posts sent before the program
  registered the name, which is why `post.c` steps the program first. A
  post over the receiver's mailbox quota is dropped too.
- **Posts keep the VM alive.** While posts are open and an actor holds a
  registered name, the VM is not done: it can still get posts, so
  `gogo_run` waits. `gogo_close_posts` ends that: later posts return
  `GOGO_ECLOSED`, and the VM is done once its actors are, as on a board.
  Without the close, `post.c`'s `gogo_run` would never return.
- **Another thread needs a reference.** A thread that may post while (or
  after) the owner frees the VM holds one from `gogo_retain` and gives it
  back with `gogo_release`. After `gogo_free` its posts return
  `GOGO_ECLOSED` instead of touching freed memory.

## 18.7 Host ports

A host can define ports of its own, which Go code opens by name like any
port (chapter 7). `examples/embed-c/port.c` defines one called `"echo"`:

```c
/* port.Open("echo", port.Host{Arg, Data}). Events emitted here reach the
 * owner once the open succeeds. */
static int echo_open(void *ctx, gogo_vm *vm, gogo_port p, int32_t arg, const uint8_t *data, size_t len) {
    (void)ctx;
    if (arg < 0) return -1;   /* refused: Open's ok is false */
    gogo_port_emit(vm, p, arg, data, len);
    return 0;
}

/* actor.Send(p, port.Post{Kind, Data}) */
static void echo_command(void *ctx, gogo_vm *vm, gogo_port p, int32_t kind, const uint8_t *data, size_t len) {
    int *commands = ctx;
    ++*commands;
    gogo_port_emit(vm, p, kind + 1, data, len);
}

static const gogo_port_driver echo_driver = { echo_open, echo_command, NULL };
```

and registers it before the program starts:

```c
    if (rc == GOGO_OK) rc = gogo_register_port(vm, "echo", &echo_driver, &commands);   /* before start */
```

The Go side ([`examples/hostecho`](../../examples/hostecho/main.go)) opens
it with a `port.Host` config, sends it `port.Post` commands and receives
`port.Post` events:

```go
	p, ok := port.Open("echo", port.Host{Arg: 7, Data: bytes.FromString("config")})
	println("open:", ok)
	e := event()
	println("hello", e.Kind, e.Data.String())
```

```
open with Arg -1: false
open an unknown port: false
open: true
hello 7 config
echo 2 4 true
echo -4 0 true
echo 2000000001 12 true
echo 4 300 true
closed
```

A driver has three callbacks, each optional: `open` (0 accepts, anything
else refuses; `NULL` accepts all), `command` (a `port.Post` sent to the
port) and `close` (the port closed, for any reason: the owner exited,
`port.Close`, `actor.Kill`, `gogo_free`). They run on the VM's thread
inside `gogo_step`. The host sends the port's owner an event with
`gogo_port_emit` from a callback or between steps, or with
`gogo_port_post` from any thread (queued like a post). A VM has at most
`GOGO_MAX_HOST_PORTS` (4) host ports, registered before `gogo_start`.

Host ports need no capability bit: what one may do is the host's
business. They keep the usual port rules: one owner, closed when the
owner exits, `port.Handoff` gives one away, a stale PID is ignored. While
posts are open an open host port keeps the VM alive, since it can still
get events; `gogo_close_posts` ends that too.

## 18.8 Storage: `/flash` on a host directory

`fs_root` names an existing host directory, which the program sees as
`/flash` (chapter 9). `/sd` is never mounted. Without `fs_root`, every
`gogo/fs` call returns `NotMounted`.

The program is **confined to the directory**. Paths are walked from it one
name at a time without following symlinks: a symlink, FIFO or device
inside it is `BadPath`, and so is opening a file that has other hard links
for writing. A `.` or `..` in a path is `BadPath`, as on a board. On a host file
system that ignores case (macOS by default), `/flash` ignores case too.
Two rules are the host's to keep: the directory must not be writable by
other users (they could link their files in), and VMs running at the same
time must not have nested roots.

`fs_quota` caps the bytes that the regular files under `fs_root` may hold.
The VM counts what is there when it is made, then tracks how much each
write grows a file and what truncation, removal and replacement give back.
A write past the quota stores what fits and returns `NoSpace`, and
`fs.Usage` reports `(used, fs_quota)`. Two things it does not see:

- **only file bytes count.** Empty files and directories cost nothing, so
  a program can create any number of them;
- **only the VM's own changes count.** Files the host or another VM adds
  to the same directory meanwhile make the count approximate.

With `fs_quota` 0 there is no quota (the host file system's own limits
apply). The quota belongs to the VM: two VMs on two directories each have
their own.

## 18.9 Capabilities

An image records the capabilities it needs (`gogo build` infers them;
chapter 13 lists them): `net`, `kbd`, `sd`, `imu` and so on. A VM has the
bits its configuration grants in `caps`, and refuses an image that needs
one it lacks. Basic I/O (`println`, actors, timers, host ports, posts,
`/flash`) needs none.

This version's library has none of those peripherals, so it grants no
bit: a configuration with any `GOGO_CAP_*` bit set makes `gogo_new` fail,
and an image that needs one is refused by `gogo_load` with `GOGO_ECAPS`
and a reason on the error stream:

```sh
build/embed-c/main netdemo.gogo
```

```
gogo: image needs net; firmware lacks net
gogo_load netdemo.gogo: error -4
```

## 18.10 Hot loading: `allow_code_load`

`code.Load` (chapter 14) turns bytes into running code, so whether a
program may use it is the host's decision. With `allow_code_load` 0, the
default, `code.Load` refuses every module, and **the refusal reads as
`NoMemory`** (3): the program cannot tell it from running out of code
memory. Bundled code (the image itself) is unaffected.

`examples/hotload` loads its worker from `/flash`, so it needs both
`fs_root` and `allow_code_load`:

```sh
build/embed-c/main --flash dir --allow-load hotload.gogo   # runs as under gogovm
build/embed-c/main --flash dir hotload.gogo
```

```
load /flash/worker_v1.gmod 3
loaded v1: false
```

## 18.11 Many VMs

Everything a VM has is its own: its memory and memory cap, actors,
timers and clock, `Bytes` handles, ports, posts, `/flash` and quota. The
library has no global state (`make lib` checks its objects with
`tools/globals.sh --objects`), so VMs can run side by side on one thread,
as in `main.c --two`, or on many threads, as in the stress test.

## 18.12 Limits

- No network, TLS or board peripherals in the library yet; host ports are
  the way to give a program access to the host's own devices.
- At most 4 host ports per VM, 256 queued posts, and 255 bytes of name
  and 262,080 bytes of data per post. A post must also fit the receiver's
  memory limit beside its heap and mailbox.
- `port.Post` carries no source: an actor that owns several host ports,
  or also gets posts, tells them apart by `Kind`.
- The `fs_quota` counts regular-file bytes only (section 18.8).
- Internal symbols with the library's reserved prefixes are visible to
  the linker (section 18.1).
