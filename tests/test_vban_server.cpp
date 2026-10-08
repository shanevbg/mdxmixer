// test_vban_server.cpp — the socket, over real loopback UDP.
//
// No audio device is involved: the ring is filled by hand, so this runs in the
// ordinary headless suite rather than behind --audio. What it covers is the part
// that cannot be reasoned about from the pure layers -- that a ping really does
// produce a reply, that audio really does come out at the right cadence, that
// nothing comes out when nobody asked, and that a port already in use is
// reported rather than swallowed.
#include "test_framework.h"
#include "net/vban_server.h"
#include "net/vban_protocol.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <chrono>
#include <thread>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

using namespace mdxm;
using namespace mdxm::vban;

namespace {

// Not 6980: a running mdxmixer on this machine must not answer these tests, and
// these tests must not take the port from it.
constexpr int kTestPort = 46980;

// Each case calls this rather than relying on an earlier case having run:
// WSACleanup is never called, so one initialisation serves the process.
void EnsureWsa() {
    static bool done = false;
    if (done) return;
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);
    done = true;
}

SOCKET MakeClient() {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(s, (sockaddr*)&a, (int)sizeof a);          // port 0: an ephemeral port
    DWORD tmo = 3000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char*)&tmo, (int)sizeof tmo);
    return s;
}

void SendPing0(SOCKET s, uint32_t txn) {
    Header h = {};
    h.vban = kMagic;
    h.format_SR = kProtoService;
    h.format_nbs = kServiceFnPing0;
    h.format_nbc = kServiceIdentification;
    FillStreamName(h.streamname, "VBAN Service");
    h.nuFrame = txn;
    sockaddr_in to = {};
    to.sin_family = AF_INET;
    to.sin_port = htons((unsigned short)kTestPort);
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sendto(s, (const char*)&h, (int)sizeof h, 0, (sockaddr*)&to, (int)sizeof to);
}

void SendTxt(SOCKET s, const std::string& utf8) {
    uint8_t buf[mdxm::vban::kMaxPacket];
    char name[mdxm::vban::kStreamNameSize];
    FillStreamName(name, "mdxmixer");
    static uint32_t seq = 0;
    const size_t n = BuildTxtPacket(buf, name, seq++, utf8);
    sockaddr_in to = {};
    to.sin_family = AF_INET;
    to.sin_port = htons((unsigned short)kTestPort);
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sendto(s, (const char*)buf, (int)n, 0, (sockaddr*)&to, (int)sizeof to);
}

// Port-parameterised variants, for the rebind test which moves the server to a
// different port mid-run.
void SendPing0To(SOCKET s, uint32_t txn, int port) {
    Header h = {};
    h.vban = kMagic;
    h.format_SR = kProtoService;
    h.format_nbs = kServiceFnPing0;
    h.format_nbc = kServiceIdentification;
    FillStreamName(h.streamname, "VBAN Service");
    h.nuFrame = txn;
    sockaddr_in to = {};
    to.sin_family = AF_INET;
    to.sin_port = htons((unsigned short)port);
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sendto(s, (const char*)&h, (int)sizeof h, 0, (sockaddr*)&to, (int)sizeof to);
}

void SendTxtTo(SOCKET s, const std::string& utf8, int port) {
    uint8_t buf[mdxm::vban::kMaxPacket];
    char name[mdxm::vban::kStreamNameSize];
    FillStreamName(name, "mdxmixer");
    static uint32_t seq = 0;
    const size_t n = BuildTxtPacket(buf, name, seq++, utf8);
    sockaddr_in to = {};
    to.sin_family = AF_INET;
    to.sin_port = htons((unsigned short)port);
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sendto(s, (const char*)buf, (int)n, 0, (sockaddr*)&to, (int)sizeof to);
}

