package main

import (
	"bufio"
	"encoding/binary"
	"net"
	"net/netip"
	"strings"
	"testing"
	"time"
)

const testSecret = "test_secret_123"

type fakeMap struct {
	t    *testing.T
	conn net.Conn
	r    *bufio.Reader
}

func startServer(t *testing.T) (linkAddr string, udpAddr *net.UDPAddr, hub *Hub) {
	t.Helper()
	hub = NewHub(14, 100)
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	uc, err := net.ListenUDP("udp", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { ln.Close(); uc.Close() })
	udp := NewUDPServer(hub, uc)
	link := NewLinkServer(hub, testSecret)
	link.OnClaim = udp.SendWelcome
	udp.OnHello = link.AutoQuery
	udp.StatsQuery = link.StatsQuery
	link.OnStats = udp.SendStats
	udp.CellsQuery = link.CellsQuery
	link.OnCells = udp.SendCells
	udp.AurasQuery = link.AurasQuery
	link.OnAuras = udp.SendAuras
	go link.Serve(ln)
	go udp.Serve()
	return ln.Addr().String(), uc.LocalAddr().(*net.UDPAddr), hub
}

func connectMap(t *testing.T, addr, secret string) (*fakeMap, byte) {
	t.Helper()
	c, err := net.Dial("tcp", addr)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { c.Close() })
	s := make([]byte, secretLength)
	copy(s, secret)
	writePacket(c, linkHello, s)
	r := bufio.NewReader(c)
	c.SetReadDeadline(time.Now().Add(2 * time.Second))
	cmd, body, err := readPacket(r)
	if err != nil || cmd != linkHelloAck {
		t.Fatalf("no hello ack: %v", err)
	}
	return &fakeMap{t, c, r}, body[0]
}

func (m *fakeMap) player(charID, party, guild uint32, mapName string, x, y uint16, flags uint8, name string) {
	b := make([]byte, playerPacketLength-4)
	binary.LittleEndian.PutUint32(b[0:], charID+1000)
	binary.LittleEndian.PutUint32(b[4:], charID)
	binary.LittleEndian.PutUint32(b[8:], party)
	binary.LittleEndian.PutUint32(b[12:], guild)
	binary.LittleEndian.PutUint16(b[16:], x)
	binary.LittleEndian.PutUint16(b[18:], y)
	b[20] = flags
	copy(b[21:], mapName)
	copy(b[21+mapLength:], name)
	writePacket(m.conn, linkPlayer, b)
}

func (m *fakeMap) claim(code []byte, charID uint32) {
	b := make([]byte, claimPacketLength-4)
	copy(b, code)
	binary.LittleEndian.PutUint32(b[16:], charID)
	writePacket(m.conn, linkClaim, b)
}

func (m *fakeMap) leave(charID uint32) {
	b := make([]byte, 4)
	binary.LittleEndian.PutUint32(b, charID)
	writePacket(m.conn, linkLeave, b)
}

func waitPlayers(t *testing.T, hub *Hub, n int) {
	t.Helper()
	for i := 0; i < 100; i++ {
		if p, _ := hub.Stats(); p == n {
			return
		}
		time.Sleep(10 * time.Millisecond)
	}
	p, _ := hub.Stats()
	t.Fatalf("expected %d players, have %d", n, p)
}

type client struct {
	t       *testing.T
	conn    *net.UDPConn
	session uint32
}

func dialClient(t *testing.T, srv *net.UDPAddr) *client {
	t.Helper()
	c, err := net.DialUDP("udp", nil, srv)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { c.Close() })
	return &client{t: t, conn: c}
}

func (c *client) read(timeout time.Duration) []byte {
	buf := make([]byte, 2048)
	c.conn.SetReadDeadline(time.Now().Add(timeout))
	n, err := c.conn.Read(buf)
	if err != nil {
		return nil
	}
	return buf[:n]
}

