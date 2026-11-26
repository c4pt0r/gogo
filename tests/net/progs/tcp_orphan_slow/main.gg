// A handler writes a 200-byte response, shuts down and exits at once; the
// peer reads it a byte at a time. The orphan left behind lives at most
// 20 s (GOGO_NET_ORPHAN_MS) whatever its progress, then is reset. main
// keeps the address 30 s, then returns.
package main

import (
	"gogo/actor"
	"gogo/bytes"
	"gogo/net"
	"gogo/port"
)

func handler(conn actor.PID) {
	switch actor.Receive().(type) {
	case net.Data:
		b := bytes.NewBuffer(200)
		for i := 0; i < 200; i++ {
			b.WriteByte('z')
		}
		actor.Send(conn, net.Write{Data: b.Freeze()})
		actor.Send(conn, net.Shutdown{})
	}
}

func main() {
	n, _ := port.Open("netif", net.IfConfig{})
	actor.Send(n, net.SetAddr{IP: 10<<24 | 2, Mask: -256, Gateway: 10<<24 | 1})
	if _, ok := port.Open("tcp_listen", net.Listen{Port: 8080}); !ok {
		println("listen failed")
	}
	for {
		switch m := actor.Receive().(type) {
		case net.Accepted:
			println("accepted")
			conn := m.Conn
			h := actor.Spawn(func() { handler(conn) })
			actor.Monitor(h)
			port.Handoff(conn, h)
		case actor.DownMsg:
			println("handler exit", m.Reason.Code)
			actor.Sleep(30000)
			return
		}
	}
}
