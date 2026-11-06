package main

import (
	"net/netip"
	"sync"
	"time"
)

// Voice channels a client can talk on.
const (
	ChannelNormal  = 0 // players on the same map within range
	ChannelParty   = 1
	ChannelGuild   = 2
	ChannelRoom    = 3 // players in the same in-game chat room
	ChannelWhisper = 4 // one player, picked by name
)

// Player flags sent by the map-server.
const (
	FlagMuted     = 0x01
	FlagNoWhisper = 0x02 // the player blocks all whispers in game
)

// Player is the game-side state of a character that linked a voice client,
// as reported by the map-server.
type Player struct {
	AccountID uint32
	CharID    uint32
	PartyID   uint32
	GuildID   uint32
	X, Y      uint16
	Flags     uint8
	Map       string
	Name      string
	ChatID    uint32
}

// Session is a client's UDP endpoint after its claim code went through the
// game connection and the map-server confirmed the character.
type Session struct {
	ID       uint32
	CharID   uint32
	Addr     netip.AddrPort
	LastSeen time.Time
	// speakers this listener has already been told the name of
	known map[uint32]bool
	// simple token bucket for flood protection
	tokens     float64
	lastRefill time.Time
	// Player Statistics window requests
	lastStats time.Time
	statsSeq  uint8
	// LGP skill cell requests
	lastCells time.Time
	// the player turned voice chat off: the client stays signed in only
	// for the Player Statistics window, so it neither talks nor hears
	Quiet bool
}

type pending struct {
	addr    netip.AddrPort
	created time.Time
	short   string // the code as players type it with @voice
}

const (
	maxPending = 10000
	// long enough to read the code and type @voice in game; the client asks
	// for a new code before this runs out
	pendingTTL = 10 * time.Minute
)

// shortAlphabet has no 0/O or 1/I, so typed codes are not mistaken.
const shortAlphabet = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"

// ShortCode turns the first 30 bits of a claim code into 6 typeable characters.
func ShortCode(code [16]byte) string {
	v := uint32(code[0]) | uint32(code[1])<<8 | uint32(code[2])<<16 | uint32(code[3])<<24
	b := make([]byte, 6)
	for i := range b {
		b[i] = shortAlphabet[v&31]
		v >>= 5
	}
	return string(b)
}

// Hub holds all players and sessions. Safe for concurrent use.
type Hub struct {
	mu         sync.Mutex
	players    map[uint32]*Player   // char_id -> player
	byName     map[string]uint32    // character name -> char_id, for whispers
	pending    map[[16]byte]pending // claim code -> UDP endpoint waiting for it
	byShort    map[string][16]byte  // short code -> claim code
	pendByAddr map[netip.AddrPort][16]byte
	sessions   map[uint32]*Session // session id -> session
	byChar     map[uint32]*Session // char_id -> session
	normalDist int
	rate       float64 // packets per second allowed per session
}

func NewHub(normalDist int, rate float64) *Hub {
	return &Hub{
		players:    map[uint32]*Player{},
		byName:     map[string]uint32{},
		pending:    map[[16]byte]pending{},
		byShort:    map[string][16]byte{},
		pendByAddr: map[netip.AddrPort][16]byte{},
		sessions:   map[uint32]*Session{},
		byChar:     map[uint32]*Session{},
		normalDist: normalDist,
		rate:       rate,
	}
}

// ResetPlayers is called when a new map-server link is accepted; the
// map-server sends every player again right after. Sessions are kept and
// start working again as soon as their player is sent.
func (h *Hub) ResetPlayers() {
	h.mu.Lock()
	defer h.mu.Unlock()
	h.players = map[uint32]*Player{}
	h.byName = map[string]uint32{}
}

func (h *Hub) UpsertPlayer(p Player) {
	h.mu.Lock()
	defer h.mu.Unlock()
	if old, ok := h.players[p.CharID]; ok && h.byName[old.Name] == p.CharID {
		delete(h.byName, old.Name)
	}
	cp := p
	h.players[p.CharID] = &cp
	h.byName[p.Name] = p.CharID
}

func (h *Hub) RemovePlayer(charID uint32) {
	h.mu.Lock()
	defer h.mu.Unlock()
	if old, ok := h.players[charID]; ok && h.byName[old.Name] == charID {
		delete(h.byName, old.Name)
	}
	delete(h.players, charID)
	h.dropSessionLocked(charID)
}

func (h *Hub) dropSessionLocked(charID uint32) {
	if s, ok := h.byChar[charID]; ok {
		delete(h.sessions, s.ID)
		delete(h.byChar, charID)
	}
}

func (h *Hub) dropPendingLocked(code [16]byte) {
	if pd, ok := h.pending[code]; ok {
		delete(h.pending, code)
		delete(h.pendByAddr, pd.addr)
		delete(h.byShort, pd.short)
	}
}