// join does the full sign-in: HELLO, pass the code through the "game", WELCOME.
func (c *client) join(m *fakeMap, charID uint32) []byte {
	c.conn.Write([]byte{cHello})
	ch := c.read(time.Second)
	if ch == nil || ch[0] != sChallenge || len(ch) != 23 || string(ch[17:]) != ShortCode([16]byte(ch[1:17])) {
		c.t.Fatalf("no challenge: %x", ch)
	}
	m.claim(ch[1:], charID)
	resp := c.read(time.Second)
	if resp != nil && resp[0] == sWelcome {
		c.session = binary.LittleEndian.Uint32(resp[1:])
	}
	return resp
}

func (c *client) talk(channel byte, payload string) {
	b := make([]byte, 8+len(payload))
	b[0] = cAudio
	binary.LittleEndian.PutUint32(b[1:], c.session)
	binary.LittleEndian.PutUint16(b[5:], 7)
	b[7] = channel
	copy(b[8:], payload)
	c.conn.Write(b)
}

// expectAudio reads the next audio packet (skipping speaker name packets).
func (c *client) expectAudio(from uint32, payload string) {
	c.t.Helper()
	for i := 0; i < 3; i++ {
		p := c.read(time.Second)
		if p == nil {
			c.t.Fatalf("expected audio from %d, got nothing", from)
		}
		if p[0] == sSpeaker {
			continue
		}
		if p[0] != sAudio || binary.LittleEndian.Uint32(p[1:]) != from || string(p[8:]) != payload {
			c.t.Fatalf("unexpected packet %x", p)
		}
		return
	}
}

func (c *client) expectNothing() {
	c.t.Helper()
	if p := c.read(200 * time.Millisecond); p != nil {
		c.t.Fatalf("expected nothing, got %x", p)
	}
}

func TestWrongSecret(t *testing.T) {
	link, _, _ := startServer(t)
	if _, res := connectMap(t, link, "nope_nope_nope"); res == 0 {
		t.Fatal("wrong secret accepted")
	}
}

