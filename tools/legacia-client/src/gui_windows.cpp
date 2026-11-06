// The plugin's windows drawn inside the game picture (see gui_d3d.cpp):
// voice bar, "who is talking" lines, Voice Settings (Ctrl+Shift+V), Voice
// Whisper, the Graymap and Stats buttons under the Cash Shop button, the
// Graymap window (Ctrl+Shift+G) and Player Statistics (Ctrl+Shift+S).
// Same buttons, settings and behaviour as the separate windows in ui.cpp,
// graymap_ui.cpp and stats_ui.cpp, in the game's own colours.
// Everything here runs on the game's render thread.
#include "gui.h"

#include "imgui_internal.h"

#include <algorithm>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <objidl.h> // voice.h uses WIN32_LEAN_AND_MEAN, GDI+ needs this
#include <gdiplus.h>
#include <shellapi.h>

volatile bool g_guiCapturingKey;

namespace {

ImU32 rgb(int r, int g, int b, int a = 255) {
	return IM_COL32(r, g, b, a);
}

// colours taken from the game's own windows (same as ui_style.h)
const ImU32 TITLE_TOP = rgb(106, 140, 205), TITLE_BOTTOM = rgb(160, 186, 232), TITLE_SHADOW = rgb(60, 80, 130);
const ImU32 BODY = rgb(248, 248, 250), BORDER = rgb(126, 152, 200), WHITE = rgb(255, 255, 255);
const ImU32 TEXT = rgb(50, 50, 60), HEADER = rgb(110, 120, 140), RULE = rgb(205, 211, 224);
const ImU32 SEL_TOP = rgb(214, 228, 250), SEL_BOTTOM = rgb(178, 202, 240), SEL_BORDER = rgb(110, 140, 200);
const ImU32 BTN_TOP = rgb(255, 255, 255), BTN_BOTTOM = rgb(232, 236, 242), BTN_BORDER = rgb(160, 170, 190);
const ImU32 HOT_TOP = rgb(244, 248, 255), HOT_BOTTOM = rgb(222, 232, 248);
const ImU32 BLUE_TEXT = rgb(40, 80, 190), SEL_TEXT = rgb(30, 50, 110), DISABLED = rgb(165, 168, 178);
const ImU32 OK = rgb(0, 150, 40), WAIT = rgb(170, 110, 0), CHANNEL_ON = rgb(30, 130, 50);

const float TITLE_H = 22.0f;
const ImGuiWindowFlags WIN_FLAGS = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
                                   ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNav;
const ImGuiWindowFlags FIXED_FLAGS = WIN_FLAGS | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBackground |
                                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoFocusOnAppearing;

struct Win {
	bool open = false;
	bool placed = false; // opened once: keeps the spot the player dragged it to
	bool focus = false;  // bring to the front on the next frame
};

volatile LONG g_toggleReq[3];

// ---------------------------------------------------------------- helpers

ImVec2 operator+(ImVec2 a, ImVec2 b) {
	return ImVec2(a.x + b.x, a.y + b.y);
}

ImVec2 operator-(ImVec2 a, ImVec2 b) {
	return ImVec2(a.x - b.x, a.y - b.y);
}

const char* label_end(const char* s) {
	const char* e = strstr(s, "##");
	return e ? e : s + strlen(s);
}

void text_at(ImDrawList* dl, ImVec2 p, ImU32 c, const char* s, const char* e = nullptr, ImFont* f = nullptr) {
	ImFont* font = f ? f : g_guiFont;
	dl->AddText(font, font->FontSize, p, c, s, e);
}

ImVec2 text_size(const char* s, const char* e = nullptr, ImFont* f = nullptr) {
	ImFont* font = f ? f : g_guiFont;
	return font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.0f, s, e);
}

void gradient(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 top, ImU32 bottom) {
	dl->AddRectFilledMultiColor(a, b, top, top, bottom, bottom);
}

// flat game-style button; selected = pressed-in look
bool button(const char* label, ImVec2 size, bool selected = false, bool enabled = true, ImU32 textColor = 0,
            float textLeft = -1) {
	ImVec2 p = ImGui::GetCursorScreenPos();
	bool clicked = ImGui::InvisibleButton(label, size) && enabled;
	bool hot = enabled && ImGui::IsItemHovered();
	bool held = enabled && ImGui::IsItemActive();
	bool lit = selected || held;
	ImDrawList* dl = ImGui::GetWindowDrawList();
	ImVec2 q = p + size;
	gradient(dl, p, q, lit ? SEL_TOP : hot ? HOT_TOP : BTN_TOP, lit ? SEL_BOTTOM : hot ? HOT_BOTTOM : BTN_BOTTOM);
	dl->AddRect(p, q, lit ? SEL_BORDER : BTN_BORDER);
	const char* e = label_end(label);
	ImFont* f = selected ? g_guiBold : g_guiFont;
	ImVec2 ts = text_size(label, e, f);
	ImU32 c = !enabled ? DISABLED : textColor ? textColor : selected ? SEL_TEXT : TEXT;
	float tx = textLeft >= 0 ? p.x + textLeft : floorf(p.x + (size.x - ts.x) / 2);
	text_at(dl, ImVec2(tx, floorf(p.y + (size.y - ts.y) / 2)), c, label, e, f);
	return clicked;
}

bool button_at(float x, float y, const char* label, ImVec2 size, bool selected = false, bool enabled = true, ImU32 tc = 0) {
	ImGui::SetCursorPos(ImVec2(x, y));
	return button(label, size, selected, enabled, tc);
}

// "Header ----------" across the window
void header(float x, float y, const char* t) {
	ImDrawList* dl = ImGui::GetWindowDrawList();
	ImVec2 w = ImGui::GetWindowPos();
	ImVec2 ts = text_size(t);
	text_at(dl, w + ImVec2(x, y), HEADER, t);
	float ry = floorf(w.y + y + ts.y / 2);
	dl->AddLine(ImVec2(w.x + x + ts.x + 6, ry), ImVec2(w.x + ImGui::GetWindowWidth() - 10, ry), RULE);
}

void label_at(float x, float y, const char* t, ImU32 c = TEXT, ImFont* f = nullptr) {
	text_at(ImGui::GetWindowDrawList(), ImGui::GetWindowPos() + ImVec2(x, y), c, t, nullptr, f);
}

bool slider_at(float x, float y, float w, const char* id, int* v, int lo, int hi, bool enabled = true) {
	ImGui::SetCursorPos(ImVec2(x, y));
	ImGui::SetNextItemWidth(w);
	if (!enabled)
		ImGui::BeginDisabled();
	bool changed = ImGui::SliderInt(id, v, lo, hi, "", ImGuiSliderFlags_AlwaysClamp);
	if (!enabled)
		ImGui::EndDisabled();
	return changed;
}

bool close_x(const char* id) {
	ImVec2 p = ImGui::GetCursorScreenPos();
	bool clicked = ImGui::InvisibleButton(id, ImVec2(16, 16));
	bool hot = ImGui::IsItemHovered();
	ImDrawList* dl = ImGui::GetWindowDrawList();
	ImVec2 c = p + ImVec2(8, 8);
	dl->AddCircleFilled(c, 7.5f, hot ? rgb(235, 110, 110) : rgb(255, 255, 255, 70));
	dl->AddCircle(c, 7.5f, rgb(255, 255, 255, 200));
	dl->AddLine(c + ImVec2(-3, -3), c + ImVec2(3.5f, 3.5f), WHITE, 1.6f);
	dl->AddLine(c + ImVec2(3, -3), c + ImVec2(-3.5f, 3.5f), WHITE, 1.6f);
	return clicked;
}

// a window in the game's style: blue title bar with a close button.
// bg: picture drawn behind everything (Player Statistics)
void begin_window(const char* id, const char* title, Win& w, ImVec2 size, ImVec2 defPos, const GuiTexture* bg = nullptr) {
	if (!w.placed) {
		ImGui::SetNextWindowPos(defPos);
		w.placed = true;
	}
	if (w.focus) {
		ImGui::SetNextWindowFocus();
		w.focus = false;
	}
	ImGui::SetNextWindowSize(size);
	ImGui::PushStyleColor(ImGuiCol_WindowBg, bg && bg->tex ? rgb(0, 0, 0, 0) : BODY);
	ImGui::Begin(id, nullptr, WIN_FLAGS);
	ImGui::PopStyleColor();
	ImDrawList* dl = ImGui::GetWindowDrawList();
	ImVec2 p = ImGui::GetWindowPos();
	if (bg && bg->tex)
		dl->AddImageRounded((ImTextureID)bg->tex, p, p + size, ImVec2(0, 0), ImVec2(1, 1), WHITE, 5.0f);
	dl->AddRectFilled(p, p + ImVec2(size.x, 8), TITLE_TOP, 5.0f, ImDrawFlags_RoundCornersTop);
	gradient(dl, p + ImVec2(0, 5), p + ImVec2(size.x, TITLE_H), TITLE_TOP, TITLE_BOTTOM);
	text_at(dl, p + ImVec2(9, 5), TITLE_SHADOW, title, nullptr, g_guiBold);
	text_at(dl, p + ImVec2(8, 4), WHITE, title, nullptr, g_guiBold);
	ImGui::SetCursorPos(ImVec2(size.x - 21, 3));
	if (close_x("##close"))
		w.open = false;
}

