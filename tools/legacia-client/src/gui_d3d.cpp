// In-game drawing: finds the game's Direct3D 9 device and draws the plugin's
// windows with Dear ImGui right before each Present, so they are part of the
// game picture (fullscreen works, nothing floats over other programs).
//
// Finding the device, without changing any game or Windows code:
//  1. DllMain points the exe's import of Direct3DCreate9Ex at us, so the
//     device is caught when the game creates it.
//  2. If the plugin was loaded too late for that, a throw-away device is made
//     to learn what a device object looks like, and the game's renderer
//     (the same object Graymap uses) is searched for the real one.
// The hooks swap the device object's own table of functions (a private copy
// with Present/Reset replaced); d3d9.dll itself is not modified.
//
// Mouse and keyboard: the game window's message function is wrapped. Clicks
// over a plugin window, and keys while a plugin text box is active, are kept
// from the game; everything else passes through unchanged.
#include "gui.h"

#include "imgui_impl_dx9.h"
#include "imgui_impl_win32.h"

#include <stdio.h>
#include <string.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

ImFont* g_guiFont;
ImFont* g_guiBold;

namespace {

typedef HRESULT(WINAPI* Create9ExFn)(UINT, IDirect3D9Ex**);
typedef HRESULT(STDMETHODCALLTYPE* CreateDeviceFn)(IDirect3D9Ex*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*,
                                                   IDirect3DDevice9**);
typedef HRESULT(STDMETHODCALLTYPE* CreateDeviceExFn)(IDirect3D9Ex*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*,
                                                     D3DDISPLAYMODEEX*, IDirect3DDevice9Ex**);
typedef HRESULT(STDMETHODCALLTYPE* PresentFn)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
typedef HRESULT(STDMETHODCALLTYPE* PresentExFn)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*, DWORD);
typedef HRESULT(STDMETHODCALLTYPE* ResetFn)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
typedef HRESULT(STDMETHODCALLTYPE* ResetExFn)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*, D3DDISPLAYMODEEX*);

// IDirect3D9Ex has 22 methods, IDirect3DDevice9Ex 134
const int D3D_SLOTS = 22, DEV_SLOTS = 134;
const int SLOT_CREATEDEVICE = 16, SLOT_CREATEDEVICEEX = 20;
const int SLOT_RESET = 16, SLOT_PRESENT = 17, SLOT_PRESENTEX = 121, SLOT_RESETEX = 132;

// the game's renderer object, see graymap.cpp (GET_DEVICE returns it)
const uintptr_t RENDERER_OBJ = 0x1380ca0;
const uintptr_t GET_DEVICE_RET = 0x55c47e; // "mov eax, 0x1380ca0" inside GET_DEVICE
const unsigned char GET_DEVICE_RET_BYTES[] = {0xb8, 0xa0, 0x0c, 0x38, 0x01};

Create9ExFn g_realCreate9Ex;
void* g_d3dVt[D3D_SLOTS];
CreateDeviceFn g_realCreateDevice;
CreateDeviceExFn g_realCreateDeviceEx;

void* g_devVt[DEV_SLOTS];
PresentFn g_realPresent;
PresentExFn g_realPresentEx;
ResetFn g_realReset;
ResetExFn g_realResetEx;

IDirect3DDevice9* volatile g_dev; // the game's device, once hooked
volatile bool g_active;           // gui_start said yes: draw
bool g_inited;                    // ImGui is set up (render thread)
bool g_initFailed;
bool g_inFrame;
WNDPROC g_realWndProc;
bool g_swallowUp[3]; // a button press went to a plugin window, so its release does too

// what a device object looks like (from a throw-away device)
void* g_knownQI;
void* g_knownPresent;

bool readable(const void* p, size_t n) {
	MEMORY_BASIC_INFORMATION mbi;
	uintptr_t a = (uintptr_t)p;
	while (n > 0) {
		if (a < 0x10000 || !VirtualQuery((void*)a, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
		    (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) ||
		    !(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
		                     PAGE_EXECUTE_WRITECOPY)))
			return false;
		uintptr_t end = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
		size_t chunk = end - a < n ? end - a : n;
		a += chunk;
		n -= chunk;
	}
	return true;
}