func TestRouting(t *testing.T) {
	link, udp, hub := startServer(t)
	m, res := connectMap(t, link, testSecret)
	if res != 0 {
		t.Fatal("secret refused")
	}
	m.player(1, 10, 100, "prontera", 150, 150, 0, "Alice")
	m.player(2, 10, 0, "prontera", 155, 150, 0, "Bob")        // near, same party
	m.player(3, 0, 100, "geffen", 50, 50, 0, "Carol")         // other map, same guild
	m.player(4, 0, 0, "prontera", 100, 100, FlagMuted, "Dan") // far away, muted
	waitPlayers(t, hub, 4)

	a, b, c, d := dialClient(t, udp), dialClient(t, udp), dialClient(t, udp), dialClient(t, udp)
	for i, cl := range []*client{a, b, c, d} {
		if p := cl.join(m, uint32(i+1)); p == nil || p[0] != sWelcome || binary.LittleEndian.Uint32(p[5:]) != uint32(i+1) {
			t.Fatalf("client %d join failed: %x", i+1, p)
		}
	}

	// a made-up code is ignored, so nobody gets welcomed
	x := dialClient(t, udp)
	x.conn.Write([]byte{cHello})
	if ch := x.read(time.Second); ch == nil || ch[0] != sChallenge {
		t.Fatalf("no challenge: %x", ch)
	}
	m.claim(make([]byte, 16), 1)
	x.expectNothing()
	a.expectNothing()

	// a claim for a character the map-server never sent is ignored
	y := dialClient(t, udp)
	y.conn.Write([]byte{cHello})
	ch := y.read(time.Second)
	m.claim(ch[1:], 99)
	y.expectNothing()

	// Normal: Bob is 5 cells away, Carol on another map, Dan 50 cells away
	a.talk(ChannelNormal, "hi")
	p := b.read(time.Second)
	if p == nil || p[0] != sSpeaker || binary.LittleEndian.Uint32(p[1:]) != 1 || cstring(p[5:]) != "Alice" {
		t.Fatalf("expected speaker name first, got %x", p)
	}
	b.expectAudio(1, "hi")
	c.expectNothing()
	d.expectNothing()
	a.expectNothing()

	// Party: Bob only
	a.talk(ChannelParty, "party")
	b.expectAudio(1, "party")
	c.expectNothing()

	// Guild: Carol only, even on another map
	a.talk(ChannelGuild, "guild")
	c.expectAudio(1, "guild")
	b.expectNothing()

	// Bob has no guild, can't use guild channel
	b.talk(ChannelGuild, "x")
	a.expectNothing()
	c.expectNothing()

	// Dan is muted; move him next to Alice first
	m.player(4, 0, 0, "prontera", 151, 151, FlagMuted, "Dan")
	time.Sleep(50 * time.Millisecond)
	d.talk(ChannelNormal, "spam")
	a.expectNothing()

	// session id from another address is ignored
	spoof := dialClient(t, udp)
	spoof.session = a.session
	spoof.talk(ChannelNormal, "fake")
	b.expectNothing()

	// after logout Alice's session is gone
	m.leave(1)
	waitPlayers(t, hub, 3)
	a.talk(ChannelNormal, "ghost")
	b.expectNothing()
	ping := make([]byte, 5)
	ping[0] = cPing
	binary.LittleEndian.PutUint32(ping[1:], a.session)
	a.conn.Write(ping)
	if p := a.read(time.Second); p == nil || p[0] != sDenied {
		t.Fatalf("expected denied after logout, got %x", p)
	}

	// relogging and claiming again works
	m.player(1, 10, 100, "prontera", 150, 150, 0, "Alice")
	if p := a.join(m, 1); p == nil || p[0] != sWelcome {
		t.Fatalf("rejoin failed: %x", p)
	}
	a.talk(ChannelParty, "back")
	b.expectAudio(1, "back")
}

func TestSessionSurvivesLinkReconnect(t *testing.T) {
	link, udp, hub := startServer(t)
	m, _ := connectMap(t, link, testSecret)
	m.player(1, 5, 0, "prontera", 1, 1, 0, "A")
	m.player(2, 5, 0, "prontera", 1, 1, 0, "B")
	waitPlayers(t, hub, 2)
	a, b := dialClient(t, udp), dialClient(t, udp)
	a.join(m, 1)
	b.join(m, 2)

	m2, _ := connectMap(t, link, testSecret)
	waitPlayers(t, hub, 0)
	a.talk(ChannelParty, "lost")
	b.expectNothing()

	m2.player(1, 5, 0, "prontera", 1, 1, 0, "A")
	m2.player(2, 5, 0, "prontera", 1, 1, 0, "B")
	waitPlayers(t, hub, 2)
	a.talk(ChannelParty, "again")
	b.expectAudio(1, "again")
}

func TestReconnectResetsPlayers(t *testing.T) {
	link, _, hub := startServer(t)
	m, _ := connectMap(t, link, testSecret)
	m.player(1, 0, 0, "prontera", 1, 1, 0, "A")
	waitPlayers(t, hub, 1)
	connectMap(t, link, testSecret)
	waitPlayers(t, hub, 0)
}

func TestFlood(t *testing.T) {
	hub := NewHub(14, 10)
	hub.UpsertPlayer(Player{CharID: 1, Map: "m"})
	hub.UpsertPlayer(Player{CharID: 2, Map: "m"})
	now := time.Now()
	addr := mustAddr("127.0.0.1:1000")
	code, _, _ := hub.Hello(addr, randomCode, now)
	s, _ := hub.Claim(code, 1, randomID, now)
	code, _, _ = hub.Hello(mustAddr("127.0.0.1:1001"), randomCode, now)
	hub.Claim(code, 2, randomID, now)
	sent := 0
	for i := 0; i < 50; i++ {
		if _, _, ok := hub.Route(s.ID, addr, ChannelNormal, "", now); ok {
			sent++
		}
	}
	if sent != 10 {
		t.Fatalf("rate limit let %d packets through, want 10", sent)
	}
}

