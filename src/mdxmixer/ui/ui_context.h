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
    // An app started playing (fj#1). Windows' per-app routing is keyed by
    // process id, so a stored assignment can only be written while the app is
    // running -- this is the moment it becomes writable, and before it existed
    // an app launched while mdxmixer sat in the tray kept whatever route
    // Windows already had for it.
    std::function<void()> onSessionsChangedDebounced;
    // IPC requests are marshaled onto the UI thread (SendMessage from the pipe
    // client thread), so config + engine control stays single-threaded.
    std::function<std::vector<std::wstring>(const std::wstring& msg, bool* wantSubscribe,
                                            int* wantIntervalMs)> dispatchIpc;
    // Theme: the live state (colors + brushes) and the mode setter ("dark"/"light"/"system").
    std::function<const ThemeState*()> theme;
    std::function<void(const std::wstring&)> setTheme;
    // A VBAN device with the right PIN that has never been approved: ask the
    // person at the PC. Called on the UI thread (the receive thread only posts),
    // so this may show a dialog.
    std::function<void(const std::wstring& deviceId, const std::wstring& name)> vbanAuthRequested;
    // What the person said. Called from the dialog's own handler on the UI thread.
    std::function<void(const std::wstring& deviceId, bool allow)> vbanAuthResult;
    // The person dismissed the prompt without answering (Esc, close, or the
    // dialog failed to open). NOT a denial: the device stays able to ask again.
    std::function<void(const std::wstring& deviceId)> vbanAuthDismissed;
    // Apply a VBAN config change that rebinds the socket, deferred off the
    // handler that requested it (see MainWindow::kVbanApplyMsg).
    std::function<void()> vbanApply;
    // One that has just been authorized, so it can be remembered and the next
    // connection is instant.
    std::function<void(const std::wstring& deviceId, const std::wstring& name)> vbanDeviceAuthorized;
    // False when the undocumented per-app routing API did not resolve on this
    // Windows build. The spec's posture is "assignment features degrade and say
    // so" — the Routing tab has to be able to say it.
    std::function<bool()> routingAvailable;
};

} // namespace mdxm
