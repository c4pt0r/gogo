# 22. Extending gogo with C

Sooner or later a program needs something gogo does not have: a sensor on
I2C, a codec, a checksum that must run at a million bytes a second, a
library that exists only in C. This chapter answers the question "can I
link C into my gogo program?"

The short answer is no, not directly, and that is deliberate. gogo has no
foreign function interface (FFI): gogo code never calls a C function. The
one bridge between gogo and C is the **port** (chapter 7). C code becomes
a port driver, gogo code opens it with `port.Open` and talks to it with
messages.

There is one driver API for that, `runtime/include/gogo.h`, the same
header a program that embeds gogo uses (chapter 18). A driver written
against it is a table of three callbacks, and the same table, unchanged,
runs in three places:

1. **in a firmware**, as an ESP-IDF component of your own that registers
   itself with `GOGO_BOARD_PORT` and is built with `gogo firmware
   -component`: no fork of gogo, no gogo source file changed;
2. **in `gogovm`**, built with the same component (`make vm DRIVERS=...`),
   so the driver can be tested on a computer;
3. **in a program that embeds gogo**, registered with
   `gogo_register_port` (a host port, section 18.7).

Sections 22.2 to 22.8 are about that API, with a small driver, `dice`,
built all three ways. The runtime's own drivers (`gpio`, `kbd`, `tcp`, ...)
are written against an internal table instead, `PortDriver`, which can do
more and promises nothing; sections 22.9 to 22.16 describe it for work on
gogo itself, and 22.17 lists what is still missing.

## 22.1 No FFI, on purpose

An FFI would let gogo code call `crc32(buf, len)` the way Go code calls
into cgo. gogo leaves it out because each of the VM's guarantees would
break at the first such call.

- **Isolation.** Every actor has its own heap, and nothing can reach into
  another actor's memory (chapter 6). That is what makes "let it crash"
  safe: a crash ends one actor and frees exactly its memory. C code has no
  such boundary. A stray write in a C function called from an actor can
  corrupt any actor's heap, or the VM's own tables, and no supervisor can
  recover from that. A driver is C too, but it never touches actor heaps
  directly: it reads commands in place and delivers events through the VM,
  which copies them into the owner's mailbox under its quota.
- **Preemption.** One thread runs every actor, and the VM switches actors
  after a budget of reductions (section 4.1). A C function cannot be
  preempted: while it runs, no other actor runs, no timer fires and no
  event is delivered. A port turns that around: a command returns at once,
  slow work happens in hardware (DMA, interrupts) or on another task, and
  the result comes back later as a message.
- **Portability.** One image runs on the StickS3, the Cardputer, `gogovm`
  on a laptop and inside any program that embeds `libgogo`, and it can be
  hot-loaded over the network (chapter 14). An image is bytecode and
  constants; it cannot bind to a C symbol, because there is no linker on
  the device and no symbol table that would be the same on two platforms.
  A port name is a string, the same everywhere.
- **Checked at load.** An image lists, by name, the drivers it opens: the
  compiler records the name of every constant `port.Open` as a
  *requirement* ([`docs/design.md`](../design.md) section 5.4.4). The
  loader checks each name against the VM's driver list and refuses an image
  that needs a driver the firmware lacks, before any of its code runs:

  ```
  gogo: image requires dice, which this VM lacks
  ```

  With FFI there would be nothing to check: a missing or different C
  function would be found only when it was called, or would crash.

So the question "how do I call C?" becomes "how do I write a port?". The
answer costs more than an `extern` declaration, but the result keeps every
property above.

## 22.2 Two APIs

| | The driver API, `gogo.h` (22.2 to 22.8) | The internal table, `PortDriver` (22.9 to 22.16) |
|---|---|---|
| Header | `runtime/include/gogo.h`: versioned, the rules of section 22.8 | `runtime/vm/port.h` and the VM's headers: change in any release |
| Lives in | your own code: an ESP-IDF component, a source of a `gogovm` build, your embedding program | this tree (`runtime/`, `platform/`) |
| Runs on | the boards, `gogovm`, embedding hosts: one table for all | the boards and `gogovm` |
| Registered by | `GOGO_BOARD_PORT` (firmware, `gogovm`), `gogo_register_port` (a host) | the board's driver list (`esp_ports`, `unix_ports_all`) |
| Message types | `port.Host` to open, `port.Post` commands and events | any reserved types, plus the generic ones |
| From an interrupt or another task | a task or thread posts through a `gogo_poster` (22.7) | the event ring, one writer (22.11) |
| Choose it for | a product's own hardware, anything outside this tree | gogo's own drivers |

Both speak to gogo code the same way: a name for `port.Open`, which is
also the image's requirement (an image that needs `dice` is refused at
load by a VM without it), commands as messages to the port, events as
messages to its owner. Only the C side differs. **Write a third-party
driver on `gogo.h`.**

## 22.3 A driver on `gogo.h`: `dice`

[`examples/dice-driver`](../../examples/dice-driver/) is a complete driver
written the way someone outside the project would write one. The
protocol:

- `port.Open("dice", port.Host{Arg: seed})`: seed must be positive, else
  the open fails with `errors.BadArg`;
- `actor.Send(p, port.Post{Kind: n})`: roll n dice (1 to 10);
- one event `port.Post{From: p, Kind: face}` per die.

Its directory `dice/` is an ESP-IDF component, and its C files include
`gogo.h` and nothing else of gogo (`make example-driver` checks that):

```text
examples/dice-driver/
  main.gg              the gogo program
  driver.out           its output
  host.c               an embedding host with the driver (22.5)
  dice/                the driver, an ESP-IDF component
    CMakeLists.txt
    gogo-driver.json   {"ports": ["dice"]}: what it provides (22.4)
    dice.h             the driver's table and state
    dice.c             the driver
    dice_port.c        GOGO_BOARD_PORT: the board's registration (22.4)
```

### The table

A driver is a `gogo_port_driver`: its size first, then three callbacks,
each optional:

```c
typedef struct gogo_port_driver {
    uint32_t size;          /* sizeof(gogo_port_driver) as the host was compiled (GOGO_PORT_DRIVER_INIT) */
    uint32_t reserved;      /* 0 */
```

- **`open(ctx, vm, port, arg, data, len)`** is `port.Open` with a
  `port.Host{Arg, Data}`: `arg` and the bytes of `Data`. 0 accepts it;
  a nonzero value refuses it (section 22.6).
- **`command(ctx, vm, port, kind, data, len)`** is a `port.Post{Kind,
  Data}` sent to the port by any actor. Anything else sent to it is
  ignored before the driver sees it.
- **`close(ctx, vm, port)`** says the port has closed, for any reason:
  `port.Close`, the owner's exit, `actor.Kill`, the end of the VM.

