// dhcp.Start gets an address; a subscriber prints Up and Down with the
// time they arrive. The store's test.timeout overrides Start's timeout.
package main

import (
	"gogo/actor"
	"gogo/config"
	"gogo/dhcp"
	"gogo/net"
	"gogo/strconv"
)

func main() {
	println("subscribe", net.Subscribe())
	timeout := 30000
	if s, ok := config.Get("test.timeout"); ok {
		timeout, _ = strconv.Atoi(s)
	}
	println("start", dhcp.Start(dhcp.Config{TimeoutMs: timeout}), actor.Now())
	for {
		switch m := actor.Receive().(type) {
		case net.Up:
			println("up", m.IP, m.Gateway, m.DNS, actor.Now())
		case net.Down:
			println("down", actor.Now())
		}
	}
}
