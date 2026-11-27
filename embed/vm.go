//go:build cgo && libgogo

package embed

/*
#cgo CFLAGS: -I${SRCDIR}/../runtime/include
#cgo LDFLAGS: -lgogo -lpthread
#include "bind.h"
*/
import "C"

import (
	"context"
	"errors"
	"fmt"
	"io"
	"os"
	"runtime"
	"runtime/cgo"
	"sync/atomic"
	"time"
	"unsafe"
)

// Host is what a VM asks of the program that embeds it: gogo_host's hooks
// as Go values. Every field may be left zero, for the default. Memory
// comes from C's malloc and is not a hook. The hooks run inside the VM's
// calls (Step, Load, ...), on the goroutine that makes them, and must
// not block: the VM, and Run's context, wait for them.
type Host struct {
	// Stdout gets the program's output (println), Stderr its
	// diagnostics: crash reports, and why a load or a start was
	// refused. nil: os.Stdout, os.Stderr. A write is one line or less.
	Stdout, Stderr io.Writer
	// Now is the monotonic clock in milliseconds, any epoch. nil: the
	// OS's monotonic clock. Run waits in real time, so a clock of
	// another pace suits a host that steps the VM itself.
	Now func() int64
	// UnixTime is the wall clock (Unix milliseconds, 0 unknown), and
	// SetUnixTime sets it (sys.SetTime; an error refuses, and the program
	// sees false). Both or neither: nil is the OS's clock, which the
	// program may not set.
	UnixTime    func() int64
	SetUnixTime func(ms int64) error
	// Random gives 32 random bits unpredictable to a network peer. nil:
	// the OS's entropy.
	Random func() uint32
	// Config is sys.Config: the value of key, and whether it is set. nil:
	// nothing is set.
	Config func(key string) ([]byte, bool)
}

// Config is a VM's configuration (gogo_config). The zero value is every
// name the VM has granted, no memory cap, the default tables and limits,
// no storage and no hot loading.
//
// MemLimit, MaxActors, MaxPorts, MaxLinks, ActorMemory, BytesHandles and
// BytesPerActor are the VM's parameters (book 18.3; gogovm's flags): a
// field that is set beats the Host.Config key of the same parameter
// (vm.memory, vm.max_actors, vm.max_ports, node.max_links,
// vm.actor_memory, vm.handles), which beats the library's default; 0 is
// not set. Where gogo.h clamps a value past its range, the VM uses the
// nearer end and says so in one line on the Host's Stderr ("gogo:
// vm.actor_memory 2147483648 is over this VM's 1G; using 1G"); where it
// refuses one, New fails with ErrInval. The library does not report the
// values a VM ends up with: a program reads the fields that are set, as
// given, with sys.Config under their keys, and its own limit with
// actor.Limit.
type Config struct {
	Host Host
	// MemLimit caps the bytes the whole VM may allocate, over every
	// actor's own limit (0: none).
	MemLimit uint64
	// MaxActors is the most live actors at once (0: 16384). Actor and
	// port PIDs share 65536 slots: MaxActors + MaxPorts may be at most
	// 65536 (65520 with 16 ports), and New fails with ErrInval past that.
	MaxActors uint32
	// MaxPorts is the most ports open at once; the table grows as they
	// open (0: 1024, or fewer, silently, when MaxActors leaves less
	// room). Above 2048, or past the room MaxActors leaves, New fails
	// with ErrInval. Experimental.
	MaxPorts uint32
	// MaxLinks is the most node links of distributed actors at once
	// (0: 8). An embedded VM has no node port today, so it takes effect
	// only for one that gains it. Above 2048 New fails with ErrInval.
	// Experimental.
	MaxLinks uint32
	// ActorMemory is an actor's default memory limit in bytes, for an
	// actor whose SpawnOpt sets none (0: 1 MB). Below 1 KB or above 1 GB
	// it is clamped, said on Stderr. Experimental.
	ActorMemory uint32
	// BytesHandles is the most off-heap Bytes handles in the whole VM
	// (0: 4096; above 65535 clamped, said on Stderr), and BytesPerActor
	// how many of them one actor may hold (0: 256, or BytesHandles if
	// that is fewer, silently; above BytesHandles clamped, said on
	// Stderr). Experimental.
	BytesHandles  uint32
	BytesPerActor uint32
	// Grant lists the requirement names the VM may serve (port drivers,
	// such as a host port's name, and mount prefixes such as "/flash");
	// nil grants every one it has, an empty non-nil slice none.
	// Experimental, as in gogo.h.
	Grant []string
	// FSRoot is a host directory served as /flash ("" none) and FSQuota
	// the bytes its files may hold (0: no quota). Experimental.
	FSRoot  string
	FSQuota int64
	// CodeLoad lets code.Load load modules (GOGO_CONFIG_CODE_LOAD).
	// Experimental.
	CodeLoad bool
}