`ctx` is the pointer given at registration, the driver's state. `port`
is a `gogo_port`, a 32-bit value naming that one open port (a later port
in the same slot gets another value), which the driver uses as the key
of its own per-port state. `vm` is a handle to give back to the calls
of the API. The dice table:

```c
const gogo_port_driver dice_driver = {
    .size = sizeof(gogo_port_driver),
    .open = dice_open,
    .command = dice_command,
    .close = dice_close,
};
```

and its state, one table per VM, with a slot per open port:

```c
typedef struct DiceTable {
    struct {
        gogo_port port;          /* 0: free */
        uint32_t seed;
    } slot[DICE_PORTS];
} DiceTable;
```

### Open, command, close

```c
static int dice_open(void *ctx, gogo_vm *vm, gogo_port port, int32_t arg, const uint8_t *data, size_t len) {
    DiceTable *t = ctx;
    (void)vm, (void)data, (void)len;
    if (arg <= 0) return GOGO_EINVAL;
    int i = slot_of(t, 0);
    if (i < 0) return GOGO_EFULL;   /* errors.TooMany */
    t->slot[i].port = port;
    t->slot[i].seed = (uint32_t)arg;
    return 0;
}
```

`GOGO_EINVAL` refuses seed 0 with `errors.BadArg`, and a full table with
`errors.TooMany` (section 22.6). A command rolls, sending each face to
the port's owner with `gogo_port_emit`:

```c
static void dice_command(void *ctx, gogo_vm *vm, gogo_port port, int32_t kind, const uint8_t *data, size_t len) {
    DiceTable *t = ctx;
    int i = slot_of(t, port);
    (void)data, (void)len;
    if (i < 0 || kind < 1 || kind > 10) return;
    while (kind-- > 0) {
        if (gogo_port_emit(vm, port, next_face(&t->slot[i].seed), NULL, 0) != 0) return;   /* the owner cannot take more now */
    }
}
```

`gogo_port_emit(vm, port, kind, data, len)` queues `port.Post{From: the
port, Kind, Data}` (the data copied) on the port's owner, whoever that
is now: after `port.Handoff` the events follow the new owner, and the
driver does nothing for it. It never kills anyone: over the owner's
mailbox quota it returns `GOGO_EFULL` and the event is dropped, so the
driver stops rolling. A command the driver does not understand (a
`Kind` out of range) is ignored: any actor holding the port's PID may
send it anything. `close` frees the slot:

```c
static void dice_close(void *ctx, gogo_vm *vm, gogo_port port) {
    DiceTable *t = ctx;
    int i = slot_of(t, port);
    (void)vm;
    if (i >= 0) t->slot[i].port = 0;
}
```

Every callback runs on the VM's thread (the VM task on a board), inside
the scheduler, never at the same time as an actor: it must return
quickly (section 22.7). Besides `gogo_port_emit` a driver may end its
port from its side with `gogo_port_close(vm, port, reason)`: the owner
gets `port.Closed{From, Reason}`, which is never dropped, and the port
closes (section 18.7).

## 22.4 Into a firmware or `gogovm`: `GOGO_BOARD_PORT`

A board registers drivers of its build at boot. One line, in a file of
the driver's own, does it:

```c
static DiceTable dice_table;   /* a board runs one VM */

GOGO_BOARD_PORT("dice", &dice_driver, &dice_table);
```

`GOGO_BOARD_PORT(name, &driver, ctx)` puts a constant entry in a linker
section; no code runs to register it, and no gogo file names it. At
boot, after the VM is made and before the image loads, the firmware (and
`gogovm`) walks the section and registers every entry as
`gogo_register_port` would, so an image's requirement `dice` is
satisfied. Entries are registered in name order. One the board refuses,
because its name is a runtime driver's (`gpio`, `tcp`, ...) or the
board's, or an earlier entry has it, or it is longer than 31 bytes, or a
board has more than 8, is named on the serial log at boot and left out:

```
gogo: board port "alpha" not registered: the name is taken (by a driver of the runtime or the board, or a board port before it)
```

The registered ones join the board's requirement names: `gogo device
info` lists them in `provides`, and `gogovm --provides` prints them.

### The component

On the ESP32-S3 the driver is an ESP-IDF component:

```cmake
idf_component_register(SRCS dice.c dice_port.c
                       INCLUDE_DIRS "."
                       REQUIRES gogo_api
                       WHOLE_ARCHIVE)
```

`gogo_api` (`platform/esp32s3/components/gogo_api`) gives it
`runtime/include` and nothing else. `WHOLE_ARCHIVE` matters: nothing
calls into `dice_port.c`, so without it the linker would leave that file
out and the port with it. Beside it, `gogo-driver.json` tells `gogo
firmware` which requirement names the component provides:

```json
{"ports": ["dice"]}
```

Then one command builds a firmware with it, for the images that need
it:

```
$ gogo build examples/dice-driver -o dice.ggb
$ gogo firmware -board sticks3 -component examples/dice-driver/dice -build dice.ggb
board ports: dice
...
```

`gogo firmware` accepts the images' requirement `dice` because the
component provides it (without `-component` it stops: "the images
require "dice", which no gogo firmware provides"), turns on the
firmware's host-port layer (`CONFIG_GOGO_BOARD_PORTS`) and passes the
directory to `idf.py` as `EXTRA_COMPONENT_DIRS` (chapter 12). Flash it
as any firmware (`idf.py flash`, or `gogo setup -release-url` with the
build's files), and the program runs:

```
$ gogo device info
... provides=gpio,netif,...,/flash,float,dice
$ gogo flash dice.ggb
```

The layer costs about 3.5 KB of flash (the StickS3 firmware with `dice`
is 4.1 KB larger than the same one without it; posters add a few hundred
bytes more to a driver that uses them) and is linked only into a
firmware built with a board port. A
`GOGO_BOARD_PORT` in a build without it fails to link, naming
`gogo_board_ports_enabled`: turn on `CONFIG_GOGO_BOARD_PORTS`.

### On `gogovm`

`gogovm` always has the layer. `make vm DRIVERS=examples/dice-driver/dice`
builds it with the component's `*.c` files (but `*_esp.c`, the
ESP-only hardware files of a component, by convention), and then:

```
$ build/gogovm --provides | tail -1
dice
$ gogo run examples/dice-driver
open with seed 0: bad argument
...
```

A driver with hardware puts it behind a small table of its own and gives
`gogovm` a simulated backend in a host-only file, as the runtime's own
drivers do (section 22.13): the driver's logic is tested on the
computer, the board adds the real hardware.

## 22.5 Into an embedding host

The same table is a host port of a program that embeds gogo. `host.c`
registers it before the load, and the image runs as on a board that has
the driver:

```c
    static DiceTable table;
    gogo_vm *vm = NULL;
    int rc = gogo_new_posix(NULL, &vm);
    if (rc == GOGO_OK) rc = gogo_register_port(vm, "dice", 4, &dice_driver, &table);   /* before the load */
    if (rc == GOGO_OK) rc = gogo_load(vm, img, len, GOGO_LOAD_COPY);
    if (rc == GOGO_OK) rc = gogo_run(vm);
