#include "main_window.h"
#include "resource.h"
#include "ui/ui_metrics.h"
#include "ui/wnd_hotkeys.h"
#include "app/log.h"
#include <commctrl.h>
#include <uxtheme.h>

#pragma comment(lib, "comctl32.lib")

namespace mdxm {

// Where mdxmixer.json lives, and now windows.json beside it. The same rule
// AppController uses: next to the exe, so a copied folder carries its
// settings with it.
static std::wstring ToolConfigDir() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring path(exe);
    size_t slash = path.find_last_of(L'\\');
    return slash == std::wstring::npos ? path : path.substr(0, slash);
}

// Tab page factories (each in its own tab_*.cpp).
HWND CreateMixerTab(HWND parent, UiContext* ctx);
HWND CreateOptionsTab(HWND parent, UiContext* ctx);
HWND CreateRoutingTab(HWND parent, UiContext* ctx);
HWND CreateEqTab(HWND parent, UiContext* ctx);
HWND CreateDevicesTab(HWND parent, UiContext* ctx);

namespace {
constexpr wchar_t kClassName[] = L"mdxmixerMainWindow";
constexpr UINT_PTR kTimerRefresh = 1;      // 250 ms: refresh the active tab
constexpr UINT_PTR kTimerTick1s = 2;       // 1 s: app layer (failover tick, config flush)
constexpr UINT_PTR kTimerDeviceDebounce = 3;   // 500 ms one-shot after a device-change signal
// kCmdTaskbar lives on MainWindow: the Options tab posts it too.
constexpr int kCmdOpen = 1, kCmdAutostart = 3, kCmdExit = 4;
constexpr int kCmdThemeDark = 5, kCmdThemeLight = 6, kCmdThemeSystem = 7, kCmdThemeDracula = 8;
constexpr int kCmdSpinBoxes = 10;

// One cached brush for the tab strip's selection fill: WM_DRAWITEM runs on
// every repaint, so creating and leaking a brush per tab per paint is not an
// option, and the colour only changes when the theme does.
HBRUSH CreateSolidBrushCached(COLORREF c) {
    static COLORREF cached = 0xFFFFFFFF;
    static HBRUSH brush = nullptr;
    if (!brush || cached != c) {
        if (brush) DeleteObject(brush);
        brush = CreateSolidBrush(c);
        cached = c;
    }
    return brush;
}
}

