#pragma once
// The seam between protocol/UI and the app. No Windows types: the protocol layer
// is headless-testable against a fake, and the UI talks to the same interface.
#include "device/endpoint_volume.h"   // DeviceLevel: no Windows types in its interface
#include "app/hotkeys.h"                  // HotkeyBinding
// DeviceRef and FailoverConfig. config.h is header-only down to <string> and
// <vector> -- no Windows types come in with it, so the rule above holds and
// the protocol layer stays testable against a fake.
#include "config/config.h"
#include <cstdint>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace mdxm {

enum class Mix { Personal, Streaming };

// One channel as every surface sees it. `peak` is what the channel's SOURCE is
// carrying — see the field comment below, which is the part a client has to
// read before sorting on it.
struct ChannelState { std::wstring id, name; bool healthy;
                      float pvol; bool pmute; float svol; bool smute; bool eqOn;
                      // The channel's own signal, 0..1, or kPeakUnknown (-1).
                      //
                      // BEFORE the personal and streaming faders, deliberately.
                      // A channel has one source and TWO gains, so a post-fader
                      // peak would have to be two numbers; and the question this
                      // answers is "which of these is making sound", which stays
                      // true of a channel muted on one side. A peak-hold, so a
                      // row that spikes is still findable a moment later: the
                      // motivating case is twenty-five faders and one of them
                      // suddenly blasting, where "the fader positions do not
                      // move when it happens" and nothing else in the state can
                      // point at the culprit.
                      //
                      // -1 means CANNOT KNOW and is not the same answer as 0.0
                      // meaning silence: an unhealthy channel has no capture to
                      // meter, and reporting it as quiet would sort it among the
                      // channels that are genuinely quiet.
                      float peak = kPeakUnknown;
                      // Not capturing, because NOTHING IS PULLING THE MIX --
                      // no personal render, no streaming cable, no feed
                      // subscriber (fj#10). Distinct from `healthy`, which is
                      // about a fault: an idle channel reports health=ok,
                      // because its cable has not been opened and so nothing
                      // is known to be wrong with it, and peak=-1, because
                      // nothing is being measured. A machine with nothing
                      // listening must not read as a machine with a problem.
                      bool idle = false;
                      // The same signal with NO peak-hold on it.
                      //
                      // `peak` above holds for 1.5 s, which is what a remote
                      // client needs and what the wire carries. A meter being
                      // watched on screen wants the instant reading, so it can
                      // fall at its own rate (dsp/meter_ballistics.h) instead
                      // of standing still for a second and a half and then
                      // dropping off a cliff. Published alongside rather than
                      // instead: both come from the same measurement, and the
                      // two surfaces want different answers.
                      //
                      // kPeakUnknown under exactly the same conditions as
                      // `peak`, for exactly the same reason.
                      float peakNow = kPeakUnknown; };
// What ONE fader of a channel is passing: the source peak through that side's
// own gain, and zero when that side is muted.
//
// WHY THIS IS NEEDED AT ALL. ChannelState::peak is deliberately ONE number,
// measured before both gain stages, because the question it answers is "which
// channel is making sound" and that stays true of audio muted on one side.
// That makes it the wrong number to draw beside a FADER, and drawing it there
// was a real bug: on 2026-10-04 sonar:aux sat with its Personal fader at zero
// and its Streaming fader at 100, reported peak=0.715927 -- the signal the apps
// were sending in -- and the Mixer tab painted a full green bar on the [P] row
// of a fader passing nothing at all. Shane read it exactly as it looked: "the
// peak meters you get for the sonar channels are the streaming channels, not
// the personal channels". With one of the two faders at 100, a source meter is
// indistinguishable from that side's output.
//
// WHAT IT CLAIMS, AND WHAT IT DOES NOT. A gain stage scales a peak linearly, so
// source x gain is arithmetic rather than a guess -- but it is DERIVED and not
// measured. The engine's personal sum also passes a limiter, and a Sonar fader
// is Sonar's own taper rather than necessarily a linear multiplier, so a bar
// here is "what this fader is letting through", accurate at the ends and close
// in between. The alternative is no per-fader measurement at all: nothing in
// Windows meters one channel's contribution to a mix.
//
// kPeakUnknown survives everything. "Cannot know" must never become "silent",
// including through a mute: the mute says what the fader is passing, not
// whether the source could be read (docs/ipc.md §2.1).
inline float FaderPeak(const ChannelState& c, Mix m) {
    if (c.peak < 0.0f) return kPeakUnknown;
    const bool muted = (m == Mix::Personal) ? c.pmute : c.smute;
    if (muted) return 0.0f;
    const float vol = (m == Mix::Personal) ? c.pvol : c.svol;
    return c.peak * vol;
}

