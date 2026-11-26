package main

import (
	"gogo/actor"
	"gogo/supervisor"
)

// Hello is what each middle-layer instance sends main when it starts; main
// answers with Config, telling it whether its inner child should crash.
type Hello struct{ From actor.PID }

type Config struct{ Crash bool }

// Up is sent by an inner child that keeps running.
type Up struct{}

func innerChild(boss actor.PID, crash bool) {
	if crash {
		zero := 0
		println(1 / zero)
	}
	actor.Send(boss, Up{})
	for {
		actor.Receive()
	}
}

func askConfig(boss actor.PID) bool {
	actor.Send(boss, Hello{actor.Self()})
	for {
		switch m := actor.Receive().(type) {
		case Config:
			return m.Crash
		}
	}
}

// middle is the outer supervisor's child: it starts an inner supervisor and
// waits. When the inner supervisor gives up (exit Shutdown), middle, linked
// to it and not trapping, exits too, and the outer supervisor restarts it.
func middle(boss actor.PID) {
	crash := askConfig(boss)
	supervisor.Supervise(func() { innerChild(boss, crash) }, supervisor.Spec{
		Restart:     supervisor.Permanent,
		MaxRestarts: 1,
		PeriodMs:    1000,
	})
	for {
		actor.Receive()
	}
}

func main() {
	me := actor.Self()
	supervisor.Supervise(func() { middle(me) }, supervisor.Spec{
		Restart:     supervisor.Permanent,
		MaxRestarts: 3,
		PeriodMs:    1000,
	})
	for gen := 1; gen <= 2; gen++ {
		switch m := actor.Receive().(type) {
		case Hello:
			println("inner generation", gen)
			actor.Send(m.From, Config{Crash: gen == 1})
		}
	}
	switch actor.Receive().(type) {
	case Up:
		println("inner child up")
	}
	// Returning shuts the whole two-level tree down.
}
