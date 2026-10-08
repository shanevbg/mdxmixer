#include "net/vban_server.h"
#include "app/log.h"
#include "engine/mix_demand.h"   // RenderRetry: the house back-off shape
#include "version.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>     // SIO_UDP_CONNRESET
#include <windows.h>
#include <objbase.h>     // CoInitializeEx, for the capture thread's apartment
#include <process.h>
#include <algorithm>
#include <cstring>

#pragma comment(lib, "ws2_32.lib")

namespace mdxm {

namespace {

// WSAStartup once per process and never cleaned up. Same posture as
// CaptureStream::Stop leaking an Impl rather than freeing under a wedged driver:
// there is nothing to gain from tearing the stack down at exit, and a
// WSACleanup racing a thread that has not quite finished is a crash on the way
// out -- the worst time to have one, because it looks like whatever the user was
// doing at the time.
std::once_flag g_wsaOnce;
void EnsureWinsock() {
    std::call_once(g_wsaOnce, [] {
        WSADATA w;
        WSAStartup(MAKEWORD(2, 2), &w);
    });
}

// A recipient, packed into one integer so the send loop's list needs no
// allocation and no winsock types in the header: ip (network order) in the low
// 32 bits, port (network order) in the next 16.
unsigned long long PackAddr(uint32_t ipNet, unsigned short portNet) {
    return (unsigned long long)ipNet | ((unsigned long long)portNet << 32);
}
void UnpackAddr(unsigned long long packed, sockaddr_in* out) {
    std::memset(out, 0, sizeof *out);
    out->sin_family = AF_INET;
    out->sin_addr.s_addr = (uint32_t)(packed & 0xFFFFFFFFull);
    out->sin_port = (unsigned short)((packed >> 32) & 0xFFFFull);
}

std::string NarrowAscii(const std::wstring& w) {
    std::string s;
    s.reserve(w.size());
    for (wchar_t c : w) s.push_back(c < 128 ? (char)c : '?');
    return s;
}

// QueryPerformanceCounter in microseconds. The sender thread may call clocks --
// it is not the audio thread.
uint64_t NowUs() {
    static LARGE_INTEGER freq = {};
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (uint64_t)((t.QuadPart * 1000000ull) / (uint64_t)freq.QuadPart);
}

// "ip:port" -> address. Returns false for anything unparseable, which is then
// treated as "no target" rather than as a reason to fail: a typo in a setting
// must not stop the rest of the stream working.
bool ParseTarget(const std::wstring& text, uint32_t* ipNet, unsigned short* portNet) {
    const size_t colon = text.rfind(L':');
    if (colon == std::wstring::npos || colon == 0 || colon + 1 >= text.size()) return false;
    const std::string ip = NarrowAscii(text.substr(0, colon));
    const int port = _wtoi(text.c_str() + colon + 1);
    if (port < 1 || port > 65535) return false;
    in_addr a = {};
    if (inet_pton(AF_INET, ip.c_str(), &a) != 1) return false;
    *ipNet = a.s_addr;
    *portNet = htons((unsigned short)port);
    return true;
}

} // namespace

VbanServer::VbanServer() {
    // Sized once, so the send path allocates nothing. kMaxPullFrames-sized
    // blocks are not needed here: the sender reads exactly one packet's worth.
    m_block.resize(vban::kFramesPerPacketI16 * 2, 0.0f);
    m_packet.resize(vban::kMaxPacket, 0);
    m_recipients.reserve(vban::PeerTable::kMaxPeers + 1);
}

VbanServer::~VbanServer() { Stop(); }

bool VbanServer::StopRequested() const {
    return !m_stopEvent || WaitForSingleObject((HANDLE)m_stopEvent, 0) == WAIT_OBJECT_0;
}

void VbanServer::RederiveLocked() {
    m_srIndex = vban::SampleRateIndex(m_mixRate);
    if (m_srIndex < 0) {
        // A rate the protocol cannot name. Say so and fall back to 48 kHz
        // rather than sending a packet whose header claims the wrong rate --
        // that would play back at the wrong speed, which sounds like a fault in
        // the music.
        m_lastError = L"mix rate " + std::to_wstring(m_mixRate) +
                      L" Hz has no VBAN sample-rate index; sending as 48000";
        Log(1, L"vban: %s", m_lastError.c_str());
        m_srIndex = vban::SampleRateIndex(48000);
    }
    m_framesPerPacket = vban::AudioFramesPerPacket(
        m_cfg.formatFloat32 ? vban::kBitFloat32 : vban::kBitInt16);
    // m_block is NOT resized here, on purpose. It is the sender thread's scratch,
    // read outside this lock; resizing it from the control thread would be a data
    // race (and a reallocation the sender could be mid-read of). It is sized once,
    // in the constructor, to the int16 maximum (256 frames) -- which is >= the
    // float32 count (128) -- so the sender's `framesPerPacket` reads always fit.
    // A burst of four is about 21 ms of audio: enough to recover from an
    // ordinary scheduling hiccup, far short of what would overrun a receiver's
    // jitter buffer.
    m_pacer.Configure(m_mixRate, m_framesPerPacket, 4);

    m_targetValid = false;
    m_targetIp = 0;
    m_targetPort = 0;
    if (!m_cfg.alwaysStreamTarget.empty()) {
        if (!ParseTarget(m_cfg.alwaysStreamTarget, &m_targetIp, &m_targetPort)) {
            m_lastError = L"always-stream target '" + m_cfg.alwaysStreamTarget +
                          L"' is not ip:port; ignoring it";
            Log(1, L"vban: %s", m_lastError.c_str());
        } else {
            m_targetValid = true;
        }
    }

    // The identification reply, built here so the receive thread only has to
    // copy it. What we advertise is what the phone reads INSTEAD of asking our
    // version, so these bits are load-bearing.
    m_identity = vban::Ping0{};
    m_identity.bitType = vban::kDeviceTransmitter | vban::kDeviceVirtualMixer;
    m_identity.bitFeature = vban::kFeatureAudio | vban::kFeatureTxt | vban::kFeatureFrame;
    m_identity.preferredRate = m_mixRate;
    m_identity.minRate = 44100;
    m_identity.maxRate = 192000;
    m_identity.nVersion[0] = (uint8_t)MDXM_VERSION_MAJOR;
    m_identity.nVersion[1] = (uint8_t)MDXM_VERSION_MINOR;
    m_identity.nVersion[2] = (uint8_t)MDXM_VERSION_PATCH;
    m_identity.nVersion[3] = 0;
    // memcpy into an already-zeroed struct rather than a string copy: these are
    // fixed-width NUL-padded wire fields, not C strings, and the padding is part
    // of what the far end compares.
    const auto setField = [](char* field, size_t size, const char* text) {
        const size_t n = std::strlen(text);
        std::memcpy(field, text, n < size - 1 ? n : size - 1);
    };
    setField(m_identity.deviceName, sizeof m_identity.deviceName, "mdxmixer");
    setField(m_identity.applicationName, sizeof m_identity.applicationName, "mdxmixer");
    setField(m_identity.manufacturerName, sizeof m_identity.manufacturerName, "mdxmixer");
    char host[64] = {};
    DWORD hostLen = (DWORD)sizeof(host) - 1;
    if (GetComputerNameA(host, &hostLen))
        setField(m_identity.hostName, sizeof m_identity.hostName, host);
}

bool VbanServer::Start(const VbanConfig& cfg, RingBuffer* ring, uint32_t mixRate,
                       Callbacks cb, std::wstring* err) {
    Stop();
    EnsureWinsock();

    // unique_lock, not lock_guard, so the thread-creation failure path below can
    // release it and call Stop() (which takes the same mutex) to tear down
    // cleanly rather than leaking a socket, an event and any thread that did
    // start.
    std::unique_lock<std::mutex> lock(m_mutex);
    m_cb = std::move(cb);
    m_cfg = cfg;
    m_ring = ring;
    m_mixRate = mixRate ? mixRate : 48000;
    m_lastError.clear();
    RederiveLocked();

    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        m_lastError = L"could not create a UDP socket (WSA " +
                      std::to_wstring(WSAGetLastError()) + L")";
        if (err) *err = m_lastError;
        return false;
    }
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)m_cfg.port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (!m_cfg.bindAddress.empty()) {
        in_addr want = {};
        if (inet_pton(AF_INET, NarrowAscii(m_cfg.bindAddress).c_str(), &want) == 1) {
            addr.sin_addr = want;
        } else {
            // Deliberately not fatal: binding every interface is the useful
            // behaviour, and a typo in an optional setting should not take the
            // feature out.
            m_lastError = L"bind address '" + m_cfg.bindAddress +
                          L"' is not an IPv4 address; listening on all interfaces";
            Log(1, L"vban: %s", m_lastError.c_str());
        }
    }
    // NO SO_REUSEADDR, on purpose. Two mdxmixers on one port would both appear
    // to work and would each receive an arbitrary half of the pings -- the
    // failure MdnsDiscovery had, where a fixed-port bind failed and was
    // swallowed. A port already in use has to be loud.
    if (bind(s, (sockaddr*)&addr, (int)sizeof addr) == SOCKET_ERROR) {
        const int e = WSAGetLastError();
        m_lastError = L"UDP port " + std::to_wstring(m_cfg.port) +
                      (e == WSAEADDRINUSE
                           ? L" is already in use (another mdxmixer, or another VBAN app)"
                           : L" could not be bound (WSA " + std::to_wstring(e) + L")");
        Log(1, L"vban: %s", m_lastError.c_str());
        closesocket(s);
        if (err) *err = m_lastError;
        return false;
    }
    // Bounded, so the receive thread notices a stop even when no datagram ever
    // arrives. closesocket() in Stop() is what normally wakes it; this is the
    // backstop for the case where that races.
    DWORD rcvTimeout = 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char*)&rcvTimeout, (int)sizeof rcvTimeout);
    // Stop Windows reporting a bounced sendto as an error on the next RECEIVE.
    // By default an ICMP port-unreachable -- which is what a phone leaving the
    // network produces -- makes the following recvfrom fail with
    // WSAECONNRESET. The receive loop tolerates that anyway, but a connectionless
    // socket has no business reporting it at all, and leaving it on would mean
    // one lost datagram every time a subscriber disappears.
    BOOL connReset = FALSE;
    DWORD unused = 0;
    WSAIoctl(s, SIO_UDP_CONNRESET, &connReset, (DWORD)sizeof connReset,
             nullptr, 0, &unused, nullptr, nullptr);

    m_sock = (uintptr_t)s;
    m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);   // manual reset
    if (!m_stopEvent) {
        m_lastError = L"could not create the stop event";
        closesocket(s);
        m_sock = ~(uintptr_t)0;
        if (err) *err = m_lastError;
        return false;
    }
    m_nuFrame = 0;
    m_lastExpireMs = GetTickCount64();
    m_failed.store(false, std::memory_order_release);
    m_running.store(true, std::memory_order_release);
    m_rxThread = (void*)_beginthreadex(nullptr, 0, &VbanServer::ReceiveTrampoline,
                                       this, 0, nullptr);
    m_txThread = (void*)_beginthreadex(nullptr, 0, &VbanServer::SendTrampoline,
                                       this, 0, nullptr);
    // The capture thread runs for the life of the server but does nothing, and
    // holds no GPU resource, until somebody asks for pictures.
    m_capThread = (void*)_beginthreadex(nullptr, 0, &VbanServer::CaptureTrampoline,
                                        this, 0, nullptr);
    if (!m_rxThread || !m_txThread || !m_capThread) {
        m_lastError = L"could not start the VBAN threads";
        if (err) *err = m_lastError;
        // Release the lock, then let Stop() do the real teardown: signal the
        // stop event, close the socket, and join whichever threads DID start.
        // m_running is already true, so Stop() takes its full path rather than
        // the "nothing to clean up" shortcut. Without this the socket, the
        // event and any started thread leaked.
        lock.unlock();
        Stop();
        return false;
    }
    Log(2, L"vban: listening on UDP %d (stream '%s', %s mix, %s)", m_cfg.port,
        m_cfg.streamName.c_str(), m_cfg.sourceStreaming ? L"streaming" : L"personal",
        m_cfg.formatFloat32 ? L"f32" : L"i16");
    return true;
}

