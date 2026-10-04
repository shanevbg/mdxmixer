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
                      float peak = kPeakUnknown; };
struct DiagState    { struct Ring { std::wstring id; size_t depth; uint64_t drops, underruns; };
                      std::vector<Ring> rings; std::wstring personalDevice; bool personalFallback = false;
                      size_t maxMixPull = 0; };   // largest personal-render pull seen (burst size)

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

    virtual FailoverConfig GetFailover() = 0;
    virtual bool SetFailoverArmed(bool armed) = 0;
    virtual bool SetFailoverTiming(int stabilitySec, int dwellSec, int minGapSec) = 0;
    virtual bool SetFailoverAllow(const std::vector<DeviceRef>& allow) = 0;

    virtual bool SetDeviceView(const std::wstring& endpointId,
                               const std::wstring& containerId,
                               const std::wstring& windowsName,
                               bool hidden, bool pinned) = 0;
};

} // namespace mdxm
