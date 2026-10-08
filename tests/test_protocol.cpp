#include "test_framework.h"
#include "ipc/protocol.h"
#include <algorithm>

using namespace mdxm;

namespace {
struct FakeControl : IMixerControl {
    float lastVol = -1; Mix lastMix = Mix::Personal; std::wstring lastCh;
    bool eqOn = false; size_t eqBand = 99; double eqF = 0, eqG = 0, eqQ = 0;
    std::wstring assignedExe, assignedCh, route;
    bool showCalled = false;
    std::vector<ChannelState> GetChannels() override {
        // Two channels, so both halves of the peak contract are on the wire:
        // `game` is metered, `dead` cannot be and must say -1 rather than 0.
        ChannelState game{L"game", L"Game", true, 0.85f, false, 1.0f, false, true};
        game.peak = 0.42f;
        ChannelState dead{L"dead", L"Dead", false, 1.0f, false, 1.0f, false, false};
        dead.peak = kPeakUnknown;
        return { game, dead };
    }
    std::vector<std::pair<std::wstring, std::wstring>> GetRoutes() override {
        return {{L"personal", L"{hp-guid}"}};
    }
    std::vector<std::tuple<std::wstring, std::wstring, bool, bool>> GetDevices() override {
        return {{L"{hp-guid}", L"Headphones", true, true}};
    }
    bool SetVolume(const std::wstring& ch, Mix m, float vol) override {
        lastCh = ch; lastMix = m; lastVol = vol; return ch == L"game";
    }
    bool SetMute(const std::wstring&, Mix, bool) override { return true; }
    bool SetEqBand(const std::wstring&, size_t b, double f, double g, double q) override {
        eqBand = b; eqF = f; eqG = g; eqQ = q; return true;
    }
    bool EnableEq(const std::wstring&, bool on) override { eqOn = on; return true; }
    bool AssignApp(const std::wstring& exe, const std::wstring& ch) override {
        assignedExe = exe; assignedCh = ch; return true;
    }
    std::vector<AppRouteState> GetAppRoutes(
        const std::vector<std::pair<std::wstring, unsigned long>>& apps) override {
        return std::vector<AppRouteState>(apps.size());
    }
    bool SetPersonalRoute(const std::wstring& ep) override { route = ep; return true; }
    bool ShowUi() override { showCalled = true; return true; }
    bool CaptureUi(const std::wstring& p, const std::wstring& w) override {
        capturePath = p; captureWindow = w; return !p.empty();
    }
    std::wstring captureWindow;
    std::wstring capturePath;
    bool ShowTab(const std::wstring& name) override {
        shownTab = name;
        // The real one matches a name against the tab list; the fake only has
        // to tell a known name from an unknown one.
        return name == L"mixer" || name == L"routing" || name == L"eq" ||
               name == L"devices" || name == L"vban" || name == L"options";
    }
    std::wstring shownTab;
    // The failover rule, so the fake can answer MDXM_FAILOVER and record
    // what a MDXM_FAILOVER_LIST actually set.
    bool feedOn = false;
    bool SetFeedEnabled(bool on, std::wstring*) override { feedOn = on; return true; }
    bool FeedEnabled() override { return feedOn; }
    uint32_t FeedRate() override { return 48000; }

    int cushionMs = 30, cushionHeadroom = 50, cushionFlat = 10;
    int GetCushionMs() override { return cushionMs; }
    int GetCushionHeadroomPercent() override { return cushionHeadroom; }
    int GetCushionFlatMs() override { return cushionFlat; }
    // The real one clamps and treats -1 as "leave alone"; the fake does too,
    // so the test sees what a client would.
    bool SetCushion(int ms, int headroom, int flat) override {
        if (ms >= 0) cushionMs = ms < 5 ? 5 : (ms > 200 ? 200 : ms);
        if (headroom >= 0) cushionHeadroom = headroom > 400 ? 400 : headroom;
        if (flat >= 0) cushionFlat = flat > 100 ? 100 : flat;
        return true;
    }

    FailoverConfig failover;
    FailoverConfig GetFailover() override { return failover; }
    // A route stuck in the loop MDropDX12 #410 described: three commits to the
    // same target that did not stick, backed off to a 40 s dwell.
    FailoverStatus status = [] {
        FailoverStatus s;
        s.routeId = L"personal";
        s.state = L"searching";
        s.reason = L"attempt 3 did not stick; waiting 40 s before retrying";
        s.current = L"{hp-guid}";
        s.target = L"{off-guid}";
        s.attempts = 3;
        s.dwellMs = 40000;
        s.sinceCommitMs = 12000;
        s.holdMs = 0;
        s.audiodgPid = 34552;
        s.audiodgRestarts = 2;
        return s;
    }();
    FailoverStatus GetFailoverStatus() override { return status; }
    bool SetFailoverArmed(bool armed) override { failover.armed = armed; return true; }
    bool SetFailoverTiming(int st, int dw, int gp) override {
        failover.stabilitySec = st; failover.dwellSec = dw; failover.minGapSec = gp;
        return true;
    }
    bool SetFailoverAllow(const std::vector<DeviceRef>& allow) override {
        failover.allow = allow; return true;
    }

