#pragma once
// The VBAN endpoint: one UDP socket, two threads (spec §3.2, §3.3).
//
// This is the first thing in mdxmixer that accepts traffic from the network, so
// the shape is deliberate. The LISTENER is up whenever vban.enabled, because the
// phone has to be able to subscribe at any moment without anyone touching the
// PC. EMISSION is separate and subscriber-gated: nothing leaves this machine
// until a peer has asked for it, which is what Wanted() reports and what the app
// layer uses to decide whether the engine should even be filling the ring.
//
// Threading:
//   receive thread  blocking recvfrom; parses, answers pings, and (phase 2)
//                   marshals control records onto the UI thread
//   sender  thread  drains the ring on a paced schedule and sends
//   control thread  Start/Stop/UpdateConfig/Status, from the UI thread
// m_mutex guards the peer table and the config snapshot; counters are atomics so
// Status() never has to wait on the sender. Neither thread touches the audio
// thread's state except through the lock-free ring.
#include "config/config.h"           // VbanConfig
#include "dsp/limiter.h"             // SoftLimiter
#include "dsp/ring_buffer.h"
#include "ipc/mixer_control.h"       // VbanStatus, VbanPeerRow
#include "net/display_capture.h"
#include "net/vban_pacer.h"
#include "net/vban_peers.h"
#include "net/vban_protocol.h"
#include <map>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace mdxm {

class VbanServer {
public:
    struct Callbacks {
        // One MDXM record in, its replies out. Bound to the shared
        // marshal-onto-the-UI-thread path (AppController::DispatchToUi), so a
        // record arriving over the network is handled on the same single thread
        // as one arriving over the pipe. Empty means inbound control is counted
        // and dropped.
        std::function<std::vector<std::wstring>(const std::wstring&)> dispatch;
        // A device with the right PIN that has never been approved. Raised ONCE
        // per device while it waits -- the phone re-sends AUTH while pending, and
        // a prompt per resend would be unusable. Called on the receive thread, so
        // the handler must only post.
        std::function<void(const std::wstring& deviceId, const std::wstring& name)> onAuthPending;
        // A device that has just become authorized, so the app layer can persist
        // it and the next connection is instant.
        std::function<void(const std::wstring& deviceId, const std::wstring& name)> onAuthorized;
    };

    VbanServer();
    ~VbanServer();
    VbanServer(const VbanServer&) = delete;
    VbanServer& operator=(const VbanServer&) = delete;

    // `ring` is Engine::VbanRing() and must outlive this server. A bind failure
    // returns false with the reason in *err AND in Status().lastError: the
    // caller logs it and carries on, because a mixer whose network stream did
    // not start is still a working mixer.
    bool Start(const VbanConfig& cfg, RingBuffer* ring, uint32_t mixRate,
               Callbacks cb, std::wstring* err);
    void Stop();

    // Control thread. Everything except the socket is re-derived in place; a
    // changed port or bind address rebinds.
    void UpdateConfig(const VbanConfig& cfg);
    void UpdateMixRate(uint32_t rate);
    // The PC's own share of the delay the listener hears, in ms: the mix cushion
    // plus a packet plus the pacer's wake. The app layer computes it because the
    // cushion belongs to the engine.
    void SetSrcLatencyBase(int ms);

    // What the person at the PC said about a pending device. Control thread.
    //
    // Allow is remembered HERE as well as in the config the app layer persists,
    // so the decision takes effect on the device's next AUTH without waiting for
    // a config round trip. Deny is remembered until mdxmixer restarts: a refusal
    // has to be terminal, or the phone sits in "waiting for approval" for ever
    // and the user is asked again on every resend.
    void AuthorizationResult(const std::wstring& deviceId, bool allow);

    // Forget a device entirely: drop its live session, its approval, and the
    // record that the PC was already asked about it. Used by Revoke, which is a
    // different statement from a denial -- the device may ask again, and the next
    // AUTH raises the prompt afresh.
    void ForgetDevice(const std::wstring& deviceId);

