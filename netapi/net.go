// Package net is the gogo network API (import path gogo/net; its stub
// source lives in netapi/ because net/ holds the C stack, and a Go package
// directory cannot contain C files without cgo). These stubs exist only so
// programs type-check; the gogo compiler replaces every call with VM
// instructions, so the bodies are never executed.
//
// The struct types below have fixed type indices 9-26 in every image
// (TYPE_NET_* in vm/opcodes.h); keep their field order in sync with
// compiler.reservedTypes.
//
// IPv4 addresses are ints: a.b.c.d is a<<24 | b<<16 | c<<8 | d (addresses
// from 128.0.0.0 up are negative).
//
// Ports (port.Open):
//
//   - "netif", cfg IfConfig{}: the network interface. One owner at a time
//     (the DHCP actor). Its owner sets the address with SetAddr; an IP of 0
//     clears it, and so does closing the port. Without an address the stack
//     passes only DHCP: UDP from local port 68 to 255.255.255.255:67, and
//     UDP to port 68 from port 67. A BOOTP request sent from port 68 to
//     port 67 gets the interface MAC in its chaddr; BOOTP replies to port
//     68 for another chaddr are dropped.
//   - "udp", cfg UDP{Port} (0 picks a free port from 49152 up): send with
//     actor.Send(u, SendTo{Addr, Data}); datagrams arrive at the owner as
//     Datagram{From, Data}, Data a heap copy of the payload. A datagram that
//     would take the owner past its memory limit is dropped (the owner is
//     not killed; UDP has no retransmission). A datagram to a host whose
//     Ethernet address is not known yet is dropped while the stack asks for
//     it (ARP); send again.
//   - "tcp", cfg Dial{IP, Port, TimeoutMs} (TimeoutMs 0: the retransmission
//     limit only): an active open. Open fails without an address, while
//     the link is down, to a broadcast or multicast address, or without a
//     free connection slot (4 in all; a TIME_WAIT or half-open inbound one
//     is reclaimed). The owner gets Connected, or Closed with Refused,
//     Timeout or NoRoute. See Write, Shutdown, Data and Closed.
//   - "tcp_listen", cfg Listen{Port}: the owner gets Accepted{Conn} for each
//     connection the peer completes; Conn is a "tcp" port owned by the
//     listener and paused (no Data, window 0) until port.Handoff gives it
//     to another actor or its owner sends it Resume{}. Inbound connections
//     never take the last free slot (a SYN that would is dropped; the peer
//     retries), so the device's own Dial always finds one.
//
// TCP is deliberately small: one segment in flight each way (window 1460
// bytes), in-order segments only, retransmission after 1 s doubling to 8
// s, given up (Closed{Timeout}) after 6 retransmissions. A peer whose
// window stays 0 gets 1-byte probes on the same timer, so a connection
// it never reopens is aborted the same way, about 39 s after its first
// probe (Closed{Timeout}). A received
// segment is acknowledged only once its Data is in the owner's mailbox: an
// owner near its memory limit is never killed, the peer resends instead
// (Data leaves room for the control events; one that still does not fit
// waits until the owner has read some).
//
// A connection lives only while the interface keeps its address: link
// loss, SetAddr to another address or IP 0, or the "netif" port closing
// (its owner exiting) ends every connection with Closed{NoRoute}, orphans
// included. Keep the netif owner (the DHCP actor) alive while connections
// should finish closing.
package net

import (
	"gogo/actor"
	"gogo/bytes"
)

// IfConfig configures the "netif" port.
type IfConfig struct{}

// SetAddr is a command to the "netif" port (owner only).
type SetAddr struct {
	IP, Mask, Gateway, DNS int
}

// Up is sent to subscribers when the interface is up (link up and an
// address set), and again when its address changes.
type Up struct {
	IP, Gateway, DNS int
}

// Down is sent to subscribers when the interface goes down (link lost or
// address cleared).
type Down struct{}

// Addr is an IPv4 address and port.
type Addr struct {
	IP, Port int
}

// UDP configures a "udp" port.
type UDP struct {
	Port int
}

// SendTo is a command to a "udp" port: send Data to Addr.
type SendTo struct {
	Addr Addr
	Data bytes.Bytes
}

// Datagram is an event of a "udp" port: Data arrived from From.
type Datagram struct {
	From Addr
	Data bytes.Bytes
}

// Subscribe makes the caller receive Up and Down; Up at once if the
// interface is up already. ok is false if there is no network or the
// subscriber table (4 live subscribers) is full. Subscribing twice is
// the same as once; a subscriber's exit ends it.
func Subscribe() (ok bool) { panic("gogo only") }

// Dial configures a "tcp" port: connect to IP:Port. TimeoutMs bounds the
// connection attempt (0: until the retransmission limit, about 39 s).
type Dial struct {
	IP, Port, TimeoutMs int
}

// Listen configures a "tcp_listen" port on local Port.
type Listen struct {
	Port int
}

// Write is a command to a "tcp" port: send Data. Written follows once the
// peer acknowledged all of it. One Write may wait behind the one in
// progress; a third before Written closes the connection with
// Closed{Overflow}, as does writing on while 3 Written events are owed
// (not yet delivered because the owner's mailbox is full). Data is pinned
// (or copied) on the owner's behalf until acknowledged; a small Write is
// copied into a Bytes handle held for the owner, so it can also close with
// Overflow when the VM's 64 Bytes handles are all taken by other actors.
type Write struct {
	Data bytes.Bytes
}

// Shutdown is a command to a "tcp" port: send FIN after the pending
// writes. After it, closing the port (or the owner's exit) lets the
// connection finish its close on its own instead of resetting it (an
// orphan; but the netif owner's exit discards it, see above). An orphan
// lives at most 20 s (GOGO_NET_ORPHAN_MS) from the close, then is reset
// whatever its progress.
type Shutdown struct{}

// Resume is a command to an accepted "tcp" port: start delivering Data
// (port.Handoff does the same).
type Resume struct{}

// Connected is sent to a "tcp" port's owner when the connection is up.
type Connected struct{}

// Data is an event of a "tcp" port: the next bytes from the peer, in
// order, one message per segment.
type Data struct {
	Data bytes.Bytes
}

// Written is sent when the peer acknowledged a Write's data.
type Written struct{}

// Closed is an event of a "tcp" port. Reason Normal: the peer finished
// sending (it may still read: Write and Shutdown work until the port is
// closed). Any other reason: the connection is gone (this may follow a
// Normal one). After either, close the port.
type Closed struct {
	Reason int
}

// Accepted is an event of a "tcp_listen" port: Conn is a new connection's
// "tcp" port, owned by the listener's owner and paused.
type Accepted struct {
	Conn actor.PID
}

// Closed reasons.
const (
	Normal   = 0
	Reset    = 1
	Timeout  = 2
	Refused  = 3
	Overflow = 4
	NoRoute  = 5
)
