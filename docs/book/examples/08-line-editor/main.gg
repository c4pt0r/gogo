// A line editor on the Cardputer keyboard: type characters, Backspace
// deletes, Enter prints the line, Esc quits. On the host the keys come
// from the stdin script in "input".
package main

import (
	"gogo/actor"
	"gogo/kbd"
	"gogo/port"
)

// printable holds ASCII 32..126, so character c is printable[c-32 : c-31]
// (string([]byte{byte(c)}) works too, but allocates a slice per key).
const printable = " !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~"

func main() {
	_, ok := port.Open("kbd", kbd.Config{RepeatDelayMs: 400, RepeatMs: 60})
	if !ok {
		println("no keyboard on this board")
		return
	}
	line := ""
	for {
		switch k := actor.Receive().(type) {
		case kbd.Key:
			if !k.Down {
				continue // ignore releases
			}
			if k.Code == kbd.Esc {
				println("bye")
				return
			} else if k.Code == kbd.Enter {
				println("line:", line)
				line = ""
			} else if k.Code == kbd.Backspace {
				if len(line) > 0 {
					line = line[:len(line)-1]
				}
			} else if k.Mods&kbd.ModCtrl != 0 && k.Char == 3 {
				println("^C: line cleared")
				line = ""
			} else if k.Char >= 32 && k.Char <= 126 {
				line = line + printable[k.Char-32:k.Char-31]
			}
		}
	}
}