ImGuiWindow* window_named(const char* name) {
	return ImGui::FindWindowByName(name);
}

// the player is dragging this window right now
bool being_moved(const char* name) {
	ImGuiWindow* w = window_named(name);
	return w && GImGui->MovingWindow && GImGui->MovingWindow->RootWindow == w;
}

const char* channel_name(int ch) {
	switch (ch) {
	case CHANNEL_PARTY: return "party";
	case CHANNEL_GUILD: return "guild";
	case CHANNEL_ROOM: return "room";
	case CHANNEL_WHISPER: return "whisper";
	}
	return "normal";
}

std::string key_name(int vk) {
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

// ---------------------------------------------------------------- state

Win g_settings, g_prompt, g_graymap, g_stats;
int g_settingsTab;
uint32_t g_selPlayer;
std::vector<std::string> g_inDevs, g_outDevs;
char g_promptBuf[NAME_LENGTH];
bool g_promptFocus;
int g_gmSel;

// voice bar
const float BAR_H = 32, BAR_PAD = 5, BAR_GAP = 4, BAR_MARGIN = 10;
enum { BAR_SPEAKER = -1, BAR_MIC = -2 };
struct BarButton {
	int id; // channel, or BAR_SPEAKER / BAR_MIC
	const char* text;
	float w;
};
const BarButton BAR[] = {
	{BAR_SPEAKER, "##spk", 26}, {CHANNEL_NORMAL, "Normal##bar", 66}, {CHANNEL_PARTY, "Party##bar", 58},
	{CHANNEL_GUILD, "Guild##bar", 58}, {CHANNEL_ROOM, "Room##bar", 58}, {CHANNEL_WHISPER, "Whisper##bar", 66},
	{BAR_MIC, "##mic", 26},
};
bool g_barMoved;
ImVec2 g_barOffset;

// Graymap / Stats buttons
const int BTN_S = 43;
struct CornerButton {
	GuiTexture tex;
	bool down = false, dragging = false;
	ImVec2 grab, dragPos;
};
CornerButton g_gmBtn, g_stBtn;

// Player Statistics
const char* const TABS[] = {"Overview", "Attack", "Magic", "Defense", "Skills", "Healing", "Procs", "EXP & Drops"};
const int TAB_COUNT = sizeof(TABS) / sizeof(TABS[0]);
const DWORD REFRESH_MS = 3000, TIMEOUT_MS = 4000;
const float ST_W = 520, ST_H = 440;
CRITICAL_SECTION g_statsLock;
std::string g_statsText[TAB_COUNT];
DWORD g_statsGot[TAB_COUNT];
DWORD g_statsAsked;
volatile bool g_statsWaiting, g_statsUnavailable;
int g_statsTab;
GuiTexture g_bgTex;
bool g_bgDirty = true;
std::vector<std::string> g_bgFiles;
DWORD g_bgNoticeAt;
ULONG_PTR g_gdiplus;

// ---------------------------------------------------------------- whisper

void open_whisper_prompt() {
	snprintf(g_promptBuf, sizeof(g_promptBuf), "%s", g_cfg.whisper);
	g_prompt.open = true;
	g_prompt.placed = false;
	g_prompt.focus = true;
	g_promptFocus = true;
}

void close_whisper_prompt(bool apply) {
	if (apply) {
		char* s = g_promptBuf;
		while (*s == ' ')
			s++;
		for (size_t n = strlen(s); n > 0 && s[n - 1] == ' '; n--)
			s[n - 1] = '\0';
		memmove(g_cfg.whisper, s, strlen(s) + 1);
	}
	if (!g_cfg.whisper[0] && g_cfg.channel == CHANNEL_WHISPER)
		g_cfg.channel = CHANNEL_NORMAL;
	g_prompt.open = false;
	config_save();
}

void set_channel(int ch) {
	g_cfg.channel = ch;
	if (ch == CHANNEL_WHISPER && !g_cfg.whisper[0])
		open_whisper_prompt();
	config_save();
}

void whisper_window() {
	if (!g_prompt.open)
		return;
	const ImVec2 size(240, 96);
	ImVec2 disp = ImGui::GetIO().DisplaySize;
	ImVec2 def(disp.x - size.x - 20, disp.y - size.y - BAR_H - BAR_MARGIN - 8);
	begin_window("##whisper", "Voice Whisper", g_prompt, size, def);
	if (!g_prompt.open) { // closed with the X
		ImGui::End();
		close_whisper_prompt(false);
		return;
	}
	label_at(10, 30, "Talk to (character name)");
	ImGui::SetCursorPos(ImVec2(11, 47));
	ImGui::SetNextItemWidth(218);
	if (g_promptFocus) {
		ImGui::SetKeyboardFocusHere();
		g_promptFocus = false;
	}
	bool enter = ImGui::InputText("##who", g_promptBuf, sizeof(g_promptBuf),
	                              ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
	bool ok = button_at(size.x - 150, size.y - 28, "OK##whisper", ImVec2(65, 22));
	bool cancel = button_at(size.x - 75, size.y - 28, "Cancel##whisper", ImVec2(65, 22));
	bool esc = ImGui::IsWindowFocused() && ImGui::IsKeyPressed(ImGuiKey_Escape);
	ImGui::End();
	if (enter || ok)
		close_whisper_prompt(true);
	else if (cancel || esc)
		close_whisper_prompt(false);
}

// ---------------------------------------------------------------- voice settings

void refresh_devices() {
	g_inDevs = audio_input_devices();
	g_outDevs = audio_output_devices();
}

void toggle_settings() {
	g_settings.open = !g_settings.open;
	if (g_settings.open) {
		refresh_devices();
		g_settings.focus = true;
	}
}

void level_meter(float x, float y, float w, float h) {
	ImDrawList* dl = ImGui::GetWindowDrawList();
	ImVec2 p = ImGui::GetWindowPos() + ImVec2(x, y);
	dl->AddRectFilled(p, p + ImVec2(w, h), WHITE);
	dl->AddRect(p, p + ImVec2(w, h), BTN_BORDER);
	int level = (int)(audio_mic_level() * 100);
	float fw = (w - 2) * level / 100.0f;
	if (fw > 0) {
		bool hot = level > 85;
		gradient(dl, p + ImVec2(1, 1), p + ImVec2(1 + fw, h - 1), hot ? rgb(250, 190, 120) : rgb(150, 220, 140),
		         hot ? rgb(220, 120, 60) : rgb(70, 170, 70));
	}
}

bool device_list(const char* id, float x, float y, const std::vector<std::string>& devs, int* sel) {
	ImGui::SetCursorPos(ImVec2(x, y));
	bool changed = false;
	ImGui::PushStyleColor(ImGuiCol_ChildBg, WHITE);
	ImGui::BeginChild(id, ImVec2(318, 128), ImGuiChildFlags_Borders);
	for (int i = -1; i < (int)devs.size(); i++) {
		bool on = *sel == i || (i == -1 && *sel >= (int)devs.size());
		ImGui::PushID(i);
		ImGui::PushStyleColor(ImGuiCol_Text, on ? BLUE_TEXT : TEXT);
		ImGui::PushFont(on ? g_guiBold : g_guiFont);
		std::string t = std::string(on ? "> " : "   ") + (i < 0 ? "System default" : devs[i]);
		if (ImGui::Selectable(t.c_str(), on) && !on) {
			*sel = i;
			changed = true;
		}
		ImGui::PopFont();
		ImGui::PopStyleColor();
		ImGui::PopID();
	}
	ImGui::EndChild();
	ImGui::PopStyleColor();
	return changed;
}

void settings_window() {
	if (!g_settings.open)
		return;
	const float W = 340, H = 440;
	ImVec2 disp = ImGui::GetIO().DisplaySize;
	begin_window("##voicesettings", "Voice Settings", g_settings, ImVec2(W, H),
	             ImVec2(std::max(0.0f, disp.x - W - 8), std::max(0.0f, disp.y - H - 48)));
	bool save = false;

	const char* tabs[] = {"Voice##tab", "Players##tab", "Devices##tab"};
	for (int i = 0; i < 3; i++)
		if (button_at(10 + i * 109.0f, 30, tabs[i], ImVec2(102, 22), g_settingsTab == i)) {
			g_settingsTab = i;
			if (i == 2)
				refresh_devices();
		}

	if (g_settingsTab == 0) {
		header(10, 62, "Mic Mode");
		if (button_at(10, 80, "Push to Talk", ImVec2(155, 22), g_cfg.mode == MODE_PTT))
			g_cfg.mode = MODE_PTT, save = true;
		if (button_at(175, 80, "Open Mic", ImVec2(155, 22), g_cfg.mode == MODE_OPEN))
			g_cfg.mode = MODE_OPEN, save = true;

		header(10, 110, "PTT Key");
		std::string key = g_guiCapturingKey ? "Press a key or mouse button..." : key_name(g_cfg.pttKey);
		key += "##pttkey";
		if (button_at(10, 128, key.c_str(), ImVec2(320, 22), g_guiCapturingKey, g_cfg.mode == MODE_PTT))
			g_guiCapturingKey = true;

		header(10, 158, "Talk To");
		const char* ch[] = {"Normal##ch", "Party##ch", "Guild##ch", "Room##ch", "Whisper##ch"};
		for (int i = 0; i < CHANNEL_COUNT; i++)
			if (button_at(10 + i * 65.0f, 176, ch[i], ImVec2(60, 22), g_cfg.channel == i)) {
				if (i == CHANNEL_WHISPER && g_cfg.channel == CHANNEL_WHISPER)
					open_whisper_prompt();
				else
					set_channel(i);
			}

		header(10, 206, "Volume");
		label_at(12, 222, "Level");
		level_meter(72, 225, 214, 10);
		char buf[32];
		label_at(12, 246, "Mic");
		slider_at(66, 242, 222, "##micvol", &g_cfg.micVolume, 0, 200);
		save |= ImGui::IsItemDeactivatedAfterEdit();
		snprintf(buf, sizeof(buf), "%d%%", g_cfg.micVolume);
		label_at(294, 246, buf);
		label_at(12, 272, "Speaker");
		slider_at(66, 268, 222, "##spkvol", &g_cfg.speakerVolume, 0, 200);
		save |= ImGui::IsItemDeactivatedAfterEdit();
		snprintf(buf, sizeof(buf), "%d%%", g_cfg.speakerVolume);
		label_at(294, 272, buf);

		header(10, 302, "Audio");
		if (button_at(10, 320, "Voice detect", ImVec2(155, 22), g_cfg.voiceDetect != 0, g_cfg.mode == MODE_OPEN))
			g_cfg.voiceDetect = !g_cfg.voiceDetect, save = true;
		if (button_at(175, 320, "Show who talks", ImVec2(155, 22), g_cfg.overlay != 0))
			g_cfg.overlay = !g_cfg.overlay, save = true;
		label_at(12, 352, "Sensitivity");
		int pos = 80 - g_cfg.sensitivity; // right = more sensitive
		if (slider_at(80, 348, 208, "##sens", &pos, 10, 70, g_cfg.mode == MODE_OPEN && g_cfg.voiceDetect))
			g_cfg.sensitivity = 80 - pos;
		save |= ImGui::IsItemDeactivatedAfterEdit();
		snprintf(buf, sizeof(buf), "-%d dB", g_cfg.sensitivity);
		label_at(294, 352, buf);
		if (button_at(10, 376, g_cfg.enabled ? "Voice chat on##en" : "Voice chat off##en", ImVec2(320, 22), g_cfg.enabled != 0))
			g_cfg.enabled = !g_cfg.enabled, save = true;
	} else if (g_settingsTab == 1) {
		header(10, 62, "Players");
		std::vector<SpeakerInfo> list = audio_speakers();
		const SpeakerInfo* sel = nullptr;
		ImGui::SetCursorPos(ImVec2(11, 81));
		ImGui::PushStyleColor(ImGuiCol_ChildBg, WHITE);
		ImGui::BeginChild("##players", ImVec2(318, 278), ImGuiChildFlags_Borders);
		if (list.empty())
			ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(HEADER), "Nobody heard yet.");
		for (const SpeakerInfo& s : list) {
			bool on = s.charId == g_selPlayer;
			if (on)
				sel = &s;
			char line[160];
			snprintf(line, sizeof(line), "%s%s  (%s)%s%s##%u", on ? "> " : "   ", s.name.c_str(), channel_name(s.channel),
			         s.muted ? "  [muted]" : "", s.volume != 100 ? ("  " + std::to_string(s.volume) + "%").c_str() : "",
			         (unsigned)s.charId);
			ImGui::PushStyleColor(ImGuiCol_Text, s.muted ? DISABLED : s.talking ? OK : on ? BLUE_TEXT : TEXT);
			ImGui::PushFont(on ? g_guiBold : g_guiFont);
			if (ImGui::Selectable(line, on))
				g_selPlayer = s.charId;
			ImGui::PopFont();
			ImGui::PopStyleColor();
		}
		ImGui::EndChild();
		ImGui::PopStyleColor();
		if (button_at(10, 368, "Mute / Unmute", ImVec2(110, 22), false, sel != nullptr) && sel)
			audio_set_speaker_muted(sel->charId, !sel->muted);
		int vol = sel ? sel->volume : 100;
		if (slider_at(128, 369, 202, "##pvol", &vol, 0, 200, sel != nullptr) && sel)
			audio_set_speaker_volume(sel->charId, vol);
	} else {
		header(10, 62, "Microphone");
		if (device_list("##indev", 11, 81, g_inDevs, &g_cfg.inDevice)) {
			audio_restart_devices();
			save = true;
		}
		header(10, 222, "Speaker");
		if (device_list("##outdev", 11, 241, g_outDevs, &g_cfg.outDevice)) {
			audio_restart_devices();
			save = true;
		}
	}

	ImDrawList* dl = ImGui::GetWindowDrawList();
	ImVec2 p = ImGui::GetWindowPos();
	dl->AddLine(p + ImVec2(10, H - 42), p + ImVec2(W - 10, H - 42), RULE);
	std::string status = net_status_text();
	if (audio_transmitting())
		status += "  -  talking (" + std::string(channel_name(g_cfg.channel)) + ")";
	ImGui::PushClipRect(p, p + ImVec2(W - 90, H), true);
	label_at(12, H - 28, status.c_str(), g_cfg.enabled && net_state() == NET_CONNECTED ? OK : WAIT, g_guiBold);
	ImGui::PopClipRect();
	if (button_at(W - 80, H - 34, "Close##settings", ImVec2(70, 24)))
		g_settings.open = false;
	ImGui::End();
	if (save)
		config_save();
}

// ---------------------------------------------------------------- voice bar

void speaker_icon(ImDrawList* dl, ImVec2 c, bool off) {
	ImU32 col = off ? rgb(150, 150, 160) : rgb(70, 100, 170);
	float cx = c.x - 3, cy = c.y;
	ImVec2 body[] = {{cx - 6, cy - 3}, {cx - 3, cy - 3}, {cx + 2, cy - 7}, {cx + 2, cy + 7}, {cx - 3, cy + 3}, {cx - 6, cy + 3}};
	dl->AddConvexPolyFilled(body, 6, col);
	if (off) {
		dl->AddLine(ImVec2(cx - 7, cy - 8), ImVec2(cx + 10, cy + 8), rgb(200, 60, 60), 2.0f);
		return;
	}
	dl->PathArcTo(ImVec2(cx + 3, cy), 5, -0.9f, 0.9f);
	dl->PathStroke(col, 0, 1.3f);
	dl->PathArcTo(ImVec2(cx + 3, cy), 8.5f, -0.9f, 0.9f);
	dl->PathStroke(col, 0, 1.3f);
}

void mic_icon(ImDrawList* dl, ImVec2 c, bool muted, bool talking, bool connected) {
	ImU32 col = !connected ? rgb(150, 150, 160) : talking ? rgb(40, 170, 60) : rgb(70, 100, 170);
	float cx = c.x, cy = c.y - 2;
	dl->AddRectFilled(ImVec2(cx - 3, cy - 7), ImVec2(cx + 4, cy + 4), col, 3.5f);
	dl->PathArcTo(ImVec2(cx + 0.5f, cy + 1), 6.0f, 0.0f, 3.14159f);
	dl->PathStroke(col, 0, 1.3f);
	dl->AddLine(ImVec2(cx + 0.5f, cy + 7), ImVec2(cx + 0.5f, cy + 10), col, 1.3f);
	dl->AddLine(ImVec2(cx - 3, cy + 10), ImVec2(cx + 4, cy + 10), col, 1.3f);
	if (muted)
		dl->AddLine(ImVec2(cx - 7, cy - 7), ImVec2(cx + 8, cy + 10), rgb(200, 60, 60), 2.0f);
}

void bar_click(int id, bool right) {
	if (right) {
		if (id == CHANNEL_WHISPER) {
			g_cfg.channel = CHANNEL_WHISPER;
			open_whisper_prompt();
		} else {
			toggle_settings();
		}
		return;
	}
	if (id == BAR_SPEAKER) {
		g_cfg.deafen = !g_cfg.deafen;
	} else if (id == BAR_MIC && g_cfg.enabled && !net_short_code().empty()) {
		ui_type_voice_command(net_short_code()); // not signed in yet: the mic signs in
		return;
	} else if (id == BAR_MIC) {
		g_cfg.micMuted = !g_cfg.micMuted;
	} else if (id == CHANNEL_WHISPER && g_cfg.channel == CHANNEL_WHISPER) {
		open_whisper_prompt(); // already on whisper: change who to
		return;
	} else {
		set_channel(id);
		return;
	}
	config_save();
}

void voice_bar() {
	if (!g_cfg.bar)
		return;
	float w = BAR_PAD * 2 - BAR_GAP;
	for (const BarButton& b : BAR)
		w += b.w + BAR_GAP;
	ImVec2 disp = ImGui::GetIO().DisplaySize;
	const char* name = "##voicebar";
	bool moving = being_moved(name);
	if (!moving) {
		ImVec2 off = g_barMoved ? g_barOffset : ImVec2(BAR_MARGIN, BAR_MARGIN);
		float x = std::min(std::max(0.0f, disp.x - off.x - w), disp.x - w);
		float y = std::min(std::max(0.0f, disp.y - off.y - BAR_H), disp.y - BAR_H);
		ImGui::SetNextWindowPos(ImVec2(x, y));
	}
	ImGui::SetNextWindowSize(ImVec2(w, BAR_H));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
	ImGui::PushStyleColor(ImGuiCol_WindowBg, BODY);
	ImGui::Begin(name, nullptr, WIN_FLAGS | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoFocusOnAppearing);
	ImGui::PopStyleColor();
	ImGui::PopStyleVar();
	if (moving) {
		ImVec2 p = ImGui::GetWindowPos();
		g_barOffset = ImVec2(disp.x - p.x - w, disp.y - p.y - BAR_H);
		g_barMoved = true;
	}
	ImDrawList* dl = ImGui::GetWindowDrawList();
	bool connected = g_cfg.enabled && net_state() == NET_CONNECTED;
	float x = BAR_PAD;
	int clicked = 0x7fff, rclicked = 0x7fff;
	for (const BarButton& b : BAR) {
		ImVec2 size(b.w, 22);
		ImGui::SetCursorPos(ImVec2(x, (BAR_H - 22) / 2));
		if (b.id < 0) {
			ImVec2 p = ImGui::GetCursorScreenPos();
			if (ImGui::InvisibleButton(b.text, size))
				clicked = b.id;
			if (ImGui::IsItemActive())
				gradient(dl, p, p + size, SEL_TOP, SEL_BOTTOM);
			else if (ImGui::IsItemHovered())
				gradient(dl, p, p + size, HOT_TOP, HOT_BOTTOM);
			ImVec2 c = p + ImVec2(b.w / 2, 11);
			if (b.id == BAR_SPEAKER)
				speaker_icon(dl, c, g_cfg.deafen != 0);
			else
				mic_icon(dl, c, g_cfg.micMuted != 0, audio_transmitting(), connected);
		} else {
			bool on = g_cfg.channel == b.id;
			if (button(b.text, size, on, true, on ? CHANNEL_ON : 0))
				clicked = b.id;
		}
		if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
			rclicked = b.id;
		if (ImGui::IsItemHovered() && b.id == BAR_MIC) {
			std::string code = g_cfg.enabled ? net_short_code() : "";
			ImGui::SetTooltip("%s", !code.empty() ? ("Click to sign in (@voice " + code + ")").c_str()
			                        : g_cfg.micMuted ? "Mic muted: click to unmute" : "Click to mute your mic");
		}
		x += b.w + BAR_GAP;
	}
	// right-click the bar itself: Voice Settings
	if (rclicked == 0x7fff && ImGui::IsWindowHovered() && !ImGui::IsAnyItemHovered() &&
	    ImGui::IsMouseClicked(ImGuiMouseButton_Right))
		rclicked = 0x7ffe;
	ImGui::End();
	if (clicked != 0x7fff)
		bar_click(clicked, false);
	if (rclicked == 0x7ffe)
		toggle_settings();
	else if (rclicked != 0x7fff)
		bar_click(rclicked, true);
}

// "who is talking" lines near the top right of the game screen
void talking_overlay() {
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
	if (lines.empty())
		return;
	ImDrawList* dl = ImGui::GetForegroundDrawList();
	ImVec2 disp = ImGui::GetIO().DisplaySize;
	float y = 190;
	for (const std::string& l : lines) {
		ImU32 c = l[0] == '(' ? rgb(120, 255, 120) : l.compare(0, 6, "Voice:") == 0 ? rgb(140, 220, 255) : rgb(255, 230, 120);
		ImVec2 p(disp.x - 270, y);
		for (int dx = -1; dx <= 1; dx++)
			for (int dy = -1; dy <= 1; dy++)
				if (dx || dy)
					text_at(dl, p + ImVec2((float)dx, (float)dy), rgb(0, 0, 0, 200), l.c_str(), nullptr, g_guiBold);
		text_at(dl, p, c, l.c_str(), nullptr, g_guiBold);
		y += 16;
	}
}

// ---------------------------------------------------------------- Graymap / Stats buttons

// draws one of the 43x43 buttons; returns 1 click, 2 right-click
int corner_button(const char* name, CornerButton& b, int* right, int* top, const uint32_t* (*pixels)(bool, bool)) {
	ImVec2 disp = ImGui::GetIO().DisplaySize;
	ImVec2 pos(disp.x - *right - BTN_S, (float)*top);
	if (b.dragging)
		pos = b.dragPos;
	pos.x = std::min(std::max(0.0f, pos.x), disp.x - BTN_S);
	pos.y = std::min(std::max(0.0f, pos.y), disp.y - BTN_S);
	ImGui::SetNextWindowPos(pos);
	ImGui::SetNextWindowSize(ImVec2(BTN_S, BTN_S));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
	ImGui::Begin(name, nullptr, FIXED_FLAGS);
	ImGui::PopStyleVar(2);
	ImGui::SetCursorPos(ImVec2(0, 0));
	ImGui::InvisibleButton("##btn", ImVec2(BTN_S, BTN_S));
	bool hover = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
	int result = 0;
	if (ImGui::IsItemActivated()) {
		b.grab = ImGui::GetIO().MousePos - pos;
		b.down = true;
	}
	if (held && !b.dragging && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 4.0f))
		b.dragging = true;
	if (b.dragging && held)
		b.dragPos = ImGui::GetIO().MousePos - b.grab;
	if (ImGui::IsItemDeactivated()) {
		if (b.dragging) {
			*right = (int)(disp.x - b.dragPos.x - BTN_S);
			*top = (int)b.dragPos.y;
			config_save();
		} else if (hover) {
			result = 1;
		}
		b.dragging = false;
		b.down = false;
	}
	if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
		result = 2;

	// the button picture, with its corners cut like the Cash Shop button
	static uint32_t px[BTN_S * BTN_S];
	const uint32_t* src = pixels(hover, held && !b.dragging);
	for (int y = 0; y < BTN_S; y++)
		for (int x = 0; x < BTN_S; x++) {
			int cx = std::min(x, BTN_S - 1 - x), cy = std::min(y, BTN_S - 1 - y);
			px[y * BTN_S + x] = (cx + cy < 2 ? 0u : 0xff000000u) | (src[y * BTN_S + x] & 0xffffff);
		}
	if (gui_texture_upload(b.tex, px, BTN_S, BTN_S)) {
		ImVec2 p = ImGui::GetWindowPos();
		ImGui::GetWindowDrawList()->AddImage((ImTextureID)b.tex.tex, p, p + ImVec2(BTN_S, BTN_S));
	}
	ImGui::End();
	return result;
}