    // Held rather than rebuilt per call, because MDXM_HIDE and MDXM_PIN echo
    // the row back: a fake that forgot the write would let an echo of the OLD
    // flags pass the test it is there to catch.
    std::vector<DeviceLevel> levels = [] {
        DeviceLevel d;
        d.id = L"{hp-guid}"; d.name = L"Headphones"; d.isRender = true;
        d.isDefault = true; d.vol = 0.42f; d.mute = false;
        d.displayName = L"Headphones";
        d.peak = 0.1f;
        // An endpoint with no meter to read: paired, switched off, no stream.
        DeviceLevel off;
        off.id = L"{off-guid}"; off.name = L"Headphones (11- WF-1000XM5)";
        off.displayName = L"XM5 Rose Gold";
        off.isRender = true; off.active = false; off.volumeKnown = false;
        off.peak = kPeakUnknown;
        return std::vector<DeviceLevel>{ d, off };
    }();
    std::vector<DeviceLevel> GetDeviceLevels() override {
        ++deviceLevelCalls;
        return levels;
    }
    int deviceLevelCalls = 0;
    bool SetDeviceVolume(const std::wstring& id, float v) override {
        devVolId = id; devVol = v; return id == L"{hp-guid}";
    }
    bool SetDeviceMute(const std::wstring& id, bool m) override {
        devMuteId = id; devMute = m; return id == L"{hp-guid}";
    }
    bool SetDeviceAlias(const std::wstring&, const std::wstring&, const std::wstring&,
                        const std::wstring&) override { return true; }
    std::wstring viewId, viewContainer, viewName;
    bool viewHidden = false, viewPinned = false;
    int viewCalls = 0;
    bool SetDeviceView(const std::wstring& id, const std::wstring& container,
                       const std::wstring& name, bool hidden, bool pinned) override {
        ++viewCalls;
        viewId = id; viewContainer = container; viewName = name;
        viewHidden = hidden; viewPinned = pinned;
        for (auto& d : levels)
            if (d.id == id) { d.hidden = hidden; d.pinned = pinned; return true; }
        return false;
    }
    bool SetDefaultOutput(const std::wstring& id, std::wstring*) override {
        defaultOut = id; return !id.empty();
    }
    std::wstring defaultOut;
    bool RouteAppToEndpoint(const std::wstring& e, const std::wstring& ep, std::wstring*) override {
        routedApp = e; routedTo = ep; return !e.empty() && !ep.empty();
    }
    std::wstring routedApp, routedTo;
    bool ExitApp() override { exited = true; return true; }
    bool exited = false;
    bool ShowHotkeysUi() override { return true; }
    std::wstring GetHotkeyStatus(const std::wstring&) override { return L"held"; }
    std::vector<HotkeyBinding> GetHotkeys() override { return {}; }
    bool SetHotkeys(const std::vector<HotkeyBinding>&) override { return true; }
    int  GetVolumeStep() override { return 5; }
    bool SetVolumeStep(int) override { return true; }
    bool TriggerHotkey(const std::wstring&) override { return true; }
    std::vector<std::pair<std::wstring, std::wstring>> GetHotkeyTargets() override { return {}; }
    bool AddChannel(const std::wstring&, const std::wstring&, std::wstring*) override { return true; }
    bool RemoveChannel(const std::wstring&) override { return true; }
    std::wstring devVolId, devMuteId;
    float devVol = -1.0f;
    bool devMute = false;
    // The ring diagnostic in the shape the engine actually reports it: a
    // capacity of 24000 frames, which is the 500 ms a channel ring holds at
    // 48 kHz, and a trim a test can move to stage a correction in progress.
    size_t diagDepth = 480;
    double diagSpeed = 1.0;
    DiagState GetDiag() override {
        DiagState d; d.rings.push_back({L"game", diagDepth, 3, 1, 24000, diagSpeed});
        d.personalDevice = L"{hp-guid}"; return d;
    }

    // The VBAN server, as the protocol layer sees it. `vbanSets` records the
    // keyed writes in order, because the thing most likely to go wrong with the
    // protocol's first keyed verb is that a set is silently read as a query.
    VbanStatus vbanStatus;
    std::vector<std::pair<std::wstring, std::wstring>> vbanSets;
    bool vbanSetFails = false;
    VbanStatus GetVbanStatus() override { return vbanStatus; }
    bool SetVbanOption(const std::wstring& key, const std::wstring& value,
                       std::wstring* err) override {
        if (vbanSetFails) {
            if (err) *err = key + L" is not a number";
            return false;
        }
        vbanSets.push_back({ key, value });
        return true;
    }
    std::vector<VbanPeerRow> vbanPeers;
    std::vector<VbanPeerRow> GetVbanPeers() override { return vbanPeers; }
    std::wstring revokedDevice;
    bool RevokeVbanDevice(const std::wstring& id) override {
        revokedDevice = id;
        return !id.empty();
    }
};
bool Contains(const std::vector<std::wstring>& v, const std::wstring& needle) {
    return std::any_of(v.begin(), v.end(), [&](const std::wstring& s) {
        return s.find(needle) != std::wstring::npos; });
}
} // namespace

MDXM_TEST_CASE(Protocol_PingPong) {
    FakeControl f; bool sub = false;
    auto r = HandleProtocolMessage(L"MDXM_PING", f, &sub);   // the two-argument form still compiles
    CHECK(r.size() == 1);
    CHECK(r[0].rfind(L"MDXM_PONG|version=", 0) == 0);
}

MDXM_TEST_CASE(Protocol_StateIsChunkedAndTerminated) {
    FakeControl f; bool sub = false;
    auto r = HandleProtocolMessage(L"MDXM_STATE", f, &sub);
    CHECK(r.front() == L"MDXM_BEGIN");
    CHECK(r.back() == L"MDXM_END");                       // the MDropDX12 chunking contract
    CHECK(Contains(r, L"MDXM_CHAN|id=game|name=Game|health=ok|pvol=0.85|pmute=0|svol=1|smute=0|eq=1|peak=0.42"));
    CHECK(Contains(r, L"MDXM_ROUTE|id=personal|device={hp-guid}"));
    CHECK(Contains(r, L"MDXM_DEV|id={hp-guid}"));
}

// -1 and 0 are different answers, and the wire has to carry the difference.
// A client sorting a channel list by what is making sound must be able to tell
// "this channel is quiet" from "nobody can say", and `%g` on -1.0f has to come
// out as the literal -1 that docs/ipc.md promises.
MDXM_TEST_CASE(Protocol_PeakUnknownIsMinusOneNotZero) {
    FakeControl f; bool sub = false;
    auto r = HandleProtocolMessage(L"MDXM_STATE", f, &sub);
    CHECK(Contains(r, L"MDXM_CHAN|id=dead|name=Dead|health=bad|pvol=1|pmute=0|svol=1|smute=0|eq=0|peak=-1"));
    for (const auto& line : r) {
        if (line.rfind(L"MDXM_DEVLVL|id={hp-guid}", 0) == 0)
            CHECK(line.find(L"|peak=0.1") != std::wstring::npos);
        if (line.rfind(L"MDXM_DEVLVL|id={off-guid}", 0) == 0)
            CHECK(line.find(L"|peak=-1") != std::wstring::npos);
    }
}

// The device sweep is what feeds the peak hold and what the Sonar peaks are
// joined off, so MDXM_STATE has to ask for it before it asks for channels --
// otherwise the first reply on a fresh connection reports every Sonar channel
// as unmetered. Once, though: enumerating twenty-eight endpoints twice to
// answer one request is a cost with nothing to show for it.
MDXM_TEST_CASE(Protocol_StateSweepsDevicesExactlyOnce) {
    FakeControl f; bool sub = false;
    HandleProtocolMessage(L"MDXM_STATE", f, &sub);
    CHECK(f.deviceLevelCalls == 1);
}

// MDXM_PEAK is ONE message carrying every row, pushed four times a second.
// Thirty-odd rows as thirty-odd messages would be a hundred and forty messages
// a second to say what fits in one, and a client would have to wait to find
// out whether it had the whole picture.
MDXM_TEST_CASE(Protocol_PeakRecordIsOneCompleteMessage) {
    FakeControl f;
    const auto rec = PeakRecord(f.GetChannels(), f.GetDeviceLevels());
    CHECK(rec.rfind(L"MDXM_PEAK|", 0) == 0);
    // Both channels and both endpoints, each as <id>~<peak>.
    CHECK(rec.find(L"|chan=game~0.42") != std::wstring::npos);
    CHECK(rec.find(L"|chan=dead~-1") != std::wstring::npos);
    CHECK(rec.find(L"|dev={hp-guid}~0.1") != std::wstring::npos);
    CHECK(rec.find(L"|dev={off-guid}~-1") != std::wstring::npos);
    // One message, so exactly one verb in it.
    CHECK(rec.find(L"MDXM_PEAK", 1) == std::wstring::npos);
}

