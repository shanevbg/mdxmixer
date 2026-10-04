#include "test_framework.h"
#include "engine/failover_watcher.h"
#include <set>

using namespace mdxm;

// A watcher with an injected clock and presence set, so every sequence below
// runs with no audio device and no waiting.
namespace {
struct Rig {
    FailoverWatcher w;
    unsigned now = 1000;
    std::set<std::wstring> present;
    std::vector<std::pair<std::wstring, std::wstring>> commits;   // route, device

    Rig() {
        w.SetClock([this] { return now; });
        w.SetPresence([this](const std::wstring& id) { return present.count(id) > 0; });
        w.SetNames([this](const std::wstring& name) -> std::wstring {
            // The Bluetooth case: a name that is currently carried by an id.
            for (const auto& id : present)
                if (id == L"{new-id}" && name == L"XM5") return id;
            return L"";
        });
        // The callback only RECORDS. Calling NoteRouteDevice from inside it
        // would re-enter Tick's own bookkeeping, which no real caller does --
        // mdx12 notes the new device from the next mixer snapshot. Doing it
        // re-entrantly here hung the suite.
        w.SetOnCommit([this](const std::wstring& route, const std::wstring& dev) {
            commits.push_back({ route, dev });
            pending.push_back({ route, reBaselineTo.empty() ? dev : reBaselineTo });
        });
    }
    // Applied after Tick returns, as the real caller does.
    std::vector<std::pair<std::wstring, std::wstring>> pending;
    std::wstring reBaselineTo;   // non-empty: simulate a move that will not stick
    void Advance(unsigned ms) {
        now += ms;
        w.Tick();
        auto todo = pending;
        pending.clear();
        for (const auto& p : todo) w.NoteRouteDevice(p.first, p.second);
    }
    void Rule(bool armed, std::vector<AllowEntry> allow) {
        RouteRule r;
        r.routeId = L"personal";
        r.armed = armed;
        r.allow = std::move(allow);
        w.SetRule(r);
    }
};
} // namespace

MDXM_TEST_CASE(Failover_CancelsWhenTheDeviceComesBackInTime) {
    // A brief Bluetooth dropout must cost nothing. That is the point of the
    // stability window, not a side effect of it -- and on this machine the
    // headsets flap: one was measured connecting and dropping every ~30 s.
    Rig r;
    r.w.SetStabilitySeconds(3);
    r.present = { L"{headset}", L"{speakers}" };
    r.Rule(true, { { L"{speakers}", L"Speakers" } });
    r.w.NoteRouteDevice(L"personal", L"{headset}");

    r.present.erase(L"{headset}");          // gone
    r.Advance(1000);
    r.present.insert(L"{headset}");         // back, inside the window
    r.Advance(1000);
    r.Advance(5000);
    CHECK(r.commits.empty());
    CHECK(r.w.StateOf(L"personal") == FailoverState::Idle);
}

MDXM_TEST_CASE(Failover_CommitsOnceTheStabilityWindowElapses) {
    Rig r;
    r.w.SetStabilitySeconds(3);
    r.w.SetMinDwellSeconds(10);
    r.present = { L"{headset}", L"{speakers}" };
    r.Rule(true, { { L"{speakers}", L"Speakers" } });
    r.w.NoteRouteDevice(L"personal", L"{headset}");

    r.present.erase(L"{headset}");
    r.Advance(1000);
    CHECK(r.commits.empty());               // still inside the window
    r.Advance(3000);
    CHECK(r.commits.size() == 1);
    if (!r.commits.empty()) CHECK(r.commits[0].second == L"{speakers}");

    // Fail over, NEVER back: the old device returning does nothing.
    r.present.insert(L"{headset}");
    r.Advance(20000);
    CHECK(r.commits.size() == 1);
    CHECK(r.w.CurrentOf(L"personal") == L"{speakers}");
}

MDXM_TEST_CASE(Failover_OptInIsAbsolute) {
    // Unarmed, or armed with nothing present to move to: the watcher does
    // nothing, forever.
    Rig r;
    r.w.SetStabilitySeconds(1);
    r.present = { L"{headset}", L"{speakers}" };
    r.Rule(false, { { L"{speakers}", L"Speakers" } });
    r.w.NoteRouteDevice(L"personal", L"{headset}");
    r.present.erase(L"{headset}");
    r.Advance(60000);
    CHECK(r.commits.empty());

    Rig r2;
    r2.w.SetStabilitySeconds(1);
    r2.present = { L"{headset}" };
    r2.Rule(true, { { L"{not-here}", L"Absent device" } });
    r2.w.NoteRouteDevice(L"personal", L"{headset}");
    r2.present.clear();
    r2.Advance(60000);
    CHECK(r2.commits.empty());
}