// Hello registers a UDP endpoint and returns the claim code the client must
// send through its game connection, and the short form players can type with
// @voice instead. A new hello from the same endpoint replaces its old code.
func (h *Hub) Hello(addr netip.AddrPort, newCode func() [16]byte, now time.Time) ([16]byte, string, bool) {
	h.mu.Lock()
	defer h.mu.Unlock()
	if old, ok := h.pendByAddr[addr]; ok {
		h.dropPendingLocked(old)
	}
	if len(h.pending) >= maxPending {
		return [16]byte{}, "", false
	}
	code := newCode()
	short := ShortCode(code)
	for tries := 0; h.byShort[short] != ([16]byte{}) || h.pending[code].short != ""; tries++ {
		if tries > 10 {
			return [16]byte{}, "", false
		}
		code = newCode()
		short = ShortCode(code)
	}
	h.pending[code] = pending{addr: addr, created: now, short: short}
	h.pendByAddr[addr] = code
	h.byShort[short] = code
	return code, short, true
}

// ClaimShort is Claim for a code typed in game with @voice.
func (h *Hub) ClaimShort(short string, charID uint32, newID func() uint32, now time.Time) (*Session, bool) {
	h.mu.Lock()
	code, ok := h.byShort[short]
	h.mu.Unlock()
	if !ok {
		return nil, false
	}
	return h.Claim(code, charID, newID, now)
}

// HasSession reports whether the character already has a voice client.
func (h *Hub) HasSession(charID uint32) bool {
	h.mu.Lock()
	defer h.mu.Unlock()
	return h.byChar[charID] != nil
}

// NewestPending returns the short code of the newest client still waiting
// for a claim from this address, for linking it without @voice.
func (h *Hub) NewestPending(ip netip.Addr) (string, bool) {
	h.mu.Lock()
	defer h.mu.Unlock()
	ip = ip.Unmap()
	var best pending
	for _, code := range h.pendByAddr {
		pd := h.pending[code]
		if pd.addr.Addr().Unmap() == ip && pd.created.After(best.created) {
			best = pd
		}
	}
	return best.short, best.short != ""
}

// Claim binds the endpoint waiting for code to the character the map-server
// says sent it. The player must already be known.
func (h *Hub) Claim(code [16]byte, charID uint32, newID func() uint32, now time.Time) (*Session, bool) {
	h.mu.Lock()
	defer h.mu.Unlock()
	pd, ok := h.pending[code]
	if !ok || now.Sub(pd.created) > pendingTTL {
		return nil, false
	}
	if _, ok := h.players[charID]; !ok {
		return nil, false
	}
	h.dropPendingLocked(code)
	h.dropSessionLocked(charID)
	// one session per endpoint as well
	for id, s := range h.sessions {
		if s.Addr == pd.addr {
			delete(h.sessions, id)
			delete(h.byChar, s.CharID)
		}
	}
	id := newID()
	for id == 0 || h.sessions[id] != nil {
		id = newID()
	}
	s := &Session{
		ID: id, CharID: charID, Addr: pd.addr, LastSeen: now,
		known: map[uint32]bool{}, tokens: h.rate, lastRefill: now,
	}
	h.sessions[id] = s
	h.byChar[charID] = s
	cp := *s
	return &cp, true
}

// Touch validates a session id against the sender address and refreshes it.
func (h *Hub) Touch(id uint32, addr netip.AddrPort, now time.Time) bool {
	h.mu.Lock()
	defer h.mu.Unlock()
	s, ok := h.sessions[id]
	if !ok || s.Addr != addr {
		return false
	}
	s.LastSeen = now
	return true
}

// statsInterval is the shortest time between two Player Statistics requests
// from one client; the window refreshes every few seconds.
const statsInterval = 250 * time.Millisecond

// StatsRequest checks a Player Statistics request from a signed-in client
// and returns its character. Requests faster than statsInterval are dropped.
func (h *Hub) StatsRequest(id uint32, addr netip.AddrPort, now time.Time) (uint32, bool) {
	h.mu.Lock()
	defer h.mu.Unlock()
	s, ok := h.sessions[id]
	if !ok || s.Addr != addr || now.Sub(s.lastStats) < statsInterval {
		return 0, false
	}
	s.lastStats = now
	s.LastSeen = now
	return s.CharID, true
}

// cellsInterval is the shortest time between two LGP skill cell requests
// from one client; it asks twice a second.
const cellsInterval = 250 * time.Millisecond

// CellsRequest checks an LGP skill cell request from a signed-in client and
// returns its character. Requests faster than cellsInterval are dropped.
func (h *Hub) CellsRequest(id uint32, addr netip.AddrPort, now time.Time) (uint32, bool) {
	h.mu.Lock()
	defer h.mu.Unlock()
	s, ok := h.sessions[id]
	if !ok || s.Addr != addr || now.Sub(s.lastCells) < cellsInterval {
		return 0, false
	}
	s.lastCells = now
	s.LastSeen = now
	return s.CharID, true
}

// CellsTarget returns where to send a character's skill cells.
func (h *Hub) CellsTarget(charID uint32) (netip.AddrPort, bool) {
	h.mu.Lock()
	defer h.mu.Unlock()
	s := h.byChar[charID]
	if s == nil {
		return netip.AddrPort{}, false
	}
	return s.Addr, true
}