// State is what Step reports.
type State int

const (
	Runnable State = C.GOGO_RUNNABLE // work remains: step again
	Idle     State = C.GOGO_IDLE     // nothing to do until NextDeadline or a wake
	Done     State = C.GOGO_DONE     // the program has finished
)

func (s State) String() string {
	switch s {
	case Runnable:
		return "runnable"
	case Idle:
		return "idle"
	case Done:
		return "done"
	}
	return fmt.Sprintf("State(%d)", int(s))
}

// Message is a port.Post: what a host posts to a registered actor, what a
// program sends a host port, and what a host port emits to its owner.
type Message struct {
	Kind int32
	Data []byte
}

// VM is one gogo VM (gogo_vm). Its methods must not be called
// concurrently (see the package documentation); Poster is the handle
// for other goroutines.
type VM struct {
	vm      *C.gogo_vm
	st      *state
	ports   []cgo.Handle
	started bool
}

// state is what the host hooks reach through the context's handle. It
// lives until the VM's last reference is gone (the destroy hook), which
// may be after Close when posters remain.
type state struct {
	host    Host
	wake    chan struct{}
	grant   []*C.char
	grantv  unsafe.Pointer // the NULL-terminated array, C memory
	panic   any            // a panic in a hook, raised again after the call
	live    atomic.Int64   // bytes still held from the host at destroy, -1 before it
	handle  cgo.Handle
	stdout  io.Writer
	stderr  io.Writer
	stepped bool // inside a VM call: Close is refused
}

// libgogoSum is set by make (go -ldflags -X) to the checksum of the
// libgogo.a it links. go does not hash C archives; a new value is what
// makes it relink after the archive changes.
var libgogoSum string

// LibraryVersion is libgogo's version, (major << 16) | (minor << 8) |
// patch (gogo_version).
func LibraryVersion() uint32 { return uint32(C.gogo_version()) }

// ImageFormat is the image formats the library loads: (major << 16) |
// newest minor (gogo_image_format).
func ImageFormat() uint32 { return uint32(C.gogo_image_format()) }

// errLibVersion is New's error when the linked libgogo.a is older than,
// or of another minor than, the gogo.h this package was built with.
var errLibVersion = errors.New("gogo: libgogo.a is not the version of gogo.h: rebuild it (make lib)")