void VbanServer::Stop() {
    if (!m_running.exchange(false, std::memory_order_acq_rel)) {
        // Not running, but a failed Start may still have left a socket or an
        // event behind.
        if (m_stopEvent) { CloseHandle((HANDLE)m_stopEvent); m_stopEvent = nullptr; }
        if (m_sock != ~(uintptr_t)0) { closesocket((SOCKET)m_sock); m_sock = ~(uintptr_t)0; }
        return;
    }
    if (m_stopEvent) SetEvent((HANDLE)m_stopEvent);
    // Closing the socket is what unblocks a thread sitting in recvfrom; the
    // receive loop then sees the error, finds the stop event set, and returns.
    if (m_sock != ~(uintptr_t)0) {
        closesocket((SOCKET)m_sock);
        m_sock = ~(uintptr_t)0;
    }
    for (void** t : { &m_rxThread, &m_txThread, &m_capThread }) {
        if (!*t) continue;
        // Bounded: a thread wedged in the network stack must not hold the UI
        // thread for ever at shutdown. Leaking the handle is the lesser evil,
        // exactly as CaptureStream::Stop leaks its Impl.
        if (WaitForSingleObject((HANDLE)*t, 3000) == WAIT_OBJECT_0)
            CloseHandle((HANDLE)*t);
        else
            Log(1, L"vban: a thread did not finish in time; leaking its handle");
        *t = nullptr;
    }
    if (m_stopEvent) { CloseHandle((HANDLE)m_stopEvent); m_stopEvent = nullptr; }
    m_wanted.store(false, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(m_mutex);
    m_peers = vban::PeerTable{};
    Log(2, L"vban: stopped");
}

void VbanServer::UpdateConfig(const VbanConfig& cfg) {
    bool rebind = false;
    Callbacks cb;
    RingBuffer* ring = nullptr;
    uint32_t rate = 48000;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        rebind = m_running.load(std::memory_order_acquire) &&
                 (cfg.port != m_cfg.port || cfg.bindAddress != m_cfg.bindAddress);
        // Turning alwaysFrames on (or pointing it at a new target) must push the
        // cached pictures to it at once: duplication only reports CHANGED
        // screens, so without this a still desktop would leave the target blank
        // until something moved. Same flush a peer's frames=1 triggers.
        const bool framesNowOn = cfg.alwaysFrames && !cfg.alwaysStreamTarget.empty() &&
                                 (!m_cfg.alwaysFrames ||
                                  m_cfg.alwaysStreamTarget != cfg.alwaysStreamTarget);
        m_cfg = cfg;
        if (framesNowOn) m_sendCachedFrames.store(true, std::memory_order_relaxed);
        if (!rebind) { RederiveLocked(); return; }
        // Carried across the rebind: Start() REPLACES m_cb with whatever it is
        // handed, so passing empty callbacks here would silently sever dispatch
        // and the two auth hooks -- an authorized phone's records dropped and a
        // pending device never prompted, with no error anywhere.
        cb = m_cb;
        ring = m_ring;
        rate = m_mixRate;
    }
    // The address changed, so the socket has to be remade. Start() does a Stop()
    // first, which is why this is not done under the lock.
    std::wstring err;
    if (!Start(cfg, ring, rate, cb, &err))
        Log(1, L"vban: rebind failed: %s", err.c_str());
}

