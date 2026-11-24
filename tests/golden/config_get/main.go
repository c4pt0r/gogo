// config.Get reads the store ($GOGO_CONFIG = this directory's config file
// under the golden runner): set keys, the empty value, missing keys, and
// "wifi." keys hidden from programs.
package main

import (
	"gogo/actor"
	"gogo/config"
)

func show(key string) {
	v, ok := config.Get(key)
	println(key, "=", "["+v+"]", ok)
}

func main() {
	show("net.ip")
	show("greeting")
	show("empty")
	show("missing")
	show("wifi.ssid")
	show("wifi.password")
	show("net")
	// Another actor reads the same store.
	done := actor.Self()
	actor.Spawn(func() {
		v, ok := config.Get("greeting")
		println("child", v, ok)
		actor.Send(done, true)
	})
	actor.Receive()
}
