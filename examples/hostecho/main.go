// hostecho: a port defined by the program that embeds the VM.
//
// The host registers a driver named "echo" (gogo_register_port,
// runtime/include/gogo.h). Its open refuses a negative Arg and otherwise
// answers at once with an event: Kind = Arg, Data = the config's Data.
// Every port.Post sent to the port comes back to the owner as an event
// with Kind + 1 and the same Data. The embed tests (runtime/embed/test)
// run it with such a host and compare its output with embed.out; on a
// board or under gogovm there is no "echo" port.
package main

import (
	"gogo/actor"
	"gogo/bytes"
	"gogo/port"
)

// event waits for the port's next event.
func event() port.Post {
	for {
		switch e := actor.Receive().(type) {
		case port.Post:
			return e
		}
	}
}

func roundtrip(p actor.PID, kind int, data bytes.Bytes) {
	actor.Send(p, port.Post{Kind: kind, Data: data})
	e := event()
	println("echo", e.Kind, e.Data.Len(), e.Data.Equal(data))
}

func main() {
	_, ok := port.Open("echo", port.Host{Arg: -1})
	println("open with Arg -1:", ok)
	_, ok = port.Open("nosuch", port.Host{})
	println("open an unknown port:", ok)

	p, ok := port.Open("echo", port.Host{Arg: 7, Data: bytes.FromString("config")})
	println("open:", ok)
	e := event()
	println("hello", e.Kind, e.Data.String())

	roundtrip(p, 1, bytes.FromString("ping"))
	roundtrip(p, -5, bytes.FromString(""))
	roundtrip(p, 2000000000, bytes.FromString("a boxed Kind"))
	b := bytes.NewBuffer(300)
	for i := 0; i < 300; i++ {
		b.WriteByte(byte(i))
	}
	roundtrip(p, 3, b.Freeze())

	port.Close(p)
	actor.Send(p, port.Post{Kind: 9}) // a closed port's PID: ignored
	println("closed")
}
