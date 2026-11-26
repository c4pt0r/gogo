//go:build cgo && libgogo

package embed

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"os"
	"strings"
	"sync"
	"testing"
	"time"
)

// image compiles one source file of package main.
func image(t testing.TB, src string) []byte {
	t.Helper()
	img, err := Compile(map[string][]byte{"main.gg": []byte(src)}, nil)
	if err != nil {
		t.Fatal(err)
	}
	return img
}

// syncBuffer is an output writer the test may read while the VM runs.
type syncBuffer struct {
	mu sync.Mutex
	b  bytes.Buffer
	// ready, if not nil, is closed at the first write containing mark.
	mark  string
	ready chan struct{}
}

func (s *syncBuffer) Write(p []byte) (int, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.b.Write(p)
	if s.ready != nil && strings.Contains(s.b.String(), s.mark) {
		close(s.ready)
		s.ready = nil
	}
	return len(p), nil
}

func (s *syncBuffer) String() string {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.b.String()
}

// newVM makes a VM whose output goes to the returned buffers, closed
// with the test, which checks that it gave back every byte.
func newVM(t *testing.T, cfg *Config) (*VM, *syncBuffer, *syncBuffer) {
	t.Helper()
	out, errOut := &syncBuffer{}, &syncBuffer{}
	if cfg == nil {
		cfg = &Config{}
	}
	cfg.Host.Stdout, cfg.Host.Stderr = out, errOut
	vm, err := New(cfg)
	if err != nil {
		t.Fatal(err)
	}
	st := vm.st
	t.Cleanup(func() {
		vm.Close()
		if n := st.live.Load(); n != 0 {
			t.Errorf("after Close the VM still holds %d bytes of host memory (-1: not destroyed)", n)
		}
	})
	return vm, out, errOut
}

func TestRunHello(t *testing.T) {
	vm, out, _ := newVM(t, nil)
	if err := vm.Load(image(t, "package main\n\nfunc main() {\n\tprintln(\"hello\", 6*7)\n}\n")); err != nil {
		t.Fatal(err)
	}
	if err := vm.Run(context.Background()); err != nil {
		t.Fatal(err)
	}
	if got := out.String(); got != "hello 42\n" {
		t.Errorf("output %q", got)
	}
	if s, err := vm.Step(0); s != Done || err != nil {
		t.Errorf("Step after the end: %v, %v", s, err)
	}
}

func TestErrors(t *testing.T) {
	vm, _, errOut := newVM(t, nil)
	if err := vm.Start(); !errors.Is(err, ErrState) {
		t.Errorf("Start before Load: %v", err)
	}
	err := vm.Load([]byte("not an image"))
	if !errors.Is(err, ErrImage) {
		t.Fatalf("Load of junk: %v", err)
	}
	var en Errno
	if !errors.As(err, &en) || en.Code() != CodeBadImage {
		t.Errorf("Load of junk: %v is not Errno %d", err, -CodeBadImage)
	}
	if !strings.HasPrefix(err.Error(), "gogo: load: ") {
		t.Errorf("message %q", err.Error())
	}
	if errOut.String() == "" {
		t.Errorf("no diagnostic on stream 2")
	}
	if ErrInval.Error() == "" || CodeIO.String() == ErrInval.Error() {
		t.Errorf("strerror: %q, %q", ErrInval.Error(), CodeIO.String())
	}
	if err := vm.Close(); err != nil {
		t.Fatal(err)
	}
	if err := vm.Load(image(t, "package main\n\nfunc main() {}\n")); !errors.Is(err, ErrClosed) {
		t.Errorf("Load after Close: %v", err)
	}
	if err := vm.Close(); err != nil {
		t.Errorf("second Close: %v", err)
	}
	if _, err := New(&Config{Host: Host{UnixTime: func() int64 { return 0 }}}); err == nil {
		t.Errorf("New with UnixTime alone")
	}
	if _, err := New(&Config{FSQuota: -1}); !errors.Is(err, ErrInval) {
		t.Errorf("New with a negative quota: %v", err)
	}
}