// The pushed MDXM_CHAN and the polled one are the SAME record, because they are
// the same function. They used to be two pieces of formatting -- protocol.cpp
// built one and app_controller.cpp hand-wrote the other with swprintf -- which
// is how a field could reach clients that poll and not clients that listen.
MDXM_TEST_CASE(Protocol_ChannelRecordIsTheOneFormatter) {
    FakeControl f; bool sub = false;
    const auto r = HandleProtocolMessage(L"MDXM_STATE", f, &sub);
    for (const auto& c : f.GetChannels())
        CHECK(Contains(r, ChannelRecord(c)));
}

MDXM_TEST_CASE(Protocol_SetVolumeParsesAndDispatches) {
    FakeControl f; bool sub = false;
    auto r = HandleProtocolMessage(L"MDXM_SET=game|streaming|0.4", f, &sub);
    CHECK(f.lastCh == L"game");
    CHECK(f.lastMix == Mix::Streaming);
    CHECK_NEAR(f.lastVol, 0.4f, 1e-6);
    CHECK(Contains(r, L"MDXM_CHAN"));                     // optimistic echo reply
}

MDXM_TEST_CASE(Protocol_VolumeClamped) {
    // Review Focus #3: out-of-range volume must reach the control clamped.
    FakeControl f; bool sub = false;
    HandleProtocolMessage(L"MDXM_SET=game|personal|1.7", f, &sub);
    CHECK_NEAR(f.lastVol, 1.0f, 1e-6);
    HandleProtocolMessage(L"MDXM_SET=game|personal|-3", f, &sub);
    CHECK_NEAR(f.lastVol, 0.0f, 1e-6);
}

MDXM_TEST_CASE(Protocol_MalformedInputsRejected) {
    // Review Focus #3: junk must produce MDXM_ERR, never a crash or a dispatch.
    FakeControl f; bool sub = false;
    for (const wchar_t* bad : { L"", L"BOGUS_VERB|x=1", L"MDXM_SET=", L"MDXM_SET=game",
                                L"MDXM_SET=game|sideways|0.5", L"MDXM_SET=game|personal|purple",
                                L"MDXM_EQ_SET=game|999|100|0|1", L"MDXM_MUTE=game|personal" }) {
        auto r = HandleProtocolMessage(bad, f, &sub);
        CHECK(!r.empty());
        CHECK(r[0].rfind(L"MDXM_ERR|", 0) == 0);
    }
    CHECK(f.lastVol == -1.0f || f.lastVol == 0.0f);        // the sideways/purple ones never dispatched
}

MDXM_TEST_CASE(Protocol_MuteParsesAndDispatches) {
    FakeControl f; bool sub = false;
    auto r = HandleProtocolMessage(L"MDXM_MUTE=game|personal|1", f, &sub);
    CHECK(f.lastCh.empty());                              // SetMute, not SetVolume, was called
    CHECK(Contains(r, L"MDXM_CHAN"));                     // optimistic echo reply
}

MDXM_TEST_CASE(Protocol_EqSetAndEnable) {
    FakeControl f; bool sub = false;
    HandleProtocolMessage(L"MDXM_EQ_SET=game|3|250|4.5|1.2", f, &sub);
    CHECK(f.eqBand == 3);
    CHECK_NEAR(f.eqF, 250.0, 1e-9);
    CHECK_NEAR(f.eqG, 4.5, 1e-9);
    CHECK_NEAR(f.eqQ, 1.2, 1e-9);
    HandleProtocolMessage(L"MDXM_EQ_ENABLE=game|1", f, &sub);
    CHECK(f.eqOn);
}

MDXM_TEST_CASE(Protocol_DeviceLevelsInState) {
    // A remote (MDropDX12's mixer window) has to see the endpoint volumes, not
    // just the channel faders, or it cannot show what the local UI shows.
    //
    // Asserted field by field rather than against one whole-record string:
    // the record is DESIGNED to gain fields as a front-end needs more of
    // them, and a client that reads by key is unaffected by that while a
    // test that pins the exact line fails on every addition. This one did.
    FakeControl f; bool sub = false;
    auto r = HandleProtocolMessage(L"MDXM_STATE", f, &sub);
    std::wstring rec;
    for (const auto& line : r)
        if (line.rfind(L"MDXM_DEVLVL|", 0) == 0 &&
            line.find(L"|id={hp-guid}|") != std::wstring::npos) rec = line;
    CHECK(!rec.empty());
    for (const wchar_t* field : { L"|name=Headphones", L"|flow=render", L"|default=1",
                                  L"|vol=0.42", L"|mute=0" })
        CHECK(rec.find(field) != std::wstring::npos);
}

MDXM_TEST_CASE(Protocol_DeviceLevelsCarryWhatAListNeedsToBeDrawn) {
    // The fields a front-end would otherwise have to recompute: our name for
    // the device, whether it is here, its battery, when it was last seen, and
    // the anchors that identify the physical device behind a pairing.
    FakeControl f; bool sub = false;
    auto r = HandleProtocolMessage(L"MDXM_STATE", f, &sub);
    std::wstring rec;
    for (const auto& line : r)
        if (line.rfind(L"MDXM_DEVLVL|", 0) == 0 &&
            line.find(L"|id={hp-guid}|") != std::wstring::npos) rec = line;
    CHECK(!rec.empty());
    for (const wchar_t* key : { L"|alias=", L"|active=", L"|battery=", L"|seen=",
                                L"|handsfree=", L"|hidden=", L"|pinned=",
                                L"|container=", L"|bt=" })
        CHECK(rec.find(key) != std::wstring::npos);
}

// ── Failover, delegated ──────────────────────────────────────────────────

MDXM_TEST_CASE(Protocol_FailoverReportsRuleAndLiveStatusTogether) {
    // One reply, because a front-end drawing this list needs both and two
    // round trips could disagree about what is present.
    FakeControl f; bool sub = false;
    f.failover.armed = true;
    f.failover.stabilitySec = 3;
    f.failover.dwellSec = 10;
    f.failover.minGapSec = 5;
    f.failover.allow.push_back({ L"{hp-guid}", L"Headphones" });
    f.failover.allow.push_back({ L"", L"Headphones (2- WF-1000XM5)" });

    auto r = HandleProtocolMessage(L"MDXM_FAILOVER", f, &sub);
    CHECK(Contains(r, L"MDXM_BEGIN"));
    CHECK(Contains(r, L"MDXM_END"));
    CHECK(Contains(r, L"MDXM_FO|armed=1|stability=3|dwell=10|gap=5"));

    std::wstring first, second;
    for (const auto& line : r) {
        if (line.rfind(L"MDXM_FOENTRY|i=0|", 0) == 0) first = line;
        if (line.rfind(L"MDXM_FOENTRY|i=1|", 0) == 0) second = line;
    }
    CHECK(!first.empty());
    CHECK(!second.empty());
    // The one with a live device behind it reports present and keeps its id;
    // the one that is only a stored NAME still gets a row, because the list
    // is mostly devices that are switched off.
    CHECK(first.find(L"|present=1") != std::wstring::npos);
    CHECK(first.find(L"|known=1") != std::wstring::npos);
    CHECK(second.find(L"|present=0") != std::wstring::npos);
    CHECK(second.find(L"|known=0") != std::wstring::npos);
}

