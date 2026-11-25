// Read settings from the config store. Set them with
//   gogo config set blink.ms 250
// (on the board when one is connected, else in ~/.gogo/config). Keys that
// start with "wifi." are never visible to programs.
package main

import (
	"gogo/config"
	"gogo/strconv"
)

// intSetting returns the key's value as an int, or def.
func intSetting(key string, def int) int {
	v, ok := config.Get(key)
	if !ok {
		return def
	}
	n, ok := strconv.Atoi(v)
	if !ok {
		println("config:", key, "is not a number:", v)
		return def
	}
	return n
}

func main() {
	println("blink.ms =", intSetting("blink.ms", 500))
	println("retries =", intSetting("retries", 3))
	name, ok := config.Get("device.name")
	println("device.name =", name, ok)
	_, ok = config.Get("wifi.ssid")
	println("wifi.ssid visible:", ok)
}
