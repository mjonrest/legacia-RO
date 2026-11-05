// Voice bar (bottom right of the game window), Voice Settings window
// (Ctrl+Shift+V, or right-click the bar) and the small "who is talking"
// overlay on top of the game window.
// The window is drawn in the game's own style (blue title bar, light body,
// flat buttons) and opens at the bottom right of the game window.
#include "ui_style.h"

#include <commctrl.h>
#include <stdio.h>

enum {
	ID_TAB_VOICE = 100, ID_TAB_PLAYERS, ID_TAB_DEVICES,
	ID_MODE_PTT, ID_MODE_OPEN, ID_PTT_KEY,
	ID_CH_NORMAL, ID_CH_PARTY, ID_CH_GUILD, ID_CH_ROOM, ID_CH_WHISPER,
	ID_LEVEL, ID_MIC_VOL, ID_SPK_VOL, ID_MIC_VOL_TXT, ID_SPK_VOL_TXT,
	ID_VAD, ID_SENS, ID_SENS_TXT, ID_OVERLAY, ID_ENABLED,
	ID_PLAYERS, ID_MUTE, ID_PLAYER_VOL,
	ID_IN_DEV, ID_OUT_DEV,
	ID_STATUS, ID_CLOSE, ID_X,
	ID_BAR_SPEAKER = 300, ID_BAR_MIC, ID_BAR_CH = 310, // ID_BAR_CH + channel
	ID_PROMPT_EDIT = 320, ID_PROMPT_OK, ID_PROMPT_CANCEL,
};

static const int WIN_W = 340, WIN_H = 440, TITLE_H = 22;
static const UINT_PTR TIMER_POLL = 1;

struct Header {
	int page; // -1 = always shown
	int y;
	const char* text;
};
static const Header g_headers[] = {
	{0, 62, "Mic Mode"}, {0, 110, "PTT Key"}, {0, 158, "Talk To"}, {0, 206, "Volume"}, {0, 302, "Audio"},
	{1, 62, "Players"},
	{2, 62, "Microphone"}, {2, 222, "Speaker"},
};

static HWND g_settings, g_overlay, g_bar, g_prompt, g_promptEdit;
HFONT g_font, g_bold;
HBRUSH g_bodyBrush, g_whiteBrush;
static int g_page;
static std::vector<HWND> g_pages[3];
static std::vector<HWND> g_buttons; // owner-drawn, repainted on every settings change
static bool g_capturingKey;
static bool g_toggleDown;
static bool g_userMoved; // once dragged, the window keeps its place
static int g_level;
static std::vector<uint32_t> g_playerIds;
static DWORD g_lastSlowUpdate;

// ---------------------------------------------------------------- helpers

static std::string key_name(int vk) {
	switch (vk) {
	case VK_XBUTTON1: return "Mouse 4";
	case VK_XBUTTON2: return "Mouse 5";
	case VK_MBUTTON: return "Middle mouse";
	}
	char buf[64] = "";
	UINT sc = MapVirtualKeyA(vk, MAPVK_VK_TO_VSC);
	if (sc && GetKeyNameTextA((LONG)(sc << 16), buf, sizeof(buf)) > 0)
		return buf;
	snprintf(buf, sizeof(buf), "Key %d", vk);
	return buf;
}

static HWND add(int page, const char* cls, const char* text, DWORD style, int x, int y, int w, int h, int id) {
	HWND hw = CreateWindowExA(0, cls, text, WS_CHILD | style, x, y, w, h, g_settings, (HMENU)(INT_PTR)id, g_module, nullptr);
	SendMessageA(hw, WM_SETFONT, (WPARAM)g_font, TRUE);
	if (page >= 0)
		g_pages[page].push_back(hw);
	else
		ShowWindow(hw, SW_SHOW);
	return hw;
}

static HWND button(int page, const char* text, int x, int y, int w, int h, int id) {
	HWND hw = add(page, "BUTTON", text, BS_OWNERDRAW, x, y, w, h, id);
	g_buttons.push_back(hw);
	return hw;
}

static HWND label(int page, const char* text, int x, int y, int w, int id = 0) {
	return add(page, "STATIC", text, SS_LEFTNOWORDWRAP, x, y, w, 16, id);
}

static HWND slider(int page, int x, int y, int w, int id, int lo, int hi) {
	HWND hw = add(page, TRACKBAR_CLASSA, "", TBS_NOTICKS | TBS_BOTH, x, y, w, 24, id);
	SendMessageA(hw, TBM_SETRANGE, TRUE, MAKELPARAM(lo, hi));
	return hw;
}

static HWND item(int id) { return GetDlgItem(g_settings, id); }

static void set_text(int id, const std::string& s) {
	char cur[256];
	GetWindowTextA(item(id), cur, sizeof(cur));
	if (s != cur)
		SetWindowTextA(item(id), s.c_str());
}

void fill(HDC dc, const RECT& r, COLORREF c) {
	SetDCBrushColor(dc, c);
	FillRect(dc, &r, (HBRUSH)GetStockObject(DC_BRUSH));
}

void frame(HDC dc, const RECT& r, COLORREF c) {
	SetDCBrushColor(dc, c);
	FrameRect(dc, &r, (HBRUSH)GetStockObject(DC_BRUSH));
}

static COLORREF mix(COLORREF a, COLORREF b, int i, int n) {
	if (n <= 1)
		return a;
	return RGB(GetRValue(a) + (GetRValue(b) - GetRValue(a)) * i / (n - 1),
	           GetGValue(a) + (GetGValue(b) - GetGValue(a)) * i / (n - 1),
	           GetBValue(a) + (GetBValue(b) - GetBValue(a)) * i / (n - 1));
}

// top-to-bottom gradient
void gradient(HDC dc, const RECT& r, COLORREF top, COLORREF bottom) {
	int h = r.bottom - r.top;
	for (int i = 0; i < h; i++) {
		RECT line = {r.left, r.top + i, r.right, r.top + i + 1};
		fill(dc, line, mix(top, bottom, i, h));
	}
}

static const char* channel_name(int ch) {
	switch (ch) {
	case CHANNEL_PARTY: return "party";
	case CHANNEL_GUILD: return "guild";
	case CHANNEL_ROOM: return "room";
	case CHANNEL_WHISPER: return "whisper";
	}
	return "normal";
}

// ---------------------------------------------------------------- drawing

// buttons that show an on/off state
static bool button_on(int id) {
	switch (id) {
	case ID_TAB_VOICE: case ID_TAB_PLAYERS: case ID_TAB_DEVICES: return g_page == id - ID_TAB_VOICE;
	case ID_MODE_PTT: return g_cfg.mode == MODE_PTT;
	case ID_MODE_OPEN: return g_cfg.mode == MODE_OPEN;
	case ID_CH_NORMAL: case ID_CH_PARTY: case ID_CH_GUILD: case ID_CH_ROOM: case ID_CH_WHISPER:
		return g_cfg.channel == id - ID_CH_NORMAL;
	case ID_PTT_KEY: return g_capturingKey;
	case ID_VAD: return g_cfg.voiceDetect != 0;
	case ID_OVERLAY: return g_cfg.overlay != 0;
	case ID_ENABLED: return g_cfg.enabled != 0;
	}
	return false;
}

static bool is_toggle(int id) {
	return id == ID_VAD || id == ID_OVERLAY || id == ID_ENABLED;
}

