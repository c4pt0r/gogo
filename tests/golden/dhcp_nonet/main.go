// Without a network dhcp.Start returns false at once; the client exits.
package main

import (
	"gogo/actor"
	"gogo/dhcp"
)

func main() {
	println("start", dhcp.Start(dhcp.Config{}), actor.Now())
}
