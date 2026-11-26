# 19. Text UI

Both boards have a small colour LCD. So far it has only shown the console:
whatever a program prints scrolls up, in white (red for errors). This
chapter turns the display into a real user interface: a grid of character
cells you can draw anywhere on, in 16 colours, with menus, progress bars and
a status line. It works like `ncurses` on a terminal, and the same program
runs on the Cardputer, on the StickS3 and on the host, where it draws in
your terminal.

There are two layers:

- The **`"screen"` port** (package `gogo/screen`) owns the grid. A program
  sends it frames of compact drawing operations, and it draws on the LCD
  the rows that changed.
- The **`gogo/tui` library**, written in gogo on top of the port, gives you
  frames and windows, a few widgets, and keys that work on both boards.

Most programs only need `gogo/tui`.

## 19.1 A first screen

```go
package main

import (
	"gogo/screen"
	"gogo/tui"
)

func main() {
	s, ok := tui.Open(screen.Config{Cols: 24, Rows: 5})
	if !ok {
		println("no screen")
		return
	}
	f := tui.NewFrame(s)
	tui.Box(f, 0, 0, 24, 5, "Hello")
	tui.Color(f, screen.BrightYellow, screen.Black)
	tui.PrintAt(f, 2, 1, "gogo on a screen")
	tui.Color(f, screen.BrightGreen, screen.Black)
	tui.DrawProgress(f, 2, 2, 15, 2, 3)
	tui.Color(f, screen.White, screen.Black)
	tui.PrintAt(f, 18, 2, tui.Percent(2, 3))
	tui.DrawStatus(tui.Window(f, 1, 1, 22, 3), "status", "ok")
	tui.Flush(s)
	tui.Close(s)
	println("back to the console")
}
```