// The same, through the UNHELD reading. For a meter on screen, which applies
// its own fall (dsp/meter_ballistics.h); the wire uses FaderPeak.
//
// Deliberately a second function rather than a parameter: every existing
// caller is a wire caller and must keep the held number, and a default
// argument is how that silently stops being true.
inline float FaderPeakNow(const ChannelState& c, Mix m) {
    if (c.peakNow < 0.0f) return kPeakUnknown;
    const bool muted = (m == Mix::Personal) ? c.pmute : c.smute;
    if (muted) return 0.0f;
    const float vol = (m == Mix::Personal) ? c.pvol : c.svol;
    return c.peakNow * vol;
}

// What the failover watcher is actually DOING, as against the rule it was
// given (fj#2 §3). MDropDX12 keeps the Failover tab and authors the rule;
// mdxmixer is the only process that watches and acts, so that tab has to be
// able to show what happened rather than infer it.
//
// `reason` matters more than it looks. In MDropDX12 #410 the machine sat for
// hours in a state that reported "idle" with no reason between attempts, while
// a route re-committed the same move every dwell; the watcher now names the
// back-off explicitly ("attempt 3 did not stick; waiting 40 s before
// retrying"), and that text is the useful output when a move will not take.
//
// `current` against `target` is the other half of that diagnosis: current is
// the device the state machine believes the route is on and every decision
// turns on it, target is the last device committed to, and THE TWO
// DISAGREEING IS THE FAULT -- it means the move was made and something
// re-baselined the route off it before the next tick.
struct FailoverStatus {
    std::wstring routeId;      // "personal"
    std::wstring state;        // idle | searching | arming
    std::wstring reason;
    std::wstring current;      // the endpoint the route is on
    std::wstring target;       // the last endpoint committed to
    unsigned attempts = 0;     // consecutive commits to the same target; 0 or 1 is healthy
    unsigned dwellMs = 0;      // in force for this route, after any back-off
    unsigned sinceCommitMs = 0;
    unsigned holdMs = 0;       // remaining hold, 0 when not held
    // AUDIODG.EXE, because "why is nothing happening" is usually this: while
    // the audio graph rebuilds every endpoint reads absent, and the watcher is
    // deliberately standing down. A front-end showing a paused watcher should
    // be able to say why it is paused.
    unsigned long audiodgPid = 0;
    unsigned audiodgRestarts = 0;
};
// Where one playing app is actually being sent, against where mdxmixer's
// config says it belongs (fj#1). Both endpoint ids; either may be empty.
//
// It exists so drift is VISIBLE rather than inferred. Windows persists a
// per-app route itself, so an app assigned once while it was playing keeps
// working across restarts without mdxmixer's help -- which is exactly what
// hid the missing reconciler: the only symptoms were "that app won't take an
// assignment" and "that app came back on the wrong device".
//
// Answered by the controller rather than worked out by the UI, because
// resolving a channel to its routable endpoint is the reconciler's own logic
// and two copies of it would drift.
struct AppRouteState {
    std::wstring intendedChannel;      // channel name, empty when nothing claims the app
    std::wstring intendedEndpointId;   // empty when that channel has nowhere to send yet
    std::wstring actualEndpointId;     // what Windows holds for the pid; empty when none
    bool available = false;            // false when per-app routing is not available at all
};
struct DiagState    { struct Ring { std::wstring id; size_t depth; uint64_t drops, underruns;
                          // What `depth` has to be read against, and whether
                          // anything is currently correcting it (fj#13).
                          //
                          // `depth` ALONE SAYS NOTHING. The 2026-10-05 backlog
                          // was diagnosed as a backlog only because 95520 was
                          // known to be a 96000-frame ring; the same number
                          // against a larger ring is an ordinary fill. The
                          // capacity now also bounds the fault -- it is 500 ms
                          // -- so publishing it is how a reader knows that.
                          //
                          // `speed` is the varispeed trim: above 1.0 draining,
                          // below 1.0 refilling, exactly 1.0 left alone. It is
                          // here because the correction is INAUDIBLE BY
                          // DESIGN, and a silent change to playback speed that
                          // cannot be observed from outside the process is
                          // indistinguishable from a bug. The log records the
                          // transitions; this records the state, which is what
                          // sampling MDXM_DIAG three times over a minute --
                          // how the original fault was pinned -- actually
                          // reads.
                          size_t capacity = 0;
                          double speed = 1.0; };
                      std::vector<Ring> rings; std::wstring personalDevice; bool personalFallback = false;
                      // The latency story, in frames, for the personal path.
                      //
                      // `cushionFrames` is what the mix waits for before it
                      // starts draining a channel, so it IS the latency; it is
                      // sized from `windowPull`, the largest pull in the last
                      // minute, and `maxMixPull` is the worst ever seen and
                      // sizes nothing (fj#12). Published because "why is this
                      // 100 ms" had no answer from outside the process.
                      size_t maxMixPull = 0;        // largest personal-render pull ever seen
                      size_t windowPull = 0;        // largest in the last 60 s -- sizes the cushion
                      size_t cushionFrames = 0;     // what the mix actually waits for
                      uint32_t mixRate = 0; };      // to turn any of the above into milliseconds

