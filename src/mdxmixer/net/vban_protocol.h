#pragma once
// The VBAN wire protocol (rev 13, SEP 2025), clean-room from the published
// specification -- see docs/specs/2026-10-07-vban-stream-server-design.md §2,
// which records the layout this file implements and where each constant comes
// from. VB-Audio publish the protocol expressly so it can be implemented; the
// reference implementations are GPL and are deliberately NOT consulted here.
//
// PURE: no Windows headers, no sockets, no clock -- headless-testable like
// dsp/. net/vban_server.cpp is the only place these bytes meet a socket, which
// is what lets the byte-level decisions be tested without a network.
//
// Every value here is the SPEC's, not ours. Where the spec is silent or
// self-contradictory the comment says so at the point of the decision, because
// the failure mode for guessing is a receiver on another machine going quiet
// with nothing to read from this end.
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace mdxm { namespace vban {

constexpr uint16_t kDefaultPort = 6980;
constexpr size_t   kHeaderSize = 28;
// The spec's maxima: 1464 bytes on the wire, chosen as a 1500-byte MTU less 64
// so a VLAN tag cannot fragment a packet, leaving 1436 for data.
constexpr size_t   kMaxData = 1436;
constexpr size_t   kMaxPacket = kHeaderSize + kMaxData;
constexpr size_t   kStreamNameSize = 16;
// 'V','B','A','N' in wire order, which is this value when the four bytes are
// loaded as a little-endian uint32 -- the form the spec's own test uses.
constexpr uint32_t kMagic = 0x4E414256;

// Sub-protocol, bits 5-7 of format_SR.
constexpr uint8_t kProtoAudio = 0x00, kProtoSerial = 0x20, kProtoTxt = 0x40,
                  kProtoService = 0x60, kProtoFrame = 0x80, kProtoMask = 0xE0;

#pragma pack(push, 1)
struct Header {
    uint32_t vban;
    uint8_t  format_SR;    // bits 5-7 sub-protocol; bits 0-4: SR index (audio),
                           // informational bps/Mbps index (txt/frame), and for
                           // SERVICE they MUST be zero -- format_SR == 0x60
                           // exactly (spec p.27), which a strict receiver checks
    uint8_t  format_nbs;   // audio: samples-1; service: function (bit 7 = reply);
                           // frame: packet index LSB
    uint8_t  format_nbc;   // audio: channels-1; service: service type;
                           // frame: packet index MSB
    uint8_t  format_bit;   // audio: bits 0-2 data type, bits 4-7 codec (PCM = 0);
                           // txt: bits 4-7 charset; frame: bits 0-2 packet type,
                           // bits 4-7 frame stream type
    char     streamname[kStreamNameSize];   // ASCII, NUL-padded; a receiver
                                            // matches on name AND source IP
    uint32_t nuFrame;      // growing counter; per packet for audio/txt, per
                           // completed frame for FRAME, transaction id for SERVICE
};
#pragma pack(pop)
// Not a comment but a build break: the whole protocol is this struct's layout,
// and a padding byte inserted by a compiler setting would put every field after
// `vban` one byte late on the wire -- which reads at the far end as corrupt
// audio, not as a struct problem.
static_assert(sizeof(Header) == kHeaderSize, "VBAN header must pack to 28 bytes");

// The sample rate <-> index mapping. NOT ordered by rate: the spec's table is
// three geometric families (48k-based, 8k-based, 44.1k-based), so 48000 is
// index 3 and 44100 is index 16. Computing an index instead of looking it up is
// the obvious mistake and it would put the stream on the wrong rate.
int      SampleRateIndex(uint32_t rate);     // -1 when not in the table
uint32_t SampleRateFromIndex(int index);     // 0 when undefined/out of range

struct Parsed {
    bool valid = false;
    uint8_t proto = 0;
    Header hdr = {};
    const uint8_t* data = nullptr;   // aliases buf + kHeaderSize
    size_t dataLen = 0;
};

// Validate a received datagram. Length (28..kMaxPacket) and magic are checked
// before anything is believed, and the header is COPIED out so the caller may
// reuse its receive buffer while still reading the header; `data` still points
// into the caller's buffer and lives exactly as long as it does.
//
// This is the program's first unauthenticated input from the network: a refusal
// here is the cheapest possible handling of a malformed packet, and `dataLen`
// is derived from the real length so nothing downstream can read past it.
Parsed ParsePacket(const uint8_t* buf, size_t len);

void FillStreamName(char out[kStreamNameSize], const std::string& name);
bool StreamNameIs(const Header& h, const char name[kStreamNameSize]);

// ── SERVICE: discovery, and our subscription ─────────────────────────────
//
// PING0 is the spec's identification exchange, and it is also how a client
// subscribes here: a request creates or renews that address as a peer, and the
// stream stops when the last one stops asking (spec §2.3). That is our
// semantic layered on a compliant mechanism -- the packets themselves are what
// any VBAN tool already sends and understands.
constexpr uint8_t kServiceIdentification = 0;   // format_nbc: service type
constexpr uint8_t kServiceFnPing0 = 0;          // format_nbs: function
constexpr uint8_t kServiceReplyBit = 0x80;      // format_nbs bit 7

// What we tell the network we are. TRANSMITTER because we send audio;
// VIRTUALMIXER because that is what mdxmixer is, and a receiver's UI groups by
// it. The feature bits are what a client reads INSTEAD of asking us our
// version, which is why MDR_Android needs no capability negotiation of its own.
constexpr uint32_t kDeviceTransmitter = 0x2, kDeviceVirtualMixer = 0x20;
constexpr uint32_t kFeatureAudio = 0x1, kFeatureFrame = 0x1000, kFeatureTxt = 0x10000;

#pragma pack(push, 1)
// The spec's identification payload (rev 13, SERVICE chapter): 676 bytes, every
// string ASCII and NUL-padded, every reserved field sent as zero.
struct Ping0 {
    uint32_t bitType, bitFeature, bitFeatureEx;
    uint32_t preferredRate, minRate, maxRate;
    uint32_t colorRgb;
    uint8_t  nVersion[4];
    char     gpsPosition[8], userPosition[8], langCode[8], reservedAscii[8];
    char     reservedEx[64];
    char     distantIp[32];
    uint16_t distantPort, distantReserved;
    char     deviceName[64], manufacturerName[64], applicationName[64], hostName[64];
    char     userName[128], userComment[128];
};
#pragma pack(pop)
static_assert(sizeof(Ping0) == 676, "spec's identification payload is 676 bytes");

// Is this an identification REQUEST (as against a reply, or one of the other
// service types -- 32/33 are the RT-packet pair, which we do not implement)?
bool IsPing0Request(const Parsed& p);

// Build the reply. `out` must hold kHeaderSize + sizeof(Ping0).
//
// The transaction id and the stream name are ECHOED from the request: the id is
// how a client matches a reply to its own outstanding ping, and echoing the
// name is what lets a client that pinged under its own stream name recognise
// the answer (spec §2.3). format_SR is kProtoService exactly -- the low five
// bits must be zero for SERVICE, and a strict receiver checks that.
size_t BuildPing0Reply(uint8_t* out, const Header& request, const Ping0& id);

// ── TXT: the MDXM control records ────────────────────────────────────────
//
// VBAN-TXT is what Voicemeeter itself uses to carry remote command strings, so
// putting MDXM records on it is the protocol's own idea of what TXT is for
// rather than a borrowed channel. `format_bit` high nibble is the charset.
constexpr uint8_t kTxtUtf8 = 0x10;

// UTF-16 (what wchar_t is here) <-> UTF-8 (what the wire carries), hand-rolled
// so this file stays Windows-free and the error cases are testable.
//
// `*ok` is false for ANY malformed input -- a truncated sequence, an overlong
// encoding, a lead byte that is not one, a surrogate smuggled through UTF-8, a
// code point past U+10FFFF. That strictness is the point: inbound TXT is
// unauthenticated network data that becomes a COMMAND, and a lenient decoder
// turns a corrupt packet into a plausible-looking verb. Malformed bytes decode
// to U+FFFD so a caller that ignores `ok` still gets something harmless.
std::string  WideToUtf8(const std::wstring& w);
std::wstring Utf8ToWide(const std::string& utf8, bool* ok);

// One TXT packet. Returns 0 when the payload will not fit, because a record
// NEVER spans packets (spec §2.4): half a record is not a slow answer, it is two
// unparseable fragments.
size_t BuildTxtPacket(uint8_t* out, const char name[kStreamNameSize],
                      uint32_t nuFrame, const std::string& utf8);
bool   ParseTxt(const Parsed& p, std::string* utf8);

// Pack whole records into packet-sized, newline-joined chunks.
//
// Multi-record replies (MDXM_BEGIN … MDXM_END) are longer than one packet, so
// they span several -- but the split always falls BETWEEN records. A single
// record too large for any packet is replaced by an error record rather than cut
// in half: the caller then has something it can act on.
std::vector<std::wstring> ChunkReplies(const std::vector<std::wstring>& records);

// ── FRAME: a whole image, in packets ─────────────────────────────────────
//
// Rev 13's image sub-protocol, and the reason the screen captures need no second
// transport: one complete JPEG per VBAN frame, chunked. VB-Audio's own
// VBAN-Screen consumes exactly this, which makes it the interoperability test.
//
// A dropped packet loses the whole frame -- there is no retransmission and no
// partial decode -- which is the right trade here: the next capture replaces it
// a fraction of a second later.
constexpr uint8_t kFrameStart = 0x01, kFrameContinue = 0x02, kFrameEnd = 0x04;

// The packet index, which is 16 bits split across two header bytes.
//
// WRAPS AT 65536, following the spec's own LSB/MSB split code. The spec's PROSE
// says packets are numbered 0..64535 and talks about 64536 -- a digit
// transposition, since its code masks 0xFF and 0xFF00. The code is the authority:
// it is what the other implementations were written against.
inline uint16_t FramePacketIndex(size_t i) { return (uint16_t)(i % 65536); }

// Split one image into FRAME packets. `out` is cleared first.
//
// Refused whole (out left empty) for an empty image, and for one too large for
// the index space -- a frame that needs more than 65536 packets is a bug
// upstream, not something to send.
//
// A SINGLE-PACKET frame is marked start|end. The spec lists first/next/last and
// says nothing about a frame that is all three; the field is laid out as three
// bits, so setting both is the reading that follows from the layout. A near-black
// screen produces this case, which is why it is pinned by a test and listed in
// the interop check rather than left to chance.
void ChunkFrame(std::vector<std::vector<uint8_t>>& out, const char name[kStreamNameSize],
                uint32_t nuFrame, const uint8_t* bytes, size_t len);

// ── AUDIO ────────────────────────────────────────────────────────────────
//
// format_bit data types. The codec is the HIGH nibble and PCM is zero, so a
// data type alone is a complete format_bit for everything we send -- the other
// codecs (VBCA/VBCV) are VB-Audio's own and are not freely licensed.
constexpr uint8_t kBitInt16 = 0x01, kBitFloat32 = 0x04;

// Frames per packet, by format. Both land on a 1024-byte payload, comfortably
// inside the spec's 1436: int16 at the spec's own maximum of 256 samples
// (5.33 ms at 48 kHz), float32 at 128 because 256 float frames would be 2048
// bytes and would not fit -- the hard ceiling there is 179.
constexpr size_t kFramesPerPacketI16 = 256, kFramesPerPacketF32 = 128;
size_t AudioFramesPerPacket(uint8_t bitType);

// Build one AUDIO packet: header + interleaved stereo PCM. Returns the total
// byte count, or 0 for arguments that cannot make a legal packet (a payload
// over kMaxData is REFUSED rather than truncated -- a short packet is a corrupt
// packet at the far end, and silently sending one is worse than sending none).
//
// Gain and limiting are NOT applied here. They are the sender's policy on the
// float block before this is called; the only thing this guarantees about
// level is that the conversion cannot wrap (see the clamp in the .cpp).
size_t BuildAudioPacket(uint8_t* out, const char name[kStreamNameSize],
                        uint32_t nuFrame, int srIndex, uint8_t bitType,
                        const float* interleavedStereo, size_t frames);

}} // namespace mdxm::vban
