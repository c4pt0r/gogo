package main

import "gogo/actor"

// Mine is this module's own struct type.
type Mine struct{ X int }

func pid(arg any) actor.PID {
	switch p := arg.(type) {
	case actor.PID:
		return p
	}
	var none actor.PID
	return none
}

// Consts sends the host a const string, an int64 const and a reserved
// struct holding a const string: all arrive as copies.
func Consts(arg any) {
	host := pid(arg)
	actor.Send(host, "const from module")
	actor.Send(host, int64(1)<<40)
	actor.Send(host, actor.Reason{Code: 7, Func: "in reason"})
}

// Struct sends the host a struct of this module: BadMsg for this actor.
func Struct(arg any) {
	actor.Send(pid(arg), Mine{1})
	println("never")
}

// Caller calls the host server, which answers with a host struct.
func Caller(arg any) {
	_, ok := actor.Call(pid(arg), 1, 200)
	println("module call ok", ok)
}

// Echo prints what it is given.
func Echo(arg any) {
	switch x := arg.(type) {
	case actor.Reason:
		println("echo reason", x.Code, x.Func)
	case string:
		println("echo string", x)
	}
	for {
		switch m := actor.Receive().(type) {
		case actor.Reason:
			println("echo got reason", m.Code)
		case int:
			return
		}
	}
}

func boom(n int) int { return 10 / n }

// Boom crashes in boom.
func Boom(arg any) {
	println(boom(0))
}
