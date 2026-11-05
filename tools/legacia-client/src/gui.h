// Internal header for the in-game drawn windows (gui_d3d.cpp, gui_windows.cpp).
#pragma once

#include "voice.h"

#include <d3d9.h>

#include "imgui.h"

// gui_d3d.cpp
IDirect3DDevice9* gui_device();
extern ImFont* g_guiFont;
extern ImFont* g_guiBold;
// a texture the windows draw from (button pictures, Stats background)
struct GuiTexture {
	IDirect3DTexture9* tex = nullptr;
	int w = 0, h = 0;
};
// (re)fills tex with w*h 0xAARRGGBB pixels; creates it when the size changes
bool gui_texture_upload(GuiTexture& t, const uint32_t* argb, int w, int h);
void gui_texture_free(GuiTexture& t);

// gui_windows.cpp, all on the game's render thread
void gui_windows_prepare(); // plugin thread, before anything else here is used
void gui_windows_init();
void gui_windows_frame();         // between ImGui::NewFrame and ImGui::Render
void gui_windows_device_lost();   // free D3DPOOL_DEFAULT textures before Reset
// requests from the plugin thread (hotkeys), handled on the next frame
void gui_request_toggle(int which); // 0 voice settings, 1 graymap, 2 stats
extern volatile bool g_guiCapturingKey; // Voice Settings waits for a push-to-talk key
void gui_key_captured();                 // plugin thread: the key was stored in g_cfg
