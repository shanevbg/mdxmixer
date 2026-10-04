#pragma once
// A frameless, always-on-top battery readout.
//
// One line per connected device that reports a battery, drawn over whatever
// is on screen with no frame, no title bar and no taskbar entry. More than
// one line is not a fault: on this machine two headsets connected at once is
// usually the sign that one of them is not charging properly, so the overlay
// shows both rather than picking one.
//
// The styles — always-on-top, adjustable opacity, click-through — are the
// same combination MDropDX12 puts on its mirror windows, and the order they
// have to be applied in is ported from there along with the measurement that
// established it. See ApplyStyles().
//
// UI thread only.
#include "config/config.h"
#include "device/endpoint_volume.h"
#include "ui/theme.h"
#include <windows.h>
#include <string>
#include <vector>

namespace mdxm {

class BatteryOverlay {
public:
    // Creates the window on first use and destroys it when disabled, so a
    // disabled overlay costs nothing and cannot be left behind on screen.
    //
    // `onGeometry` is called after a drag or a resize with the position and
    // the font size, so the caller can persist them; the overlay never writes
    // config itself.
    void Apply(HINSTANCE inst, const BatteryOverlayConfig& cfg,
               const ThemeState* theme,
               std::function<void(int x, int y, int fontSize)> onGeometry);

    // The device list, already aliased and sorted. Devices are filtered and
    // de-duplicated here: a headset publishes a stereo and a hands-free
    // endpoint under one ContainerId and they carry the SAME battery, so
    // without that it would read every headset twice.
    void SetDevices(const std::vector<DeviceLevel>& devices);

    void Destroy();
    bool Visible() const { return m_hwnd != nullptr; }

    // Where it is and how big, in screen coordinates. Needed to snap it to a
    // RIGHT or BOTTOM corner, where the position depends on its own width.
    RECT Bounds() const;

    // Make it grabbable until the drag ends.
    //
    // Dragging a click-through window is impossible: WS_EX_TRANSPARENT means
    // it never receives the mouse at all. So this turns click-through off,
    // paints a solid background and goes fully opaque, so there is something
    // visible to take hold of — and then puts every one of those back from
    // config the moment the drag finishes.
    //
    // Nothing is written while it is in this state, so the temporary styles
    // cannot outlive it: not by being forgotten, and not by a crash mid-drag.
    void BeginInteractiveMove();
    bool Moving() const { return m_moving; }

private:
    void ApplyStyles();
    void Resize();
    void RebuildFont(int pt);
    // The largest font size at which every line still fits inside w x h.
    //
    // Resizing sets the FONT, not a stored box: the window is always exactly
    // as big as its text, so there is one source of truth (fontSize) instead
    // of a width and a height that can disagree with it. Dragging a corner
    // therefore scales the text, which is what "resize font to fit" means
    // when the thing being resized is nothing but text.
    int FitFontToBox(int w, int h) const;
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

    HWND m_hwnd = nullptr;
    HFONT m_font = nullptr;
    int m_fontSize = 0;
    BatteryOverlayConfig m_cfg;
    const ThemeState* m_theme = nullptr;
    std::vector<std::wstring> m_lines;
    std::vector<int> m_levels;     // parallel to m_lines; drives the colour
    std::function<void(int, int, int)> m_onGeometry;
    bool m_moving = false;      // transient: see BeginInteractiveMove
    bool m_sizing = false;      // inside a move/resize gesture
    SIZE m_sizeAtGestureStart = {};
};

// The text of one line, with the tokens substituted. Pure, so the token rules
// are tested rather than discovered by typing into the Options box.
//
//   $b   battery percent, digits only
//   $sn  the short name — our alias when one is set, else the trimmed
//        Windows name, which is what the mixer column shows
//   $n   the full Windows name
//   $$   a literal dollar
//
// An unknown token is left exactly as typed: someone writing "$5" means "$5",
// and silently eating it would be worse than printing it.
std::wstring FormatBatteryLine(const std::wstring& pattern,
                               const std::wstring& shortName,
                               const std::wstring& windowsName,
                               int battery);

// One entry per display, in the order Windows enumerates them. `rc` is the
// full monitor rectangle in VIRTUAL-screen coordinates, which is why it can
// be negative: a monitor placed left of or above the primary has negative
// origins, and on this machine they are large. That is exactly what makes
// typing coordinates hopeless and snapping necessary.
struct DisplayInfo {
    RECT rc = {};
    bool primary = false;
    std::wstring label;      // "1: 3840x2160 (primary)"
};
std::vector<DisplayInfo> Displays();

// Where a window of size `w` x `h` goes to sit in one corner of `monitor`,
// inset by a small margin so it is not flush against the edge.
//
//   corner 0 = top left   1 = top right   2 = bottom left   3 = bottom right
//
// Pure, so the arithmetic that has to cope with negative origins is tested
// rather than eyeballed on a three-monitor desk.
POINT SnapToCorner(const RECT& monitor, int w, int h, int corner);

// Which devices get a line, in order: connected, reporting a battery, one per
// physical device. Separated from the window so the rule is testable.
std::vector<const DeviceLevel*> BatteryDevices(const std::vector<DeviceLevel>& devices);

// The colour for a battery level.
//
// PORTED from MDropDX12's on-screen readout, thresholds and colours both:
// green above half, amber down to a quarter, red at a quarter and below.
// Its comment explains the shades — "muted rather than pure so they stay
// legible over a bright preset with only the one-pixel shadow behind them" —
// and the same reasoning holds here, where the overlay sits over a game.
//
// A device with no figure gets the amber of an unknown rather than the red of
// an empty one: -1 means "not reported", and printing that in alarm red says
// something the data does not.
COLORREF BatteryColor(int percent);

} // namespace mdxm
