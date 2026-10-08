#include "net/vban_peers.h"
#include <algorithm>

namespace mdxm { namespace vban {

namespace {
// How long since this peer last pinged. Guarded against a tick that has not
// advanced: unsigned arithmetic on a backwards tick would underflow to an
// enormous age and expire every peer at once, which presents as a stream
// dropping for no reason any log could explain.
uint64_t AgeMs(uint64_t nowMs, uint64_t lastMs) {
    return nowMs > lastMs ? nowMs - lastMs : 0;
}
} // namespace

Peer* PeerTable::Find(PeerKey k) {
    for (auto& p : m_peers)
        if (p.key == k) return &p;
    return nullptr;
}

Peer* PeerTable::Ping(PeerKey k, uint64_t nowMs) {
    if (Peer* p = Find(k)) { p->lastPingMs = nowMs; return p; }
    if (m_peers.size() >= kMaxPeers) {
        int victim = -1;
        // Anything already stale goes first, authorized or not: a table that
        // has gone quiet must not start refusing live peers.
        for (size_t i = 0; i < m_peers.size(); ++i)
            if (AgeMs(nowMs, m_peers[i].lastPingMs) > kExpiryMs) { victim = (int)i; break; }
        if (victim < 0) {
            // Then the oldest UNAUTHENTICATED entry, and only those: an
            // authorized device is never pushed out by something that has not
            // authenticated.
            uint64_t oldest = UINT64_MAX;
            for (size_t i = 0; i < m_peers.size(); ++i)
                if (m_peers[i].auth != AuthState::Authorized &&
                    m_peers[i].lastPingMs < oldest) {
                    oldest = m_peers[i].lastPingMs;
                    victim = (int)i;
                }
        }
        if (victim < 0) return nullptr;   // every slot authorized: no entry for this one
        m_peers.erase(m_peers.begin() + victim);
    }
    Peer p;
    p.key = k;
    p.lastPingMs = nowMs;
    m_peers.push_back(p);   // capacity is reserved, so this cannot reallocate
    return &m_peers.back();
}

void PeerTable::Expire(uint64_t nowMs) {
    m_peers.erase(std::remove_if(m_peers.begin(), m_peers.end(),
        [nowMs](const Peer& p) { return AgeMs(nowMs, p.lastPingMs) > kExpiryMs; }),
        m_peers.end());
}

void PeerTable::RevokeDevice(const std::wstring& deviceId) {
    if (deviceId.empty()) return;
    for (auto& p : m_peers)
        if (p.deviceId == deviceId) p.auth = AuthState::None;
}

size_t PeerTable::CountAudioEntitled(bool pinSet, bool openSubscribe) const {
    size_t n = 0;
    for (const auto& p : m_peers)
        if (AudioEntitled(p, pinSet, openSubscribe)) ++n;
    return n;
}

bool AudioEntitled(const Peer& p, bool pinSet, bool openSubscribe) {
    // The hatch for standard VBAN tools, which cannot authenticate because the
    // protocol has no notion of it. It grants audio and nothing else.
    if (openSubscribe) return true;
    // No PIN means no way to authenticate, so the remembered devices are inert
    // rather than trusted from memory.
    if (!pinSet) return false;
    return p.auth == AuthState::Authorized && p.audioOn;
}

bool FramesEntitled(const Peer& p, bool pinSet) {
    return pinSet && p.auth == AuthState::Authorized && p.framesOn;
}

bool VbanWanted(bool enabled, size_t audioEntitledPeers,
                bool alwaysStream, bool targetSet) {
    if (!enabled) return false;
    // A configured target with alwaysStream is the one case where emission
    // happens with nobody subscribed -- for a receiver that never pings.
    return audioEntitledPeers > 0 || (alwaysStream && targetSet);
}

AuthOutcome DecideAuth(const std::wstring& configuredPin, const std::wstring& requestPin,
                       bool deviceAuthorized, bool deviceDenied, bool ipLocked) {
    // The order is the security property; see the header.
    if (ipLocked) return AuthOutcome::Locked;
    if (deviceDenied) return AuthOutcome::Denied;
    // An empty configured PIN matches nothing at all, including an empty
    // request. The caller should never reach here with one -- no PIN means TXT
    // is dead -- but the alternative reading of this line is that turning the
    // PIN off turns authentication off, which is the opposite of what it means.
    if (configuredPin.empty() || requestPin != configuredPin) return AuthOutcome::BadPin;
    return deviceAuthorized ? AuthOutcome::Ok : AuthOutcome::Pending;
}

bool AuthStrikes::Locked(uint32_t ip, uint64_t nowMs) const {
    for (const auto& r : m_rows)
        if (r.ip == ip) return nowMs < r.lockedUntilMs;
    return false;
}

void AuthStrikes::Strike(uint32_t ip, uint64_t nowMs) {
    for (auto& r : m_rows) {
        if (r.ip != ip) continue;
        r.lastMs = nowMs;
        if (++r.strikes >= kMaxStrikes) {
            r.lockedUntilMs = nowMs + kLockoutMs;
            r.strikes = 0;      // the lockout replaces the count
        }
        return;
    }
    if (m_rows.size() >= kMaxIps) {
        // Replace the oldest row -- but never one whose lockout is still live,
        // or striking from eight other addresses would be a way to clear your
        // own lockout.
        int victim = -1;
        uint64_t oldest = UINT64_MAX;
        for (size_t i = 0; i < m_rows.size(); ++i) {
            if (nowMs < m_rows[i].lockedUntilMs) continue;
            if (m_rows[i].lastMs < oldest) { oldest = m_rows[i].lastMs; victim = (int)i; }
        }
        // Every row locked: this attempt simply goes uncounted, which is the
        // safe failure -- eight addresses are already locked out.
        if (victim < 0) return;
        m_rows.erase(m_rows.begin() + victim);
    }
    Row r;
    r.ip = ip;
    r.strikes = 1;
    r.lastMs = nowMs;
    m_rows.push_back(r);
}

void AuthStrikes::Clear(uint32_t ip) {
    m_rows.erase(std::remove_if(m_rows.begin(), m_rows.end(),
        [ip](const Row& r) { return r.ip == ip; }), m_rows.end());
}

}} // namespace mdxm::vban
