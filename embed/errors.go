//go:build cgo && libgogo

package embed

/*
#include "gogo.h"
*/
import "C"

// Errno is an error result of the library: a negative GOGO_E* code of
// gogo.h, which is a shared error code (Code) negated, or ErrState. Its
// message is gogo_strerror's. Compare with errors.Is(err, embed.ErrClosed)
// (the binding wraps an Errno with what it was doing).
type Errno int

func (e Errno) Error() string { return C.GoString(C.gogo_strerror(C.int(e))) }

// Code is the shared error code the result negates (ErrInval.Code() is
// CodeBadArg); 36 for ErrState, which is the API's own.
func (e Errno) Code() Code { return Code(-e) }

// The library's results (gogo.h).
const (
	ErrNotFound = Errno(C.GOGO_ENOTFOUND) // Start: the image has no main
	ErrExist    = Errno(C.GOGO_EEXIST)    // RegisterPort: the name is taken
	ErrNoMem    = Errno(C.GOGO_ENOMEM)    // out of memory, or over Config.MemLimit
	ErrFull     = Errno(C.GOGO_EFULL)     // a fixed table is full: queued posts, host ports
	ErrBusy     = Errno(C.GOGO_EBUSY)     // called from a hook while the VM is stepping
	ErrInval    = Errno(C.GOGO_EINVAL)    // a bad argument
	ErrClosed   = Errno(C.GOGO_ECLOSED)   // the VM is closed, posts are closed, or the port is
	ErrRequires = Errno(C.GOGO_EREQUIRES) // the image requires a driver or mount the VM lacks
	ErrImage    = Errno(C.GOGO_EIMAGE)    // not a valid image
	Err2Big     = Errno(C.GOGO_E2BIG)     // a name or data is too long
	ErrVersion  = Errno(C.GOGO_EVERSION)  // the image's format is one this library does not run
	ErrState    = Errno(C.GOGO_ESTATE)    // out of order: not loaded, loaded twice, not started, ...
)

// Code is a shared error code of gogo_errors.h, the codes gogo programs
// see as gogo/errors: errors.IO there is CodeIO here. A host port closed
// from the host's side (Port.Close) gives its owner one as the reason.
type Code int32

// The shared error codes (gogo_errors.h).
const (
	CodeOK           = Code(C.GOGO_E_OK)
	CodeNotFound     = Code(C.GOGO_E_NOTFOUND)
	CodeExists       = Code(C.GOGO_E_EXISTS)
	CodeNoMemory     = Code(C.GOGO_E_NOMEMORY)
	CodeTooMany      = Code(C.GOGO_E_TOOMANY)
	CodeBusy         = Code(C.GOGO_E_BUSY)
	CodeBadArg       = Code(C.GOGO_E_BADARG)
	CodeClosed       = Code(C.GOGO_E_CLOSED)
	CodeIO           = Code(C.GOGO_E_IO)
	CodeTimeout      = Code(C.GOGO_E_TIMEOUT)
	CodeUnsupported  = Code(C.GOGO_E_UNSUPPORTED)
	CodeNoSpace      = Code(C.GOGO_E_NOSPACE)
	CodeBadPath      = Code(C.GOGO_E_BADPATH)
	CodeIsDir        = Code(C.GOGO_E_ISDIR)
	CodeNotDir       = Code(C.GOGO_E_NOTDIR)
	CodeNotEmpty     = Code(C.GOGO_E_NOTEMPTY)
	CodeNotMounted   = Code(C.GOGO_E_NOTMOUNTED)
	CodeBadImage     = Code(C.GOGO_E_BADIMAGE)
	CodeRefused      = Code(C.GOGO_E_REFUSED)
	CodeReset        = Code(C.GOGO_E_RESET)
	CodeNoRoute      = Code(C.GOGO_E_NOROUTE)
	CodeBadCert      = Code(C.GOGO_E_BADCERT)
	CodeNoTime       = Code(C.GOGO_E_NOTIME)
	CodeTLS          = Code(C.GOGO_E_TLS)
	CodeOverflow     = Code(C.GOGO_E_OVERFLOW)
	CodeBadURL       = Code(C.GOGO_E_BADURL)
	CodeBadFormat    = Code(C.GOGO_E_BADFORMAT)
	CodeBadVersion   = Code(C.GOGO_E_BADVERSION)
	CodeNoProc       = Code(C.GOGO_E_NOPROC)
	CodeNoConnection = Code(C.GOGO_E_NOCONNECTION)
)

// sharedLast is the highest shared code in use (GOGO_E_SHARED_LAST): the
// list above ends there (a test checks it).
const sharedLast = Code(C.GOGO_E_SHARED_LAST)

func (c Code) String() string { return C.GoString(C.gogo_strerror(C.int(-c))) }

// opError wraps a result with the call that gave it ("gogo: load: not a
// valid image"); errors.Is and errors.As see the Errno.
type opError struct {
	op  string
	err Errno
}

func (e *opError) Error() string { return "gogo: " + e.op + ": " + e.err.Error() }
func (e *opError) Unwrap() error { return e.err }

// check turns a library result into an error.
func check(op string, rc C.int) error {
	if rc >= 0 {
		return nil
	}
	return &opError{op, Errno(rc)}
}
