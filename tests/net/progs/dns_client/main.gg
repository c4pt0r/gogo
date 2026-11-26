// Runs the lookups in the store's test.script, space separated, in order:
// "name" looks name up, "a+b" looks a and b up at once from two actors,
// "sleep:ms" sleeps. test.timeout is the lookup timeout (default 3000).
// Each result prints as "name ip ok time".
package main

import (
	"gogo/actor"
	"gogo/config"
	"gogo/dhcp"
	"gogo/dns"
	"gogo/strconv"
	"gogo/strings"
)

func lookup(name string, timeout int) {
	ip, ok := dns.Lookup(name, timeout)
	println(name, ip, ok, actor.Now())
}

func timeoutMs() int {
	if s, ok := config.Get("test.timeout"); ok {
		t, _ := strconv.Atoi(s)
		return t
	}
	return 3000
}

func main() {
	println("start", dhcp.Start(dhcp.Config{}))
	script, _ := config.Get("test.script")
	timeout := timeoutMs()
	for _, step := range strings.Split(script, " ") {
		if strings.HasPrefix(step, "sleep:") {
			ms, _ := strconv.Atoi(step[6:])
			actor.Sleep(ms)
		} else if strings.Index(step, "+") >= 0 {
			names := strings.Split(step, "+")
			self := actor.Self()
			for _, n := range names {
				actor.Spawn(func() {
					lookup(n, timeout)
					actor.Send(self, true)
				})
			}
			for range names {
				actor.Receive()
			}
		} else {
			lookup(step, timeout)
		}
	}
	println("done")
}
