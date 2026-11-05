// Drawing helpers and colours shared by the plugin's windows, so they all
// look like the game's own windows.
#pragma once

#include "voice.h"

// colours taken from the game's own windows
static const COLORREF C_TITLE_TOP = RGB(106, 140, 205), C_TITLE_BOTTOM = RGB(160, 186, 232);
static const COLORREF C_BODY = RGB(248, 248, 250), C_BORDER = RGB(126, 152, 200);
static const COLORREF C_TEXT = RGB(50, 50, 60), C_HEADER = RGB(110, 120, 140), C_RULE = RGB(205, 211, 224);
static const COLORREF C_SEL_TOP = RGB(214, 228, 250), C_SEL_BOTTOM = RGB(178, 202, 240), C_SEL_BORDER = RGB(110, 140, 200);
static const COLORREF C_BTN_TOP = RGB(255, 255, 255), C_BTN_BOTTOM = RGB(232, 236, 242), C_BTN_BORDER = RGB(160, 170, 190);
static const COLORREF C_BLUE_TEXT = RGB(40, 80, 190), C_SEL_TEXT = RGB(30, 50, 110), C_DISABLED = RGB(165, 168, 178);
static const COLORREF C_OK = RGB(0, 150, 40), C_WAIT = RGB(170, 110, 0);

extern HFONT g_font, g_bold;
extern HBRUSH g_bodyBrush, g_whiteBrush;

void fill(HDC dc, const RECT& r, COLORREF c);
void frame(HDC dc, const RECT& r, COLORREF c);
void gradient(HDC dc, const RECT& r, COLORREF top, COLORREF bottom); // top to bottom
void draw_close_x(const DRAWITEMSTRUCT* d); // round close button on a title bar