// The rule says what SHOULD happen; this says what IS happening, and without
// it MDropDX12's Failover tab can only infer (fj#2 §3). The motivating case is
// MDropDX12 #410, where a route re-committed the same move for hours while the
// only thing readable said "idle" with no reason.
MDXM_TEST_CASE(Protocol_FailoverPublishesWhatTheWatcherIsDoing) {
    FakeControl f; bool sub = false;
    auto r = HandleProtocolMessage(L"MDXM_FAILOVER", f, &sub);
    std::wstring st;
    for (const auto& line : r)
        if (line.rfind(L"MDXM_FOSTATE|", 0) == 0) st = line;
    CHECK(!st.empty());
    if (st.empty()) return;

    // Everything the watcher exposes and nothing could read.
    CHECK(st.find(L"|route=personal") != std::wstring::npos);
    CHECK(st.find(L"|state=searching") != std::wstring::npos);
    // current != target is the fault signature: the move was made and
    // something re-baselined the route off it before the next tick.
    CHECK(st.find(L"|current={hp-guid}") != std::wstring::npos);
    CHECK(st.find(L"|target={off-guid}") != std::wstring::npos);
    CHECK(st.find(L"|attempts=3") != std::wstring::npos);
    CHECK(st.find(L"|dwell=40000") != std::wstring::npos);
    CHECK(st.find(L"|since=12000") != std::wstring::npos);
    CHECK(st.find(L"|hold=0") != std::wstring::npos);
    // Why nothing is happening is usually AUDIODG, so the pid and the restart
    // count travel with the state rather than living in a diagnostic verb.
    CHECK(st.find(L"|audiodg=34552") != std::wstring::npos);
    CHECK(st.find(L"|restarts=2") != std::wstring::npos);
    // The reason is free text and goes LAST, so a future field cannot end up
    // after it and be lost to a reader that stops at the first unknown key.
    CHECK(st.find(L"|reason=attempt 3 did not stick; waiting 40 s before retrying") !=
          std::wstring::npos);
    CHECK(st.substr(st.rfind(L"|reason=")).find(L"|reason=") == 0);
}

MDXM_TEST_CASE(Protocol_FailoverSetTakesAnySubset) {
    FakeControl f; bool sub = false;
    f.failover.stabilitySec = 3;
    HandleProtocolMessage(L"MDXM_FAILOVER_SET|armed=1", f, &sub);
    CHECK(f.failover.armed);
    CHECK(f.failover.stabilitySec == 3);     // untouched by an armed-only call

    HandleProtocolMessage(L"MDXM_FAILOVER_SET|stability=7|dwell=20|gap=9", f, &sub);
    CHECK(f.failover.stabilitySec == 7);
    CHECK(f.failover.dwellSec == 20);
    CHECK(f.failover.minGapSec == 9);
    CHECK(f.failover.armed);                 // and this survived in turn

    // A call that sets nothing is a mistake worth reporting, not a no-op.
    auto r = HandleProtocolMessage(L"MDXM_FAILOVER_SET", f, &sub);
    CHECK(!r.empty() && r[0].rfind(L"MDXM_ERR", 0) == 0);
}

MDXM_TEST_CASE(Protocol_FailoverListReplacesInOrder) {
    // The ORDER is the rule, so it has to survive the round trip exactly.
    FakeControl f; bool sub = false;
    f.failover.allow.push_back({ L"{old}", L"Old" });
    HandleProtocolMessage(
        L"MDXM_FAILOVER_LIST|dev={a}~Alpha|dev={b}~Beta|dev=~NameOnly", f, &sub);
    CHECK(f.failover.allow.size() == 3);
    if (f.failover.allow.size() == 3) {
        CHECK(f.failover.allow[0].id == L"{a}");
        CHECK(f.failover.allow[0].name == L"Alpha");
        CHECK(f.failover.allow[1].id == L"{b}");
        // A name with no id: the only way to list a device that is switched
        // off right now, since it has no endpoint id to give.
        CHECK(f.failover.allow[2].id.empty());
        CHECK(f.failover.allow[2].name == L"NameOnly");
    }
}

MDXM_TEST_CASE(Protocol_FailoverListCanEmptyTheRule) {
    // An armed rule with nothing allowed is legitimate -- it simply never
    // commits -- and is how a front-end clears the list.
    FakeControl f; bool sub = false;
    f.failover.allow.push_back({ L"{a}", L"Alpha" });
    HandleProtocolMessage(L"MDXM_FAILOVER_LIST", f, &sub);
    CHECK(f.failover.allow.empty());
}

MDXM_TEST_CASE(Protocol_SetDeviceVolumeAndMute) {
    FakeControl f; bool sub = false;
    auto r = HandleProtocolMessage(L"MDXM_DEVVOL={hp-guid}|0.3", f, &sub);
    CHECK(f.devVolId == L"{hp-guid}");
    CHECK_NEAR(f.devVol, 0.3f, 1e-6);
    CHECK(r[0] == L"MDXM_OK");
    HandleProtocolMessage(L"MDXM_DEVVOL={hp-guid}|5", f, &sub);
    CHECK_NEAR(f.devVol, 1.0f, 1e-6);                  // clamped, like channel volume
    auto m = HandleProtocolMessage(L"MDXM_DEVMUTE={hp-guid}|1", f, &sub);
    CHECK(f.devMuteId == L"{hp-guid}" && f.devMute);
    CHECK(m[0] == L"MDXM_OK");
    auto bad = HandleProtocolMessage(L"MDXM_DEVVOL={hp-guid}|loud", f, &sub);
    CHECK(bad[0].rfind(L"MDXM_ERR|", 0) == 0);
}

MDXM_TEST_CASE(Protocol_ShowDispatches) {
    // A second instance sends MDXM_SHOW so the running app raises its window.
    FakeControl f; bool sub = false;
    auto r = HandleProtocolMessage(L"MDXM_SHOW", f, &sub);
    CHECK(f.showCalled);
    CHECK(r.size() == 1 && r[0] == L"MDXM_OK");
}

MDXM_TEST_CASE(Protocol_AssignRouteSubscribeDiag) {
    FakeControl f; bool sub = false;
    HandleProtocolMessage(L"MDXM_ASSIGN=C:/Games/game.exe|game", f, &sub);
    CHECK(f.assignedExe == L"C:/Games/game.exe" && f.assignedCh == L"game");
    HandleProtocolMessage(L"MDXM_ROUTE_SET=personal|{usb-guid}", f, &sub);
    CHECK(f.route == L"{usb-guid}");
    HandleProtocolMessage(L"MDXM_SUBSCRIBE=1", f, &sub);
    CHECK(sub);
    auto d = HandleProtocolMessage(L"MDXM_DIAG", f, &sub);
    CHECK(Contains(d, L"MDXM_RING|id=game|depth=480|cap=24000|drops=3|underruns=1|speed=1.0000"));
}