func mustAddr(s string) netip.AddrPort { return netip.MustParseAddrPort(s) }

// playerInRoom sends the longer PLAYER packet that carries the chat room id.
func (m *fakeMap) playerInRoom(charID uint32, room uint32, flags uint8, name string) {
	b := make([]byte, chatIDOffset+4)
	binary.LittleEndian.PutUint32(b[0:], charID+1000)
	binary.LittleEndian.PutUint32(b[4:], charID)
	b[20] = flags
	copy(b[21:], "prontera")
	copy(b[21+mapLength:], name)
	binary.LittleEndian.PutUint32(b[chatIDOffset:], room)
	writePacket(m.conn, linkPlayer, b)
}

func (c *client) whisper(target, payload string) {
	b := make([]byte, 8+nameLength+len(payload))
	b[0] = cAudio
	binary.LittleEndian.PutUint32(b[1:], c.session)
	b[7] = ChannelWhisper
	copy(b[8:], target)
	copy(b[8+nameLength:], payload)
	c.conn.Write(b)
}

func TestRoomAndWhisper(t *testing.T) {
	link, udp, hub := startServer(t)
	m, _ := connectMap(t, link, testSecret)
	m.playerInRoom(1, 7, 0, "Alice")
	m.playerInRoom(2, 7, 0, "Bob")
	m.playerInRoom(3, 8, 0, "Carol")
	m.playerInRoom(4, 0, FlagNoWhisper, "Dan")
	waitPlayers(t, hub, 4)
	a, b, c, d := dialClient(t, udp), dialClient(t, udp), dialClient(t, udp), dialClient(t, udp)
	for i, cl := range []*client{a, b, c, d} {
		if p := cl.join(m, uint32(i+1)); p == nil || p[0] != sWelcome {
			t.Fatalf("client %d join failed: %x", i+1, p)
		}
	}

	// Room: only the same chat room
	a.talk(ChannelRoom, "room")
	b.expectAudio(1, "room")
	c.expectNothing()
	d.expectNothing()

	// Dan is in no room, so can't talk there (and doesn't reach others with room 0)
	d.talk(ChannelRoom, "x")
	a.expectNothing()

	// Whisper: only Carol hears it, without the name in the audio
	a.whisper("Carol", "psst")
	c.expectAudio(1, "psst")
	b.expectNothing()
	d.expectNothing()

	// Dan blocks all whispers; unknown names and yourself go nowhere
	a.whisper("Dan", "psst")
	d.expectNothing()
	a.whisper("Nobody", "psst")
	a.whisper("Alice", "psst")
	a.expectNothing()
	b.expectNothing()

	// renamed or logged out targets are forgotten
	m.leave(3)
	waitPlayers(t, hub, 3)
	a.whisper("Carol", "gone")
	c.expectNothing()
}

func (m *fakeMap) claimShort(short string, charID uint32) {
	b := make([]byte, 12)
	copy(b, short)
	binary.LittleEndian.PutUint32(b[8:], charID)
	writePacket(m.conn, linkClaimShort, b)
}

// next reads the next packet of the given kind, skipping auto queries the
// test doesn't care about. ok is false when nothing came.
func (m *fakeMap) next(want uint16, timeout time.Duration) ([]byte, bool) {
	for {
		m.conn.SetReadDeadline(time.Now().Add(timeout))
		cmd, body, err := readPacket(m.r)
		if err != nil {
			return nil, false
		}
		if cmd == want {
			return body, true
		}
		if cmd != linkAutoQuery {
			m.t.Fatalf("unexpected link packet 0x%04x", cmd)
		}
	}
}

