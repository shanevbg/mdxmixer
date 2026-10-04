#include "test_framework.h"
#include "routing/reconcile.h"

using namespace mdxm;

static RouteIntent Intent(const wchar_t* exe, const wchar_t* ch, const wchar_t* ep) {
    return { exe, ch, ep };
}
static SessionRoute Live(const wchar_t* exe, unsigned long pid, const wchar_t* cur) {
    return { exe, pid, cur };
}

MDXM_TEST_CASE(Reconcile_AppliesAStoredAssignmentWhenTheAppAppears) {
    // The case fj#1 is about: the assignment was made while the app was shut,
    // so Windows knows nothing about it until the app plays.
    std::vector<RouteIntent> intents = { Intent(L"C:\\x\\spotify.exe", L"media", L"{ep-media}") };
    std::vector<SessionRoute> live = { Live(L"C:\\x\\spotify.exe", 1234, L"") };
    auto fixes = PlanRouteFixes(intents, live);
    CHECK(fixes.size() == 1);
    CHECK(fixes[0].pid == 1234);
    CHECK(fixes[0].endpointId == L"{ep-media}");
    CHECK(fixes[0].channelId == L"media");
}

MDXM_TEST_CASE(Reconcile_LeavesAppsNobodyAssignedAlone) {
    // A mixer that grabbed every process that happened to be playing would be
    // moving other people's audio around.
    std::vector<RouteIntent> intents = { Intent(L"C:\\x\\spotify.exe", L"media", L"{ep-media}") };
    std::vector<SessionRoute> live = {
        Live(L"C:\\x\\firefox.exe", 22, L""),
        Live(L"C:\\x\\game.exe", 33, L"{ep-somewhere}"),
    };
    CHECK(PlanRouteFixes(intents, live).empty());
}

MDXM_TEST_CASE(Reconcile_WritesNothingWhenTheRouteIsAlreadyRight) {
    // The reconcile runs on every session arrival, so the common case — the
    // app is already where it belongs — has to cost nothing.
    std::vector<RouteIntent> intents = { Intent(L"C:\\x\\spotify.exe", L"media", L"{ep-media}") };
    std::vector<SessionRoute> live = { Live(L"C:\\x\\spotify.exe", 1234, L"{ep-media}") };
    CHECK(PlanRouteFixes(intents, live).empty());
    // Windows is inconsistent about the case of an image path, and two
    // spellings of one path are one app.
    std::vector<SessionRoute> shouty = { Live(L"C:\X\SPOTIFY.EXE", 1234, L"{EP-MEDIA}") };
    CHECK(PlanRouteFixes(intents, shouty).empty());
}

MDXM_TEST_CASE(Reconcile_RepairsAnAppLeftOnADeadEndpoint) {
    // A re-paired or reinstalled cable comes back under a new endpoint id. The
    // channel recovers by name, but Windows still holds the old id for every
    // app on it — the third gap in fj#1, and the one nothing noticed.
    std::vector<RouteIntent> intents = { Intent(L"C:\\x\\game.exe", L"game", L"{ep-new}") };
    std::vector<SessionRoute> live = { Live(L"C:\\x\\game.exe", 77, L"{ep-old-and-gone}") };
    auto fixes = PlanRouteFixes(intents, live);
    CHECK(fixes.size() == 1);
    CHECK(fixes[0].endpointId == L"{ep-new}");
}

MDXM_TEST_CASE(Reconcile_SkipsAChannelWithNowhereToSendYet) {
    // A channel whose cable has not resolved has no endpoint. Writing an empty
    // route would clear whatever the app had, which is worse than waiting.
    std::vector<RouteIntent> intents = { Intent(L"C:\\x\\spotify.exe", L"media", L"") };
    std::vector<SessionRoute> live = { Live(L"C:\\x\\spotify.exe", 1234, L"{ep-old}") };
    CHECK(PlanRouteFixes(intents, live).empty());
}

MDXM_TEST_CASE(Reconcile_HandlesEveryPidOfOneApp) {
    // Chromium-shaped apps play from several processes at once; the assignment
    // is per exe and has to reach all of them.
    std::vector<RouteIntent> intents = { Intent(L"C:\\x\\chrome.exe", L"media", L"{ep-media}") };
    std::vector<SessionRoute> live = {
        Live(L"C:\\x\\chrome.exe", 10, L""),
        Live(L"C:\\x\\chrome.exe", 11, L"{ep-media}"),   // this one is fine already
        Live(L"C:\\x\\chrome.exe", 12, L"{ep-other}"),
    };
    auto fixes = PlanRouteFixes(intents, live);
    CHECK(fixes.size() == 2);
    CHECK(fixes[0].pid == 10);
    CHECK(fixes[1].pid == 12);
}
