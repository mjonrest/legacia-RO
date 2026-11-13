package main

import (
	"bufio"
	"bytes"
	"crypto/subtle"
	"encoding/binary"
	"errors"
	"io"
	"log"
	"net"
	"net/netip"
	"sync"
	"time"
)

// map-server link packets, see src/map/voice.cpp
const (
	linkHello       = 0x7600
	linkPlayer      = 0x7601
	linkLeave       = 0x7602
	linkPing        = 0x7603
	linkClaim       = 0x7604
	linkClaimShort  = 0x7605 // short_code(8) char_id(4), from @voice
	linkOnline      = 0x7606 // ip(4): a player logged in from this address
	linkClaimAuto   = 0x7607 // short_code(8) char_id(4), answer to linkAutoQuery
	linkStats       = 0x7608 // char_id(4) page(1) text: a Player Statistics page
	linkCells       = 0x7609 // char_id(4) count(2) count * {x(2) y(2) skill_id(2) kind(1)}: LGP skill cells
	linkClaimIdent  = 0x760b // short_code(8) char_id(4), answer to linkIdentQuery: the login matched
	linkAuras       = 0x760a // char_id(4) count(2) count * {account_id(4) part1(1) part2(1) part3(1)}: auras around the player
	linkSnap        = 0x760c // ticket(4) page(1) text: one page of a statistics snapshot; page snapDenied = the login didn't match a player in game
	linkHelloAck    = 0x7680
	linkPong        = 0x7681
	linkClaimResult = 0x7682 // char_id(4) result(1): 0 linked, 1 unknown or expired code
	linkAutoQuery   = 0x7683 // ip(4) short_code(8): claim this code if one player plays from ip
	linkStatsQuery  = 0x7684 // char_id(4) page(1): only sent if the map-server said it knows it
	linkCellsQuery  = 0x7685 // char_id(4): only sent if the map-server said it knows it
	linkIdentQuery  = 0x7687 // account_id(4) char_id(4) auth_code(4) short_code(8): only sent if the map-server said it knows it
	linkAurasQuery  = 0x7686 // char_id(4): only sent if the map-server said it knows it
	linkSnapQuery   = 0x7688 // ticket(4) account_id(4) char_id(4) auth_code(4): all statistics pages at once, only sent if the map-server said it knows it

	// features a map-server lists in the byte after the secret in HELLO
	featureStats = 0x01
	featureCells = 0x02
	featureAuras = 0x04
	featureIdent = 0x08 // answers linkIdentQuery
	featureSnap  = 0x10 // answers linkSnapQuery

	snapDenied = 0xff // linkSnap page: no such player in game with that login

	secretLength       = 32
	nameLength         = 24
	mapLength          = 16
	playerPacketLength = 4 + 4*4 + 2*2 + 1 + mapLength + nameLength // older map-servers stop here
	chatIDOffset       = playerPacketLength - 4                     // then chat_id(4), in the body
	claimPacketLength  = 4 + 16 + 4
)

// LinkServer accepts the map-server connection. Only one map-server link is
// active at a time; a new authenticated link replaces the old one.
type LinkServer struct {
	hub    *Hub
	secret [secretLength]byte
	// called when a claim bound a UDP endpoint to a character
	OnClaim func(*Session)
	// called with a Player Statistics page from the map-server
	OnStats func(charID uint32, page uint8, text []byte)
	// called with the skill cells around a character from the map-server
	OnCells func(charID uint32, cells []byte)
	// called with the auras of the players around a character from the map-server
	OnAuras func(charID uint32, auras []byte)
	// called with one page of a statistics snapshot from the map-server
	OnSnap func(ticket uint32, page uint8, text []byte)

	mu      sync.Mutex // guards current and writes to it
	current net.Conn
	// the current map-server answers linkStatsQuery (older ones close the
	// link on packets they don't know)
	statsOK bool
	cellsOK bool
	aurasOK bool
	identOK bool // and linkIdentQuery
	snapOK  bool // and linkSnapQuery
	// last aura query and answer written to the log (one a minute each)
	aurasAskLog, aurasAnswerLog time.Time
	// last auto query per address, so UDP hellos can't flood the map-server
	queried map[netip.Addr]time.Time
	// last ident query per address
	identQueried map[netip.Addr]time.Time
	// last snapshot query per address
	snapQueried map[netip.Addr]time.Time
	// and overall, since each query makes the map-server look at every player
	queryTokens float64
	queryRefill time.Time
}

