// test_vban_peers.cpp — who is subscribed, who is allowed what, and what the
// sender should therefore be doing (spec §3.3, §4).
//
// All pure: addresses are opaque integers and time is a tick the caller passes
// in, so the security-shaped decisions in here are decided by a table of cases
// rather than by whatever a real network happened to do during a manual test.
#include "test_framework.h"
#include "net/vban_peers.h"

using namespace mdxm::vban;

MDXM_TEST_CASE(VbanPeers_PingCreatesRenewsExpires) {
    PeerTable t;
    PeerKey k{ 0x0A000001, 50000 };
    Peer* p = t.Ping(k, 1000);
    CHECK(p && t.All().size() == 1);
    t.Ping(k, 5000);                       // renew, not duplicate
    CHECK(t.All().size() == 1);
    t.Expire(14999);                       // 9999 ms since last ping: still alive
    CHECK(t.All().size() == 1);
    t.Expire(15001);                       // 10001 ms: gone, auth and all
    CHECK(t.All().empty());
}

MDXM_TEST_CASE(VbanPeers_TableFullEviction) {
    // Review Focus #3: eight slots and an unauthenticated network. A scanner, a
    // misconfigured Voicemeeter, anything that pings must not be able to push
    // the one device that matters out of the table.
    PeerTable t;
    for (uint16_t i = 0; i < 8; ++i)
        t.Ping(PeerKey{ 0x0A000001, (uint16_t)(50000 + i) }, 1000);
    CHECK(t.All().size() == 8);
    t.Find(PeerKey{ 0x0A000001, 50003 })->auth = AuthState::Authorized;   // the phone
    t.Ping(PeerKey{ 0x0A000001, 50001 }, 2000);   // leaves 50000 the oldest

    // A ninth pinger with nothing expired: the oldest UNAUTHENTICATED entry
    // goes, never the authorized one.
    Peer* np = t.Ping(PeerKey{ 0x0B000001, 60000 }, 3000);
    CHECK(np != nullptr);
    CHECK(t.Find(PeerKey{ 0x0A000001, 50000 }) == nullptr);   // evicted
    CHECK(t.Find(PeerKey{ 0x0A000001, 50003 }) != nullptr);   // phone survives

    // With every slot authorized there is nothing an unauthenticated newcomer
    // may take: it is refused an entry. Its identification reply is sent
    // anyway -- that is stateless and happens in the server, not here.
    for (uint16_t i = 1; i < 8; ++i)
        t.Find(PeerKey{ 0x0A000001, (uint16_t)(50000 + i) })->auth = AuthState::Authorized;
    t.Find(PeerKey{ 0x0B000001, 60000 })->auth = AuthState::Authorized;
    CHECK(t.Ping(PeerKey{ 0x0C000001, 60001 }, 4000) == nullptr);
}

MDXM_TEST_CASE(VbanPeers_ExpiredSlotIsTakenBeforeAnyLiveOne) {
    // Expiry comes first in the eviction order, so a table that has gone stale
    // does not start refusing live peers.
    PeerTable t;
    for (uint16_t i = 0; i < 8; ++i)
        t.Ping(PeerKey{ 0x0A000001, (uint16_t)(50000 + i) }, 1000);
    for (uint16_t i = 0; i < 8; ++i)
        t.Find(PeerKey{ 0x0A000001, (uint16_t)(50000 + i) })->auth = AuthState::Authorized;
    // 20 s later every entry is stale, authorized or not: a newcomer gets in
    // even though nothing has called Expire.
    CHECK(t.Ping(PeerKey{ 0x0C000001, 60001 }, 21000) != nullptr);
}

