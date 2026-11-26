# snake: the classic game, with an autopilot actor

Snake on the text screen (`gogo/tui`), for the M5Stack Cardputer ADV. It
also runs in a terminal on the host. The field fills the 40 x 11 grid except
the bottom row, which shows the mode (`AUTO` or `MANUAL`), the score and the
high score. The snake speeds up as it grows (180 ms a step, 4 ms less per
food, down to 70 ms). The game starts in auto mode, so a board that nobody
touches is a live demo.

## Controls

| Key | What it does |
|---|---|
| `;` `.` `,` `/` (with or without Fn), arrows, WASD | steer up, down, left, right; switches to manual |
| `m` | auto / manual |
| `p` | pause |
| Enter or Space | new game after game over (auto mode starts one by itself after 2 s) |
| `k` | kill the autopilot actor, to watch it come back |
| Esc (Fn+`` ` ``) | quit |

The high score follows the score live and is stored with `gogo/kv` in
`/flash/kv/snake.hi` at game over and on Esc. Without
a flash file system (`gogo run` without `--fs-root`) it lasts for the run.

## Packages

```text
examples/snake/
  main.gg                 package main: the screen, the keys, the timer, supervision
  sim.gg                  package main: the headless autopilot check (see Checking the autopilot)
  board/board.gg          snake/board: the game as values, and the View and Move messages
  autopilot/autopilot.gg  snake/autopilot: the autopilot actor and its search
```

The imports start with `snake/`, the name of the build root (book,
section 3.1.1). `board` knows nothing about screens or actors other than
the PID in a `View`; `autopilot` imports only `board`.

## Two actors

- **The game** (`main.gg`) owns the screen and the keyboard. A timer
  message moves the snake one cell (`board.Step`) and redraws the screen;
  the screen port draws only the rows that changed.
- **The autopilot** (`autopilot.Run`) is a separate actor. After every
  step the game sends it a `board.View` (the body, the food, the direction,
  the steps since the last meal and a step number); it answers with a
  `board.Move`. In auto mode a step waits for that answer (at most 1 s), so
  a slow autopilot slows the snake down instead of steering it blindly; a
  `Move` for an old step is dropped.

The autopilot's searches are breadth first and know that the body moves:
body cell *i* (0 is the tail) is free after *i*+1 steps. It takes the
shortest path to the food if, after eating, the snake could still reach its
tail or has more room than its length. Otherwise it makes the move that
keeps the tail in reach by the longest way round, which stretches the snake
out instead of circling in a knot; failing that, the move with the most
room. As a last guard, once the snake has been hungry for longer than the
`Patience` in the `View` (the game sends 2 x 400 steps) it takes the food
path whatever the risk. `autopilot.Choose` also says which rule picked the
move, which the test counts.

## Supervision

The game monitors the autopilot (`actor.Monitor`). When the autopilot dies
in auto mode, the game gets an `actor.DownMsg`, restarts it and shows
`pilot restarted`; in manual mode it lets it go (`pilot stopped`), and `m`
starts a new one.
After more than 3 deaths within 10 s it gives up, like a supervisor's
restart intensity, and switches to manual (`pilot down: manual`); `m`
starts a fresh autopilot. The autopilot keeps no state of its own, so a
restart loses nothing. Press `k` to try it: the game keeps running
whatever happens to the autopilot.

The game prints a line on the serial console at every game over (score,
high score, frames sent, frames the screen port dropped and the game
actor's memory) and when the autopilot
is restarted. While the screen is open, only `gogo monitor` shows them.

## The autopilot on another machine

With `gogo/node` the autopilot can run on a server node (the host) while
the game stays on the board: the same image plays both parts, chosen by
configuration keys.

- The **server** is a host VM with real sockets (`GOGO_NET=host`, or
  `gogovm --host-net`) and `snake.serve` set to a TCP port. It joins no
  game: it registers an autopilot as `autopilot` on the node
  `snake-server`, restarts it when it dies, and serves every board.
- The **board** has `snake.server` set to the server's `a.b.c.d:port`. A
  joiner actor gets an address (`gogo/dhcp`), connects as the node `snake-board`, finds the server's
  `autopilot` and hands it to the game, which then sends it its `View`s and
  gets `Move`s back over the network: one request and one reply per step.
  The status row says `AUTO NET`. `board.View` and `board.Move` cross the
  network as they are: both ends compile the same `snake/board`, and a
  struct goes from node to node by its qualified name and field count.
- Both need the same `node.token`.

When the server or the link goes, the game's monitor on the remote
autopilot fires (`actor.DownMsg`, reason `actor.NoConnection`): a local
autopilot takes over at once (`server lost: local`; not counted as a
crash) and the joiner tries the server again every 3 s. When it is back,
the server's autopilot takes over again (`pilot on server`). `k` does
not kill a remote autopilot (`actor.Kill` of a remote PID does nothing).

```sh
printf 'snake.serve=4370\nnode.token=<a long random string>\n' > /tmp/snake-server.cfg
GOGO_NET=host GOGO_CONFIG=/tmp/snake-server.cfg build/gogo run examples/snake

build/gogo config set snake.server 192.168.1.10:4370    # the host's address
build/gogo config set node.token '<the same string>'
build/gogo flash examples/snake
```

## Checking the autopilot

This is a demo, so it is not part of `make test`. With the config key
`snake.sim` (steps) set, and optionally `snake.seeds`, the program runs the
autopilot without a screen or actors (`sim.gg`) and prints, per seed, the
games, the food, the best score, the longest wait for food and how often
each of the autopilot's rules picked the move:

- on the 40 x 10 field, for each seed, the snake must never go 400 steps
  without food (`stuck` counts the seeds that did);
- on a 12 x 6 field, which fills up, the starvation fallback must fire and
  keep the waits bounded.

A snake circling in a knot shows only after thousands of steps:

```sh
printf 'snake.sim=20000\nsnake.seeds=20\n' > /tmp/snake.cfg
build/gogo build examples/snake -o snake.ggb
GOGO_CONFIG=/tmp/snake.cfg build/gogovm snake.ggb    # about 100 s
```

## Build and run

```sh
go build -o build/gogo ./cmd/gogo
build/gogo run examples/snake                     # in a terminal on the host
build/gogo run --fs-root /tmp/snake examples/snake  # keeps the high score

build/gogo build examples/snake -o snake.ggb
build/gogo flash snake.ggb                        # the Cardputer ADV
build/gogo monitor -timeout 60
```

The image needs a firmware with the `screen`, `netif` and `udp` drivers
(`gogo/dhcp`, for the server mode) and image format 15.2 or later (for
`gogo/node`, whose own drivers are optional). On a board without a keyboard (the StickS3) it runs the demo in
auto mode; button A is Enter and B is down.