func TestVersions(t *testing.T) {
	if LibraryVersion()>>8 != 2 {
		t.Errorf("library version %#x", LibraryVersion())
	}
	if ImageFormat()>>16 == 0 {
		t.Errorf("image format %#x", ImageFormat())
	}
	if CodeNoConnection != sharedLast {
		t.Errorf("the Code list ends at %d, gogo_errors.h at %d", CodeNoConnection, sharedLast)
	}
	// Every shared code has a message of its own.
	if CodeNoConnection.String() == ErrState.Error() || CodeNoConnection.String() == "unknown error" {
		t.Errorf("CodeNoConnection: %q", CodeNoConnection.String())
	}
}

// The examples/hostecho program against a Go driver that does what
// examples/embed-c/port.c does: its output is the example's embed.out.
func TestHostPort(t *testing.T) {
	img, err := CompileDir("../examples/hostecho", nil)
	if err != nil {
		t.Fatal(err)
	}
	reqs, err := ImageRequires(img)
	if err != nil || len(reqs) != 1 || reqs[0] != "echo" {
		t.Fatalf("requires %q, %v", reqs, err)
	}
	info, err := GetImageInfo(img)
	if err != nil || info.Requires != 1 || info.Exports < 1 || info.Format>>16 != ImageFormat()>>16 {
		t.Errorf("info %+v, %v", info, err)
	}
	// Unaligned: copied before the library reads it.
	odd := append([]byte{0}, img...)[1:]
	if r, err := ImageRequires(odd); err != nil || len(r) != 1 {
		t.Errorf("unaligned image: %q, %v", r, err)
	}

	vm, out, _ := newVM(t, nil)
	if err := vm.Load(img); !errors.Is(err, ErrRequires) {
		t.Fatalf("Load without the port: %v", err)
	}
	commands, opens, closes := 0, 0, 0
	echo := PortFuncs{
		OpenFunc: func(p Port, arg int32, data []byte) error {
			opens++
			if arg < 0 {
				return ErrInval
			}
			return p.Emit(Message{arg, data})
		},
		CommandFunc: func(p Port, m Message) {
			commands++
			if m.Kind == 100 {
				if err := p.Close(CodeIO); err != nil {
					t.Error(err)
				}
				return
			}
			if err := p.Emit(Message{m.Kind + 1, m.Data}); err != nil {
				t.Error(err)
			}
		},
		CloseFunc: func(p Port) { closes++ },
	}
	if err := vm.RegisterPort("echo", echo); err != nil {
		t.Fatal(err)
	}
	if err := vm.RegisterPort("echo", echo); !errors.Is(err, ErrExist) {
		t.Errorf("second registration: %v", err)
	}
	if err := vm.Load(img); err != nil {
		t.Fatal(err)
	}
	if err := vm.RegisterPort("late", echo); !errors.Is(err, ErrState) {
		t.Errorf("registration after Load: %v", err)
	}
	if err := vm.Run(context.Background()); err != nil {
		t.Fatal(err)
	}
	want, err := os.ReadFile("../examples/hostecho/embed.out")
	if err != nil {
		t.Fatal(err)
	}
	if got := out.String(); got != string(want) {
		t.Errorf("output:\n%s\nwant:\n%s", got, want)
	}
	if commands != 5 || opens != 3 || closes != 2 {
		t.Errorf("%d commands, %d opens, %d closes; want 5, 3, 2", commands, opens, closes)
	}
}

// A panic in a driver comes out of the call that ran it, and Close is
// refused inside one.
func TestDriverPanicAndBusy(t *testing.T) {
	src := `package main

import (
	"gogo/actor"
	"gogo/port"
)

func main() {
	p, ok := port.Open("dev", port.Host{Arg: 1})
	println("open", ok)
	actor.Send(p, port.Post{Kind: 2})
}
`
	vm, _, _ := newVM(t, nil)
	var busy error
	if err := vm.RegisterPort("dev", PortFuncs{
		OpenFunc:    func(p Port, arg int32, data []byte) error { busy = p.vm.Close(); return nil },
		CommandFunc: func(p Port, m Message) { panic(fmt.Sprint("command ", m.Kind)) },
	}); err != nil {
		t.Fatal(err)
	}
	if err := vm.Load(image(t, src)); err != nil {
		t.Fatal(err)
	}
	var got any
	func() {
		defer func() { got = recover() }()
		vm.Run(context.Background())
	}()
	if got != "command 2" {
		t.Errorf("panic %v", got)
	}
	if !errors.Is(busy, ErrBusy) {
		t.Errorf("Close from Open: %v", busy)
	}
}