    VbanStatus Status() const;
    std::vector<VbanPeerRow> Peers() const;
    bool Running() const { return m_running.load(std::memory_order_acquire); }
    // The receive socket failed for a reason that was not Stop() -- a resume that
    // left it dead, an adapter pulled. The control tick notices this and rebuilds
    // the server; it is not something Stop() can key on, because m_running stays
    // true so the thread joins still work.
    bool Failed() const { return m_failed.load(std::memory_order_acquire); }
    // Is anybody owed audio? What the app layer polls to turn the engine's ring
    // write on and off, so an enabled listener with nobody connected costs no
    // copy per audio block.
    bool Wanted() const { return m_wanted.load(std::memory_order_relaxed); }

private:
    void ReceiveLoop();
    void SendLoop();
    // One inbound TXT record. Receive thread; `from` is the sender's address and
    // `peerKey` its table key. Returns the replies to send back, already chunked.
    std::vector<std::wstring> HandleTxt(const vban::PeerKey& key,
                                        const std::wstring& record);
    std::vector<std::wstring> HandleAuth(const vban::PeerKey& key,
                                         const std::wstring& record);
    // Screen captures. Its own thread because duplication and a JPEG encode are
    // slow enough to disturb the audio cadence if they shared the sender's.
    void CaptureLoop();
    // Who is owed pictures: an entitled peer that asked, or the always-stream
    // target when alwaysFrames is on. m_mutex held by the caller.
    void CollectFrameRecipientsLocked();
    static unsigned __stdcall ReceiveTrampoline(void* self);
    static unsigned __stdcall SendTrampoline(void* self);
    static unsigned __stdcall CaptureTrampoline(void* self);
    // Control thread, m_mutex held: rebuild the identity block and the pacer
    // from m_cfg and m_mixRate.
    void RederiveLocked();
    bool StopRequested() const;

    mutable std::mutex m_mutex;
    Callbacks m_cb;
    VbanConfig m_cfg;                       // snapshot; guarded by m_mutex
    vban::AuthStrikes m_strikes;            // per-IP, outlives peer entries
    // Decisions the person at the PC has made since this process started.
    // `approved` supplements the config list so an Allow takes effect on the next
    // AUTH without waiting for the config to be written and pushed back;
    // `denied` makes a refusal terminal rather than a prompt that returns.
    std::vector<std::wstring> m_approvedDevices, m_deniedDevices;
    // Devices the PC has already been asked about, so a phone re-sending AUTH
    // while it waits does not raise a second prompt.
    std::vector<std::wstring> m_promptedDevices;
    vban::Ping0 m_identity{};               // rebuilt on config/rate change
    vban::PeerTable m_peers;
    RingBuffer* m_ring = nullptr;
    uint32_t m_mixRate = 48000;
    int m_srIndex = 3;                      // 48 kHz
    size_t m_framesPerPacket = vban::kFramesPerPacketI16;
    int m_srcLatencyBase = 0;
    // The always-stream target, parsed once rather than per packet.
    bool m_targetValid = false;
    uint32_t m_targetIp = 0;                // network byte order
    unsigned short m_targetPort = 0;

    uintptr_t m_sock = ~(uintptr_t)0;       // SOCKET; INVALID_SOCKET at rest
    void* m_stopEvent = nullptr;            // HANDLE, manual reset
    void* m_rxThread = nullptr;
    void* m_txThread = nullptr;
    void* m_capThread = nullptr;
    std::atomic<bool> m_running{ false };
    std::atomic<bool> m_wanted{ false };
    std::atomic<bool> m_failed{ false };   // receive socket died unexpectedly

    // Sender-thread state. The scratch buffers are members so the send path
    // allocates nothing once it is running.
    vban::Pacer m_pacer;
    SoftLimiter m_limiter;
    std::vector<float> m_block;
    std::vector<uint8_t> m_packet;
    std::vector<unsigned long long> m_recipients;   // packed sockaddr_in payloads
    uint32_t m_nuFrame = 0;                 // shared across recipients, monotonic
    uint64_t m_lastExpireMs = 0;

    // Capture-thread state.
    DisplayCapture m_capture;
    std::vector<unsigned long long> m_frameRecipients;   // packed sockaddr_in
    // The last picture of each display, so a peer that has just asked for frames
    // sees something at once rather than waiting for that screen to change (spec
    // §5) -- a still desktop could otherwise leave the panel blank indefinitely.
    std::map<int, std::vector<uint8_t>> m_lastFrame;
    std::map<int, uint32_t> m_frameSeq;                  // per display, per spec
    // Set when a peer newly becomes frames-entitled: the cached pictures go out
    // on the capture thread's next wake rather than from the receive thread.
    std::atomic<bool> m_sendCachedFrames{ false };

    std::atomic<uint64_t> m_sent{ 0 }, m_starved{ 0 }, m_txtDropped{ 0 },
                          m_framesSent{ 0 };
    std::atomic<int> m_behindMs{ 0 };
    std::wstring m_lastError;               // guarded by m_mutex
};

} // namespace mdxm
