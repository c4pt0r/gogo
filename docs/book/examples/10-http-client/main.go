// Fetch a page with http.Get. The URL comes from the config store
// (gogo config set fetch.url http://...); http:// only, no https.
package main

import (
	"gogo/config"
	"gogo/dhcp"
	"gogo/http"
)

func main() {
	url, ok := config.Get("fetch.url")
	if !ok {
		url = "http://example.com/"
	}
	if !dhcp.Start(dhcp.Config{}) {
		println("no network")
		return
	}
	r, ok := http.Get(url, 8000)
	if !ok {
		println("GET", url, "failed (DNS, connect or timeout)")
		return
	}
	ct, _ := http.HeaderValue(r.Headers, "content-type")
	println("GET", url, "->", r.Status, ct, r.Body.Len(), "bytes", "truncated:", r.Truncated)

	// https is refused at once: there is no TLS yet.
	_, ok = http.Get("https://example.com/", 1000)
	println("https ok:", ok)
}
