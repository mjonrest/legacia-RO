// Graymap: draws the map as flat coloured cells (walkable, unwalkable,
// water, shootable wall) instead of the 3D ground, water and buildings.
//
// It hooks the client's own ground drawing, so it is tied to one client
// build: the addresses below are for the Legacia client exe (2026-01-07 Ragexe).
// Every address is checked against the expected code before anything is
// patched; on any other exe Graymap stays off and the rest of the plugin
// works as usual.
#include "voice.h"

#include <math.h>

namespace {

// ---- the Legacia client exe (2026-01-07) ----
// C3dGround15 vtable: slot 5 draws water, slot 6 draws the ground. Both take
// (const matrix* view, const RECT* area, int flag); area is in ground tiles,
// one tile being 2x2 cells.
const uintptr_t GROUND_VTABLE = 0x1154078;
const int GROUND_SLOT_WATER = 5, GROUND_SLOT_RENDER = 6;
const uintptr_t GROUND_WATER = 0xac03b0, GROUND_RENDER = 0xabffb0;
// C3dNodeV1/V2 vtables, slot 3 draws one node of a map model (4 arguments)
const uintptr_t NODE_V1_VTABLE = 0x1153f38, NODE_V2_VTABLE = 0x1153f4c;
const int NODE_SLOT_RENDER = 3;
const uintptr_t NODE_V1_RENDER = 0xab6750, NODE_V2_RENDER = 0xab6ec0;
// C3dGround::RenderAttrTile(view, x, y, argb, texture, uv[4]): the client's
// own "draw one cell" used for the ground cursor and skill target cells
const uintptr_t RENDER_ATTR_TILE = 0xac0130;
const uintptr_t UV_FULL = 0x1341a00; // {0,0, 1,0, 0,1, 1,1}
// device wrapper (returned by GET_DEVICE): CreateTexture(w, h, format, 0, 0),
// format 0 = D3DFMT_A1R5G5B5 (1-bit alpha, like the game's own .tga cursors);
// texture vtable +0x20: Update(x, y, w, h, argb pixels, 0, 0)
const uintptr_t GET_DEVICE = 0x55c440, CREATE_TEXTURE = 0x569040;
// renderer (frame counter at +0x38) and the mode manager, whose game mode
// holds the world (+0xcc); the world holds the cell attributes (+0x30)
const uintptr_t RENDERER_PTR = 0x1382fa8, MODE_MGR = 0x1343108;

struct Expect {
	uintptr_t at;
	const char* bytes;
	int len;
};
// first bytes of the functions called or replaced
const Expect EXPECT[] = {
	{RENDER_ATTR_TILE, "\x55\x8b\xec\x83\xec\x38", 6},
	{GROUND_RENDER, "\x55\x8b\xec\x83\xec\x4c", 6},
	{GROUND_WATER, "\x55\x8b\xec\x81\xec\xa0\x00\x00\x00", 9},
	{CREATE_TEXTURE, "\x55\x8b\xec\x8b\x09\x85\xc9", 7},
	{GET_DEVICE, "\x55\x8b\xec\x6a\xff\x68\x71\x7c\xeb\x00", 10},
};

// C3dAttr cell: four corner heights and the cell type
struct AttrCell {
	float h[4];
	uint32_t flag;
};

typedef void(__thiscall* AreaRenderFn)(void* self, void* view, int* area, int flag);
typedef void(__thiscall* NodeRenderFn)(void* self, void* a, void* b, void* c, void* d);
typedef void(__thiscall* TileFn)(void* ground, void* view, int x, int y, uint32_t argb, void* tex, void* uv);
typedef void*(__cdecl* GetDeviceFn)();
typedef void*(__thiscall* CreateTextureFn)(void* dev, int w, int h, int format, int a, int b);
typedef void(__thiscall* UpdateTextureFn)(void* tex, int x, int y, int w, int h, const uint32_t* px, int a, int b);

bool g_ok;
AreaRenderFn g_origGround, g_origWater;
NodeRenderFn g_origNodeV1, g_origNodeV2;
// one texture per cell kind, with the cell colour inside and the grid
// colour on its edges, so each cell is a single draw (see make_textures)
enum CellKind { KIND_WALK, KIND_BLOCK, KIND_WATER, KIND_WALL, KIND_COUNT };
void* g_cellTex[KIND_COUNT];
int g_texColor[KIND_COUNT], g_texGrid = -1;
bool g_texFailed;
// frame each cell was last drawn in, so a cell drawn twice in a frame
// (overlapping areas) isn't stacked
std::vector<int> g_stamp;
const void* g_stampAttr;

bool readable(uintptr_t at, int len) {
	MEMORY_BASIC_INFORMATION mi;
	if (!VirtualQuery((void*)at, &mi, sizeof(mi)) || mi.State != MEM_COMMIT)
		return false;
	return at + len <= (uintptr_t)mi.BaseAddress + mi.RegionSize;
}

bool patch_slot(uintptr_t vtable, int slot, uintptr_t expect, void* hook, void** orig) {
	uintptr_t* p = (uintptr_t*)vtable + slot;
	if (!readable((uintptr_t)p, 4) || *p != expect)
		return false;
	DWORD old;
	if (!VirtualProtect(p, 4, PAGE_READWRITE, &old))
		return false;
	*orig = (void*)*p;
	*p = (uintptr_t)hook;
	VirtualProtect(p, 4, old, &old);
	return true;
}

void* world_attr() {
	uint8_t* mm = (uint8_t*)MODE_MGR;
	if (*(int*)(mm + 0x58) != 1)
		return nullptr; // not in the game itself (login, char select)
	uint8_t* mode = *(uint8_t**)(mm + 4);
	if (!mode)
		return nullptr;
	uint8_t* world = *(uint8_t**)(mode + 0xcc);
	return world ? *(void**)(world + 0x30) : nullptr;
}

const int TEX = 32; // texels per cell side: a cell is ~20 screen pixels, so the line stays about 1 pixel

void* make_texture(void* dev) {
	void* tex = ((CreateTextureFn)CREATE_TEXTURE)(dev, TEX, TEX, 0, 0, 0);
	for (int& c : g_texColor)
		c = -1; // filled in by update_textures
	return tex;
}

int kind_color(int kind) {
	switch (kind) {
	case KIND_BLOCK: return g_cfg.gmColor[GM_UNWALKABLE];
	case KIND_WATER: return g_cfg.gmColor[GM_WATER];
	case KIND_WALL: return g_cfg.gmColor[GM_WALL];
	}
	return g_cfg.gmColor[GM_WALKABLE];
}

// Each cell kind gets its own texture: the cell colour inside and a
// one-texel grid line on all four edges (none on unwalkable cells). Drawing
// the grid as a second, see-through layer didn't work: the game sorts its
// queued faces, and the grid layer sat at exactly the same depth as the
// cell, so it went under the cells in places that changed as the camera
// moved. Baking the line into the cell texture needs no second layer. The
// line is on all four edges because the texture is filtered: edge pixels
// sample between the first and last texel, so both must be line.
void update_textures() {
	int grid = g_cfg.gmColor[GM_GRID] & 0xffffff;
	for (int k = 0; k < KIND_COUNT; k++) {
		int color = kind_color(k) & 0xffffff;
		if (!g_cellTex[k] || (color == g_texColor[k] && grid == g_texGrid))
			continue;
		uint32_t px[TEX * TEX];
		for (int y = 0; y < TEX; y++)
			for (int x = 0; x < TEX; x++) {
				bool edge = k != KIND_BLOCK && (x == 0 || y == 0 || x == TEX - 1 || y == TEX - 1);
				px[y * TEX + x] = 0xff000000u | (uint32_t)(edge ? grid : color);
			}
		void** vt = *(void***)g_cellTex[k];
		((UpdateTextureFn)vt[8])(g_cellTex[k], 0, 0, TEX, TEX, px, 0, 0);
		g_texColor[k] = color;
	}
	g_texGrid = grid;
}

void make_textures() {
	if (g_cellTex[0] || g_texFailed)
		return;
	void* dev = ((GetDeviceFn)GET_DEVICE)();
	bool ok = dev != nullptr;
	for (int k = 0; k < KIND_COUNT && ok; k++)
		ok = (g_cellTex[k] = make_texture(dev)) != nullptr;
	if (!ok) {
		g_cellTex[0] = nullptr;
		g_texFailed = true;
		log_line("graymap: could not create the cell textures");
	} else {
		log_line("graymap: textures ready");
	}
}

bool active() {
	return g_ok && g_cfg.gmEnabled;
}

int cell_kind(uint32_t flag) {
	switch (flag & 0xff) {
	case 1: case 6: return KIND_BLOCK;
	case 3: return KIND_WATER;
	case 5: return KIND_WALL;
	}
	return KIND_WALK;
}

// ---------------------------------------------------------------- LGP
//
// Coloured cells on the ground around the player: a square and a circle
// (ring or filled, with waves), cells you can't walk on, and the skill cells
// the map-server sends. They're drawn with the same "draw one cell" function
// as Graymap, so they lie on the ground and work with Graymap on or off.
// With Graymap on, an LGP cell takes the place of the Graymap cell (two cells
// at the same depth would fight, see update_textures).
//
// Where the player is: the game's camera always looks at the player, so the
// point on the ground at the middle of the view is where they stand. That
// comes from the view matrix the ground is drawn with, so no player data in
// the exe is read, and it glides with the camera.

enum LgpColor { LC_SQUARE, LC_CIRCLE, LC_DEAD, LC_HIT, LC_SAFE, LC_COUNT };
const int LGP_LEVELS = 8;            // brightness steps, for the waves
const int LGP_R = 20, LGP_N = 2 * LGP_R + 1; // cells handled around the player
const int LC_DEAD_COLOR = 0xb02a2a, LC_HIT_COLOR = 0xff5a3c, LC_SAFE_COLOR = 0x3c8cff;

void* g_lgpTex[LC_COUNT][LGP_LEVELS];
long long g_lgpTexStyle[LC_COUNT]; // what each colour's textures were made for
int g_lgpTexColor[LC_COUNT];
bool g_lgpTexFailed;
void* g_lgpGrid[LGP_N * LGP_N];  // texture of each cell around the player this frame, or null
int g_lgpStampGrid[LGP_N * LGP_N];
int g_lgpX0, g_lgpY0;            // map cell of g_lgpGrid[0]
int g_lgpFrame = -1;             // frame g_lgpGrid was made in
bool g_lgpAny;                   // the grid has something in it
DWORD g_lgpDrewAt;

int lgp_color_of(int c) {
	switch (c) {
	case LC_SQUARE: return g_cfg.lgpSquare.color;
	case LC_CIRCLE: return g_cfg.lgpCircle.color;
	case LC_DEAD: return LC_DEAD_COLOR;
	case LC_HIT: return LC_HIT_COLOR;
	}
	return LC_SAFE_COLOR;
}

uint32_t shade(int rgb, float k) {
	int r = (int)(((rgb >> 16) & 255) * k), g = (int)(((rgb >> 8) & 255) * k), b = (int)((rgb & 255) * k);
	r = r > 255 ? 255 : r;
	g = g > 255 ? 255 : g;
	b = b > 255 ? 255 : b;
	return 0xff000000u | (uint32_t)(r << 16) | (uint32_t)(g << 8) | (uint32_t)b;
}

// One LGP cell picture. Box or round, flat or raised, with or without a gap
// to the next cell. With Graymap on, the gap shows the Graymap cell (bg and
// its grid line), otherwise it's see-through and the ground shows.
void lgp_pixels(uint32_t* px, int rgb, float bright, bool graymapBg) {
	const bool round = g_cfg.lgpTile == 1, raised = g_cfg.lgpDepth == 1, gutter = g_cfg.lgpGutter != 0;
	const uint32_t bg = graymapBg ? 0xff000000u | (uint32_t)(kind_color(KIND_WALK) & 0xffffff) : 0;
	const uint32_t grid = graymapBg ? 0xff000000u | (uint32_t)(g_cfg.gmColor[GM_GRID] & 0xffffff) : 0;
	const float c = (TEX - 1) / 2.0f;
	for (int y = 0; y < TEX; y++) {
		for (int x = 0; x < TEX; x++) {
			uint32_t out;
			bool edge = x == 0 || y == 0 || x == TEX - 1 || y == TEX - 1;
			if (round) {
				float dx = (x - c) / c, dy = (y - c) / c;
				float r = sqrtf(dx * dx + dy * dy), rmax = gutter ? 0.80f : 0.97f;
				if (r > rmax) {
					out = edge ? grid : bg;
				} else if (raised) {
					// a dome lit from the top left
					float nx = dx / rmax, ny = dy / rmax, nz = sqrtf(fmaxf(0.0f, 1 - nx * nx - ny * ny));
					float lum = 0.55f + 0.5f * (-0.45f * nx - 0.55f * ny + 0.70f * nz);
					float spec = powf(fmaxf(0.0f, -0.45f * nx - 0.55f * ny + 0.70f * nz), 18.0f) * 0.6f;
					out = shade(rgb, bright * lum + spec);
				} else {
					out = shade(rgb, bright * (r > rmax - 0.12f ? 0.72f : 1.0f));
				}
			} else {
				int m = gutter ? 2 : 0; // gap in texels
				int lo = m, hi = TEX - 1 - m;
				if (x < lo || y < lo || x > hi || y > hi) {
					out = edge ? grid : bg;
				} else if (raised) {
					int b = 3; // bevel width
					float k = 1.0f - 0.18f * (float)(y - lo) / (hi - lo); // face a bit darker at the bottom
					if (x - lo < b && x - lo <= y - lo && x - lo <= hi - y)
						k = 1.30f; // left
					else if (y - lo < b && y - lo <= hi - x)
						k = 1.45f; // top
					else if (hi - x < b && hi - x <= hi - y)
						k = 0.62f; // right
					else if (hi - y < b)
						k = 0.50f; // bottom
					out = shade(rgb, bright * k);
				} else {
					bool rim = x == lo || y == lo || x == hi || y == hi;
					out = shade(rgb, bright * (rim && !gutter ? 0.70f : 1.0f));
				}
			}
			px[y * TEX + x] = out;
		}
	}
}

bool lgp_textures(bool graymapBg) {
	if (g_lgpTexFailed)
		return false;
	void* dev = nullptr;
	long long style = (long long)g_cfg.lgpTile | (long long)g_cfg.lgpDepth << 1 | (long long)(g_cfg.lgpGutter != 0) << 2 |
	                  (long long)graymapBg << 3;
	if (graymapBg)
		style |= (long long)(kind_color(KIND_WALK) & 0xffffff) << 4 | (long long)(g_cfg.gmColor[GM_GRID] & 0xffffff) << 28;
	for (int c = 0; c < LC_COUNT; c++) {
		int color = lgp_color_of(c) & 0xffffff;
		if (g_lgpTex[c][0] && g_lgpTexStyle[c] == style && g_lgpTexColor[c] == color)
			continue;
		static uint32_t px[TEX * TEX];
		for (int l = 0; l < LGP_LEVELS; l++) {
			if (!g_lgpTex[c][l]) {
				if (!dev)
					dev = ((GetDeviceFn)GET_DEVICE)();
				if (!dev || !(g_lgpTex[c][l] = ((CreateTextureFn)CREATE_TEXTURE)(dev, TEX, TEX, 0, 0, 0))) {
					g_lgpTexFailed = true;
					log_line("lgp: could not create the cell textures");
					return false;
				}
			}
			lgp_pixels(px, lgp_color_of(c), 0.40f + 0.60f * l / (LGP_LEVELS - 1), graymapBg);
			void** vt = *(void***)g_lgpTex[c][l];
			((UpdateTextureFn)vt[8])(g_lgpTex[c][l], 0, 0, TEX, TEX, px, 0, 0);
		}
		g_lgpTexStyle[c] = style;
		g_lgpTexColor[c] = color;
	}
	return true;
}

// the ground point at the middle of the view, in map cells (fractional)
bool lgp_center(void* ground, const float* m, const uint8_t* attr, float* fx, float* fy) {
	if (!readable((uintptr_t)m, 48) || !readable((uintptr_t)ground, 0x14))
		return false;
	int wt = *(int*)((uint8_t*)ground + 8), ht = *(int*)((uint8_t*)ground + 0xc);
	float zoom = *(float*)((uint8_t*)ground + 0x10);
	int w = *(int*)(attr + 0x110), h = *(int*)(attr + 0x114);
	const AttrCell* cells = *(AttrCell* const*)(attr + 0x11c);
	if (wt <= 0 || ht <= 0 || !(zoom > 0.1f && zoom < 1000.0f))
		return false;
	// view: camera = A * world + t, A's rows are (m0 m3 m6), (m1 m4 m7), (m2 m5 m8), t = (m9 m10 m11)
	double a[3][3] = {{m[0], m[3], m[6]}, {m[1], m[4], m[7]}, {m[2], m[5], m[8]}};
	double det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
	             a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
	if (fabs(det) < 1e-12)
		return false;
	double inv[3][3];
	inv[0][0] = (a[1][1] * a[2][2] - a[1][2] * a[2][1]) / det;
	inv[0][1] = (a[0][2] * a[2][1] - a[0][1] * a[2][2]) / det;
	inv[0][2] = (a[0][1] * a[1][2] - a[0][2] * a[1][1]) / det;
	inv[1][0] = (a[1][2] * a[2][0] - a[1][0] * a[2][2]) / det;
	inv[1][1] = (a[0][0] * a[2][2] - a[0][2] * a[2][0]) / det;
	inv[1][2] = (a[0][2] * a[1][0] - a[0][0] * a[1][2]) / det;
	inv[2][0] = (a[1][0] * a[2][1] - a[1][1] * a[2][0]) / det;
	inv[2][1] = (a[0][1] * a[2][0] - a[0][0] * a[2][1]) / det;
	inv[2][2] = (a[0][0] * a[1][1] - a[0][1] * a[1][0]) / det;
	double t[3] = {m[9], m[10], m[11]};
	double cam[3], dir[3]; // camera position and viewing direction, in the world
	for (int i = 0; i < 3; i++) {
		cam[i] = -(inv[i][0] * t[0] + inv[i][1] * t[1] + inv[i][2] * t[2]);
		dir[i] = inv[i][2];
	}
	if (fabs(dir[1]) < 1e-9)
		return false;
	// meet the ground: start at height 0, then use the height of the cell hit
	double gy = 0, cx = 0, cy = 0;
	for (int it = 0; it < 4; it++) {
		double s = (gy - cam[1]) / dir[1];
		double X = cam[0] + s * dir[0], Z = cam[2] + s * dir[2];
		cx = 2.0 * (X / zoom + wt / 2);
		cy = 2.0 * (Z / zoom + ht / 2);
		int ix = (int)floor(cx), iy = (int)floor(cy);
		if (ix < 0 || iy < 0 || ix >= w || iy >= h)
			return false;
		const AttrCell& cell = cells[iy * w + ix];
		gy = (cell.h[0] + cell.h[1] + cell.h[2] + cell.h[3]) / 4;
	}
	*fx = (float)cx;
	*fy = (float)cy;
	return true;
}

// brightness step for a shape cell at distance d from the middle
int lgp_level(const Config::LgpShape& s, float d, float now) {
	if (s.wave == 0)
		return LGP_LEVELS - 1;
	float len = 7.0f - s.wave;          // 1 = long slow swells, 5 = quick ripples
	float speed = 1.0f + 0.9f * s.wave; // cells a second, outwards
	float wave = 0.5f + 0.5f * sinf(6.2831853f * (d - now * speed) / len);
	float v = 0.45f + 0.55f * wave;
	int l = (int)(v * (LGP_LEVELS - 1) + 0.5f);
	return l < 0 ? 0 : l >= LGP_LEVELS ? LGP_LEVELS - 1 : l;
}

// works out this frame's LGP cells (once per frame, the ground draws in parts)
void lgp_build(void* ground, void* view, const uint8_t* attr, int frame, bool graymapBg) {
	if (g_lgpFrame == frame)
		return;
	g_lgpFrame = frame;
	g_lgpAny = false;
	if (!g_cfg.lgpEnabled || !lgp_textures(graymapBg))
		return;
	float fx, fy;
	if (!lgp_center(ground, (const float*)view, attr, &fx, &fy))
		return;
	if (!g_cfg.lgpSmooth) {
		fx = floorf(fx) + 0.5f;
		fy = floorf(fy) + 0.5f;
	}
	int w = *(int*)(attr + 0x110), h = *(int*)(attr + 0x114);
	const AttrCell* cells = *(AttrCell* const*)(attr + 0x11c);
	g_lgpX0 = (int)floorf(fx) - LGP_R;
	g_lgpY0 = (int)floorf(fy) - LGP_R;
	memset(g_lgpGrid, 0, sizeof(g_lgpGrid));

	const Config::LgpShape &sq = g_cfg.lgpSquare, &ci = g_cfg.lgpCircle;
	float reach = 0;
	if (sq.on)
		reach = fmaxf(reach, sq.size * 1.42f);
	if (ci.on)
		reach = fmaxf(reach, (float)ci.size);
	float now = (GetTickCount() % 3600000) / 1000.0f;

	for (int gy = 0; gy < LGP_N; gy++) {
		for (int gx = 0; gx < LGP_N; gx++) {
			int x = g_lgpX0 + gx, y = g_lgpY0 + gy;
			if (x < 0 || y < 0 || x >= w || y >= h)
				continue;
			float dx = x + 0.5f - fx, dy = y + 0.5f - fy;
			float d = sqrtf(dx * dx + dy * dy), cheb = fmaxf(fabsf(dx), fabsf(dy));
			bool blocked = cell_kind(cells[y * w + x].flag) == KIND_BLOCK;
			void* tex = nullptr;
			if (blocked) {
				if (g_cfg.lgpDead && d <= reach + 1.0f)
					tex = g_lgpTex[LC_DEAD][LGP_LEVELS - 1];
			} else if (sq.on && (sq.fill ? cheb < sq.size + 0.5f : fabsf(cheb - sq.size) < 0.5f)) {
				tex = g_lgpTex[LC_SQUARE][lgp_level(sq, cheb, now)];
			} else if (ci.on && (ci.fill ? d < ci.size + 0.5f : fabsf(d - ci.size) < 0.55f)) {
				tex = g_lgpTex[LC_CIRCLE][lgp_level(ci, d, now)];
			}
			if (tex) {
				g_lgpGrid[gy * LGP_N + gx] = tex;
				g_lgpAny = true;
			}
		}
	}
	// skill cells on top of everything, pulsing slowly
	if (g_cfg.lgpAoe) {
		int pulse = 5 + (int)(2.0f * sinf(now * 5.0f) + 0.5f);
		pulse = pulse >= LGP_LEVELS ? LGP_LEVELS - 1 : pulse;
		for (const LgpCell& c : lgp_cells()) {
			int gx = c.x - g_lgpX0, gy = c.y - g_lgpY0;
			if (gx < 0 || gy < 0 || gx >= LGP_N || gy >= LGP_N)
				continue;
			g_lgpGrid[gy * LGP_N + gx] = g_lgpTex[c.kind ? LC_HIT : LC_SAFE][pulse];
			g_lgpAny = true;
		}
	}
	if (g_lgpAny)
		g_lgpDrewAt = GetTickCount();
}

void* lgp_tex_at(int x, int y) {
	if (!g_lgpAny)
		return nullptr;
	int gx = x - g_lgpX0, gy = y - g_lgpY0;
	if (gx < 0 || gy < 0 || gx >= LGP_N || gy >= LGP_N)
		return nullptr;
	return g_lgpGrid[gy * LGP_N + gx];
}

// Graymap off: the LGP cells on top of the game's own ground
void lgp_draw_alone(void* ground, void* view, const int* area) {
	uint8_t* attr = (uint8_t*)world_attr();
	if (!attr || !g_cfg.lgpEnabled)
		return;
	int frame = *(int*)(*(uint8_t**)RENDERER_PTR + 0x38);
	lgp_build(ground, view, attr, frame, false);
	if (!g_lgpAny)
		return;
	TileFn tile = (TileFn)RENDER_ATTR_TILE;
	int x0 = area[0] * 2, y0 = area[1] * 2, x1 = area[2] * 2 + 1, y1 = area[3] * 2 + 1;
	for (int gy = 0; gy < LGP_N; gy++) {
		for (int gx = 0; gx < LGP_N; gx++) {
			int i = gy * LGP_N + gx, x = g_lgpX0 + gx, y = g_lgpY0 + gy;
			if (!g_lgpGrid[i] || x < x0 || x > x1 || y < y0 || y > y1 || g_lgpStampGrid[i] == frame)
				continue;
			g_lgpStampGrid[i] = frame;
			tile(ground, view, x, y, 0xffffffff, g_lgpGrid[i], (void*)UV_FULL);
		}
	}
}

void draw_cells(void* ground, void* view, const int* area) {
	make_textures();
	uint8_t* attr = (uint8_t*)world_attr();
	if (!g_cellTex[0] || !attr)
		return;
	int w = *(int*)(attr + 0x110), h = *(int*)(attr + 0x114);
	AttrCell* cells = *(AttrCell**)(attr + 0x11c);
	if (w <= 0 || h <= 0 || !cells)
		return;
	if (g_stampAttr != attr || (int)g_stamp.size() != w * h) {
		g_stamp.assign(w * h, -1);
		g_stampAttr = attr;
		log_line("graymap: map %dx%d, drawing tiles %d,%d to %d,%d", w, h, area[0], area[1], area[2], area[3]);
	}
	int frame = *(int*)(*(uint8_t**)RENDERER_PTR + 0x38);
	update_textures();
	lgp_build(ground, view, attr, frame, true);
	TileFn tile = (TileFn)RENDER_ATTR_TILE;
	// area is inclusive, in ground tiles of 2x2 cells
	int x0 = area[0] * 2, y0 = area[1] * 2, x1 = area[2] * 2 + 1, y1 = area[3] * 2 + 1;
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 >= w) x1 = w - 1;
	if (y1 >= h) y1 = h - 1;
	for (int y = y0; y <= y1; y++) {
		for (int x = x0; x <= x1; x++) {
			int i = y * w + x;
			if (g_stamp[i] == frame)
				continue;
			g_stamp[i] = frame;
			void* lgp = lgp_tex_at(x, y);
			tile(ground, view, x, y, 0xffffffff, lgp ? lgp : g_cellTex[cell_kind(cells[i].flag)], (void*)UV_FULL);
		}
	}
}