// New makes a VM. cfg may be nil (the zero Config). Free it with Close.
func New(cfg *Config) (*VM, error) {
	v := uint32(C.gogo_version())
	if v>>8 != uint32(C.GOGO_VERSION)>>8 || v < uint32(C.GOGO_VERSION) {
		return nil, errLibVersion
	}
	if cfg == nil {
		cfg = &Config{}
	}
	h := cfg.Host
	if (h.UnixTime == nil) != (h.SetUnixTime == nil) {
		return nil, errors.New("gogo: new: Host.UnixTime and Host.SetUnixTime go together")
	}
	st := &state{host: h, wake: make(chan struct{}, 1), stdout: h.Stdout, stderr: h.Stderr}
	st.live.Store(-1)
	if st.stdout == nil {
		st.stdout = os.Stdout
	}
	if st.stderr == nil {
		st.stderr = os.Stderr
	}
	var hooks C.uint
	if h.Now != nil {
		hooks |= C.BIND_NOW
	}
	if h.UnixTime != nil {
		hooks |= C.BIND_UNIX_TIME
	}
	if h.Random != nil {
		hooks |= C.BIND_RANDOM
	}
	if h.Config != nil {
		hooks |= C.BIND_CONFIG
	}

	c := C.gogo_config{size: C.uint32_t(unsafe.Sizeof(C.gogo_config{}))}
	c.mem_limit = C.uint64_t(cfg.MemLimit)
	c.max_actors = C.uint32_t(cfg.MaxActors)
	c.max_ports = C.uint32_t(cfg.MaxPorts)
	c.max_links = C.uint32_t(cfg.MaxLinks)
	c.actor_memory = C.uint32_t(cfg.ActorMemory)
	c.bytes_handles = C.uint32_t(cfg.BytesHandles)
	c.bytes_per_actor = C.uint32_t(cfg.BytesPerActor)
	c.fs_quota = C.int64_t(cfg.FSQuota)
	if cfg.CodeLoad {
		c.flags |= C.GOGO_CONFIG_CODE_LOAD
	}
	if cfg.FSRoot != "" {
		root := C.CString(cfg.FSRoot) // not kept by gogo_new
		defer C.free(unsafe.Pointer(root))
		c.fs_root = root
	}
	if cfg.Grant != nil {
		// Kept by the VM while it exists: C memory, freed at destroy.
		st.grantv = C.calloc(C.size_t(len(cfg.Grant)+1), C.size_t(unsafe.Sizeof((*C.char)(nil))))
		if st.grantv == nil {
			return nil, check("new", C.GOGO_ENOMEM)
		}
		names := unsafe.Slice((**C.char)(st.grantv), len(cfg.Grant)+1)
		for i, g := range cfg.Grant {
			names[i] = C.CString(g)
			st.grant = append(st.grant, names[i])
		}
		c.grant = (**C.char)(st.grantv)
	}

	st.handle = cgo.NewHandle(st)
	vm := &VM{st: st}
	if rc := C.bind_new(C.uintptr_t(st.handle), hooks, &c, &vm.vm); rc != C.GOGO_OK {
		st.release()
		return nil, check("new", rc)
	}
	return vm, nil
}

// release frees what the state holds in C and its handle.
func (st *state) release() {
	for _, g := range st.grant {
		C.free(unsafe.Pointer(g))
	}
	C.free(st.grantv)
	st.grant, st.grantv = nil, nil
	st.handle.Delete()
}

// call runs a library call that may run hooks, and raises again a panic
// one of them caught once the outermost call returns. Inside a hook (a
// call in progress) it refuses with GOGO_EBUSY without calling C, as
// gogo.h's re-entrancy rule does for gogo_step, gogo_load and gogo_start,
// so that the flag Close checks stays set for the whole outer call.
func (vm *VM) call(f func() C.int) C.int {
	if vm.st.stepped {
		return C.GOGO_EBUSY
	}
	vm.st.stepped = true
	rc := f()
	vm.st.stepped = false
	if p := vm.st.panic; p != nil {
		vm.st.panic = nil
		panic(p)
	}
	return rc
}

// Load gives the VM its program, an image (gogo build output, or
// Compile's), which it copies. Once, before Start; host ports are
// registered before it. ErrRequires names the missing driver or mount on
// the Host's Stderr.
func (vm *VM) Load(image []byte) error {
	if vm.vm == nil {
		return check("load", C.GOGO_ECLOSED)
	}
	return check("load", vm.call(func() C.int {
		return C.gogo_load(vm.vm, bytesPtr(image), C.size_t(len(image)), C.GOGO_LOAD_COPY)
	}))
}

