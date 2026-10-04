#include "test_framework.h"
#include <vector>
#include <atomic>
#include <thread>
#include "ipc/stream_feed.h"
#include <windows.h>

using namespace mdxm;

MDXM_TEST_CASE(StreamFeed_ReaderTakesOnlyWhatIsNew) {
    uint64_t start = 0;
    CHECK(StreamFeedReadable(1000, 1000, 4096, &start) == 0);   // caught up
    CHECK(start == 1000);
    CHECK(StreamFeedReadable(1480, 1000, 4096, &start) == 480);
    CHECK(start == 1000);
}

MDXM_TEST_CASE(StreamFeed_ReaderResyncsInsteadOfReplayingALap) {
    // A reader that stalls -- the visualiser hitching, a breakpoint -- comes
    // back to find the ring has gone round. Those frames are gone. Replaying
    // the lap-old buffer is what a naive reader does, and it sounds like a
    // stutter loop rather than a gap.
    uint64_t start = 0;
    size_t n = StreamFeedReadable(100000, 1000, 4096, &start);
    CHECK(n == 4096);
    CHECK(start == 100000 - 4096);   // the oldest frame still held
}

MDXM_TEST_CASE(StreamFeed_NothingToReadBeforeAnythingIsWritten) {
    uint64_t start = 123;
    CHECK(StreamFeedReadable(0, 0, 4096, &start) == 0);
    CHECK(start == 0);
    // A nonsense capacity must not divide by zero or hand out frames.
    CHECK(StreamFeedReadable(500, 0, 0, &start) == 0);
}

MDXM_TEST_CASE(StreamFeed_WritesAndIsReadableThroughTheSharedMapping) {
    StreamFeed feed;
    std::wstring err;
    CHECK(feed.Open(48000, 2, 4096, &err));
    CHECK(feed.IsOpen());
    feed.SetGain(1.0f);

    // A reader in another process would open the same name; opening it here
    // exercises the same path.
    HANDLE h = OpenFileMappingW(FILE_MAP_READ, FALSE, kStreamFeedName);
    CHECK(h != nullptr);
    void* view = MapViewOfFile(h, FILE_MAP_READ, 0, 0, 0);
    CHECK(view != nullptr);
    const StreamFeedHeader* hdr = (const StreamFeedHeader*)view;
    CHECK(hdr->magic == kStreamFeedMagic);
    CHECK(hdr->sampleRate == 48000);
    CHECK(hdr->channels == 2);
    CHECK(hdr->writeFrames == 0);

    float block[8] = { 0.5f, -0.5f, 0.25f, -0.25f, 1.0f, -1.0f, 0.0f, 0.0f };
    feed.Write(block, 4);
    CHECK(hdr->writeFrames == 4);
    const float* samples = (const float*)((const char*)view + sizeof(StreamFeedHeader));
    CHECK(samples[0] == 0.5f);
    CHECK(samples[5] == -1.0f);

    // Gain is applied by the writer, so the reader needs no knowledge of it.
    feed.SetGain(0.5f);
    feed.Write(block, 4);
    CHECK(hdr->writeFrames == 8);
    CHECK(samples[8] == 0.25f);

    UnmapViewOfFile(view);
    CloseHandle(h);
    feed.Close();
    CHECK(!feed.IsOpen());
}

// ── mdxmixer#3: a writer that kept writing after Close ───────────────────

MDXM_TEST_CASE(StreamFeed_ASecondWriterIsRefusedNotShared) {
    // CreateFileMappingW with a name that is already taken returns the
    // EXISTING section, at whatever size its creator chose. Sharing it means
    // writing one geometry into a header describing another, and -- the part
    // that was actually observed -- whichever writer closes first zeroes the
    // readiness flag while the other carries on writing.
    StreamFeed first, second;
    std::wstring err;
    CHECK(first.Open(48000, 2, 4096, &err));
    CHECK(first.IsOpen());

    err.clear();
    CHECK(!second.Open(48000, 2, 4096, &err));
    CHECK(!second.IsOpen());
    CHECK(!err.empty());               // and it says why

    // The first is untouched by the refusal.
    CHECK(first.IsOpen());
    first.Close();
}

MDXM_TEST_CASE(StreamFeed_WriteAfterCloseIsSilentlyDropped) {
    // The defect as a reader saw it: magic zeroed, writeFrames still
    // climbing. Write must do nothing at all once Close has run, and must
    // not touch the unmapped view to find that out.
    StreamFeed f;
    std::wstring err;
    CHECK(f.Open(48000, 2, 1024, &err));
    const std::vector<float> block(256 * 2, 0.25f);
    f.Write(block.data(), 256);
    f.Close();
    CHECK(!f.IsOpen());
    // Would have been an access violation through a dangling pointer before.
    f.Write(block.data(), 256);
    f.Write(block.data(), 256);
    CHECK(!f.IsOpen());
}

MDXM_TEST_CASE(StreamFeed_CloseWaitsForAWriteInFlight) {
    // Close shuts the gate, then waits for whoever is already through it.
    // Exercised with a real thread, because the ordering is the whole point
    // and a single-threaded call cannot show it.
    StreamFeed f;
    std::wstring err;
    CHECK(f.Open(48000, 2, 4096, &err));

    std::atomic<bool> stop{ false };
    std::atomic<long long> writes{ 0 };
    std::thread writer([&] {
        const std::vector<float> block(128 * 2, 0.1f);
        while (!stop.load(std::memory_order_relaxed)) {
            f.Write(block.data(), 128);
            writes.fetch_add(1, std::memory_order_relaxed);
        }
    });
    while (writes.load() < 50) Sleep(1);     // it is genuinely running

    f.Close();                                // must not fault, must not hang
    CHECK(!f.IsOpen());

    // Keep hammering a closed feed for a moment: every call has to be a
    // no-op rather than a write through a freed mapping.
    const long long after = writes.load();
    while (writes.load() < after + 50) Sleep(1);
    stop.store(true);
    writer.join();
    CHECK(!f.IsOpen());
}
