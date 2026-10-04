// EQ tab: channel picker (channels + Mic), Enable checkbox, 10 rows of
// freq/gain/Q edits. Edits apply on kill-focus; invalid text reverts.
#include "ui/ui_context.h"
#include "ui/main_window.h"
#include <windows.h>
#include <cstdlib>
#include <cwchar>
#include <vector>

namespace mdxm {

namespace {

constexpr wchar_t kClass[] = L"mdxmixerTabEq";
constexpr int kCombo = 1200, kEnable = 1201;
constexpr int kGridBase = 1210;   // + row*3 + {0 freq, 1 gain, 2 q}
constexpr size_t kBands = 10;

struct EqTabState {
    UiContext* ctx = nullptr;
    std::vector<std::wstring> chIds;   // combobox order; L"mic" last when mic configured
    int current = 0;
};

std::wstring SelectedId(EqTabState* st) {
    if (st->current < 0 || st->current >= (int)st->chIds.size()) return L"";
    return st->chIds[(size_t)st->current];
}

// Band values come from the config snapshot: the engine owns runtime EQ state,
// the config is what the UI edits and what persists.
void LoadGrid(HWND hwnd, EqTabState* st) {
    std::wstring id = SelectedId(st);
    const MixerConfig& cfg = st->ctx->store->Get();
    const EqConfig* eq = nullptr;
    if (id == L"mic") eq = &cfg.mic.eq;
    else
        for (const auto& c : cfg.channels)
            if (c.id == id) { eq = &c.eq; break; }
    SendMessageW(GetDlgItem(hwnd, kEnable), BM_SETCHECK,
                 (eq && eq->enabled) ? BST_CHECKED : BST_UNCHECKED, 0);
    for (size_t r = 0; r < kBands; ++r) {
        EqBandConfig band;   // defaults when the config has fewer bands
        if (eq && r < eq->bands.size()) band = eq->bands[r];
        wchar_t buf[32];
        swprintf(buf, 32, L"%g", band.freq);
        SetWindowTextW(GetDlgItem(hwnd, kGridBase + (int)r * 3 + 0), buf);
        swprintf(buf, 32, L"%g", band.gainDb);
        SetWindowTextW(GetDlgItem(hwnd, kGridBase + (int)r * 3 + 1), buf);
        swprintf(buf, 32, L"%g", band.q);
        SetWindowTextW(GetDlgItem(hwnd, kGridBase + (int)r * 3 + 2), buf);
    }
}

void ApplyRow(HWND hwnd, EqTabState* st, int row) {
    std::wstring id = SelectedId(st);
    if (id.empty()) return;
    double v[3] = {};
    for (int c = 0; c < 3; ++c) {
        wchar_t buf[64] = {};
        GetWindowTextW(GetDlgItem(hwnd, kGridBase + row * 3 + c), buf, 64);
        wchar_t* end = nullptr;
        v[c] = wcstod(buf, &end);
        if (end == buf || *end != L'\0') { LoadGrid(hwnd, st); return; }   // invalid: revert
    }
    st->ctx->ctl->SetEqBand(id, (size_t)row, v[0], v[1], v[2]);
    st->ctx->store->Mutate([&](MixerConfig& cfg) {
        EqConfig* eq = nullptr;
        if (id == L"mic") eq = &cfg.mic.eq;
        else
            for (auto& ch : cfg.channels)
                if (ch.id == id) { eq = &ch.eq; break; }
        if (!eq) return;
        while (eq->bands.size() <= (size_t)row) eq->bands.push_back({});
        eq->bands[(size_t)row] = { v[0], v[1], v[2] };
    });
}

LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = (EqTabState*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    try {
        switch (msg) {
        case WM_CREATE: {
            st = new EqTabState;
            st->ctx = (UiContext*)((CREATESTRUCTW*)lp)->lpCreateParams;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)st);
            HINSTANCE inst = ((CREATESTRUCTW*)lp)->hInstance;
            HWND combo = CreateWindowExW(0, L"COMBOBOX", L"",
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                10, 10, 220, 300, hwnd, (HMENU)(INT_PTR)kCombo, inst, nullptr);
            const MixerConfig& cfg = st->ctx->store->Get();
            for (const auto& c : cfg.channels) {
                SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)(c.name.empty() ? c.id : c.name).c_str());
                st->chIds.push_back(c.id);
            }
            if (!cfg.mic.input.id.empty() || !cfg.mic.input.name.empty()) {
                SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"Mic");
                st->chIds.push_back(L"mic");
            }
            SendMessageW(combo, CB_SETCURSEL, 0, 0);
            CreateWindowExW(0, L"BUTTON", L"EQ enabled", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                            250, 12, 110, 20, hwnd, (HMENU)(INT_PTR)kEnable, inst, nullptr);
            CreateWindowExW(0, L"STATIC", L"Freq (Hz)", WS_CHILD | WS_VISIBLE, 70, 44, 80, 18, hwnd, nullptr, inst, nullptr);
            CreateWindowExW(0, L"STATIC", L"Gain (dB)", WS_CHILD | WS_VISIBLE, 160, 44, 80, 18, hwnd, nullptr, inst, nullptr);
            CreateWindowExW(0, L"STATIC", L"Q", WS_CHILD | WS_VISIBLE, 250, 44, 40, 18, hwnd, nullptr, inst, nullptr);
            for (int r = 0; r < (int)kBands; ++r) {
                wchar_t label[8];
                swprintf(label, 8, L"%d", r + 1);
                int y = 66 + r * 28;
                CreateWindowExW(0, L"STATIC", label, WS_CHILD | WS_VISIBLE, 40, y + 3, 20, 18, hwnd, nullptr, inst, nullptr);
                for (int c = 0; c < 3; ++c)
                    // No WS_EX_CLIENTEDGE: light 3D edge in dark themes.
                    CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                                    70 + c * 90, y, 80, 22, hwnd,
                                    (HMENU)(INT_PTR)(kGridBase + r * 3 + c), inst, nullptr);
            }
            LoadGrid(hwnd, st);
            return 0;
        }
        case WM_COMMAND: {
            if (!st) break;
            int id = LOWORD(wp), code = HIWORD(wp);
            if (id == kCombo && code == CBN_SELCHANGE) {
                st->current = (int)SendMessageW((HWND)lp, CB_GETCURSEL, 0, 0);
                LoadGrid(hwnd, st);
            } else if (id == kEnable && code == BN_CLICKED) {
                bool on = SendMessageW((HWND)lp, BM_GETCHECK, 0, 0) == BST_CHECKED;
                std::wstring ch = SelectedId(st);
                if (!ch.empty()) {
                    st->ctx->ctl->EnableEq(ch, on);
                    st->ctx->store->Mutate([&](MixerConfig& cfg) {
                        if (ch == L"mic") { cfg.mic.eq.enabled = on; return; }
                        for (auto& c : cfg.channels)
                            if (c.id == ch) { c.eq.enabled = on; return; }
                    });
                }
            } else if (id >= kGridBase && id < kGridBase + (int)kBands * 3 && code == EN_KILLFOCUS) {
                ApplyRow(hwnd, st, (id - kGridBase) / 3);
            }
            return 0;
        }
        case MainWindow::kRefreshMsg:
            return 0;   // edits are user-driven; nothing to poll
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

HWND CreateEqTab(HWND parent, UiContext* ctx) {
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
