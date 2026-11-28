//go:build windows

// The simulator's end of the VM link on Windows. There are no socketpairs to
// inherit as fd 3, so the link is a TCP connection on loopback: this side
// listens on 127.0.0.1:0 and the VM's --net-fd is that PORT (the Windows
// gogovm connects to it; see platform/windows/platform_win.c). The accept
// happens after the child starts, so it runs in a goroutine and accept waits
// for it.
package sim

import (
	"fmt"
	"net"
	"os"
	"strconv"
	"time"
)

type accepted struct {
	c   net.Conn
	err error
}

func openLink(limit time.Duration) (args []string, files []*os.File, accept func() (net.Conn, error), err error) {
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		return nil, nil, nil, err
	}
	port := ln.Addr().(*net.TCPAddr).Port
	ch := make(chan accepted, 1)
	go func() {
		c, aerr := ln.Accept()
		ln.Close()
		ch <- accepted{c, aerr}
	}()
	return []string{"--net-fd", strconv.Itoa(port)}, nil, func() (net.Conn, error) {
		select {
		case a := <-ch:
			if a.err != nil {
				return nil, fmt.Errorf("link: accept: %w", a.err)
			}
			return a.c, nil
		case <-time.After(limit):
			ln.Close()
			return nil, fmt.Errorf("link: the VM did not connect within %v", limit)
		}
	}, nil
}
