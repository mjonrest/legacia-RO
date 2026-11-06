// Auras: the plugin draws the Gepard-style aura of every player around (part 1
// ground circle, part 2 rings, part 3 sparkles), since the Gepard client no
// longer draws them. The map-server keeps the aura (AURA_DATA, the Aura Master
// NPC) and sends the auras of the players around through the voice server;
// net.cpp asks about once a second.
//
// Drawing uses the client's own 3D pipeline, like Graymap: each piece is one
// of the client's polygons (projected with the client's own camera and added
// to its sorted list of see-through polygons), so the aura sits on the ground
// in 3D, turns with the camera and is drawn in the right order with the
// characters. It is drawn from Graymap's ground hook (graymap.cpp), once a
// frame. Positions come from the client's actor list: account ID at +0x110,
// position at +0x10. Addresses are for the Legacia client exe (2026-01-07
// Ragexe) and are checked before use; on another exe auras stay off.
//
// Textures: plugins\auras\<part>\<nnn>.bmp, 32-bit BMPs made from the Gepard
// aura textures by tools/legacia_aura/genplugin.py, drawn additively.
#include "voice.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <unordered_map>

namespace {

// ---- the Legacia client exe (2026-01-07) ----
// CRenderer::ProjectVertex(const vector3d* world, const matrix* view, tlvertex* out)
const uintptr_t PROJECT_VERTEX = 0x55c750;
// CRenderer::BorrowNullRP(): a 4-vertex polygon from the frame's pool
const uintptr_t BORROW_RP = 0x543060;
// CRenderer::AddRP(polygon, flags): flag 1 = sorted with the other see-through polygons
const uintptr_t ADD_RP = 0x558da0;
const uintptr_t RENDERER_PTR = 0x1382fa8, MODE_MGR = 0x1343108;
const uintptr_t SESSION_AID = 0x172d55c; // CSession: our own account ID
// device wrapper and texture creation, see graymap.cpp
const uintptr_t GET_DEVICE = 0x55c440, CREATE_TEXTURE = 0x569040;
const int TEX_FORMAT_ARGB8888 = 4, TEX_FORMAT_ARGB1555 = 0;
// CWorld: actor list (std::list) at +0x10, our own player at +0x2c
// CGameActor: position (x, y = height, negative is up, z) at +0x10, account/GID at +0x110

struct Expect {
	uintptr_t at;
	const char* bytes;
	int len;
};
const Expect EXPECT[] = {
	{PROJECT_VERTEX, "\x55\x8b\xec\x8b\x55\x0c\xf3\x0f\x10\x1d", 10},
	{BORROW_RP, "\x55\x8b\xec\x83\xec\x08\x53\x56\x57\x8b", 10},
	{ADD_RP, "\x55\x8b\xec\x83\xec\x08\x56\x8b\xf1\x57", 10},
	{CREATE_TEXTURE, "\x55\x8b\xec\x8b\x09\x85\xc9", 7},
	{GET_DEVICE, "\x55\x8b\xec\x6a\xff\x68\x71\x7c\xeb\x00", 10},
};

struct V3 {
	float x, y, z;
};
// D3D pre-transformed vertex as the client keeps it
struct TLV {
	float x, y, z, rhw;
	uint32_t color, specular;
	float u, v;
};
static_assert(sizeof(TLV) == 0x20, "vertex size");

typedef void(__thiscall* ProjectFn)(void* renderer, const V3* src, const void* view, TLV* out);
typedef uint8_t*(__thiscall* BorrowFn)(void* renderer);
typedef void(__thiscall* AddRpFn)(void* renderer, void* face, int flags);
typedef void*(__cdecl* GetDeviceFn)();
typedef void*(__thiscall* CreateTextureFn)(void* dev, int w, int h, int format, int a, int b);
typedef void(__thiscall* UpdateTextureFn)(void* tex, int x, int y, int w, int h, const uint32_t* px, int a, int b);

bool g_ok;

// ---- aura data from the server (net thread writes, game thread reads) ----
struct Aura {
	uint8_t part[3]; // ground circle, ring, sparkle; 0 = none
};
CRITICAL_SECTION g_lock;
std::unordered_map<uint32_t, Aura> g_auras; // account ID -> aura
DWORD g_aurasAt;                             // when the last answer came
bool g_none;                                 // the map-server can't answer

bool readable(uintptr_t at, int len) {
	MEMORY_BASIC_INFORMATION mi;
	if (!VirtualQuery((void*)at, &mi, sizeof(mi)) || mi.State != MEM_COMMIT || (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
		return false;
	return at + len <= (uintptr_t)mi.BaseAddress + mi.RegionSize;
}

// ---- textures ----
std::unordered_map<int, void*> g_tex; // part * 1000 + number -> client texture, null = could not load
bool g_texFailed;
int g_loadsThisFrame;

std::string tex_folder() {
	char path[MAX_PATH];
	GetModuleFileNameA(g_module, path, MAX_PATH); // ...\plugins\legacia_plugin.dll
	char* slash = strrchr(path, '\\');
	if (slash)
		slash[1] = '\0';
	else
		path[0] = '\0';
	return std::string(path) + "auras\\";
}

// a 32-bit bottom-up BMP as genplugin.py writes it, as 0xAARRGGBB rows top down
bool read_bmp32(const char* path, std::vector<uint32_t>& px, int& w, int& h) {
	FILE* f = fopen(path, "rb");
	if (!f)
		return false;
	uint8_t head[54];
	bool ok = fread(head, 1, 54, f) == 54 && head[0] == 'B' && head[1] == 'M';
	if (ok) {
		uint32_t off, comp;
		int32_t iw, ih;
		uint16_t bpp;
		memcpy(&off, head + 10, 4);
		memcpy(&iw, head + 18, 4);
		memcpy(&ih, head + 22, 4);
		memcpy(&bpp, head + 28, 2);
		memcpy(&comp, head + 30, 4);
		bool flip = ih > 0;
		w = iw;
		h = ih < 0 ? -ih : ih;
		ok = bpp == 32 && (comp == 0 || comp == 3) && w >= 8 && h >= 8 && w <= 512 && h <= 512 && fseek(f, off, SEEK_SET) == 0;
		if (ok) {
			px.resize((size_t)w * h);
			for (int y = 0; ok && y < h; y++)
				ok = fread(&px[(size_t)(flip ? h - 1 - y : y) * w], 4, w, f) == (size_t)w;
		}
	}
	fclose(f);
	return ok;
}

void* texture(int part, int num) {
	int key = part * 1000 + num;
	auto it = g_tex.find(key);
	if (it != g_tex.end())
		return it->second;
	if (g_texFailed || g_loadsThisFrame >= 3)
		return nullptr; // a few new textures a frame, so a crowd doesn't stall the game
	g_loadsThisFrame++;
	char path[MAX_PATH];
	snprintf(path, sizeof(path), "%s%d\\%03d.bmp", tex_folder().c_str(), part, num);
	std::vector<uint32_t> px;
	int w = 0, h = 0;
	void* tex = nullptr;
	if (read_bmp32(path, px, w, h)) {
		void* dev = ((GetDeviceFn)GET_DEVICE)();
		if (dev) {
			tex = ((CreateTextureFn)CREATE_TEXTURE)(dev, w, h, TEX_FORMAT_ARGB8888, 0, 0);
			if (!tex) // the alpha isn't needed (drawn additively), so the format Graymap uses works too
				tex = ((CreateTextureFn)CREATE_TEXTURE)(dev, w, h, TEX_FORMAT_ARGB1555, 0, 0);
		}
		if (tex) {
			void** vt = *(void***)tex;
			((UpdateTextureFn)vt[8])(tex, 0, 0, w, h, px.data(), 0, 0);
		} else {
			log_line("aura: could not create a texture, auras are off");
			g_texFailed = true;
		}
	} else {
		log_line("aura: missing or bad texture %s", path);
	}
	g_tex[key] = tex;
	return tex;
}

// ---- drawing ----
uint8_t* g_renderer;
const void* g_view;
float g_scale = 1.0f; // world units per Gepard "pixel" (the sizes below come from the old STR canvas)

// One polygon (strip order: top left, top right, bottom left, bottom right),
// projected with the client's camera and added to its see-through list.
// zbias lifts ground pieces off the ground, like the client's own ground cells.
void emit(const TLV* v, void* tex) {
	uint8_t* face = ((BorrowFn)BORROW_RP)(g_renderer);
	if (!face)
		return;
	TLV* out = *(TLV**)face;
	memcpy(out, v, sizeof(TLV) * 4);
	*(void**)(face + 8) = tex;
	*(int*)(face + 0x18) = 2; // D3DBLEND_ONE: additive, black adds nothing
	*(int*)(face + 0x1c) = 2;
	((AddRpFn)ADD_RP)(g_renderer, face, 1);
}

bool project(const V3& p, TLV& out) {
	((ProjectFn)PROJECT_VERTEX)(g_renderer, &p, g_view, &out);
	return out.rhw > 0.0f && out.z > 0.0f && out.z < 1.0f;
}

uint32_t grey(float a) {
	int g = (int)(a + 0.5f);
	g = g < 0 ? 0 : g > 255 ? 255 : g;
	return 0xff000000u | (uint32_t)(g << 16) | (uint32_t)(g << 8) | (uint32_t)g;
}

bool quad(const V3 p[4], const float uv[8], uint32_t color, void* tex, float zbias = 0) {
	TLV v[4];
	for (int i = 0; i < 4; i++) {
		if (!project(p[i], v[i]))
			return false;
		v[i].z -= zbias;
		v[i].color = color;
		v[i].specular = 0xff000000u;
		v[i].u = uv[i * 2];
		v[i].v = uv[i * 2 + 1];
	}
	emit(v, tex);
	return true;
}

uint32_t hash(uint32_t x) {
	x ^= x >> 16;
	x *= 0x7feb352d;
	x ^= x >> 15;
	x *= 0x846ca68b;
	x ^= x >> 16;
	return x;
}
float rnd(uint32_t& s, float lo, float hi) {
	s = hash(s + 0x9e3779b9u);
	return lo + (hi - lo) * (s & 0xffffff) / 16777215.0f;
}

const float PI = 3.14159265f;

// part 1: the ground circle, turning flat on the floor once every 6 s
void draw_ground(const V3& foot, void* tex, float t) {
	const float R = 62 * g_scale;
	float th = 2 * PI * fmodf(t / 6.0f, 1.0f), c = cosf(th), s = sinf(th);
	// corners in order around the square, with their texture corners
	const float cx[4] = {-1, 1, 1, -1}, cz[4] = {-1, -1, 1, 1};
	const float cu[4] = {0, 1, 1, 0}, cv[4] = {0, 0, 1, 1};
	V3 p[4];
	TLV v[4];
	for (int i = 0; i < 4; i++) {
		float x = cx[i] * R * c - cz[i] * R * s, z = cx[i] * R * s + cz[i] * R * c;
		p[i] = {foot.x + x, foot.y - 0.1f, foot.z + z};
		if (!project(p[i], v[i]))
			return;
	}
	// The client sorts see-through polygons by their first two corners: start
	// at the far edge, so the circle is drawn before (under) the character.
	int k = 0;
	float best = 1e30f;
	for (int i = 0; i < 4; i++) {
		float d = v[i].rhw + v[(i + 1) & 3].rhw;
		if (d < best) {
			best = d;
			k = i;
		}
	}
	const int order[4] = {k, (k + 1) & 3, (k + 3) & 3, (k + 2) & 3};
	TLV out[4];
	for (int i = 0; i < 4; i++) {
		out[i] = v[order[i]];
		out[i].z -= 0.00003f; // the client's own ground cells lift themselves the same way
		out[i].color = grey(230);
		out[i].specular = 0xff000000u;
		out[i].u = cu[order[i]];
		out[i].v = cv[order[i]];
	}
	emit(out, tex);
}

// part 2: three rings round the player. Each one starts at the inner edge of
// the ground circle, grows and fades, tilted, while its tilt turns round the
// player; the texture runs once round each ring.
void draw_rings(const V3& foot, void* tex, float t) {
	const float LIFE = 2.0f, R0 = 36 * g_scale, R1 = 62 * g_scale, BAND = 9 * g_scale;
	const float TILT[3] = {40, 25, 10};
	const int SEGS = 20;
	for (int ri = 0; ri < 3; ri++) {
		float u = fmodf(t / LIFE - ri / 3.0f + 3.0f, 1.0f);
		float alpha = 230 * fminf(1, u / 0.15f) * fmaxf(0, 1 - fmaxf(0, (u - 0.55f) / 0.35f));
		if (alpha < 2)
			continue;
		float yaw = 2 * PI * fmodf(t / LIFE + ri / 3.0f, 1.0f);
		float th = TILT[ri] * PI / 180;
		V3 d = {cosf(yaw), 0, sinf(yaw)};
		V3 e1 = {-d.z, 0, d.x};
		V3 e2 = {cosf(th) * d.x, sinf(th), cosf(th) * d.z};
		float r = R0 + (R1 - R0) * u;
		auto at = [&](float rr, float a) {
			if (rr < 0.5f * g_scale)
				rr = 0.5f * g_scale;
			float ca = cosf(a), sa = sinf(a);
			float x = rr * (ca * e1.x + sa * e2.x);
			float y = r * e2.y + rr * (sa * e2.y); // the lowest edge rests on the floor
			float z = rr * (ca * e1.z + sa * e2.z);
			return V3{foot.x + x, foot.y - y, foot.z + z}; // up is -y in the client
		};
		uint32_t col = grey(alpha);
		for (int k = 0; k < SEGS; k++) {
			float a0 = 2 * PI * k / SEGS - PI / 2, a1 = a0 + 2 * PI / SEGS;
			float u0 = (float)k / SEGS, u1 = (float)(k + 1) / SEGS;
			V3 p[4] = {at(r + BAND, a0), at(r + BAND, a1), at(r - BAND, a0), at(r - BAND, a1)};
			float uv[8] = {u0, 0, u1, 0, u0, 1, u1, 1};
			quad(p, uv, col, tex);
		}
	}
}

// part 3: small sparkles shooting out from the feet across the floor
void draw_sparkles(const V3& foot, uint32_t aid, void* tex, float t) {
	const float PERIOD = 4.0f, LIFE = 1.5f;
	const int COUNT = 10;
	float hpc = fabsf(*(float*)(g_renderer + 4)), vpc = fabsf(*(float*)(g_renderer + 8));
	for (int k = 0; k < COUNT; k++) {
		uint32_t seed = hash(aid * 31u + (uint32_t)k);
		float start = PERIOD * k / COUNT + rnd(seed, 0, 0.1f);
		float since = t - start;
		float cyc = floorf(since / PERIOD);
		float phase = since - cyc * PERIOD;
		if (phase > LIFE)
			continue;
		float u = phase / LIFE;
		uint32_t s = hash(seed ^ (uint32_t)(int)cyc);
		float a = rnd(s, 0, 2 * PI), dist = 95 * g_scale * rnd(s, 0.75f, 1.15f);
		float rise = 14 * g_scale * rnd(s, 0, 1), size = 10 * g_scale * rnd(s, 0.8f, 1.1f);
		float alpha = 255 * fminf(1, u / 0.12f) * (1 - fmaxf(0, (u - 0.7f) / 0.3f));
		V3 c = {foot.x + dist * u * cosf(a), foot.y - rise * u, foot.z + dist * u * sinf(a)};
		TLV m;
		if (!project(c, m) || alpha < 2)
			continue;
		// a flat picture facing the camera, as big as `size` world units would be there
		float hx = size * hpc * m.rhw, hy = size * vpc * m.rhw;
		const float dx[4] = {-hx, hx, -hx, hx}, dy[4] = {-hy, -hy, hy, hy};
		const float uv[8] = {0, 0, 1, 0, 0, 1, 1, 1};
		TLV v[4];
		for (int i = 0; i < 4; i++) {
			v[i] = m;
			v[i].x += dx[i];
			v[i].y += dy[i];
			v[i].color = grey(alpha);
			v[i].specular = 0xff000000u;
			v[i].u = uv[i * 2];
			v[i].v = uv[i * 2 + 1];
		}
		emit(v, tex);
	}
}

void draw_actor(const uint8_t* actor, const Aura& a, uint32_t aid, float t) {
	if (!readable((uintptr_t)actor, 0x114))
		return;
	V3 foot;
	memcpy(&foot, actor + 0x10, sizeof(foot));
	if (!(fabsf(foot.x) < 1e6f && fabsf(foot.y) < 1e6f && fabsf(foot.z) < 1e6f))
		return;
	void* tex;
	if (a.part[0] && (tex = texture(1, a.part[0])))
		draw_ground(foot, tex, t);
	if (a.part[1] && (tex = texture(2, a.part[1])))
		draw_rings(foot, tex, t);
	if (a.part[2] && (tex = texture(3, a.part[2])))
		draw_sparkles(foot, aid, tex, t);
}

int g_frame = -1;
std::unordered_map<uint32_t, Aura> g_frameAuras; // this frame's copy

} // namespace

void aura_init() {
	InitializeCriticalSection(&g_lock);
}

bool aura_install() {
	for (const Expect& e : EXPECT) {
		if (!readable(e.at, e.len) || memcmp((void*)e.at, e.bytes, e.len) != 0) {
			log_line("aura: this game exe is not supported (code at %08x differs), auras are off", (unsigned)e.at);
			return false;
		}
	}
	g_ok = true;
	return true;
}

bool aura_available() {
	return g_ok;
}

bool aura_wants() {
	return g_ok && g_cfg.auraEnabled && !g_texFailed;
}

void aura_set(const uint8_t* entries, int n) {
	Lock l(g_lock);
	g_auras.clear();
	for (int i = 0; i < n; i++) {
		const uint8_t* e = entries + i * 7;
		uint32_t aid;
		memcpy(&aid, e, 4);
		Aura a = {{e[4], e[5], e[6]}};
		if (a.part[0] || a.part[1] || a.part[2])
			g_auras[aid] = a;
	}
	g_aurasAt = GetTickCount();
	g_none = false;
}

void aura_set_none() {
	Lock l(g_lock);
	g_auras.clear();
	g_none = true;
}

// From Graymap's ground hook, during the world drawing (game thread).
void aura_draw(void* view) {
	if (!g_ok || !g_cfg.auraEnabled || !view)
		return;
	uint8_t* r = *(uint8_t**)RENDERER_PTR;
	if (!r)
		return;
	int frame = *(int*)(r + 0x38);
	if (frame == g_frame)
		return; // the ground is drawn in parts: once a frame is enough
	g_frame = frame;
	g_loadsThisFrame = 0;
	{
		Lock l(g_lock);
		// answers come about once a second; after 3 s without one they are stale
		if (GetTickCount() - g_aurasAt > 3000)
			g_auras.clear();
		g_frameAuras = g_auras;
	}
	if (g_frameAuras.empty())
		return;

	uint8_t* mm = (uint8_t*)MODE_MGR;
	if (*(int*)(mm + 0x58) != 1)
		return; // not in the game itself
	uint8_t* mode = *(uint8_t**)(mm + 4);
	if (!mode || !readable((uintptr_t)mode + 0xcc, 4))
		return;
	uint8_t* world = *(uint8_t**)(mode + 0xcc);
	if (!world || !readable((uintptr_t)world, 0x34))
		return;

	g_renderer = r;
	g_view = view;
	g_scale = 0.25f * g_cfg.auraSize / 100.0f;
	float t = (float)(GetTickCount() % 3600000u) / 1000.0f;

	uint32_t self = *(uint32_t*)SESSION_AID;
	auto it = g_frameAuras.find(self);
	uint8_t* player = *(uint8_t**)(world + 0x2c);
	if (player && it != g_frameAuras.end() && g_cfg.auraOwn)
		draw_actor(player, it->second, self, t);

	if (!g_cfg.auraOthers)
		return;
	// std::list<CGameActor*>: head node at +0x10, size at +0x14; node = next, prev, value
	uint8_t* head = *(uint8_t**)(world + 0x10);
	int size = *(int*)(world + 0x14);
	if (!head || size < 0 || size > 5000 || !readable((uintptr_t)head, 12))
		return;
	uint8_t* node = *(uint8_t**)head;
	for (int i = 0; node && node != head && i <= size; i++) {
		if (!readable((uintptr_t)node, 12))
			break;
		uint8_t* actor = *(uint8_t**)(node + 8);
		if (actor && readable((uintptr_t)actor + 0x110, 4)) {
			uint32_t aid = *(uint32_t*)(actor + 0x110);
			auto a = g_frameAuras.find(aid);
			if (aid != self && a != g_frameAuras.end())
				draw_actor(actor, a->second, aid, t);
		}
		node = *(uint8_t**)node;
	}
}

AuraServer aura_server_state(int* count) {
	*count = 0;
	if (net_state() != NET_CONNECTED)
		return AURA_SERVER_NO_VOICE;
	Lock l(g_lock);
	if (g_none)
		return AURA_SERVER_UNSUPPORTED;
	if (GetTickCount() - g_aurasAt > 3000)
		return AURA_SERVER_WAITING;
	*count = (int)g_auras.size();
	return AURA_SERVER_ON;
}