`tui.Open` takes the display. `screen.Config{}` asks for the whole display:
40 columns by 11 rows on both boards (6 x 12 pixel cells, the console's
font), and the same by default on the host. This example asks for a
smaller grid so that its output is short. `s.Cols` and `s.Rows` give the
size you got. While the screen is open, the console is hidden. It still
takes everything the program prints, and it comes back, whole, when the
screen closes (`tui.Close`, or the owner's exit).

Drawing goes into a **frame**. `tui.NewFrame(s)` starts one over the whole
screen. Every drawing call adds a few bytes of operations to the screen's
buffer, and nothing reaches the display until `tui.Flush(s)` sends them as
one message. The port then draws the whole frame at once, so a half-drawn
frame is never seen.

On the host, when standard output is not a terminal (as in the book's
tests), the VM prints the grid after every frame instead of drawing it:

```text
screen 1 24x5
+------------------------+
|┌ Hello ───────────────┐|
|│ gogo on a screen     │| ..bbbbbbbbbbbbbbbb
|│ ██████████░░░░░ 66%  │| ..aaaaaaaaaaaaaaa
|│status              ok│| .RRRRRRRRRRRRRRRRRRRRRR
|└──────────────────────┘|
+------------------------+
back to the console
```

The characters after a row show the style of each of its cells:

| Character | Style of the cell |
|---|---|
| `.` | default (White on Black) |
| `R` | reverse |
| `U` | underline |
| `B` | bold |
| `#` | a background other than Black |
| a hex digit | the foreground colour (`b` is BrightYellow) |

In a terminal, `gogo run` draws the grid in place, with colours, on the
terminal's alternate screen. `build/gogovm --screen ansi|text|none`
chooses explicitly.

## 19.2 Cells, colours and the drawing calls

Each cell holds one character, a foreground colour, a background colour and
three attributes:

- **Colours:** `screen.Black`, `Red`, `Green`, `Yellow`, `Blue`, `Magenta`,
  `Cyan`, `White`, then their bright versions `Gray`, `BrightRed` ... up
  to `BrightWhite` (0 to 15). On the LCD they are the VGA palette in
  RGB565; in a terminal they are the 16 ANSI colours.
- **Attributes:** `screen.Bold` (each glyph thickened by one pixel),
  `screen.Underline` (the bottom pixel row) and `screen.Reverse` (the two
  colours swapped).

A Chinese character, or any other wide character, takes two cells. The
box-drawing characters (`─ │ ┌ ┐ └ ┘ ├ ┤ ┬ ┴ ┼`) and the blocks
(`█ ░ ▒ ▓ ▀ ▄ ▌ ▐`) take one cell each. The LCD draws them as lines and
fills, so they work even with the ASCII-only font (`GOGO_FONT_CJK` off).

The calls, all on a `tui.Frame`:

| Call | What it does |
|---|---|
| `Clear(f)` | blanks the frame in the current colours |
| `Move(f, x, y)` | moves the cursor |
| `Print(f, s)` | draws `s` at the cursor, which moves on; text does not wrap |
| `PrintAt(f, x, y, s)` | `Move` then `Print` |
| `Fill(f, x, y, w, h, ch)` | fills a rectangle with one character |
| `Box(f, x, y, w, h, title)` | a frame of lines, with an optional title |
| `Color(f, fg, bg)`, `Attr(f, a)`, `Style(f, fg, bg, a)` | set the colours and attributes for what follows (they stay set across frames) |
| `Cursor(f, x, y, on)` | shows the cursor (a reversed cell) at `x, y`, or hides it |
| `Scroll(f, x, y, w, h, n)` | moves a rectangle up `n` rows (down if `n` is negative) |

Every frame starts with the cursor hidden. The frame remembers which
colours, attributes and clip the port already has, and only sends what
changes.

**Windows.** `tui.Window(f, x, y, w, h)` is a frame over a sub-rectangle.
It has its own origin: `0, 0` is its top-left cell. Everything drawn
through it is clipped to it, and to its parent. Drawing partly or wholly
outside it is fine: that part is dropped. In the example, the status line
belongs to a window inside the box, so it stays within the box's walls.
`tui.Size(w)` gives a window's width and height.

**Redraw everything.** The simplest way to keep a screen right is to redraw
the whole interface after every event: clear, then draw it all. That costs
little. The port compares each row at the end of a frame with what it was
before, and the LCD only redraws the rows that really changed.

## 19.3 Widgets

`gogo/tui` predates methods, so each widget is a plain struct and some
functions. The doc comments in `lib/tui` give the method form each will
take when the package moves to methods (`tui.ListKey(l, k)` becomes
`l.Key(k)`, and so on). A widget's key function returns the updated
struct, which is also how a method with a value receiver would return it.

- **`Label(f, x, y, w, text, align)`** draws text in `w` columns,
  `tui.Left`, `Center` or `Right`. It cuts the text to fit and pads it with
  spaces, so it also erases what was there before. `tui.Width(s)` counts
  columns (wide characters count 2) and `tui.Cut(s, w)` cuts a string to a
  width.
- **`Box(f, x, y, w, h, title)`** is listed above.
- **`List`** is a menu. `tui.NewList(items, rows)` shows `rows` items at a
  time. `tui.ListKey(l, k)` moves the selection with Up and Down, wrapping
  around at both ends, and returns the item chosen with Enter (-1 for
  none). `tui.DrawList(f, l, x, y, w)` draws the selected item in reverse
  video with a `>`, and puts `^` or `v` in the last column when more items
  are hidden above or below.
- **`DrawProgress(f, x, y, w, value, max)`** draws a progress bar of full
  blocks and light shade; `tui.Percent(value, max)` formats it as `"66%"`.
- **`DrawStatus(f, left, right)`** draws a status line on the frame's last
  row, in reverse video, with `left` at the start and `right` at the end.
- **`Edit`** is a one-line text editor for boards with a keyboard.
  `tui.NewEdit(text, max)` makes one. `tui.EditKey(e, k)` inserts
  characters and handles Backspace, Delete, Left and Right. It returns
  `tui.EditDone` on Enter and `tui.EditCancel` on Esc. `tui.DrawEdit(f, e,
  x, y, w, focus)` draws the text underlined, scrolled so that the cursor
  shows, and puts the screen's cursor at the edit position when `focus` is
  true. It returns the editor with its scroll position; keep that one.

## 19.4 Keys on both boards

The Cardputer has a keyboard; the StickS3 has two buttons. `tui.OpenKeys`
opens whichever the board has, and `tui.Event` turns their messages into
one kind of key, `tui.Key{Code, Char}`:

- **Keyboard:** `KeyUp`, `KeyDown`, `KeyLeft`, `KeyRight` (on the
  Cardputer Fn with `;` `.` `,` `/`), `KeyEnter`, `KeyEsc` (Fn+`` ` ``),
  `KeyTab`, `KeyBackspace`, `KeyDelete`, and `KeyChar` with the character
  in `Char` for the printable keys. Key releases are dropped.
- **Buttons** (`tui.Buttons{A: tui.ButtonA, B: tui.ButtonB}`: KEY1 on G11
  and KEY2 on G12):
  - pressing A is `KeyEnter`;
  - releasing B is `KeyDown`;
  - releasing B after holding it for `tui.LongPressMs` (600 ms) is `KeyEsc`.

  Since lists wrap around, Down alone reaches every item.

`gogo/tui` declares `kbd` optional, so importing it does not make an image
need a keyboard: one image runs on both boards. (Importing `gogo/kbd` for
its types alone does not require a keyboard either; a constant
`port.Open("kbd", ...)` in your own code still does.)

Nothing in `gogo/tui` waits for a message on its own. Your actor keeps its
receive loop and hands each message to `tui.Event(s, keys, msg)`, which
returns the updated key state, the key (`KeyNone` if the message meant
none) and whether the message was the UI's at all. A UI message is a key,
a button edge, or a `port.Done` answering one of the screen's frames.
Everything else is yours to handle: timers, network events, messages from
other actors. A UI never blocks the rest of a program.

Every `port.Done` names the port that sent it in `From`. `Event` takes
exactly the `Done`s whose `From` is the screen's port (`s.Port`) and hands
every other one back, so a program that also drives another stream port,
the speaker or a TCP connection, gets each of its own `Done`s, with its
`Seq`, untouched.

```go
func main() {
	s, ok := tui.Open(screen.Config{Cols: 14, Rows: 4})
	keys, kok := tui.OpenKeys(tui.Buttons{A: tui.ButtonA, B: tui.ButtonB})
	if !ok || !kok {
		println("open failed")
		return
	}
	items := []string{"Tea", "Coffee", "Water"}
	l := tui.NewList(items, 3)
	draw(s, l)
	for {
		msg := actor.Receive()
		k2, k, ui := tui.Event(s, keys, msg)
		keys = k2
		if !ui {
			continue // not the UI's: a real program handles it here
		}
		l2, chosen := tui.ListKey(l, k)
		l = l2
		if chosen >= 0 {
			println("chose", items[chosen])
			break
		}
		if k.Code != tui.KeyNone {
			draw(s, l)
		}
	}
	tui.CloseKeys(keys)
	tui.Close(s)
}

func draw(s tui.Screen, l tui.List) {
	f := tui.NewFrame(s)
	tui.DrawList(f, l, 0, 0, 14)
	tui.DrawStatus(f, "Enter: pick", "")
	tui.Flush(s)
}
```

With the input script `key down` and `key enter` it prints two frames, then
`chose Coffee`.

**`tui.Run`** is a ready-made loop for an actor that does nothing but its
UI. You give it three callbacks in a `tui.Handlers`:

- `Key` gets every key;
- `Msg` gets every other message (use `tui.Ignore` to drop them);
- `Draw` redraws the interface.

`Key` or `Msg` returns false to end the loop. `Run` draws and flushes
before the first message and after every call to `Key` or `Msg`. Closures
capture by value, so keep the state the callbacks change in a slice they
share: `st := []State{...}`, then `st[0] = next` inside a callback.
`examples/tuidemo` is built this way. It is a menu, a job with a progress
bar driven by a timer, a status line, a rename dialog and an About box, and
the same image runs on both boards.
`examples/snake` keeps its own receive loop instead: a game whose steps
come from a timer, with a second actor, the autopilot, that plays it.

## 19.5 The screen port underneath

`gogo/tui` is plain gogo over a port you can use directly:

```go
package main

import (
	"gogo/actor"
	"gogo/bytes"
	"gogo/port"
	"gogo/screen"
)

// waitDone waits up to a second for the port's Done and returns its Seq
// (0: none came). A Done can come late when memory is short: never wait
// for one without a timeout.
func waitDone() int {
	for {
		msg, ok := actor.ReceiveTimeout(1000)
		if !ok {
			return 0
		}
		switch m := msg.(type) {
		case port.Done:
			return m.Seq
		}
	}
}

func main() {
	p, err := port.Open("screen", screen.Config{Cols: 12, Rows: 2})
	if err != nil {
		println("no screen")
		return
	}
	switch m := actor.Receive().(type) {
	case port.Chunk: // the grid: cols, rows, colours, attributes
		println("grid", m.Data.At(0), "x", m.Data.At(1), "colours", m.Data.At(2))
	}
	b := bytes.NewBuffer(32)
	b.Write([]byte{screen.OpMove, 1, 0, screen.OpAttr, screen.Reverse, screen.OpText, 5})
	b.WriteString("hello")
	b.Write([]byte{screen.OpAttr, 0, screen.OpFill, 0, 1, 12, 1, 0x00, 0x25}) // ─ is U+2500
	actor.Send(p, port.Out{Data: b.Freeze()})
	println("done", waitDone())
	actor.Send(p, port.Out{Data: bytes.FromSlice([]byte{42})}) // not an operation
	println("done", waitDone())
	port.Close(p)
}
```

```text
grid 12 x 2 colours 16
screen 1 12x2
+------------+
| hello      | .RRRRR
|────────────|
+------------+
done 1
done -1
```

- **Opening.** `port.Open("screen", screen.Config{Cols, Rows})`. Zeros mean
  the whole display. A board refuses a grid larger than its display; the
  host takes up to 255 x 32. Opening also fails if another actor has the
  screen. The port's first message to its owner is a `port.Chunk` whose
  `Data` is the grid's columns, rows, colour count and attribute mask, one
  byte each. (`tui.Open` reads it in a helper actor, so your mailbox is not
  touched.)
- **Drawing.** Each `port.Out` is a frame: a sequence of operations, each an
  op byte followed by its arguments, coordinates in cells:

  | Operation | Arguments | What it does |
  |---|---|---|
  | `OpClear` | | blanks the clip rectangle |
  | `OpMove` | `x y` | moves the cursor |
  | `OpText` | `n`, then n bytes of UTF-8 | draws text at the cursor |
  | `OpColor` | `fg<<4 \| bg` | sets the colours |
  | `OpAttr` | mask | sets the attributes |
  | `OpFill` | `x y w h lo hi` | fills with the character `lo \| hi<<8` |
  | `OpClip` | `x y w h` | sets the clip rectangle |
  | `OpScroll` | `x y w h n` | scrolls a rectangle by the signed byte `n` |
  | `OpCursor` | `0` or `1` | hides the cursor, or shows it at the current position |

  The package comment of `gogo/screen` has the details.
- **Done.** The port checks the whole frame first. A malformed frame is
  drawn not at all and answered with `port.Done{Seq: -1}`; a good one is
  drawn and answered with the next `Seq`. Every `Out` gets its `Done`, in
  order, but not always at once: when the owner's memory is too full to
  take the message, the port tries again every 10 ms, and the `Done`
  arrives once the owner has room (waiting in `Receive` usually frees
  it). With 8 `Done`s waiting, a further `Out` is not drawn and gets
  `Done{-1}`. So, as `waitDone` above does, never wait for a `Done`
  without a timeout. Its `From` is the screen port's PID.
- **Memory.** The grid is 4 bytes a cell (1760 bytes for 40 x 11). It is
  allocated when the port opens and charged to the owner. The frame's data
  is read in place, never copied. A frame that changes nothing draws
  nothing.

On the boards, the console's render task draws the screen's rows on the
LCD. While the screen is open the console keeps everything that is printed
and shows it again when the screen closes. The port is a firmware feature:
the `GOGO_SCREEN` switch, on by default; `gogo firmware`'s feature is
`screen` (chapter 12). An image that opens `"screen"` requires the driver
`screen` (appendix F), and uses image format 15.1 (`screen.Config`), so it
needs a firmware of 15.1 or later.

## 19.6 Limits

- One screen per VM, and one owner at a time.
- No mouse or touch, no pixels: only cells. Characters outside the Basic
  Multilingual Plane are shown as a box.
- On the host, in a terminal, `println` output lands on top of the grid.
  Use the text dump, or print nothing while the screen is open.
- The StickS3 has no way to move up: lists wrap around instead. Esc
  (holding B) fires when B is released, with no sign on the screen when
  the 600 ms have passed.
- While the screen is open on a board, the console is hidden: crash
  reports of other actors (and anything printed) show only when the
  screen closes, or on the serial console. Watch the serial console
  (`gogo monitor`) while developing a UI.

See [Known issues](../known-issues.md) for the rest.
