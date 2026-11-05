// Hooks the game's send/closesocket imports so the plugin can pass its claim
// code to the map-server as one extra packet (CZ_VOICE_CLAIM, 0x0cf0).
// The hooks only watch and append; they never change the game's own packets.
#include "voice.h"

#include <string.h>
#include <tlhelp32.h>

typedef int(WSAAPI* send_fn)(SOCKET, const char*, int, int);
typedef int(WSAAPI* wsasend_fn)(SOCKET, LPWSABUF, DWORD, LPDWORD, DWORD, LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);
typedef int(WSAAPI* closesocket_fn)(SOCKET);

static send_fn real_send, ws2_send;
static wsasend_fn real_wsasend;
static closesocket_fn real_closesocket;
static ULONG_PTR* g_sendSlot; // the exe's import slot for send, watched in case something replaces our hook
static thread_local int g_sendDepth;

static CRITICAL_SECTION g_hookLock;
static SOCKET g_mapSocket = INVALID_SOCKET;
static bool g_claimPending;
static uint8_t g_claim[16];

static const uint16_t CZ_NOTIFY_ACTORINIT = 0x007d; // sent once the map has loaded
static const uint16_t CZ_ENTER2 = 0x0436;           // first packet on the map-server connection
static const uint16_t CZ_VOICE_CLAIM = 0x0cf0;
static const DWORD ENTER_GRACE_MS = 5000;           // map-server has the character by then

// A socket that opened with CZ_ENTER2 counts as the map connection once it has
// been up a few seconds, in case LoadEndAck (0x007d) is not seen on its own.
static SOCKET g_enterSocket = INVALID_SOCKET;
static DWORD g_enterTime;
static int g_logged; // packet ids logged so far, to diagnose clients that differ

// Called after every complete send (or WSASend) of the game. buf/len is the
// first buffer. Returns true when a claim should follow on this socket.
static bool after_game_send(SOCKET s, const uint8_t* buf, int len, uint8_t claim[18]) {
	Lock l(g_hookLock);
	uint16_t id = len >= 2 ? *(const uint16_t*)buf : 0;
	if (g_logged < 60) {
		// packet id and length only, never the contents
		log_line("send %u: id %04x, %d bytes", (unsigned)s, id, len);
		g_logged++;
	}
	if (id == CZ_NOTIFY_ACTORINIT) {
		if (g_mapSocket != s)
			log_line("entered the map server");
		g_mapSocket = s;
	} else if (id == CZ_ENTER2) {
		g_enterSocket = s;
		g_enterTime = GetTickCount();
	} else if (s == g_enterSocket && g_mapSocket != s && GetTickCount() - g_enterTime > ENTER_GRACE_MS) {
		log_line("entered the map server (by enter packet)");
		g_mapSocket = s;
	}
	if (g_claimPending && s == g_mapSocket) {
		*(uint16_t*)claim = CZ_VOICE_CLAIM;
		memcpy(claim + 2, g_claim, 16);
		g_claimPending = false;
		return true;
	}
	return false;
}

static int WSAAPI hook_send(SOCKET s, const char* buf, int len, int flags) {
	// If whatever replaced our hook calls back into it, go straight to winsock.
	if (g_sendDepth > 0)
		return ws2_send(s, buf, len, flags);
	g_sendDepth++;
	int ret = real_send(s, buf, len, flags);

	// Only append after a send that went out whole: the client flushes complete
	// packets, so the stream is at a packet boundary here.
	uint8_t claim[18];
	if (ret == len && len >= 2 && after_game_send(s, (const uint8_t*)buf, len, claim)) {
		int r = real_send(s, (const char*)claim, sizeof(claim), 0);
		log_line("sent voice claim (%d)", r);
	}
	g_sendDepth--;
	return ret;
}

static int WSAAPI hook_wsasend(SOCKET s, LPWSABUF bufs, DWORD count, LPDWORD sent, DWORD flags, LPWSAOVERLAPPED ov,
                               LPWSAOVERLAPPED_COMPLETION_ROUTINE done) {
	if (g_sendDepth > 0)
		return real_wsasend(s, bufs, count, sent, flags, ov, done);
	g_sendDepth++;
	int ret = real_wsasend(s, bufs, count, sent, flags, ov, done);
	g_sendDepth--;
	if (ret != 0 || ov || !sent || count == 0)
		return ret;
	DWORD total = 0;
	for (DWORD i = 0; i < count; i++)
		total += bufs[i].len;
	uint8_t claim[18];
	if (*sent == total && bufs[0].len >= 2 && after_game_send(s, (const uint8_t*)bufs[0].buf, (int)bufs[0].len, claim)) {
		int r = ws2_send(s, (const char*)claim, sizeof(claim), 0);
		log_line("sent voice claim (%d)", r);
	}
	return ret;
}

static int WSAAPI hook_closesocket(SOCKET s) {
	{
		Lock l(g_hookLock);
		if (s == g_mapSocket) {
			g_mapSocket = INVALID_SOCKET;
			g_claimPending = false;
			log_line("left the map server");
		}
		if (s == g_enterSocket)
			g_enterSocket = INVALID_SOCKET;
	}
	return real_closesocket(s);
}

