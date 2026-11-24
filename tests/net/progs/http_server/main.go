// Serves on port 80: GET /crash crashes (divzero), GET /proxy fetches
// http://10.0.0.1:9000/data and returns it, POST echoes its body (201),
// /badheader returns a header with CRLF in it, anything else answers "hello <path>". A second Serve on 80 fails; main
// traps that exit and prints its code. Each request prints a line.
package main

import (
	"gogo/actor"
	"gogo/bytes"
	"gogo/dhcp"
	"gogo/http"
)

func handle(r http.Request) http.Response {
	ua, _ := http.HeaderValue(r.Headers, "user-agent")
	println("req", r.Method, r.Path, len(r.Headers), ua, r.Body.Len(), actor.Now())
	if r.Path == "/crash" {
		println(1 / (len(r.Path) - 6)) // divzero
	}
	if r.Path == "/proxy" {
		p, ok := http.Get("http://10.0.0.1:9000/data", 5000)
		println("proxy", ok, p.Status, actor.Now())
		return http.Response{Status: p.Status, Body: p.Body}
	}
	if r.Path == "/badheader" {
		return http.Response{Headers: []http.Header{{"X-Split", "a\r\nSet-Cookie: x=1"}}}
	}
	if r.Method == "POST" {
		return http.Response{Status: 201, Headers: []http.Header{{"Content-Type", "text/plain"}, {"Content-Length", "999"}}, Body: r.Body}
	}
	return http.Response{Headers: []http.Header{{"Content-Type", "text/plain"}}, Body: bytes.FromString("hello " + r.Path)}
}

func main() {
	println("start", dhcp.Start(dhcp.Config{}))
	actor.TrapExit(true)
	http.Serve(80, handle)
	http.Serve(80, handle)
	for {
		switch m := actor.Receive().(type) {
		case actor.ExitMsg:
			println("exit", m.Reason.Code)
		}
	}
}