void VbanServer::UpdateMixRate(uint32_t rate) {
    if (!rate) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    if (rate == m_mixRate) return;
    m_mixRate = rate;
    RederiveLocked();
    Log(2, L"vban: mix rate is now %u Hz", rate);
}

void VbanServer::SetSrcLatencyBase(int ms) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_srcLatencyBase = ms < 0 ? 0 : ms;
}

VbanStatus VbanServer::Status() const {
    VbanStatus s;
    std::lock_guard<std::mutex> lock(m_mutex);
    s.on = m_running.load(std::memory_order_acquire);
    s.emitting = m_wanted.load(std::memory_order_relaxed);
    s.port = m_cfg.port;
    s.name = m_cfg.streamName;
    s.peers = (int)m_peers.All().size();
    s.sourceStreaming = m_cfg.sourceStreaming;
    s.formatFloat32 = m_cfg.formatFloat32;
    s.gainPercent = m_cfg.gainPercent;
    s.fps = m_cfg.frames.fps;
    s.open = m_cfg.openSubscribe;
    s.always = m_cfg.alwaysStream;
    s.alwaysFrames = m_cfg.alwaysFrames;
    s.target = m_cfg.alwaysStreamTarget;
    s.sent = m_sent.load(std::memory_order_relaxed);
    s.starved = m_starved.load(std::memory_order_relaxed);
    s.framesSent = m_framesSent.load(std::memory_order_relaxed);
    // The ring's own drop counter, which is the producer overrunning a consumer
    // that is not keeping up -- a different number from `starved`, which is the
    // consumer finding nothing there.
    s.dropped = m_ring ? m_ring->Drops() : 0;
    s.depthMs = (m_ring && m_mixRate)
                    ? (int)(m_ring->Depth() * 1000 / m_mixRate)
                    : 0;
    s.behindMs = m_behindMs.load(std::memory_order_relaxed);
    const int packetMs = m_mixRate ? (int)(m_framesPerPacket * 1000 / m_mixRate) : 0;
    s.srcLatencyMs = m_srcLatencyBase + packetMs;
    // Devices with the right PIN that are waiting on the person at the PC, so a
    // UI can show "someone is asking". Counted from the peer table rather than
    // tracked separately -- a pending peer that expires stops being counted,
    // which is the honest answer.
    for (const auto& p : m_peers.All())
        if (p.auth == vban::AuthState::Pending) ++s.authPending;
    s.lastError = m_lastError;
    return s;
}

