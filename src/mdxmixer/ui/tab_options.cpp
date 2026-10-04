// Options tab: the battery overlay, and the handful of global switches that
// until now lived only in the tray menu.
//
// MDropDX12 has an Options tab carrying exactly this kind of thing — fader
// control, confirmation, the sort switches — so a reader coming from there
// finds them where they expect. Theme stays on the tray menu, because it is a
// four-way choice that is better made by pointing at it than by reading it.
#include "ui/controls.h"
#include "ui/owner_draw.h"
#include "ui/theme.h"
#include "ui/ui_context.h"
#include "ui/ui_metrics.h"
#include "ui/main_window.h"
#include <windows.h>
#include <commctrl.h>
#include <algorithm>
#include <string>

namespace mdxm {

namespace {

constexpr int kEnable = 300, kText = 301, kOpacity = 302, kOpacitySpin = 303;
constexpr int kFontSize = 304, kFontSpin = 305, kClickThru = 306, kBackground = 307;
constexpr int kPosX = 308, kPosY = 310;
constexpr int kSpinBoxes = 320, kTaskbar = 321, kAutostart = 322, kVirtual = 325;
constexpr int kStep = 323, kStepSpin = 324;
constexpr int kMoveNow = 330, kDisplay = 331, kSnapTL = 332, kSnapTR = 333;
constexpr int kHotkeys = 337;
constexpr int kSnapBL = 334, kSnapBR = 335, kSnapLabel = 408, kFrame = 336;

struct OptionsTabState {
    UiContext* ctx = nullptr;
    UiMetrics m;
    // Set while the controls are being filled in from config, so the
    // EN_CHANGE and BN_CLICKED those writes raise are not read back as the
    // user having typed something.
    bool loading = false;
};

// CreateCheck makes a BS_OWNERDRAW button, because Windows paints a themed
// checkbox in the SYSTEM colours whatever the window around it is doing — a
// dark page would wear a white box beside every option. The cost is that an
// owner-draw button tracks nothing itself: the tick lives in a "Checked"
// prop, the parent paints it on WM_DRAWITEM, and the parent flips it on
// BN_CLICKED. Miss any of the three and the box is simply not there, which is
// how this page first shipped with six invisible checkboxes.
bool Checked(HWND hwnd, int id) {
    HWND h = GetDlgItem(hwnd, id);
    return h && (bool)(intptr_t)GetPropW(h, L"Checked");
}

void SetChecked(HWND hwnd, int id, bool on) {
    if (HWND h = GetDlgItem(hwnd, id)) {
        SetPropW(h, L"Checked", (HANDLE)(intptr_t)(on ? 1 : 0));
        InvalidateRect(h, nullptr, TRUE);
    }
}

// A signed coordinate field. Signed, which is the whole reason it is not
// written with SetDlgItemInt's unsigned default anywhere.
// A field and its spinner together, so the arrows continue from what is on
// screen instead of from wherever the spinner was last left.
void SetSpin(HWND hwnd, int editId, int spinId, int value) {
    SetDlgItemInt(hwnd, editId, (UINT)value, FALSE);
    if (HWND sp = GetDlgItem(hwnd, spinId)) SendMessageW(sp, UDM_SETPOS32, 0, (LPARAM)value);
}

void SetPos(HWND hwnd, int editId, int value) {
    SetDlgItemInt(hwnd, editId, (UINT)value, TRUE);
}

int ReadInt(HWND hwnd, int id, int lo, int hi, int fallback) {
    wchar_t buf[32] = {};
    GetDlgItemTextW(hwnd, id, buf, 32);
    if (!buf[0]) return fallback;
    return std::clamp(_wtoi(buf), lo, hi);
}

// Everything on the page, read back into config in one go. Simpler than a
// handler per control, and it cannot leave two settings disagreeing.
void Save(HWND hwnd, OptionsTabState* st) {
    if (st->loading) return;
    wchar_t text[256] = {};
    GetDlgItemTextW(hwnd, kText, text, 256);

    st->ctx->store->Mutate([&](MixerConfig& c) {
        auto& b = c.batteryOverlay;
        b.enabled      = Checked(hwnd, kEnable);
        b.clickThrough = Checked(hwnd, kClickThru);
        b.background   = Checked(hwnd, kBackground);
        b.frame        = Checked(hwnd, kFrame);
        b.opacity      = ReadInt(hwnd, kOpacity, 10, 100, b.opacity);
        b.fontSize     = ReadInt(hwnd, kFontSize, 8, 72, b.fontSize);
        // The position boxes span both monitors and can go negative, which a
        // left-hand second monitor needs.
        b.x            = ReadInt(hwnd, kPosX, -32000, 32000, b.x);
        b.y            = ReadInt(hwnd, kPosY, -32000, 32000, b.y);
        if (text[0]) b.text = text;

        c.ui.spinBoxes     = Checked(hwnd, kSpinBoxes);
        c.ui.showVirtualEndpoints = Checked(hwnd, kVirtual);
        c.volumeStepPercent = ReadInt(hwnd, kStep, 1, 50, c.volumeStepPercent);
    });

    // The taskbar button and autostart are not plain config: one restyles the
    // window, the other writes an HKCU Run key, and both have owners already.
    const bool taskbar = Checked(hwnd, kTaskbar);
    if (taskbar != st->ctx->store->Get().ui.taskbarButton)
        PostMessageW(GetAncestor(hwnd, GA_ROOT), WM_COMMAND, MainWindow::kCmdTaskbar, 0);
    const bool autostart = Checked(hwnd, kAutostart);
    if (st->ctx->getAutostart && st->ctx->setAutostart &&
        autostart != st->ctx->getAutostart())
        st->ctx->setAutostart(autostart);

    // The overlay and the fader rows both change shape from these, so the
    // window is told rather than left to notice on its next tick.
    if (HWND root = GetAncestor(hwnd, GA_ROOT))
        PostMessageW(root, MainWindow::kOptionsChangedMsg, 0, 0);
}

void Load(HWND hwnd, OptionsTabState* st) {
    const MixerConfig& c = st->ctx->store->Get();
    st->loading = true;
    SetChecked(hwnd, kEnable, c.batteryOverlay.enabled);
    SetChecked(hwnd, kClickThru, c.batteryOverlay.clickThrough);
    SetChecked(hwnd, kBackground, c.batteryOverlay.background);
    SetChecked(hwnd, kFrame, c.batteryOverlay.frame);
    SetDlgItemTextW(hwnd, kText, c.batteryOverlay.text.c_str());
    SetSpin(hwnd, kOpacity, kOpacitySpin, c.batteryOverlay.opacity);
    SetSpin(hwnd, kFontSize, kFontSpin, c.batteryOverlay.fontSize);
    SetPos(hwnd, kPosX, c.batteryOverlay.x);
    SetPos(hwnd, kPosY, c.batteryOverlay.y);
    SetChecked(hwnd, kSpinBoxes, c.ui.spinBoxes);
    SetChecked(hwnd, kVirtual, c.ui.showVirtualEndpoints);
    SetChecked(hwnd, kTaskbar, c.ui.taskbarButton);
    SetSpin(hwnd, kStep, kStepSpin, c.volumeStepPercent);
    if (st->ctx->getAutostart) SetChecked(hwnd, kAutostart, st->ctx->getAutostart());
    st->loading = false;
}

// The three things the overlay changes by itself: dragging moves it, and
// resizing picks a font size. Pushed back so the boxes agree with the window,
// skipping any box the user is in the middle of editing.
void ReloadGeometry(HWND hwnd, OptionsTabState* st) {
    const BatteryOverlayConfig& b = st->ctx->store->Get().batteryOverlay;
    const HWND focus = GetFocus();
    st->loading = true;
    if (focus != GetDlgItem(hwnd, kPosX)) SetPos(hwnd, kPosX, b.x);
    if (focus != GetDlgItem(hwnd, kPosY)) SetPos(hwnd, kPosY, b.y);
    if (focus != GetDlgItem(hwnd, kFontSize))
        SetSpin(hwnd, kFontSize, kFontSpin, b.fontSize);
    st->loading = false;
}

// "Move it": one click, drag it where you want it, done.
//
// Dragging a click-through window is impossible — WS_EX_TRANSPARENT means it
// never receives the mouse at all — so the only way to move it was to untick
// click-through, drag, and tick it back, or to type coordinates: "if I have
// to move a window pixel by pixel I will go insane".
//
// This turns click-through off and paints a background so there is something
// solid to grab, WITHOUT touching the saved settings. The overlay restores
// itself from config the moment the drag ends, so the temporary state cannot
// be left behind — not by forgetting, and not by a crash mid-drag either,
// since nothing was written.
void BeginMove(HWND hwnd, OptionsTabState* st) {
    (void)st;
    if (HWND root = GetAncestor(hwnd, GA_ROOT))
        PostMessageW(root, MainWindow::kMoveOverlayMsg, 0, 0);
}

void Layout(HWND hwnd, OptionsTabState* st) {
    UiMetrics& m = st->m;
    RECT rc;
    GetClientRect(hwnd, &rc);
    const int x0 = m.Margin();
    const int labelW = m.S(150), fieldW = m.S(90), wideW = m.S(260);
    int y = m.Margin();

    auto row = [&](int labelId, int fieldId, int spinId, int fw) {
        if (HWND l = GetDlgItem(hwnd, labelId))
            MoveWindow(l, x0, y + m.S(3), labelW, m.S(20), TRUE);
        if (HWND f = GetDlgItem(hwnd, fieldId))
            MoveWindow(f, x0 + labelW, y, fw, m.S(24), TRUE);
        // UDS_ALIGNRIGHT only sizes the spinner when the buddy is (re)set, so
        // it has to be set again on every layout or the arrows stay at zero.
        if (spinId)
            if (HWND s = GetDlgItem(hwnd, spinId))
                SendMessageW(s, UDM_SETBUDDY, (WPARAM)GetDlgItem(hwnd, fieldId), 0);
        y += m.S(30);
    };
    auto heading = [&](int id) {
        if (HWND h = GetDlgItem(hwnd, id))
            MoveWindow(h, x0, y, rc.right - x0 * 2, m.S(20), TRUE);
        y += m.HeadingH() + m.S(6);
    };
    auto check = [&](int id) {
        if (HWND ck = GetDlgItem(hwnd, id))
            MoveWindow(ck, x0 + labelW, y, m.S(320), m.S(22), TRUE);
        y += m.S(28);
    };

    heading(400);
    check(kEnable);
    row(401, kText, 0, wideW);
    if (HWND hint = GetDlgItem(hwnd, 410))
        MoveWindow(hint, x0 + labelW, y, rc.right - x0 - labelW - m.S(8), m.S(20), TRUE);
    y += m.S(24);
    row(402, kOpacity, kOpacitySpin, fieldW);
    row(403, kFontSize, kFontSpin, fieldW);
    row(404, kPosX, 0, fieldW);
    row(405, kPosY, 0, fieldW);
    if (HWND b = GetDlgItem(hwnd, kMoveNow))
        MoveWindow(b, x0 + labelW, y, m.S(200), m.S(26), TRUE);
    y += m.S(32);
    // Snap: a display, then its four corners. The arithmetic is what copes
    // with a monitor whose origin is -2700; the buttons are so that nobody
    // has to know that.
    if (HWND l = GetDlgItem(hwnd, kSnapLabel))
        MoveWindow(l, x0, y + m.S(3), labelW, m.S(20), TRUE);
    if (HWND cb = GetDlgItem(hwnd, kDisplay))
        MoveWindow(cb, x0 + labelW, y, m.S(200), m.S(200), TRUE);
    y += m.S(30);
    {
        int bx = x0 + labelW;
        const int bw = m.S(128), gap = m.S(6);
        for (int id : { kSnapTL, kSnapTR, kSnapBL, kSnapBR }) {
            if (HWND b = GetDlgItem(hwnd, id))
                MoveWindow(b, bx, y, bw, m.S(26), TRUE);
            bx += bw + gap;
            // Two per line on a narrow window rather than running off it.
            if (bx + bw > rc.right - x0) { bx = x0 + labelW; y += m.S(30); }
        }
        y += m.S(32);
    }
    check(kFrame);
    check(kClickThru);
    check(kBackground);

    y += m.SectionGap();
    heading(406);
    check(kSpinBoxes);
    check(kVirtual);
    row(407, kStep, kStepSpin, fieldW);
    if (HWND hint = GetDlgItem(hwnd, 411))
        MoveWindow(hint, x0 + labelW, y, rc.right - x0 - labelW - m.S(8), m.S(20), TRUE);
    y += m.S(24);
    check(kTaskbar);
    check(kAutostart);
    y += m.S(6);
    if (HWND b = GetDlgItem(hwnd, kHotkeys))
        MoveWindow(b, x0 + labelW, y, m.S(220), m.S(26), TRUE);
    y += m.S(32);
}

LRESULT CALLBACK OptionsProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = (OptionsTabState*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_SIZE:
        if (st) Layout(hwnd, st);
        return 0;

    case WM_DRAWITEM: {
        // CreateCheck AND CreateBtn both make BS_OWNERDRAW controls, so BOTH
        // have to be painted here. Painting only the checkboxes left the five
        // buttons — Move it and the four corners — present, clickable and
        // completely invisible, which read as "snap to doesn't seem to work":
        // there was nothing on screen to aim at.
        if (!st) break;
        auto* di = (DRAWITEMSTRUCT*)lp;
        if (!di || di->CtlType != ODT_BUTTON) break;
        const ThemeState* t = st->ctx->theme ? st->ctx->theme() : nullptr;
        const ThemeColors c = t ? t->colors : ThemePalette(ThemeKind::Light);
        if ((bool)(intptr_t)GetPropW(di->hwndItem, L"IsCheckbox")) {
            DrawOwnerCheckbox(di, c.dark, c.bg, c.surface, c.border, c.text);
            return TRUE;
        }
        DrawOwnerButton(di, c.dark, c.surface, c.border, c.border, c.text);
        return TRUE;
    }

    case WM_COMMAND: {
        if (!st) break;
        const int id = LOWORD(wp), code = HIWORD(wp);
        if (code == BN_CLICKED) {
            // An owner-draw button does not toggle itself. Without this the
            // box paints, clicks, and never changes.
            HWND ck = GetDlgItem(hwnd, id);
            if (ck && (bool)(intptr_t)GetPropW(ck, L"IsCheckbox"))
                SetChecked(hwnd, id, !Checked(hwnd, id));
            if (id == kHotkeys) {
                // The window is the main window's to own -- it registers the
                // keys, and RegisterHotKey demands the thread that owns the
                // window it binds to -- so this asks rather than opens.
                if (HWND root = GetAncestor(hwnd, GA_ROOT))
                    PostMessageW(root, WM_COMMAND, MainWindow::kCmdHotkeys, 0);
                return 0;
            }
            if (id == kMoveNow) {
                // Turn the frame on as well as starting the drag: the button
                // is a shortcut for the toggle, not a second hidden mode, so
                // what it leaves behind is a state the checkbox shows.
                SetChecked(hwnd, kFrame, true);
                Save(hwnd, st);
                BeginMove(hwnd, st);
                return 0;
            }
            if (id == kSnapTL || id == kSnapTR || id == kSnapBL || id == kSnapBR) {
                const int corner = (id == kSnapTL) ? 0 : (id == kSnapTR) ? 1
                                 : (id == kSnapBL) ? 2 : 3;
                const int display = (int)SendDlgItemMessageW(hwnd, kDisplay, CB_GETCURSEL, 0, 0);
                if (HWND root = GetAncestor(hwnd, GA_ROOT))
                    PostMessageW(root, MainWindow::kSnapOverlayMsg,
                                 (WPARAM)corner, (LPARAM)(display < 0 ? 0 : display));
                return 0;
            }
            Save(hwnd, st);
            return 0;
        }
        // Every field commits as it is typed, not when it loses focus.
        //
        // On EN_KILLFOCUS alone a typed number sat there doing nothing until
        // something else was clicked -- and the nearest clickable thing is
        // the spinner beside it, so it looked as though the spinner were the
        // only control that worked: "I can type number in the step size for
        // the buttons ... but it doesn't take effect until I move the
        // spinner".
        //
        // The transient is harmless now. ReadInt clamps, the last keystroke
        // wins, and the only fields pushed BACK from config are position and
        // font size -- and those are skipped while they have focus, so a
        // part-typed value cannot be read back over the typist.
        //
        // On EN_KILLFOCUS alone, a new pattern sat in the box doing nothing
        // until something else was clicked -- and the thing most likely to be
        // clicked next is the enable checkbox, which is why it looked like
        // the overlay needed turning off and on again: "to get the battery
        // display to refresh after I changed the text, I had to toggle the
        // overlay on/off". Nothing was stuck; the text had simply never been
        // saved.
        //
        // Only this field. The numeric ones are still committed on focus
        // loss, because a part-typed number is a real value -- "1" on the way
        // to "15" would be applied and then the field reloaded from it.
        if (code == EN_CHANGE || code == EN_KILLFOCUS) {
            // Keep a spinner in step with its buddy. UDS_SETBUDDYINT writes
            // the text FROM the spinner but never reads it back, so after
            // typing 7 the first arrow click jumped to whatever the spinner
            // still thought it was -- the other half of the same complaint.
            if (code == EN_CHANGE) {
                const int spinId = (id == kOpacity)  ? kOpacitySpin
                                 : (id == kFontSize) ? kFontSpin
                                 : (id == kStep)     ? kStepSpin : 0;
                if (spinId) {
                    wchar_t buf[32] = {};
                    GetDlgItemTextW(hwnd, id, buf, 32);
                    if (buf[0])
                        if (HWND sp = GetDlgItem(hwnd, spinId))
                            SendMessageW(sp, UDM_SETPOS32, 0, (LPARAM)_wtoi(buf));
                }
            }
            Save(hwnd, st);
            return 0;
        }
        return 0;
    }

    case WM_NOTIFY: {
        // The up-down arrows: UDN_DELTAPOS fires BEFORE the buddy's text is
        // updated, so the save is posted rather than done here.
        auto* nm = (NMHDR*)lp;
        if (st && nm && nm->code == UDN_DELTAPOS)
            PostMessageW(hwnd, WM_COMMAND, MAKEWPARAM(0, EN_KILLFOCUS), 0);
        return 0;
    }

    case MainWindow::kRebuildMsg:
    case MainWindow::kRefreshMsg:
        // ONLY what the overlay changes behind the user's back, and only
        // when it is not being typed into.
        //
        // This used to reload the whole page, four times a second, from a
        // 250 ms tick. Typing "1" into Step size put a 1 in the box and the
        // next tick put "10" back before the digit could be committed:
        // "the step size field doesn't let me type 1 into the field instead
        // the default is 10". Every other field had the same fault; the step
        // box is simply the one short enough to lose the race every time.
        //
        // Position and font size are the only fields that change behind the
        // user's back: a drag moves the overlay, and dragging a corner scales
        // its text. Everything else only ever changes here.
        if (st) ReloadGeometry(hwnd, st);
        return 0;

    case WM_DESTROY:
        delete st;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        return 0;
    }