// ── reading a ring from outside the process (fj#13) ──────────────────────
//
// A DEPTH ALONE SAYS NOTHING. The 1.99-second backlog of 2026-10-05 was read
// off this very line -- "depth=95520" -- and it only meant anything because
// the ring was known FROM THE SOURCE to hold 96000 frames. Sampled three
// times over a minute and frozen, that was the whole diagnosis. Anyone
// without the source had a large number and no way to tell a backlog from an
// ordinary fill, so the capacity belongs in the line.
MDXM_TEST_CASE(Protocol_RingDepthIsReportedAgainstItsCapacity) {
    FakeControl f; bool sub = false;
    f.diagDepth = 23900;                              // all but 100 frames of 24000
    auto d = HandleProtocolMessage(L"MDXM_DIAG", f, &sub);
    CHECK(Contains(d, L"depth=23900|cap=24000"));
}

// The varispeed trim is INAUDIBLE BY DESIGN, which is exactly why it has to be
// observable: a silent change to playback speed that cannot be seen from
// outside the process is indistinguishable from a bug. The log records the two
// transitions; this records the state, which is what a sample actually reads.
MDXM_TEST_CASE(Protocol_RingReportsADrainInProgress) {
    FakeControl f; bool sub = false;
    f.diagSpeed = 1.05;                               // kMaxRingTrim, draining hard
    auto d = HandleProtocolMessage(L"MDXM_DIAG", f, &sub);
    CHECK(Contains(d, L"speed=1.0500"));
}

// Refilling has to be as visible as draining. The half that was missing at
// first produced ten underruns with the trim sitting at 1.0 throughout, and a
// reader who can only see a drain would have read that as "nothing is wrong".
MDXM_TEST_CASE(Protocol_RingReportsARefillInProgress) {
    FakeControl f; bool sub = false;
    f.diagSpeed = 0.975;
    auto d = HandleProtocolMessage(L"MDXM_DIAG", f, &sub);
    CHECK(Contains(d, L"speed=0.9750"));
}

// ── A subscriber's own push rate ─────────────────────────────────────────
//
// "You can have the subscriber get 100 ms if they ask for it." The window was
// sped up to 100 ms for its own meters, and the push to subscribers was left
// at 250 deliberately: the screen's refresh rate is a local choice and the
// push rate is a published one. This is how a client opts in -- and a client
// that only wants to know when a device appears can opt DOWN and save both
// ends the work.

MDXM_TEST_CASE(Protocol_SubscribeStillWorksWithNoRate) {
    FakeControl f; bool sub = false; int interval = -1;
    const auto r = HandleProtocolMessage(L"MDXM_SUBSCRIBE=1", f, &sub, &interval);
    CHECK(sub);
    CHECK(Contains(r, L"MDXM_OK"));
    CHECK(interval == -1);     // untouched: the client keeps whatever it had
}

MDXM_TEST_CASE(Protocol_SubscribeTakesARate) {
    FakeControl f; bool sub = false; int interval = -1;
    const auto r = HandleProtocolMessage(L"MDXM_SUBSCRIBE=1|100", f, &sub, &interval);
    CHECK(sub);
    CHECK(interval == 100);
    // Echoed back, so a client never has to guess what it actually got.
    CHECK(Contains(r, L"MDXM_OK|intervalMs=100"));
}

// Clamped rather than refused: a client that asks for something silly should
// still get its state, at a rate that cannot hurt either end. The floor is the
// window's own refresh -- a push cannot carry what the sweep has not produced.
MDXM_TEST_CASE(Protocol_ARateIsClampedNotRejected) {
    FakeControl f; bool sub = false;
    int tooFast = -1, tooSlow = -1, nonsense = -1;
    CHECK(Contains(HandleProtocolMessage(L"MDXM_SUBSCRIBE=1|5", f, &sub, &tooFast),
                   L"MDXM_OK|intervalMs=100"));
    CHECK(tooFast == kPushIntervalMinMs);
    CHECK(Contains(HandleProtocolMessage(L"MDXM_SUBSCRIBE=1|600000", f, &sub, &tooSlow),
                   L"MDXM_OK|intervalMs=5000"));
    CHECK(tooSlow == kPushIntervalMaxMs);
    // Unreadable falls back to the default rather than failing the whole
    // subscription.
    HandleProtocolMessage(L"MDXM_SUBSCRIBE=1|banana", f, &sub, &nonsense);
    CHECK(nonsense == kPushIntervalDefaultMs);
}

MDXM_TEST_CASE(Protocol_UnsubscribingMayAlsoCarryARate) {
    // Odd, but it must not be an error: a client that always sends the pair
    // should be able to turn the subscription off with the same shape.
    FakeControl f; bool sub = true; int interval = -1;
    const auto r = HandleProtocolMessage(L"MDXM_SUBSCRIBE=0|100", f, &sub, &interval);
    CHECK(!sub);
    CHECK(!r.empty() && r[0].rfind(L"MDXM_ERR", 0) != 0);
}

MDXM_TEST_CASE(Protocol_SubscribeStillRejectsNonsense) {
    FakeControl f; bool sub = false; int interval = -1;
    for (const wchar_t* bad : { L"MDXM_SUBSCRIBE", L"MDXM_SUBSCRIBE=1|100|7",
                                L"MDXM_SUBSCRIBE=yes" }) {
        const auto r = HandleProtocolMessage(bad, f, &sub, &interval);
        CHECK(!r.empty() && r[0].rfind(L"MDXM_ERR", 0) == 0);
    }
}

MDXM_TEST_CASE(ClampPushInterval_KeepsWhatIsReasonable) {
    CHECK(ClampPushIntervalMs(250) == 250);
    CHECK(ClampPushIntervalMs(100) == 100);
    CHECK(ClampPushIntervalMs(1000) == 1000);
    CHECK(ClampPushIntervalMs(0) == kPushIntervalDefaultMs);
    CHECK(ClampPushIntervalMs(-5) == kPushIntervalDefaultMs);
}

// ── The feed is off until something asks ─────────────────────────────────

MDXM_TEST_CASE(Protocol_FeedIsOffUntilAsked) {
    // Running a shared-memory ring nobody reads costs a mapping and a copy
    // per audio block for nothing. It exists for when a visualiser cannot
    // get the audio any other way, which is not the normal case.
    FakeControl f; bool sub = false;
    auto r = HandleProtocolMessage(L"MDXM_FEED", f, &sub);
    CHECK(!r.empty());
    CHECK(r[0].find(L"|on=0") != std::wstring::npos);
    CHECK(!f.feedOn);
}

