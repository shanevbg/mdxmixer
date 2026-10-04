#pragma once
// WASAPI shared-mode capture on its own thread. Decodes every packet to interleaved
// stereo float at the SOURCE rate and hands it to the callback (mono duplicated,
// >2ch keeps the first two — passthrough-monitor lessons).
//
// All WASAPI/COM work happens on the stream's own thread — never on a notification
// callback, never on someone else's audio thread (fj#401).
#include <cstdint>
#include <functional>
#include <string>

namespace mdxm {

class CaptureStream {
public:
    ~CaptureStream();
    using OnFrames = std::function<void(const float* interleaved, size_t frames)>;
    // loopback=true captures a render endpoint's output (unused by v1's graph; the
    // flag exists because the same class serves either endpoint direction).
    // Blocks until the stream is running or failed; on failure *err says why.
    bool Start(const std::wstring& endpointId, bool loopback, OnFrames cb, std::wstring* err);
    void Stop();
    uint32_t SourceRate() const { return m_rate; }
    bool Invalidated() const;   // device vanished mid-stream; thread has exited

private:
    struct Impl;
    Impl* m_impl = nullptr;
    uint32_t m_rate = 0;
};

} // namespace mdxm