MDXM_TEST_CASE(Failover_BacksOffWhenTheSameMoveKeepsNotSticking) {
    // #410, measured: thirteen commits of the same move, 24 s apart, each one a
    // teardown and rebuild of the audio graph. The anti-flap timers were not
    // failing -- they were setting the loop's PERIOD. Doubling the dwell per
    // consecutive attempt turns "for ever" into a handful of tries.
    Rig r;
    r.w.SetStabilitySeconds(1);
    r.w.SetMinDwellSeconds(10);
    r.w.SetMinGapSeconds(0);
    r.present = { L"{speakers}" };
    r.Rule(true, { { L"{speakers}", L"Speakers" } });
    // The route is on a device that is not present, and the commit does not
    // stick: something keeps putting it back.
    r.reBaselineTo = L"{dead}";
    r.w.NoteRouteDevice(L"personal", L"{dead}");

    r.Advance(2000);            // arms
    r.Advance(2000);            // stability elapsed: commits
    CHECK(r.commits.size() == 1);
    CHECK(r.w.AttemptsOf(L"personal") >= 1);

    // The second attempt waits the configured dwell; later ones wait longer.
    unsigned firstDwell = r.w.DwellMsOf(L"personal");
    for (int i = 0; i < 3; ++i) r.Advance(400000);
    CHECK(r.commits.size() >= 2);
    CHECK(r.w.AttemptsOf(L"personal") >= 2);
    CHECK(r.w.DwellMsOf(L"personal") > firstDwell);   // it is backing off
}

MDXM_TEST_CASE(Failover_HoldsWhileTheAudioGraphIsRebuilding) {
    // When AUDIODG restarts every endpoint is briefly invalid and Present()
    // answers false for all of them at once. Acting on that snapshot moves a
    // route during the one window where no move can succeed (#410).
    Rig r;
    r.w.SetStabilitySeconds(1);
    r.present = { L"{headset}", L"{speakers}" };
    r.Rule(true, { { L"{speakers}", L"Speakers" } });
    r.w.NoteRouteDevice(L"personal", L"{headset}");

    r.w.HoldFor(60000);
    r.present.clear();                 // the graph went away
    r.Advance(10000);
    CHECK(r.commits.empty());          // nothing acted on while held
    CHECK(r.w.HoldRemainingMs() > 0);

    // The hold is a CEILING: the caller releases as soon as the engine is back.
    r.present = { L"{headset}", L"{speakers}" };
    r.w.ReleaseHold();
    CHECK(r.w.HoldRemainingMs() == 0);
    r.Advance(10000);
    CHECK(r.commits.empty());          // the device is back; nothing to do
}

MDXM_TEST_CASE(Failover_FindsARePairedDeviceByName) {
    // A re-paired Bluetooth headset returns under a brand new endpoint id. An
    // allowlist keyed on the id alone silently stops matching -- which is the
    // whole reason both halves are stored.
    Rig r;
    r.w.SetStabilitySeconds(1);
    r.present = { L"{headset}" };
    r.Rule(true, { { L"{old-id-gone}", L"XM5" } });
    r.w.NoteRouteDevice(L"personal", L"{headset}");

    r.present.erase(L"{headset}");
    r.present.insert(L"{new-id}");     // same headset, new id, same name
    r.Advance(2000);                   // arms on the re-paired device
    r.Advance(2000);                   // stability elapsed: commits
    CHECK(r.commits.size() == 1);
    if (!r.commits.empty()) CHECK(r.commits[0].second == L"{new-id}");
}

MDXM_TEST_CASE(Failover_AllowEntryMatchingAndReordering) {
    // The order IS the preference -- "first present entry wins" -- so moving an
    // entry is what decides which replacement gets reached for.
    RouteRule rule;
    rule.routeId = L"personal";
    rule.allow = { { L"{a}", L"Alpha" }, { L"{b}", L"Beta" }, { L"", L"Gamma" } };

    CHECK(AllowEntryMatches(rule.allow[0], L"{a}"));
    CHECK(AllowEntryMatches(rule.allow[0], L"Alpha"));
    // An entry added while its device was switched off has no id at all, and
    // has to stay reachable by name or it can never be removed.
    CHECK(AllowEntryMatches(rule.allow[2], L"Gamma"));
    CHECK(!AllowEntryMatches(rule.allow[1], L"{z}"));

    CHECK(MoveAllowEntry(rule, L"Beta", -1) == 0);
    CHECK(rule.allow[0].name == L"Beta");
    CHECK(MoveAllowEntry(rule, L"Beta", -1) == 0);     // clamps at the top
    CHECK(MoveAllowEntry(rule, L"nobody", 1) == -1);
}

MDXM_TEST_CASE(Failover_DwellBlocksACascadeDownTheList) {
    // Carried over from the decider's tests: one bad unplug must not walk the
    // whole allowlist in a few seconds. The replacement dying straight after a
    // commit is the case — stability passes long before the dwell does.
    Rig r;
    r.w.SetStabilitySeconds(5);
    r.w.SetMinDwellSeconds(30);
    r.w.SetMinGapSeconds(0);
    r.present = { L"{spk}" };
    r.Rule(true, { { L"{spk}", L"Speakers" }, { L"{tv}", L"TV" } });
    r.w.NoteRouteDevice(L"personal", L"{headset}");

    r.Advance(1000);                 // arms on {spk}
    r.Advance(6000);                 // stability elapsed: commits
    CHECK(r.commits.size() == 1);
    if (!r.commits.empty()) CHECK(r.commits[0].second == L"{spk}");

    // The new device dies a second later and {tv} appears.
    r.present.erase(L"{spk}");
    r.present.insert(L"{tv}");
    r.Advance(1000);
    r.Advance(6000);                 // stability would allow it...
    CHECK(r.commits.size() == 1);    // ...but the dwell does not
    r.Advance(30000);                // past the dwell: arms on {tv}
    r.Advance(6000);                 // stability elapsed: commits
    CHECK(r.commits.size() == 2);
    // Guarded, because indexing past a failed size check raises a debug-CRT
    // assertion rather than failing the test.
    if (r.commits.size() == 2) CHECK(r.commits[1].second == L"{tv}");
}