MDXM_TEST_CASE(Protocol_FeedSwitchesAndReportsTheRing) {
    FakeControl f; bool sub = false;
    auto r = HandleProtocolMessage(L"MDXM_FEED|1", f, &sub);
    CHECK(f.feedOn);
    CHECK(!r.empty());
    CHECK(r[0].find(L"|on=1") != std::wstring::npos);
    // Enough for a reader to decide whether to map it, without mapping it.
    // Doubled. Written with ONE backslash this passed while the code was
    // wrong, because `\m` is an unknown escape that the compiler drops --
    // so both sides agreed on "Localmdxmixer_stream_v1", a name no reader
    // would ever find.
    CHECK(r[0].find(L"Local\\mdxmixer_stream_v1") != std::wstring::npos);
    CHECK(r[0].find(L"|rate=48000") != std::wstring::npos);
    CHECK(r[0].find(L"|channels=2") != std::wstring::npos);

    r = HandleProtocolMessage(L"MDXM_FEED|0", f, &sub);
    CHECK(!f.feedOn);
    CHECK(r[0].find(L"|on=0") != std::wstring::npos);
}

MDXM_TEST_CASE(Protocol_FeedRejectsNonsense) {
    FakeControl f; bool sub = false;
    auto r = HandleProtocolMessage(L"MDXM_FEED|maybe", f, &sub);
    CHECK(!r.empty() && r[0].rfind(L"MDXM_ERR", 0) == 0);
    CHECK(!f.feedOn);
}

// MDXM_TAB and the overlay capture target both exist for one reason: the
// README's screenshots have to be REGENERABLE. A picture nobody can retake is
// a picture that will be wrong by the next release.
MDXM_TEST_CASE(Protocol_TabSwitchTakesANameNotAnIndex) {
    FakeControl f; bool sub = false;
    auto r = HandleProtocolMessage(L"MDXM_TAB=devices", f, &sub);
    CHECK(Contains(r, L"MDXM_OK"));
    CHECK(f.shownTab == L"devices");
    // An index would silently mean a different tab the day one is inserted.
    r = HandleProtocolMessage(L"MDXM_TAB=3", f, &sub);
    CHECK(Contains(r, L"MDXM_ERR"));
    r = HandleProtocolMessage(L"MDXM_TAB", f, &sub);
    CHECK(Contains(r, L"MDXM_ERR"));
}

MDXM_TEST_CASE(Protocol_CaptureTakesTheOverlayAsWellAsTheWindows) {
    FakeControl f; bool sub = false;
    for (const wchar_t* w : { L"main", L"hotkeys", L"overlay" }) {
        auto r = HandleProtocolMessage(std::wstring(L"MDXM_CAPTURE=shot.png|") + w, f, &sub);
        CHECK(Contains(r, L"MDXM_OK"));
        CHECK(f.captureWindow == w);
    }
    // The overlay is frameless, click-through and always on top, so nothing
    // else can get a picture of it short of grabbing the whole desktop --
    // which would publish whatever else was on screen.
    auto r = HandleProtocolMessage(L"MDXM_CAPTURE=shot.png|desktop", f, &sub);
    CHECK(Contains(r, L"MDXM_ERR"));
}

// ── hidden and pinned, which were readable and not writable (fj#8) ───────

// The same hazard ChannelRecord was extracted for, one record along: there
// are now two producers of an MDXM_DEVLVL -- the MDXM_STATE reply and the
// echo these verbs send -- and a field added to one spelling is invisible to
// clients reading the other.
MDXM_TEST_CASE(Protocol_DeviceRecordIsTheSpellingTheStateReplyUses) {
    FakeControl f; bool sub = false;
    auto state = HandleProtocolMessage(L"MDXM_STATE", f, &sub);
    std::wstring fromState;
    for (const auto& m : state)
        if (m.rfind(L"MDXM_DEVLVL|id={hp-guid}", 0) == 0) { fromState = m; break; }
    CHECK(!fromState.empty());
    CHECK(DeviceRecord(f.levels[0]) == fromState);
}

// A channel that is not capturing because nothing is pulling the mix says so,
// and says it WITHOUT claiming a fault (fj#10). The three fields have to be
// readable together: health=ok because the cable has not been opened and
// nothing is known to be wrong, peak=-1 because nothing is being measured,
// idle=1 because that is the reason for both.
MDXM_TEST_CASE(Protocol_AnIdleChannelIsNotAnUnhealthyOne) {
    ChannelState idle{ L"sonar", L"Sonar", true, 0.1f, false, 1.0f, false, false };
    idle.peak = kPeakUnknown;
    idle.idle = true;
    const std::wstring rec = ChannelRecord(idle);
    CHECK(rec.find(L"|health=ok") != std::wstring::npos);
    CHECK(rec.find(L"|peak=-1") != std::wstring::npos);
    CHECK(rec.find(L"|idle=1") != std::wstring::npos);

    // And an ordinary running channel says so too, rather than omitting the
    // field -- a reader should never have to treat absent as false.
    ChannelState live{ L"sonar", L"Sonar", true, 0.1f, false, 1.0f, false, false };
    live.peak = 0.42f;
    CHECK(ChannelRecord(live).find(L"|idle=0") != std::wstring::npos);
}

// The latency, live. A verb rather than config-only because finding the right
// value is an experiment -- lower it, watch `underruns` -- and a config file
// plus a restart would reset the counter being watched (fj#12).
MDXM_TEST_CASE(Protocol_CushionQueriesAndSets) {
    FakeControl f; bool sub = false;
    auto r = HandleProtocolMessage(L"MDXM_CUSHION", f, &sub);
    CHECK(r.size() == 1);
    CHECK(r[0] == L"MDXM_CUSHIONSTATE|ms=30|headroom=50|flat=10");

    r = HandleProtocolMessage(L"MDXM_CUSHION|12", f, &sub);
    CHECK(f.cushionMs == 12);
    CHECK(r[0] == L"MDXM_CUSHIONSTATE|ms=12|headroom=50|flat=10");

    // Clamped, and the reply says what was ACTUALLY applied rather than
    // echoing the request -- a client lowering it until underruns appear has
    // to know where it really landed.
    r = HandleProtocolMessage(L"MDXM_CUSHION|1", f, &sub);
    CHECK(f.cushionMs == 5);
    CHECK(r[0] == L"MDXM_CUSHIONSTATE|ms=5|headroom=50|flat=10");
    r = HandleProtocolMessage(L"MDXM_CUSHION|9999", f, &sub);
    CHECK(f.cushionMs == 200);
}

// The two adaptive terms are what actually govern once the floor is out of the
// way, so they are settable the same way -- and independently, because a sweep
// moves one at a time.
MDXM_TEST_CASE(Protocol_CushionAdaptiveTermsAreSettableOnTheirOwn) {
    FakeControl f; bool sub = false;
    auto r = HandleProtocolMessage(L"MDXM_CUSHION|flat=0", f, &sub);
    CHECK(f.cushionFlat == 0);
    CHECK(f.cushionHeadroom == 50);   // untouched
    CHECK(f.cushionMs == 30);         // untouched
    CHECK(r[0] == L"MDXM_CUSHIONSTATE|ms=30|headroom=50|flat=0");

    r = HandleProtocolMessage(L"MDXM_CUSHION|headroom=25", f, &sub);
    CHECK(f.cushionHeadroom == 25);
    CHECK(f.cushionFlat == 0);        // the previous change stands

    // All three at once, floor positionally as usual.
    r = HandleProtocolMessage(L"MDXM_CUSHION|8|headroom=100|flat=3", f, &sub);
    CHECK(f.cushionMs == 8);
    CHECK(f.cushionHeadroom == 100);
    CHECK(f.cushionFlat == 3);
}

