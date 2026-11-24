// Package sim is the peer simulator for the network tests (spec M5 §6,
// §9.7): the link protocol to a VM started with --net-fd N --sim-clock,
// the VM child process, and packet builders/parsers for Ethernet, ARP,
// IPv4, ICMP, UDP, TCP, DHCP and DNS. Go standard library only.
//
// Link protocol (platform/unix/net_unix.h): a stream of messages
// [u16 len, big-endian][u8 type][len bytes]; MsgFrame carries a raw
// Ethernet frame (no FCS) either way, MsgAdvance (simulator -> VM, u32 ms
// big-endian) moves the simulated clock forward and MsgAck (VM ->
// simulator, empty) says the VM has run everything due by the new time and
// is idle. One ACK answers all the advances sent before it. MsgLink
// (simulator -> VM, u8) takes the link up (1) or down (0).
package sim

import (
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"net"
	"time"
)

// Link message types.
const (
	MsgFrame   byte = 0
	MsgAdvance byte = 1
	MsgAck     byte = 2
	MsgLink    byte = 3 // simulator -> VM: u8, 1 = link up, 0 = down
)

// WriteMsg writes one link message.
func WriteMsg(w io.Writer, typ byte, payload []byte) error {
	if len(payload) > 0xFFFF {
		return fmt.Errorf("sim: message of %d bytes", len(payload))
	}
	b := make([]byte, 3+len(payload))
	binary.BigEndian.PutUint16(b, uint16(len(payload)))
	b[2] = typ
	copy(b[3:], payload)
	_, err := w.Write(b)
	return err
}

// ReadMsg reads one link message.
func ReadMsg(r io.Reader) (typ byte, payload []byte, err error) {
	var h [3]byte
	if _, err := io.ReadFull(r, h[:]); err != nil {
		return 0, nil, err
	}
	payload = make([]byte, binary.BigEndian.Uint16(h[:]))
	if _, err := io.ReadFull(r, payload); err != nil {
		if err == io.EOF {
			err = io.ErrUnexpectedEOF
		}
		return 0, nil, err
	}
	return h[2], payload, nil
}

// DefaultTimeout bounds, in real time, how long Advance and Recv wait for
// the VM: a hung VM fails the test instead of hanging it.
const DefaultTimeout = 5 * time.Second

// Link is the simulator's end of the link to one VM.
type Link struct {
	c       net.Conn
	now     uint64 // simulated ms advanced to so far
	Timeout time.Duration
	// Frames the VM sent that no Advance/Recv has returned yet (Recv
	// queues ones it skips).
	pending [][]byte
	eof     bool
}

// NewLink wraps the simulator's end of the link.
func NewLink(c net.Conn) *Link { return &Link{c: c, Timeout: DefaultTimeout} }

// Now is the simulated time the simulator has advanced the VM to, in ms.
func (l *Link) Now() uint64 { return l.now }

// Closed reports whether the VM's end has closed (it exited).
func (l *Link) Closed() bool { return l.eof }

// Close closes the simulator's end (a VM with --sim-clock then exits 0).
func (l *Link) Close() error { return l.c.Close() }

// Send sends one Ethernet frame to the VM. It fails with ErrTimeout if
// the VM does not take it within Timeout: the VM reads one frame per idle
// wait and blocks writing its own frames while the simulator is not
// reading, so a scenario sending many frames should interleave Send with
// Sync (or use Exchange) rather than queue them all first.
func (l *Link) Send(frame []byte) error {
	l.c.SetWriteDeadline(time.Now().Add(l.Timeout))
	err := WriteMsg(l.c, MsgFrame, frame)
	var ne net.Error
	if errors.As(err, &ne) && ne.Timeout() {
		return ErrTimeout
	}
	return err
}

// ErrTimeout is returned when the VM does not answer within Timeout.
var ErrTimeout = errors.New("sim: VM did not answer in time")

func (l *Link) read() (byte, []byte, error) {
	if l.eof {
		return 0, nil, io.EOF
	}
	l.c.SetReadDeadline(time.Now().Add(l.Timeout))
	typ, p, err := ReadMsg(l.c)
	if err != nil {
		var ne net.Error
		if errors.As(err, &ne) && ne.Timeout() {
			return 0, nil, ErrTimeout
		}
		l.eof = true
		return 0, nil, io.EOF
	}
	return typ, p, nil
}

// Advance moves the VM's clock forward by ms and returns, in order, the
// frames it sent until it acknowledged (it then is idle at Now()). If the
// VM exits first, the frames are returned with io.EOF. Advance(0) is a
// sync: it waits until the VM is idle. After an ErrTimeout the link is
// out of step (a late ACK would end the next Advance early): fail the
// test rather than carry on.
func (l *Link) Advance(ms uint32) ([][]byte, error) {
	if l.eof {
		return l.take(), io.EOF
	}
	var b [4]byte
	binary.BigEndian.PutUint32(b[:], ms)
	if err := WriteMsg(l.c, MsgAdvance, b[:]); err != nil {
		return l.drain(), io.EOF
	}
	l.now += uint64(ms)
	for {
		typ, p, err := l.read()
		if err != nil {
			return l.take(), err
		}
		switch typ {
		case MsgFrame:
			l.pending = append(l.pending, p)
		case MsgAck:
			return l.take(), nil
		default:
			return l.take(), fmt.Errorf("sim: unexpected link message type %d", typ)
		}
	}
}

// SetLink takes the VM's link up or down (the VM's sends fail while it
// is down) and syncs: the frames the VM sent in response.
func (l *Link) SetLink(up bool) ([][]byte, error) {
	b := []byte{0}
	if up {
		b[0] = 1
	}
	if err := WriteMsg(l.c, MsgLink, b); err != nil {
		return l.drain(), io.EOF
	}
	return l.Sync()
}

// Sync is Advance(0).
func (l *Link) Sync() ([][]byte, error) { return l.Advance(0) }

// drain is for a write that failed because the VM has exited: the frames
// it sent before exiting may still be unread in the socket, so read them
// up to EOF (or Timeout) and return them with the pending ones.
func (l *Link) drain() [][]byte {
	for !l.eof {
		typ, p, err := l.read()
		if err != nil {
			break
		}
		if typ == MsgFrame {
			l.pending = append(l.pending, p)
		}
	}
	l.eof = true
	return l.take()
}

func (l *Link) take() [][]byte {
	f := l.pending
	l.pending = nil
	return f
}

// Exchange sends frame, then syncs: the frames the VM sent in response,
// once it is idle again.
func (l *Link) Exchange(frame []byte) ([][]byte, error) {
	if err := l.Send(frame); err != nil {
		return nil, err
	}
	return l.Sync()
}
