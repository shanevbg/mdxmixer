#include "net/vban_protocol.h"
#include <cstring>

namespace mdxm { namespace vban {

namespace {
// The spec's table, in the spec's order: index IS the position. Three
// geometric families rather than one ascending list -- see the header.
constexpr uint32_t kRates[] = {
    6000, 12000, 24000, 48000, 96000, 192000, 384000,
    8000, 16000, 32000, 64000, 128000, 256000, 512000,
    11025, 22050, 44100, 88200, 176400, 352800, 705600 };
constexpr int kRateCount = (int)(sizeof(kRates) / sizeof(kRates[0]));
static_assert(kRateCount == 21, "the spec defines 21 rates; 21..31 are undefined");
} // namespace

int SampleRateIndex(uint32_t rate) {
    for (int i = 0; i < kRateCount; ++i)
        if (kRates[i] == rate) return i;
    return -1;
}

uint32_t SampleRateFromIndex(int index) {
    if (index < 0 || index >= kRateCount) return 0;
    return kRates[index];
}

Parsed ParsePacket(const uint8_t* buf, size_t len) {
    Parsed p;
    if (!buf || len < kHeaderSize || len > kMaxPacket) return p;
    std::memcpy(&p.hdr, buf, kHeaderSize);
    if (p.hdr.vban != kMagic) return p;
    p.proto = (uint8_t)(p.hdr.format_SR & kProtoMask);
    p.data = buf + kHeaderSize;
    p.dataLen = len - kHeaderSize;
    p.valid = true;
    return p;
}

void FillStreamName(char out[kStreamNameSize], const std::string& name) {
    // Zero first, then copy at most the field width: the field is compared
    // whole (StreamNameIs memcmps all 16 bytes), so the padding has to be
    // deterministic or two spellings of one name would not match.
    std::memset(out, 0, kStreamNameSize);
    std::memcpy(out, name.data(),
                name.size() < kStreamNameSize ? name.size() : kStreamNameSize);
}

bool StreamNameIs(const Header& h, const char name[kStreamNameSize]) {
    return std::memcmp(h.streamname, name, kStreamNameSize) == 0;
}

bool IsPing0Request(const Parsed& p) {
    // The reply bit being clear is part of what makes it a request: our own
    // replies go out on the same socket, and a receiver that answered them
    // would ping-pong with itself.
    return p.valid && p.proto == kProtoService &&
           p.hdr.format_nbc == kServiceIdentification &&
           p.hdr.format_nbs == kServiceFnPing0;
}

size_t BuildPing0Reply(uint8_t* out, const Header& request, const Ping0& id) {
    Header h = {};
    h.vban = kMagic;
    h.format_SR  = kProtoService;   // low five bits MUST be zero (spec p.27)
    h.format_nbs = (uint8_t)(kServiceFnPing0 | kServiceReplyBit);
    h.format_nbc = kServiceIdentification;
    h.format_bit = 0;
    std::memcpy(h.streamname, request.streamname, kStreamNameSize);
    h.nuFrame = request.nuFrame;    // the client's transaction id, echoed
    std::memcpy(out, &h, kHeaderSize);
    std::memcpy(out + kHeaderSize, &id, sizeof(Ping0));
    return kHeaderSize + sizeof(Ping0);
}

std::string WideToUtf8(const std::wstring& w) {
    std::string out;
    out.reserve(w.size() + w.size() / 4);
    for (size_t i = 0; i < w.size(); ++i) {
        uint32_t cp = (uint32_t)(uint16_t)w[i];
        // A surrogate PAIR is one code point in four bytes. Treating each code
        // unit as a code point is the classic bug here, and the input includes
        // device names typed on a phone.
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < w.size()) {
            const uint32_t lo = (uint32_t)(uint16_t)w[i + 1];
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                ++i;
            }
        }
        // An unpaired surrogate cannot be encoded; U+FFFD keeps the output
        // valid UTF-8 rather than emitting something a strict reader rejects.
        if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;
        if (cp < 0x80) {
            out.push_back((char)cp);
        } else if (cp < 0x800) {
            out.push_back((char)(0xC0 | (cp >> 6)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back((char)(0xE0 | (cp >> 12)));
            out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        } else {
            out.push_back((char)(0xF0 | (cp >> 18)));
            out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

std::wstring Utf8ToWide(const std::string& utf8, bool* ok) {
    if (ok) *ok = true;
    std::wstring out;
    out.reserve(utf8.size());
    const auto bad = [&](size_t advance) {
        if (ok) *ok = false;
        out.push_back(L'\xFFFD');
        return advance;
    };
    size_t i = 0;
    while (i < utf8.size()) {
        const uint8_t b0 = (uint8_t)utf8[i];
        uint32_t cp = 0;
        size_t len = 0;
        if (b0 < 0x80) { cp = b0; len = 1; }
        else if ((b0 & 0xE0) == 0xC0) { cp = b0 & 0x1Fu; len = 2; }
        else if ((b0 & 0xF0) == 0xE0) { cp = b0 & 0x0Fu; len = 3; }
        else if ((b0 & 0xF8) == 0xF0) { cp = b0 & 0x07u; len = 4; }
        else { i += bad(1); continue; }            // 0x80-0xBF or 0xF8-0xFF: no lead byte
        if (i + len > utf8.size()) { i += bad(utf8.size() - i); break; }   // truncated
        bool malformed = false;
        for (size_t k = 1; k < len; ++k) {
            const uint8_t bk = (uint8_t)utf8[i + k];
            if ((bk & 0xC0) != 0x80) { malformed = true; break; }   // not a continuation
            cp = (cp << 6) | (bk & 0x3Fu);
        }
        if (malformed) { i += bad(1); continue; }
        // OVERLONG encodings are rejected rather than accepted-and-normalised:
        // they are the classic way to smuggle a byte past a check that was
        // looking for its shortest form.
        if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) ||
            (len == 4 && cp < 0x10000)) { i += bad(len); continue; }
        // A surrogate is not a character, and a code point past U+10FFFF is not
        // one either.
        if ((cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) { i += bad(len); continue; }
        if (cp < 0x10000) {
            out.push_back((wchar_t)cp);
        } else {
            cp -= 0x10000;
            out.push_back((wchar_t)(0xD800 + (cp >> 10)));
            out.push_back((wchar_t)(0xDC00 + (cp & 0x3FF)));
        }
        i += len;
    }
    return out;
}

size_t BuildTxtPacket(uint8_t* out, const char name[kStreamNameSize],
                      uint32_t nuFrame, const std::string& utf8) {
    if (!out || utf8.size() > kMaxData) return 0;
    Header h = {};
    h.vban = kMagic;
    // The low five bits are an informational bitrate index; zero is honest for a
    // channel whose rate is "whenever somebody types something".
    h.format_SR = kProtoTxt;
    h.format_nbs = 0;
    h.format_nbc = 0;
    h.format_bit = kTxtUtf8;
    std::memcpy(h.streamname, name, kStreamNameSize);
    h.nuFrame = nuFrame;
    std::memcpy(out, &h, kHeaderSize);
    if (!utf8.empty()) std::memcpy(out + kHeaderSize, utf8.data(), utf8.size());
    return kHeaderSize + utf8.size();
}

bool ParseTxt(const Parsed& p, std::string* utf8) {
    if (!p.valid || p.proto != kProtoTxt || !utf8) return false;
    utf8->assign((const char*)p.data, p.dataLen);
    return true;
}

std::vector<std::wstring> ChunkReplies(const std::vector<std::wstring>& records) {
    std::vector<std::wstring> out;
    std::wstring current;
    size_t currentBytes = 0;
    for (const auto& rec : records) {
        const size_t recBytes = WideToUtf8(rec).size();
        if (recBytes > kMaxData) {
            // No packet can hold it. Flush what is pending, then say so: an
            // error the caller can act on beats a fragment it cannot parse.
            if (!current.empty()) { out.push_back(current); current.clear(); currentBytes = 0; }
            out.push_back(L"MDXM_ERR|msg=toolong");
            continue;
        }
        const size_t joined = current.empty() ? recBytes : currentBytes + 1 + recBytes;
        if (!current.empty() && joined > kMaxData) {
            out.push_back(current);
            current.clear();
            currentBytes = 0;
        }
        if (!current.empty()) { current += L'\n'; ++currentBytes; }
        current += rec;
        currentBytes += recBytes;
    }
    if (!current.empty()) out.push_back(current);
    return out;
}

void ChunkFrame(std::vector<std::vector<uint8_t>>& out, const char name[kStreamNameSize],
                uint32_t nuFrame, const uint8_t* bytes, size_t len) {
    out.clear();
    if (!bytes || len == 0) return;
    const size_t count = (len + kMaxData - 1) / kMaxData;
    // More packets than the index can name. Refused whole rather than sent with
    // a wrapped index the far end would reassemble in the wrong order.
    if (count > 65536) return;
    out.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        const size_t offset = i * kMaxData;
        const size_t chunk = (len - offset) < kMaxData ? (len - offset) : kMaxData;
        const uint16_t index = FramePacketIndex(i);
        uint8_t type = kFrameContinue;
        if (i == 0) type = (count == 1) ? (uint8_t)(kFrameStart | kFrameEnd) : kFrameStart;
        else if (i + 1 == count) type = kFrameEnd;

        Header h = {};
        h.vban = kMagic;
        // The low five bits are an informational bandwidth index; zero is honest
        // for a stream whose rate is "whenever the screen changes".
        h.format_SR = kProtoFrame;
        h.format_nbs = (uint8_t)(index & 0xFF);          // index LSB
        h.format_nbc = (uint8_t)((index >> 8) & 0xFF);   // index MSB
        h.format_bit = type;                             // frame stream type stays 0
        std::memcpy(h.streamname, name, kStreamNameSize);
        // The SAME number on every packet of one frame: it is what tells a
        // receiver these belong together, as against the index that orders them.
        h.nuFrame = nuFrame;

        std::vector<uint8_t> packet(kHeaderSize + chunk);
        std::memcpy(packet.data(), &h, kHeaderSize);
        std::memcpy(packet.data() + kHeaderSize, bytes + offset, chunk);
        out.push_back(std::move(packet));
    }
}

size_t AudioFramesPerPacket(uint8_t bitType) {
    return bitType == kBitFloat32 ? kFramesPerPacketF32 : kFramesPerPacketI16;
}

size_t BuildAudioPacket(uint8_t* out, const char name[kStreamNameSize],
                        uint32_t nuFrame, int srIndex, uint8_t bitType,
                        const float* interleavedStereo, size_t frames) {
    // Every refusal happens before anything is read or written, so a caller
    // that gets 0 has had nothing touched -- including an `out` it may be
    // about to reuse and an input array it may have sized to a smaller count.
    if (!out || !interleavedStereo || frames == 0 || frames > 256) return 0;
    if (srIndex < 0 || srIndex > 20) return 0;
    const size_t bytesPer = (bitType == kBitFloat32) ? 4u : 2u;
    const size_t payload = frames * 2 * bytesPer;
    if (payload > kMaxData) return 0;

    Header h = {};
    h.vban = kMagic;
    h.format_SR  = (uint8_t)(kProtoAudio | (uint8_t)srIndex);
    h.format_nbs = (uint8_t)(frames - 1);   // spec stores the count minus one
    h.format_nbc = 1;                       // stereo, likewise minus one
    h.format_bit = bitType;                 // PCM codec: high nibble stays 0
    std::memcpy(h.streamname, name, kStreamNameSize);
    h.nuFrame = nuFrame;
    std::memcpy(out, &h, kHeaderSize);

    uint8_t* d = out + kHeaderSize;
    if (bitType == kBitFloat32) {
        // Interleaved float32 is already the mix's own format, so the wire
        // form is a copy. No clamp: float32 carries anything the mix produced,
        // and a receiver asking for f32 asked for exactly that.
        std::memcpy(d, interleavedStereo, payload);
    } else {
        for (size_t i = 0; i < frames * 2; ++i) {
            float v = interleavedStereo[i];
            // CLAMPED HERE, not only in the sender's limiter. The limiter is
            // policy and can be changed or bypassed; this is the wire's own
            // guarantee, and what it prevents is a wrap -- which is not a
            // quiet artefact but a full-amplitude sign flip straight into
            // somebody's headphones.
            if (v > 1.0f) v = 1.0f;
            if (v < -1.0f) v = -1.0f;
            // Symmetric scaling: full scale is +/-32767, giving up the one
            // extra negative code so that a symmetric overload stays
            // symmetric. The alternative (x32768 with an asymmetric clamp)
            // adds even-order distortion to exactly the signal most likely to
            // reach it.
            const int16_t w = (int16_t)(v * 32767.0f);
            std::memcpy(d + i * 2, &w, 2);
        }
    }
    return kHeaderSize + payload;
}

}} // namespace mdxm::vban
