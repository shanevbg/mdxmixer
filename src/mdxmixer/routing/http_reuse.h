#pragma once
// When a kept-open HTTP connection may be reused, and what to do when one that
// was reused fails.
//
// WHY THIS EXISTS. Every Sonar call -- a mute, a fader, a channel read -- built
// its own WinHttpOpen session, its own WinHttpConnect, and its own request, and
// closed all three again. Measured over the pipe on 2026-10-05: a write to a
// Sonar channel cost 8-78 ms (mean 35), against 0.3 ms for a channel mdxmixer
// owns itself. That gap is connection setup, not SteelSeries being slow, and it
// is what "can we make the system a little more responsive with the sonar
// muting" is asking about. GG speaks ordinary HTTP/1.1 on loopback and keeps a
// connection alive perfectly well; nothing was giving it the chance.
//
// WHAT THE OLD CODE BOUGHT, and must not be thrown away with it: tearing
// everything down after every call meant a GG that had restarted could never
// be answered through a stale handle. Reuse has to re-earn that, which is what
// this file is. The rule is deliberately narrow:
//
//   * only a TRANSPORT failure is retried. A 404 or a 500 is GG answering, and
//     answering badly; re-connecting cannot change it and would double every
//     error's cost.
//   * only a POOLED connection is retried. If a connection opened seconds ago
//     cannot reach GG, GG is gone -- that is a real failure and the caller's
//     own re-discovery handles it.
//   * once. A retry loop against a service that is down is a hang with extra
//     steps, and SteelSeries hangs regularly enough that sonar_control.h exists
//     because of it.
#include <string>

namespace mdxm {

// Is the kept-open connection pointed at the host this request wants?
//
// Compared in full -- scheme, host and port -- because discovery can move GG
// to a different port, and a pooled connection to the OLD one would fail in a
// way that looks exactly like GG being down.
inline bool PooledConnectionMatches(const std::wstring& poolHost, unsigned short poolPort,
                                    bool poolHttps, const std::wstring& wantHost,
                                    unsigned short wantPort, bool wantHttps) {
    if (poolHost.empty() || poolPort == 0) return false;
    return poolHost == wantHost && poolPort == wantPort && poolHttps == wantHttps;
}

// A request failed. Should it be tried once more on a freshly opened
// connection? `attempt` is 0 for the first try.
inline bool RetryOnFreshConnection(bool transportFailed, bool usedPooledConnection, int attempt) {
    return transportFailed && usedPooledConnection && attempt == 0;
}

} // namespace mdxm
