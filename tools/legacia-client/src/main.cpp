#include "voice.h"

#include <stdarg.h>
#include <stdio.h>

Config g_cfg;
HMODULE g_module;
HWND g_gameWnd;

static char g_iniPath[MAX_PATH];
static char g_logPath[MAX_PATH];
static CRITICAL_SECTION g_logLock;

static void exe_dir_file(char* out, const char* name) {
	GetModuleFileNameA(nullptr, out, MAX_PATH);
	char* slash = strrchr(out, '\\');
	if (slash)
		slash[1] = '\0';
	else
		out[0] = '\0';
	strncat(out, name, MAX_PATH - strlen(out) - 1);
}

void log_line(const char* fmt, ...) {
	Lock l(g_logLock);
	FILE* f = fopen(g_logPath, "a");
	if (!f)
		return;
	SYSTEMTIME t;
	GetLocalTime(&t);
	fprintf(f, "[%02d:%02d:%02d] ", t.wHour, t.wMinute, t.wSecond);
	va_list ap;
	va_start(ap, fmt);
	vfprintf(f, fmt, ap);
	va_end(ap);
	fputc('\n', f);
	fclose(f);
}

// default spot of the Graymap button: under the Cash Shop button, which
// sits left of the minimap
// (8 px gap so the buttons don't touch)
static const int BTN_GAP = 8, BTN_SIZE = 43;
static const int GM_BTN_DEFAULT_RIGHT = 142, GM_BTN_DEFAULT_TOP = 69;
// and the Player Statistics button right under it
static const int ST_BTN_DEFAULT_RIGHT = 142, ST_BTN_DEFAULT_TOP = GM_BTN_DEFAULT_TOP + BTN_SIZE + BTN_GAP;
// spots older plugin versions saved by default; an ini still holding them was
// never dragged by the player, so it gets the new spacing
static const int GM_BTN_OLD_TOP = 63, ST_BTN_OLD_TOP = 110;

static const char* const GM_COLOR_KEYS[GM_COLOR_COUNT] = {"walkable", "grid", "unwalkable", "water", "wall"};

static int ini_int(const char* key, int def) {
	return GetPrivateProfileIntA("voice", key, def, g_iniPath);
}

static void ini_set(const char* key, int value) {
	char buf[32];
	snprintf(buf, sizeof(buf), "%d", value);
	WritePrivateProfileStringA("voice", key, buf, g_iniPath);
}

const char* config_ini_path() {
	return g_iniPath;
}

void config_load() {
	GetPrivateProfileStringA("server", "host", "", g_cfg.host, sizeof(g_cfg.host), g_iniPath);
	g_cfg.port = GetPrivateProfileIntA("server", "port", 6951, g_iniPath);
	g_cfg.enabled = ini_int("enabled", 1);
	g_cfg.mode = ini_int("mode", MODE_PTT);
	g_cfg.pttKey = ini_int("ptt_key", VK_XBUTTON1);
	g_cfg.toggleKey = ini_int("window_key", 'V');
	g_cfg.channel = ini_int("channel", CHANNEL_PARTY);
	g_cfg.micVolume = ini_int("mic_volume", 100);
	g_cfg.speakerVolume = ini_int("speaker_volume", 100);
	g_cfg.voiceDetect = ini_int("voice_detect", 1);
	g_cfg.sensitivity = ini_int("sensitivity", 45);
	g_cfg.inDevice = ini_int("input_device", -1);
	g_cfg.outDevice = ini_int("output_device", -1);
	g_cfg.overlay = ini_int("overlay", 1);
	g_cfg.bar = ini_int("bar", 1);
	g_cfg.deafen = ini_int("deafen", 0);
	g_cfg.micMuted = ini_int("mic_muted", 0);
	GetPrivateProfileStringA("voice", "whisper", "", g_cfg.whisper, sizeof(g_cfg.whisper), g_iniPath);
	if (g_cfg.channel < 0 || g_cfg.channel >= CHANNEL_COUNT)
		g_cfg.channel = CHANNEL_PARTY;
	g_cfg.gmBtnRight = GetPrivateProfileIntA("graymap", "button_right", GM_BTN_DEFAULT_RIGHT, g_iniPath);
	g_cfg.gmBtnTop = GetPrivateProfileIntA("graymap", "button_top", GM_BTN_DEFAULT_TOP, g_iniPath);
	g_cfg.gmEnabled = GetPrivateProfileIntA("graymap", "enabled", 0, g_iniPath);
	g_cfg.stBtnRight = GetPrivateProfileIntA("stats", "button_right", ST_BTN_DEFAULT_RIGHT, g_iniPath);
	g_cfg.stBtnTop = GetPrivateProfileIntA("stats", "button_top", ST_BTN_DEFAULT_TOP, g_iniPath);
	if (g_cfg.gmBtnRight == GM_BTN_DEFAULT_RIGHT && g_cfg.gmBtnTop == GM_BTN_OLD_TOP)
		g_cfg.gmBtnTop = GM_BTN_DEFAULT_TOP;
	if (g_cfg.stBtnRight == ST_BTN_DEFAULT_RIGHT && g_cfg.stBtnTop == ST_BTN_OLD_TOP)
		g_cfg.stBtnTop = ST_BTN_DEFAULT_TOP;
	GetPrivateProfileStringA("stats", "background", "", g_cfg.stBackground, sizeof(g_cfg.stBackground), g_iniPath);
	g_cfg.stOpacity = GetPrivateProfileIntA("stats", "panel_opacity", 70, g_iniPath);
	if (g_cfg.stOpacity < 30 || g_cfg.stOpacity > 100)
		g_cfg.stOpacity = 70;
	for (int i = 0; i < GM_COLOR_COUNT; i++) {
		char buf[16];
		GetPrivateProfileStringA("graymap", GM_COLOR_KEYS[i], "", buf, sizeof(buf), g_iniPath);
		char* end;
		long v = strtol(buf, &end, 16);
		g_cfg.gmColor[i] = buf[0] && !*end ? (int)(v & 0xffffff) : GM_DEFAULT_COLORS[i];
	}
}

