// Fetch a page over HTTPS. The clock comes first: TLS checks the
// certificate's dates, and the board does not know the date after boot.
// The URL comes from the config store (gogo config set fetch.url
// https://...).
package main

import (
	"gogo/config"
	"gogo/dhcp"
	"gogo/http"
	"gogo/sntp"
)

func main() {
	url, ok := config.Get("fetch.url")
	if !ok {
		url = "https://example.com/"
	}
	if !dhcp.Start(dhcp.Config{}) {
		println("no network")
		return
	}
	if !sntp.Sync("pool.ntp.org", 5000) {
		println("no time: https would fail")
		return
	}
	// The request runs in an actor of its own with room for the TLS
	// state (about 23 KB); this actor only needs room for the body.
	r, ok := http.Get(url, 15000)
	if !ok {
		println("GET", url, "failed (DNS, connect, certificate or timeout)")
		return
	}
	println("GET", url, "->", r.Status, r.Body.Len(), "bytes")
}
