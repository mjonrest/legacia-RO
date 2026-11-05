package main

import (
	"crypto/rand"
	"encoding/binary"
	"net"
	"net/netip"
	"time"
)

// Client <-> voice server UDP packets. See doc/voice_chat.md
const (
	cHello = 0x01 // (no body)
	cAudio = 0x02 // session(4) seq(2) channel(1) [whisper: target name(24)] opus...
	cPing  = 0x03 // session(4)
	cBye   = 0x04 // session(4)
	cStats = 0x05 // session(4) page(1): Player Statistics window
	cMode  = 0x06 // session(4) flags(1): modeQuiet = voice chat off, stay signed in for statistics only

	sWelcome   = 0x81 // session(4) char_id(4)
	sDenied    = 0x82 // reason(1)
	sPong      = 0x83
	sAudio     = 0x84 // speaker char_id(4) seq(2) channel(1) opus...
	sSpeaker   = 0x85 // char_id(4) name(24)
	sChallenge = 0x86 // code(16) short_code(6): send code as CZ_VOICE_CLAIM, or show short_code for @voice
	sStats     = 0x87 // page(1) seq(1) part(1) parts(1) text...: one piece of a statistics page
	sNoStats   = 0x88 // page(1): the map-server can't answer (offline or too old)

	modeQuiet = 0x01

	statsPart     = 1000 // text bytes per sStats packet
	statsMaxParts = 16

	deniedBusy           = 1
	deniedUnknownSession = 2

	maxOpusFrame = 1000
)

type UDPServer struct {
	hub  *Hub
	conn *net.UDPConn
	// called with each new code, to link it right away when the map-server
	// can tell which player the address belongs to
	OnHello func(ip netip.Addr, short string, now time.Time)
	// asks the map-server for a statistics page; false if it can't
	StatsQuery func(charID uint32, page uint8) bool
}

func NewUDPServer(hub *Hub, conn *net.UDPConn) *UDPServer {
	return &UDPServer{hub: hub, conn: conn}
}

func randomCode() (c [16]byte) {
	rand.Read(c[:])
	return
}

func randomID() uint32 {
	var b [4]byte
	rand.Read(b[:])
	return binary.LittleEndian.Uint32(b[:])
}

func (u *UDPServer) Serve() error {
	buf := make([]byte, 2048)
	for {
		n, addr, err := u.conn.ReadFromUDPAddrPort(buf)
		if err != nil {
			return err
		}
		if n < 1 {
			continue
		}
		u.handle(buf[:n], addr, time.Now())
	}
}

func (u *UDPServer) send(addr netip.AddrPort, b []byte) {
	u.conn.WriteToUDPAddrPort(b, addr)
}

// SendWelcome tells a client its claim went through.
func (u *UDPServer) SendWelcome(s *Session) {
	out := make([]byte, 9)
	out[0] = sWelcome
	binary.LittleEndian.PutUint32(out[1:], s.ID)
	binary.LittleEndian.PutUint32(out[5:], s.CharID)
	u.send(s.Addr, out)
}

// SendStats passes a Player Statistics page from the map-server on to the
// character's client, split into packets that fit in one datagram.
func (u *UDPServer) SendStats(charID uint32, page uint8, text []byte) {
	addr, seq, ok := u.hub.StatsTarget(charID)
	if !ok {
		return
	}
	parts := (len(text) + statsPart - 1) / statsPart
	if parts == 0 {
		parts = 1
	}
	if parts > statsMaxParts {
		parts = statsMaxParts
		text = text[:statsMaxParts*statsPart]
	}
	for i := 0; i < parts; i++ {
		chunk := text[i*statsPart:]
		if len(chunk) > statsPart {
			chunk = chunk[:statsPart]
		}
		out := make([]byte, 5+len(chunk))
		out[0] = sStats
		out[1] = page
		out[2] = seq
		out[3] = byte(i)
		out[4] = byte(parts)
		copy(out[5:], chunk)
		u.send(addr, out)
	}
}

func (u *UDPServer) handle(pkt []byte, addr netip.AddrPort, now time.Time) {
	switch pkt[0] {
	case cMode:
		if len(pkt) < 6 {
			return
		}
		u.hub.SetQuiet(binary.LittleEndian.Uint32(pkt[1:]), addr, pkt[5]&modeQuiet != 0, now)

	case cStats:
		if len(pkt) < 6 {
			return
		}
		charID, ok := u.hub.StatsRequest(binary.LittleEndian.Uint32(pkt[1:]), addr, now)
		if !ok {
			return
		}
		if u.StatsQuery == nil || !u.StatsQuery(charID, pkt[5]) {
			u.send(addr, []byte{sNoStats, pkt[5]})
		}

	case cHello:
		code, short, ok := u.hub.Hello(addr, randomCode, now)
		if !ok {
			u.send(addr, []byte{sDenied, deniedBusy})
			return
		}
		out := make([]byte, 17+len(short))
		out[0] = sChallenge
		copy(out[1:], code[:])
		copy(out[17:], short)
		u.send(addr, out)
		if u.OnHello != nil {
			u.OnHello(addr.Addr(), short, now)
		}

	case cPing:
		if len(pkt) < 5 {
			return
		}
		if !u.hub.Touch(binary.LittleEndian.Uint32(pkt[1:]), addr, now) {
			u.send(addr, []byte{sDenied, deniedUnknownSession})
			return
		}
		u.send(addr, []byte{sPong})

	case cBye:
		if len(pkt) < 5 {
			return
		}
		u.hub.Bye(binary.LittleEndian.Uint32(pkt[1:]), addr)

	case cAudio:
		if len(pkt) < 9 || len(pkt)-8 > maxOpusFrame {
			return
		}
		id := binary.LittleEndian.Uint32(pkt[1:])
		seq := binary.LittleEndian.Uint16(pkt[5:])
		channel := pkt[7]
		opus := pkt[8:]
		target := ""
		if channel == ChannelWhisper {
			// whispers carry the target name before the audio
			if len(opus) <= nameLength {
				return
			}
			target = cstring(opus[:nameLength])
			opus = opus[nameLength:]
		}
		speaker, out, ok := u.hub.Route(id, addr, channel, target, now)
		if !ok {
			return
		}
		if len(out) == 0 {
			return
		}
		audio := make([]byte, 8+len(opus))
		audio[0] = sAudio
		binary.LittleEndian.PutUint32(audio[1:], speaker.CharID)
		binary.LittleEndian.PutUint16(audio[5:], seq)
		audio[7] = channel
		copy(audio[8:], opus)
		var name []byte
		for _, d := range out {
			if d.SendName {
				if name == nil {
					name = make([]byte, 5+nameLength)
					name[0] = sSpeaker
					binary.LittleEndian.PutUint32(name[1:], speaker.CharID)
					copy(name[5:5+nameLength-1], speaker.Name)
				}
				u.send(d.Addr, name)
			}
			u.send(d.Addr, audio)
		}
	}
}
