#pragma once
// A low-resolution JPEG of each display, for the network stream (spec §5).
//
// DXGI Desktop Duplication, and the choice matters: duplication is the only API
// that captures a whole monitor INCLUDING another process's swapchain
// presentation. PrintWindow -- which is what ui/window_capture.cpp uses, and what
// its comments explain the limits of -- renders one window's own content and
// cannot be aimed at a monitor at all. Since the thing most worth seeing on these
// screens is a DX12 visualiser, that distinction is the whole feature.
//
// Runs on its own thread, never the UI thread: MDXM_CAPTURE goes through the
// marshalled IPC path with a five-second timeout, and a per-second full-screen
// capture there would spend that budget on pictures.
#include <cstdint>
#include <string>
#include <vector>

namespace mdxm {

struct CapturedFrame {
    // The `n` in \\.\DISPLAYn, which is NOT an enumeration index: on a
    // three-monitor desktop the first output enumerated is DISPLAY1, but on a
    // machine whose panels have been replugged the numbers are neither contiguous
    // nor bounded by the monitor count. MDR_Android learned this the other way
    // round -- building a command from the enumeration index aimed at the wrong
    // monitor entirely -- so the number travels with the picture.
    int deviceNumber = 0;
    std::vector<uint8_t> jpeg;
};

class DisplayCapture {
public:
    ~DisplayCapture();
    DisplayCapture() = default;
    DisplayCapture(const DisplayCapture&) = delete;
    DisplayCapture& operator=(const DisplayCapture&) = delete;

    // One duplication per output whose GDI name parses as \\.\DISPLAYn with
    // 1 <= n <= kMaxDisplayNumber. Anything else is skipped and counted: it has
    // no VIDEOn stream to go to.
    bool Init(std::wstring* err);
    void Shutdown();

    // The displays that have CHANGED since the last call, as JPEGs. A still
    // desktop returns nothing, which is what makes a 2 fps stream nearly free.
    //
    // `maxEdge` caps the long edge; the downscale happens on the GPU, so a 4K
    // frame is read back as a few hundred KB rather than the 33 MB full-res copy
    // MDropDX12's canvas_metric.h documents as the mistake.
    std::vector<CapturedFrame> Poll(int maxEdge, int quality);

    // The desktop went away under us -- a UAC prompt, a mode change, a session
    // switch. The caller shuts down and re-initialises with a backoff; audio is
    // unaffected, because none of this touches it.
    bool Lost() const { return m_lost; }
    uint64_t SkippedOutputs() const { return m_skipped; }

    static constexpr int kMaxDisplayNumber = 8;

private:
    struct Output;                    // holds the COM pointers; see the .cpp
    std::vector<Output*> m_outputs;
    void* m_device = nullptr;         // ID3D11Device
    void* m_context = nullptr;        // ID3D11DeviceContext
    void* m_wic = nullptr;            // IWICImagingFactory
    bool m_lost = false;
    uint64_t m_skipped = 0;
};

} // namespace mdxm
