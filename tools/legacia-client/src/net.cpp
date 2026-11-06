// UDP link to the voice server: sign in with a claim code, keep alive,
// send our audio and hand received audio to the mixer.
#include "voice.h"

#include <stdio.h>
#include <string.h>

enum : uint8_t {
	C_HELLO = 0x01,
	C_AUDIO = 0x02,
	C_PING = 0x03,
	C_BYE = 0x04,
	C_STATS = 0x05,
	C_MODE = 0x06,
	C_CELLS = 0x07,
	C_AURAS = 0x08,
	S_WELCOME = 0x81,
	S_DENIED = 0x82,
	S_PONG = 0x83,
	S_AUDIO = 0x84,
	S_SPEAKER = 0x85,
	S_CHALLENGE = 0x86,
	S_STATS = 0x87,
	S_NO_STATS = 0x88,
	S_CELLS = 0x89,
	S_NO_CELLS = 0x8a,
	S_AURAS = 0x8b,
	S_NO_AURAS = 0x8c,
};

static CRITICAL_SECTION g_netLock;
static SOCKET g_sock = INVALID_SOCKET;
static volatile LONG g_restart;
static NetState g_state = NET_OFF;
static uint32_t g_session;
static uint32_t g_charId;
static char g_shortCode[8]; // shown to the player for @voice while signing in
static char g_statusText[64];

NetState net_state() {
	Lock l(g_netLock);
	return g_state;
}

uint32_t net_char_id() {
	Lock l(g_netLock);
	return g_charId;
}

std::string net_short_code() {
	Lock l(g_netLock);
	return g_state == NET_CLAIMING ? g_shortCode : "";
}

const char* net_status_text() {
	if (!g_cfg.enabled && g_cfg.host[0])
		return net_state() == NET_CONNECTED ? "voice off (still signed in for Player Statistics)" : "voice off";
	switch (net_state()) {
	case NET_OFF: return g_cfg.host[0] ? "voice off" : "no voice server set";
	case NET_WAIT_GAME: return "waiting for the game";
	case NET_HELLO: return "connecting...";
	case NET_CLAIMING: {
		std::string code = net_short_code();
		if (code.empty())
			return "signing in...";
		snprintf(g_statusText, sizeof(g_statusText), "click the mic, or type @voice %s", code.c_str());
		return g_statusText;
	}
	case NET_CONNECTED: return "connected";
	}
	return "";
}

static void set_state(NetState s) {
	Lock l(g_netLock);
	if (g_state != s)
		log_line("voice: %s", s == NET_CONNECTED ? "connected" : s == NET_HELLO ? "connecting" : s == NET_CLAIMING ? "claim queued" : s == NET_WAIT_GAME ? "waiting for game" : "off");
	g_state = s;
	if (s != NET_CONNECTED)
		g_session = 0;
}

static void send_raw(const void* data, int len) {
	if (g_sock != INVALID_SOCKET)
		send(g_sock, (const char*)data, len, 0);
}

static void send_with_session(uint8_t type) {
	uint8_t b[5];
	b[0] = type;
	{
		Lock l(g_netLock);
		memcpy(b + 1, &g_session, 4);
	}
	send_raw(b, sizeof(b));
}

void net_send_audio(uint16_t seq, uint8_t channel, const uint8_t* data, int len) {
	uint8_t b[8 + NAME_LENGTH + 1000];
	if (len > 1000)
		return;
	int head = 8;
	if (channel == CHANNEL_WHISPER) {
		// whispers name their target before the audio
		memset(b + 8, 0, NAME_LENGTH);
		strncpy((char*)b + 8, g_cfg.whisper, NAME_LENGTH - 1);
		head += NAME_LENGTH;
	}
	{
		Lock l(g_netLock);
		if (g_state != NET_CONNECTED)
			return;
		memcpy(b + 1, &g_session, 4);
	}
	b[0] = C_AUDIO;
	memcpy(b + 5, &seq, 2);
	b[7] = channel;
	memcpy(b + head, data, len);
	send_raw(b, head + len);
}

// Tells the voice server whether voice chat is on. With it off the client
// stays signed in for the Player Statistics window only: the server sends it
// no audio and takes none from it.
static void send_mode() {
	uint8_t b[6];
	{
		Lock l(g_netLock);
		if (g_state != NET_CONNECTED)
			return;
		memcpy(b + 1, &g_session, 4);
	}
	b[0] = C_MODE;
	b[5] = g_cfg.enabled ? 0 : 0x01;
	send_raw(b, sizeof(b));
}