```

`dice.c` is compiled in unchanged; `dice_port.c`, the board's
registration, is not (in a build without a board it would not link).
Chapter 18 covers the rest of the embedding API: section 18.7 the host
ports, 18.12 the same in Go.

### The program

`main.gg` is ordinary gogo. It rolls, hands the port to another actor
(no driver code is involved), and opens a second port with the same seed:

```go
// roll asks the dice port p for n dice and prints the faces.
func roll(who string, p actor.PID, n int) {
	actor.Send(p, port.Post{Kind: n})
	faces := ""
	for got := 0; got < n; {
		switch e := actor.Receive().(type) {
		case port.Post:
			if e.From == p {
				faces += " " + strconv.Itoa(e.Kind)
				got++
			}
		}
	}
	println(who, "rolls"+faces)
}

func main() {
	_, err := port.Open("dice", port.Host{Arg: 0})
	println("open with seed 0:", err.Error())

	p, err := port.Open("dice", port.Host{Arg: 42})
	println("open with seed 42:", err == nil)
	roll("main", p, 3)
	roll("main", p, 5)
```

The output is the same on `gogovm` with the driver, in `host.c` and on
the StickS3.

Its output:

```text
open with seed 0: bad argument
open with seed 42: true
main rolls 1 5 4
main rolls 3 1 1 1 5
handoff: true
player rolls 2 6 5 2
done
a second port, same seed: true
main rolls 1 5 4
```

Seed 0 is refused with `bad argument`, the code the driver returned. The
second port starts the same sequence again, since each port has its own
slot. On a plain `gogovm` the image does not start at all:

```
$ gogo run examples/dice-driver
gogo: image requires dice, which this VM lacks
```

`make example-driver` (part of `make test`) runs the program on a
`gogovm` built with the component and in `host.c`, compares both with
`driver.out`, checks that a plain `gogovm` refuses the image and that
`dice/` includes only `gogo.h`, and checks that every C and Go block in
this chapter is cut verbatim from the tree.

## 22.6 Opening, refusing and owners

`port.Open(name, cfg)` returns `(actor.PID, error)`. The VM decides most
errors before or after the driver:

| Error | When |
|---|---|
| `errors.NotFound` | no driver of that name in this VM |
| `errors.BadArg` | `cfg` is not a struct of the driver's type (`port.Host` for a `gogo.h` driver) |
| `errors.TooMany` | the VM's port table is at its cap (section 22.9) |
| `errors.NoMemory` | the owner cannot take the port |
| the driver's code | its open refused with a code: a `gogo.h` driver returns a negative `GOGO_E*` (the shared code negated), a `PortDriver` a positive shared code |
| `errors.Busy` | its open refused with any other nonzero value |

A `gogo.h` driver refuses an open by returning nonzero from `open`. A
negative `GOGO_E*` whose code is a shared one (`gogo_errors.h`, the same
numbers as `gogo/errors`, negated: `GOGO_EINVAL` is `-GOGO_E_BADARG`) is
exactly the error `port.Open` returns; any other nonzero value
(`GOGO_ESTATE`, a positive number) means `errors.Busy`, the generic "the
resource is taken". Pick the code a caller can act on:

| Return | `port.Open` gets | For |
|---|---|---|
| `GOGO_EINVAL` | `errors.BadArg` | a config value the driver will never accept |
| `GOGO_EBUSY` | `errors.Busy` | a resource someone else holds now |
| `GOGO_EFULL` | `errors.TooMany` | a table of the driver's is full |
| `GOGO_ENOMEM` | `errors.NoMemory` | a buffer that does not fit |
| `-GOGO_E_UNSUPPORTED` | `errors.Unsupported` | hardware this board lacks |
| `-GOGO_E_IO` | `errors.IO` | the device did not answer |

Mind that -1 is `GOGO_ENOTFOUND` ("not found"), not a generic refusal.

Events a driver emits during its own `open` reach the owner once the
open succeeds; if it refuses, they are discarded. A driver that did set
up hardware before refusing undoes it before it returns.

**Owners.** The actor that opened the port owns it. Its events go only to
the owner; the owner's exit closes the port; `port.Handoff(p, to)` makes
another actor the owner. The driver keeps nothing about the owner: it
emits to the port, and the VM delivers to whoever owns it, so a handoff
needs no driver code, as the dice example shows.

## 22.7 Events from tasks and threads

A callback must return quickly: while it runs, no actor runs. Slow work
(a bus transfer, a network request, waiting for a sensor) goes to a task
or thread of the driver's own, and the result comes back as an event
through a **poster**: `gogo_poster_new(vm)` (in a callback) makes a
handle that any thread may use, and `gogo_poster_port_post(poster, port,
kind, data, len)` queues `port.Post{Kind, Data}` for the port's owner and
wakes the VM. Posts from one thread arrive in the order made; one for a
port that has closed since is dropped. On a board the wake is the VM
task's latched notification (section 18.4), so a post made while the VM
sleeps is never lost. `make test-board-ports` has a driver,
`runtime/board/test/ports/ticker.c`, whose thread posts 2000 events to
its port this way and the program checks each arrives, in order.

The rules:

- **Post from a task or a thread, never from an interrupt handler.** A
  post allocates its message (from the heap, which may block), so an
  ISR hands its data to a task first (a FreeRTOS queue or a task
  notification), and the task posts.
- **The VM's calls belong to its thread.** `gogo_port_emit` and
  `gogo_port_close` run only in a callback; from elsewhere the driver
  posts. On a board the `gogo_vm` handle a callback gets takes
  `gogo_port_emit`, `gogo_port_close`, `gogo_post`, `gogo_close_posts`
  and `gogo_poster_new`, and nothing else of the header (`gogo_step`,
  `gogo_get_stats`, ... are libgogo's).
- **A post can fail.** `GOGO_EFULL` with 256 posts queued (try again
  later), `GOGO_ECLOSED` once the VM has finished. Free the poster
  (`gogo_poster_free`) when the thread is done with it.
- **Never block in `close` on a thread that may be posting.** `close`
  runs on the VM's thread, the only one that drains posts: a driver
  thread waiting for room (`GOGO_EFULL`) waits for that very thread, so
  a `close` that joins it, or waits on a semaphore it gives, deadlocks.
  `close` only tells the thread to stop (an atomic flag, a task
  notification); the thread checks the flag before each post and while
  it waits for room, frees its own poster and ends; then `close` may
  join it. Wait for room with a pause (`vTaskDelay`, `nanosleep`), not a
  yield: a driver task of higher priority than the VM task that yields
  never lets the VM run. A post to a port that has closed is not an
  error (it is dropped when delivered), so the flag is the thread's one
  sign to stop. `ticker.c` follows this, and `make test-board-ports`
  closes one with 100,000 posts still to make.
- **An open port keeps the VM alive**, as it may still get events: a
  program waiting for a sensor's next reading does not end. Its owner's
  exit closes it.

These events do not use the VM's event ring or an event-source slot,
which stay the runtime's own drivers' business (section 22.11).

## 22.8 The stability rules

`gogo.h` follows the rules written at its top. Within a major version
(during 0.x, within a minor) a function keeps its signature and meaning,
and a struct that starts with its size only grows at the end: a driver
compiled against an older header keeps working with a newer gogo (the
fields it lacks take their defaults), and one compiled against a newer
header works with an older gogo as far as the fields it knows. The
driver part of the header (`gogo_port_driver`, `gogo_board_port`,
`GOGO_BOARD_PORT`, `gogo_register_port`, `gogo_port_emit`,
`gogo_port_close`, `gogo_poster_port_post`) is still marked
`GOGO_EXPERIMENTAL` in v0.2: it may change in a minor release until
v0.3, when it joins the promise. The image side needs no rule of its
own: a driver on `gogo.h` uses only `port.Host`, `port.Post` and the
shared error codes, which every image format since they came has, so an
image that opens it runs on any firmware of its format that has the
driver.

What is not promised: everything under `runtime/` but `runtime/include`.
A driver that includes a VM header (`port.h`, `heap.h`) is an in-tree
driver, and can break with any release.

## 22.9 The internal driver table

The rest of the chapter is about gogo's own drivers, for work on gogo
itself. They are written against the VM's internal headers, which can
change in any release: a driver outside this tree uses `gogo.h`
(sections 22.2 to 22.8) instead.

An in-tree driver is a `PortDriver` (`runtime/vm/port.h`), a table of C
functions with a name:

```c
typedef struct PortDriver {
    const char *name;
    uint32_t cfg_type;      /* reserved type index the cfg struct must have */
    uint32_t event_type;    /* reserved type of event messages: struct of 2 ints (a, b) */
    /* cfg: the cfg struct's int fields in order. 0 = ok; a GOGO_E_* code
     * (> 0) refuses for that reason, anything else (< 0) as busy. */
    int  (*open)(struct Vm *vm, Port *p, uint16_t index, const int32_t *cfg, uint32_t n);
    /* A command struct of reserved type `type` whose fields are all int. */
    void (*command)(struct Vm *vm, Port *p, uint32_t type, const int32_t *f, uint32_t n);
    void (*close)(struct Vm *vm, Port *p);
    bool (*can_event)(const struct Vm *vm, const Port *p);   /* may still produce events */
```

followed by five optional fields: `handoff`, `command_term`, `open_term`,
`hw` and `event`. Every callback may be `NULL`. What each field is for:

- **`name`** is what gogo code passes to `port.Open`, and the requirement
  name the compiler records.
- **`cfg_type`** is the reserved type the config must have: `port.Open`
  with any other struct fails with `errors.BadArg` before the driver sees
  it.
- **`open`** gets the config's fields, decoded to an `int32_t` array
  (at most `PORT_MAX_FIELDS`, 8, and all of them ints). It returns 0 to
  accept the open, or refuses it with an error code (section 22.6). **`open_term`** replaces it for a config with other
  fields (a string, `Bytes`): it gets the config struct itself, read in
  place on the owner's heap.
- **`command`** gets every message sent to the port whose fields are all
  ints, with its type, so the driver dispatches on the type and ignores
  the rest. **`command_term`** replaces it for commands with other fields
  (a `port.Out`'s `Bytes`): it gets the message itself, read in place on
  the sender's heap. Any actor that holds the port's PID may send it
  commands; the driver does not see who the owner is unless it asks
  (`port_owner`).
- **`close`** runs when the port closes for any reason: `port.Close`, the
  owner's exit, `actor.Kill` on the port, the end of the VM, and after an
  open that the VM refuses after the driver accepted it (the charge did
  not fit, section 22.11).
- **`can_event`** says whether the port can still produce events by
  itself. While any open port says yes, the VM keeps running even when
  every actor is waiting; a port that only answers commands says no.
- **`handoff`** runs after `port.Handoff` gave the port to a new owner
  (the TCP driver uses it to resume an accepted connection).
- **`hw`** is the driver *instance*'s hardware layer, a table the board
  fills. The VM never reads it; the driver finds it at `p->drv->hw`.
- **`event`** takes the driver's events from the event ring (section 22.11)
  instead of the VM delivering each one as it is; the driver delivers
  with `port_deliver` when it wants (`gpio` debounces there).

Each open port is a `Port` in the VM's port table. Besides its driver
and owner, it has four words of driver-private state (`p->state`) and
the memory the driver charges to the owner (section 22.11). A driver whose per-port state fits
in 16 bytes needs nothing else. The table has a cap per VM: 16 ports by
default on a board (`GOGO_MAX_PORTS`), 1024 on `gogovm` and in an
embedded VM, changed with the config key `vm.max_ports` or the embedding
config's `max_ports`, at most 2048. The first 16 `Port`s live in the
`Vm` itself, so opening them takes no memory from the actors' arena; a
larger table grows in chunks of 16 that never move, so a driver may keep
a `Port *` for as long as the port is open.

Every callback runs on the VM's thread, inside the scheduler, never at
the same time as an actor. It must return quickly (section 22.16).

## 22.10 A real driver: `gpio`

The `gpio` driver (`runtime/gpio/gpio.c`, 211 lines) is the smallest driver
that has a config, a command and events from an interrupt; about half of
it is the debounce of its events (below). It is written
once, in the runtime, and each board gives it a hardware layer.

### The gogo side

The types are in `lib/gpio/gpio.gg`, each with a fixed index in every image
(`//gogo:reserved N`, section 22.14):

```go
// Config configures a "gpio" port: one pin.
//
//gogo:reserved 6
type Config struct {
	Pin        int
	Mode       int // Input, InputPullUp, InputPullDown, Output
	Edge       int // None, Rising, Falling, Both (inputs only)
	DebounceMs int // input debounce; <= 0 means the default, 20 ms
}

// Write is a command: drive an output pin to Value (0 or 1).
//
//gogo:reserved 7
type Write struct{ Value int }

// Edge is an event: the pin's level changed; Value is the level after it.
//
//gogo:reserved 8
type Edge struct{ Pin, Value int }
```

On the C side the same indices are `TYPE_GPIO_CONFIG`, `TYPE_GPIO_WRITE`
and `TYPE_GPIO_EDGE` in `runtime/vm/opcodes.h`.

### The table

`runtime/gpio/gpio.h` builds the table with a macro, so a board can make an
instance over its own hardware layer in one line:

```c
/* A board's gpio driver instance over its HAL:
 *   const PortDriver my_gpio_driver = GPIO_DRIVER(&my_gpio_hal); */
#define GPIO_DRIVER(hal) \
    { .name = PORT_NAME_GPIO, .cfg_type = TYPE_GPIO_CONFIG, .event_type = TYPE_GPIO_EDGE, .open = gpio_open, \
      .command = gpio_command, .close = gpio_close, .can_event = gpio_can_event, .hw = (hal), \
      .event = gpio_event }
```

The name comes from `runtime/vm/port_names.h`, the list of every runtime
driver's name (section 22.12).

### Open

`gpio_open` gets `gpio.Config`'s four fields as `cfg[0..3]`. It checks
them, asks the hardware layer to configure the pin, and keeps what it
needs in the port's state words:

```c
int gpio_open(struct Vm *vm, Port *p, uint16_t index, const int32_t *cfg, uint32_t n) {
    const GpioHal *h = hal_of(p);
    if (!h || n != 4) return -1;
    int32_t pin = cfg[0], mode = cfg[1], edge = cfg[2];
    if (pin < 0 || (uint32_t)pin >= h->npins || mode < GPIO_INPUT || mode > GPIO_OUTPUT ||
        edge < GPIO_EDGE_NONE || edge > GPIO_BOTH) return -1;
    GpioPin *s = &h->pins[pin];
    if (reserved(h, (uint32_t)pin) || s->open) return -1;
    bool watch = watched((uint32_t)mode, (uint32_t)edge);
    uint16_t deb = (uint16_t)(cfg[3] > 0 ? (cfg[3] > 65535 ? 65535 : cfg[3]) : 20);
    if (h->config(h->ctx, (uint32_t)pin, mode, watch, &deb) != 0) return -1;
    /* A debounced input needs the driver's event source in this VM (it
     * may hold a value): refused with TooMany if no source slot is free. */
    if (watch && deb && !src_slot(vm) && !port_add_source(vm, &gpio_source, NULL)) {
        h->release(h->ctx, (uint32_t)pin, mode, watch);
        return GOGO_E_TOOMANY;
    }
    p->state[0] = (uint32_t)pin;
    p->state[1] = (uint32_t)mode << 4 | (uint32_t)edge << 6 | (uint32_t)deb << 16;   /* deb: gpio_event */
    s->vm = vm;
    s->index = index;
    s->gen = p->gen;
    s->debounce_ms = h->raw_edges ? 0 : deb;   /* the bounce filter's window */
    s->mode = (uint8_t)mode;
    s->edge = (uint8_t)edge;
    s->level = (uint8_t)(h->read(h->ctx, (uint32_t)pin) != 0);
    s->settled = 1;
    vm_barrier();                 /* the slot is whole before gpio_edge sees it open */
    s->open = 1;
    return 0;
}
```

Three things to notice. Every refusal but one is `return -1`, which
`port.Open` reports as `errors.Busy` (section 22.6): a pin out of range,
a reserved pin, a pin already open; a debounced input that finds no free
event-source slot returns `GOGO_E_TOOMANY` (the debounce, below). The
port's state words keep the pin, mode and edge (`PIN(p)`, `MODE(p)`,
`EDGE(p)` read them back) and the debounce window (below). And the
per-pin slot `s` is shared with the interrupt handler, so the driver
fills it completely, then publishes it with `vm_barrier()` before
setting `open`.

### Command and close

```c
void gpio_command(struct Vm *vm, Port *p, uint32_t type, const int32_t *f, uint32_t n) {
    (void)vm;
    const GpioHal *h = hal_of(p);
    if (type != TYPE_GPIO_WRITE || n != 1 || MODE(p) != GPIO_OUTPUT) return;
    h->write(h->ctx, PIN(p), f[0] != 0);
}
```

```c
void gpio_close(struct Vm *vm, Port *p) {
    const GpioHal *h = hal_of(p);
    uint32_t pin = PIN(p);
    GpioPin *s = &h->pins[pin];
    s->open = 0;
    vm_barrier();
    h->release(h->ctx, pin, (int)MODE(p), watched(MODE(p), EDGE(p)));
```

```c
bool gpio_can_event(const struct Vm *vm, const Port *p) {
    (void)vm;
    const GpioHal *h = hal_of(p);
    return watched(MODE(p), EDGE(p)) && (!h->live || h->live(h->ctx));
}
```

A command of the wrong type, or a `Write` to an input pin, is ignored: a
driver never crashes the sender. `gpio_close` goes on with the debounce's
bookkeeping (below): a value held for the port is dropped. `gpio_can_event` is true for a watched
input while the hardware can still produce edges, so a program waiting
for a button press keeps the VM alive.

### Events from an interrupt

The board's interrupt handler reads the pin and calls `gpio_edge`, which
pushes a `PortEvent` to the event ring of the VM that opened the pin:

```c
bool gpio_edge(const GpioHal *hal, uint32_t pin, int level, uint32_t now_ms) {
    if (pin >= hal->npins) return false;
    GpioPin *s = &hal->pins[pin];
    if (!s->open || s->mode == GPIO_OUTPUT) return false;
    uint8_t lv = level != 0;
    if (lv == s->level) return false;
    s->level = lv;
    uint32_t since = s->settled ? UINT32_MAX : now_ms - s->changed_ms;
    s->settled = 0;
    s->changed_ms = now_ms;
    if (!(s->edge == GPIO_BOTH || s->edge == (lv ? GPIO_RISING : GPIO_FALLING))) return false;
    if (s->edge != GPIO_BOTH && since < s->debounce_ms) return false;
    PortEvent e;
    e.port = s->index;
    e.gen = s->gen;
    e.a = (int32_t)pin;
    e.b = lv;
    e.time_ms = now_ms;
    evq_push(s->vm, &e);
    return true;
}
```

It never locks and never allocates, so it is safe in an interrupt handler.
The event names the port by index and generation: if the port was closed
in the meantime, the VM finds a stale generation and drops the event. The
scheduler turns each event into the driver's `event_type` struct with the
two ints `{a, b}`, here `gpio.Edge{Pin: a, Value: b}`, and queues it on the
owner; but `gpio` has an `event` hook, so the scheduler hands it each
event first.

### Debounce

A button bounces: one press can be a burst of edges within a few
milliseconds. `gpio_edge` already drops, on a single-edge pin, an edge
that follows the last level change too closely. The rest is done on the
VM thread, in the driver's `event` hook, per port, with the `DebounceMs`
of its config: an edge that comes at least that long after the last
delivery is delivered at once; one inside the window is held (the latest
wins), and when the window ends the held level is delivered if it
differs from the last one delivered:

```c
void gpio_event(struct Vm *vm, Port *p, const PortEvent *e, uint64_t now) {
    uint32_t deb = DEB(p);
    if (!deb) {
        port_deliver(vm, p, e->a, e->b);
        return;
    }
    uint64_t t = event_time(e->time_ms - (uint32_t)vm->t0, now);
    flush_due(vm, p, t);
    if (!(p->state[1] & DB_DELIVERED) || t >= last_t(p) + deb) {
        deliver_at(vm, p, e->b, t);
        return;
    }
```

`port_deliver` is what the scheduler does for a driver without the hook.
The end of a window is a deadline, so the driver registers an event
source (section 22.11) when a VM first opens a debounced input: its
`next_deadline` is the earliest window end, its `pending` is true once
that time has come, and its `drain` delivers what is due. The source's
context is the number of ports holding a value (no memory to allocate or
free), so a VM with nothing held pays one test per scheduler pass, and
the source stays registered for the VM's life. If every source slot is
taken, `gpio_open` refuses the debounced input with
`GOGO_E_TOOMANY` (`port.Open` fails with `errors.TooMany`) rather than
open a pin whose debounce could not work; an output, an input without an
edge or one whose debounce the hardware layer lowered to 0 needs no
slot.

### Two hardware layers

On the ESP32-S3 (`platform/esp32s3/main/gpio_esp.c`) the interrupt handler
is four lines, and the board's instance is one:

```c
static void gpio_isr(void *arg) {
    uint32_t pin = (uint32_t)(uintptr_t)arg;
    int level = gpio_get_level((gpio_num_t)pin);
    if (gpio_edge(&gpio_esp_hal, pin, level, (uint32_t)platform_esp_ms())) platform_notify();
}
```

```c
const PortDriver gpio_esp_driver = GPIO_DRIVER(&gpio_esp_hal);
```

`platform_notify` wakes the VM task (the latched wake: no wakeup is lost,
section 18.4). On `gogovm` (`platform/unix/gpio_unix.c`) the "interrupt" is
a line `12 0` in the stdin script, which the input reader turns into the
same call:

```c
void gpio_unix_set_level(int pin, int level) {
    if (gpio_edge(&gpio_unix_hal, (uint32_t)pin, level, (uint32_t)platform_now_ms())) platform_notify();
}
```

So the simulator runs the very driver the board runs; only the hardware
layer differs. That is the pattern to copy for a new device: the logic
once in `runtime/`, a small table of hardware hooks per board (design
section 5.11.1).

## 22.11 Events and memory

There are three ways for a driver to send its owner a message. They
differ in where they may be called from and in what happens when the
owner's mailbox is full.

**The event ring** (`evq_push`, `runtime/vm/evq.h`), as `gpio` does. It is
the only way in from an interrupt handler. Its events are two ints, made
into the driver's `event_type` struct (which must therefore be a struct
of two ints), unless the driver takes them itself through its `event`
hook (as `gpio` does to debounce, section 22.10). The ring holds
`GOGO_EVQ_CAP` (64) events per VM; when it is full, new events are
dropped and counted. The ring has **one writer**: on the ESP32-S3 every
GPIO interrupt runs on the VM's core, so they never overlap. A second
interrupt that could preempt them, or run on the other core, must not push
to the same ring. Be aware that a ring event is queued like an ordinary
message: one that would take the owner past its memory limit ends the
owner with `mailbox_overflow` (chapter 6).

