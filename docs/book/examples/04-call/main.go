// Call and Reply: a request-reply server holding a small key-value table.
package main

import "gogo/actor"

type Put struct {
	Key   string
	Value int
}

type Lookup struct{ Key string }

type Found struct {
	Value int
	OK    bool
}

type entry struct {
	Key   string
	Value int
}

func store() {
	var tbl []entry
	for {
		switch r := actor.Receive().(type) {
		case actor.Request:
			switch b := r.Body.(type) {
			case Put:
				tbl = put(tbl, b.Key, b.Value)
				actor.Reply(r, true)
			case Lookup:
				v, ok := get(tbl, b.Key)
				actor.Reply(r, Found{v, ok})
			}
		}
	}
}

func put(tbl []entry, k string, v int) []entry {
	for i, e := range tbl {
		if e.Key == k {
			tbl[i] = entry{k, v}
			return tbl
		}
	}
	return append(tbl, entry{k, v})
}

func get(tbl []entry, k string) (int, bool) {
	for _, e := range tbl {
		if e.Key == k {
			return e.Value, true
		}
	}
	return 0, false
}

// lookup wraps the Call in a function with a typed result.
func lookup(s actor.PID, k string) (int, bool) {
	reply, ok := actor.Call(s, Lookup{k}, 100)
	if !ok {
		return 0, false // timeout
	}
	switch f := reply.(type) {
	case Found:
		return f.Value, f.OK
	}
	return 0, false
}

func main() {
	s := actor.Spawn(store)
	_, ok := actor.Call(s, Put{"led", 1}, 100)
	println("put led:", ok)
	_, ok = actor.Call(s, Put{"fan", 0}, 100)
	println("put fan:", ok)
	v, found := lookup(s, "led")
	println("led", v, found)
	v, found = lookup(s, "door")
	println("door", v, found)

	// A message sent to main before a Call stays in the mailbox: Call
	// waits only for its own reply.
	actor.Send(actor.Self(), "kept for later")
	v, found = lookup(s, "fan")
	println("fan", v, found)
	println(actor.Receive())

	// A Call to an actor that never answers times out.
	silent := actor.Spawn(func() { actor.Receive() })
	_, ok = actor.Call(silent, Lookup{"x"}, 50)
	println("silent answered:", ok)
}