// ---------------------------------------------------------------- rendering

void init_imgui(IDirect3DDevice9* dev) {
	if (!g_gameWnd) {
		return; // not yet: the plugin thread is still looking for the window
	}
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.IniFilename = nullptr;
	io.LogFilename = nullptr;
	io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange; // the game draws its own cursor
	io.ConfigWindowsMoveFromTitleBarOnly = false;

	char fonts[MAX_PATH], path[MAX_PATH];
	GetWindowsDirectoryA(fonts, sizeof(fonts));
	snprintf(path, sizeof(path), "%s\\Fonts\\tahoma.ttf", fonts);
	g_guiFont = GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES ? io.Fonts->AddFontFromFileTTF(path, 13.0f) : nullptr;
	snprintf(path, sizeof(path), "%s\\Fonts\\tahomabd.ttf", fonts);
	g_guiBold = GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES ? io.Fonts->AddFontFromFileTTF(path, 13.0f) : nullptr;
	if (!g_guiFont)
		g_guiFont = io.Fonts->AddFontDefault();
	if (!g_guiBold)
		g_guiBold = g_guiFont;

	if (!ImGui_ImplWin32_Init(g_gameWnd) || !ImGui_ImplDX9_Init(dev)) {
		log_line("gui: could not set up drawing on the game's device");
		g_initFailed = true;
		return;
	}
	// leave the game's IME (chat box) alone
	ImGui::GetPlatformIO().Platform_SetImeDataFn = nullptr;

	gui_windows_init();
	g_inited = true;
	log_line("gui: drawing inside the game");
}

void render(IDirect3DDevice9* dev) {
	if (!g_active || dev != g_dev || g_inFrame || g_initFailed)
		return;
	if (!g_inited) {
		init_imgui(dev);
		if (!g_inited)
			return;
	}
	g_inFrame = true;

	ImGui_ImplDX9_NewFrame();
	ImGui_ImplWin32_NewFrame();
	ImGui::NewFrame();
	gui_windows_frame();
	ImGui::EndFrame();
	ImGui::Render();

	// draw on the back buffer even if the game left another target set
	IDirect3DSurface9 *back = nullptr, *cur = nullptr;
	D3DVIEWPORT9 vp;
	bool swapped = false;
	if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) &&
	    SUCCEEDED(dev->GetRenderTarget(0, &cur)) && cur != back) {
		dev->GetViewport(&vp);
		dev->SetRenderTarget(0, back);
		swapped = true;
	}
	if (SUCCEEDED(dev->BeginScene())) {
		ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
		dev->EndScene();
	}
	if (swapped) {
		dev->SetRenderTarget(0, cur);
		dev->SetViewport(&vp);
	}
	if (cur)
		cur->Release();
	if (back)
		back->Release();

	g_inFrame = false;
}

void before_reset(IDirect3DDevice9* dev) {
	if (g_inited && dev == g_dev) {
		gui_windows_device_lost();
		ImGui_ImplDX9_InvalidateDeviceObjects();
	}
}

HRESULT STDMETHODCALLTYPE hk_present(IDirect3DDevice9* dev, const RECT* a, const RECT* b, HWND w, const RGNDATA* r) {
	render(dev);
	return g_realPresent(dev, a, b, w, r);
}

HRESULT STDMETHODCALLTYPE hk_present_ex(IDirect3DDevice9* dev, const RECT* a, const RECT* b, HWND w, const RGNDATA* r,
                                        DWORD f) {
	render(dev);
	return g_realPresentEx(dev, a, b, w, r, f);
}

HRESULT STDMETHODCALLTYPE hk_reset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp) {
	before_reset(dev);
	return g_realReset(dev, pp);
}

HRESULT STDMETHODCALLTYPE hk_reset_ex(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp, D3DDISPLAYMODEEX* m) {
	before_reset(dev);
	return g_realResetEx(dev, pp, m);
}

