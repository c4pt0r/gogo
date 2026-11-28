package sim

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"os/exec"
	"time"
)

// VM is a gogovm child process linked to the simulator over the platform's
// link (a socketpair end as fd 3, or a loopback TCP port on Windows), with
// the simulated clock.
type VM struct {
	*Link
	Cmd            *exec.Cmd
	Stdout, Stderr bytes.Buffer
	cancel         context.CancelFunc
	waited         bool
	err            error
}

// Start runs vmPath --net-fd 3 --sim-clock [extra...] image. The child is
// killed after limit (real time) if still running. The link waits as long
// for each answer: on a loaded host one answer (the first, while the
// child starts) can take seconds, and a hung child is caught by the kill
// anyway (the link then reads EOF and Wait says it was killed), so a
// shorter wait would only fail a slow run that would have finished.
//
// The link's OS ends differ per platform (openLink): a socketpair end as
// fd 3 on POSIX, a TCP port on 127.0.0.1 on Windows, where a child cannot
// inherit a socketpair end.
func Start(vmPath, image string, limit time.Duration, extra ...string) (*VM, error) {
	linkArgs, files, accept, err := openLink(limit)
	if err != nil {
		return nil, err
	}
	for _, f := range files {
		defer f.Close()
	}
	ctx, cancel := context.WithTimeout(context.Background(), limit)
	args := append(linkArgs, "--sim-clock")
	args = append(args, extra...)
	args = append(args, image)
	v := &VM{cancel: cancel}
	v.Link = nil // set below, once the peer's end exists
	v.Cmd = exec.CommandContext(ctx, vmPath, args...)
	v.Cmd.ExtraFiles = files // fd 3 on POSIX; nil on Windows
	v.Cmd.Stdout = &v.Stdout
	v.Cmd.Stderr = &v.Stderr
	v.Cmd.WaitDelay = time.Second
	if err := v.Cmd.Start(); err != nil {
		cancel()
		return nil, err
	}
	conn, err := accept()
	if err != nil {
		cancel()
		_ = v.Cmd.Process.Kill()
		_ = v.Cmd.Wait()
		return nil, err
	}
	v.Link = NewLink(conn)
	v.Link.Timeout = limit
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
