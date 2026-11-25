// A TCP echo server on port 7: the listener accepts connections and hands
// each one to its own actor, which echoes until the peer closes.
package main

import (
	"gogo/actor"
	"gogo/bytes"
	"gogo/dhcp"
	"gogo/net"
	"gogo/port"
)

// echo owns one connection. A tcp port takes at most two Writes before a
// Written comes back (a third closes the connection with Overflow), so
// data that arrives while two are in flight waits in a Buffer.
func echo(conn actor.PID) {
	inFlight := 0
	waiting := bytes.NewBuffer(256)
	for {
		switch m := actor.Receive().(type) {
		case net.Data:
			if inFlight < 2 {
				actor.Send(conn, net.Write{Data: m.Data})
				inFlight++
			} else {
				waiting.WriteBytes(m.Data)
			}
		case net.Written:
			inFlight--
			if waiting.Len() > 0 {
				actor.Send(conn, net.Write{Data: waiting.Freeze()})
				inFlight++
			}
		case net.Closed:
			if m.Reason == net.Normal {
				// The peer has finished sending: finish our side too.
				actor.Send(conn, net.Shutdown{})
			}
			port.Close(conn) // after Shutdown the stack completes the close
			return
		}
	}
}

func main() {
	if !dhcp.Start(dhcp.Config{}) {
		println("no network")
		return
	}
	_, ok := port.Open("tcp_listen", net.Listen{Port: 7})
	if !ok {
		println("port 7 is taken")
		return
	}
	for {
		switch m := actor.Receive().(type) {
		case net.Accepted:
			conn := m.Conn
			h := actor.Spawn(func() { echo(conn) })
			if !port.Handoff(conn, h) {
				port.Close(conn)
			}
		}
	}
}
