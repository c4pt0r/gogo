// UDP echo on port 7: echoes four datagrams to their senders, then exits.
package main

import (
	"gogo/actor"
	"gogo/net"
	"gogo/port"
)

func main() {
	n, _ := port.Open("netif", net.IfConfig{})
	actor.Send(n, net.SetAddr{IP: 10<<24 | 2, Mask: -256, Gateway: 10<<24 | 1})
	u, ok := port.Open("udp", net.UDP{Port: 7})
	println("udp", ok)
	for i := 0; i < 4; {
		switch d := actor.Receive().(type) {
		case net.Datagram:
			println("echo", d.From.IP, d.From.Port, d.Data.Len())
			actor.Send(u, net.SendTo{Addr: d.From, Data: d.Data})
			i++
		}
	}
	println("done")
}