// Start starts the image's main. Run starts it when it has not been.
func (vm *VM) Start() error {
	if vm.vm == nil {
		return check("start", C.GOGO_ECLOSED)
	}
	err := check("start", vm.call(func() C.int { return C.gogo_start(vm.vm) }))
	if err == nil {
		vm.started = true
	}
	return err
}

// Step runs at most budget scheduler passes (0: until idle or done) and
// says what is left: Runnable, Idle (wait until NextDeadline, or until
// Wake says there is work) or Done.
func (vm *VM) Step(budget uint32) (State, error) {
	if vm.vm == nil {
		return Done, check("step", C.GOGO_ECLOSED)
	}
	rc := vm.call(func() C.int { return C.gogo_step(vm.vm, C.uint32_t(budget)) })
	if rc < 0 {
		return Done, check("step", rc)
	}
	return State(rc), nil
}

// NextDeadline is when the VM has work next without a wake: a value of
// the Host's clock (Now; it may have passed already: step at once), and
// false for never.
func (vm *VM) NextDeadline() (int64, bool) {
	if vm.vm == nil {
		return 0, false
	}
	d := int64(C.gogo_next_deadline(vm.vm))
	return d, d != C.GOGO_NEVER
}

// Now reads the Host's clock (Host.Now, or the OS's monotonic clock), in
// the milliseconds of NextDeadline.
func (vm *VM) Now() int64 {
	if vm.st.host.Now != nil {
		return vm.st.host.Now()
	}
	return int64(C.bind_now())
}

// Wake is a channel that receives when the VM may have work, its latch:
// posts and port events send to it (with room for one). A host that
// steps the VM itself takes any pending wake before each Step, and after
// an Idle step waits on it and on NextDeadline.
func (vm *VM) Wake() <-chan struct{} { return vm.st.wake }

// runBudget is how many scheduler passes Run's steps take, so that it
// sees its context's end while an actor spins.
const runBudget = 1024

// Run starts the VM if it has not been started, then steps it, waiting
// while it is idle, until the program has finished (nil) or ctx ends
// (ctx.Err(); the VM stays as it was, and Run may be called again).
// ctx is checked between steps: a hook or port driver that blocks delays
// Run's return until it returns.
func (vm *VM) Run(ctx context.Context) error {
	if !vm.started {
		if err := vm.Start(); err != nil {
			return err
		}
	}
	var timer *time.Timer
	defer func() {
		if timer != nil {
			timer.Stop()
		}
	}()
	for {
		select { // the latch is taken before each step
		case <-vm.st.wake:
		default:
		}
		if err := ctx.Err(); err != nil {
			return err
		}
		s, err := vm.Step(runBudget)
		if err != nil {
			return err
		}
		switch s {
		case Done:
			return nil
		case Runnable:
			continue
		}
		var due <-chan time.Time
		if d, ok := vm.NextDeadline(); ok {
			wait := d - vm.Now()
			if wait <= 0 {
				continue
			}
			if timer == nil {
				timer = time.NewTimer(time.Duration(wait) * time.Millisecond)
			} else {
				timer.Reset(time.Duration(wait) * time.Millisecond)
			}
			due = timer.C
		}
		select {
		case <-vm.st.wake:
		case <-due:
		case <-ctx.Done():
			return ctx.Err()
		}
	}
}

// Post sends the actor registered as name (actor.Register) a port.Post
// with m's Kind and Data (copied), From the zero PID; it is delivered at
// the next step. ErrClosed once posts are closed or the program has
// finished; ErrFull with too many posts queued. A post to a name not
// registered when it is delivered is dropped (Stats counts it).
func (vm *VM) Post(name string, m Message) error {
	if vm.vm == nil {
		return check("post", C.GOGO_ECLOSED)
	}
	return check("post", C.gogo_post(vm.vm, strPtr(name), C.size_t(len(name)), C.int32_t(m.Kind), bytesPtr(m.Data), C.size_t(len(m.Data))))
}

