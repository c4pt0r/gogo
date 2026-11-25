//go:build gogo

// The build tag keeps the Go toolchain away: gogo/net (netapi/) is not a
// Go-importable package path; the gogo compiler ignores build tags.

// Package sntp sets the wall clock (gogo/sys) from an SNTP server (RFC 4330,
// version 4, over the "udp" port), written in the gogo subset and compiled
// into every image that imports it. A TLS client needs the wall clock to
// check certificate validity, and nothing else on the device knows the date.
//
// Sync does one exchange in a helper actor, so the caller's mailbox is
// untouched. A reply counts only if it comes from the server's address and
// port 123, echoes the request's transmit timestamp (a nonce), is a server
// mode reply of version 3 or 4 with a synchronized clock (leap indicator not
// 3) and a stratum of 1..15 (0 is a kiss-of-death), and carries a transmit
// time after 2020. The time set is the server's transmit time plus half the
// round trip, measured on the actor clock; the precision is a few ms on a
// LAN and the path asymmetry on the internet. Sync does not apply the NTP
// clock-filter algorithm, step limits or polling: it is for setting the
// clock now and then, not for keeping it.
//
// SNTP is unauthenticated. The nonce (the transmit timestamp, 60 random bits
// from sys.Random) stops an off-path attacker from guessing a reply, but an
// on-path attacker can set any time the build-time floor allows (see
// gogo/sys); there is no defence against that here. Sync returns false if
// the floor refuses the time.
package sntp

import (
	"gogo/actor"
	"gogo/bytes"
	"gogo/dns"
	"gogo/net"
	"gogo/port"
	"gogo/sys"
)

// ntpToUnix is the seconds between 1900-01-01 and 1970-01-01 (NTP era 0).
const ntpToUnix = 2208988800

// minUnix is 2020-01-01: an earlier transmit time is a broken server.
const minUnix = 1577836800

// job is Sync's request to the helper: ask the server at IP, giving up at
// Deadline (actor.Now time).
type job struct {
	IP       int
	Deadline int64
}

// result is the helper's reply: the Unix time in ms at the moment of the
// reply, if OK.
type result struct {
	OK bool
	Ms int64
}

// Sync asks server (a name, resolved with gogo/dns, or a dotted IPv4
// address) for the time and sets the wall clock (sys.SetTime). It returns
// false, leaving the clock alone, if the server cannot be resolved, gives
// a time before the build-time floor, or gives no valid reply within timeoutMs (<= 0: 5000); the request is sent up to 3
// times, sharing the time left after the lookup.
func Sync(server string, timeoutMs int) bool {
	if timeoutMs <= 0 {
		timeoutMs = 5000
	}
	deadline := actor.Now() + int64(timeoutMs)
	ip, ok := dns.Lookup(server, timeoutMs)
	if !ok {
		return false
	}
	left := int(deadline - actor.Now())
	if left <= 0 {
		return false
	}
	pid := actor.Spawn(worker)
	r, got := actor.Call(pid, job{IP: ip, Deadline: deadline}, left+100)
	if !got {
		actor.Kill(pid, actor.Shutdown)
		return false
	}
	switch a := r.(type) {
	case result:
		if a.OK {
			return sys.SetTime(a.Ms)
		}
	}
	return false
}

// Now returns the wall clock, Unix time in ms, or 0 if it is not set (see
// gogo/sys).
func Now() int64 {
	return sys.Time()
}

// worker is the helper actor of one Sync.
func worker() {
	m := actor.Receive()
	switch q := m.(type) {
	case actor.Request:
		switch j := q.Body.(type) {
		case job:
			ms, ok := exchange(j.IP, j.Deadline)
			actor.Reply(q, result{OK: ok, Ms: ms})
		}
	}
}

// exchange sends the request to ip:123 (up to 3 times before deadline) and
// returns the time from the first valid reply.
func exchange(ip int, deadline int64) (int64, bool) {
	u, ok := port.Open("udp", net.UDP{})
	if !ok {
		return 0, false
	}
	ms, ok := try(u, ip, deadline)
	port.Close(u)
	return ms, ok
}

// try is exchange on the open port u.
func try(u actor.PID, ip int, deadline int64) (int64, bool) {
	for tries := 1; tries <= 3; tries++ {
		now := actor.Now()
		left := deadline - now
		if left <= 0 {
			return 0, false
		}
		// the nonce: 60 random bits (never 0: the low bit is set)
		nonce := int64(sys.Random())<<31 | int64(sys.Random())<<1 | 1
		sent := now
		actor.Send(u, net.SendTo{Addr: net.Addr{IP: ip, Port: 123}, Data: request(nonce)})
		wait := left / int64(4-tries)
		until := now + wait
		for {
			w := until - actor.Now()
			if w < 0 {
				w = 0
			}
			m, got := actor.ReceiveTimeout(int(w))
			if !got {
				break
			}
			switch d := m.(type) {
			case net.Datagram:
				if d.From.IP == ip && d.From.Port == 123 {
					ms, ok := parse(d.Data, nonce)
					if ok {
						return ms + (actor.Now()-sent)/2, true
					}
				}
			}
		}
	}
	return 0, false
}

// request builds the 48-byte client request: LI 0, version 4, mode 3, and
// nonce as the transmit timestamp.
func request(nonce int64) bytes.Bytes {
	b := make([]byte, 48)
	b[0] = 0x23
	for i := 0; i < 8; i++ {
		b[40+i] = byte(nonce >> (56-8*i))
	}
	return bytes.FromSlice(b)
}

// parse validates the reply d to the request with this nonce and returns
// the server's transmit time as Unix ms.
func parse(d bytes.Bytes, nonce int64) (int64, bool) {
	if d.Len() < 48 {
		return 0, false
	}
	b0 := int(d.At(0))
	ver := (b0 >> 3) & 7
	if b0>>6 == 3 || b0&7 != 4 || (ver != 3 && ver != 4) {
		return 0, false
	}
	if st := int(d.At(1)); st < 1 || st > 15 {
		return 0, false
	}
	for i := 0; i < 8; i++ { // the originate timestamp is our nonce
		if int64(d.At(24+i)) != (nonce>>(56-8*i))&255 {
			return 0, false
		}
	}
	secs := int64(d.At(40))<<24 | int64(d.At(41))<<16 | int64(d.At(42))<<8 | int64(d.At(43))
	if secs&0x80000000 == 0 { // era 1 (2036 on)
		secs += 4294967296
	}
	secs -= ntpToUnix
	if secs < minUnix {
		return 0, false
	}
	frac := int64(d.At(44))<<8 | int64(d.At(45)) // the top 16 bits are enough for ms
	return secs*1000 + (frac*1000)>>16, true
}
