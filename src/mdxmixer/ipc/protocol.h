#pragma once
// MDXM pipe protocol — same record grammar as MDropDX12's pipe: VERB|field=value|…,
// positional payload after VERB=, ids contain no '|'. Chunked replies are framed
// MDXM_BEGIN … MDXM_END (the MIXER_STATE contract). Errors: MDXM_ERR|msg=<text>.
#include "mixer_control.h"
#include <initializer_list>
#include <string>
#include <vector>

namespace mdxm {

struct Record {
    std::wstring verb;
    // key/value fields; positional tokens (no '=') carry an empty key, in order.
    std::vector<std::pair<std::wstring, std::wstring>> fields;
    const std::wstring* Find(const std::wstring& key) const;
};

Record       ParseRecord(const std::wstring& msg);
std::wstring BuildRecord(const std::wstring& verb,
                         std::initializer_list<std::pair<std::wstring, std::wstring>> fields);

// Returns every reply message for one incoming message, in order. Never throws.
std::vector<std::wstring> HandleProtocolMessage(const std::wstring& msg, IMixerControl& ctl,
                                                bool* wantSubscribe);

// One MDXM_CHAN record.
//
// Exported because there are TWO producers of this record -- a reply to
// MDXM_STATE and an unprompted push to a subscriber -- and they were separate
// pieces of formatting that had already drifted: the push was hand-written with
// swprintf in app_controller.cpp, so a field added here appeared only to
// clients that polled. Two spellings of one record is a bug with a delay fuse.
std::wstring ChannelRecord(const ChannelState& c);

// One MDXM_PEAK record: every channel and every endpoint's current level, in
// one message.
//
// WHY A SEPARATE VERB. Peaks move constantly and the rest of the state does
// not. Pushing MDXM_CHAN four times a second to carry one changed number would
// make a subscriber re-read every fader position it already knows, and a client
// cannot tell a push that means "a fader moved" from one that means "the music
// got louder". This record says only the second thing.
//
// WHY ONE MESSAGE. Thirty-odd rows as thirty-odd messages, four times a
// second, is a hundred and forty messages a second to say what fits in one.
// Repeated keys with a `~` separator, the same shape MDXM_FAILOVER_LIST uses.
//
// IT IS COMPLETE, NOT A DELTA. Every channel and every endpoint appears every
// time, so a client never has to remember which rows it was last told about,
// and a row that is absent genuinely no longer exists.
std::wstring PeakRecord(const std::vector<ChannelState>& channels,
                        const std::vector<DeviceLevel>& devices);

constexpr wchar_t kProtocolVersion[] = L"1";

} // namespace mdxm
