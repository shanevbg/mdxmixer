#include "app_controller.h"
#include "app/autostart.h"
#include "app/log.h"
#include "app/channel_setup.h"
#include "app/hotkeys.h"
#include "device/endpoints.h"
#include "device/endpoint_volume.h"
#include "device/device_identity.h"
#include "device/device_order.h"
#include "device/sweep_selection.h"
#include "ipc/protocol.h"
#include "routing/sessions.h"
#include "routing/reconcile.h"
#include "routing/default_endpoint.h"
#include "ui/window_capture.h"
#include <algorithm>

namespace mdxm {

// THE one marshalling discipline, shared by every transport that carries MDXM
// records: the pipe, and now the network. Extracted from the pipe handler so the
// two crashes it encodes stay fixed in exactly one place -- see
// MainWindow::IpcRequest for what they were.
//
// Called from a pipe client thread or from the VBAN receive thread. Marshals onto
// the UI thread so control stays single-threaded. SendMessageTimeout, not
// SendMessage: a UI thread that is busy, modal, or shutting down must never park
// the caller indefinitely (ABORTIFHUNG returns rather than waiting on a hung
// queue).
//
// HEAP OWNED AND REFERENCE COUNTED, never a pointer to this stack.
// SendMessageTimeout returning does not take the message back out of the UI
// thread's queue, so a request that times out here is still dispatched
// afterwards -- into whatever has replaced this frame by then.
std::vector<std::wstring> DispatchToUi(HWND hwnd, const std::wstring& msg,
                                       bool* wantSubscribe, int* wantIntervalMs) {
    auto* req = new MainWindow::IpcRequest(msg, wantSubscribe && *wantSubscribe);
    DWORD_PTR unused = 0;
    const bool handled = SendMessageTimeoutW(hwnd, MainWindow::kIpcMsg, (WPARAM)req, 0,
                                             SMTO_ABORTIFHUNG | SMTO_NORMAL, 5000,
                                             &unused) != 0;
    std::vector<std::wstring> replies;
    if (handled) {
        // Only now is the handler known to have finished, so only now are these
        // worth reading.
        replies = std::move(req->replies);
        if (wantSubscribe) *wantSubscribe = req->wantSubscribe;
        if (wantIntervalMs) *wantIntervalMs = req->wantIntervalMs;
    } else {
        replies.push_back(L"MDXM_ERR|msg=busy");
    }
    req->Release();
    return replies;
}

std::wstring AppController::ExeDir() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring path(exe);
    size_t slash = path.find_last_of(L'\\');
    return slash == std::wstring::npos ? path : path.substr(0, slash);
}

bool AppController::Start(HINSTANCE hInstance) {
    std::wstring dir = ExeDir();
    bool usedDefaults = false;
    MixerConfig cfg = LoadConfig(dir + L"\\mdxmixer.json", &usedDefaults);
    m_firstRun = usedDefaults;
    m_store.Init(dir + L"\\mdxmixer.json", cfg);
    LogInit(dir, cfg.logLevel);
    Log(2, L"mdxmixer starting%s", usedDefaults ? L" (defaults: no or corrupt config)" : L"");

    std::wstring perr;
    if (!m_policy.Init(&perr))
        Log(1, L"per-app routing degraded: %s", perr.c_str());   // spec risk posture: degrade, say so
    else
        Log(2, L"per-app routing available (%s iid)", m_policy.IidUsed());

    std::wstring eerr;
    m_engine.Start(cfg, &eerr);
    if (!eerr.empty()) Log(1, L"engine notes: %s", eerr.c_str());
    m_engine.SetFailoverCommitCallback([this](const DeviceRef& d) {
        // UI thread (TickFailover runs there). Fail over, never back: the commit
        // rewrites the configured personal output.
        m_store.Mutate([&](MixerConfig& c) {
            // A commit under "use failover" must NOT write the device back:
            // that would bind the route to whatever happened to be present
            // and silently leave the mode. The whole point of the mode is
            // that the list decides, every time.
            if (c.personalOutput.id != kFollowFailover) c.personalOutput = d;
        });
        Log(2, L"failover committed to %s", d.name.c_str());
        BroadcastState();
    });

    m_uiCtx.ctl = this;
    m_uiCtx.store = &m_store;
    m_uiCtx.restartEngine = [this] { RestartEngine(); };
    m_uiCtx.setAutostart = [](bool on) { mdxm::SetAutostart(on); };
    m_uiCtx.getAutostart = [] { return mdxm::GetAutostart(); };
    m_uiCtx.exitApp = [] { PostQuitMessage(0); };
    m_uiCtx.onTick1s = [this] {
        // BEFORE the failover tick. The watchdog can restart a wedged render
        // on the device we are already bound to, which is the cheap repair;
        // letting failover look first would have it re-home to a different
        // headset for a fault that one restart fixes.
        m_engine.TickWatchdog();
        m_engine.TickFailover();
        m_store.FlushIfDue();
        // Sonar's levels move from Sonar's own window too, so they are polled
        // rather than assumed. 1 Hz is the ceiling SonarChannels enforces.
        //
        // WHETHER Sonar answers decides whether its rows exist at all, so a
        // change of that answer rebuilds the pages — Sonar being disabled or
        // crashing must take its faders away, and it coming back must put
        // them back, without a restart.
        const bool had = m_sonar.Available();
        m_sonar.Refresh(false);
        if (m_sonar.Available() != had) {
            Log(2, L"sonar channels: %s", m_sonar.Available() ? L"available" : L"gone");
            m_window.RebuildPages();
        }
    };
    m_uiCtx.onTick250ms = [this] { PushToSubscribers(); TickVban(); };
    m_uiCtx.onSuspend = [this] { m_engine.OnSuspend(); };
    m_uiCtx.onResume = [this] {
        m_engine.OnResume();
        // A socket does not necessarily survive a Modern Standby resume, and
        // nothing reports that it did not -- the same shape as the render stream
        // that came back listed, active and silent. Re-arm it the way
        // RenderRetry re-arms a render rather than waiting to be told.
        ApplyVbanConfig();
        // Sonar comes back from a resume in its own time, and sometimes not at
        // all -- the same bug on their side, as Shane put it. Forcing the
        // refresh rather than waiting for the 1 Hz poll means its rows and
        // levels are right as soon as it answers, and the rebuild below takes
        // its faders away if it does not.
        const bool had = m_sonar.Available();
        m_sonar.Refresh(true);
        if (m_sonar.Available() != had) {
            Log(2, L"sonar channels after resume: %s",
                m_sonar.Available() ? L"available" : L"gone");
            m_window.RebuildPages();
        }
        // An app routed to a cable can come back pointing somewhere else: the
        // channel recovers by name, its apps do not (fj#1). Same reason the
        // device-change path does this.
        ReconcileRouting(L"resumed from suspend");
        BroadcastState();
    };
    // An app started playing somewhere. The only thing to do is make the
    // stored assignments true, which is cheap when they already are:
    // PlanRouteFixes writes nothing for a session that is already on the right
    // endpoint, and nothing at all for an app no channel claims.
    m_uiCtx.onSessionsChangedDebounced = [this] {
        ReconcileRouting(L"an app started playing");
    };
    // Give every stored device reference the anchors it is missing, now, with
    // whatever is connected. See HealStoredDeviceRefs.
    HealStoredDeviceRefs(L"startup");
    m_uiCtx.onDeviceChangeDebounced = [this] {
        // The cached per-endpoint COM interfaces belong to the device set that
        // just changed: an endpoint that has gone takes its volume and meter
        // objects with it. Dropped FIRST, so everything below re-reads.
        //
        // Here and not in the notification itself, which may only signal
        // (fj#401) -- this runs on the UI thread, which is the thread that
        // owns the cache.
        InvalidateEndpointCache();
        // A device arriving is the moment its stored entries can learn what
        // they are missing -- and a change of ADAPTER arrives as a burst of
        // exactly these, with every headset wearing a new id.
        HealStoredDeviceRefs(L"devices changed");
        m_engine.OnDeviceSetChanged();
        // The endpoints have moved, so the session registrations have to
        // follow them: a headset that just connected has a session manager
        // nobody is registered with, and an app starting on it would go
        // unnoticed (fj#1).
        m_sessions.Rebind();
        // The device set moving is exactly when an app can end up on a dead
        // endpoint: the channel recovers by name, its apps do not (fj#1).
        ReconcileRouting(L"devices changed");
        BroadcastState();
        // WHICH rows exist has changed — see MainWindow::RebuildPages. The
        // 250 ms tick only updates rows that already exist, so without this a
        // headset you just switched on has no row, and one you switched off
        // keeps the row it had, status text and all.
        m_window.RebuildPages();
    };
    m_uiCtx.dispatchIpc = [this](const std::wstring& msg, bool* sub, int* intervalMs) {
        return HandleProtocolMessage(msg, *this, sub, intervalMs);
    };
    m_uiCtx.routingAvailable = [this] { return m_policy.IsAvailable(); };
    // A device asking to be let in. The window raises the dialog; this only logs
    // that it was asked, so the log tells the same story as the screen did.
    m_uiCtx.vbanAuthRequested = [](const std::wstring& id, const std::wstring& name) {
        Log(2, L"vban: '%s' (%s) is asking for access", name.c_str(), id.c_str());
    };
    // What the person said.
    m_uiCtx.vbanAuthResult = [this](const std::wstring& id, bool allow) {
        m_vban.AuthorizationResult(id, allow);
        Log(2, L"vban: access for '%s' was %s", id.c_str(), allow ? L"allowed" : L"refused");
    };
    // The prompt was dismissed without an answer. Forget that the device was
    // asked about -- but do NOT deny it -- so a re-sent AUTH prompts again.
    m_uiCtx.vbanAuthDismissed = [this](const std::wstring& id) {
        m_vban.ForgetDevice(id);
        Log(2, L"vban: access prompt for '%s' was dismissed; it may ask again", id.c_str());
    };
    // A deferred socket-rebinding apply (see SetVbanOption / kVbanApplyMsg).
    m_uiCtx.vbanApply = [this] { ApplyVbanConfig(); };
    // One that has just been authorized: remember it, so the next connection from
    // it is instant rather than another prompt.
    m_uiCtx.vbanDeviceAuthorized = [this](const std::wstring& id, const std::wstring& name) {
        SYSTEMTIME lt = {};
        GetLocalTime(&lt);
        wchar_t when[32];
        swprintf(when, 32, L"%04d-%02d-%02d %02d:%02d", lt.wYear, lt.wMonth, lt.wDay,
                 lt.wHour, lt.wMinute);
        bool changed = false;
        m_store.Mutate([&](MixerConfig& c) {
            changed = UpsertAuthorizedDevice(c.vban.authorizedDevices, id, name, when);
        });
        if (changed) Log(2, L"vban: remembered '%s' (%s)", name.c_str(), id.c_str());
    };
    Log(2, L"theme: %s (system dark=%d)", cfg.ui.theme.c_str(), SystemPrefersDark() ? 1 : 0);
    m_theme.Rebuild(ResolveTheme(cfg.ui.theme, SystemPrefersDark()));
    m_uiCtx.theme = [this]() -> const ThemeState* { return &m_theme; };
    m_uiCtx.setTheme = [this](const std::wstring& mode) {
        m_store.Mutate([&](MixerConfig& c) { c.ui.theme = mode; });
        m_theme.Rebuild(ResolveTheme(mode, SystemPrefersDark()));
        m_window.ApplyThemeVisuals();
    };
    if (!m_window.Create(hInstance, &m_uiCtx)) {
        Log(1, L"window creation failed");
        return false;
    }

    std::wstring pipeErr;
    HWND hwnd = m_window.Hwnd();
    bool pipeOk = m_pipe.Start([hwnd](const std::wstring& msg, bool* sub, int* intervalMs) {
        return DispatchToUi(hwnd, msg, sub, intervalMs);
    }, &pipeErr);
    if (!pipeOk) Log(1, L"pipe server: %s", pipeErr.c_str());

    // The VBAN listener, if it is switched on. Unlike the shared-memory feed
    // this one PERSISTS across restarts: the point of the feature is that the
    // phone can subscribe at any moment without anyone touching the PC. What
    // stays on demand is emission -- TickVban keeps the engine's ring write
    // following whether anybody is actually listening.
    ApplyVbanConfig();

    // Signal-only (fj#401): the notification callback only posts.
    m_watcher.Start([hwnd] { PostMessageW(hwnd, MainWindow::kDeviceChangeMsg, 0, 0); });

    // An app starting to play is the moment a stored assignment becomes
    // writable, because Windows keys per-app routing by process id (fj#1).
    // Same contract as the device watcher: the callback only posts, and the
    // reconcile happens on the UI thread after a debounce.
    //
    // Started AFTER the pipe and the device watcher, and before the startup
    // reconcile below, so nothing that begins playing during startup falls
    // between the two.
    if (m_sessions.Start([hwnd] { PostMessageW(hwnd, MainWindow::kSessionChangeMsg, 0, 0); }))
        Log(2, L"session watcher: %d endpoint(s)", m_sessions.Registered());
    else
        Log(1, L"session watcher did not start; stored app routes will be applied "
               L"on device changes and at startup only");

    // fj#1: apply the stored app assignments to whatever is already playing.
    // Windows persists a per-app route itself, so this is usually a no-op --
    // the cases it exists for are an app assigned while it was shut, and an
    // app left pointing at a cable that has come back under a new endpoint id.
    ReconcileRouting(L"startup");

    if (m_firstRun) {
        // Spec: present setup (and the autostart choice, unticked in the tray
        // menu) at first run. Devices tab is where cables get bound.
        m_window.SelectTab(3);
        m_window.Show();
    }
    return true;
}

