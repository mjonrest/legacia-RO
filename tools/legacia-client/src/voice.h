// Legacia voice chat client plugin.
// Loaded into the game client, it captures the microphone, sends Opus audio
// to tools/voice-server and plays back other players. Protocol: doc/voice_chat.md
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <stdint.h>
#include <string>
#include <vector>

// ---- channels / modes ----
enum VoiceChannel { CHANNEL_NORMAL = 0, CHANNEL_PARTY = 1, CHANNEL_GUILD = 2, CHANNEL_ROOM = 3, CHANNEL_WHISPER = 4 };
static const int CHANNEL_COUNT = 5;
static const int NAME_LENGTH = 24; // character names, as in the game
enum MicMode { MODE_PTT = 0, MODE_OPEN = 1 };

// ---- settings (legacia.ini next to the game exe) ----
struct Config {
	char host[128];
	int port;
	int enabled;
	int mode;          // MicMode
	int pttKey;        // virtual key code
	int toggleKey;     // virtual key code, used with Ctrl+Shift
	int channel;       // VoiceChannel
	int micVolume;     // percent, 0-200
	int speakerVolume; // percent, 0-200
	int voiceDetect;   // open mic only sends when louder than sensitivity
	int sensitivity;   // dBFS * -1, 10-70
	int inDevice;      // -1 = system default
	int outDevice;     // -1 = system default
	int overlay;       // show who is talking in the game window
	int bar;           // show the voice bar at the bottom right of the game window
	int deafen;        // hear nobody
	int micMuted;      // send nothing, whatever the mic mode
	char whisper[NAME_LENGTH]; // who CHANNEL_WHISPER talks to
	int gmEnabled;               // graymap on
	int gmColor[5];              // 0xRRGGBB, indexed by GraymapColor
	int gmBtnRight, gmBtnTop;     // Graymap button: distance from the game window's top right corner
	int stBtnRight, stBtnTop;     // Player Statistics button, the same way
	char stBackground[MAX_PATH];  // Player Statistics background picture (file name in plugins\stats_backgrounds), "" = none
	int stOpacity;                // how solid the text panels are over the picture, percent 30-100
	// LGP (Graymap window, LGP tab): cells drawn on the ground around the player
	int lgpEnabled;
	int lgpAoe;    // skill cells on the ground, from the map-server
	int lgpDead;   // cells you can't walk on, inside the shapes
	int lgpGutter; // a gap between cells
	int lgpSmooth; // the shapes glide with the camera instead of jumping cell by cell
	int lgpTile;   // 0 box, 1 round
	int lgpDepth;  // 0 flat (2D), 1 raised (3D)
	struct LgpShape {
		int on, color, fill, size, wave; // fill 0 = ring; size in cells; wave 0 flat, 1-5
	} lgpSquare, lgpCircle;
	// auras (Graymap window, Aura tab): Gepard-style auras drawn by the plugin
	int auraEnabled;
	int auraOwn;    // your own aura
	int auraOthers; // the auras of the players around
	int auraSize;   // percent, 50-150
};

extern Config g_cfg;
extern HMODULE g_module;
extern HWND g_gameWnd;

void config_load();
void config_save();
const char* config_ini_path();
void log_line(const char* fmt, ...);

// shared lock for small cross-thread state
struct Lock {
	CRITICAL_SECTION* cs;
	explicit Lock(CRITICAL_SECTION& c) : cs(&c) { EnterCriticalSection(cs); }
	~Lock() { LeaveCriticalSection(cs); }
};

// ---- hooks.cpp: game connection ----
bool hooks_install();
bool hooks_in_game();
void hooks_check(); // re-hook if something replaced ours; call now and then
// queue a claim code to be sent through the game connection
void hooks_queue_claim(const uint8_t code[16]);

// ---- game_login.cpp: the game's own login, to sign in without @voice ----
struct GameLogin {
	uint32_t accountId, charId, authCode;
};
bool game_login(GameLogin& out); // false on other exes, or before a map was entered

// ---- net.cpp: UDP link to the voice server ----
enum NetState { NET_OFF, NET_WAIT_GAME, NET_HELLO, NET_CLAIMING, NET_CONNECTED };
void net_start();
void net_restart(); // host/port changed
NetState net_state();
const char* net_status_text();
std::string net_short_code(); // code for @voice while signing in, else empty
uint32_t net_char_id();
void net_send_audio(uint16_t seq, uint8_t channel, const uint8_t* data, int len);
bool net_request_stats(uint8_t page); // false while voice chat isn't connected

// ---- audio.cpp ----
struct SpeakerInfo {
	uint32_t charId;
	std::string name;
	bool talking;
	bool muted;
	int volume; // percent
	int channel;
};
void audio_start();
void audio_restart_devices();
void audio_set_transmit(bool on); // push-to-talk state
bool audio_transmitting();        // currently sending voice
float audio_mic_level();          // 0..1
void audio_on_packet(uint32_t speaker, uint16_t seq, uint8_t channel, const uint8_t* data, int len);
void audio_on_speaker_name(uint32_t speaker, const char* name);
std::vector<SpeakerInfo> audio_speakers();
void audio_set_speaker_muted(uint32_t speaker, bool muted);
void audio_set_speaker_volume(uint32_t speaker, int volume);
std::vector<std::string> audio_input_devices();
std::vector<std::string> audio_output_devices();