    if (st) {
        LRESULT out = 0;
        if (const ThemeState* t = st->ctx->theme ? st->ctx->theme() : nullptr) {
            if (ThemeCtlColor(msg, wp, *t, &out)) return out;
            if (msg == WM_ERASEBKGND && ThemeEraseBkgnd(hwnd, wp, *t, &out)) return out;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

HWND CreateOptionsTab(HWND parent, UiContext* ctx) {
    static bool registered = false;
    HINSTANCE inst = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    if (!registered) {
        WNDCLASSEXW wc = { sizeof wc };
        wc.lpfnWndProc = OptionsProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"mdxmixerOptionsTab";
        RegisterClassExW(&wc);
        registered = true;
    }
    HWND hwnd = CreateWindowExW(WS_EX_CONTROLPARENT, L"mdxmixerOptionsTab", L"",
                                WS_CHILD | WS_CLIPCHILDREN, 0, 0, 10, 10,
                                parent, nullptr, inst, nullptr);
    if (!hwnd) return nullptr;
    auto* st = new OptionsTabState();
    st->ctx = ctx;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)st);

    // CreateLabel takes no control id, and every label here has to be found
    // again by Layout, so they are made directly.
    auto label = [&](const wchar_t* text, int id) {
        CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
                        0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)id, inst, nullptr);
    };
    HFONT f = nullptr;
    label(L"Battery overlay", 400);
    CreateCheck(hwnd, L"Show the overlay", kEnable, 0, 0, 10, 10, f, false);
    label(L"Text", 401);
    CreateEdit(hwnd, L"", kText, 0, 0, 10, 10, f);
    label(L"$b battery   $sn short name   $n Windows name   $$ a literal $", 410);

