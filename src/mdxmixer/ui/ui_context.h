#pragma once
// What the UI is allowed to touch. ALL mixer logic stays out of the UI: every
// user action is one IMixerControl call; every displayed value comes from
// GetChannels()/GetDiag() snapshots or the config store. That seam is what the
// protocol tests already cover.
#include "config/config.h"
#include "ipc/mixer_control.h"
#include "ui/theme.h"
#include <functional>

namespace mdxm {

struct UiContext {
    IMixerControl* ctl = nullptr;
    ConfigStore* store = nullptr;
    std::function<void()> restartEngine;        // Devices tab Apply
    // The hotkey table changed: the main window re-registers, because
    // RegisterHotKey demands the thread that owns the window.
    std::function<void()> hotkeysChanged;
    std::function<void(bool)> setAutostart;     // writes the HKCU Run key (only on explicit toggle)
    std::function<bool()> getAutostart;
    std::function<void()> exitApp;
    std::function<void()> onTick1s;             // app layer: failover tick + config flush
    // app layer, four times a second, WHETHER OR NOT THE WINDOW IS VISIBLE:
    // pushes peak levels to subscribed IPC clients. Separate from the 250 ms
    // page refresh, which is rightly skipped while hidden -- a client watching
    // for which channel just started blasting is watching precisely when
    // mdxmixer is in the tray and nobody is looking at its window.
    std::function<void()> onTick250ms;
    // The machine is going to sleep / has come back (WM_POWERBROADCAST).
    //
    // A top-level window gets these without registering for anything, which is
    // why they land on the UI layer and are handed down. Nothing in mdxmixer
    // listened for them at all until a Modern Standby resume left the render
    // stream wedged on a Bluetooth endpoint that still looked perfectly
    // healthy -- no audio, and not one log line, for seven hours.
    std::function<void()> onSuspend;
    std::function<void()> onResume;
    std::function<void()> onDeviceChangeDebounced;  // app layer: engine OnDeviceSetChanged
    // IPC requests are marshaled onto the UI thread (SendMessage from the pipe
    // client thread), so config + engine control stays single-threaded.
    std::function<std::vector<std::wstring>(const std::wstring& msg, bool* wantSubscribe)> dispatchIpc;
    // Theme: the live state (colors + brushes) and the mode setter ("dark"/"light"/"system").
    std::function<const ThemeState*()> theme;
    std::function<void(const std::wstring&)> setTheme;
    // False when the undocumented per-app routing API did not resolve on this
    // Windows build. The spec's posture is "assignment features degrade and say
    // so" — the Routing tab has to be able to say it.
    std::function<bool()> routingAvailable;
};

} // namespace mdxm
