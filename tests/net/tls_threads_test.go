package nettest

import (
	"net"
	"os"
	"os/exec"
	"strconv"
	"strings"
	"testing"
	"time"
)

// Two VMs on two threads of one process run TLS handshakes at the same
// time with different wall clocks ($GOGO_TLS_THREADS, the C program
// runtime/net/tls/test/tls_threads.c; make test-tls-threads, or
// test-tls-tsan under ThreadSanitizer): mbedTLS's time hooks are
// process-global, yet each VM must check the certificate against its own
// clock. VM 0 (now) connects; VM 1 (three days on, past the certificate's
// end) gets Closed{BadCert}. Run a few times for more interleavings.
func TestTLSThreads(t *testing.T) {
	bin := os.Getenv("GOGO_TLS_THREADS")
	if bin == "" {
		t.Skip("GOGO_TLS_THREADS not set")
	}
	p := pki(t)
	cert := p.leaf(t, certOpts{host: "gogo.test", notAfter: time.Now().Add(24 * time.Hour)})
	srv := startServer(t, &cert, echoHandler)
	port := strconv.Itoa(srv.ln.Addr().(*net.TCPAddr).Port)
	for round := 0; round < 3; round++ {
		cmd := exec.Command(bin, port, p.caFile, "gogo.test")
		out, err := cmd.CombinedOutput()
		got := string(out)
		if err != nil || !strings.Contains(got, "vm0 connected\n") || !strings.Contains(got, "vm1 closed 7\n") {
			t.Fatalf("round %d: %v\n%s", round, err, got)
		}
	}
}
