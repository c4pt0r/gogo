package sim

import (
	"encoding/binary"
	"errors"
)

// IP protocols.
const (
	ProtoICMP uint8 = 1
	ProtoTCP  uint8 = 6
	ProtoUDP  uint8 = 17
)

// Checksum is the Internet checksum (RFC 1071) of b, folded and
// complemented; a buffer that includes a correct checksum sums to 0.
func Checksum(b []byte) uint16 { return ^fold(sum(0, b)) }

func sum(s uint32, b []byte) uint32 {
	for len(b) >= 2 {
		s += uint32(b[0])<<8 | uint32(b[1])
		b = b[2:]
	}
	if len(b) == 1 {
		s += uint32(b[0]) << 8
	}
	return s
}

func fold(s uint32) uint16 {
	for s > 0xFFFF {
		s = s>>16 + s&0xFFFF
	}
	return uint16(s)
}

// pseudoSum is the TCP/UDP pseudo-header sum.
func pseudoSum(src, dst IP, proto uint8, length int) uint32 {
	s := sum(0, src[:])
	s = sum(s, dst[:])
	return s + uint32(proto) + uint32(length)
}

// IPv4Pkt is an IPv4 header and payload. Zero TTL encodes as 64.
type IPv4Pkt struct {
	TOS      uint8
	ID       uint16
	Flags    uint8  // 3 bits: 2 = DF, 1 = MF
	FragOff  uint16 // in 8-byte units
	TTL      uint8
	Proto    uint8
	Src, Dst IP
	Options  []byte // a multiple of 4 bytes
	Payload  []byte
}

// Bytes encodes the packet with a correct header checksum.
func (p IPv4Pkt) Bytes() []byte {
	hl := 20 + len(p.Options)
	b := make([]byte, hl+len(p.Payload))
	b[0] = 0x40 | byte(hl/4)
	b[1] = p.TOS
	binary.BigEndian.PutUint16(b[2:4], uint16(len(b)))
	binary.BigEndian.PutUint16(b[4:6], p.ID)
	binary.BigEndian.PutUint16(b[6:8], uint16(p.Flags)<<13|p.FragOff&0x1FFF)
	b[8] = p.TTL
	if b[8] == 0 {
		b[8] = 64
	}
	b[9] = p.Proto
	copy(b[12:16], p.Src[:])
	copy(b[16:20], p.Dst[:])
	copy(b[20:], p.Options)
	binary.BigEndian.PutUint16(b[10:12], Checksum(b[:hl]))
	copy(b[hl:], p.Payload)
	return b
}

// ParseIPv4 decodes an IPv4 packet, checking version, lengths and the
// header checksum; Payload is cut to the total length (padding dropped).
func ParseIPv4(b []byte) (IPv4Pkt, error) {
	if len(b) < 20 {
		return IPv4Pkt{}, ErrShort
	}
	if b[0]>>4 != 4 {
		return IPv4Pkt{}, errors.New("sim: not IPv4")
	}
	hl := int(b[0]&0xF) * 4
	tl := int(binary.BigEndian.Uint16(b[2:4]))
	if hl < 20 || tl < hl || tl > len(b) {
		return IPv4Pkt{}, errors.New("sim: bad IPv4 lengths")
	}
	if Checksum(b[:hl]) != 0 {
		return IPv4Pkt{}, errors.New("sim: bad IPv4 header checksum")
	}
	var p IPv4Pkt
	p.TOS = b[1]
	p.ID = binary.BigEndian.Uint16(b[4:6])
	ff := binary.BigEndian.Uint16(b[6:8])
	p.Flags = uint8(ff >> 13)
	p.FragOff = ff & 0x1FFF
	p.TTL = b[8]
	p.Proto = b[9]
	copy(p.Src[:], b[12:16])
	copy(p.Dst[:], b[16:20])
	p.Options = b[20:hl]
	p.Payload = b[hl:tl]
	return p, nil
}

// ICMP types.
const (
	ICMPEchoReply   uint8 = 0
	ICMPUnreachable uint8 = 3
	ICMPEchoRequest uint8 = 8
)

// ICMP is an ICMP message; for echo, ID and Seq are the rest of header.
type ICMP struct {
	Type, Code uint8
	ID, Seq    uint16
	Data       []byte
}

// Bytes encodes the message with a correct checksum.
func (m ICMP) Bytes() []byte {
	b := make([]byte, 8+len(m.Data))
	b[0], b[1] = m.Type, m.Code
	binary.BigEndian.PutUint16(b[4:6], m.ID)
	binary.BigEndian.PutUint16(b[6:8], m.Seq)
	copy(b[8:], m.Data)
	binary.BigEndian.PutUint16(b[2:4], Checksum(b))
	return b
}

// ParseICMP decodes an ICMP message, checking its checksum.
func ParseICMP(b []byte) (ICMP, error) {
	if len(b) < 8 {
		return ICMP{}, ErrShort
	}
	if Checksum(b) != 0 {
		return ICMP{}, errors.New("sim: bad ICMP checksum")
	}
	return ICMP{Type: b[0], Code: b[1], ID: binary.BigEndian.Uint16(b[4:6]),
		Seq: binary.BigEndian.Uint16(b[6:8]), Data: b[8:]}, nil
}