// gives this one device object our copy of its function table
bool hook_device(IDirect3DDevice9* dev) {
	if (g_dev)
		return g_dev == dev;
	void*** obj = (void***)dev;
	void** vt = *obj;
	if (!readable(vt, sizeof(void*) * DEV_SLOTS))
		return false;
	memcpy(g_devVt, vt, sizeof(g_devVt));
	g_realReset = (ResetFn)vt[SLOT_RESET];
	g_realPresent = (PresentFn)vt[SLOT_PRESENT];
	g_realPresentEx = (PresentExFn)vt[SLOT_PRESENTEX];
	g_realResetEx = (ResetExFn)vt[SLOT_RESETEX];
	g_devVt[SLOT_RESET] = (void*)hk_reset;
	g_devVt[SLOT_PRESENT] = (void*)hk_present;
	g_devVt[SLOT_PRESENTEX] = (void*)hk_present_ex;
	g_devVt[SLOT_RESETEX] = (void*)hk_reset_ex;
	g_dev = dev;
	InterlockedExchangePointer((void**)obj, (void*)g_devVt);
	log_line("gui: hooked the game's Direct3D device");
	return true;
}

// ---------------------------------------------------------------- catching the device at creation

HRESULT STDMETHODCALLTYPE hk_create_device(IDirect3D9Ex* d3d, UINT a, D3DDEVTYPE t, HWND w, DWORD f, D3DPRESENT_PARAMETERS* pp,
                                           IDirect3DDevice9** out) {
	HRESULT hr = g_realCreateDevice(d3d, a, t, w, f, pp, out);
	if (SUCCEEDED(hr) && out && *out && t == D3DDEVTYPE_HAL)
		hook_device(*out);
	return hr;
}

HRESULT STDMETHODCALLTYPE hk_create_device_ex(IDirect3D9Ex* d3d, UINT a, D3DDEVTYPE t, HWND w, DWORD f,
                                              D3DPRESENT_PARAMETERS* pp, D3DDISPLAYMODEEX* m, IDirect3DDevice9Ex** out) {
	HRESULT hr = g_realCreateDeviceEx(d3d, a, t, w, f, pp, m, out);
	if (SUCCEEDED(hr) && out && *out && t == D3DDEVTYPE_HAL)
		hook_device((IDirect3DDevice9*)*out);
	return hr;
}

HRESULT WINAPI hk_create9ex(UINT sdk, IDirect3D9Ex** out) {
	HRESULT hr = g_realCreate9Ex(sdk, out);
	if (SUCCEEDED(hr) && out && *out && !g_realCreateDeviceEx) {
		void*** obj = (void***)*out;
		void** vt = *obj;
		if (readable(vt, sizeof(void*) * D3D_SLOTS)) {
			memcpy(g_d3dVt, vt, sizeof(g_d3dVt));
			g_realCreateDevice = (CreateDeviceFn)vt[SLOT_CREATEDEVICE];
			g_realCreateDeviceEx = (CreateDeviceExFn)vt[SLOT_CREATEDEVICEEX];
			g_d3dVt[SLOT_CREATEDEVICE] = (void*)hk_create_device;
			g_d3dVt[SLOT_CREATEDEVICEEX] = (void*)hk_create_device_ex;
			InterlockedExchangePointer((void**)obj, (void*)g_d3dVt);
		}
	}
	return hr;
}

// ---------------------------------------------------------------- finding it later

bool learn_device_shape() {
	if (g_knownPresent)
		return true;
	HMODULE d3d9 = LoadLibraryA("d3d9.dll");
	Create9ExFn create = d3d9 ? (Create9ExFn)GetProcAddress(d3d9, "Direct3DCreate9Ex") : nullptr;
	IDirect3D9Ex* d3d = nullptr;
	if (!create || FAILED(create(D3D_SDK_VERSION, &d3d)) || !d3d)
		return false;
	HWND w = CreateWindowExA(0, "STATIC", "", WS_POPUP, 0, 0, 16, 16, nullptr, nullptr, g_module, nullptr);
	D3DPRESENT_PARAMETERS pp = {};
	pp.Windowed = TRUE;
	pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
	pp.BackBufferFormat = D3DFMT_UNKNOWN;
	pp.hDeviceWindow = w;
	IDirect3DDevice9Ex* dev = nullptr;
	HRESULT hr = d3d->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, w, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, nullptr, &dev);
	if (SUCCEEDED(hr) && dev) {
		void** vt = *(void***)dev;
		g_knownQI = vt[0];
		g_knownPresent = vt[SLOT_PRESENT];
		dev->Release();
	} else {
		log_line("gui: test device failed (%08lx)", (unsigned long)hr);
	}
	d3d->Release();
	if (w)
		DestroyWindow(w);
	return g_knownPresent != nullptr;
}

