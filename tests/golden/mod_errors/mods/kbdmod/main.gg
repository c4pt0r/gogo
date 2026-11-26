package main

import (
	"gogo/kbd"
	"gogo/port"
)

func Main(arg any) {
	p, ok := port.Open("kbd", kbd.Config{RepeatDelayMs: 400})
	println(p, ok)
}