void AppController::Stop() {
    m_watcher.Stop();
    // Before the pipe and the engine: it owns a thread that holds COM
    // references into the audio stack, and those have to be let go while
    // there is still an audio stack to let go of.
    m_sessions.Stop();
    m_pipe.Stop();
    // Before the engine: the sender thread reads the engine's ring, so it has to
    // be finished before the engine it borrows from goes away.
    m_vban.Stop();
    m_engine.Stop();
    m_store.FlushNow();
    m_window.Destroy();
    Log(2, L"mdxmixer stopped");
}

void AppController::RestartEngine() {
    Log(2, L"engine restart (device bindings changed)");
    m_engine.Stop();
    std::wstring eerr;
    m_engine.Start(m_store.Get(), &eerr);
    if (!eerr.empty()) Log(1, L"engine notes: %s", eerr.c_str());
    BroadcastState();
}

void AppController::BroadcastState() {
    // GetChannels(), not m_engine.GetChannelStates(), and ChannelRecord(), not
    // a swprintf of the same fields. Both were wrong in the same quiet way.
    //
    // The engine's list has no Sonar rows, so a subscriber was never pushed a
    // Sonar channel at all -- it saw six faders appear on an MDXM_STATE poll
    // and then never move again, while a client that only subscribed saw them
    // not at all. And the record was formatted HERE as well as in protocol.cpp,
    // so `peak` would have reached clients that poll and not clients that
    // listen: two spellings of one record is a bug with a delay fuse.
    for (const auto& c : GetChannels()) m_pipe.Broadcast(ChannelRecord(c));
    DiagState d = m_engine.GetDiag();
    m_pipe.Broadcast(L"MDXM_ROUTE|id=personal|device=" + d.personalDevice);
}

// Peak levels to anyone listening, four times a second.
//
// A SEPARATE PUSH from BroadcastState, because the two answer different
// questions and move at different speeds. BroadcastState fires when something
// was changed -- a fader moved, a route switched -- and a subscriber reads it
// as "state changed". Peaks change constantly and change nothing; folding them
// into MDXM_CHAN would make every client re-read every fader position it
// already knows, four times a second, and leave it unable to tell a push that
// means "someone moved a fader" from one that means "the music got louder".
//
// NOTHING HAPPENS WHEN NOBODY IS LISTENING, and the test is made before the
// work rather than inside Broadcast: this runs forever, and the endpoint sweep
// it needs activates COM on every endpoint. That is the cost the issue was
// worried about, and it is paid only while a client has actually asked.
//
// This is also why it is a pushed record and not something a client polls.
// MDXM_STATE is a full state block; polling one four times a second to watch a
// number move is exactly what section 7 of docs/ipc.md says not to do.
void AppController::PushToSubscribers() {
    const bool subscribed = m_pipe.HasSubscribers();
    if (subscribed) {
        // Nobody is owed a push yet. This runs on the 100 ms timer so that a
        // client asking for 100 ms can have it; a client on the default 250
        // simply is not due on two ticks out of three, and the cost of
        // noticing that is one lock and a comparison.
        if (!m_pipe.AnySubscriberDue((unsigned)GetTickCount())) return;
    }
    if (!subscribed) {
        // Forget what was pushed, so the next client to subscribe is handed
        // the whole device list rather than the tail of a conversation it was
        // not part of.
        if (m_hadSubscribers) { m_hadSubscribers = false; m_pushedDevs.clear(); }
        return;
    }
    m_hadSubscribers = true;
    // Half a tick's grace. The mixer tab's own 250 ms refresh runs on the same
    // timer message as this does, so when the window is visible one of the two
    // has almost always just swept; sweeping again would double the rate of the
    // one call known to fault inside AudioSes.dll to rebuild a list that is
    // already in hand. When nothing has swept recently -- mdxmixer in the tray,
    // which is the normal case for a subscribed client -- this is the sweep.
    const unsigned now = (unsigned)GetTickCount();
    const bool fresh = m_lastSweepMs != 0 && (now - m_lastSweepMs) < 125 &&
                       !m_lastLevels.empty();
    const auto& levels = fresh ? m_lastLevels : (m_lastLevels = GetDeviceLevels());
    // One `now` for the whole push: the records go to whoever is due at that
    // instant, and exactly those clients have their next due time advanced.
    // See PipeServer's AnySubscriberDue / BroadcastDue / MarkPushed.
    m_pipe.BroadcastDue(PeakRecord(GetChannels(), levels), now);
    PushDeviceRows(levels, now);
    m_pipe.MarkPushed(now);
}