// ClosePosts ends posting: later posts fail with ErrClosed, and
// registered names and host ports no longer keep the VM alive, so a
// program whose actors only wait for posts finishes. Posts already
// queued are still delivered.
func (vm *VM) ClosePosts() {
	if vm.vm != nil {
		C.gogo_close_posts(vm.vm)
	}
}

// Stats are the VM's counters (gogo_stats). Experimental.
type Stats struct {
	PostsDelivered uint64 // posts queued on their receiver
	PostsDropped   uint64 // posts not delivered
	MemUsed        uint64 // the VM allocator's bytes in use
	MemPeak        uint64 // its highest use
}

// Stats reads the VM's counters. Experimental, as gogo_get_stats.
func (vm *VM) Stats() (Stats, error) {
	if vm.vm == nil {
		return Stats{}, check("stats", C.GOGO_ECLOSED)
	}
	s := C.gogo_stats{size: C.uint32_t(unsafe.Sizeof(C.gogo_stats{}))}
	if err := check("stats", C.gogo_get_stats(vm.vm, &s)); err != nil {
		return Stats{}, err
	}
	return Stats{uint64(s.posts_delivered), uint64(s.posts_dropped), uint64(s.mem_used), uint64(s.mem_peak)}, nil
}

// Close frees the VM: closes posts, closes its ports (their drivers'
// Close runs) and stops its program. Posters keep the VM's memory until
// they are closed too; their posts fail with ErrClosed. Close from a
// hook or a port driver is refused (ErrBusy); a second Close does
// nothing.
func (vm *VM) Close() error {
	if vm.vm == nil {
		return nil
	}
	if vm.st.stepped {
		return check("close", C.GOGO_EBUSY)
	}
	// Cleared first: a panic in a driver's Close comes out of call, and
	// the VM is freed by then. Drivers running inside gogo_free see a
	// closed VM (Emit is ErrClosed, as gogo.h says for a closing port).
	g, ports := vm.vm, vm.ports
	vm.vm, vm.ports = nil, nil
	defer func() {
		for _, h := range ports {
			h.Delete()
		}
	}()
	vm.call(func() C.int { C.gogo_free(g); return 0 })
	return nil
}

// ImageInfo describes an image (gogo_image_info). Experimental.
type ImageInfo struct {
	Format   uint32 // major << 16 | minor
	Requires int    // its requirements (ImageRequires)
	Exports  int    // its exported entry points, main among them
}

// GetImageInfo validates image as Load would and describes it, without a
// VM. On ErrVersion, Format is still set. Experimental.
func GetImageInfo(image []byte) (ImageInfo, error) {
	image = aligned(image)
	in := C.gogo_image_info{size: C.uint32_t(unsafe.Sizeof(C.gogo_image_info{}))}
	err := check("image info", C.gogo_image_info_get(bytesPtr(image), C.size_t(len(image)), &in))
	return ImageInfo{uint32(in.format), int(in.n_requires), int(in.n_exports)}, err
}

// ImageRequires lists an image's requirement names, the port drivers and
// mount prefixes Load checks: a host may register exactly the host ports
// an image needs. Experimental.
func ImageRequires(image []byte) ([]string, error) {
	image = aligned(image)
	var n C.uint32_t
	if err := check("image requires", C.gogo_image_requires(bytesPtr(image), C.size_t(len(image)), nil, 0, &n)); err != nil {
		return nil, err
	}
	if n == 0 {
		return nil, nil
	}
	// C fills the array with pointers into image. The array is C memory,
	// so C writes no Go pointer into Go memory, and each pointer is read
	// back as an offset into image, which stays live until then.
	names := (*C.gogo_name)(C.calloc(C.size_t(n), C.size_t(unsafe.Sizeof(C.gogo_name{}))))
	if names == nil {
		return nil, check("image requires", C.GOGO_ENOMEM)
	}
	defer C.free(unsafe.Pointer(names))
	if err := check("image requires", C.gogo_image_requires(bytesPtr(image), C.size_t(len(image)), names, n, &n)); err != nil {
		return nil, err
	}
	out := make([]string, 0, n)
	base := uintptr(unsafe.Pointer(&image[0]))
	for _, nm := range unsafe.Slice(names, n) {
		off := uintptr(unsafe.Pointer(nm.ptr)) - base
		out = append(out, string(image[off:off+uintptr(nm.len)]))
	}
	runtime.KeepAlive(image)
	return out, nil
}

