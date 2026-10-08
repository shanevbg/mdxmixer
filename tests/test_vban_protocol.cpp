// test_vban_protocol.cpp — the VBAN wire layer, which is pure and therefore
// testable without a socket, a device or a network (spec §2, §9).
//
// These are byte-level tests on purpose. Every value here was taken from the
// published specification rather than from our own implementation, so a change
// that makes mdxmixer self-consistent and wrong on the wire fails here instead
// of in a silence nobody can diagnose from the other end.
#include "test_framework.h"
#include "net/vban_protocol.h"
#include "net/vban_pacer.h"
#include "dsp/limiter.h"
#include <cstring>

using namespace mdxm::vban;

// The spec's numbers, asserted at COMPILE time: a wrong constant is a build
// break rather than a test failure, and a constant in a CHECK() is a constant
// conditional (C4127) in a W4 build. The runtime case below is the part that
// cannot be known statically.
static_assert(sizeof(Header) == 28, "VBAN header is 28 bytes on the wire");
static_assert(kHeaderSize == 28, "spec: 28-byte header");
static_assert(kMaxData == 1436, "spec: 1436 bytes of data per packet");
static_assert(kMaxPacket == 1464, "spec: 1464 bytes total per packet");
static_assert(kDefaultPort == 6980, "spec: default UDP port 6980");

// WHERE each field lands, which sizeof cannot tell you: a reordered or padded
// struct of the right total size puts every field after the change at the wrong
// wire offset, and the far end reads that as corrupt audio rather than as a
// layout fault. Checked through a real buffer, so this is the wire and not the
// compiler's opinion of it.
MDXM_TEST_CASE(Vban_HeaderFieldsLandAtTheirWireOffsets) {
    Header h = {};
    h.vban = kMagic;
    h.format_SR = 0x11;
    h.format_nbs = 0x22;
    h.format_nbc = 0x33;
    h.format_bit = 0x44;
    FillStreamName(h.streamname, "ABCDEFGHIJKLMNOP");   // all 16 bytes used
    h.nuFrame = 0x08070605;

    uint8_t w[kHeaderSize] = {};
    memcpy(w, &h, sizeof h);
    CHECK(w[0] == 'V' && w[1] == 'B' && w[2] == 'A' && w[3] == 'N');   // magic, in order
    CHECK(w[4] == 0x11);          // format_SR at offset 4
    CHECK(w[5] == 0x22);          // format_nbs at 5
    CHECK(w[6] == 0x33);          // format_nbc at 6
    CHECK(w[7] == 0x44);          // format_bit at 7
    CHECK(w[8] == 'A' && w[23] == 'P');                 // streamname spans 8..23
    CHECK(w[24] == 0x05 && w[27] == 0x08);              // nuFrame little-endian at 24
}

MDXM_TEST_CASE(Vban_SampleRateTable) {
    // Three geometric families, NOT ascending (spec): 48000 is index 3.
    CHECK(SampleRateIndex(48000) == 3);
    CHECK(SampleRateIndex(44100) == 16);
    CHECK(SampleRateIndex(96000) == 4);
    CHECK(SampleRateIndex(192000) == 5);
    CHECK(SampleRateIndex(12345) == -1);
    CHECK(SampleRateFromIndex(3) == 48000);
    CHECK(SampleRateFromIndex(16) == 44100);
    CHECK(SampleRateFromIndex(21) == 0);   // 21..31 undefined
    CHECK(SampleRateFromIndex(-1) == 0);
    // Round-trip every defined index.
    for (int i = 0; i < 21; ++i) CHECK(SampleRateIndex(SampleRateFromIndex(i)) == i);
}

MDXM_TEST_CASE(Vban_ParseRoundTrip) {
    uint8_t buf[64] = {};
    Header h = {};
    h.vban = kMagic;
    h.format_SR = (uint8_t)(kProtoAudio | SampleRateIndex(48000));
    h.format_nbs = 255;             // 256 samples, stored minus one
    h.format_nbc = 1;               // stereo
    h.format_bit = 0x01;            // int16, PCM
    FillStreamName(h.streamname, "mdxmixer");
    h.nuFrame = 42;
    memcpy(buf, &h, sizeof h);
    buf[sizeof h] = 0xAB;           // 1 data byte
    Parsed p = ParsePacket(buf, sizeof h + 1);
    CHECK(p.valid);
    CHECK(p.proto == kProtoAudio);
    CHECK(p.hdr.nuFrame == 42);
    CHECK(p.dataLen == 1);
    CHECK(p.data[0] == 0xAB);
    CHECK(StreamNameIs(p.hdr, h.streamname));
}