// Device rows to anyone listening (fj#7).
//
// BroadcastState pushes MDXM_CHAN per channel and one MDXM_ROUTE, and that was
// all: a subscriber never learned that a DEVICE row had changed -- an
// endpoint's volume, its mute, whether it went active or away, its battery.
// So a front-end delegating its device list to MDXM_DEVLVL had to go on
// polling MDXM_STATE, which section 7 of the ipc doc tells it not to do, for
// the right reason: on this machine that block is seven channel rows plus
// eighty-odd device rows. It is the same fault BroadcastState's own comment
// records about Sonar channels -- a row that appears on a poll and then never
// moves for a subscriber -- one record along.
//
// WHY THIS RUNS ON THE SWEEP RATHER THAN ON EVENTS. The issue asked for device
// arrival and loss to come from IMMNotificationClient and for volume and mute
// changes to be "already observed". Only the first is true: nothing in this
// program is told when a Windows volume moves, a battery falls, or an endpoint
// goes inactive -- those are read by the sweep and in no other way. An
// event-driven push would therefore cover a subset of the changes and need its
// own sweeps to format a row, while this sweep is already running, is already
// paid for by the peak push, and runs ONLY while a client is subscribed. The
// sweep is the event. Latency is one tick, 250 ms, which is below the rate any
// of this is drawn at.
//
// What is NOT done here is the thing a timer would get wrong: nothing is sent
// when nothing changed. Every row is diffed against what the subscriber was
// last told, peak excluded (SameDeviceRow) -- peaks move constantly and travel
// on MDXM_PEAK, so including them would turn this into eighty records four
// times a second saying nothing.
void AppController::PushDeviceRows(const std::vector<DeviceLevel>& levels, unsigned nowMs) {
    // Has the SET of rows changed, or their order? A client with a control per
    // row has to rebuild its controls then, and only then -- MDropDX12's
    // window does exactly that, and updates values otherwise. A removal is
    // also the one change no row can carry: a device that has gone has no row
    // to push, so without this record nothing would say so.
    bool setChanged = m_pushedDevs.size() != levels.size();
    for (size_t i = 0; !setChanged && i < levels.size(); ++i)
        if (m_pushedDevs[i].id != levels[i].id) setChanged = true;

    if (setChanged) {
        m_pipe.BroadcastDue(DeviceSetRecord(levels), nowMs);
        for (const auto& d : levels) m_pipe.BroadcastDue(DeviceRecord(d), nowMs);
    } else {
        for (size_t i = 0; i < levels.size(); ++i)
            if (!SameDeviceRow(m_pushedDevs[i], levels[i]))
                m_pipe.BroadcastDue(DeviceRecord(levels[i]), nowMs);
    }
    m_pushedDevs = levels;
}

std::vector<ChannelState> AppController::GetChannels() {
    auto out = m_engine.GetChannelStates();

    // Is the mix actually running?
    //
    // The engine's peak-holds are fed by MixPull, which is driven by the
    // personal render callback -- so with no render device at all MixPull stops
    // being called and every held peak freezes at whatever was playing when the
    // device went away. Frozen and presented as current, it points at the wrong
    // row, which is worse than saying nothing.
    //
    // Watched for MOVEMENT against this thread's clock rather than a timestamp
    // the audio thread writes, because the audio thread must not call a clock
    // (dsp/peak_hold.h). Two sweeps' grace at the UI's 250 ms tick: enough that
    // an ordinary scheduling hiccup does not blank the meters.
    {
        const unsigned now = (unsigned)GetTickCount();
        const uint64_t frames = m_engine.MixFrames();
        if (!m_mixSeen || frames != m_lastMixFrames) {
            m_lastMixFrames = frames;
            m_mixMovedMs = now;
            m_mixSeen = true;
        }
        if ((now - m_mixMovedMs) > 600)
            for (auto& c : out) c.peak = kPeakUnknown;
    }

    // Sonar's channels alongside our own, each carrying the same two levels.
    //
    // This is the thing Shane could do in mdx12 and not here: "we gave up
    // completely on being able to adjust the personal aux and streaming aux
    // volumes in mdxmixer but it works in mdx12". It works there because mdx12
    // asks Sonar over its local HTTP API, which is the only thing that moves a
    // Sonar channel — its virtual endpoints accept a Windows volume write,
    // return success, and stay at 1.0.
    //
    // Appended rather than merged: these are not engine channels, nothing
    // about them is persisted in mdxmixer.json, and their levels live in
    // Sonar. The "sonar:" prefix on the id is what routes a write back here.
    //
    // The peak comes from the last endpoint sweep rather than a fresh one, and
    // only while that sweep is recent enough for the value to still be inside
    // its own hold window: past kPeakHoldMs the reading has expired by its own
    // rules, and reporting it would be the frozen-meter problem again with a
    // different cause.
    const bool sonarPeaksFresh =
        m_sonarPeaksMs != 0 && ((unsigned)GetTickCount() - m_sonarPeaksMs) <= kPeakHoldMs;
    for (const auto& c : m_sonar.Channels()) {
        ChannelState s;
        s.id = L"sonar:" + c.key;
        s.name = c.label;
        s.healthy = true;
        s.pvol = c.personalVol;
        s.pmute = c.personalMute;
        s.svol = c.streamingVol;
        s.smute = c.streamingMute;
        s.eqOn = false;
        s.peak = kPeakUnknown;
        if (sonarPeaksFresh) {
            auto it = m_sonarPeaks.find(c.key);
            if (it != m_sonarPeaks.end()) s.peak = it->second;
            auto nit = m_sonarPeaksNow.find(c.key);
            if (nit != m_sonarPeaksNow.end()) s.peakNow = nit->second;
        }
        out.push_back(s);
    }
    return out;
}

std::vector<std::pair<std::wstring, std::wstring>> AppController::GetRoutes() {
    return { { L"personal", m_engine.GetDiag().personalDevice } };
}

std::vector<std::tuple<std::wstring, std::wstring, bool, bool>> AppController::GetDevices() {
    std::vector<std::tuple<std::wstring, std::wstring, bool, bool>> out;
    for (const auto& e : EnumerateEndpoints())
        out.push_back({ e.id, e.name, e.isRender, e.isActive });
    return out;
}

bool AppController::SetVolume(const std::wstring& ch, Mix m, float vol) {
    vol = std::clamp(vol, 0.0f, 1.0f);
    // A "sonar:" channel belongs to Sonar, not to the engine, and nothing
    // about it is ours to persist — Sonar holds the level.
    if (const std::wstring key = SonarKeyFromChannelId(ch); !key.empty()) {
        if (!m_sonar.SetVolume(key, m == Mix::Personal, vol)) return false;
        BroadcastState();
        return true;
    }
    if (!m_engine.SetVolume(ch, m, vol)) return false;
    m_store.Mutate([&](MixerConfig& cfg) {
        if (ch == L"mic") { cfg.mic.gain = vol; return; }
        for (auto& c : cfg.channels)
            if (c.id == ch) { (m == Mix::Personal ? c.personal : c.streaming).vol = vol; return; }
    });
    BroadcastState();
    return true;
}

bool AppController::SetMute(const std::wstring& ch, Mix m, bool mute) {
    // As SetVolume. Returns false for the Sonar master, whose mute is
    // destructive and is refused rather than sent — see sonar_api.cpp.
    if (const std::wstring key = SonarKeyFromChannelId(ch); !key.empty()) {
        if (!m_sonar.SetMute(key, m == Mix::Personal, mute)) return false;
        BroadcastState();
        return true;
    }
    if (!m_engine.SetMute(ch, m, mute)) return false;
    m_store.Mutate([&](MixerConfig& cfg) {
        for (auto& c : cfg.channels)
            if (c.id == ch) { (m == Mix::Personal ? c.personal : c.streaming).mute = mute; return; }
    });
    BroadcastState();
    return true;
}

bool AppController::SetEqBand(const std::wstring& ch, size_t band, double f, double g, double q) {
    if (!m_engine.SetEqBand(ch, band, f, g, q)) return false;
    m_store.Mutate([&](MixerConfig& cfg) {
        EqConfig* eq = nullptr;
        if (ch == L"mic") eq = &cfg.mic.eq;
        else
            for (auto& c : cfg.channels)
                if (c.id == ch) { eq = &c.eq; break; }
        if (!eq) return;
        while (eq->bands.size() <= band) eq->bands.push_back({});
        eq->bands[band] = { f, g, q };
    });
    BroadcastState();
    return true;
}

bool AppController::EnableEq(const std::wstring& ch, bool on) {
    if (!m_engine.EnableEq(ch, on)) return false;
    m_store.Mutate([&](MixerConfig& cfg) {
        if (ch == L"mic") { cfg.mic.eq.enabled = on; return; }
        for (auto& c : cfg.channels)
            if (c.id == ch) { c.eq.enabled = on; return; }
    });
    BroadcastState();
    return true;
}