func (m *fakeMap) claimResult(t *testing.T) (uint32, byte) {
	t.Helper()
	body, ok := m.next(linkClaimResult, time.Second)
	if !ok || len(body) < 5 {
		t.Fatalf("no claim result")
	}
	return binary.LittleEndian.Uint32(body), body[4]
}

func (m *fakeMap) claimAuto(short string, charID uint32) {
	b := make([]byte, 12)
	copy(b, short)
	binary.LittleEndian.PutUint32(b[8:], charID)
	writePacket(m.conn, linkClaimAuto, b)
}

// autoQuery waits for the voice server to ask about an address.
func (m *fakeMap) autoQuery(t *testing.T) (ip [4]byte, short string, ok bool) {
	t.Helper()
	m.conn.SetReadDeadline(time.Now().Add(300 * time.Millisecond))
	cmd, body, err := readPacket(m.r)
	if err != nil {
		return ip, "", false
	}
	if cmd != linkAutoQuery || len(body) < 12 {
		t.Fatalf("unexpected link packet 0x%04x", cmd)
	}
	return [4]byte(body[0:4]), cstring(body[4:12]), true
}

func TestAutoLink(t *testing.T) {
	link, udp, hub := startServer(t)
	m, _ := connectMap(t, link, testSecret)
	m.player(1, 5, 0, "prontera", 1, 1, 0, "Alice")
	m.player(2, 5, 0, "prontera", 1, 1, 0, "Bob")
	waitPlayers(t, hub, 2)

	// a new client makes the voice server ask about its address
	a := dialClient(t, udp)
	a.conn.Write([]byte{cHello})
	ch := a.read(time.Second)
	if ch == nil || len(ch) != 23 {
		t.Fatalf("no challenge: %x", ch)
	}
	ip, short, ok := m.autoQuery(t)
	if !ok || ip != [4]byte{127, 0, 0, 1} || short != string(ch[17:]) {
		t.Fatalf("auto query: %v %v %q", ok, ip, short)
	}

	// a failed auto claim tells the player nothing
	m.claimAuto("ZZZZZZ", 1)
	if _, ok := m.next(linkClaimResult, 200*time.Millisecond); ok {
		t.Fatal("failed auto claim was reported")
	}

	// hellos from the same address right after are not asked about again
	b := dialClient(t, udp)
	b.conn.Write([]byte{cHello})
	chB := b.read(time.Second)
	if _, _, ok := m.autoQuery(t); ok {
		t.Fatal("auto query was not limited")
	}

	// a player logging in from the address is matched to the newest client
	on := []byte{127, 0, 0, 1}
	writePacket(m.conn, linkOnline, on)
	if _, short, ok := m.autoQuery(t); !ok || short != string(chB[17:]) {
		t.Fatalf("login did not query the newest client: %v %q", ok, short)
	}

	// the map-server answers for the one player there
	m.claimAuto(string(chB[17:]), 2)
	if id, res := m.claimResult(t); id != 2 || res != 0 {
		t.Fatalf("auto claim: result %d for %d", res, id)
	}
	w := b.read(time.Second)
	if w == nil || w[0] != sWelcome || binary.LittleEndian.Uint32(w[5:]) != 2 {
		t.Fatalf("no welcome: %x", w)
	}

	// a linked player is not moved to another client by address
	c := dialClient(t, udp)
	time.Sleep(autoQueryInterval)
	c.conn.Write([]byte{cHello})
	chC := c.read(time.Second)
	if _, _, ok := m.autoQuery(t); !ok {
		t.Fatal("no auto query")
	}
	m.claimAuto(string(chC[17:]), 2)
	if _, ok := m.next(linkClaimResult, 200*time.Millisecond); ok {
		t.Fatal("auto claim took over a linked player")
	}
	c.expectNothing()
	if !hub.HasSession(2) {
		t.Fatal("session lost")
	}
}