bool looks_like_device(uintptr_t p) {
	if ((p & 3) || !readable((void*)p, sizeof(void*)))
		return false;
	void** vt = *(void***)p;
	if (!readable(vt, sizeof(void*) * (SLOT_PRESENT + 1)))
		return false;
	return vt[0] == g_knownQI && (vt[SLOT_PRESENT] == g_knownPresent || vt == g_devVt);
}

// breadth-first through the pointers of the game's renderer, a few levels deep
IDirect3DDevice9* search_renderer() {
	if (!readable((void*)GET_DEVICE_RET, sizeof(GET_DEVICE_RET_BYTES)) ||
	    memcmp((void*)GET_DEVICE_RET, GET_DEVICE_RET_BYTES, sizeof(GET_DEVICE_RET_BYTES)) != 0)
		return nullptr; // not the exe this was made for
	std::vector<uintptr_t> level = {RENDERER_OBJ}, next, seen;
	for (int depth = 0; depth < 4 && !level.empty(); depth++) {
		next.clear();
		for (uintptr_t obj : level) {
			const size_t bytes = depth == 0 ? 0x40 : 0x400;
			if (!readable((void*)obj, sizeof(uintptr_t)))
				continue;
			for (size_t off = 0; off < bytes; off += 4) {
				if (!readable((void*)(obj + off), 4))
					break;
				uintptr_t p = *(uintptr_t*)(obj + off);
				if (p < 0x10000 || (p & 3))
					continue;
				if (looks_like_device(p))
					return (IDirect3DDevice9*)p;
				bool known = false;
				for (uintptr_t s : seen)
					if (s == p) {
						known = true;
						break;
					}
				if (!known && seen.size() < 4000) {
					seen.push_back(p);
					next.push_back(p);
				}
			}
		}
		level.swap(next);
	}
	return nullptr;
}

LRESULT CALLBACK wndproc(HWND hw, UINT msg, WPARAM wp, LPARAM lp) {
	if (g_inited && ImGui::GetCurrentContext()) {
		ImGui_ImplWin32_WndProcHandler(hw, msg, wp, lp);
		ImGuiIO& io = ImGui::GetIO();
		int button = -1;
		bool down = false;
		switch (msg) {
		case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: button = 0; down = true; break;
		case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: button = 1; down = true; break;
		case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK: button = 2; down = true; break;
		case WM_LBUTTONUP: button = 0; break;
		case WM_RBUTTONUP: button = 1; break;
		case WM_MBUTTONUP: button = 2; break;
		case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
			if (io.WantCaptureMouse)
				return 0;
			break;
		case WM_KEYDOWN: case WM_KEYUP: case WM_CHAR: case WM_SYSKEYDOWN: case WM_SYSKEYUP:
		case WM_IME_CHAR: case WM_IME_COMPOSITION: case WM_IME_STARTCOMPOSITION: case WM_IME_ENDCOMPOSITION:
			if (io.WantTextInput)
				return 0;
			break;
		}
		if (button >= 0) {
			if (down) {
				g_swallowUp[button] = io.WantCaptureMouse;
				if (g_swallowUp[button])
					return 0;
			} else if (g_swallowUp[button]) {
				g_swallowUp[button] = false;
				return 0;
			}
		}
	}
	return CallWindowProcA(g_realWndProc, hw, msg, wp, lp);
}

} // namespace

// ---------------------------------------------------------------- public

IDirect3DDevice9* gui_device() {
	return g_dev;
}

