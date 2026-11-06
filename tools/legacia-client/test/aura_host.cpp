// Test host for src/aura.cpp (run under Wine): builds a fake game at the real
// exe addresses (renderer, mode manager, world, actor list, our account ID),
// copies the exe's own ProjectVertex code, lets aura.cpp draw a few frames and
// writes every polygon it adds to aura_faces.txt (see test/render_faces.py).
//
//   i686-w64-mingw32-g++ -O1 -std=c++17 -I../src -Wl,--image-base,0x400000 \
//       aura_host.cpp -o aura_host.exe -static
//   wine aura_host.exe LegaciaRO.exe <plugins folder with auras\> > aura_faces.txt
#include "voice.h"

#include <math.h>
#include <stdio.h>

static DWORD g_fakeTick;
#define GetTickCount() (g_fakeTick)
#include "../src/aura.cpp"
#undef GetTickCount

Config g_cfg;
HMODULE g_module;
HWND g_gameWnd;
void log_line(const char* fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	fprintf(stderr, "log: ");
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}
NetState net_state() {
	return NET_CONNECTED;
}

static std::vector<uint8_t> g_exe;
// stands in for the exe's memory from 0x400000 on (see main)
__attribute__((section(".fakeexe"), used)) static uint8_t g_space[0x1400000];
static uint8_t* va(uintptr_t a) {
	return (uint8_t*)a;
}
// copy bytes of the real exe (mapped by section) to the same address
static void copy_exe(uintptr_t at, int len) {
	const uint8_t* pe = g_exe.data();
	uint32_t peoff = *(uint32_t*)(pe + 0x3c);
	int nsec = *(uint16_t*)(pe + peoff + 6), optsz = *(uint16_t*)(pe + peoff + 20);
	const uint8_t* sec = pe + peoff + 24 + optsz;
	for (int i = 0; i < nsec; i++, sec += 40) {
		uint32_t vaddr = 0x400000 + *(uint32_t*)(sec + 12), raw = *(uint32_t*)(sec + 20), rawsz = *(uint32_t*)(sec + 16);
		if (at >= vaddr && at + len <= vaddr + rawsz) {
			memcpy(va(at), pe + raw + (at - vaddr), len);
			return;
		}
	}
	fprintf(stderr, "copy_exe: %08x not in the file\n", (unsigned)at);
	exit(1);
}
static void jump_to(uintptr_t at, void* fn) {
	uint8_t* p = va(at);
	p[0] = 0xe9;
	*(int32_t*)(p + 1) = (int32_t)((uintptr_t)fn - (at + 5));
}

// ---- fakes ----
struct Face {
	TLV* verts;
	int n;
	void* tex;
	int a, b;
	short c;
	short pad;
	int src, dst, e, prim;
	TLV v[4];
};
static Face* g_pool = new Face[20000];
static int g_used;
static std::vector<Face*> g_added;
static uint8_t* __fastcall fake_borrow(void*, void*) {
	Face* f = &g_pool[g_used++ % 20000];
	memset(f, 0, sizeof(*f));
	f->verts = f->v;
	f->n = 4;
	f->c = 1;
	f->prim = 5;
	for (auto& v : f->v)
		v.specular = 0xff000000u;
	return (uint8_t*)f;
}
static void __fastcall fake_add(void*, void*, void* face, int flags) {
	if (flags != 1)
		fprintf(stderr, "AddRP flags %d\n", flags);
	g_added.push_back((Face*)face);
}
struct FakeTex {
	void** vt;
	int id, w, h;
};
static void* g_texVt[16];
static int g_texCount;
static void __fastcall fake_update(void* tex, void*, int, int, int w, int h, const uint32_t* px, int, int) {
	FakeTex* t = (FakeTex*)tex;
	char name[64];
	snprintf(name, sizeof(name), "tex_%03d.raw", t->id);
	FILE* f = fopen(name, "wb");
	fwrite(&w, 4, 1, f);
	fwrite(&h, 4, 1, f);
	fwrite(px, 4, (size_t)w * h, f);
	fclose(f);
}
static void* __fastcall fake_create(void*, void*, int w, int h, int format, int, int) {
	FakeTex* t = new FakeTex{g_texVt, ++g_texCount, w, h};
	fprintf(stderr, "CreateTexture %dx%d format %d -> tex %d\n", w, h, format, t->id);
	return t;
}
static int g_dev;
static void* __cdecl fake_device() {
	return &g_dev;
}

