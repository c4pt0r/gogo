// netdemo: network demo (M5StickS3).
//
//	gogo config set wifi.ssid <name>          # once; stored on the device
//	gogo config set wifi.password -          # reads the password from stdin
//	gogo config set demo.url http://example.com/   # optional: the URL to fetch
//	gogo build examples/netdemo -o netdemo.gogo && gogo flash netdemo.gogo
//
// On boot it gets an address over DHCP and shows http://<IP>/ on the
// screen. The page (Chinese/English) shows the KEY1/KEY2 press counts, the
// last fetch and the uptime, refreshing every 5 s. Every 10 s it fetches
// demo.url (default http://example.com/) and prints the result.
package main

import (
	"gogo/actor"
	"gogo/bytes"
	"gogo/config"
	"gogo/dhcp"
	"gogo/http"
	"gogo/net"
	"gogo/port"
	"gogo/strconv"
)

const (
	key1 = 11
	key2 = 12
)

// Snapshot is the state actor's answer to a page request.
type Snapshot struct {
	K1, K2, Fetches, OKs int
	Last                 string
}

type getSnapshot struct{}

// fetched is the fetcher's report.
type fetched struct {
	OK   bool
	Text string
}

// ipString formats an IPv4 address held in an int (host order).
func ipString(ip int) string {
	v := int64(ip)
	if v < 0 {
		v = v + 4294967296
	}
	a := v / 16777216
	b := v / 65536 % 256
	c := v / 256 % 256
	d := v % 256
	return strconv.FormatInt64(a, 10) + "." + strconv.FormatInt64(b, 10) + "." + strconv.FormatInt64(c, 10) + "." + strconv.FormatInt64(d, 10)
}

// escapeHTML makes s safe inside an HTML element (the URL comes from the
// config store).
func escapeHTML(s string) string {
	out := ""
	start := 0
	for i := 0; i < len(s); i++ {
		c := s[i]
		rep := ""
		if c == '<' {
			rep = "&lt;"
		} else if c == '>' {
			rep = "&gt;"
		} else if c == '&' {
			rep = "&amp;"
		} else if c == '"' {
			rep = "&quot;"
		}
		if rep != "" {
			out = out + s[start:i] + rep
			start = i + 1
		}
	}
	return out + s[start:]
}

// state owns the keys and the counters; registered as "state".
func state() {
	actor.Register("state", actor.Self())
	_, ok1 := port.Open("gpio", port.GPIO{Pin: key1, Mode: port.InputPullUp, Edge: port.Falling})
	_, ok2 := port.Open("gpio", port.GPIO{Pin: key2, Mode: port.InputPullUp, Edge: port.Falling})
	if !ok1 || !ok2 {
		println("keys: 打不开 / cannot open")
	}
	k1, k2, fetches, oks := 0, 0, 0, 0
	last := "还没有 / none yet"
	for {
		switch m := actor.Receive().(type) {
		case port.Edge:
			if m.Pin == key1 {
				k1++
				println("KEY1", k1)
			} else {
				k2++
				println("KEY2", k2)
			}
		case fetched:
			fetches++
			if m.OK {
				oks++
			}
			last = m.Text
		case actor.Request:
			actor.Reply(m, Snapshot{K1: k1, K2: k2, Fetches: fetches, OKs: oks, Last: last})
		}
	}
}

// fetcher gets demo.url every 10 s and reports to state.
func fetcher() {
	url, ok := config.Get("demo.url")
	if !ok || url == "" {
		url = "http://example.com/"
	}
	println("GET", url, "每 10 秒 / every 10 s")
	for {
		actor.Sleep(10000)
		t0 := actor.Now()
		r, ok := http.Get(url, 8000)
		ms := strconv.FormatInt64(actor.Now()-t0, 10)
		text := "失败 / failed (" + ms + " ms)"
		if ok {
			text = "HTTP " + strconv.Itoa(r.Status) + ", " + strconv.Itoa(r.Body.Len()) + " B, " + ms + " ms"
			if r.Truncated {
				text = text + " (截断 / truncated)"
			}
		}
		println("GET:", text)
		if s, found := actor.Whereis("state"); found {
			actor.Send(s, fetched{OK: ok, Text: url + " → " + text})
		}
	}
}

func handle(r http.Request) http.Response {
	if r.Path != "/" {
		return http.Response{Status: 404, Headers: []http.Header{{"Content-Type", "text/plain; charset=utf-8"}}, Body: bytes.FromString("404 找不到 / not found\n")}
	}
	var snap Snapshot
	if s, found := actor.Whereis("state"); found {
		if v, ok := actor.Call(s, getSnapshot{}, 1000); ok {
			switch x := v.(type) {
			case Snapshot:
				snap = x
			}
		}
	}
	up := strconv.FormatInt64(actor.Now()/1000, 10)
	b := bytes.NewBuffer(1024)
	b.WriteString("<!doctype html><html><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width\">")
	b.WriteString("<meta http-equiv=\"refresh\" content=\"5\"><title>gogo netdemo</title></head><body style=\"font-family:sans-serif\">")
	b.WriteString("<h1>gogo 网络演示 / network demo</h1>")
	b.WriteString("<p>KEY1 按下次数 / presses: <b>" + strconv.Itoa(snap.K1) + "</b></p>")
	b.WriteString("<p>KEY2 按下次数 / presses: <b>" + strconv.Itoa(snap.K2) + "</b></p>")
	b.WriteString("<p>抓取 / fetches: 成功 " + strconv.Itoa(snap.OKs) + " / 共 " + strconv.Itoa(snap.Fetches) + " (ok / total)</p>")
	b.WriteString("<p>最近一次 / last: " + escapeHTML(snap.Last) + "</p>")
	b.WriteString("<p>运行时间 / uptime: " + up + " s</p>")
	b.WriteString("<p>本连接 actor 内存 / this connection's actor: " + strconv.Itoa(actor.Info(actor.Self()).Memory) + " B</p>")
	b.WriteString("<p><small>M5StickS3 · gogo VM · 自己写的 TCP/IP / own TCP/IP stack</small></p></body></html>")
	return http.Response{Headers: []http.Header{{"Content-Type", "text/html; charset=utf-8"}}, Body: b.Freeze()}
}

func main() {
	println("netdemo: 连接中 / connecting")
	actor.Spawn(state)
	net.Subscribe()
	dhcp.Start(dhcp.Config{TimeoutMs: 1000})
	actor.TrapExit(true)
	serving := false
	for {
		switch m := actor.Receive().(type) {
		case net.Up:
			println("打开 / open http://" + ipString(m.IP) + "/")
			if !serving {
				serving = true
				http.Serve(80, handle)
				actor.Spawn(fetcher)
			}
		case net.Down:
			println("网络断开 / network down")
		case actor.ExitMsg:
			println("exit", m.Reason.Code)
		}
	}
}
