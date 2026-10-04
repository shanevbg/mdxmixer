#include "test_framework.h"
#include "ui/theme.h"
#include "config/config.h"
#include <uxtheme.h>
#include <shlwapi.h>

using namespace mdxm;

MDXM_TEST_CASE(Theme_ComCtl32IsVersion6) {
    // Without a Common-Controls v6 manifest the process binds comctl32 5.82
    // instead of the 6.x side-by-side assembly: tabs, trackbars and listviews
    // render in classic style and every SetWindowTheme(L"DarkMode_Explorer" /
    // L"DarkMode_CFD") call is a silent no-op — the theme lands as a dark
    // window full of grey 95-era controls. (IsAppThemed() is no use here: it
    // reports system-wide theming and returns TRUE either way.) The manifest is
    // a link setting shared by every configuration, so proving it for this test
    // process proves it for the app.
    HMODULE h = LoadLibraryW(L"comctl32.dll");
    CHECK(h != nullptr);
    auto getVersion = (DLLGETVERSIONPROC)GetProcAddress(h, "DllGetVersion");
    CHECK(getVersion != nullptr);
    DLLVERSIONINFO dvi = {};
    dvi.cbSize = sizeof(dvi);
    CHECK(SUCCEEDED(getVersion(&dvi)));
    std::printf("comctl32 %u.%u\n", dvi.dwMajorVersion, dvi.dwMinorVersion);
    CHECK(dvi.dwMajorVersion >= 6);
}

MDXM_TEST_CASE(Theme_ResolveModes) {
    CHECK(ResolveTheme(L"dracula", false) == ThemeKind::Dracula);
    CHECK(ResolveTheme(L"dracula", true) == ThemeKind::Dracula);
    CHECK(ResolveTheme(L"dark", false) == ThemeKind::DarkGreen);
    CHECK(ResolveTheme(L"dark", true) == ThemeKind::DarkGreen);
    CHECK(ResolveTheme(L"light", true) == ThemeKind::Light);
    CHECK(ResolveTheme(L"light", false) == ThemeKind::Light);
    // "system" follows the OS, and uses Dracula rather than a flat grey.
    CHECK(ResolveTheme(L"system", true) == ThemeKind::Dracula);
    CHECK(ResolveTheme(L"system", false) == ThemeKind::Light);
    CHECK(ResolveTheme(L"garbage", false) == ThemeKind::Light);    // unknown mode: safe default
    CHECK(ResolveTheme(L"garbage", true) == ThemeKind::Dracula);   // still follows the OS
}

MDXM_TEST_CASE(Theme_Palettes) {
    // Dracula, the official palette.
    ThemeColors dr = ThemePalette(ThemeKind::Dracula);
    CHECK(dr.dark);
    CHECK(dr.bg == RGB(0x28, 0x2A, 0x36));
    CHECK(dr.surface == RGB(0x21, 0x22, 0x2C));
    CHECK(dr.text == RGB(0xF8, 0xF8, 0xF2));
    CHECK(dr.muted == RGB(0x62, 0x72, 0xA4));
    CHECK(dr.selBg == RGB(0x44, 0x47, 0x5A));
    CHECK(dr.accent == RGB(0xBD, 0x93, 0xF9));
    CHECK(dr.good == RGB(0x50, 0xFA, 0x7B));
    CHECK(dr.bad == RGB(0xFF, 0x55, 0x55));
    // Dark green: the AppBooster terminal palette.
    ThemeColors dg = ThemePalette(ThemeKind::DarkGreen);
    CHECK(dg.dark);
    CHECK(dg.bg == RGB(18, 18, 18));
    CHECK(dg.text == RGB(30, 235, 30));
    CHECK(dg.surface == RGB(6, 6, 6));
    CHECK(dg.selBg == RGB(0, 100, 0));
    // Light: not dark.
    ThemeColors lt = ThemePalette(ThemeKind::Light);
    CHECK(!lt.dark);
}

MDXM_TEST_CASE(Config_ThemeRoundTrips) {
    MixerConfig a;
    CHECK(a.ui.theme == L"system");                       // default
    a.ui.theme = L"dracula";
    MixerConfig b = ConfigFromJson(JsonParse(ConfigToJson(a)));
    CHECK(b.ui.theme == L"dracula");
}

MDXM_TEST_CASE(Theme_NonAsciiLiteralsSurviveCompilation) {
    // Guards the /utf-8 compile option. Sources are UTF-8 without a BOM; built
    // without /utf-8, MSVC decodes them as the system ANSI codepage and a
    // literal like this becomes mojibake on screen ("â€"" for an em dash).
    const wchar_t* dash = L"—";              // escape: correct either way
    const wchar_t* literal = L"—";                // raw source bytes
    CHECK(wcscmp(dash, literal) == 0);
    CHECK(wcslen(literal) == 1);                  // one character, not three bytes
}
