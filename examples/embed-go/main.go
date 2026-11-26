//go:build cgo && libgogo

// embed-go: a Go program that compiles a gogo program and runs it, with
// a host port written in Go (package gogotools/embed).
//
//	make lib
//	go run -tags libgogo ./examples/embed-go
//
// It compiles app/main.gg in process, registers the host port "upper"
// (each command comes back upper-cased), and runs the VM on a goroutine
// of its own. Meanwhile it posts three words, and four goroutines post
// ticks through posters of their own; then it ends the program with a
// last post. Its output is embed.out.
package main

import (
	"bytes"
	"context"
	_ "embed"
	"fmt"
	"log"
	"sync"

	"gogotools/embed"
)

//go:embed app/main.gg
var program []byte

func main() {
	img, err := embed.Compile(map[string][]byte{"main.gg": program}, nil)
	if err != nil {
		log.Fatal(err)
	}
	vm, err := embed.New(nil)
	if err != nil {
		log.Fatal(err)
	}
	defer vm.Close()

	ready := make(chan struct{})
	upper := embed.PortFuncs{
		OpenFunc: func(p embed.Port, arg int32, data []byte) error {
			close(ready) // the program has registered "words"
			return nil
		},
		CommandFunc: func(p embed.Port, m embed.Message) {
			p.Emit(embed.Message{Kind: m.Kind, Data: bytes.ToUpper(m.Data)})
		},
	}
	if err := vm.RegisterPort("upper", upper); err != nil { // before Load
		log.Fatal(err)
	}
	if err := vm.Load(img); err != nil {
		log.Fatal(err)
	}

	// Posters are made on the VM's goroutine, before Run takes it over.
	posters := make([]*embed.Poster, 5)
	for i := range posters {
		if posters[i], err = vm.NewPoster(); err != nil {
			log.Fatal(err)
		}
	}
	done := make(chan error, 1)
	go func() { done <- vm.Run(context.Background()) }()

	<-ready
	words := posters[4]
	for _, w := range []string{"gopher", "actor", "port"} {
		if err := words.Post("words", embed.Message{Kind: 1, Data: []byte(w)}); err != nil {
			log.Fatal(err)
		}
	}
	var wg sync.WaitGroup
	for _, p := range posters[:4] {
		wg.Add(1)
		go func(p *embed.Poster) {
			defer wg.Done()
			defer p.Close()
			for i := 0; i < 25; i++ {
				if err := p.Post("words", embed.Message{Kind: 2}); err != nil {
					log.Fatal(err)
				}
			}
		}(p)
	}
	wg.Wait()
	words.Post("words", embed.Message{Kind: 0}) // after every tick: the end
	words.Close()

	if err := <-done; err != nil {
		log.Fatal(err)
	}
	st, err := vm.Stats()
	if err != nil {
		log.Fatal(err)
	}
	fmt.Printf("posts delivered: %d, dropped: %d\n", st.PostsDelivered, st.PostsDropped)
}