void toggle_graymap_window() {
	g_graymap.open = !g_graymap.open;
	g_graymap.focus = g_graymap.open;
}

void ask_stats();

void toggle_stats_window() {
	g_stats.open = !g_stats.open;
	if (g_stats.open) {
		g_stats.focus = true;
		g_statsUnavailable = false;
		g_bgDirty = true;
		ask_stats();
	}
}

void set_graymap(bool on) {
	if (!graymap_available())
		return;
	g_cfg.gmEnabled = on;
	config_save();
}

// ---------------------------------------------------------------- Graymap window

const char* const GM_NAMES[GM_COLOR_COUNT] = {"Walkable", "Grid lines", "Unwalkable", "Water", "Shootable wall"};

bool switch_at(float x, float y, const char* id, bool on, bool enabled) {
	ImGui::SetCursorPos(ImVec2(x, y));
	ImVec2 p = ImGui::GetCursorScreenPos(), s(60, 22);
	bool clicked = ImGui::InvisibleButton(id, s) && enabled;
	ImDrawList* dl = ImGui::GetWindowDrawList();
	ImU32 bg = !enabled ? rgb(215, 218, 226) : on ? rgb(70, 150, 90) : rgb(185, 192, 206);
	dl->AddRectFilled(p, p + s, bg, 11.0f);
	float kx = on ? p.x + s.x - 11 : p.x + 11;
	dl->AddCircleFilled(ImVec2(kx, p.y + 11), 8.5f, WHITE);
	const char* t = on ? "ON" : "OFF";
	ImVec2 ts = text_size(t, nullptr, g_guiBold);
	float tx = on ? p.x + 8 : p.x + s.x - 8 - ts.x;
	text_at(dl, ImVec2(tx, p.y + (s.y - ts.y) / 2), WHITE, t, nullptr, g_guiBold);
	return clicked;
}