// Collect every TXT record that arrives within the window, splitting the
// newline-joined chunks back into records. Used by the large-reply test.
std::vector<std::string> RecvAllTxtRecords(SOCKET s, int timeoutMs = 2000) {
    DWORD tmo = 400;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char*)&tmo, (int)sizeof tmo);
    std::vector<std::string> records;
    uint8_t buf[2048];
    const ULONGLONG until = GetTickCount64() + (ULONGLONG)timeoutMs;
    while (GetTickCount64() < until) {
        const int n = recvfrom(s, (char*)buf, (int)sizeof buf, 0, nullptr, nullptr);
        if (n <= 0) continue;
        std::string text;
        if (!ParseTxt(ParsePacket(buf, (size_t)n), &text)) continue;
        size_t start = 0;
        while (start <= text.size()) {
            const size_t nl = text.find('\n', start);
            records.push_back(text.substr(start, nl == std::string::npos ? std::string::npos
                                                                          : nl - start));
            if (nl == std::string::npos) break;
            start = nl + 1;
        }
    }
    return records;
}

// The next TXT record, skipping everything else -- an authorized peer has audio
// arriving at ~188 packets a second, and the ping reply lands first.
std::string RecvTxt(SOCKET s, int timeoutMs = 3000) {
    DWORD tmo = (DWORD)timeoutMs;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char*)&tmo, (int)sizeof tmo);
    uint8_t buf[2048];
    const ULONGLONG until = GetTickCount64() + (ULONGLONG)timeoutMs;
    while (GetTickCount64() < until) {
        const int n = recvfrom(s, (char*)buf, (int)sizeof buf, 0, nullptr, nullptr);
        if (n <= 0) break;
        const Parsed p = ParsePacket(buf, (size_t)n);
        std::string text;
        if (ParseTxt(p, &text)) return text;
    }
    return {};
}

} // namespace

MDXM_TEST_CASE(Vban_Ping0GetsIdentificationReply) {
    EnsureWsa();
    RingBuffer ring(48000 * 2);
    VbanConfig cfg; cfg.enabled = true; cfg.port = kTestPort;
    VbanServer srv;
    std::wstring err;
    CHECK(srv.Start(cfg, &ring, 48000, {}, &err));
    SOCKET c = MakeClient();
    SendPing0(c, 1234);

    uint8_t buf[2048];
    sockaddr_in from; int flen = (int)sizeof from;
    int n = recvfrom(c, (char*)buf, (int)sizeof buf, 0, (sockaddr*)&from, &flen);
    CHECK(n == (int)(kHeaderSize + sizeof(Ping0)));
    if (n > 0) {
        Parsed p = ParsePacket(buf, (size_t)n);
        CHECK(p.valid && p.proto == kProtoService);
        CHECK(p.hdr.format_nbs == (kServiceFnPing0 | kServiceReplyBit));
        CHECK(p.hdr.nuFrame == 1234);               // the transaction id, echoed
        Ping0 id; memcpy(&id, p.data, sizeof id);
        // What we advertise, which is how the phone learns what we can do
        // without asking our version.
        CHECK((id.bitFeature & kFeatureAudio) != 0);
        CHECK((id.bitFeature & kFeatureTxt) != 0);
        CHECK((id.bitFeature & kFeatureFrame) != 0);
        CHECK((id.bitType & kDeviceTransmitter) != 0);
        CHECK(std::string(id.deviceName) == "mdxmixer");
        CHECK(id.preferredRate == 48000);
    }
    closesocket(c);
    srv.Stop();
}

