#include "ui/battery_overlay.h"
#include "device/device_identity.h"
#include <windowsx.h>
#include <algorithm>
#include <set>

namespace mdxm {

namespace {

const wchar_t* const kClass = L"mdxmixerBatteryOverlay";

// The colour keyed out when the background is off. Magenta, because it must
// not occur in the text or the panel, and because a wrong key shows up as a
// magenta block rather than as nothing at all.
constexpr COLORREF kKey = RGB(255, 0, 255);

constexpr int kPadX = 8, kPadY = 4;

// The range a resize may choose from, and the range the Options box allows.
// One pair of numbers, so dragging a corner cannot reach a size that cannot
// then be typed back.
constexpr int kMinFont = 8, kMaxFont = 72;

} // namespace

namespace {
BOOL CALLBACK CollectMonitor(HMONITOR mon, HDC, LPRECT, LPARAM param) {
    MONITORINFO mi = { sizeof mi };
    if (!GetMonitorInfoW(mon, &mi)) return TRUE;
    auto* out = (std::vector<DisplayInfo>*)param;
    DisplayInfo d;
    d.rc = mi.rcMonitor;
    d.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
    wchar_t buf[96];
    swprintf(buf, 96, L"%d: %ldx%ld%s", (int)out->size() + 1,
             mi.rcMonitor.right - mi.rcMonitor.left,
             mi.rcMonitor.bottom - mi.rcMonitor.top,
             d.primary ? L" (primary)" : L"");
    d.label = buf;
    out->push_back(d);
    return TRUE;
}
} // namespace

std::vector<DisplayInfo> Displays() {
    std::vector<DisplayInfo> out;
    EnumDisplayMonitors(nullptr, nullptr, &CollectMonitor, (LPARAM)&out);
    return out;
}

POINT SnapToCorner(const RECT& monitor, int w, int h, int corner) {
    // Not flush: a readout touching the screen edge is harder to read and, on
    // the bottom, lands under the taskbar.
    constexpr int kInset = 12;
    const bool right  = (corner == 1 || corner == 3);
    const bool bottom = (corner == 2 || corner == 3);
    POINT p;
    p.x = right  ? monitor.right  - w - kInset : monitor.left + kInset;
    p.y = bottom ? monitor.bottom - h - kInset : monitor.top  + kInset;
    return p;
}

COLORREF BatteryColor(int percent) {
    // MDropDX12 writes these as ARGB for its renderer (0xFF3CE07A, 0xFFFFC83C,
    // 0xFFFF5050); the same three, byte for byte, as COLORREF.
    if (percent < 0)  return RGB(0xFF, 0xC8, 0x3C);   // unknown, not empty
    if (percent > 50) return RGB(0x3C, 0xE0, 0x7A);
    if (percent > 25) return RGB(0xFF, 0xC8, 0x3C);
    return RGB(0xFF, 0x50, 0x50);
}

std::wstring FormatBatteryLine(const std::wstring& pattern,
                               const std::wstring& shortName,
                               const std::wstring& windowsName,
                               int battery) {
    std::wstring out;
    out.reserve(pattern.size() + 16);
    for (size_t i = 0; i < pattern.size(); ) {
        if (pattern[i] != L'$') { out += pattern[i++]; continue; }
        // Longest token first: "$sn" must be read before any "$s" that might
        // be added later, and "$$" before the single-character ones.
        if (pattern.compare(i, 3, L"$sn") == 0) { out += shortName; i += 3; continue; }
        if (pattern.compare(i, 2, L"$$") == 0)  { out += L'$';      i += 2; continue; }
        if (pattern.compare(i, 2, L"$b") == 0)  {
            // A device that reports no battery prints "--" rather than "-1",
            // which would read as a real number.
            out += (battery < 0) ? L"--" : std::to_wstring(battery);
            i += 2;
            continue;
        }
        if (pattern.compare(i, 2, L"$n") == 0)  { out += windowsName; i += 2; continue; }
        out += pattern[i++];    // not a token: print the dollar as typed
    }
    return out;
}

std::vector<const DeviceLevel*> BatteryDevices(const std::vector<DeviceLevel>& devices) {
    std::vector<const DeviceLevel*> out;
    std::set<std::wstring> seen;
    for (const DeviceLevel& d : devices) {
        if (!d.active || d.battery < 0) continue;
        // One line per PHYSICAL device. A headset publishes a stereo endpoint
        // and a hands-free one under the same ContainerId, both carrying the
        // same battery, so without this every headset is listed twice. The
        // list arrives sorted, so the first of a pair is the stereo half —
        // the one named the way the user reads it.
        const std::wstring key = IsNullContainer(d.containerId) ? d.id : d.containerId;
        if (!seen.insert(key).second) continue;
        out.push_back(&d);
    }
    return out;
}

LRESULT CALLBACK BatteryOverlay::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = (BatteryOverlay*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_NCHITTEST: {
        // Only reachable with the frame on — a WS_EX_TRANSPARENT window never
        // sees the mouse at all, which is why the frame toggle is what makes
        // the overlay touchable in the first place.
        //
        // A margin at each edge resizes, everything inside it drags. The
        // window is WS_POPUP with no WS_THICKFRAME, so there is no system
        // border to grab: returning the HT* codes from here is what gives it
        // one, without the caption and sizing border a real frame would draw
        // around what is only a line of text.
        RECT rc = {};
        GetWindowRect(hwnd, &rc);
        const POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        const LONG g = 6;      // grip width, in pixels
        const bool left = pt.x < rc.left + g, right = pt.x >= rc.right - g;
        const bool top = pt.y < rc.top + g, bottom = pt.y >= rc.bottom - g;
        if (top && left)     return HTTOPLEFT;
        if (top && right)    return HTTOPRIGHT;
        if (bottom && left)  return HTBOTTOMLEFT;
        if (bottom && right) return HTBOTTOMRIGHT;
        if (left)   return HTLEFT;
        if (right)  return HTRIGHT;
        if (top)    return HTTOP;
        if (bottom) return HTBOTTOM;
        return HTCAPTION;      // the body is its own title bar
    }

    case WM_ENTERSIZEMOVE:
        if (self) {
            RECT rc = {};
            GetWindowRect(hwnd, &rc);
            self->m_sizing = true;
            self->m_sizeAtGestureStart = { rc.right - rc.left, rc.bottom - rc.top };
        }
        return 0;

    case WM_SIZE:
        // Live, so the text scales under the pointer rather than jumping when
        // the mouse is let go. Deliberately does NOT call Resize(): that
        // snaps the window to its natural size, which during a drag would be
        // the overlay fighting the hand holding it.
        if (self && self->m_sizing) {
            const int pt = self->FitFontToBox(LOWORD(lp), HIWORD(lp));
            if (pt != self->m_fontSize) {
                self->RebuildFont(pt);
                InvalidateRect(hwnd, nullptr, TRUE);
            }
        }
        return 0;

    case WM_EXITSIZEMOVE:
        if (self) {
            RECT rc = {};
            GetWindowRect(hwnd, &rc);
            self->m_sizing = false;
            self->m_cfg.x = (int)rc.left;
            self->m_cfg.y = (int)rc.top;
            // A resize chose a font; a move left it alone. Either way the
            // caller is told both, so config and the Options boxes follow.
            self->m_cfg.fontSize = self->m_fontSize;
            if (self->m_onGeometry)
                self->m_onGeometry((int)rc.left, (int)rc.top, self->m_fontSize);
            // Out of the temporary move state and back to what config says —
            // click-through, opacity and background all restored together.
            // The frame is a MODE and stays on; only the one-shot drag that
            // "Move it" starts is transient.
            if (self->m_moving) {
                self->m_moving = false;
                self->ApplyStyles();
            }
            // Back to exactly the size of the text, at whatever font the
            // resize settled on. The dragged box was the INPUT to that
            // choice, not the result: leaving the window bigger than its text
            // would put a slab of background around it, which is the opposite
            // of "only the text Batt: ##% will appear".
            self->Resize();
            InvalidateRect(hwnd, nullptr, TRUE);
        }
        return 0;

    case WM_ERASEBKGND:
        return 1;   // painted whole in WM_PAINT; erasing first only flickers

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        if (self) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            const bool framed = self->m_cfg.frame || self->m_moving;
            const bool panel = self->m_cfg.background || framed;
            COLORREF bg = kKey, fg = RGB(255, 255, 255);
            if (self->m_theme) {
                fg = self->m_theme->colors.text;
                if (panel) bg = self->m_theme->colors.bg;
            }
            HBRUSH back = CreateSolidBrush(bg);
            FillRect(dc, &rc, back);
            DeleteObject(back);

            HGDIOBJ old = SelectObject(dc, self->m_font);
            SetBkMode(dc, TRANSPARENT);
            TEXTMETRICW tm = {};
            GetTextMetricsW(dc, &tm);
            int y = kPadY;
            for (size_t i = 0; i < self->m_lines.size(); ++i) {
                RECT lr = { kPadX, y, rc.right - kPadX, rc.bottom };
                const UINT fmt = DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX;
                // The one-pixel shadow MDropDX12 draws behind this readout.
                // Its colours are muted so they stay legible over a bright
                // preset WITH the shadow -- without it they are not, and this
                // overlay has even less to sit on than the renderer does.
                //
                // Black is safe under the colour key: the key is magenta, so
                // the shadow is never the pixel that gets removed.
                RECT sh = lr;
                OffsetRect(&sh, 1, 1);
                SetTextColor(dc, RGB(0, 0, 0));
                DrawTextW(dc, self->m_lines[i].c_str(), -1, &sh, fmt);
                // Colour by LEVEL, per line -- which is the point of having a
                // line each: with two headsets connected because one is not
                // charging, the one that is low is the red one.
                SetTextColor(dc, i < self->m_levels.size()
                                     ? BatteryColor(self->m_levels[i]) : fg);
                DrawTextW(dc, self->m_lines[i].c_str(), -1, &lr, fmt);
                y += tm.tmHeight;
            }
            SelectObject(dc, old);
            if (framed) {
                // The frame itself: what tells you this thing is draggable
                // right now, and the only visual difference between the two
                // modes once the background is on.
                HBRUSH edge = CreateSolidBrush(RGB(0x5A, 0x9B, 0xFF));
                FrameRect(dc, &rc, edge);
                DeleteObject(edge);
            }
        }
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_DESTROY:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void BatteryOverlay::ApplyStyles() {
    if (!m_hwnd) return;

    // With the frame on the overlay is a window you can grab: solid, fully
    // opaque and NOT click-through, whatever the other three settings say.
    // They are not independent — a click-through window cannot be dragged at
    // all — so the frame is what resolves them.
    const bool framed    = m_cfg.frame || m_moving;
    const bool clickThru = m_cfg.clickThrough && !framed;
    const bool panel     = m_cfg.background || framed;
    const int  opacity   = framed ? 100 : m_cfg.opacity;

    // Opacity below 100, or a colour key, or click-through: any of the three
    // needs the layered bit.
    const bool needLayered = opacity < 100 || !panel || clickThru;
    LONG_PTR ex = GetWindowLongPtrW(m_hwnd, GWL_EXSTYLE);

    // ── Reach the topmost band with CLICK-THROUGH OFF ────────────────────
    //
    // PORTED from MDropDX12 (its #403/#408). A window carrying
    // WS_EX_TRANSPARENT cannot be promoted into the topmost band:
    // SetWindowPos returns TRUE, GetLastError is 0, and the bit does not
    // change. A window that is ALREADY topmost keeps it when click-through
    // goes back on, so the ORDER is the whole problem — and an overlay that
    // is click-through from the moment it is created, which this one is by
    // default, would be refused on every attempt and sit under everything
    // permanently.
    //
    // Both bits come off for the duration of the call: microseconds, well
    // inside one compositor frame, and they go straight back below.
    if (!(ex & WS_EX_TOPMOST)) {
        const LONG_PTR bare = ex & ~(WS_EX_LAYERED | WS_EX_TRANSPARENT);
        if (bare != ex) SetWindowLongPtrW(m_hwnd, GWL_EXSTYLE, bare);
        SetWindowPos(m_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        // Re-read: the promotion changes the very word being edited below.
        ex = GetWindowLongPtrW(m_hwnd, GWL_EXSTYLE);
    }

    if (needLayered) ex |= WS_EX_LAYERED; else ex &= ~WS_EX_LAYERED;
    if (clickThru) ex |= WS_EX_TRANSPARENT; else ex &= ~WS_EX_TRANSPARENT;
    SetWindowLongPtrW(m_hwnd, GWL_EXSTYLE, ex);

    if (needLayered) {
        int pct = std::clamp(opacity, 10, 100);
        BYTE alpha = (BYTE)(pct * 255 / 100);
        if (alpha < 3) alpha = 3;   // 0 is invisible AND unfindable
        // The key and the alpha together: the key removes the background so
        // only the glyphs remain, the alpha fades what is left.
        SetLayeredWindowAttributes(m_hwnd, kKey, alpha,
                                   (panel ? 0 : LWA_COLORKEY) | LWA_ALPHA);
    }
}

void BatteryOverlay::RebuildFont(int pt) {
    if (m_font) DeleteObject(m_font);
    m_fontSize = pt;
    // NONANTIALIASED when the background is keyed out: an antialiased glyph
    // blends its edge pixels toward the background, and those blended pixels
    // are what the colour key cannot remove — the text would wear a magenta
    // halo. With a panel behind it there is nothing to key, so it can be
    // smooth, and the frame brings a panel with it.
    const bool panel = m_cfg.background || m_cfg.frame || m_moving;
    m_font = CreateFontW(-pt, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                         panel ? ANTIALIASED_QUALITY : NONANTIALIASED_QUALITY,
                         DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

int BatteryOverlay::FitFontToBox(int w, int h) const {
    if (!m_hwnd || m_lines.empty()) return m_fontSize;
    const int innerH = h - kPadY * 2, innerW = w - kPadX * 2;
    if (innerH <= 0 || innerW <= 0) return kMinFont;

    HDC dc = GetDC(m_hwnd);
    // The largest size that still fits, by binary search over exactly the
    // range the Options box allows — so a corner drag cannot reach a size
    // that then cannot be typed back.
    //
    // MEASURED rather than calculated: tmHeight is not the point size, and
    // the width depends on which glyphs the pattern produced. Both matter,
    // because dragging narrow has to shrink the text just as dragging short
    // does.
    int lo = kMinFont, hi = kMaxFont, best = kMinFont;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        HFONT f = CreateFontW(-mid, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        HGDIOBJ old = SelectObject(dc, f);
        TEXTMETRICW tm = {};
        GetTextMetricsW(dc, &tm);
        int widest = 0;
        for (const std::wstring& line : m_lines) {
            SIZE sz = {};
            GetTextExtentPoint32W(dc, line.c_str(), (int)line.size(), &sz);
            if (sz.cx > widest) widest = sz.cx;
        }
        const bool fits = (tm.tmHeight * (int)m_lines.size() <= innerH) && (widest <= innerW);
        SelectObject(dc, old);
        DeleteObject(f);
        if (fits) { best = mid; lo = mid + 1; } else { hi = mid - 1; }
    }
    ReleaseDC(m_hwnd, dc);
    return best;
}

void BatteryOverlay::Resize() {
    if (!m_hwnd || !m_font) return;
    HDC dc = GetDC(m_hwnd);
    HGDIOBJ old = SelectObject(dc, m_font);
    int w = 0, h = 0;
    TEXTMETRICW tm = {};
    GetTextMetricsW(dc, &tm);
    for (const std::wstring& line : m_lines) {
        SIZE sz = {};
        GetTextExtentPoint32W(dc, line.c_str(), (int)line.size(), &sz);
        w = std::max<int>(w, sz.cx);
        h += tm.tmHeight;
    }
    SelectObject(dc, old);
    ReleaseDC(m_hwnd, dc);
    if (m_lines.empty()) { w = 1; h = 1; }
    else { w += 1; h += 1; }      // the shadow is drawn one pixel down and right
    SetWindowPos(m_hwnd, nullptr, m_cfg.x, m_cfg.y, w + kPadX * 2, h + kPadY * 2,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    InvalidateRect(m_hwnd, nullptr, TRUE);
}

void BatteryOverlay::Apply(HINSTANCE inst, const BatteryOverlayConfig& cfg,
                           const ThemeState* theme,
                           std::function<void(int, int, int)> onGeometry) {
    // The antialiasing choice depends on whether anything is painted behind
    // the glyphs, so turning the frame or the background on has to remake the
    // font even when its size has not changed.
    const bool panelChanged = (m_cfg.background != cfg.background) ||
                              (m_cfg.frame != cfg.frame);
    m_cfg = cfg;
    m_theme = theme;
    m_onGeometry = std::move(onGeometry);

    if (!cfg.enabled) { Destroy(); return; }

    if (!m_hwnd) {
        static bool registered = false;
        if (!registered) {
            WNDCLASSEXW wc = { sizeof wc };
            wc.lpfnWndProc = &BatteryOverlay::WndProc;
            wc.hInstance = inst;
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.lpszClassName = kClass;
            RegisterClassExW(&wc);
            registered = true;
        }
        // WS_POPUP: no frame, no caption, nothing to remove afterwards.
        // WS_EX_TOOLWINDOW keeps it out of the taskbar and out of Alt-Tab;
        // WS_EX_NOACTIVATE keeps a click — when click-through is off and it
        // is being dragged — from stealing focus from a game.
        m_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                 kClass, L"", WS_POPUP,
                                 cfg.x, cfg.y, 10, 10, nullptr, nullptr, inst, nullptr);
        if (!m_hwnd) return;
        SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, (LONG_PTR)this);
        ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
    }

    if (!m_font || m_fontSize != cfg.fontSize || panelChanged)
        RebuildFont(std::clamp(cfg.fontSize, kMinFont, kMaxFont));

    ApplyStyles();
    Resize();
}

void BatteryOverlay::SetDevices(const std::vector<DeviceLevel>& devices) {
    if (!m_hwnd) return;
    std::vector<std::wstring> lines;
    std::vector<int> levels;
    for (const DeviceLevel* d : BatteryDevices(devices)) {
        lines.push_back(FormatBatteryLine(m_cfg.text, d->displayName, d->name, d->battery));
        levels.push_back(d->battery);
    }
    // Both, because a level can change the COLOUR without changing the text:
    // a pattern with no $b in it reads the same at 60% and at 20%, and the
    // whole point of the colour is that it moves when the text does not.
    if (lines == m_lines && levels == m_levels) return;
    m_lines.swap(lines);
    m_levels.swap(levels);
    Resize();
}

RECT BatteryOverlay::Bounds() const {
    RECT rc = {};
    if (m_hwnd) GetWindowRect(m_hwnd, &rc);
    return rc;
}

void BatteryOverlay::BeginInteractiveMove() {
    if (!m_hwnd) return;
    m_moving = true;
    ApplyStyles();
    InvalidateRect(m_hwnd, nullptr, TRUE);
    // Start the drag immediately, so it follows the pointer from the click
    // that asked for it rather than having to be found and grabbed. The whole
    // window answers WM_NCHITTEST with HTCAPTION, so a plain SC_MOVE moves it.
    SetForegroundWindow(m_hwnd);
    SendMessageW(m_hwnd, WM_SYSCOMMAND, SC_MOVE | 0x0002, 0);
}

void BatteryOverlay::Destroy() {
    if (m_hwnd) { DestroyWindow(m_hwnd); m_hwnd = nullptr; }
    if (m_font) { DeleteObject(m_font); m_font = nullptr; m_fontSize = 0; }
    m_sizing = false;
    m_lines.clear();
    m_levels.clear();
}

} // namespace mdxm