// Posts from many goroutines, each through a poster of its own, all
// arrive; a post after the program has finished fails.
func TestPostersConcurrent(t *testing.T) {
	const goroutines, each = 16, 200
	src := `package main

import (
	"gogo/actor"
	"gogo/port"
)

func main() {
	actor.Register("sink", actor.Self())
	println("ready")
	n, sum := 0, 0
	for {
		switch p := actor.Receive().(type) {
		case port.Post:
			if p.Kind == 0 {
				println("count", n, "sum", sum, "bytes", p.Data.Len())
				return
			}
			n++
			sum += p.Kind
		}
	}
}
`
	vm, out, _ := newVM(t, nil)
	ready := make(chan struct{})
	out.mark, out.ready = "ready", ready
	if err := vm.Load(image(t, src)); err != nil {
		t.Fatal(err)
	}
	posters := make([]*Poster, goroutines+1)
	for i := range posters {
		p, err := vm.NewPoster()
		if err != nil {
			t.Fatal(err)
		}
		posters[i] = p
	}
	done := make(chan error, 1)
	go func() { done <- vm.Run(context.Background()) }() // the VM's goroutine from here on

	<-ready
	var wg sync.WaitGroup
	for g := 0; g < goroutines; g++ {
		wg.Add(1)
		go func(p *Poster) {
			defer wg.Done()
			defer p.Close()
			for i := 1; i <= each; i++ {
				for {
					err := p.Post("sink", Message{Kind: int32(i)})
					if err == nil {
						break
					}
					if !errors.Is(err, ErrFull) {
						t.Error(err)
						return
					}
					time.Sleep(50 * time.Microsecond)
				}
			}
		}(posters[g])
	}
	wg.Wait()
	last := posters[goroutines]
	if err := last.Post("sink", Message{Kind: 0, Data: []byte("end")}); err != nil {
		t.Fatal(err)
	}
	if err := <-done; err != nil {
		t.Fatal(err)
	}
	want := fmt.Sprintf("ready\ncount %d sum %d bytes 3\n", goroutines*each, goroutines*each*(each+1)/2)
	if got := out.String(); got != want {
		t.Errorf("output %q, want %q", got, want)
	}
	st, err := vm.Stats()
	if err != nil || st.PostsDelivered != goroutines*each+1 || st.PostsDropped != 0 {
		t.Errorf("stats %+v, %v", st, err)
	}
	if err := last.Post("sink", Message{Kind: 1}); !errors.Is(err, ErrClosed) {
		t.Errorf("post after the end: %v", err)
	}
	// The poster outlives the VM: its memory stays until the poster goes.
	vmst := vm.st
	vm.Close()
	if n := vmst.live.Load(); n != -1 {
		t.Errorf("destroyed while a poster holds the VM (%d)", n)
	}
	if err := last.Post("sink", Message{Kind: 1}); !errors.Is(err, ErrClosed) {
		t.Errorf("post after Close: %v", err)
	}
	last.Close()
	last.Close() // a second Close does nothing
	if err := last.Post("sink", Message{}); !errors.Is(err, ErrClosed) {
		t.Errorf("post on a closed poster: %v", err)
	}
}