// Everything MDXM_VBANSTATE reports (spec §6.3), snapshotted under one lock so a
// reader never sees half of one moment and half of another.
//
// It lives HERE rather than in net/ because it is IMixerControl's currency and
// this header carries no Windows types -- which is what keeps the protocol layer
// testable against a fake.
//
// The three latency numbers answer three different questions and are easy to
// confuse. `depthMs` is how far ahead the PRODUCER is: a Bluetooth render writes
// seconds beyond what its own radio has played, and that is not delay the sender
// can remove. `behindMs` is the sender falling behind its own schedule, which IS
// this machine or the network failing to keep up. `srcLatencyMs` is the PC's own
// contribution to what the listener hears late, and it exists so the phone can
// add its half and show a figure somebody can type into a video player's
// audio-delay box.
struct VbanStatus {
    bool on = false;            // the listener socket, i.e. vban.enabled
    bool emitting = false;      // packets are actually going out to somebody
    int port = 0;
    std::wstring name;
    int peers = 0;
    bool sourceStreaming = false, formatFloat32 = false;
    int gainPercent = 100;
    double fps = 2.0;
    bool open = false, always = false, alwaysFrames = false;
    std::wstring target;
    uint64_t sent = 0, starved = 0, dropped = 0, framesSent = 0;
    int depthMs = 0, behindMs = 0, srcLatencyMs = 0;
    int authPending = 0;
    // A bind failure, a bad always-stream target. Published because a listener
    // that is not listening has to be able to say why: the symptom otherwise is
    // a phone that will not connect and nothing anywhere that explains it.
    std::wstring lastError;
};

// One row of MDXM_VBANPEERS: who is connected, and what they are getting.
struct VbanPeerRow {
    std::wstring addr;          // "ip:port"
    std::wstring deviceName;    // empty until a device has authenticated
    bool authed = false;
    unsigned sinceMs = 0;       // how long since this peer last pinged
    bool audioOn = false, framesOn = false;
    // The configured always-stream target, which is not a peer: it never pings,
    // holds no table slot and is not counted in `peers`. Shown as a row anyway,
    // because something IS being sent there and a reader needs to see it.
    bool isAlwaysTarget = false;
};

