#pragma once
// One place for how the UI is measured and lettered.
//
// Two problems this solves. Controls were positioned with hardcoded pixels
// computed by hand, which drifted until the status text was drawn on top of the
// Apply button. And nothing ever called WM_SETFONT, so every control rendered
// in the ancient default bitmap face instead of the shell font — the single
// biggest reason the window looked dated.
#include <windows.h>

namespace mdxm {

struct UiMetrics {
    int dpi = 96;

    void Init(HWND hwnd);
    int S(int px96) const { return MulDiv(px96, dpi, 96); }   // scale a 96-dpi measure

    // The grid. A row is a label in the left gutter and one control beside it;
    // rows advance by rowH so they cannot collide however many there are.
    int Margin()   const { return S(16); }
    int LabelW()   const { return S(120); }
    int Gutter()   const { return S(12); }
    int CtlW()     const { return S(300); }
    int CtlH()     const { return S(24); }
    int RowH()     const { return S(32); }
    int SectionGap() const { return S(20); }
    int HeadingH() const { return S(26); }
    // Four of these plus their gaps must fit inside a 300-unit column:
    // 4*68 + 3*8 = 296. Sized to the row it lives in, not guessed.
    int BtnW()     const { return S(68); }
    int BtnH()     const { return S(28); }
    int CtlX()     const { return Margin() + LabelW() + Gutter(); }
    int RightX()   const { return CtlX() + CtlW() + S(36); }   // second column
    // Vertical nudge that centres a label's text against the control beside it.
    int LabelPad() const { return S(4); }

    // The narrowest the window can be and still show everything.
    //
    // Derived, not guessed. The Devices tab is the widest page: a left column
    // of label + control, then the failover panel beside it, and that panel
    // carries a three-column list. 860 was hardcoded at the window and the
    // panel then grew from 300 to 360 to fit the list -- so the default was
    // quietly 60 units short and every launch needed widening by hand: "the
    // window size is too narrow for the controls if I have to widden it every
    // time". Asking the layout means a panel growing again cannot repeat it.
    int FailoverPanelW() const { return S(360); }
    int MinClientW() const { return RightX() + FailoverPanelW() + Margin(); }
    int MinClientH() const { return S(560); }
};

// The shell's own UI face (Segoe UI on Windows 10/11) at the user's size,
// scaled for this window's DPI. Native settings UI should letter itself the
// way the rest of the system does; the design work is in the rhythm, not in
// importing a typeface the platform does not use.
HFONT CreateUiFont(int dpi, bool semibold);

// Set this property on a control that has its own font (an icon glyph face,
// say) and the blanket font pass will leave it alone.
constexpr wchar_t kKeepFontProp[] = L"mdxmKeepFont";

// WM_SETFONT down the whole child tree, skipping controls marked kKeepFontProp.
void ApplyFontToChildren(HWND root, HFONT font);

} // namespace mdxm
