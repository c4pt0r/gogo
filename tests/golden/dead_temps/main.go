package main

import "gogo/actor"

// Dead registers are not GC roots: a worker with a 2400-byte limit can
// build and drop a 100-node chain (about 1600 bytes) again and again.

type Node struct {
	V    int
	Next any
}

func chain(n int) any {
	var x any
	for i := 0; i < n; i++ {
		x = Node{i, x}
	}
	return x
}

func keep(x any) any { return x }

func twice() {
	_ = chain(100)
	_ = chain(100)
	println("done")
}

func exprStmt() {
	keep(chain(100))
	keep(chain(100))
	println("done")
}

func loop() {
	for i := 0; i < 50; i++ {
		c := chain(100)
		_ = c
	}
	println("done")
}

func loopBreak() {
	for i := 0; i < 50; i++ {
		c := chain(100)
		if i == 2 {
			_ = c
			break
		}
	}
	_ = chain(100)
	println("done")
}

func block() {
	{
		c := chain(100)
		_ = c
	}
	_ = chain(100)
	println("done")
}

// run spawns w with the 2400-byte limit and waits for it to finish.
func run(w func()) {
	ref := actor.Monitor(actor.SpawnOpt(w, actor.Options{MaxMemory: 2400}))
	for {
		switch m := actor.Receive().(type) {
		case actor.DownMsg:
			if m.Ref == ref {
				return
			}
		}
	}
}

func main() {
	run(twice)
	run(exprStmt)
	run(loop)
	run(loopBreak)
	run(block)
}