void draw_close_x(const DRAWITEMSTRUCT* d) {
	HDC dc = d->hDC;
	RECT r = d->rcItem;
	gradient(dc, r, C_TITLE_TOP, C_TITLE_BOTTOM); // the circle sits on the title bar
	bool down = (d->itemState & ODS_SELECTED) != 0;
	SelectObject(dc, GetStockObject(DC_PEN));
	SelectObject(dc, GetStockObject(DC_BRUSH));
	SetDCPenColor(dc, RGB(70, 100, 170));
	SetDCBrushColor(dc, down ? RGB(210, 222, 245) : RGB(255, 255, 255));
	Ellipse(dc, r.left + 1, r.top + 1, r.right - 1, r.bottom - 1);
	SetDCPenColor(dc, RGB(70, 100, 170));
	int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
	for (int o = 0; o < 2; o++) {
		MoveToEx(dc, cx - 3 + o, cy - 3, nullptr);
		LineTo(dc, cx + 4 + o, cy + 4);
		MoveToEx(dc, cx + 3 + o, cy - 3, nullptr);
		LineTo(dc, cx - 4 + o, cy + 4);
	}
}

static void draw_button(const DRAWITEMSTRUCT* d) {
	int id = (int)d->CtlID;
	if (id == ID_X) {
		draw_close_x(d);
		return;
	}
	HDC dc = d->hDC;
	RECT r = d->rcItem;
	bool disabled = (d->itemState & ODS_DISABLED) != 0;
	bool pressed = (d->itemState & ODS_SELECTED) != 0;
	bool on = button_on(id);
	bool lit = (on && !is_toggle(id)) || pressed;

	gradient(dc, r, lit ? C_SEL_TOP : C_BTN_TOP, lit ? C_SEL_BOTTOM : C_BTN_BOTTOM);
	frame(dc, r, lit ? C_SEL_BORDER : C_BTN_BORDER);

	char text[128];
	GetWindowTextA(d->hwndItem, text, sizeof(text));
	SetBkMode(dc, TRANSPARENT);
	SelectObject(dc, lit ? g_bold : g_font);
	SetTextColor(dc, disabled ? C_DISABLED : lit ? C_SEL_TEXT : C_TEXT);

	if (is_toggle(id)) {
		// small square marker, filled when on
		int cy = (r.top + r.bottom) / 2;
		RECT box = {r.left + 8, cy - 5, r.left + 18, cy + 5};
		frame(dc, box, disabled ? C_DISABLED : C_SEL_BORDER);
		if (on) {
			RECT dot = {box.left + 2, box.top + 2, box.right - 2, box.bottom - 2};
			fill(dc, dot, disabled ? C_DISABLED : RGB(60, 100, 200));
		}
		RECT tr = {r.left + 24, r.top, r.right - 4, r.bottom};
		DrawTextA(dc, text, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
	} else {
		DrawTextA(dc, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
	}
}

// list item data flags (players list)
enum { ITEM_TALKING = 1, ITEM_MUTED = 2 };

static void draw_list_item(const DRAWITEMSTRUCT* d) {
	HDC dc = d->hDC;
	RECT r = d->rcItem;
	fill(dc, r, RGB(255, 255, 255));
	if (d->itemID == (UINT)-1)
		return;
	bool sel = (d->itemState & ODS_SELECTED) != 0;
	if (sel)
		fill(dc, r, RGB(232, 240, 252));
	char text[256];
	SendMessageA(d->hwndItem, LB_GETTEXT, d->itemID, (LPARAM)text);
	LPARAM flags = d->CtlID == ID_PLAYERS ? SendMessageA(d->hwndItem, LB_GETITEMDATA, d->itemID, 0) : 0;
	std::string line = std::string(sel ? "> " : "   ") + text;
	COLORREF c = sel ? C_BLUE_TEXT : C_TEXT;
	if (flags & ITEM_MUTED)
		c = C_DISABLED;
	else if (flags & ITEM_TALKING)
		c = C_OK;
	SetBkMode(dc, TRANSPARENT);
	SetTextColor(dc, c);
	SelectObject(dc, sel ? g_bold : g_font);
	RECT tr = {r.left + 4, r.top, r.right - 4, r.bottom};
	DrawTextA(dc, line.c_str(), -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

static void draw_level(const DRAWITEMSTRUCT* d) {
	HDC dc = d->hDC;
	RECT r = d->rcItem;
	fill(dc, r, RGB(255, 255, 255));
	frame(dc, r, C_BTN_BORDER);
	int w = (r.right - r.left - 2) * g_level / 100;
	if (w > 0) {
		RECT bar = {r.left + 1, r.top + 1, r.left + 1 + w, r.bottom - 1};
		bool hot = g_level > 85;
		gradient(dc, bar, hot ? RGB(250, 190, 120) : RGB(150, 220, 140), hot ? RGB(220, 120, 60) : RGB(70, 170, 70));
	}
}

static void paint_window(HDC dc) {
	RECT cr;
	GetClientRect(g_settings, &cr);

	RECT title = {0, 0, cr.right, TITLE_H};
	gradient(dc, title, C_TITLE_TOP, C_TITLE_BOTTOM);
	SetBkMode(dc, TRANSPARENT);
	SelectObject(dc, g_bold);
	RECT tt = {9, 1, cr.right - 30, TITLE_H + 1};
	SetTextColor(dc, RGB(60, 80, 130));
	DrawTextA(dc, "Voice Settings", -1, &tt, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
	OffsetRect(&tt, -1, -1);
	SetTextColor(dc, RGB(255, 255, 255));
	DrawTextA(dc, "Voice Settings", -1, &tt, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

	// section headers with a rule after the text
	for (const Header& h : g_headers) {
		if (h.page != -1 && h.page != g_page)
			continue;
		SetTextColor(dc, C_HEADER);
		SIZE sz;
		GetTextExtentPoint32A(dc, h.text, (int)strlen(h.text), &sz);
		TextOutA(dc, 10, h.y, h.text, (int)strlen(h.text));
		RECT rule = {10 + sz.cx + 6, h.y + sz.cy / 2, cr.right - 10, h.y + sz.cy / 2 + 1};
		fill(dc, rule, C_RULE);
	}

	// thin line above the status bar
	RECT foot = {10, cr.bottom - 42, cr.right - 10, cr.bottom - 41};
	fill(dc, foot, C_RULE);

	// borders around the visible lists
	for (int id : {ID_PLAYERS, ID_IN_DEV, ID_OUT_DEV}) {
		HWND lb = item(id);
		if (!lb || !IsWindowVisible(lb))
			continue;
		RECT lr;
		GetWindowRect(lb, &lr);
		MapWindowPoints(nullptr, g_settings, (POINT*)&lr, 2);
		InflateRect(&lr, 1, 1);
		frame(dc, lr, C_BTN_BORDER);
	}

	frame(dc, cr, C_BORDER);
}

// ---------------------------------------------------------------- state

static void redraw_bar();
static void set_channel(int ch);
static void open_whisper_prompt();

static void redraw_buttons() {
	for (HWND hw : g_buttons)
		InvalidateRect(hw, nullptr, FALSE);
}

static void show_page(int page) {
	g_page = page;
	for (int p = 0; p < 3; p++)
		for (HWND hw : g_pages[p])
			ShowWindow(hw, p == page ? SW_SHOW : SW_HIDE);
	InvalidateRect(g_settings, nullptr, TRUE);
	redraw_buttons();
}

static void sync_controls() {
	set_text(ID_PTT_KEY, g_capturingKey ? "Press a key or mouse button..." : key_name(g_cfg.pttKey));
	SendMessageA(item(ID_MIC_VOL), TBM_SETPOS, TRUE, g_cfg.micVolume);
	SendMessageA(item(ID_SPK_VOL), TBM_SETPOS, TRUE, g_cfg.speakerVolume);
	SendMessageA(item(ID_SENS), TBM_SETPOS, TRUE, 80 - g_cfg.sensitivity);
	set_text(ID_MIC_VOL_TXT, std::to_string(g_cfg.micVolume) + "%");
	set_text(ID_SPK_VOL_TXT, std::to_string(g_cfg.speakerVolume) + "%");
	set_text(ID_SENS_TXT, "-" + std::to_string(g_cfg.sensitivity) + " dB");
	EnableWindow(item(ID_PTT_KEY), g_cfg.mode == MODE_PTT);
	EnableWindow(item(ID_VAD), g_cfg.mode == MODE_OPEN);
	EnableWindow(item(ID_SENS), g_cfg.mode == MODE_OPEN && g_cfg.voiceDetect);
	redraw_buttons();
	redraw_bar();
}

static void fill_devices() {
	for (int which = 0; which < 2; which++) {
		HWND lb = item(which == 0 ? ID_IN_DEV : ID_OUT_DEV);
		SendMessageA(lb, LB_RESETCONTENT, 0, 0);
		SendMessageA(lb, LB_ADDSTRING, 0, (LPARAM) "System default");
		std::vector<std::string> devs = which == 0 ? audio_input_devices() : audio_output_devices();
		for (const std::string& d : devs)
			SendMessageA(lb, LB_ADDSTRING, 0, (LPARAM)d.c_str());
		int sel = (which == 0 ? g_cfg.inDevice : g_cfg.outDevice) + 1;
		SendMessageA(lb, LB_SETCURSEL, sel < (int)devs.size() + 1 ? sel : 0, 0);
	}
}

static void fill_players() {
	HWND lb = item(ID_PLAYERS);
	std::vector<SpeakerInfo> list = audio_speakers();
	int sel = (int)SendMessageA(lb, LB_GETCURSEL, 0, 0);
	uint32_t selId = sel >= 0 && sel < (int)g_playerIds.size() ? g_playerIds[sel] : 0;
	int top = (int)SendMessageA(lb, LB_GETTOPINDEX, 0, 0);
	SendMessageA(lb, WM_SETREDRAW, FALSE, 0);
	SendMessageA(lb, LB_RESETCONTENT, 0, 0);
	g_playerIds.clear();
	int newSel = -1;
	for (const SpeakerInfo& s : list) {
		char line[128];
		snprintf(line, sizeof(line), "%s  (%s)%s%s", s.name.c_str(), channel_name(s.channel), s.muted ? "  [muted]" : "",
		         s.volume != 100 ? ("  " + std::to_string(s.volume) + "%").c_str() : "");
		int idx = (int)SendMessageA(lb, LB_ADDSTRING, 0, (LPARAM)line);
		SendMessageA(lb, LB_SETITEMDATA, idx, (s.talking ? ITEM_TALKING : 0) | (s.muted ? ITEM_MUTED : 0));
		if (s.charId == selId)
			newSel = (int)g_playerIds.size();
		g_playerIds.push_back(s.charId);
	}
	if (newSel >= 0) {
		SendMessageA(lb, LB_SETCURSEL, newSel, 0);
		for (const SpeakerInfo& s : list)
			if (s.charId == selId)
				SendMessageA(item(ID_PLAYER_VOL), TBM_SETPOS, TRUE, s.volume);
	}
	SendMessageA(lb, LB_SETTOPINDEX, top, 0);
	SendMessageA(lb, WM_SETREDRAW, TRUE, 0);
	InvalidateRect(lb, nullptr, TRUE);
}

static uint32_t selected_player() {
	int sel = (int)SendMessageA(item(ID_PLAYERS), LB_GETCURSEL, 0, 0);
	return sel >= 0 && sel < (int)g_playerIds.size() ? g_playerIds[sel] : 0;
}

// bottom right corner of the game screen, just above the bottom edge
static void place_bottom_right() {
	RECT cr;
	GetClientRect(g_gameWnd, &cr);
	POINT br = {cr.right, cr.bottom};
	ClientToScreen(g_gameWnd, &br);
	int x = br.x - WIN_W - 8, y = br.y - WIN_H - 48; // leaves room for the voice bar
	POINT tl = {0, 0};
	ClientToScreen(g_gameWnd, &tl);
	if (x < tl.x)
		x = tl.x;
	if (y < tl.y)
		y = tl.y;
	SetWindowPos(g_settings, HWND_TOP, x, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
}

static void toggle_settings() {
	if (IsWindowVisible(g_settings)) {
		ShowWindow(g_settings, SW_HIDE);
		return;
	}
	if (!g_userMoved)
		place_bottom_right();
	fill_devices();
	sync_controls();
	ShowWindow(g_settings, SW_SHOW);
	SetForegroundWindow(g_settings);
}

// ---------------------------------------------------------------- overlay

static const COLORREF OVERLAY_KEY = RGB(255, 0, 255);

static void update_overlay() {
	if (!g_overlay)
		return;
	std::vector<std::string> lines;
	std::string code = g_cfg.enabled ? net_short_code() : "";
	if (!code.empty())
		lines.push_back("Voice: click the mic, or type @voice " + code);
	if (g_cfg.overlay && g_cfg.enabled && net_state() == NET_CONNECTED) {
		if (audio_transmitting())
			lines.push_back(std::string("(you) - ") +
			                (g_cfg.channel == CHANNEL_WHISPER ? std::string("whisper to ") + g_cfg.whisper : channel_name(g_cfg.channel)));
		for (const SpeakerInfo& s : audio_speakers())
			if (s.talking && !s.muted)
				lines.push_back(s.name + " - " + channel_name(s.channel));
	}
	HWND fg = GetForegroundWindow();
	bool gameActive = fg == g_gameWnd || fg == g_settings || fg == g_prompt;
	if (lines.empty() || !gameActive || IsIconic(g_gameWnd)) {
		if (IsWindowVisible(g_overlay))
			ShowWindow(g_overlay, SW_HIDE);
		return;
	}
	std::string text;
	for (const std::string& l : lines)
		text += l + "\n";
	SetWindowTextA(g_overlay, text.c_str());

	RECT cr;
	GetClientRect(g_gameWnd, &cr);
	POINT pt = {cr.right - 270, 190};
	ClientToScreen(g_gameWnd, &pt);
	int h = 8 + 16 * (int)lines.size();
	SetWindowPos(g_overlay, HWND_TOP, pt.x, pt.y, 260, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
	InvalidateRect(g_overlay, nullptr, TRUE);
}

static LRESULT CALLBACK overlay_proc(HWND hw, UINT msg, WPARAM wp, LPARAM lp) {
	if (msg == WM_PAINT) {
		PAINTSTRUCT ps;
		HDC dc = BeginPaint(hw, &ps);
		RECT r;
		GetClientRect(hw, &r);
		HBRUSH key = CreateSolidBrush(OVERLAY_KEY);
		FillRect(dc, &r, key);
		DeleteObject(key);
		char text[1024];
		GetWindowTextA(hw, text, sizeof(text));
		SetBkMode(dc, TRANSPARENT);
		SelectObject(dc, g_bold);
		int y = 4;
		for (char* line = strtok(text, "\n"); line; line = strtok(nullptr, "\n")) {
			// dark outline so it reads on any map
			SetTextColor(dc, RGB(0, 0, 0));
			for (int dx = -1; dx <= 1; dx++)
				for (int dy = -1; dy <= 1; dy++)
					TextOutA(dc, 4 + dx, y + dy, line, (int)strlen(line));
			// green: you, cyan: sign-in hint, yellow: others talking
			SetTextColor(dc, line[0] == '(' ? RGB(120, 255, 120) : line[0] == 'V' && strncmp(line, "Voice:", 6) == 0 ? RGB(140, 220, 255) : RGB(255, 230, 120));
			TextOutA(dc, 4, y, line, (int)strlen(line));
			y += 16;
		}
		EndPaint(hw, &ps);
		return 0;
	}
	if (msg == WM_NCHITTEST)
		return HTTRANSPARENT;
	return DefWindowProcA(hw, msg, wp, lp);
}

// ---------------------------------------------------------------- voice bar

// Small always-on bar at the bottom right of the game window, like the chat
// tabs: speaker (hear nobody), the talk channels, and the mic (mute).
// Right-click it to open the Voice Settings window.

static const int BAR_H = 32, BAR_PAD = 5, BAR_GAP = 4, BAR_ICON_W = 26, BAR_MARGIN = 10;
struct BarButton {
	int id;
	const char* text;
	int w;
};
static const BarButton g_barLayout[] = {
	{ID_BAR_SPEAKER, "", BAR_ICON_W},
	{ID_BAR_CH + CHANNEL_NORMAL, "Normal", 66},
	{ID_BAR_CH + CHANNEL_PARTY, "Party", 58},
	{ID_BAR_CH + CHANNEL_GUILD, "Guild", 58},
	{ID_BAR_CH + CHANNEL_ROOM, "Room", 58},
	{ID_BAR_CH + CHANNEL_WHISPER, "Whisper", 66},
	{ID_BAR_MIC, "", BAR_ICON_W},
};
static int g_barW;
static int g_barPressed = -1; // button index held down with the mouse
static bool g_barMoved;       // dragged by the player: keep that offset from the corner
static POINT g_barOffset;

static const int BAR_BUTTONS = sizeof(g_barLayout) / sizeof(g_barLayout[0]);

static RECT bar_button_rect(int i) {
	int x = BAR_PAD;
	for (int k = 0; k < i; k++)
		x += g_barLayout[k].w + BAR_GAP;
	return RECT{x, (BAR_H - 22) / 2, x + g_barLayout[i].w, (BAR_H - 22) / 2 + 22};
}

static int bar_button_at(POINT pt) {
	for (int i = 0; i < BAR_BUTTONS; i++) {
		RECT r = bar_button_rect(i);
		if (PtInRect(&r, pt))
			return i;
	}
	return -1;
}

static void open_whisper_prompt();

static void redraw_bar() {
	if (g_bar)
		InvalidateRect(g_bar, nullptr, FALSE);
}

static bool bar_on(int id) {
	if (id >= ID_BAR_CH && id < ID_BAR_CH + CHANNEL_COUNT)
		return g_cfg.channel == id - ID_BAR_CH;
	return false;
}

static void draw_speaker_icon(HDC dc, const RECT& r, bool off) {
	int cx = (r.left + r.right) / 2 - 3, cy = (r.top + r.bottom) / 2;
	COLORREF c = off ? RGB(150, 150, 160) : RGB(70, 100, 170);
	SelectObject(dc, GetStockObject(DC_PEN));
	SelectObject(dc, GetStockObject(DC_BRUSH));
	SetDCPenColor(dc, c);
	SetDCBrushColor(dc, c);
	POINT body[] = {{cx - 6, cy - 3}, {cx - 3, cy - 3}, {cx + 2, cy - 7}, {cx + 2, cy + 7}, {cx - 3, cy + 3}, {cx - 6, cy + 3}};
	Polygon(dc, body, 6);
	if (off) {
		SetDCPenColor(dc, RGB(200, 60, 60));
		for (int o = 0; o < 2; o++) {
			MoveToEx(dc, cx - 7 + o, cy - 8, nullptr);
			LineTo(dc, cx + 10 + o, cy + 8);
		}
		return;
	}
	SelectObject(dc, GetStockObject(NULL_BRUSH));
	Arc(dc, cx - 2, cy - 5, cx + 8, cy + 5, cx + 4, cy + 6, cx + 4, cy - 6);
	Arc(dc, cx - 4, cy - 9, cx + 12, cy + 9, cx + 6, cy + 10, cx + 6, cy - 10);
}

static void draw_mic_icon(HDC dc, const RECT& r, bool muted, bool talking, bool connected) {
	int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2 - 2;
	COLORREF c = !connected ? RGB(150, 150, 160) : talking ? RGB(40, 170, 60) : RGB(70, 100, 170);
	SelectObject(dc, GetStockObject(DC_PEN));
	SelectObject(dc, GetStockObject(DC_BRUSH));
	SetDCPenColor(dc, c);
	SetDCBrushColor(dc, c);
	RoundRect(dc, cx - 3, cy - 7, cx + 4, cy + 4, 7, 7);
	SelectObject(dc, GetStockObject(NULL_BRUSH));
	Arc(dc, cx - 6, cy - 4, cx + 7, cy + 7, cx - 6, cy + 1, cx + 7, cy + 1);
	MoveToEx(dc, cx, cy + 7, nullptr);
	LineTo(dc, cx, cy + 10);
	MoveToEx(dc, cx - 3, cy + 10, nullptr);
	LineTo(dc, cx + 4, cy + 10);
	if (muted) {
		SetDCPenColor(dc, RGB(200, 60, 60));
		for (int o = 0; o < 2; o++) {
			MoveToEx(dc, cx - 7 + o, cy - 7, nullptr);
			LineTo(dc, cx + 8 + o, cy + 10);
		}
	}
}

static void draw_bar_button(HDC dc, int i, bool pressed) {
	RECT r = bar_button_rect(i);
	int id = g_barLayout[i].id;

	if (id == ID_BAR_SPEAKER || id == ID_BAR_MIC) {
		fill(dc, r, C_BODY);
		if (pressed)
			gradient(dc, r, C_SEL_TOP, C_SEL_BOTTOM);
		if (id == ID_BAR_SPEAKER)
			draw_speaker_icon(dc, r, g_cfg.deafen != 0);
		else
			draw_mic_icon(dc, r, g_cfg.micMuted != 0, audio_transmitting(), g_cfg.enabled && net_state() == NET_CONNECTED);
		return;
	}

	bool lit = bar_on(id) || pressed;
	gradient(dc, r, lit ? C_SEL_TOP : C_BTN_TOP, lit ? C_SEL_BOTTOM : C_BTN_BOTTOM);
	frame(dc, r, lit ? C_SEL_BORDER : C_BTN_BORDER);
	SetBkMode(dc, TRANSPARENT);
	SelectObject(dc, g_font);
	// the selected channel is green like the game's chat tabs
	SetTextColor(dc, lit ? RGB(30, 130, 50) : C_TEXT);
	DrawTextA(dc, g_barLayout[i].text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

static void set_channel(int ch) {
	g_cfg.channel = ch;
	if (ch == CHANNEL_WHISPER && !g_cfg.whisper[0])
		open_whisper_prompt();
	config_save();
	sync_controls();
}

// Types "@voice CODE" into the game's chat and sends it, for when the voice
// server could not tell on its own which player this is (two clients on one
// connection). The bar never has focus, so the game gets the keys. The text
// also goes to the clipboard, in case the game ignores posted keys: Ctrl+V
// and Enter then do the same.
void ui_type_voice_command(const std::string& code) {
	std::string cmd = "@voice " + code;
	if (OpenClipboard(g_bar)) {
		EmptyClipboard();
		HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, cmd.size() + 1);
		if (mem) {
			memcpy(GlobalLock(mem), cmd.c_str(), cmd.size() + 1);
			GlobalUnlock(mem);
			if (!SetClipboardData(CF_TEXT, mem))
				GlobalFree(mem);
		}
		CloseClipboard();
	}
	// characters only, so nothing reaches the skill hotkeys
	for (char c : cmd)
		PostMessageA(g_gameWnd, WM_CHAR, (WPARAM)(unsigned char)c, 1);
	UINT scan = MapVirtualKeyA(VK_RETURN, 0);
	// the game's TranslateMessage turns this into the '\r' character
	PostMessageA(g_gameWnd, WM_KEYDOWN, VK_RETURN, 1 | (scan << 16));
	PostMessageA(g_gameWnd, WM_KEYUP, VK_RETURN, 1 | (scan << 16) | (1u << 30) | (1u << 31));
	log_line("voice: typed the sign-in command into the game");
}

static void bar_click(int id, bool right) {
	if (right) {
		if (id == ID_BAR_CH + CHANNEL_WHISPER) {
			g_cfg.channel = CHANNEL_WHISPER;
			open_whisper_prompt();
			sync_controls();
		} else {
			toggle_settings();
		}
		return;
	}
	if (id == ID_BAR_SPEAKER)
		g_cfg.deafen = !g_cfg.deafen;
	else if (id == ID_BAR_MIC && g_cfg.enabled && !net_short_code().empty()) {
		ui_type_voice_command(net_short_code()); // not signed in yet: the mic signs in
		return;
	} else if (id == ID_BAR_MIC)
		g_cfg.micMuted = !g_cfg.micMuted;
	else if (id == ID_BAR_CH + CHANNEL_WHISPER && g_cfg.channel == CHANNEL_WHISPER) {
		open_whisper_prompt(); // already on whisper: change who to
		return;
	} else if (id >= ID_BAR_CH && id < ID_BAR_CH + CHANNEL_COUNT) {
		set_channel(id - ID_BAR_CH);
		return;
	}
	config_save();
	sync_controls();
}

// The bar never takes focus, so clicking it doesn't stop the game from
// getting keys. That is why its buttons are drawn here and not child windows.
static LRESULT CALLBACK bar_proc(HWND hw, UINT msg, WPARAM wp, LPARAM lp) {
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
		FillRect(dc, &r, g_bodyBrush);
		for (int i = 0; i < BAR_BUTTONS; i++)
			draw_bar_button(dc, i, i == g_barPressed);
		frame(dc, r, C_BORDER);
		BitBlt(wdc, 0, 0, r.right, r.bottom, dc, 0, 0, SRCCOPY);
		SelectObject(dc, old);
		DeleteObject(bmp);
		DeleteDC(dc);
		EndPaint(hw, &ps);
		return 0;
	}
	case WM_NCHITTEST: {
		// the gaps between buttons drag the bar
		LRESULT hit = DefWindowProcA(hw, msg, wp, lp);
		POINT pt = {(short)LOWORD(lp), (short)HIWORD(lp)};
		ScreenToClient(hw, &pt);
		return hit == HTCLIENT && bar_button_at(pt) < 0 ? HTCAPTION : hit;
	}
	case WM_NCRBUTTONUP:
		toggle_settings();
		return 0;
	case WM_EXITSIZEMOVE: {
		RECT r, cr;
		GetWindowRect(hw, &r);
		GetClientRect(g_gameWnd, &cr);
		POINT br = {cr.right, cr.bottom};
		ClientToScreen(g_gameWnd, &br);
		g_barOffset = {br.x - r.right, br.y - r.bottom};
		g_barMoved = true;
		return 0;
	}
	case WM_MOUSEACTIVATE:
		return MA_NOACTIVATE;
	case WM_LBUTTONDOWN:
	case WM_LBUTTONDBLCLK: {
		POINT pt = {(short)LOWORD(lp), (short)HIWORD(lp)};
		g_barPressed = bar_button_at(pt);
		if (g_barPressed >= 0)
			SetCapture(hw);
		redraw_bar();
		return 0;
	}
	case WM_LBUTTONUP: {
		POINT pt = {(short)LOWORD(lp), (short)HIWORD(lp)};
		int i = g_barPressed;
		g_barPressed = -1;
		ReleaseCapture();
		redraw_bar();
		if (i >= 0 && bar_button_at(pt) == i)
			bar_click(g_barLayout[i].id, false);
		return 0;
	}
	case WM_RBUTTONUP: {
		POINT pt = {(short)LOWORD(lp), (short)HIWORD(lp)};
		int i = bar_button_at(pt);
		bar_click(i >= 0 ? g_barLayout[i].id : 0, true);
		return 0;
	}
	}
	return DefWindowProcA(hw, msg, wp, lp);
}

static void build_bar() {
	g_barW = BAR_PAD - BAR_GAP;
	for (const BarButton& b : g_barLayout)
		g_barW += b.w + BAR_GAP;
	g_barW += BAR_PAD;
	SetWindowPos(g_bar, nullptr, 0, 0, g_barW, BAR_H, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
	SetWindowRgn(g_bar, CreateRoundRectRgn(0, 0, g_barW + 1, BAR_H + 1, 7, 7), FALSE);
}

static void update_bar(bool gameActive) {
	if (!g_cfg.bar || !gameActive || IsIconic(g_gameWnd)) {
		if (IsWindowVisible(g_bar))
			ShowWindow(g_bar, SW_HIDE);
		return;
	}
	RECT cr;
	GetClientRect(g_gameWnd, &cr);
	POINT br = {cr.right, cr.bottom};
	ClientToScreen(g_gameWnd, &br);
	POINT off = g_barMoved ? g_barOffset : POINT{BAR_MARGIN, BAR_MARGIN};
	int x = br.x - off.x - g_barW, y = br.y - off.y - BAR_H;
	RECT cur;
	GetWindowRect(g_bar, &cur);
	if (!IsWindowVisible(g_bar) || cur.left != x || cur.top != y)
		SetWindowPos(g_bar, HWND_TOP, x, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

// ---------------------------------------------------------------- whisper name box

static const int PROMPT_W = 240, PROMPT_H = 96;

static void close_whisper_prompt(bool apply) {
	if (apply) {
		char name[NAME_LENGTH] = "";
		GetWindowTextA(g_promptEdit, name, sizeof(name));
		// trim spaces, game names can't start or end with one
		char* s = name;
		while (*s == ' ')
			s++;
		for (size_t n = strlen(s); n > 0 && s[n - 1] == ' '; n--)
			s[n - 1] = '\0';
		memmove(g_cfg.whisper, s, strlen(s) + 1);
		if (!g_cfg.whisper[0] && g_cfg.channel == CHANNEL_WHISPER)
			g_cfg.channel = CHANNEL_NORMAL;
	} else if (!g_cfg.whisper[0] && g_cfg.channel == CHANNEL_WHISPER) {
		g_cfg.channel = CHANNEL_NORMAL;
	}
	ShowWindow(g_prompt, SW_HIDE);
	SetForegroundWindow(g_gameWnd);
	config_save();
	sync_controls();
}

static void open_whisper_prompt() {
	SetWindowTextA(g_promptEdit, g_cfg.whisper);
	SendMessageA(g_promptEdit, EM_SETSEL, 0, -1);
	RECT br;
	GetWindowRect(g_bar, &br);
	int x = br.right - PROMPT_W, y = br.top - PROMPT_H - 6;
	if (!IsWindowVisible(g_bar)) {
		RECT gr;
		GetWindowRect(g_gameWnd, &gr);
		x = gr.right - PROMPT_W - 20;
		y = gr.bottom - PROMPT_H - 60;
	}
	SetWindowPos(g_prompt, HWND_TOP, x, y, 0, 0, SWP_NOSIZE | SWP_SHOWWINDOW);
	SetForegroundWindow(g_prompt);
	SetFocus(g_promptEdit);
}

static LRESULT CALLBACK prompt_proc(HWND hw, UINT msg, WPARAM wp, LPARAM lp) {
	switch (msg) {
	case WM_ERASEBKGND: {
		RECT r;
		GetClientRect(hw, &r);
		FillRect((HDC)wp, &r, g_bodyBrush);
		return 1;
	}
	case WM_PAINT: {
		PAINTSTRUCT ps;
		HDC dc = BeginPaint(hw, &ps);
		RECT cr;
		GetClientRect(hw, &cr);
		RECT title = {0, 0, cr.right, TITLE_H};
		gradient(dc, title, C_TITLE_TOP, C_TITLE_BOTTOM);
		SetBkMode(dc, TRANSPARENT);
		SelectObject(dc, g_bold);
		RECT tt = {8, 0, cr.right, TITLE_H};
		SetTextColor(dc, RGB(255, 255, 255));
		DrawTextA(dc, "Voice Whisper", -1, &tt, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
		SelectObject(dc, g_font);
		SetTextColor(dc, C_TEXT);
		TextOutA(dc, 10, 30, "Talk to (character name)", 24);
		RECT er;
		GetWindowRect(g_promptEdit, &er);
		MapWindowPoints(nullptr, hw, (POINT*)&er, 2);
		InflateRect(&er, 1, 1);
		frame(dc, er, C_BTN_BORDER);
		frame(dc, cr, C_BORDER);
		EndPaint(hw, &ps);
		return 0;
	}
	case WM_NCHITTEST: {
		LRESULT hit = DefWindowProcA(hw, msg, wp, lp);
		POINT pt = {(short)LOWORD(lp), (short)HIWORD(lp)};
		ScreenToClient(hw, &pt);
		return hit == HTCLIENT && pt.y < TITLE_H ? HTCAPTION : hit;
	}
	case WM_DRAWITEM:
		draw_button((const DRAWITEMSTRUCT*)lp);
		return TRUE;
	case WM_COMMAND:
		if (LOWORD(wp) == ID_PROMPT_OK || LOWORD(wp) == IDOK)
			close_whisper_prompt(true);
		else if (LOWORD(wp) == ID_PROMPT_CANCEL || LOWORD(wp) == IDCANCEL)
			close_whisper_prompt(false);
		return 0;
	case WM_CLOSE:
		close_whisper_prompt(false);
		return 0;
	}
	return DefWindowProcA(hw, msg, wp, lp);
}

static void build_prompt() {
	g_promptEdit = CreateWindowExA(0, "EDIT", "", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 11, 47, 218, 18,
	                               g_prompt, (HMENU)(INT_PTR)ID_PROMPT_EDIT, g_module, nullptr);
	SendMessageA(g_promptEdit, WM_SETFONT, (WPARAM)g_font, TRUE);
	SendMessageA(g_promptEdit, EM_LIMITTEXT, NAME_LENGTH - 1, 0);
	for (int i = 0; i < 2; i++) {
		HWND b = CreateWindowExA(0, "BUTTON", i == 0 ? "OK" : "Cancel", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
		                         PROMPT_W - 150 + i * 75, PROMPT_H - 28, 65, 22, g_prompt,
		                         (HMENU)(INT_PTR)(i == 0 ? ID_PROMPT_OK : ID_PROMPT_CANCEL), g_module, nullptr);
		SendMessageA(b, WM_SETFONT, (WPARAM)g_font, TRUE);
	}
	SetWindowRgn(g_prompt, CreateRoundRectRgn(0, 0, PROMPT_W + 1, PROMPT_H + 1, 7, 7), FALSE);
}

// ---------------------------------------------------------------- polling

static bool game_has_focus() {
	HWND fg = GetForegroundWindow();
	return fg == g_gameWnd || fg == g_settings || fg == graymap_ui_window() || fg == stats_ui_window();
}

static void poll() {
	bool focus = game_has_focus();
	static int lastMicState = -1;
	static DWORD lastHookCheck;
	if (GetTickCount() - lastHookCheck > 2000) {
		lastHookCheck = GetTickCount();
		hooks_check();
	}

	if (g_capturingKey) {
		for (int vk = 1; vk < 255; vk++) {
			if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU ||
			    vk == VK_LSHIFT || vk == VK_RSHIFT || vk == VK_LCONTROL || vk == VK_RCONTROL || vk == VK_LMENU || vk == VK_RMENU)
				continue;
			if (GetAsyncKeyState(vk) & 0x8000) {
				if (vk != VK_ESCAPE)
					g_cfg.pttKey = vk;
				g_capturingKey = false;
				config_save();
				sync_controls();
				break;
			}
		}
	}

	audio_set_transmit(focus && !g_capturingKey && (GetAsyncKeyState(g_cfg.pttKey) & 0x8000));

	bool toggle = focus && (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000) &&
	              (GetAsyncKeyState(g_cfg.toggleKey) & 0x8000);
	if (toggle && !g_toggleDown)
		toggle_settings();
	g_toggleDown = toggle;

	static bool graymapDown;
	bool graymapKey = focus && (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000) &&
	                  (GetAsyncKeyState('G') & 0x8000);
	if (graymapKey && !graymapDown)
		graymap_ui_toggle();
	graymapDown = graymapKey;

	static bool statsDown;
	bool statsKey = focus && (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000) &&
	                (GetAsyncKeyState('S') & 0x8000);
	if (statsKey && !statsDown)
		stats_ui_toggle();
	statsDown = statsKey;

	if (IsWindowVisible(g_settings)) {
		int level = (int)(audio_mic_level() * 100);
		if (level != g_level) {
			g_level = level;
			InvalidateRect(item(ID_LEVEL), nullptr, FALSE);
		}
		DWORD now = GetTickCount();
		if (now - g_lastSlowUpdate > 500) {
			g_lastSlowUpdate = now;
			std::string status = net_status_text();
			if (audio_transmitting())
				status += "  -  talking (" + std::string(channel_name(g_cfg.channel)) + ")";
			set_text(ID_STATUS, status);
			if (g_page == 1)
				fill_players();
		}
	}
	update_overlay();

	HWND fg = GetForegroundWindow();
	update_bar(focus || fg == g_prompt);
	graymap_button_update(focus || fg == g_prompt);
	stats_button_update(focus || fg == g_prompt);
	int micState = (audio_transmitting() ? 1 : 0) | (net_state() == NET_CONNECTED ? 2 : 0) | (g_cfg.enabled ? 4 : 0);
	if (micState != lastMicState) {
		lastMicState = micState;
		redraw_bar();
	}
}

// ---------------------------------------------------------------- settings window

static void build_controls() {
	button(-1, "", WIN_W - 21, 3, 16, 16, ID_X);

	button(-1, "Voice", 10, 30, 102, 22, ID_TAB_VOICE);
	button(-1, "Players", 119, 30, 102, 22, ID_TAB_PLAYERS);
	button(-1, "Devices", 228, 30, 102, 22, ID_TAB_DEVICES);

	// Voice
	button(0, "Push to Talk", 10, 80, 155, 22, ID_MODE_PTT);
	button(0, "Open Mic", 175, 80, 155, 22, ID_MODE_OPEN);
	button(0, "", 10, 128, 320, 22, ID_PTT_KEY);
	button(0, "Normal", 10, 176, 60, 22, ID_CH_NORMAL);
	button(0, "Party", 75, 176, 60, 22, ID_CH_PARTY);
	button(0, "Guild", 140, 176, 60, 22, ID_CH_GUILD);
	button(0, "Room", 205, 176, 60, 22, ID_CH_ROOM);
	button(0, "Whisper", 270, 176, 60, 22, ID_CH_WHISPER);
	label(0, "Level", 12, 225, 55);
	add(0, "STATIC", "", SS_OWNERDRAW, 72, 227, 214, 10, ID_LEVEL);
	label(0, "Mic", 12, 249, 55);
	slider(0, 66, 244, 222, ID_MIC_VOL, 0, 200);
	label(0, "", 292, 249, 42, ID_MIC_VOL_TXT);
	label(0, "Speaker", 12, 275, 55);
	slider(0, 66, 270, 222, ID_SPK_VOL, 0, 200);
	label(0, "", 292, 275, 42, ID_SPK_VOL_TXT);
	button(0, "Voice detect", 10, 320, 155, 22, ID_VAD);
	button(0, "Show who talks", 175, 320, 155, 22, ID_OVERLAY);
	label(0, "Sensitivity", 12, 353, 55);
	slider(0, 66, 348, 222, ID_SENS, 10, 70); // shown as 80 - dB, so right = more sensitive
	label(0, "", 292, 353, 42, ID_SENS_TXT);
	button(0, "Voice chat on", 10, 376, 320, 22, ID_ENABLED);

	// Players
	add(1, "LISTBOX", "", WS_VSCROLL | LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT, 11, 81, 318,
	    278, ID_PLAYERS);
	button(1, "Mute / Unmute", 10, 368, 110, 22, ID_MUTE);
	HWND pv = slider(1, 124, 367, 210, ID_PLAYER_VOL, 0, 200);
	SendMessageA(pv, TBM_SETPOS, TRUE, 100);

	// Devices
	add(2, "LISTBOX", "", WS_VSCROLL | LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT, 11, 81, 318,
	    128, ID_IN_DEV);
	add(2, "LISTBOX", "", WS_VSCROLL | LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT, 11, 241, 318,
	    128, ID_OUT_DEV);

	SendMessageA(label(-1, "", 12, WIN_H - 30, 240, ID_STATUS), WM_SETFONT, (WPARAM)g_bold, TRUE);
	button(-1, "Close", WIN_W - 80, WIN_H - 34, 70, 24, ID_CLOSE);
}

static LRESULT CALLBACK settings_proc(HWND hw, UINT msg, WPARAM wp, LPARAM lp) {
	switch (msg) {
	case WM_TIMER:
		poll();
		return 0;
	case WM_CLOSE:
		ShowWindow(hw, SW_HIDE);
		return 0;
	case WM_NCHITTEST: {
		// drag by the title bar
		LRESULT hit = DefWindowProcA(hw, msg, wp, lp);
		POINT pt = {(short)LOWORD(lp), (short)HIWORD(lp)};
		ScreenToClient(hw, &pt);
		if (hit == HTCLIENT && pt.y < TITLE_H)
			return HTCAPTION;
		return hit;
	}
	case WM_EXITSIZEMOVE:
		g_userMoved = true;
		return 0;
	case WM_ERASEBKGND: {
		RECT r;
		GetClientRect(hw, &r);
		FillRect((HDC)wp, &r, g_bodyBrush);
		return 1;
	}
	case WM_PAINT: {
		PAINTSTRUCT ps;
		HDC dc = BeginPaint(hw, &ps);
		paint_window(dc);
		EndPaint(hw, &ps);
		return 0;
	}
	case WM_MEASUREITEM: {
		MEASUREITEMSTRUCT* m = (MEASUREITEMSTRUCT*)lp;
		if (m->CtlType == ODT_LISTBOX)
			m->itemHeight = 18;
		return TRUE;
	}
	case WM_DRAWITEM: {
		const DRAWITEMSTRUCT* d = (const DRAWITEMSTRUCT*)lp;
		if (d->CtlType == ODT_BUTTON)
			draw_button(d);
		else if (d->CtlType == ODT_LISTBOX)
			draw_list_item(d);
		else if (d->CtlID == ID_LEVEL)
			draw_level(d);
		return TRUE;
	}
	case WM_HSCROLL: {
		HWND bar = (HWND)lp;
		int pos = (int)SendMessageA(bar, TBM_GETPOS, 0, 0);
		int id = GetDlgCtrlID(bar);
		if (id == ID_MIC_VOL)
			g_cfg.micVolume = pos;
		else if (id == ID_SPK_VOL)
			g_cfg.speakerVolume = pos;
		else if (id == ID_SENS)
			g_cfg.sensitivity = 80 - pos;
		else if (id == ID_PLAYER_VOL && selected_player())
			audio_set_speaker_volume(selected_player(), pos);
		if (id != ID_PLAYER_VOL) {
			sync_controls();
			if (LOWORD(wp) == TB_ENDTRACK)
				config_save();
		}
		return 0;
	}
	case WM_COMMAND: {
		int id = LOWORD(wp);
		int code = HIWORD(wp);
		bool clicked = code == BN_CLICKED || code == BN_DOUBLECLICKED; // owner-drawn buttons report fast clicks as double clicks
		switch (id) {
		case ID_TAB_VOICE: case ID_TAB_PLAYERS: case ID_TAB_DEVICES:
			if (!clicked)
				return 0;
			show_page(id - ID_TAB_VOICE);
			if (id == ID_TAB_PLAYERS)
				fill_players();
			if (id == ID_TAB_DEVICES)
				fill_devices();
			return 0;
		case ID_MODE_PTT: case ID_MODE_OPEN:
			if (!clicked)
				return 0;
			g_cfg.mode = id == ID_MODE_PTT ? MODE_PTT : MODE_OPEN;
			break;
		case ID_CH_NORMAL: case ID_CH_PARTY: case ID_CH_GUILD: case ID_CH_ROOM: case ID_CH_WHISPER:
			if (!clicked)
				return 0;
			if (id == ID_CH_WHISPER && g_cfg.channel == CHANNEL_WHISPER)
				open_whisper_prompt(); // already on whisper: change who to
			else
				set_channel(id - ID_CH_NORMAL);
			return 0;
		case ID_PTT_KEY:
			if (!clicked)
				return 0;
			g_capturingKey = true;
			break;
		case ID_VAD:
			if (!clicked)
				return 0;
			g_cfg.voiceDetect = !g_cfg.voiceDetect;
			break;
		case ID_OVERLAY:
			if (!clicked)
				return 0;
			g_cfg.overlay = !g_cfg.overlay;
			break;
		case ID_ENABLED:
			if (!clicked)
				return 0;
			g_cfg.enabled = !g_cfg.enabled;
			break;
		case ID_MUTE: {
			if (!clicked)
				return 0;
			uint32_t id2 = selected_player();
			for (const SpeakerInfo& s : audio_speakers())
				if (s.charId == id2)
					audio_set_speaker_muted(id2, !s.muted);
			fill_players();
			return 0;
		}
		case ID_PLAYERS:
			if (code == LBN_SELCHANGE)
				fill_players();
			return 0;
		case ID_IN_DEV: case ID_OUT_DEV:
			if (code == LBN_SELCHANGE) {
				int sel = (int)SendMessageA(item(id), LB_GETCURSEL, 0, 0) - 1;
				if (id == ID_IN_DEV)
					g_cfg.inDevice = sel;
				else
					g_cfg.outDevice = sel;
				audio_restart_devices();
				config_save();
			}
			return 0;
		case ID_CLOSE: case ID_X:
			if (clicked)
				ShowWindow(hw, SW_HIDE);
			return 0;
		default:
			return 0;
		}
		sync_controls();
		config_save();
		return 0;
	}
	case WM_CTLCOLORSTATIC: {
		// labels and sliders sit on the window body
		HDC dc = (HDC)wp;
		SetBkMode(dc, TRANSPARENT);
		if ((HWND)lp == item(ID_STATUS)) {
			SetTextColor(dc, g_cfg.enabled && net_state() == NET_CONNECTED ? C_OK : C_WAIT);
		} else {
			SetTextColor(dc, C_TEXT);
		}
		return (LRESULT)g_bodyBrush;
	}
	case WM_CTLCOLORLISTBOX:
		return (LRESULT)g_whiteBrush;
	}
	return DefWindowProcA(hw, msg, wp, lp);
}

// ---------------------------------------------------------------- startup

static BOOL CALLBACK find_game_window(HWND hw, LPARAM lp) {
	DWORD pid;
	GetWindowThreadProcessId(hw, &pid);
	if (pid != GetCurrentProcessId() || !IsWindowVisible(hw) || GetWindow(hw, GW_OWNER))
		return TRUE;
	char cls[64];
	GetClassNameA(hw, cls, sizeof(cls));
	if (strncmp(cls, "LegaciaVoice", 12) == 0)
		return TRUE;
	RECT r;
	GetClientRect(hw, &r);
	if (r.right < 640 || r.bottom < 480)
		return TRUE; // skip splash/patcher-sized windows
	*(HWND*)lp = hw;
	return FALSE;
}

void ui_run() {
	while (!g_gameWnd) {
		HWND found = nullptr;
		EnumWindows(find_game_window, (LPARAM)&found);
		g_gameWnd = found;
		if (!g_gameWnd)
			Sleep(500);
	}
	log_line("game window found");

	// draw everything inside the game picture when the game's Direct3D device
	// can be hooked; the separate windows below are the fallback
	if (GetPrivateProfileIntA("ui", "ingame", 1, config_ini_path()) && gui_start()) {
		gui_loop();
		return;
	}

	INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
	InitCommonControlsEx(&icc);
	g_font = CreateFontA(-11, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Tahoma");
	g_bold = CreateFontA(-11, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Tahoma");
	g_bodyBrush = CreateSolidBrush(C_BODY);
	g_whiteBrush = CreateSolidBrush(RGB(255, 255, 255));

	WNDCLASSA wc = {};
	wc.lpfnWndProc = settings_proc;
	wc.hInstance = g_module;
	wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
	wc.hbrBackground = g_bodyBrush;
	wc.lpszClassName = "LegaciaVoiceSettings";
	RegisterClassA(&wc);

	WNDCLASSA oc = {};
	oc.lpfnWndProc = overlay_proc;
	oc.hInstance = g_module;
	oc.lpszClassName = "LegaciaVoiceOverlay";
	RegisterClassA(&oc);

	// no system frame: the title bar, border and close button are drawn by us
	g_settings = CreateWindowExA(WS_EX_TOOLWINDOW, "LegaciaVoiceSettings", "Voice Settings", WS_POPUP | WS_CLIPCHILDREN, 100,
	                             100, WIN_W, WIN_H, g_gameWnd, nullptr, g_module, nullptr);
	SetWindowRgn(g_settings, CreateRoundRectRgn(0, 0, WIN_W + 1, WIN_H + 1, 7, 7), FALSE);
	g_overlay = CreateWindowExA(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
	                            "LegaciaVoiceOverlay", "", WS_POPUP, 0, 0, 220, 20, g_gameWnd, nullptr, g_module, nullptr);
	SetLayeredWindowAttributes(g_overlay, OVERLAY_KEY, 0, LWA_COLORKEY);

	WNDCLASSA bc = {};
	bc.lpfnWndProc = bar_proc;
	bc.hInstance = g_module;
	bc.hCursor = LoadCursor(nullptr, IDC_ARROW);
	bc.lpszClassName = "LegaciaVoiceBar";
	RegisterClassA(&bc);
	g_bar = CreateWindowExA(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, "LegaciaVoiceBar", "Voice", WS_POPUP, 0, 0, 100, BAR_H,
	                        g_gameWnd, nullptr, g_module, nullptr);
	build_bar();

	WNDCLASSA pc = {};
	pc.lpfnWndProc = prompt_proc;
	pc.hInstance = g_module;
	pc.hCursor = LoadCursor(nullptr, IDC_ARROW);
	pc.lpszClassName = "LegaciaVoiceWhisper";
	RegisterClassA(&pc);
	g_prompt = CreateWindowExA(WS_EX_TOOLWINDOW, "LegaciaVoiceWhisper", "Voice Whisper", WS_POPUP | WS_CLIPCHILDREN, 0, 0,
	                           PROMPT_W, PROMPT_H, g_gameWnd, nullptr, g_module, nullptr);
	build_prompt();

	graymap_ui_create();
	graymap_button_create();
	stats_ui_create();
	stats_button_create();
	build_controls();
	show_page(0);
	sync_controls();
	SetTimer(g_settings, TIMER_POLL, 20, nullptr);

	MSG msg;
	while (GetMessageA(&msg, nullptr, 0, 0) > 0) {
		if (!IsDialogMessageA(g_settings, &msg) && !IsDialogMessageA(g_prompt, &msg) &&
		    !IsDialogMessageA(graymap_ui_window(), &msg) && !IsDialogMessageA(stats_ui_window(), &msg)) {
			TranslateMessage(&msg);
			DispatchMessageA(&msg);
		}
	}
}