// UDP is a UDP datagram.
type UDP struct {
	SrcPort, DstPort uint16
	Payload          []byte
	NoChecksum       bool // encode checksum 0 (none)
}

// Bytes encodes the datagram with a correct checksum for src -> dst.
func (u UDP) Bytes(src, dst IP) []byte {
	b := make([]byte, 8+len(u.Payload))
	binary.BigEndian.PutUint16(b[0:2], u.SrcPort)
	binary.BigEndian.PutUint16(b[2:4], u.DstPort)
	binary.BigEndian.PutUint16(b[4:6], uint16(len(b)))
	copy(b[8:], u.Payload)
	if !u.NoChecksum {
		c := ^fold(sum(pseudoSum(src, dst, ProtoUDP, len(b)), b))
		if c == 0 {
			c = 0xFFFF
		}
		binary.BigEndian.PutUint16(b[6:8], c)
	}
	return b
}

// ParseUDP decodes a datagram sent src -> dst, checking its length and
// checksum (0 = none, accepted).
func ParseUDP(b []byte, src, dst IP) (UDP, error) {
	if len(b) < 8 {
		return UDP{}, ErrShort
	}
	l := int(binary.BigEndian.Uint16(b[4:6]))
	if l < 8 || l > len(b) {
		return UDP{}, errors.New("sim: bad UDP length")
	}
	b = b[:l]
	c := binary.BigEndian.Uint16(b[6:8])
	if c != 0 && fold(sum(pseudoSum(src, dst, ProtoUDP, l), b)) != 0xFFFF {
		return UDP{}, errors.New("sim: bad UDP checksum")
	}
	return UDP{SrcPort: binary.BigEndian.Uint16(b[0:2]), DstPort: binary.BigEndian.Uint16(b[2:4]),
		Payload: b[8:], NoChecksum: c == 0}, nil
}

// TCP flags.
const (
	FIN uint8 = 0x01
	SYN uint8 = 0x02
	RST uint8 = 0x04
	PSH uint8 = 0x08
	ACK uint8 = 0x10
	URG uint8 = 0x20
)

// TCP is a TCP segment.
type TCP struct {
	SrcPort, DstPort uint16
	Seq, Ack         uint32
	Flags            uint8
	Window           uint16
	Urgent           uint16
	Options          []byte // padded to a multiple of 4 by Bytes
	Payload          []byte
}

// MSSOption is the TCP maximum-segment-size option.
func MSSOption(mss uint16) []byte { return []byte{2, 4, byte(mss >> 8), byte(mss)} }

// Bytes encodes the segment with a correct checksum for src -> dst.
func (t TCP) Bytes(src, dst IP) []byte {
	opts := append([]byte(nil), t.Options...)
	for len(opts)%4 != 0 {
		opts = append(opts, 0)
	}
	hl := 20 + len(opts)
	b := make([]byte, hl+len(t.Payload))
	binary.BigEndian.PutUint16(b[0:2], t.SrcPort)
	binary.BigEndian.PutUint16(b[2:4], t.DstPort)
	binary.BigEndian.PutUint32(b[4:8], t.Seq)
	binary.BigEndian.PutUint32(b[8:12], t.Ack)
	b[12] = byte(hl/4) << 4
	b[13] = t.Flags
	binary.BigEndian.PutUint16(b[14:16], t.Window)
	binary.BigEndian.PutUint16(b[18:20], t.Urgent)
	copy(b[20:], opts)
	copy(b[hl:], t.Payload)
	binary.BigEndian.PutUint16(b[16:18], ^fold(sum(pseudoSum(src, dst, ProtoTCP, len(b)), b)))
	return b
}

// ParseTCP decodes a segment sent src -> dst, checking its checksum.
func ParseTCP(b []byte, src, dst IP) (TCP, error) {
	if len(b) < 20 {
		return TCP{}, ErrShort
	}
	hl := int(b[12]>>4) * 4
	if hl < 20 || hl > len(b) {
		return TCP{}, errors.New("sim: bad TCP header length")
	}
	if fold(sum(pseudoSum(src, dst, ProtoTCP, len(b)), b)) != 0xFFFF {
		return TCP{}, errors.New("sim: bad TCP checksum")
	}
	return TCP{
		SrcPort: binary.BigEndian.Uint16(b[0:2]), DstPort: binary.BigEndian.Uint16(b[2:4]),
		Seq: binary.BigEndian.Uint32(b[4:8]), Ack: binary.BigEndian.Uint32(b[8:12]),
		Flags: b[13], Window: binary.BigEndian.Uint16(b[14:16]), Urgent: binary.BigEndian.Uint16(b[18:20]),
		Options: b[20:hl], Payload: b[hl:],
	}, nil
}

// MSS returns the segment's MSS option, if any.
func (t TCP) MSS() (uint16, bool) {
	o := t.Options
	for len(o) > 0 {
		switch o[0] {
		case 0: // end
			return 0, false
		case 1: // nop
			o = o[1:]
			continue
		}
		if len(o) < 2 || int(o[1]) < 2 || int(o[1]) > len(o) {
			return 0, false
		}
		if o[0] == 2 && o[1] == 4 {
			return binary.BigEndian.Uint16(o[2:4]), true
		}
		o = o[o[1]:]
	}
	return 0, false
}