func NewLinkServer(hub *Hub, secret string) *LinkServer {
	ls := &LinkServer{hub: hub, queried: map[netip.Addr]time.Time{}, identQueried: map[netip.Addr]time.Time{},
		snapQueried: map[netip.Addr]time.Time{}}
	copy(ls.secret[:], secret)
	return ls
}

func (ls *LinkServer) Serve(l net.Listener) error {
	for {
		c, err := l.Accept()
		if err != nil {
			return err
		}
		go ls.handle(c)
	}
}

func readPacket(r *bufio.Reader) (cmd uint16, body []byte, err error) {
	var hdr [4]byte
	if _, err = io.ReadFull(r, hdr[:]); err != nil {
		return
	}
	cmd = binary.LittleEndian.Uint16(hdr[0:])
	length := binary.LittleEndian.Uint16(hdr[2:])
	if length < 4 {
		return 0, nil, errors.New("bad packet length")
	}
	body = make([]byte, length-4)
	_, err = io.ReadFull(r, body)
	return
}

func writePacket(w io.Writer, cmd uint16, body []byte) error {
	buf := make([]byte, 4+len(body))
	binary.LittleEndian.PutUint16(buf[0:], cmd)
	binary.LittleEndian.PutUint16(buf[2:], uint16(4+len(body)))
	copy(buf[4:], body)
	_, err := w.Write(buf)
	return err
}

func cstring(b []byte) string {
	if i := bytes.IndexByte(b, 0); i >= 0 {
		b = b[:i]
	}
	return string(b)
}

// send writes a packet to the link, from any goroutine.
func (ls *LinkServer) send(c net.Conn, cmd uint16, body []byte) error {
	ls.mu.Lock()
	defer ls.mu.Unlock()
	return writePacket(c, cmd, body)
}

const (
	autoQueryInterval = 2 * time.Second
	autoQueryRate     = 20 // per second, for all addresses together
)

// allowQuery applies the per-address and overall limits shared by auto and
// ident queries. Called with ls.mu held.
func (ls *LinkServer) allowQueryLocked(seen map[netip.Addr]time.Time, ip netip.Addr, now time.Time) bool {
	return ls.allowQueryEveryLocked(seen, ip, now, autoQueryInterval)
}

func (ls *LinkServer) allowQueryEveryLocked(seen map[netip.Addr]time.Time, ip netip.Addr, now time.Time, every time.Duration) bool {
	if now.Sub(seen[ip]) < every {
		return false
	}
	if len(seen) > maxPending {
		for k, t := range seen {
			if now.Sub(t) >= every {
				delete(seen, k)
			}
		}
		if len(seen) > maxPending {
			return false
		}
	}
	ls.queryTokens += now.Sub(ls.queryRefill).Seconds() * autoQueryRate
	if ls.queryTokens > autoQueryRate {
		ls.queryTokens = autoQueryRate
	}
	ls.queryRefill = now
	if ls.queryTokens < 1 {
		return false
	}
	ls.queryTokens--
	seen[ip] = now
	return true
}

