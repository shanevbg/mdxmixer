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

// How often a subscriber is pushed to, in milliseconds.
//
// 250 is the default and was the only rate: it is the tick the window itself
// used to run at, and nothing on the wire ever asked for anything else. A
// client that wants to draw meters rather than poll them can ask for faster --
// "You can have the subscriber get 100 ms if they ask for it" -- and one that
// only wants to know when a device appears can ask for slower and save both
// ends the work.
//
// The floor is the rate the window refreshes at: a push cannot carry anything
// the sweep behind it has not produced, so asking for 20 ms would send the same
// numbers five times. The ceiling is there because a subscription that reports
// in once a minute is indistinguishable from a dead one.
constexpr int kPushIntervalDefaultMs = 250;
constexpr int kPushIntervalMinMs = 100;
constexpr int kPushIntervalMaxMs = 5000;

// Clamped, with anything unreadable falling back to the default rather than
// failing the subscription: a client that gets this wrong should still get its
// state, at a rate that cannot hurt anyone.
inline int ClampPushIntervalMs(int ms) {
    if (ms <= 0) return kPushIntervalDefaultMs;
    if (ms < kPushIntervalMinMs) return kPushIntervalMinMs;
    if (ms > kPushIntervalMaxMs) return kPushIntervalMaxMs;
    return ms;
}

// Returns every reply message for one incoming message, in order. Never throws.
//
// `wantIntervalMs` is written only by MDXM_SUBSCRIBE, and only when it carried
// a rate; it is optional so that every existing caller -- and every test --
// goes on compiling against the two-argument form.
std::vector<std::wstring> HandleProtocolMessage(const std::wstring& msg, IMixerControl& ctl,
                                                bool* wantSubscribe,
                                                int* wantIntervalMs = nullptr);

// One MDXM_CHAN record.
//
// Exported because there are TWO producers of this record -- a reply to
// MDXM_STATE and an unprompted push to a subscriber -- and they were separate
// pieces of formatting that had already drifted: the push was hand-written with
// swprintf in app_controller.cpp, so a field added here appeared only to
// clients that polled. Two spellings of one record is a bug with a delay fuse.
std::wstring ChannelRecord(const ChannelState& c);

// One MDXM_DEVLVL record: an endpoint as a front-end can draw it without
// recomputing anything.
//
// Exported for the same reason ChannelRecord is, and against the same hazard:
// there are now three producers of this record -- the MDXM_STATE reply, the
// echo from MDXM_HIDE / MDXM_PIN, and the push to a subscriber -- and a field
// added to one hand-written spelling is invisible to every client reading
// another. One formatter, so they cannot drift.
std::wstring DeviceRecord(const DeviceLevel& d);

// Which endpoint rows exist, in order: MDXM_DEVSET|dev=<id>|dev=<id>|…
//
// Pushed when the SET of rows changes rather than when a row's values change,
// because a client with a control per row has to rebuild its controls then and
// only then (MDropDX12's window does exactly that). A removal is the reason it
// cannot be inferred from MDXM_DEVLVL pushes: a device that has gone has no
// row to push, so nothing would arrive to say so.
//
// Complete and self-contained, like MDXM_PEAK: the ids ARE the set, in the
// order mdxmixer sorts them, so a client never has to reconcile a sequence of
// edits and the record cannot be misread if something else lands between it
// and the rows that follow.
std::wstring DeviceSetRecord(const std::vector<DeviceLevel>& devices);

// Do these two readings of one endpoint differ in anything a client would
// redraw -- ignoring `peak`, which moves constantly and travels on MDXM_PEAK?
//
// This is what makes a device push event-driven in effect rather than a timer:
// the 250 ms sweep that already runs for the peaks is diffed through here, and
// a row is only sent when something about it actually changed.
bool SameDeviceRow(const DeviceLevel& a, const DeviceLevel& b);

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

// The VBAN server's state, as one record.
//
// Exported for the reason ChannelRecord is: there will be more than one producer
// (the MDXM_VBAN reply, the VBAN tab's own status strip, and a push if one is
// ever added), and two hand-written spellings of one record is a bug with a
// delay fuse.
std::wstring VbanStateRecord(const VbanStatus& s);
// One MDXM_VBANPEER row.
std::wstring VbanPeerRecord(const VbanPeerRow& p);

constexpr wchar_t kProtocolVersion[] = L"1";

} // namespace mdxm
