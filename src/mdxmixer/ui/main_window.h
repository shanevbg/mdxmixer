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
#include <atomic>
#include <memory>
#include "ui/ui_metrics.h"
#include <windows.h>

namespace mdxm {

class MainWindow {
public:
    // The Hotkeys window's handle, or null when it is not open. For
    // MDXM_CAPTURE, which has to draw it from inside this process.
    HWND HotkeysHwnd() const;
    HWND OverlayHwnd() const { return m_battery.Hwnd(); }
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
    // Posted by the session watcher (signal-only, fj#1): an app started
    // playing somewhere, so the stored app assignments are worth re-applying.
    // Debounced here like a device change, because one app starting can create
    // several sessions and every render endpoint reports its own.
    static constexpr UINT kSessionChangeMsg = WM_APP + 13;
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
    // A VBAN device with the right PIN that has never been approved, and the
    // device that has just been approved. POSTED from the VBAN receive thread,
    // never sent: the first raises a dialog the user may leave on screen for
    // minutes, and the receive thread has pings to answer in the meantime.
    //
    // lParam is a heap `std::wstring*` of "deviceId\nname", owned by the handler.
    static constexpr UINT kVbanAuthMsg = WM_APP + 14;
    static constexpr UINT kVbanAuthorizedMsg = WM_APP + 15;
    // Posted to apply a VBAN config change that rebinds the socket (on/port),
    // DEFERRED rather than applied inline. A network-carried `MDXM_VBAN|port=`
    // runs on this thread while the VBAN receive thread is blocked waiting for
    // the same handler to return; applying inline would have Stop() join that
    // blocked thread. Posting runs the rebuild after the handler returns.
    static constexpr UINT kVbanApplyMsg = WM_APP + 16;

    // One marshalled IPC request, REFERENCE COUNTED AND HEAP OWNED.
    //
    // It used to live on the sending pipe thread's stack, with the window
    // handler writing its replies back through a pointer. That is a
    // use-after-free waiting for a slow tick, and on 2026-10-07 it crashed
    // twice: SendMessageTimeout gives up after five seconds and the pipe
    // thread returns MDXM_ERR|msg=busy, but the message is still in the UI
    // thread's queue. When the UI thread got to it, `req->replies = ...`
    // assigned a std::vector into a stack frame that no longer existed --
    // and assigning a vector first destroys what it believes is already
    // there, so it freed garbage pointers. The dump reads exactly that:
    // vector<wstring>::operator= at the handler, _Destroy_range,
    // _Tidy_deallocate, _free_base, then __fastfail(FAST_FAIL_INVALID_ARG).
    //
    // So both sides now hold a reference and the last one out frees it. The
    // sender takes nothing by pointer -- the message is COPIED in, because a
    // reference to the sender's string is the same bug wearing a different
    // hat -- and reads the replies only when the Send actually completed.
    //
    // A request whose message is never dispatched (the window closed while it
    // sat in the queue) leaks one small object. That is the deliberate trade:
    // a bounded leak at shutdown against a write into a dead stack.
    struct IpcRequest {
        explicit IpcRequest(std::wstring m, bool sub) : msg(std::move(m)), wantSubscribe(sub) {}
        std::wstring msg;                   // owned, not borrowed
        bool wantSubscribe = false;
        // The push rate this client asked for, or -1 when it did not
        // (MDXM_SUBSCRIBE's optional second argument).
        int wantIntervalMs = -1;
        std::vector<std::wstring> replies;
        // 2 at birth: one for the sender, one for the queued message.
        std::atomic<int> refs{2};
        void Release() { if (refs.fetch_sub(1, std::memory_order_acq_rel) == 1) delete this; }
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
    // The sticky tack, PORTED from MDropDX12's ToolWindow, which carries the
    // same \xE718 Segoe MDL2 pin on every one of its tool windows: an
    // owner-drawn glyph button at the right of the title row, blue when it is
    // holding the window up and grey when it is not. mdxmixer already has that
    // code for the Hotkeys window (ui/tool_window.cpp); the main window is not
    // a ToolWindow, so it wears the same button built the same way rather than
    // a second design.
    void BuildPin();
    void LayoutPin();        // re-anchored on every resize: it hangs off the right edge
    void ApplyAlwaysOnTop(); // push the window into or out of the topmost band

    UiContext* m_ctx = nullptr;
    HWND m_hwnd = nullptr;
    HWND m_tabs = nullptr;
    // Mixer, Routing, EQ, Devices, VBAN, Options.
    //
    // VBAN sits before Options so Options stays last, which is where people look
    // for it. The cost is that a config saved while Options was tab 4 reopens on
    // VBAN once; it corrects itself on the next deliberate tab change.
    static constexpr int kPageCount = 6;
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
    HWND  m_pin = nullptr;         // the always-on-top tack
    HFONT m_pinFont = nullptr;     // Segoe MDL2 Assets, sized for the pin's box
    bool  m_onTop = false;         // mirrors ui.alwaysOnTop; the pin's state
};

} // namespace mdxm