// Where apps routed to this channel actually go.
//
// Apps are routed INTO the cable: its render side when one is configured, and
// a render endpoint bound as the channel SOURCE (the loopback tap) also works
// -- mirroring a Sonar channel is exactly that case. Null when the channel has
// nowhere to send right now, which is a cable that is unplugged rather than an
// error.
//
// One copy, because there are three callers -- assignment, the reconciler, and
// the drift column on the Routing tab -- and a fourth answer to "where does
// this channel send" is how a UI comes to disagree with what was written.
static const EndpointInfo* RoutableEndpointFor(const ChannelConfig& c,
                                               const std::vector<EndpointInfo>& eps) {
    if (const EndpointInfo* ep = MatchBinding(eps, c.cable.render)) return ep;
    const EndpointInfo* ep = MatchBinding(eps, c.cable.capture);
    return (ep && ep->isRender) ? ep : nullptr;
}

// Are these apps where they are supposed to be? (fj#1 item 4.)
//
// ONE endpoint enumeration for the whole list. The Routing tab asks about
// every row it draws, and a sweep per row would be twenty of them per refresh
// on this machine -- on the call path that faulted inside AudioSes.dll while
// headsets came and went. The per-app part that remains is a policy read,
// which is keyed by pid and genuinely is per row.
std::vector<AppRouteState> AppController::GetAppRoutes(
    const std::vector<std::pair<std::wstring, unsigned long>>& apps) {
    std::vector<AppRouteState> out;
    out.reserve(apps.size());
    const bool available = m_policy.IsAvailable();
    const auto eps = EnumerateEndpoints();

    // Each channel resolved once, rather than once per app on it.
    // The exe path is COPIED rather than pointed at inside the config. The
    // store does hand out a reference that would stay valid for this call, but
    // a list of pointers into someone else's container is a lifetime trap for
    // the next person to touch this, and a handful of paths is nothing.
    struct Claim { std::wstring exe, channel, endpointId; };
    std::vector<Claim> claims;
    for (const auto& c : m_store.Get().channels) {
        if (c.apps.empty()) continue;
        const EndpointInfo* ep = RoutableEndpointFor(c, eps);
        for (const auto& app : c.apps)
            claims.push_back({ app, c.name.empty() ? c.id : c.name,
                               ep ? ep->id : std::wstring() });
    }

    for (const auto& a : apps) {
        AppRouteState s;
        s.available = available;
        for (const auto& cl : claims)
            if (_wcsicmp(cl.exe.c_str(), a.first.c_str()) == 0) {
                s.intendedChannel = cl.channel;
                s.intendedEndpointId = cl.endpointId;
                break;
            }
        // Only worth asking Windows about an app something actually claims:
        // an unassigned app's route is not mdxmixer's business, and this is a
        // COM round trip per row.
        if (available && a.second != 0 && !s.intendedChannel.empty())
            m_policy.GetPersistedDefaultRender(a.second, &s.actualEndpointId);
        out.push_back(std::move(s));
    }
    return out;
}

bool AppController::AssignApp(const std::wstring& exePath, const std::wstring& chOrDash) {
    bool clearing = (chOrDash == L"-");
    std::wstring targetEndpoint;
    if (!clearing) {
        const ChannelConfig* target = nullptr;
        for (const auto& c : m_store.Get().channels)
            if (c.id == chOrDash) { target = &c; break; }
        if (!target) return false;
        auto eps = EnumerateEndpoints();
        const EndpointInfo* ep = RoutableEndpointFor(*target, eps);
        if (!ep) {
            Log(1, L"assign: channel %s has no routable render endpoint", chOrDash.c_str());
            return false;
        }
        targetEndpoint = ep->id;
    }
    bool any = false;
    for (const auto& s : EnumerateSessions()) {
        if (_wcsicmp(s.exePath.c_str(), exePath.c_str()) != 0) continue;
        std::wstring err;
        bool ok = clearing ? m_policy.ClearPersistedDefaultRender(s.pid, &err)
                           : m_policy.SetPersistedDefaultRender(s.pid, targetEndpoint, &err);
        if (!ok) Log(1, L"assign pid %lu: %s", s.pid, err.c_str());
        any = any || ok;
    }
    // The assignment is RECORDED whether or not anything is playing (fj#1).
    //
    // Windows' API is keyed by process id, so an app that is shut cannot be
    // routed this second — but "assign Spotify to Media" is a statement about
    // Spotify, not about this second. This used to return false here, before
    // the config was touched, so assigning an app meant catching it while it
    // happened to be making noise, and the rollout's own rollback plan —
    // "reassign apps back to their previous devices" — could not be carried
    // out on an app that was not running. The reconciler below is what makes
    // the recorded intent true when the app next plays.
    m_store.Mutate([&](MixerConfig& cfg) {
        for (auto& c : cfg.channels)
            c.apps.erase(std::remove_if(c.apps.begin(), c.apps.end(),
                             [&](const std::wstring& a) { return _wcsicmp(a.c_str(), exePath.c_str()) == 0; }),
                         c.apps.end());
        if (!clearing)
            for (auto& c : cfg.channels)
                if (c.id == chOrDash) { c.apps.push_back(exePath); break; }
    });
    if (!clearing && !any)
        Log(2, L"assign: %s -> %s recorded; it will be applied when the app next plays",
            exePath.c_str(), chOrDash.c_str());
    BroadcastState();
    return true;
}

// ── Reconcile: make the stored assignments true (fj#1) ───────────────────
//
// Runs on the CONTROL thread only. Every call here activates COM interfaces,
// so it must never run on an audio thread or inside a device notification
// (MDropDX12 fj#401) — the session-arrival path signals and this runs later.
void AppController::ReconcileRouting(const wchar_t* why) {
    if (!m_policy.IsAvailable()) return;

    // What the config says should be true, resolved to endpoints as they
    // stand now. A channel whose cable has moved resolves to its NEW id, which
    // is what repairs apps left pointing at the old one.
    auto eps = EnumerateEndpoints();
    std::vector<RouteIntent> intents;
    for (const auto& c : m_store.Get().channels) {
        if (c.apps.empty()) continue;
        const EndpointInfo* ep = RoutableEndpointFor(c, eps);
        for (const auto& app : c.apps)
            intents.push_back({ app, c.id, ep ? ep->id : std::wstring() });
    }
    if (intents.empty()) return;

    std::vector<SessionRoute> live;
    for (const auto& s : EnumerateSessions()) {
        if (s.exePath.empty()) continue;
        SessionRoute r;
        r.exePath = s.exePath;
        r.pid = s.pid;
        m_policy.GetPersistedDefaultRender(s.pid, &r.currentEndpointId);
        live.push_back(r);
    }

    int applied = 0;
    for (const RouteFix& f : PlanRouteFixes(intents, live)) {
        std::wstring err;
        if (m_policy.SetPersistedDefaultRender(f.pid, f.endpointId, &err)) {
            ++applied;
            Log(2, L"reconcile (%s): %s pid %lu -> %s", why, f.exePath.c_str(), f.pid,
                f.channelId.c_str());
        } else {
            Log(1, L"reconcile (%s): %s pid %lu failed: %s", why, f.exePath.c_str(), f.pid,
                err.c_str());
        }
    }
    if (applied) BroadcastState();
}

bool AppController::RouteAppToEndpoint(const std::wstring& exePath,
                                       const std::wstring& endpointId,
                                       std::wstring* err) {
    if (exePath.empty() || endpointId.empty()) {
        if (err) *err = L"need an exe path and an endpoint";
        return false;
    }
    if (!m_policy.IsAvailable()) {
        if (err) *err = L"per-app routing is not available on this Windows build";
        return false;
    }
    int moved = 0, failed = 0;
    std::wstring last;
    for (const auto& s : EnumerateSessions()) {
        if (_wcsicmp(s.exePath.c_str(), exePath.c_str()) != 0) continue;
        std::wstring e;
        if (m_policy.SetPersistedDefaultRender(s.pid, endpointId, &e)) ++moved;
        else { ++failed; last = e; }
    }
    if (!moved) {
        if (err) *err = failed ? last : L"that app has no audio session right now";
        Log(1, L"route %s -> endpoint: nothing moved (%d failures)", exePath.c_str(), failed);
        return false;
    }
    Log(2, L"route %s -> %s: %d process(es)", exePath.c_str(), endpointId.c_str(), moved);
    BroadcastState();
    return true;
}

bool AppController::SetDefaultOutput(const std::wstring& endpointId, std::wstring* err) {
    std::wstring name = endpointId;
    for (const auto& e : EnumerateEndpoints())
        if (e.id == endpointId) { name = e.name; break; }
    if (!SetDefaultRenderEndpoint(endpointId, err)) {
        Log(1, L"default output -> %s FAILED: %s", name.c_str(),
            err && !err->empty() ? err->c_str() : L"(no reason given)");
        return false;
    }
    // Read it back. The first live attempt at this reported S_OK from all
    // three roles and left the default exactly where it was, so "Windows said
    // yes" is not evidence that the default moved.
    std::wstring now = DefaultRenderEndpointId();
    if (_wcsicmp(now.c_str(), endpointId.c_str()) != 0) {
        Log(1, L"default output -> %s: Windows accepted it but the default is "
               L"still %s", name.c_str(), now.c_str());
        if (err) *err = L"Windows accepted the change but did not apply it - "
                        L"something else is holding the default";
        return false;
    }
    Log(2, L"default output -> %s", name.c_str());
    // Apps that were routed per-app are unaffected by this, but the ones that
    // follow the default have just moved; the Routing tab should say so.
    BroadcastState();
    return true;
}

