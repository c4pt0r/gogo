// A reader with a 4 KB limit sleeps while datagrams arrive: those over its
// quota are dropped, it is not killed, and it still receives afterwards.
package main

import (
	"gogo/actor"
	"gogo/net"
	"gogo/port"
)

func main() {
	n, _ := port.Open("netif", net.IfConfig{})
	actor.Send(n, net.SetAddr{IP: 10<<24 | 2, Mask: -256, Gateway: 10<<24 | 1})
	me := actor.Self()
	r := actor.SpawnOpt(func() {
		_, ok := port.Open("udp", net.UDP{Port: 9})
		println("udp", ok)
		actor.Sleep(1000)
		got := 0
		for {
			m, ok := actor.ReceiveTimeout(0)
			if !ok {
				break
			}
			switch m.(type) {
			case net.Datagram:
				got++
			}
		}
		println("got", got)
		switch d := actor.Receive().(type) {
		case net.Datagram:
			println("after", d.Data.Len())
		}
		actor.Send(me, 1)
	}, actor.Options{MaxMemory: 4096})
	actor.Monitor(r)
	switch m := actor.Receive().(type) {
	case actor.DownMsg:
		println("reader died", m.Reason.Code)
	default:
		println("done")
	}
}
