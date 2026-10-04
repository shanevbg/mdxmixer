#pragma once
// SteelSeries Sonar's own channels as mdxmixer channels.
//
// PORTED from MDropDX12's mixer_sonar_http.{h,cpp} and the volume half of
// mixer_provider_sonar.{h,cpp}. The transport, the discovery path, the write
// path's argument order and the master's behaviour were all established over
// there against a live GG; the comments carrying those measurements are
// carried across with the code, because re-deriving them costs hours.
//
// It exists because Sonar's virtual endpoints cannot be driven through
// Windows: measured in mdx12, `SteelSeries Sonar - Aux` accepts
// SetMasterVolumeLevelScalar, returns success, and holds its level at 1.0.
// Only Sonar can move a Sonar channel. That is why mdxmixer's own endpoint
// faders are no substitute, and why Shane could adjust Aux personal and
// streaming in mdx12 but not here.
//
// In streamer mode every Sonar channel carries independent monitoring and
// streaming levels, which is exactly mdxmixer's Personal/Streaming pair — one
// channel, two faders, the same two-knob model the rest of the mixer uses.
//
// Control thread only: it does network I/O with short timeouts.
#include <string>
#include <vector>

namespace mdxm {

// ── Transport ────────────────────────────────────────────────────────────
//
// Knows nothing about mixer channels. Its whole job is: find the sub-app,
// speak to it, and never block a caller for long.
//
// Discovery is coreProps.json -> ggEncryptedAddress -> ONE HTTPS GET /subApps
// -> the sonar sub-app's plain-HTTP address. Only that first call needs TLS,
// and its certificate is self-signed for localhost, so validation is relaxed
// for that request alone and for nothing else.
//
// The port MOVES when GG restarts. A connection-level failure therefore
// triggers exactly one re-discovery before the caller is told no: a GG that
// restarted onto a new port and a GG that hung are otherwise identical.
class SonarHttp {
public:
    // Deliberately short. SteelSeries hangs regularly — that is the whole
    // reason sonar_control.h exists — and a wedged GG must park one call
    // rather than the application.
    static const int kConnectTimeoutMs = 1000;
    static const int kReceiveTimeoutMs = 2000;

    bool Discover();
    bool IsDiscovered() const { return !m_base.empty(); }
    std::wstring BaseUrl() const { return m_base; }

    // Both retry discovery once on a connection-level failure, never on an
    // HTTP status. Both return false for any non-2xx.
    bool Get(const std::wstring& path, std::wstring& body);
    bool Put(const std::wstring& path);

    // Drops the cached address, so the next call re-discovers.
    void Forget() { m_base.clear(); }

    // Test seam: point the client at a chosen address without discovery.
    //
    // `pinned` suppresses re-discovery. Without it a dead address silently
    // heals — the connection fails, discovery finds the real GG, and the retry
    // succeeds — so the timeout path could never be observed. That healing is
    // exactly right in production and exactly wrong in a test of the timeout.
    void SetBaseUrlForTest(const std::wstring& base, bool pinned = false) {
        m_base = base;
        m_pinned = pinned;
    }

private:
    bool Request(const wchar_t* verb, const std::wstring& path,
                 std::wstring* body, bool allowRediscover);

    std::wstring m_base;    // "http://127.0.0.1:32371", no trailing slash
    bool m_pinned = false;  // test only: never re-discover
};

// ── Channels ─────────────────────────────────────────────────────────────

// One Sonar channel's two levels. Named for mdxmixer's halves rather than
// Sonar's: Sonar calls them monitoring and streaming, mdxmixer calls the first
// one personal everywhere else, and one vocabulary per codebase is worth more
// than matching the wire.
struct SonarChannel {
    std::wstring key;       // "aux", "game", "chatRender", "chatCapture",
                            // "media", "masters" — Sonar's own
    std::wstring label;     // "Aux", "Game", "Chat", "Mic", "Media", "Master"
    float personalVol = 1.0f;
    bool  personalMute = false;
    float streamingVol = 1.0f;
    bool  streamingMute = false;
    // The master's mute is destructive and is not offered. See the
    // measurements in Refresh().
    bool  canMute = true;
};

// The cached view, refreshed only when someone is looking.
class SonarChannels {
public:
    // True when Sonar answered and the channel list is usable.
    bool Available() const { return m_available; }
    bool StreamMode() const { return m_streamMode; }

    // Re-read the levels. Rate-limited to 1 Hz unless `force`, which is what a
    // write uses to replace its optimistic value with the truth.
    //
    // Returns false when Sonar could not be reached; the previous snapshot is
    // kept rather than blanked, so a momentary GG hang does not make every
    // fader vanish and come back.
    bool Refresh(bool force);

    std::vector<SonarChannel> Channels() const { return m_channels; }

    // `key` is Sonar's, without the "sonar:" prefix. `personal` picks which of
    // the two faders moves.
    bool SetVolume(const std::wstring& key, bool personal, float vol);
    bool SetMute(const std::wstring& key, bool personal, bool mute);

    // Test seam, as on SonarHttp.
    SonarHttp& Http() { return m_http; }

private:
    // Builds a write path. CONFIRMED in mdx12 against a live GG on 2026-08-30:
    // the SLIDER comes BEFORE the CHANNEL, which is the reverse of the obvious
    // reading of the GET shape. Channel-first returns 400 "Request validation
    // error", and so does a nonsense channel, so the error distinguishes
    // nothing.
    std::wstring WritePath(const std::wstring& key, bool personal,
                           const wchar_t* property,
                           const std::wstring& value) const;

    SonarHttp m_http;
    bool m_available = false;
    bool m_streamMode = true;
    unsigned m_lastTick = 0;
    std::vector<SonarChannel> m_channels;
};

// "sonar:aux" -> "aux". Empty when the id is not one of ours.
std::wstring SonarKeyFromChannelId(const std::wstring& id);

// The write path, as a free function so it can be tested without a server.
//
// Pure, and worth testing, because the two things it encodes are the two most
// expensive facts in this file and neither is guessable: the slider segment
// comes BEFORE the channel segment, and the master is written as `master`
// while it is read as `masters`. Both were established against a live GG, and
// both fail as a 400 with the fader unmoved — which reads as "Sonar refuses to
// be driven from outside", not as a wrong URL.
std::wstring SonarWritePath(const std::wstring& key, bool personal,
                            bool streamMode, const wchar_t* property,
                            const std::wstring& value);

} // namespace mdxm