int main(int argc, char** argv) {
	if (argc < 3) {
		fprintf(stderr, "usage: aura_host LegaciaRO.exe plugins_folder\n");
		return 1;
	}
	FILE* f = fopen(argv[1], "rb");
	if (!f)
		return 1;
	fseek(f, 0, SEEK_END);
	g_exe.resize(ftell(f));
	fseek(f, 0, SEEK_SET);
	fread(g_exe.data(), 1, g_exe.size(), f);
	fclose(f);

	// the exe's address range
	// The host is linked at the exe's own base, and g_space (below) covers the
	// addresses aura.cpp uses; make it executable for the copied exe code.
	if ((uintptr_t)g_space > 0x540000 || (uintptr_t)g_space + sizeof(g_space) < 0x1730000) {
		fprintf(stderr, "g_space is at %p, link with --image-base 0x400000\n", (void*)g_space);
		return 1;
	}
	DWORD old;
	VirtualProtect((void*)0x540000, 0x1730000 - 0x540000, PAGE_EXECUTE_READWRITE, &old);
	// real code where aura.cpp checks it, real ProjectVertex and its constants
	copy_exe(0x55c750, 0xf3);
	copy_exe(0x543060, 16);
	copy_exe(0x558da0, 16);
	copy_exe(0x569040, 16);
	copy_exe(0x55c440, 16);
	copy_exe(0x10d6f98, 4);
	copy_exe(0x10d814c, 4);
	copy_exe(0x10d812c, 4);
	// g_module: aura.cpp looks for <dir of module>\auras\  -> fake a module path via the exe name
	static char modpath[MAX_PATH];
	snprintf(modpath, sizeof(modpath), "%s\\legacia_plugin.dll", argv[2]);

	if (!aura_install()) {
		fprintf(stderr, "aura_install failed\n");
		return 1;
	}
	jump_to(0x543060, (void*)fake_borrow);
	jump_to(0x558da0, (void*)fake_add);
	jump_to(0x569040, (void*)fake_create);
	jump_to(0x55c440, (void*)fake_device);
	g_texVt[8] = (void*)fake_update;

	// renderer: frame +0x38, focal +4/+8, screen centre +0x30/+0x34 (1024x768)
	static uint8_t renderer[0x400];
	*(float*)(renderer + 4) = 1100.0f;
	*(float*)(renderer + 8) = 1100.0f;
	*(int*)(renderer + 0x30) = 512;
	*(int*)(renderer + 0x34) = 384;
	*(uint8_t**)va(0x1382fa8) = renderer;
	// world with our player at the centre and 2 more players + a monster
	static uint8_t world[0x100], mode[0x100], actors[4][0x200];
	uint8_t* mm = va(0x1343108);
	*(int*)(mm + 0x58) = 1;
	*(uint8_t**)(mm + 4) = mode;
	*(uint8_t**)(mode + 0xcc) = world;
	*(uint32_t*)va(0x172d55c) = 2000001;
	*(uint8_t**)(world + 0x2c) = actors[0];
	struct Node {
		Node *next, *prev;
		void* val;
	};
	static Node head, nodes[3];
	head.next = &nodes[0];
	for (int i = 0; i < 3; i++) {
		nodes[i].next = i < 2 ? &nodes[i + 1] : &head;
		nodes[i].val = actors[i + 1];
	}
	*(Node**)(world + 0x10) = &head;
	*(int*)(world + 0x14) = 3;
	const float pos[4][3] = {{0, -2, 0}, {-45, 0, 10}, {40, -4, -20}, {10, 0, 40}};
	const uint32_t aids[4] = {2000001, 2000002, 2000003, 110000123};
	for (int i = 0; i < 4; i++) {
		memcpy(actors[i] + 0x10, pos[i], 12);
		*(uint32_t*)(actors[i] + 0x110) = aids[i];
	}
	// auras from the "server": me 12/3/1, player 2 ground only, player 3 rings + sparkles
	uint8_t entries[3 * 7] = {};
	const uint8_t parts[3][3] = {{12, 3, 1}, {40, 0, 0}, {0, 20, 5}};
	for (int i = 0; i < 3; i++) {
		memcpy(entries + i * 7, &aids[i], 4);
		memcpy(entries + i * 7 + 4, parts[i], 3);
	}

	g_cfg.auraEnabled = g_cfg.auraOwn = g_cfg.auraOthers = 1;
	g_cfg.auraSize = 100;
	aura_init();
	// a GetModuleFileName for g_module: use a real loaded module whose folder is argv[2]? Simpler: copy
	// the folder into the current dir. aura.cpp uses GetModuleFileNameA(g_module) -> this exe's folder.
	(void)modpath;

	// camera like the game's: looking down at 50 degrees from 300 units, turned 30 degrees
	float elev = 50 * 3.14159265f / 180, yaw = 30 * 3.14159265f / 180, dist = 300;
	// view: camera = A * world + t; rows (m0 m3 m6), (m1 m4 m7), (m2 m5 m8); up is -y in the world
	// camera axes in world space: right r, down d, forward f
	float fx = sinf(yaw) * cosf(elev), fy = sinf(elev), fz = cosf(yaw) * cosf(elev); // looking down (+y is down)
	float rx = cosf(yaw), ry = 0, rz = -sinf(yaw);
	// down = f x r ... screen y grows downwards
	float dx = fy * rz - fz * ry, dy = fz * rx - fx * rz, dz = fx * ry - fy * rx;
	float cam[3] = {-fx * dist, -fy * dist, -fz * dist};
	float m[12];
	m[0] = rx, m[3] = ry, m[6] = rz;
	m[1] = dx, m[4] = dy, m[7] = dz;
	m[2] = fx, m[5] = fy, m[8] = fz;
	m[9] = -(rx * cam[0] + ry * cam[1] + rz * cam[2]);
	m[10] = -(dx * cam[0] + dy * cam[1] + dz * cam[2]);
	m[11] = -(fx * cam[0] + fy * cam[1] + fz * cam[2]);

	int frames = argc > 3 ? atoi(argv[3]) : 1;
	for (int fr = 0; fr < frames; fr++) {
		g_fakeTick = 100000 + fr * 50; // 20 frames a second
		aura_set(entries, 3);
		g_added.clear();
		g_used = 0;
		for (int k = 0; k < 3; k++) { // several textures load over the first frames
			(*(int*)(renderer + 0x38))++;
			g_added.clear();
			aura_draw(m);
		}
		// one more call in the same frame must add nothing
		size_t n = g_added.size();
		aura_draw(m);
		if (g_added.size() != n)
			fprintf(stderr, "drew twice in one frame\n");
		printf("frame %d %u\n", fr, (unsigned)g_added.size());
		for (int i = 0; i < 4; i++) { // where each character stands, for the preview
			V3 foot = {pos[i][0], pos[i][1], pos[i][2]};
			TLV v;
			((ProjectFn)PROJECT_VERTEX)(renderer, &foot, m, &v);
			printf("actor %u %.2f %.2f %.6f\n", aids[i], v.x, v.y, v.rhw);
		}
		for (Face* fc : g_added) {
			printf("face %d %d %d", ((FakeTex*)fc->tex)->id, fc->src, fc->dst);
			for (auto& v : fc->v)
				printf(" %.2f %.2f %.6f %.6f %08x %.4f %.4f", v.x, v.y, v.z, v.rhw, v.color, v.u, v.v);
			printf("\n");
		}
	}
	fprintf(stderr, "ok\n");
	return 0;
}
