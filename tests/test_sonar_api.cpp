#include "test_framework.h"
#include "routing/sonar_api.h"

using namespace mdxm;

// The two expensive facts in the Sonar write path, pinned.
//
// Both were established in MDropDX12 against a live GG, and both fail the same
// way when wrong: HTTP 400 with the fader unmoved, which reads as Sonar
// refusing to be driven from outside its own app rather than as a bad URL.
// That misreading cost a whole investigation over there (mdx12 forgejo#63
// concluded "Sonar refuses"; it was a typo). A unit test is the cheap way to
// stop a future tidy-up from re-deriving it.

MDXM_TEST_CASE(Sonar_WritePathPutsTheSliderBeforeTheChannel) {
    // Channel-first — /streamer/aux/monitoring/... — returns 400, and so does
    // a nonsense channel, so the error distinguishes nothing.
    CHECK(SonarWritePath(L"aux", true, true, L"Volume", L"0.02") ==
          L"/volumeSettings/streamer/monitoring/aux/Volume/0.02");
    CHECK(SonarWritePath(L"aux", false, true, L"Volume", L"1.00") ==
          L"/volumeSettings/streamer/streaming/aux/Volume/1.00");
}

MDXM_TEST_CASE(Sonar_MasterIsWrittenSingularAndReadPlural) {
    // Read as `masters`, written as `master`. One letter.
    CHECK(SonarWritePath(L"masters", true, true, L"Volume", L"0.42") ==
          L"/volumeSettings/streamer/monitoring/master/Volume/0.42");
}

MDXM_TEST_CASE(Sonar_ClassicModeHasNoSliderSegment) {
    CHECK(SonarWritePath(L"media", true, false, L"Volume", L"0.50") ==
          L"/volumeSettings/classic/media/Volume/0.50");
    // The singular applies there too, on the same reasoning — unverified,
    // because this machine runs streamer mode.
    CHECK(SonarWritePath(L"masters", false, false, L"isMuted", L"true") ==
          L"/volumeSettings/classic/master/isMuted/true");
}

MDXM_TEST_CASE(Sonar_MuteUsesCamelCaseIsMuted) {
    // `Mute` returns 404, and it does not match `Volume`'s capitalisation.
    // The API is simply inconsistent here.
    CHECK(SonarWritePath(L"game", false, true, L"isMuted", L"false") ==
          L"/volumeSettings/streamer/streaming/game/isMuted/false");
}

MDXM_TEST_CASE(Sonar_ChannelIdPrefixRoutesWritesToSonar) {
    CHECK(SonarKeyFromChannelId(L"sonar:aux") == L"aux");
    CHECK(SonarKeyFromChannelId(L"sonar:chatRender") == L"chatRender");
    // An engine channel must NOT be mistaken for one of Sonar's: that would
    // send a local fader's write to a server that has never heard of it.
    CHECK(SonarKeyFromChannelId(L"sonar").empty());
    CHECK(SonarKeyFromChannelId(L"mic").empty());
    CHECK(SonarKeyFromChannelId(L"").empty());
}