// the colour square in front of a Square/Circle row; click it to change the colour
void swatch_at(float x, float y, const char* id, int c, bool* clicked) {
	ImGui::SetCursorPos(ImVec2(x, y));
	ImVec2 p = ImGui::GetCursorScreenPos();
	*clicked = ImGui::InvisibleButton(id, ImVec2(22, 22));
	ImDrawList* dl = ImGui::GetWindowDrawList();
	dl->AddRectFilled(p, p + ImVec2(22, 22), rgb((c >> 16) & 255, (c >> 8) & 255, c & 255), 4.0f);
	dl->AddRect(p, p + ImVec2(22, 22), ImGui::IsItemHovered() ? SEL_BORDER : BTN_BORDER, 4.0f);
}

// a colour picker with Hex/R/G/B boxes; true when a change is finished (save it)
bool color_editor(const char* id, int& c, float x, float y) {
	bool save = false;
	float col[3] = {((c >> 16) & 255) / 255.0f, ((c >> 8) & 255) / 255.0f, (c & 255) / 255.0f};
	ImGui::SetCursorPos(ImVec2(x, y));
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, 3));
	// the picker square is as tall as it is wide: keep it short so the boxes fit below
	ImGui::PushItemWidth(150);
	char pid[48];
	snprintf(pid, sizeof(pid), "##pick%s", id);
	if (ImGui::ColorPicker3(pid, col,
	                        ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_NoAlpha | ImGuiColorEditFlags_PickerHueBar |
	                            ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel)) {
		int r = (int)(col[0] * 255 + 0.5f), g = (int)(col[1] * 255 + 0.5f), b = (int)(col[2] * 255 + 0.5f);
		c = (r << 16) | (g << 8) | b;
	}
	ImGui::PopItemWidth();
	save |= ImGui::IsItemDeactivatedAfterEdit();
	// preview swatch next to the picker
	float bx = x + 168, bw = 104;
	ImVec2 p = ImGui::GetWindowPos() + ImVec2(bx, y);
	ImGui::GetWindowDrawList()->AddRectFilled(p, p + ImVec2(bw, 60), rgb((c >> 16) & 255, (c >> 8) & 255, c & 255));
	ImGui::GetWindowDrawList()->AddRect(p, p + ImVec2(bw, 60), BTN_BORDER);
	label_at(bx, y + 66, "Hex", HEADER);
	ImGui::SetCursorPos(ImVec2(bx, y + 82));
	ImGui::SetNextItemWidth(bw);
	char edit[8], hid[48];
	snprintf(edit, sizeof(edit), "%06X", c & 0xffffff);
	snprintf(hid, sizeof(hid), "##hex%s", id);
	if (ImGui::InputText(hid, edit, sizeof(edit), ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_CharsUppercase) &&
	    strlen(edit) == 6)
		c = (int)strtoul(edit, nullptr, 16);
	save |= ImGui::IsItemDeactivatedAfterEdit();
	const char* names[3] = {"R", "G", "B"};
	for (int k = 0; k < 3; k++) {
		int shift = 16 - 8 * k, v = (c >> shift) & 255;
		label_at(bx, y + 112 + k * 24.0f, names[k], HEADER);
		ImGui::SetCursorPos(ImVec2(bx + 16, y + 108 + k * 24.0f));
		ImGui::SetNextItemWidth(bw - 16);
		char cid[48];
		snprintf(cid, sizeof(cid), "##%s%s", names[k], id);
		if (ImGui::InputInt(cid, &v, 0, 0)) {
			v = std::min(255, std::max(0, v));
			c = (c & ~(255 << shift)) | (v << shift);
		}
		save |= ImGui::IsItemDeactivatedAfterEdit();
	}
	ImGui::PopStyleVar();
	return save;
}

