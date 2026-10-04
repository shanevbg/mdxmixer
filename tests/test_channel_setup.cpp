#include "test_framework.h"
#include "app/channel_setup.h"

using namespace mdxm;

MDXM_TEST_CASE(ChannelId_IsAStableAsciiSlug) {
    std::vector<ChannelConfig> none;
    CHECK(MakeChannelId(none, L"Gaming") == L"gaming");
    CHECK(MakeChannelId(none, L"Game Audio") == L"game-audio");
    CHECK(MakeChannelId(none, L"  Chat!  ") == L"chat");
    CHECK(MakeChannelId(none, L"***") == L"channel");   // never empty
}

MDXM_TEST_CASE(ChannelId_DoesNotCollide) {
    std::vector<ChannelConfig> existing;
    ChannelConfig a; a.id = L"gaming"; a.name = L"Gaming";
    existing.push_back(a);
    CHECK(MakeChannelId(existing, L"Gaming") == L"gaming-2");
    ChannelConfig b; b.id = L"gaming-2";
    existing.push_back(b);
    CHECK(MakeChannelId(existing, L"Gaming") == L"gaming-3");
}

MDXM_TEST_CASE(Sonar_AdoptsTheFourMixChannelsOnly) {
    CHECK(SonarChannelName(L"SteelSeries Sonar - Gaming (SteelSeries Sonar Virtual Audio Device)")
          == L"Gaming");
    CHECK(SonarChannelName(L"SteelSeries Sonar - Chat (SteelSeries Sonar Virtual Audio Device)")
          == L"Chat");
    CHECK(SonarChannelName(L"SteelSeries Sonar - Media (SteelSeries Sonar Virtual Audio Device)")
          == L"Media");
    CHECK(SonarChannelName(L"SteelSeries Sonar - Aux (SteelSeries Sonar Virtual Audio Device)")
          == L"Aux");
    // Stream is where the streaming mix is sent and Microphone is its own
    // chain: adopting either as a channel would be a loop, not a channel.
    CHECK(SonarChannelName(L"SteelSeries Sonar - Stream (SteelSeries Sonar Virtual Audio Device)").empty());
    CHECK(SonarChannelName(L"SteelSeries Sonar - Microphone (SteelSeries Sonar Virtual Audio Device)").empty());
    CHECK(SonarChannelName(L"Headphones (2- WF-1000XM5-1)").empty());
    CHECK(SonarChannelName(L"Speakers (NVIDIA Broadcast)").empty());
}

MDXM_TEST_CASE(Feedback_RefusesTheEndpointTheMixIsPlayingTo) {
    // The default output on this machine IS Sonar Gaming, so this is the
    // mistake one click away at all times.
    CHECK(WouldFeedBack(L"{0.0.0.00000000}.{abc}", L"{0.0.0.00000000}.{ABC}"));   // case
    CHECK(!WouldFeedBack(L"{0.0.0.00000000}.{abc}", L"{0.0.0.00000000}.{def}"));
    CHECK(!WouldFeedBack(L"", L"{0.0.0.00000000}.{abc}"));
    CHECK(!WouldFeedBack(L"{0.0.0.00000000}.{abc}", L""));
}

MDXM_TEST_CASE(Feedback_CoversTheUnsetPersonalOutput) {
    // The dangerous case is not "the configured output": it is NO configured
    // output, where the engine follows the system default — which on this
    // machine is itself a Sonar channel. The caller checks the live device,
    // the configured one and the default fallback; each has to be capable of
    // refusing on its own.
    const wchar_t* sonarGaming = L"{0.0.0.00000000}.{gaming}";
    CHECK(WouldFeedBack(sonarGaming, L"{0.0.0.00000000}.{gaming}"));   // default fallback
    CHECK(!WouldFeedBack(sonarGaming, L""));                           // nothing to compare
}