bool net_request_stats(uint8_t page) {
	uint8_t b[6];
	{
		Lock l(g_netLock);
		if (g_state != NET_CONNECTED)
			return false;
		memcpy(b + 1, &g_session, 4);
	}
	b[0] = C_STATS;
	b[5] = page;
	send_raw(b, sizeof(b));
	return true;
}

// a statistics page arrives in up to 16 pieces; only the net thread uses this
struct StatsAssembly {
	int page = -1, seq = -1, parts = 0, got = 0;
	std::string piece[16];
	bool have[16] = {};
};
static StatsAssembly g_stats;

static void on_stats_part(const uint8_t* p, int n) {
	int page = p[1], seq = p[2], part = p[3], parts = p[4];
	if (parts < 1 || parts > 16 || part >= parts)
		return;
	StatsAssembly& a = g_stats;
	if (a.page != page || a.seq != seq || a.parts != parts) {
		a = StatsAssembly();
		a.page = page;
		a.seq = seq;
		a.parts = parts;
	}
	if (a.have[part])
		return;
	a.have[part] = true;
	a.piece[part].assign((const char*)p + 5, n - 5);
	if (++a.got < a.parts)
		return;
	std::string text;
	for (int i = 0; i < a.parts; i++)
		text += a.piece[i];
	a = StatsAssembly();
	stats_ui_on_page((uint8_t)page, text);
}

static bool open_socket() {
	if (g_sock != INVALID_SOCKET) {
		closesocket(g_sock);
		g_sock = INVALID_SOCKET;
	}
	if (!g_cfg.host[0])
		return false;

	char port[16];
	snprintf(port, sizeof(port), "%d", g_cfg.port);
	addrinfo hints = {}, *res = nullptr;
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_DGRAM;
	if (getaddrinfo(g_cfg.host, port, &hints, &res) != 0 || !res) {
		log_line("voice: cannot resolve %s", g_cfg.host);
		return false;
	}
	g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (g_sock != INVALID_SOCKET && connect(g_sock, res->ai_addr, (int)res->ai_addrlen) != 0) {
		closesocket(g_sock);
		g_sock = INVALID_SOCKET;
	}
	freeaddrinfo(res);
	return g_sock != INVALID_SOCKET;
}

static void handle_packet(const uint8_t* p, int n, DWORD now, DWORD& lastHeard) {
	switch (p[0]) {
	case S_CHALLENGE:
		if (n >= 17 && net_state() == NET_HELLO) {
			{
				Lock l(g_netLock);
				memset(g_shortCode, 0, sizeof(g_shortCode));
				if (n >= 23)
					memcpy(g_shortCode, p + 17, 6);
			}
			if (n >= 23)
				log_line("voice: sign-in code %.6s (type @voice %.6s in game chat)", p + 17, p + 17);
			// sent by the game hook if it sees the game connection, else the
			// player types the short code with @voice
			hooks_queue_claim(p + 1);
			set_state(NET_CLAIMING);
		}
		break;
	case S_WELCOME:
		if (n >= 9) {
			Lock l(g_netLock);
			memcpy(&g_session, p + 1, 4);
			memcpy(&g_charId, p + 5, 4);
			g_state = NET_CONNECTED;
			log_line("voice: connected as char %u", g_charId);
		}
		break;
	case S_DENIED:
		if (net_state() == NET_CONNECTED || net_state() == NET_CLAIMING)
			set_state(NET_HELLO);
		break;
	case S_PONG:
		break;
	case S_AUDIO:
		if (n > 8 && g_cfg.enabled) { // voice off: older voice servers may still send audio
			uint32_t speaker;
			uint16_t seq;
			memcpy(&speaker, p + 1, 4);
			memcpy(&seq, p + 5, 2);
			audio_on_packet(speaker, seq, p[7], p + 8, n - 8);
		}
		break;
	case S_STATS:
		if (n >= 5)
			on_stats_part(p, n);
		break;
	case S_CELLS:
		if (n >= 3) {
			uint16_t count;
			memcpy(&count, p + 1, 2);
			if (n < 3 + count * 7)
				break;
			std::vector<LgpCell> cells(count);
			for (int i = 0; i < count; i++) {
				const uint8_t* e = p + 3 + i * 7;
				memcpy(&cells[i].x, e, 2);
				memcpy(&cells[i].y, e + 2, 2);
				memcpy(&cells[i].skill, e + 4, 2);
				cells[i].kind = e[6];
			}
			lgp_set_cells(cells.data(), count);
		}
		break;
	case S_NO_CELLS:
		lgp_set_no_cells();
		break;
	case S_AURAS:
		if (n >= 3) {
			uint16_t count;
			memcpy(&count, p + 1, 2);
			if (n >= 3 + count * 7)
				aura_set(p + 3, count);
		}
		break;
	case S_NO_AURAS:
		aura_set_none();
		break;
	case S_NO_STATS:
		if (n >= 2)
			stats_ui_on_unavailable(p[1]);
		break;
	case S_SPEAKER:
		if (n >= 5 + 24) {
			char name[25];
			memcpy(name, p + 5, 24);
			name[24] = '\0';
			uint32_t speaker;
			memcpy(&speaker, p + 1, 4);
			audio_on_speaker_name(speaker, name);
		}
		break;
	}
	lastHeard = now;
}