bool MainWindow::Create(HINSTANCE hInstance, UiContext* ctx) {
    m_ctx = ctx;
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_TAB_CLASSES | ICC_BAR_CLASSES | ICC_LISTVIEW_CLASSES };
    InitCommonControlsEx(&icc);

    WNDCLASSW wc = {};
    wc.lpfnWndProc = &MainWindow::WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = kClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    // Both sizes, explicitly. hIcon is the large one (Alt-Tab, the taskbar)
    // and hIconSm the small one (the title bar) -- and with only hIcon set
    // Windows SHRINKS the 256px entry for the caption rather than using the
    // 16px drawing made for exactly that spot.
    //
    // WNDCLASSW has no hIconSm, so the class is registered as WNDCLASSEXW.
    WNDCLASSEXW wcx = { sizeof wcx };
    wcx.lpfnWndProc = wc.lpfnWndProc;
    wcx.hInstance = wc.hInstance;
    wcx.lpszClassName = wc.lpszClassName;
    wcx.hCursor = wc.hCursor;
    wcx.hbrBackground = wc.hbrBackground;
    wcx.hIcon = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_MDXMIXER), IMAGE_ICON,
                                  GetSystemMetrics(SM_CXICON),
                                  GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR);
    wcx.hIconSm = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_MDXMIXER), IMAGE_ICON,
                                    GetSystemMetrics(SM_CXSMICON),
                                    GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    RegisterClassExW(&wcx);

    // Taskbar mode is a normal app window: taskbar button, minimize and
    // maximize boxes, close minimizes. Tray mode is a tool window: no taskbar
    // button (that is what WS_EX_TOOLWINDOW buys), no minimize box, and close
    // hides to the tray. The tray icon is present in both.
    bool taskbar = m_ctx->store->Get().ui.taskbarButton;
    DWORD exStyle = taskbar ? WS_EX_APPWINDOW : WS_EX_TOOLWINDOW;
    // Resizable: the mixer is a list that grows with the channel count, and a
    // fixed window either strands space or clips rows.
    m_hwnd = CreateWindowExW(exStyle, kClassName, L"mdxmixer",
                             WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT, 860, 600,
                             nullptr, nullptr, hInstance, this);
    if (!m_hwnd) return false;

    m_metrics.Init(m_hwnd);
    m_font = CreateUiFont(m_metrics.dpi, false);
    m_fontStrong = CreateUiFont(m_metrics.dpi, true);

    // TCS_BUTTONS draws the tabs as a plain strip with no display-area frame.
    // The default frame is the light rectangle that outlined the whole client
    // area in every dark theme, and it cannot be recoloured; owner-draw lets
    // the strip carry the theme's own colours instead.
    m_tabs = CreateWindowExW(0, WC_TABCONTROLW, L"",
                             WS_CHILD | WS_VISIBLE | TCS_BUTTONS | TCS_FLATBUTTONS |
                             TCS_OWNERDRAWFIXED | TCS_FIXEDWIDTH,
                             0, 0, 800, 520, m_hwnd, (HMENU)(INT_PTR)100, hInstance, nullptr);
    TabCtrl_SetItemSize(m_tabs, m_metrics.S(96), m_metrics.S(30));
    // Owner-draw only paints the tabs themselves; the strip behind them is
    // erased by the control with the system button face, which is the light
    // band that was left running across the top of every dark theme.
    SetWindowSubclass(m_tabs, &MainWindow::TabStripProc, 0, (DWORD_PTR)this);
    const wchar_t* names[kPageCount] = { L"Mixer", L"Routing", L"EQ", L"Devices", L"Options" };
    for (int i = 0; i < kPageCount; ++i) {
        TCITEMW item = {};
        item.mask = TCIF_TEXT;
        item.pszText = const_cast<wchar_t*>(names[i]);
        TabCtrl_InsertItem(m_tabs, i, &item);
    }
    RECT rc;
    GetClientRect(m_hwnd, &rc);
    MoveWindow(m_tabs, 0, 0, rc.right, rc.bottom, TRUE);
    RECT disp = rc;
    TabCtrl_AdjustRect(m_tabs, FALSE, &disp);

    m_pages[0] = CreateMixerTab(m_tabs, m_ctx);
    m_pages[1] = CreateRoutingTab(m_tabs, m_ctx);
    m_pages[2] = CreateEqTab(m_tabs, m_ctx);
    m_pages[3] = CreateDevicesTab(m_tabs, m_ctx);
    m_pages[4] = CreateOptionsTab(m_tabs, m_ctx);
    for (int i = 0; i < kPageCount; ++i)
        if (m_pages[i])
            MoveWindow(m_pages[i], disp.left, disp.top, disp.right - disp.left, disp.bottom - disp.top, TRUE);
    SwitchTab(0);
    // The overlay is independent of this window: it comes up if config says
    // so, whether or not the mixer is ever shown.
    ApplyOptions();

    // Nothing called WM_SETFONT before, so every control drew in the ancient
    // default bitmap face. This is the single biggest visual difference.
    ApplyFontToChildren(m_hwnd, m_font);

    // The tray is a 16px slot, so it is handed the 16px drawing rather than
    // a shrunk 256. LoadIcon would give the large one and let the shell
    // scale it.
    m_tray.Add(m_hwnd,
               (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_MDXMIXER), IMAGE_ICON,
                                 GetSystemMetrics(SM_CXSMICON),
                                 GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR),
               L"mdxmixer");
    // The tool windows share one host: the parent to centre on, where their
    // geometry file lives, the font size the +/- buttons move, and the live
    // theme — so a window added later matches this one without being told to.
    m_toolHost.parent = m_hwnd;
    m_toolHost.configDir = ToolConfigDir();
    m_toolHost.theme = m_ctx->theme;
    m_toolHost.fontSize = m_ctx->store->Get().ui.toolFontSize;
    m_toolHost.onFontSizeChanged = [this](int size) {
        m_ctx->store->Mutate([&](MixerConfig& c) { c.ui.toolFontSize = size; });
    };
    ToolWindow::InitWindowStore(m_toolHost.configDir.c_str());
    ApplyHotkeys();

    // Where it was last time, if that is still somewhere a person can see.
    //
    // Validated against the CURRENT monitors rather than trusted: a window
    // saved on a second display that is now unplugged would open off-screen,
    // and on this desk one monitor sits at a large negative origin, so "off
    // -screen" is not something a sign test can decide. MonitorFromRect with
    // MONITOR_DEFAULTTONULL answers it properly.
    {
        // The size the layout actually needs, as a floor under both the
        // first-run default and anything restored from a narrower build.
        RECT need = { 0, 0, m_metrics.MinClientW(), m_metrics.MinClientH() };
        AdjustWindowRectEx(&need, (DWORD)GetWindowLongPtrW(m_hwnd, GWL_STYLE), FALSE,
                           (DWORD)GetWindowLongPtrW(m_hwnd, GWL_EXSTYLE));
        const int minW = need.right - need.left, minH = need.bottom - need.top;

        WindowPlacement wp = m_ctx->store->Get().window;
        if (wp.w < minW) wp.w = minW;
        if (wp.h < minH) wp.h = minH;
        if (wp.w > 0 && wp.h > 0) {
            RECT want = { wp.x, wp.y, wp.x + wp.w, wp.y + wp.h };
            const bool placed = m_ctx->store->Get().window.w > 0 &&
                                MonitorFromRect(&want, MONITOR_DEFAULTTONULL) != nullptr;
            if (placed)
                MoveWindow(m_hwnd, wp.x, wp.y, wp.w, wp.h, FALSE);
            else
                SetWindowPos(m_hwnd, nullptr, 0, 0, wp.w, wp.h,
                             SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        if (wp.maximized) ShowWindow(m_hwnd, SW_MAXIMIZE);
    }

    SetTimer(m_hwnd, kTimerRefresh, 250, nullptr);
    SetTimer(m_hwnd, kTimerTick1s, 1000, nullptr);
    ApplyThemeVisuals();
    return true;
}

void MainWindow::SavePlacement() {
    if (!m_hwnd || !m_ctx || !m_ctx->store) return;
    WINDOWPLACEMENT wp = { sizeof wp };
    if (!GetWindowPlacement(m_hwnd, &wp)) return;
    // rcNormalPosition, not the window rect: it is the restored size even
    // while the window is maximized, so maximising does not overwrite the
    // size to come back to.
    const RECT& r = wp.rcNormalPosition;
    const bool maxed = (wp.showCmd == SW_SHOWMAXIMIZED);
    m_ctx->store->Mutate([&](MixerConfig& c) {
        c.window.x = (int)r.left;
        c.window.y = (int)r.top;
        c.window.w = (int)(r.right - r.left);
        c.window.h = (int)(r.bottom - r.top);
        c.window.maximized = maxed;
    });
}

void MainWindow::RebuildPages() {
    for (int i = 0; i < kPageCount; ++i)
        if (m_pages[i]) SendMessageW(m_pages[i], kRebuildMsg, 0, 0);
}

void MainWindow::ApplyOptions() {
    if (!m_ctx || !m_ctx->store) return;
    const MixerConfig& c = m_ctx->store->Get();
    m_battery.Apply((HINSTANCE)GetWindowLongPtrW(m_hwnd, GWLP_HINSTANCE),
                    c.batteryOverlay,
                    m_ctx->theme ? m_ctx->theme() : nullptr,
                    [this](int x, int y, int fontSize) {
                        // Persisted after a drag or a resize, the two ways the
                        // overlay changes without the Options tab knowing. The
                        // tab reloads these from config on its refresh tick,
                        // so its boxes catch up on their own.
                        m_ctx->store->Mutate([&](MixerConfig& cfg) {
                            cfg.batteryOverlay.x = x;
                            cfg.batteryOverlay.y = y;
                            cfg.batteryOverlay.fontSize = fontSize;
                        });
                    });
    if (m_ctx->ctl) m_battery.SetDevices(m_ctx->ctl->GetDeviceLevels());
}

void MainWindow::ApplyThemeVisuals() {
    if (!m_ctx->theme) return;
    const ThemeState* t = m_ctx->theme();
    if (!t) return;
    ApplyWindowFrameTheme(m_hwnd, t->colors);   // caption, border, caption text
    ApplyControlTheme(m_hwnd, t->colors);
}

void MainWindow::Destroy() {
    // Before the main window: the overlay is a top-level window of its own,
    // so nothing else takes it down, and one left on screen after mdxmixer
    // exits could not be closed at all — it has no frame and, click-through,
    // no way to be clicked.
    m_battery.Destroy();
    m_tray.Remove();
    if (m_hwnd) DestroyWindow(m_hwnd);
    m_hwnd = nullptr;
    if (m_font) { DeleteObject(m_font); m_font = nullptr; }
    if (m_fontStrong) { DeleteObject(m_fontStrong); m_fontStrong = nullptr; }
}

void MainWindow::Show() {
    ShowWindow(m_hwnd, SW_SHOW);
    if (IsIconic(m_hwnd)) ShowWindow(m_hwnd, SW_RESTORE);

    // SetForegroundWindow alone is not enough, and that is by design.
    //
    // Windows refuses the call outright when the asking process is not
    // already the foreground one -- it flashes the taskbar button instead.
    // A global hotkey is exactly that case: the point of it is that some
    // OTHER window has the focus, usually a full-screen game, so "open the
    // mixer" opened the mixer somewhere behind it.
    //
    // PORTED from MDropDX12's ForceForegroundWindow (engine_hotkeys.cpp).
    // Attaching this thread's input queue to the foreground window's thread
    // makes the two count as one for the purposes of that rule, which is
    // what lets the call through; the attachment is undone immediately.
    //
    // Deliberately only on an EXPLICIT request -- a hotkey, the tray, a
    // second instance asking the first to show itself. mdx12 learned the
    // other half of this the hard way: its tool windows did the same thing
    // on an automatic show and stole focus mid-sentence, which at 80wpm is
    // several characters delivered to the wrong window. Nothing in mdxmixer
    // calls Show() on its own.
    const HWND fore = GetForegroundWindow();
    if (fore == m_hwnd) return;

    const DWORD foreThread = GetWindowThreadProcessId(fore, nullptr);
    const DWORD thisThread = GetCurrentThreadId();
    const bool attach = (foreThread != 0 && foreThread != thisThread);
    if (attach) AttachThreadInput(foreThread, thisThread, TRUE);

    BringWindowToTop(m_hwnd);
    SetForegroundWindow(m_hwnd);
    SetActiveWindow(m_hwnd);

    if (attach) AttachThreadInput(foreThread, thisThread, FALSE);
}

void MainWindow::Hide() { ShowWindow(m_hwnd, SW_HIDE); }

void MainWindow::SwitchTab(int index) {
    m_activeTab = index;
    for (int i = 0; i < kPageCount; ++i)
        if (m_pages[i]) ShowWindow(m_pages[i], i == index ? SW_SHOW : SW_HIDE);
    TabCtrl_SetCurSel(m_tabs, index);
    if (m_pages[index]) SendMessageW(m_pages[index], kRefreshMsg, 0, 0);
}

void MainWindow::ApplyTaskbarStyle() {
    bool taskbar = m_ctx->store->Get().ui.taskbarButton;
    bool wasVisible = IsWindowVisible(m_hwnd);
    // The shell only re-reads the ex-style when the window re-registers, hence hide/show.
    ShowWindow(m_hwnd, SW_HIDE);
    LONG_PTR ex = GetWindowLongPtrW(m_hwnd, GWL_EXSTYLE);
    ex = taskbar ? ((ex & ~WS_EX_TOOLWINDOW) | WS_EX_APPWINDOW)
                 : ((ex & ~WS_EX_APPWINDOW) | WS_EX_TOOLWINDOW);
    SetWindowLongPtrW(m_hwnd, GWL_EXSTYLE, ex);
    if (wasVisible) ShowWindow(m_hwnd, SW_SHOW);
}


MainWindow::~MainWindow() = default;

void MainWindow::ApplyHotkeys() {
    if (!m_ctx || !m_ctx->ctl) return;
    m_hotkeys.Apply(m_hwnd, m_ctx->ctl->GetHotkeys());
    // Say what Windows actually granted, once per pass. A combination another
    // application owns is the one fact the user cannot discover any other way.
    for (const HotkeyGrant& g : m_hotkeys.Grants()) {
        if (g.held) continue;
        Log(1, L"hotkey %s could not be registered (error %lu) - another "
               L"application holds it", FormatCombo(g.mod, g.vk).c_str(), g.lastError);
    }
}

void MainWindow::ShowHotkeysWindow() {
    if (!m_hotkeysWnd)
        m_hotkeysWnd.reset(new HotkeysWindow(&m_toolHost, m_ctx ? m_ctx->ctl : nullptr));
    m_hotkeysWnd->Open();
}

std::wstring MainWindow::HotkeyStatus(const std::wstring& bindingId) const {
    const HotkeyGrant* g = m_hotkeys.Find(bindingId);
    if (!g) return L"unbound";
    if (g->held) return L"held";
    // ERROR_HOTKEY_ALREADY_REGISTERED is the one worth naming: it is the
    // answer the user cannot get any other way.
    if (g->lastError == ERROR_HOTKEY_ALREADY_REGISTERED)
        return L"another app holds it";
    return L"refused (error " + std::to_wstring(g->lastError) + L")";
}

HWND MainWindow::HotkeysHwnd() const {
    return m_hotkeysWnd ? m_hotkeysWnd->GetHWND() : nullptr;
}

void MainWindow::ShowTrayMenu() {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kCmdOpen, L"Open Mixer");
    bool taskbar = m_ctx->store->Get().ui.taskbarButton;
    AppendMenuW(menu, MF_STRING | (taskbar ? MF_CHECKED : 0), kCmdTaskbar, L"Show in taskbar");
    bool autostart = m_ctx->getAutostart ? m_ctx->getAutostart() : false;
    AppendMenuW(menu, MF_STRING | (autostart ? MF_CHECKED : 0), kCmdAutostart, L"Start with Windows");
    HMENU themeMenu = CreatePopupMenu();
    const std::wstring& mode = m_ctx->store->Get().ui.theme;
    AppendMenuW(themeMenu, MF_STRING | (mode == L"dracula" ? MF_CHECKED : 0), kCmdThemeDracula, L"Dracula");
    AppendMenuW(themeMenu, MF_STRING | (mode == L"dark" ? MF_CHECKED : 0), kCmdThemeDark, L"Dark green");
    AppendMenuW(themeMenu, MF_STRING | (mode == L"light" ? MF_CHECKED : 0), kCmdThemeLight, L"Light");
    AppendMenuW(themeMenu, MF_STRING | (mode == L"system" ? MF_CHECKED : 0), kCmdThemeSystem, L"System");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)themeMenu, L"Theme");
    AppendMenuW(menu, MF_STRING, kCmdHotkeys, L"Hotkeys...");
    bool spin = m_ctx->store->Get().ui.spinBoxes;
    AppendMenuW(menu, MF_STRING | (spin ? MF_CHECKED : 0), kCmdSpinBoxes,
                L"Volume as step buttons");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdExit, L"Exit");
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(m_hwnd);   // required or the menu won't dismiss properly
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, m_hwnd, nullptr);
    DestroyMenu(menu);
}