// thiscall hooks written as fastcall: this in ecx, edx unused, callee pops
void* volatile g_groundMap; // cell table of the map whose ground was drawn last

void __fastcall ground_hook(void* self, void*, void* view, int* area, int flag) {
	g_groundMap = world_attr();
	aura_draw(view); // once a frame, see aura.cpp
	if (active() && area) {
		draw_cells(self, view, area);
		if (g_cellTex[0])
			return;
	}
	g_origGround(self, view, area, flag);
	if (g_ok && area)
		lgp_draw_alone(self, view, area);
}

void __fastcall water_hook(void* self, void*, void* view, int* area, int flag) {
	if (active() && g_cellTex[0])
		return;
	g_origWater(self, view, area, flag);
}

void __fastcall node_v1_hook(void* self, void*, void* a, void* b, void* c, void* d) {
	if (active() && g_cellTex[0])
		return;
	g_origNodeV1(self, a, b, c, d);
}

void __fastcall node_v2_hook(void* self, void*, void* a, void* b, void* c, void* d) {
	if (active() && g_cellTex[0])
		return;
	g_origNodeV2(self, a, b, c, d);
}

} // namespace

const int GM_DEFAULT_COLORS[GM_COLOR_COUNT] = {
	0x5f6380, // walkable
	0x2c3640, // grid lines
	0x101014, // unwalkable
	0x3a6ea5, // water
	0x707070, // shootable wall
};