**Direct delivery** (`port_deliver_ints`, `runtime/vm/port.h`), on the VM
thread only: from a callback, or from an event source's drain (below). It
builds a struct of up to 8 fields of any reserved type, ints or immediate
terms (a PID, a bool, the empty `Bytes`), and queues it. It never kills
anyone: when the owner is gone, out of memory or over its quota, it
returns false and the message is not sent. The `power` driver delivers a
`power.Battery` this way and simply tries again at the next sample:

```c
    int32_t v[4] = { (int32_t)avg, pct, ch, ext };
    if (!port_deliver_ints(vm, w->port, TYPE_POWER_BATTERY, v, 4, 0)) {
        dropped_inc(vm);                            /* not reported: the next sample tries again */
```

**An event source** (`PortSource`, `port_add_source`) is how a driver
polls. It is not a delivery path of its own but the place to call
`port_deliver_ints` or the stream helpers from: a set of callbacks the
scheduler asks "is there work now?" (`pending`), "do it" (`drain`), and
"when is the next deadline?" (`next_deadline`). The scheduler never
sleeps while a source has work pending, so `pending` must turn false once
`drain` has done what it can; work that waits for time belongs in the
deadline. The keyboard driver is the pattern for interrupt-driven hardware
that must be read over a bus: its interrupt only sets a flag, and the
source reads the chip on the VM thread when the flag is up. The input
drivers (`kbd`, `power`, `imu`, `ir`, `mic`, `speaker`) share one source,
`runtime/input/source.c`.

