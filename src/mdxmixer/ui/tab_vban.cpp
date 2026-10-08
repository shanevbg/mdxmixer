// VBAN tab: the network stream's settings, its live state, and the devices that
// have been let in (spec §6.2).
//
// Every control writes through IMixerControl::SetVbanOption -- one keyed field at
// a time, validated and clamped by the callee -- rather than editing config here,
// so the tab, the pipe and the phone all take the same path and cannot disagree
// about what a value means.
//
// The status strip refreshes on the 250 ms tick. It carries the three latency
// numbers deliberately, because they answer three different questions and the one
// everybody reaches for first (depth) is the one that usually means nothing is
// wrong: see the status line's own comment.
#include "ui/ui_context.h"
#include "ui/controls.h"
#include "ui/main_window.h"
#include "ui/owner_draw.h"
#include "ui/theme.h"
#include <windows.h>
#include <commctrl.h>
#include <string>
#include <vector>

namespace mdxm {

namespace {

constexpr wchar_t kClass[] = L"mdxmixerTabVban";

constexpr int kEnable = 1400, kPort = 1401, kName = 1402, kPin = 1403,
              kSource = 1404, kFormat = 1405, kGain = 1406, kFps = 1407,
              kOpen = 1408, kAlways = 1409, kAlwaysFrames = 1410, kTarget = 1411,
              kDevices = 1412, kRevoke = 1413,
              kGainLabel = 1414, kStatus = 1415, kBadge = 1416;

struct VbanTabState {
    UiContext* ctx = nullptr;
    HFONT font = nullptr;
    // Set while the controls are being filled from state, so the EN_CHANGE and
    // BN_CLICKED those writes raise are not read back as the user typing.
    bool loading = false;
    // The device ids behind the list rows, in the order they are shown: the list
    // displays names, and Revoke needs the id.
    std::vector<std::wstring> deviceIds;
    // Why the last write was refused, shown on the status line. A value rejected
    // silently is the worst outcome here: the box keeps the typed text, the
    // setting does not change, and nothing says which of the two is true.
    std::wstring lastRefusal;
};

// A static with an id, which CreateLabel cannot make -- it takes no id, because
// every label in the ported pages is decoration. These three are read and written.
HWND CreateIdLabel(HWND parent, int id, int x, int y, int w, int h, HFONT font) {
    HWND label = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, x, y, w, h,
                                 parent, (HMENU)(INT_PTR)id,
                                 (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE),
                                 nullptr);
    if (label && font) SendMessageW(label, WM_SETFONT, (WPARAM)font, TRUE);
    return label;
}

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

std::wstring TextOf(HWND hwnd, int id) {
    wchar_t buf[256] = {};
    GetDlgItemTextW(hwnd, id, buf, 256);
    return buf;
}

void Set(VbanTabState* st, const wchar_t* key, const std::wstring& value) {
    st->lastRefusal.clear();
    if (!st->ctx->ctl) return;
    std::wstring err;
    if (!st->ctx->ctl->SetVbanOption(key, value, &err)) {
        // Kept for the status line. A refusal that only reached the log would
        // leave the typed text sitting in the box looking applied.
        st->lastRefusal = err.empty() ? std::wstring(key) + L" was refused" : err;
    }
}

// Fill the controls from the server's own state, which is the single source of
// truth -- including when it differs from what was typed, because the callee
// clamps.
void Load(HWND hwnd, VbanTabState* st) {
    if (!st->ctx->ctl) return;
    const VbanStatus s = st->ctx->ctl->GetVbanStatus();
    st->loading = true;
    SetChecked(hwnd, kEnable, s.on);
    SetDlgItemTextW(hwnd, kPort, std::to_wstring(s.port).c_str());
    SetDlgItemTextW(hwnd, kName, s.name.c_str());
    SendMessageW(GetDlgItem(hwnd, kSource), CB_SETCURSEL, s.sourceStreaming ? 1 : 0, 0);
    SendMessageW(GetDlgItem(hwnd, kFormat), CB_SETCURSEL, s.formatFloat32 ? 1 : 0, 0);
    SendMessageW(GetDlgItem(hwnd, kGain), TBM_SETPOS, TRUE, s.gainPercent);
    SetDlgItemTextW(hwnd, kGainLabel, (std::to_wstring(s.gainPercent) + L" %").c_str());
    wchar_t fps[32];
    swprintf(fps, 32, L"%g", s.fps);
    SetDlgItemTextW(hwnd, kFps, fps);
    SetChecked(hwnd, kOpen, s.open);
    SetChecked(hwnd, kAlways, s.always);
    SetChecked(hwnd, kAlwaysFrames, s.alwaysFrames);
    SetDlgItemTextW(hwnd, kTarget, s.target.c_str());

    // The PIN is never read back from anywhere: it is write-only on this page, so
    // the box stays empty and typing into it sets a new one. Reading it back into
    // a password field would show its LENGTH to anyone glancing at the screen and
    // would make "leave it alone" indistinguishable from "clear it".
    const std::vector<VbanAuthorizedDevice>& known =
        st->ctx->store->Get().vban.authorizedDevices;
    // The one state worth saying out loud: devices have been remembered, and with
    // no PIN they are all inert (spec §4). Without this the page looks configured
    // and nothing works.
    const bool pinUnset = st->ctx->store->Get().vban.pin.empty();
    SetDlgItemTextW(hwnd, kBadge,
                    (pinUnset && !known.empty())
                        ? L"PIN unset \x2014 remembered devices are inert and control is off"
                        : (pinUnset ? L"PIN unset \x2014 remote control is off" : L""));

    HWND list = GetDlgItem(hwnd, kDevices);
    const int wasSelected = (int)SendMessageW(list, LB_GETCURSEL, 0, 0);
    SendMessageW(list, LB_RESETCONTENT, 0, 0);
    st->deviceIds.clear();
    for (const auto& d : known) {
        std::wstring row = d.name.empty() ? d.id : d.name + L"  (" + d.id + L")";
        if (!d.lastSeen.empty()) row += L"  \x2014 " + d.lastSeen;   // em dash + when
        SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)row.c_str());
        st->deviceIds.push_back(d.id);
    }
    if (wasSelected >= 0 && wasSelected < (int)st->deviceIds.size())
        SendMessageW(list, LB_SETCURSEL, wasSelected, 0);
    EnableWindow(GetDlgItem(hwnd, kRevoke), !st->deviceIds.empty());
    st->loading = false;
}