bool graymap_install() {
	for (const Expect& e : EXPECT) {
		if (!readable(e.at, e.len) || memcmp((void*)e.at, e.bytes, e.len) != 0) {
			log_line("graymap: this game exe is not supported (code at %08x differs), graymap is off", (unsigned)e.at);
			return false;
		}
	}
	bool ok = patch_slot(GROUND_VTABLE, GROUND_SLOT_RENDER, GROUND_RENDER, (void*)ground_hook, (void**)&g_origGround) &&
	          patch_slot(GROUND_VTABLE, GROUND_SLOT_WATER, GROUND_WATER, (void*)water_hook, (void**)&g_origWater) &&
	          patch_slot(NODE_V1_VTABLE, NODE_SLOT_RENDER, NODE_V1_RENDER, (void*)node_v1_hook, (void**)&g_origNodeV1) &&
	          patch_slot(NODE_V2_VTABLE, NODE_SLOT_RENDER, NODE_V2_RENDER, (void*)node_v2_hook, (void**)&g_origNodeV2);
	if (!ok) {
		log_line("graymap: could not hook the map drawing, graymap is off");
		return false;
	}
	g_ok = true;
	log_line("graymap: ready");
	return true;
}

bool graymap_available() {
	return g_ok;
}

// True once the current map's ground has been drawn: not on the login screen
// or character select (no game mode), and not while a new map is loading (its
// cell table is new and not drawn yet). The game doesn't draw the ground every
// frame, so this doesn't look at how long ago it was drawn. On an exe where
// the map drawing could not be hooked, say yes so the buttons still show.
bool game_map_shown() {
	if (!g_ok)
		return true;
	// may run off the game thread while the game swaps maps: read with
	// ReadProcessMemory so a freed pointer fails instead of crashing
	auto rd = [](uintptr_t at, void* out, size_t n) {
		SIZE_T got = 0;
		return ReadProcessMemory(GetCurrentProcess(), (void*)at, out, n, &got) && got == n;
	};
	int kind = 0;
	uintptr_t mode = 0, world = 0, map = 0;
	if (!rd(MODE_MGR + 0x58, &kind, 4) || kind != 1 || !rd(MODE_MGR + 4, &mode, 4) || !mode ||
	    !rd(mode + 0xcc, &world, 4) || !world || !rd(world + 0x30, &map, 4) || !map)
		return false;
	return (void*)map == g_groundMap;
}

bool lgp_drawing() {
	return GetTickCount() - g_lgpDrewAt < 1000;
}
