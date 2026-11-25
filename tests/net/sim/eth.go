package sim

import (
	"encoding/binary"
	"errors"
	"fmt"
)

// MAC is an Ethernet address.
type MAC [6]byte

// IP is an IPv4 address.
type IP [4]byte

// Broadcast is the Ethernet broadcast address.
var Broadcast = MAC{0xff, 0xff, 0xff, 0xff, 0xff, 0xff}

// VMMAC is the MAC the unix platform gives the VM (unix_netif.mac).
var VMMAC = MAC{0x02, 0, 0, 0, 0, 0x01}

// IPv4 makes an address; IPFromInt / Int convert to the int form gogo
// programs use (a.b.c.d = a<<24 | b<<16 | c<<8 | d).
func IPv4(a, b, c, d byte) IP { return IP{a, b, c, d} }
func IPFromInt(v uint32) IP   { var ip IP; binary.BigEndian.PutUint32(ip[:], v); return ip }
func (ip IP) Int() uint32     { return binary.BigEndian.Uint32(ip[:]) }
func (ip IP) String() string  { return fmt.Sprintf("%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]) }
func (m MAC) String() string {
	return fmt.Sprintf("%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5])
}

// EtherTypes.
const (
	EtherIPv4 uint16 = 0x0800
	EtherARP  uint16 = 0x0806
)

// ErrShort is returned for a truncated packet.
var ErrShort = errors.New("sim: packet too short")

// Eth is an Ethernet II frame (no FCS).
type Eth struct {
	Dst, Src MAC
	Type     uint16
	Payload  []byte
}

// Bytes encodes the frame. Like a real NIC it pads to the 60-byte minimum
// only if Pad is used; Bytes itself does not pad.
func (e Eth) Bytes() []byte {
	b := make([]byte, 14+len(e.Payload))
	copy(b[0:6], e.Dst[:])
	copy(b[6:12], e.Src[:])
	binary.BigEndian.PutUint16(b[12:14], e.Type)
	copy(b[14:], e.Payload)
	return b
}

// Pad pads a frame with zeros to the 60-byte Ethernet minimum.
func Pad(frame []byte) []byte {
	for len(frame) < 60 {
		frame = append(frame, 0)
	}
	return frame
}

// ParseEth decodes an Ethernet II frame.
func ParseEth(b []byte) (Eth, error) {
	if len(b) < 14 {
		return Eth{}, ErrShort
	}
	var e Eth
	copy(e.Dst[:], b[0:6])
	copy(e.Src[:], b[6:12])
	e.Type = binary.BigEndian.Uint16(b[12:14])
	e.Payload = b[14:]
	return e, nil
}

// ARP operations.
const (
	ARPRequest uint16 = 1
	ARPReply   uint16 = 2
)

// ARP is an Ethernet/IPv4 ARP packet.
type ARP struct {
	Op  uint16
	SHA MAC
	SPA IP
	THA MAC
	TPA IP
}

// Bytes encodes the 28-byte ARP packet.
func (a ARP) Bytes() []byte {
	b := make([]byte, 28)
	binary.BigEndian.PutUint16(b[0:2], 1)      // Ethernet
	binary.BigEndian.PutUint16(b[2:4], 0x0800) // IPv4
	b[4], b[5] = 6, 4
	binary.BigEndian.PutUint16(b[6:8], a.Op)
	copy(b[8:14], a.SHA[:])
	copy(b[14:18], a.SPA[:])
	copy(b[18:24], a.THA[:])
	copy(b[24:28], a.TPA[:])
	return b
}

// ParseARP decodes an Ethernet/IPv4 ARP packet.
func ParseARP(b []byte) (ARP, error) {
	if len(b) < 28 {
		return ARP{}, ErrShort
	}
	if binary.BigEndian.Uint16(b[0:2]) != 1 || binary.BigEndian.Uint16(b[2:4]) != 0x0800 || b[4] != 6 || b[5] != 4 {
		return ARP{}, errors.New("sim: not an Ethernet/IPv4 ARP packet")
	}
	var a ARP
	a.Op = binary.BigEndian.Uint16(b[6:8])
	copy(a.SHA[:], b[8:14])
	copy(a.SPA[:], b[14:18])
	copy(a.THA[:], b[18:24])
	copy(a.TPA[:], b[24:28])
	return a, nil
}

// ARPFrame is an ARP packet in an Ethernet frame (request: broadcast).
func ARPFrame(a ARP) []byte {
	dst := a.THA
	if a.Op == ARPRequest {
		dst = Broadcast
	}
	return Pad(Eth{Dst: dst, Src: a.SHA, Type: EtherARP, Payload: a.Bytes()}.Bytes())
}
