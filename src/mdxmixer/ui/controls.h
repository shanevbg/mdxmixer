#pragma once
// Control constructors, ported from MDropDX12's engine_helpers.h alongside
// tool_window.cpp. Declarations copied as they stand so the ported window code
// compiles against them unchanged.
#include <windows.h>

namespace mdxm {

HWND CreateLabel(HWND hParent, const wchar_t* text, int x, int y, int w, int h, HFONT hFont, bool visible = true);
HWND CreateEdit(HWND hParent, const wchar_t* text, int id, int x, int y, int w, int h, HFONT hFont, DWORD extraStyle = 0, bool visible = true);
HWND CreateReadonlyField(HWND hParent, const wchar_t* text, int id, int x, int y, int w, int h, HFONT hFont);
HWND CreateCheck(HWND hParent, const wchar_t* text, int id, int x, int y, int w, int h, HFONT hFont, bool checked, bool visible = true);
HWND CreateRadio(HWND hParent, const wchar_t* text, int id, int x, int y, int w, int h, HFONT hFont, bool checked, bool firstInGroup = false, bool visible = true, int radioGroup = 0);
HWND CreateBtn(HWND hParent, const wchar_t* text, int id, int x, int y, int w, int h, HFONT hFont, bool visible = true);
HWND CreateCombo(HWND hParent, int id, int x, int y, int w, int dropH, HFONT hFont,
                 DWORD style = CBS_DROPDOWNLIST | WS_VSCROLL, bool visible = true,
                 DWORD exStyle = 0);
HWND CreateListBox(HWND hParent, int id, int x, int y, int w, int h, HFONT hFont,
                   DWORD extraStyle = 0, bool visible = true,
                   DWORD exStyle = WS_EX_CLIENTEDGE);
HWND CreateSlider(HWND hParent, int id, int x, int y, int w, int h, int rangeMin, int rangeMax, int pos, bool visible = true);

} // namespace mdxm
