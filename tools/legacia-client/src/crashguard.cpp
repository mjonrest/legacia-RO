// Crash guard: stops a client crash that the game exe itself has.
//
// the Legacia client exe (2026-01-07 Ragexe) handles "use a skill on this target" in
// CGameMode (function at 0xce05a0). On one path (message type 0) it looks the
// target up with CWorld::GetActorByGID (0xac6c70) and reads the actor's
// position without checking that the actor still exists:
//
//   ce0d29  call  0xac6c70            ; eax = actor or NULL
//   ce0d2e  lea   ecx,[ebp+8]
//   ce0d31  push  ecx
//   ce0d32  lea   ecx,[ebp-0x18]
//   ce0d35  movss xmm0,[eax+0x18]     ; crash when eax == NULL
//
// That happens when the target has just left the screen (a monster killed by
// someone else's AoE, for example) while the skill is being aimed at it; seen
// in Constellation Tower, where many players kill the same monsters. Every
// other lookup in that function checks for NULL and gives up, so we do the
// same: a vectored exception handler catches the access violation at exactly
// that instruction with eax == 0, drops the one value pushed at 0xce0d31 and
// continues at the function's own "give up" exit (0xce18b3), which another
// branch already jumps to from the same spot (0xce0d1b). No game code is
// changed; on any other exe the guard stays off.
#include "voice.h"

namespace {

const uintptr_t FAULT_AT = 0xce0d29; // start of the checked bytes
const uintptr_t FAULT_EIP = 0xce0d35;
const uintptr_t GIVE_UP = 0xce18b3; // mov esi,[ebp+0xc]; pop edi; ... ret 8
const unsigned char EXPECT_FAULT[] = {
	0xe8, 0x42, 0x5f, 0xde, 0xff, // call 0xac6c70
	0x8d, 0x4d, 0x08,             // lea ecx,[ebp+8]
	0x51,                         // push ecx
	0x8d, 0x4d, 0xe8,             // lea ecx,[ebp-0x18]
	0xf3, 0x0f, 0x10, 0x40, 0x18, // movss xmm0,[eax+0x18]
};
const unsigned char EXPECT_GIVE_UP[] = {0x8b, 0x75, 0x0c, 0x5f, 0x8b, 0xc6, 0x5e, 0x5b, 0x8b, 0xe5, 0x5d, 0xc2, 0x08, 0x00};

volatile LONG g_hits;

bool matches(uintptr_t at, const unsigned char* bytes, size_t len) {
	MEMORY_BASIC_INFORMATION mi;
	if (!VirtualQuery((void*)at, &mi, sizeof(mi)) || mi.State != MEM_COMMIT)
		return false;
	if ((uintptr_t)mi.BaseAddress + mi.RegionSize < at + len)
		return false;
	return memcmp((const void*)at, bytes, len) == 0;
}

LONG CALLBACK guard(EXCEPTION_POINTERS* ep) {
	EXCEPTION_RECORD* er = ep->ExceptionRecord;
	CONTEXT* c = ep->ContextRecord;
	if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || c->Eip != FAULT_EIP || c->Eax != 0)
		return EXCEPTION_CONTINUE_SEARCH;
	c->Esp += 4; // the push ecx at 0xce0d31
	c->Eip = GIVE_UP;
	if (InterlockedIncrement(&g_hits) <= 20)
		log_line("crash guard: skill target was gone, skipped (avoided a client crash)");
	return EXCEPTION_CONTINUE_EXECUTION;
}

} // namespace

bool crashguard_install() {
	if (!matches(FAULT_AT, EXPECT_FAULT, sizeof(EXPECT_FAULT)) ||
	    !matches(GIVE_UP, EXPECT_GIVE_UP, sizeof(EXPECT_GIVE_UP))) {
		log_line("crash guard: this game exe is not supported, crash guard is off");
		return false;
	}
	if (!AddVectoredExceptionHandler(1, guard)) {
		log_line("crash guard: could not add the exception handler");
		return false;
	}
	log_line("crash guard: ready");
	return true;
}
