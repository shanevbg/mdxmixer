#pragma once
// Who is subscribed, who is allowed what (spec §3.3, §4).
//
// PURE: an address is an opaque pair of integers and time is a tick the caller
// passes in -- no winsock, no clock. These are the program's access-control
// decisions, and the reason they live in a testable file rather than inside the
// receive loop is that the failure modes are quiet ones: a peer that should not
// have been served, or one that should have been and silently was not.
//
// WHY A SUBSCRIPTION EXISTS AT ALL. VBAN audio is fire-and-forget UDP with no
// notion of a session, so "stream only while somebody is listening" has to be
// built on something. It is built on PING0: a request renews the asker as a
// peer, and a peer that stops asking expires. Standard mechanism, our semantic.
#include <cstdint>
#include <string>
#include <vector>

namespace mdxm { namespace vban {

struct PeerKey {
    uint32_t ip = 0;     // host byte order; opaque here
    uint16_t port = 0;
    bool operator==(const PeerKey& o) const { return ip == o.ip && port == o.port; }
};

enum class AuthState { None, Pending, Authorized };

struct Peer {
    PeerKey key;
    uint64_t lastPingMs = 0;
    AuthState auth = AuthState::None;
    std::wstring deviceId, deviceName;
    // Per-peer grants (spec §4). audio defaults ON because a peer that pings is
    // asking to listen; frames default OFF because a picture of the screens is
    // a second, larger thing that has to be asked for.
    bool audioOn = true;
    bool framesOn = false;
    uint32_t txTxtNuFrame = 0;   // our TXT sequence number TO this peer
};

class PeerTable {
public:
    static constexpr size_t kMaxPeers = 8;
    static constexpr uint64_t kExpiryMs = 10000;   // ~5 missed pings at 2 s

    PeerTable() { m_peers.reserve(kMaxPeers); }

    // A PING0 from `k`: create the peer or renew it.
    //
    // Returns null only when the table is full of authenticated peers, in which
    // case the caller still answers the identification request -- that reply is
    // stateless, and refusing to be discovered would be a worse failure than
    // refusing to serve.
    //
    // Eviction order (spec §3.3): anything already expired, then the oldest
    // UNAUTHENTICATED entry. An authenticated peer is never evicted to make
    // room for an unauthenticated one; otherwise anything on the network that
    // pings could push the one device that matters out of the table.
    //
    // POINTER LIFETIME: valid until the next Ping() or Expire() on this table.
    // The backing store is reserved to kMaxPeers at construction and never
    // grows, so arrivals cannot reallocate it out from under a caller; an
    // eviction still shifts elements, which is why the bound is the next
    // mutating call. Every caller uses the pointer immediately, under the same
    // lock.
    Peer* Ping(PeerKey k, uint64_t nowMs);

    // Drop peers that have stopped pinging. Authorization dies with the entry
    // by design (spec §4): it is bound to an address, and an address that has
    // gone quiet has not proved anything lately.
    void Expire(uint64_t nowMs);

    Peer* Find(PeerKey k);
    const std::vector<Peer>& All() const { return m_peers; }

    // Drop any peer's authorization that was granted to this device id. Used when
    // the person at the PC denies or revokes a device: a session it is no longer
    // entitled to must not outlive the decision.
    void RevokeDevice(const std::wstring& deviceId);

    // How many peers the AUDIO stream is currently owed. The sender's demand
    // predicate reads this; see VbanWanted.
    size_t CountAudioEntitled(bool pinSet, bool openSubscribe) const;

private:
    std::vector<Peer> m_peers;   // size <= kMaxPeers, capacity fixed at kMaxPeers
};

// ── Entitlement: the spec's §4 matrix, as predicates ─────────────────────
//
// Pure functions rather than conditions scattered through the receive loop, so
// the matrix is a table of test cases. `pinSet` is "a PIN is configured", which
// is what makes remote control possible at all: with no PIN there is no way to
// authenticate, so the remembered devices are INERT rather than
// trusted-by-memory -- clearing the PIN withdraws the grant it issued.
bool AudioEntitled(const Peer& p, bool pinSet, bool openSubscribe);

// Frames are authorized-only and opt-in per peer. Deliberately NOT covered by
// openSubscribe: that hatch exists so a standard VBAN receiver can play audio,
// and a picture of what is on the screens is not something to hand to anything
// on the network that asks.
bool FramesEntitled(const Peer& p, bool pinSet);

// Should the sender be emitting at all (spec §3.1)? This is also what the app
// layer polls to turn the engine's ring write on and off, so that an enabled
// listener with nobody connected costs no copy per audio block.
bool VbanWanted(bool enabled, size_t audioEntitledPeers,
                bool alwaysStream, bool targetSet);

// ── Authentication (spec §4) ─────────────────────────────────────────────
//
// Mirrors MDropDX12's model, so one approval concept covers both services: a PIN
// plus a per-device approval on the PC, after which that device reconnects
// without asking again.
enum class AuthOutcome { Ok, BadPin, Locked, Denied, Pending };

// The whole decision, pure. ORDER MATTERS and is part of the contract:
//   locked   outranks everything -- otherwise a brute-force attempt that
//            eventually guesses right is rewarded for persistence
//   denied   outranks a correct PIN -- saying no has to mean no
//   the PIN  decides the rest; an empty configured PIN matches NOTHING, so
//            "no PIN set" can never come to mean "no password required"
AuthOutcome DecideAuth(const std::wstring& configuredPin, const std::wstring& requestPin,
                       bool deviceAuthorized, bool deviceDenied, bool ipLocked);

// Failed-attempt counting, keyed on the IP ALONE.
//
// Deliberately not a field on Peer: a peer is keyed by ip:port and expires after
// ten seconds, so an attacker rotating source ports would get unlimited fresh
// attempts and a lockout would evaporate long before it elapsed.
class AuthStrikes {
public:
    static constexpr int kMaxStrikes = 3;
    static constexpr uint64_t kLockoutMs = 60000;
    static constexpr size_t kMaxIps = 8;

    bool Locked(uint32_t ip, uint64_t nowMs) const;
    void Strike(uint32_t ip, uint64_t nowMs);
    void Clear(uint32_t ip);          // a successful auth forgives the record

private:
    struct Row {
        uint32_t ip = 0;
        int strikes = 0;
        uint64_t lockedUntilMs = 0;
        uint64_t lastMs = 0;
    };
    // Fixed and small. Replacement takes the oldest row, and a row whose lockout
    // is still live is never the oldest -- otherwise striking from other
    // addresses would be a way to flush your own lockout out of the table.
    std::vector<Row> m_rows;
};

}} // namespace mdxm::vban