bool AppController::SetPersonalRoute(const std::wstring& endpointId) {
    // The sentinel is not an endpoint. The engine is told "nothing is bound",
    // which is exactly the state the failover watcher acts on, and the
    // sentinel is kept in config so the mode survives a restart and a commit.
    const bool follow = (endpointId == kFollowFailover);
    if (!m_engine.SetPersonalOutput(follow ? std::wstring() : endpointId)) return false;
    m_store.Mutate([&](MixerConfig& cfg) {
        if (follow) { cfg.personalOutput = { kFollowFailover, L"Use failover list" }; return; }
        cfg.personalOutput = { endpointId, endpointId };
        for (const auto& e : EnumerateEndpoints())
            if (e.id == endpointId) { cfg.personalOutput.name = e.name; break; }
    });
    BroadcastState();
    return true;
}

DiagState AppController::GetDiag() { return m_engine.GetDiag(); }

// Endpoint volumes are the device's own level, so these go straight to Windows
// — no engine involvement, nothing to persist in our config.
// The Bluetooth address of one endpoint, for keying a name or a pin on the
// DEVICE rather than on the pairing.
//
// A full sweep per call, which is fine because the callers are user actions
// -- renaming a device, pinning one -- and not a tick. Empty for anything not
// on Bluetooth, which simply leaves the weaker anchors to do the work.
std::wstring BluetoothAddressOf(const std::wstring& endpointId) {
    if (endpointId.empty()) return {};
    for (const auto& d : ListEndpointVolumes())
        if (d.id == endpointId) return d.btAddress;
    return {};
}

void ApplyDeviceView(MixerConfig& cfg, std::vector<DeviceLevel>& levels,
                     const std::wstring& personalRouteId) {
    // Apply our names. DisplayName re-homes an alias onto the endpoint's
    // current id when it matches by Windows name, so a re-paired headset keeps
    // the name it was given — which is the whole point when five of them are
    // called WF-1000XM5-1 through -5.
    auto& names = cfg.deviceNames;
    for (auto& d : levels) {
        d.displayName = DisplayName(names, d.id, d.containerId, d.name, d.btAddress);
        DeviceView view = LookupView(names, d.id, d.containerId, d.name, d.btAddress);
        d.hidden = view.hidden;
        d.pinned = view.pinned;
    }

    // Failover devices held at the top, while they are here.
    //
    // PORTED from MDropDX12, which carries this as `pinFailoverDevices` and
    // applies it last, after its stored order and its sorts, for the reason
    // its comment gives: a device good enough to be on a failover allowlist is
    // one you want to find in the same place every time. There it is a switch;
    // here it is unconditional, because mdxmixer has no stored row order for
    // it to override and nothing to trade against.
    //
    // Only a device that is actually CONNECTED is pinned, and `active` is the
    // test — NOT `present`, which means paired and is true of every headset in
    // a drawer. Getting that wrong put all seven of Shane's pairings, six of
    // them switched off, above everything else: "I see a lot of disconnected
    // devices at the top of the list". mdx12 never has to make the choice —
    // its provider publishes a fader only for an active endpoint, so a dormant
    // pairing has no row to pin.
    //
    // Matched by id or by the Windows name the entry stored, the same pair
    // mdx12 matches on: an entry added while its device was switched off has
    // no id, and the name is the only thread back to it.
    //
    // `autoPinned`, not `pinned`: pinning here is a consequence of the
    // allowlist, while `pinned` is something the user did on purpose and is
    // what the "[pinned]" tag and the Unpin menu item speak for. Folding the
    // two together labelled seven rows as pinned that nobody had pinned, and
    // offered an Unpin that would not have unpinned them.
    {
        const auto& allow = cfg.personalFailover.allow;
        for (auto& d : levels) {
            if (!d.active || d.pinned) continue;
            // The endpoint we are playing to, first — it is the one he means
            // by "the connected headset", and it is hoisted whether or not
            // anyone ever put it on the allowlist.
            if (!personalRouteId.empty() && d.id == personalRouteId) {
                d.autoPinned = true;
                continue;
            }
            // SameDevice, the same rule the devices tab draws with and the
            // failover watcher commits on. It was id-or-name here, id-only
            // there, and name-or-id in the engine: three readings of one list.
            const DeviceRefKeys dev{ d.id, d.name,
                                     AliasOrNone(d.displayName, d.name), d.btAddress };
            for (const auto& e : allow) {
                const DeviceRefKeys ref{
                    e.id, e.name,
                    AliasOrNone(AliasFor(names, e.id, L"", e.name), e.name), e.btAddress };
                if (SameDevice(ref, dev)) { d.autoPinned = true; break; }
            }
        }
    }
    // Sorted here rather than in each surface, so the mixer list, the failover
    // list and anything reading MDXM_STATE all agree on the order.
    SortDevices(levels);
}

// Teach every stored device reference what the devices in front of it know.
//
// "I would prefer to not have to guess every time I change out the bluetooth
// adapter hoping for a better and more stable connection." A new dongle
// re-pairs every headset, so every stored endpoint id and ContainerId dies in
// one afternoon and Windows renames the devices on top of that. The one anchor
// that survives is the headset's own Bluetooth address -- so each entry picks
// it up the first time it is seen beside its device, and from then on the swap
// costs nothing.
//
// NOT a cleanup the user has to run. It writes only better anchors onto
// entries that already match, never removes one and never touches an alias,
// which is why it is safe to do unprompted: "I never touched the remove
// unknown button as I have enough of a time keeping things solid as
// development changes happen".
//
// Runs at startup and on the debounced device change -- the two moments the
// device set is new -- and not on the sweep, which would ask to write the
// config file ten times a second.
void AppController::HealStoredDeviceRefs(const wchar_t* why) {
    auto levels = ListEndpointVolumes();
    if (levels.empty()) return;
    MixerConfig& live = m_store.Get();
    auto keysFor = [&](const DeviceLevel& d) {
        const std::wstring alias =
            DisplayName(live.deviceNames, d.id, d.containerId, d.name, d.btAddress);
        return DeviceRefKeys{ d.id, d.name, AliasOrNone(alias, d.name), d.btAddress };
    };
    std::vector<DeviceRefKeys> devs;
    devs.reserve(levels.size());
    for (const auto& d : levels) devs.push_back(keysFor(d));

    int healed = 0;
    m_store.Mutate([&](MixerConfig& c) {
        auto heal = [&](DeviceRef& e) {
            if (e.id == kFollowFailover) return;   // a mode, not a device
            DeviceRefKeys ref{ e.id, e.name,
                               AliasOrNone(AliasFor(c.deviceNames, e.id, L"", e.name), e.name),
                               e.btAddress };
            for (const auto& dev : devs) {
                if (!SameDevice(ref, dev)) continue;
                if (HealDeviceRef(ref, dev)) {
                    e.id = ref.id;
                    e.name = ref.name;
                    e.btAddress = ref.btAddress;
                    ++healed;
                }
                return;
            }
        };
        for (auto& e : c.personalFailover.allow) heal(e);
        heal(c.personalOutput);
    });
    if (healed > 0)
        Log(2, L"device references healed (%s): %d entr%s now carry current anchors",
            why, healed, healed == 1 ? L"y" : L"ies");
}

std::vector<DeviceLevel> AppController::GetDeviceLevels() {
    // Hidden endpoints are swept for their identity alone, which on this
    // machine is 16 VBMatrix rows nobody looks at out of 53 endpoints. The
    // set comes from the LAST sweep, because "hidden" is resolved from the
    // name and container this sweep produces (sweep_selection.h): a device
    // hidden a moment ago is read in full once more and then goes quiet.
    auto levels = ListEndpointVolumes(m_identityOnlyIds);
    // The LIVE route, from the engine — not cfg.personalOutput, which is what
    // was asked for. Failover can have moved us somewhere else since, and the
    // row worth hoisting is the one actually carrying the audio.
    ApplyDeviceView(m_store.Get(), levels, m_engine.GetDiag().personalDevice);
    HoldDevicePeaks(levels);
    return levels;
}

