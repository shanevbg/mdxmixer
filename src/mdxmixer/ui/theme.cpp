#include "theme.h"
#include "app/log.h"
#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")

namespace mdxm {

namespace {
// Windows 11 (22000+) frame attributes. Declared here rather than relying on
// the SDK headers so the build does not depend on which SDK is installed;
// older Windows simply fails the call and keeps its default frame.
constexpr DWORD kDwmUseImmersiveDarkMode = 20;
constexpr DWORD kDwmBorderColor = 34;
constexpr DWORD kDwmCaptionColor = 35;
constexpr DWORD kDwmTextColor = 36;
}

ThemeKind ResolveTheme(const std::wstring& mode, bool systemIsDark) {
    if (mode == L"dracula") return ThemeKind::Dracula;
    if (mode == L"dark")    return ThemeKind::DarkGreen;
    if (mode == L"light")   return ThemeKind::Light;
    // "system" and anything unrecognised: follow the OS, and when it is dark
    // use Dracula rather than a flat grey.
    return systemIsDark ? ThemeKind::Dracula : ThemeKind::Light;
}

ThemeColors ThemePalette(ThemeKind kind) {
    switch (kind) {
    case ThemeKind::Dracula:
        // The official Dracula palette. Purple carries emphasis, green/red
        // carry channel health — the only two places colour means something.
        return { RGB(0x28, 0x2A, 0x36),   // bg
                 RGB(0x21, 0x22, 0x2C),   // surface (darker background)
                 RGB(0xF8, 0xF8, 0xF2),   // foreground
                 RGB(0x62, 0x72, 0xA4),   // comment
                 RGB(0x44, 0x47, 0x5A),   // current line, used for hairlines
                 RGB(0x44, 0x47, 0x5A),   // selection
                 RGB(0xF8, 0xF8, 0xF2),
                 RGB(0xBD, 0x93, 0xF9),   // purple
                 RGB(0x50, 0xFA, 0x7B),   // green
                 RGB(0xFF, 0x55, 0x55),   // red
                 true };
    case ThemeKind::DarkGreen:   // the AppBooster terminal palette
        return { RGB(18, 18, 18), RGB(6, 6, 6), RGB(30, 235, 30), RGB(20, 120, 20),
                 RGB(0, 90, 0), RGB(0, 100, 0), RGB(60, 255, 60),
                 RGB(40, 245, 40), RGB(40, 245, 40), RGB(255, 85, 85), true };
    case ThemeKind::SysDark:     // neutral dark, matches the OS dark mode
        return { RGB(32, 32, 32), RGB(25, 25, 25), RGB(240, 240, 240), RGB(155, 155, 155),
                 RGB(60, 60, 60), RGB(0, 80, 120), RGB(255, 255, 255),
                 RGB(96, 205, 255), RGB(108, 203, 95), RGB(255, 107, 107), true };
    case ThemeKind::Light:
    default:
        return { GetSysColor(COLOR_BTNFACE), GetSysColor(COLOR_WINDOW),
                 GetSysColor(COLOR_WINDOWTEXT), GetSysColor(COLOR_GRAYTEXT),
                 GetSysColor(COLOR_3DSHADOW), GetSysColor(COLOR_HIGHLIGHT),
                 GetSysColor(COLOR_HIGHLIGHTTEXT), GetSysColor(COLOR_HOTLIGHT),
                 RGB(0, 128, 0), RGB(192, 0, 0), false };
    }
}

bool SystemPrefersDark() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                      0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return false;
    DWORD value = 1, size = sizeof(value);
    RegQueryValueExW(key, L"AppsUseLightTheme", nullptr, nullptr, (BYTE*)&value, &size);
    RegCloseKey(key);
    return value == 0;
}

void ThemeState::Rebuild(ThemeKind kind) {
    if (bgBrush) DeleteObject(bgBrush);
    if (surfaceBrush) DeleteObject(surfaceBrush);
    if (borderPen) DeleteObject(borderPen);
    colors = ThemePalette(kind);
    bgBrush = CreateSolidBrush(colors.bg);
    surfaceBrush = CreateSolidBrush(colors.surface);
    borderPen = CreatePen(PS_SOLID, 1, colors.border);
}

ThemeState::~ThemeState() {
    if (bgBrush) DeleteObject(bgBrush);
    if (surfaceBrush) DeleteObject(surfaceBrush);
    if (borderPen) DeleteObject(borderPen);
}

