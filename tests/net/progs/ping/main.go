// Configures 10.0.0.2/24 and idles 10 s: the stack answers pings.
package main

import (
	"gogo/actor"
	"gogo/net"
	"gogo/port"
)

func main() {
	n, _ := port.Open("netif", net.IfConfig{})
	actor.Send(n, net.SetAddr{IP: 10<<24 | 2, Mask: -256, Gateway: 10<<24 | 1})
	actor.Sleep(10000)
	println("done")
}