int g_gmTab; // 0 Graymap, 1 LGP, 2 Aura
int g_lgpEdit = -1; // colour being edited in the LGP tab: 0 square, 1 circle, -1 none

// Graymap tab: the colours of the flat map
bool graymap_tab(float W, float oy) {
	bool avail = graymap_available();
	label_at(16, oy + 4, "Graymap", TEXT, g_guiBold);
	label_at(16, oy + 22, avail ? "The map as flat coloured cells, to see where you can walk."
	                            : "Not available: this game exe isn't the one Graymap was made for.",
	         avail ? HEADER : WAIT);
	if (switch_at(W - 76, oy + 1, "##gmswitch", g_cfg.gmEnabled && avail, avail))
		set_graymap(!g_cfg.gmEnabled);
	header(16, oy + 48, "Colours");

	bool save = false;
	for (int i = 0; i < GM_COLOR_COUNT; i++) {
		char id[48];
		snprintf(id, sizeof(id), "%s##gmc%d", GM_NAMES[i], i);
		ImGui::SetCursorPos(ImVec2(16, oy + 72 + i * 34.0f));
		ImVec2 p = ImGui::GetCursorScreenPos();
		if (button(id, ImVec2(152, 28), g_gmSel == i, true, 0, 30))
			g_gmSel = i;
		int c = g_cfg.gmColor[i];
		ImGui::GetWindowDrawList()->AddRectFilled(p + ImVec2(8, 7), p + ImVec2(22, 21), rgb((c >> 16) & 255, (c >> 8) & 255, c & 255));
		ImGui::GetWindowDrawList()->AddRect(p + ImVec2(8, 7), p + ImVec2(22, 21), BTN_BORDER);
	}
	if (button_at(16, oy + 246, "Reset all colours", ImVec2(152, 26))) {
		for (int i = 0; i < GM_COLOR_COUNT; i++)
			g_cfg.gmColor[i] = GM_DEFAULT_COLORS[i];
		save = true;
	}

	// Graymap picks a changed colour up on its next frame
	int& c = g_cfg.gmColor[g_gmSel];
	label_at(192, oy + 72, GM_NAMES[g_gmSel], TEXT, g_guiBold);
	char hex[8];
	snprintf(hex, sizeof(hex), "#%06X", c & 0xffffff);
	ImVec2 hs = text_size(hex);
	label_at(W - 16 - hs.x, oy + 72, hex, BLUE_TEXT);
	save |= color_editor("gm", c, 192, oy + 92);
	return save;
}

// one ON/OFF layer row in the LGP tab
bool layer_row(float x, float y, float w, const char* id, const char* title, const char* sub, int* v, bool enabled) {
	ImDrawList* dl = ImGui::GetWindowDrawList();
	ImVec2 p = ImGui::GetWindowPos() + ImVec2(x, y);
	dl->AddRectFilled(p, p + ImVec2(w, 38), WHITE, 6.0f);
	dl->AddRect(p, p + ImVec2(w, 38), RULE, 6.0f);
	label_at(x + 10, y + 4, title, enabled ? TEXT : DISABLED, g_guiBold);
	label_at(x + 10, y + 20, sub, enabled ? HEADER : DISABLED);
	if (switch_at(x + w - 68, y + 8, id, *v && enabled, enabled)) {
		*v = !*v;
		return true;
	}
	return false;
}

// Square or Circle settings in the LGP tab
bool shape_rows(float x, float y, float w, const char* name, Config::LgpShape& s, int maxSize, int which, bool enabled) {
	bool save = false;
	char id[48];
	bool clicked;
	snprintf(id, sizeof(id), "##sw%s", name);
	swatch_at(x, y + 2, id, s.color, &clicked);
	if (clicked && enabled)
		g_lgpEdit = g_lgpEdit == which ? -1 : which;
	label_at(x + 32, y + 6, name, enabled ? TEXT : DISABLED, g_guiBold);
	snprintf(id, sizeof(id), "Ring##%s", name);
	if (button_at(x + 100, y + 2, id, ImVec2(56, 24), !s.fill, enabled))
		s.fill = 0, save = true;
	snprintf(id, sizeof(id), "Fill##%s", name);
	if (button_at(x + 160, y + 2, id, ImVec2(56, 24), s.fill != 0, enabled))
		s.fill = 1, save = true;
	snprintf(id, sizeof(id), "##on%s", name);
	if (switch_at(x + w - 60, y + 3, id, s.on && enabled, enabled))
		s.on = !s.on, save = true;

	label_at(x, y + 38, "Size", enabled ? HEADER : DISABLED);
	char cells[32];
	snprintf(cells, sizeof(cells), "%d cells", s.size);
	label_at(x + w - text_size(cells).x, y + 38, cells, enabled ? BLUE_TEXT : DISABLED);
	snprintf(id, sizeof(id), "##size%s", name);
	slider_at(x, y + 56, w, id, &s.size, which == 0 ? 1 : 2, maxSize, enabled);
	save |= ImGui::IsItemDeactivatedAfterEdit();

	label_at(x, y + 88, "Wave", enabled ? HEADER : DISABLED);
	const char* waves[6] = {"Flat", "1", "2", "3", "4", "5"};
	float bx = x + 44, bw = (w - 44 - 5 * 4) / 6;
	for (int i = 0; i < 6; i++) {
		snprintf(id, sizeof(id), "%s##wave%s%d", waves[i], name, i);
		if (button_at(bx + i * (bw + 4), y + 84, id, ImVec2(bw, 24), s.wave == i, enabled))
			s.wave = i, save = true;
	}
	return save;
}