func TestShortCodeClaim(t *testing.T) {
	link, udp, hub := startServer(t)
	m, _ := connectMap(t, link, testSecret)
	m.player(1, 5, 0, "prontera", 1, 1, 0, "Alice")
	m.player(2, 5, 0, "prontera", 1, 1, 0, "Bob")
	waitPlayers(t, hub, 2)

	a := dialClient(t, udp)
	a.conn.Write([]byte{cHello})
	ch := a.read(time.Second)
	if ch == nil || len(ch) != 23 {
		t.Fatalf("no challenge: %x", ch)
	}
	short := string(ch[17:])
	for _, c := range short {
		if !strings.ContainsRune(shortAlphabet, c) {
			t.Fatalf("bad short code %q", short)
		}
	}

	// a wrong code is refused and reported back
	m.claimShort("ZZZZZZ", 1)
	if id, res := m.claimResult(t); id != 1 || res != 1 {
		t.Fatalf("wrong code: result %d for %d", res, id)
	}
	a.expectNothing()

	// the right one links Alice and welcomes the client
	m.claimShort(short, 1)
	if id, res := m.claimResult(t); id != 1 || res != 0 {
		t.Fatalf("right code: result %d for %d", res, id)
	}
	w := a.read(time.Second)
	if w == nil || w[0] != sWelcome || binary.LittleEndian.Uint32(w[5:]) != 1 {
		t.Fatalf("no welcome: %x", w)
	}
	a.session = binary.LittleEndian.Uint32(w[1:])

	// codes work once
	m.claimShort(short, 2)
	if _, res := m.claimResult(t); res != 1 {
		t.Fatal("a used code worked again")
	}

	b := dialClient(t, udp)
	b.join(m, 2)
	a.talk(ChannelParty, "hi")
	b.expectAudio(1, "hi")
}

// connectMapWith is connectMap for a map-server that lists features in HELLO.
func connectMapWith(t *testing.T, addr string, features byte) *fakeMap {
	t.Helper()
	c, err := net.Dial("tcp", addr)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { c.Close() })
	s := make([]byte, secretLength+1)
	copy(s, testSecret)
	s[secretLength] = features
	writePacket(c, linkHello, s)
	r := bufio.NewReader(c)
	c.SetReadDeadline(time.Now().Add(2 * time.Second))
	cmd, body, err := readPacket(r)
	if err != nil || cmd != linkHelloAck || body[0] != 0 {
		t.Fatalf("no hello ack: %v", err)
	}
	return &fakeMap{t, c, r}
}

func (c *client) askStats(page byte) {
	b := make([]byte, 6)
	b[0] = cStats
	binary.LittleEndian.PutUint32(b[1:], c.session)
	b[5] = page
	c.conn.Write(b)
}

func TestStats(t *testing.T) {
	linkAddr, udpAddr, hub := startServer(t)
	m := connectMapWith(t, linkAddr, featureStats)
	m.player(1, 0, 0, "prontera", 100, 100, 0, "Alice")
	m.player(2, 0, 0, "prontera", 100, 100, 0, "Bob")
	waitPlayers(t, hub, 2)
	a := dialClient(t, udpAddr)
	b := dialClient(t, udpAddr)
	a.join(m, 1)
	b.join(m, 2)

	a.askStats(3)
	body, ok := m.next(linkStatsQuery, time.Second)
	if !ok || binary.LittleEndian.Uint32(body) != 1 || body[4] != 3 {
		t.Fatalf("stats query: %x", body)
	}
	// asking again right away is dropped
	a.askStats(3)
	if _, ok := m.next(linkStatsQuery, 100*time.Millisecond); ok {
		t.Fatal("rate limit let a second query through")
	}

	// a client can't ask with someone else's session id
	stolen := a.session
	a.session, b.session = b.session, stolen
	time.Sleep(statsInterval)
	a.askStats(0)
	if _, ok := m.next(linkStatsQuery, 100*time.Millisecond); ok {
		t.Fatal("query with another client's session went through")
	}
	a.session, b.session = b.session, a.session

	text := strings.Repeat("0123456789", 250) // 2500 bytes -> 3 packets
	reply := make([]byte, 5+len(text))
	binary.LittleEndian.PutUint32(reply, 1)
	reply[4] = 3
	copy(reply[5:], text)
	writePacket(m.conn, linkStats, reply)

	got := make([]string, 3)
	for i := 0; i < 3; i++ {
		p := a.read(time.Second)
		if p == nil || p[0] != sStats || p[1] != 3 || p[4] != 3 {
			t.Fatalf("stats part: %x", p)
		}
		got[p[3]] = string(p[5:])
	}
	if strings.Join(got, "") != text {
		t.Fatal("stats text mangled")
	}
	b.expectNothing()
}

