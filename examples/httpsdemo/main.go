// httpsdemo: HTTPS on the device (the "tls" port, gogo/http over TLS).
//
//	gogo config set wifi.ssid <名字>          # 一次即可，存在设备上
//	gogo config set wifi.password -          # 从 stdin 读密码
//	gogo build examples/httpsdemo -o httpsdemo.gogo && gogo flash httpsdemo.gogo
//	gogo monitor -reset -t 90 -until "httpsdemo: done"
//
// 开机后：DHCP 拿地址，用 SNTP（pool.ntp.org）对时（证书检查要用日期），
// 然后：
//  1. http.Get("https://example.com/") 一次（一次性：一个连接、一次握手）；
//  2. NewClient 连发 3 个 GET（keep-alive：只握手一次）；
//  3. 反例：连 example.com 的地址，但 Host 写 wrong.example，证书对不上，
//     应当被拒绝（Closed{BadCert}；服务器自己不认这个名字时是 Closed{TLS}，
//     example.com 所在的 Cloudflare 就是这样）。
//
// 每一步打印状态、正文长度和耗时；之后安静地待着。握手的切片时间和
// TLS 内存在 `gogo config stats`（tls_max_step_us、tls_last_hs_us、
// mbedtls_mem_peak、psram_min）里看。
//
// On boot: DHCP, SNTP from pool.ntp.org (certificate checks need the
// date), then (1) one http.Get of https://example.com/ (one connection, one
// handshake), (2) three GETs through a NewClient (keep-alive: one
// handshake for all three), (3) a negative test: a "tls" port to
// example.com's address with Host "wrong.example", which must be refused
// (Closed{BadCert}, or Closed{TLS} when the server itself rejects the name,
// as Cloudflare, example.com's host, does). Each step prints its status, body length and time;
// then the program idles silently. `gogo config stats` shows the handshake
// slices (tls_max_step_us, tls_last_hs_us) and the TLS memory
// (mbedtls_mem_peak, psram_min). It pauses 15 s before the TLS work (and
// prints nothing then), so `gogo config stats` can take the "before"
// figures.
package main

import (
	"gogo/actor"
	"gogo/bytes"
	"gogo/dhcp"
	"gogo/dns"
	"gogo/http"
	"gogo/net"
	"gogo/port"
	"gogo/sntp"
	"gogo/strconv"
)

const url = "https://example.com/"

func ms(t0 int64) string {
	return strconv.FormatInt64(actor.Now()-t0, 10) + " ms"
}

func show(what string, r http.Response, ok bool, t0 int64) {
	if !ok {
		println(what, "失败 / failed,", ms(t0))
		return
	}
	println(what, "HTTP", r.Status, "body", r.Body.Len(), "B,", ms(t0))
}

// wrongHost dials example.com's address with the wrong name; the
// certificate does not match, so the handshake must fail: BadCert, or TLS
// if the server rejects the name (SNI) before sending a certificate.
// Spawned with room for the TLS state (net.DialTLS: charged to the owner).
func wrongHost(ip int) {
	t0 := actor.Now()
	p, ok := port.Open("tls", net.DialTLS{IP: ip, Port: 443, TimeoutMs: 15000, Host: "wrong.example"})
	if !ok {
		println("wrong.example: open 失败 / open failed")
		return
	}
	for {
		switch m := actor.Receive().(type) {
		case net.Connected:
			println("wrong.example: 竟然连上了 / CONNECTED (wrong!),", ms(t0))
			port.Close(p)
			return
		case net.Closed:
			if m.Reason == net.BadCert {
				println("wrong.example: 拒绝 / refused, Closed{BadCert},", ms(t0))
			} else {
				if m.Reason == net.TLS {
					println("wrong.example: 拒绝 / refused by the server, Closed{TLS},", ms(t0))
				} else {
					println("wrong.example: closed", m.Reason, ",", ms(t0))
				}
			}
			return
		}
	}
}

func run() {
	t0 := actor.Now()
	if !sntp.Sync("pool.ntp.org", 8000) {
		println("sntp: 失败 / failed,", ms(t0))
		return
	}
	println("sntp: ok,", ms(t0), "unix ms", sntp.Now())
	println("httpsdemo: 15 s 后开始 / TLS in 15 s")
	actor.Sleep(15000)

	t0 = actor.Now()
	r, ok := http.Get(url, 20000)
	show("Get", r, ok, t0)

	var none bytes.Bytes
	c := http.NewClient()
	for i := 1; i <= 3; i++ {
		t0 = actor.Now()
		r, ok = http.ClientDo(c, "GET", url, "", none, 20000)
		show("Client "+strconv.Itoa(i), r, ok, t0)
	}
	http.CloseClient(c)

	ip, found := dns.Lookup("example.com", 5000)
	if !found {
		println("dns: example.com 失败 / failed")
	} else {
		actor.Monitor(actor.SpawnOpt(func() { wrongHost(ip) }, actor.Options{MaxMemory: 40000}))
		waiting := true
		for waiting {
			switch actor.Receive().(type) {
			case actor.DownMsg:
				waiting = false
			}
		}
	}
	println("httpsdemo: done")
}

func main() {
	println("httpsdemo: 连接中 / connecting")
	net.Subscribe()
	dhcp.Start(dhcp.Config{TimeoutMs: 1000})
	started := false
	for {
		switch actor.Receive().(type) {
		case net.Up:
			println("httpsdemo: net up")
			if !started {
				started = true
				run()
			}
		case net.Down:
			println("httpsdemo: net down")
		}
	}
}
