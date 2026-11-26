# 18. Embedding gogo

So far a gogo program has run on a board or under `gogovm`. **Embed mode**
puts the same VM inside a program of your own, the way Lua is embedded: a
desktop or server program, a phone app, a test tool. The program (the
**host**) loads an image that `gogo build` made, runs it, sends its actors
messages, and gives it ports of its own. Any number of VMs can live in one
process, on any threads; they share nothing.

The API is one C header, [`runtime/include/gogo.h`](../../runtime/include/gogo.h)
(C99, no threads), with
[`gogo_posix.h`](../../runtime/include/gogo_posix.h) for the POSIX host
and a VM on a thread of its own, and one library, `libgogo.a`. This
chapter walks through it with the
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

The examples include `gogo_posix.h`, which includes `gogo.h`; a host
with a table of its own needs only `gogo.h`. That split is in the headers
only: `libgogo.a` is one prelinked object, and it needs
POSIX threads (`pthread_create`, `pthread_join`, mutexes and condition
variables), `getentropy`, `clock_gettime`, the `*at` file calls with
`open`, `fdopendir`, `readdir`, `fstatvfs` and `fsync` (for `/flash` on a
host directory), and the C library (`malloc`, `snprintf`, string
functions), whichever header the host includes. A library build for an
RTOS without POSIX is still to come. The library exports only the `gogo_*` functions: everything
else in it is local, so your program may use any other names. `make lib` also builds
`build/libgogo-debug.a`, the same code as separate objects, for debugging
on a machine other than the one that built it
([`runtime/include/README.md`](../../runtime/include/README.md) explains
the difference).

## 18.2 The smallest host

`examples/embed-c/main.c` runs one image. It reads the file, makes a VM on
the POSIX host and loads the image:

```c
    gogo_vm *vm;
    int rc = h ? gogo_new(h, ctx, cfg, &vm) : gogo_new_posix(cfg, &vm);
    if (rc != GOGO_OK) { fprintf(stderr, "gogo_new: %s\n", gogo_strerror(rc)); free(img); return NULL; }
    rc = gogo_load(vm, img, len, GOGO_LOAD_COPY);   /* copied: img can go */
    free(img);
    if (rc != GOGO_OK) {
        fprintf(stderr, "gogo_load %s: %s (%d)\n", path, gogo_strerror(rc), rc);
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
gogo build examples/many_idle -o many_idle.ggb
build/embed-c/main many_idle.ggb
```

```
spawned 1000
idle memory total: 132000 B
```

The program is done when every actor has exited or is blocked for good,
as on a board: `many_idle`'s 1000 actors wait in `Receive` forever, so the
run ends when `main` returns. Every call returns a negative `GOGO_E*` on
error, and `gogo_strerror` describes one; `gogo_load`'s reasons (a bad
image, an unsupported image format, a missing driver) are also written to
the host's error stream, with the names involved.

The error codes are the shared codes of gogo programs (`gogo/errors`,
appendix C; `GOGO_E_*` in `gogo_errors.h`, which `gogo.h` includes),
negated: `GOGO_EINVAL` is `-GOGO_E_BADARG`, `GOGO_ENOMEM` is
`-GOGO_E_NOMEMORY`, `GOGO_EREQUIRES` is `-GOGO_E_UNSUPPORTED`, as
`code.Load` says for the same refusal. One table of meanings covers both
sides, so `gogo_strerror` also describes a code a program hands the host
(a `port.Closed` reason, say), negated: `gogo_strerror(-reason)`.
`gogo_step`'s `GOGO_IDLE` and `GOGO_DONE` are not errors, and it says so. Only `GOGO_ESTATE`, a call
out of order, is the API's own (-36; 36 to 99 are never shared codes).