// LGP tab: cells drawn on the ground around the player
bool lgp_tab(float W, float H, float oy) {
	bool avail = graymap_available(), on = g_cfg.lgpEnabled && avail;
	bool save = false;
	label_at(16, oy + 4, "LGP ground cells", TEXT, g_guiBold);
	label_at(16, oy + 22,
	         !avail ? "Not available: this game exe isn't the one LGP was made for."
	         : on   ? "On - cells drawn on the ground around you"
	                : "Off - nothing is drawn on the ground",
	         !avail ? WAIT : on ? OK : HEADER);
	if (switch_at(W - 76, oy + 1, "##lgpswitch", on, avail)) {
		g_cfg.lgpEnabled = !g_cfg.lgpEnabled;
		save = true;
	}

	// layers, on the left
	float lx = 16, lw = 236, y = oy + 48;
	header(lx, y, "Layers");
	y += 20;
	save |= layer_row(lx, y, lw, "##lgpaoe", "AoE skill cells", "skills on the ground", &g_cfg.lgpAoe, on);
	save |= layer_row(lx, y + 44, lw, "##lgpdead", "Dead cells", "where you can't walk", &g_cfg.lgpDead, on);
	save |= layer_row(lx, y + 88, lw, "##lgpgut", "Gutter lines", "gaps between cells", &g_cfg.lgpGutter, on);
	save |= layer_row(lx, y + 132, lw, "##lgpsmooth", "Smooth follow", "glides as you walk", &g_cfg.lgpSmooth, on);

	// what the skill cell colours mean
	{
		ImDrawList* dl = ImGui::GetWindowDrawList();
		ImVec2 p = ImGui::GetWindowPos();
		float ky = y + 186;
		label_at(lx, ky, "Colours", HEADER);
		float ry = floorf(p.y + ky + text_size("Colours").y / 2);
		dl->AddLine(ImVec2(p.x + lx + text_size("Colours").x + 6, ry), ImVec2(p.x + lx + lw, ry), RULE);
		const struct {
			ImU32 c;
			const char* t;
		} keys[3] = {{rgb(255, 90, 60), "skill that can hit you"}, {rgb(60, 140, 255), "skill that can't hit you"},
		             {rgb(176, 42, 42), "dead cell"}};
		for (int k = 0; k < 3; k++) {
			ImVec2 q = p + ImVec2(lx + 2, ky + 24 + k * 22.0f);
			dl->AddRectFilled(q, q + ImVec2(14, 14), keys[k].c, 3.0f);
			ImGui::PushClipRect(p + ImVec2(lx, 0), p + ImVec2(lx + lw, 2000), true);
			label_at(lx + 22, ky + 24 + k * 22.0f, keys[k].t, on ? TEXT : DISABLED);
			ImGui::PopClipRect();
		}
	}

	// shapes, on the right
	float rx = lx + lw + 14, rw = W - 16 - rx;
	{
		ImDrawList* dl = ImGui::GetWindowDrawList();
		ImVec2 p = ImGui::GetWindowPos() + ImVec2(rx, oy + 68);
		dl->AddRectFilled(p, p + ImVec2(rw, H - 46 - oy - 68 - 8), WHITE, 8.0f);
		dl->AddRect(p, p + ImVec2(rw, H - 46 - oy - 68 - 8), RULE, 8.0f);
	}
	float ix = rx + 12, iw = rw - 24;
	if (g_lgpEdit >= 0) {
		// colour editor in place of the shape rows
		Config::LgpShape& s = g_lgpEdit == 0 ? g_cfg.lgpSquare : g_cfg.lgpCircle;
		label_at(ix, oy + 80, g_lgpEdit == 0 ? "Square colour" : "Circle colour", TEXT, g_guiBold);
		save |= color_editor(g_lgpEdit == 0 ? "lgpsq" : "lgpci", s.color, ix, oy + 102);
		if (button_at(ix + iw - 70, oy + 76, "Done##lgpcol", ImVec2(70, 24)))
			g_lgpEdit = -1, save = true;
	} else {
		save |= shape_rows(ix, oy + 80, iw, "Square", g_cfg.lgpSquare, LGP_SQUARE_MAX, 0, on);
		ImVec2 wp = ImGui::GetWindowPos();
		ImGui::GetWindowDrawList()->AddLine(wp + ImVec2(ix, oy + 198), wp + ImVec2(ix + iw, oy + 198), RULE);
		save |= shape_rows(ix, oy + 210, iw, "Circle", g_cfg.lgpCircle, LGP_CIRCLE_MAX, 1, on);
		ImGui::GetWindowDrawList()->AddLine(wp + ImVec2(ix, oy + 328), wp + ImVec2(ix + iw, oy + 328), RULE);
		float ty = oy + 340, bw = (iw - 52 - 4) / 2;
		label_at(ix, ty + 4, "Tile", on ? HEADER : DISABLED);
		if (button_at(ix + 52, ty, "Box##lgptile", ImVec2(bw, 24), g_cfg.lgpTile == 0, on))
			g_cfg.lgpTile = 0, save = true;
		if (button_at(ix + 56 + bw, ty, "Round##lgptile", ImVec2(bw, 24), g_cfg.lgpTile == 1, on))
			g_cfg.lgpTile = 1, save = true;
		label_at(ix, ty + 36, "Depth", on ? HEADER : DISABLED);
		if (button_at(ix + 52, ty + 32, "2D flat##lgpdepth", ImVec2(bw, 24), g_cfg.lgpDepth == 0, on))
			g_cfg.lgpDepth = 0, save = true;
		if (button_at(ix + 56 + bw, ty + 32, "3D raised##lgpdepth", ImVec2(bw, 24), g_cfg.lgpDepth == 1, on))
			g_cfg.lgpDepth = 1, save = true;
	}
	return save;
}

// Aura tab: the Gepard-style auras the plugin draws (aura.cpp)
bool aura_tab(float W, float oy) {
	bool avail = aura_available(), on = g_cfg.auraEnabled && avail;
	bool save = false;
	label_at(16, oy + 4, "Auras", TEXT, g_guiBold);
	label_at(16, oy + 22,
	         !avail ? "Not available: this game exe isn't the one auras were made for."
	         : on   ? "On - auras drawn around you and other players"
	                : "Off - no auras are drawn",
	         !avail ? WAIT : on ? OK : HEADER);
	if (switch_at(W - 76, oy + 1, "##auraswitch", on, avail)) {
		g_cfg.auraEnabled = !g_cfg.auraEnabled;
		save = true;
	}
	float x = 16, w = W - 32, y = oy + 48;
	header(x, y, "Show");
	y += 20;
	save |= layer_row(x, y, w, "##auraown", "My aura", "the aura you wear", &g_cfg.auraOwn, on);
	save |= layer_row(x, y + 44, w, "##auraothers", "Other players", "the auras of the players around", &g_cfg.auraOthers, on);
	y += 100;
	header(x, y, "Size");
	char pct[16];
	snprintf(pct, sizeof(pct), "%d%%", g_cfg.auraSize);
	label_at(x + w - text_size(pct).x, y, pct, on ? BLUE_TEXT : DISABLED);
	slider_at(x, y + 22, w, "##aurasize", &g_cfg.auraSize, 50, 150, on);
	save |= ImGui::IsItemDeactivatedAfterEdit();
	label_at(x, y + 60, "Get an aura from the Aura Master in Prontera, or type @aura.", on ? HEADER : DISABLED);
	return save;
}

void graymap_window() {
	if (!g_graymap.open)
		return;
	const float W = g_gmTab == 1 ? 620.0f : 480.0f, H = g_gmTab == 1 ? 524.0f : g_gmTab == 2 ? 360.0f : 402.0f;
	ImVec2 disp = ImGui::GetIO().DisplaySize;
	begin_window("##graymap", "Graymap", g_graymap, ImVec2(W, H), ImVec2((disp.x - W) / 2, (disp.y - H) / 2));
	// tabs
	if (button_at(12, 30, "Graymap##gmtab", ImVec2(100, 24), g_gmTab == 0))
		g_gmTab = 0;
	if (button_at(116, 30, "LGP##gmtab", ImVec2(100, 24), g_gmTab == 1))
		g_gmTab = 1;
	if (button_at(220, 30, "Aura##gmtab", ImVec2(100, 24), g_gmTab == 2))
		g_gmTab = 2;
	{
		ImVec2 p = ImGui::GetWindowPos();
		ImGui::GetWindowDrawList()->AddLine(p + ImVec2(10, 54), p + ImVec2(W - 10, 54), RULE);
	}
	const float oy = 64;
	bool save = g_gmTab == 1 ? lgp_tab(W, H, oy) : g_gmTab == 2 ? aura_tab(W, oy) : graymap_tab(W, oy);

	ImVec2 p = ImGui::GetWindowPos();
	ImGui::GetWindowDrawList()->AddLine(p + ImVec2(10, H - 42), p + ImVec2(W - 10, H - 42), RULE);
	ImGui::PushClipRect(ImGui::GetWindowPos(), ImGui::GetWindowPos() + ImVec2(W - 90, H), true);
	if (g_gmTab == 1) {
		int count = 0;
		LgpServer st = lgp_server_state(&count);
		char line[96];
		ImU32 dot = OK;
		if (!g_cfg.lgpEnabled || !g_cfg.lgpAoe) {
			snprintf(line, sizeof(line), "Server: AoE skill cells are off");
			dot = DISABLED;
		} else if (st == LGP_SERVER_ON) {
			snprintf(line, sizeof(line), "Server: on - %d skill cell%s around you", count, count == 1 ? "" : "s");
		} else if (st == LGP_SERVER_WAITING) {
			snprintf(line, sizeof(line), "Server: asking...");
			dot = WAIT;
		} else if (st == LGP_SERVER_UNSUPPORTED) {
			snprintf(line, sizeof(line), "Server: this map-server doesn't send skill cells");
			dot = WAIT;
		} else {
			snprintf(line, sizeof(line), "Server: needs the voice server (skill cells come through it)");
			dot = WAIT;
		}
		ImGui::GetWindowDrawList()->AddCircleFilled(p + ImVec2(21, H - 23), 4.5f, dot);
		label_at(32, H - 30, line, HEADER);
	} else if (g_gmTab == 2) {
		int count = 0;
		AuraServer st = aura_server_state(&count);
		char line[96];
		ImU32 dot = OK;
		if (!g_cfg.auraEnabled) {
			snprintf(line, sizeof(line), "Server: auras are off");
			dot = DISABLED;
		} else if (st == AURA_SERVER_ON) {
			snprintf(line, sizeof(line), "Server: on - %d aura%s around you", count, count == 1 ? "" : "s");
		} else if (st == AURA_SERVER_WAITING) {
			snprintf(line, sizeof(line), "Server: asking...");
			dot = WAIT;
		} else if (st == AURA_SERVER_UNSUPPORTED) {
			snprintf(line, sizeof(line), "Server: this map-server doesn't send auras");
			dot = WAIT;
		} else {
			snprintf(line, sizeof(line), "Server: needs the voice server (auras come through it)");
			dot = WAIT;
		}
		ImGui::GetWindowDrawList()->AddCircleFilled(p + ImVec2(21, H - 23), 4.5f, dot);
		label_at(32, H - 30, line, HEADER);
	} else {
		label_at(16, H - 30, "Ctrl+Shift+G, or right-click the Graymap button.", HEADER);
	}
	ImGui::PopClipRect();
	if (button_at(W - 80, H - 34, "Close##gm", ImVec2(70, 24)))
		g_graymap.open = false;
	ImGui::End();
	if (save)
		config_save();
}