std::vector<VbanPeerRow> VbanServer::Peers() const {
    std::vector<VbanPeerRow> out;
    const uint64_t now = GetTickCount64();
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto& p : m_peers.All()) {
        VbanPeerRow r;
        in_addr a = {};
        a.s_addr = htonl(p.key.ip);
        char text[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &a, text, sizeof text);
        const std::string addr = std::string(text) + ":" + std::to_string(p.key.port);
        r.addr.assign(addr.begin(), addr.end());
        r.deviceName = p.deviceName;
        r.authed = p.auth == vban::AuthState::Authorized;
        r.sinceMs = (unsigned)(now > p.lastPingMs ? now - p.lastPingMs : 0);
        r.audioOn = p.audioOn;
        r.framesOn = p.framesOn;
        out.push_back(r);
    }
    if (m_targetValid) {
        // Not a peer -- it never pings and holds no slot -- but something is
        // being sent there, so it has to be visible.
        VbanPeerRow r;
        r.addr = m_cfg.alwaysStreamTarget;
        r.isAlwaysTarget = true;
        r.audioOn = m_cfg.alwaysStream;
        r.framesOn = m_cfg.alwaysFrames;
        out.push_back(r);
    }
    return out;
}

namespace {

// The verb is the record up to the first '|'. Enough to route; the real parse
// happens in the protocol layer, which is the only place that should know the
// grammar in detail.
std::wstring VerbOf(const std::wstring& record) {
    const size_t bar = record.find(L'|');
    return bar == std::wstring::npos ? record : record.substr(0, bar);
}

// One keyed field out of a record, without pulling in the protocol parser.
bool FieldOf(const std::wstring& record, const std::wstring& key, std::wstring* out) {
    const std::wstring needle = L"|" + key + L"=";
    const size_t at = record.find(needle);
    if (at == std::wstring::npos) return false;
    const size_t from = at + needle.size();
    const size_t end = record.find(L'|', from);
    *out = record.substr(from, end == std::wstring::npos ? std::wstring::npos : end - from);
    return true;
}

bool Holds(const std::vector<std::wstring>& v, const std::wstring& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

} // namespace

std::vector<std::wstring> VbanServer::HandleAuth(const vban::PeerKey& key,
                                                 const std::wstring& record) {
    std::wstring pin, deviceId, name;
    FieldOf(record, L"pin", &pin);
    FieldOf(record, L"device", &deviceId);
    FieldOf(record, L"name", &name);
    if (deviceId.empty()) return { L"MDXM_AUTHSTATE|ok=0|err=nodevice" };

    std::function<void()> notifyPending, notifyAuthorized;
    std::vector<std::wstring> reply;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const uint64_t now = GetTickCount64();
        bool authorized = Holds(m_approvedDevices, deviceId);
        if (!authorized)
            for (const auto& d : m_cfg.authorizedDevices)
                if (d.id == deviceId) { authorized = true; break; }
        const bool denied = Holds(m_deniedDevices, deviceId);
        const bool locked = m_strikes.Locked(key.ip, now);

        switch (vban::DecideAuth(m_cfg.pin, pin, authorized, denied, locked)) {
        case vban::AuthOutcome::Locked:
            reply = { L"MDXM_AUTHSTATE|ok=0|err=locked" };
            break;
        case vban::AuthOutcome::Denied:
            reply = { L"MDXM_AUTHSTATE|ok=0|err=denied" };
            break;
        case vban::AuthOutcome::BadPin:
            // Counted per IP, not per peer: see AuthStrikes.
            m_strikes.Strike(key.ip, now);
            reply = { L"MDXM_AUTHSTATE|ok=0|err=badpin" };
            break;
        case vban::AuthOutcome::Pending:
            if (vban::Peer* p = m_peers.Find(key)) {
                p->auth = vban::AuthState::Pending;
                p->deviceId = deviceId;
                p->deviceName = name;
            }
            // Asked once per device. The phone re-sends while it waits, and a
            // prompt per resend would make the feature unusable.
            if (!Holds(m_promptedDevices, deviceId)) {
                m_promptedDevices.push_back(deviceId);
                if (m_cb.onAuthPending) {
                    auto cb = m_cb.onAuthPending;
                    notifyPending = [cb, deviceId, name] { cb(deviceId, name); };
                }
            }
            reply = { L"MDXM_AUTHSTATE|pending=1" };
            break;
        case vban::AuthOutcome::Ok:
            if (vban::Peer* p = m_peers.Find(key)) {
                p->auth = vban::AuthState::Authorized;
                p->deviceId = deviceId;
                p->deviceName = name;
            }
            // A success forgives the record: a phone that mistyped twice and then
            // got it right should not carry strikes toward a future lockout.
            m_strikes.Clear(key.ip);
            if (!Holds(m_approvedDevices, deviceId)) m_approvedDevices.push_back(deviceId);
            if (m_cb.onAuthorized) {
                auto cb = m_cb.onAuthorized;
                notifyAuthorized = [cb, deviceId, name] { cb(deviceId, name); };
            }
            reply = { L"MDXM_AUTHSTATE|ok=1" };
            break;
        }
    }
    // Outside the lock: these reach the app layer, which will take its own.
    if (notifyPending) notifyPending();
    if (notifyAuthorized) notifyAuthorized();
    return reply;
}

