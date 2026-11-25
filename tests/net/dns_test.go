package nettest

import (
	"encoding/binary"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"gogotools/tests/net/sim"
)

// startDNS starts dns_client with a static address (10.0.0.2/24, gateway
// 10.0.0.1), DNS server dns (none if zero), the script and the timeout,
// and answers the gateway's ARP.
func startDNS(t *testing.T, dns sim.IP, script, timeout string) *sim.VM {
	t.Helper()
	cfg := "net.ip=10.0.0.2\nnet.gw=10.0.0.1\ntest.script=" + script + "\n"
	if dns != anyIP {
		cfg += "net.dns=" + dns.String() + "\n"
	}
	if timeout != "" {
		cfg += "test.timeout=" + timeout + "\n"
	}
	path := filepath.Join(t.TempDir(), "config")
	if err := os.WriteFile(path, []byte(cfg), 0o600); err != nil {
		t.Fatal(err)
	}
	t.Setenv("GOGO_CONFIG", path)
	v := start(t, "dns_client")
	arpFor(t, one(t)(v.Sync()), vmHost.IP, gwHost.IP)
	none(t)(v.Exchange(arpReply(gwHost, vmHost)))
	return v
}

// dnsQuery checks p is an A query for name from the VM to server and
// returns it with the VM's port.
func dnsQuery(t *testing.T, p sim.Packet, server sim.IP, name string) (sim.DNS, uint16) {
	t.Helper()
	if p.UDP == nil || p.IP.Src != vmHost.IP || p.IP.Dst != server || p.UDP.DstPort != sim.DNSPort || p.UDP.SrcPort < 49152 {
		t.Fatalf("want a DNS query to %v, got %+v %+v", server, p.IP, p.UDP)
	}
	q, err := sim.ParseDNS(p.UDP.Payload)
	if err != nil {
		t.Fatal(err)
	}
	if q.Flags != sim.DNSFlagRD || len(q.Questions) != 1 || len(q.Answers) != 0 ||
		q.Questions[0] != (sim.DNSQuestion{Name: name, Type: sim.DNSTypeA, Class: sim.DNSClassIN}) {
		t.Fatalf("want an A query for %s, got %+v", name, q)
	}
	return q, p.UDP.SrcPort
}

// dnsFrame is a DNS message from server:53 to the VM's port.
func dnsFrame(server sim.Host, port uint16, payload []byte) []byte {
	return sim.UDPFrame(server, vmHost, sim.UDP{SrcPort: sim.DNSPort, DstPort: port, Payload: payload})
}

func ipLine(name string, ip sim.IP, ok bool, at uint64) string {
	s := name + " " + ipInt(ip) + " false "
	if ok {
		s = name + " " + ipInt(ip) + " true "
	}
	return s + itoa(at) + "\n"
}

// Lookups through the gateway's resolver: replies with the wrong id, port
// or question are ignored; a CNAME chain with compressed names is
// followed; an A record for another owner does not count; a literal address needs no query; a cached answer needs none;
// NXDOMAIN and no A record fail; a malformed name fails at once; two
// concurrent lookups are queued.
func TestDNSLookup(t *testing.T) {
	v := startDNS(t, gwHost.IP, "sleep:100 example.com 10.9.8.7 example.com nx.example noa.example bad..name a.test+b.test", "")
	q, port := dnsQuery(t, one(t)(v.Advance(100)), gwHost.IP, "example.com")
	wrongID := sim.DNSAnswer(q, false, sim.ARecord("example.com", sim.IPv4(9, 9, 9, 9), 300))
	wrongID.ID++
	none(t)(v.Exchange(dnsFrame(gwHost, port, wrongID.Bytes())))
	ans := sim.DNSAnswer(q, false, sim.ARecord("example.com", sim.IPv4(9, 9, 9, 9), 300)).Bytes()
	none(t)(v.Exchange(sim.UDPFrame(gwHost, vmHost, sim.UDP{SrcPort: 5353, DstPort: port, Payload: ans})))
	// CNAME (compressed owner name) then the A record (compressed too).
	resp := sim.DNSAnswer(q, false).Bytes()
	binary.BigEndian.PutUint16(resp[6:], 2)
	cname := []byte{1, 'x', 0xC0, 12}
	resp = append(resp, 0xC0, 12, 0, 5, 0, 1, 0, 0, 1, 0, 0, byte(len(cname)))
	resp = append(resp, cname...)
	resp = append(resp, 0xC0, byte(12+len("example.com")+2+4+12), 0, 1, 0, 1, 0, 0, 1, 0x2C, 0, 4, 93, 184, 216, 34) // owner: the CNAME's target
	// A response whose question is another name: ignored.
	other := sim.DNSAnswer(q, false, sim.ARecord("example.org", sim.IPv4(9, 9, 9, 9), 300))
	other.Questions = []sim.DNSQuestion{{Name: "example.org", Type: sim.DNSTypeA, Class: sim.DNSClassIN}}
	none(t)(v.Exchange(dnsFrame(gwHost, port, other.Bytes())))
	q, port = dnsQuery(t, one(t)(v.Exchange(dnsFrame(gwHost, port, resp))), gwHost.IP, "nx.example")
	q, port = dnsQuery(t, one(t)(v.Exchange(dnsFrame(gwHost, port, sim.DNSAnswer(q, true).Bytes()))), gwHost.IP, "noa.example")
	// An A record for another owner is not an answer.
	q, port = dnsQuery(t, one(t)(v.Exchange(dnsFrame(gwHost, port,
		sim.DNSAnswer(q, false, sim.ARecord("other.example", sim.IPv4(9, 9, 9, 9), 60)).Bytes()))), gwHost.IP, "a.test")
	q, port = dnsQuery(t, one(t)(v.Exchange(dnsFrame(gwHost, port,
		sim.DNSAnswer(q, false, sim.ARecord("a.test", sim.IPv4(1, 2, 3, 4), 60)).Bytes()))), gwHost.IP, "b.test")
	none(t)(v.Exchange(dnsFrame(gwHost, port, sim.DNSAnswer(q, false, sim.ARecord("b.test", sim.IPv4(200, 1, 2, 3), 60)).Bytes())))
	ex := sim.IPv4(93, 184, 216, 34)
	stop(t, v, "start true\n"+ipLine("example.com", ex, true, 100)+ipLine("10.9.8.7", sim.IPv4(10, 9, 8, 7), true, 100)+
		ipLine("example.com", ex, true, 100)+ipLine("nx.example", anyIP, false, 100)+ipLine("noa.example", anyIP, false, 100)+
		ipLine("bad..name", anyIP, false, 100)+ipLine("a.test", sim.IPv4(1, 2, 3, 4), true, 100)+
		ipLine("b.test", sim.IPv4(200, 1, 2, 3), true, 100)+"done\n")
}

