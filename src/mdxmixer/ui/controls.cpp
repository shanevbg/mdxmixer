#include "ui/controls.h"
#include <commctrl.h>

// The control constructors tool_window.cpp is written against.
//
// PORTED VERBATIM from MDropDX12 (src/mDropDX12/engine.cpp at 29c4806d). Thin
// wrappers over CreateWindowExW that set the font and the common style bits,
// carried across with the framework so its call sites read the same in both
// repositories.
namespace mdxm {

//----------------------------------------------------------------------
//----------------------------------------------------------------------
//----------------------------------------------------------------------
//----------------------------------------------------------------------

HWND CreateLabel(HWND hParent, const wchar_t* text, int x, int y, int w, int h, HFONT hFont, bool visible) {
  DWORD style = WS_CHILD | SS_LEFT | (visible ? WS_VISIBLE : 0);
  HWND hw = CreateWindowExW(0, L"STATIC", text, style,
    x, y, w, h, hParent, NULL, GetModuleHandle(NULL), NULL);
  if (hw && hFont) SendMessage(hw, WM_SETFONT, (WPARAM)hFont, TRUE);
  return hw;
}

HWND CreateEdit(HWND hParent, const wchar_t* text, int id, int x, int y, int w, int h, HFONT hFont, DWORD extraStyle, bool visible) {
  DWORD style = WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL | extraStyle | (visible ? WS_VISIBLE : 0);
  HWND hw = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", text, style,
    x, y, w, h, hParent, (HMENU)(INT_PTR)id, GetModuleHandle(NULL), NULL);
  if (hw && hFont) SendMessage(hw, WM_SETFONT, (WPARAM)hFont, TRUE);
  return hw;
}

HWND CreateReadonlyField(HWND hParent, const wchar_t* text, int id, int x, int y, int w, int h, HFONT hFont) {
  // A read-only value that does not LOOK like a field you can type in.
  //
  // ES_READONLY on its own is not enough, and that mismatch is the complaint it
  // exists to answer: the control keeps the sunken WS_EX_CLIENTEDGE border, so
  // it reads as a text box, invites a click, and then refuses the keystroke.
  // Without the border it looks like the label it behaves as.
  //
  // Still an EDIT rather than a STATIC, for two reasons: the text can be
  // selected and copied, which is the one thing people actually want from a
  // path on screen; and it keeps a control ID, which other windows address it
  // by (engine_input.cpp, engine_settings_ui.cpp).
  HWND hw = CreateWindowExW(0, L"EDIT", text,
    WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_READONLY,
    x, y, w, h, hParent, (HMENU)(INT_PTR)id, GetModuleHandle(NULL), NULL);
  if (hw && hFont) SendMessage(hw, WM_SETFONT, (WPARAM)hFont, TRUE);
  return hw;
}

HWND CreateCheck(HWND hParent, const wchar_t* text, int id, int x, int y, int w, int h, HFONT hFont, bool checked, bool visible) {
  DWORD style = WS_CHILD | WS_TABSTOP | BS_OWNERDRAW | (visible ? WS_VISIBLE : 0);
  HWND hw = CreateWindowExW(0, L"BUTTON", text, style,
    x, y, w, h, hParent, (HMENU)(INT_PTR)id, GetModuleHandle(NULL), NULL);
  if (hw) {
    if (hFont) SendMessage(hw, WM_SETFONT, (WPARAM)hFont, TRUE);
    // Mark as checkbox and store check state (BS_OWNERDRAW doesn't track state)
    SetPropW(hw, L"IsCheckbox", (HANDLE)(intptr_t)1);
    SetPropW(hw, L"Checked", (HANDLE)(intptr_t)(checked ? 1 : 0));
  }
  return hw;
}

