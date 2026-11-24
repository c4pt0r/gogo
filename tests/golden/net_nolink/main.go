// Without a network (no --net-fd) the net ports refuse to open and
// Subscribe fails; the program is otherwise unaffected.
package main

import (
	"gogo/net"
	"gogo/port"
)

func main() {
	_, ok := port.Open("netif", net.IfConfig{})
	println("netif", ok)
	_, ok = port.Open("udp", net.UDP{Port: 7})
	println("udp", ok)
	println("subscribe", net.Subscribe())
	var d net.Datagram
	println(d.From.IP, d.From.Port, d.Data.Len())
}
