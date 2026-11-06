# Legacia voice chat client plugin (32-bit Windows DLL), with Graymap.
# Needs libopus built for the same target; build.sh does all of it with MinGW.
cmake_minimum_required(VERSION 3.16)
project(legacia_plugin CXX)

set(CMAKE_CXX_STANDARD 17)
find_path(OPUS_INCLUDE_DIR opus/opus.h REQUIRED)
find_library(OPUS_LIBRARY opus REQUIRED)

add_library(legacia_plugin SHARED
	src/main.cpp
	src/hooks.cpp
	src/net.cpp
	src/audio.cpp
	src/ui.cpp
	src/graymap.cpp
	src/graymap_ui.cpp
	src/graymap_button.cpp
	src/stats_ui.cpp
	src/stats_button.cpp
	src/crashguard.cpp
	src/lgp.cpp
	src/gui_d3d.cpp
	src/gui_windows.cpp
	third_party/imgui/imgui.cpp
	third_party/imgui/imgui_draw.cpp
	third_party/imgui/imgui_tables.cpp
	third_party/imgui/imgui_widgets.cpp
	third_party/imgui/backends/imgui_impl_dx9.cpp
	third_party/imgui/backends/imgui_impl_win32.cpp
)
set_target_properties(legacia_plugin PROPERTIES PREFIX "")
target_include_directories(legacia_plugin PRIVATE ${OPUS_INCLUDE_DIR} third_party/imgui third_party/imgui/backends)
target_compile_definitions(legacia_plugin PRIVATE _WIN32_WINNT=0x0601 WINVER=0x0601 NOMINMAX)
target_link_libraries(legacia_plugin PRIVATE ${OPUS_LIBRARY} ws2_32 winmm comctl32 gdi32 user32 gdiplus msimg32 shell32 dwmapi imm32)
if(MINGW)
	target_link_options(legacia_plugin PRIVATE -static -static-libgcc -static-libstdc++ -s)
endif()

# Loader: a tiny legacia.dll that loads plugins\legacia_plugin.dll (the
# DLL above, renamed). Gepard only hash-checks DLLs in the client folder, so
# this loader is approved once and plugin updates in plugins\ need no new
# approval. See README.md.
add_library(legacia_loader SHARED loader/loader.cpp)
set_target_properties(legacia_loader PROPERTIES PREFIX "" OUTPUT_NAME legacia
	RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/loader)
target_compile_definitions(legacia_loader PRIVATE _WIN32_WINNT=0x0601 WINVER=0x0601)
if(MINGW)
	target_link_options(legacia_loader PRIVATE -static -static-libgcc -static-libstdc++ -s)
endif()
