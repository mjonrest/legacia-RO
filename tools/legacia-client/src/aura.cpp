// Auras: the plugin draws the Gepard-style aura of every player around (part 1
// ground circle, part 2 rings, part 3 sparkles), since the Gepard client no
// longer draws them. The map-server keeps the aura (AURA_DATA, the Aura Master
// NPC) and sends the auras of the players around through the voice server;
// net.cpp asks about once a second.
//
// Drawing uses the client's own 3D pipeline, like Graymap: each piece is one
// of the client's polygons (projected with the client's own camera and added
// to its list of effect polygons), so the aura sits on the ground
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
#include <unordered_set>

namespace {

// ---- the Legacia client exe (2026-01-07) ----
// CRenderer::ProjectVertex(const vector3d* world, const matrix* view, tlvertex* out)
const uintptr_t PROJECT_VERTEX = 0x55c750;
// CRenderer::BorrowNullRP(): a 4-vertex polygon from the frame's pool
const uintptr_t BORROW_RP = 0x543060;
// CRenderer::AddRP(polygon, flags). Flags 1 alone put it in the see-through
// lists, which are drawn with SRCALPHA/INVSRCALPHA whatever the polygon says
// (our black backgrounds showed as black boxes). Flags 1|4 put it in the
// effect list (+0x1ec), drawn by 0x55a820 with each polygon's own blend
// (+0x18 src, +0x1c dst), like skill effects.
const int ADD_RP_EFFECT = 1 | 4;
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
struct RingLook {
	int style; // 0 full discs, 1 sweeping crescents, 2 thin dotted ring (a strip picture)
	float v0, v1; // the lit rows
	float tint[3] = {1, 1, 1}; // vertex colour: leans toward the picture's own colour
};

// ring colour: additive light of `a`, leaning toward the picture's colour so it reads
// as rich as Gepard's instead of washing out grey-white (owner 2026-11-16)
uint32_t tinted(float a, const RingLook& lk) {
	int c[3];
	for (int i = 0; i < 3; i++) {
		float v = a * lk.tint[i];
		c[i] = v < 0 ? 0 : v > 255 ? 255 : (int)(v + 0.5f);
	}
	return 0xff000000u | (uint32_t)c[0] << 16 | (uint32_t)c[1] << 8 | (uint32_t)c[2];
}
std::unordered_map<void*, RingLook> g_ringLook; // ring textures, worked out when loaded
std::unordered_set<void*> g_alphaTex; // textures with a real alpha (ground circles)
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
			if (tex && part == 1) {
				// ground circles are mixed by alpha (see emit): the brightness
				// becomes the alpha (a bit stronger, so the circle reads clearly)
				// and the colour is un-darkened to match
				for (uint32_t& c : px) {
					int r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
					int m = r > g ? (r > b ? r : b) : (g > b ? g : b);
					if (m == 0) {
						c = 0;
						continue;
					}
					int a = m * 3 / 2 > 255 ? 255 : m * 3 / 2;
					r = r * 255 / m, g = g * 255 / m, b = b * 255 / m;
					c = (uint32_t)a << 24 | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
				}
				g_alphaTex.insert(tex);
			}
			if (!tex) // the alpha isn't needed (drawn additively), so the format Graymap uses works too
				tex = ((CreateTextureFn)CREATE_TEXTURE)(dev, w, h, TEX_FORMAT_ARGB1555, 0, 0);
		}
		if (tex && part == 2) {
			// how Gepard shows this picture (owner's videos of #3, #5, #6): a thin strip
			// (#3) is a dotted ring; a picture that fades out at its left and right edges
			// (#6) sweeps round as crescents; anything else (#5) is a full disc
			int y0 = h, y1 = -1;
			std::vector<double> col(w, 0.0);
			double mean[3] = {0, 0, 0};
			for (int y = 0; y < h; y++)
				for (int x = 0; x < w; x++) {
					uint32_t c = px[(size_t)y * w + x];
					int r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
					int m = r > g ? (r > b ? r : b) : (g > b ? g : b);
					col[x] += m;
					if (m > 40) {
						y0 = y < y0 ? y : y0, y1 = y > y1 ? y : y1;
						mean[0] += r, mean[1] += g, mean[2] += b;
					}
				}
			RingLook lk = {0, 0, 1};
			double top = 0, ends = 0;
			for (int x = 0; x < w; x++)
				top = col[x] > top ? col[x] : top;
			int e = w / 16 > 1 ? w / 16 : 1;
			for (int x = 0; x < e; x++)
				ends += col[x] + col[w - 1 - x];
			ends /= 2 * e;
			if (y1 >= 0 && (y1 - y0 + 1) < h / 4)
				lk = {2, (float)y0 / h, (float)(y1 + 1) / h};
			else if (top > 0 && ends < 0.35 * top)
				lk.style = 1;
			// tint = (colour / its brightest channel) ^ 1.5, scaled to average 0.85
			double big = mean[0] > mean[1] ? (mean[0] > mean[2] ? mean[0] : mean[2]) : (mean[1] > mean[2] ? mean[1] : mean[2]);
			if (big > 0) {
				float tv[3], avg = 0;
				for (int i = 0; i < 3; i++)
					tv[i] = powf((float)(mean[i] / big), 1.5f), avg += tv[i] / 3;
				for (int i = 0; i < 3; i++)
					lk.tint[i] = avg > 0 ? tv[i] / avg * 0.85f : 1;
			}
			g_ringLook[tex] = lk;
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
// Blend: additive (D3DBLEND_ONE/ONE, black adds nothing) unless `blended`,
// which mixes by the texture's alpha (SRCALPHA/INVSRCALPHA) so the colours
// stay true on light floors instead of adding up to white.
// blend factors: 5 = SRCALPHA, 6 = INVSRCALPHA, 2 = ONE
void emit_blend(const TLV* v, void* tex, int src, int dst) {
	uint8_t* face = ((BorrowFn)BORROW_RP)(g_renderer);
	if (!face)
		return;
	TLV* out = *(TLV**)face;
	memcpy(out, v, sizeof(TLV) * 4);
	*(void**)(face + 8) = tex;
	*(int*)(face + 0x18) = src;
	*(int*)(face + 0x1c) = dst;
	((AddRpFn)ADD_RP)(g_renderer, face, ADD_RP_EFFECT);
}

void emit(const TLV* v, void* tex, bool blended = false) {
	emit_blend(v, tex, blended ? 5 : 2, blended ? 6 : 2);
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

bool quad(const V3 p[4], const float uv[8], uint32_t color, void* tex, float zbias = 0, bool blended = false) {
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
	emit(v, tex, blended);
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

// part 1: the ground circle, turning flat on the floor once every 6 s. Drawn
// additively, so it is dimmed a little: at full strength light floors washed it
// out to white.
const float GROUND_BRIGHTNESS = 165, GROUND_GLOW = 150;
void draw_ground(const V3& foot, void* tex, float t) {
	bool blended = g_alphaTex.count(tex) != 0;
	// about 3.7 cells across at its widest (owner: smaller, 2026-11-13), and it
	// slowly breathes: 82% -> 100% -> 82% every 3 s (a small change, user 2026-11-13)
	const float R = 37 * g_scale * (0.91f + 0.09f * sinf(2 * PI * t / 3.0f));
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
		out[i].color = blended ? 0xffffffffu : grey(GROUND_BRIGHTNESS);
		out[i].specular = 0xff000000u;
		out[i].u = cu[order[i]];
		out[i].v = cv[order[i]];
	}
	emit(out, tex, blended);
	// a second, added-light pass makes the circle glow brighter and whiter
	if (blended) {
		for (int i = 0; i < 4; i++)
			out[i].color = grey(GROUND_GLOW);
		emit_blend(out, tex, 5, 2); // light added, weighted by the circle's alpha
	}
}

// part 2: rings. Two families:
//  - flame / spike strips (ring_is_pillar): draw_pillar, a soft fire bloom round the body
//  - every other picture: draw_rings, matched side by side to the owner's Gepard videos
//    of rings #3, #5 and #6 (2026-11-16). Three discs ("petals") round the player, each
//    a wide ring with the picture wrapped twice round it (top row at the hole, bottom
//    row at the rim). They grow from small at the feet to full size in 1.3 s, the
//    flower turns round the player, each disc spins (one turn in 2 s), and they start
//    again every 5.7 s, one after the other. How a disc looks depends on the picture
//    (see RingLook): full discs with no hole (#5), sweeping crescents with an off-centre hole that
//    fade out on their thin side (#6), or a thin dotted ring of the picture's lit rows (#3).
// Sizes: g_scale * 4 = 1 world unit; a cell is 5 units.
void draw_rings(const V3& foot, void* tex, float t) {
	RingLook lk = {0, 0, 1};
	auto li = g_ringLook.find(tex);
	if (li != g_ringLook.end())
		lk = li->second;
	const int PETALS = 3, SEGS = 40;
	const float TILT = 32 * PI / 180;    // petals lean up, outward
	const float TURN = 6.0f, SPIN = 2.0f;
	const float CYCLE = 5.7f, GROW = 1.3f, FADE = 0.6f, STAGGER = 0.35f;
	// full discs (#5) float round the waist as three petals. Crescents (#6) and dotted
	// rings (#3) are centred on the player and wobble round the body at very different
	// tilts: sizes, heights and tilts were fitted to where Gepard's ring shows up over
	// the owner's whole clips (time-averaged coverage, 2026-11-16)
	float R = 48, hole = 0, ecc = 0, bright = 78, off = 16, arc = 0, lift = 16;
	float tilts[3] = {0.4f, 0.8f, 1.1f}; // times TILT
	float ringIn = 0.64f, ringOut = 0.8f; // dotted ring band, as part of R
	if (lk.style == 1) {
		R = 47.5f, hole = 0.6f, ecc = 0.25f, bright = 85, arc = -0.04f, off = 0, lift = 31.7f;
		tilts[0] = 16.5f / 32, tilts[1] = 58.7f / 32, tilts[2] = 74.7f / 32;
	} else if (lk.style == 2) {
		R = 40.9f, bright = 40, off = 0, lift = 20.6f;
		tilts[0] = 14.2f / 32, tilts[1] = 63.3f / 32, tilts[2] = 89.2f / 32;
	}
	R *= g_scale, off *= g_scale, lift *= g_scale;
	float turn = 2 * PI * t / TURN;
	for (int pi = 0; pi < PETALS; pi++) {
		// each petal's own life: grow from the feet, stay, fade, start again
		float ph = fmodf(t + pi * STAGGER, CYCLE);
		float g = ph < GROW ? ph / GROW : 1.0f;
		g = 1 - (1 - g) * (1 - g); // ease out
		float fade = ph > CYCLE - FADE ? (CYCLE - ph) / FADE : 1.0f;
		fade *= fminf(1.0f, ph / 0.15f);
		float size = 0.2f + 0.8f * g;
		float dir = turn + 2 * PI / 3 * pi;
		V3 c = {foot.x + off * g * cosf(dir), foot.y - lift * g, foot.z + off * g * sinf(dir)};
		V3 d = {cosf(dir), 0, sinf(dir)};
		V3 e1 = {-d.z, 0, d.x};
		float tilt = TILT * tilts[pi]; // not all the same, like Gepard
		V3 e2 = {cosf(tilt) * d.x, sinf(tilt), cosf(tilt) * d.z};
		// all discs spin the same way as the flower turns, like a fan (owner 2026-11-16);
		// the disc's own angle runs the other way round, hence the minus
		float spin = -(t / SPIN + pi / 3.0f);
		float r1 = R * size, r0 = r1 * hole, ex = r1 * ecc;
		float holeAt = 2 * PI * spin + pi; // where the off-centre hole leans (turns with the spin)
		auto at = [&](float rr, float ang) {
			float ca = cosf(ang), sa = sinf(ang);
			float x = rr * (ca * e1.x + sa * e2.x);
			float y = rr * (sa * e2.y);
			float z = rr * (ca * e1.z + sa * e2.z);
			return V3{c.x + x, c.y - y, c.z + z}; // up is -y in the client
		};
		for (int s = 0; s < SEGS; s++) {
			float a0 = 2 * PI * ((float)s / SEGS + spin), a1 = a0 + 2 * PI / SEGS;
			float w = 1;
			if (lk.style == 1) {
				// crescent: bright on the wide side, fading out on the thin side
				w = (-cosf((a0 + a1) / 2 - holeAt) + arc) / 0.8f;
				w = w < 0 ? 0 : w > 1 ? 1 : powf(w, 1.2f);
				if (w < 0.01f)
					continue;
			}
			// the picture goes round twice (each half of the segments is one copy)
			float uu = 2.0f * s / SEGS;
			float u0 = uu - floorf(uu), u1 = u0 + 2.0f / SEGS;
			float in0, in1, out0, out1, v0 = 0, v1 = 1;
			if (lk.style == 2) {
				in0 = in1 = r1 * ringIn, out0 = out1 = r1 * ringOut;
				v0 = lk.v0, v1 = lk.v1;
			} else {
				in0 = r0 + ex * cosf(a0 - holeAt), in1 = r0 + ex * cosf(a1 - holeAt);
				out0 = out1 = r1;
			}
			V3 p[4] = {at(in0, a0), at(in1, a1), at(out0, a0), at(out1, a1)};
			float uv[8] = {u0, v0, u1, v0, u0, v1, u1, v1};
			quad(p, uv, tinted(bright * fade * w, lk), tex);
		}
	}
}

// #6-style rings (pictures that fade at their left and right edges): like the other
// rings they come up from the feet and circle the player, but as a fan of three
// curved blades (owner 2026-11-16, compared with the Gepard #6 video). The fan lies
// almost flat, grows from small at the feet while rising to chest height, and spins.
// Each blade is thick near its head and tapers to a point, and its tail curls inward
// like a spiral arm; the picture runs once along each blade.
void draw_blades(const V3& foot, void* tex, float t) {
	const int BLADES = 3, SEGS = 24, RS = 5;
	const float R = 40 * g_scale, LIFT = 40 * g_scale, TILT = 6 * PI / 180;
	const float TURN = 6.0f, SPIN = 2.1f;
	const float WIDTH = 0.3f, SPAN = 1.3f, CURL = 0.4f, HEAD = 0.7f, TWIST = 0.1f;
	const float CYCLE = 5.7f, GROW = 1.3f, FADE = 0.6f, BRIGHT = 60; // thin and soft, like Gepard's
	RingLook lk;
	auto li = g_ringLook.find(tex);
	if (li != g_ringLook.end())
		lk = li->second;
	float ph = fmodf(t, CYCLE);
	float g = ph < GROW ? ph / GROW : 1.0f;
	g = 1 - (1 - g) * (1 - g); // ease out
	float fade = ph > CYCLE - FADE ? (CYCLE - ph) / FADE : 1.0f;
	fade *= fminf(1.0f, ph / 0.15f);
	float r1 = R * (0.2f + 0.8f * g);
	V3 c = {foot.x, foot.y - LIFT * g * g, foot.z}; // up is -y in the client
	float dir = 2 * PI * t / TURN; // the slight tilt slowly goes round
	V3 d = {cosf(dir), 0, sinf(dir)};
	V3 e1 = {-d.z, 0, d.x};
	V3 e2 = {cosf(TILT) * d.x, sinf(TILT), cosf(TILT) * d.z};
	auto at = [&](float rr, float ang) {
		float ca = cosf(ang), sa = sinf(ang);
		float x = rr * (ca * e1.x + sa * e2.x);
		float y = rr * (sa * e2.y);
		float z = rr * (ca * e1.z + sa * e2.z);
		return V3{c.x + x, c.y - y, c.z + z};
	};
	float rot = 2 * PI * t / SPIN;
	uint32_t col = tinted(BRIGHT * fade, lk);
	for (int b = 0; b < BLADES; b++) {
		float base = rot + 2 * PI * b / BLADES, span = 2 * PI / BLADES * SPAN;
		for (int s = 0; s < SEGS; s++) {
			float f0 = (float)s / SEGS, f1 = (float)(s + 1) / SEGS;
			float a0 = base + span * f0, a1 = base + span * f1;
			float w0 = powf(sinf(PI * powf(f0, HEAD)), 0.8f), w1 = powf(sinf(PI * powf(f1, HEAD)), 0.8f);
			float o0 = r1 * (1 - CURL * f0), o1 = r1 * (1 - CURL * f1);
			float in0 = o0 * (1 - WIDTH * w0), in1 = o1 * (1 - WIDTH * w1);
			for (int j = 0; j < RS; j++) {
				float g0 = (float)j / RS, g1 = (float)(j + 1) / RS;
				V3 p[4] = {at(in0 + (o0 - in0) * g0, a0), at(in1 + (o1 - in1) * g0, a1),
				           at(in0 + (o0 - in0) * g1, a0), at(in1 + (o1 - in1) * g1, a1)};
				float tw0 = TWIST * g0, tw1 = TWIST * g1;
				float uv[8] = {f0 + tw0, g0, f1 + tw0, g0, f0 + tw1, g1, f1 + tw1, g1};
				quad(p, uv, col, tex);
			}
		}
	}
}

// #3-style rings (a thin strip picture): three slightly curved lines circling the
// player (owner 2026-11-16, in their words: "a circle with 3 lines coming slowly from
// the bottom of the player, then higher, moving round the player; thin at the bottom
// but bolder on the other side; curved, but only about 15 degrees"). Each line lies
// on a cylinder round the body, climbing at TILT, and the picture runs along it.
void draw_strokes(const V3& foot, void* tex, float t, const RingLook& lk) {
	const int LINES = 3, SEGS = 16;
	const float R = 30 * g_scale, L = 50 * g_scale, W = 6 * g_scale, RISE = 60 * g_scale;
	const float TILT = 30 * PI / 180, BEND = 15 * PI / 180, TAPER = 0.25f;
	const float CYCLE = 5.7f, ORBIT = 2.4f, BRIGHT = 170;
	float ph = fmodf(t, CYCLE) / CYCLE;
	float alpha = fminf(1.0f, ph / 0.12f) * fminf(1.0f, (1 - ph) / 0.2f);
	float h = RISE * ph; // the circle comes up slowly from the feet
	uint32_t col = tinted(BRIGHT * alpha, lk);
	for (int k = 0; k < LINES; k++) {
		float a = 2 * PI * t / ORBIT + 2 * PI * k / LINES;
		auto pt = [&](float f, float& up, V3& out) {
			float sl = (f - 0.5f) * L;
			float ang = sl * cosf(TILT) / R;
			up = h + sl * sinf(TILT) - (f - 0.5f) * (f - 0.5f) * L * sinf(BEND); // a slight curve
			out = V3{foot.x + R * cosf(a + ang), foot.y - up, foot.z + R * sinf(a + ang)};
		};
		for (int i = 0; i < SEGS; i++) {
			float f0 = (float)i / SEGS, f1 = (float)(i + 1) / SEGS;
			float up0, up1;
			V3 c0, c1;
			pt(f0, up0, c0), pt(f1, up1, c1);
			float w0 = W * (TAPER + (1 - TAPER) * f0), w1 = W * (TAPER + (1 - TAPER) * f1); // thin bottom, bold top
			V3 p[4] = {{c0.x, c0.y - w0, c0.z}, {c1.x, c1.y - w1, c1.z}, {c0.x, c0.y + w0, c0.z}, {c1.x, c1.y + w1, c1.z}};
			float u0 = f0, u1 = f1;
			float uv[8] = {u0, lk.v0, u1, lk.v0, u0, lk.v1, u1, lk.v1};
			quad(p, uv, col, tex);
		}
	}
}

// Ring textures that are flame / spike strips (bright at the bottom, fading
// up). Like a skill effect, these rise round the body as a glowing pillar
// instead of the flower rings, opening like a flower of fire (owner 2026-11-13, see draw_pillar).
bool ring_is_pillar(int num) {
	static const unsigned char list[] = {1, 9, 14, 15, 16, 18, 19, 22, 23, 24, 25, 29, 30, 34, 39, 43, 47,
		54, 59, 64, 68, 69, 70, 71, 72, 76, 78, 81, 82, 86, 96, 97, 98, 100, 103, 107, 113};
	for (unsigned char n : list)
		if (n == num)
			return true;
	return false;
}

// a flame bloom, matched to the owner's videos (2026-11-13/15): a ring of
// flame round the feet about as wide as the player that opens out like a bowl
// to about 3.5 player widths and rises to about head height. One flame swirls
// round the player (the flames themselves travel, their tips trailing behind), brighter patches travel the other way, and the
// tongues lick up and down at three speeds like fire. The whole bloom slowly
// breathes smaller and bigger, like the ground circle. Texture bottom at the
// feet. Checked in the offline preview before shipping.
void draw_pillar(const V3& foot, void* tex, float t) {
	const int SEGS = 36, REP = 1, ROWS = 4; // texture once round: a few broad soft tongues, not many spikes (owner: too pointy)
	const float R0 = 14 * g_scale, R1 = 36 * g_scale, H = 58 * g_scale;
	float grow = 0.9f + 0.1f * sinf(2 * PI * t / 3.0f); // breathes like the ground circle
	for (int layer = 0; layer < 1; layer++) { // one flame only, like the Gepard video (owner 2026-11-16)
		float dir = layer ? -1.0f : 1.0f;
		float open = 1.0f;
		float hl = layer ? 0.8f : 1.0f;
		float spin = 2 * PI * fmodf(t / (layer ? 4.2f : 3.0f), 1.0f) * dir;          // the flames travel round the player
		float arc = 2 * PI * fmodf(t / (layer ? 6.5f : 5.0f), 1.0f) * -dir + layer * 1.3f; // bright patches go the other way
		float bright = layer ? 120.0f : 190.0f;
		auto at = [&](float a, float f) {
			// slow, broad licks only (the faster 7/11-lobe ripples made it look spiky)
			float lick = 0.72f + 0.18f * sinf(2 * a + 2 * PI * t / 1.6f + layer * 2.0f) + 0.08f * sinf(3 * a - 2 * PI * t / 1.1f + layer);
			float h = H * grow * hl * f * lick;
			float ang = a + spin - 0.2f * f * f * dir; // tips trail a little behind the turn
			float r = grow * (R0 + (R1 * open - R0) * sqrtf(f) * (0.9f + 0.25f * (lick - 0.62f)));
			return V3{foot.x + r * cosf(ang), foot.y - h, foot.z + r * sinf(ang)}; // up is -y
		};
		for (int s = 0; s < SEGS; s++) {
			float a0 = 2 * PI * s / SEGS, a1 = a0 + 2 * PI / SEGS;
			// no dark gaps: the ring burns all the way round (owner 2026-11-16), only a little brighter in places
			float m = 0.82f + 0.18f * cosf(2 * ((a0 + a1) / 2 + spin - arc));
			m = m < 0 ? 0 : m > 1 ? 1 : m;
			if (m < 0.02f)
				continue;
			uint32_t col = grey(bright * m);
			int k = s % (SEGS / REP);
			float u0 = (float)k / (SEGS / REP), u1 = (float)(k + 1) / (SEGS / REP);
			for (int row = 0; row < ROWS; row++) {
				float f0 = (float)row / ROWS, f1 = (float)(row + 1) / ROWS; // 0 feet .. 1 top
				V3 p[4] = {at(a0, f1), at(a1, f1), at(a0, f0), at(a1, f0)};
				float uv[8] = {u0, 1 - f1, u1, 1 - f1, u0, 1 - f0, u1, 1 - f0};
				quad(p, uv, col, tex);
			}
		}
	}
}

// part 3: small sparkles shooting out from the feet across the floor
void draw_sparkles(const V3& foot, uint32_t aid, void* tex, float t) {
	const float PERIOD = 5.0f, LIFE = 3.4f;
	const int COUNT = 10; // about 6 on screen at once, drifting out slowly as in the Gepard recording
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
		float a = rnd(s, 0, 2 * PI), dist = 80 * g_scale * rnd(s, 0.75f, 1.15f);
		float rise = 14 * g_scale * rnd(s, 0, 1), size = 6.0f * g_scale * rnd(s, 0.8f, 1.1f);
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

// ---- replayed Gepard rings ----
// For rings the owner recorded in Gepard (#3, #5, #6), the plugin plays back the
// recording itself: each frame of the video with the floor taken away (only the
// light the aura adds) as plugins/auras/replay/<ring>/<frame>.bmp, one loop of the
// effect. info.txt: frames fps width height footX footY pixelsPerUnit. The frames
// are drawn flat on the screen at the player's feet, scaled with the camera zoom,
// and added like Gepard's light (owner's choice 2026-11-16: an exact copy).
struct ReplayInfo {
	bool ok = false;
	int frames = 0, w = 0, h = 0;
	float fps = 15, fx = 0, fy = 0, ppu = 2.49f;
	DWORD next_try = 0; // a missing folder is looked for again later (copied in while playing)
};
std::unordered_map<int, ReplayInfo> g_replay; // ring number -> info (ok false = none)

const ReplayInfo& replay_info(int ring) {
	auto it = g_replay.find(ring);
	if (it != g_replay.end() && (it->second.ok || GetTickCount() < it->second.next_try))
		return it->second;
	bool first = it == g_replay.end();
	ReplayInfo ri;
	char path[MAX_PATH];
	snprintf(path, sizeof(path), "%sreplay\\%03d\\info.txt", tex_folder().c_str(), ring);
	if (FILE* f = fopen(path, "r")) {
		ri.ok = fscanf(f, "%d %f %d %d %f %f %f", &ri.frames, &ri.fps, &ri.w, &ri.h, &ri.fx, &ri.fy, &ri.ppu) == 7 &&
		        ri.frames > 0 && ri.fps > 0 && ri.ppu > 0;
		fclose(f);
		if (ri.ok)
			log_line("aura: ring %d plays back %d recorded frames", ring, ri.frames);
		else
			log_line("aura: ring %d replay info.txt unreadable: %s", ring, path);
	} else if (first && (ring == 3 || ring == 5 || ring == 6)) {
		log_line("aura: ring %d has no recorded frames, looked for %s", ring, path);
	}
	if (!ri.ok)
		ri.next_try = GetTickCount() + 5000;
	return g_replay[ring] = ri;
}

void* replay_texture(int ring, int frame) {
	int key = 100000 + ring * 1000 + frame;
	auto it = g_tex.find(key);
	if (it != g_tex.end())
		return it->second;
	if (g_texFailed || g_loadsThisFrame >= 3)
		return nullptr;
	g_loadsThisFrame++;
	char path[MAX_PATH];
	snprintf(path, sizeof(path), "%sreplay\\%03d\\%03d.bmp", tex_folder().c_str(), ring, frame);
	std::vector<uint32_t> px;
	int w = 0, h = 0;
	void* tex = nullptr;
	if (read_bmp32(path, px, w, h)) {
		void* dev = ((GetDeviceFn)GET_DEVICE)();
		if (dev)
			tex = ((CreateTextureFn)CREATE_TEXTURE)(dev, w, h, TEX_FORMAT_ARGB8888, 0, 0);
		if (tex) {
			void** vt = *(void***)tex;
			((UpdateTextureFn)vt[8])(tex, 0, 0, w, h, px.data(), 0, 0);
		}
	} else {
		log_line("aura: missing or bad replay frame %s", path);
	}
	g_tex[key] = tex;
	return tex;
}

bool draw_replay(const V3& foot, int ring, float t) {
	const ReplayInfo& ri = replay_info(ring);
	if (!ri.ok)
		return false;
	// load every frame once (a few per game frame), so playback never stutters
	for (int i = 0; i < ri.frames && g_loadsThisFrame < 3; i++)
		replay_texture(ring, i);
	int frame = (int)(t * ri.fps) % ri.frames;
	void* tex = replay_texture(ring, frame);
	TLV m;
	if (!tex || !project(foot, m))
		return true;
	float hpc = fabsf(*(float*)(g_renderer + 4)), vpc = fabsf(*(float*)(g_renderer + 8));
	float sx = g_scale * hpc * m.rhw / ri.ppu, sy = g_scale * vpc * m.rhw / ri.ppu;
	float x0 = m.x - ri.fx * sx, x1 = m.x + (ri.w - ri.fx) * sx;
	float y0 = m.y - ri.fy * sy, y1 = m.y + (ri.h - ri.fy) * sy;
	const float xs[4] = {x0, x1, x0, x1}, ys[4] = {y0, y0, y1, y1};
	const float uv[8] = {0, 0, 1, 0, 0, 1, 1, 1};
	TLV v[4];
	for (int i = 0; i < 4; i++) {
		v[i] = m;
		v[i].x = xs[i];
		v[i].y = ys[i];
		v[i].color = 0xffffffffu;
		v[i].specular = 0xff000000u;
		v[i].u = uv[i * 2];
		v[i].v = uv[i * 2 + 1];
	}
	emit(v, tex); // additive, like Gepard
	return true;
}

// ---- Gepard swirl rings, measured from the owner's recordings (2026-11-16) ----
// Gepard turns each ring picture into two brush-stroke blades that spiral round the
// player on the screen, counter-clockwise: each blade starts thin near the body and
// swings outward to a thick, feathered tip. The numbers are in pixels of the recorded
// video (2.3435 px per aura unit, feet at y 197.65) and were fitted frame by frame
// against #3 (thin strip pictures), #5 (full pictures) and #6 (edge-fading pictures).
struct Swirl {
	int n;          // blades
	float cy;       // swirl centre, video y (feet at 197.65)
	float w;        // turn speed, degrees a second (counter-clockwise)
	float S;        // blade length, degrees
	float Rs, Rt;   // centre-line radius at the blade's tail and tip
	float W, W0;    // half-width at the tip and the tail
	float q, cut;   // width growth curve; how soon the tip ends
	float tip40;    // where a tip is at 40/15 s (phase)
	float us;       // how much of the picture's width one blade uses
	float tint[3];  // Gepard's colour on the picture (measured)
};
const Swirl SWIRL_FADE = {2, 158, 250, 190, 52, 90, 30, 5, 1.3f, 0.15f, 145, 1, {0.36f, 0.36f, 0.85f}};  // #6
const Swirl SWIRL_STRIP = {2, 165, 250, 170, 48, 68, 8, 3, 1, 0.1f, 60, 1, {0.36f, 0.36f, 0.85f}};      // #3
const Swirl SWIRL_FULL = {2, 150, 200, 300, 48, 72, 34, 20, 1, 0.1f, 5, 0.3f, {0.52f, 0.51f, 0.65f}};  // #5

void draw_swirl(const V3& foot, void* tex, float t, const Swirl& sw, float v0, float v1) {
	const float PPU = 2.3435f, FOOT_Y = 197.65f;
	const int SEGS = 40;
	TLV m;
	if (!project(foot, m))
		return;
	float hpc = fabsf(*(float*)(g_renderer + 4)), vpc = fabsf(*(float*)(g_renderer + 8));
	float sx = g_scale * hpc * m.rhw / PPU, sy = g_scale * vpc * m.rhw / PPU;
	float cxs = m.x, cys = m.y - (FOOT_Y - sw.cy) * sy;
	uint32_t col = 0xff000000u;
	for (int i = 0; i < 3; i++) {
		int c = (int)(sw.tint[i] * 255 + 0.5f);
		col |= (uint32_t)(c > 255 ? 255 : c) << (16 - 8 * i);
	}
	float phi = sw.tip40 - sw.w * 40 / 15.0f;
	for (int k = 0; k < sw.n; k++) {
		float tip = phi + sw.w * t + k * 360.0f / sw.n;
		TLV prev[2];
		for (int i = 0; i <= SEGS; i++) {
			float s = (float)i / SEGS; // 0 tail .. 1 tip
			float th = (tip - (1 - s) * sw.S) * PI / 180;
			float rc = sw.Rs + (sw.Rt - sw.Rs) * s;
			float end = fminf(1.0f, (1 - s) / sw.cut);
			float hw = sw.W0 + (sw.W - sw.W0) * powf(s, sw.q) * sqrtf(fmaxf(end, 0));
			if (hw < 0.5f)
				hw = 0.5f;
			TLV cur[2];
			for (int j = 0; j < 2; j++) {
				float r = j ? rc + hw : rc - hw;
				cur[j] = m;
				cur[j].x = cxs + r * cosf(th) * sx;
				cur[j].y = cys - r * sinf(th) * sy;
				cur[j].color = col;
				cur[j].specular = 0xff000000u;
				cur[j].u = s * sw.us;
				cur[j].v = j ? v1 : v0;
			}
			if (i) {
				TLV v[4] = {prev[0], cur[0], prev[1], cur[1]};
				emit(v, tex); // additive, like Gepard
			}
			prev[0] = cur[0], prev[1] = cur[1];
		}
	}
}

// #5-style rings (full pictures): Gepard draws three round discs ("bubbles"), each
// hinged near the player's feet, tilted up and turning round the player. Every
// 5.7 s they start again: one after another they come up from the feet and open.
// Fitted to the owner's #5 recording in 3D (80% overlap of the shapes); sizes are
// pixels of that video (2.3435 px per aura unit).
void draw_petals(const V3& foot, void* tex, float t) {
	const float PPU = 2.3435f;
	const float R = 90.5f, D0 = -49.2f, H0 = 34.7f, BETA = 26.5f * PI / 180;
	const float W = 219.9f, CYC = 5.733f, GAP = 0.45f, GROW = 1.0f, FADE = 0.25f;
	const float TINT[3] = {0.3f, 0.3f, 0.38f};
	const int NA = 32, NR = 5;
	float k2w = g_scale / PPU; // video pixels -> world
	// which way round looks counter-clockwise on this camera
	TLV o, ox, oz;
	V3 px = {foot.x + 10, foot.y, foot.z}, pz = {foot.x, foot.y, foot.z + 10};
	if (!project(foot, o) || !project(px, ox) || !project(pz, oz))
		return;
	float cross = (ox.x - o.x) * (oz.y - o.y) - (ox.y - o.y) * (oz.x - o.x);
	float dir = cross < 0 ? 1.0f : -1.0f;
	float tc = fmodf(t, CYC);
	for (int k = 0; k < 3; k++) {
		float age = tc - k * GAP;
		if (age < 0)
			continue;
		float g = fminf(1.0f, age / GROW);
		g = g * g * (3 - 2 * g);
		float alpha = fminf(1.0f, age / FADE) * fminf(1.0f, (CYC - tc) / FADE);
		if (alpha <= 0)
			continue;
		uint32_t col = 0xff000000u;
		for (int i = 0; i < 3; i++) {
			int c = (int)(TINT[i] * 255 * alpha + 0.5f);
			col |= (uint32_t)(c > 255 ? 255 : c) << (16 - 8 * i);
		}
		float ps = dir * (W * t + k * 120.0f) * PI / 180, b = BETA * g;
		float rx = cosf(ps), rz = sinf(ps); // out from the player
		float tx = -rz, tz = rx;            // along the ground, sideways
		float cOut = (D0 + R * cosf(b)) * k2w, cUp = (H0 * g + R * sinf(b)) * k2w;
		float e1o = cosf(b), e1u = sinf(b), r = R * k2w;
		auto at = [&](float rr, float a) {
			float ca = rr * cosf(a), sa = rr * sinf(a);
			float out = cOut + r * ca * e1o, up = cUp + r * ca * e1u;
			return V3{foot.x + out * rx + r * sa * tx, foot.y - up, foot.z + out * rz + r * sa * tz};
		};
		for (int j = 0; j < NR; j++) {
			float r0 = (float)j / NR, r1 = (float)(j + 1) / NR;
			for (int i = 0; i < NA; i++) {
				float a0 = 2 * PI * i / NA, a1 = 2 * PI * (i + 1) / NA;
				V3 q[4] = {at(r0, a0), at(r0, a1), at(r1, a0), at(r1, a1)};
				float u0 = (float)i / NA, u1 = (float)(i + 1) / NA;
				float uv[8] = {u0, r0, u1, r0, u0, r1, u1, r1};
				quad(q, uv, col, tex);
			}
		}
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
	if (a.part[1] && (tex = texture(2, a.part[1]))) {
		auto li = g_ringLook.find(tex);
		if (draw_replay(foot, a.part[1], t))
			; // a recorded Gepard ring
		else if (ring_is_pillar(a.part[1]))
			draw_pillar(foot, tex, t);
		else if (li != g_ringLook.end() && li->second.style == 1)
			draw_swirl(foot, tex, t, SWIRL_FADE, 0, 1);
		else if (li != g_ringLook.end() && li->second.style == 2)
			draw_swirl(foot, tex, t, SWIRL_STRIP, li->second.v0, li->second.v1);
		else
			draw_petals(foot, tex, t);
	}
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
// The server sends each aura as hat effects (pc_aura_apply in pc.cpp,
// aura_hateffect: yes): part n of type 1/2/3 is hat effect 1000/1300/1600 + n.
// The game keeps every actor's effects in a std::set<int> at +0x3fc
// (CGameActor; 0xcac080 adds to it and walks it to show or hide them all).
// Hat effects are stored there as hat ID + 0x9ac (ZC_HAT_EFFECT handler
// 0xd6bb50 -> actor message 0x37f -> 0xcb9ea4 adds 0x9ac), so the auras come
// with the normal game packets, for everyone in sight, and stay as long as
// the character wears them. No voice server needed.
const uintptr_t ACTOR_HAT_EFFECTS = 0x3fc;
const int HAT_EFFECT_OFFSET = 0x9ac, HAT_AURA_BASE = 1000, HAT_AURA_STEP = 300;

bool hat_aura(const uint8_t* actor, Aura& out) {
	out = Aura{};
	if (!readable((uintptr_t)actor + ACTOR_HAT_EFFECTS, 4))
		return false;
	// MSVC tree node: left +0, parent +4, right +8, isnil +0xd, value +0x10
	const uint8_t* head = *(const uint8_t* const*)(actor + ACTOR_HAT_EFFECTS);
	if (!head || !readable((uintptr_t)head, 0x14))
		return false;
	const uint8_t* n = *(const uint8_t* const*)head; // leftmost = begin
	bool any = false;
	for (int guard = 0; guard < 64 && n && n != head; guard++) {
		if (!readable((uintptr_t)n, 0x14) || n[0xd])
			break;
		int id = *(const int*)(n + 0x10) - HAT_EFFECT_OFFSET;
		int k = (id - HAT_AURA_BASE - 1) / HAT_AURA_STEP, num = id - HAT_AURA_BASE - k * HAT_AURA_STEP;
		if (id > HAT_AURA_BASE && k >= 0 && k < 3 && num >= 1 && num < HAT_AURA_STEP) {
			out.part[k] = (uint8_t)num;
			any = true;
		}
		// in-order successor
		const uint8_t* r = *(const uint8_t* const*)(n + 8);
		if (readable((uintptr_t)r, 0x14) && !r[0xd]) {
			n = r;
			for (int g2 = 0; g2 < 64; g2++) {
				const uint8_t* l = *(const uint8_t* const*)n;
				if (!readable((uintptr_t)l, 0x14) || l[0xd])
					break;
				n = l;
			}
		} else {
			const uint8_t* p = *(const uint8_t* const*)(n + 4);
			for (int g2 = 0; g2 < 64 && readable((uintptr_t)p, 0x14) && !p[0xd] && n == *(const uint8_t* const*)(p + 8); g2++) {
				n = p;
				p = *(const uint8_t* const*)(n + 4);
			}
			n = p;
		}
	}
	return any;
}

// For your own character the game handles ZC_HAT_EFFECT in its status code
// (0xd537a0, self branch) and keeps each hat effect in its own status list,
// a std::vector of 0x1c-byte entries at 0x14a0258 (begin) / 0x14a025c (end):
// +0 status 0x37f, +8 the hat ID. Right after the first login the effect does
// not reach the character's effect set until a map change, so for yourself
// the auras are read from this list.
const uintptr_t SELF_STATUS_BEGIN = 0x14a0258, SELF_STATUS_END = 0x14a025c;
const int STATUS_HAT_EFFECT = 0x37f;

bool self_aura(Aura& out) {
	out = Aura{};
	const uint8_t* b = *(const uint8_t* const*)SELF_STATUS_BEGIN;
	const uint8_t* e = *(const uint8_t* const*)SELF_STATUS_END;
	if (!b || e < b || (e - b) % 0x1c != 0 || (e - b) / 0x1c > 2000 || !readable((uintptr_t)b, (size_t)(e - b)))
		return false;
	bool any = false;
	for (const uint8_t* p = b; p < e; p += 0x1c) {
		if (*(const int*)p != STATUS_HAT_EFFECT)
			continue;
		int id = *(const int*)(p + 8);
		int k = (id - HAT_AURA_BASE - 1) / HAT_AURA_STEP, num = id - HAT_AURA_BASE - k * HAT_AURA_STEP;
		if (id > HAT_AURA_BASE && k >= 0 && k < 3 && num >= 1 && num < HAT_AURA_STEP) {
			out.part[k] = (uint8_t)num;
			any = true;
		}
	}
	return any;
}

// an actor's aura: from its hat effects, else from the voice server's list
bool actor_aura(const uint8_t* actor, uint32_t aid, Aura& out, bool self = false) {
	if (aid < 2000000 || aid >= 100000000)
		return false; // only players (account IDs); monsters and NPCs have other classes
	if (self && readable(SELF_STATUS_BEGIN, 8) && self_aura(out))
		return true; // the game's own list is current for you even right after login
	// Only the hat effects count. The voice server's old aura list is not used
	// any more: it could hold a stale aura (e.g. 153 118 36) and show that instead.
	return hat_aura(actor, out);
}

// The game drops its textures when it leaves the game for the character
// select (and on a device reset), which left the cached aura textures dead:
// the aura vanished after changing character. So the cache is emptied then
// and the textures are loaded again on the way back in.
bool g_dropTextures = false;

void aura_frame() {
	if (!g_ok)
		return;
	uint8_t* mm = (uint8_t*)MODE_MGR;
	if (*(int*)(mm + 0x58) != 1)
		g_dropTextures = true;
}

void aura_device_reset() {
	g_dropTextures = true;
}

void aura_draw(void* view) {
	if (!g_ok || !view)
		return;
	if (g_dropTextures) {
		g_dropTextures = false;
		if (!g_tex.empty())
			log_line("aura: left the game or device reset, reloading %d aura textures", (int)g_tex.size());
		g_tex.clear(); // the game freed them (char select / reset); not released here
		g_alphaTex.clear();
		g_ringLook.clear();
		g_texFailed = false;
	}
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
	g_scale = 0.25f;
	float t = (float)(GetTickCount() % 3600000u) / 1000.0f;

	uint32_t self = *(uint32_t*)SESSION_AID;
	uint8_t* player = *(uint8_t**)(world + 0x2c);
	Aura a;
	bool mine = player && actor_aura(player, self, a, true);
	{
		// log your own aura when it changes, to tell a missing packet from a drawing problem
		static uint32_t last = 0xffffffffu;
		uint32_t now = mine ? (uint32_t)a.part[0] | (uint32_t)a.part[1] << 8 | (uint32_t)a.part[2] << 16 : 0;
		if (now != last) {
			last = now;
			log_line("aura: yours is now %u %u %u", now & 255, (now >> 8) & 255, now >> 16);
		}
	}
	if (mine)
		draw_actor(player, a, self, t);

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
		if (actor && actor != player && readable((uintptr_t)actor + 0x110, 4)) {
			uint32_t aid = *(uint32_t*)(actor + 0x110);
			if (aid != self && actor_aura(actor, aid, a))
				draw_actor(actor, a, aid, t);
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
