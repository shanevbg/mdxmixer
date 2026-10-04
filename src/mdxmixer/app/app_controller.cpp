#include "app_controller.h"
#include "app/autostart.h"
#include "app/log.h"
#include "app/channel_setup.h"
#include "app/hotkeys.h"
#include "device/endpoints.h"
#include "device/endpoint_volume.h"
#include "device/device_identity.h"
#include "device/device_order.h"
#include "ipc/protocol.h"
#include "routing/sessions.h"
#include "routing/reconcile.h"
#include "routing/default_endpoint.h"
#include "ui/window_capture.h"
#include <algorithm>

namespace mdxm {

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
    m_uiCtx.onTick250ms = [this] { PushPeaks(); };
    m_uiCtx.onSuspend = [this] { m_engine.OnSuspend(); };
    m_uiCtx.onResume = [this] {
        m_engine.OnResume();
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
    m_uiCtx.onDeviceChangeDebounced = [this] {
        m_engine.OnDeviceSetChanged();
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
    m_uiCtx.dispatchIpc = [this](const std::wstring& msg, bool* sub) {
        return HandleProtocolMessage(msg, *this, sub);
    };
    m_uiCtx.routingAvailable = [this] { return m_policy.IsAvailable(); };
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
    bool pipeOk = m_pipe.Start([hwnd](const std::wstring& msg, bool* sub) {
        // Pipe client thread: marshal onto the UI thread so control stays
        // single-threaded. SendMessageTimeout, not SendMessage: a UI thread that
        // is busy, modal, or shutting down must never park a pipe thread
        // indefinitely (ABORTIFHUNG returns rather than waiting on a hung queue).
        MainWindow::IpcRequest req{ &msg, sub, {} };
        DWORD_PTR unused = 0;
        if (!SendMessageTimeoutW(hwnd, MainWindow::kIpcMsg, (WPARAM)&req, 0,
                                 SMTO_ABORTIFHUNG | SMTO_NORMAL, 5000, &unused))
            return std::vector<std::wstring>{ L"MDXM_ERR|msg=busy" };
        return req.replies;
    }, &pipeErr);
    if (!pipeOk) Log(1, L"pipe server: %s", pipeErr.c_str());

    // Signal-only (fj#401): the notification callback only posts.
    m_watcher.Start([hwnd] { PostMessageW(hwnd, MainWindow::kDeviceChangeMsg, 0, 0); });

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
    m_pipe.Stop();
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
void AppController::PushPeaks() {
    if (!m_pipe.HasSubscribers()) return;
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
    m_pipe.Broadcast(PeakRecord(GetChannels(), levels));
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

bool AppController::AssignApp(const std::wstring& exePath, const std::wstring& chOrDash) {
    bool clearing = (chOrDash == L"-");
    std::wstring targetEndpoint;
    if (!clearing) {
        const ChannelConfig* target = nullptr;
        for (const auto& c : m_store.Get().channels)
            if (c.id == chOrDash) { target = &c; break; }
        if (!target) return false;
        // Apps are routed INTO the cable: its render side when configured; a
        // render endpoint bound as the channel source (loopback tap) also works.
        auto eps = EnumerateEndpoints();
        const EndpointInfo* ep = MatchBinding(eps, target->cable.render);
        if (!ep) {
            ep = MatchBinding(eps, target->cable.capture);
            if (ep && !ep->isRender) ep = nullptr;
        }
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
        const EndpointInfo* ep = MatchBinding(eps, c.cable.render);
        if (!ep) {
            ep = MatchBinding(eps, c.cable.capture);
            if (ep && !ep->isRender) ep = nullptr;
        }
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
            for (const auto& e : allow)
                if ((!e.id.empty() && e.id == d.id) ||
                    (!e.name.empty() && e.name == d.name)) { d.autoPinned = true; break; }
        }
    }
    // Sorted here rather than in each surface, so the mixer list, the failover
    // list and anything reading MDXM_STATE all agree on the order.
    SortDevices(levels);
}

std::vector<DeviceLevel> AppController::GetDeviceLevels() {
    auto levels = ListEndpointVolumes();
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
    for (const wchar_t* key : { L"aux", L"media", L"game",
                                L"chatRender", L"chatCapture" })
        m_sonarPeaks[key] = SonarChannelPeak(levels, key);
    m_sonarPeaksMs = now;

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

FailoverConfig AppController::GetFailover() { return m_store.Get().personalFailover; }

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
    HWND target = (window == L"hotkeys") ? m_window.HotkeysHwnd() : m_window.Hwnd();
    if (!target) { Log(1, L"capture: no %s window open", window.c_str()); return false; }
    bool ok = CaptureWindowToPng(target, path);
    Log(2, L"capture %s: %s", ok ? L"saved" : L"FAILED", path.c_str());
    return ok;
}

std::wstring AppController::GetHotkeyStatus(const std::wstring& bindingId) {
    return m_window.HotkeyStatus(bindingId);
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
