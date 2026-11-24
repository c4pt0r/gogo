package sim

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"net"
	"os"
	"os/exec"
	"syscall"
	"time"
)

// VM is a gogovm child process linked to the simulator over a socketpair
// (its end is the child's fd 3), with the simulated clock.
type VM struct {
	*Link
	Cmd            *exec.Cmd
	Stdout, Stderr bytes.Buffer
	cancel         context.CancelFunc
	waited         bool
	err            error
}

// Start runs vmPath --net-fd 3 --sim-clock [extra...] image. The child is
// killed after limit (real time) if still running.
func Start(vmPath, image string, limit time.Duration, extra ...string) (*VM, error) {
	fds, err := syscall.Socketpair(syscall.AF_UNIX, syscall.SOCK_STREAM, 0)
	if err != nil {
		return nil, err
	}
	mine := os.NewFile(uintptr(fds[0]), "sim-link")
	theirs := os.NewFile(uintptr(fds[1]), "vm-link")
	defer theirs.Close()
	conn, err := net.FileConn(mine) // dups the fd
	mine.Close()
	if err != nil {
		return nil, err
	}
	ctx, cancel := context.WithTimeout(context.Background(), limit)
	args := append([]string{"--net-fd", "3", "--sim-clock"}, extra...)
	args = append(args, image)
	v := &VM{Link: NewLink(conn), cancel: cancel}
	v.Cmd = exec.CommandContext(ctx, vmPath, args...)
	v.Cmd.ExtraFiles = []*os.File{theirs} // fd 3
	v.Cmd.Stdout = &v.Stdout
	v.Cmd.Stderr = &v.Stderr
	v.Cmd.WaitDelay = time.Second
	if err := v.Cmd.Start(); err != nil {
		cancel()
		conn.Close()
		return nil, err
	}
	return v, nil
}

// Wait closes the link (a VM still waiting on it exits 0) and waits for
// the child; nil if it exited with status 0.
func (v *VM) Wait() error {
	if v.waited {
		return v.err
	}
	v.waited = true
	v.Link.Close()
	v.err = v.Cmd.Wait()
	v.cancel()
	var ee *exec.ExitError
	if errors.As(v.err, &ee) && ee.ExitCode() < 0 {
		v.err = fmt.Errorf("%v (killed: hung or timed out)", v.err)
	}
	return v.err
}