// Answers are cached until their TTL ends, 4 at most, the one expiring
// first replaced; a TTL of 0 is not cached.
func TestDNSCacheTTL(t *testing.T) {
	v := startDNS(t, gwHost.IP, "sleep:100 a.test a.test sleep:5000 a.test a.test b.test c.test d.test e.test b.test f.test b.test c.test", "")
	// answer answers the one query in frames, for name, and returns the
	// frames the VM sent next.
	answer := func(frames [][]byte, name string, ttl uint32) [][]byte {
		t.Helper()
		q, port := dnsQuery(t, one(t)(frames, nil), gwHost.IP, name)
		fr, err := v.Exchange(dnsFrame(gwHost, port, sim.DNSAnswer(q, false, sim.ARecord(name, sim.IPv4(10, 1, 1, byte(ttl%250)), ttl)).Bytes()))
		if err != nil {
			t.Fatal(err)
		}
		return fr
	}
	advance := func(ms uint32) [][]byte {
		t.Helper()
		fr, err := v.Advance(ms)
		if err != nil {
			t.Fatal(err)
		}
		return fr
	}
	fr := answer(advance(100), "a.test", 5) // then a.test cached, then sleep
	none(t)(fr, nil)
	fr = answer(advance(5000), "a.test", 0) // expired at 5.1 s: queried again; TTL 0
	fr = answer(fr, "a.test", 0)            // not cached: queried again
	fr = answer(fr, "b.test", 100)
	fr = answer(fr, "c.test", 200)
	fr = answer(fr, "d.test", 300)
	fr = answer(fr, "e.test", 400) // replaces the expired a.test; b.test still cached
	fr = answer(fr, "f.test", 500) // replaces b.test (expires first)
	fr = answer(fr, "b.test", 100) // replaces c.test
	fr = answer(fr, "c.test", 200)
	none(t)(fr, nil)
	out := v.Stdout.String()
	if n := strings.Count(out, "\n"); n != 14 || !strings.HasSuffix(out, "done\n") {
		t.Fatalf("%d lines:\n%s", n, out)
	}
	stop(t, v, out)
}

