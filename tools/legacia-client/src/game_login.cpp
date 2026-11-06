// Reads the game's own login (account id, character id and login code) from
// the client, so the voice server can ask the map-server who this client is.
// The login code (login_id1) is a random number the login server gives this
// client only; the client sends it to the map-server when it enters a map
// (CZ_ENTER, packet 0x0436). Sending the same three numbers to the voice
// server proves which character this plugin belongs to, even when several
// players share one address and without typing @voice.
//
// The addresses are for the 2026-01-07 Ragexe (the Legacia exe matches). They are
// taken from the code that builds CZ_ENTER, and that code is checked first,
// so on any other exe this simply stays off.
#include "voice.h"

#include <string.h>

namespace {

// the CZ_ENTER builder: mov ecx,g_session / mov eax,[AID] / mov [ebp-3e],eax /
// mov eax,[GID] / mov [ebp-3a],eax / mov eax,[AuthCode] / mov [ebp-36],eax
const uintptr_t CZ_ENTER_CODE = 0xd04be9;
const unsigned char EXPECT_CZ_ENTER[] = {
	0xb9, 0x58, 0xbf, 0x72, 0x01,       // mov ecx, 0x172bf58
	0xa1, 0x5c, 0xd5, 0x72, 0x01,       // mov eax, [0x172d55c]  account id
	0x89, 0x45, 0xc2,                   // mov [ebp-0x3e], eax
	0xa1, 0x60, 0xd5, 0x72, 0x01,       // mov eax, [0x172d560]  character id
	0x89, 0x45, 0xc6,                   // mov [ebp-0x3a], eax
	0xa1, 0xfc, 0xcd, 0x72, 0x01,       // mov eax, [0x172cdfc]  login code
	0x89, 0x45, 0xca,                   // mov [ebp-0x36], eax
};
const uintptr_t ACCOUNT_ID = 0x172d55c, CHAR_ID = 0x172d560, AUTH_CODE = 0x172cdfc;

bool readable(uintptr_t at, size_t len) {
	MEMORY_BASIC_INFORMATION mi;
	if (!VirtualQuery((void*)at, &mi, sizeof(mi)) || mi.State != MEM_COMMIT)
		return false;
	if (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD))
		return false;
	return (uintptr_t)mi.BaseAddress + mi.RegionSize >= at + len;
}

int g_supported = -1; // -1 not checked yet

bool supported() {
	if (g_supported < 0) {
		g_supported = readable(CZ_ENTER_CODE, sizeof(EXPECT_CZ_ENTER)) &&
		              memcmp((const void*)CZ_ENTER_CODE, EXPECT_CZ_ENTER, sizeof(EXPECT_CZ_ENTER)) == 0 &&
		              readable(AUTH_CODE, 4) && readable(ACCOUNT_ID, 8);
		log_line(g_supported ? "voice: signs in with the game login"
		                     : "voice: this game exe is not supported for signing in with the game login, by address or @voice only");
	}
	return g_supported == 1;
}

} // namespace

bool game_login(GameLogin& out) {
	if (!supported())
		return false;
	out.accountId = *(volatile uint32_t*)ACCOUNT_ID;
	out.charId = *(volatile uint32_t*)CHAR_ID;
	out.authCode = *(volatile uint32_t*)AUTH_CODE;
	// zero until the player picked a character and entered a map
	return out.accountId != 0 && out.charId != 0 && out.authCode != 0;
}
