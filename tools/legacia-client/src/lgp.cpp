// LGP: coloured cells drawn on the ground around the player (Graymap window,
// LGP tab). graymap.cpp draws them; this file keeps the skill cells around
// the player, which the map-server sends through the voice server (net.cpp
// asks twice a second while the layer is on).
#include "voice.h"

// ---------------------------------------------------------------- skill cells

static CRITICAL_SECTION g_cellLock;
static bool g_cellLockReady;
static std::vector<LgpCell> g_cells;
static DWORD g_cellsAt;   // when the last answer came
static bool g_cellsNone; // the voice server said the map-server can't answer

static void cell_lock_init() {
	if (!g_cellLockReady) {
		InitializeCriticalSection(&g_cellLock);
		g_cellLockReady = true;
	}
}

void lgp_init() {
	cell_lock_init();
}

bool lgp_wants_cells() {
	return g_cfg.lgpEnabled && g_cfg.lgpAoe && graymap_available();
}

void lgp_set_cells(const LgpCell* c, int n) {
	Lock l(g_cellLock);
	g_cells.assign(c, c + n);
	g_cellsAt = GetTickCount();
	g_cellsNone = false;
}

void lgp_set_no_cells() {
	Lock l(g_cellLock);
	g_cells.clear();
	g_cellsNone = true;
}

std::vector<LgpCell> lgp_cells() {
	Lock l(g_cellLock);
	// answers come twice a second; after 2 s without one the cells are stale
	if (GetTickCount() - g_cellsAt > 2000)
		return {};
	return g_cells;
}

LgpServer lgp_server_state(int* count) {
	*count = 0;
	if (net_state() != NET_CONNECTED)
		return LGP_SERVER_NO_VOICE;
	Lock l(g_cellLock);
	if (g_cellsNone)
		return LGP_SERVER_UNSUPPORTED;
	if (GetTickCount() - g_cellsAt > 2000)
		return LGP_SERVER_WAITING;
	*count = (int)g_cells.size();
	return LGP_SERVER_ON;
}