// Turn each sweep's instantaneous reading into a peak that stays up for
// kPeakHoldMs.
//
// Called from GetDeviceLevels, which is every path that produces endpoint
// peaks: the mixer tab's 250 ms timer, MDXM_STATE, and the subscription push.
// Whichever of those is running keeps the hold fed, and the elapsed time is
// measured rather than assumed, so a 250 ms tick and a one-off request a
// minute apart both decay correctly.
//
// GetTickCount rather than GetTickCount64 to match the rest of this file; the
// 49-day wrap is handled by the unsigned subtraction, which is correct across
// it.
void AppController::HoldDevicePeaks(std::vector<DeviceLevel>& levels) {
    const unsigned now = (unsigned)GetTickCount();
    const unsigned elapsed = m_lastSweepMs ? (now - m_lastSweepMs) : 0;
    m_lastSweepMs = now;

    for (auto& d : levels) {
        if (d.id.empty()) continue;
        DevicePeak& e = m_devPeaks[d.id];
        e.hold.Configure(kPeakHoldMs);
        e.lastSweepMs = now;
        if (d.peak < 0.0f) {
            // The sweep could not read a meter on this endpoint -- unplugged,
            // or IAudioMeterInformation refused. Forget the held value instead
            // of holding it: a peak from before a headset switched off,
            // presented as current, points at a device that is not there.
            e.hold.MarkUnknown();
        } else {
            e.hold.Push(d.peak, elapsed);
        }
        d.peak = e.hold.Value();
    }

    // Endpoints that have stopped appearing in the sweep. Dropped rather than
    // kept, so the map does not grow for the life of the process on a machine
    // that re-pairs headsets daily and mints a new endpoint id each time.
    for (auto it = m_devPeaks.begin(); it != m_devPeaks.end(); ) {
        if (it->second.lastSweepMs != now) it = m_devPeaks.erase(it);
        else ++it;
    }

    // While the whole list with its names and its held peaks is in hand, do
    // the Sonar join. GetChannels reads the result; see m_sonarPeaks.
    m_sonarPeaks.clear();
    m_sonarPeaksNow.clear();
    for (const wchar_t* key : { L"aux", L"media", L"game",
                                L"chatRender", L"chatCapture" }) {
        m_sonarPeaks[key] = SonarChannelPeak(levels, key);
        // The unheld figure too: the Mixer tab's meters fall at their own rate
        // rather than inheriting the wire's 1.5 s hold.
        m_sonarPeaksNow[key] = SonarChannelPeakNow(levels, key);
    }
    m_sonarPeaksMs = now;

    // Which endpoints the NEXT sweep may read identity alone for. Computed
    // here because this is where `hidden` has just been resolved, and kept as
    // ids because that is what the sweep can test cheaply.
    //
    // The Sonar prefix is passed in rather than known by the device layer: the
    // meters above are joined onto those endpoints by name, so hiding one in
    // the device list must not silently kill a channel meter in the mixer.
    {
        std::vector<std::wstring> routes;
        const DiagState diag = m_engine.GetDiag();
        if (!diag.personalDevice.empty()) routes.push_back(diag.personalDevice);
        for (const auto& c : m_store.Get().channels) {
            if (!c.cable.render.id.empty()) routes.push_back(c.cable.render.id);
            if (!c.cable.capture.id.empty()) routes.push_back(c.cable.capture.id);
        }
        m_identityOnlyIds =
            IdentityOnlyIds(levels, routes, { L"SteelSeries Sonar - " });
    }

    m_lastLevels = levels;
}

// SonarChannelPeak, which does the join, lives in routing/sonar_api.cpp beside
// SonarWritePath — it is pure, it is about Sonar's vocabulary rather than this
// controller's, and it needs testing without a live GG.

// ── Failover ─────────────────────────────────────────────────────────────
//
// Config is the single copy: the engine's watcher reads it on every tick, so
// a write here is live on the next one without restarting anything.

bool AppController::SetFeedEnabled(bool on, std::wstring* err) {
    return m_engine.SetFeedEnabled(on, err);
}
bool AppController::FeedEnabled() { return m_engine.FeedEnabled(); }
uint32_t AppController::FeedRate() { return m_engine.FeedRate(); }

// ── The VBAN stream server ───────────────────────────────────────────────

void AppController::ApplyVbanConfig() {
    const VbanConfig v = m_store.Get().vban;
    // The PC's own share of what the listener hears late is cushion + one packet
    // + the sender's wake. The BASE is just the parts the server cannot see --
    // the engine's cushion and a ~2 ms wake margin; the server adds the real
    // packet duration itself (it knows the format and rate). Folding a packet
    // estimate in here too, as this first did, double-counted it by ~5 ms.
    m_vban.SetSrcLatencyBase(m_engine.CushionMs() + 2);
    if (!v.enabled) {
        m_vban.Stop();
        m_engine.SetVbanSink(false, v.sourceStreaming);
        return;
    }
    if (m_vban.Running()) {
        m_vban.UpdateConfig(v);
    } else {
        const HWND hwnd = m_window.Hwnd();
        VbanServer::Callbacks cb;
        // A record from the network goes through the SAME marshalling as one from
        // the pipe, so control stays single-threaded whatever carried it.
        cb.dispatch = [hwnd](const std::wstring& msg) {
            return DispatchToUi(hwnd, msg, nullptr, nullptr);
        };
        // Both of these arrive on the receive thread, so they only POST.
        cb.onAuthPending = [hwnd](const std::wstring& id, const std::wstring& name) {
            // Heap-owned, deleted by the handler: the receive thread must not
            // wait for a dialog the user may leave on screen for minutes.
            auto* text = new std::wstring(id + L"\n" + name);
            if (!PostMessageW(hwnd, MainWindow::kVbanAuthMsg, 0, (LPARAM)text))
                delete text;
        };
        cb.onAuthorized = [hwnd](const std::wstring& id, const std::wstring& name) {
            auto* text = new std::wstring(id + L"\n" + name);
            if (!PostMessageW(hwnd, MainWindow::kVbanAuthorizedMsg, 0, (LPARAM)text))
                delete text;
        };
        std::wstring err;
        if (!m_vban.Start(v, &m_engine.VbanRing(), m_engine.MixRate(), cb, &err)) {
            // Logged and carried: a mixer whose network stream did not start is
            // still a working mixer, and the reason is in MDXM_VBANSTATE for
            // anyone who asks.
            Log(1, L"vban did not start: %s", err.c_str());
            m_engine.SetVbanSink(false, v.sourceStreaming);
            return;
        }
    }
    TickVban();
}

void AppController::TickVban() {
    // The receive socket died for a reason that was not us (a resume that left
    // it dead, an adapter pulled). The server cannot rebuild itself -- Stop()
    // keys its thread joins on the still-true running flag -- so the control
    // layer does it: a full Stop/Start, which the config re-apply performs.
    if (m_vban.Running() && m_vban.Failed()) {
        Log(1, L"vban: receive socket failed; rebuilding the server");
        m_vban.Stop();
        ApplyVbanConfig();
        return;
    }
    if (!m_vban.Running()) {
        if (m_engine.VbanSinkOn()) m_engine.SetVbanSink(false, false);
        return;
    }
    // The clock master can change rate under us (a different personal output
    // device), and the rate is in every packet header -- a sender left on the old
    // one would have the far end play at the wrong speed, which sounds like a
    // fault in the music rather than a configuration problem. Cheap: the server
    // returns at once when the rate has not moved.
    m_vban.UpdateMixRate(m_engine.MixRate());
    // THE SINK FOLLOWS DEMAND, NOT CONFIG. An enabled listener with nobody
    // connected must not cost a copy per audio block -- that is the same "do not
    // publish what nobody asked for" rule the shared-memory feed follows, and the
    // reason the engine's demand bit cannot be trusted to do it (mix_demand.h).
    //
    // Pushed UNCONDITIONALLY, not only on a demand change: the source
    // (personal/streaming) can switch while the stream is up, and SetVbanSink
    // stores it on every call (the engine early-returns when nothing changed, so
    // this is cheap). Gating on `wanted != VbanSinkOn()` alone let a live
    // `source=streaming` be acknowledged while the wire kept carrying the
    // personal sum until every listener left and came back.
    m_engine.SetVbanSink(m_vban.Wanted(), m_store.Get().vban.sourceStreaming);
}

VbanStatus AppController::GetVbanStatus() {
    VbanStatus s = m_vban.Status();
    // When the server is stopped its own snapshot cannot say what the config
    // wants, and a reader asking "is this on?" means the setting, not the socket.
    if (!m_vban.Running()) {
        const VbanConfig& v = m_store.Get().vban;
        s.on = v.enabled;
        s.port = v.port;
        s.name = v.streamName;
        s.sourceStreaming = v.sourceStreaming;
        s.formatFloat32 = v.formatFloat32;
        s.gainPercent = v.gainPercent;
        s.fps = v.frames.fps;
        s.open = v.openSubscribe;
        s.always = v.alwaysStream;
        s.alwaysFrames = v.alwaysFrames;
        s.target = v.alwaysStreamTarget;
    }
    return s;
}

std::vector<VbanPeerRow> AppController::GetVbanPeers() { return m_vban.Peers(); }

bool AppController::RevokeVbanDevice(const std::wstring& deviceId) {
    bool removed = false;
    m_store.Mutate([&](MixerConfig& c) {
        removed = RemoveAuthorizedDevice(c.vban.authorizedDevices, deviceId);
    });
    // Told to the server directly as well as removed from config: a session it is
    // no longer entitled to must end now rather than at the next config push, and
    // the server's own approval memory has to forget it too or the next AUTH would
    // still succeed from that.
    //
    // FORGET rather than deny. A revoke is not the same statement as a refusal:
    // the device should be able to ask again -- the next AUTH gets `pending` and
    // raises the prompt -- whereas a denial is terminal until restart.
    m_vban.ForgetDevice(deviceId);
    if (removed) Log(2, L"vban: revoked access for '%s'", deviceId.c_str());
    return removed;
}