MDXM_TEST_CASE(Vban_OpenSubscribePingerReceivesPacedAudio) {
    EnsureWsa();
    RingBuffer ring(48000 * 2);
    // A second of a known tone, so what this measures is pacing and not
    // starvation.
    std::vector<float> tone(48000 * 2, 0.25f);
    ring.Write(tone.data(), 48000);
    VbanConfig cfg; cfg.enabled = true; cfg.port = kTestPort;
    cfg.openSubscribe = true;                 // the phase-1 entitlement path
    VbanServer srv; std::wstring err;
    CHECK(srv.Start(cfg, &ring, 48000, {}, &err));
    SOCKET c = MakeClient();
    SendPing0(c, 1);

    uint8_t buf[2048];
    sockaddr_in from; int flen = (int)sizeof from;
    int audioPackets = 0;
    uint32_t firstSeq = 0, lastSeq = 0;
    int16_t lastSample = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(300)) {
        int n = recvfrom(c, (char*)buf, (int)sizeof buf, 0, (sockaddr*)&from, &flen);
        if (n <= 0) break;
        Parsed p = ParsePacket(buf, (size_t)n);
        if (!p.valid || p.proto != kProtoAudio) continue;   // the ping reply
        CHECK(p.hdr.format_nbs == 255 && p.hdr.format_nbc == 1);
        CHECK((p.hdr.format_SR & 0x1F) == 3);
        if (audioPackets == 0) firstSeq = p.hdr.nuFrame;
        lastSeq = p.hdr.nuFrame;
        memcpy(&lastSample, p.data, 2);
        ++audioPackets;
    }
    // 300 ms at 5.33 ms a packet is about 56. The bounds are wide on purpose:
    // what they catch is a flood (no pacing at all) or a trickle (a sender that
    // only wakes on something else), not a few packets either way.
    CHECK(audioPackets > 25 && audioPackets < 90);
    CHECK(lastSeq - firstSeq == (uint32_t)(audioPackets - 1));   // contiguous
    CHECK(lastSample > 7000 && lastSample < 9000);               // 0.25f * 32767
    closesocket(c);
    VbanStatus st = srv.Status();
    CHECK(st.emitting);
    CHECK(st.peers == 1);
    CHECK(st.sent >= (uint64_t)audioPackets);
    CHECK(st.starved == 0);        // a second of tone against 300 ms of sending
    srv.Stop();
}

MDXM_TEST_CASE(Vban_BindFailureIsReported) {
    // Review Focus #4, and the lesson from MdnsDiscovery binding a fixed port
    // and swallowing the failure: a listener that is not listening has to say
    // so, or the symptom is a phone that cannot connect and no way to find out
    // why.
    EnsureWsa();
    RingBuffer ring(48000 * 2);
    VbanConfig cfg; cfg.enabled = true; cfg.port = kTestPort;
    VbanServer a, b;
    std::wstring errA, errB;
    CHECK(a.Start(cfg, &ring, 48000, {}, &errA));
    CHECK(errA.empty());
    CHECK(!b.Start(cfg, &ring, 48000, {}, &errB));
    CHECK(!errB.empty());
    CHECK(!b.Status().lastError.empty());
    CHECK(!b.Running());
    a.Stop();
}

MDXM_TEST_CASE(Vban_NoSubscriberNoPackets) {
    // The whole on-demand rule in one case: a listener that is up, a ring with
    // audio in it, and nobody asking -- so nothing goes out (spec §1).
    EnsureWsa();
    RingBuffer ring(48000 * 2);
    std::vector<float> tone(9600 * 2, 0.5f);
    ring.Write(tone.data(), 9600);
    VbanConfig cfg; cfg.enabled = true; cfg.port = kTestPort;   // open=false, no pings
    VbanServer srv; std::wstring err;
    CHECK(srv.Start(cfg, &ring, 48000, {}, &err));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    VbanStatus st = srv.Status();
    CHECK(st.sent == 0);
    CHECK(!st.emitting);
    CHECK(!srv.Wanted());          // what the app layer polls to gate the sink
    CHECK(st.peers == 0);
    srv.Stop();
}

MDXM_TEST_CASE(Vban_AlwaysStreamNeedsATargetToBeWanted) {
    // alwaysStream exists for receivers that never ping. With no target it is
    // inert rather than broadcasting at the network (spec §4).
    EnsureWsa();
    RingBuffer ring(48000 * 2);
    VbanConfig cfg; cfg.enabled = true; cfg.port = kTestPort;
    cfg.alwaysStream = true;                      // ... but no target
    VbanServer srv; std::wstring err;
    CHECK(srv.Start(cfg, &ring, 48000, {}, &err));
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    CHECK(!srv.Wanted());
    CHECK(srv.Status().sent == 0);
    srv.Stop();
}

