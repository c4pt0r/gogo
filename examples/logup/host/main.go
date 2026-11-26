// Command host is the collector end of examples/logup: it takes the log
// batches a board uploads and prints them as they land, oldest first, with
// the time each line arrived. It is deliberately small enough to read: one
// HTTP handler, one line of output per log line.
//
//	host (Linux or macOS)                       board
//	---------------------                       -----
//	go run ./examples/logup/host -addr :8080
//	                                            gogo config set logup.url \
//	                                              'http://<host-ip>:8080/ingest?board=cardputer'
//	                                            gogo flash examples/logup
//
//	go run ./examples/logup/host -stdin         # or over the serial link, no network:
//	                                            gogo monitor -reset -timeout 60 |
//	                                              go run ./examples/logup/host -stdin
//
//	GET  /            a one-screen help page (what this binary expects)
//	POST /ingest      text/plain, one log line per line; ?board=NAME or an
//	                  X-Board header says which board sent it
//	GET  /stats       the counters as JSON: lines, batches, bytes, and the
//	                  age of each board's last batch
//
// With -out DIR each line is also appended to DIR/<board>-<YYYY-MM-DD>.log,
// prefixed with its arrival time, so a run outlives the terminal. Ctrl-C
// prints the totals and exits.
package main

import (
	"bufio"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"log"
	"net/http"
	"os"
	"os/signal"
	"path/filepath"
	"strings"
	"sync"
	"syscall"
	"time"
)

// maxBatch caps one request body or one stdin line: the board sends a few
// lines at a time, so anything bigger is a mistake or an attack, and
// reading it all would grow the collector's memory.
const maxBatch = 64 << 10

type board struct {
	lines    int
	batches  int
	bytes    int
	lastSeen time.Time
}

// tally is the collector's state. The HTTP server is concurrent, so the
// mutex is what keeps two boards' batches from landing on one another.
type tally struct {
	mu     sync.Mutex
	boards map[string]*board
	lines  int
	bytes  int
	begun  time.Time
	out    string
}

// emit prints one log line and appends it to that board's file. The file is
// opened per line: simple, and fine for a few boards at a few lines a second;
// a busier collector would keep one handle per board and day. A line
// starting with "# " is the board's own status (a failed post, its
// summary), not data: it prints indented and does not count as a log line.
func (t *tally) emit(name, line string, at time.Time) {
	t.mu.Lock()
	defer t.mu.Unlock()
	b := t.boards[name]
	if b == nil {
		b = &board{}
		t.boards[name] = b
	}
	if strings.HasPrefix(line, "# ") {
		fmt.Printf("%s %14s   -> %s\n", at.Format("15:04:05.000"), name, line[2:])
	} else {
		b.lines++
		t.lines++
		fmt.Printf("%s %14s   %s\n", at.Format("15:04:05.000"), name, line)
	}
	b.bytes += len(line) + 1
	t.bytes += len(line) + 1
	b.lastSeen = at
	if t.out == "" {
		return
	}
	p := filepath.Join(t.out, fmt.Sprintf("%s-%s.log", name, at.Format("2006-01-02")))
	f, err := os.OpenFile(p, os.O_APPEND|os.O_CREATE|os.O_WRONLY, 0o644)
	if err != nil {
		fmt.Printf("%s %14s   !! %s\n", at.Format("15:04:05.000"), name, err)
		return
	}
	defer f.Close()
	fmt.Fprintf(f, "%s %s\n", at.Format("2006-01-02 15:04:05.000"), line)
}

// batch counts one delivery: one POST, or one line in -stdin mode.
func (t *tally) batch(name string) {
	t.mu.Lock()
	defer t.mu.Unlock()
	b := t.boards[name]
	if b == nil {
		b = &board{}
		t.boards[name] = b
	}
	b.batches++
	b.lastSeen = time.Now()
}

// clean keeps a board name to one file name's worth of characters.
func clean(s string) string {
	return strings.Map(func(c rune) rune {
		switch {
		case c == '/' || c == '\\' || c == ':' || c == ' ' || c == '.':
			return '-'
		case c < 0x20:
			return -1
		}
		return c
	}, s)
}

