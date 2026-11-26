// Without an address the stack passes only DHCP: a broadcast from port 68
// to port 67 goes out from 0.0.0.0, a reply to port 68 for our MAC comes in.
package main

import (
	"gogo/actor"
	"gogo/bytes"
	"gogo/net"
	"gogo/port"
)

func main() {
	c, _ := port.Open("udp", net.UDP{Port: 68})
	s, _ := port.Open("udp", net.UDP{Port: 7})
	actor.Send(c, net.SendTo{Addr: net.Addr{IP: -1, Port: 67}, Data: bytes.FromString("discover")})
	actor.Send(c, net.SendTo{Addr: net.Addr{IP: 10<<24 | 1, Port: 67}, Data: bytes.FromString("unicast")})
	actor.Send(s, net.SendTo{Addr: net.Addr{IP: -1, Port: 67}, Data: bytes.FromString("wrong port")})
	switch d := actor.Receive().(type) {
	case net.Datagram:
		println("got", d.From.IP, d.From.Port, d.Data.Len(), d.Data.At(0))
	}
}