MDXM_TEST_CASE(Protocol_CushionRejectsNonsense) {
    FakeControl f; bool sub = false;
    f.cushionMs = 30;
    auto r = HandleProtocolMessage(L"MDXM_CUSHION|low", f, &sub);
    CHECK(!r.empty() && r[0].rfind(L"MDXM_ERR", 0) == 0);
    CHECK(f.cushionMs == 30);   // untouched
}

// ── the device push (fj#7) ───────────────────────────────────────────────

// The ids ARE the set, in mdxmixer's own sort order. A client with a control
// per row rebuilds its controls on this record and only on this record.
MDXM_TEST_CASE(Protocol_DeviceSetRecordIsTheIdsInOrder) {
    FakeControl f;
    CHECK(DeviceSetRecord(f.levels) == L"MDXM_DEVSET|dev={hp-guid}|dev={off-guid}");
    // An empty machine is a legitimate answer and has to be sayable.
    CHECK(DeviceSetRecord({}) == L"MDXM_DEVSET");
    // Order is content: the same two devices the other way round is a
    // different record, because the order is what a client draws.
    std::vector<DeviceLevel> swapped{ f.levels[1], f.levels[0] };
    CHECK(DeviceSetRecord(swapped) == L"MDXM_DEVSET|dev={off-guid}|dev={hp-guid}");
}

// This function decides whether a change reaches a client at all, so a field
// it forgets is a change that silently never pushes. Every field DeviceRecord
// carries is listed here; `peak` is the only one it must ignore, because that
// one moves constantly and travels on MDXM_PEAK.
MDXM_TEST_CASE(Protocol_SameDeviceRowIgnoresPeakAndNothingElse) {
    DeviceLevel base;
    base.id = L"{hp-guid}"; base.name = L"Headphones"; base.displayName = L"XM6";
    base.containerId = L"{container}"; base.btAddress = L"a1b2c3";
    base.isRender = true; base.isDefault = true; base.vol = 0.42f; base.mute = false;
    base.active = true; base.battery = 90;
    // A FILETIME (100ns ticks) on an exact minute boundary, so the 30 s case
    // below is genuinely inside one minute and the 60 s one genuinely crosses.
    // Local offsets are whole minutes, so this holds in any timezone.
    base.lastConnectedUtc = 221666666ULL * 600000000ULL;
    base.isHandsFree = false; base.hidden = false; base.pinned = false;
    base.peak = 0.3f;

    DeviceLevel sameButLouder = base;
    sameButLouder.peak = 0.91f;
    CHECK(SameDeviceRow(base, sameButLouder));
    // Including the two unknowns, which are a reading rather than a change of
    // the row: a meter that stops being readable must not push the row.
    sameButLouder.peak = kPeakUnknown;
    CHECK(SameDeviceRow(base, sameButLouder));

    // `seen` is minute resolution on the wire, so a sighting a few seconds
    // later is the SAME row to a client. Pushing it would send a record
    // byte-identical to the last one, which is noise with a cost.
    DeviceLevel secondsLater = base;
    secondsLater.lastConnectedUtc += 30ULL * 10000000ULL;   // 30 s
    CHECK(SameDeviceRow(base, secondsLater));
    CHECK(DeviceRecord(base) == DeviceRecord(secondsLater));

    struct Change { const char* what; void (*apply)(DeviceLevel&); };
    static const Change kChanges[] = {
        { "id",        [](DeviceLevel& d) { d.id = L"{other}"; } },
        { "name",      [](DeviceLevel& d) { d.name = L"Speakers"; } },
        { "alias",     [](DeviceLevel& d) { d.displayName = L"XM5 BLK 1 RTK"; } },
        { "flow",      [](DeviceLevel& d) { d.isRender = false; } },
        { "default",   [](DeviceLevel& d) { d.isDefault = false; } },
        { "vol",       [](DeviceLevel& d) { d.vol = 0.43f; } },
        { "mute",      [](DeviceLevel& d) { d.mute = true; } },
        { "active",    [](DeviceLevel& d) { d.active = false; } },
        { "battery",   [](DeviceLevel& d) { d.battery = 89; } },
        { "seen",      [](DeviceLevel& d) { d.lastConnectedUtc += 60ULL * 10000000ULL; } },
        { "handsfree", [](DeviceLevel& d) { d.isHandsFree = true; } },
        { "hidden",    [](DeviceLevel& d) { d.hidden = true; } },
        { "pinned",    [](DeviceLevel& d) { d.pinned = true; } },
        { "container", [](DeviceLevel& d) { d.containerId = L"{elsewhere}"; } },
        { "bt",        [](DeviceLevel& d) { d.btAddress = L"ffffff"; } },
    };
    for (const auto& c : kChanges) {
        DeviceLevel changed = base;
        c.apply(changed);
        if (SameDeviceRow(base, changed))
            std::printf("     change to %s does not push\n", c.what);
        CHECK(!SameDeviceRow(base, changed));
        // And the records really do differ, so the diff and the formatter
        // cannot disagree about what a row says.
        CHECK(DeviceRecord(base) != DeviceRecord(changed));
    }
}

MDXM_TEST_CASE(Protocol_HideFilesADeviceAwayAndEchoesTheRow) {
    FakeControl f; bool sub = false;
    auto r = HandleProtocolMessage(L"MDXM_HIDE|{hp-guid}|1", f, &sub);
    CHECK(f.viewCalls == 1);
    CHECK(f.viewId == L"{hp-guid}");
    CHECK(f.viewHidden);
    // The container and the Windows name are looked up here rather than
    // demanded, as MDXM_NAME does it: a caller holding an endpoint id should
    // not have to carry the other anchors to file a device away.
    CHECK(f.viewName == L"Headphones");
    // Echoed as the row, not MDXM_OK -- the caller gets the new state without
    // a second round trip, the way MDXM_SET echoes MDXM_CHAN.
    CHECK(r.size() == 1);
    CHECK(r[0].rfind(L"MDXM_DEVLVL|id={hp-guid}", 0) == 0);
    CHECK(r[0].find(L"|hidden=1") != std::wstring::npos);

    r = HandleProtocolMessage(L"MDXM_HIDE|{hp-guid}|0", f, &sub);
    CHECK(!f.viewHidden);
    CHECK(r[0].find(L"|hidden=0") != std::wstring::npos);
}

MDXM_TEST_CASE(Protocol_PinHoistsADeviceAndEchoesTheRow) {
    FakeControl f; bool sub = false;
    auto r = HandleProtocolMessage(L"MDXM_PIN|{hp-guid}|1", f, &sub);
    CHECK(f.viewPinned);
    CHECK(r.size() == 1);
    CHECK(r[0].find(L"|pinned=1") != std::wstring::npos);
}