| result | | from |
|---|---|---|
| `GOGO_EINVAL` | a bad argument, table or configuration | every call |
| `GOGO_ENOMEM` | out of memory, or over `mem_limit` | every call that allocates |
| `GOGO_ESTATE` | out of order: no image, loaded twice, started twice | `gogo_load`, `gogo_start`, `gogo_step`, ... |
| `GOGO_EBUSY` | called from a hook while the VM steps | `gogo_step`, `gogo_run`, `gogo_load`, `gogo_start`, ... |
| `GOGO_EIMAGE` | not an image | `gogo_load` |
| `GOGO_EVERSION` | an image format this library does not run | `gogo_load` |
| `GOGO_EREQUIRES` | the image requires a driver or mount the VM lacks | `gogo_load` |
| `GOGO_ENOTFOUND` | the image has no `main` | `gogo_start` |
| `GOGO_EFULL`, `GOGO_E2BIG` | a queue or table is full; a name or data is too long | posts, host ports |
| `GOGO_ECLOSED`, `GOGO_EEXIST` | posts are closed or the VM is gone; a name is taken | posts, host ports |

**Versions.** `gogo_version()` is the library's version,
`(major << 16) | (minor << 8) | patch`, and `GOGO_VERSION` the header's
(0.2.0 here). A host checks at start that it runs on a library at least
as new as the header it was compiled with, of the same major (during
0.x, the same minor). `gogo_image_format()` says which images the
library loads, `(major << 16) | newest minor`.

`gogo_new_posix` is the ready-made host for POSIX systems: `malloc`,
`CLOCK_MONOTONIC`, the system's wall clock (which the program may not
set), `getentropy`, stdout and stderr, and a latched wait. The VM owns
that host's state and frees it with the VM.

Functions that make something return a result code and hand the new
object back through their last argument: `gogo_new(h, ctx, cfg, &vm)`
is `GOGO_OK`, or an error with `vm` set to `NULL`, so a host can say why
it failed.

## 18.3 The configuration

The third argument of `gogo_new` (or the first of `gogo_new_posix`) is a
`gogo_config`. Start it from `GOGO_CONFIG_INIT`: that is a VM that
grants every name it has, with no memory cap, no storage and no hot
loading (a `NULL` config is the same). `main.c` fills in three fields
from its options:

```c
        if (strcmp(argv[i], "--flash") == 0 && i + 1 < argc - 1) cfg.fs_root = argv[++i];
        else if (strcmp(argv[i], "--quota") == 0 && i + 1 < argc - 1) cfg.fs_quota = strtoll(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--allow-load") == 0) cfg.flags |= GOGO_CONFIG_CODE_LOAD;
```

| field | meaning |
|---|---|
| `size` | the struct's size, set by `GOGO_CONFIG_INIT` (section 18.4 explains why) |
| `flags` | `GOGO_CONFIG_CODE_LOAD` lets `code.Load` load modules (section 18.10) |
| `mem_limit` | bytes the whole VM may allocate, on top of each actor's own limit; 0 = no cap |
| `max_actors` | live actors at once; 0 = the build's default (16384), at most 65520 |
| `grant` | the requirement names the VM may serve, a `NULL`-terminated list; `NULL` = all it has (section 18.9) |
| `fs_root` | a host directory served as `/flash`, or `NULL` for no storage (section 18.8) |
| `fs_quota` | bytes the files under `fs_root` may hold; 0 = no quota |

Over `mem_limit`, the VM's allocations fail and the actor that asked is
stopped with `oom`, as when an actor goes over its own limit
(chapter 6). `gogo_new` returns `GOGO_EINVAL` for a configuration it
cannot honour: no size, a flag it does not know, too many actors, a
negative quota, an `fs_root` that is not a directory.

## 18.4 The host table and the latched wake

`gogo_new` takes a `gogo_host`: the hooks through which the VM reaches the
outside world, and a `ctx` pointer that every hook gets back. `main.c`'s
`--two` mode has a table of its own:

```c
/* No wait hook: this loop sleeps in poll() itself. */
static const gogo_host pipe_host = {
    .size = sizeof(gogo_host),
    .write = w_write,
    .alloc = w_alloc,
    .free = w_free,
    .now_ms = w_now,
    .wake = w_wake,
};
```

The table, like the configuration, begins with its own size; name the
hooks you set, and leave the rest out (they are `NULL`). A table filled
at run time starts from `GOGO_HOST_INIT`:

```
gogo_host h = GOGO_HOST_INIT;
h.write = my_write;
...
```

The size is what lets the library grow without breaking a host built
against an older header. Fields are only ever added at the end. A newer
library reads a smaller table's missing hooks as `NULL` (and a smaller
configuration's missing fields as 0); an older library ignores the
fields it does not know. `gogo_new` copies the table, so it may live on
the stack; the `ctx` must stay valid until the VM is gone. The structs
the library fills in (`gogo_stats`, `gogo_image_info`) work the other way
round: the caller sets `size`, and the library writes back how much of
it it filled, so a host built against a newer header can tell a field
this library lacks from one that is 0. A size smaller than the v0.2
struct's (no host has an older one), in or out, or a reserved word that
is not 0, is refused with `GOGO_EINVAL`, so both can be given a meaning later.

| hook | |
|---|---|
| `write(ctx, stream, s, n)` | output: stream 1 is `println`, stream 2 crash reports and refusals. Required |
| `alloc(ctx, n)`, `free(ctx, p, n)` | raw memory; `NULL` from `alloc` is out of memory. Required, and thread-safe if other threads post |
| `now_ms(ctx)` | a monotonic clock in milliseconds, any epoch. Required |
| `wake(ctx)` | wakes the host's wait, from any thread |
| `wait(ctx, timeout_ms)` | the idle wait `gogo_run` uses; `NULL` if the host steps the VM itself |
| `unix_time_ms`, `set_unix_time_ms` | a wall clock (an RTC); `NULL`: the VM keeps its own, unknown until a program sets it |
| `random(ctx)` | 32 random bits; `NULL`: the VM's own clock-seeded mixer (not for secrets) |
| `config_get` | the values `sys.Config` reads; `NULL`: nothing is set |
| `config_get` (key `"\0args"`) | also how `sys.Args` gets the program's arguments: answer the 5-byte key `\0args` with every argument followed by a NUL byte (so `a`, empty, `b` is `a\0\0b\0`); unset: no arguments |
| `code_alloc`, `code_free` | special memory for hot-loaded code; `NULL`: ordinary memory |
| `destroy(ctx)` | called once when the VM is gone, to free the host's context; `NULL`: nothing |

