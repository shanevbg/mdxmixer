#pragma once
// session_watcher.h — notice a new audio session, so a stored app assignment
// becomes true when the app next plays (fj#1).
//
// WHY THIS HAS TO EXIST. Windows' per-app routing API is keyed by PROCESS ID,
// so an assignment can only be WRITTEN while the app is running, while the
// config records it by exe path -- which is what a person means by "Spotify
// goes to Media". Everything in routing/reconcile.{h,cpp} turns the second
// into the first, and until now nothing called it when an app started
// playing: the reconcile ran at startup, on a device change and after a
// resume, so an app launched while mdxmixer sat in the tray kept whatever
// route Windows already had for it.
//
// NOT A POLL, for the reason the spec gives for device events: EnumerateSessions
// activates IAudioSessionManager2 on every active render endpoint, and that is
// the call path that faulted inside AudioSes.dll while Bluetooth headsets came
// and went. Running it every second to find out whether anything started is
// more dangerous than the problem.
//
// SIGNAL-ONLY CONTRACT (MDropDX12 fj#401), exactly as DeviceWatcher states it:
// the Callback runs inside the audio stack's own notification dispatch. It must
// only signal -- PostMessage / SetEvent -- and must never call back into
// MMDevice, IAudioSessionControl or any COM device API. The
// IAudioSessionControl handed to OnSessionCreated is deliberately not touched,
// not even to read the process id: that is a call back into the API from
// inside its own callback.
//
// ON ITS OWN MTA THREAD, which is the one real difference from DeviceWatcher.
// mdxmixer's UI thread is an STA (main.cpp joins APARTMENTTHREADED, because the
// window needs it), and a session notification registered from an STA is
// delivered by marshaling back into that apartment -- so the audio stack ends
// up waiting on mdxmixer's message queue to dispatch it. The registrations and
// the COM objects behind them therefore live on a dedicated MTA thread that
// does nothing else, and the callback they produce only posts.
#include <functional>

namespace mdxm {

class SessionWatcher {
public:
    ~SessionWatcher();
    // "a new audio session appeared somewhere" -- which one is not reported,
    // because finding that out means asking the API from inside its own
    // callback. The receiver debounces and re-sweeps on its own thread, the
    // same shape as a device change.
    using Callback = std::function<void()>;
    // Starts the watcher thread and registers on every active render endpoint.
    // True when the thread started; registration happens on that thread, so
    // Registered() is not meaningful the instant this returns. Calling it
    // again replaces the previous watcher rather than stacking a second one.
    bool Start(Callback onSessionCreated);
    void Stop();
    // The device set has changed: drop the registrations and take new ones.
    // A new endpoint has no registration and a departed one's manager is dead,
    // so without this an app that starts playing to a headset plugged in since
    // startup is never noticed. Returns at once -- the work happens on the
    // watcher's thread.
    void Rebind();
    // How many endpoints are currently registered. For the log, and for a test
    // that cannot make a session appear.
    int Registered() const;

private:
    struct Impl;
    Impl* m_impl = nullptr;
};

} // namespace mdxm
