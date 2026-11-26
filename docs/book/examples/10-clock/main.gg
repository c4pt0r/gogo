// The wall clock and randomness: gogo/sys, set from the network with
// gogo/sntp. The server comes from the config store
// (gogo config set clock.server pool.ntp.org).
package main

import (
	"gogo/config"
	"gogo/dhcp"
	"gogo/sntp"
	"gogo/strconv"
	"gogo/sys"
)

// pad2 formats 0..99 with two digits.
func pad2(n int) string {
	if n < 10 {
		return "0" + strconv.Itoa(n)
	}
	return strconv.Itoa(n)
}

// date formats Unix time ms (UTC) as "YYYY-MM-DD hh:mm"
// (the days-to-civil algorithm, for dates after 1970).
func date(ms int64) string {
	mins := ms / 60000
	days := int(mins / 1440)
	hm := int(mins % 1440)
	z := days + 719468
	era := z / 146097
	doe := z - era*146097
	yoe := (doe - doe/1460 + doe/36524 - doe/146096) / 365
	doy := doe - (365*yoe + yoe/4 - yoe/100)
	mp := (5*doy + 2) / 153
	d := doy - (153*mp+2)/5 + 1
	m := mp + 3
	if m > 12 {
		m -= 12
	}
	y := yoe + era*400
	if m <= 2 {
		y++
	}
	return strconv.Itoa(y) + "-" + pad2(m) + "-" + pad2(d) + " " + pad2(hm/60) + ":" + pad2(hm%60)
}

func main() {
	server, ok := config.Get("clock.server")
	if !ok {
		server = "pool.ntp.org"
	}
	if dhcp.Start(dhcp.Config{}) && sntp.Sync(server, 5000) {
		println("synced with", server, "->", date(sys.Time()), "UTC")
	} else {
		println("not synced: no network or no answer")
	}

	// Forget the time, then set it by hand.
	sys.SetTime(0)
	println("known:", sys.Time() != 0)
	noon := int64(4102488000000) // 2100-01-01 12:00 UTC
	println("set 2100:", sys.SetTime(noon))
	println("now:", date(sys.Time()))
	// Earlier than the build: refused, the clock is unchanged.
	println("set 1970:", sys.SetTime(1000))
	println("still:", date(sys.Time()))

	// 30 random bits.
	a, b := sys.Random(), sys.Random()
	println("random in range:", a >= 0 && a < 1<<30, "differ:", a != b)
}