class IMixerControl {
public:
    virtual ~IMixerControl() = default;
    virtual std::vector<ChannelState> GetChannels() = 0;
    virtual std::vector<std::pair<std::wstring, std::wstring>> GetRoutes() = 0; // id -> endpointId
    virtual std::vector<std::tuple<std::wstring, std::wstring, bool, bool>> GetDevices() = 0; // id, name, isRender, active
    virtual bool SetVolume(const std::wstring& ch, Mix m, float vol) = 0;       // clamped by callee
    virtual bool SetMute(const std::wstring& ch, Mix m, bool mute) = 0;
    virtual bool SetEqBand(const std::wstring& ch, size_t band, double f, double g, double q) = 0;
    virtual bool EnableEq(const std::wstring& ch, bool on) = 0;
    virtual bool AssignApp(const std::wstring& exePath, const std::wstring& chOrDash) = 0;
    // What is true for these playing processes, against what is meant to be.
    //
    // A BATCH, not one call per row, because answering needs an endpoint
    // enumeration and the Routing tab asks for every row it draws: per-row it
    // would be twenty full MMDevice sweeps per refresh, on the one call path
    // known to fault while Bluetooth devices come and go. One sweep answers
    // them all. Returns one entry per input, in order.
    virtual std::vector<AppRouteState> GetAppRoutes(
        const std::vector<std::pair<std::wstring, unsigned long>>& apps) = 0;
    virtual bool SetPersonalRoute(const std::wstring& endpointId) = 0;
    // Move the WINDOWS default output. Distinct from SetPersonalRoute, which
    // only moves what mdxmixer's own mix plays to: this is what every other
    // application on the machine follows, and the way out of a default that
    // points at a virtual endpoint whose owner has stopped mixing.
    virtual bool SetDefaultOutput(const std::wstring& endpointId, std::wstring* err) = 0;
    // Send ONE app straight to ONE endpoint, bypassing both the default and
    // the channel map.
    //
    // Needed because the default is not always ours to take: measured on
    // 2026-10-03, SteelSeries Sonar re-asserts itself as the default output
    // within moments of anything else claiming it, so a machine whose Sonar
    // engine has died cannot be rescued by moving the default. A per-app route
    // is a different Windows store and Sonar does not contest it.
    virtual bool RouteAppToEndpoint(const std::wstring& exePath,
                                    const std::wstring& endpointId,
                                    std::wstring* err) = 0;
    virtual DiagState GetDiag() = 0;
    virtual bool ShowUi() = 0;
    // MDXM_EXIT: shut down through the REAL exit path -- config flushed,
    // engine stopped, routed apps released -- rather than being killed.
    //
    // It exists for the `mdxmixer` refresh command, which has to replace a
    // running exe. WM_CLOSE will not do: this app hides to the tray on close,
    // so the file would stay locked and the settings unsaved. Deliberately
    // skips the tray menu's "apps routed to cables go silent" confirmation;
    // asking over IPC IS the confirmation.
    virtual bool ExitApp() = 0;   // MDXM_SHOW: a second instance asks the app to raise its window
    // MDXM_CAPTURE: write a PNG of the window as it stands. In-process, so
    // owner-drawn content actually paints and the app need not be in front.
    virtual bool CaptureUi(const std::wstring& path, const std::wstring& window) = 0;
    // MDXM_TAB: bring one tab of the main window to the front, by name.
    // False for a name that is not a tab. Exists so documentation screenshots
    // can be regenerated instead of grabbed by hand and left to go stale.
    virtual bool ShowTab(const std::wstring& name) = 0;

    // The Windows volume of each audio endpoint — a separate axis from the
    // channel faders above, and the one a person reaches for when they want
    // the device itself louder rather than one channel within the mix.
    virtual std::vector<DeviceLevel> GetDeviceLevels() = 0;
    virtual bool SetDeviceVolume(const std::wstring& endpointId, float vol01) = 0;
    virtual bool SetDeviceMute(const std::wstring& endpointId, bool mute) = 0;
    // Our name for a device. Empty clears it. Persisted with the Windows name
    // so it survives the device re-pairing under a new endpoint id.
    virtual bool SetDeviceAlias(const std::wstring& endpointId,
                                const std::wstring& containerId,
                                const std::wstring& windowsName,
                                const std::wstring& alias) = 0;
    // How the device is filed in the list: pinned to the top, or hidden from it
    // entirely. Windows publishes far more endpoints than anyone mixes with —
    // five headsets' hands-free twins, every virtual Sonar device — and a list
    // that cannot be cut down is a list nobody reads.
    // Channels are what the Personal and Streaming faders act on. A render
    // endpoint as the source is tapped by WASAPI loopback, so a channel can
    // mirror an output — "SteelSeries Sonar - Gaming", say — with no cable
    // between them. Returns false with a reason in `err`; the reason a caller
    // must expect is the feedback refusal.
    virtual bool AddChannel(const std::wstring& name, const std::wstring& sourceEndpointId,
                            std::wstring* err) = 0;
    virtual bool RemoveChannel(const std::wstring& channelId) = 0;

