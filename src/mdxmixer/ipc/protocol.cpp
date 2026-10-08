#include "protocol.h"
#include "device/seen_format.h"
#include <algorithm>
#include <cwchar>

namespace mdxm {

namespace {

std::wstring FmtNum(double v) {
    wchar_t buf[32];
    swprintf(buf, 32, L"%g", v);   // 0.85, not 0.850000
    return buf;
}

std::vector<std::wstring> Err(const std::wstring& msg) {
    return { L"MDXM_ERR|msg=" + msg };
}

// Whole-token numeric parse: "purple" and "" are malformed, not 0.
bool ParseNum(const std::wstring& s, double* out) {
    if (s.empty()) return false;
    wchar_t* end = nullptr;
    double v = wcstod(s.c_str(), &end);
    if (end != s.c_str() + s.size()) return false;
    *out = v;
    return true;
}

bool ParseMix(const std::wstring& s, Mix* out) {
    if (s == L"personal")  { *out = Mix::Personal;  return true; }
    if (s == L"streaming") { *out = Mix::Streaming; return true; }
    return false;
}

bool ParseBool01(const std::wstring& s, bool* out) {
    if (s == L"0") { *out = false; return true; }
    if (s == L"1") { *out = true;  return true; }
    return false;
}

} // namespace

std::wstring ChannelRecord(const ChannelState& c) {
    return L"MDXM_CHAN|id=" + c.id + L"|name=" + c.name +
           L"|health=" + (c.healthy ? L"ok" : L"bad") +
           L"|pvol=" + FmtNum(c.pvol) + L"|pmute=" + (c.pmute ? L"1" : L"0") +
           L"|svol=" + FmtNum(c.svol) + L"|smute=" + (c.smute ? L"1" : L"0") +
           L"|eq=" + (c.eqOn ? L"1" : L"0") +
           // Appended, never inserted. A field added at the END of an existing
           // record is invisible to a client that does not know about it,
           // which is why this is not a new verb: MDropDX12 relays MDXM_CHAN
           // onto its own MIXER_FADER, and a reader that ignores `peak` keeps
           // working unchanged.
           L"|peak=" + FmtNum(c.peak) +
           // Same rule, same reason. `idle` says the channel is not capturing
           // because nothing is pulling the mix -- which is why its peak is
           // -1 and its health is ok at the same time (fj#10).
           L"|idle=" + (c.idle ? L"1" : L"0");
}

// Everything a front-end needs to draw the device list the way mdxmixer draws
// it, rather than recomputing any of it: the alias, the battery, when it was
// last here, and the anchors that identify the physical device behind a
// pairing.
//
// `seen` is the sortable absolute form, never the friendly one -- "Today
// 16:07" sorts below "Ystrdy" and both above every real date, and a client in
// another timezone could not undo the substitution anyway. Rendering it is the
// client's job.
std::wstring DeviceRecord(const DeviceLevel& d) {
    return L"MDXM_DEVLVL|id=" + d.id + L"|name=" + d.name +
           L"|alias=" + d.displayName +
           L"|flow=" + (d.isRender ? L"render" : L"capture") +
           L"|default=" + (d.isDefault ? L"1" : L"0") +
           L"|vol=" + FmtNum(d.vol) +
           L"|mute=" + (d.mute ? L"1" : L"0") +
           L"|active=" + (d.active ? L"1" : L"0") +
           L"|battery=" + std::to_wstring(d.battery) +
           L"|seen=" + AbsoluteSeen(d.lastConnectedUtc) +
           L"|handsfree=" + (d.isHandsFree ? L"1" : L"0") +
           L"|hidden=" + (d.hidden ? L"1" : L"0") +
           L"|pinned=" + (d.pinned ? L"1" : L"0") +
           L"|container=" + d.containerId +
           L"|bt=" + d.btAddress +
           // What is FLOWING on the endpoint, as against `vol`, which is where
           // its slider sits. -1 means the meter could not be read and is a
           // different answer from 0, exactly as `battery` above. Appended
           // last, for the reason ChannelRecord gives.
           L"|peak=" + FmtNum(d.peak);
}

std::wstring DeviceSetRecord(const std::vector<DeviceLevel>& devices) {
    std::wstring out = L"MDXM_DEVSET";
    for (const auto& d : devices) out += L"|dev=" + d.id;
    return out;
}

bool SameDeviceRow(const DeviceLevel& a, const DeviceLevel& b) {
    // Everything DeviceRecord carries except `peak`. Compared field by field
    // rather than by formatting both records and comparing the strings,
    // because this runs over every endpoint four times a second while a client
    // is subscribed -- eighty-odd rows on this machine -- and the one field
    // that has to be excluded is the one that changes every time.
    return a.id == b.id && a.name == b.name && a.displayName == b.displayName &&
           a.isRender == b.isRender && a.isDefault == b.isDefault &&
           a.vol == b.vol && a.mute == b.mute && a.active == b.active &&
           a.battery == b.battery &&
           // The RENDERED form, not the raw FILETIME. `seen` is minute
           // resolution on the wire, so two timestamps inside one minute are
           // the same row to a client -- comparing the ticks would push a
           // record byte-identical to the one just sent. The diff has to mean
           // "differs in what a client would see", or it is not a diff of the
           // record.
           AbsoluteSeen(a.lastConnectedUtc) == AbsoluteSeen(b.lastConnectedUtc) &&
           a.isHandsFree == b.isHandsFree && a.hidden == b.hidden &&
           a.pinned == b.pinned && a.containerId == b.containerId &&
           a.btAddress == b.btAddress;
}

std::wstring VbanStateRecord(const VbanStatus& s) {
    // `error` is LAST, like MDXM_FOSTATE's `reason`: it is free text, and a
    // field added after it would be invisible to a reader that stops at the
    // first key it does not recognise.
    return L"MDXM_VBANSTATE|on=" + std::wstring(s.on ? L"1" : L"0") +
           L"|emitting=" + (s.emitting ? L"1" : L"0") +
           L"|port=" + std::to_wstring(s.port) +
           L"|name=" + s.name +
           L"|peers=" + std::to_wstring(s.peers) +
           L"|source=" + (s.sourceStreaming ? L"streaming" : L"personal") +
           L"|format=" + (s.formatFloat32 ? L"f32" : L"i16") +
           L"|gain=" + std::to_wstring(s.gainPercent) +
           L"|fps=" + FmtNum(s.fps) +
           L"|open=" + (s.open ? L"1" : L"0") +
           L"|always=" + (s.always ? L"1" : L"0") +
           L"|alwaysframes=" + (s.alwaysFrames ? L"1" : L"0") +
           L"|target=" + s.target +
           L"|sent=" + std::to_wstring(s.sent) +
           L"|starved=" + std::to_wstring(s.starved) +
           L"|dropped=" + std::to_wstring(s.dropped) +
           L"|depthms=" + std::to_wstring(s.depthMs) +
           L"|behindms=" + std::to_wstring(s.behindMs) +
           L"|srclatencyms=" + std::to_wstring(s.srcLatencyMs) +
           L"|framessent=" + std::to_wstring(s.framesSent) +
           L"|authpending=" + std::to_wstring(s.authPending) +
           L"|error=" + s.lastError;
}

std::wstring VbanPeerRecord(const VbanPeerRow& p) {
    return L"MDXM_VBANPEER|addr=" + p.addr +
           L"|device=" + p.deviceName +
           L"|authed=" + std::wstring(p.authed ? L"1" : L"0") +
           L"|since=" + std::to_wstring(p.sinceMs) +
           L"|audio=" + (p.audioOn ? L"1" : L"0") +
           L"|frames=" + (p.framesOn ? L"1" : L"0") +
           // Says "this row is the configured always-stream target, not a
           // subscriber": it never pings, holds no slot, and is not counted in
           // `peers` -- but something is being sent to it.
           L"|always=" + (p.isAlwaysTarget ? L"1" : L"0");
}

std::wstring PeakRecord(const std::vector<ChannelState>& channels,
                        const std::vector<DeviceLevel>& devices) {
    std::wstring out = L"MDXM_PEAK";
    for (const auto& c : channels) out += L"|chan=" + c.id + L"~" + FmtNum(c.peak);
    for (const auto& d : devices)  out += L"|dev="  + d.id + L"~" + FmtNum(d.peak);
    return out;
}

namespace {

// Echo reply after a successful channel mutation: the affected channel's record
// (writes are optimistic; the subscription push carries the truth — spec).
std::vector<std::wstring> ChanEcho(IMixerControl& ctl, const std::wstring& id) {
    for (const auto& c : ctl.GetChannels())
        if (c.id == id) return { ChannelRecord(c) };
    return { ChannelRecord({ id, id, true, 1.0f, false, 1.0f, false, false }) };
}

std::vector<std::wstring> HandleInner(const std::wstring& msg, IMixerControl& ctl,
                                      bool* wantSubscribe, int* wantIntervalMs) {
    Record r = ParseRecord(msg);
    if (r.verb.empty()) return Err(L"empty message");

    // Positional args, in order (empty-key fields).
    std::vector<std::wstring> a;
    for (const auto& f : r.fields)
        if (f.first.empty()) a.push_back(f.second);

    if (r.verb == L"MDXM_PING")
        return { std::wstring(L"MDXM_PONG|version=") + kProtocolVersion };

    if (r.verb == L"MDXM_STATE") {
        std::vector<std::wstring> out;
        out.push_back(L"MDXM_BEGIN");
        // Device levels are fetched FIRST and emitted last, which looks
        // backwards and is deliberate: that sweep is what feeds the peak hold,
        // and the Sonar channel peaks are joined off its results. Asked in
        // record order, the very first MDXM_STATE on a fresh connection would
        // report every Sonar channel's peak as unknown. The order of the
        // RECORDS in the reply is unchanged, which is the part that is a
        // contract.
        const auto levels = ctl.GetDeviceLevels();
        for (const auto& c : ctl.GetChannels()) out.push_back(ChannelRecord(c));
        for (const auto& rt : ctl.GetRoutes())
            out.push_back(L"MDXM_ROUTE|id=" + rt.first + L"|device=" + rt.second);
        for (const auto& d : ctl.GetDevices())
            out.push_back(L"MDXM_DEV|id=" + std::get<0>(d) + L"|name=" + std::get<1>(d) +
                          L"|flow=" + (std::get<2>(d) ? L"render" : L"capture") +
                          L"|active=" + (std::get<3>(d) ? L"1" : L"0"));
        // One formatter, shared with the echo and the push. See DeviceRecord.
        for (const auto& d : levels) out.push_back(DeviceRecord(d));
        out.push_back(L"MDXM_END");
        return out;
    }

    if (r.verb == L"MDXM_SET") {
        if (a.size() != 3) return Err(L"MDXM_SET wants <ch>|<personal|streaming>|<0..1>");
        Mix mix; double vol;
        if (!ParseMix(a[1], &mix)) return Err(L"bad mix: " + a[1]);
        if (!ParseNum(a[2], &vol)) return Err(L"bad volume: " + a[2]);
        vol = std::clamp(vol, 0.0, 1.0);
        if (!ctl.SetVolume(a[0], mix, (float)vol)) return Err(L"unknown channel: " + a[0]);
        return ChanEcho(ctl, a[0]);
    }

    if (r.verb == L"MDXM_MUTE") {
        if (a.size() != 3) return Err(L"MDXM_MUTE wants <ch>|<personal|streaming>|<0|1>");
        Mix mix; bool mute;
        if (!ParseMix(a[1], &mix)) return Err(L"bad mix: " + a[1]);
        if (!ParseBool01(a[2], &mute)) return Err(L"bad mute flag: " + a[2]);
        if (!ctl.SetMute(a[0], mix, mute)) return Err(L"unknown channel: " + a[0]);
        return ChanEcho(ctl, a[0]);
    }

    if (r.verb == L"MDXM_EQ_SET") {
        if (a.size() != 5) return Err(L"MDXM_EQ_SET wants <ch>|<band>|<freq>|<gain>|<q>");
        double band, freq, gain, q;
        if (!ParseNum(a[1], &band) || band < 0 || band >= 10 || band != (double)(int)band)
            return Err(L"bad band index: " + a[1]);
        if (!ParseNum(a[2], &freq)) return Err(L"bad freq: " + a[2]);
        if (!ParseNum(a[3], &gain)) return Err(L"bad gain: " + a[3]);
        if (!ParseNum(a[4], &q))    return Err(L"bad q: " + a[4]);
        if (!ctl.SetEqBand(a[0], (size_t)band, freq, gain, q)) return Err(L"unknown channel: " + a[0]);
        return ChanEcho(ctl, a[0]);
    }

    if (r.verb == L"MDXM_EQ_ENABLE") {
        if (a.size() != 2) return Err(L"MDXM_EQ_ENABLE wants <ch>|<0|1>");
        bool on;
        if (!ParseBool01(a[1], &on)) return Err(L"bad enable flag: " + a[1]);
        if (!ctl.EnableEq(a[0], on)) return Err(L"unknown channel: " + a[0]);
        return ChanEcho(ctl, a[0]);
    }

    if (r.verb == L"MDXM_ASSIGN") {
        if (a.size() != 2) return Err(L"MDXM_ASSIGN wants <exePath>|<ch or ->");
        if (!ctl.AssignApp(a[0], a[1])) return Err(L"assign failed");
        return { L"MDXM_OK" };
    }

    if (r.verb == L"MDXM_ROUTE_SET") {
        if (a.size() != 2) return Err(L"MDXM_ROUTE_SET wants personal|<endpointId>");
        if (a[0] != L"personal") return Err(L"unknown route: " + a[0]);
        if (!ctl.SetPersonalRoute(a[1])) return Err(L"route set failed");
        return { L"MDXM_OK" };
    }

    if (r.verb == L"MDXM_SUBSCRIBE") {
        if (a.empty() || a.size() > 2)
            return Err(L"MDXM_SUBSCRIBE wants <0|1> [intervalMs]");
        bool on;
        if (!ParseBool01(a[0], &on)) return Err(L"bad subscribe flag: " + a[0]);
        if (wantSubscribe) *wantSubscribe = on;
        // The optional rate. A client that wants to DRAW what it is sent asks
        // for a short one; a client watching for a device to appear can ask
        // for a long one and save both ends the work.
        //
        // Clamped rather than rejected, and reported back, so a client never
        // has to guess what it actually got: the reply is the rate in force.
        if (a.size() == 2 && wantIntervalMs)
            *wantIntervalMs = ClampPushIntervalMs(_wtoi(a[1].c_str()));
        if (a.size() == 2) {
            wchar_t ms[32];
            swprintf(ms, 32, L"%d", ClampPushIntervalMs(_wtoi(a[1].c_str())));
            return { std::wstring(L"MDXM_OK|intervalMs=") + ms };
        }
        return { L"MDXM_OK" };
    }

    if (r.verb == L"MDXM_CAPTURE") {
        if (a.empty() || a.size() > 2 || a[0].empty())
            return Err(L"MDXM_CAPTURE wants <pngPath> [main|hotkeys|overlay]");
        std::wstring which = a.size() == 2 ? a[1] : L"main";
        // `overlay` is the battery readout. It is frameless, click-through and
        // always on top, so it cannot be focused and no window-picker can
        // select it -- without this the only way to get a picture of it is a
        // full-desktop grab, which publishes whatever else is on screen.
        if (which != L"main" && which != L"hotkeys" && which != L"overlay")
            return Err(L"MDXM_CAPTURE window must be main, hotkeys or overlay");
        if (!ctl.CaptureUi(a[0], which)) return Err(L"capture failed");
        return { L"MDXM_OK" };
    }

    // Switch the main window to a tab, by name.
    //
    // It exists so the README's screenshots can be REGENERATED rather than
    // grabbed by hand once and left to rot as the window changes. Same reason
    // MDXM_CAPTURE exists at all: a picture nobody can retake is a picture
    // that will be wrong by the next release.
    if (r.verb == L"MDXM_TAB") {
        if (a.size() != 1 || a[0].empty())
            return Err(L"MDXM_TAB wants <mixer|routing|eq|devices|vban|options>");
        if (!ctl.ShowTab(a[0])) return Err(L"unknown tab: " + a[0]);
        return { L"MDXM_OK" };
    }

    if (r.verb == L"MDXM_DEVVOL") {
        if (a.size() != 2) return Err(L"MDXM_DEVVOL wants <endpointId>|<0..1>");
        double vol;
        if (!ParseNum(a[1], &vol)) return Err(L"bad volume: " + a[1]);
        if (!ctl.SetDeviceVolume(a[0], (float)std::clamp(vol, 0.0, 1.0)))
            return Err(L"unknown endpoint: " + a[0]);
        return { L"MDXM_OK" };
    }

    if (r.verb == L"MDXM_DEVMUTE") {
        if (a.size() != 2) return Err(L"MDXM_DEVMUTE wants <endpointId>|<0|1>");
        bool mute;
        if (!ParseBool01(a[1], &mute)) return Err(L"bad mute flag: " + a[1]);
        if (!ctl.SetDeviceMute(a[0], mute)) return Err(L"unknown endpoint: " + a[0]);
        return { L"MDXM_OK" };
    }

    if (r.verb == L"MDXM_SHOW") {
        if (!ctl.ShowUi()) return Err(L"show failed");
        return { L"MDXM_OK" };
    }

    if (r.verb == L"MDXM_DEFAULT") {
        if (a.size() != 1 || a[0].empty()) return Err(L"MDXM_DEFAULT wants <endpointId>");
        std::wstring derr;
        if (!ctl.SetDefaultOutput(a[0], &derr))
            return Err(derr.empty() ? L"default output change failed" : derr);
        return { L"MDXM_OK" };
    }

    if (r.verb == L"MDXM_APPROUTE") {
        if (a.size() != 2 || a[0].empty() || a[1].empty())
            return Err(L"MDXM_APPROUTE wants <exePath>|<endpointId>");
        std::wstring rerr;
        if (!ctl.RouteAppToEndpoint(a[0], a[1], &rerr))
            return Err(rerr.empty() ? L"app route failed" : rerr);
        return { L"MDXM_OK" };
    }

    if (r.verb == L"MDXM_EXIT") {
        if (!ctl.ExitApp()) return Err(L"exit failed");
        return { L"MDXM_OK" };
    }

    if (r.verb == L"MDXM_HOTKEYS") {
        if (!ctl.ShowHotkeysUi()) return Err(L"hotkeys window failed");
        return { L"MDXM_OK" };
    }

    // The rule AND what it currently sees, in one reply: a front-end drawing
    // this list needs both, and two round trips could disagree.
    // The shared-memory feed, which does not run unless asked.
    //
    // A reader turns it on, reads, and turns it off. Query form reports
    // whether it is up and what the ring is -- enough to decide whether to
    // bother mapping it.
    if (r.verb == L"MDXM_FEED") {
        const auto say = [&ctl]() {
            return std::vector<std::wstring>{
                L"MDXM_FEEDSTATE|on=" + std::wstring(ctl.FeedEnabled() ? L"1" : L"0") +
                // Doubled: a lone backslash before 'm' is an unknown escape
                // and the compiler drops it, which published the name as
                // "Localmdxmixer_stream_v1" -- a name no reader would find.
                L"|name=Local\\mdxmixer_stream_v1" +
                L"|rate=" + std::to_wstring(ctl.FeedRate()) +
                L"|channels=2"
            };
        };
        if (a.empty()) return say();
        bool on;
        if (!ParseBool01(a[0], &on)) return Err(L"MDXM_FEED wants <0|1>, or nothing to query");
        std::wstring ferr;
        if (!ctl.SetFeedEnabled(on, &ferr))
            return Err(ferr.empty() ? L"feed switch failed" : ferr);
        return say();
    }

    // The VBAN stream server (spec §6.3).
    //
    // THE FIRST KEYED-ARGUMENT INBOUND VERBS IN THIS PROTOCOL. Every other verb
    // above is positional, and the `a` vector is built from empty-key fields
    // only -- so for a keyed record it is EMPTY. The nearest precedent,
    // MDXM_FEED, reads empty args as "query", and a handler written in that
    // shape would turn every keyed SET into a query and answer with a state
    // record as though the write had happened.
    if (r.verb == L"MDXM_VBAN") {
        for (const auto& f : r.fields) {
            if (f.first.empty()) continue;      // a stray positional token
            // Per-peer grants mean nothing here: the pipe is not a peer, so
            // there is nothing to apply them to. MDXM_ERR is the grammar's only
            // negative reply -- there is no "warning" kind.
            if (f.first == L"frames") return Err(L"frames is VBAN-only");
            if (f.first == L"audio")  return Err(L"audio is VBAN-only");
            std::wstring verr;
            if (!ctl.SetVbanOption(f.first, f.second, &verr))
                return Err(verr.empty() ? L"bad vban option: " + f.first : verr);
        }
        // Set or query, the answer is the state: a caller never has to make a
        // second round trip to find out what its write actually produced.
        return { VbanStateRecord(ctl.GetVbanStatus()) };
    }

    if (r.verb == L"MDXM_VBANPEERS") {
        std::vector<std::wstring> out;
        out.push_back(L"MDXM_BEGIN");
        for (const auto& p : ctl.GetVbanPeers()) out.push_back(VbanPeerRecord(p));
        out.push_back(L"MDXM_END");
        return out;
    }

    // MDXM_AUTH is meaningful only over VBAN-TXT, where the server answers it
    // against a known peer before anything is dispatched (spec §4). Reaching
    // this handler means it arrived on the pipe, which has its own ACL and needs
    // no PIN.
    if (r.verb == L"MDXM_AUTH")
        return Err(L"auth is VBAN-only");

    if (r.verb == L"MDXM_FAILOVER") {
        const FailoverConfig fo = ctl.GetFailover();
        const auto levels = ctl.GetDeviceLevels();
        std::vector<std::wstring> out;
        out.push_back(L"MDXM_BEGIN");
        out.push_back(L"MDXM_FO|armed=" + std::wstring(fo.armed ? L"1" : L"0") +
                      L"|stability=" + std::to_wstring(fo.stabilitySec) +
                      L"|dwell=" + std::to_wstring(fo.dwellSec) +
                      L"|gap=" + std::to_wstring(fo.minGapSec));
        // What the watcher is DOING with that rule. MDropDX12 keeps the
        // Failover tab and authors the rule; this is the only process that
        // watches and acts, so that tab has to be able to show what happened
        // rather than infer it from the device list.
        //
        // `reason` is last because it is free text: a field added after it
        // would be invisible to a reader that stops at the first key it does
        // not know, and this is the field whose whole value is being readable.
        const FailoverStatus fs = ctl.GetFailoverStatus();
        out.push_back(L"MDXM_FOSTATE|route=" + fs.routeId +
                      L"|state=" + fs.state +
                      L"|current=" + fs.current +
                      L"|target=" + fs.target +
                      L"|attempts=" + std::to_wstring(fs.attempts) +
                      L"|dwell=" + std::to_wstring(fs.dwellMs) +
                      L"|since=" + std::to_wstring(fs.sinceCommitMs) +
                      L"|hold=" + std::to_wstring(fs.holdMs) +
                      L"|audiodg=" + std::to_wstring(fs.audiodgPid) +
                      L"|restarts=" + std::to_wstring(fs.audiodgRestarts) +
                      L"|reason=" + fs.reason);
        for (size_t i = 0; i < fo.allow.size(); ++i) {
            const DeviceRef& e = fo.allow[i];
            // Matched by id, or -- for an entry added while its device was
            // switched off, which has no id -- by the Windows name it
            // stored. Both, because this list is mostly devices not here.
            const DeviceLevel* d = nullptr;
            for (const auto& l : levels)
                if ((!e.id.empty() && l.id == e.id) ||
                    (e.id.empty() && !e.name.empty() && l.name == e.name)) { d = &l; break; }
            out.push_back(L"MDXM_FOENTRY|i=" + std::to_wstring(i) +
                          L"|id=" + e.id + L"|name=" + e.name +
                          L"|alias=" + (d ? d->displayName : e.name) +
                          L"|present=" + std::wstring(d && d->active ? L"1" : L"0") +
                          // Battery only when the device is HERE: Windows
                          // keeps the last figure after a disconnect, so one
                          // shown beside an absent device is the past dressed
                          // up as the present.
                          L"|battery=" + std::to_wstring(d && d->active ? d->battery : -1) +
                          L"|seen=" + (d ? AbsoluteSeen(d->lastConnectedUtc) : std::wstring()) +
                          L"|known=" + std::wstring(d ? L"1" : L"0"));
        }
        out.push_back(L"MDXM_END");
        return out;
    }

    if (r.verb == L"MDXM_FAILOVER_SET") {
        // Any subset of the settings; whatever is absent is left alone.
        const FailoverConfig cur = ctl.GetFailover();
        bool armed = cur.armed, sawArmed = false;
        int stability = cur.stabilitySec, dwell = cur.dwellSec, gap = cur.minGapSec;
        bool sawTiming = false;
        for (const auto& f : r.fields) {
            if (f.first == L"armed") sawArmed = ParseBool01(f.second, &armed);
            else if (f.first == L"stability") { stability = _wtoi(f.second.c_str()); sawTiming = true; }
            else if (f.first == L"dwell") { dwell = _wtoi(f.second.c_str()); sawTiming = true; }
            else if (f.first == L"gap") { gap = _wtoi(f.second.c_str()); sawTiming = true; }
        }
        if (!sawArmed && !sawTiming)
            return Err(L"MDXM_FAILOVER_SET wants armed=<0|1> and/or stability=|dwell=|gap=");
        if (sawArmed) ctl.SetFailoverArmed(armed);
        if (sawTiming) ctl.SetFailoverTiming(stability, dwell, gap);
        return { L"MDXM_OK" };
    }

    if (r.verb == L"MDXM_FAILOVER_LIST") {
        // The ORDERED allowlist, replaced outright. Repeated dev= fields keep
        // their order through the parser, which is the whole reason the list
        // is sent this way rather than as add/remove/move: the order IS the
        // rule, and a sequence of edits has intermediate states that are each
        // a different rule -- one of which would be live if the caller died
        // halfway through.
        //
        // Each value is "<endpointId>~<windowsName>", either half optional. A
        // name alone is how a device that is switched off right now gets onto
        // the list at all: it has no endpoint id to give.
        std::vector<DeviceRef> allow;
        for (const auto& f : r.fields) {
            if (f.first != L"dev") continue;
            const size_t tilde = f.second.find(L'~');
            DeviceRef d;
            if (tilde == std::wstring::npos) d.id = f.second;
            else { d.id = f.second.substr(0, tilde); d.name = f.second.substr(tilde + 1); }
            if (d.id.empty() && d.name.empty()) continue;
            allow.push_back(d);
        }
        ctl.SetFailoverAllow(allow);
        return { L"MDXM_OK" };
    }

    if (r.verb == L"MDXM_NAME") {
        // Our name for a device, by endpoint id. The container and the
        // Windows name are looked up here rather than demanded: a caller
        // holding an id should not have to carry the other two anchors.
        if (a.size() != 2) return Err(L"MDXM_NAME wants <endpointId>|<alias, empty to clear>");
        std::wstring container, winName;
        for (const auto& d : ctl.GetDeviceLevels())
            if (d.id == a[0]) { container = d.containerId; winName = d.name; break; }
        if (winName.empty()) return Err(L"no such endpoint");
        if (!ctl.SetDeviceAlias(a[0], container, winName, a[1])) return Err(L"rename failed");
        return { L"MDXM_OK" };
    }

    // How the user has FILED a device: held at the top of the list, or out of
    // it altogether. The same kind of statement as an alias -- it is about the
    // person's list and not about the audio -- and settable for the same
    // reason MDXM_NAME is: a front-end that delegates its device list to
    // MDXM_DEVLVL has nowhere else to put those two actions, and keeping its
    // own private hidden/pinned set beside this one would get the two out of
    // step the moment either was used.
    //
    // NOT REFUSED FOR A DEVICE IN USE. The precedent for refusing a write is
    // MDXM_MUTE on sonar:masters, which is refused because it would do harm --
    // it rewrites every channel's mute. Hiding the endpoint the personal mix
    // plays to does nothing to the audio: it is a view, the mix keeps running,
    // and the Mixer tab hoists the live personal route back to the top
    // regardless of the flag. Recorded, therefore, rather than second-guessed.
    if (r.verb == L"MDXM_HIDE" || r.verb == L"MDXM_PIN") {
        const bool hiding = (r.verb == L"MDXM_HIDE");
        if (a.size() != 2 || a[0].empty())
            return Err(r.verb + L" wants <endpointId>|<0|1>");
        bool on;
        if (!ParseBool01(a[1], &on)) return Err(L"bad flag: " + a[1]);
        // The container and the Windows name are looked up rather than
        // demanded, as MDXM_NAME does it: both are anchors that let the flag
        // survive the device coming back under a new endpoint id, and a caller
        // holding an id should not have to carry them.
        DeviceLevel row;
        bool found = false;
        for (const auto& d : ctl.GetDeviceLevels())
            if (d.id == a[0]) { row = d; found = true; break; }
        if (!found) return Err(L"no such endpoint");

        // The two flags cannot both be true: a device cannot be held at the
        // top of a list and absent from it. Setting either to 1 clears the
        // other; clearing one says nothing about the other, so unpinning a
        // visible device does not hide it.
        const bool hidden = hiding ? on : (on ? false : row.hidden);
        const bool pinned = hiding ? (on ? false : row.pinned) : on;
        if (!ctl.SetDeviceView(row.id, row.containerId, row.name, hidden, pinned))
            return Err(hiding ? L"hide failed" : L"pin failed");

        // Echoed as the row rather than MDXM_OK, the way MDXM_SET echoes
        // MDXM_CHAN: the caller gets the new state without a second round
        // trip. Optimistic, like every other write here -- the flags are the
        // ones just asked for, and the subscription push carries what the
        // store actually ended up holding for every row it affected.
        row.hidden = hidden;
        row.pinned = pinned;
        return { DeviceRecord(row) };
    }

    // The latency, in milliseconds, live.
    //
    // A verb rather than config-only because finding the right value is an
    // EXPERIMENT: lower it, watch `underruns` in MDXM_DIAG, and keep going
    // until they appear. Doing that through a config file and a restart would
    // reset the very counter being watched. Query with no argument.
    //
    // Setting it re-cushions the running channels, which costs a gap of the
    // new cushion's length while they refill -- the honest price of changing
    // the latency of a graph that is already running.
    if (r.verb == L"MDXM_CUSHION") {
        const auto say = [&ctl]() {
            return std::vector<std::wstring>{
                L"MDXM_CUSHIONSTATE|ms=" + std::to_wstring(ctl.GetCushionMs()) +
                L"|headroom=" + std::to_wstring(ctl.GetCushionHeadroomPercent()) +
                L"|flat=" + std::to_wstring(ctl.GetCushionFlatMs())
            };
        };
        // Positional is the floor, which is the common case; the two adaptive
        // terms are keyed, and anything absent is left alone.
        int ms = -1, headroom = -1, flat = -1;
        if (!a.empty()) {
            double v;
            if (!ParseNum(a[0], &v)) return Err(L"MDXM_CUSHION wants <ms>, or nothing to query");
            ms = (int)v;
        }
        for (const auto& f : r.fields) {
            double v;
            if (f.first == L"headroom") {
                if (!ParseNum(f.second, &v)) return Err(L"bad headroom: " + f.second);
                headroom = (int)v;
            } else if (f.first == L"flat") {
                if (!ParseNum(f.second, &v)) return Err(L"bad flat: " + f.second);
                flat = (int)v;
            } else if (f.first == L"ms") {
                if (!ParseNum(f.second, &v)) return Err(L"bad ms: " + f.second);
                ms = (int)v;
            }
        }
        if (ms < 0 && headroom < 0 && flat < 0) return say();
        if (!ctl.SetCushion(ms, headroom, flat)) return Err(L"cushion change failed");
        return say();
    }

    if (r.verb == L"MDXM_DIAG") {
        DiagState d = ctl.GetDiag();
        std::vector<std::wstring> out;
        out.push_back(L"MDXM_BEGIN");
        for (const auto& ring : d.rings) {
            // `cap` and `speed` carry the two things a depth cannot say on its
            // own (fj#13): what the number is a fraction OF, and whether the
            // varispeed trim is currently pulling it anywhere.
            //
            // THE ORIGINAL FAULT WAS READ FROM THIS LINE. "depth=95520" was a
            // 1.99-second backlog only because the ring was known from the
            // source to hold 96000 frames; sampled three times over a minute
            // and frozen, it proved the depth would never come back on its
            // own. Both halves of that reading are now in the line itself --
            // and a trim of anything but 1.0000 says the engine has already
            // noticed and is giving the latency back.
            wchar_t buf[192];
            swprintf(buf, 192, L"|depth=%zu|cap=%zu|drops=%llu|underruns=%llu|speed=%.4f",
                     ring.depth, ring.capacity,
                     (unsigned long long)ring.drops, (unsigned long long)ring.underruns,
                     ring.speed);
            out.push_back(L"MDXM_RING|id=" + ring.id + buf);
        }
        out.push_back(L"MDXM_DIAGDEV|personal=" + d.personalDevice +
                      L"|fallback=" + (d.personalFallback ? L"1" : L"0"));
        // The latency story for the personal path, in frames plus the rate to
        // read them with (fj#12). `cushion` is what the mix waits for before
        // draining a channel, so it IS the latency; it is sized from `window`,
        // the largest pull in the last minute, and `peak` is the worst ever
        // seen and sizes nothing. Before this there was no way to ask from
        // outside the process why the latency was what it was.
        out.push_back(L"MDXM_DIAGMIX|rate=" + std::to_wstring(d.mixRate) +
                      L"|cushion=" + std::to_wstring(d.cushionFrames) +
                      L"|cushionms=" +
                      std::to_wstring(d.mixRate ? d.cushionFrames * 1000 / d.mixRate : 0) +
                      L"|window=" + std::to_wstring(d.windowPull) +
                      L"|peak=" + std::to_wstring(d.maxMixPull));
        out.push_back(L"MDXM_END");
        return out;
    }

    return Err(L"unknown verb: " + r.verb);
}

} // namespace

const std::wstring* Record::Find(const std::wstring& key) const {
    for (const auto& f : fields)
        if (f.first == key) return &f.second;
    return nullptr;
}

Record ParseRecord(const std::wstring& msg) {
    Record r;
    size_t pos = 0;
    bool first = true;
    while (pos <= msg.size()) {
        size_t bar = msg.find(L'|', pos);
        std::wstring tok = msg.substr(pos, bar == std::wstring::npos ? std::wstring::npos : bar - pos);
        if (first) {
            first = false;
            size_t eq = tok.find(L'=');
            if (eq == std::wstring::npos) {
                r.verb = tok;
            } else {
                r.verb = tok.substr(0, eq);
                r.fields.push_back({ L"", tok.substr(eq + 1) });   // first positional payload
            }
        } else {
            size_t eq = tok.find(L'=');
            if (eq == std::wstring::npos) r.fields.push_back({ L"", tok });
            else r.fields.push_back({ tok.substr(0, eq), tok.substr(eq + 1) });
        }
        if (bar == std::wstring::npos) break;
        pos = bar + 1;
    }
    return r;
}

std::wstring BuildRecord(const std::wstring& verb,
                         std::initializer_list<std::pair<std::wstring, std::wstring>> fields) {
    std::wstring out = verb;
    for (const auto& f : fields) out += L"|" + f.first + L"=" + f.second;
    return out;
}

std::vector<std::wstring> HandleProtocolMessage(const std::wstring& msg, IMixerControl& ctl,
                                                bool* wantSubscribe, int* wantIntervalMs) {
    try {
        return HandleInner(msg, ctl, wantSubscribe, wantIntervalMs);
    } catch (...) {
        return Err(L"internal");   // no-crash rule
    }
}

} // namespace mdxm
