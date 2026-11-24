// The caller's memory and the response body (http.Get's doc): an actor
// with about 6 KB of its own heap fetches an 8 KB body from
// http://10.0.0.1:<port>/big under a 16 KB limit (the MCU default) and is
// killed receiving it; the remedies work: a small maxBody (Do), or a
// larger limit (SpawnOpt). Each case prints its result and how the actor
// exited.
package main

import (
	"gogo/actor"
	"gogo/bytes"
	"gogo/dhcp"
	"gogo/http"
	"gogo/strconv"
)

func fetch(port, maxBody int) {
	ballast := make([]int, 1500)
	for i := range ballast {
		ballast[i] = i
	}
	var none bytes.Bytes
	r, ok := http.Do("GET", "http://10.0.0.1:"+strconv.Itoa(port)+"/big", "", none, maxBody, 3000)
	println("got", ok, r.Body.Len(), r.Truncated, ballast[1499])
}

func main() {
	println("start", dhcp.Start(dhcp.Config{}))
	actor.Sleep(100)
	cases := [][]int{{80, 16384, 0}, {81, 16384, 2048}, {82, 32768, 0}}
	for _, c := range cases {
		port, limit, maxBody := c[0], c[1], c[2]
		pid := actor.SpawnOpt(func() { fetch(port, maxBody) }, actor.Options{MaxMemory: limit})
		actor.Monitor(pid)
		switch m := actor.Receive().(type) {
		case actor.DownMsg:
			println("exit", port, m.Reason.Code)
		}
	}
	println("done")
}
