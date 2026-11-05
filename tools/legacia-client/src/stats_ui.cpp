// Player Statistics window (Ctrl+Shift+S, or the Stats button under the
// Graymap button): the character's stats and every bonus, one
// tab per page. Drawn like the Graymap window.
//
// The text is the same as the in-game @statsview sections. The plugin asks the
// voice server for a page (UDP 0x05), the voice server asks the map-server,
// and the answer comes back as ^RRGGBB-coloured lines. The plugin stays
// signed in to the voice server even with voice chat turned off (it just
// neither talks nor hears then), so this works either way. See doc/voice_chat.md.
//
// Background pictures: any .png/.jpg/.bmp put in plugins\stats_backgrounds
// can be shown behind the window (the Background button lists them, sets
// how see-through the panels are and opens the folder; < > step through). Any size works: the picture is scaled to cover
// the window and cropped around its centre. The text panels stay
// see-through by [stats] panel_opacity percent in legacia.ini.
#include "ui_style.h"

#include <algorithm>
#include <stdio.h>
#include <string.h>
using std::max;
using std::min;
#include <objidl.h> // voice.h uses WIN32_LEAN_AND_MEAN, GDI+ needs this
#include <gdiplus.h>
#include <shellapi.h>

namespace {

enum { ID_X = 600, ID_REFRESH, ID_BG_PREV, ID_BG_NEXT, ID_BG_FOLDER, ID_TAB = 610 };

const int W = 520, H = 440, TITLE_H = 22;
const int TAB_X = 16, TAB_Y = 34, TAB_W = 116, TAB_H = 28, TAB_GAP = 4;
const int PANEL_X = 144, PANEL_Y = 34, PANEL_R = W - 16, PANEL_B = H - 40;
const int PAD = 10, LINE_H = 16, SCROLL_W = 8;
const UINT_PTR TIMER_REFRESH = 1;
const DWORD REFRESH_MS = 3000; // live values while the window is open
const DWORD TIMEOUT_MS = 4000;
const UINT WM_STATS = WM_APP + 41;

const char* const TABS[] = {"Overview", "Attack", "Magic", "Defense", "Skills", "Healing", "Procs", "EXP & Drops"};
const int TAB_COUNT = sizeof(TABS) / sizeof(TABS[0]);

HWND g_wnd;
HWND g_refreshBtn; // "Sign in" while the plugin waits for @voice
int g_tab;
bool g_placed;
int g_scroll;       // pixels scrolled down in the text panel
int g_contentH;     // height of the laid out text, from the last paint
bool g_dragThumb;
int g_dragY, g_dragScroll;

CRITICAL_SECTION g_lock; // the page text is written by the net thread
std::string g_text[TAB_COUNT];
DWORD g_gotAt[TAB_COUNT];   // GetTickCount when it arrived, 0 = never
DWORD g_askedAt;            // last request for the current tab
volatile bool g_unavailable; // the server said it can't answer
volatile bool g_waiting;     // asked, no answer yet

HWND item(int id) { return GetDlgItem(g_wnd, id); }

// ---- background picture ----

std::vector<std::string> g_bgFiles; // pictures in the folder, sorted
HBITMAP g_bgBmp;                    // the chosen one, scaled to the window
HDC g_bgDC;                         // memory DC holding g_bgBmp
DWORD g_bgNoticeAt;                 // show the picture's name in the status line for a moment
ULONG_PTR g_gdiplus;

std::string bg_folder() {
	char path[MAX_PATH];
	GetModuleFileNameA(g_module, path, MAX_PATH); // ...\plugins\legacia_plugin.dll
	char* slash = strrchr(path, '\\');
	if (slash)
		slash[1] = '\0';
	else
		path[0] = '\0';
	return std::string(path) + "stats_backgrounds\\";
}

void scan_backgrounds() {
	g_bgFiles.clear();
	std::string dir = bg_folder();
	CreateDirectoryA(dir.c_str(), nullptr); // so players can find where to put pictures
	WIN32_FIND_DATAA fd;
	HANDLE h = FindFirstFileA((dir + "*").c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE)
		return;
	do {
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			continue;
		const char* dot = strrchr(fd.cFileName, '.');
		if (dot && (!_stricmp(dot, ".png") || !_stricmp(dot, ".jpg") || !_stricmp(dot, ".jpeg") || !_stricmp(dot, ".bmp")))
			g_bgFiles.push_back(fd.cFileName);
	} while (FindNextFileA(h, &fd));
	FindClose(h);
	std::sort(g_bgFiles.begin(), g_bgFiles.end(), [](const std::string& a, const std::string& b) { return _stricmp(a.c_str(), b.c_str()) < 0; });
}

void free_background() {
	if (g_bgDC) {
		DeleteDC(g_bgDC);
		g_bgDC = nullptr;
	}
	if (g_bgBmp) {
		DeleteObject(g_bgBmp);
		g_bgBmp = nullptr;
	}
}

// Loads g_cfg.stBackground scaled to cover the whole window. Returns false
// (and shows no picture) when it's empty or can't be read.
bool load_background() {
	free_background();
	if (!g_cfg.stBackground[0])
		return false;
	std::string path = bg_folder() + g_cfg.stBackground;
	wchar_t wpath[MAX_PATH];
	if (!MultiByteToWideChar(CP_ACP, 0, path.c_str(), -1, wpath, MAX_PATH))
		return false;
	Gdiplus::Bitmap* img = Gdiplus::Bitmap::FromFile(wpath);
	if (!img || img->GetLastStatus() != Gdiplus::Ok || img->GetWidth() == 0 || img->GetHeight() == 0) {
		delete img;
		log_line("stats: cannot read background %s", path.c_str());
		return false;
	}
	HDC screen = GetDC(nullptr);
	g_bgDC = CreateCompatibleDC(screen);
	g_bgBmp = CreateCompatibleBitmap(screen, W, H);
	ReleaseDC(nullptr, screen);
	SelectObject(g_bgDC, g_bgBmp);
	{
		Gdiplus::Graphics gr(g_bgDC);
		gr.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
		gr.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
		// cover: scale until both sides fill the window, crop the rest evenly
		float iw = (float)img->GetWidth(), ih = (float)img->GetHeight();
		float scale = max(W / iw, H / ih);
		float dw = iw * scale, dh = ih * scale;
		gr.DrawImage(img, (W - dw) / 2, (H - dh) / 2, dw, dh);
	}
	delete img;
	return true;
}

void background_changed() {
	load_background();
	config_save();
	g_bgNoticeAt = GetTickCount();
	if (!g_bgNoticeAt)
		g_bgNoticeAt = 1;
	InvalidateRect(g_wnd, nullptr, FALSE);
	for (HWND c = GetWindow(g_wnd, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT))
		InvalidateRect(c, nullptr, FALSE);
}

// step through None, picture 1, picture 2, ...
void cycle_background(int dir) {
	scan_backgrounds();
	int n = (int)g_bgFiles.size(), cur = -1;
	for (int i = 0; i < n; i++)
		if (!_stricmp(g_bgFiles[i].c_str(), g_cfg.stBackground))
			cur = i;
	int next = cur + dir; // -1 = no picture
	if (next < -1)
		next = n - 1;
	if (next >= n)
		next = -1;
	snprintf(g_cfg.stBackground, sizeof(g_cfg.stBackground), "%s", next < 0 ? "" : g_bgFiles[next].c_str());
	background_changed();
}

// Background button: pick a picture, how see-through the panels are, or
// open the folder to add pictures
void background_menu() {
	enum { M_NONE = 1, M_FOLDER = 2, M_OPACITY = 10, M_PICTURE = 100 };
	static const int OPACITY[] = {40, 55, 70, 85, 100};
	scan_backgrounds();
	HMENU m = CreatePopupMenu();
	AppendMenuA(m, MF_STRING | (g_cfg.stBackground[0] ? 0 : MF_CHECKED), M_NONE, "No picture");
	for (size_t i = 0; i < g_bgFiles.size() && i < 200; i++) {
		bool on = !_stricmp(g_bgFiles[i].c_str(), g_cfg.stBackground);
		AppendMenuA(m, MF_STRING | (on ? MF_CHECKED : 0), M_PICTURE + (UINT)i, g_bgFiles[i].c_str());
	}
	AppendMenuA(m, MF_SEPARATOR, 0, nullptr);
	HMENU op = CreatePopupMenu();
	for (int i = 0; i < 5; i++) {
		char t[32];
		snprintf(t, sizeof(t), i == 4 ? "Solid" : "%d%%", OPACITY[i]);
		AppendMenuA(op, MF_STRING | (g_cfg.stOpacity == OPACITY[i] ? MF_CHECKED : 0), M_OPACITY + i, t);
	}
	AppendMenuA(m, MF_POPUP, (UINT_PTR)op, "Panel see-through");
	AppendMenuA(m, MF_STRING, M_FOLDER, "Open backgrounds folder...");
	RECT br;
	GetWindowRect(item(ID_BG_FOLDER), &br);
	int pick = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, br.left, br.top, 0, g_wnd, nullptr);
	DestroyMenu(m); // also destroys the submenu
	if (pick == M_NONE) {
		g_cfg.stBackground[0] = '\0';
		background_changed();
	} else if (pick >= M_PICTURE && pick < M_PICTURE + (int)g_bgFiles.size()) {
		snprintf(g_cfg.stBackground, sizeof(g_cfg.stBackground), "%s", g_bgFiles[pick - M_PICTURE].c_str());
		background_changed();
	} else if (pick >= M_OPACITY && pick < M_OPACITY + 5) {
		g_cfg.stOpacity = OPACITY[pick - M_OPACITY];
		background_changed();
	} else if (pick == M_FOLDER) {
		ShellExecuteA(nullptr, "open", bg_folder().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
	}
}

// see-through fill over the picture
void glass(HDC dc, const RECT& r, COLORREF c, int percent) {
	int w = r.right - r.left, h = r.bottom - r.top;
	if (w <= 0 || h <= 0)
		return;
	HDC mem = CreateCompatibleDC(dc);
	HBITMAP bmp = CreateCompatibleBitmap(dc, w, h);
	HGDIOBJ old = SelectObject(mem, bmp);
	RECT all = {0, 0, w, h};
	fill(mem, all, c);
	BLENDFUNCTION bf = {AC_SRC_OVER, 0, (BYTE)(percent * 255 / 100), 0};
	AlphaBlend(dc, r.left, r.top, w, h, mem, 0, 0, w, h, bf);
	SelectObject(mem, old);
	DeleteObject(bmp);
	DeleteDC(mem);
}

// copies the picture behind a window area; x/y in window coordinates
void blit_background(HDC dc, const RECT& dst, int x, int y) {
	BitBlt(dc, dst.left, dst.top, dst.right - dst.left, dst.bottom - dst.top, g_bgDC, x, y, SRCCOPY);
}

void ask() {
	g_askedAt = GetTickCount();
	g_waiting = net_request_stats((uint8_t)g_tab);
	InvalidateRect(g_wnd, nullptr, FALSE);
}

void select_tab(int t) {
	if (t != g_tab)
		g_scroll = 0;
	g_tab = t;
	for (int i = 0; i < TAB_COUNT; i++)
		InvalidateRect(item(ID_TAB + i), nullptr, FALSE);
	ask();
}

// ---- text layout ----

struct Run {
	std::string text;
	COLORREF color;
};

COLORREF hex_color(const char* p) {
	char buf[7];
	memcpy(buf, p, 6);
	buf[6] = 0;
	unsigned v = (unsigned)strtoul(buf, nullptr, 16);
	switch (v) {
	case 0x000000: return C_TEXT;
	case 0x0055FF: return C_BLUE_TEXT;
	case 0x777777: return C_HEADER;
	case 0xFF6600: return RGB(214, 96, 0);
	}
	return RGB((v >> 16) & 255, (v >> 8) & 255, v & 255);
}

bool is_hex6(const char* p) {
	for (int i = 0; i < 6; i++) {
		char c = p[i];
		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
			return false;
	}
	return true;
}

// splits "^0055FFTitle^000000 rest" into coloured runs
std::vector<Run> parse(const std::string& line) {
	std::vector<Run> out;
	COLORREF c = C_TEXT;
	std::string cur;
	for (size_t i = 0; i < line.size(); i++) {
		if (line[i] == '^' && i + 7 <= line.size() && is_hex6(line.c_str() + i + 1)) {
			if (!cur.empty())
				out.push_back({cur, c});
			cur.clear();
			c = hex_color(line.c_str() + i + 1);
			i += 6;
			continue;
		}
		cur += line[i];
	}
	if (!cur.empty())
		out.push_back({cur, c});
	return out;
}

std::string plain(const std::vector<Run>& runs) {
	std::string s;
	for (const Run& r : runs)
		s += r.text;
	return s;
}

// A header is a blue line with no indent, like "^0055FFStats^000000".
bool is_header(const std::string& line) {
	return line.compare(0, 7, "^0055FF") == 0;
}

// Draws (or only measures when dc draws nothing) the runs from x, wrapping at
// right; continuation lines are indented. Returns the y below the text.
int draw_runs(HDC dc, const std::vector<Run>& runs, int x, int y, int right, bool draw, int top, int bottom) {
	int cx = x, wrapX = x + 14;
	for (const Run& r : runs) {
		SetTextColor(dc, r.color);
		size_t i = 0;
		while (i < r.text.size()) {
			// one word plus the spaces after it
			size_t j = i;
			while (j < r.text.size() && r.text[j] != ' ')
				j++;
			while (j < r.text.size() && r.text[j] == ' ')
				j++;
			SIZE sz;
			GetTextExtentPoint32A(dc, r.text.c_str() + i, (int)(j - i), &sz);
			if (cx + sz.cx > right && cx > wrapX) {
				cx = wrapX;
				y += LINE_H;
			}
			if (draw && y + LINE_H > top && y < bottom)
				TextOutA(dc, cx, y, r.text.c_str() + i, (int)(j - i));
			cx += sz.cx;
			i = j;
		}
	}
	return y + LINE_H;
}

// Lays out and draws the page; returns its full height.
int draw_page(HDC dc, const std::string& text, const RECT& area, int scroll, bool draw) {
	int y = area.top + PAD - scroll;
	int left = area.left + PAD, right = area.right - PAD - SCROLL_W;
	bool first = true;
	size_t pos = 0;
	while (pos <= text.size()) {
		size_t nl = text.find('\n', pos);
		if (nl == std::string::npos)
			nl = text.size();
		std::string line = text.substr(pos, nl - pos);
		pos = nl + 1;
		if (line.empty()) {
			if (pos > text.size())
				break;
			continue;
		}
		std::vector<Run> runs = parse(line);
		if (is_header(line)) {
			if (!first)
				y += 6;
			SelectObject(dc, g_bold);
			if (draw && y + LINE_H > area.top && y < area.bottom) {
				SetTextColor(dc, C_BLUE_TEXT);
				std::string t = plain(runs);
				TextOutA(dc, left, y, t.c_str(), (int)t.size());
			}
			y += LINE_H;
			if (draw && y > area.top && y < area.bottom) {
				RECT rule = {left, y, right, y + 1};
				fill(dc, rule, C_RULE);
			}
			y += 4;
			first = false;
			continue;
		}
		first = false;
		SelectObject(dc, g_font);
		// drop the two-space indent the chat version uses
		if (!runs.empty() && runs[0].text.compare(0, 2, "  ") == 0)
			runs[0].text.erase(0, 2);
		// "a  |  b  |  c" lines are drawn as columns
		std::string flat = plain(runs);
		if (flat.find("  |  ") != std::string::npos) {
			std::vector<std::vector<Run>> cols(1);
			for (const Run& r : runs) {
				size_t s = 0;
				for (;;) {
					size_t bar = r.text.find("  |  ", s);
					std::string part = r.text.substr(s, bar == std::string::npos ? std::string::npos : bar - s);
					if (!part.empty())
						cols.back().push_back({part, r.color});
					if (bar == std::string::npos)
						break;
					cols.emplace_back();
					s = bar + 5;
				}
			}
			int n = (int)cols.size(), colW = (right - left) / n, bottomY = y + LINE_H;
			for (int c = 0; c < n; c++) {
				int cy = draw_runs(dc, cols[c], left + c * colW, y, left + (c + 1) * colW - 6, draw, area.top, area.bottom);
				if (cy > bottomY)
					bottomY = cy;
			}
			y = bottomY;
		} else {
			y = draw_runs(dc, runs, left, y, right, draw, area.top, area.bottom);
		}
	}
	return y + scroll - area.top + PAD;
}

RECT panel_rect() {
	return RECT{PANEL_X, PANEL_Y, PANEL_R, PANEL_B};
}

int max_scroll() {
	int view = PANEL_B - PANEL_Y;
	return g_contentH > view ? g_contentH - view : 0;
}

void scroll_to(int s) {
	int m = max_scroll();
	g_scroll = s < 0 ? 0 : s > m ? m : s;
	InvalidateRect(g_wnd, nullptr, FALSE);
}

// scrollbar thumb, in window coordinates; empty when everything fits
RECT thumb_rect() {
	int view = PANEL_B - PANEL_Y, m = max_scroll();
	if (m <= 0)
		return RECT{0, 0, 0, 0};
	int track = view - 4;
	int th = track * view / g_contentH;
	if (th < 24)
		th = 24;
	int ty = PANEL_Y + 2 + (track - th) * g_scroll / m;
	return RECT{PANEL_R - SCROLL_W - 2, ty, PANEL_R - 2, ty + th};
}

void text(HDC dc, const char* s, int x, int y, COLORREF c, HFONT f, int w = 300) {
	SelectObject(dc, f);
	SetTextColor(dc, c);
	RECT r = {x, y, x + w, y + 16};
	DrawTextA(dc, s, -1, &r, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
}

void paint(HDC dc) {
	RECT cr;
	GetClientRect(g_wnd, &cr);
	bool bg = g_bgDC != nullptr;
	int op = g_cfg.stOpacity;
	if (bg)
		blit_background(dc, cr, 0, 0);
	else
		fill(dc, cr, C_BODY);
	RECT title = {0, 0, cr.right, TITLE_H};
	gradient(dc, title, C_TITLE_TOP, C_TITLE_BOTTOM);
	SetBkMode(dc, TRANSPARENT);
	text(dc, "Player Statistics", 9, 4, RGB(60, 80, 130), g_bold);
	text(dc, "Player Statistics", 8, 3, RGB(255, 255, 255), g_bold);

	RECT panel = panel_rect();
	if (bg)
		glass(dc, panel, RGB(255, 255, 255), op);
	else
		fill(dc, panel, RGB(255, 255, 255));

	std::string page;
	DWORD gotAt;
	{
		Lock l(g_lock);
		page = g_text[g_tab];
		gotAt = g_gotAt[g_tab];
	}

	const char* status;
	COLORREF statusColor = C_HEADER;
	char buf[96];
	if (net_state() != NET_CONNECTED) {
		if (!g_cfg.host[0])
			status = "No server is set in legacia.ini.";
		else if (!net_short_code().empty()) {
			// typed keys don't always reach the chat box, so show the command
			snprintf(buf, sizeof(buf), "Not signed in: type @voice %s in game chat, or relog.", net_short_code().c_str());
			status = buf;
		}
		else
			status = "Signing in to the server...";
		statusColor = C_WAIT;
	} else if (g_unavailable) {
		status = "The server can't send statistics right now. Try again later.";
		statusColor = C_WAIT;
	} else if (gotAt == 0) {
		status = g_waiting && GetTickCount() - g_askedAt > TIMEOUT_MS ? "No answer from the server yet..." : "Loading...";
	} else if (g_bgNoticeAt && GetTickCount() - g_bgNoticeAt < 2500) {
		int n = (int)g_bgFiles.size(), cur = 0;
		for (int i = 0; i < n; i++)
			if (!_stricmp(g_bgFiles[i].c_str(), g_cfg.stBackground))
				cur = i + 1;
		if (!g_cfg.stBackground[0])
			snprintf(buf, sizeof(buf), n ? "Background: none (%d pictures in the folder)" : "Background: none - put pictures in the folder", n);
		else
			snprintf(buf, sizeof(buf), "Background %d/%d: %s", cur, n, g_cfg.stBackground);
		status = buf;
		statusColor = C_BLUE_TEXT;
	} else {
		DWORD age = (GetTickCount() - gotAt) / 1000;
		if (age < 2)
			snprintf(buf, sizeof(buf), "Live - updates every %u seconds", (unsigned)(REFRESH_MS / 1000));
		else
			snprintf(buf, sizeof(buf), "Updated %u seconds ago", (unsigned)age);
		status = buf;
		statusColor = age < 2 ? C_OK : C_HEADER;
	}

	if (page.empty()) {
		text(dc, gotAt ? "Nothing to show." : status, PANEL_X + PAD, PANEL_Y + PAD, C_HEADER, g_font, PANEL_R - PANEL_X - 2 * PAD);
		g_contentH = 0;
	} else {
		HRGN clip = CreateRectRgn(panel.left + 1, panel.top + 1, panel.right - 1, panel.bottom - 1);
		SelectClipRgn(dc, clip);
		g_contentH = draw_page(dc, page, panel, g_scroll, true);
		SelectClipRgn(dc, nullptr);
		DeleteObject(clip);
		if (g_scroll > max_scroll())
			g_scroll = max_scroll();
		RECT th = thumb_rect();
		if (th.right > th.left) {
			RECT track = {th.left, PANEL_Y + 2, th.right, PANEL_B - 2};
			fill(dc, track, RGB(238, 241, 247));
			gradient(dc, th, C_SEL_TOP, C_SEL_BOTTOM);
			frame(dc, th, C_SEL_BORDER);
		}
	}
	frame(dc, panel, C_BTN_BORDER);

	RECT foot = {16, cr.bottom - 30, cr.right - 16, cr.bottom - 29};
	if (bg) {
		RECT strip = {0, cr.bottom - 32, cr.right, cr.bottom};
		glass(dc, strip, C_BODY, op);
	} else {
		fill(dc, foot, C_RULE);
	}
	text(dc, status, 16, cr.bottom - 23, statusColor, g_font, W - 16 - 16 - 210);
	frame(dc, cr, C_BORDER);
}

void draw_button(const DRAWITEMSTRUCT* d) {
	int id = (int)d->CtlID;
	if (id == ID_X) {
		draw_close_x(d);
		return;
	}
	HDC dc = d->hDC;
	RECT r = d->rcItem;
	bool pressed = (d->itemState & ODS_SELECTED) != 0;
	bool tab = id >= ID_TAB && id < ID_TAB + TAB_COUNT;
	bool sel = tab && id - ID_TAB == g_tab;
	SelectObject(dc, GetStockObject(DC_PEN));
	SelectObject(dc, GetStockObject(DC_BRUSH));
	if (g_bgDC) {
		// see-through button over the picture
		POINT at = {0, 0};
		MapWindowPoints(d->hwndItem, g_wnd, &at, 1);
		blit_background(dc, r, at.x, at.y);
		RECT in = {r.left + 1, r.top + 1, r.right - 1, r.bottom - 1};
		int op = g_cfg.stOpacity;
		if (sel)
			glass(dc, in, C_SEL_BOTTOM, op);
		else
			glass(dc, in, pressed ? RGB(232, 240, 252) : RGB(255, 255, 255), op * 9 / 10);
		SelectObject(dc, GetStockObject(NULL_BRUSH));
		SetDCPenColor(dc, sel || pressed ? C_SEL_BORDER : C_BTN_BORDER);
		RoundRect(dc, r.left, r.top, r.right, r.bottom, 7, 7);
	} else if (sel) {
		fill(dc, r, C_BODY);
		SetDCPenColor(dc, C_SEL_BORDER);
		SetDCBrushColor(dc, C_SEL_TOP);
		RoundRect(dc, r.left, r.top, r.right, r.bottom, 7, 7);
		RECT g = {r.left + 2, (r.top + r.bottom) / 2, r.right - 2, r.bottom - 2};
		gradient(dc, g, C_SEL_TOP, C_SEL_BOTTOM);
	} else {
		fill(dc, r, C_BODY);
		SetDCPenColor(dc, pressed ? C_SEL_BORDER : C_BTN_BORDER);
		SetDCBrushColor(dc, pressed ? RGB(232, 240, 252) : RGB(255, 255, 255));
		RoundRect(dc, r.left, r.top, r.right, r.bottom, 7, 7);
	}
	SetBkMode(dc, TRANSPARENT);
	SelectObject(dc, sel ? g_bold : g_font);
	SetTextColor(dc, sel ? C_SEL_TEXT : C_TEXT);
	char t[64];
	GetWindowTextA(d->hwndItem, t, sizeof(t));
	RECT tr = r;
	if (tab)
		tr.left += 12;
	DrawTextA(dc, t, -1, &tr, (tab ? DT_LEFT : DT_CENTER) | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
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
	case WM_MOUSEWHEEL:
		scroll_to(g_scroll - GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA * LINE_H * 3);
		return 0;
	case WM_LBUTTONDOWN: {
		POINT pt = {(short)LOWORD(lp), (short)HIWORD(lp)};
		RECT th = thumb_rect();
		if (th.right > th.left && pt.x >= th.left - 2 && pt.x <= th.right + 2 && pt.y >= PANEL_Y && pt.y < PANEL_B) {
			if (pt.y >= th.top && pt.y < th.bottom) {
				g_dragThumb = true;
				g_dragY = pt.y;
				g_dragScroll = g_scroll;
				SetCapture(hw);
			} else {
				// click on the track: page up or down
				int view = PANEL_B - PANEL_Y - LINE_H;
				scroll_to(g_scroll + (pt.y < th.top ? -view : view));
			}
		}
		SetFocus(hw); // so the mouse wheel reaches us
		return 0;
	}
	case WM_MOUSEMOVE:
		if (g_dragThumb) {
			int view = PANEL_B - PANEL_Y, track = view - 4;
			RECT th = thumb_rect();
			int free = track - (th.bottom - th.top);
			if (free > 0)
				scroll_to(g_dragScroll + ((short)HIWORD(lp) - g_dragY) * max_scroll() / free);
		}
		return 0;
	case WM_LBUTTONUP:
		if (g_dragThumb) {
			g_dragThumb = false;
			ReleaseCapture();
		}
		return 0;
	case WM_KEYDOWN:
		if (wp == VK_ESCAPE)
			ShowWindow(hw, SW_HIDE);
		else if (wp == VK_UP || wp == VK_DOWN)
			scroll_to(g_scroll + (wp == VK_UP ? -LINE_H : LINE_H));
		else if (wp == VK_PRIOR || wp == VK_NEXT)
			scroll_to(g_scroll + (wp == VK_PRIOR ? -1 : 1) * (PANEL_B - PANEL_Y - LINE_H));
		return 0;
	case WM_TIMER:
		if (wp == TIMER_REFRESH && IsWindowVisible(hw)) {
			const char* caption = net_short_code().empty() ? "Refresh" : "Sign in";
			char cur[16];
			GetWindowTextA(g_refreshBtn, cur, sizeof(cur));
			if (strcmp(cur, caption))
				SetWindowTextA(g_refreshBtn, caption), InvalidateRect(g_refreshBtn, nullptr, FALSE);
			DWORD now = GetTickCount(), gotAt;
			{
				Lock l(g_lock);
				gotAt = g_gotAt[g_tab];
			}
			if (now - g_askedAt >= REFRESH_MS && (gotAt == 0 || now - gotAt >= REFRESH_MS))
				ask();
			else
				InvalidateRect(hw, nullptr, FALSE); // status line ages
		}
		return 0;
	case WM_STATS:
		InvalidateRect(hw, nullptr, FALSE);
		return 0;
	case WM_DRAWITEM:
		draw_button((const DRAWITEMSTRUCT*)lp);
		return TRUE;
	case WM_COMMAND: {
		int id = LOWORD(wp), code = HIWORD(wp);
		bool clicked = code == BN_CLICKED || code == BN_DOUBLECLICKED;
		if (!clicked)
			return 0;
		if (id == ID_X)
			ShowWindow(hw, SW_HIDE);
		else if (id == ID_REFRESH && !net_short_code().empty()) {
			// the server can't tell which player this is: sign in through
			// the game chat, which needs the game to have the keyboard
			SetForegroundWindow(g_gameWnd);
			SetFocus(g_gameWnd);
			ui_type_voice_command(net_short_code());
			return 0;
		} else if (id == ID_REFRESH)
			ask();
		else if (id == ID_BG_PREV || id == ID_BG_NEXT)
			cycle_background(id == ID_BG_NEXT ? 1 : -1);
		else if (id == ID_BG_FOLDER)
			background_menu();
		else if (id >= ID_TAB && id < ID_TAB + TAB_COUNT)
			select_tab(id - ID_TAB);
		SetFocus(hw);
		return 0;
	}
	case WM_CLOSE:
		ShowWindow(hw, SW_HIDE);
		return 0;
	}
	return DefWindowProcA(hw, msg, wp, lp);
}

HWND child(const char* text, int x, int y, int w, int h, int id) {
	HWND c = CreateWindowExA(0, "BUTTON", text, WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, x, y, w, h, g_wnd,
	                         (HMENU)(INT_PTR)id, g_module, nullptr);
	SendMessageA(c, WM_SETFONT, (WPARAM)g_font, TRUE);
	return c;
}

} // namespace

void stats_ui_create() {
	InitializeCriticalSection(&g_lock);
	WNDCLASSA wc = {};
	wc.lpfnWndProc = proc;
	wc.hInstance = g_module;
	wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
	wc.hbrBackground = g_bodyBrush;
	wc.lpszClassName = "LegaciaStats";
	RegisterClassA(&wc);
	g_wnd = CreateWindowExA(WS_EX_TOOLWINDOW, "LegaciaStats", "Player Statistics", WS_POPUP | WS_CLIPCHILDREN, 0, 0, W, H,
	                        g_gameWnd, nullptr, g_module, nullptr);
	SetWindowRgn(g_wnd, CreateRoundRectRgn(0, 0, W + 1, H + 1, 7, 7), FALSE);

	child("", W - 21, 3, 16, 16, ID_X);
	for (int i = 0; i < TAB_COUNT; i++)
		child(TABS[i], TAB_X, TAB_Y + i * (TAB_H + TAB_GAP), TAB_W, TAB_H, ID_TAB + i);
	g_refreshBtn = child("Refresh", W - 16 - 70, H - 27, 70, 22, ID_REFRESH);
	child(">", W - 16 - 70 - 6 - 24, H - 27, 24, 22, ID_BG_NEXT);
	child("<", W - 16 - 70 - 6 - 24 - 2 - 24, H - 27, 24, 22, ID_BG_PREV);
	child("Background", W - 16 - 70 - 6 - 24 - 2 - 24 - 2 - 80, H - 27, 80, 22, ID_BG_FOLDER);
	Gdiplus::GdiplusStartupInput gsi;
	Gdiplus::GdiplusStartup(&g_gdiplus, &gsi, nullptr);
	scan_backgrounds();
	load_background();
	SetTimer(g_wnd, TIMER_REFRESH, 500, nullptr);
}

void stats_ui_toggle() {
	if (IsWindowVisible(g_wnd)) {
		ShowWindow(g_wnd, SW_HIDE);
		return;
	}
	if (!g_placed) {
		RECT cr;
		GetClientRect(g_gameWnd, &cr);
		POINT c = {cr.right / 2, cr.bottom / 2};
		ClientToScreen(g_gameWnd, &c);
		SetWindowPos(g_wnd, HWND_TOP, c.x - W / 2, c.y - H / 2, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
	}
	g_unavailable = false;
	ShowWindow(g_wnd, SW_SHOW);
	SetForegroundWindow(g_wnd);
	SetFocus(g_wnd);
	ask();
}

HWND stats_ui_window() {
	return g_wnd;
}

void stats_ui_on_page(uint8_t page, const std::string& text) {
	if (page >= TAB_COUNT)
		return;
	{
		Lock l(g_lock);
		g_text[page] = text;
		g_gotAt[page] = GetTickCount();
		if (g_gotAt[page] == 0)
			g_gotAt[page] = 1;
	}
	g_waiting = false;
	g_unavailable = false;
	if (g_wnd)
		PostMessageA(g_wnd, WM_STATS, 0, 0);
}

void stats_ui_on_unavailable(uint8_t page) {
	(void)page;
	g_waiting = false;
	g_unavailable = true;
	if (g_wnd)
		PostMessageA(g_wnd, WM_STATS, 0, 0);
}