// Closing posts from another goroutine while the VM runs ends a program
// that only waits for them; Close during posting is safe.
func TestClosePostsRace(t *testing.T) {
	src := `package main

import "gogo/actor"

func main() {
	actor.Register("x", actor.Self())
	for {
		actor.Receive()
	}
}
`
	for round := 0; round < 20; round++ {
		vm, _, _ := newVM(t, nil)
		if err := vm.Load(image(t, src)); err != nil {
			t.Fatal(err)
		}
		var ps []*Poster
		for i := 0; i < 4; i++ {
			p, err := vm.NewPoster()
			if err != nil {
				t.Fatal(err)
			}
			ps = append(ps, p)
		}
		var wg sync.WaitGroup
		for _, p := range ps {
			wg.Add(1)
			go func(p *Poster) {
				defer wg.Done()
				for i := 0; i < 100; i++ {
					if err := p.Post("x", Message{Kind: 1, Data: []byte("data")}); errors.Is(err, ErrClosed) {
						break
					}
				}
				p.ClosePosts()
				p.Close()
			}(p)
		}
		if round%2 == 0 {
			if err := vm.Run(context.Background()); err != nil {
				t.Fatal(err)
			}
		} else {
			vm.Step(10)
			vm.Close() // while the posters may still post
		}
		wg.Wait()
	}
}

// A host that steps the VM itself: Idle with a deadline while it sleeps.
func TestStepDeadline(t *testing.T) {
	vm, out, _ := newVM(t, nil)
	if err := vm.Load(image(t, "package main\n\nimport \"gogo/actor\"\n\nfunc main() {\n\tprintln(\"a\")\n\tactor.Sleep(30)\n\tprintln(\"b\")\n}\n")); err != nil {
		t.Fatal(err)
	}
	if _, err := vm.Step(0); !errors.Is(err, ErrState) {
		t.Errorf("Step before Start: %v", err)
	}
	if err := vm.Start(); err != nil {
		t.Fatal(err)
	}
	idle := 0
	for {
		select {
		case <-vm.Wake():
		default:
		}
		s, err := vm.Step(0)
		if err != nil {
			t.Fatal(err)
		}
		if s == Done {
			break
		}
		if s == Idle {
			idle++
			d, ok := vm.NextDeadline()
			if !ok {
				t.Fatal("idle with no deadline")
			}
			if w := d - vm.Now(); w > 0 {
				select {
				case <-vm.Wake():
				case <-time.After(time.Duration(w) * time.Millisecond):
				}
			}
		}
	}
	if idle == 0 || out.String() != "a\nb\n" {
		t.Errorf("idle %d times, output %q", idle, out.String())
	}
}

// Run ends with its context, even while an actor spins, and the VM
// stays usable.
func TestRunContext(t *testing.T) {
	vm, _, _ := newVM(t, nil)
	if err := vm.Load(image(t, "package main\n\nfunc main() {\n\tfor {\n\t}\n}\n")); err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 50*time.Millisecond)
	defer cancel()
	if err := vm.Run(ctx); !errors.Is(err, context.DeadlineExceeded) {
		t.Fatalf("Run: %v", err)
	}
	if s, err := vm.Step(10); s != Runnable || err != nil {
		t.Errorf("Step after Run: %v, %v", s, err)
	}
}

// The Host's hooks reach the program.
func TestHostHooks(t *testing.T) {
	src := `package main

import (
	"gogo/actor"
	"gogo/sys"
)

func main() {
	v, ok := sys.Config("name")
	_, ok2 := sys.Config("other")
	println(v, ok, ok2, sys.Random(), sys.Time())
	println(sys.SetTime(1800000000000), sys.Time())
	t0 := actor.Now()
	actor.Sleep(1000)
	println(actor.Now() - t0)
}
`
	var mu sync.Mutex
	now, unix := int64(1000), int64(1_700_000_000_000)
	cfg := &Config{Host: Host{
		Now:         func() int64 { mu.Lock(); defer mu.Unlock(); return now },
		UnixTime:    func() int64 { return unix },
		SetUnixTime: func(ms int64) error { unix = ms; return nil },
		Random:      func() uint32 { return 7 },
		Config: func(key string) ([]byte, bool) {
			if key == "name" {
				return []byte("gogo"), true
			}
			return nil, false
		},
	}}
	vm, out, _ := newVM(t, cfg)
	if err := vm.Load(image(t, src)); err != nil {
		t.Fatal(err)
	}
	if err := vm.Start(); err != nil {
		t.Fatal(err)
	}
	// A clock the host moves: jump to each deadline.
	for {
		s, err := vm.Step(0)
		if err != nil {
			t.Fatal(err)
		}
		if s == Done {
			break
		}
		if d, ok := vm.NextDeadline(); s == Idle && ok {
			mu.Lock()
			now = d
			mu.Unlock()
		}
	}
	want := "gogo true false 7 1700000000000\ntrue 1800000000000\n1000\n"
	if got := out.String(); got != want {
		t.Errorf("output %q, want %q", got, want)
	}
}

