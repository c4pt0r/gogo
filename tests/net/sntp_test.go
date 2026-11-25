package nettest

import (
	"encoding/binary"
	"os"
	"path/filepath"
	"strconv"
	"testing"
	"time"

	"gogotools/tests/net/sim"
)

// startSNTP starts sntp_client with a static address (10.0.0.2/24, gateway
// and DNS server 10.0.0.1), the script and the Sync timeout, and answers the
// gateway's ARP. The simulated clock leaves the wall clock unknown.
func startSNTP(t *testing.T, script, timeout string) *sim.VM {
	t.Helper()
	cfg := "net.ip=10.0.0.2\nnet.gw=10.0.0.1\nnet.dns=10.0.0.1\ntest.script=" + script + "\n"
	if timeout != "" {
		cfg += "test.timeout=" + timeout + "\n"
	}
	path := filepath.Join(t.TempDir(), "config")
	if err := os.WriteFile(path, []byte(cfg), 0o600); err != nil {
		t.Fatal(err)
	}
	t.Setenv("GOGO_CONFIG", path)
	v := start(t, "sntp_client")
	arpFor(t, one(t)(v.Sync()), vmHost.IP, gwHost.IP)
	none(t)(v.Exchange(arpReply(gwHost, vmHost)))
	return v
}

// sntpRequest checks p is an SNTP client request from the VM to server and
// returns its transmit timestamp (the nonce the reply must echo) and the
// VM's port.
func sntpRequest(t *testing.T, p sim.Packet, server sim.IP) ([]byte, uint16) {
	t.Helper()
	if p.UDP == nil || p.IP.Src != vmHost.IP || p.IP.Dst != server || p.UDP.DstPort != 123 || p.UDP.SrcPort < 49152 {
		t.Fatalf("want an SNTP request to %v:123, got %+v %+v", server, p.IP, p.UDP)
	}
	d := p.UDP.Payload
	if len(d) != 48 || d[0] != 0x23 || d[1] != 0 {
		t.Fatalf("not a v4 client request: % x", d)
	}
	nonce := d[40:48]
	if binary.BigEndian.Uint64(nonce) == 0 {
		t.Fatal("zero transmit timestamp")
	}
	return append([]byte(nil), nonce...), p.UDP.SrcPort
}

// ntpTime is the NTP seconds and fraction of Unix time sec + ms/1000.
func ntpTime(sec, ms uint64) (uint32, uint32) {
	return uint32(sec + 2208988800), uint32(ms * (1 << 32) / 1000)
}

// sntpReply is a server reply to the request with this nonce: server mode,
// version 4, stratum 2, transmit time sec + ms (Unix).
func sntpReply(nonce []byte, sec, ms uint64) []byte {
	d := make([]byte, 48)
	d[0] = 0x24
	d[1] = 2
	copy(d[24:], nonce)
	s, f := ntpTime(sec, ms)
	binary.BigEndian.PutUint32(d[40:], s)
	binary.BigEndian.PutUint32(d[44:], f)
	return d
}

func sntpFrame(from sim.Host, srcPort, port uint16, payload []byte) []byte {
	return sim.UDPFrame(from, vmHost, sim.UDP{SrcPort: srcPort, DstPort: port, Payload: payload})
}

// unix2026 is a time after the VM's build-time floor (the VM was built
// before this test runs): the start of tomorrow, UTC.
var unix2026 = uint64(time.Now().Unix()/86400+1) * 86400

// Sync sets the wall clock to the server's transmit time plus half the round
// trip on the actor clock (40 ms here); the clock then advances with the VM's.
func TestSNTPSync(t *testing.T) {
	v := startSNTP(t, "wall sleep:100 sync:10.0.0.1 wall sleep:2000 wall", "")
	_, port := sntpRequest(t, one(t)(v.Advance(100)), gwHost.IP)
	// no reply: the request is sent again after a third of the time left,
	// with a new nonce; this one is answered 40 ms later
	nonce, port2 := sntpRequest(t, one(t)(v.Advance(1000)), gwHost.IP)
	if port2 != port {
		t.Errorf("port changed from %d to %d", port, port2)
	}
	none(t)(v.Advance(40))
	none(t)(v.Exchange(sntpFrame(gwHost, 123, port, sntpReply(nonce, unix2026, 125))))
	set := strconv.FormatUint(unix2026*1000+125+20, 10)
	later := strconv.FormatUint(unix2026*1000+125+20+2000, 10)
	none(t)(v.Advance(2000))
	stop(t, v, "start true\nwall 0\nsync 10.0.0.1 true 1140 "+set+"\nwall "+set+"\nwall "+later+"\ndone\n")
}