std::vector<std::wstring> VbanServer::HandleTxt(const vban::PeerKey& key,
                                                const std::wstring& record) {
    const std::wstring verb = VerbOf(record);
    if (verb == L"MDXM_AUTH") return HandleAuth(key, record);

    // Everything below needs an authorized peer. An unauthenticated sender is
    // dropped in silence: the whole mixer surface is behind this gate, and there
    // is nothing it is owed an explanation about.
    std::wstring forward = record;
    bool perPeerOnly = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        vban::Peer* p = m_peers.Find(key);
        if (!p || p->auth != vban::AuthState::Authorized) {
            m_txtDropped.fetch_add(1, std::memory_order_relaxed);
            return {};
        }
        if (verb == L"MDXM_SUBSCRIBE") {
            // The one documented gap: every broadcast site is wired to the pipe
            // object and the push direction has no transport-independent seam yet
            // (spec §2.4). Refusing is honest; accepting and never pushing would
            // leave a client waiting for ever.
            return { L"MDXM_ERR|msg=subscribe is pipe-only" };
        }
        if (verb == L"MDXM_VBAN") {
            // Per-peer grants are applied HERE, because they belong to this peer
            // rather than to the configuration -- the protocol layer has no peer
            // to apply them to and refuses them on the pipe for that reason.
            std::wstring v;
            bool touched = false;
            if (FieldOf(record, L"audio", &v)) { p->audioOn = v != L"0"; touched = true; }
            if (FieldOf(record, L"frames", &v)) {
                const bool want = v != L"0";
                // Newly asking: the cached pictures go out on the capture
                // thread's next wake, so a still desktop does not leave the
                // panels blank until something moves.
                if (want && !p->framesOn) m_sendCachedFrames.store(true, std::memory_order_relaxed);
                p->framesOn = want;
                touched = true;
            }
            if (touched) {
                // Strip them so the rest of the record can go on to the real
                // handler; a record of nothing but per-peer keys becomes a plain
                // query, which answers with the state either way.
                std::wstring rest = L"MDXM_VBAN";
                size_t pos = record.find(L'|');
                while (pos != std::wstring::npos) {
                    const size_t next = record.find(L'|', pos + 1);
                    const std::wstring field = record.substr(
                        pos + 1, next == std::wstring::npos ? std::wstring::npos
                                                            : next - pos - 1);
                    if (field.rfind(L"audio=", 0) != 0 && field.rfind(L"frames=", 0) != 0)
                        rest += L"|" + field;
                    pos = next;
                }
                forward = rest;
                perPeerOnly = rest == L"MDXM_VBAN";
            }
        }
        // The PIN is pipe-only: setting the secret over the channel it protects is
        // circular, and the protocol layer cannot see which transport a record
        // arrived on (ledger ruling R2).
        std::wstring ignored;
        if (verb == L"MDXM_VBAN" && FieldOf(record, L"pin", &ignored))
            return { L"MDXM_ERR|msg=pin is pipe-only" };
    }
    (void)perPeerOnly;   // a per-peer-only record still queries, which is wanted

    if (!m_cb.dispatch) {
        m_txtDropped.fetch_add(1, std::memory_order_relaxed);
        return {};
    }
    // MARSHALLED onto the UI thread, so a record from the network is handled on
    // the same single thread as one from the pipe. This blocks the receive thread
    // for as long as that takes (bounded at five seconds by the dispatcher); a UI
    // wedged for that long twice over would let a peer's subscription expire,
    // which is a UI already broken by then.
    return m_cb.dispatch(forward);
}

void VbanServer::AuthorizationResult(const std::wstring& deviceId, bool allow) {
    if (deviceId.empty()) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    // The prompt has been answered, so the device may be asked about again if it
    // somehow returns to pending.
    m_promptedDevices.erase(std::remove(m_promptedDevices.begin(), m_promptedDevices.end(),
                                        deviceId), m_promptedDevices.end());
    if (allow) {
        if (!Holds(m_approvedDevices, deviceId)) m_approvedDevices.push_back(deviceId);
        m_deniedDevices.erase(std::remove(m_deniedDevices.begin(), m_deniedDevices.end(),
                                          deviceId), m_deniedDevices.end());
        Log(2, L"vban: device '%s' approved", deviceId.c_str());
    } else {
        if (!Holds(m_deniedDevices, deviceId)) m_deniedDevices.push_back(deviceId);
        m_approvedDevices.erase(std::remove(m_approvedDevices.begin(),
                                            m_approvedDevices.end(), deviceId),
                                m_approvedDevices.end());
        // Any peer currently holding that identity loses it at once rather than
        // keeping a session it is no longer entitled to.
        m_peers.RevokeDevice(deviceId);
        Log(2, L"vban: device '%s' denied", deviceId.c_str());
    }
}

