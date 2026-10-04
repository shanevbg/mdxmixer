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

private:
    struct Impl;
    Impl* m_impl = nullptr;
    uint32_t m_rate = 0;
};

} // namespace mdxm