void config_save() {
	ini_set("enabled", g_cfg.enabled);
	ini_set("mode", g_cfg.mode);
	ini_set("ptt_key", g_cfg.pttKey);
	ini_set("window_key", g_cfg.toggleKey);
	ini_set("channel", g_cfg.channel);
	ini_set("mic_volume", g_cfg.micVolume);
	ini_set("speaker_volume", g_cfg.speakerVolume);
	ini_set("voice_detect", g_cfg.voiceDetect);
	ini_set("sensitivity", g_cfg.sensitivity);
	ini_set("input_device", g_cfg.inDevice);
	ini_set("output_device", g_cfg.outDevice);
	ini_set("overlay", g_cfg.overlay);
	ini_set("bar", g_cfg.bar);
	ini_set("deafen", g_cfg.deafen);
	ini_set("mic_muted", g_cfg.micMuted);
	WritePrivateProfileStringA("voice", "whisper", g_cfg.whisper, g_iniPath);
	char pos[16];
	snprintf(pos, sizeof(pos), "%d", g_cfg.gmBtnRight);
	WritePrivateProfileStringA("graymap", "button_right", pos, g_iniPath);
	snprintf(pos, sizeof(pos), "%d", g_cfg.gmBtnTop);
	WritePrivateProfileStringA("graymap", "button_top", pos, g_iniPath);
	WritePrivateProfileStringA("graymap", "enabled", g_cfg.gmEnabled ? "1" : "0", g_iniPath);
	snprintf(pos, sizeof(pos), "%d", g_cfg.stBtnRight);
	WritePrivateProfileStringA("stats", "button_right", pos, g_iniPath);
	snprintf(pos, sizeof(pos), "%d", g_cfg.stBtnTop);
	WritePrivateProfileStringA("stats", "button_top", pos, g_iniPath);
	WritePrivateProfileStringA("stats", "background", g_cfg.stBackground, g_iniPath);
	snprintf(pos, sizeof(pos), "%d", g_cfg.stOpacity);
	WritePrivateProfileStringA("stats", "panel_opacity", pos, g_iniPath);
	for (int i = 0; i < GM_COLOR_COUNT; i++) {
		char buf[16];
		snprintf(buf, sizeof(buf), "%06X", g_cfg.gmColor[i] & 0xffffff);
		WritePrivateProfileStringA("graymap", GM_COLOR_KEYS[i], buf, g_iniPath);
	}
}

static DWORD WINAPI plugin_main(LPVOID) {
	WSADATA wsa;
	WSAStartup(MAKEWORD(2, 2), &wsa);
	log_line("voice plugin started, server %s:%d", g_cfg.host, g_cfg.port);
	audio_start();
	net_start();
	ui_run();
	return 0;
}

extern "C" BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
	if (reason == DLL_PROCESS_ATTACH) {
		g_module = inst;
		DisableThreadLibraryCalls(inst);
		InitializeCriticalSection(&g_logLock);
		exe_dir_file(g_iniPath, "legacia.ini");
		exe_dir_file(g_logPath, "legacia.log");
		config_load();
		// the hooks must be in place before the client connects to the map-server
		if (!hooks_install())
			log_line("could not hook the game connection, signing in by address or @voice instead");
		graymap_install();
		crashguard_install();
		gui_install_early();
		HANDLE t = CreateThread(nullptr, 0, plugin_main, nullptr, 0, nullptr);
		if (t)
			CloseHandle(t);
	}
	return TRUE;
}

// Exported so the client exe can import it (WARP "Load Custom DLLs" needs one
// named function per DLL); calling it is optional, loading the DLL is enough.
extern "C" __declspec(dllexport) int LegaciaVersion() {
	return 1;
}
