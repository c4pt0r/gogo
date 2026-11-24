// Dials 10.0.0.1:80, writes 3000 bytes (an off-heap handle) and "world"
// (a heap copy), shuts down and prints what comes back.
package main

import (
	"gogo/actor"
	"gogo/bytes"
	"gogo/net"
	"gogo/port"
)

func main() {
	n, _ := port.Open("netif", net.IfConfig{})
	actor.Send(n, net.SetAddr{IP: 10<<24 | 2, Mask: -256, Gateway: 10<<24 | 1})
	actor.Sleep(100)
	b := bytes.NewBuffer(3000)
	for i := 0; i < 3000; i++ {
		b.WriteByte(byte('a' + i%26))
	}
	big := b.Freeze()
	c, ok := port.Open("tcp", net.Dial{IP: 10<<24 | 1, Port: 80})
	println("dial", ok)
	for {
		switch m := actor.Receive().(type) {
		case net.Connected:
			println("connected")
			actor.Send(c, net.Write{Data: big})
			actor.Send(c, net.Write{Data: bytes.FromString("world")})
			actor.Send(c, net.Shutdown{})
		case net.Written:
			println("written")
		case net.Data:
			println("data", m.Data.String())
		case net.Closed:
			println("closed", m.Reason)
			port.Close(c)
			return
		}
	}
}