HWND CreateRadio(HWND hParent, const wchar_t* text, int id, int x, int y, int w, int h, HFONT hFont, bool checked, bool firstInGroup, bool visible, int radioGroup) {
  DWORD style = WS_CHILD | WS_TABSTOP | BS_OWNERDRAW | (visible ? WS_VISIBLE : 0);
  if (firstInGroup) style |= WS_GROUP;
  HWND hw = CreateWindowExW(0, L"BUTTON", text, style,
    x, y, w, h, hParent, (HMENU)(INT_PTR)id, GetModuleHandle(NULL), NULL);
  if (hw) {
    if (hFont) SendMessage(hw, WM_SETFONT, (WPARAM)hFont, TRUE);
    SetPropW(hw, L"IsRadio", (HANDLE)(intptr_t)1);
    SetPropW(hw, L"Checked", (HANDLE)(intptr_t)(checked ? 1 : 0));
    if (radioGroup != 0)
      SetPropW(hw, L"RadioGroup", (HANDLE)(intptr_t)radioGroup);
  }
  return hw;
}

HWND CreateBtn(HWND hParent, const wchar_t* text, int id, int x, int y, int w, int h, HFONT hFont, bool visible) {
  DWORD style = WS_CHILD | WS_TABSTOP | BS_OWNERDRAW | (visible ? WS_VISIBLE : 0);
  HWND hw = CreateWindowExW(0, L"BUTTON", text, style,
    x, y, w, h, hParent, (HMENU)(INT_PTR)id, GetModuleHandle(NULL), NULL);
  if (hw && hFont) SendMessage(hw, WM_SETFONT, (WPARAM)hFont, TRUE);
  return hw;
}

// A drop-down. WS_TABSTOP is NOT a parameter, deliberately (issue 67).
//
// Ten of the 46 hand-rolled COMBOBOX creations omitted it, and that is a
// shipped accessibility defect rather than a style nit: ToolWindow's message
// pump is `if (!IsDialogMessage(m_hWnd, &msg))` (tool_window.cpp), and
// IsDialogMessage only navigates controls that CARRY WS_TABSTOP -- it does not
// confer it. So those ten drop-downs could not be reached by keyboard at all.
// Baking it in is what stops the eleventh from happening.
//
// CBS_HASSTRINGS likewise: every one of these is a string list, and only 11 of
// the 46 said so.
//
// `dropH` is the height of the control INCLUDING its dropped list, which is the
// Win32 wart behind the 16 different height expressions at the old call sites.
HWND CreateCombo(HWND hParent, int id, int x, int y, int w, int dropH, HFONT hFont,
                 DWORD style, bool visible, DWORD exStyle) {
  DWORD s = WS_CHILD | WS_TABSTOP | CBS_HASSTRINGS | style | (visible ? WS_VISIBLE : 0);
  HWND hw = CreateWindowExW(exStyle, L"COMBOBOX", NULL, s,
    x, y, w, dropH, hParent, (HMENU)(INT_PTR)id, GetModuleHandle(NULL), NULL);
  if (hw && hFont) SendMessage(hw, WM_SETFONT, (WPARAM)hFont, TRUE);
  return hw;
}

// A plain LISTBOX -- not SysListView32, which CreateThemedListView already
// serves and which is a different control (issue 67).
HWND CreateListBox(HWND hParent, int id, int x, int y, int w, int h, HFONT hFont,
                   DWORD extraStyle, bool visible, DWORD exStyle) {
  DWORD s = WS_CHILD | WS_TABSTOP | LBS_NOTIFY | extraStyle | (visible ? WS_VISIBLE : 0);
  HWND hw = CreateWindowExW(exStyle, L"LISTBOX", NULL, s,
    x, y, w, h, hParent, (HMENU)(INT_PTR)id, GetModuleHandle(NULL), NULL);
  if (hw && hFont) SendMessage(hw, WM_SETFONT, (WPARAM)hFont, TRUE);
  return hw;
}

HWND CreateSlider(HWND hParent, int id, int x, int y, int w, int h,
                   int rangeMin, int rangeMax, int pos, bool visible) {
  DWORD style = WS_CHILD | WS_TABSTOP | TBS_HORZ | TBS_NOTICKS | (visible ? WS_VISIBLE : 0);
  HWND hw = CreateWindowExW(0, TRACKBAR_CLASSW, NULL, style,
    x, y, w, h, hParent, (HMENU)(INT_PTR)id, GetModuleHandle(NULL), NULL);
  if (hw) {
    SendMessage(hw, TBM_SETRANGE, TRUE, MAKELPARAM(rangeMin, rangeMax));
    SendMessage(hw, TBM_SETPOS, TRUE, pos);
  }
  return hw;
}

} // namespace mdxm