static DWORD WINAPI net_thread(LPVOID) {
	DWORD lastSend = 0, lastHeard = 0, stateSince = 0, lastCells = 0, lastAuras = 0;
	int sentEnabled = -1; // voice on/off last told to the server; -1 = not yet in this session
	NetState prev = NET_OFF;
	bool wasInGame = false;
	uint8_t buf[2048];

	for (;;) {
		DWORD now = GetTickCount();

		if (InterlockedExchange(&g_restart, 0)) {
			if (net_state() == NET_CONNECTED)
				send_with_session(C_BYE);
			set_state(NET_OFF);
			if (g_sock != INVALID_SOCKET) {
				closesocket(g_sock);
				g_sock = INVALID_SOCKET;
			}
		}

		NetState st = net_state();
		if (st != prev) {
			stateSince = now;
			prev = st;
			sentEnabled = -1; // a new session starts with voice on, tell it again
		}

		// With voice chat turned off the client still signs in (quietly, see
		// send_mode) so the Player Statistics window keeps working.
		if (!g_cfg.host[0]) {
			if (st == NET_CONNECTED)
				send_with_session(C_BYE);
			if (st != NET_OFF)
				set_state(NET_OFF);
			Sleep(500);
			continue;
		}
		// The hook may not see the game connection (anti-cheats can hide it),
		// so signing in doesn't wait for it. If it does see the player leave
		// the map-server, start over with a fresh code.
		bool inGame = hooks_in_game();
		if (wasInGame && !inGame && st != NET_OFF) {
			if (st == NET_CONNECTED)
				send_with_session(C_BYE);
			set_state(NET_HELLO);
			lastSend = 0;
		}
		wasInGame = inGame;
		if (st == NET_OFF || st == NET_WAIT_GAME) {
			if (g_sock == INVALID_SOCKET && !open_socket()) {
				Sleep(5000);
				continue;
			}
			set_state(NET_HELLO);
			lastSend = 0;
			continue;
		}

		if (st == NET_HELLO && now - lastSend > 3000) {
			uint8_t hello = C_HELLO;
			send_raw(&hello, 1);
			lastSend = now;
		} else if (st == NET_CLAIMING && now - stateSince > 9 * 60 * 1000) {
			// codes last 10 minutes on the server: get a new one before it runs out
			set_state(NET_HELLO);
		} else if (st == NET_CONNECTED) {
			if (now - lastSend > 10000) {
				send_with_session(C_PING);
				if (!g_cfg.enabled)
					send_mode(); // again now and then, in case the first one got lost
				lastSend = now;
			}
			// LGP: the skill cells around the player, twice a second
			if (lgp_wants_cells() && now - lastCells >= 500) {
				send_with_session(C_CELLS);
				lastCells = now;
			}
			// auras of the players around, once a second
			if (aura_wants() && now - lastAuras >= 1000) {
				send_with_session(C_AURAS);
				lastAuras = now;
			}
			if (sentEnabled != g_cfg.enabled) {
				sentEnabled = g_cfg.enabled;
				send_mode();
			}
			if (now - lastHeard > 35000)
				set_state(NET_HELLO);
		}

		fd_set rd;
		FD_ZERO(&rd);
		FD_SET(g_sock, &rd);
		timeval tv = {0, 100 * 1000};
		if (select(0, &rd, nullptr, nullptr, &tv) > 0) {
			int n = recv(g_sock, (char*)buf, sizeof(buf), 0);
			if (n > 0)
				handle_packet(buf, n, GetTickCount(), lastHeard);
		}
	}
}

void net_start() {
	InitializeCriticalSection(&g_netLock);
	HANDLE t = CreateThread(nullptr, 0, net_thread, nullptr, 0, nullptr);
	if (t)
		CloseHandle(t);
}

void net_restart() {
	InterlockedExchange(&g_restart, 1);
}