bool gui_texture_upload(GuiTexture& t, const uint32_t* argb, int w, int h) {
	IDirect3DDevice9* dev = g_dev;
	if (!dev)
		return false;
	if (t.tex && (t.w != w || t.h != h))
		gui_texture_free(t);
	if (!t.tex) {
		if (FAILED(dev->CreateTexture(w, h, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &t.tex, nullptr)))
			return false;
		t.w = w;
		t.h = h;
	}
	D3DLOCKED_RECT lr;
	if (FAILED(t.tex->LockRect(0, &lr, nullptr, D3DLOCK_DISCARD)))
		return false;
	for (int y = 0; y < h; y++)
		memcpy((uint8_t*)lr.pBits + y * lr.Pitch, argb + y * w, w * 4);
	t.tex->UnlockRect(0);
	return true;
}

void gui_texture_free(GuiTexture& t) {
	if (t.tex)
		t.tex->Release();
	t.tex = nullptr;
	t.w = t.h = 0;
}

void gui_install_early() {
	if (!GetPrivateProfileIntA("ui", "ingame", 1, config_ini_path()))
		return;
	if (patch_import(GetModuleHandleA(nullptr), "d3d9.dll", "Direct3DCreate9Ex", 0, (void*)hk_create9ex,
	                 (void**)&g_realCreate9Ex))
		log_line("gui: waiting for the game to create its Direct3D device");
}

bool gui_start() {
	DWORD t0 = GetTickCount();
	bool learned = false;
	while (!g_dev && GetTickCount() - t0 < 20000) {
		if (!learned)
			learned = learn_device_shape();
		if (learned) {
			IDirect3DDevice9* dev = search_renderer();
			if (dev && hook_device(dev))
				break;
		}
		Sleep(500);
	}
	if (!g_dev) {
		log_line("gui: the game's Direct3D device was not found, using separate windows");
		return false;
	}
	g_realWndProc = (WNDPROC)SetWindowLongPtrA(g_gameWnd, GWLP_WNDPROC, (LONG_PTR)wndproc);
	if (!g_realWndProc) {
		log_line("gui: could not watch the game window's input, using separate windows");
		return false;
	}
	gui_windows_prepare();
	g_active = true;
	return true;
}

bool gui_active() {
	return g_active;
}

void gui_loop() {
	DWORD lastHookCheck = 0;
	bool keyDown[3] = {};
	for (;;) {
		Sleep(15);
		HWND fg = GetForegroundWindow();
		bool focus = fg == g_gameWnd;
		if (GetTickCount() - lastHookCheck > 2000) {
			lastHookCheck = GetTickCount();
			hooks_check();
			// something replaced the game window's message function: wrap it again
			if ((WNDPROC)GetWindowLongPtrA(g_gameWnd, GWLP_WNDPROC) != wndproc) {
				g_realWndProc = (WNDPROC)SetWindowLongPtrA(g_gameWnd, GWLP_WNDPROC, (LONG_PTR)wndproc);
				log_line("gui: the game window's input was taken over by something else, watching it again");
			}
		}

		if (g_guiCapturingKey) {
			for (int vk = 1; vk < 255; vk++) {
				if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU ||
				    vk == VK_LSHIFT || vk == VK_RSHIFT || vk == VK_LCONTROL || vk == VK_RCONTROL || vk == VK_LMENU || vk == VK_RMENU)
					continue;
				if (GetAsyncKeyState(vk) & 0x8000) {
					if (vk != VK_ESCAPE)
						g_cfg.pttKey = vk;
					gui_key_captured();
					break;
				}
			}
		}

		audio_set_transmit(focus && !g_guiCapturingKey && (GetAsyncKeyState(g_cfg.pttKey) & 0x8000));

		bool cs = focus && (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000);
		const int keys[3] = {g_cfg.toggleKey, 'G', 'S'};
		for (int i = 0; i < 3; i++) {
			bool d = cs && (GetAsyncKeyState(keys[i]) & 0x8000);
			if (d && !keyDown[i])
				gui_request_toggle(i);
			keyDown[i] = d;
		}
	}
}
