// Graymap button: sits under the game's Cash Shop button at the top right of
// the game window and copies its look (43x43 blue box, cut corners, white
// rim, cloud with outlined title, picture at the bottom).
//   click        Graymap on/off
//   right-click  the Graymap colour window
//   drag         move it (saved as a distance from the top right corner)
// It is drawn pixel by pixel into a small buffer, so it looks like the
// game's own art, and animated: a shine sweeps over it now and then, the
// little map bobs while the mouse is over it, and while Graymap is on the map
// shows your colours and sparkles twinkle around it.
#include "voice.h"

#include <math.h>
#include <stdlib.h>

namespace {

const int S = 43; // the Cash Shop box is 43x43
const UINT_PTR TIMER_ANIM = 1;
const int FRAME_MS = 40;

// colours taken from the game's Cash Shop button
const uint32_t OUTLINE = 0x635a94, OUTLINE_DARK = 0x102963, CORNER = 0x6b63ad;
const uint32_t RIM = 0xadc6f7, WHITE = 0xffffff, INNER = 0x847bad, FILL = 0x9cbdf7;
const uint32_t CLOUD = 0xdef7ff;

HWND g_btn;
bool g_down, g_dragging, g_hover;
POINT g_grab, g_start;
DWORD g_t0;
uint32_t g_px[S * S];

bool on() {
	return g_cfg.gmEnabled && graymap_available();
}

void put(int x, int y, uint32_t c) {
	if (x >= 0 && y >= 0 && x < S && y < S)
		g_px[y * S + x] = c;
}

uint32_t get(int x, int y) {
	return g_px[y * S + x];
}

uint32_t mix(uint32_t a, uint32_t b, int t /* 0-256 */) {
	int r = (((a >> 16) & 0xff) * (256 - t) + ((b >> 16) & 0xff) * t) >> 8;
	int g = (((a >> 8) & 0xff) * (256 - t) + ((b >> 8) & 0xff) * t) >> 8;
	int bl = ((a & 0xff) * (256 - t) + (b & 0xff) * t) >> 8;
	return (uint32_t)(r << 16 | g << 8 | bl);
}

uint32_t gray(uint32_t c) {
	int l = (((c >> 16) & 0xff) * 3 + ((c >> 8) & 0xff) * 6 + (c & 0xff)) / 10;
	return mix((uint32_t)(l << 16 | l << 8 | l), 0xc4d2ee, 170);
}

// hand-made bold pixel letters (2px strokes, like the Cash Shop title);
// rows 0-6 are the letter, rows 7-8 the tail of y and p
struct Glyph {
	char ch;
	const char* rows[9];
};
const Glyph GLYPHS[] = {
	{'G', {".XXXX.", "XX..XX", "XX....", "XX.XXX", "XX..XX", "XX..XX", ".XXXX.", "", ""}},
	{'r', {"", "", "XX.XX", "XXX..", "XX...", "XX...", "XX...", "", ""}},
	{'a', {"", "", ".XXXX.", "....XX", ".XXXXX", "XX..XX", ".XXXXX", "", ""}},
	{'y', {"", "", "XX..XX", "XX..XX", "XX..XX", ".XXXXX", "....XX", "XXXXX.", ""}},
	{'m', {"", "", "XXXXXXX.", "XX.XX.XX", "XX.XX.XX", "XX.XX.XX", "XX.XX.XX", "", ""}},
	{'p', {"", "", "XXXXX.", "XX..XX", "XX..XX", "XX..XX", "XXXXX.", "XX....", "XX...."}},
};

const Glyph* glyph(char c) {
	for (const Glyph& g : GLYPHS)
		if (g.ch == c)
			return &g;
	return nullptr;
}

int glyph_w(const Glyph* g) {
	int w = 0;
	for (const char* r : g->rows)
		if ((int)strlen(r) > w)
			w = (int)strlen(r);
	return w;
}

// white letters with a 1px dark outline, centred on cx
void draw_word(const char* word, int cx, int top) {
	int total = -1;
	for (const char* c = word; *c; c++)
		total += glyph_w(glyph(*c)) + 1;
	for (int pass = 0; pass < 2; pass++) {
		int x = cx - total / 2;
		for (const char* c = word; *c; c++) {
			const Glyph* g = glyph(*c);
			for (int ry = 0; ry < 9; ry++)
				for (int rx = 0; g->rows[ry][rx]; rx++) {
					if (g->rows[ry][rx] != 'X')
						continue;
					if (pass == 0) {
						for (int dy = -1; dy <= 1; dy++)
							for (int dx = -1; dx <= 1; dx++)
								put(x + rx + dx, top + ry + dy, OUTLINE);
					} else {
						put(x + rx, top + ry, WHITE);
					}
				}
			x += glyph_w(g) + 1;
		}
	}
}

void draw_frame() {
	for (int i = 0; i < S * S; i++)
		g_px[i] = FILL;
	int e = S - 1;
	for (int k = 0; k < S; k++) {
		// ring 0 outline, ring 1 light rim, ring 2 white, ring 3 inner line
		put(k, 0, OUTLINE), put(k, e, OUTLINE), put(0, k, OUTLINE), put(e, k, OUTLINE);
		if (k >= 1 && k <= e - 1)
			put(k, 1, RIM), put(k, e - 1, RIM), put(1, k, RIM), put(e - 1, k, RIM);
		if (k >= 2 && k <= e - 2)
			put(k, 2, WHITE), put(k, e - 2, WHITE), put(2, k, WHITE), put(e - 2, k, WHITE);
		if (k >= 3 && k <= e - 3)
			put(k, 3, INNER), put(k, e - 3, INNER), put(3, k, INNER), put(e - 3, k, INNER);
	}
	// dark studs on the outline near each corner
	const int studs[] = {3, 4, e - 4, e - 3};
	for (int s : studs) {
		put(s, 0, OUTLINE_DARK), put(s, e, OUTLINE_DARK), put(0, s, OUTLINE_DARK), put(e, s, OUTLINE_DARK);
	}
	// cut corners
	for (int cx = 0; cx < 2; cx++) {
		for (int cy = 0; cy < 2; cy++) {
			auto P = [&](int x, int y, uint32_t c) { put(cx ? e - x : x, cy ? e - y : y, c); };
			P(1, 1, CORNER);
			P(2, 1, OUTLINE), P(1, 2, OUTLINE);
			// inner line: rounded, darker next to the corner
			P(3, 3, WHITE);
			P(4, 3, OUTLINE), P(5, 3, OUTLINE), P(3, 4, OUTLINE), P(3, 5, OUTLINE);
			P(4, 4, OUTLINE);
		}
	}
}

void draw_cloud(int top) {
	// soft cloud behind the title, as on the Cash Shop button
	for (int y = top; y < top + 19; y++) {
		for (int x = 5; x < S - 5; x++) {
			float dx = (x - (S - 1) / 2.0f) / 15.5f, dy = (y - (top + 9.0f)) / 9.5f;
			float d = dx * dx * dx * dx + dy * dy * dy * dy; // squircle
			if (d < 0.75f)
				put(x, y, CLOUD);
			else if (d < 1.0f)
				put(x, y, WHITE);
		}
	}
}

// little isometric map at the bottom, in the Graymap colours
void draw_map(float t) {
	static const int GRID[5][5] = {
		{GM_UNWALKABLE, GM_WALKABLE, GM_WALKABLE, GM_WALL, GM_UNWALKABLE},
		{GM_WALKABLE, GM_WALKABLE, GM_WALKABLE, GM_WALKABLE, GM_WALL},
		{GM_WATER, GM_WALKABLE, GM_WALKABLE, GM_WALKABLE, GM_WALKABLE},
		{GM_WATER, GM_WATER, GM_WALKABLE, GM_WALKABLE, GM_UNWALKABLE},
		{GM_UNWALKABLE, GM_WATER, GM_WALKABLE, GM_UNWALKABLE, GM_UNWALKABLE},
	};
	bool lit = on();
	int bob = g_hover ? (int)lroundf(sinf(t * 7.0f) * 1.2f) : 0;
	if (lit && !g_hover)
		bob = (int)lroundf(sinf(t * 2.2f) * 0.6f);
	const int cx = S / 2, top = 23 + bob;
	const int HW = 3; // half width of one diamond cell (cells are 6x3)
	// shadow under the map
	for (int y = 37; y <= 38; y++)
		for (int x = cx - 12; x <= cx + 12; x++) {
			float dx = (x - cx) / 12.5f;
			if (dx * dx + (y - 37.5f) * (y - 37.5f) / 1.5f < 1.0f)
				put(x, y, mix(get(x, y), OUTLINE, 110));
		}
	for (int y = 0; y < 15; y++) {
		for (int x = -17; x <= 17; x++) {
			// screen -> map cell (isometric 2:1)
			float u = (x / (float)HW + (y - 0.0f) / 1.5f) / 2.0f;
			float v = ((y - 0.0f) / 1.5f - x / (float)HW) / 2.0f;
			if (u < 0 || v < 0 || u >= 5 || v >= 5)
				continue;
			int cu = (int)u, cv = (int)v;
			uint32_t c = (uint32_t)g_cfg.gmColor[GRID[cv][cu]] & 0xffffff;
			c = lit ? mix(c, WHITE, 40) : gray(c);
			// grid line on the cell edges
			float fu = u - cu, fv = v - cv;
			bool edge = fu < 0.12f || fv < 0.12f;
			if (edge)
				c = lit ? mix(c, (uint32_t)g_cfg.gmColor[GM_GRID] & 0xffffff, 150) : mix(c, 0x7080a8, 120);
			put(cx + x, top + y, c);
		}
	}
	// outline around the diamond
	for (int y = -1; y < 16; y++) {
		for (int x = -18; x <= 18; x++) {
			int px = cx + x, py = top + y;
			if (px < 4 || py < 4 || px > S - 5 || py > S - 5)
				continue;
			float u = (x / (float)HW + y / 1.5f) / 2.0f, v = (y / 1.5f - x / (float)HW) / 2.0f;
			bool inside = u >= 0 && v >= 0 && u < 5 && v < 5;
			if (inside)
				continue;
			bool adj = false;
			for (int dy = -1; dy <= 1 && !adj; dy++)
				for (int dx = -1; dx <= 1 && !adj; dx++) {
					float u2 = ((x + dx) / (float)HW + (y + dy) / 1.5f) / 2.0f, v2 = ((y + dy) / 1.5f - (x + dx) / (float)HW) / 2.0f;
					adj = u2 >= 0 && v2 >= 0 && u2 < 5 && v2 < 5;
				}
			if (adj)
				put(px, py, OUTLINE);
		}
	}
}

// red map pin hopping on the little map
void draw_pin(float t) {
	static const char* PIN[] = {
		".OOOO.",
		"OrrrrO",
		"OrWrrO",
		"OrrrrO",
		".OrrO.",
		"..OO..",
	};
	float speed = g_hover ? 9.0f : on() ? 4.0f : 2.0f;
	int hop = (int)lroundf(fabsf(sinf(t * speed)) * (g_hover ? 4.0f : on() ? 3.0f : 1.5f));
	int x0 = S / 2 - 3, y0 = 24 - hop;
	// shadow stays on the map and shrinks while the pin is up
	int sw = hop > 2 ? 1 : 2;
	for (int x = S / 2 - sw; x < S / 2 + sw; x++)
		put(x, 30, mix(get(x, 30), 0x102963, 150));
	for (int y = 0; y < 6; y++)
		for (int x = 0; PIN[y][x]; x++) {
			char c = PIN[y][x];
			if (c == 'O')
				put(x0 + x, y0 + y, 0x5a1010);
			else if (c == 'r')
				put(x0 + x, y0 + y, y < 2 ? 0xff5a4a : 0xe02828);
			else if (c == 'W')
				put(x0 + x, y0 + y, WHITE);
		}
}

void sparkle(int x, int y, float phase) {
	// 4-point star that grows and fades
	float s = sinf(phase);
	if (s <= 0)
		return;
	int r = s > 0.66f ? 2 : s > 0.33f ? 1 : 0;
	uint32_t c = mix(0xfff2a0, WHITE, (int)(s * 256));
	put(x, y, WHITE);
	for (int k = 1; k <= r; k++) {
		uint32_t ck = k == r ? c : WHITE;
		put(x + k, y, ck), put(x - k, y, ck), put(x, y + k, ck), put(x, y - k, ck);
	}
}

void render() {
	float t = (GetTickCount() - g_t0) / 1000.0f;
	draw_frame();
	draw_cloud(3);
	draw_map(t);
	draw_pin(t);
	draw_word("Gray", S / 2, 5);
	draw_word("map", S / 2, 12);

	if (on()) {
		// the rim glows gold and pulses while Graymap is on
		int glow = 130 + (int)(sinf(t * 4.0f) * 90.0f);
		int e = S - 1;
		for (int k = 1; k < e; k++) {
			for (int ring = 1; ring <= 2; ring++) {
				if (k < ring || k > e - ring)
					continue;
				int idx[4][2] = {{k, ring}, {k, e - ring}, {ring, k}, {e - ring, k}};
				for (auto& q : idx)
					put(q[0], q[1], mix(get(q[0], q[1]), 0xffd84a, ring == 1 ? glow : glow / 2));
			}
		}
		// sparkles taking turns around the box
		static const int SP[][2] = {{8, 9}, {35, 8}, {36, 27}, {7, 31}, {21, 38}};
		for (int i = 0; i < 5; i++)
			sparkle(SP[i][0], SP[i][1], fmodf(t * 2.4f + i * 1.9f, 6.2832f * 1.6f));
	}

	// shine: a bright diagonal band sweeps across every few seconds
	// (faster and more often while the mouse is over the button)
	float period = g_hover ? 1.4f : 3.5f;
	float p = fmodf(t, period) / 0.7f; // 0..1 during the sweep
	if (p < 1.0f) {
		float band = -12.0f + p * (S + 24.0f);
		for (int y = 1; y < S - 1; y++)
			for (int x = 1; x < S - 1; x++) {
				float d = fabsf((x + y * 0.6f) - band);
				if (d < 4.0f)
					put(x, y, mix(get(x, y), WHITE, (int)((4.0f - d) * 30)));
			}
	}

	// hover / pressed
	if (g_down) {
		for (int i = 0; i < S * S; i++)
			g_px[i] = mix(g_px[i], OUTLINE, 40);
	} else if (g_hover) {
		for (int i = 0; i < S * S; i++)
			g_px[i] = mix(g_px[i], WHITE, 28);
	}
}

void paint(HDC dc) {
	render();
	BITMAPINFO bi = {};
	bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
	bi.bmiHeader.biWidth = S;
	bi.bmiHeader.biHeight = -S;
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 32;
	SetDIBitsToDevice(dc, 0, 0, S, S, 0, 0, 0, S, g_px, &bi, DIB_RGB_COLORS);
}

void save_spot() {
	RECT cr, wr;
	GetClientRect(g_gameWnd, &cr);
	POINT tr = {cr.right, 0};
	ClientToScreen(g_gameWnd, &tr);
	GetWindowRect(g_btn, &wr);
	g_cfg.gmBtnRight = tr.x - wr.right;
	g_cfg.gmBtnTop = wr.top - tr.y;
	config_save();
}

LRESULT CALLBACK proc(HWND hw, UINT msg, WPARAM wp, LPARAM lp) {
	switch (msg) {
	case WM_ERASEBKGND:
		return 1;
	case WM_PAINT: {
		PAINTSTRUCT ps;
		HDC dc = BeginPaint(hw, &ps);
		paint(dc);
		EndPaint(hw, &ps);
		return 0;
	}
	case WM_TIMER:
		if (wp == TIMER_ANIM && IsWindowVisible(hw)) {
			HDC dc = GetDC(hw);
			paint(dc);
			ReleaseDC(hw, dc);
		}
		return 0;
	case WM_MOUSEACTIVATE:
		return MA_NOACTIVATE;
	case WM_MOUSEMOVE: {
		if (!g_hover) {
			g_hover = true;
			TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, hw, 0};
			TrackMouseEvent(&tme);
		}
		if (g_down) {
			POINT c;
			GetCursorPos(&c);
			if (!g_dragging && (abs(c.x - g_start.x) > 3 || abs(c.y - g_start.y) > 3))
				g_dragging = true;
			if (g_dragging)
				SetWindowPos(hw, HWND_TOP, c.x - g_grab.x, c.y - g_grab.y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
		}
		return 0;
	}
	case WM_MOUSELEAVE:
		g_hover = false;
		return 0;
	case WM_LBUTTONDOWN:
	case WM_LBUTTONDBLCLK:
		g_down = true;
		g_dragging = false;
		g_grab = {(short)LOWORD(lp), (short)HIWORD(lp)};
		GetCursorPos(&g_start);
		SetCapture(hw);
		return 0;
	case WM_LBUTTONUP: {
		if (!g_down)
			return 0;
		bool dragged = g_dragging;
		g_down = false;
		g_dragging = false;
		ReleaseCapture();
		if (dragged)
			save_spot();
		else if (graymap_available())
			graymap_set_enabled(!g_cfg.gmEnabled);
		else
			graymap_ui_toggle(); // the window says why Graymap is off
		return 0;
	}
	case WM_RBUTTONUP:
		graymap_ui_toggle();
		return 0;
	case WM_CAPTURECHANGED:
		g_down = false;
		g_dragging = false;
		return 0;
	}
	return DefWindowProcA(hw, msg, wp, lp);
}

} // namespace

