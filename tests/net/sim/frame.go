package sim

import "fmt"

// Host is one end of a frame: its MAC and IPv4 address.
type Host struct {
	MAC MAC
	IP  IP
}

func ipFrame(from, to Host, proto uint8, payload []byte) []byte {
	ip := IPv4Pkt{Proto: proto, Src: from.IP, Dst: to.IP, Payload: payload}
	return Pad(Eth{Dst: to.MAC, Src: from.MAC, Type: EtherIPv4, Payload: ip.Bytes()}.Bytes())
}

// UDPFrame is a UDP datagram from -> to in an IPv4 packet in an Ethernet
// frame.
func UDPFrame(from, to Host, u UDP) []byte {
	return ipFrame(from, to, ProtoUDP, u.Bytes(from.IP, to.IP))
}

// TCPFrame is a TCP segment from -> to, framed like UDPFrame.
func TCPFrame(from, to Host, t TCP) []byte {
	return ipFrame(from, to, ProtoTCP, t.Bytes(from.IP, to.IP))
}

// ICMPFrame is an ICMP message from -> to, framed like UDPFrame.
func ICMPFrame(from, to Host, m ICMP) []byte {
	return ipFrame(from, to, ProtoICMP, m.Bytes())
}

// Packet is a decoded frame: Eth always, then at most one of ARP or IP,
// and for IP at most one of ICMP, UDP or TCP.
type Packet struct {
	Eth  Eth
	ARP  *ARP
	IP   *IPv4Pkt
	ICMP *ICMP
	UDP  *UDP
	TCP  *TCP
}

// Decode parses a frame down to its transport layer, checking every
// length and checksum on the way.
func Decode(frame []byte) (Packet, error) {
	var p Packet
	e, err := ParseEth(frame)
	if err != nil {
		return p, err
	}
	p.Eth = e
	switch e.Type {
	case EtherARP:
		a, err := ParseARP(e.Payload)
		if err != nil {
			return p, err
		}
		p.ARP = &a
	case EtherIPv4:
		ip, err := ParseIPv4(e.Payload)
		if err != nil {
			return p, err
		}
		p.IP = &ip
		if ip.Flags&1 != 0 || ip.FragOff != 0 {
			return p, nil // a fragment: no transport decode
		}
		switch ip.Proto {
		case ProtoICMP:
			m, err := ParseICMP(ip.Payload)
			if err != nil {
				return p, err
			}
			p.ICMP = &m
		case ProtoUDP:
			u, err := ParseUDP(ip.Payload, ip.Src, ip.Dst)
			if err != nil {
				return p, err
			}
			p.UDP = &u
		case ProtoTCP:
			t, err := ParseTCP(ip.Payload, ip.Src, ip.Dst)
			if err != nil {
				return p, err
			}
			p.TCP = &t
		}
	default:
		return p, fmt.Errorf("sim: unknown EtherType %#04x", e.Type)
	}
	return p, nil
}
