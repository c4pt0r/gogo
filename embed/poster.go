//go:build cgo && libgogo

package embed

/*
#include "gogo.h"
*/
import "C"

import "sync"

// Poster posts to a VM from any goroutine (gogo_poster); it is safe for
// concurrent use. Posts made one after another (each call returning
// before the next starts, from one goroutine or synchronized ones) are
// delivered in that order. A poster keeps the VM's memory, not its
// program: after VM.Close its posts fail with ErrClosed. Close it when
// done.
type Poster struct {
	mu sync.RWMutex // held for writing by Close, for reading by the posts
	p  *C.gogo_poster
}

// NewPoster makes a poster for the VM. On the VM's goroutine.
func (vm *VM) NewPoster() (*Poster, error) {
	if vm.vm == nil {
		return nil, check("new poster", C.GOGO_ECLOSED)
	}
	p := C.gogo_poster_new(vm.vm)
	if p == nil {
		return nil, check("new poster", C.GOGO_ENOMEM)
	}
	return &Poster{p: p}, nil
}

// Post is VM.Post from any goroutine.
func (p *Poster) Post(name string, m Message) error {
	p.mu.RLock()
	defer p.mu.RUnlock()
	if p.p == nil {
		return check("post", C.GOGO_ECLOSED)
	}
	return check("post", C.gogo_poster_post(p.p, strPtr(name), C.size_t(len(name)), C.int32_t(m.Kind), bytesPtr(m.Data), C.size_t(len(m.Data))))
}

// PortPost is Port.Emit from any goroutine: queued, delivered at the
// VM's next step. A post the port can no longer take then (closed since,
// its owner exiting, over quota) is dropped and counted (Stats).
// Experimental.
func (p *Poster) PortPost(port Port, m Message) error {
	p.mu.RLock()
	defer p.mu.RUnlock()
	if p.p == nil {
		return check("port post", C.GOGO_ECLOSED)
	}
	return check("port post", C.gogo_poster_port_post(p.p, port.id, C.int32_t(m.Kind), bytesPtr(m.Data), C.size_t(len(m.Data))))
}

// ClosePosts is VM.ClosePosts from any goroutine.
func (p *Poster) ClosePosts() {
	p.mu.RLock()
	defer p.mu.RUnlock()
	if p.p != nil {
		C.gogo_poster_close_posts(p.p)
	}
}

// Close drops the poster's reference to the VM. Later calls fail with
// ErrClosed (Close itself does nothing again).
func (p *Poster) Close() {
	p.mu.Lock()
	defer p.mu.Unlock()
	if p.p != nil {
		C.gogo_poster_free(p.p)
		p.p = nil
	}
}