// The live numbers. Separate from Load because this runs four times a second and
// must not fight the user's cursor in an edit box.
void Refresh(HWND hwnd, VbanTabState* st) {
    if (!st->ctx->ctl) return;
    const VbanStatus s = st->ctx->ctl->GetVbanStatus();
    std::wstring line;
    if (!s.on) {
        line = L"off";
    } else {
        line = s.emitting ? L"sending to " + std::to_wstring(s.peers) +
                                (s.peers == 1 ? L" listener" : L" listeners")
                          : L"listening, nobody subscribed";
        wchar_t buf[320];
        // depth, behind and srclatency are three different questions. A big depth
        // is normal -- it is how far the PRODUCER is ahead, and a Bluetooth render
        // runs seconds ahead of its own radio -- whereas `behind` is this machine
        // or the network failing to keep up, and `latency` is what the listener
        // actually hears late from this end.
        swprintf(buf, 320,
                 L"   |   sent %llu   starved %llu   dropped %llu"
                 L"   |   producer lead %d ms   behind %d ms   this end adds %d ms",
                 (unsigned long long)s.sent, (unsigned long long)s.starved,
                 (unsigned long long)s.dropped, s.depthMs, s.behindMs, s.srcLatencyMs);
        line += buf;
        if (s.framesSent) line += L"   |   " + std::to_wstring(s.framesSent) + L" frames";
    }
    if (!s.lastError.empty()) line += L"   |   " + s.lastError;
    if (!st->lastRefusal.empty()) line += L"   |   " + st->lastRefusal;
    // Only written when it changed: a static rewritten four times a second
    // flickers, and it is the one control on this page nobody is interacting with.
    if (TextOf(hwnd, kStatus) != line) SetDlgItemTextW(hwnd, kStatus, line.c_str());
}

void Layout(HWND hwnd, VbanTabState* st) {
    (void)st;
    RECT rc;
    GetClientRect(hwnd, &rc);
    const int w = rc.right - rc.left;
    // The two full-width rows: the status strip at the bottom, the device list
    // above it. Everything else is fixed.
    if (HWND h = GetDlgItem(hwnd, kStatus))
        MoveWindow(h, 12, rc.bottom - 28, w - 24, 20, TRUE);
    if (HWND h = GetDlgItem(hwnd, kDevices))
        MoveWindow(h, 12, 236, w - 110, rc.bottom - 236 - 36, TRUE);
    if (HWND h = GetDlgItem(hwnd, kRevoke))
        MoveWindow(h, w - 92, 236, 80, 26, TRUE);
    if (HWND h = GetDlgItem(hwnd, kBadge))
        MoveWindow(h, 12, 210, w - 24, 20, TRUE);
}

LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = (VbanTabState*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    try {
        switch (msg) {
        case WM_CREATE: {
            st = new VbanTabState;
            st->ctx = (UiContext*)((CREATESTRUCTW*)lp)->lpCreateParams;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)st);
            st->font = (HFONT)SendMessageW(GetParent(hwnd), WM_GETFONT, 0, 0);
            HFONT f = st->font;

            int y = 12;
            CreateCheck(hwnd, L"Stream on the network (VBAN)", kEnable, 12, y, 260, 22, f, false);
            CreateLabel(hwnd, L"UDP port", 290, y + 2, 60, 18, f);
            CreateEdit(hwnd, L"", kPort, 352, y, 60, 22, f, ES_NUMBER);
            CreateLabel(hwnd, L"Stream name", 428, y + 2, 80, 18, f);
            CreateEdit(hwnd, L"", kName, 512, y, 120, 22, f);

            y += 32;
            CreateLabel(hwnd, L"PIN (write-only)", 12, y + 2, 100, 18, f);
            CreateEdit(hwnd, L"", kPin, 116, y, 100, 22, f, ES_PASSWORD);
            CreateLabel(hwnd, L"Mix", 240, y + 2, 30, 18, f);
            {
                HWND c = CreateCombo(hwnd, kSource, 274, y, 110, 120, f);
                SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)L"personal");
                SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)L"streaming");
            }
            CreateLabel(hwnd, L"Format", 396, y + 2, 50, 18, f);
            {
                HWND c = CreateCombo(hwnd, kFormat, 450, y, 80, 120, f);
                SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)L"i16");
                SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)L"f32");
            }

            y += 32;
            // 0..6400 because the personal mix runs at a few percent of full
            // scale -- the faders are the listening level -- so this is makeup
            // gain, not a trim.
            CreateLabel(hwnd, L"Stream gain", 12, y + 2, 80, 18, f);
            CreateSlider(hwnd, kGain, 96, y, 300, 24, 0, 6400, 100);
            CreateIdLabel(hwnd, kGainLabel, 404, y + 2, 60, 18, f);

            y += 32;
            CreateLabel(hwnd, L"Screens per second", 12, y + 2, 120, 18, f);
            CreateEdit(hwnd, L"", kFps, 136, y, 50, 22, f);

            y += 34;
            CreateLabel(hwnd, L"For VBAN tools that cannot authenticate:", 12, y, 320, 18, f);
            y += 24;
            CreateCheck(hwnd, L"Let any pinger hear the audio", kOpen, 12, y, 230, 22, f, false);
            CreateCheck(hwnd, L"Send with no subscriber", kAlways, 250, y, 180, 22, f, false);
            CreateCheck(hwnd, L"\x2026 and screens", kAlwaysFrames, 436, y, 120, 22, f, false);
            y += 28;
            CreateLabel(hwnd, L"Send to", 12, y + 2, 50, 18, f);
            CreateEdit(hwnd, L"", kTarget, 66, y, 160, 22, f);
            CreateLabel(hwnd, L"(ip:port)", 232, y + 2, 60, 18, f);

            CreateIdLabel(hwnd, kBadge, 12, 210, 500, 20, f);
            CreateListBox(hwnd, kDevices, 12, 236, 400, 100, f);
            CreateBtn(hwnd, L"Revoke", kRevoke, 420, 236, 80, 26, f);
            CreateIdLabel(hwnd, kStatus, 12, 350, 500, 20, f);

            Load(hwnd, st);
            Layout(hwnd, st);
            return 0;
        }

        case WM_SIZE:
            if (st) Layout(hwnd, st);
            return 0;

        case WM_DRAWITEM: {
            // CreateCheck and CreateBtn both make BS_OWNERDRAW controls, so both
            // are painted here. Painting only one leaves the other present,
            // clickable and invisible -- the trap the Options page fell into.
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

        case WM_HSCROLL: {
            // The gain slider. Applied as it moves, because the point of a gain
            // is to hear the change while dragging.
            if (!st || st->loading) return 0;
            if ((HWND)lp != GetDlgItem(hwnd, kGain)) break;
            const int pos = (int)SendMessageW((HWND)lp, TBM_GETPOS, 0, 0);
            SetDlgItemTextW(hwnd, kGainLabel, (std::to_wstring(pos) + L" %").c_str());
            Set(st, L"gain", std::to_wstring(pos));
            return 0;
        }

        case WM_COMMAND: {
            if (!st || st->loading) return 0;
            const int id = LOWORD(wp), code = HIWORD(wp);
            if (code == BN_CLICKED) {
                // An owner-draw button does not toggle itself: without this the
                // box paints, clicks, and never changes.
                HWND ck = GetDlgItem(hwnd, id);
                if (ck && (bool)(intptr_t)GetPropW(ck, L"IsCheckbox"))
                    SetChecked(hwnd, id, !Checked(hwnd, id));
                switch (id) {
                case kEnable: Set(st, L"on", Checked(hwnd, kEnable) ? L"1" : L"0"); break;
                case kOpen:   Set(st, L"open", Checked(hwnd, kOpen) ? L"1" : L"0"); break;
                case kAlways: Set(st, L"always", Checked(hwnd, kAlways) ? L"1" : L"0"); break;
                case kAlwaysFrames:
                    Set(st, L"alwaysframes", Checked(hwnd, kAlwaysFrames) ? L"1" : L"0");
                    break;
                case kRevoke: {
                    HWND list = GetDlgItem(hwnd, kDevices);
                    const int sel = (int)SendMessageW(list, LB_GETCURSEL, 0, 0);
                    if (sel >= 0 && sel < (int)st->deviceIds.size() && st->ctx->ctl)
                        st->ctx->ctl->RevokeVbanDevice(st->deviceIds[(size_t)sel]);
                    Load(hwnd, st);
                    break;
                }
                default: break;
                }
                if (id == kEnable) Load(hwnd, st);   // the port may have rebound
                return 0;
            }
            if (code == CBN_SELCHANGE) {
                const int sel = (int)SendMessageW((HWND)lp, CB_GETCURSEL, 0, 0);
                if (id == kSource) Set(st, L"source", sel == 1 ? L"streaming" : L"personal");
                else if (id == kFormat) Set(st, L"format", sel == 1 ? L"f32" : L"i16");
                return 0;
            }
            // Edits apply on kill-focus, like the EQ grid: applying per keystroke
            // would rebind the socket on the way from "6" to "6980".
            if (code == EN_KILLFOCUS) {
                switch (id) {
                case kPort: Set(st, L"port", TextOf(hwnd, kPort)); Load(hwnd, st); break;
                case kName: Set(st, L"name", TextOf(hwnd, kName)); break;
                case kFps:  Set(st, L"fps", TextOf(hwnd, kFps)); break;
                case kTarget: Set(st, L"target", TextOf(hwnd, kTarget)); break;
                case kPin: {
                    // Empty means "leave it alone", not "clear it": the box is
                    // always empty when the page loads, so treating empty as a
                    // write would wipe the PIN every time the page lost focus.
                    const std::wstring typed = TextOf(hwnd, kPin);
                    if (!typed.empty()) {
                        Set(st, L"pin", typed);
                        SetDlgItemTextW(hwnd, kPin, L"");
                        Load(hwnd, st);
                    }
                    break;
                }
                default: break;
                }
                return 0;
            }
            return 0;
        }

        case MainWindow::kRefreshMsg:
            if (st) Refresh(hwnd, st);
            return 0;

        case MainWindow::kRebuildMsg:
            if (st) Load(hwnd, st);
            return 0;

        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX: {
            if (st && st->ctx->theme) {
                LRESULT r;
                if (const ThemeState* t = st->ctx->theme(); t && ThemeCtlColor(msg, wp, *t, &r))
                    return r;
            }
            break;
        }

        case WM_ERASEBKGND: {
            if (st && st->ctx->theme) {
                LRESULT r;
                if (const ThemeState* t = st->ctx->theme(); t && ThemeEraseBkgnd(hwnd, wp, *t, &r))
                    return r;
            }
            break;
        }

        case WM_DESTROY:
            delete st;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return 0;
        }
    } catch (...) {}
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

HWND CreateVbanTab(HWND parent, UiContext* ctx) {
    HINSTANCE inst = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    WNDCLASSW wc = {};
    wc.lpfnWndProc = Proc;
    wc.hInstance = inst;
    wc.lpszClassName = kClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassW(&wc);
    return CreateWindowExW(0, kClass, L"", WS_CHILD, 0, 0, 100, 100, parent, nullptr, inst, ctx);
}

} // namespace mdxm