    // `signed`: ES_NUMBER accepts DIGITS ONLY — it silently swallows the
    // minus key, so a monitor left of the primary could not be typed at all.
    // Shane's are at large negative coordinates ("typing number is better
    // than spinning to get to -2700"), so the position fields drop it and are
    // range-clamped on save instead.
    auto spin = [&](int editId, int spinId, int lo, int hi) {
        CreateEdit(hwnd, L"", editId, 0, 0, 10, 10, f, ES_NUMBER | ES_RIGHT);
        HWND s = CreateWindowExW(0, UPDOWN_CLASSW, L"",
                    WS_CHILD | WS_VISIBLE | UDS_SETBUDDYINT | UDS_ALIGNRIGHT |
                    UDS_ARROWKEYS | UDS_NOTHOUSANDS,
                    0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)spinId, inst, nullptr);
        SendMessageW(s, UDM_SETRANGE32, lo, hi);
    };
    // X and Y get NO spinner.
    //
    // Arrows that step by one are the wrong instrument for a coordinate that
    // starts near -2700 on this desk, and once the snap buttons and the
    // draggable frame exist there is nothing left for them to do: "then with
    // the snap we don't have to worry about using spinners at all". The typed
    // field stays, because typing a number was always the faster of the two
    // and is still the way to nudge by an exact amount.
    //
    // No ES_NUMBER either: it accepts digits only and silently swallows the
    // minus key, so a monitor left of the primary could not be typed at all.
    // The value is range-clamped on save instead.
    auto coord = [&](int editId) {
        CreateEdit(hwnd, L"", editId, 0, 0, 10, 10, f, ES_RIGHT);
    };
    label(L"Opacity %", 402);
    spin(kOpacity, kOpacitySpin, 10, 100);
    label(L"Font size", 403);
    spin(kFontSize, kFontSpin, 8, 72);
    label(L"Position X", 404);
    coord(kPosX);
    label(L"Position Y", 405);
    coord(kPosY);
    // Spelled out because the two halves are not obviously the same switch: a
    // click-through window cannot be dragged, so this box is the way to move
    // it with the mouse, and the X and Y above are the way to move it without.
    CreateBtn(hwnd, L"Move it — drag it into place", kMoveNow, 0, 0, 10, 10, f);
    label(L"Snap to", kSnapLabel);
    CreateCombo(hwnd, kDisplay, 0, 0, 10, 200, f);
    CreateBtn(hwnd, L"┌ Top left",     kSnapTL, 0, 0, 10, 10, f);
    CreateBtn(hwnd, L"┐ Top right",    kSnapTR, 0, 0, 10, 10, f);
    CreateBtn(hwnd, L"└ Bottom left",  kSnapBL, 0, 0, 10, 10, f);
    CreateBtn(hwnd, L"┘ Bottom right", kSnapBR, 0, 0, 10, 10, f);
    for (const DisplayInfo& d : Displays())
        SendDlgItemMessageW(hwnd, kDisplay, CB_ADDSTRING, 0, (LPARAM)d.label.c_str());
    SendDlgItemMessageW(hwnd, kDisplay, CB_SETCURSEL, 0, 0);
    CreateCheck(hwnd, L"Show frame — drag to move it, drag an edge to resize",
                kFrame, 0, 0, 10, 10, f, false);
    CreateCheck(hwnd, L"Click through (the overlay ignores the mouse)",
                kClickThru, 0, 0, 10, 10, f, true);
    CreateCheck(hwnd, L"Draw a background behind the text",
                kBackground, 0, 0, 10, 10, f, false);

    label(L"General", 406);
    CreateCheck(hwnd, L"Fader control: step buttons instead of sliders",
                kSpinBoxes, 0, 0, 10, 10, f, false);
    // Spelled out because the number governs two things that look unrelated
    // on screen, and the old label named neither: it is how far the -N / +N
    // buttons on a fader move, AND how far one press of a volume hotkey
    // moves. mdx12 has the same single number for the same two things.
    label(L"How far the \u2212N / +N fader buttons and the volume hotkeys move", 411);
    CreateCheck(hwnd, L"Show a provider’s own endpoints (Sonar’s virtual devices)",
                kVirtual, 0, 0, 10, 10, f, false);
    label(L"Volume step %", 407);
    spin(kStep, kStepSpin, 1, 50);
    CreateCheck(hwnd, L"Taskbar button as well as the tray icon",
                kTaskbar, 0, 0, 10, 10, f, true);
    CreateCheck(hwnd, L"Start with Windows", kAutostart, 0, 0, 10, 10, f, false);
    // Until now the only way in was the tray menu, which is not where anyone
    // looks for a setting.
    CreateBtn(hwnd, L"Hotkeys\u2026", kHotkeys, 0, 0, 10, 10, f);

    Load(hwnd, st);
    Layout(hwnd, st);
    return hwnd;
}

} // namespace mdxm
