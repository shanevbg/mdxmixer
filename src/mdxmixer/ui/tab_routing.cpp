// Routing tab: live sessions listview, channel assignment, and the spec's stated
// limitation as a footer fact.
#include "ui/ui_context.h"
#include "ui/main_window.h"
#include "routing/sessions.h"
#include <windows.h>
#include <commctrl.h>
#include <windowsx.h>
#include <vector>

namespace mdxm {

namespace {

constexpr wchar_t kClass[] = L"mdxmixerTabRouting";
constexpr int kList = 1400, kCombo = 1401, kAssign = 1402, kRefresh = 1403, kStatus = 1404;
// Straight to a device, and the bulk move off one.
constexpr int kDevCombo = 1405, kSendDev = 1406;
constexpr int kFromCombo = 1407, kToCombo = 1408, kMoveAll = 1409;

struct RoutingTabState {
    UiContext* ctx = nullptr;
    std::vector<SessionInfo> sessions;      // listview row order
    std::vector<std::wstring> chIds;        // combobox order; row 0 = unassigned ("-")
    // Endpoints, for sending an app straight at one rather than at a channel.
    std::vector<std::wstring> devIds;
    // The endpoints that currently have something playing on them, which is
    // what "move everything off X" needs to offer.
    std::vector<std::wstring> fromIds;
};

// Every active render endpoint, into a combo. Used three times on this tab, so
// it is one function and the three lists cannot drift apart.
void FillDeviceCombo(HWND combo, std::vector<std::wstring>& ids,
                     const std::vector<DeviceLevel>& devs) {
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    ids.clear();
    for (const auto& d : devs) {
        if (!d.isRender) continue;
        SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)d.displayName.c_str());
        ids.push_back(d.id);
    }
    if (!ids.empty()) SendMessageW(combo, CB_SETCURSEL, 0, 0);
}

std::wstring AssignedChannel(const MixerConfig& cfg, const std::wstring& exePath) {
    for (const auto& c : cfg.channels)
        for (const auto& app : c.apps)
            if (_wcsicmp(app.c_str(), exePath.c_str()) == 0)
                return c.name.empty() ? c.id : c.name;
    return L"";
}

// The Route column: assigned-channel against what Windows actually holds for
// the process (fj#1 item 4).
//
// Windows persists a per-app route ITSELF, which is what hid the missing
// reconciler for so long -- an app assigned once while it was playing keeps
// working across restarts with no help from mdxmixer, so the only symptoms
// were "that app won't take an assignment" and "that app came back on the
// wrong device". A column that says which is which turns both into something
// visible rather than inferred.
//
// The comparison is made by the controller, not here: resolving a channel to
// its routable endpoint is the reconciler's own logic and a second copy of it
// in the UI is how the two come to disagree.
std::wstring RouteCell(const AppRouteState& r) {
    if (r.intendedChannel.empty()) return L"";          // nobody assigned it; not our business
    if (!r.available) return L"unavailable";            // the API did not resolve on this build
    if (r.intendedEndpointId.empty()) return L"no cable";   // channel has nowhere to send yet
    if (r.actualEndpointId.empty()) return L"not applied";  // recorded, waiting for the app to play
    if (_wcsicmp(r.actualEndpointId.c_str(), r.intendedEndpointId.c_str()) == 0) return L"ok";
    return L"elsewhere";
}

void Reload(HWND hwnd, RoutingTabState* st) {
    HWND list = GetDlgItem(hwnd, kList);
    ListView_DeleteAllItems(list);
    st->sessions = EnumerateSessions();
    const MixerConfig& cfg = st->ctx->store->Get();
    // Every row's route in one call: see IMixerControl::GetAppRoutes, which
    // needs an endpoint enumeration and must not run one per row.
    std::vector<std::pair<std::wstring, unsigned long>> apps;
    apps.reserve(st->sessions.size());
    for (const auto& s : st->sessions) apps.push_back({ s.exePath, s.pid });
    const std::vector<AppRouteState> routes = st->ctx->ctl->GetAppRoutes(apps);
    int row = 0;
    for (const auto& s : st->sessions) {
        const wchar_t* exeName = wcsrchr(s.exePath.c_str(), L'\\');
        exeName = exeName ? exeName + 1 : s.exePath.c_str();
        LVITEMW item = {};
        item.mask = LVIF_TEXT;
        item.iItem = row;
        item.pszText = const_cast<wchar_t*>(exeName);
        ListView_InsertItem(list, &item);
        wchar_t pid[16];
        swprintf(pid, 16, L"%lu", s.pid);
        ListView_SetItemText(list, row, 1, pid);
        ListView_SetItemText(list, row, 2, const_cast<wchar_t*>(s.endpointName.c_str()));
        std::wstring ch = AssignedChannel(cfg, s.exePath);
        ListView_SetItemText(list, row, 3, const_cast<wchar_t*>(ch.c_str()));
        std::wstring route = (size_t)row < routes.size() ? RouteCell(routes[(size_t)row])
                                                         : std::wstring();
        ListView_SetItemText(list, row, 4, const_cast<wchar_t*>(route.c_str()));
        ++row;
    }
}

LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = (RoutingTabState*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    try {
        switch (msg) {
        case WM_CREATE: {
            st = new RoutingTabState;
            st->ctx = (UiContext*)((CREATESTRUCTW*)lp)->lpCreateParams;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)st);
            HINSTANCE inst = ((CREATESTRUCTW*)lp)->hInstance;
            // No WS_EX_CLIENTEDGE: its 3D edge renders as a light rectangle in
            // every dark theme. The themed control draws its own border.
            HWND list = CreateWindowExW(0, WC_LISTVIEWW, L"",
                WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                10, 10, 810, 330, hwnd, (HMENU)(INT_PTR)kList, inst, nullptr);
            ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT);
            // "Channel" is what mdxmixer was told; "Route" is what Windows is
            // actually doing about it. Both, because the two can disagree and
            // the disagreement is the thing worth seeing (fj#1).
            struct { const wchar_t* name; int width; } cols[5] = {
                { L"Application", 190 }, { L"PID", 60 }, { L"Playing on", 280 },
                { L"Channel", 120 }, { L"Route", 90 } };
            for (int i = 0; i < 5; ++i) {
                LVCOLUMNW col = {};
                col.mask = LVCF_TEXT | LVCF_WIDTH;
                col.pszText = const_cast<wchar_t*>(cols[i].name);
                col.cx = cols[i].width;
                ListView_InsertColumn(list, i, &col);
            }
            HWND combo = CreateWindowExW(0, L"COMBOBOX", L"",
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                10, 350, 220, 300, hwnd, (HMENU)(INT_PTR)kCombo, inst, nullptr);
            SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"— unassigned —");
            st->chIds.push_back(L"-");
            for (const auto& c : st->ctx->store->Get().channels) {
                SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)(c.name.empty() ? c.id : c.name).c_str());
                st->chIds.push_back(c.id);
            }
            SendMessageW(combo, CB_SETCURSEL, 0, 0);
            CreateWindowExW(0, L"BUTTON", L"Assign", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                            240, 349, 90, 26, hwnd, (HMENU)(INT_PTR)kAssign, inst, nullptr);
            CreateWindowExW(0, L"BUTTON", L"Refresh", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                            340, 349, 90, 26, hwnd, (HMENU)(INT_PTR)kRefresh, inst, nullptr);
            // ── Straight to a device ──────────────────────────────────────
            //
            // Assigning to a CHANNEL is the normal path, but it only helps
            // when the channel's cable is alive. The whole point of keeping
            // Sonar installed but not load-bearing is being able to pull an
            // app off it the moment it dies, mid-game, without first building
            // a channel for somewhere to put it.
            CreateWindowExW(0, L"STATIC", L"or to",
                            WS_CHILD | WS_VISIBLE, 440, 353, 40, 18, hwnd, nullptr, inst, nullptr);
            HWND devCombo = CreateWindowExW(0, L"COMBOBOX", L"",
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                482, 350, 258, 300, hwnd, (HMENU)(INT_PTR)kDevCombo, inst, nullptr);
            CreateWindowExW(0, L"BUTTON", L"Send", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                            748, 349, 70, 26, hwnd, (HMENU)(INT_PTR)kSendDev, inst, nullptr);

            // ── Get everything off a device ───────────────────────────────
            //
            // The action today's outage needed and nothing had: Sonar's engine
            // died holding every app, and each one had to be moved by hand
            // from a command line. One press, while a game is running.
            CreateWindowExW(0, L"STATIC", L"Move everything from",
                            WS_CHILD | WS_VISIBLE, 10, 386, 140, 18, hwnd, nullptr, inst, nullptr);
            HWND fromCombo = CreateWindowExW(0, L"COMBOBOX", L"",
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                150, 383, 250, 300, hwnd, (HMENU)(INT_PTR)kFromCombo, inst, nullptr);
            CreateWindowExW(0, L"STATIC", L"to", WS_CHILD | WS_VISIBLE,
                            408, 386, 20, 18, hwnd, nullptr, inst, nullptr);
            HWND toCombo = CreateWindowExW(0, L"COMBOBOX", L"",
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                430, 383, 250, 300, hwnd, (HMENU)(INT_PTR)kToCombo, inst, nullptr);
            CreateWindowExW(0, L"BUTTON", L"Move all", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                            690, 382, 90, 26, hwnd, (HMENU)(INT_PTR)kMoveAll, inst, nullptr);
            {
                auto devs = st->ctx->ctl->GetDeviceLevels();
                FillDeviceCombo(devCombo, st->devIds, devs);
                FillDeviceCombo(toCombo, st->fromIds, devs);   // same list, own ids
                st->fromIds.clear();
                FillDeviceCombo(fromCombo, st->fromIds, devs);
            }

            CreateWindowExW(0, L"STATIC",
                L"Apps that select a specific output device themselves ignore per-app routing.",
                WS_CHILD | WS_VISIBLE, 10, 418, 700, 18, hwnd, nullptr, inst, nullptr);
            CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                            10, 440, 940, 36, hwnd, (HMENU)(INT_PTR)kStatus, inst, nullptr);
            // Say it up front rather than letting Assign look like it worked.
            if (st->ctx->routingAvailable && !st->ctx->routingAvailable()) {
                SetWindowTextW(GetDlgItem(hwnd, kStatus),
                    L"Per-app routing is unavailable on this Windows build — assignment is disabled.\n"
                    L"Apps can still be pointed at a channel cable in Windows Sound settings.");
                EnableWindow(GetDlgItem(hwnd, kAssign), FALSE);
            }
            Reload(hwnd, st);
            return 0;
        }
        case WM_CONTEXTMENU: {
            // Right-click a row and pick a destination: two gestures, which is
            // what Sonar's list cost and what select-combo-button did not.
            if (!st) break;
            HWND list = GetDlgItem(hwnd, kList);
            if ((HWND)wp != list) break;
            int sel = ListView_GetNextItem(list, -1, LVNI_SELECTED);
            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            if (pt.x == -1 && pt.y == -1) {
                RECT rc; GetWindowRect(list, &rc);
                pt = { rc.left + 40, rc.top + 40 };
            } else {
                // Select whatever was actually clicked, so the menu acts on
                // the row under the cursor rather than the last one touched.
                POINT client = pt;
                ScreenToClient(list, &client);
                LVHITTESTINFO ht = {};
                ht.pt = client;
                int hit = ListView_HitTest(list, &ht);
                if (hit >= 0) {
                    ListView_SetItemState(list, hit, LVIS_SELECTED | LVIS_FOCUSED,
                                          LVIS_SELECTED | LVIS_FOCUSED);
                    sel = hit;
                }
            }
            if (sel < 0 || sel >= (int)st->sessions.size()) return 0;

            HMENU menu = CreatePopupMenu();
            if (!menu) return 0;
            const std::wstring& exe = st->sessions[(size_t)sel].exePath;
            const std::wstring& onNow = st->sessions[(size_t)sel].endpointId;
            auto devs = st->ctx->ctl->GetDeviceLevels();
            std::vector<std::wstring> ids;
            int id = 1;
            for (const auto& d : devs) {
                if (!d.isRender) continue;
                // The device it is already on is shown ticked rather than
                // hidden: the menu doubles as the answer to "where is this?".
                bool here = _wcsicmp(d.id.c_str(), onNow.c_str()) == 0;
                AppendMenuW(menu, MF_STRING | (here ? MF_CHECKED : 0), id++,
                            d.displayName.c_str());
                ids.push_back(d.id);
            }
            int cmd = (int)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                          pt.x, pt.y, 0, hwnd, nullptr);
            DestroyMenu(menu);
            if (cmd >= 1 && cmd <= (int)ids.size()) {
                std::wstring err;
                bool ok = st->ctx->ctl->RouteAppToEndpoint(exe, ids[(size_t)cmd - 1], &err);
                SetWindowTextW(GetDlgItem(hwnd, kStatus),
                               ok ? L"Moved." : (err.empty() ? L"Could not move it." : err.c_str()));
                Reload(hwnd, st);
            }
            return 0;
        }

        case WM_COMMAND: {
            if (!st || HIWORD(wp) != BN_CLICKED) break;
            if (LOWORD(wp) == kRefresh) { Reload(hwnd, st); return 0; }

            if (LOWORD(wp) == kSendDev) {
                HWND list = GetDlgItem(hwnd, kList);
                int sel = ListView_GetNextItem(list, -1, LVNI_SELECTED);
                int dev = (int)SendMessageW(GetDlgItem(hwnd, kDevCombo), CB_GETCURSEL, 0, 0);
                if (sel < 0 || sel >= (int)st->sessions.size()) {
                    SetWindowTextW(GetDlgItem(hwnd, kStatus), L"Select an application first.");
                    return 0;
                }
                if (dev < 0 || dev >= (int)st->devIds.size()) return 0;
                std::wstring err;
                bool ok = st->ctx->ctl->RouteAppToEndpoint(st->sessions[(size_t)sel].exePath,
                                                           st->devIds[(size_t)dev], &err);
                SetWindowTextW(GetDlgItem(hwnd, kStatus),
                               ok ? L"Sent." : (err.empty() ? L"Could not send it." : err.c_str()));
                Reload(hwnd, st);
                return 0;
            }

            if (LOWORD(wp) == kMoveAll) {
                int from = (int)SendMessageW(GetDlgItem(hwnd, kFromCombo), CB_GETCURSEL, 0, 0);
                int to   = (int)SendMessageW(GetDlgItem(hwnd, kToCombo), CB_GETCURSEL, 0, 0);
                if (from < 0 || from >= (int)st->fromIds.size() ||
                    to < 0 || to >= (int)st->devIds.size()) return 0;
                const std::wstring fromId = st->fromIds[(size_t)from];
                const std::wstring toId = st->devIds[(size_t)to];
                if (_wcsicmp(fromId.c_str(), toId.c_str()) == 0) {
                    SetWindowTextW(GetDlgItem(hwnd, kStatus),
                                   L"That is the same device on both sides.");
                    return 0;
                }
                // Snapshot first: routing an app changes the session list, and
                // iterating it while it moves underneath would skip apps --
                // which in the case this exists for means leaving some of them
                // silent with no sign of why.
                std::vector<std::wstring> exes;
                for (const auto& sess : st->sessions) {
                    if (sess.exePath.empty()) continue;
                    if (_wcsicmp(sess.endpointId.c_str(), fromId.c_str()) != 0) continue;
                    bool already = false;
                    for (const auto& e : exes)
                        if (_wcsicmp(e.c_str(), sess.exePath.c_str()) == 0) already = true;
                    if (!already) exes.push_back(sess.exePath);
                }
                int moved = 0;
                std::wstring firstErr;
                for (const auto& exe : exes) {
                    std::wstring err;
                    if (st->ctx->ctl->RouteAppToEndpoint(exe, toId, &err)) ++moved;
                    else if (firstErr.empty()) firstErr = err;
                }
                wchar_t msg[256];
                if (exes.empty())
                    swprintf(msg, 256, L"Nothing is playing on that device.");
                else
                    swprintf(msg, 256, L"Moved %d of %d app(s).%s%s", moved, (int)exes.size(),
                             firstErr.empty() ? L"" : L" ", firstErr.c_str());
                SetWindowTextW(GetDlgItem(hwnd, kStatus), msg);
                Reload(hwnd, st);
                return 0;
            }
            if (LOWORD(wp) == kAssign) {
                HWND list = GetDlgItem(hwnd, kList);
                int sel = ListView_GetNextItem(list, -1, LVNI_SELECTED);
                int chSel = (int)SendMessageW(GetDlgItem(hwnd, kCombo), CB_GETCURSEL, 0, 0);
                if (sel < 0) {
                    SetWindowTextW(GetDlgItem(hwnd, kStatus), L"Select an application first.");
                    return 0;
                }
                if (sel < (int)st->sessions.size() && chSel >= 0 && chSel < (int)st->chIds.size()) {
                    const std::wstring& exe = st->sessions[(size_t)sel].exePath;
                    bool ok = st->ctx->ctl->AssignApp(exe, st->chIds[(size_t)chSel]);
                    // A failed assignment used to look identical to a successful
                    // one: the row simply did not change. Spec: degrade and say so.
                    SetWindowTextW(GetDlgItem(hwnd, kStatus),
                        ok ? L"Assigned."
                           : L"Assignment failed — the channel has no routable cable endpoint, "
                             L"or the app has no live audio session to key the route from.");
                    Reload(hwnd, st);
                }
                return 0;
            }
            break;
        }
        case MainWindow::kRefreshMsg:
            return 0;   // the Refresh button re-enumerates; no 250 ms polling of COM
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX: {
            if (st && st->ctx->theme) {
                LRESULT r;
                if (const ThemeState* t = st->ctx->theme(); t && ThemeCtlColor(msg, wp, *t, &r)) return r;
            }
            break;
        }
        case WM_ERASEBKGND: {
            if (st && st->ctx->theme) {
                LRESULT r;
                if (const ThemeState* t = st->ctx->theme(); t && ThemeEraseBkgnd(hwnd, wp, *t, &r)) return r;
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

HWND CreateRoutingTab(HWND parent, UiContext* ctx) {
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