LRESULT CALLBACK MainWindow::TabStripProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                          UINT_PTR, DWORD_PTR ref) {
    auto* self = (MainWindow*)ref;
    const ThemeState* t = (self && self->m_ctx && self->m_ctx->theme) ? self->m_ctx->theme() : nullptr;

    if (t && t->colors.dark) {
        // The strip is painted end to end here. comctl32 draws a frame around
        // every button no matter which styles are set, and those boxes are pure
        // noise once the selected tab is already a filled block: the selection
        // is the information, the boxes just repeat the control's existence.
        if (msg == WM_ERASEBKGND) return 1;   // WM_PAINT covers every pixel
        if (msg == WM_PAINT) {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            FillRect(dc, &rc, t->bgBrush);
            int count = TabCtrl_GetItemCount(hwnd);
            int active = TabCtrl_GetCurSel(hwnd);
            SetBkMode(dc, TRANSPARENT);
            for (int i = 0; i < count; ++i) {
                RECT ir;
                if (!TabCtrl_GetItemRect(hwnd, i, &ir)) continue;
                bool sel = (i == active);
                if (sel) {
                    HBRUSH fill = CreateSolidBrush(t->colors.selBg);
                    FillRect(dc, &ir, fill);
                    DeleteObject(fill);
                    RECT bar = ir;
                    bar.top = bar.bottom - self->m_metrics.S(2);
                    HBRUSH accent = CreateSolidBrush(t->colors.accent);
                    FillRect(dc, &bar, accent);
                    DeleteObject(accent);
                }
                wchar_t text[64] = {};
                TCITEMW item = {};
                item.mask = TCIF_TEXT;
                item.pszText = text;
                item.cchTextMax = 63;
                TabCtrl_GetItem(hwnd, i, &item);
                SetTextColor(dc, sel ? t->colors.text : t->colors.muted);
                HGDIOBJ oldFont = SelectObject(dc, sel ? self->m_fontStrong : self->m_font);
                DrawTextW(dc, text, -1, &ir, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                SelectObject(dc, oldFont);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
    }
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(hwnd, &MainWindow::TabStripProc, 0);
    return DefSubclassProc(hwnd, msg, wp, lp);
}

LRESULT CALLBACK MainWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    MainWindow* self;
    if (msg == WM_NCCREATE) {
        self = (MainWindow*)((CREATESTRUCTW*)lp)->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    self = (MainWindow*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    try {
        return self->Handle(hwnd, msg, wp, lp);
    } catch (...) {
        return DefWindowProcW(hwnd, msg, wp, lp);   // no-crash rule
    }
}

LRESULT MainWindow::Handle(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case TrayIcon::kTrayMessage:
        if (LOWORD(lp) == WM_LBUTTONDBLCLK) Show();
        else if (LOWORD(lp) == WM_RBUTTONUP) ShowTrayMenu();
        return 0;

    case WM_DRAWITEM: {
        // The tab strip. Active tab reads as a filled block in the selection
        // colour with the accent underlining it; the rest stay quiet.
        auto* di = (DRAWITEMSTRUCT*)lp;
        if (!m_ctx->theme || di->hwndItem != m_tabs) break;
        const ThemeState* t = m_ctx->theme();
        if (!t) break;
        bool active = (int)di->itemID == m_activeTab;
        FillRect(di->hDC, &di->rcItem, active ? CreateSolidBrushCached(t->colors.selBg)
                                              : t->bgBrush);
        if (active) {
            RECT bar = di->rcItem;
            bar.top = bar.bottom - m_metrics.S(2);
            HBRUSH accent = CreateSolidBrush(t->colors.accent);
            FillRect(di->hDC, &bar, accent);
            DeleteObject(accent);
        }
        wchar_t text[64] = {};
        TCITEMW item = {};
        item.mask = TCIF_TEXT;
        item.pszText = text;
        item.cchTextMax = 63;
        TabCtrl_GetItem(m_tabs, di->itemID, &item);
        SetBkMode(di->hDC, TRANSPARENT);
        SetTextColor(di->hDC, active ? t->colors.text : t->colors.muted);
        HGDIOBJ oldFont = SelectObject(di->hDC, active ? m_fontStrong : m_font);
        DrawTextW(di->hDC, text, -1, &di->rcItem, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(di->hDC, oldFont);
        return TRUE;
    }

    case kShowMsg:
        Show();
        return 0;

    case kMoveOverlayMsg:
        // Bring it up first if it is not showing: asking to move something
        // invisible should produce the thing, not nothing.
        if (!m_battery.Visible()) {
            m_ctx->store->Mutate([](MixerConfig& c) { c.batteryOverlay.enabled = true; });
            ApplyOptions();
            RebuildPages();
        }
        m_battery.BeginInteractiveMove();
        return 0;

    case kSnapOverlayMsg: {
        if (!m_battery.Visible()) {
            m_ctx->store->Mutate([](MixerConfig& c) { c.batteryOverlay.enabled = true; });
            ApplyOptions();
            RebuildPages();
        }
        const auto displays = Displays();
        const size_t which = (size_t)lp < displays.size() ? (size_t)lp : 0;
        if (displays.empty()) return 0;
        const RECT b = m_battery.Bounds();
        const POINT p = SnapToCorner(displays[which].rc,
                                     (int)(b.right - b.left), (int)(b.bottom - b.top),
                                     (int)wp);
        m_ctx->store->Mutate([&](MixerConfig& c) {
            c.batteryOverlay.x = p.x;
            c.batteryOverlay.y = p.y;
        });
        ApplyOptions();
        // So the X and Y boxes show where it actually went.
        if (m_pages[m_activeTab]) SendMessageW(m_pages[m_activeTab], kRefreshMsg, 0, 0);
        return 0;
    }

    case kOptionsChangedMsg:
        ApplyOptions();
        // The step buttons and their labels are built into each row, so a
        // change to either switch is a rebuild rather than a repaint.
        RebuildPages();
        return 0;

    case kIpcMsg: {
        auto* req = (IpcRequest*)wp;
        if (req && m_ctx->dispatchIpc)
            req->replies = m_ctx->dispatchIpc(*req->msg, req->wantSubscribe);
        return 0;
    }

    case kDeviceChangeMsg:
        // Signal-only watcher posted this (fj#401); coalesce bursts with a
        // one-shot 500 ms timer, then do the real work on this (UI) thread.
        SetTimer(hwnd, kTimerDeviceDebounce, 500, nullptr);
        return 0;

    // Suspend and resume. A top-level window receives these with nothing to
    // register for, which is the only reason this is cheap enough to have
    // been an oversight rather than a decision.
    //
    // PBT_APMRESUMEAUTOMATIC fires for every wake; PBT_APMRESUMESUSPEND only
    // when a user is present. Both are handled and the engine's resume path is
    // idempotent, because a wake with no user is still a wake and the mixer
    // still has to be playing when he sits down.
    //
    // Not relied upon. Modern Standby does not guarantee these for every
    // transition, so Engine::TickWatchdog recovers without them; this only
    // makes the recovery immediate instead of three seconds late, and puts a
    // line in the log saying a resume happened at all.
    case WM_POWERBROADCAST:
        switch (wp) {
        case PBT_APMSUSPEND:
            if (m_ctx->onSuspend) m_ctx->onSuspend();
            break;
        case PBT_APMRESUMEAUTOMATIC:
        case PBT_APMRESUMESUSPEND:
            if (m_ctx->onResume) m_ctx->onResume();
            break;
        default:
            break;
        }
        return TRUE;   // TRUE, not 0: for PBT_APMQUERYSUSPEND 0 means "deny"

    case WM_TIMER:
        if (wp == kTimerRefresh) {
            // UNCONDITIONAL, before the visible-only refresh below. This is
            // the peak push, and a client watching for which channel just
            // started blasting is watching exactly when mdxmixer is in the
            // tray and nobody is looking at its window. It costs one atomic
            // read when nothing is subscribed.
            if (m_ctx->onTick250ms) m_ctx->onTick250ms();
            if (IsWindowVisible(hwnd) && m_pages[m_activeTab])
                SendMessageW(m_pages[m_activeTab], kRefreshMsg, 0, 0);
        } else if (wp == kTimerTick1s) {
            if (m_ctx->onTick1s) m_ctx->onTick1s();
            // The overlay updates on the SECOND tick, not the 250 ms one, and
            // whether or not the main window is visible — being readable with
            // mdxmixer closed is the whole point of it. A battery percentage
            // moves once every several minutes, so a second is already far
            // more often than it changes; SetDevices repaints only when a
            // line or a level actually differs, so an idle overlay draws
            // nothing over whatever is underneath it.
            if (m_battery.Visible() && m_ctx->ctl)
                m_battery.SetDevices(m_ctx->ctl->GetDeviceLevels());
        } else if (wp == kTimerDeviceDebounce) {
            KillTimer(hwnd, kTimerDeviceDebounce);
            if (m_ctx->onDeviceChangeDebounced) m_ctx->onDeviceChangeDebounced();
        }
        return 0;

    case WM_NOTIFY: {
        auto* hdr = (NMHDR*)lp;
        if (hdr->hwndFrom == m_tabs && hdr->code == TCN_SELCHANGE)
            SwitchTab(TabCtrl_GetCurSel(m_tabs));
        return 0;
    }

    case WM_SIZE: {
        // Tray mode has no taskbar button, so a minimize would send the window
        // somewhere with nothing to click. Hide it to the tray instead, which
        // is where it is recovered from anyway.
        if (wp == SIZE_MINIMIZED && !m_ctx->store->Get().ui.taskbarButton) {
            Hide();
            return 0;
        }
        if (!m_tabs) return 0;
        RECT rc;
        GetClientRect(hwnd, &rc);
        MoveWindow(m_tabs, 0, 0, rc.right, rc.bottom, TRUE);
        RECT disp = rc;
        TabCtrl_AdjustRect(m_tabs, FALSE, &disp);
        for (HWND page : m_pages)
            if (page)
                MoveWindow(page, disp.left, disp.top,
                           disp.right - disp.left, disp.bottom - disp.top, TRUE);
        return 0;
    }


    case kExitMsg:
        if (m_ctx->exitApp) m_ctx->exitApp();
        return 0;

    case kShowHotkeysMsg:
        ShowHotkeysWindow();
        return 0;

    case WM_HOTKEY: {
        // The id is ours, handed out by the registrar; look up which binding
        // it belongs to rather than assuming an order.
        std::wstring id = m_hotkeys.BindingForId((int)wp);
        if (!id.empty() && m_ctx && m_ctx->ctl) m_ctx->ctl->TriggerHotkey(id);
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case kCmdOpen:
            Show();
            return 0;
        case kCmdTaskbar:
            m_ctx->store->Mutate([](MixerConfig& c) { c.ui.taskbarButton = !c.ui.taskbarButton; });
            ApplyTaskbarStyle();
            return 0;
        case kCmdAutostart:
            if (m_ctx->setAutostart && m_ctx->getAutostart)
                m_ctx->setAutostart(!m_ctx->getAutostart());
            return 0;
        case kCmdHotkeys:
            ShowHotkeysWindow();
            return 0;
        case kCmdSpinBoxes:
            m_ctx->store->Mutate([](MixerConfig& c) { c.ui.spinBoxes = !c.ui.spinBoxes; });
            // The rows are built around whichever control is in use, so the
            // page is rebuilt rather than patched.
            RebuildPages();
            return 0;
        case kCmdThemeDracula:
            if (m_ctx->setTheme) m_ctx->setTheme(L"dracula");
            return 0;
        case kCmdThemeDark:
            if (m_ctx->setTheme) m_ctx->setTheme(L"dark");
            return 0;
        case kCmdThemeLight:
            if (m_ctx->setTheme) m_ctx->setTheme(L"light");
            return 0;
        case kCmdThemeSystem:
            if (m_ctx->setTheme) m_ctx->setTheme(L"system");
            return 0;
        case kCmdExit:
            if (MessageBoxW(hwnd,
                    L"Apps routed to cables go silent until mdxmixer runs again. Exit anyway?",
                    L"mdxmixer", MB_YESNO | MB_ICONWARNING) == IDYES) {
                if (m_ctx->exitApp) m_ctx->exitApp();
            }
            return 0;
        }
        return 0;

    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
        if (m_ctx->theme) {
            LRESULT r;
            if (const ThemeState* t = m_ctx->theme(); t && ThemeCtlColor(msg, wp, *t, &r)) return r;
        }
        break;
    }

    case WM_ERASEBKGND: {
        if (m_ctx->theme) {
            LRESULT r;
            if (const ThemeState* t = m_ctx->theme(); t && ThemeEraseBkgnd(hwnd, wp, *t, &r)) return r;
        }
        break;
    }

    case WM_CLOSE:
        // Taskbar mode: behave like a normal app (minimize); tray mode: vanish to tray.
        if (m_ctx->store->Get().ui.taskbarButton) ShowWindow(hwnd, SW_MINIMIZE);
        else Hide();
        return 0;

    case WM_GETMINMAXINFO: {
        // Enforced rather than merely defaulted: a window dragged narrower
        // than its contents clips controls silently, and the Devices tab has
        // a panel pinned to its right edge that simply leaves the screen.
        if (!m_hwnd) break;
        RECT need = { 0, 0, m_metrics.MinClientW(), m_metrics.MinClientH() };
        AdjustWindowRectEx(&need, (DWORD)GetWindowLongPtrW(m_hwnd, GWL_STYLE), FALSE,
                           (DWORD)GetWindowLongPtrW(m_hwnd, GWL_EXSTYLE));
        auto* mm = (MINMAXINFO*)lp;
        mm->ptMinTrackSize.x = need.right - need.left;
        mm->ptMinTrackSize.y = need.bottom - need.top;
        return 0;
    }

    case WM_EXITSIZEMOVE:
        SavePlacement();
        return 0;

    case WM_DESTROY:
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace mdxm