func TestStatsOldMapServer(t *testing.T) {
	linkAddr, udpAddr, hub := startServer(t)
	m, _ := connectMap(t, linkAddr, testSecret) // no feature byte
	m.player(1, 0, 0, "prontera", 100, 100, 0, "Alice")
	waitPlayers(t, hub, 1)
	a := dialClient(t, udpAddr)
	a.join(m, 1)
	a.askStats(0)
	if p := a.read(time.Second); p == nil || p[0] != sNoStats || p[1] != 0 {
		t.Fatalf("want no-stats answer, got %x", p)
	}
	if _, ok := m.next(linkStatsQuery, 100*time.Millisecond); ok {
		t.Fatal("old map-server got a packet it doesn't know")
	}
}

func (c *client) askCells() {
	b := make([]byte, 5)
	b[0] = cCells
	binary.LittleEndian.PutUint32(b[1:], c.session)
	c.conn.Write(b)
}

func TestCells(t *testing.T) {
	linkAddr, udpAddr, hub := startServer(t)
	m := connectMapWith(t, linkAddr, featureStats|featureCells)
	m.player(1, 0, 0, "prontera", 100, 100, 0, "Alice")
	m.player(2, 0, 0, "prontera", 100, 100, 0, "Bob")
	waitPlayers(t, hub, 2)
	a := dialClient(t, udpAddr)
	b := dialClient(t, udpAddr)
	a.join(m, 1)
	b.join(m, 2)

	a.askCells()
	body, ok := m.next(linkCellsQuery, time.Second)
	if !ok || binary.LittleEndian.Uint32(body) != 1 {
		t.Fatalf("cells query: %x", body)
	}
	a.askCells()
	if _, ok := m.next(linkCellsQuery, 100*time.Millisecond); ok {
		t.Fatal("rate limit let a second query through")
	}

	// two cells: (100,101) Storm Gust (89) hostile, (102,103) Pneuma (25) friendly
	reply := []byte{1, 0, 0, 0, 2, 0, 100, 0, 101, 0, 89, 0, 1, 102, 0, 103, 0, 25, 0, 0}
	writePacket(m.conn, linkCells, reply)
	p := a.read(time.Second)
	if p == nil || p[0] != sCells || string(p[1:]) != string(reply[4:]) {
		t.Fatalf("cells answer: %x", p)
	}
	b.expectNothing()
}

func TestCellsOldMapServer(t *testing.T) {
	linkAddr, udpAddr, hub := startServer(t)
	m := connectMapWith(t, linkAddr, featureStats) // knows stats, not cells
	m.player(1, 0, 0, "prontera", 100, 100, 0, "Alice")
	waitPlayers(t, hub, 1)
	a := dialClient(t, udpAddr)
	a.join(m, 1)
	a.askCells()
	if p := a.read(time.Second); p == nil || p[0] != sNoCells {
		t.Fatalf("want no-cells answer, got %x", p)
	}
	if _, ok := m.next(linkCellsQuery, 100*time.Millisecond); ok {
		t.Fatal("old map-server got a packet it doesn't know")
	}
}

