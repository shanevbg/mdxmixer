#include "ui/wnd_hotkeys.h"
#include "ui/controls.h"
#include <commctrl.h>
#include <algorithm>

namespace mdxm {

namespace {
constexpr int kList = 9110, kAdd = 9111, kEdit = 9112, kRemove = 9113;
constexpr int kStepLabel = 9114, kStep = 9115, kStepSpin = 9116, kHint = 9117;

const wchar_t* ActionName(HotkeyAction a) {
    switch (a) {
    case HotkeyAction::VolumeUp:   return L"Volume up";
    case HotkeyAction::VolumeDown: return L"Volume down";
    case HotkeyAction::MuteToggle: return L"Mute / unmute";
    default:                       return L"Show the mixer";
    }
}
} // namespace

HotkeysWindow::HotkeysWindow(ToolHost* host, IMixerControl* ctl)
    : ToolWindow(host, 820, 420), m_ctl(ctl) {}

void HotkeysWindow::DoBuildControls() {
    auto L = BuildBaseControls();     // pin + font buttons, and the grid
    HFONT hFont = m_hFont;
    RECT rcClient;
    GetClientRect(m_hWnd, &rcClient);
    const int clientH = rcClient.bottom;
    const int margin = 10;
    int y = L.y;
    const int cw = L.clientW - 2 * margin;

    TrackAnchored(CreateLabel(m_hWnd,
        L"Every key here is global and ships unbound. A key may move any "
        L"number of faders — one for a Sonar-style per-channel key, "
        L"several for a group.",
        margin, y, cw, L.lineH * 2, hFont), kAnchorStretchWide);
    y += L.lineH * 2 + L.gap;

    // Four columns: the name, what it does, the key, and whether Windows gave
    // it to us. The last is not decoration — a combination another application
    // owns registers as nothing at all, and mdx12 shipped a window that showed
    // the request and called it the answer (fj#72).
    const int listH = std::max(L.lineH * 4, clientH - y - L.lineH * 3 - margin * 3);
    m_list = CreateThemedListView(kList, margin, y, cw, listH);
    TrackAnchored(m_list, kAnchorFill);
    ListView_SetExtendedListViewStyle(m_list, LVS_EX_FULLROWSELECT);
    struct { const wchar_t* text; int pct; } cols[] = {
        { L"Name", 20 }, { L"Does", 16 }, { L"Key", 22 },
        { L"Moves", 14 }, { L"Step", 9 }, { L"Windows says", 19 },
    };
    const int avail = cw - GetSystemMetrics(SM_CXVSCROLL) - 4;
    for (int i = 0; i < 6; ++i) {
        LVCOLUMNW c = {};
        c.mask = LVCF_TEXT | LVCF_WIDTH;
        c.pszText = const_cast<wchar_t*>(cols[i].text);
        c.cx = avail * cols[i].pct / 100;
        ListView_InsertColumn(m_list, i, &c);
    }
    y += listH + L.gap;

    TrackAnchored(CreateBtn(m_hWnd, L"Add...", kAdd, margin, y, 80, L.lineH + 4, hFont),
                  kAnchorBottomLeft);
    TrackAnchored(CreateBtn(m_hWnd, L"Edit...", kEdit, margin + 86, y, 80, L.lineH + 4, hFont),
                  kAnchorBottomLeft);
    TrackAnchored(CreateBtn(m_hWnd, L"Remove", kRemove, margin + 172, y, 80, L.lineH + 4, hFont),
                  kAnchorBottomLeft);

    TrackAnchored(CreateLabel(m_hWnd, L"Step %", margin + 280, y + 4, 50, L.lineH, hFont),
                  kAnchorBottomLeft);
    HWND hStep = CreateEdit(m_hWnd, L"", kStep, margin + 336, y, 50, L.lineH + 2,
                            hFont, ES_NUMBER | ES_RIGHT);
    TrackAnchored(hStep, kAnchorBottomLeft);
    HWND hSpin = CreateWindowExW(0, UPDOWN_CLASSW, L"",
                                 WS_CHILD | WS_VISIBLE | UDS_SETBUDDYINT |
                                 UDS_ALIGNRIGHT | UDS_ARROWKEYS | UDS_NOTHOUSANDS,
                                 0, 0, 0, 0, m_hWnd, (HMENU)(INT_PTR)kStepSpin,
                                 (HINSTANCE)GetWindowLongPtrW(m_hWnd, GWLP_HINSTANCE), NULL);
    SendMessageW(hSpin, UDM_SETBUDDY, (WPARAM)hStep, 0);
    SendMessageW(hSpin, UDM_SETRANGE32, 1, 50);
    SendMessageW(hSpin, UDM_SETPOS32, 0, m_ctl ? m_ctl->GetVolumeStep() : 5);
    TrackAnchored(hSpin, kAnchorBottomLeft);
    // What one press of a volume key moves. mdx12 defaults to 5, which is the
    // number that makes a key feel like a volume control rather than a nudge.
    AttachTip(hStep, L"How far one press of a volume key moves a fader.");

    RefreshList();
}

void HotkeysWindow::RefreshList() {
    if (!m_list || !m_ctl) return;
    m_bindings = m_ctl->GetHotkeys();
    ListView_DeleteAllItems(m_list);
    for (size_t i = 0; i < m_bindings.size(); ++i) {
        const HotkeyBinding& b = m_bindings[i];
        LVITEMW it = {};
        it.mask = LVIF_TEXT;
        it.iItem = (int)i;
        std::wstring name = b.label.empty() ? L"(unnamed)" : b.label;
        it.pszText = const_cast<wchar_t*>(name.c_str());
        ListView_InsertItem(m_list, &it);
        ListView_SetItemText(m_list, (int)i, 1, const_cast<wchar_t*>(ActionName(b.action)));
        std::wstring combo = FormatCombo(b.mod, b.vk);
        ListView_SetItemText(m_list, (int)i, 2, const_cast<wchar_t*>(combo.c_str()));

        // What the key moves, and then what Windows actually answered. The
        // second is asked for rather than guessed: a combination another
        // application owns registers as nothing at all.
        std::wstring moves;
        if (b.action == HotkeyAction::ShowWindow) moves = L"the window";
        else if (b.targets.empty()) moves = L"nothing chosen";
        else moves = std::to_wstring(b.targets.size()) +
                     (b.targets.size() == 1 ? L" fader" : L" faders");
        ListView_SetItemText(m_list, (int)i, 3, const_cast<wchar_t*>(moves.c_str()));
        // Its own step, or nothing at all when it follows the default --
        // an empty cell reads as "same as everything else", which a repeated
        // copy of the default number would not.
        std::wstring step;
        if (b.stepPercent > 0 && b.action != HotkeyAction::MuteToggle &&
            b.action != HotkeyAction::ShowWindow)
            step = std::to_wstring(b.stepPercent) + L"%";
        ListView_SetItemText(m_list, (int)i, 4, const_cast<wchar_t*>(step.c_str()));
        std::wstring status = m_ctl->GetHotkeyStatus(b.id);
        ListView_SetItemText(m_list, (int)i, 5, const_cast<wchar_t*>(status.c_str()));
    }
}

int HotkeysWindow::SelectedRow() const {
    return m_list ? ListView_GetNextItem(m_list, -1, LVNI_SELECTED) : -1;
}

void HotkeysWindow::EditBinding(int index) {
    if (!m_ctl) return;
    ActionEditData data;
    data.pHost = m_pHost;
    data.allTargets = m_ctl->GetHotkeyTargets();
    data.defaultStepPercent = m_ctl->GetVolumeStep();
    data.others = m_bindings;
    if (index >= 0 && index < (int)m_bindings.size()) {
        const HotkeyBinding& b = m_bindings[(size_t)index];
        data.label = b.label;
        data.action = b.action;
        data.mod = b.mod;
        data.vk = b.vk;
        data.targets = b.targets;
        data.stepPercent = b.stepPercent;
        data.selfId = b.id;
    } else {
        data.label = L"New hotkey";
    }
    if (!ShowActionEditDialog(m_hWnd, data)) return;

    HotkeyBinding b;
    b.id = data.selfId;
    if (b.id.empty()) {
        // An id nothing else holds, and stable once given: it is what the
        // config file and the registration table agree on.
        int n = 1;
        auto taken = [&](const std::wstring& id) {
            for (const auto& x : m_bindings) if (x.id == id) return true;
            return false;
        };
        while (taken(L"hk" + std::to_wstring(n))) ++n;
        b.id = L"hk" + std::to_wstring(n);
    }
    b.label = data.label;
    b.action = data.action;
    b.mod = data.mod;
    b.vk = data.vk;
    b.targets = data.targets;
    b.stepPercent = data.stepPercent;

    if (index >= 0 && index < (int)m_bindings.size()) m_bindings[(size_t)index] = b;
    else m_bindings.push_back(b);
    m_ctl->SetHotkeys(m_bindings);
    RefreshList();
}

LRESULT HotkeysWindow::DoCommand(HWND hWnd, int id, int code, LPARAM lParam) {
    (void)hWnd; (void)lParam;
    if (id == kAdd && code == BN_CLICKED) { EditBinding(-1); return 0; }
    if (id == kEdit && code == BN_CLICKED) { EditBinding(SelectedRow()); return 0; }
    if (id == kRemove && code == BN_CLICKED) {
        int row = SelectedRow();
        if (row >= 0 && row < (int)m_bindings.size() && m_ctl) {
            m_bindings.erase(m_bindings.begin() + row);
            m_ctl->SetHotkeys(m_bindings);
            RefreshList();
        }
        return 0;
    }
    if (id == kStep && code == EN_KILLFOCUS && m_ctl) {
        wchar_t buf[16] = {};
        GetWindowTextW(GetDlgItem(m_hWnd, kStep), buf, 16);
        m_ctl->SetVolumeStep(_wtoi(buf));
        return 0;
    }
    return -1;
}

LRESULT HotkeysWindow::DoNotify(HWND hWnd, NMHDR* pnm) {
    (void)hWnd;
    // The dark ListView header is painted by the base class for every tool
    // window, so there is deliberately nothing about it here.
    // Double-click a row to edit it: the list is the table, so the row is the
    // thing to act on.
    if (pnm && pnm->idFrom == kList && pnm->code == NM_DBLCLK) {
        EditBinding(SelectedRow());
        return 0;
    }
    return -1;
}

} // namespace mdxm