void VbanServer::ForgetDevice(const std::wstring& deviceId) {
    if (deviceId.empty()) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto drop = [&deviceId](std::vector<std::wstring>& v) {
        v.erase(std::remove(v.begin(), v.end(), deviceId), v.end());
    };
    drop(m_approvedDevices);
    drop(m_deniedDevices);
    drop(m_promptedDevices);
    m_peers.RevokeDevice(deviceId);
    Log(2, L"vban: forgot device '%s'", deviceId.c_str());
}

void VbanServer::CollectFrameRecipientsLocked() {
    m_frameRecipients.clear();
    const bool pinSet = !m_cfg.pin.empty();
    for (const auto& p : m_peers.All())
        if (vban::FramesEntitled(p, pinSet))
            m_frameRecipients.push_back(PackAddr(htonl(p.key.ip), htons(p.key.port)));
    if (m_cfg.alwaysFrames && m_targetValid)
        m_frameRecipients.push_back(PackAddr(m_targetIp, m_targetPort));
}

void VbanServer::CaptureLoop() {
    try {
        // The encoder is COM; this thread owns its own apartment, like every
        // other worker in this program.
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        bool initialised = false;
        // The house back-off, for a desktop that will not duplicate: a UAC prompt
        // or a secure desktop is not going to change in the next 50 ms, and
        // retrying that fast would be a COM round trip and a log line forever.
        RenderRetry retry;
        uint64_t nextCaptureMs = 0;
        std::vector<std::vector<uint8_t>> packets;

        while (!StopRequested()) {
            if (WaitForSingleObject((HANDLE)m_stopEvent, 50) == WAIT_OBJECT_0) break;
            const uint64_t nowMs = GetTickCount64();

            double fps = 2.0;
            int maxEdge = 480, quality = 60;
            bool wanted = false;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                CollectFrameRecipientsLocked();
                wanted = !m_frameRecipients.empty();
                fps = m_cfg.frames.fps;
                maxEdge = m_cfg.frames.maxEdge;
                quality = m_cfg.frames.quality;
            }
            if (!wanted) {
                // Nobody is owed pictures, so the duplication is released rather
                // than left running: it is a GPU resource and a desktop lock, and
                // this is the same on-demand rule the audio follows.
                if (initialised) {
                    m_capture.Shutdown();
                    initialised = false;
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_lastFrame.clear();
                }
                continue;
            }
            if (!initialised) {
                if (!retry.Due((unsigned)nowMs)) continue;
                std::wstring err;
                if (!m_capture.Init(&err)) {
                    retry.Failed((unsigned)nowMs);
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_lastError = err;
                    continue;
                }
                retry.Reset();
                initialised = true;
            }

            // A peer that has just asked for pictures gets the cached ones at
            // once. Duplication only reports CHANGED screens, so waiting for a
            // change could leave a panel blank for as long as nobody touches the
            // mouse -- which on a visualiser rig might be hours.
            const bool flush = m_sendCachedFrames.exchange(false, std::memory_order_relaxed);
            if (nowMs < nextCaptureMs && !flush) continue;
            if (fps < 0.2) fps = 0.2;
            nextCaptureMs = nowMs + (uint64_t)(1000.0 / fps);

            const std::vector<CapturedFrame> frames = m_capture.Poll(maxEdge, quality);
            if (m_capture.Lost()) {
                m_capture.Shutdown();
                initialised = false;
                retry.Failed((unsigned)nowMs);
                std::lock_guard<std::mutex> lock(m_mutex);
                m_lastError = L"the desktop stopped being capturable; retrying";
                continue;
            }

            const SOCKET s = (SOCKET)m_sock;
            if (s == (SOCKET)~(uintptr_t)0) break;
            // Freshly captured first, then anything cached that did not change --
            // which is what a newly-entitled peer is waiting for.
            // Copied out of the cache rather than pointed into it: the map is
            // under the lock and the sends are not.
            std::vector<std::pair<int, std::vector<uint8_t>>> toSend;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                for (const auto& f : frames) m_lastFrame[f.deviceNumber] = f.jpeg;
                if (flush) {
                    // Everything known, changed or not: this is the catch-up.
                    for (const auto& kv : m_lastFrame) toSend.push_back({ kv.first, kv.second });
                } else {
                    for (const auto& f : frames) toSend.push_back({ f.deviceNumber, f.jpeg });
                }
            }
            for (const auto& item : toSend) {
                // One stream per display, named by its Windows device number --
                // the VB-Audio convention, so VBAN-Screen can watch them too.
                char name[vban::kStreamNameSize];
                vban::FillStreamName(name, "VIDEO" + std::to_string(item.first));
                uint32_t seq = 0;
                std::vector<unsigned long long> recipients;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    seq = m_frameSeq[item.first]++;
                    recipients = m_frameRecipients;
                }
                vban::ChunkFrame(packets, name, seq, item.second.data(), item.second.size());
                if (packets.empty()) continue;
                for (unsigned long long packed : recipients) {
                    sockaddr_in to;
                    UnpackAddr(packed, &to);
                    for (const auto& packet : packets)
                        sendto(s, (const char*)packet.data(), (int)packet.size(), 0,
                               (sockaddr*)&to, (int)sizeof to);
                }
                m_framesSent.fetch_add(1, std::memory_order_relaxed);
            }
        }
        m_capture.Shutdown();
        CoUninitialize();
    } catch (const std::exception& e) {
        Log(1, L"vban capture thread: exception (%hs)", e.what());
    } catch (...) {
        Log(1, L"vban capture thread: unknown exception");
    }
}