bool hooks_in_game() {
	Lock l(g_hookLock);
	return g_mapSocket != INVALID_SOCKET;
}

void hooks_queue_claim(const uint8_t code[16]) {
	Lock l(g_hookLock);
	memcpy(g_claim, code, 16);
	g_claimPending = true;
}

// Patch one import of the exe. Slots are found through the original thunk
// (name or ordinal), so this works even if something else already replaced
// the pointer; in that case we chain to it.
bool patch_import(HMODULE exe, const char* dll, const char* name, WORD ordinal, void* hook, void** original,
                  ULONG_PTR** slot) {
	BYTE* base = (BYTE*)exe;
	IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
	IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
	IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
	if (!dir.VirtualAddress)
		return false;

	for (IMAGE_IMPORT_DESCRIPTOR* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); imp->Name; imp++) {
		if (_stricmp((const char*)(base + imp->Name), dll) != 0)
			continue;
		IMAGE_THUNK_DATA* orig = (IMAGE_THUNK_DATA*)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
		IMAGE_THUNK_DATA* iat = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
		for (; orig->u1.AddressOfData; orig++, iat++) {
			bool match;
			if (IMAGE_SNAP_BY_ORDINAL(orig->u1.Ordinal))
				match = IMAGE_ORDINAL(orig->u1.Ordinal) == ordinal;
			else if (imp->OriginalFirstThunk)
				match = strcmp((const char*)((IMAGE_IMPORT_BY_NAME*)(base + orig->u1.AddressOfData))->Name, name) == 0;
			else
				match = false;
			if (!match)
				continue;
			DWORD old;
			if (!VirtualProtect(&iat->u1.Function, sizeof(void*), PAGE_READWRITE, &old))
				return false;
			*original = (void*)iat->u1.Function;
			iat->u1.Function = (ULONG_PTR)hook;
			VirtualProtect(&iat->u1.Function, sizeof(void*), old, &old);
			if (slot)
				*slot = (ULONG_PTR*)&iat->u1.Function;
			return true;
		}
	}
	return false;
}

// ---- hooks in winsock itself ----
// Some clients (or an anti-cheat loaded into them) don't call send through
// the exe's import table, so winsock's own send and WSASend are hooked too,
// using the hot-patch space Windows leaves before each export. If something
// already put a jump there, we chain to it. Calls that already went through
// one of our hooks pass straight on (g_sendDepth).

static send_fn inline_send_next;
static wsasend_fn inline_wsasend_next;

// send through next, appending the claim when it is due
static int send_through(send_fn next, SOCKET s, const char* buf, int len, int flags) {
	if (g_sendDepth > 0)
		return next(s, buf, len, flags);
	g_sendDepth++;
	int ret = next(s, buf, len, flags);
	uint8_t claim[18];
	if (ret == len && len >= 2 && after_game_send(s, (const uint8_t*)buf, len, claim)) {
		int r = next(s, (const char*)claim, sizeof(claim), 0);
		log_line("sent voice claim (%d)", r);
	}
	g_sendDepth--;
	return ret;
}

static int WSAAPI inline_send(SOCKET s, const char* buf, int len, int flags) {
	return send_through(inline_send_next, s, buf, len, flags);
}

// for the import tables of DLLs other than the exe
static int WSAAPI hook_send_other(SOCKET s, const char* buf, int len, int flags) {
	return send_through(ws2_send, s, buf, len, flags);
}

static int WSAAPI inline_wsasend(SOCKET s, LPWSABUF bufs, DWORD count, LPDWORD sent, DWORD flags, LPWSAOVERLAPPED ov,
                                 LPWSAOVERLAPPED_COMPLETION_ROUTINE done) {
	if (g_sendDepth > 0)
		return inline_wsasend_next(s, bufs, count, sent, flags, ov, done);
	g_sendDepth++;
	int ret = inline_wsasend_next(s, bufs, count, sent, flags, ov, done);
	uint8_t claim[18];
	bool sendClaim = false;
	if (ret == 0 && !ov && sent && count > 0) {
		DWORD total = 0;
		for (DWORD i = 0; i < count; i++)
			total += bufs[i].len;
		sendClaim = *sent == total && bufs[0].len >= 2 &&
		            after_game_send(s, (const uint8_t*)bufs[0].buf, (int)bufs[0].len, claim);
	}
	if (sendClaim) {
		int r = ws2_send(s, (const char*)claim, sizeof(claim), 0); // goes through inline_send, which passes it on
		log_line("sent voice claim (%d)", r);
	}
	g_sendDepth--;
	return ret;
}

static bool write_code(void* at, const void* bytes, size_t n) {
	DWORD old;
	if (!VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &old))
		return false;
	memcpy(at, bytes, n);
	VirtualProtect(at, n, old, &old);
	FlushInstructionCache(GetCurrentProcess(), at, n);
	return true;
}

