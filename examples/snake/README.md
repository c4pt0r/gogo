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

The high score is stored with `gogo/kv` in `/flash/kv/snake.hi`. Without
a flash file system (`gogo run` without `--fs-root`) it lasts for the run.

## Packages

```text
examples/snake/
  main.gg                 package main: the screen, the keys, the timer, supervision
  sim.gg                  package main: the headless autopilot check (see Test)
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
room. As a last guard, after 2 x 400 steps without food it takes the food
path whatever the risk.

## Supervision

The game monitors the autopilot (`actor.Monitor`). When the autopilot dies,
the game gets an `actor.DownMsg`, restarts it and shows `pilot restarted`.
After more than 3 deaths within 10 s it gives up, like a supervisor's
restart intensity, and switches to manual (`pilot down: manual`); `m`
starts a fresh autopilot. The autopilot keeps no state of its own, so a
restart loses nothing. Press `k` to try it: the game keeps running
whatever happens to the autopilot.

The game prints a line on the serial console at every game over (score,
high score, frames sent and the game actor's memory) and when the autopilot
is restarted. While the screen is open, only `gogo monitor` shows them.

## Test

With the config key `snake.sim` (steps) set, and optionally `snake.seeds`,
the program plays that many autopilot steps for each seed without a screen
or actors and prints, per seed, the games, the food, the best score and the
longest wait for food. The golden test (`make test-go`) runs it with the
`config` file of this directory and compares the output with
`expected.out`; `ok false` would mean the snake went a whole field (400
steps) without eating.

## Build and run

```sh
go build -o build/gogo ./cmd/gogo
build/gogo run examples/snake                     # in a terminal on the host
build/gogo run --fs-root /tmp/snake examples/snake  # keeps the high score

build/gogo build examples/snake -o snake.ggb
build/gogo flash snake.ggb                        # the Cardputer ADV
build/gogo monitor -t 60
```

The image needs a firmware with the `screen` driver (image format 15.1 or
later). On a board without a keyboard (the StickS3) it runs the demo in
auto mode; button A is Enter and B is down.
