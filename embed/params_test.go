//go:build cgo && libgogo

package embed

import (
	"context"
	"errors"
	"fmt"
	"strings"
	"testing"
)

// limitsSrc prints the main actor's memory limit and the keys the VM's
// sys.Config answers for the parameters.
const limitsSrc = `package main

import (
	"gogo/actor"
	"gogo/sys"
)

func show(key string) {
	v, ok := sys.Config(key)
	println(key, v, ok)
}

func main() {
	println("limit", actor.Limit(actor.Self()))
	show("vm.actor_memory")
	show("vm.max_ports")
	show("node.max_links")
	show("vm.handles")
}
`

// portsSrc opens the host port "dev" until it is refused.
const portsSrc = `package main

import (
	"gogo/errors"
	"gogo/port"
)

func main() {
	n := 0
	for n < 5000 {
		_, err := port.Open("dev", port.Host{})
		if err != nil {
			println("opened", n, err == errors.TooMany)
			return
		}
		n++
	}
	println("opened", n, "never refused")
}
`

// bytesSrc: actor a holds 4 off-heap Bytes and waits; then main takes
// them until it is stopped. Each line is "who count".
const bytesSrc = `package main

import (
	"gogo/actor"
	"gogo/bytes"
)

func big() bytes.Bytes {
	b := bytes.NewBuffer(100)
	for j := 0; j < 100; j++ {
		b.WriteByte(byte(j))
	}
	return b.Freeze()
}

func hold(who string, n int, then actor.PID) {
	var held []bytes.Bytes
	for len(held) < n {
		held = append(held, big())
		println(who, len(held))
	}
	actor.Send(then, 1)
	actor.ReceiveTimeout(200)
	println(who, "kept", len(held), held[0].Len())
}

func main() {
	self := actor.Self()
	actor.Spawn(func() { hold("a", 4, self) })
	actor.Receive()
	hold("main", 100000, self)
}
`

// runParams runs src in a VM of cfg and returns its output and
// diagnostics.
func runParams(t *testing.T, cfg *Config, src string) (string, string) {
	t.Helper()
	vm, out, errOut := newVM(t, cfg)
	if err := vm.RegisterPort("dev", PortFuncs{}); err != nil {
		t.Fatal(err)
	}
	if err := vm.Load(image(t, src)); err != nil {
		t.Fatal(err)
	}
	if err := vm.Run(context.Background()); err != nil {
		t.Fatal(err)
	}
	return out.String(), errOut.String()
}

// last is the count on the last line of out that starts with who.
func last(out, who string) int {
	n := -1
	for _, l := range strings.Split(out, "\n") {
		var c int
		if _, err := fmt.Sscanf(l, who+" %d", &c); err == nil {
			n = c
		}
	}
	return n
}

// Each field reaches the VM: an actor's limit, the keys sys.Config
// answers, the port table, the Bytes handles VM-wide and per actor.
func TestParamsReachVM(t *testing.T) {
	cfg := &Config{ActorMemory: 64 << 10, MaxPorts: 5, MaxLinks: 16, BytesHandles: 6, BytesPerActor: 5}
	out, errOut := runParams(t, cfg, limitsSrc)
	want := "limit 65536\nvm.actor_memory 65536 true\nvm.max_ports 5 true\nnode.max_links 16 true\nvm.handles 6/5 true\n"
	if out != want {
		t.Errorf("output %q, want %q", out, want)
	}
	if errOut != "" {
		t.Errorf("diagnostics %q", errOut)
	}

	if out, _ := runParams(t, &Config{MaxPorts: 5}, portsSrc); out != "opened 5 true\n" {
		t.Errorf("ports: %q", out)
	}

	// 5 per actor: main alone stops at 5.
	out, _ = runParams(t, &Config{BytesHandles: 100, BytesPerActor: 5}, strings.Replace(bytesSrc, `hold("a", 4, self)`, `actor.Send(self, 1)`, 1))
	if n := last(out, "main"); n != 5 {
		t.Errorf("per actor 5: main held %d\n%s", n, out)
	}
	// 6 in the VM, a holding 4 of them: main gets 2.
	out, _ = runParams(t, &Config{BytesHandles: 6, BytesPerActor: 5}, bytesSrc)
	if last(out, "a") != 4 || last(out, "main") != 2 || !strings.Contains(out, "a kept 4 100\n") {
		t.Errorf("VM-wide 6: %q", out)
	}
}