static bool hotpatch(const char* name, void* func, void* hook, void** next) {
	BYTE* p = (BYTE*)func;
	if (!p)
		return false;
	// someone else's hot-patch: short jump back into a long jump
	if (p[0] == 0xEB && p[-5] == 0xE9 && (int8_t)p[1] == -7)
		p -= 5;
	if (p[0] == 0xE9) {
		// already a jump (another hook): take its place and call where it went
		BYTE* target = p + 5 + *(int32_t*)(p + 1);
		int32_t rel = (int32_t)((BYTE*)hook - (p + 5));
		*next = target;
		if (!write_code(p + 1, &rel, 4))
			return false;
		log_line("%s: chained to an existing hook", name);
		return true;
	}
	if (p[0] == 0x8B && p[1] == 0xFF) {
		for (int i = 1; i <= 5; i++)
			if (p[-i] != 0x90 && p[-i] != 0xCC)
				goto unknown;
		BYTE jump[5] = {0xE9};
		*(int32_t*)(jump + 1) = (int32_t)((BYTE*)hook - p);
		*next = p + 2; // past mov edi,edi: the function's real start
		if (!write_code(p - 5, jump, 5))
			return false;
		static const BYTE back[2] = {0xEB, 0xF9}; // jmp -7, to the long jump
		if (!write_code(p, back, 2))
			return false;
		log_line("%s: hooked in winsock", name);
		return true;
	}
unknown:
	log_line("%s: cannot hook in winsock (starts %02x %02x %02x %02x %02x)", name, p[0], p[1], p[2], p[3], p[4]);
	return false;
}

// ---- import tables of the other modules ----
// A DLL in the game (an anti-cheat that does the networking, for example)
// may import send itself. Point its import at our hook too.
static void hook_module_imports(HMODULE mod, HMODULE self) {
	static std::vector<HMODULE> done;
	if (mod == self || mod == GetModuleHandleA(nullptr))
		return;
	for (HMODULE m : done)
		if (m == mod)
			return;
	done.push_back(mod);
	// leave Windows' own DLLs alone
	char path[MAX_PATH], windir[MAX_PATH];
	GetModuleFileNameA(mod, path, sizeof(path));
	GetWindowsDirectoryA(windir, sizeof(windir));
	if (_strnicmp(path, windir, strlen(windir)) == 0)
		return;
	const char* slash = strrchr(path, '\\');
	const char* name = slash ? slash + 1 : path;
	void* original;
	for (const char* dll : {"ws2_32.dll", "wsock32.dll"})
		if (patch_import(mod, dll, "send", 19, (void*)hook_send_other, &original))
			log_line("send import of %s hooked", name);
}

static void hook_all_modules() {
	HMODULE self;
	GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                   (LPCSTR)hook_all_modules, &self);
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
	if (snap == INVALID_HANDLE_VALUE)
		return;
	MODULEENTRY32 me = {sizeof(me)};
	for (BOOL ok = Module32First(snap, &me); ok; ok = Module32Next(snap, &me))
		hook_module_imports(me.hModule, self);
	CloseHandle(snap);
}

bool hooks_install() {
	InitializeCriticalSection(&g_hookLock);
	HMODULE exe = GetModuleHandleA(nullptr);
	HMODULE ws2 = LoadLibraryA("ws2_32.dll");
	ws2_send = (send_fn)GetProcAddress(ws2, "send");
	bool ok = false;
	// ws2_32 and wsock32 share these ordinals: closesocket 3, send 19
	for (const char* dll : {"ws2_32.dll", "wsock32.dll"}) {
		if (!real_send && patch_import(exe, dll, "send", 19, (void*)hook_send, (void**)&real_send, &g_sendSlot))
			ok = true;
		if (!real_closesocket)
			patch_import(exe, dll, "closesocket", 3, (void*)hook_closesocket, (void**)&real_closesocket);
	}
	if (patch_import(exe, "ws2_32.dll", "WSASend", 76, (void*)hook_wsasend, (void**)&real_wsasend)) {
		log_line("WSASend hooked too");
		ok = true;
	}
	if (!real_closesocket)
		real_closesocket = (closesocket_fn)GetProcAddress(ws2, "closesocket");
	if (hotpatch("send", (void*)ws2_send, (void*)inline_send, (void**)&inline_send_next))
		ok = true;
	if (hotpatch("WSASend", (void*)GetProcAddress(ws2, "WSASend"), (void*)inline_wsasend, (void**)&inline_wsasend_next))
		ok = true;
	hook_all_modules();
	log_line(ok ? "game connection hooked" : "send import not found");
	return ok;
}

// Something loaded after us (an anti-cheat, another plugin) may put its own
// function into the send slot. Hook again on top of it and pass calls on.
void hooks_check() {
	hook_all_modules(); // DLLs loaded since last time
	if (!g_sendSlot || *g_sendSlot == (ULONG_PTR)hook_send)
		return;
	DWORD old;
	if (!VirtualProtect(g_sendSlot, sizeof(void*), PAGE_READWRITE, &old))
		return;
	real_send = (send_fn)*g_sendSlot;
	*g_sendSlot = (ULONG_PTR)hook_send;
	VirtualProtect(g_sendSlot, sizeof(void*), old, &old);
	log_line("send hook was replaced by something else, hooked again");
}
