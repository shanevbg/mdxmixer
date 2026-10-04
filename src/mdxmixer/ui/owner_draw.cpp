#include "ui/owner_draw.h"
#include <algorithm>

// Owner-drawn checkbox, radio and push button.
//
// PORTED VERBATIM from MDropDX12 (src/mDropDX12/engine.cpp at 29c4806d), which
// is where tool_window.cpp calls them from. Windows draws a themed checkbox in
// the system colours whatever else is set, so a dark tool window has to paint
// its own or wear a white box in the corner of every option.
namespace mdxm {

void draw3DEdge(HDC hdc, const RECT& rc, COLORREF hi, COLORREF shadow, bool raised) {
  COLORREF topLeft  = raised ? hi : shadow;
  COLORREF botRight = raised ? shadow : hi;

  // Top + left edges
  HPEN pen = CreatePen(PS_SOLID, 1, topLeft);
  HPEN oldPen = (HPEN)SelectObject(hdc, pen);
  MoveToEx(hdc, rc.left, rc.top, NULL);
  LineTo(hdc, rc.right - 1, rc.top);
  MoveToEx(hdc, rc.left, rc.top, NULL);
  LineTo(hdc, rc.left, rc.bottom - 1);
  SelectObject(hdc, oldPen);
  DeleteObject(pen);

  // Bottom + right edges
  pen = CreatePen(PS_SOLID, 1, botRight);
  oldPen = (HPEN)SelectObject(hdc, pen);
  MoveToEx(hdc, rc.left, rc.bottom - 1, NULL);
  LineTo(hdc, rc.right, rc.bottom - 1);
  MoveToEx(hdc, rc.right - 1, rc.top, NULL);
  LineTo(hdc, rc.right - 1, rc.bottom);
  SelectObject(hdc, oldPen);
  DeleteObject(pen);
}

void DrawOwnerCheckbox(DRAWITEMSTRUCT* pDIS, bool bDark, COLORREF colBg, COLORREF colCtrlBg, COLORREF colBorder, COLORREF colText) {
  HDC hdc = pDIS->hDC;
  RECT rc = pDIS->rcItem;
  bool bChecked = (bool)(intptr_t)GetPropW(pDIS->hwndItem, L"Checked");
  bool bFocused = (pDIS->itemState & ODS_FOCUS) != 0;

  // Fill entire background
  HBRUSH hBrBg = CreateSolidBrush(bDark ? colBg : GetSysColor(COLOR_BTNFACE));
  FillRect(hdc, &rc, hBrBg);
  DeleteObject(hBrBg);

  // Draw checkbox indicator square, scaled to control height
  int ctrlH = rc.bottom - rc.top;
  int boxSize = std::max(ctrlH / 2, 11);
  int boxY = rc.top + (ctrlH - boxSize) / 2;
  RECT rcBox = { rc.left + 1, boxY, rc.left + 1 + boxSize, boxY + boxSize };

  if (bDark) {
    HBRUSH hBrBox = CreateSolidBrush(colCtrlBg);
    FillRect(hdc, &rcBox, hBrBox);
    DeleteObject(hBrBox);
    HBRUSH hBrBorder = CreateSolidBrush(bFocused ? RGB(100, 150, 220) : colBorder);
    FrameRect(hdc, &rcBox, hBrBorder);
    DeleteObject(hBrBorder);
  } else {
    DrawFrameControl(hdc, &rcBox, DFC_BUTTON, DFCS_BUTTONCHECK | (bChecked ? DFCS_CHECKED : 0));
    // Draw text for light mode and return
    RECT rcText = { rcBox.right + 4, rc.top, rc.right, rc.bottom };
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, GetSysColor(COLOR_BTNTEXT));
    HFONT hFont = (HFONT)SendMessage(pDIS->hwndItem, WM_GETFONT, 0, 0);
    HFONT hOld = hFont ? (HFONT)SelectObject(hdc, hFont) : NULL;
    wchar_t szText[128] = {};
    GetWindowTextW(pDIS->hwndItem, szText, 128);
    DrawTextW(hdc, szText, -1, &rcText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    if (hOld) SelectObject(hdc, hOld);
    return;
  }

  // Draw checkmark in dark mode (scaled proportionally to boxSize)
  if (bChecked) {
    int pw = std::max(boxSize / 7, 1) + 1;
    HPEN hPen = CreatePen(PS_SOLID, pw, colText);
    HPEN hOld = (HPEN)SelectObject(hdc, hPen);
    int pad = std::max(boxSize / 4, 2);
    MoveToEx(hdc, rcBox.left + pad, rcBox.top + boxSize * 6 / 10, NULL);
    LineTo(hdc, rcBox.left + boxSize * 4 / 10, rcBox.bottom - pad);
    LineTo(hdc, rcBox.right - pad, rcBox.top + pad);
    SelectObject(hdc, hOld);
    DeleteObject(hPen);
  }

  // Draw text
  RECT rcText = { rcBox.right + 4, rc.top, rc.right, rc.bottom };
  SetBkMode(hdc, TRANSPARENT);
  SetTextColor(hdc, colText);
  HFONT hFont = (HFONT)SendMessage(pDIS->hwndItem, WM_GETFONT, 0, 0);
  HFONT hOldFont = hFont ? (HFONT)SelectObject(hdc, hFont) : NULL;
  wchar_t szText[128] = {};
  GetWindowTextW(pDIS->hwndItem, szText, 128);
  DrawTextW(hdc, szText, -1, &rcText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
  if (hOldFont) SelectObject(hdc, hOldFont);

  if (bFocused) {
    RECT rcFocus = rc;
    InflateRect(&rcFocus, -1, -1);
    DrawFocusRect(hdc, &rcFocus);
  }
}

void DrawOwnerRadio(DRAWITEMSTRUCT* pDIS, bool bDark, COLORREF colBg, COLORREF colCtrlBg, COLORREF colBorder, COLORREF colText) {
  HDC hdc = pDIS->hDC;
  RECT rc = pDIS->rcItem;
  bool bChecked = (bool)(intptr_t)GetPropW(pDIS->hwndItem, L"Checked");
  bool bFocused = (pDIS->itemState & ODS_FOCUS) != 0;

  // Fill entire background
  HBRUSH hBrBg = CreateSolidBrush(bDark ? colBg : GetSysColor(COLOR_BTNFACE));
  FillRect(hdc, &rc, hBrBg);
  DeleteObject(hBrBg);

  // Draw radio circle indicator, scaled to control height
  int ctrlH = rc.bottom - rc.top;
  int circSize = std::max(ctrlH / 2, 11);
  int circY = rc.top + (ctrlH - circSize) / 2;
  int cx = rc.left + 1 + circSize / 2;
  int cy = circY + circSize / 2;
  int r = circSize / 2;

  if (bDark) {
    // Draw circle background
    HBRUSH hBrCirc = CreateSolidBrush(colCtrlBg);
    HBRUSH hBrBorderBr = CreateSolidBrush(bFocused ? RGB(100, 150, 220) : colBorder);
    HPEN hPenBorder = CreatePen(PS_SOLID, 1, bFocused ? RGB(100, 150, 220) : colBorder);
    HPEN hOldPen = (HPEN)SelectObject(hdc, hPenBorder);
    HBRUSH hOldBr = (HBRUSH)SelectObject(hdc, hBrCirc);
    Ellipse(hdc, cx - r, cy - r, cx + r, cy + r);
    SelectObject(hdc, hOldBr);
    SelectObject(hdc, hOldPen);
    DeleteObject(hBrCirc);
    DeleteObject(hBrBorderBr);
    DeleteObject(hPenBorder);
  } else {
    RECT rcRadio = { cx - r, cy - r, cx + r, cy + r };
    DrawFrameControl(hdc, &rcRadio, DFC_BUTTON, DFCS_BUTTONRADIO | (bChecked ? DFCS_CHECKED : 0));
    // Draw text for light mode and return
    RECT rcText = { rc.left + 1 + circSize + 4, rc.top, rc.right, rc.bottom };
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, GetSysColor(COLOR_BTNTEXT));
    HFONT hFont = (HFONT)SendMessage(pDIS->hwndItem, WM_GETFONT, 0, 0);
    HFONT hOld = hFont ? (HFONT)SelectObject(hdc, hFont) : NULL;
    wchar_t szText[128] = {};
    GetWindowTextW(pDIS->hwndItem, szText, 128);
    DrawTextW(hdc, szText, -1, &rcText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    if (hOld) SelectObject(hdc, hOld);
    return;
  }

  // Draw filled dot in dark mode when selected
  if (bChecked) {
    int dotR = std::max(r / 2, 2);
    HBRUSH hBrDot = CreateSolidBrush(colText);
    HPEN hPenDot = CreatePen(PS_SOLID, 1, colText);
    HPEN hOldPen = (HPEN)SelectObject(hdc, hPenDot);
    HBRUSH hOldBr = (HBRUSH)SelectObject(hdc, hBrDot);
    Ellipse(hdc, cx - dotR, cy - dotR, cx + dotR, cy + dotR);
    SelectObject(hdc, hOldBr);
    SelectObject(hdc, hOldPen);
    DeleteObject(hBrDot);
    DeleteObject(hPenDot);
  }

  // Draw text
  RECT rcText = { rc.left + 1 + circSize + 4, rc.top, rc.right, rc.bottom };
  SetBkMode(hdc, TRANSPARENT);
  SetTextColor(hdc, colText);
  HFONT hFont = (HFONT)SendMessage(pDIS->hwndItem, WM_GETFONT, 0, 0);
  HFONT hOldFont = hFont ? (HFONT)SelectObject(hdc, hFont) : NULL;
  wchar_t szText[128] = {};
  GetWindowTextW(pDIS->hwndItem, szText, 128);
  DrawTextW(hdc, szText, -1, &rcText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
  if (hOldFont) SelectObject(hdc, hOldFont);

  // No DrawFocusRect around the whole control: on these radios it looks
  // like a second "selected" state. Focus is the circle border above.
  (void)bFocused;
}

void DrawOwnerButton(DRAWITEMSTRUCT* pDIS, bool bDark,
  COLORREF colBtnFace, COLORREF colBtnHi, COLORREF colBtnShadow, COLORREF colText,
  bool bAccent) {
  HDC hdc = pDIS->hDC;
  RECT rc = pDIS->rcItem;
  bool pressed = (pDIS->itemState & ODS_SELECTED) != 0;
  bool focused = (pDIS->itemState & ODS_FOCUS) != 0;

  bool disabled = (pDIS->itemState & ODS_DISABLED) != 0;

  if (bAccent) {
    // Dark enough to keep white text readable, bright enough to read as a
    // warning against the normal grey face.
    colBtnFace   = RGB(150, 42, 42);
    colBtnHi     = RGB(205, 90, 90);
    colBtnShadow = RGB(90, 20, 20);
    colText      = RGB(255, 235, 235);
  }

  if (bDark) {
    // Fill button face
    HBRUSH hBrFill = CreateSolidBrush(colBtnFace);
    FillRect(hdc, &rc, hBrFill);
    DeleteObject(hBrFill);

    // 3D beveled edges (outer)
    draw3DEdge(hdc, rc, colBtnHi, colBtnShadow, !pressed);

    // Inner bevel (1px inset for thicker 3D look)
    RECT inner = { rc.left + 1, rc.top + 1, rc.right - 1, rc.bottom - 1 };
    COLORREF innerHi = RGB(75, 75, 75);   // subtle inner highlight
    COLORREF innerSh = RGB(45, 45, 45);   // subtle inner shadow
    draw3DEdge(hdc, inner, innerHi, innerSh, !pressed);

    // Focus rectangle (inside the 3D border)
    if (focused) {
      RECT rcFocus = { rc.left + 3, rc.top + 3, rc.right - 3, rc.bottom - 3 };
      DrawFocusRect(hdc, &rcFocus);
    }

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, disabled ? RGB(128, 128, 128) : colText);
  } else if (bAccent) {
    // Same red treatment as the dark path, drawn by hand: the system frame
    // control has no colour input, so accenting has to bypass it.
    HBRUSH hBrFill = CreateSolidBrush(colBtnFace);
    FillRect(hdc, &rc, hBrFill);
    DeleteObject(hBrFill);
    draw3DEdge(hdc, rc, colBtnHi, colBtnShadow, !pressed);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, disabled ? RGB(128, 128, 128) : colText);
  } else {
    // Light theme: standard system button look
    UINT edge = pressed ? DFCS_BUTTONPUSH | DFCS_PUSHED : DFCS_BUTTONPUSH;
    DrawFrameControl(hdc, &rc, DFC_BUTTON, edge);

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, GetSysColor(COLOR_BTNTEXT));
  }

  // Offset text when pressed
  RECT textRc = rc;
  if (pressed) OffsetRect(&textRc, 1, 1);

  wchar_t szText[128] = {};
  GetWindowTextW(pDIS->hwndItem, szText, 128);
  HFONT hFont = (HFONT)SendMessage(pDIS->hwndItem, WM_GETFONT, 0, 0);
  HFONT hOldFont = hFont ? (HFONT)SelectObject(hdc, hFont) : NULL;
  DrawTextW(hdc, szText, -1, &textRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  if (hOldFont) SelectObject(hdc, hOldFont);
}

} // namespace mdxm