// aligned returns image, or a copy of it if it does not start on a
// 4-byte boundary (the inspection functions read words in place).
func aligned(image []byte) []byte {
	if len(image) == 0 || uintptr(unsafe.Pointer(&image[0]))%4 == 0 {
		return image
	}
	w := make([]uint32, (len(image)+3)/4)
	b := unsafe.Slice((*byte)(unsafe.Pointer(&w[0])), len(image))
	copy(b, image)
	return b
}

func bytesPtr(b []byte) unsafe.Pointer { return unsafe.Pointer(unsafe.SliceData(b)) }

func strPtr(s string) *C.char { return (*C.char)(unsafe.Pointer(unsafe.StringData(s))) }

// stateOf finds the Go state behind a hook's handle.
func stateOf(h C.uintptr_t) *state { return cgo.Handle(h).Value().(*state) }

// catch records a panic in a hook, to be raised again when the library
// call that ran the hook returns (a panic must not unwind through C).
func (st *state) catch() {
	if p := recover(); p != nil && st.panic == nil {
		st.panic = p
	}
}

//export bindWrite
func bindWrite(h C.uintptr_t, stream C.int, s *C.char, n C.size_t) {
	st := stateOf(h)
	defer st.catch()
	w := st.stdout
	if stream == 2 {
		w = st.stderr
	}
	w.Write(C.GoBytes(unsafe.Pointer(s), C.int(n)))
}

//export bindNow
func bindNow(h C.uintptr_t) C.uint64_t {
	st := stateOf(h)
	defer st.catch()
	return C.uint64_t(st.host.Now())
}

//export bindWake
func bindWake(h C.uintptr_t) {
	select {
	case stateOf(h).wake <- struct{}{}:
	default:
	}
}

//export bindUnixTime
func bindUnixTime(h C.uintptr_t) C.int64_t {
	st := stateOf(h)
	defer st.catch()
	return C.int64_t(st.host.UnixTime())
}

//export bindSetUnixTime
func bindSetUnixTime(h C.uintptr_t, ms C.int64_t) (rc C.int) {
	st := stateOf(h)
	defer st.catch()
	rc = C.GOGO_EINVAL // also after a panic
	if err := st.host.SetUnixTime(int64(ms)); err != nil {
		return C.GOGO_EINVAL
	}
	return 0
}

//export bindRandom
func bindRandom(h C.uintptr_t) C.uint32_t {
	st := stateOf(h)
	defer st.catch()
	return C.uint32_t(st.host.Random())
}

//export bindConfigGet
func bindConfigGet(h C.uintptr_t, k *C.char, kl C.uint32_t, buf *C.uint8_t, capacity C.uint32_t) (n C.int32_t) {
	st := stateOf(h)
	defer st.catch()
	n = -1
	v, ok := st.host.Config(C.GoStringN(k, C.int(kl)))
	if !ok {
		return -1
	}
	if capacity > 0 {
		copy(unsafe.Slice((*byte)(unsafe.Pointer(buf)), int(capacity)), v)
	}
	return C.int32_t(len(v))
}

//export bindDestroy
func bindDestroy(h C.uintptr_t, live C.int64_t) {
	st := stateOf(h)
	st.live.Store(int64(live))
	st.release()
}
