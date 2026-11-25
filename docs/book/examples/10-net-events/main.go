// net.Subscribe delivers net.Up when the interface gets an address and
// net.Down when it loses it. IP addresses are ints; format them yourself.
package main

import (
	"gogo/actor"
	"gogo/dhcp"
	"gogo/net"
	"gogo/strconv"
)

// ipString formats a.b.c.d held in an int (addresses from 128.0.0.0 up are
// negative ints, so work in int64).
func ipString(ip int) string {
	v := int64(ip)
	if v < 0 {
		v += 4294967296
	}
	s := ""
	for shift := 24; shift >= 0; shift -= 8 {
		if s != "" {
			s = s + "."
		}
		s = s + strconv.FormatInt64(v>>shift&255, 10)
	}
	return s
}

// ipv4 packs a.b.c.d into an int (wrapping past 2^31, as int does).
func ipv4(a, b, c, d int) int { return a<<24 | b<<16 | c<<8 | d }

func main() {
	println(ipString(ipv4(192, 168, 1, 42)), ipString(ipv4(10, 0, 0, 1)))
	if !net.Subscribe() {
		println("no network to watch")
		return
	}
	dhcp.Start(dhcp.Config{TimeoutMs: 1000}) // keeps trying in the background
	for {
		switch m := actor.Receive().(type) {
		case net.Up:
			println("up: ip", ipString(m.IP), "gateway", ipString(m.Gateway), "dns", ipString(m.DNS))
		case net.Down:
			println("down")
		}
	}
}
