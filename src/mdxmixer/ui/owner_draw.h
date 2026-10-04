#pragma once
// Owner-drawn controls for the dark themes, ported from MDropDX12 alongside
// tool_window.cpp — which is the only caller, and calls them by these names.
//
// Windows paints a themed checkbox and radio in the system colours no matter
// what the window around them is doing, so a dark window that does not draw
// its own wears a white box beside every option.
#include <windows.h>

namespace mdxm {

void DrawOwnerCheckbox(DRAWITEMSTRUCT* pDIS, bool bDark, COLORREF colBg,
                       COLORREF colCtrlBg, COLORREF colBorder, COLORREF colText);
void DrawOwnerRadio(DRAWITEMSTRUCT* pDIS, bool bDark, COLORREF colBg,
                    COLORREF colCtrlBg, COLORREF colBorder, COLORREF colText);
void DrawOwnerButton(DRAWITEMSTRUCT* pDIS, bool bDark, COLORREF colBtnFace,
                     COLORREF colBtnHi, COLORREF colBtnShadow, COLORREF colText,
                     bool bAccent = false);

} // namespace mdxm