unsigned __stdcall VbanServer::ReceiveTrampoline(void* self) {
    ((VbanServer*)self)->ReceiveLoop();
    return 0;
}

unsigned __stdcall VbanServer::CaptureTrampoline(void* self) {
    ((VbanServer*)self)->CaptureLoop();
    return 0;
}

unsigned __stdcall VbanServer::SendTrampoline(void* self) {
    ((VbanServer*)self)->SendLoop();
    return 0;
}

void VbanServer::ReceiveLoop() {
    try {
        std::vector<uint8_t> buf(vban::kMaxPacket);
        // A whole packet. The PING0 reply is only 704 bytes, but this same buffer
        // carries the TXT replies too, and a chunked MDXM_BEGIN..MDXM_END reply
        // fills a packet to kMaxData -- sizing it for the ping alone overran the
        // heap the moment a reply chunk exceeded 676 bytes, which MDXM_STATE does
        // on the first request a mixer client makes.
        std::vector<uint8_t> reply(vban::kMaxPacket);
        while (!StopRequested()) {
            sockaddr_in from = {};
            int fromLen = (int)sizeof from;
            const SOCKET s = (SOCKET)m_sock;
            if (s == (SOCKET)~(uintptr_t)0) break;
            const int n = recvfrom(s, (char*)buf.data(), (int)buf.size(), 0,
                                   (sockaddr*)&from, &fromLen);
            if (n == SOCKET_ERROR) {
                const int e = WSAGetLastError();
                // PER-DATAGRAM FAILURES MUST NOT END THE LOOP. This cost a
                // caught bug: a 1600-byte datagram -- larger than any legal
                // VBAN packet, so larger than this buffer -- returns
                // WSAEMSGSIZE, and treating that as fatal meant one oversized
                // packet from anywhere on the network permanently stopped
                // mdxmixer answering pings. WSAECONNRESET is the same hazard in
                // everyday clothing: Windows reports a bounced sendto (the phone
                // walked out of range) as an error on the next RECEIVE, so a
                // listener that quit on it would die the first time a subscriber
                // left. A timeout is the ordinary case and how this loop gets to
                // check the stop event at all.
                if (e == WSAETIMEDOUT || e == WSAEMSGSIZE || e == WSAECONNRESET ||
                    e == WSAEINTR || e == WSAENETRESET)
                    continue;
                // Anything else is the socket itself going. Usually that is
                // Stop() closing it (m_stopEvent is set, and the loop exits
                // next). But if it happens for any OTHER reason -- a resume that
                // left the socket dead, an adapter pulled -- the listener must
                // not just vanish silently: that is the "listed, active and
                // silent" failure onResume exists to undo. It is recorded and
                // flagged, and the control tick rebuilds the whole server (which
                // is why m_running is left TRUE -- Stop() keys its thread joins
                // on it, and a worker clearing it would make Stop() leak the
                // sender and capture threads).
                if (!StopRequested()) {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_lastError = L"the receive socket failed (WSA " +
                                  std::to_wstring(e) + L"); rebuilding";
                    Log(1, L"vban: %s", m_lastError.c_str());
                    m_failed.store(true, std::memory_order_release);
                }
                break;
            }
            if (n <= 0) continue;
            const vban::Parsed p = vban::ParsePacket(buf.data(), (size_t)n);
            if (!p.valid) continue;   // not VBAN, or malformed: ignored in silence

            if (vban::IsPing0Request(p)) {
                vban::Ping0 id;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    // Renew or create the subscription. A null return means the
                    // table is full of authenticated peers -- we still answer,
                    // because the reply is stateless and refusing to be
                    // discovered is worse than refusing to serve.
                    m_peers.Ping(vban::PeerKey{ ntohl(from.sin_addr.s_addr),
                                                ntohs(from.sin_port) },
                                 GetTickCount64());
                    id = m_identity;
                }
                const size_t len = vban::BuildPing0Reply(reply.data(), p.hdr, id);
                sendto(s, (const char*)reply.data(), (int)len, 0,
                       (sockaddr*)&from, fromLen);
                continue;
            }
            if (p.proto == vban::kProtoTxt) {
                // 1. The stream name. A receiver matches on it, and so do we: a
                // record addressed to something else on this network is not ours
                // to act on.
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    char want[vban::kStreamNameSize];
                    vban::FillStreamName(want, NarrowAscii(m_cfg.streamName));
                    if (!vban::StreamNameIs(p.hdr, want)) {
                        m_txtDropped.fetch_add(1, std::memory_order_relaxed);
                        continue;
                    }
                    // 2. No PIN means no way to authenticate, so control is dead
                    // -- MDXM_AUTH included. Silently, because there is nobody
                    // entitled to an explanation.
                    if (m_cfg.pin.empty()) {
                        m_txtDropped.fetch_add(1, std::memory_order_relaxed);
                        continue;
                    }
                }
                // 3. Decode. A malformed record is dropped whole rather than
                // partially decoded into something that happens to parse.
                std::string utf8;
                if (!vban::ParseTxt(p, &utf8)) continue;
                bool ok = false;
                const std::wstring record = vban::Utf8ToWide(utf8, &ok);
                if (!ok || record.empty()) {
                    m_txtDropped.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                const vban::PeerKey key{ ntohl(from.sin_addr.s_addr),
                                         ntohs(from.sin_port) };
                const std::vector<std::wstring> replies = HandleTxt(key, record);
                if (replies.empty()) continue;
                for (const auto& chunk : vban::ChunkReplies(replies)) {
                    uint32_t seq = 0;
                    char name[vban::kStreamNameSize];
                    {
                        std::lock_guard<std::mutex> lock(m_mutex);
                        vban::FillStreamName(name, NarrowAscii(m_cfg.streamName));
                        if (vban::Peer* pr = m_peers.Find(key)) seq = pr->txTxtNuFrame++;
                    }
                    const size_t len = vban::BuildTxtPacket(reply.data(), name, seq,
                                                            vban::WideToUtf8(chunk));
                    if (len) sendto(s, (const char*)reply.data(), (int)len, 0,
                                    (sockaddr*)&from, fromLen);
                }
                continue;
            }
            // Inbound AUDIO or SERIAL: not something this end consumes.
        }
    } catch (const std::exception& e) {
        Log(1, L"vban receive thread: exception (%hs)", e.what());
    } catch (...) {
        Log(1, L"vban receive thread: unknown exception");
    }
}

