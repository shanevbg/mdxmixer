#include "ui_metrics.h"

namespace mdxm {

namespace {
// GetDpiForWindow is Win10 1607+; fall back to the desktop DC so the build
// still runs somewhere older rather than laying out at the wrong scale.
using GetDpiForWindowFn = UINT(WINAPI*)(HWND);

int WindowDpi(HWND hwnd) {
    static GetDpiForWindowFn fn = [] {
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        return user32 ? (GetDpiForWindowFn)GetProcAddress(user32, "GetDpiForWindow") : nullptr;
    }();
    if (fn && hwnd) {
        UINT dpi = fn(hwnd);
        if (dpi) return (int)dpi;
    }
    HDC dc = GetDC(nullptr);
    int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSY) : 96;
    if (dc) ReleaseDC(nullptr, dc);
    return dpi ? dpi : 96;
}

BOOL CALLBACK SetFontOnChild(HWND child, LPARAM lp) {
    // A control that set its own face keeps it: the icon glyphs are drawn from
    // Segoe MDL2 Assets, and a blanket pass over the tree would replace them
    // with the text font and render them as empty boxes.
    if (!GetPropW(child, kKeepFontProp))
        SendMessageW(child, WM_SETFONT, (WPARAM)lp, TRUE);
    EnumChildWindows(child, SetFontOnChild, lp);
    return TRUE;
}
} // namespace

void UiMetrics::Init(HWND hwnd) { dpi = WindowDpi(hwnd); }

HFONT CreateUiFont(int dpi, bool semibold) {
    NONCLIENTMETRICSW ncm = {};
    ncm.cbSize = sizeof(ncm);
    LOGFONTW lf = {};
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
        lf = ncm.lfMessageFont;      // the shell's UI face at the user's size
    } else {
        lstrcpynW(lf.lfFaceName, L"Segoe UI", LF_FACESIZE);
        lf.lfHeight = -12;
    }
    // SPI returns the metrics for the system DPI; rescale for this window's.
    lf.lfHeight = MulDiv(lf.lfHeight, dpi, 96);
    if (semibold) lf.lfWeight = FW_SEMIBOLD;
    return CreateFontIndirectW(&lf);
}

void ApplyFontToChildren(HWND root, HFONT font) {
    if (!font) return;
    EnumChildWindows(root, SetFontOnChild, (LPARAM)font);
}

} // namespace mdxm
