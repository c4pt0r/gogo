// Configures 10.0.0.2/24 (gateway 10.0.0.1) after 1 s and reports Up;
// meanwhile the stack answers ARP for its address. Exits at 6 s.
package main

import (
	"gogo/actor"
	"gogo/net"
	"gogo/port"
)

func main() {
	println("subscribe", net.Subscribe())
	n, ok := port.Open("netif", net.IfConfig{})
	println("netif", ok)
	actor.Sleep(1000)
	actor.Send(n, net.SetAddr{IP: 10<<24 | 2, Mask: -256, Gateway: 10<<24 | 1, DNS: 10<<24 | 53})
	switch m := actor.Receive().(type) {
	case net.Up:
		println("up", m.IP, m.Gateway, m.DNS)
	}
	actor.Sleep(5000)
	println("done")
}
