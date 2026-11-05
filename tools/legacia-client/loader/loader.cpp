// Legacia plugin loader.
//
// Gepard Shield checks the hash of every DLL in the client folder, so each
// new plugin build would need a new approval. DLLs loaded from a subfolder
// are not checked. This tiny DLL is the one the game exe imports (it has the
// same name and export as the plugin, legacia.dll) and it only loads
// the real plugin from plugins\legacia_plugin.dll. It never needs to change,
// so Gepard approves it once and later plugin updates just replace the file
// in plugins\.
#include <windows.h>
#include <stdio.h>

static char g_dir[MAX_PATH]; // client folder, with a trailing backslash

static void log_line(const char* text) {
	char path[MAX_PATH];
	snprintf(path, sizeof(path), "%slegacia.log", g_dir);
	FILE* f = fopen(path, "a");
	if (!f)
		return;
	SYSTEMTIME t;
	GetLocalTime(&t);
	fprintf(f, "[%02d:%02d:%02d] loader: %s\n", t.wHour, t.wMinute, t.wSecond, text);
	fclose(f);
}

// Runs once the game's own start-up has released the loader lock
// (LoadLibrary inside DllMain is not safe).
static DWORD WINAPI load_plugin(LPVOID) {
	char path[MAX_PATH];
	snprintf(path, sizeof(path), "%splugins\\legacia_plugin.dll", g_dir);
	if (LoadLibraryA(path)) {
		log_line("loaded plugins\\legacia_plugin.dll");
	} else {
		char msg[128];
		snprintf(msg, sizeof(msg), "could not load plugins\\legacia_plugin.dll (error %lu)", GetLastError());
		log_line(msg);
	}
	return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
	if (reason == DLL_PROCESS_ATTACH) {
		DisableThreadLibraryCalls(inst);
		GetModuleFileNameA(nullptr, g_dir, MAX_PATH);
		char* slash = strrchr(g_dir, '\\');
		if (slash)
			slash[1] = '\0';
		HANDLE t = CreateThread(nullptr, 0, load_plugin, nullptr, 0, nullptr);
		if (t)
			CloseHandle(t);
	}
	return TRUE;
}

// the export the game exe imports from legacia.dll
extern "C" __declspec(dllexport) int LegaciaVersion() {
	return 1;
}