MDXM_TEST_CASE(Vban_ParseRejectsGarbage) {
    // Review Focus #1: the socket is the first thing in this program that
    // accepts unauthenticated input from the network. Everything malformed has
    // to be inert, and nothing may read past `len`.
    uint8_t buf[2000] = {};
    CHECK(!ParsePacket(buf, 0).valid);
    CHECK(!ParsePacket(buf, 27).valid);          // one byte short of a header
    CHECK(!ParsePacket(buf, 28).valid);          // zero magic
    Header h = {}; h.vban = 0x12345678;          // wrong magic
    memcpy(buf, &h, sizeof h);
    CHECK(!ParsePacket(buf, 100).valid);
    h.vban = kMagic; memcpy(buf, &h, sizeof h);
    CHECK(ParsePacket(buf, 28).valid);           // header-only is a legal packet
    CHECK(ParsePacket(buf, 28).dataLen == 0);
    CHECK(!ParsePacket(buf, 2000).valid);        // over kMaxPacket: refuse, don't trust
    CHECK(!ParsePacket(nullptr, 100).valid);
}

// The identification payload's own layout. Same reasoning as the header's: a
// field at the wrong offset means a standard VBAN tool shows garbage where the
// device name should be, and the fault is invisible from this end.
static_assert(sizeof(Ping0) == 676, "spec's identification payload is 676 bytes");

MDXM_TEST_CASE(Vban_Ping0FieldsLandAtTheirWireOffsets) {
    Ping0 id = {};
    id.bitType = 0x04030201;
    id.bitFeature = 0x08070605;
    id.preferredRate = 48000;
    memcpy(id.deviceName, "DEV", 3);
    memcpy(id.applicationName, "APP", 3);
    memcpy(id.userComment, "C", 1);

    uint8_t w[sizeof(Ping0)] = {};
    memcpy(w, &id, sizeof id);
    CHECK(w[0] == 0x01 && w[3] == 0x04);     // bitType at 0, little-endian
    CHECK(w[4] == 0x05 && w[7] == 0x08);     // bitFeature at 4
    uint32_t rate = 0; memcpy(&rate, w + 12, 4);
    CHECK(rate == 48000);                    // preferredRate at 12
    CHECK(memcmp(w + 164, "DEV", 3) == 0);   // deviceName at 164
    CHECK(memcmp(w + 292, "APP", 3) == 0);   // applicationName at 292
    CHECK(w[548] == 'C');                    // userComment at 548, last field
}

MDXM_TEST_CASE(Vban_Ping0ReplyEchoesAndFlags) {
    Header req = {};
    req.vban = kMagic;
    req.format_SR = kProtoService;
    req.format_nbs = kServiceFnPing0;
    req.format_nbc = kServiceIdentification;
    FillStreamName(req.streamname, "VBAN Service");
    req.nuFrame = 0xDEADBEEF;
    uint8_t rbuf[64] = {};
    memcpy(rbuf, &req, sizeof req);
    Parsed preq = ParsePacket(rbuf, sizeof req);
    CHECK(IsPing0Request(preq));

    Ping0 id = {};
    id.bitType = kDeviceTransmitter | kDeviceVirtualMixer;
    id.bitFeature = kFeatureAudio | kFeatureTxt | kFeatureFrame;
    id.preferredRate = 48000;
    uint8_t out[kHeaderSize + sizeof(Ping0)];
    size_t n = BuildPing0Reply(out, req, id);
    CHECK(n == kHeaderSize + 676);
    Parsed p = ParsePacket(out, n);
    CHECK(p.valid && p.proto == kProtoService);
    CHECK(p.hdr.format_SR == kProtoService);          // low 5 bits MUST be 0 (spec p.27)
    CHECK(p.hdr.format_nbs == (kServiceFnPing0 | kServiceReplyBit));
    CHECK(p.hdr.format_nbc == kServiceIdentification);
    CHECK(p.hdr.nuFrame == 0xDEADBEEF);               // transaction id echoed
    CHECK(StreamNameIs(p.hdr, req.streamname));       // name echoed (spec §2.3)
    Ping0 got; memcpy(&got, p.data, sizeof got);
    CHECK(got.bitFeature == id.bitFeature);
}