// IdentQuery asks the map-server whether accountID/charID is in game with
// this login code (the game client's own, read by the plugin), and to claim
// short for that character if so. Works when several players share an
// address. Limited like AutoQuery, so a 32-bit login code can't be guessed.
func (ls *LinkServer) IdentQuery(ip netip.Addr, accountID, charID, authCode uint32, short string, now time.Time) {
	ip = ip.Unmap()
	ls.mu.Lock()
	if ls.current == nil || !ls.identOK || !ls.allowQueryLocked(ls.identQueried, ip, now) {
		ls.mu.Unlock()
		return
	}
	c := ls.current
	ls.mu.Unlock()
	b := make([]byte, 20)
	binary.LittleEndian.PutUint32(b[0:], accountID)
	binary.LittleEndian.PutUint32(b[4:], charID)
	binary.LittleEndian.PutUint32(b[8:], authCode)
	copy(b[12:], short)
	ls.send(c, linkIdentQuery, b)
}

// AutoQuery asks the map-server to claim short for the player at ip, if
// exactly one plays from there. Called for every new UDP hello, so it is
// limited per address.
func (ls *LinkServer) AutoQuery(ip netip.Addr, short string, now time.Time) {
	ip = ip.Unmap()
	if !ip.Is4() {
		return
	}
	ls.mu.Lock()
	if !ls.allowQueryLocked(ls.queried, ip, now) {
		ls.mu.Unlock()
		return
	}
	c := ls.current
	ls.mu.Unlock()
	if c != nil {
		ls.autoQuery(c, ip, short)
	}
}

func (ls *LinkServer) autoQuery(c net.Conn, ip netip.Addr, short string) {
	b := make([]byte, 12)
	a := ip.As4()
	copy(b, a[:])
	copy(b[4:], short)
	ls.send(c, linkAutoQuery, b)
}

// StatsQuery asks the map-server for a Player Statistics page. Returns
// false when no map-server is linked or it is too old to answer.
func (ls *LinkServer) StatsQuery(charID uint32, page uint8) bool {
	ls.mu.Lock()
	c, ok := ls.current, ls.statsOK
	ls.mu.Unlock()
	if c == nil || !ok {
		return false
	}
	b := make([]byte, 5)
	binary.LittleEndian.PutUint32(b, charID)
	b[4] = page
	return ls.send(c, linkStatsQuery, b) == nil
}

// CellsQuery asks the map-server for the LGP skill cells around a
// character. Returns false when no map-server is linked or it is too old.
func (ls *LinkServer) CellsQuery(charID uint32) bool {
	ls.mu.Lock()
	c, ok := ls.current, ls.cellsOK
	ls.mu.Unlock()
	if c == nil || !ok {
		return false
	}
	b := make([]byte, 4)
	binary.LittleEndian.PutUint32(b, charID)
	return ls.send(c, linkCellsQuery, b) == nil
}

// snapQueryInterval: snapshot queries per address. Several players may share
// one address and the plugin asks again until it has every page, so this is
// shorter than the ident limit; the overall limit still applies.
const snapQueryInterval = time.Second

// SnapQueryAvailable says whether the linked map-server answers snapshot queries.
func (ls *LinkServer) SnapQueryAvailable() bool {
	ls.mu.Lock()
	defer ls.mu.Unlock()
	return ls.current != nil && ls.snapOK
}

// SnapQuery asks the map-server for every Player Statistics page of the
// character this game login belongs to, answered with linkSnap under ticket.
// No voice sign-in is needed: the login code (the game client's own) proves
// whose statistics they are. Returns false when the query wasn't sent (no
// map-server, too old, or over the query limits).
func (ls *LinkServer) SnapQuery(ip netip.Addr, ticket, accountID, charID, authCode uint32, now time.Time) bool {
	ip = ip.Unmap()
	ls.mu.Lock()
	if ls.current == nil || !ls.snapOK || !ls.allowQueryEveryLocked(ls.snapQueried, ip, now, snapQueryInterval) {
		ls.mu.Unlock()
		return false
	}
	c := ls.current
	ls.mu.Unlock()
	b := make([]byte, 16)
	binary.LittleEndian.PutUint32(b[0:], ticket)
	binary.LittleEndian.PutUint32(b[4:], accountID)
	binary.LittleEndian.PutUint32(b[8:], charID)
	binary.LittleEndian.PutUint32(b[12:], authCode)
	return ls.send(c, linkSnapQuery, b) == nil
}