// The zero Config keeps the defaults: 1 MB per actor, 1024 ports, 256
// Bytes per actor, and nothing said.
func TestParamsDefaults(t *testing.T) {
	out, errOut := runParams(t, nil, limitsSrc)
	want := "limit 1048576\nvm.actor_memory  false\nvm.max_ports  false\nnode.max_links  false\nvm.handles  false\n"
	if out != want {
		t.Errorf("output %q, want %q", out, want)
	}
	if errOut != "" {
		t.Errorf("diagnostics %q", errOut)
	}
	if out, _ := runParams(t, nil, portsSrc); out != "opened 1024 true\n" {
		t.Errorf("ports: %q", out)
	}
	out, _ = runParams(t, nil, strings.Replace(bytesSrc, `hold("a", 4, self)`, `actor.Send(self, 1)`, 1))
	if n := last(out, "main"); n != 256 {
		t.Errorf("per actor: main held %d", n)
	}
	// Ports default to what MaxActors leaves, silently.
	if out, errOut := runParams(t, &Config{MaxActors: 65520}, portsSrc); out != "opened 16 true\n" || errOut != "" {
		t.Errorf("ports beside 65520 actors: %q, %q", out, errOut)
	}
}

// A field beats the host's config key of its parameter, which beats the
// default.
func TestParamsHostKeys(t *testing.T) {
	keys := Host{Config: func(key string) ([]byte, bool) {
		if key == "vm.actor_memory" {
			return []byte("64K"), true
		}
		return nil, false
	}}
	out, _ := runParams(t, &Config{Host: keys}, limitsSrc)
	if !strings.HasPrefix(out, "limit 65536\n") {
		t.Errorf("host key: %q", out)
	}
	out, _ = runParams(t, &Config{Host: keys, ActorMemory: 128 << 10}, limitsSrc)
	if !strings.HasPrefix(out, "limit 131072\nvm.actor_memory 131072 true\n") {
		t.Errorf("field over host key: %q", out)
	}
}

// Out of range: the sizes and handle counts are clamped with a line on
// stream 2; the table sizes gogo_new cannot honour are ErrInval.
func TestParamsOutOfRange(t *testing.T) {
	for _, c := range []struct {
		cfg   Config
		limit int
		said  string
	}{
		{Config{ActorMemory: 100}, 1024, "gogo: vm.actor_memory 100 is under 1K; using 1K\n"},
		{Config{ActorMemory: 2 << 30}, 1 << 30, "gogo: vm.actor_memory 2147483648 is over this VM's 1G; using 1G\n"},
		{Config{BytesHandles: 70000}, 1 << 20, "gogo: vm.handles 70000 is over this VM's 65535; using 65535\n"},
		{Config{BytesHandles: 8, BytesPerActor: 9}, 1 << 20, "gogo: vm.handles per actor 9 is over this VM's 8; using 8\n"},
	} {
		cfg := c.cfg
		out, errOut := runParams(t, &cfg, limitsSrc)
		if !strings.HasPrefix(out, fmt.Sprintf("limit %d\n", c.limit)) || errOut != c.said {
			t.Errorf("%+v: output %q, said %q, want %q", c.cfg, out, errOut, c.said)
		}
	}
	// The clamped per-actor count is the one enforced.
	out, _ := runParams(t, &Config{BytesHandles: 3, BytesPerActor: 9}, strings.Replace(bytesSrc, `hold("a", 4, self)`, `actor.Send(self, 1)`, 1))
	if n := last(out, "main"); n != 3 {
		t.Errorf("per actor 9 of 3: main held %d", n)
	}

	for _, cfg := range []Config{
		{MaxPorts: 2049},
		{MaxLinks: 2049},
		{MaxActors: 65000, MaxPorts: 1000},
		{MaxActors: 65521},
	} {
		if vm, err := New(&cfg); !errors.Is(err, ErrInval) {
			t.Errorf("New(%+v): %v", cfg, err)
			if err == nil {
				vm.Close()
			}
		}
	}
	// At the ceilings: accepted.
	for _, cfg := range []Config{{MaxPorts: 2048, MaxLinks: 2048}, {MaxActors: 63488, MaxPorts: 2048}} {
		vm, err := New(&cfg)
		if err != nil {
			t.Errorf("New(%+v): %v", cfg, err)
			continue
		}
		vm.Close()
	}
}