MDXM_TEST_CASE(VbanPeers_PointersSurviveATableFillingUp) {
    // Ping() and Find() hand out pointers INTO the table, and the server uses
    // one to record an authorization. If the backing store reallocated as peers
    // arrived, that write would land in freed memory -- so the table reserves
    // its whole capacity up front and never grows past it.
    PeerTable t;
    Peer* first = t.Ping(PeerKey{ 0x0A000001, 50000 }, 1000);
    first->deviceId = L"phone";
    for (uint16_t i = 1; i < 8; ++i)
        t.Ping(PeerKey{ 0x0A000001, (uint16_t)(50000 + i) }, 1000);
    // The pointer from before seven more arrivals still addresses that peer.
    CHECK(first->deviceId == L"phone");
    CHECK(first == t.Find(PeerKey{ 0x0A000001, 50000 }));
}

MDXM_TEST_CASE(VbanPeers_ATickThatDoesNotAdvanceExpiresNothing) {
    // Unsigned arithmetic on a tick that went backwards would underflow to an
    // enormous age and expire every peer at once -- a stream that drops for no
    // reason a log could explain. GetTickCount64 does not go backwards, but a
    // caller holding a stale tick is a bug this must not amplify.
    PeerTable t;
    t.Ping(PeerKey{ 1, 1 }, 10000);
    t.Expire(9000);                        // earlier than the last ping
    CHECK(t.All().size() == 1);
    t.Expire(10000);                       // the same instant
    CHECK(t.All().size() == 1);
}

MDXM_TEST_CASE(VbanPeers_EntitlementMatrixPhase1) {
    Peer anon;  anon.auth = AuthState::None;
    Peer authd; authd.auth = AuthState::Authorized;
    // PIN set, openSubscribe off -- the normal posture: only an authorized
    // device hears anything.
    CHECK(!AudioEntitled(anon,  true, false));
    CHECK( AudioEntitled(authd, true, false));
    // openSubscribe on: any pinger hears AUDIO, which is the whole grant --
    // it buys no control, and that is what makes it safe to offer at all.
    CHECK( AudioEntitled(anon,  true, true));
    CHECK( AudioEntitled(anon,  false, true));
    // PIN empty, open off: answers discovery, serves nobody. A legal posture
    // and the default one.
    CHECK(!AudioEntitled(anon,  false, false));
    // And with no PIN the remembered devices are INERT (spec §4) -- clearing
    // the PIN is not a half-measure, it withdraws the grant it issued.
    CHECK(!AudioEntitled(authd, false, false));
    // audio=0 is how a phone keeps a control session without the stream.
    authd.audioOn = false;
    CHECK(!AudioEntitled(authd, true, false));

    // Frames are opt-in per peer AND authorized-only: a picture of the screens
    // is not something an anonymous pinger gets for asking.
    Peer f; f.auth = AuthState::Authorized; f.framesOn = true;
    CHECK( FramesEntitled(f, true));
    CHECK(!FramesEntitled(f, false));      // no PIN: no frames either
    f.framesOn = false;
    CHECK(!FramesEntitled(f, true));
    Peer anonF; anonF.framesOn = true;
    CHECK(!FramesEntitled(anonF, true));
    // openSubscribe does NOT extend to frames, for the reason above.
    CHECK(!FramesEntitled(anonF, false));
}