// StatsTarget returns where to send a character's statistics page, and a
// number that tells this answer apart from the previous one.
func (h *Hub) StatsTarget(charID uint32) (netip.AddrPort, uint8, bool) {
	h.mu.Lock()
	defer h.mu.Unlock()
	s := h.byChar[charID]
	if s == nil {
		return netip.AddrPort{}, 0, false
	}
	s.statsSeq++
	return s.Addr, s.statsSeq, true
}

// SetQuiet switches a session between voice chat and statistics only.
func (h *Hub) SetQuiet(id uint32, addr netip.AddrPort, quiet bool, now time.Time) bool {
	h.mu.Lock()
	defer h.mu.Unlock()
	s, ok := h.sessions[id]
	if !ok || s.Addr != addr {
		return false
	}
	s.Quiet = quiet
	s.LastSeen = now
	return true
}

func (h *Hub) Bye(id uint32, addr netip.AddrPort) {
	h.mu.Lock()
	defer h.mu.Unlock()
	if s, ok := h.sessions[id]; ok && s.Addr == addr {
		delete(h.sessions, id)
		delete(h.byChar, s.CharID)
	}
}

// Delivery is one outgoing audio packet, plus an optional speaker name
// packet the listener has not seen yet.
type Delivery struct {
	Addr     netip.AddrPort
	SendName bool
}

// Route checks a talking session and returns who should hear it.
// target is the character name for ChannelWhisper and ignored otherwise.
// ok is false when the sender is unknown, flooding, muted or not allowed
// on the channel.
func (h *Hub) Route(id uint32, addr netip.AddrPort, channel uint8, target string, now time.Time) (speaker Player, out []Delivery, ok bool) {
	h.mu.Lock()
	defer h.mu.Unlock()
	s, found := h.sessions[id]
	if !found || s.Addr != addr {
		return Player{}, nil, false
	}
	s.LastSeen = now
	if s.Quiet {
		return Player{}, nil, false
	}
	// token bucket
	s.tokens += now.Sub(s.lastRefill).Seconds() * h.rate
	if s.tokens > h.rate {
		s.tokens = h.rate
	}
	s.lastRefill = now
	if s.tokens < 1 {
		return Player{}, nil, false
	}
	s.tokens--

	p, found := h.players[s.CharID]
	if !found || p.Flags&FlagMuted != 0 {
		return Player{}, nil, false
	}
	switch channel {
	case ChannelParty:
		if p.PartyID == 0 {
			return Player{}, nil, false
		}
	case ChannelGuild:
		if p.GuildID == 0 {
			return Player{}, nil, false
		}
	case ChannelRoom:
		if p.ChatID == 0 {
			return Player{}, nil, false
		}
	case ChannelWhisper:
		// a whisper goes to one session, so there is nothing to loop over
		tid, found := h.byName[target]
		if !found || tid == p.CharID {
			return *p, nil, true
		}
		tp := h.players[tid]
		ls, found := h.byChar[tid]
		if tp == nil || !found || ls.Quiet || tp.Flags&FlagNoWhisper != 0 {
			return *p, nil, true
		}
		d := Delivery{Addr: ls.Addr}
		if !ls.known[p.CharID] {
			ls.known[p.CharID] = true
			d.SendName = true
		}
		return *p, []Delivery{d}, true
	case ChannelNormal:
	default:
		return Player{}, nil, false
	}

	for _, ls := range h.sessions {
		if ls.CharID == p.CharID || ls.Quiet {
			continue
		}
		lp, found := h.players[ls.CharID]
		if !found {
			continue
		}
		if !hears(p, lp, channel, h.normalDist) {
			continue
		}
		d := Delivery{Addr: ls.Addr}
		if !ls.known[p.CharID] {
			ls.known[p.CharID] = true
			d.SendName = true
		}
		out = append(out, d)
	}
	return *p, out, true
}

func hears(speaker, listener *Player, channel uint8, dist int) bool {
	switch channel {
	case ChannelParty:
		return listener.PartyID == speaker.PartyID
	case ChannelGuild:
		return listener.GuildID == speaker.GuildID
	case ChannelRoom:
		return listener.ChatID == speaker.ChatID
	default:
		if listener.Map != speaker.Map {
			return false
		}
		dx := int(listener.X) - int(speaker.X)
		dy := int(listener.Y) - int(speaker.Y)
		return abs(dx) <= dist && abs(dy) <= dist
	}
}

func abs(v int) int {
	if v < 0 {
		return -v
	}
	return v
}

// Expire removes sessions that sent nothing for the given duration.
func (h *Hub) Expire(now time.Time, idle time.Duration) {
	h.mu.Lock()
	defer h.mu.Unlock()
	for id, s := range h.sessions {
		if now.Sub(s.LastSeen) > idle {
			delete(h.sessions, id)
			delete(h.byChar, s.CharID)
		}
	}
	for code, pd := range h.pending {
		if now.Sub(pd.created) > pendingTTL {
			h.dropPendingLocked(code)
		}
	}
}

// Stats returns the number of known players and connected sessions.
func (h *Hub) Stats() (players, sessions int) {
	h.mu.Lock()
	defer h.mu.Unlock()
	return len(h.players), len(h.sessions)
}
