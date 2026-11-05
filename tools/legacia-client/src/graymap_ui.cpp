// Graymap window (Ctrl+Shift+G): turns the flat-colour map on and off and
// picks the colour of each cell type. Drawn like the Voice Settings window.
#include "ui_style.h"

#include <commctrl.h>
#include <math.h>
#include <stdio.h>

namespace {

enum {
	ID_X = 500, ID_SWITCH, ID_RESET,
	ID_COLOR = 510, // ID_COLOR + GraymapColor
	ID_BRIGHT = 530, ID_HEX, ID_RED, ID_GREEN, ID_BLUE,
};

const int W = 480, H = 372, TITLE_H = 22;
const int PANEL_X = 180, PICK_X = 192, PICK_Y = 126, PICK_W = 190, PICK_H = 110;
const int PREVIEW_X = 392, PREVIEW_W = 72;
const UINT_PTR TIMER_SAVE = 1;

const char* const NAMES[GM_COLOR_COUNT] = {"Walkable", "Grid lines", "Unwalkable", "Water", "Shootable wall"};

HWND g_wnd;
HBITMAP g_pickBmp;
int g_sel;
float g_h, g_s, g_v; // selected colour as hue 0-360, saturation and brightness 0-1
bool g_dragging;
bool g_syncing; // filling the edit boxes ourselves, ignore their change events
bool g_placed;

HWND item(int id) { return GetDlgItem(g_wnd, id); }

int rgb_of(float h, float s, float v) {
	float c = v * s, x = c * (1 - fabsf(fmodf(h / 60.0f, 2) - 1)), m = v - c;
	float r = 0, g = 0, b = 0;
	if (h < 60) r = c, g = x;
	else if (h < 120) r = x, g = c;
	else if (h < 180) g = c, b = x;
	else if (h < 240) g = x, b = c;
	else if (h < 300) r = x, b = c;
	else r = c, b = x;
	auto ch = [m](float f) { return (int)lroundf((f + m) * 255); };
	return (ch(r) << 16) | (ch(g) << 8) | ch(b);
}

void hsv_of(int rgb, float* h, float* s, float* v) {
	float r = ((rgb >> 16) & 255) / 255.0f, g = ((rgb >> 8) & 255) / 255.0f, b = (rgb & 255) / 255.0f;
	float mx = fmaxf(r, fmaxf(g, b)), mn = fminf(r, fminf(g, b)), d = mx - mn;
	*v = mx;
	*s = mx > 0 ? d / mx : 0;
	if (d <= 0)
		return; // grey: keep the hue the picker had
	if (mx == r) *h = 60 * fmodf((g - b) / d, 6);
	else if (mx == g) *h = 60 * ((b - r) / d + 2);
	else *h = 60 * ((r - g) / d + 4);
	if (*h < 0)
		*h += 360;
}

COLORREF colorref(int rgb) {
	return RGB((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255);
}

void make_picker() {
	BITMAPINFO bi = {};
	bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
	bi.bmiHeader.biWidth = PICK_W;
	bi.bmiHeader.biHeight = -PICK_H;
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 32;
	uint32_t* px = nullptr;
	g_pickBmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, (void**)&px, nullptr, 0);
	if (!g_pickBmp)
		return;
	for (int y = 0; y < PICK_H; y++)
		for (int x = 0; x < PICK_W; x++)
			px[y * PICK_W + x] = (uint32_t)rgb_of(360.0f * x / PICK_W, 1 - (float)y / (PICK_H - 1), 1);
}

void save_soon() {
	SetTimer(g_wnd, TIMER_SAVE, 500, nullptr);
}

void sync_fields() {
	int rgb = g_cfg.gmColor[g_sel];
	char buf[16];
	g_syncing = true;
	HWND focus = GetFocus();
	snprintf(buf, sizeof(buf), "#%06X", rgb);
	if (focus != item(ID_HEX))
		SetWindowTextA(item(ID_HEX), buf);
	int parts[3] = {(rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255};
	for (int i = 0; i < 3; i++) {
		if (focus == item(ID_RED + i))
			continue;
		snprintf(buf, sizeof(buf), "%d", parts[i]);
		SetWindowTextA(item(ID_RED + i), buf);
	}
	SendMessageA(item(ID_BRIGHT), TBM_SETPOS, TRUE, (LPARAM)lroundf(g_v * 100));
	g_syncing = false;
	InvalidateRect(g_wnd, nullptr, FALSE);
	for (int i = 0; i < GM_COLOR_COUNT; i++)
		InvalidateRect(item(ID_COLOR + i), nullptr, FALSE);
}

void select_color(int i) {
	g_sel = i;
	hsv_of(g_cfg.gmColor[i], &g_h, &g_s, &g_v);
	sync_fields();
}

// the colour changed from the picker or slider
void set_from_hsv() {
	g_cfg.gmColor[g_sel] = rgb_of(g_h, g_s, g_v);
	save_soon();
	sync_fields();
}

// the colour changed from the hex or RGB boxes
void set_rgb(int rgb) {
	g_cfg.gmColor[g_sel] = rgb & 0xffffff;
	hsv_of(rgb, &g_h, &g_s, &g_v);
	save_soon();
	sync_fields();
}

void pick_at(int x, int y) {
	x = x < PICK_X ? PICK_X : x >= PICK_X + PICK_W ? PICK_X + PICK_W - 1 : x;
	y = y < PICK_Y ? PICK_Y : y >= PICK_Y + PICK_H ? PICK_Y + PICK_H - 1 : y;
	g_h = 360.0f * (x - PICK_X) / PICK_W;
	g_s = 1 - (float)(y - PICK_Y) / (PICK_H - 1);
	if (g_v < 0.05f)
		g_v = 1; // picking a hue on black would show nothing
	set_from_hsv();
}

void text(HDC dc, const char* s, int x, int y, COLORREF c, HFONT f, UINT align = DT_LEFT, int w = 300) {
	SelectObject(dc, f);
	SetTextColor(dc, c);
	RECT r = {x, y, x + w, y + 16};
	if (align == DT_RIGHT)
		r = {x - w, y, x, y + 16};
	DrawTextA(dc, s, -1, &r, align | DT_SINGLELINE | DT_NOPREFIX);
}

void paint(HDC dc) {
	RECT cr;
	GetClientRect(g_wnd, &cr);
	fill(dc, cr, C_BODY);
	RECT title = {0, 0, cr.right, TITLE_H};
	gradient(dc, title, C_TITLE_TOP, C_TITLE_BOTTOM);
	SetBkMode(dc, TRANSPARENT);
	text(dc, "Graymap", 9, 4, RGB(60, 80, 130), g_bold);
	text(dc, "Graymap", 8, 3, RGB(255, 255, 255), g_bold);

	text(dc, "Graymap", 16, 34, C_TEXT, g_bold);
	if (!graymap_available())
		text(dc, "Not available for this game version", 16, 52, C_WAIT, g_font);
	else if (g_cfg.gmEnabled)
		text(dc, "On - map cells drawn in flat colours", 16, 52, C_OK, g_font);
	else
		text(dc, "Off - the normal map", 16, 52, C_HEADER, g_font);
	RECT rule = {16, 76, cr.right - 16, 77};
	fill(dc, rule, C_RULE);

	text(dc, "Colours", 16, 86, C_HEADER, g_font);

	// right panel: picker, preview, brightness
	RECT panel = {PANEL_X, 100, cr.right - 16, 336};
	fill(dc, panel, RGB(255, 255, 255));
	frame(dc, panel, C_BTN_BORDER);
	int rgb = g_cfg.gmColor[g_sel];
	char buf[32];
	text(dc, NAMES[g_sel], PICK_X, 106, C_TEXT, g_bold);
	snprintf(buf, sizeof(buf), "#%06X", rgb);
	text(dc, buf, cr.right - 28, 106, C_BLUE_TEXT, g_font, DT_RIGHT, 100);

	if (g_pickBmp) {
		HDC mem = CreateCompatibleDC(dc);
		HGDIOBJ old = SelectObject(mem, g_pickBmp);
		BitBlt(dc, PICK_X, PICK_Y, PICK_W, PICK_H, mem, 0, 0, SRCCOPY);
		SelectObject(mem, old);
		DeleteDC(mem);
	}
	RECT pr = {PICK_X - 1, PICK_Y - 1, PICK_X + PICK_W + 1, PICK_Y + PICK_H + 1};
	frame(dc, pr, C_BTN_BORDER);
	// marker on the picked hue and saturation
	int mx = PICK_X + (int)(g_h / 360.0f * PICK_W), my = PICK_Y + (int)((1 - g_s) * (PICK_H - 1));
	SelectObject(dc, GetStockObject(NULL_BRUSH));
	SelectObject(dc, GetStockObject(DC_PEN));
	SetDCPenColor(dc, RGB(255, 255, 255));
	Ellipse(dc, mx - 5, my - 5, mx + 6, my + 6);
	SetDCPenColor(dc, RGB(40, 40, 40));
	Ellipse(dc, mx - 6, my - 6, mx + 7, my + 7);

	RECT prev = {PREVIEW_X, PICK_Y, PREVIEW_X + PREVIEW_W, PICK_Y + PICK_H};
	SelectObject(dc, GetStockObject(DC_BRUSH));
	SetDCBrushColor(dc, colorref(rgb));
	SetDCPenColor(dc, C_BTN_BORDER);
	RoundRect(dc, prev.left, prev.top, prev.right, prev.bottom, 8, 8);
	int lum = ((rgb >> 16) & 255) * 3 + ((rgb >> 8) & 255) * 6 + (rgb & 255);
	text(dc, "Preview", PREVIEW_X + 8, PICK_Y + PICK_H - 20, lum > 1300 ? C_TEXT : RGB(255, 255, 255), g_font);

	text(dc, "Brightness", PICK_X, 244, C_TEXT, g_font);
	snprintf(buf, sizeof(buf), "%d%%", (int)lroundf(g_v * 100));
	text(dc, buf, cr.right - 28, 244, C_BLUE_TEXT, g_font, DT_RIGHT, 60);

	text(dc, "Hex", PICK_X, 288, C_HEADER, g_font);
	text(dc, "Red", 300, 288, C_HEADER, g_font);
	text(dc, "Green", 352, 288, C_HEADER, g_font);
	text(dc, "Blue", 404, 288, C_HEADER, g_font);

	RECT foot = {16, cr.bottom - 30, cr.right - 16, cr.bottom - 29};
	fill(dc, foot, C_RULE);
	text(dc, "Changes show on the map right away. Ctrl+Shift+G opens this window.", 16, cr.bottom - 23, C_HEADER, g_font,
	     DT_LEFT, cr.right - 32);
	frame(dc, cr, C_BORDER);
}

void draw_switch(const DRAWITEMSTRUCT* d) {
	HDC dc = d->hDC;
	RECT r = d->rcItem;
	fill(dc, r, C_BODY);
	bool on = g_cfg.gmEnabled && graymap_available();
	SelectObject(dc, GetStockObject(DC_PEN));
	SelectObject(dc, GetStockObject(DC_BRUSH));
	SetDCPenColor(dc, C_BTN_BORDER);
	SetDCBrushColor(dc, RGB(255, 255, 255));
	RoundRect(dc, r.left, r.top, r.right, r.bottom, 6, 6);
	int mid = (r.left + r.right) / 2;
	RECT knob = on ? RECT{r.left + 2, r.top + 2, mid, r.bottom - 2} : RECT{mid, r.top + 2, r.right - 2, r.bottom - 2};
	gradient(dc, knob, on ? C_SEL_TOP : C_BTN_TOP, on ? C_SEL_BOTTOM : C_BTN_BOTTOM);
	frame(dc, knob, on ? C_SEL_BORDER : C_BTN_BORDER);
	SetBkMode(dc, TRANSPARENT);
	SelectObject(dc, g_bold);
	RECT lt = {r.left, r.top, mid, r.bottom}, rt = {mid, r.top, r.right, r.bottom};
	SetTextColor(dc, on ? C_SEL_TEXT : C_DISABLED);
	DrawTextA(dc, "ON", -1, &lt, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
	SetTextColor(dc, on ? C_DISABLED : C_TEXT);
	DrawTextA(dc, "OFF", -1, &rt, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void draw_button(const DRAWITEMSTRUCT* d) {
	int id = (int)d->CtlID;
	if (id == ID_X) {
		draw_close_x(d);
		return;
	}
	if (id == ID_SWITCH) {
		draw_switch(d);
		return;
	}
	HDC dc = d->hDC;
	RECT r = d->rcItem;
	bool pressed = (d->itemState & ODS_SELECTED) != 0;
	bool lit = pressed || (id >= ID_COLOR && id - ID_COLOR == g_sel);
	fill(dc, r, C_BODY);
	SelectObject(dc, GetStockObject(DC_PEN));
	SelectObject(dc, GetStockObject(DC_BRUSH));
	SetDCPenColor(dc, lit ? C_SEL_BORDER : C_BTN_BORDER);
	SetDCBrushColor(dc, lit ? RGB(232, 240, 252) : RGB(255, 255, 255));
	RoundRect(dc, r.left, r.top, r.right, r.bottom, 7, 7);
	SetBkMode(dc, TRANSPARENT);
	SelectObject(dc, g_font);
	SetTextColor(dc, C_TEXT);
	if (id >= ID_COLOR && id < ID_COLOR + GM_COLOR_COUNT) {
		int i = id - ID_COLOR;
		int cy = (r.top + r.bottom) / 2;
		SetDCBrushColor(dc, colorref(g_cfg.gmColor[i]));
		SetDCPenColor(dc, C_BTN_BORDER);
		RoundRect(dc, r.left + 8, cy - 8, r.left + 24, cy + 8, 4, 4);
		RECT tr = {r.left + 34, r.top, r.right - 4, r.bottom};
		DrawTextA(dc, NAMES[i], -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
	} else {
		char t[64];
		GetWindowTextA(d->hwndItem, t, sizeof(t));
		DrawTextA(dc, t, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
	}
}

void on_edit(int id) {
	if (g_syncing)
		return;
	char buf[16];
	GetWindowTextA(item(id), buf, sizeof(buf));
	int rgb = g_cfg.gmColor[g_sel];
	if (id == ID_HEX) {
		const char* p = buf[0] == '#' ? buf + 1 : buf;
		char* end;
		long v = strtol(p, &end, 16);
		if (strlen(p) != 6 || *end)
			return; // wait until the code is complete
		rgb = (int)v;
	} else {
		char* end;
		long v = strtol(buf, &end, 10);
		if (!buf[0] || *end || v < 0 || v > 255)
			return;
		int shift = 16 - 8 * (id - ID_RED);
		rgb = (rgb & ~(255 << shift)) | ((int)v << shift);
	}
	if (rgb != g_cfg.gmColor[g_sel])
		set_rgb(rgb);
}

LRESULT CALLBACK proc(HWND hw, UINT msg, WPARAM wp, LPARAM lp) {
	switch (msg) {
	case WM_ERASEBKGND:
		return 1;
	case WM_PAINT: {
		PAINTSTRUCT ps;
		HDC wdc = BeginPaint(hw, &ps);
		RECT r;
		GetClientRect(hw, &r);
		HDC dc = CreateCompatibleDC(wdc);
		HBITMAP bmp = CreateCompatibleBitmap(wdc, r.right, r.bottom);
		HGDIOBJ old = SelectObject(dc, bmp);
		paint(dc);
		BitBlt(wdc, 0, 0, r.right, r.bottom, dc, 0, 0, SRCCOPY);
		SelectObject(dc, old);
		DeleteObject(bmp);
		DeleteDC(dc);
		EndPaint(hw, &ps);
		return 0;
	}
	case WM_NCHITTEST: {
		LRESULT hit = DefWindowProcA(hw, msg, wp, lp);
		POINT pt = {(short)LOWORD(lp), (short)HIWORD(lp)};
		ScreenToClient(hw, &pt);
		return hit == HTCLIENT && pt.y < TITLE_H ? HTCAPTION : hit;
	}
	case WM_EXITSIZEMOVE:
		g_placed = true;
		return 0;
	case WM_LBUTTONDOWN: {
		int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
		if (x >= PICK_X && x < PICK_X + PICK_W && y >= PICK_Y && y < PICK_Y + PICK_H) {
			g_dragging = true;
			SetCapture(hw);
			pick_at(x, y);
		}
		return 0;
	}
	case WM_MOUSEMOVE:
		if (g_dragging)
			pick_at((short)LOWORD(lp), (short)HIWORD(lp));
		return 0;
	case WM_LBUTTONUP:
		if (g_dragging) {
			g_dragging = false;
			ReleaseCapture();
		}
		return 0;
	case WM_HSCROLL:
		if ((HWND)lp == item(ID_BRIGHT)) {
			g_v = (int)SendMessageA(item(ID_BRIGHT), TBM_GETPOS, 0, 0) / 100.0f;
			set_from_hsv();
		}
		return 0;
	case WM_TIMER:
		if (wp == TIMER_SAVE) {
			KillTimer(hw, TIMER_SAVE);
			config_save();
		}
		return 0;
	case WM_DRAWITEM:
		draw_button((const DRAWITEMSTRUCT*)lp);
		return TRUE;
	case WM_COMMAND: {
		int id = LOWORD(wp), code = HIWORD(wp);
		bool clicked = code == BN_CLICKED || code == BN_DOUBLECLICKED;
		if (code == EN_CHANGE) {
			on_edit(id);
		} else if (id == ID_X && clicked) {
			ShowWindow(hw, SW_HIDE);
		} else if (id == ID_SWITCH && clicked && graymap_available()) {
			graymap_set_enabled(!g_cfg.gmEnabled);
		} else if (id == ID_RESET && clicked) {
			for (int i = 0; i < GM_COLOR_COUNT; i++)
				g_cfg.gmColor[i] = GM_DEFAULT_COLORS[i];
			config_save();
			select_color(g_sel);
		} else if (id >= ID_COLOR && id < ID_COLOR + GM_COLOR_COUNT && clicked) {
			select_color(id - ID_COLOR);
		}
		return 0;
	}
	case WM_CTLCOLORSTATIC:
	case WM_CTLCOLOREDIT:
		SetBkMode((HDC)wp, TRANSPARENT);
		SetTextColor((HDC)wp, C_TEXT);
		return (LRESULT)g_whiteBrush;
	case WM_CLOSE:
		ShowWindow(hw, SW_HIDE);
		return 0;
	}
	return DefWindowProcA(hw, msg, wp, lp);
}

HWND child(const char* cls, const char* text, DWORD style, int x, int y, int w, int h, int id) {
	HWND c = CreateWindowExA(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, g_wnd, (HMENU)(INT_PTR)id, g_module,
	                         nullptr);
	SendMessageA(c, WM_SETFONT, (WPARAM)g_font, TRUE);
	return c;
}

} // namespace

void graymap_ui_create() {
	WNDCLASSA wc = {};
	wc.lpfnWndProc = proc;
	wc.hInstance = g_module;
	wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
	wc.hbrBackground = g_bodyBrush;
	wc.lpszClassName = "LegaciaGraymap";
	RegisterClassA(&wc);
	g_wnd = CreateWindowExA(WS_EX_TOOLWINDOW, "LegaciaGraymap", "Graymap", WS_POPUP | WS_CLIPCHILDREN, 0, 0, W, H, g_gameWnd,
	                        nullptr, g_module, nullptr);
	SetWindowRgn(g_wnd, CreateRoundRectRgn(0, 0, W + 1, H + 1, 7, 7), FALSE);
	make_picker();

	child("BUTTON", "", BS_OWNERDRAW, W - 21, 3, 16, 16, ID_X);
	child("BUTTON", "", BS_OWNERDRAW, W - 76, 38, 60, 22, ID_SWITCH);
	for (int i = 0; i < GM_COLOR_COUNT; i++)
		child("BUTTON", NAMES[i], BS_OWNERDRAW | WS_TABSTOP, 16, 104 + i * 34, 152, 28, ID_COLOR + i);
	child("BUTTON", "Reset all colours", BS_OWNERDRAW | WS_TABSTOP, 16, 278, 152, 26, ID_RESET);
	HWND tb = child(TRACKBAR_CLASSA, "", TBS_NOTICKS | TBS_BOTH, PICK_X - 4, 260, W - 16 - PICK_X - 8, 24, ID_BRIGHT);
	SendMessageA(tb, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
	DWORD es = WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL;
	child("EDIT", "", es, PICK_X, 304, 96, 20, ID_HEX);
	child("EDIT", "", es | ES_NUMBER, 300, 304, 44, 20, ID_RED);
	child("EDIT", "", es | ES_NUMBER, 352, 304, 44, 20, ID_GREEN);
	child("EDIT", "", es | ES_NUMBER, 404, 304, 44, 20, ID_BLUE);
	SendMessageA(item(ID_HEX), EM_LIMITTEXT, 7, 0);
	for (int id = ID_RED; id <= ID_BLUE; id++)
		SendMessageA(item(id), EM_LIMITTEXT, 3, 0);
	select_color(0);
}

void graymap_ui_toggle() {
	if (IsWindowVisible(g_wnd)) {
		ShowWindow(g_wnd, SW_HIDE);
		return;
	}
	if (!g_placed) {
		// middle of the game screen
		RECT cr;
		GetClientRect(g_gameWnd, &cr);
		POINT c = {cr.right / 2, cr.bottom / 2};
		ClientToScreen(g_gameWnd, &c);
		SetWindowPos(g_wnd, HWND_TOP, c.x - W / 2, c.y - H / 2, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
	}
	select_color(g_sel);
	ShowWindow(g_wnd, SW_SHOW);
	SetForegroundWindow(g_wnd);
}

void graymap_set_enabled(bool on) {
	if (!graymap_available())
		return;
	g_cfg.gmEnabled = on;
	config_save();
	if (g_wnd) {
		InvalidateRect(item(ID_SWITCH), nullptr, FALSE);
		InvalidateRect(g_wnd, nullptr, FALSE);
	}
	graymap_button_refresh();
}

HWND graymap_ui_window() {
	return g_wnd;
}