// ingest takes one batch. The reply body is the number of lines it kept, so
// the board's own "delivered" line says something checkable.
func (t *tally) ingest(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "POST text/plain lines to /ingest\n", http.StatusMethodNotAllowed)
		return
	}
	name := clean(r.URL.Query().Get("board"))
	if name == "" {
		name = clean(r.Header.Get("X-Board"))
	}
	if name == "" {
		name = "board"
	}
	body, err := io.ReadAll(io.LimitReader(r.Body, maxBatch+1))
	if err != nil {
		http.Error(w, "read: "+err.Error()+"\n", http.StatusBadRequest)
		return
	}
	if len(body) > maxBatch {
		// Refuse rather than keep a cut batch: a 200 would tell the board
		// that every line arrived.
		http.Error(w, "batch over 64 KB\n", http.StatusRequestEntityTooLarge)
		return
	}
	t.batch(name)
	at := time.Now()
	n := 0
	for _, line := range strings.Split(strings.TrimRight(string(body), "\r\n"), "\n") {
		if line = strings.TrimRight(line, "\r"); line != "" {
			t.emit(name, line, at)
			n++
		}
	}
	fmt.Fprintf(w, "%d\n", n)
}

func (t *tally) stats(w http.ResponseWriter, r *http.Request) {
	type one struct {
		Lines   int    `json:"lines"`
		Batches int    `json:"batches"`
		Bytes   int    `json:"bytes"`
		LastAge string `json:"last_batch_age"`
	}
	t.mu.Lock()
	out := struct {
		Uptime string         `json:"uptime"`
		Lines  int            `json:"lines"`
		Bytes  int            `json:"bytes"`
		Boards map[string]one `json:"boards"`
	}{time.Since(t.begun).Round(time.Second).String(), t.lines, t.bytes, map[string]one{}}
	for name, b := range t.boards {
		age := "never"
		if !b.lastSeen.IsZero() {
			age = time.Since(b.lastSeen).Round(time.Second).String()
		}
		out.Boards[name] = one{b.lines, b.batches, b.bytes, age}
	}
	t.mu.Unlock()
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(out)
}

func (t *tally) help(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "text/plain; charset=utf-8")
	io.WriteString(w, `gogo log collector

  POST /ingest?board=NAME   text/plain, one log line per line
  GET  /stats               counters as JSON

On the board:
  gogo config set logup.url 'http://<this host>:<port>/ingest?board=NAME'
`)
}

// stdinMode reads the board's own output, as `gogo monitor` prints it, so
// the serial link collects the log with no network at all. Lines the
// monitor itself prints (boot banners) pass through as they are: a "# "
// prefix already marks the board's status.
func (t *tally) stdinMode(name string) {
	sc := bufio.NewScanner(os.Stdin)
	sc.Buffer(make([]byte, 4096), maxBatch)
	for sc.Scan() {
		line := strings.TrimRight(sc.Text(), "\r")
		if line == "" {
			continue
		}
		t.batch(name)
		t.emit(name, line, time.Now())
	}
}

func (t *tally) report() {
	t.mu.Lock()
	defer t.mu.Unlock()
	fmt.Printf("\ntotal: %d lines, %d bytes", t.lines, t.bytes)
	for name, b := range t.boards {
		fmt.Printf("\n  %s: %d lines, %d batches, %d bytes", name, b.lines, b.batches, b.bytes)
	}
	fmt.Println()
}

func main() {
	addr := flag.String("addr", ":8080", "address to serve /ingest on")
	out := flag.String("out", "", "append every line to a per-board file in DIR")
	def := flag.String("board", "cardputer", "board name for -stdin")
	in := flag.Bool("stdin", false, "read the board's lines from stdin instead of serving")
	flag.Parse()

	t := &tally{boards: map[string]*board{}, begun: time.Now(), out: *out}
	if t.out != "" {
		if err := os.MkdirAll(t.out, 0o755); err != nil {
			log.Fatal(err)
		}
	}
	go func() {
		c := make(chan os.Signal, 1)
		signal.Notify(c, os.Interrupt, syscall.SIGTERM)
		<-c
		t.report()
		os.Exit(0)
	}()

	if *in {
		t.stdinMode(*def)
		t.report()
		return
	}
	mux := http.NewServeMux()
	mux.HandleFunc("/ingest", t.ingest)
	mux.HandleFunc("/stats", t.stats)
	mux.HandleFunc("/", t.help)
	fmt.Printf("gogo log collector on http://0.0.0.0%s/ingest (Ctrl-C to stop)\n", *addr)
	if *out != "" {
		fmt.Printf("writing %s/<board>-<date>.log\n", *out)
	}
	// The collector listens on the LAN: a client that opens a connection and
	// never finishes its request must not hold it forever.
	srv := &http.Server{
		Addr:              *addr,
		Handler:           mux,
		ReadHeaderTimeout: 5 * time.Second,
		ReadTimeout:       30 * time.Second,
		WriteTimeout:      30 * time.Second,
		IdleTimeout:       2 * time.Minute,
	}
	log.Fatal(srv.ListenAndServe())
}