bool AppController::SetVbanOption(const std::wstring& key, const std::wstring& value,
                                  std::wstring* err) {
    const auto fail = [err](const wchar_t* why) {
        if (err) *err = why;
        return false;
    };
    const auto parseBool = [](const std::wstring& v, bool* out) {
        if (v == L"0") { *out = false; return true; }
        if (v == L"1") { *out = true; return true; }
        return false;
    };
    // '|' and '=' are the record's own delimiters: a value carrying either one
    // corrupts every MDXM_VBANSTATE that echoes it, for every reader. The free-
    // text fields (name, target) are the only ones that could, so they are
    // refused here.
    const auto hasDelimiter = [](const std::wstring& s) {
        return s.find(L'|') != std::wstring::npos || s.find(L'=') != std::wstring::npos;
    };
    // Edited as a copy and committed only once every field has been accepted, so
    // a rejected value leaves nothing half-applied.
    VbanConfig v = m_store.Get().vban;
    bool b = false;
    if (key == L"on") {
        if (!parseBool(value, &b)) return fail(L"on wants 0 or 1");
        v.enabled = b;
    } else if (key == L"port") {
        const int p = _wtoi(value.c_str());
        if (p < 1 || p > 65535) return fail(L"port wants 1..65535");
        v.port = p;
    } else if (key == L"name") {
        // 16 bytes is the field width on the wire, not a style choice.
        if (value.empty() || value.size() > 16) return fail(L"name wants 1..16 characters");
        if (hasDelimiter(value)) return fail(L"name cannot contain | or =");
        v.streamName = value;
    } else if (key == L"source") {
        if (value != L"personal" && value != L"streaming")
            return fail(L"source wants personal or streaming");
        v.sourceStreaming = value == L"streaming";
    } else if (key == L"format") {
        if (value != L"i16" && value != L"f32") return fail(L"format wants i16 or f32");
        v.formatFloat32 = value == L"f32";
    } else if (key == L"gain") {
        const int g = _wtoi(value.c_str());
        if (g < 0 || g > 6400) return fail(L"gain wants 0..6400 percent");
        v.gainPercent = g;
    } else if (key == L"fps") {
        const double f = _wtof(value.c_str());
        if (f < 0.2 || f > 10.0) return fail(L"fps wants 0.2..10");
        v.frames.fps = f;
    } else if (key == L"open") {
        if (!parseBool(value, &b)) return fail(L"open wants 0 or 1");
        v.openSubscribe = b;
    } else if (key == L"always") {
        if (!parseBool(value, &b)) return fail(L"always wants 0 or 1");
        v.alwaysStream = b;
    } else if (key == L"alwaysframes") {
        if (!parseBool(value, &b)) return fail(L"alwaysframes wants 0 or 1");
        v.alwaysFrames = b;
    } else if (key == L"target") {
        // Shape is not validated here -- the server parses it, says so in
        // lastError when it cannot, and treats a bad one as absent, so a typo
        // does not lock the user out of fixing it. But the record delimiters are
        // refused, because a target carrying one corrupts MDXM_VBANSTATE.
        if (hasDelimiter(value)) return fail(L"target cannot contain | or =");
        v.alwaysStreamTarget = value;
    } else if (key == L"pin") {
        v.pin = value;
    } else {
        if (err) *err = L"unknown vban option: " + key;
        return false;
    }
    m_store.Mutate([&](MixerConfig& c) { c.vban = v; });
    // on/port rebind the socket. Apply them DEFERRED -- posted back to the UI
    // pump -- because a network-carried MDXM_VBAN|port= runs on the UI thread
    // while the VBAN receive thread is blocked in DispatchToUi waiting for THIS
    // handler to return; applying inline, ApplyVbanConfig -> Stop() would join
    // that blocked thread (3 s timeout, a leaked handle, and for a port change a
    // second receive thread left on the same socket). The post runs the rebuild
    // after this returns and the receive thread is unblocked, and the reply still
    // leaves on the old socket first. Every other key applies inline.
    if (key == L"on" || key == L"port")
        PostMessageW(m_window.Hwnd(), MainWindow::kVbanApplyMsg, 0, 0);
    else
        ApplyVbanConfig();
    return true;
}

int AppController::GetCushionMs() { return m_engine.CushionMs(); }
int AppController::GetCushionHeadroomPercent() { return m_engine.CushionHeadroomPercent(); }
int AppController::GetCushionFlatMs() { return m_engine.CushionFlatMs(); }

// Live AND persisted: the engine re-cushions now, and the values survive a
// restart so an experiment that lands on good numbers does not have to be
// repeated (fj#12).
bool AppController::SetCushion(int ms, int headroomPercent, int flatMs) {
    if (!m_engine.SetCushion(ms, headroomPercent, flatMs)) return false;
    m_store.Mutate([&](MixerConfig& c) {
        c.cushionMs = m_engine.CushionMs();
        c.cushionHeadroomPercent = m_engine.CushionHeadroomPercent();
        c.cushionFlatMs = m_engine.CushionFlatMs();
    });
    BroadcastState();
    return true;
}

FailoverConfig AppController::GetFailover() { return m_store.Get().personalFailover; }

// The rule comes from config; what the watcher is doing with it comes from the
// engine, which is the only thing that has it (fj#2 §3).
FailoverStatus AppController::GetFailoverStatus() { return m_engine.GetFailoverStatus(); }

bool AppController::SetFailoverArmed(bool armed) {
    m_store.Mutate([&](MixerConfig& c) { c.personalFailover.armed = armed; });
    Log(2, L"failover: %s", armed ? L"armed" : L"disarmed");
    BroadcastState();
    return true;
}

bool AppController::SetFailoverTiming(int stabilitySec, int dwellSec, int minGapSec) {
    // Clamped to the same bounds the Devices tab allows. A zero stability
    // window would commit on the first tick a device flickers, which is the
    // flapping this watcher exists to prevent.
    m_store.Mutate([&](MixerConfig& c) {
        c.personalFailover.stabilitySec = std::clamp(stabilitySec, 1, 120);
        c.personalFailover.dwellSec     = std::clamp(dwellSec, 1, 600);
        c.personalFailover.minGapSec    = std::clamp(minGapSec, 1, 600);
    });
    Log(2, L"failover timing: steady %ds, dwell %ds, gap %ds",
        stabilitySec, dwellSec, minGapSec);
    BroadcastState();
    return true;
}

bool AppController::SetFailoverAllow(const std::vector<DeviceRef>& allow) {
    // Replaced wholesale, including with an empty list -- an armed rule with
    // nothing allowed is a legitimate state (it simply never commits) and is
    // how a front-end clears the list.
    m_store.Mutate([&](MixerConfig& c) { c.personalFailover.allow = allow; });
    Log(2, L"failover allowlist: %zu entr%s", allow.size(),
        allow.size() == 1 ? L"y" : L"ies");
    BroadcastState();
    return true;
}

// ── Hotkeys ──────────────────────────────────────────────────────────────

std::vector<HotkeyBinding> AppController::GetHotkeys() { return m_store.Get().hotkeys; }

bool AppController::SetHotkeys(const std::vector<HotkeyBinding>& bindings) {
    m_store.Mutate([&](MixerConfig& c) { c.hotkeys = bindings; });
    // The window that owns the keys re-registers; it is the one on the right
    // thread for RegisterHotKey, which this is not.
    if (m_uiCtx.hotkeysChanged) m_uiCtx.hotkeysChanged();
    Log(2, L"hotkeys: %zu binding(s) saved", bindings.size());
    return true;
}

int AppController::GetVolumeStep() { return m_store.Get().volumeStepPercent; }

bool AppController::SetVolumeStep(int percent) {
    if (percent < 1) percent = 1;
    if (percent > 50) percent = 50;
    m_store.Mutate([&](MixerConfig& c) { c.volumeStepPercent = percent; });
    return true;
}

std::vector<std::pair<std::wstring, std::wstring>> AppController::GetHotkeyTargets() {
    std::vector<std::pair<std::wstring, std::wstring>> out;
    // Channel faders first, both halves of each, then the devices — the same
    // order the Mixer tab shows them in, so the list reads the same way.
    for (const auto& c : m_engine.GetChannelStates()) {
        out.push_back({ ChannelTargetKey(c.id, true),  c.name + L" [P]" });
        out.push_back({ ChannelTargetKey(c.id, false), c.name + L" [S]" });
    }
    for (const auto& d : GetDeviceLevels()) {
        if (d.hidden) continue;   // filed away on the Mixer tab: not offered here
        out.push_back({ DeviceTargetKey(d.id),
                        d.displayName + (d.isRender ? L"" : L" (in)") });
    }
    return out;
}