A hook that can fail returns 0 for success and a negative `GOGO_E*` for
failure (`set_unix_time_ms` refusing a time, a host port's `open`
refusing it); hooks that return a value, a length or a time, say what
they return. `destroy` runs after the VM's last reference is dropped
(`gogo_free`, or the last posting thread's), on that thread, and never
if `gogo_new` failed.

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
that thread, the host only posts to it (through posters, section 18.6),
and `gogo_join` returns the result (`gogo_free` stops the thread and
joins it first).

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
build/embed-c/main --two httpparse.ggb many_idle.ggb
```

```
a| GET /index.html
a|   Host = [example.org]
...
a| malformed request, 40 bytes
b| spawned 1000
b| idle memory total: 132000 B
```

A VM is single-threaded: every call that takes a `gogo_vm` happens on the
thread that owns it. Other threads get a `gogo_poster` (section 18.6),
whose calls any thread may make: the type says which is which. Different VMs
may run on different threads at once; the embed tests run 8 VMs on 4
threads, each thread stepping two of them as above
(`runtime/embed/test/test_multi.c`). In `--two` nothing wakes the loop
from another thread; the embed tests do (`runtime/embed/test/test_embed.c`
posts from other threads at the worst moment, just before the wait, many
thousands of times), and so does `post.c` in the next section.

## 18.6 Posts: messages from the host

The host sends an actor a message with `gogo_post(vm, name, name_len,
kind, data, len)`. The actor must have registered `name`
(`actor.Register`, chapter 4), and gets a `port.Post{From, Kind, Data}`
(`From` is the zero PID; a host port's events carry the port's PID there
instead). Like every name the API takes, `name` is a pointer and a
length, and need not end in a NUL:

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
    gogo_post(vm, "greeter", 7, 2, "Alan", 4);
```

`gogo_post` belongs to the VM's own thread, like every call on a
`gogo_vm`. Another thread posts through a **poster**: a handle made on the
owner's thread with `gogo_poster_new` and handed over. Here a second
thread posts two more names and **closes posts**, while the main thread
runs the VM with `gogo_run`:

```c
    gogo_poster *p = gogo_poster_new(vm);   /* made here, used and freed there */
    int started = p && pthread_create(&t, NULL, poster, p) == 0;
```

```c
static void *poster(void *arg) {
    gogo_poster *p = arg;
    gogo_poster_post(p, "greeter", 7, 1, "Edsger", 6);
    gogo_poster_post(p, "greeter", 7, 1, "Barbara", 7);
    gogo_poster_close_posts(p);   /* no more posts: the VM ends when its actors do */
    gogo_poster_free(p);
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
  next step, in order per posting thread. A post copies the data and
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
- **A poster is a small object of its own.** `gogo_poster_new` can fail
  (`NULL`: out of memory), and each poster is freed exactly once. Using
  one after its free, a second free included, is undefined behaviour,
  which a debug build (`GOGO_DEBUG`) often catches.
- **A poster outlives the free.** It holds a reference to the VM, so a
  thread may post while (or after) the owner calls `gogo_free`: its posts
  then return `GOGO_ECLOSED` instead of touching freed memory. The VM's
  last bytes go back, and the host's `destroy` runs, at the last of
  `gogo_free` and the `gogo_poster_free`s. In Rust terms, a `gogo_vm` is
  `Send` but not `Sync`, and a `gogo_poster` is both.

## 18.7 Host ports

A host can define ports of its own, which Go code opens by name like any
port (chapter 7). `examples/embed-c/port.c` defines one called `"echo"`:

```c
/* port.Open("echo", port.Host{Arg, Data}). Events emitted here reach the
 * owner once the open succeeds. */
static int echo_open(void *ctx, gogo_vm *vm, gogo_port p, int32_t arg, const uint8_t *data, size_t len) {
    (void)ctx;
    if (arg < 0) return GOGO_EINVAL;   /* refused: Open's ok is false */
    gogo_port_emit(vm, p, arg, data, len);
    return 0;
}

/* actor.Send(p, port.Post{Kind, Data}) */
static void echo_command(void *ctx, gogo_vm *vm, gogo_port p, int32_t kind, const uint8_t *data, size_t len) {
    int *commands = ctx;
    ++*commands;
    if (kind == 100) {
        gogo_port_close(vm, p, GOGO_E_IO);   /* the owner gets port.Closed{p, errors.IO} */
        return;
    }
    gogo_port_emit(vm, p, kind + 1, data, len);
}

static const gogo_port_driver echo_driver = {
    .size = sizeof(gogo_port_driver),
    .open = echo_open,
    .command = echo_command,
};
```

and registers it before the program starts:

```c
    if (rc == GOGO_OK && requires(img, len, "echo"))
        rc = gogo_register_port(vm, "echo", 4, &echo_driver, &commands);   /* before load */
```

(`requires` reads the image's requirements, section 18.9.)

The gogo side ([`examples/hostecho`](../../examples/hostecho/main.gg)) opens
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
ended by the host: true true
```

A driver, like the host table, begins with its size; it has three
callbacks, each optional: `open` (0 accepts, a negative `GOGO_E*`
refuses; `NULL` accepts all), `command` (a `port.Post` sent to the port)
and `close` (the port closed, for any reason: the owner exited,
`port.Close`, `actor.Kill`, `gogo_port_close`, `gogo_free`). They run on
the VM's thread inside `gogo_step`, and get the port as a `gogo_port`: a
nonzero 32-bit value that names that one open port, and no later one in
its slot until 4096 more closes there. The host sends the port's owner
an event with `gogo_port_emit` from a callback or between steps, or with
`gogo_poster_port_post` from any thread (queued like a post). A VM takes
a few host ports (4 in this version; `GOGO_EFULL` beyond), registered
before `gogo_load`: an image that opens a host port by a constant name
requires it (section 18.9), and a registration after the load returns
`GOGO_ESTATE`.

**A port can end on the host's side**, when the device behind it goes
away, say. `gogo_port_close(vm, port, reason)` sends the owner
`port.Closed{From, Reason}` (chapter 7's stream event: the port does
nothing more), with a shared code as the reason (`GOGO_E_IO` is
`errors.IO`; it must be positive, as 0 is a stream's Normal half-close),
and closes the port: the driver's `close` runs, and the program's own
`port.Close` of it later finds a stale PID and does nothing. Every post
already queued is delivered first (the whole inbox, as the next step
would, so the port's own events come before the end), and the
`Closed` is never dropped, not even over the owner's mailbox quota: it
is made when the port opens, as a stream port keeps room for its own. `port.c` ends its port when it gets a command of Kind 100, and
`hostecho` waits for the `Closed`:

```go
	actor.Send(q, port.Post{Kind: 100}) // the host ends the port
	for ended := false; !ended; {
		switch c := actor.Receive().(type) {
		case port.Closed:
			println("ended by the host:", c.From == q, c.Reason == int(errors.IO))
			ended = true
		}
	}
```

What a host port may do is the host's business. They keep the usual port rules: one owner, closed when the
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

## 18.9 Requirements and `grant`

An image records, by name, what it needs (`gogo build` infers it; chapter
13 lists the names): the driver of every constant `port.Open` and `/sd`
for constant `/sd` paths. On an embedded VM the drivers are the host ports
registered before `gogo_load`, and the one mount is `/flash` when
`fs_root` serves it. `gogo_load` refuses an image that requires anything
else with `GOGO_EREQUIRES` (-10) and names the first missing name on the
error stream. Basic I/O (`println`, actors, timers, posts) needs nothing.

This version's library has no board peripherals, so an image that uses
the network, say, is refused. The first missing name in sorted order is
reported, here `gpio` (netdemo opens buttons too) rather than a network
driver:

```sh
build/embed-c/main netdemo.ggb
```

```
gogo: image requires gpio, which this VM lacks
gogo_load netdemo.ggb: unsupported: a driver or mount is missing (-10)
```

A host can read an image's requirements before it loads it, without a
VM: `gogo_image_info_get` gives the image's format and the number of its
requirements and exports, and `gogo_image_requires` their names, as
`gogo_name` pairs (pointer and length).
`port.c` registers its port only for an image that needs it:

```c
static int requires(const void *img, size_t len, const char *want) {
    gogo_name names[16];
    uint32_t n;
    int found = 0;
    if (gogo_image_requires(img, len, names, 16, &n) != GOGO_OK) return 0;
    for (uint32_t i = 0; i < n && i < 16; i++) {
        fprintf(stderr, "requires %.*s\n", (int)names[i].len, names[i].ptr);
        if (names[i].len == strlen(want) && memcmp(names[i].ptr, want, names[i].len) == 0) found = 1;
    }
    return found;
}
```

The names point into the image and are not NUL-terminated. Each call
validates the whole image once, as `gogo_load` does, and returns its
errors (`GOGO_EIMAGE`, `GOGO_EVERSION`, with the format still filled
in), so they also tell a host whether an image will load at all.

`grant` narrows what the VM has: a `NULL`-terminated list of names, and
whatever is not listed is absent for this VM (an image requiring it is
refused, `port.Open` of it fails, and `/flash` is not mounted unless
`"/flash"` is listed). `NULL` grants everything. The list and its strings
must stay valid while the VM exists.

```
static const char *const grant[] = { "ui", "/flash", NULL };
gogo_config cfg = GOGO_CONFIG_INIT;
cfg.grant = grant;     /* a host port "sensor" registered later stays hidden */
```

An image whose format is another major, or a newer minor than this
library's, is refused with `GOGO_EVERSION` (-27), and both formats are
named (`gogo: unsupported image format 15.2 (this VM runs 15.0 to
15.1)`).

## 18.10 Hot loading: `GOGO_CONFIG_CODE_LOAD`

`code.Load` (chapter 14) turns bytes into running code, so whether a
program may use it is the host's decision. Without the flag, the
default, `code.Load` refuses every module, and **the refusal reads as
`NoMemory`** (3): the program cannot tell it from running out of code
memory. Bundled code (the image itself) is unaffected.

`examples/hotload` loads its worker from `/flash`, so it needs both
`fs_root` and `GOGO_CONFIG_CODE_LOAD`:

```sh
build/embed-c/main --flash dir --allow-load hotload.ggb   # runs as under gogovm
build/embed-c/main --flash dir hotload.ggb
```

```
load /flash/worker_v1.ggb 3
loaded v1: false
```

## 18.11 Many VMs

Everything a VM has is its own: its memory and memory cap, actors,
timers and clock, `Bytes` handles, ports, posts, `/flash` and quota. The
library has no global state (`make lib` checks its objects with
`tools/globals.sh --objects`), so VMs can run side by side on one thread,
as in `main.c --two`, or on many threads, as in the stress test.

## 18.12 What is stable

From v0.2 the API has a compatibility promise: within a major version
(during 0.x, within a minor) functions keep their signatures and
meaning, and the size-first structs only grow at the end. A part still
taking shape is marked `GOGO_EXPERIMENTAL` in the headers and may change
in any minor release:

| stable | experimental |
|---|---|
| `gogo_new`, `gogo_load`, `gogo_start`, `gogo_step`, `gogo_next_deadline`, `gogo_run`, `gogo_free` | host ports: `gogo_register_port`, `gogo_port_emit`, `gogo_port_close`, `gogo_poster_port_post`, `gogo_port_driver` |
| the host table, `gogo_host_posix`, `gogo_new_posix` | the flag `GOGO_CONFIG_CODE_LOAD`; the configuration's `grant`, `fs_root` and `fs_quota` |
| the configuration's `size`, `flags`, `mem_limit`, `max_actors` | `gogo_get_stats` and its fields |
| the error codes, `gogo_strerror`, `gogo_version`, `gogo_image_format` | `gogo_image_info_get`, `gogo_image_requires` |
| `gogo_post`, `gogo_close_posts` and the poster (`gogo_poster_new`, `_post`, `_close_posts`, `_free`) | `gogo_run_thread`, `gogo_join` |

`GOGO_EXPERIMENTAL` expands to nothing. To see every place a program
leans on the experimental part, define it before the include as a
deprecation:

```
#define GOGO_EXPERIMENTAL __attribute__((deprecated("experimental")))
#include "gogo_posix.h"
```

The compiler then warns at each use (`make test-libgogo` builds its
host this way with `-Werror`, to keep it on the stable part).

## 18.13 Limits

- No network, TLS or board peripherals in the library yet; host ports are
  the way to give a program access to the host's own devices.
- At most 4 host ports per VM, 256 queued posts, and 255 bytes of name
  and 262,080 bytes of data per post. A post must also fit the receiver's
  memory limit beside its heap and mailbox.
- `gogo_port_close` belongs to the VM's thread; a host whose device goes
  away on another thread hands the news to that thread first (a flag it
  checks between steps, say).
- The `fs_quota` counts regular-file bytes only (section 18.8).