MDXM_TEST_CASE(VbanAuth_Decide) {
    // The whole table, one case per cell, in the order the decision has to be
    // made: a lockout outranks everything (otherwise a brute-force attempt that
    // happens to guess right is rewarded), a denial outranks a correct PIN
    // (saying no has to mean no), and only then does the PIN decide.
    CHECK(DecideAuth(L"42", L"42", true,  false, false) == AuthOutcome::Ok);
    CHECK(DecideAuth(L"42", L"42", false, false, false) == AuthOutcome::Pending);
    CHECK(DecideAuth(L"42", L"41", false, false, false) == AuthOutcome::BadPin);
    CHECK(DecideAuth(L"42", L"42", false, true,  false) == AuthOutcome::Denied);
    CHECK(DecideAuth(L"42", L"42", true,  false, true)  == AuthOutcome::Locked);
    CHECK(DecideAuth(L"42", L"41", false, false, true)  == AuthOutcome::Locked);
    // A denied device stays denied even once authorized: the two cannot both be
    // true in practice, and if they are, the refusal is the safe reading.
    CHECK(DecideAuth(L"42", L"42", true,  true,  false) == AuthOutcome::Denied);
    // No PIN configured. The caller never gets here (empty PIN kills TXT
    // entirely), but if it did, an empty request must NOT read as a match --
    // that would make "no PIN" mean "no password required".
    CHECK(DecideAuth(L"", L"",  false, false, false) == AuthOutcome::BadPin);
    CHECK(DecideAuth(L"", L"x", false, false, false) == AuthOutcome::BadPin);
    // The PIN is compared whole: no prefix match, no case folding.
    CHECK(DecideAuth(L"4242", L"42",   false, false, false) == AuthOutcome::BadPin);
    CHECK(DecideAuth(L"abc",  L"ABC",  false, false, false) == AuthOutcome::BadPin);
}

MDXM_TEST_CASE(VbanAuth_StrikesLockoutAndRotation) {
    AuthStrikes s;
    CHECK(!s.Locked(7, 1000));
    s.Strike(7, 1000);
    s.Strike(7, 2000);
    CHECK(!s.Locked(7, 3000));               // two wrong guesses is not a lockout
    s.Strike(7, 3000);                       // the third is
    CHECK(s.Locked(7, 3001));
    CHECK(s.Locked(7, 62999));
    CHECK(!s.Locked(7, 63001));              // 60 s after the third strike
    // A different IP is unaffected -- one phone mistyping must not lock out
    // another device.
    CHECK(!s.Locked(8, 3001));

    // Strikes key on the IP ALONE, which is the reason this is not a field on
    // the peer: a peer entry is keyed by ip:port and expires in ten seconds, so
    // an attacker rotating source ports would get unlimited fresh attempts.
    AuthStrikes s2;
    s2.Strike(9, 0);
    s2.Strike(9, 1);
    s2.Strike(9, 2);
    CHECK(s2.Locked(9, 10));
    s2.Clear(9);                             // a successful auth forgives
    CHECK(!s2.Locked(9, 10));
}

MDXM_TEST_CASE(VbanAuth_StrikeTableCannotBeFlushedByScanning) {
    // The table is small and fixed, so an attacker who can make entries could
    // try to push their own lockout out of it by striking from other addresses.
    // Replacement takes the OLDEST row, and a locked row is not the oldest while
    // its lockout is live.
    AuthStrikes s;
    for (int i = 0; i < 3; ++i) s.Strike(100, 1000);     // 100 is locked
    CHECK(s.Locked(100, 1500));
    // Eight more addresses, all newer.
    for (uint32_t ip = 200; ip < 208; ++ip) s.Strike(ip, 2000 + ip);
    CHECK(s.Locked(100, 2500));
}

MDXM_TEST_CASE(VbanPeers_VbanWanted) {
    CHECK(!VbanWanted(false, 5, true, true));      // listener off: never
    CHECK( VbanWanted(true, 1, false, false));
    CHECK(!VbanWanted(true, 0, false, false));     // nobody asked: nothing sent
    CHECK( VbanWanted(true, 0, true, true));       // alwaysStream + a target
    CHECK(!VbanWanted(true, 0, true, false));      // target empty = inert (spec §4)
}

MDXM_TEST_CASE(VbanPeers_CountAudioEntitled) {
    PeerTable t;
    t.Ping(PeerKey{ 1, 1 }, 0);
    t.Ping(PeerKey{ 1, 2 }, 0);
    t.Find(PeerKey{ 1, 2 })->auth = AuthState::Authorized;
    CHECK(t.CountAudioEntitled(true, false) == 1);
    CHECK(t.CountAudioEntitled(true, true)  == 2);
    CHECK(t.CountAudioEntitled(false, false) == 0);
}