func (c *client) askAuras() {
	b := make([]byte, 5)
	b[0] = cAuras
	binary.LittleEndian.PutUint32(b[1:], c.session)
	c.conn.Write(b)
}

func TestAuras(t *testing.T) {
	linkAddr, udpAddr, hub := startServer(t)
	m := connectMapWith(t, linkAddr, featureStats|featureCells|featureAuras)
	m.player(1, 0, 0, "prontera", 100, 100, 0, "Alice")
	m.player(2, 0, 0, "prontera", 100, 100, 0, "Bob")
	waitPlayers(t, hub, 2)
	a := dialClient(t, udpAddr)
	b := dialClient(t, udpAddr)
	a.join(m, 1)
	b.join(m, 2)

	a.askAuras()
	body, ok := m.next(linkAurasQuery, time.Second)
	if !ok || binary.LittleEndian.Uint32(body) != 1 {
		t.Fatalf("auras query: %x", body)
	}
	a.askAuras()
	if _, ok := m.next(linkAurasQuery, 100*time.Millisecond); ok {
		t.Fatal("rate limit let a second query through")
	}

	// one aura: account 2000001 wears ground circle 12, ring 3, no sparkle
	reply := []byte{1, 0, 0, 0, 1, 0, 0x81, 0x84, 0x1e, 0, 12, 3, 0}
	writePacket(m.conn, linkAuras, reply)
	p := a.read(time.Second)
	if p == nil || p[0] != sAuras || string(p[1:]) != string(reply[4:]) {
		t.Fatalf("auras answer: %x", p)
	}
	b.expectNothing()
}

func TestAurasOldMapServer(t *testing.T) {
	linkAddr, udpAddr, hub := startServer(t)
	m := connectMapWith(t, linkAddr, featureStats|featureCells) // no auras
	m.player(1, 0, 0, "prontera", 100, 100, 0, "Alice")
	waitPlayers(t, hub, 1)
	a := dialClient(t, udpAddr)
	a.join(m, 1)
	a.askAuras()
	if p := a.read(time.Second); p == nil || p[0] != sNoAuras {
		t.Fatalf("want no-auras answer, got %x", p)
	}
	if _, ok := m.next(linkAurasQuery, 100*time.Millisecond); ok {
		t.Fatal("old map-server got a packet it doesn't know")
	}
}

func (c *client) mode(flags byte) {
	b := make([]byte, 6)
	b[0] = cMode
	binary.LittleEndian.PutUint32(b[1:], c.session)
	b[5] = flags
	c.conn.Write(b)
}

func TestQuietSession(t *testing.T) {
	linkAddr, udpAddr, hub := startServer(t)
	m := connectMapWith(t, linkAddr, featureStats)
	m.player(1, 0, 0, "prontera", 100, 100, 0, "Alice")
	m.player(2, 0, 0, "prontera", 101, 100, 0, "Bob")
	waitPlayers(t, hub, 2)
	a := dialClient(t, udpAddr)
	b := dialClient(t, udpAddr)
	a.join(m, 1)
	b.join(m, 2)

	// Bob turns voice off: he neither hears Alice nor can talk
	b.mode(modeQuiet)
	time.Sleep(50 * time.Millisecond)
	a.talk(ChannelNormal, "hello")
	b.expectNothing()
	b.talk(ChannelNormal, "hi")
	a.expectNothing()

	// but his statistics still work
	b.askStats(0)
	if body, ok := m.next(linkStatsQuery, time.Second); !ok || binary.LittleEndian.Uint32(body) != 2 {
		t.Fatalf("quiet session lost statistics: %x", body)
	}

	// voice back on
	b.mode(0)
	time.Sleep(50 * time.Millisecond)
	a.talk(ChannelNormal, "again")
	b.expectAudio(1, "again")
}