MDXM_TEST_CASE(Vban_Ping0RequestDetection) {
    Header h = {}; h.vban = kMagic;
    h.format_SR = kProtoService; h.format_nbs = kServiceFnPing0;
    h.format_nbc = kServiceIdentification;
    uint8_t b[28]; memcpy(b, &h, 28);
    CHECK(IsPing0Request(ParsePacket(b, 28)));
    h.format_nbs = kServiceFnPing0 | kServiceReplyBit;      // a REPLY is not a request
    memcpy(b, &h, 28);
    CHECK(!IsPing0Request(ParsePacket(b, 28)));
    h.format_nbs = kServiceFnPing0; h.format_nbc = 32;      // RT-register: not ours
    memcpy(b, &h, 28);
    CHECK(!IsPing0Request(ParsePacket(b, 28)));
}

MDXM_TEST_CASE(Vban_AudioPacketInt16) {
    float frames[256 * 2];
    for (int i = 0; i < 256; ++i) { frames[i * 2] = 0.5f; frames[i * 2 + 1] = -0.5f; }
    uint8_t out[kMaxPacket];
    size_t n = BuildAudioPacket(out, "mdxmixer\0\0\0\0\0\0\0\0", 7,
                                SampleRateIndex(48000), kBitInt16, frames, 256);
    CHECK(n == kHeaderSize + 256 * 2 * 2);       // 1024 B payload, 5.33 ms of audio
    Parsed p = ParsePacket(out, n);
    CHECK(p.valid && p.proto == kProtoAudio);
    CHECK((p.hdr.format_SR & 0x1F) == 3);        // 48 kHz is index 3, not a computed value
    CHECK(p.hdr.format_nbs == 255);              // 256 samples, stored minus one
    CHECK(p.hdr.format_nbc == 1);                // stereo, stored minus one
    CHECK(p.hdr.format_bit == kBitInt16);        // PCM codec = high nibble 0
    CHECK(p.hdr.nuFrame == 7);
    int16_t s0; memcpy(&s0, p.data, 2);
    CHECK(s0 == 16383 || s0 == 16384);           // 0.5f scaled
}

MDXM_TEST_CASE(Vban_AudioPacketFloat32CapsAt128) {
    // Sized for the largest count this case passes, so the refusal below is a
    // refusal and not an out-of-bounds read if the guards are ever reordered.
    float frames[180 * 2] = {};
    uint8_t out[kMaxPacket];
    size_t n = BuildAudioPacket(out, "mdxmixer\0\0\0\0\0\0\0\0", 0,
                                3, kBitFloat32, frames, 128);
    CHECK(n == kHeaderSize + 128 * 2 * 4);       // 1024 B: fits; 179 is the hard max
    CHECK(AudioFramesPerPacket(kBitFloat32) == 128);
    CHECK(AudioFramesPerPacket(kBitInt16) == 256);
    // A frame count whose payload would exceed the spec's 1436 bytes is refused
    // rather than truncated: a short packet is a corrupt packet at the far end.
    CHECK(BuildAudioPacket(out, "x\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 0, 3,
                           kBitFloat32, frames, 180) == 0);
}