// AurasQuery asks the map-server for the auras of the players around a
// character. Returns false when no map-server is linked or it is too old.
func (ls *LinkServer) AurasQuery(charID uint32) bool {
	ls.mu.Lock()
	c, ok := ls.current, ls.aurasOK
	ls.mu.Unlock()
	if c == nil || !ok {
		return false
	}
	b := make([]byte, 4)
	binary.LittleEndian.PutUint32(b, charID)
	err := ls.send(c, linkAurasQuery, b)
	ls.mu.Lock()
	if now := time.Now(); now.Sub(ls.aurasAskLog) >= time.Minute {
		ls.aurasAskLog = now
		log.Printf("auras: asked the map-server for char %d (err %v)", charID, err)
	}
	ls.mu.Unlock()
	return err == nil
}

func (ls *LinkServer) handle(c net.Conn) {
	defer c.Close()
	r := bufio.NewReader(c)

	c.SetReadDeadline(time.Now().Add(10 * time.Second))
	cmd, body, err := readPacket(r)
	if err != nil || cmd != linkHello || len(body) < secretLength {
		log.Printf("link %s: no hello, closing", c.RemoteAddr())
		return
	}
	if subtle.ConstantTimeCompare(body[:secretLength], ls.secret[:]) != 1 {
		log.Printf("link %s: wrong secret, closing", c.RemoteAddr())
		writePacket(c, linkHelloAck, []byte{1})
		return
	}

	ls.mu.Lock()
	if ls.current != nil {
		ls.current.Close()
	}
	ls.current = c
	ls.statsOK = len(body) > secretLength && body[secretLength]&featureStats != 0
	ls.cellsOK = len(body) > secretLength && body[secretLength]&featureCells != 0
	ls.aurasOK = len(body) > secretLength && body[secretLength]&featureAuras != 0
	ls.identOK = len(body) > secretLength && body[secretLength]&featureIdent != 0
	ls.snapOK = len(body) > secretLength && body[secretLength]&featureSnap != 0
	ls.mu.Unlock()
	defer func() {
		ls.mu.Lock()
		if ls.current == c {
			ls.current = nil
		}
		ls.mu.Unlock()
	}()

	ls.hub.ResetPlayers()
	if err := ls.send(c, linkHelloAck, []byte{0}); err != nil {
		return
	}
	ls.mu.Lock()
	log.Printf("link %s: map-server connected (stats %v, cells %v, auras %v, game login %v, stats snapshot %v)",
		c.RemoteAddr(), ls.statsOK, ls.cellsOK, ls.aurasOK, ls.identOK, ls.snapOK)
	ls.mu.Unlock()

	for {
		// the map-server pings when the link is idle (stall_time, 60s by default)
		c.SetReadDeadline(time.Now().Add(5 * time.Minute))
		cmd, body, err := readPacket(r)
		if err != nil {
			log.Printf("link %s: closed: %v", c.RemoteAddr(), err)
			return
		}
		switch cmd {
		case linkPlayer:
			if len(body) < playerPacketLength-4 {
				log.Printf("link: short player packet")
				return
			}
			var p Player
			p.AccountID = binary.LittleEndian.Uint32(body[0:])
			p.CharID = binary.LittleEndian.Uint32(body[4:])
			p.PartyID = binary.LittleEndian.Uint32(body[8:])
			p.GuildID = binary.LittleEndian.Uint32(body[12:])
			p.X = binary.LittleEndian.Uint16(body[16:])
			p.Y = binary.LittleEndian.Uint16(body[18:])
			p.Flags = body[20]
			p.Map = cstring(body[21 : 21+mapLength])
			p.Name = cstring(body[21+mapLength : 21+mapLength+nameLength])
			if len(body) >= chatIDOffset+4 {
				p.ChatID = binary.LittleEndian.Uint32(body[chatIDOffset:])
			}
			ls.hub.UpsertPlayer(p)
		case linkClaim:
			if len(body) < claimPacketLength-4 {
				return
			}
			var code [16]byte
			copy(code[:], body[0:16])
			charID := binary.LittleEndian.Uint32(body[16:])
			if sess, ok := ls.hub.Claim(code, charID, randomID, time.Now()); ok && ls.OnClaim != nil {
				ls.OnClaim(sess)
			}
		case linkClaimShort, linkClaimAuto:
			if len(body) < 12 {
				return
			}
			short := cstring(body[0:8])
			charID := binary.LittleEndian.Uint32(body[8:])
			if cmd == linkClaimAuto && ls.hub.HasSession(charID) {
				// linking by address never takes over a working client: a
				// HELLO with a forged source address could otherwise cut it off
				break
			}
			sess, ok := ls.hub.ClaimShort(short, charID, randomID, time.Now())
			if ok && ls.OnClaim != nil {
				ls.OnClaim(sess)
			}
			if !ok && cmd == linkClaimAuto {
				// the client asked for a new code meanwhile; nothing to tell the player
				break
			}
			res := make([]byte, 5)
			binary.LittleEndian.PutUint32(res, charID)
			if !ok {
				res[4] = 1
			}
			if err := ls.send(c, linkClaimResult, res); err != nil {
				return
			}
		case linkClaimIdent:
			if len(body) < 12 {
				return
			}
			// the map-server matched the game's own login: this is the
			// player's client, so it may replace an older session
			charID := binary.LittleEndian.Uint32(body[8:])
			sess, ok := ls.hub.ClaimShort(cstring(body[0:8]), charID, randomID, time.Now())
			if !ok {
				break
			}
			if ls.OnClaim != nil {
				ls.OnClaim(sess)
			}
			res := make([]byte, 5)
			binary.LittleEndian.PutUint32(res, charID)
			if err := ls.send(c, linkClaimResult, res); err != nil {
				return
			}
		case linkOnline:
			if len(body) < 4 {
				return
			}
			ip := netip.AddrFrom4([4]byte(body[0:4]))
			if short, ok := ls.hub.NewestPending(ip); ok {
				ls.autoQuery(c, ip, short)
			}
		case linkStats:
			if len(body) < 5 {
				return
			}
			if ls.OnStats != nil {
				ls.OnStats(binary.LittleEndian.Uint32(body), body[4], body[5:])
			}
		case linkCells:
			if len(body) < 6 {
				return
			}
			n := int(binary.LittleEndian.Uint16(body[4:]))
			if len(body) < 6+n*cellSize {
				log.Printf("link: short cells packet")
				return
			}
			if ls.OnCells != nil {
				ls.OnCells(binary.LittleEndian.Uint32(body), body[4:6+n*cellSize])
			}
		case linkSnap:
			if len(body) < 5 {
				return
			}
			if ls.OnSnap != nil {
				ls.OnSnap(binary.LittleEndian.Uint32(body), body[4], body[5:])
			}
		case linkAuras:
			if len(body) < 6 {
				return
			}
			n := int(binary.LittleEndian.Uint16(body[4:]))
			if len(body) < 6+n*auraSize {
				log.Printf("link: short auras packet")
				return
			}
			ls.mu.Lock()
			if now := time.Now(); now.Sub(ls.aurasAnswerLog) >= time.Minute {
				ls.aurasAnswerLog = now
				log.Printf("auras: map-server answered char %d with %d aura(s)", binary.LittleEndian.Uint32(body), n)
			}
			ls.mu.Unlock()
			if ls.OnAuras != nil {
				ls.OnAuras(binary.LittleEndian.Uint32(body), body[4:6+n*auraSize])
			}
		case linkLeave:
			if len(body) < 4 {
				return
			}
			ls.hub.RemovePlayer(binary.LittleEndian.Uint32(body))
		case linkPing:
			if err := ls.send(c, linkPong, nil); err != nil {
				return
			}
		default:
			log.Printf("link: unknown packet 0x%04x", cmd)
			return
		}
	}
}