void VbanServer::SendLoop() {
    try {
        while (!StopRequested()) {
            // 1 ms, so the pacer's schedule is the thing that decides when a
            // packet goes rather than this wait's granularity.
            if (WaitForSingleObject((HANDLE)m_stopEvent, 1) == WAIT_OBJECT_0) break;

            const uint64_t nowMs = GetTickCount64();
            size_t framesPerPacket = 0;
            uint8_t bitType = vban::kBitInt16;
            int srIndex = 3;
            float gain = 1.0f;
            char streamName[vban::kStreamNameSize] = {};
            m_recipients.clear();
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                // Once a second, drop peers that have stopped pinging. Done
                // here rather than on a timer of its own: this thread already
                // wakes constantly and already holds the lock.
                if (nowMs - m_lastExpireMs >= 1000) {
                    m_peers.Expire(nowMs);
                    m_lastExpireMs = nowMs;
                }
                framesPerPacket = m_framesPerPacket;
                bitType = m_cfg.formatFloat32 ? vban::kBitFloat32 : vban::kBitInt16;
                srIndex = m_srIndex;
                gain = (float)m_cfg.gainPercent / 100.0f;
                vban::FillStreamName(streamName, NarrowAscii(m_cfg.streamName));
                const bool pinSet = !m_cfg.pin.empty();
                for (const auto& p : m_peers.All())
                    if (vban::AudioEntitled(p, pinSet, m_cfg.openSubscribe))
                        m_recipients.push_back(PackAddr(htonl(p.key.ip),
                                                        htons(p.key.port)));
                if (m_cfg.alwaysStream && m_targetValid)
                    m_recipients.push_back(PackAddr(m_targetIp, m_targetPort));
                m_wanted.store(vban::VbanWanted(true, m_recipients.size(),
                                                m_cfg.alwaysStream, m_targetValid),
                               std::memory_order_relaxed);
            }

            const unsigned grant = m_pacer.Due(NowUs());
            m_behindMs.store((int)(m_pacer.BehindUs(NowUs()) / 1000),
                             std::memory_order_relaxed);
            // The grant is consumed either way: a schedule that only advanced
            // while somebody was listening would hand out a backlog of packets
            // the moment one subscribed.
            if (grant == 0 || m_recipients.empty() || !m_ring) continue;

            const SOCKET s = (SOCKET)m_sock;
            if (s == (SOCKET)~(uintptr_t)0) break;
            for (unsigned g = 0; g < grant; ++g) {
                const size_t got = m_ring->Read(m_block.data(), framesPerPacket);
                if (got < framesPerPacket) {
                    // RingBuffer::Read has already zero-filled the shortfall.
                    // Sending the silence keeps the cadence, which is what lets
                    // the far end tell "no signal" from "network gone" -- and
                    // the count is how this end can tell the same thing.
                    m_starved.fetch_add(1, std::memory_order_relaxed);
                }
                if (gain != 1.0f)
                    for (size_t i = 0; i < framesPerPacket * 2; ++i) m_block[i] *= gain;
                // After the gain, because the gain is what can push a signal
                // that was fine into clipping.
                m_limiter.Process(m_block.data(), framesPerPacket);
                const size_t len = vban::BuildAudioPacket(
                    m_packet.data(), streamName, m_nuFrame, srIndex, bitType,
                    m_block.data(), framesPerPacket);
                if (!len) break;   // cannot happen with a derived frame count
                ++m_nuFrame;
                for (unsigned long long packed : m_recipients) {
                    sockaddr_in to;
                    UnpackAddr(packed, &to);
                    sendto(s, (const char*)m_packet.data(), (int)len, 0,
                           (sockaddr*)&to, (int)sizeof to);
                }
                m_sent.fetch_add(1, std::memory_order_relaxed);
            }
        }
    } catch (const std::exception& e) {
        Log(1, L"vban sender thread: exception (%hs)", e.what());
    } catch (...) {
        Log(1, L"vban sender thread: unknown exception");
    }
}

} // namespace mdxm
