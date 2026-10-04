#include "ipc/stream_feed.h"
#include <windows.h>
#include <atomic>
#include <cstring>

namespace mdxm {

namespace {
std::atomic<float> g_gain{ 1.0f };

uint32_t RoundUpPow2(uint32_t v) {
    if (v < 1024) v = 1024;
    uint32_t p = 1;
    while (p < v && p < (1u << 30)) p <<= 1;
    return p;
}
} // namespace

size_t StreamFeedReadable(uint64_t writeFrames, uint64_t readFrames,
                          uint32_t capacityFrames, uint64_t* startFrame) {
    if (capacityFrames == 0 || writeFrames <= readFrames) {
        if (startFrame) *startFrame = writeFrames;
        return 0;
    }
    uint64_t behind = writeFrames - readFrames;
    uint64_t start = readFrames;
    if (behind > capacityFrames) {
        // Lapped: those frames are gone. Resync to the oldest the ring still
        // holds rather than replaying a lap-old buffer, which is what a naive
        // reader does and it sounds like a stutter loop.
        start = writeFrames - capacityFrames;
        behind = capacityFrames;
    }
    if (startFrame) *startFrame = start;
    return (size_t)behind;
}

StreamFeed::~StreamFeed() { Close(); }

bool StreamFeed::Open(uint32_t sampleRate, uint32_t channels, uint32_t capacityFrames,
                      std::wstring* err) {
    Close();
    if (channels == 0) channels = 2;
    m_channels = channels;
    m_capacity = RoundUpPow2(capacityFrames);

    const size_t bytes = sizeof(StreamFeedHeader) +
                         (size_t)m_capacity * m_channels * sizeof(float);
    HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                  (DWORD)(bytes >> 32), (DWORD)(bytes & 0xFFFFFFFF),
                                  kStreamFeedName);
    const bool existed = (GetLastError() == ERROR_ALREADY_EXISTS);
    if (!h) {
        if (err) *err = L"could not create the stream feed mapping";
        return false;
    }
    // REFUSED rather than shared. CreateFileMappingW with a name that is
    // already taken hands back the EXISTING section, at whatever size its
    // creator chose -- so this would go on to write its own geometry into a
    // header it does not own, and copy frames into a buffer that may be
    // smaller than it believes.
    //
    // mdxmixer#3 found a section reporting capacityFrames=4096, a number the
    // only call site here cannot produce (m_mixRate * 2 rounds up to
    // 131072). Something else had created it. Two writers over one section
    // is also the plainest explanation for magic being zeroed by one while
    // the other kept writing: whoever closes first clears the readiness flag
    // out from under the other.
    //
    // There is exactly one writer by design, so a name that is taken means
    // something is wrong and saying so beats sharing.
    if (existed) {
        CloseHandle(h);
        if (err) *err = L"the stream feed name is already taken -- another "
                        L"mdxmixer, or another writer, has it open";
        return false;
    }
    void* view = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, bytes);
    if (!view) {
        CloseHandle(h);
        if (err) *err = L"could not map the stream feed";
        return false;
    }
    m_mapping = h;
    m_view = view;
    auto* hdr = (StreamFeedHeader*)view;
    m_samples = (float*)((char*)view + sizeof(StreamFeedHeader));

    // Zero the audio before announcing the format: a reader that attaches
    // between these two writes must not find last run's samples described by
    // this run's header.
    memset(m_samples, 0, (size_t)m_capacity * m_channels * sizeof(float));
    hdr->sampleRate = sampleRate;
    hdr->channels = m_channels;
    hdr->capacityFrames = m_capacity;
    hdr->reserved = 0;
    hdr->writeFrames = 0;
    hdr->writeTickMs = GetTickCount64();
    hdr->version = kStreamFeedVersion;
    std::atomic_thread_fence(std::memory_order_release);
    hdr->magic = kStreamFeedMagic;        // published last: the readiness flag
    // LAST of all: until this is set, Write sees null and does nothing, so
    // the audio thread cannot reach a half-built header.
    m_header.store(hdr, std::memory_order_release);
    return true;
}

void StreamFeed::Close() {
    // Shut the gate FIRST, then wait for whoever is already through it.
    //
    // The old order -- zero magic, unmap, then null the pointer -- let the
    // audio thread be inside Write with a pointer into a view that had just
    // been unmapped. mdxmixer#3 saw the visible half of that: a writer still
    // advancing writeFrames after magic had gone to zero.
    StreamFeedHeader* hdr = m_header.exchange(nullptr, std::memory_order_acq_rel);

    // Any Write that got past the gate before the exchange is counted here;
    // any that arrives after it loads null and leaves. Blocks are ~10 ms, so
    // this spins for microseconds in practice -- and it is the control
    // thread waiting, never the audio one.
    for (int spin = 0; m_inWrite.load(std::memory_order_acquire) != 0 && spin < 1000; ++spin)
        Sleep(1);

    // Only now is it safe to touch the view.
    if (hdr) hdr->magic = 0;              // readers stop immediately
    if (m_view) UnmapViewOfFile(m_view);
    if (m_mapping) CloseHandle((HANDLE)m_mapping);
    m_view = nullptr;
    m_mapping = nullptr;
    m_samples = nullptr;
}

void StreamFeed::Write(const float* interleaved, size_t frames) {
    if (!interleaved || frames == 0) return;

    // Count IN first, then look: a Close that happens between the two sees a
    // non-zero count and waits. Looking first and counting after is the race
    // -- Close could null the pointer, see zero in flight, and unmap while
    // this thread still held the pointer it had just loaded.
    m_inWrite.fetch_add(1, std::memory_order_acq_rel);
    StreamFeedHeader* hdr = m_header.load(std::memory_order_acquire);
    if (!hdr) {
        m_inWrite.fetch_sub(1, std::memory_order_acq_rel);
        return;
    }

    const float gain = g_gain.load(std::memory_order_relaxed);
    const uint32_t mask = m_capacity - 1;          // capacity is a power of two
    uint64_t pos = hdr->writeFrames;

    for (size_t f = 0; f < frames; ++f) {
        float* dst = m_samples + (size_t)((pos + f) & mask) * m_channels;
        const float* src = interleaved + f * m_channels;
        for (uint32_t c = 0; c < m_channels; ++c) dst[c] = src[c] * gain;
    }
    // Samples first, then the cursor: a reader that sees the new count is
    // guaranteed to see the samples it counts.
    std::atomic_thread_fence(std::memory_order_release);
    hdr->writeFrames = pos + frames;
    hdr->writeTickMs = GetTickCount64();
    m_inWrite.fetch_sub(1, std::memory_order_acq_rel);
}

void StreamFeed::SetGain(float gain) {
    if (gain < 0.0f) gain = 0.0f;
    if (gain > 2.0f) gain = 2.0f;
    g_gain.store(gain, std::memory_order_relaxed);
}

float StreamFeed::Gain() const { return g_gain.load(std::memory_order_relaxed); }

} // namespace mdxm