bool AppController::TriggerHotkey(const std::wstring& bindingId) {
    const HotkeyBinding* b = nullptr;
    for (const auto& h : m_store.Get().hotkeys)
        if (h.id == bindingId) { b = &h; break; }
    if (!b) return false;

    if (b->action == HotkeyAction::ShowWindow) {
        ShowUi();
        return true;
    }

    // Read every target as it stands right now. A group's members are only
    // guaranteed to move together if they are all read before any of them is
    // written.
    auto channels = m_engine.GetChannelStates();
    auto devices = GetDeviceLevels();
    std::vector<TargetLevel> levels;
    for (const std::wstring& key : b->targets) {
        bool isDevice = false, personal = true;
        std::wstring id;
        if (!ParseTargetKey(key, &isDevice, &id, &personal)) continue;
        TargetLevel t;
        t.key = key;
        if (isDevice) {
            for (const auto& d : devices)
                if (d.id == id) { t.vol = d.vol; t.muted = d.mute; levels.push_back(t); break; }
        } else {
            for (const auto& c : channels)
                if (c.id == id) {
                    t.vol = personal ? c.pvol : c.svol;
                    t.muted = personal ? c.pmute : c.smute;
                    levels.push_back(t);
                    break;
                }
        }
    }
    if (levels.empty()) {
        Log(2, L"hotkey '%s': nothing to move", b->label.c_str());
        return false;
    }

    auto apply = [&](const std::wstring& key, float vol, bool setMute, bool mute) {
        bool isDevice = false, personal = true;
        std::wstring id;
        if (!ParseTargetKey(key, &isDevice, &id, &personal)) return;
        if (isDevice) {
            if (setMute) SetDeviceMute(id, mute);
            else SetDeviceVolume(id, vol);
        } else {
            Mix m = personal ? Mix::Personal : Mix::Streaming;
            if (setMute) SetMute(id, m, mute);
            else SetVolume(id, m, vol);
        }
    };

    if (b->action == HotkeyAction::MuteToggle) {
        const bool mute = MuteAllDecision(levels);
        for (const auto& t : levels) apply(t.key, 0.0f, true, mute);
        Log(2, L"hotkey '%s': %s %zu fader(s)", b->label.c_str(),
            mute ? L"muted" : L"unmuted", levels.size());
    } else {
        const bool up = (b->action == HotkeyAction::VolumeUp);
        // The binding's OWN step when it has one, the default otherwise.
        for (const auto& nv : StepVolumes(levels,
                                          StepForBinding(*b, m_store.Get().volumeStepPercent),
                                          up))
            apply(nv.first, nv.second, false, false);
        Log(2, L"hotkey '%s': %s %zu fader(s)", b->label.c_str(),
            up ? L"raised" : L"lowered", levels.size());
    }
    BroadcastState();
    return true;
}

bool AppController::AddChannel(const std::wstring& name, const std::wstring& sourceEndpointId,
                               std::wstring* err) {
    auto fail = [err](const wchar_t* why) { if (err) *err = why; return false; };
    if (name.empty() || sourceEndpointId.empty()) return fail(L"Pick a source device and a name.");

    const EndpointInfo* src = nullptr;
    auto eps = EnumerateEndpoints();
    for (const auto& e : eps) if (e.id == sourceEndpointId) { src = &e; break; }
    if (!src) return fail(L"That device is no longer there.");

    // The mix must never be tapped from the endpoint it is played to. On this
    // machine the default output is a Sonar channel, so this is the mistake
    // one click away — refused outright rather than warned about, because the
    // symptom is a howl at whatever volume the headset happens to be at.
    //
    // Three candidates, because the engine may not be running yet and an unset
    // personal output follows the system default: the device the engine has
    // open, the one configured, and the default it would fall back to. Any
    // match refuses.
    std::wstring live = m_engine.GetDiag().personalDevice;
    std::wstring configured = m_store.Get().personalOutput.id;
    std::wstring fallback = configured.empty() ? DefaultRenderEndpointId() : std::wstring();
    if (WouldFeedBack(sourceEndpointId, live) || WouldFeedBack(sourceEndpointId, configured) ||
        WouldFeedBack(sourceEndpointId, fallback))
        return fail(L"That is the device the personal mix plays to, so tapping it would "
                    L"feed back. Pick a different personal output on the Devices tab first.");
    for (const auto& c : m_store.Get().channels)
        if (_wcsicmp(c.cable.capture.id.c_str(), sourceEndpointId.c_str()) == 0)
            return fail(L"That device is already a channel.");

    ChannelConfig ch;
    ch.id = MakeChannelId(m_store.Get().channels, name);
    ch.name = name;
    ch.cable.capture = { src->id, src->name };   // render source => loopback tap
    m_store.Mutate([&](MixerConfig& c) { c.channels.push_back(ch); });
    Log(2, L"channel added: %s <- %s", ch.name.c_str(), src->name.c_str());
    RestartEngine();
    return true;
}

bool AppController::RemoveChannel(const std::wstring& channelId) {
    bool found = false;
    m_store.Mutate([&](MixerConfig& c) {
        for (size_t i = 0; i < c.channels.size(); ++i) {
            if (c.channels[i].id == channelId) {
                c.channels.erase(c.channels.begin() + (ptrdiff_t)i);
                found = true;
                break;
            }
        }
    });
    if (!found) return false;
    Log(2, L"channel removed: %s", channelId.c_str());
    RestartEngine();
    return true;
}

bool AppController::SetDeviceView(const std::wstring& endpointId,
                                  const std::wstring& containerId,
                                  const std::wstring& windowsName,
                                  bool hidden, bool pinned) {
    if (endpointId.empty()) return false;
    m_store.Mutate([&](MixerConfig& c) {
        SetView(c.deviceNames, endpointId, containerId, windowsName, { hidden, pinned },
                BluetoothAddressOf(endpointId));
    });
    Log(2, L"device view: %s hidden=%d pinned=%d", windowsName.c_str(), (int)hidden, (int)pinned);
    // WHICH rows exist has changed, so the window is rebuilt here rather than
    // by whoever called — which is what makes hiding work in both directions
    // (fj#8). A hide arriving over the pipe has to file the device away in
    // mdxmixer's own window, and the 250 ms refresh tick cannot do it: it
    // updates the values in rows that already exist and can neither add a row
    // nor remove one.
    //
    // The Mixer tab's own right-click Hide used to rebuild itself and nothing
    // else did, so there were two paths to the same state. Now there is one,
    // and it is the same principle the tray menu and the Options tab already
    // share for "Show in taskbar": one command, one handler, no second way for
    // the two to disagree.
    m_window.RebuildPages();
    BroadcastState();
    return true;
}

bool AppController::SetDeviceAlias(const std::wstring& endpointId,
                                   const std::wstring& containerId,
                                   const std::wstring& windowsName,
                                   const std::wstring& alias) {
    if (endpointId.empty()) return false;
    m_store.Mutate([&](MixerConfig& c) {
        SetAlias(c.deviceNames, endpointId, containerId, windowsName, alias,
                 BluetoothAddressOf(endpointId));
    });
    Log(2, L"device name: %s -> '%s'", windowsName.c_str(), alias.c_str());
    BroadcastState();
    return true;
}

bool AppController::SetDeviceVolume(const std::wstring& endpointId, float vol01) {
    return SetEndpointVolume(endpointId, vol01);
}

bool AppController::SetDeviceMute(const std::wstring& endpointId, bool mute) {
    return SetEndpointMute(endpointId, mute);
}

bool AppController::CaptureUi(const std::wstring& path, const std::wstring& window) {
    HWND target = m_window.Hwnd();
    if (window == L"hotkeys") target = m_window.HotkeysHwnd();
    else if (window == L"overlay") target = m_window.OverlayHwnd();
    if (!target) { Log(1, L"capture: no %s window open", window.c_str()); return false; }
    bool ok = CaptureWindowToPng(target, path);
    Log(2, L"capture %s: %s", ok ? L"saved" : L"FAILED", path.c_str());
    return ok;
}

std::wstring AppController::GetHotkeyStatus(const std::wstring& bindingId) {
    return m_window.HotkeyStatus(bindingId);
}

// Tab order is the one in MainWindow::Create: Mixer, Routing, EQ, Devices,
// Options. Matched case-insensitively on the name a person would type rather
// than on an index, because an index is a thing that silently means something
// else the day a tab is inserted.
bool AppController::ShowTab(const std::wstring& name) {
    std::wstring want;
    for (wchar_t c : name) want += (wchar_t)towlower(c);
    static const wchar_t* kTabs[] = { L"mixer", L"routing", L"eq", L"devices",
                                      L"vban", L"options" };
    for (int i = 0; i < (int)(sizeof kTabs / sizeof kTabs[0]); ++i)
        if (want == kTabs[i]) {
            m_window.SelectTab(i);
            return true;
        }
    return false;
}

bool AppController::ShowHotkeysUi() {
    PostMessageW(m_window.Hwnd(), MainWindow::kShowHotkeysMsg, 0, 0);
    return true;
}

bool AppController::ExitApp() {
    Log(2, L"exit requested over IPC");
    // Posted, not called: this arrives on a pipe thread, and the message loop
    // must unwind on its own thread so Stop() runs where everything was built.
    PostMessageW(m_window.Hwnd(), MainWindow::kExitMsg, 0, 0);
    return true;
}

bool AppController::ShowUi() {
    // May arrive from a pipe thread (second instance); posting is thread-safe.
    PostMessageW(m_window.Hwnd(), MainWindow::kShowMsg, 0, 0);
    return true;
}

} // namespace mdxm