MDXM_TEST_CASE(Vban_AudioPacketGainExtremes) {
    // Review Focus #2: the PC-side gain goes to 6400%, and the personal mix it
    // multiplies is already a real signal. Full scale times 64 must arrive as
    // loud, never as wrapped -- a wrap is a full-amplitude sign flip, which is
    // the worst sound a pair of headphones can be asked to make.
    float frames[4 * 2];
    for (int i = 0; i < 8; ++i) frames[i] = 1.0f;         // already at full scale
    for (int i = 0; i < 8; ++i) frames[i] *= 64.0f;       // gain 6400%
    mdxm::SoftLimiter lim; lim.Process(frames, 4);        // what the sender runs
    uint8_t out[kMaxPacket];
    size_t n = BuildAudioPacket(out, "g\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 0, 3,
                                kBitInt16, frames, 4);
    Parsed p = ParsePacket(out, n);
    int16_t s; memcpy(&s, p.data, 2);
    CHECK(s > 32000);                            // still loud, still positive
    // And a 64x overload with NO limiter in front still clamps: the limiter is
    // sender policy, the conversion's clamp is the wire's own guarantee.
    // Symmetric scaling (x32767), so full-scale negative is -32767: an
    // asymmetric clamp would add even-order distortion to a symmetric overload.
    float hot[2] = { 64.0f, -64.0f };
    n = BuildAudioPacket(out, "g\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 0, 3, kBitInt16, hot, 1);
    p = ParsePacket(out, n);
    int16_t l, r; memcpy(&l, p.data, 2); memcpy(&r, p.data + 2, 2);
    CHECK(l == 32767 && r == -32767);
}

MDXM_TEST_CASE(Vban_PacerPacesAndCatchesUp) {
    Pacer p;
    p.Configure(48000, 256, 4);                  // 256 frames = 5333 us/packet
    CHECK(p.Due(1000000) == 1);                  // first call anchors + grants one
    CHECK(p.Due(1000000) == 0);                  // nothing more due yet
    CHECK(p.Due(1005333) == 1);                  // one packet-time later
    CHECK(p.Due(1026665) == 4);                  // behind by four: the full burst
    CHECK(p.Due(1026665) == 0);                  // caught up
    // A huge stall (a wedged sendto, a lid closed): re-anchor rather than
    // granting hundreds of packets into a receiver that cannot hold them.
    unsigned g = p.Due(3000000);                 // ~2 s behind
    CHECK(g <= 4);
    CHECK(p.Reanchors() == 1);
    CHECK(p.BehindUs(3000000) == 0);             // schedule reset to now
}

MDXM_TEST_CASE(VbanTxt_Utf8RoundTrip) {
    const std::wstring w = L"MDXM_VBAN|gain=250|name=caf\x00E9 \x2713";
    bool ok = false;
    CHECK(Utf8ToWide(WideToUtf8(w), &ok) == w);
    CHECK(ok);
    // An astral-plane character, which on Windows is a SURROGATE PAIR in
    // wchar_t and four bytes in UTF-8: a codec that treats a code unit as a
    // code point mangles exactly this, and device names come from a phone.
    const std::wstring emoji = L"Pixel \xD83D\xDCF1";
    const std::string bytes = WideToUtf8(emoji);
    CHECK(bytes.size() == 10);                    // "Pixel " + 4
    CHECK(Utf8ToWide(bytes, &ok) == emoji);
    CHECK(ok);
    // Plain ASCII must be byte-identical, since that is every record we send.
    CHECK(WideToUtf8(L"MDXM_PING") == "MDXM_PING");
}

MDXM_TEST_CASE(VbanTxt_RejectsOversizeAndBadUtf8) {
    // Review Focus #5: inbound TXT is unauthenticated network input that becomes
    // a COMMAND. Oversized or malformed must be refused outright -- never
    // partially decoded into something that happens to parse as a verb.
    uint8_t out[kMaxPacket];
    std::string big(kMaxData + 1, 'x');
    CHECK(BuildTxtPacket(out, "mdxmixer\0\0\0\0\0\0\0\0", 0, big) == 0);

    bool ok = true;
    Utf8ToWide(std::string("\xFF\xFE\x80 garbage"), &ok);
    CHECK(!ok);                                   // 0xFF is no lead byte; 0x80 no lead
    ok = true;
    Utf8ToWide(std::string("abc\xE2\x82"), &ok);  // truncated 3-byte sequence
    CHECK(!ok);
    ok = true;
    Utf8ToWide(std::string("\xC0\xAF"), &ok);     // overlong encoding of '/'
    CHECK(!ok);
    ok = true;
    Utf8ToWide(std::string("\xED\xA0\x80"), &ok); // a surrogate encoded in UTF-8
    CHECK(!ok);
    ok = false;
    CHECK(Utf8ToWide(std::string(), &ok).empty() && ok);   // empty is valid

    std::string okStr = "MDXM_PING";
    size_t n = BuildTxtPacket(out, "mdxmixer\0\0\0\0\0\0\0\0", 9, okStr);
    Parsed p = ParsePacket(out, n);
    CHECK(p.valid && p.proto == kProtoTxt);
    CHECK((p.hdr.format_bit & 0xF0) == kTxtUtf8);
    CHECK(p.hdr.nuFrame == 9);
    std::string back;
    CHECK(ParseTxt(p, &back) && back == okStr);
    // An AUDIO packet is not text, however much it looks like bytes.
    uint8_t audio[kMaxPacket];
    float silence[2] = {};
    const size_t an = BuildAudioPacket(audio, "mdxmixer\0\0\0\0\0\0\0\0", 0, 3,
                                       kBitInt16, silence, 1);
    CHECK(!ParseTxt(ParsePacket(audio, an), &back));
}

MDXM_TEST_CASE(VbanTxt_ChunkRepliesAtRecordBoundaries) {
    // A multi-record reply is longer than one packet, and the rule is that a
    // single RECORD never spans two: the far end parses whole records out of
    // whatever arrives, so a split record would be two unparseable halves rather
    // than one slow answer.
    std::vector<std::wstring> recs{ L"MDXM_BEGIN" };
    for (int i = 0; i < 100; ++i)
        recs.push_back(L"MDXM_CHAN|id=ch" + std::to_wstring(i) +
                       L"|name=" + std::wstring(40, L'x'));
    recs.push_back(L"MDXM_END");
    auto chunks = ChunkReplies(recs);
    CHECK(chunks.size() > 1);                         // it does span packets
    for (const auto& c : chunks)
        CHECK(WideToUtf8(c).size() <= kMaxData);      // and every chunk fits one
    // Every record survives exactly once, in order: newline-joined within a
    // chunk, and the chunk boundaries fall between records.
    std::wstring all;
    for (const auto& c : chunks) { if (!all.empty()) all += L"\n"; all += c; }
    size_t count = 1;
    for (wchar_t ch : all) if (ch == L'\n') ++count;
    CHECK(count == recs.size());
    CHECK(all.rfind(L"MDXM_BEGIN", 0) == 0);          // BEGIN still first
    CHECK(all.size() >= 8 && all.compare(all.size() - 8, 8, L"MDXM_END") == 0);

    // A single record too big for any packet is REPLACED, not split: the caller
    // gets an error it can act on rather than a fragment it cannot parse.
    auto bad = ChunkReplies({ std::wstring(kMaxData + 10, L'z') });
    CHECK(bad.size() == 1 && bad[0] == L"MDXM_ERR|msg=toolong");
    // Nothing in, nothing out.
    CHECK(ChunkReplies({}).empty());
    // One short record is one chunk, unchanged.
    auto one = ChunkReplies({ L"MDXM_OK" });
    CHECK(one.size() == 1 && one[0] == L"MDXM_OK");
}

MDXM_TEST_CASE(VbanFrame_ChunksAndReassembles) {
    // One JPEG becomes several packets and has to come back byte-identical: a
    // frame that reassembles one byte wrong is a frame the far end cannot decode,
    // and from this end it looks like it was sent perfectly.
    std::vector<uint8_t> image(8000);
    for (size_t i = 0; i < image.size(); ++i) image[i] = (uint8_t)(i * 7 + 3);

    std::vector<std::vector<uint8_t>> packets;
    char name[kStreamNameSize];
    FillStreamName(name, "VIDEO1");
    ChunkFrame(packets, name, 42, image.data(), image.size());
    CHECK(packets.size() == 6);                   // 8000 / 1436 = 5.57
    if (packets.size() < 6) return;

    std::vector<uint8_t> rebuilt;
    for (size_t i = 0; i < packets.size(); ++i) {
        const Parsed p = ParsePacket(packets[i].data(), packets[i].size());
        CHECK(p.valid && p.proto == kProtoFrame);
        CHECK(p.hdr.nuFrame == 42);               // one frame, one number
        // The 16-bit packet index, split across two bytes.
        const uint16_t index = (uint16_t)(p.hdr.format_nbs | (p.hdr.format_nbc << 8));
        CHECK(index == (uint16_t)i);
        const uint8_t type = (uint8_t)(p.hdr.format_bit & 0x07);
        if (i == 0) CHECK(type == kFrameStart);
        else if (i + 1 == packets.size()) CHECK(type == kFrameEnd);
        else CHECK(type == kFrameContinue);
        CHECK((p.hdr.format_bit & 0xF0) == 0);    // frame stream type 0 (spec p.24)
        CHECK(p.dataLen <= kMaxData);
        rebuilt.insert(rebuilt.end(), p.data, p.data + p.dataLen);
    }
    CHECK(rebuilt.size() == image.size());
    CHECK(rebuilt == image);
}

MDXM_TEST_CASE(VbanFrame_OnePacketFrameIsStartAndEnd) {
    // The degenerate case a near-black screen produces, and the one the spec does
    // not define: it lists first/next/last and says nothing about a frame that is
    // all three. Treating the field as the bit flags it is laid out as gives
    // start|end, and that inference is pinned here and in the interop check.
    std::vector<uint8_t> tiny(100, 0xAB);
    std::vector<std::vector<uint8_t>> packets;
    char name[kStreamNameSize];
    FillStreamName(name, "VIDEO1");
    ChunkFrame(packets, name, 7, tiny.data(), tiny.size());
    CHECK(packets.size() == 1);
    if (packets.empty()) return;
    const Parsed p = ParsePacket(packets[0].data(), packets[0].size());
    CHECK(p.valid && p.proto == kProtoFrame);
    CHECK((p.hdr.format_bit & 0x07) == (kFrameStart | kFrameEnd));
    CHECK((p.hdr.format_bit & 0x07) == 0x05);
    CHECK(p.dataLen == 100);
    CHECK(p.hdr.format_nbs == 0 && p.hdr.format_nbc == 0);   // index 0
}

MDXM_TEST_CASE(VbanFrame_IndexWrapsAtTheSpecsOwnArithmetic) {
    // The spec's prose says packets are numbered 0..64535, but its own LSB/MSB
    // split code wraps at 65536 -- a digit transposition in the document. The code
    // is the authority, because that is what the other implementations compile.
    CHECK(FramePacketIndex(0) == 0);
    CHECK(FramePacketIndex(65535) == 65535);
    CHECK(FramePacketIndex(65536) == 0);
    CHECK(FramePacketIndex(65537) == 1);
}

MDXM_TEST_CASE(VbanFrame_RefusesTheAbsurdAndTheEmpty) {
    std::vector<std::vector<uint8_t>> packets;
    char name[kStreamNameSize];
    FillStreamName(name, "VIDEO1");
    // Nothing in, nothing out -- not one empty packet.
    ChunkFrame(packets, name, 0, nullptr, 0);
    CHECK(packets.empty());
    uint8_t one = 1;
    ChunkFrame(packets, name, 0, &one, 0);
    CHECK(packets.empty());
    // A frame too large for the index space is a bug upstream, not something to
    // send 65540 packets about: refused whole.
    ChunkFrame(packets, name, 0, &one, kMaxData * 65540);
    CHECK(packets.empty());
}

MDXM_TEST_CASE(Vban_StreamNameFillAndCompare) {
    char a[16], b[16];
    FillStreamName(a, "mdxmixer");
    FillStreamName(b, "mdxmixer");
    CHECK(memcmp(a, b, 16) == 0);
    CHECK(a[8] == 0 && a[15] == 0);              // NUL-padded to 16
    FillStreamName(b, "a-very-long-name-that-overflows");
    CHECK(b[15] != 0 || b[0] == 'a');            // truncated at 16, never past
    Header h = {}; memcpy(h.streamname, a, 16);
    CHECK(StreamNameIs(h, a));
    CHECK(!StreamNameIs(h, b));
}
