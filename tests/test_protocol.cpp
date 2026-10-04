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
    bool SetPersonalRoute(const std::wstring& ep) override { route = ep; return true; }
    bool ShowUi() override { showCalled = true; return true; }
    bool CaptureUi(const std::wstring& p, const std::wstring& w) override {
        capturePath = p; captureWindow = w; return !p.empty();
    }
    std::wstring captureWindow;
    std::wstring capturePath;
    // The failover rule, so the fake can answer MDXM_FAILOVER and record
    // what a MDXM_FAILOVER_LIST actually set.
    bool feedOn = false;
    bool SetFeedEnabled(bool on, std::wstring*) override { feedOn = on; return true; }
    bool FeedEnabled() override { return feedOn; }
    uint32_t FeedRate() override { return 48000; }

    FailoverConfig failover;
    FailoverConfig GetFailover() override { return failover; }
    bool SetFailoverArmed(bool armed) override { failover.armed = armed; return true; }
    bool SetFailoverTiming(int st, int dw, int gp) override {
        failover.stabilitySec = st; failover.dwellSec = dw; failover.minGapSec = gp;
        return true;
    }
    bool SetFailoverAllow(const std::vector<DeviceRef>& allow) override {
        failover.allow = allow; return true;
    }

    std::vector<DeviceLevel> GetDeviceLevels() override {
        ++deviceLevelCalls;
        DeviceLevel d;
        d.id = L"{hp-guid}"; d.name = L"Headphones"; d.isRender = true;
        d.isDefault = true; d.vol = 0.42f; d.mute = false;
        d.peak = 0.1f;
        // An endpoint with no meter to read: paired, switched off, no stream.
        DeviceLevel off;
        off.id = L"{off-guid}"; off.name = L"Headphones (11- WF-1000XM5)";
        off.isRender = true; off.active = false; off.volumeKnown = false;
        off.peak = kPeakUnknown;
        return { d, off };
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
    bool SetDeviceView(const std::wstring&, const std::wstring&, const std::wstring&,
                       bool, bool) override { return true; }
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
    DiagState GetDiag() override {
        DiagState d; d.rings.push_back({L"game", 480, 3, 1}); d.personalDevice = L"{hp-guid}"; return d;
    }
};
bool Contains(const std::vector<std::wstring>& v, const std::wstring& needle) {
    return std::any_of(v.begin(), v.end(), [&](const std::wstring& s) {
        return s.find(needle) != std::wstring::npos; });
}
} // namespace

MDXM_TEST_CASE(Protocol_PingPong) {
    FakeControl f; bool sub = false;
    auto r = HandleProtocolMessage(L"MDXM_PING", f, &sub);
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
    CHECK(Contains(d, L"MDXM_RING|id=game|depth=480|drops=3|underruns=1"));
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