### Charging the owner

Memory a driver keeps for a port is the owner's, so it counts against the
owner's limit like its heap does:

- **`p->charge_words`**, set in `open`: the words of state the driver keeps
  outside the `Port` for this port (a TCP control block, say). The VM
  charges them to the owner after `open` returns 0; if they do not fit,
  it calls the driver's `close` and `port.Open` fails with
  `errors.NoMemory`. The charge moves with `port.Handoff` and is given back
  at close.
- **`p->room_64w`**, set in `open`: how much more the owner must have free
  (in 64-word units) for the driver's largest event, checked once at open
  and not charged. A port whose every event would be over the owner's
  quota is refused at once instead of dropping everything silently.
- **`port_bytes_new` / `port_bytes_retain`**: off-heap `Bytes` the port
  holds for its owner (at most 2), charged to the owner, released at close,
  moved by `Handoff`.

A driver does not `malloc` per event. Event messages are allocated by the
VM and charged to the receiver when queued; a driver's own buffers are
fixed (static storage on the board, `Port.state`, or a charged table).

### Streams

A driver that moves bytes uses the stream protocol (section 7.6, and
`lib/port/port.gg`): `port.Chunk` events bring input, `port.Out` commands
send output, each `Out` is answered by one `port.Done`, and `port.Closed`
says the port ended by itself. `runtime/stream/stream.h` implements the
rules once, for every byte port (IMU, IR, audio, TCP, TLS, the screen):

