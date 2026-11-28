//go:build !windows

// The simulator's end of the VM link on POSIX: a socketpair, the child's end
// inherited as fd 3 (--net-fd 3).
package sim

import (
	"net"
	"os"
	"syscall"
	"time"
)

// openLink makes the link's OS ends: the VM's args for it, the files the
// child inherits, and accept, which returns the simulator's end (already
// open here: the pair exists before the child starts).
func openLink(limit time.Duration) (args []string, files []*os.File, accept func() (net.Conn, error), err error) {
	fds, err := syscall.Socketpair(syscall.AF_UNIX, syscall.SOCK_STREAM, 0)
	if err != nil {
		return nil, nil, nil, err
	}
	mine := os.NewFile(uintptr(fds[0]), "sim-link")
	theirs := os.NewFile(uintptr(fds[1]), "vm-link")
	conn, cerr := net.FileConn(mine) // dups the fd
	mine.Close()
	if cerr != nil {
		theirs.Close()
		return nil, nil, nil, cerr
	}
	return []string{"--net-fd", "3"}, []*os.File{theirs}, func() (net.Conn, error) { return conn, nil }, nil
}
