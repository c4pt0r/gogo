// greeter: answers messages from the program that embeds the VM.
//
// It registers the name "greeter"; the host sends it port.Post messages
// with gogo_post (runtime/include/gogo.h). examples/embed-c/post.c posts
// names to it from its main thread and from a second thread, then closes
// posts, and the program ends. Its output under post.c is embed.out; on a
// board or under gogovm nothing posts, and it waits forever.
package main

import (
	"gogo/actor"
	"gogo/port"
)

func main() {
	actor.Register("greeter", actor.Self())
	println("greeter ready")
	for {
		switch p := actor.Receive().(type) {
		case port.Post:
			println("hello,", p.Data.String(), "( kind", p.Kind, ")")
		}
	}
}
