package sim

import (
	"encoding/binary"
	"errors"
	"strings"
)

// DNS constants.
const (
	DNSPort    uint16 = 53
	DNSTypeA   uint16 = 1
	DNSClassIN uint16 = 1
	DNSFlagQR  uint16 = 0x8000 // response
	DNSFlagRD  uint16 = 0x0100
	DNSFlagRA  uint16 = 0x0080
	DNSRcodeNX uint16 = 3 // NXDOMAIN (low 4 bits of Flags)
)

// DNSQuestion is one question.
type DNSQuestion struct {
	Name        string // dotted, no trailing dot
	Type, Class uint16
}

// DNSRR is one resource record (answers only; authority and additional
// sections are skipped by ParseDNS and never built).
type DNSRR struct {
	Name        string
	Type, Class uint16
	TTL         uint32
	Data        []byte
}

// DNS is a DNS message.
type DNS struct {
	ID, Flags uint16
	Questions []DNSQuestion
	Answers   []DNSRR
}

// ARecord is an IN A answer.
func ARecord(name string, ip IP, ttl uint32) DNSRR {
	return DNSRR{Name: name, Type: DNSTypeA, Class: DNSClassIN, TTL: ttl, Data: append([]byte(nil), ip[:]...)}
}

func putName(b []byte, name string) []byte {
	if name != "" {
		for _, l := range strings.Split(name, ".") {
			b = append(b, byte(len(l)))
			b = append(b, l...)
		}
	}
	return append(b, 0)
}

// Bytes encodes the message (no name compression).
func (d DNS) Bytes() []byte {
	b := make([]byte, 12)
	binary.BigEndian.PutUint16(b[0:2], d.ID)
	binary.BigEndian.PutUint16(b[2:4], d.Flags)
	binary.BigEndian.PutUint16(b[4:6], uint16(len(d.Questions)))
	binary.BigEndian.PutUint16(b[6:8], uint16(len(d.Answers)))
	var u [10]byte
	for _, q := range d.Questions {
		b = putName(b, q.Name)
		binary.BigEndian.PutUint16(u[0:2], q.Type)
		binary.BigEndian.PutUint16(u[2:4], q.Class)
		b = append(b, u[:4]...)
	}
	for _, r := range d.Answers {
		b = putName(b, r.Name)
		binary.BigEndian.PutUint16(u[0:2], r.Type)
		binary.BigEndian.PutUint16(u[2:4], r.Class)
		binary.BigEndian.PutUint32(u[4:8], r.TTL)
		binary.BigEndian.PutUint16(u[8:10], uint16(len(r.Data)))
		b = append(b, u[:]...)
		b = append(b, r.Data...)
	}
	return b
}

var errDNS = errors.New("sim: malformed DNS message")

// name reads a possibly compressed name at off; returns it and the offset
// after it (in the original position).
func readName(b []byte, off int) (string, int, error) {
	var labels []string
	end := -1
	for jumps := 0; ; {
		if off >= len(b) {
			return "", 0, errDNS
		}
		l := int(b[off])
		switch {
		case l == 0:
			if end < 0 {
				end = off + 1
			}
			return strings.Join(labels, "."), end, nil
		case l&0xC0 == 0xC0:
			if off+1 >= len(b) || jumps > 16 {
				return "", 0, errDNS
			}
			if end < 0 {
				end = off + 2
			}
			off = int(binary.BigEndian.Uint16(b[off:]) & 0x3FFF)
			jumps++
		case l&0xC0 != 0:
			return "", 0, errDNS
		default:
			if off+1+l > len(b) {
				return "", 0, errDNS
			}
			labels = append(labels, string(b[off+1:off+1+l]))
			off += 1 + l
		}
	}
}

// ParseDNS decodes a message's header, questions and answers.
func ParseDNS(b []byte) (DNS, error) {
	if len(b) < 12 {
		return DNS{}, ErrShort
	}
	d := DNS{ID: binary.BigEndian.Uint16(b[0:2]), Flags: binary.BigEndian.Uint16(b[2:4])}
	qd, an := int(binary.BigEndian.Uint16(b[4:6])), int(binary.BigEndian.Uint16(b[6:8]))
	off := 12
	for i := 0; i < qd; i++ {
		n, o, err := readName(b, off)
		if err != nil || o+4 > len(b) {
			return DNS{}, errDNS
		}
		d.Questions = append(d.Questions, DNSQuestion{n, binary.BigEndian.Uint16(b[o:]), binary.BigEndian.Uint16(b[o+2:])})
		off = o + 4
	}
	for i := 0; i < an; i++ {
		n, o, err := readName(b, off)
		if err != nil || o+10 > len(b) {
			return DNS{}, errDNS
		}
		rl := int(binary.BigEndian.Uint16(b[o+8:]))
		if o+10+rl > len(b) {
			return DNS{}, errDNS
		}
		d.Answers = append(d.Answers, DNSRR{n, binary.BigEndian.Uint16(b[o:]), binary.BigEndian.Uint16(b[o+2:]),
			binary.BigEndian.Uint32(b[o+4:]), b[o+10 : o+10+rl]})
		off = o + 10 + rl
	}
	return d, nil
}

// DNSAnswer builds the response to query q answering its first question
// with the given A records (none and nx: NXDOMAIN).
func DNSAnswer(q DNS, nx bool, answers ...DNSRR) DNS {
	r := DNS{ID: q.ID, Flags: DNSFlagQR | DNSFlagRA | (q.Flags & DNSFlagRD), Questions: q.Questions, Answers: answers}
	if nx {
		r.Flags |= DNSRcodeNX
	}
	return r
}
