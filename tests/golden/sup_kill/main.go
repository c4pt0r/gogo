package main

import (
	"gogo/actor"
	"gogo/supervisor"
)

// Hello is what every new worker instance sends main first.
type Hello struct{ From actor.PID }

type Crash struct{}

type Ping struct{ From actor.PID }

type Pong struct{}

type Done struct{}

func worker(boss actor.PID) {
	actor.Send(boss, Hello{actor.Self()})
	for {
		switch m := actor.Receive().(type) {
		case Crash:
			zero := 0
			println(1 / zero)
		case Ping:
			actor.Send(m.From, Pong{})
		}
	}
}

func waitHello() actor.PID {
	for {
		switch m := actor.Receive().(type) {
		case Hello:
			return m.From
		}
	}
}

// killFrom has a third actor send sup an exit signal and waits until it has.
func killFrom(me actor.PID, sup actor.PID, code int) {
	actor.Spawn(func() {
		actor.Kill(sup, code)
		actor.Send(me, Done{})
	})
	for {
		switch actor.Receive().(type) {
		case Done:
			return
		}
	}
}

// waitDown returns the reason code of the DownMsg carrying ref.
func waitDown(ref actor.Ref) int {
	for {
		switch m := actor.Receive().(type) {
		case actor.DownMsg:
			if m.Ref == ref {
				return m.Reason.Code
			}
		}
	}
}

// Codes holds two reason codes.
type Codes struct{ A, B int }

// waitBothDown returns the reason codes of the DownMsgs carrying a and b,
// in whichever order they arrive.
func waitBothDown(a actor.Ref, b actor.Ref) Codes {
	ca := -1
	cb := -1
	for ca < 0 || cb < 0 {
		switch m := actor.Receive().(type) {
		case actor.DownMsg:
			if m.Ref == a {
				ca = m.Reason.Code
			}
			if m.Ref == b {
				cb = m.Reason.Code
			}
		}
	}
	return Codes{ca, cb}
}

func main() {
	actor.TrapExit(true)
	me := actor.Self()
	sup := supervisor.Supervise(func() { worker(me) }, supervisor.Spec{
		Restart:     supervisor.Permanent,
		MaxRestarts: 3,
		PeriodMs:    1000,
	})

	// A foreign exit signal (not from the supervisor's caller) is ignored:
	// the child keeps answering, and its later crash, queued behind the
	// signal, is still handled with a restart.
	c1 := waitHello()
	r1 := actor.Monitor(c1)
	killFrom(me, sup, 150)
	actor.Send(c1, Ping{me})
	switch actor.Receive().(type) {
	case Pong:
		println("child alive after foreign kill")
	}
	actor.Send(c1, Crash{})
	println("first child down", waitDown(r1))
	c2 := waitHello()
	println("restarted", c2 != c1)

	// Killed cannot be trapped: the supervisor dies and the cascade takes
	// the child with it.
	rs := actor.Monitor(sup)
	r2 := actor.Monitor(c2)
	killFrom(me, sup, actor.Killed)
	d := waitBothDown(rs, r2)
	println("sup down", d.A)
	println("child down", d.B)
}