void graymap_button_create() {
	g_t0 = GetTickCount();
	WNDCLASSA wc = {};
	wc.lpfnWndProc = proc;
	wc.hInstance = g_module;
	wc.hCursor = LoadCursor(nullptr, IDC_HAND);
	wc.lpszClassName = "LegaciaGraymapButton";
	RegisterClassA(&wc);
	g_btn = CreateWindowExA(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, "LegaciaGraymapButton", "Graymap", WS_POPUP, 0, 0, S, S,
	                        g_gameWnd, nullptr, g_module, nullptr);
	// the same cut corners as the Cash Shop box
	HRGN rgn = CreateRectRgn(0, 2, S, S - 2);
	HRGN part = CreateRectRgn(1, 1, S - 1, S - 1);
	CombineRgn(rgn, rgn, part, RGN_OR);
	DeleteObject(part);
	part = CreateRectRgn(2, 0, S - 2, S);
	CombineRgn(rgn, rgn, part, RGN_OR);
	DeleteObject(part);
	SetWindowRgn(g_btn, rgn, FALSE);
	SetTimer(g_btn, TIMER_ANIM, FRAME_MS, nullptr);
}

void graymap_button_refresh() {
	if (g_btn)
		InvalidateRect(g_btn, nullptr, FALSE);
}

void graymap_button_update(bool gameActive) {
	if (!g_btn)
		return;
	if (!gameActive || IsIconic(g_gameWnd)) {
		if (IsWindowVisible(g_btn))
			ShowWindow(g_btn, SW_HIDE);
		return;
	}
	if (g_dragging)
		return;
	RECT cr;
	GetClientRect(g_gameWnd, &cr);
	POINT tl = {0, 0};
	ClientToScreen(g_gameWnd, &tl);
	int x = tl.x + cr.right - g_cfg.gmBtnRight - S, y = tl.y + g_cfg.gmBtnTop;
	// keep it inside the game window if the window got smaller
	if (x > tl.x + cr.right - S)
		x = tl.x + cr.right - S;
	if (x < tl.x)
		x = tl.x;
	if (y > tl.y + cr.bottom - S)
		y = tl.y + cr.bottom - S;
	if (y < tl.y)
		y = tl.y;
	RECT cur;
	GetWindowRect(g_btn, &cur);
	if (!IsWindowVisible(g_btn) || cur.left != x || cur.top != y)
		SetWindowPos(g_btn, HWND_TOP, x, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
}
