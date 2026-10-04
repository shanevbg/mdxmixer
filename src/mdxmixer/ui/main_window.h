#pragma once
// The one native window: a tab control hosting Mixer / Routing / EQ / Devices,
// plus the tray icon and its menu. Close hides to tray (or minimizes, in
// taskbar mode); Exit lives only in the tray menu, behind the routed-apps-go-
// silent warning (spec).
#include "ui/ui_context.h"
#include "ui/battery_overlay.h"
#include "ui/tray_icon.h"
#include "ui/tool_window.h"
#include "ui/wnd_hotkeys.h"
#include "app/hotkeys.h"
#include <memory>
#include "ui/ui_metrics.h"
#include <windows.h>

namespace mdxm {

class MainWindow {
public:
    // The Hotkeys window's handle, or null when it is not open. For
    // MDXM_CAPTURE, which has to draw it from inside this process.
    HWND HotkeysHwnd() const;
    // What the last registration pass got for this binding.
    std::wstring HotkeyStatus(const std::wstring& bindingId) const;

    // Out of line: m_hotkeysWnd holds a type this header only forward
    // declares, and unique_ptr needs the full type wherever it is destroyed.
    ~MainWindow();
    // ctx must outlive the window (the app controller owns it).
    bool Create(HINSTANCE hInstance, UiContext* ctx);
    void Show();
    void Hide();
    HWND Hwnd() const { return m_hwnd; }
    void Destroy();

    static constexpr UINT kDeviceChangeMsg = WM_APP + 2;  // posted by the device watcher (signal-only)
    static constexpr UINT kShowMsg = WM_APP + 4;          // posted from any thread: raise the window
    static constexpr UINT kRefreshMsg = WM_APP + 5;       // sent to the active tab every 250 ms
    // Posted from any thread: open the Hotkeys window. It has to happen on
    // the UI thread, both because the window framework expects it and because
    // the keys it edits are registered against this window.
    static constexpr UINT kShowHotkeysMsg = WM_APP + 7;
    // Posted from any thread: exit through the ordinary path, no prompt.
    static constexpr UINT kExitMsg = WM_APP + 8;
    // Sent to a page when a setting changed the shape of its controls.
    static constexpr UINT kRebuildMsg = WM_APP + 9;
    // The Options tab changed something the window owns: the battery overlay
    // and the fader rows both take their shape from config, so they are told
    // rather than left to notice on a later tick.
    static constexpr UINT kOptionsChangedMsg = WM_APP + 10;
    // Make the battery overlay grabbable and start dragging it.
    static constexpr UINT kMoveOverlayMsg = WM_APP + 11;
    // Park it in a corner: wParam = corner (0 TL, 1 TR, 2 BL, 3 BR),
    // lParam = display index as Displays() enumerates them.
    static constexpr UINT kSnapOverlayMsg = WM_APP + 12;
    // The tray menu's "Show in taskbar", shared with the Options tab so the
    // checkbox and the menu item drive one code path instead of two.
    static constexpr int kCmdTaskbar = 2;
    // The hotkey assignment window, opened from the tray menu and from the
    // Options tab. Shared for the same reason as kCmdTaskbar: one command,
    // one handler, no second way for the two to disagree.
    static constexpr int kCmdHotkeys = 9;
    static constexpr UINT kIpcMsg = WM_APP + 6;           // SendMessage from pipe threads: wParam = IpcRequest*

    struct IpcRequest {                                   // lives on the sender's stack for the Send
        const std::wstring* msg;
        bool* wantSubscribe;
        std::vector<std::wstring> replies;
    };

    void SelectTab(int index) { SwitchTab(index); }
    void ApplyThemeVisuals();   // frame + control styles + repaint, from ctx->theme()
    // Rebuild every tab's rows, because WHICH rows exist has changed.
    //
    // The 250 ms kRefreshMsg tick updates the values in rows that already
    // exist; it cannot add a row, remove one, or rewrite a label. So a headset
    // connecting left its row exactly as it was built — and that is how Shane
    // came to find a row whose status column read "27 Sep 05:34" sitting under
    // a peak meter that was moving: the meter is painted live, the status text
    // was written once, when the device really was disconnected.
    //
    // Nothing called this on a device change before; the only rebuild was the
    // spin-boxes option. With disconnected devices now filed out of the list
    // it is load-bearing — plugging a headset in has to make its row appear.
    void RebuildPages();
    // Remember where the window is and how big. Called when a move or resize
    // finishes and once on the way out.
    void SavePlacement();
    // Re-apply everything the config says about the window's own furniture:
    // the battery overlay's styles, position and text.
    void ApplyOptions();

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    // Paints the tab strip's own background; see the note at its call site.
    static LRESULT CALLBACK TabStripProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    LRESULT Handle(HWND, UINT, WPARAM, LPARAM);
    void ShowTrayMenu();
    // Re-reads the table and asks Windows for every bound combination. Safe to
    // call whenever the table changes; it releases what it held first.
    void ApplyHotkeys();
    void ShowHotkeysWindow();
    void ApplyTaskbarStyle();
    void SwitchTab(int index);

    UiContext* m_ctx = nullptr;
    HWND m_hwnd = nullptr;
    HWND m_tabs = nullptr;
    // Mixer, Routing, EQ, Devices, Options.
    static constexpr int kPageCount = 5;
    HWND m_pages[kPageCount] = {};
    BatteryOverlay m_battery;
    int m_activeTab = 0;
    TrayIcon m_tray;

    // Global hotkeys live here because RegisterHotKey demands the thread that
    // owns the window, and WM_HOTKEY is delivered to it.
    HotkeyRegistrar m_hotkeys;
    ToolHost m_toolHost;
    std::unique_ptr<HotkeysWindow> m_hotkeysWnd;
    UiMetrics m_metrics;
    HFONT m_font = nullptr;        // shell UI face, applied to every control
    HFONT m_fontStrong = nullptr;  // section headings and the active tab
};

} // namespace mdxm
