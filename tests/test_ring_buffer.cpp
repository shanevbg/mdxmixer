#include "test_framework.h"
#include "dsp/ring_buffer.h"
#include <vector>

using mdxm::RingBuffer;

MDXM_TEST_CASE(Ring_WriteReadRoundTrip) {
    RingBuffer rb(16);
    float in[8] = {1,2,3,4,5,6,7,8}; // 4 frames
    rb.Write(in, 4);
    CHECK(rb.Depth() == 4);
    float out[8] = {};
    CHECK(rb.Read(out, 4) == 4);
    for (int i = 0; i < 8; ++i) CHECK(out[i] == in[i]);
    CHECK(rb.Depth() == 0);
}

MDXM_TEST_CASE(Ring_UnderflowZeroFillsAndCounts) {
    RingBuffer rb(16);
    float in[4] = {1,2,3,4};
    rb.Write(in, 2);
    float out[12] = {9,9,9,9,9,9,9,9,9,9,9,9};
    CHECK(rb.Read(out, 6) == 2);           // only 2 real frames
    CHECK(out[0] == 1 && out[3] == 4);
    for (int i = 4; i < 12; ++i) CHECK(out[i] == 0.0f); // zero-filled tail
    CHECK(rb.Underruns() == 1);
}

MDXM_TEST_CASE(Ring_OverflowDropsOldestAndCounts) {
    RingBuffer rb(4); // capacity 4 frames
    float a[8] = {1,1,2,2,3,3,4,4};
    rb.Write(a, 4);
    float b[4] = {5,5,6,6};
    rb.Write(b, 2);                        // must drop frames 1 and 2
    CHECK(rb.Drops() >= 2);
    CHECK(rb.Depth() == 4);
    float out[8] = {};
    rb.Read(out, 4);
    CHECK(out[0] == 3.0f);                 // oldest surviving frame is 3
    CHECK(out[6] == 6.0f);
}

MDXM_TEST_CASE(Ring_WriteLargerThanCapacityKeepsNewest) {
    RingBuffer rb(4);
    std::vector<float> big(20); // 10 frames: values 0..19
    for (int i = 0; i < 20; ++i) big[(size_t)i] = (float)i;
    rb.Write(big.data(), 10);
    CHECK(rb.Depth() == 4);
    float out[8] = {};
    rb.Read(out, 4);
    CHECK(out[0] == 12.0f);                // frames 6..9 survive
    CHECK(out[7] == 19.0f);
}

// Capacity is now derived from the mix rate rather than fixed, because it IS
// the worst-case latency a stall can leave behind (fj#13): the producer fills
// it while the consumer is away, and whatever fits is what the listener is
// left holding.
MDXM_TEST_CASE(Ring_ResizeSetsCapacityAndEmptiesIt) {
    mdxm::RingBuffer rb(4);
    float in[8] = { 1, 1, 2, 2, 3, 3, 4, 4 };
    rb.Write(in, 4);
    CHECK(rb.Depth() == 4);
    CHECK(rb.Capacity() == 4);

    rb.Resize(24000);                      // 500 ms at 48 kHz
    CHECK(rb.Capacity() == 24000);
    // Emptied: the audio in it belonged to a graph that is being rebuilt, and
    // keeping it would mean playing the old device's backlog on the new one.
    CHECK(rb.Depth() == 0);

    // And it works afterwards.
    rb.Write(in, 4);
    CHECK(rb.Depth() == 4);
    float out[8] = {};
    CHECK(rb.Read(out, 4) == 4);
    CHECK(out[0] == 1.0f);
    CHECK(out[6] == 4.0f);
}

MDXM_TEST_CASE(Ring_ResizeNeverLeavesAZeroCapacity) {
    mdxm::RingBuffer rb(16);
    rb.Resize(0);                          // a rate of 0 would compute this
    CHECK(rb.Capacity() >= 1);
    float in[2] = { 7, 7 };
    rb.Write(in, 1);                       // must not divide by zero or crash
    CHECK(rb.Depth() <= rb.Capacity());
}