// Unanswered queries are sent 3 times within the timeout, then Lookup
// fails; the first is lost to ARP here (an on-link server not yet
// resolved). A late answer to an old query is ignored; an answer to a
// retransmission counts.
func TestDNSTimeout(t *testing.T) {
	srv := sim.Host{MAC: otherMAC, IP: sim.IPv4(10, 0, 0, 53)}
	v := startDNS(t, srv.IP, "sleep:100 slow.test slow.test", "3000")
	arpFor(t, one(t)(v.Advance(100)), vmHost.IP, srv.IP) // the first query is dropped
	none(t)(v.Exchange(arpReply(srv, vmHost)))
	none(t)(v.Advance(999))
	q1, port := dnsQuery(t, one(t)(v.Advance(1)), srv.IP, "slow.test") // 1100: (3100-100)/3 later
	none(t)(v.Advance(999))
	q2, _ := dnsQuery(t, one(t)(v.Advance(1)), srv.IP, "slow.test") // 2100: half of the 2 s left
	if q2.ID != q1.ID {
		t.Fatalf("retransmission id %#x, want %#x", q2.ID, q1.ID)
	}
	none(t)(v.Advance(999))
	// 3100: fails; the next lookup's first query goes out at once.
	q3, port3 := dnsQuery(t, one(t)(v.Advance(1)), srv.IP, "slow.test")
	if q3.ID == q1.ID {
		t.Fatal("new lookup reuses the old id")
	}
	none(t)(v.Exchange(dnsFrame(srv, port3, sim.DNSAnswer(q1, false, sim.ARecord("slow.test", sim.IPv4(6, 6, 6, 6), 60)).Bytes())))
	none(t)(v.Exchange(dnsFrame(srv, port, sim.DNSAnswer(q3, false, sim.ARecord("slow.test", sim.IPv4(6, 6, 6, 6), 60)).Bytes()))) // old port: closed
	none(t)(v.Advance(999))
	q4, port4 := dnsQuery(t, one(t)(v.Advance(1)), srv.IP, "slow.test")
	none(t)(v.Exchange(dnsFrame(srv, port4, sim.DNSAnswer(q4, false, sim.ARecord("slow.test", sim.IPv4(7, 7, 7, 7), 60)).Bytes())))
	stop(t, v, "start true\n"+ipLine("slow.test", anyIP, false, 3100)+ipLine("slow.test", sim.IPv4(7, 7, 7, 7), true, 4100)+"done\n")
}

// Without a DNS server Lookup fails at once.
func TestDNSNoServer(t *testing.T) {
	v := startDNS(t, anyIP, "x.test", "")
	none(t)(v.Advance(1000))
	stop(t, v, "start true\n"+ipLine("x.test", anyIP, false, 0)+"done\n")
}

// Link loss (Down) flushes the cache and fails a lookup in flight at once
// rather than at its timeout; while down, lookups fail at once.
func TestDNSDownFlushes(t *testing.T) {
	v := startDNS(t, gwHost.IP, "sleep:100 a.test a.test sleep:500 a.test a.test", "3000")
	q, port := dnsQuery(t, one(t)(v.Advance(100)), gwHost.IP, "a.test")
	none(t)(v.Exchange(dnsFrame(gwHost, port, sim.DNSAnswer(q, false, sim.ARecord("a.test", sim.IPv4(1, 2, 3, 4), 300)).Bytes())))
	none(t)(v.Advance(100)) // the second lookup: cached
	none(t)(v.SetLink(false))
	arpFor(t, one(t)(v.SetLink(true)), vmHost.IP, gwHost.IP) // up again: the gateway re-resolved
	none(t)(v.Exchange(arpReply(gwHost, vmHost)))
	dnsQuery(t, one(t)(v.Advance(400)), gwHost.IP, "a.test") // 600: not cached any more: asked again
	none(t)(v.Advance(50))
	none(t)(v.SetLink(false)) // 650: in flight: fails now; the next one at once (no server)
	stop(t, v, "start true\n"+ipLine("a.test", sim.IPv4(1, 2, 3, 4), true, 100)+ipLine("a.test", sim.IPv4(1, 2, 3, 4), true, 100)+
		ipLine("a.test", anyIP, false, 650)+ipLine("a.test", anyIP, false, 650)+"done\n")
}

// A resolver whose net.Subscribe failed (all 4 slots taken) is not broken
// for good: a later lookup subscribes again (spec M5 final review).
func TestDNSResubscribe(t *testing.T) {
	cfg := "net.ip=10.0.0.2\nnet.gw=10.0.0.1\nnet.dns=" + gwHost.IP.String() + "\n"
	path := filepath.Join(t.TempDir(), "config")
	if err := os.WriteFile(path, []byte(cfg), 0o600); err != nil {
		t.Fatal(err)
	}
	t.Setenv("GOGO_CONFIG", path)
	v := start(t, "dns_resubscribe")
	// The gateway's pre-resolution, then the second lookup's first query,
	// dropped for the same ARP miss (a second request); answered at the
	// retry, 1 s later.
	fr, err := v.Sync()
	if err != nil || len(fr) != 2 {
		t.Fatalf("want 2 ARP requests, got %d frames (%v)", len(fr), err)
	}
	arpFor(t, decode(t, fr[0]), vmHost.IP, gwHost.IP)
	arpFor(t, decode(t, fr[1]), vmHost.IP, gwHost.IP)
	none(t)(v.Exchange(arpReply(gwHost, vmHost)))
	q, port := dnsQuery(t, one(t)(v.Advance(1000)), gwHost.IP, "a.test")
	none(t)(v.Exchange(dnsFrame(gwHost, port, sim.DNSAnswer(q, false, sim.ARecord("a.test", sim.IPv4(1, 2, 3, 4), 300)).Bytes())))
	stop(t, v, "start true\nsubscribed true\nsubscribed true\nsubscribed true\nsubscribed true\n"+
		"dns: net.Subscribe failed (no network or 4 subscribers): retrying on each lookup\n"+
		ipLine("a.test", anyIP, false, 0)+ipLine("a.test", sim.IPv4(1, 2, 3, 4), true, 1000)+"done\n")
}