// ---- graymap.cpp: flat coloured map cells ----
enum GraymapColor { GM_WALKABLE, GM_GRID, GM_UNWALKABLE, GM_WATER, GM_WALL, GM_COLOR_COUNT };
extern const int GM_DEFAULT_COLORS[GM_COLOR_COUNT];
bool graymap_install(); // hooks the map drawing; false on an unknown game exe
bool crashguard_install(); // catches a known null-actor crash in the game exe (crashguard.cpp)
bool graymap_available();

// ---- lgp.cpp: what LGP needs from other threads (graymap.cpp draws it) ----
struct LgpCell {
	uint16_t x, y, skill;
	uint8_t kind; // 1 = it can hit you
};
enum LgpServer { LGP_SERVER_WAITING, LGP_SERVER_ON, LGP_SERVER_UNSUPPORTED, LGP_SERVER_NO_VOICE };
static const int LGP_SQUARE_MAX = 10, LGP_CIRCLE_MAX = 14; // shape sizes in cells (half width / radius)
void lgp_init();
bool lgp_wants_cells();                      // net thread: ask the voice server for skill cells
void lgp_set_cells(const LgpCell* c, int n); // net thread
void lgp_set_no_cells();                     // net thread: the map-server can't answer
std::vector<LgpCell> lgp_cells();
LgpServer lgp_server_state(int* count);
// graymap.cpp
bool lgp_drawing(); // LGP drew something in the last second

// ---- aura.cpp: the auras of the players around, drawn in 3D (graymap.cpp calls aura_draw) ----
// The auras come from the map-server through the voice server.
void aura_init();
bool aura_install();  // false on an unknown game exe
bool aura_available();
bool aura_wants();                              // net thread: ask the voice server for the auras around
void aura_set(const uint8_t* entries, int n);   // net thread: n * {account_id(4) part1 part2 part3}
void aura_set_none();                           // net thread: the map-server can't answer
void aura_draw(void* view);                     // game thread, from the ground drawing
enum AuraServer { AURA_SERVER_WAITING, AURA_SERVER_ON, AURA_SERVER_UNSUPPORTED, AURA_SERVER_NO_VOICE };
AuraServer aura_server_state(int* count);

// ---- ui.cpp ----
void ui_run(); // runs on the plugin's main thread, never returns

// ---- graymap_ui.cpp: Graymap window (Ctrl+Shift+G) ----
void graymap_ui_create();
void graymap_ui_toggle();
void graymap_set_enabled(bool on); // switch Graymap on/off, save, refresh the window and bar
HWND graymap_ui_window();

// ---- graymap_button.cpp: Graymap button under the Cash Shop button ----
void graymap_button_create();
void graymap_button_update(bool gameActive); // place/show it; called from the UI poll
void graymap_button_refresh();               // Graymap switched on/off or colours changed

// ---- stats_ui.cpp: Player Statistics window (Ctrl+Shift+S) ----
// The text comes from the map-server through the voice server (same pages as
// @statsview), so it needs voice chat to be connected.
// types "@voice CODE" into the game chat, signing this plugin in when the
// server can't tell the player by address (several games on one connection)
void ui_type_voice_command(const std::string& code);
void stats_ui_create();
void stats_ui_toggle();
HWND stats_ui_window();
void stats_ui_on_page(uint8_t page, const std::string& text); // from the net thread
void stats_ui_on_unavailable(uint8_t page);                    // from the net thread

// ---- gui_*.cpp: the same windows drawn inside the game picture ----
// The plugin hooks the game's Direct3D 9 device and draws the voice bar,
// Voice Settings, Graymap and Player Statistics windows with Dear ImGui as
// part of the game frame (fullscreen works, nothing floats over other apps).
// If the device can't be found, the separate windows above are used instead.
void gui_install_early();   // DllMain: catch the device when the game creates it
bool gui_start();           // plugin thread, after the game window is found; false = use the old windows
void gui_loop();            // plugin thread: hotkeys, push-to-talk, hook checks; never returns
bool gui_active();
void gui_stats_on_page(uint8_t page, const std::string& text);
void gui_stats_on_unavailable(uint8_t page);
// the animated 43x43 button pictures (0x00RRGGBB), shared with the old windows
const uint32_t* graymap_button_pixels(bool hover, bool down);
const uint32_t* stats_button_pixels(bool hover, bool down);
// hooks.cpp: point one import of a module at hook (false if not imported)
bool patch_import(HMODULE exe, const char* dll, const char* name, WORD ordinal, void* hook, void** original,
                  ULONG_PTR** slot = nullptr);

// ---- stats_button.cpp: Player Statistics button under the Graymap button ----
void stats_button_create();
void stats_button_update(bool gameActive);
