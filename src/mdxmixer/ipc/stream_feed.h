#pragma once
// The streaming mix, handed to another process without an audio device.
//
// mdxmixer produces two mixes: the personal one, which on this machine runs at
// 1-10% because that is the level its owner listens at, and the streaming one,
// which runs at full level. MDropDX12 wants the streaming mix to visualise,
// and reading the personal mix would give it a signal 20-40 dB down and would
// blank the visualiser whenever a channel was muted personally.
//
// The obvious way to carry it between two processes is a virtual audio cable,
// and that was rejected deliberately: a cable is another audio driver, on a
// machine where an audio driver had just cost an afternoon, and it consumes an
// endpoint that then has to be kept out of every device list. This is a plain
// shared-memory ring instead. No driver, no endpoint, nothing for a crashed
// audio stack to hold hostage, and when mdxmixer is not running the reader
// simply sees no data rather than a dead device.
//
// ── The contract, for the reader in the other repository ────────────────
//
// A named file mapping, "Local\\mdxmixer_stream_v1", laid out as
// StreamFeedHeader followed by capacityFrames * channels floats, interleaved.
//
// Single writer, any number of readers, no locks. The writer copies its frames
// into the ring and then publishes `writeFrames` with a release store; a
// reader loads `writeFrames` with acquire, reads what it has not seen, and
// keeps its own cursor. There is no handshake and no back-pressure: a reader
// that stops reading is simply overwritten, which is the right behaviour for
// audio — the writer is an audio thread and must never wait for anybody.
//
// A reader that has fallen further behind than the ring holds has missed audio
// and must resync to the newest data rather than play a lap-old buffer; see
// StreamFeedReadable.
//
// `writeTickMs` is GetTickCount64 at the last write. A reader uses it to tell
// "mdxmixer is running and the mix is silent" from "mdxmixer has gone away",
// which are the same thing in the samples themselves.
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <string>

namespace mdxm {

constexpr uint32_t kStreamFeedMagic   = 0x5358444D;   // 'MDXS', little-endian
constexpr uint32_t kStreamFeedVersion = 1;
constexpr wchar_t  kStreamFeedName[]  = L"Local\\mdxmixer_stream_v1";

struct StreamFeedHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t sampleRate;
    uint32_t channels;
    uint32_t capacityFrames;
    uint32_t reserved;
    // Total frames EVER written, not an index. The ring position is
    // writeFrames % capacityFrames; the total is what lets a reader work out
    // how far behind it has fallen, which an index alone cannot say.
    uint64_t writeFrames;
    uint64_t writeTickMs;
};

// How many frames a reader holding `readFrames` may take, and where to start.
//
// Pure, because the two ways to get this wrong are both silent: reading a lap
// behind plays old audio over and over, and reading past the writer plays
// whatever was in the buffer last time round.
//
// Returns the number of frames available; *startFrame is the absolute frame to
// begin at, which equals readFrames unless the reader had fallen too far
// behind, in which case it is moved up to the oldest frame still in the ring.
size_t StreamFeedReadable(uint64_t writeFrames, uint64_t readFrames,
                          uint32_t capacityFrames, uint64_t* startFrame);

// The writer side. Opened once; Write is called from the audio thread and
// takes no locks and allocates nothing.
class StreamFeed {
public:
    ~StreamFeed();

    // Creates (or re-creates) the mapping. `capacityFrames` is rounded up to
    // the next power of two so the ring index is a mask rather than a modulo
    // on the audio thread.
    bool Open(uint32_t sampleRate, uint32_t channels, uint32_t capacityFrames,
              std::wstring* err);
    void Close();
    bool IsOpen() const { return m_header.load(std::memory_order_acquire) != nullptr; }

    // AUDIO THREAD. Interleaved float frames, `channels` wide as declared to
    // Open. Scaled by the current gain before being published.
    void Write(const float* interleaved, size_t frames);

    // What the reader should receive relative to the streaming mix, 0..2.
    // Set from the control thread, read on the audio thread, so it is a plain
    // atomic float rather than anything that could block.
    void SetGain(float gain);
    float Gain() const;

private:
    void*  m_mapping = nullptr;      // HANDLE
    void*  m_view = nullptr;
    // ATOMIC, and the gate that Write checks.
    //
    // mdxmixer#3 measured a writer still advancing writeFrames at 48 kHz
    // after magic had been zeroed -- i.e. after Close() had run. A plain
    // pointer is why that is possible: the audio thread tests it, and the
    // control thread nulls it and unmaps the view, with nothing ordering the
    // two. The test can pass and the view be gone before the copy lands,
    // which is an access violation on the audio thread rather than merely a
    // stale number.
    std::atomic<StreamFeedHeader*> m_header{ nullptr };
    float* m_samples = nullptr;
    // How many Writes are inside the view right now. Close publishes the
    // null first and then waits for this to fall to zero, so no copy can
    // still be in flight when the mapping goes.
    std::atomic<int> m_inWrite{ 0 };
    uint32_t m_capacity = 0;         // frames, a power of two
    uint32_t m_channels = 2;
};

} // namespace mdxm
