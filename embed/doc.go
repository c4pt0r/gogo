// Package embed runs gogo programs inside a Go program, and compiles them
// in process.
//
// The compiler half (Compile, CompileFS, CompileDir) is plain Go: it is
// the compiler of gogo build, called directly, and is always built.
//
// The VM half (New, VM, Poster, host ports) is a cgo binding over
// libgogo.a, the embedding library of runtime/include/gogo.h. It is built
// only with the build tag libgogo and cgo, and links the libgogo.a that
// CGO_LDFLAGS points the linker at (make lib builds build/libgogo.a):
//
//	make lib
//	CGO_LDFLAGS="-L$PWD/build" go build -tags libgogo ./...
//
// go does not notice a rebuilt libgogo.a: after make lib changes it,
// build with -a (make test-gobind and make example-embed relink by
// themselves).
//
// Without the tag the package holds only the compiler half, so a build
// without a C compiler or without libgogo.a still works.
//
// The binding follows gogo.h's rules. A VM is single-threaded: its methods
// are not safe for concurrent use, and must be called from one goroutine
// at a time (the goroutine may move between OS threads; the library does
// not care which thread calls, only that calls do not overlap). Other
// goroutines post through a Poster, which is safe for concurrent use.
// Host port drivers and the Host hooks run inside Step, on the goroutine
// that calls it, and must not call Step, Run, Load, Start or Close (they
// return ErrBusy there), nor block: slow work goes to a goroutine of its
// own, which reports back through a Poster.
//
// A VM and its posters hold C memory and cgo handles that the garbage
// collector does not reclaim: Close every VM and every Poster.
//
// Book chapter 18 (docs/book/18-embedding.md) is the guide; examples/embed-go
// is a complete host.
package embed
