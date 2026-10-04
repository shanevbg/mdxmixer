#pragma once
// WASAPI shared-mode event-driven render on its own thread. Each event:
// GetCurrentPadding -> pull bufFrames-padding stereo float frames from the callback
// -> convert to the device mix format -> ReleaseBuffer.
//
// All WASAPI/COM work happens on the stream's own thread (fj#401).
#include <cstdint>
#include <functional>
#include <string>

namespace mdxm {

class RenderStream {
public:
    ~RenderStream();
    using PullFrames = std::function<void(float* interleaved, size_t frames)>;
    // Blocks until the stream is running or failed; on failure *err says why.
    bool Start(const std::wstring& endpointId, PullFrames cb, std::wstring* err);
    void Stop();
    uint32_t DeviceRate() const { return m_rate; }
    bool Invalidated() const;   // AUDCLNT_E_DEVICE_INVALIDATED seen; thread has exited
    // The render event stopped being signalled for a second while nothing
    // returned an error. Thread has exited, device released.
    //
    // A SEPARATE condition from Invalidated(), because a stalled stream never
    // sets that flag and this is the state a Bluetooth endpoint reaches across
    // a Modern Standby resume: still listed, still DEVICE_STATE_ACTIVE, still
    // answering its volume and its meter, no longer clocking. Before this
    // existed the wait was INFINITE and nothing in the program could see it;
    // see the measurements at the wait itself.
    bool Stalled() const;
    // Either way of being dead. What a caller deciding whether to restart
    // actually wants to ask -- the distinction above is for the log.
    bool Dead() const { return Invalidated() || Stalled(); }

private:
    struct Impl;
    Impl* m_impl = nullptr;
    uint32_t m_rate = 0;
};

} // namespace mdxm