```c
bool stream_deliver_chunk(struct Vm *vm, const Port *p, StreamIn *s, uint32_t time_ms,
                          const uint8_t *data, uint32_t len);
```

```c
int stream_out_accept(struct Vm *vm, const struct Actor *from, Port *p, StreamOut *o,
                      term msg, StreamCheck check);
```

```c
bool stream_out_done(struct Vm *vm, Port *p, StreamOut *o);
```

`stream_deliver_chunk` copies the data into a `Chunk` and queues it,
dropping and counting it (in the next `Chunk`'s `Dropped`) when the owner
has no room. A driver's `command_term` hands each message to
`stream_out_accept`, which keeps at most two `Out`s in flight (the data
pinned for the port) and answers a third with `Done{Seq: -1}`; when the
hardware has finished the head, the driver calls `stream_out_done` (or
`stream_out_fail`), and a `Done` the owner has no room for is owed and
sent later by `stream_out_flush`. A `StreamOut` is 16 bytes: it fits
`Port.state` exactly.

`Closed{From, Reason}` is sent with `port_deliver_ints`, the reason a
shared error code (`runtime/include/gogo_errors.h`, the same numbers as
`gogo/errors`), as the TCP driver does:

```c
static bool post(Vm *vm, const Port *p, uint32_t type, int32_t reason) {
    int32_t v[2] = { (int32_t)p->id, reason };
    return port_deliver_ints(vm, p, type, v, type == TYPE_CLOSED ? 2u : 0u, PORT_RAW(0));
}
```

`PORT_RAW(0)` says that field 0 is a term (the port's PID, `p->id`), not an
int. Reason 0 (`errors.OK`) is reserved for a stream's half-close ("the
peer finished sending"); a port that is finished for good sends a
positive code such as `GOGO_E_IO`, and does nothing more.

## 22.12 Requirement names and optional features

The driver's `name` is the requirement name. The compiler records it for
every constant `port.Open("name", ...)` in the program and its packages,
and the loader refuses an image whose names the VM does not have; a
driver opened by a name computed at run time requires nothing (the build
warns), and a name listed in a package's `//gogo:optional` directive is
left out for programs that handle its absence. There is no separate
capability table: a board's capabilities are exactly the drivers in its
list. So adding a driver to the tree means:

- **The name** goes in `runtime/vm/port_names.h` as a `PORT_NAME_*` macro,
  and the driver uses the macro. `tests/portnames_test.go` fails `make
  test` for a driver name spelled as a literal in `runtime/` or
  `platform/`, and for a `PORT_NAME_*` that no `gogo/...` package opens
  and no firmware feature provides. The list is also what the embedding
  layer refuses as host port names, so no host can shadow a runtime
  driver.
- **The board lists it.** On the ESP32-S3, `esp_ports` in
  `platform/esp32s3/main/platform_esp.c` lists the drivers a build has,
  each behind its switches:

  ```c
  static const struct PortDriver *const esp_ports[] = {
      &gpio_esp_driver,
  #if CONFIG_GOGO_NET
      &net_netif_driver, &net_udp_driver, &net_tcp_driver, &net_tcp_listen_driver,
  #endif
  ```

  On `gogovm`, `unix_ports_all` in `platform/unix/main.c` lists them, and
  `gogovm --without NAME,...` (for tests) leaves names out to model a
  firmware without them.
- **A compile-time switch**, to make the driver optional like the existing
  features (design section 5.4.5): a `config GOGO_X` in
  `platform/esp32s3/main/Kconfig.projbuild`, `#if CONFIG_GOGO_X` around its
  entry in `esp_ports` (and `HAS_X` from `board.h` if only some boards
  have the hardware), its sources added conditionally in
  `platform/esp32s3/main/CMakeLists.txt`, and a row in `fwFeatures` in
  `cmd/gogo/firmware.go`, so that `gogo firmware` turns the feature on when
  an image requires the name:

  ```go
  	{name: "power", kconf: "CONFIG_GOGO_BATTERY", provides: []string{"power"}},
  ```

  `TestFirmwareTableMatchesSources` checks that table against `board.h` and
  `Kconfig.projbuild`. A switched-off driver is simply not in the list:
  `port.Open` returns `errors.NotFound`, and an image that requires it is
  refused, exactly as on a board without the hardware.

`gogo firmware` knows the names in its table, and those that a
`-component`'s `gogo-driver.json` provides (section 22.4): for any other
name it stops with "the images require "x", which no gogo firmware
provides".

## 22.13 Simulating the driver on the host VM

A driver that exists only on the board can be tested only on the board.
The existing devices avoid that by having a second hardware layer in
`platform/unix`, so `gogovm` runs the same driver code on a computer, fed
by a script on stdin (the `input` file next to a book example):

- `gpio`: `gpio_unix.c`, script lines `<pin> <0|1>`, and `gogo run --key
  c=pin` in a terminal;
- `kbd`, `power`, `imu`, `ir_rx`, `mic`: `input_unix.c`, lines such as `key
  enter`, `battery 3900 1 1`, `imu gesture tap`, `ir rx 9000 4500 ...` and
  `mic tone 440 200`;
- `ir_tx` and `speaker` print what they would send on stderr and stay busy
  for as long as the real hardware would.

`input_unix.c` is the only reader of stdin, so a new device adds its script
lines there and its hardware layer next to it (`platform/unix/x_unix.c`),
lists its instance in `unix_ports_all`, and adds the file to the Makefile's
`gogovm` sources. Then a golden test or a book example can run programs
that use it under `make test`, and `--without x` covers the "board without
it" case.

## 22.14 The gogo side: types

The `gpio` driver needs three struct types, and the VM must know them by
number: it checks a config's type, decodes commands by type and builds
`gpio.Edge` events itself. Such types are **reserved**: each has a fixed
index in every image (`//gogo:reserved 6` in the `.gg` source, `TYPE_GPIO_CONFIG`
in C). A new driver with types of its own needs:

- a system package in `lib/` with the types, each marked
  `//gogo:reserved N` (only `gogo/...` packages may reserve types), with a
  row in `lib/README.md`;
- the types appended to `reservedTypes` in `compiler/codegen.go` (and
  `reservedName`, and the package path in `compiler/load.go`);
- `TYPE_*` constants, `TYPE_RESERVED_COUNT` and `TYPE_NFIELDS` in
  `runtime/vm/opcodes.h`, which `compiler/asm_test.go` checks against the
  compiler's table;
- a new **image format minor**: reserved types are append-only and a new
  one is a format change (`IMAGE_MINOR` in `runtime/vm/image.h`, the type in
  `typeMinor` in `compiler/image.go`), so an image that uses the new types
  runs only on firmware of that format or later.

That is a lot of the VM to touch for one peripheral, and a firmware
update for every user. **A driver can avoid all of it** by using only the
generic types that already exist in `gogo/port`:

```go
//gogo:reserved 42
type Host struct {
	Arg  int
	Data bytes.Bytes
}
```

```go
//gogo:reserved 43
type Post struct {
	From actor.PID
	Kind int
	Data bytes.Bytes
}
```

They are what every `gogo.h` driver speaks (sections 22.2 to 22.8), and
an in-tree `PortDriver` may use them too: `cfg_type = TYPE_HOST`,
`port.Post` as its commands and events, and `port.Chunk` / `Out` / `Done`
/ `Closed` if it moves bytes. Such a driver needs no change to the
compiler, the image format or `lib/`, and its images run on any firmware
of the current format that has the driver. The price is that the gogo
side reads `e.Kind` instead of a field with a name; a small gogo package
that wraps the port can give it names again.

## 22.15 Checklist: the files a new driver touches

For a driver in the tree, in the order you write them:

1. **The driver**, chip-independent C99 in `runtime/<name>/` (or
   `runtime/input/` for an input device): the callbacks, a hardware-layer
   table if more than one board will have it, and a `*_DRIVER(hal)` macro.
   No mutable globals (`make globals`).
2. **The name**: `PORT_NAME_*` in `runtime/vm/port_names.h`.
3. **The types**: generic `gogo/port` types (nothing to do), or a `lib/`
   package and the reserved-type changes of section 22.14 with a format minor.
4. **The ESP32-S3**: the hardware layer in `platform/esp32s3/main/` (with
   its board storage), the instance in `esp_ports`, the sources in
   `main/CMakeLists.txt`, and for an optional feature `Kconfig.projbuild`,
   `board.h` (`HAS_*`) and `fwFeatures` in `cmd/gogo/firmware.go`.
5. **The host VM**: a hardware layer in `platform/unix/`, script lines in
   `input_unix.c`, the instance in `unix_ports_all` in `platform/unix/main.c`,
   the sources in the `Makefile`.
6. **Tests**: C tests of the driver over a fake hardware layer (as
   `runtime/gpio/test/test_gpio.c` and `runtime/input/test/` do), and a
   golden test or example run by `gogovm` with an `input` script.
7. **Documentation**: the book (a chapter or section), `docs/design.md`,
   `docs/FAQ.md` and `docs/known-issues.md` where they apply, and the
   requirement names in chapter 13.

## 22.16 Pitfalls

- **Never block in a callback.** A callback runs on the one thread that
  runs every actor; while it waits, nothing else runs. Start slow work and
  return: let DMA or an interrupt do it, or a task of the platform's (the
  ESP's Wi-Fi and LCD tasks), and deliver the result later from an event
  source or the event ring. If the hardware must be polled, poll from a
  source with a deadline, not in a loop. A few operations still block
  today (storage calls, the TLS key exchange), and
  `docs/known-issues.md` lists them as a cost every actor pays.
- **Know your thread.** Driver callbacks and source callbacks run on the VM
  thread. From an interrupt handler or another thread, only two of the
  VM's functions are safe: `evq_push` (one writer per VM) and the host's
  wake (`platform_notify`, `vm_wake`). Everything else, `port_deliver_ints`, the
  stream helpers, `port_bytes_*`, reading `Port` fields, belongs on the VM
  thread. State shared with an interrupt handler is filled first and
  published last, with `vm_barrier()` between, as `gpio_open` and
  `gpio_close` do; the handler must not run while `close` releases the
  hardware (`runtime/gpio/gpio.h` explains the two-core case).
- **A source must make progress.** The scheduler never sleeps while a
  source's `pending` is true. A `pending` that stays true while `drain`
  does nothing (a stuck hardware flag, an error retried at once) spins the
  CPU at 100%: count those passes and back off, as `kbd` does.
- **Charge the owner, do not allocate per event.** Memory the driver keeps
  for a port goes in `charge_words` or held `Bytes`; event messages come
  from the VM. Prefer delivery that drops (`port_deliver_ints`, the stream
  helpers) over the event ring when events can come faster than an actor
  reads them, or thin them out in an `event` hook (as `gpio`'s debounce
  does): a flood of ring events ends a slow owner with `mailbox_overflow`.
- **Keep state per VM.** Several VMs can run in one process (chapter 18), so
  `runtime/` has no mutable globals; `make globals` (`tools/globals.sh`)
  fails on any new one. Per-port state goes in `Port.state` or behind the
  hardware layer's `ctx`; state that must outlive a port is board storage,
  declared by the platform and passed in through the hardware layer, like
  `gpio`'s `GpioPin pins[]`. Do not size a table by port index for 16
  ports: the cap is per VM, 1024 by default on `gogovm` and in embedded
  VMs, up to 2048 (section 22.9). Index such state by the device (a pin,
  a channel), as `gpio` does, or keep it in the `Port`.
- **Validate everything.** A command can come from any actor and any
  module, with any type and any values. Check the type, the field count
  and the ranges, and ignore what you do not understand.

## 22.17 What is still missing

The driver API of sections 22.2 to 22.8 closes most of what used to
stand between a third party and a firmware driver: there is a stable
header, a registration that needs no fork, `gogo firmware` builds the
component in, and a refusal names its error. What remains, plainly:

- **The API is still experimental.** The driver part of `gogo.h` keeps
  the `GOGO_EXPERIMENTAL` mark until v0.3 (section 22.8).
- **No byte streams.** A `gogo.h` driver has `port.Post` events and
  commands of up to 262,080 bytes each, but not the stream protocol of
  the runtime's byte ports (`port.Chunk`, `Out`, `Done` with flow
  control, section 22.11); a helper for it can follow.
- **No interrupt-safe post.** An interrupt handler hands its data to a
  task, which posts (section 22.7). The event ring, which an ISR can
  push to, is the runtime's own and holds two-int events only.
- **A few ports per board.** A board registers at most 8 board ports; a
  VM has `GOGO_MAX_SOURCES` event sources, one more of them taken by the
  post inbox in a firmware with board ports.
- **No hardware access through gogo.** A component's driver talks to
  its hardware through ESP-IDF directly (its own I2C device, GPIO, task);
  the runtime's shared I2C bus and pin reservations are internal. Pick
  pins and buses the board's own drivers do not use.
- **C only, compiled in.** No driver is loaded at run time (no FFI and
  no native modules, by design), and there is no package manager for
  components.