// A device cannot be both held at the top of the list and absent from it, so
// setting either flag clears the other. The Mixer tab's Hide menu item has
// always cleared the pin for this reason; the rule is now symmetric and both
// surfaces go through it.
MDXM_TEST_CASE(Protocol_HideAndPinCannotBothBeTrue) {
    FakeControl f; bool sub = false;
    HandleProtocolMessage(L"MDXM_PIN|{hp-guid}|1", f, &sub);
    auto r = HandleProtocolMessage(L"MDXM_HIDE|{hp-guid}|1", f, &sub);
    CHECK(f.viewHidden);
    CHECK(!f.viewPinned);
    CHECK(r[0].find(L"|pinned=0") != std::wstring::npos);

    r = HandleProtocolMessage(L"MDXM_PIN|{hp-guid}|1", f, &sub);
    CHECK(f.viewPinned);
    CHECK(!f.viewHidden);
    CHECK(r[0].find(L"|hidden=0") != std::wstring::npos);

    // Clearing one says nothing about the other: unpinning a visible device
    // must not hide it.
    r = HandleProtocolMessage(L"MDXM_PIN|{hp-guid}|0", f, &sub);
    CHECK(!f.viewPinned);
    CHECK(!f.viewHidden);
}

MDXM_TEST_CASE(Protocol_HideAndPinRejectNonsense) {
    FakeControl f; bool sub = false;
    for (const wchar_t* bad : { L"MDXM_HIDE|{hp-guid}", L"MDXM_HIDE|{hp-guid}|maybe",
                                L"MDXM_HIDE|{nope}|1", L"MDXM_PIN|{nope}|1",
                                L"MDXM_PIN", L"MDXM_PIN|{hp-guid}|2" }) {
        auto r = HandleProtocolMessage(bad, f, &sub);
        CHECK(!r.empty() && r[0].rfind(L"MDXM_ERR", 0) == 0);
    }
    // Nothing was written by any of them.
    CHECK(f.viewCalls == 0);
}

MDXM_TEST_CASE(Protocol_VbanQueryReturnsState) {
    FakeControl f;
    f.vbanStatus.on = true;
    f.vbanStatus.emitting = true;
    f.vbanStatus.port = 6980;
    f.vbanStatus.gainPercent = 1600;
    f.vbanStatus.srcLatencyMs = 37;
    auto r = HandleProtocolMessage(L"MDXM_VBAN", f, nullptr);
    CHECK(r.size() == 1);
    CHECK(r[0].find(L"MDXM_VBANSTATE|on=1|emitting=1|port=6980") == 0);
    CHECK(r[0].find(L"|gain=1600") != std::wstring::npos);
    // The PC's share of the delay, which is what the phone adds its own half to
    // in order to show a figure somebody can type into a video player.
    CHECK(r[0].find(L"|srclatencyms=37") != std::wstring::npos);
    CHECK(f.vbanSets.empty());                     // a query writes nothing
}

MDXM_TEST_CASE(Protocol_VbanKeyedSetIsSetNotQuery) {
    // MDXM_VBAN and MDXM_AUTH are the protocol's FIRST keyed-argument inbound
    // verbs. Every other verb is positional, and HandleInner's positional vector
    // is built from empty-key fields only -- so for a keyed record it is EMPTY.
    // The nearest precedent, MDXM_FEED, treats empty args as "query", and a
    // handler copying that shape would turn every keyed SET into a query and
    // answer MDXM_VBANSTATE as though it had worked.
    FakeControl f;
    auto r = HandleProtocolMessage(L"MDXM_VBAN|gain=250", f, nullptr);
    CHECK(f.vbanSets.size() == 1);
    CHECK(f.vbanSets[0] == std::make_pair(std::wstring(L"gain"), std::wstring(L"250")));
    CHECK(r.size() == 1 && r[0].rfind(L"MDXM_VBANSTATE|", 0) == 0);   // echo is the state
    r = HandleProtocolMessage(L"MDXM_VBAN|on=1|port=7001", f, nullptr);
    CHECK(f.vbanSets.size() == 3);                 // applied in order, both of them
    CHECK(f.vbanSets[1].first == L"on" && f.vbanSets[2].first == L"port");
}

MDXM_TEST_CASE(Protocol_VbanPerPeerAndAuthKeysAreVbanOnlyOnThePipe) {
    // These three mean something only over VBAN-TXT, where the server answers
    // them against a known peer. On the pipe there is no peer to apply them to,
    // and MDXM_ERR is the grammar's only negative reply.
    FakeControl f;
    auto r = HandleProtocolMessage(L"MDXM_VBAN|frames=1", f, nullptr);
    CHECK(r.size() == 1 && r[0] == L"MDXM_ERR|msg=frames is VBAN-only");
    r = HandleProtocolMessage(L"MDXM_VBAN|audio=0", f, nullptr);
    CHECK(r.size() == 1 && r[0] == L"MDXM_ERR|msg=audio is VBAN-only");
    r = HandleProtocolMessage(L"MDXM_AUTH|pin=1|device=d|name=n", f, nullptr);
    CHECK(r.size() == 1 && r[0] == L"MDXM_ERR|msg=auth is VBAN-only");
    CHECK(f.vbanSets.empty());                     // and none of them wrote anything
}

MDXM_TEST_CASE(Protocol_VbanRejectionStopsAtTheFirstBadKey) {
    // A rejected value must not leave half a record applied: the reply says what
    // went wrong and the caller re-sends the whole thing.
    FakeControl f;
    f.vbanSetFails = true;
    auto r = HandleProtocolMessage(L"MDXM_VBAN|gain=purple", f, nullptr);
    CHECK(r.size() == 1 && r[0].rfind(L"MDXM_ERR|", 0) == 0);
    CHECK(r[0].find(L"gain") != std::wstring::npos);   // names the key that failed
    CHECK(f.vbanSets.empty());
}

MDXM_TEST_CASE(Protocol_VbanPeersListsPeersAndTheAlwaysTarget) {
    FakeControl f;
    VbanPeerRow phone;
    phone.addr = L"192.168.0.77:50001";
    phone.deviceName = L"Pixel 9";
    phone.authed = true;
    phone.audioOn = true;
    phone.sinceMs = 1200;
    VbanPeerRow target;
    target.addr = L"192.168.0.90:6980";
    target.isAlwaysTarget = true;
    f.vbanPeers = { phone, target };
    auto r = HandleProtocolMessage(L"MDXM_VBANPEERS", f, nullptr);
    CHECK(r.size() == 4);                           // BEGIN, two rows, END
    CHECK(r.front() == L"MDXM_BEGIN" && r.back() == L"MDXM_END");
    CHECK(Contains(r, L"MDXM_VBANPEER|addr=192.168.0.77:50001"));
    CHECK(Contains(r, L"|device=Pixel 9"));
    CHECK(Contains(r, L"|authed=1"));
    // The configured target is not a peer and says so, because something is
    // being sent there and a reader has to be able to tell the difference.
    CHECK(Contains(r, L"|always=1"));
}
