package sim

import (
	"encoding/binary"
	"errors"
)

// DHCP ports.
const (
	DHCPServerPort uint16 = 67
	DHCPClientPort uint16 = 68
)

// DHCP message types (option 53).
const (
	DHCPDiscover uint8 = 1
	DHCPOffer    uint8 = 2
	DHCPRequest  uint8 = 3
	DHCPDecline  uint8 = 4
	DHCPAck      uint8 = 5
	DHCPNak      uint8 = 6
	DHCPRelease  uint8 = 7
)

// DHCP option codes.
const (
	OptSubnetMask  uint8 = 1
	OptRouter      uint8 = 3
	OptDNS         uint8 = 6
	OptRequestedIP uint8 = 50
	OptLeaseTime   uint8 = 51
	OptMsgType     uint8 = 53
	OptServerID    uint8 = 54
	OptParamList   uint8 = 55
	OptRenewalT1   uint8 = 58
	OptRebindT2    uint8 = 59
	OptEnd         uint8 = 255
)

// DHCPOpt is one option, in order.
type DHCPOpt struct {
	Code uint8
	Data []byte
}

// DHCP is a BOOTP/DHCP message (RFC 2131) over Ethernet (htype 1).
type DHCP struct {
	Op                             uint8 // 1 request, 2 reply
	XID                            uint32
	Secs, Flags                    uint16 // Flags 0x8000 = broadcast
	CIAddr, YIAddr, SIAddr, GIAddr IP
	CHAddr                         MAC
	Options                        []DHCPOpt
}

var dhcpCookie = []byte{99, 130, 83, 99}

// Bytes encodes the message (options then End; no padding to 300 bytes).
func (d DHCP) Bytes() []byte {
	b := make([]byte, 236, 300)
	b[0], b[1], b[2] = d.Op, 1, 6
	binary.BigEndian.PutUint32(b[4:8], d.XID)
	binary.BigEndian.PutUint16(b[8:10], d.Secs)
	binary.BigEndian.PutUint16(b[10:12], d.Flags)
	copy(b[12:16], d.CIAddr[:])
	copy(b[16:20], d.YIAddr[:])
	copy(b[20:24], d.SIAddr[:])
	copy(b[24:28], d.GIAddr[:])
	copy(b[28:34], d.CHAddr[:])
	b = append(b, dhcpCookie...)
	for _, o := range d.Options {
		b = append(b, o.Code, byte(len(o.Data)))
		b = append(b, o.Data...)
	}
	return append(b, OptEnd)
}

// ParseDHCP decodes a message, checking the magic cookie and option bounds.
func ParseDHCP(b []byte) (DHCP, error) {
	if len(b) < 240 {
		return DHCP{}, ErrShort
	}
	if string(b[236:240]) != string(dhcpCookie) {
		return DHCP{}, errors.New("sim: no DHCP magic cookie")
	}
	var d DHCP
	d.Op = b[0]
	d.XID = binary.BigEndian.Uint32(b[4:8])
	d.Secs = binary.BigEndian.Uint16(b[8:10])
	d.Flags = binary.BigEndian.Uint16(b[10:12])
	copy(d.CIAddr[:], b[12:16])
	copy(d.YIAddr[:], b[16:20])
	copy(d.SIAddr[:], b[20:24])
	copy(d.GIAddr[:], b[24:28])
	copy(d.CHAddr[:], b[28:34])
	o := b[240:]
	for len(o) > 0 {
		c := o[0]
		if c == OptEnd {
			return d, nil
		}
		if c == 0 { // pad
			o = o[1:]
			continue
		}
		if len(o) < 2 || int(o[1])+2 > len(o) {
			return DHCP{}, errors.New("sim: bad DHCP option")
		}
		d.Options = append(d.Options, DHCPOpt{c, o[2 : 2+o[1]]})
		o = o[2+o[1]:]
	}
	return DHCP{}, errors.New("sim: DHCP options without End")
}

// Opt returns the first option with this code.
func (d DHCP) Opt(code uint8) ([]byte, bool) {
	for _, o := range d.Options {
		if o.Code == code {
			return o.Data, true
		}
	}
	return nil, false
}

// MsgType is option 53, or 0.
func (d DHCP) MsgType() uint8 {
	if v, ok := d.Opt(OptMsgType); ok && len(v) == 1 {
		return v[0]
	}
	return 0
}

// OptIP is an option holding one address (requested IP, server id, ...).
func (d DHCP) OptIP(code uint8) (IP, bool) {
	var ip IP
	v, ok := d.Opt(code)
	if !ok || len(v) < 4 {
		return ip, false
	}
	copy(ip[:], v)
	return ip, true
}

// U32 encodes a 4-byte option value (lease time, T1, T2).
func U32(v uint32) []byte { b := make([]byte, 4); binary.BigEndian.PutUint32(b, v); return b }

// DHCPReply builds a server reply (Offer or Ack) to req offering yiaddr
// with the usual options: server id, mask, router, DNS, lease.
func DHCPReply(req DHCP, typ uint8, server, yiaddr, mask, router, dns IP, leaseS uint32) DHCP {
	return DHCP{
		Op: 2, XID: req.XID, Flags: req.Flags, YIAddr: yiaddr, SIAddr: server, CHAddr: req.CHAddr,
		Options: []DHCPOpt{
			{OptMsgType, []byte{typ}},
			{OptServerID, server[:]},
			{OptLeaseTime, U32(leaseS)},
			{OptSubnetMask, mask[:]},
			{OptRouter, router[:]},
			{OptDNS, dns[:]},
		},
	}
}
