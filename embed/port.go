//go:build cgo && libgogo

package embed

/*
#include "bind.h"
*/
import "C"

import (
	"errors"
	"runtime/cgo"
	"unsafe"
)

// PortDriver serves a host port: a port of the host's own that a program
// opens by name, port.Open(name, port.Host{Arg, Data}) (gogo.h's host
// ports, experimental). Its methods run inside the VM's calls (Step,
// Close), on the goroutine that makes them; data is the driver's to keep.
// They must not block: slow work goes to a goroutine of the driver's own,
// which delivers its result with Poster.PortPost.
type PortDriver interface {
	// Open is port.Open: nil accepts, an error refuses (port.Open
	// fails). An Errno of a shared code (ErrInval, ErrNoMem, ...) is the
	// error port.Open returns (errors.BadArg, errors.NoMemory, ...); any
	// other error, ErrState among them, is errors.Busy. Events it emits
	// reach the owner once the open succeeds.
	Open(p Port, arg int32, data []byte) error
	// Command is a port.Post an actor sent to the port.
	Command(p Port, m Message)
	// Close says the port has closed, for any reason (the owner's exit,
	// port.Close, Port.Close, VM.Close); Emit to it fails already.
	Close(p Port)
}

// PortFuncs is a PortDriver made of functions; a nil one accepts every
// open, ignores commands or does nothing on a close.
type PortFuncs struct {
	OpenFunc    func(p Port, arg int32, data []byte) error
	CommandFunc func(p Port, m Message)
	CloseFunc   func(p Port)
}

func (f PortFuncs) Open(p Port, arg int32, data []byte) error {
	if f.OpenFunc == nil {
		return nil
	}
	return f.OpenFunc(p, arg, data)
}

func (f PortFuncs) Command(p Port, m Message) {
	if f.CommandFunc != nil {
		f.CommandFunc(p, m)
	}
}

func (f PortFuncs) Close(p Port) {
	if f.CloseFunc != nil {
		f.CloseFunc(p)
	}
}

// Port is one open host port of a VM.
type Port struct {
	vm *VM
	id C.gogo_port
}

// ID is the port's gogo_port value: nonzero, and naming no later port
// until its slot has been reused 4096 times.
func (p Port) ID() uint32 { return uint32(p.id) }

// Emit sends the port's owner a port.Post with m's Kind and Data
// (copied), From the port, now. On the VM's goroutine: in a driver
// method or between steps (other goroutines use Poster.PortPost).
// ErrClosed (the port is closed or its owner exiting), ErrFull (over the
// owner's quota: dropped), Err2Big.
func (p Port) Emit(m Message) error {
	if p.vm == nil || p.vm.vm == nil {
		return check("emit", C.GOGO_ECLOSED)
	}
	return check("emit", C.gogo_port_emit(p.vm.vm, p.id, C.int32_t(m.Kind), bytesPtr(m.Data), C.size_t(len(m.Data))))
}

// Close ends the port from the host's side (a device gone): the posts
// already queued are delivered, then the owner gets port.Closed{From:
// the port, Reason: reason}, then the driver's Close runs. On the VM's
// goroutine; not from the port's own Open (ErrBusy: refuse the open
// instead). reason is a positive Code such as CodeIO.
func (p Port) Close(reason Code) error {
	if p.vm == nil || p.vm.vm == nil {
		return check("port close", C.GOGO_ECLOSED)
	}
	return check("port close", C.gogo_port_close(p.vm.vm, p.id, C.int32_t(reason)))
}

// portEntry is what a registered driver's handle holds.
type portEntry struct {
	vm *VM
	d  PortDriver
}

// RegisterPort registers d as the host port name (at most 31 bytes),
// before Load: a registered port satisfies an image's requirement of its
// name. A VM takes 4 host ports (ErrFull beyond); ErrExist if the name is
// taken or is one of the runtime's own drivers ("tcp", "screen", ...).
// Experimental.
func (vm *VM) RegisterPort(name string, d PortDriver) error {
	if vm.vm == nil {
		return check("register port", C.GOGO_ECLOSED)
	}
	if d == nil {
		return check("register port", C.GOGO_EINVAL)
	}
	h := cgo.NewHandle(&portEntry{vm, d})
	err := check("register port", vm.call(func() C.int {
		return C.bind_register_port(vm.vm, strPtr(name), C.size_t(len(name)), C.uintptr_t(h))
	}))
	if err != nil {
		h.Delete()
		return err
	}
	vm.ports = append(vm.ports, h)
	return nil
}

func portOf(h C.uintptr_t) *portEntry { return cgo.Handle(h).Value().(*portEntry) }

//export bindPortOpen
func bindPortOpen(h C.uintptr_t, port C.gogo_port, arg C.int32_t, data *C.uint8_t, n C.size_t) (rc C.int) {
	e := portOf(h)
	defer e.vm.st.catch()
	rc = C.GOGO_EBUSY // refused, also after a panic
	err := e.d.Open(Port{e.vm, port}, int32(arg), C.GoBytes(unsafe.Pointer(data), C.int(n)))
	if err == nil {
		return 0
	}
	if en := Errno(0); errors.As(err, &en) && en < 0 {
		return C.int(en) // the library makes a code past the shared ones Busy
	}
	return C.GOGO_EBUSY
}

//export bindPortCommand
func bindPortCommand(h C.uintptr_t, port C.gogo_port, kind C.int32_t, data *C.uint8_t, n C.size_t) {
	e := portOf(h)
	defer e.vm.st.catch()
	e.d.Command(Port{e.vm, port}, Message{int32(kind), C.GoBytes(unsafe.Pointer(data), C.int(n))})
}

//export bindPortClose
func bindPortClose(h C.uintptr_t, port C.gogo_port) {
	e := portOf(h)
	defer e.vm.st.catch()
	e.d.Close(Port{e.vm, port})
}
