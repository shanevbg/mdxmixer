#pragma once
// Themes: "dracula" (default dark), "dark" (the AppBooster green terminal
// palette), "light", and "system" (follows the OS, using Dracula when Windows
// is in dark mode). Resolution and palettes are pure and unit-tested; the Win32
// application side lives in theme.cpp.
//
// A dark theme is not just control colours: Windows paints the caption and the
// 1px window border itself, and left alone they stay light and frame the whole
// app in white. ApplyWindowFrameTheme sets them explicitly.
#include <windows.h>
#include <string>

namespace mdxm {

enum class ThemeKind { Dracula, DarkGreen, Light, SysDark };

struct ThemeColors {
    COLORREF bg;        // page and window background
    COLORREF surface;   // inset fields: edits, lists
    COLORREF text;
    COLORREF muted;     // secondary text, disabled, hairlines' text
    COLORREF border;    // hairline dividers and control edges
    COLORREF selBg;
    COLORREF selText;
    COLORREF accent;    // section headings, active tab
    COLORREF good;      // healthy channel
    COLORREF bad;       // unhealthy channel, failure text
    bool dark;
};

// mode: config ui.theme. Unknown modes act as "system".
ThemeKind ResolveTheme(const std::wstring& mode, bool systemIsDark);
ThemeColors ThemePalette(ThemeKind kind);

// HKCU ...\Themes\Personalize\AppsUseLightTheme == 0 -> the OS is in dark mode.
bool SystemPrefersDark();

// Live theme state the UI reads: colours plus the GDI objects WM_CTLCOLOR* and
// the page painters hand back.
struct ThemeState {
    ThemeColors colors = ThemePalette(ThemeKind::Light);
    HBRUSH bgBrush = nullptr;
    HBRUSH surfaceBrush = nullptr;
    HPEN borderPen = nullptr;
    void Rebuild(ThemeKind kind);   // frees old objects, makes new ones
    ~ThemeState();
};

// Per-control visual styles (DarkMode_Explorer / DarkMode_CFD) plus listview
// colours, applied down the whole child tree. Call after creation and on every
// theme change.
void ApplyControlTheme(HWND root, const ThemeColors& t);

// Caption, window border and caption text. Without this the title bar and the
// window's outer edge stay light around a dark client area.
void ApplyWindowFrameTheme(HWND hwnd, const ThemeColors& t);

// Shared WM_CTLCOLOR*/WM_ERASEBKGND handling for the window and every page.
bool ThemeCtlColor(UINT msg, WPARAM wp, const ThemeState& t, LRESULT* out);
bool ThemeEraseBkgnd(HWND hwnd, WPARAM wp, const ThemeState& t, LRESULT* out);

// Trackbars ignore the dark visual styles and keep drawing a bright channel and
// a system-accent thumb. Faders are the main control in a mixer, so they get
// drawn here instead. Call from the page's WM_NOTIFY on NM_CUSTOMDRAW.
bool ThemeTrackbarCustomDraw(LPARAM lp, const ThemeState& t, LRESULT* out);

} // namespace mdxm