MDXM_TEST_CASE(VbanTxt_WrongPinStrikesAndThenLocksOut) {
    EnsureWsa();
    RingBuffer ring(48000 * 2);
    VbanConfig cfg; cfg.enabled = true; cfg.port = kTestPort; cfg.pin = L"42";
    VbanServer srv; std::wstring err;
    CHECK(srv.Start(cfg, &ring, 48000, {}, &err));
    SOCKET c = MakeClient();
    SendPing0(c, 1);

    SendTxt(c, "MDXM_AUTH|pin=9|device=d1|name=P");
    CHECK(RecvTxt(c) == "MDXM_AUTHSTATE|ok=0|err=badpin");
    SendTxt(c, "MDXM_AUTH|pin=9|device=d1|name=P");
    CHECK(RecvTxt(c) == "MDXM_AUTHSTATE|ok=0|err=badpin");
    SendTxt(c, "MDXM_AUTH|pin=9|device=d1|name=P");
    CHECK(RecvTxt(c) == "MDXM_AUTHSTATE|ok=0|err=badpin");
    // Three wrong guesses, so the RIGHT pin is now refused too: a brute-force
    // attempt that eventually lands must not be rewarded for persistence.
    SendTxt(c, "MDXM_AUTH|pin=42|device=d1|name=P");
    CHECK(RecvTxt(c) == "MDXM_AUTHSTATE|ok=0|err=locked");
    closesocket(c);
    srv.Stop();
}