namespace {
struct ApplyCtx { const ThemeColors* t; };

BOOL CALLBACK ApplyToChild(HWND child, LPARAM lp) {
    auto* ctx = (ApplyCtx*)lp;
    wchar_t cls[64] = {};
    GetClassNameW(child, cls, 64);
    const bool dark = ctx->t->dark;
    if (_wcsicmp(cls, WC_LISTVIEWW) == 0) {
        SetWindowTheme(child, dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
        ListView_SetBkColor(child, ctx->t->surface);
        ListView_SetTextBkColor(child, ctx->t->surface);
        ListView_SetTextColor(child, ctx->t->text);
    } else if (_wcsicmp(cls, L"ComboBox") == 0 || _wcsicmp(cls, L"Edit") == 0) {
        // CFD is the combo/edit variant: it themes the field and its border,
        // which is what stops a light rectangle being drawn around each one.
        SetWindowTheme(child, dark ? L"DarkMode_CFD" : nullptr, nullptr);
    } else if (_wcsicmp(cls, L"Button") == 0 || _wcsicmp(cls, L"ScrollBar") == 0 ||
               _wcsicmp(cls, TRACKBAR_CLASSW) == 0 || _wcsicmp(cls, L"ListBox") == 0) {
        SetWindowTheme(child, dark ? L"DarkMode_Explorer" : nullptr, nullptr);
    }
    EnumChildWindows(child, ApplyToChild, lp);
    return TRUE;
}
} // namespace

void ApplyControlTheme(HWND root, const ThemeColors& t) {
    ApplyCtx ctx{ &t };
    EnumChildWindows(root, ApplyToChild, (LPARAM)&ctx);
    RedrawWindow(root, nullptr, nullptr, RDW_ERASE | RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

void ApplyWindowFrameTheme(HWND hwnd, const ThemeColors& t) {
    BOOL useDark = t.dark ? TRUE : FALSE;
    HRESULT hrDark = DwmSetWindowAttribute(hwnd, kDwmUseImmersiveDarkMode, &useDark, sizeof(useDark));
    Log(3, L"frame theme: dark=%d immersive hr=0x%08X", t.dark ? 1 : 0, (unsigned)hrDark);
    if (t.dark) {
        // Paint the caption and the outer 1px border in the theme's own colours.
        // Immersive dark mode alone leaves a light border framing the window.
        COLORREF caption = t.bg, border = t.border, text = t.text;
        HRESULT a = DwmSetWindowAttribute(hwnd, kDwmCaptionColor, &caption, sizeof(caption));
        HRESULT b = DwmSetWindowAttribute(hwnd, kDwmBorderColor, &border, sizeof(border));
        HRESULT c = DwmSetWindowAttribute(hwnd, kDwmTextColor, &text, sizeof(text));
        Log(3, L"frame colors: caption hr=0x%08X border hr=0x%08X text hr=0x%08X",
            (unsigned)a, (unsigned)b, (unsigned)c);
    } else {
        // DWMWA_COLOR_DEFAULT: hand the frame back to Windows.
        COLORREF reset = 0xFFFFFFFF;
        DwmSetWindowAttribute(hwnd, kDwmCaptionColor, &reset, sizeof(reset));
        DwmSetWindowAttribute(hwnd, kDwmBorderColor, &reset, sizeof(reset));
        DwmSetWindowAttribute(hwnd, kDwmTextColor, &reset, sizeof(reset));
    }
}

bool ThemeCtlColor(UINT msg, WPARAM wp, const ThemeState& t, LRESULT* out) {
    if (!t.colors.dark) return false;   // light: system defaults
    HDC dc = (HDC)wp;
    switch (msg) {
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
        SetTextColor(dc, t.colors.text);
        SetBkColor(dc, t.colors.bg);
        *out = (LRESULT)t.bgBrush;
        return true;
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        SetTextColor(dc, t.colors.text);
        SetBkColor(dc, t.colors.surface);
        *out = (LRESULT)t.surfaceBrush;
        return true;
    }
    return false;
}

bool ThemeTrackbarCustomDraw(LPARAM lp, const ThemeState& t, LRESULT* out) {
    auto* nm = (LPNMHDR)lp;
    if (nm->code != NM_CUSTOMDRAW) return false;
    wchar_t cls[64] = {};
    GetClassNameW(nm->hwndFrom, cls, 64);
    if (_wcsicmp(cls, TRACKBAR_CLASSW) != 0) return false;
    if (!t.colors.dark) return false;

    auto* cd = (LPNMCUSTOMDRAW)lp;
    if (cd->dwDrawStage == CDDS_PREPAINT) { *out = CDRF_NOTIFYITEMDRAW; return true; }
    if (cd->dwDrawStage != CDDS_ITEMPREPAINT) return false;

    switch (cd->dwItemSpec) {
    case TBCD_CHANNEL: {
        // A thin recessed groove rather than the default bright bar.
        RECT r = cd->rc;
        int mid = (r.top + r.bottom) / 2;
        r.top = mid - 2;
        r.bottom = mid + 2;
        HBRUSH groove = CreateSolidBrush(t.colors.surface);
        FillRect(cd->hdc, &r, groove);
        DeleteObject(groove);
        FrameRect(cd->hdc, &r, (HBRUSH)GetStockObject(NULL_BRUSH));
        *out = CDRF_SKIPDEFAULT;
        return true;
    }
    case TBCD_THUMB: {
        HBRUSH thumb = CreateSolidBrush(t.colors.accent);
        FillRect(cd->hdc, &cd->rc, thumb);
        DeleteObject(thumb);
        *out = CDRF_SKIPDEFAULT;
        return true;
    }
    case TBCD_TICS:
        *out = CDRF_SKIPDEFAULT;   // no tick marks: they only add noise here
        return true;
    }
    return false;
}

bool ThemeEraseBkgnd(HWND hwnd, WPARAM wp, const ThemeState& t, LRESULT* out) {
    if (!t.colors.dark) return false;
    RECT rc;
    GetClientRect(hwnd, &rc);
    FillRect((HDC)wp, &rc, t.bgBrush);
    *out = 1;
    return true;
}

} // namespace mdxm