    // Global hotkeys. The list is replaced wholesale rather than edited in
    // place: the window that edits them holds the whole table anyway, and one
    // write means one re-registration pass rather than one per row.
    // MDXM_HOTKEYS: open the Hotkeys window. A verb rather than only a tray
    // item because a window that can only be reached through the tray cannot
    // be opened by anything else -- a test, a remote, or MDropDX12.
    virtual bool ShowHotkeysUi() = 0;
    // What Windows answered for one binding: "held", "taken by another
    // application", "unbound". A binding is a REQUEST and RegisterHotKey is
    // first-come-first-served, so the window must show the answer and not the
    // request -- mdx12 shipped one that showed the request and called it the
    // answer, and a key another application owned looked configured, did
    // nothing, and explained nothing (fj#72).
    virtual std::wstring GetHotkeyStatus(const std::wstring& bindingId) = 0;
    virtual std::vector<HotkeyBinding> GetHotkeys() = 0;
    virtual bool SetHotkeys(const std::vector<HotkeyBinding>& bindings) = 0;
    virtual int  GetVolumeStep() = 0;
    virtual bool SetVolumeStep(int percent) = 0;
    // Run what a binding says. Called from WM_HOTKEY, so it must not block.
    virtual bool TriggerHotkey(const std::wstring& bindingId) = 0;
    // Every fader a hotkey could be pointed at: key and a label to show.
    virtual std::vector<std::pair<std::wstring, std::wstring>> GetHotkeyTargets() = 0;

    // ── Failover, for a remote front-end ────────────────────────────
    //
    // MDropDX12 keeps its own mixer window, but when mdxmixer is running the
    // GUI reads and writes THROUGH it rather than touching Windows directly:
    // "mdx12 will keep it's mixer, but when mdxmixer is running, the commands
    // run through to mdxmixer rather than windows api directly", and for this
    // rule specifically, "it can be set in mdx12 but the handling should be
    // in mdxmixer" -- one watcher, so two of them cannot fight and flap the
    // route between them.
    //
    // The whole ordered list is set at once rather than add/remove/move,
    // because the ORDER is the rule: a sequence of incremental edits has
    // intermediate states that are each a different rule, and a front-end
    // that dies halfway through would leave one of them live.
    // The shared-memory audio feed, off until something asks.
    //
    // It exists for the case where a visualiser cannot get the audio any
    // other way -- Sonar wedged, or not installed. That is no longer the
    // normal case here, so running it unasked means a mapping nobody reads
    // and a copy per audio block for nothing. A client turns it on when it
    // wants audio and off when it is done; it is off again on restart,
    // because "needed" is a property of the moment and not of the config.
    virtual bool SetFeedEnabled(bool on, std::wstring* err) = 0;
    virtual bool FeedEnabled() = 0;
    virtual uint32_t FeedRate() = 0;

    // The mix cushion's floor in milliseconds, which is the latency when the
    // link is behaving. Settable live because the right value is a property of
    // the hardware and the radio environment rather than something to
    // hard-code: the way to find it is to lower it and watch `underruns`.
    // cushion = max( floor , pull + pull*headroom% + flat ). -1 leaves a term
    // alone. Clamped by the callee; the reply reports what was applied.
    virtual int  GetCushionMs() = 0;
    virtual int  GetCushionHeadroomPercent() = 0;
    virtual int  GetCushionFlatMs() = 0;
    virtual bool SetCushion(int ms, int headroomPercent, int flatMs) = 0;

    virtual FailoverConfig GetFailover() = 0;
    // The rule is above; this is what the watcher is doing with it.
    virtual FailoverStatus GetFailoverStatus() = 0;
    virtual bool SetFailoverArmed(bool armed) = 0;
    virtual bool SetFailoverTiming(int stabilitySec, int dwellSec, int minGapSec) = 0;
    virtual bool SetFailoverAllow(const std::vector<DeviceRef>& allow) = 0;

    virtual bool SetDeviceView(const std::wstring& endpointId,
                               const std::wstring& containerId,
                               const std::wstring& windowsName,
                               bool hidden, bool pinned) = 0;

    // ── The VBAN stream server (spec §6.3) ──────────────────────────────
    virtual VbanStatus GetVbanStatus() = 0;
    // One keyed field at a time: "on", "port", "name", "source", "format",
    // "gain", "fps", "open", "always", "alwaysframes", "target", "pin".
    //
    // ONE KEY PER CALL rather than a whole config, because the caller is a wire
    // record that may carry any subset, and each field has its own validation
    // and its own clamp. The callee validates, applies live, and persists;
    // `false` with a reason in *err means nothing was applied, so a client
    // re-sends the record rather than guessing which half took.
    virtual bool SetVbanOption(const std::wstring& key, const std::wstring& value,
                               std::wstring* err) = 0;
    virtual std::vector<VbanPeerRow> GetVbanPeers() = 0;
    // Forget a device, and drop any session it is currently holding. UI-only by
    // design: revoking access over the very channel being revoked is a knot not
    // worth tying, and the list this acts on is shown in the VBAN tab.
    virtual bool RevokeVbanDevice(const std::wstring& deviceId) = 0;
};

} // namespace mdxm