// Every byte a VM takes from the host comes back at Close, also when the
// program is stopped in the middle of its work, with posts queued and a
// grant list.
func TestCloseFreesAll(t *testing.T) {
	src := `package main

import (
	"gogo/actor"
	"gogo/bytes"
)

func main() {
	actor.Register("x", actor.Self())
	for i := 0; i < 8; i++ {
		actor.Spawn(func() {
			b := bytes.NewBuffer(1000)
			for {
				b.WriteByte(1)
				actor.Sleep(1)
			}
		})
	}
	for {
		actor.Receive()
	}
}
`
	vm, _, _ := newVM(t, &Config{Grant: []string{"/flash", "echo"}, MemLimit: 4 << 20})
	if err := vm.Load(image(t, src)); err != nil {
		t.Fatal(err)
	}
	if err := vm.Start(); err != nil {
		t.Fatal(err)
	}
	for i := 0; i < 50; i++ {
		vm.Step(100)
	}
	for i := 0; i < 10; i++ {
		if err := vm.Post("x", Message{Kind: 1, Data: make([]byte, 5000)}); err != nil {
			t.Fatal(err)
		}
	}
	st, err := vm.Stats()
	if err != nil || st.MemUsed == 0 {
		t.Errorf("stats %+v, %v", st, err)
	}
	// newVM's cleanup closes the VM and checks the count.
}

// A panic in a driver's Close comes out of VM.Close with the VM freed:
// a second Close does nothing and posts fail. Inside a hook a nested VM
// call is refused and leaves Close refused too.
func TestCloseAfterDriverPanic(t *testing.T) {
	src := `package main

import (
	"gogo/actor"
	"gogo/port"
)

func main() {
	port.Open("dev", port.Host{})
	actor.Register("x", actor.Self())
	for {
		actor.Receive()
	}
}
`
	vm, _, _ := newVM(t, nil)
	var nested, busy error
	if err := vm.RegisterPort("dev", PortFuncs{
		OpenFunc: func(p Port, arg int32, data []byte) error {
			nested = p.vm.RegisterPort("other", PortFuncs{})
			_, err := p.vm.Step(1)
			if err == nil {
				t.Error("Step inside a hook succeeded")
			}
			busy = p.vm.Close()
			return nil
		},
		CloseFunc: func(p Port) {
			if err := p.Emit(Message{Kind: 1}); !errors.Is(err, ErrClosed) {
				t.Errorf("Emit while closing: %v", err)
			}
			panic("close")
		},
	}); err != nil {
		t.Fatal(err)
	}
	if err := vm.Load(image(t, src)); err != nil {
		t.Fatal(err)
	}
	if err := vm.Start(); err != nil {
		t.Fatal(err)
	}
	vm.Step(0)
	if !errors.Is(nested, ErrBusy) || !errors.Is(busy, ErrBusy) {
		t.Errorf("nested RegisterPort: %v; Close after it: %v", nested, busy)
	}
	var got any
	func() {
		defer func() { got = recover() }()
		vm.Close()
	}()
	if got != "close" {
		t.Errorf("panic %v", got)
	}
	if err := vm.Close(); err != nil {
		t.Errorf("second Close: %v", err)
	}
	if err := vm.Post("x", Message{}); !errors.Is(err, ErrClosed) {
		t.Errorf("Post after Close: %v", err)
	}
	if len(vm.ports) != 0 {
		t.Errorf("port handles kept")
	}
}