MDXM_TEST_CASE(VbanTxt_PendingThenApprovedThenDispatch) {
    EnsureWsa();
    RingBuffer ring(48000 * 2);
    VbanConfig cfg; cfg.enabled = true; cfg.port = kTestPort; cfg.pin = L"42";
    std::vector<std::wstring> pendings, approved;
    VbanServer::Callbacks cb;
    // Stands in for the marshalled protocol handler. It answers MDXM_VBAN with a
    // state record and everything else with a pong, because that is what the real
    // dispatcher does -- a fake that answered everything identically would make
    // the per-peer echo below a test of the fake rather than of the server.
    cb.dispatch = [](const std::wstring& m) {
        if (m.rfind(L"MDXM_VBAN", 0) == 0)
            return std::vector<std::wstring>{ L"MDXM_VBANSTATE|on=1|emitting=1" };
        return std::vector<std::wstring>{ L"MDXM_PONG|version=1" };
    };
    cb.onAuthPending = [&](const std::wstring& id, const std::wstring&) {
        pendings.push_back(id);
    };
    cb.onAuthorized = [&](const std::wstring& id, const std::wstring&) {
        approved.push_back(id);
    };
    VbanServer srv; std::wstring err;
    CHECK(srv.Start(cfg, &ring, 48000, cb, &err));
    SOCKET c = MakeClient();
    SendPing0(c, 1);

    // An unauthenticated peer's ordinary verb is DROPPED, not dispatched: the
    // whole mixer surface is behind this gate.
    SendTxt(c, "MDXM_PING");
    CHECK(RecvTxt(c, 400).empty());

    // Right PIN, unknown device: pending, and the PC is asked once. The phone
    // re-sends while it waits, and that must not raise a second prompt.
    SendTxt(c, "MDXM_AUTH|pin=42|device=d1|name=Pixel");
    CHECK(RecvTxt(c) == "MDXM_AUTHSTATE|pending=1");
    SendTxt(c, "MDXM_AUTH|pin=42|device=d1|name=Pixel");
    CHECK(RecvTxt(c) == "MDXM_AUTHSTATE|pending=1");
    CHECK(pendings.size() == 1);
    CHECK(approved.empty());

    srv.AuthorizationResult(L"d1", true);
    SendTxt(c, "MDXM_AUTH|pin=42|device=d1|name=Pixel");
    CHECK(RecvTxt(c) == "MDXM_AUTHSTATE|ok=1");
    CHECK(approved.size() == 1 && approved[0] == L"d1");

    // Authorized: the mixer surface answers, and audio is now flowing.
    SendTxt(c, "MDXM_PING");
    CHECK(RecvTxt(c) == "MDXM_PONG|version=1");
    // Push is the one documented exception -- there is no fan-out seam for it yet.
    SendTxt(c, "MDXM_SUBSCRIBE|1");
    CHECK(RecvTxt(c) == "MDXM_ERR|msg=subscribe is pipe-only");
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    CHECK(srv.Wanted());

    // audio=0 keeps the control session and stops the stream: what a phone sends
    // when the user closes the player but keeps the mixer on screen.
    SendTxt(c, "MDXM_VBAN|audio=0");
    const std::string echo = RecvTxt(c);
    CHECK(echo.rfind("MDXM_VBANSTATE|", 0) == 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    CHECK(!srv.Wanted());
    closesocket(c);
    srv.Stop();
}

MDXM_TEST_CASE(VbanTxt_DenyIsRememberedUntilRestart) {
    EnsureWsa();
    RingBuffer ring(48000 * 2);
    VbanConfig cfg; cfg.enabled = true; cfg.port = kTestPort; cfg.pin = L"42";
    VbanServer srv; std::wstring err;
    CHECK(srv.Start(cfg, &ring, 48000, {}, &err));
    SOCKET c = MakeClient();
    SendPing0(c, 1);
    SendTxt(c, "MDXM_AUTH|pin=42|device=bad|name=Unknown");
    CHECK(RecvTxt(c) == "MDXM_AUTHSTATE|pending=1");
    srv.AuthorizationResult(L"bad", false);
    // A refusal has to be terminal, or the phone sits in "waiting for approval"
    // for ever and the user is asked again on every resend.
    SendTxt(c, "MDXM_AUTH|pin=42|device=bad|name=Unknown");
    CHECK(RecvTxt(c) == "MDXM_AUTHSTATE|ok=0|err=denied");
    closesocket(c);
    srv.Stop();
}

MDXM_TEST_CASE(VbanTxt_EmptyPinDropsEveryRecordIncludingAuth) {
    // With no PIN there is no way to authenticate, so TXT is dead -- and that
    // includes MDXM_AUTH itself. Discovery and (opened) audio still work: this is
    // "serve, but take no orders", which is the default posture (spec §4).
    EnsureWsa();
    RingBuffer ring(48000 * 2);
    std::vector<float> tone(48000 * 2, 0.25f);
    ring.Write(tone.data(), 48000);
    VbanConfig cfg; cfg.enabled = true; cfg.port = kTestPort;
    cfg.openSubscribe = true;                  // audio yes, control no
    bool dispatched = false;
    VbanServer::Callbacks cb;
    cb.dispatch = [&](const std::wstring&) {
        dispatched = true;
        return std::vector<std::wstring>{ L"MDXM_OK" };
    };
    VbanServer srv; std::wstring err;
    CHECK(srv.Start(cfg, &ring, 48000, cb, &err));
    SOCKET c = MakeClient();
    SendPing0(c, 1);
    SendTxt(c, "MDXM_AUTH|pin=|device=d1|name=P");
    SendTxt(c, "MDXM_PING");
    CHECK(RecvTxt(c, 400).empty());            // no TXT answer of any kind
    CHECK(!dispatched);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    CHECK(srv.Wanted());                       // but the audio stream is live
    closesocket(c);
    srv.Stop();
}

MDXM_TEST_CASE(VbanTxt_WrongStreamNameIsDropped) {
    // A receiver matches on stream name, and so do we: a record addressed to
    // something else on this network is not ours to act on.
    EnsureWsa();
    RingBuffer ring(48000 * 2);
    VbanConfig cfg; cfg.enabled = true; cfg.port = kTestPort; cfg.pin = L"42";
    bool dispatched = false;
    VbanServer::Callbacks cb;
    cb.dispatch = [&](const std::wstring&) {
        dispatched = true;
        return std::vector<std::wstring>{ L"MDXM_OK" };
    };
    VbanServer srv; std::wstring err;
    CHECK(srv.Start(cfg, &ring, 48000, cb, &err));
    SOCKET c = MakeClient();
    SendPing0(c, 1);
    // Hand-built with the wrong stream name.
    uint8_t buf[kMaxPacket];
    char other[kStreamNameSize];
    FillStreamName(other, "someoneelse");
    const std::string rec = "MDXM_AUTH|pin=42|device=d1|name=P";
    const size_t n = BuildTxtPacket(buf, other, 0, rec);
    sockaddr_in to = {};
    to.sin_family = AF_INET;
    to.sin_port = htons((unsigned short)kTestPort);
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sendto(c, (const char*)buf, (int)n, 0, (sockaddr*)&to, (int)sizeof to);
    CHECK(RecvTxt(c, 400).empty());
    CHECK(!dispatched);
    closesocket(c);
    srv.Stop();
}

MDXM_TEST_CASE(VbanTxt_LargeReplyDoesNotOverflowTheReplyBuffer) {
    // An authorized peer that asks for a multi-record reply (MDXM_STATE, which is
    // the first thing a mixer client requests) gets several records chunked into
    // packets of up to 1436 bytes. The reply buffer must be big enough for one
    // whole packet -- a buffer sized for the 704-byte PING0 reply overran the
    // heap on the receive thread the moment a chunk exceeded 676 bytes.
    EnsureWsa();
    RingBuffer ring(48000 * 2);
    VbanConfig cfg; cfg.enabled = true; cfg.port = kTestPort; cfg.pin = L"42";
    // A reply big enough to span several full packets: ~60 records of ~55 bytes.
    std::vector<std::wstring> big{ L"MDXM_BEGIN" };
    for (int i = 0; i < 60; ++i)
        big.push_back(L"MDXM_CHAN|id=ch" + std::to_wstring(i) + L"|name=" +
                      std::wstring(40, L'x'));
    big.push_back(L"MDXM_END");
    VbanServer::Callbacks cb;
    cb.dispatch = [&](const std::wstring&) { return big; };
    VbanServer srv; std::wstring err;
    CHECK(srv.Start(cfg, &ring, 48000, cb, &err));
    SOCKET c = MakeClient();
    SendPing0(c, 1);
    srv.AuthorizationResult(L"d1", true);          // pre-approve so AUTH is instant
    SendTxt(c, "MDXM_AUTH|pin=42|device=d1|name=P");
    CHECK(RecvTxt(c) == "MDXM_AUTHSTATE|ok=1");

    SendTxt(c, "MDXM_STATE");
    const std::vector<std::string> got = RecvAllTxtRecords(c);
    // Every record arrives, in one piece, across however many packets it took.
    CHECK(got.size() == big.size());
    if (!got.empty()) {
        CHECK(got.front() == "MDXM_BEGIN");
        CHECK(got.back() == "MDXM_END");
    }
    CHECK(srv.Running());                           // the receive thread survived
    closesocket(c);
    srv.Stop();
}

MDXM_TEST_CASE(VbanTxt_RebindKeepsItsCallbacks) {
    // Changing the port must not silently sever the control plane. The rebind
    // used to pass empty callbacks, so after it an authorized phone'\''s records
    // were dropped and a pending device was never prompted -- with no error
    // anywhere.
    EnsureWsa();
    RingBuffer ring(48000 * 2);
    VbanConfig cfg; cfg.enabled = true; cfg.port = kTestPort; cfg.pin = L"42";
    std::vector<std::wstring> pendings;
    VbanServer::Callbacks cb;
    cb.dispatch = [](const std::wstring&) {
        return std::vector<std::wstring>{ L"MDXM_PONG|version=1" };
    };
    cb.onAuthPending = [&](const std::wstring& id, const std::wstring&) {
        pendings.push_back(id);
    };
    VbanServer srv; std::wstring err;
    CHECK(srv.Start(cfg, &ring, 48000, cb, &err));

    VbanConfig moved = cfg;
    moved.port = kTestPort + 1;
    srv.UpdateConfig(moved);                        // the rebind under test
    CHECK(srv.Running());

    SOCKET c = MakeClient();
    SendPing0To(c, 1, kTestPort + 1);
    SendTxtTo(c, "MDXM_AUTH|pin=42|device=dX|name=Phone", kTestPort + 1);
    // The prompt still fires, which proves onAuthPending survived the rebind.
    CHECK(RecvTxt(c) == "MDXM_AUTHSTATE|pending=1");
    CHECK(pendings.size() == 1);
    closesocket(c);
    srv.Stop();
}

MDXM_TEST_CASE(VbanTxt_AuthPendingIsReportedInStatus) {
    // MDXM_VBANSTATE carries authpending so a UI can show "a device is waiting".
    // It was hardcoded to 0.
    EnsureWsa();
    RingBuffer ring(48000 * 2);
    VbanConfig cfg; cfg.enabled = true; cfg.port = kTestPort; cfg.pin = L"42";
    VbanServer srv; std::wstring err;
    CHECK(srv.Start(cfg, &ring, 48000, {}, &err));
    CHECK(srv.Status().authPending == 0);
    SOCKET c = MakeClient();
    SendPing0(c, 1);
    SendTxt(c, "MDXM_AUTH|pin=42|device=waiting|name=P");
    CHECK(RecvTxt(c) == "MDXM_AUTHSTATE|pending=1");
    CHECK(srv.Status().authPending == 1);
    closesocket(c);
    srv.Stop();
}

MDXM_TEST_CASE(Vban_GarbageDatagramsDoNotDisturbTheServer) {
    // Review Focus #1 at the socket: this port accepts unauthenticated input
    // from anywhere on the network. Nonsense must be ignored, and the server
    // must still be answering pings afterwards.
    EnsureWsa();
    RingBuffer ring(48000 * 2);
    VbanConfig cfg; cfg.enabled = true; cfg.port = kTestPort;
    VbanServer srv; std::wstring err;
    CHECK(srv.Start(cfg, &ring, 48000, {}, &err));
    SOCKET c = MakeClient();
    sockaddr_in to = {};
    to.sin_family = AF_INET;
    to.sin_port = htons((unsigned short)kTestPort);
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    const char empty[1] = { 0 };
    sendto(c, empty, 0, 0, (sockaddr*)&to, (int)sizeof to);            // zero length
    sendto(c, "short", 5, 0, (sockaddr*)&to, (int)sizeof to);          // under a header
    // Larger than any legal VBAN packet, so larger than the server's receive
    // buffer: this is the datagram that used to end the receive thread with
    // WSAEMSGSIZE and stop mdxmixer answering pings for good.
    std::vector<char> junk(1600, 'x');
    sendto(c, junk.data(), (int)junk.size(), 0, (sockaddr*)&to, (int)sizeof to);
    Header h = {}; h.vban = 0x12345678;                                 // wrong magic
    sendto(c, (const char*)&h, (int)sizeof h, 0, (sockaddr*)&to, (int)sizeof to);

    // Still alive and still answering.
    SendPing0(c, 77);
    uint8_t buf[2048];
    sockaddr_in from; int flen = (int)sizeof from;
    int n = recvfrom(c, (char*)buf, (int)sizeof buf, 0, (sockaddr*)&from, &flen);
    CHECK(n == (int)(kHeaderSize + sizeof(Ping0)));
    if (n > 0) CHECK(ParsePacket(buf, (size_t)n).hdr.nuFrame == 77);
    CHECK(srv.Running());
    closesocket(c);
    srv.Stop();
}
