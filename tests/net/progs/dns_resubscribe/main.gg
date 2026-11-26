// Every net.Subscribe slot is taken when the resolver starts (4 holders;
// the address is static, so DHCP holds none), so its Subscribe fails and a lookup fails at once; once a
// subscriber exits, the next lookup subscribes again and is answered.
package main

import (
	"gogo/actor"
	"gogo/dhcp"
	"gogo/dns"
	"gogo/net"
)

func holder(parent actor.PID) {
	actor.Send(parent, net.Subscribe())
	for {
		switch actor.Receive().(type) {
		case bool:
			return
		}
	}
}

func main() {
	println("start", dhcp.Start(dhcp.Config{}))
	self := actor.Self()
	var hs []actor.PID
	for i := 0; i < 4; i++ {
		hs = append(hs, actor.Spawn(func() { holder(self) }))
	}
	for i := 0; i < 4; i++ {
		switch m := actor.Receive().(type) {
		case bool:
			println("subscribed", m)
		}
	}
	ip, ok := dns.Lookup("a.test", 3000)
	println("a.test", ip, ok, actor.Now())
	r := actor.Monitor(hs[0])
	actor.Send(hs[0], true)
	for done := false; !done; {
		switch m := actor.Receive().(type) {
		case actor.DownMsg:
			done = m.Ref == r
		}
	}
	ip, ok = dns.Lookup("a.test", 3000)
	println("a.test", ip, ok, actor.Now())
	println("done")
}