// Replies that are not valid answers to the request are ignored: another
// source port, a wrong nonce, a client-mode packet, stratum 0 (kiss-of-death),
// an unsynchronized server (LI 3), version 2, a time before 2020, and a short
// packet; then a valid one counts.
func TestSNTPValidation(t *testing.T) {
	v := startSNTP(t, "sleep:100 sync:10.0.0.1 wall", "")
	nonce, port := sntpRequest(t, one(t)(v.Advance(100)), gwHost.IP)
	good := func() []byte { return sntpReply(nonce, unix2026, 0) }
	bad := func(name string, mod func(d []byte)) {
		t.Helper()
		d := good()
		mod(d)
		none(t)(v.Exchange(sntpFrame(gwHost, 123, port, d)))
	}
	none(t)(v.Exchange(sntpFrame(gwHost, 5353, port, good())))
	bad("nonce", func(d []byte) { d[31] ^= 1 })
	bad("mode 3", func(d []byte) { d[0] = 0x23 })
	bad("stratum 0", func(d []byte) { d[1] = 0 })
	bad("stratum 16", func(d []byte) { d[1] = 16 })
	bad("li 3", func(d []byte) { d[0] |= 0xC0 })
	bad("version 2", func(d []byte) { d[0] = 0x14 })
	bad("2019", func(d []byte) { s, _ := ntpTime(1546300800, 0); binary.BigEndian.PutUint32(d[40:], s) })
	none(t)(v.Exchange(sntpFrame(gwHost, 123, port, good()[:47])))
	none(t)(v.Exchange(sntpFrame(gwHost, 123, port+1, good())))
	none(t)(v.Exchange(sntpFrame(gwHost, 123, port, good())))
	set := strconv.FormatUint(unix2026*1000, 10)
	stop(t, v, "start true\nsync 10.0.0.1 true 100 "+set+"\nwall "+set+"\ndone\n")
}

// No valid reply: three requests (the time left shared), then false and the
// clock stays unknown; a late reply to the first one is no use.
func TestSNTPTimeout(t *testing.T) {
	v := startSNTP(t, "sleep:100 sync:10.0.0.1 wall", "3000")
	nonce, port := sntpRequest(t, one(t)(v.Advance(100)), gwHost.IP)
	_, _ = sntpRequest(t, one(t)(v.Advance(1000)), gwHost.IP)
	_, _ = sntpRequest(t, one(t)(v.Advance(1000)), gwHost.IP)
	none(t)(v.Advance(999))
	// a reply to the first request, after the 3rd
	none(t)(v.Exchange(sntpFrame(gwHost, 123, port, sntpReply(nonce, unix2026, 0))))
	none(t)(v.Advance(1))
	stop(t, v, "start true\nsync 10.0.0.1 false 3100 0\nwall 0\ndone\n")
}

// A server name is resolved with the DNS server first; a name that does not
// resolve fails.
func TestSNTPName(t *testing.T) {
	v := startSNTP(t, "sleep:100 sync:ntp.test", "")
	q, dport := dnsQuery(t, one(t)(v.Advance(100)), gwHost.IP, "ntp.test")
	fr, err := v.Exchange(dnsFrame(gwHost, dport, sim.DNSAnswer(q, false, sim.ARecord("ntp.test", gwHost.IP, 60)).Bytes()))
	nonce, port := sntpRequest(t, one(t)(fr, err), gwHost.IP)
	none(t)(v.Exchange(sntpFrame(gwHost, 123, port, sntpReply(nonce, unix2026, 500))))
	stop(t, v, "start true\nsync ntp.test true 100 "+strconv.FormatUint(unix2026*1000+500, 10)+"\ndone\n")

	v = startSNTP(t, "sleep:100 sync:nx.test", "")
	q, dport = dnsQuery(t, one(t)(v.Advance(100)), gwHost.IP, "nx.test")
	none(t)(v.Exchange(dnsFrame(gwHost, dport, sim.DNSAnswer(q, true).Bytes())))
	stop(t, v, "start true\nsync nx.test false 100 0\ndone\n")
}

// sys.SetTime / sys.Time from a program: the clock keeps its value and
// advances; the unix platform starts from $GOGO_UNIX_TIME_MS.
func TestSNTPClockEnv(t *testing.T) {
	base := strconv.FormatUint(unix2026*1000, 10)
	t.Setenv("GOGO_UNIX_TIME_MS", base)
	v := startSNTP(t, "wall sleep:50 wall set", "")
	none(t)(v.Advance(50))
	stop(t, v, "start true\nwall "+base+"\nwall "+strconv.FormatUint(unix2026*1000+50, 10)+"\nset true true\nfloor false true\nrand true\ndone\n")
	// a time before the floor is refused: the clock stays unknown
	t.Setenv("GOGO_UNIX_TIME_MS", "1700000000000")
	v = startSNTP(t, "wall", "")
	stop(t, v, "start true\nwall 0\ndone\n")
}

// A time before the build-time floor is refused (the clock stays unknown),
// and a server at 128.0.0.0 or above (a negative int) works through the
// gateway.
func TestSNTPFloorAndHighAddress(t *testing.T) {
	v := startSNTP(t, "sleep:100 sync:10.0.0.1 wall", "")
	nonce, port := sntpRequest(t, one(t)(v.Advance(100)), gwHost.IP)
	none(t)(v.Exchange(sntpFrame(gwHost, 123, port, sntpReply(nonce, 1767225600, 0)))) // 2026-01-01, before this build
	stop(t, v, "start true\nsync 10.0.0.1 false 100 0\nwall 0\ndone\n")

	far := sim.Host{MAC: gwHost.MAC, IP: sim.IPv4(200, 1, 2, 3)}
	v = startSNTP(t, "sleep:100 sync:200.1.2.3", "")
	nonce, port = sntpRequest(t, one(t)(v.Advance(100)), far.IP)
	none(t)(v.Exchange(sntpFrame(far, 123, port, sntpReply(nonce, unix2026, 0))))
	stop(t, v, "start true\nsync 200.1.2.3 true 100 "+strconv.FormatUint(unix2026*1000, 10)+"\ndone\n")
}