// ---------------------------------------------------------------- Player Statistics

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
	std::sort(g_bgFiles.begin(), g_bgFiles.end(),
	          [](const std::string& a, const std::string& b) { return _stricmp(a.c_str(), b.c_str()) < 0; });
}

// the chosen picture scaled to cover the window, cropped around its centre
void load_background() {
	g_bgDirty = false;
	gui_texture_free(g_bgTex);
	if (!g_cfg.stBackground[0])
		return;
	std::string path = bg_folder() + g_cfg.stBackground;
	wchar_t wpath[MAX_PATH];
	if (!MultiByteToWideChar(CP_ACP, 0, path.c_str(), -1, wpath, MAX_PATH))
		return;
	Gdiplus::Bitmap* img = Gdiplus::Bitmap::FromFile(wpath);
	if (!img || img->GetLastStatus() != Gdiplus::Ok || img->GetWidth() == 0 || img->GetHeight() == 0) {
		delete img;
		log_line("stats: cannot read background %s", path.c_str());
		return;
	}
	const int W = (int)ST_W, H = (int)ST_H;
	std::vector<uint32_t> px(W * H);
	{
		Gdiplus::Bitmap out(W, H, W * 4, PixelFormat32bppARGB, (BYTE*)px.data());
		Gdiplus::Graphics gr(&out);
		gr.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
		gr.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
		float iw = (float)img->GetWidth(), ih = (float)img->GetHeight();
		float scale = std::max(W / iw, H / ih);
		float dw = iw * scale, dh = ih * scale;
		gr.DrawImage(img, (W - dw) / 2, (H - dh) / 2, dw, dh);
	}
	delete img;
	for (uint32_t& p : px)
		p |= 0xff000000u;
	gui_texture_upload(g_bgTex, px.data(), W, H);
}

void background_changed() {
	g_bgDirty = true;
	config_save();
	g_bgNoticeAt = GetTickCount();
	if (!g_bgNoticeAt)
		g_bgNoticeAt = 1;
}

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

void ask_stats() {
	g_statsAsked = GetTickCount();
	g_statsWaiting = net_request_stats((uint8_t)g_statsTab);
}

struct Run {
	std::string text;
	ImU32 color;
};

bool is_hex6(const char* p) {
	for (int i = 0; i < 6; i++)
		if (!isxdigit((unsigned char)p[i]))
			return false;
	return true;
}

ImU32 hex_color(const char* p) {
	char buf[7];
	memcpy(buf, p, 6);
	buf[6] = 0;
	unsigned v = (unsigned)strtoul(buf, nullptr, 16);
	switch (v) {
	case 0x000000: return TEXT;
	case 0x0055FF: return BLUE_TEXT;
	case 0x777777: return HEADER;
	case 0xFF6600: return rgb(214, 96, 0);
	}
	return rgb((v >> 16) & 255, (v >> 8) & 255, v & 255);
}

std::vector<Run> parse(const std::string& line) {
	std::vector<Run> out;
	ImU32 c = TEXT;
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

// the page text, word-wrapped, continuation lines indented
void draw_page(const std::string& page, float width) {
	ImDrawList* dl = ImGui::GetWindowDrawList();
	ImVec2 o = ImGui::GetCursorScreenPos();
	const float LINE_H = 16, INDENT = 14;
	float y = 0;
	size_t start = 0;
	while (start <= page.size()) {
		size_t nl = page.find('\n', start);
		if (nl == std::string::npos)
			nl = page.size();
		std::string line = page.substr(start, nl - start);
		start = nl + 1;
		if (line.empty()) {
			if (start <= page.size())
				y += LINE_H / 2;
			continue;
		}
		bool head = line.compare(0, 7, "^0055FF") == 0 || line.compare(0, 7, "^0055AA") == 0;
		ImFont* f = head ? g_guiBold : g_guiFont;
		float x = 0;
		for (const Run& r : parse(line)) {
			size_t i = 0;
			while (i < r.text.size()) {
				size_t j = i;
				while (j < r.text.size() && r.text[j] != ' ')
					j++;
				while (j < r.text.size() && r.text[j] == ' ')
					j++;
				float ww = text_size(r.text.c_str() + i, r.text.c_str() + j, f).x;
				if (x + ww > width && x > INDENT) {
					x = INDENT;
					y += LINE_H;
				}
				text_at(dl, o + ImVec2(x, y), r.color, r.text.c_str() + i, r.text.c_str() + j, f);
				x += ww;
				i = j;
			}
		}
		y += LINE_H;
	}
	ImGui::Dummy(ImVec2(width, y));
}

void stats_window() {
	if (!g_stats.open)
		return;
	if (g_bgDirty)
		load_background();
	DWORD now = GetTickCount();
	if (net_state() == NET_CONNECTED && now - g_statsAsked >= REFRESH_MS)
		ask_stats();

	const float W = ST_W, H = ST_H;
	ImVec2 disp = ImGui::GetIO().DisplaySize;
	begin_window("##stats", "Player Statistics", g_stats, ImVec2(W, H), ImVec2((disp.x - W) / 2, (disp.y - H) / 2), &g_bgTex);
	bool bg = g_bgTex.tex != nullptr;
	int op = g_cfg.stOpacity;

	for (int i = 0; i < TAB_COUNT; i++) {
		char id[40];
		snprintf(id, sizeof(id), "%s##st%d", TABS[i], i);
		if (button_at(16, 34 + i * 32.0f, id, ImVec2(116, 28), g_statsTab == i) && g_statsTab != i) {
			g_statsTab = i;
			ask_stats();
		}
	}

	std::string page;
	DWORD gotAt;
	{
		Lock l(g_statsLock);
		page = g_statsText[g_statsTab];
		gotAt = g_statsGot[g_statsTab];
	}

	// status line text
	char buf[160];
	const char* status;
	ImU32 statusColor = HEADER;
	if (net_state() != NET_CONNECTED) {
		if (!g_cfg.host[0])
			status = "No server is set in legacia.ini.";
		else if (!net_short_code().empty()) {
			snprintf(buf, sizeof(buf), "Not signed in: type @voice %s in game chat, or relog.", net_short_code().c_str());
			status = buf;
		} else
			status = "Signing in to the server...";
		statusColor = WAIT;
	} else if (g_statsUnavailable) {
		status = "The server can't send statistics right now. Try again later.";
		statusColor = WAIT;
	} else if (gotAt == 0) {
		status = g_statsWaiting && now - g_statsAsked > TIMEOUT_MS ? "No answer from the server yet..." : "Loading...";
	} else if (g_bgNoticeAt && now - g_bgNoticeAt < 2500) {
		int n = (int)g_bgFiles.size(), cur = 0;
		for (int i = 0; i < n; i++)
			if (!_stricmp(g_bgFiles[i].c_str(), g_cfg.stBackground))
				cur = i + 1;
		if (!g_cfg.stBackground[0])
			snprintf(buf, sizeof(buf), n ? "Background: none (%d pictures in the folder)" : "Background: none - put pictures in the folder", n);
		else
			snprintf(buf, sizeof(buf), "Background %d/%d: %s", cur, n, g_cfg.stBackground);
		status = buf;
		statusColor = BLUE_TEXT;
	} else {
		DWORD age = (now - gotAt) / 1000;
		if (age < 2)
			snprintf(buf, sizeof(buf), "Live - updates every %u seconds", (unsigned)(REFRESH_MS / 1000));
		else
			snprintf(buf, sizeof(buf), "Updated %u seconds ago", (unsigned)age);
		status = buf;
		statusColor = age < 2 ? OK : HEADER;
	}

	// text panel
	ImGui::SetCursorPos(ImVec2(144, 34));
	ImGui::PushStyleColor(ImGuiCol_ChildBg, bg ? rgb(255, 255, 255, op * 255 / 100) : WHITE);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 8));
	static int shownTab = -1;
	ImGui::BeginChild("##statspanel", ImVec2(W - 16 - 144, H - 40 - 34), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
	if (shownTab != g_statsTab) {
		ImGui::SetScrollY(0);
		shownTab = g_statsTab;
	}
	if (page.empty())
		ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(HEADER), "%s", gotAt ? "Nothing to show." : status);
	else
		draw_page(page, ImGui::GetContentRegionAvail().x);
	ImGui::EndChild();
	ImGui::PopStyleVar();
	ImGui::PopStyleColor();

	// bottom row
	if (bg) {
		ImVec2 p = ImGui::GetWindowPos();
		ImGui::GetWindowDrawList()->AddRectFilled(p + ImVec2(10, H - 34), p + ImVec2(W - 236, H - 10),
		                                          rgb(255, 255, 255, op * 255 / 100), 3.0f);
	}
	ImVec2 wp = ImGui::GetWindowPos();
	ImGui::PushClipRect(wp, wp + ImVec2(W - 238, H), true);
	label_at(16, H - 29, status, statusColor, g_guiBold);
	ImGui::PopClipRect();

	if (button_at(W - 228, H - 34, "<##bg", ImVec2(22, 24)))
		cycle_background(-1);
	if (button_at(W - 204, H - 34, ">##bg", ImVec2(22, 24)))
		cycle_background(1);
	if (button_at(W - 178, H - 34, "Background", ImVec2(88, 24))) {
		scan_backgrounds();
		ImGui::OpenPopup("##bgmenu");
	}
	bool signIn = net_state() != NET_CONNECTED && !net_short_code().empty();
	if (button_at(W - 86, H - 34, signIn ? "Sign in##st" : "Refresh##st", ImVec2(70, 24))) {
		if (signIn)
			ui_type_voice_command(net_short_code());
		else
			ask_stats();
	}

	if (ImGui::BeginPopup("##bgmenu")) {
		if (ImGui::MenuItem("No picture", nullptr, !g_cfg.stBackground[0])) {
			g_cfg.stBackground[0] = '\0';
			background_changed();
		}
		for (size_t i = 0; i < g_bgFiles.size() && i < 200; i++) {
			bool on = !_stricmp(g_bgFiles[i].c_str(), g_cfg.stBackground);
			if (ImGui::MenuItem(g_bgFiles[i].c_str(), nullptr, on)) {
				snprintf(g_cfg.stBackground, sizeof(g_cfg.stBackground), "%s", g_bgFiles[i].c_str());
				background_changed();
			}
		}
		ImGui::Separator();
		if (ImGui::BeginMenu("Panel see-through")) {
			static const int OPACITY[] = {40, 55, 70, 85, 100};
			for (int o : OPACITY) {
				char t[16];
				snprintf(t, sizeof(t), o == 100 ? "Solid" : "%d%%", o);
				if (ImGui::MenuItem(t, nullptr, g_cfg.stOpacity == o)) {
					g_cfg.stOpacity = o;
					config_save();
				}
			}
			ImGui::EndMenu();
		}
		if (ImGui::MenuItem("Open backgrounds folder..."))
			ShellExecuteA(nullptr, "open", bg_folder().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
		ImGui::EndPopup();
	}

	ImGui::End();
}

