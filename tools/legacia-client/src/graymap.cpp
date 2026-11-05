// Graymap: draws the map as flat coloured cells (walkable, unwalkable,
// water, shootable wall) instead of the 3D ground, water and buildings.
//
// It hooks the client's own ground drawing, so it is tied to one client
// build: the addresses below are for the Legacia client exe (2026-01-07 Ragexe).
// Every address is checked against the expected code before anything is
// patched; on any other exe Graymap stays off and the rest of the plugin
// works as usual.
#include "voice.h"

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
			tile(ground, view, x, y, 0xffffffff, g_cellTex[cell_kind(cells[i].flag)], (void*)UV_FULL);
		}
	}
}

// thiscall hooks written as fastcall: this in ecx, edx unused, callee pops
void __fastcall ground_hook(void* self, void*, void* view, int* area, int flag) {
	if (active() && area) {
		draw_cells(self, view, area);
		if (g_cellTex[0])
			return;
	}
	g_origGround(self, view, area, flag);
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