void apply_style() {
	ImGuiStyle& s = ImGui::GetStyle();
	s.WindowRounding = 5.0f;
	s.WindowBorderSize = 1.0f;
	s.WindowPadding = ImVec2(10, 8);
	s.ChildRounding = 0.0f;
	s.ChildBorderSize = 1.0f;
	s.FrameRounding = 2.0f;
	s.FrameBorderSize = 1.0f;
	s.FramePadding = ImVec2(4, 3);
	s.GrabRounding = 2.0f;
	s.GrabMinSize = 10.0f;
	s.ScrollbarSize = 10.0f;
	s.ScrollbarRounding = 3.0f;
	s.PopupRounding = 3.0f;
	s.PopupBorderSize = 1.0f;
	s.ItemSpacing = ImVec2(6, 3);
	s.SelectableTextAlign = ImVec2(0.0f, 0.5f);
	ImVec4* c = s.Colors;
	auto v = [](ImU32 u) { return ImGui::ColorConvertU32ToFloat4(u); };
	c[ImGuiCol_Text] = v(TEXT);
	c[ImGuiCol_TextDisabled] = v(DISABLED);
	c[ImGuiCol_WindowBg] = v(BODY);
	c[ImGuiCol_ChildBg] = v(WHITE);
	c[ImGuiCol_PopupBg] = v(rgb(252, 252, 254));
	c[ImGuiCol_Border] = v(BORDER);
	c[ImGuiCol_BorderShadow] = v(rgb(0, 0, 0, 0));
	c[ImGuiCol_FrameBg] = v(WHITE);
	c[ImGuiCol_FrameBgHovered] = v(rgb(240, 246, 255));
	c[ImGuiCol_FrameBgActive] = v(SEL_TOP);
	c[ImGuiCol_SliderGrab] = v(rgb(110, 140, 200));
	c[ImGuiCol_SliderGrabActive] = v(rgb(70, 100, 170));
	c[ImGuiCol_CheckMark] = v(BLUE_TEXT);
	c[ImGuiCol_Button] = v(BTN_BOTTOM);
	c[ImGuiCol_ButtonHovered] = v(HOT_BOTTOM);
	c[ImGuiCol_ButtonActive] = v(SEL_BOTTOM);
	c[ImGuiCol_Header] = v(rgb(232, 240, 252));
	c[ImGuiCol_HeaderHovered] = v(rgb(240, 245, 252));
	c[ImGuiCol_HeaderActive] = v(SEL_TOP);
	c[ImGuiCol_ScrollbarBg] = v(rgb(240, 242, 246));
	c[ImGuiCol_ScrollbarGrab] = v(rgb(180, 190, 210));
	c[ImGuiCol_ScrollbarGrabHovered] = v(rgb(150, 165, 195));
	c[ImGuiCol_ScrollbarGrabActive] = v(rgb(110, 140, 200));
	c[ImGuiCol_Separator] = v(RULE);
	c[ImGuiCol_TextSelectedBg] = v(rgb(178, 202, 240));
	c[ImGuiCol_NavHighlight] = v(rgb(0, 0, 0, 0));
}

} // namespace

// ---------------------------------------------------------------- public

void gui_windows_prepare() {
	InitializeCriticalSection(&g_statsLock);
}

void gui_windows_init() {
	Gdiplus::GdiplusStartupInput gsi;
	Gdiplus::GdiplusStartup(&g_gdiplus, &gsi, nullptr);
	apply_style();
	scan_backgrounds();
}

void gui_windows_frame() {
	for (int i = 0; i < 3; i++)
		if (InterlockedExchange(&g_toggleReq[i], 0)) {
			if (i == 0)
				toggle_settings();
			else if (i == 1)
				toggle_graymap_window();
			else
				toggle_stats_window();
		}

	ImGui::PushFont(g_guiFont);
	voice_bar();
	int r = corner_button("##gmbutton", g_gmBtn, &g_cfg.gmBtnRight, &g_cfg.gmBtnTop, graymap_button_pixels);
	if (r == 1)
		set_graymap(!g_cfg.gmEnabled);
	else if (r == 2)
		toggle_graymap_window();
	r = corner_button("##stbutton", g_stBtn, &g_cfg.stBtnRight, &g_cfg.stBtnTop, stats_button_pixels);
	if (r)
		toggle_stats_window();
	settings_window();
	graymap_window();
	stats_window();
	whisper_window();
	talking_overlay();
	ImGui::PopFont();
}

void gui_windows_device_lost() {
	gui_texture_free(g_gmBtn.tex);
	gui_texture_free(g_stBtn.tex);
	gui_texture_free(g_bgTex);
	g_bgDirty = true;
}

void gui_request_toggle(int which) {
	if (which >= 0 && which < 3)
		InterlockedExchange(&g_toggleReq[which], 1);
}

void gui_key_captured() {
	g_guiCapturingKey = false;
	config_save();
}

void gui_stats_on_page(uint8_t page, const std::string& text) {
	if (page >= TAB_COUNT)
		return;
	{
		Lock l(g_statsLock);
		g_statsText[page] = text;
		g_statsGot[page] = GetTickCount();
		if (g_statsGot[page] == 0)
			g_statsGot[page] = 1;
	}
	g_statsWaiting = false;
	g_statsUnavailable = false;
}

void gui_stats_on_unavailable(uint8_t) {
	g_statsWaiting = false;
	g_statsUnavailable = true;
}
